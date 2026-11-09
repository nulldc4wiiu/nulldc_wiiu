#include "aica.h"
#include "sgc_if.h"
#include "aica_mem.h"
#include "wii/wii_audio.h"
#include <math.h>

// config.h (pulled in transitively above) deliberately poisons BIG_ENDIAN and
// LITTLE_ENDIAN to catch accidental use; must undef before any libogc header,
// which legitimately defines those same names via machine/endian.h. Same
// pattern as wii/wii_audio.cpp.
#ifdef BIG_ENDIAN
#  undef BIG_ENDIAN
#endif
#ifdef LITTLE_ENDIAN
#  undef LITTLE_ENDIAN
#endif
#include <ogc/lwp_watchdog.h>   // gettime(), ticks_to_millisecs()

#define SH4_IRQ_BIT (1<<(u8)holly_SPU_IRQ)
#define AICA_RATE_DIAG 0   // 1 = count real libAICA_TimeStep() calls/sec (see there)

CommonData_struct* CommonData;
DSPData_struct*    DSPData;
InterruptInfo*     MCIEB;
InterruptInfo*     MCIPD;
InterruptInfo*     MCIRE;
InterruptInfo*     SCIEB;
InterruptInfo*     SCIPD;
InterruptInfo*     SCIRE;

// ---- Interrupts ----

u32 GetL(u32 witch)
{
    if (witch > 7)
        witch = 7;

    u32 bit = 1 << witch;
    u32 rv  = 0;

    if (CommonData->SCILV0 & bit) rv  = 1;
    if (CommonData->SCILV1 & bit) rv |= 2;
    if (CommonData->SCILV2 & bit) rv |= 4;

    return rv;
}

// ArmInterruptChange is provided by the vbaARM plugin (arm_mem.cpp). It sets
// the AICA interrupt-pending state which raises the ARM7 FIQ. The wrestler
// test (and real games) program SCIEB + timers and then wait for this FIQ to
// drive their main loop, so this MUST be wired up or the ARM7 spins in init
// and never reaches KeyOn.
// NOTE: declared with C++ linkage (NOT extern "C") to match the definition in
// vbaARM/arm_mem.cpp; an extern "C" wrapper here breaks the link.
void FASTCALL ArmInterruptChange(u32 bits, u32 L);

// AICA_RACE (wii/main.cpp; Flycast a920ae590): SCIEB / MCIEB writes update the
// interrupt lines at once, and a write that raises the SH4 interrupt gives the
// ARM7 slice >= 50 more cycles (ArmAvoidRaceCondition, vbaARM arm_mem.cpp --
// C++ linkage, like ArmInterruptChange). Read only on those register writes.
extern "C" int get_aica_race_preset();
#define AICA_RACE_FIX() (get_aica_race_preset() != 0)
void FASTCALL ArmAvoidRaceCondition();

extern int g_aica_fast_preset;   // wii/main.cpp

void update_arm_interrupts()
{
    u32 p_ints = SCIEB->full & SCIPD->full;

    u32 Lval = 0;
    if (p_ints && g_aica_fast_preset)
    {
        // Runs every sample. Lowest pending source among bits 0..10, same as
        // the loop below; SAMPLE_DONE is bit 10, so the loop often ran all 11.
        const u32 low = p_ints & 0x7FF;
        if (low)
            Lval = GetL(__builtin_ctz(low));
    }
    else if (p_ints)
    {
        u32 bit_value = 1;
        for (u32 i = 0; i < 11; i++)
        {
            if (p_ints & bit_value)
            {
                Lval = GetL(i);
                break;
            }
            bit_value <<= 1;
        }
    }

    ArmInterruptChange(p_ints, Lval);
}

// Returns whether an SH4 interrupt is pending (Flycast's shape; AICA_RACE uses it).
#ifdef __WIIU__
bool aica_on_sound_thread();
bool aica_defer_reg(u32 reg, u32 data, u32 sz);
static volatile bool sh4_ints_dirty;
#endif
bool UpdateSh4Ints()
{
    u32 p_ints = MCIEB->full & MCIPD->full;
#ifdef __WIIU__
    if (aica_on_sound_thread())
    {
        sh4_ints_dirty = true;   // the SH4's interrupt state belongs to the emulator thread (aica_poll_ints)
        return p_ints != 0;
    }
#endif
    if (p_ints)
    {
        if ((*aica_params.SB_ISTEXT & SH4_IRQ_BIT) == 0)
            aica_params.RaiseInterrupt(holly_SPU_IRQ);
        return true;
    }
    else
    {
        if (*aica_params.SB_ISTEXT & SH4_IRQ_BIT)
            aica_params.CancelInterrupt(holly_SPU_IRQ);
        return false;
    }
}

// ---- Timers ----

struct AicaTimerData
{
    union
    {
        struct
        {
#ifdef WII
            u32 pad:16;
            u32 nil:5;
            u32 md:3;
            u32 count:8;
#else
            u32 count:8;
            u32 md:3;
            u32 nil:5;
            u32 pad:16;
#endif
        };
        u32 data;
    };
};

class AicaTimer
{
public:
    AicaTimerData* data;
    s32  c_step;
    u32  m_step;
    u32  id;

    void Init(u8* regbase, u32 timer)
    {
        data   = (AicaTimerData*)&regbase[0x2890 + timer * 4];
        id     = timer;
        m_step = 1 << (data->md);
        c_step = m_step;
    }

    void StepTimer()
    {
        c_step--;
        if (c_step == 0)
        {
            c_step = m_step;
            data->count++;
            if (data->count == 0)
            {
                if      (id == 0) { SCIPD->TimerA = 1; MCIPD->TimerA = 1; }
                else if (id == 1) { SCIPD->TimerB = 1; MCIPD->TimerB = 1; }
                else              { SCIPD->TimerC = 1; MCIPD->TimerC = 1; }
            }
        }
    }

    void RegisterWrite()
    {
        u32 n_step = 1 << (data->md);
        if (n_step != m_step)
        {
            m_step = n_step;
            c_step = m_step;
        }
    }
};

AicaTimer timers[3];

// ---- Main loop ----

void FASTCALL UpdateAICA(u32 Samples)
{
    while (Samples > 0)
    {
        Samples--;

        AICA_Sample();
        SCIPD->SAMPLE_DONE = 1;

        for (int i = 0; i < 3; i++)
            timers[i].StepTimer();
    }

    update_arm_interrupts();
    UpdateSh4Ints();
}

// ---- Register writes ----

template<u32 sz>
void WriteAicaReg(u32 reg, u32 data)
{
#ifdef __WIIU__
    if (aica_defer_reg(reg, data, sz))
        return;
#endif
    switch (reg)
    {
    case SCIPD_addr:
        verify(sz != 1);
        if (data & (1 << 5))
        {
            SCIPD->SCPU = 1;
            update_arm_interrupts();
        }
        return; // read-only

    case SCIRE_addr:
        verify(sz != 1);
        SCIPD->full &= ~data;
        data = 0;
        update_arm_interrupts();
        break;

    case MCIPD_addr:
        if (data & (1 << 5))
        {
            verify(sz != 1);
            MCIPD->SCPU = 1;
            if (UpdateSh4Ints() && AICA_RACE_FIX())
                ArmAvoidRaceCondition();
        }
        return; // read-only

    // AICA_RACE: the enable masks take effect now, not at the next UpdateAICA().
    case SCIEB_addr:
        WriteMemArr(aica_reg, reg, data, sz);
        if (AICA_RACE_FIX())
            update_arm_interrupts();
        break;

    case MCIEB_addr:
        WriteMemArr(aica_reg, reg, data, sz);
        if (AICA_RACE_FIX() && UpdateSh4Ints())
            ArmAvoidRaceCondition();
        break;

    case MCIRE_addr:
        verify(sz != 1);
        MCIPD->full &= ~data;
        UpdateSh4Ints();
        break;

    case TIMER_A:
        WriteMemArr(aica_reg, reg, data, sz);
        timers[0].RegisterWrite();
        break;

    case TIMER_B:
        WriteMemArr(aica_reg, reg, data, sz);
        timers[1].RegisterWrite();
        break;

    case TIMER_C:
        WriteMemArr(aica_reg, reg, data, sz);
        timers[2].RegisterWrite();
        break;

    default:
        WriteMemArr(aica_reg, reg, data, sz);
        break;
    }
}

template void WriteAicaReg<1>(u32 reg, u32 data);
template void WriteAicaReg<2>(u32 reg, u32 data);

// ---- Init / Term ----

void AICA_Init()
{
    //printf("[AICA] AICA_Init: start\n");
    //printf("[AICA] AICA_Init: sizeof(CommonData)=%d (need 0x508=%d)\n", (int)sizeof(*CommonData), 0x508);
    //printf("[AICA] AICA_Init: sizeof(DSPData)=%d (need 0x15C8=%d)\n",   (int)sizeof(*DSPData),    0x15C8);

    verify(sizeof(*CommonData) == 0x508);
    verify(sizeof(*DSPData)    == 0x15C8);
    //printf("[AICA] AICA_Init: struct sizes OK\n");

    //printf("[AICA] AICA_Init: aica_reg ptr = %p\n", (void*)aica_reg);
    CommonData = (CommonData_struct*)&aica_reg[0x2800];
    DSPData    = (DSPData_struct*)   &aica_reg[0x3000];
    SCIEB = (InterruptInfo*)&aica_reg[0x289C];
    SCIPD = (InterruptInfo*)&aica_reg[0x289C + 4];
    SCIRE = (InterruptInfo*)&aica_reg[0x289C + 8];
    MCIEB = (InterruptInfo*)&aica_reg[0x28B4];
    MCIPD = (InterruptInfo*)&aica_reg[0x28B4 + 4];
    MCIRE = (InterruptInfo*)&aica_reg[0x28B4 + 8];
    //printf("[AICA] AICA_Init: pointers set\n");

    //printf("[AICA] AICA_Init: sgc_Init()...\n");
    sgc_Init();
    //printf("[AICA] AICA_Init: sgc_Init() done\n");

    //printf("[AICA] AICA_Init: timers init...\n");
    for (int i = 0; i < 3; i++)
        timers[i].Init(aica_reg, i);
    //printf("[AICA] AICA_Init: timers done\n");

    //printf("[AICA] AICA_Init: wii_audio_aica_ready()...\n");
    wii_audio_aica_ready();
    //printf("[AICA] AICA_Init: done\n");
}

#ifdef __WIIU__
void aica_thread_stop();
#endif
void AICA_Term()
{
#ifdef __WIIU__
    aica_thread_stop();
#endif
    sgc_Term();
}

// Steps one AICA sample, ticks timers, sets SAMPLE_DONE, and updates the
// ARM7 / SH4 interrupt lines. Driven once per sample from the SH4 timeslice
// (armUpdateARM) — see [[aica-arm-timing]].
#ifdef __WIIU__
// exp_aica_thread (1: core 0, 2: core 2): mixing runs on another core, at most AICA_AHEAD samples behind. Timers and
// interrupts stay here. The mixer is the only thread that changes channel state: a channel register write stores its
// value at once (reads must see it) and queues its processing, tagged with the sample it belongs to, so voices key on
// and off at the same sample as without the thread. Reads of channel position lag by up to AICA_AHEAD samples.
#include <coreinit/thread.h>
#include <coreinit/time.h>
extern int g_exp_aica_thread;
int g_exp_arm_thread;   // exp_arm_thread (needs exp_aica_thread): timers and the ARM7 run on the sound thread too
void arm_SetEnabled(bool enabled);
void aica_cdda_prefetch();
static const u32 AICA_AHEAD = 512;
static const u32 AICA_OPS = 8192;
void aica_clear_looped_now(u32 chan);
// reg 0xFFFF: clear the channel's loop flag; 0xFFFE: WriteAicaReg (chan the register, data, size in sz); 0xFFFD: ARM enable
static struct { u32 sample; u16 chan, reg; u32 data, sz; } aica_ops[AICA_OPS];
static volatile u32 ops_w, ops_r;
static volatile u32 aica_pending, aica_done;
static volatile bool aica_stop, aica_started;
static OSThread aica_thread;
static void aica_apply_ops(u32 sample)
{
    while (ops_r != ops_w)
    {
        // the queued op and the register values written before it, as the emulator's core published them (its
        // sync orders the writes; on the console this core must not read them from before ops_w moved)
        __asm__ volatile("sync" : : : "memory");
        if ((s32)(aica_ops[ops_r % AICA_OPS].sample - sample) > 0)
            break;
        const auto &op = aica_ops[ops_r % AICA_OPS];
        if (op.reg == 0xFFFF)
            aica_clear_looped_now(op.chan);
        else if (op.reg == 0xFFFE)
        {
            if (op.sz == 1)
                WriteAicaReg<1>(op.chan, op.data);
            else
                WriteAicaReg<2>(op.chan, op.data);
        }
        else if (op.reg == 0xFFFD)
            arm_SetEnabled(op.data != 0);
        else
            WriteChannelReg8(op.chan, op.reg);
        ops_r = ops_r + 1;
    }
}
static void aica_queue(u32 chan, u32 reg, u32 data = 0, u32 sz = 0)
{
    while (ops_w - ops_r >= AICA_OPS)
        OSSleepTicks(OSMicrosecondsToTicks(100));
    auto &op = aica_ops[ops_w % AICA_OPS];
    op.sample = aica_pending;
    op.chan = (u16)chan;
    op.reg = (u16)reg;
    op.data = data;
    op.sz = sz;
    __asm__ volatile("sync" : : : "memory");
    ops_w = ops_w + 1;
}
bool aica_on_sound_thread() { return aica_started && OSGetCurrentThread() == &aica_thread; }
// exp_arm_thread: registers that drive the ARM side go through the queue from the emulator thread. The SH4's own
// interrupt clears (MCIRE, MCIEB) stay immediate, or its handler would re-enter until the queue caught up
bool aica_defer_reg(u32 reg, u32 data, u32 sz)
{
    if (!g_exp_arm_thread || !aica_started || aica_on_sound_thread())
        return false;
    if (reg != SCIPD_addr && reg != SCIRE_addr && reg != SCIEB_addr && reg != TIMER_A && reg != TIMER_B && reg != TIMER_C)
        return false;
    aica_queue(reg, 0xFFFE, data, sz);
    return true;
}
void aica_arm_enable(bool on)
{
    if (g_exp_arm_thread && aica_started)
        aica_queue(0, 0xFFFD, on);
    else
        arm_SetEnabled(on);
}
bool aica_sound_thread_up() { return aica_started; }
void aica_poll_ints()
{
    if (sh4_ints_dirty)
    {
        sh4_ints_dirty = false;
        UpdateSh4Ints();
    }
}
void aica_channel_write(u32 chan, u32 reg)
{
    if (aica_started && !aica_on_sound_thread())
        aica_queue(chan, reg);
    else
        WriteChannelReg8(chan, reg);
}
void aica_clear_looped(u32 chan)
{
    if (aica_started && !aica_on_sound_thread())
        aica_queue(chan, 0xFFFF);
    else
        aica_clear_looped_now(chan);
}
void arm_run_samples(u32 samples);   // arm_aica.cpp
static u32 arm_due;
static int aica_main(int, const char **)
{
    while (!aica_stop)
    {
        while (aica_done != aica_pending)
        {
            aica_apply_ops(aica_done);
            AICA_Sample();
            if (g_exp_arm_thread)
            {
                SCIPD->SAMPLE_DONE = 1;
                for (int i = 0; i < 3; i++)
                    timers[i].StepTimer();
                update_arm_interrupts();
                if (++arm_due == 8)
                {
                    arm_run_samples(arm_due);
                    arm_due = 0;
                }
            }
            aica_done = aica_done + 1;
        }
        aica_apply_ops(aica_done);
        OSSleepTicks(OSMicrosecondsToTicks(100));
    }
    return 0;
}
static void aica_thread_step()
{
    if (!aica_started)
    {
        static u8 stack[64 * 1024] __attribute__((aligned(16)));
        aica_started = true;
        // below the GX2 thread: CafeOS doesn't time-slice, and with the ARM7 this thread can stay busy
        OSCreateThread(&aica_thread, aica_main, 0, nullptr, stack + sizeof(stack), sizeof(stack), 18,
                       g_exp_aica_thread == 2 ? OS_THREAD_ATTRIB_AFFINITY_CPU2 : OS_THREAD_ATTRIB_AFFINITY_CPU0);
        OSSetThreadName(&aica_thread, "nullDC AICA");
        OSResumeThread(&aica_thread);
        printf("[aica] mixing on core %d\n", g_exp_aica_thread == 2 ? 2 : 0);
    }
    if (aica_pending % 588 == 0)
        aica_cdda_prefetch();
    while (aica_pending - aica_done >= AICA_AHEAD)
        OSSleepTicks(OSMicrosecondsToTicks(100));
    aica_pending = aica_pending + 1;
}
void aica_thread_stop()
{
    if (aica_started)
    {
        aica_stop = true;
        OSJoinThread(&aica_thread, nullptr);
        aica_started = false;
    }
}
#endif
#ifdef __WIIU__
// where an AICA sample's time goes, for the [aica-prof] line next to the speed report (SPG.cpp); the rest of
// AicaTicks is AICA_Sample's own mixing and the time-step loop
u64 AicaProfTicks[AICA_PROF_COUNT];
#endif

void libAICA_TimeStep()
{
#ifdef __WIIU__
    const u64 t0 = gettime_fast();
#endif
#ifdef __WIIU__
    if (g_exp_aica_thread)
    {
        aica_thread_step();
        if (g_exp_arm_thread)
            return;
    }
    else
#endif
    AICA_Sample();
    SCIPD->SAMPLE_DONE = 1;
#ifdef __WIIU__
    const u64 t1 = gettime_fast();
    AicaProfTicks[AICA_PROF_SAMPLE] += t1 - t0;
#endif

    for (int i = 0; i < 3; i++)
        timers[i].StepTimer();

    update_arm_interrupts();
    UpdateSh4Ints();
#ifdef __WIIU__
    AicaProfTicks[AICA_PROF_TIMERS_INTS] += gettime_fast() - t1;
#endif

#if AICA_RATE_DIAG
    // Off by default: even with the printf commented out, this ran a gettime()
    // and a 64-bit ticks->ms division (a libgcc call on the 750) 44,100x/s.
    //
    // Diagnostic: measure the ACTUAL rate of libAICA_TimeStep() calls (i.e.
    // AICA_Sample() generation) against a real hardware wall-clock timer,
    // independent of the emulator's own SH4-cycle accounting. Should read
    // ~44100/sec if the cycle-to-sample pipeline (armUpdateARM's
    // AICA_SAMPLE_CYCLES accumulator) is correctly paced; a value far below
    // that (e.g. ~4000-4500/sec) would mean AICA is being driven at roughly
    // 1/10 the intended rate, which would explain audio sounding both
    // pitched too low AND stretched in duration by that same factor.
    static u32 sample_count = 0;
    static u64 last_check_ms = 0;
    sample_count++;
    u64 now_ms = ticks_to_millisecs(gettime());
    if (now_ms - last_check_ms >= 1000)
    {
        //printf("[AICA] real-time sample rate: %u samples/%llums (target 44100/1000ms)\n",
        //       sample_count, (unsigned long long)(now_ms - last_check_ms));
        (void)sample_count;
        sample_count = 0;
        last_check_ms = now_ms;
    }
#endif
}
