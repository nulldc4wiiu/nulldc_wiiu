// Sampling profiler for benches. sampler=1 keeps fight segments (sq above 7M in 600 vblanks), sampler=2 everything.
// A thread on core 0 pauses the emulator thread once a millisecond and notes where it was, so hot paths pay nothing and
// time in translated code gets split by SH4 block and by PPC instruction. Lowest-priority spinners on cores 0 and 2
// measure their idle time. Results go out as [samp] lines at the bench's RESULT; scratch/nulldc-samp-report.sh reads them.
#ifdef WIIU_INSTRUMENT
#include <coreinit/alarm.h>
#include <coreinit/context.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <algorithm>
#include <stdio.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"
#include "dc/mem/sh4_mem.h"
#include "dc/sh4/rec_v2/blockmanager.h"
#include "dc/sh4/rec_v2/ngen.h"

extern u8 *CodeCache;
extern u32 wiiu_fm_pool_lo(), wiiu_fm_pool_hi();   // wii_driver.cpp
u32 g_cnt_dyn, g_cnt_getcode, g_cnt_compile;       // dynamic block exits, table misses into C, blocks compiled

int wiiu_sampler_mode, wiiu_sampler_how;
OSThread *wiiu_emu_thread, *wiiu_main_thread;

static const u32 MAX_SAMPLES = 512 * 1024;
static u32 *samples, *callers;
static volatile u32 by_thread[4];   // samples by current thread: emulator, main, other, none
static volatile u32 n_samples, kept;
static volatile bool stop_threads, sampler_done;
static OSThread sampler, spinner[3];
static volatile u64 idle_ticks[3], idle_kept[3], idle_mark[3];
static OSTime span_kept, span_mark;

static inline void add_sample(u32 pc, u32 lr = 0)
{
    if (pc & 3)
        pc = __builtin_bswap32(pc);   // Cemu keeps it little-endian; a real PC is word aligned
    if (lr & 3)
        lr = __builtin_bswap32(lr);
    if (n_samples < MAX_SAMPLES)
    {
        samples[n_samples] = pc;
        callers[n_samples] = lr;
        n_samples = n_samples + 1;
    }
}

// sampler=1/2: an alarm on the emulator's core; its callback gets the interrupted context
static OSAlarm alarm_;
static void alarm_cb(OSAlarm *, OSContext *c)
{
    if (c)
    {
        const OSThread *t = OSGetCurrentThread();
        by_thread[!t ? 3 : t == wiiu_emu_thread ? 0 : t == wiiu_main_thread ? 1 : 2]++;
        add_sample(c->srr0, c->lr);
    }
}

// sampler_how=1, the fallback: suspend from core 0. The thread's own context then always shows the OS switch point
// (console, 2026-11-06), so look up its stack for the interrupted context the OS saved there
static u32 interrupted_pc(OSThread *t)
{
    u32 sp = t->context.gpr[1];
    const u32 top = (u32)(uintptr_t)t->stackStart;
    if (sp & 7)
        sp = (sp + 7) & ~7u;
    for (u32 a = sp, n = 0; a + sizeof(OSContext) < top && n < 1024; a += 8, n++)
    {
        const OSContext *c = (const OSContext *)(uintptr_t)a;
        if (c->tag == OS_CONTEXT_TAG)
            return c->srr0;
    }
    return 0x10;   // none found: shows up under OS
}
static int sampler_main(int, const char **)
{
    while (!stop_threads)
    {
        OSSleepTicks(OSMicrosecondsToTicks(1000));
        OSSuspendThread(wiiu_emu_thread);
        const u32 pc = interrupted_pc(wiiu_emu_thread);
        OSResumeThread(wiiu_emu_thread);
        add_sample(pc);
    }
    sampler_done = true;
    return 0;
}

static int spinner_main(int which, const char **)
{
    OSTime last = OSGetSystemTime();
    const OSTime gap = OSMicrosecondsToTicks(20);   // longer: something else ran on this core
    while (!stop_threads)
    {
        const OSTime t = OSGetSystemTime();
        if (t - last < gap)
            idle_ticks[which] = idle_ticks[which] + (t - last);
        last = t;
    }
    return 0;
}

void wiiu_sampler_start()
{
    if (!wiiu_sampler_mode || !wiiu_emu_thread)
        return;
    samples = (u32 *)malloc(MAX_SAMPLES * 4);
    callers = (u32 *)malloc(MAX_SAMPLES * 4);
    if (!samples || !callers)
    {
        wiiu_sampler_mode = 0;
        return;
    }
    u8 (*st)[32 * 1024] = (u8 (*)[32 * 1024])memalign(16, 4 * 32 * 1024);
    if (wiiu_sampler_how)
        OSCreateThread(&sampler, sampler_main, 0, nullptr, st[0] + 32 * 1024, 32 * 1024, 5, OS_THREAD_ATTRIB_AFFINITY_CPU0);
    else
    {
        OSCreateAlarm(&alarm_);
        OSSetPeriodicAlarm(&alarm_, OSGetTime() + OSMillisecondsToTicks(1), OSMicrosecondsToTicks(1000), alarm_cb);
    }
    OSCreateThread(&spinner[0], spinner_main, 0, nullptr, st[1] + 32 * 1024, 32 * 1024, 31, OS_THREAD_ATTRIB_AFFINITY_CPU0);
    OSCreateThread(&spinner[1], spinner_main, 1, nullptr, st[2] + 32 * 1024, 32 * 1024, 31, OS_THREAD_ATTRIB_AFFINITY_CPU2);
    OSCreateThread(&spinner[2], spinner_main, 2, nullptr, st[3] + 32 * 1024, 32 * 1024, 31, OS_THREAD_ATTRIB_AFFINITY_CPU1);
    if (wiiu_sampler_how)
        OSResumeThread(&sampler);
    OSResumeThread(&spinner[0]);
    OSResumeThread(&spinner[1]);
    OSResumeThread(&spinner[2]);
    span_mark = OSGetSystemTime();
    printf("[samp] started, mode %d\n", wiiu_sampler_mode);
}

// a 600-vblank bench segment ended: keep its samples or drop them
void wiiu_sampler_segment(bool fight)
{
    if (!wiiu_sampler_mode)
        return;
    const OSTime now = OSGetSystemTime();
    if (fight || wiiu_sampler_mode == 2)
    {
        kept = n_samples;
        span_kept += now - span_mark;
        for (int i = 0; i < 3; i++)
            idle_kept[i] += idle_ticks[i] - idle_mark[i];
    }
    else
        n_samples = kept;
    span_mark = now;
    for (int i = 0; i < 3; i++)
        idle_mark[i] = idle_ticks[i];
}

struct Hit { u32 key, n; };
static void line_pause() { OSSleepTicks(OSMicrosecondsToTicks(3000)); }   // UDP drops bursts

void wiiu_sampler_report()
{
    if (!wiiu_sampler_mode)
        return;
    // no OSJoinThread: joining here hung on a console (Sonic, 2026-11-06), before the report. The sampler must be out of
    // its suspend/resume pair, so wait for that briefly; the spinners end by themselves whenever they next run
    stop_threads = true;
    if (!wiiu_sampler_how)
        OSCancelAlarm(&alarm_);
    for (int i = 0; wiiu_sampler_how && i < 100 && !sampler_done; i++)
        OSSleepTicks(OSMillisecondsToTicks(1));
    const u32 n = kept;
    printf("[samp] %u samples over %.1f s; anchor wiiu_sampler_report=%p\n", n, OSTicksToMicroseconds(span_kept) / 1e6,
           (void *)&wiiu_sampler_report);
    printf("[samp] idle: core 0 %.1f%%, core 1 %.1f%%, core 2 %.1f%%\n", 100.0 * idle_kept[0] / (span_kept ? span_kept : 1),
           100.0 * idle_kept[2] / (span_kept ? span_kept : 1), 100.0 * idle_kept[1] / (span_kept ? span_kept : 1));
    printf("[samp] threads: emulator %u, main %u, other %u, none %u\n", by_thread[0], by_thread[1], by_thread[2], by_thread[3]);
    printf("[samp] counters: dynamic exits %u, table misses %u, compiled %u\n", g_cnt_dyn, g_cnt_getcode, g_cnt_compile);
    if (!n)
        return;
    // who called: the commonest return addresses (LR is the caller's while a leaf like memcpy runs)
    {
        u32 *lrs = (u32 *)malloc(n * 4);
        if (lrs)
        {
            memcpy(lrs, callers, n * 4);
            std::sort(lrs, lrs + n);
            static const int TOP = 40;
            Hit top[TOP] = {};
            for (u32 i = 0; i < n; )
            {
                const u32 lr = lrs[i];
                u32 run = 0;
                while (i < n && lrs[i] == lr)
                {
                    i++;
                    run++;
                }
                for (int k = 0; k < TOP; k++)
                    if (run > top[k].n)
                    {
                        for (int m = TOP - 1; m > k; m--)
                            top[m] = top[m - 1];
                        top[k] = { lr, run };
                        break;
                    }
            }
            for (int k = 0; k < TOP && top[k].n; k++)
            {
                printf("[samp] lr %08x %.2f%%\n", top[k].key, 100.0 * top[k].n / n);
                line_pause();
            }
            free(lrs);
        }
    }
    std::sort(samples, samples + n);

    u32 *codes = (u32 *)malloc(65536 * 4), *addrs = (u32 *)malloc(65536 * 4);
    u64 *pairs = (u64 *)malloc(65536 * 8);
    Hit *blocks = (Hit *)malloc(65536 * sizeof(Hit)), *pcs = (Hit *)malloc(65536 * sizeof(Hit));
    if (!codes || !addrs || !pairs || !blocks || !pcs)
        return;
    const u32 nb = bm_Snapshot(codes, addrs, 65536);
    // sort the blocks by host address (indices through a pair array)
    for (u32 i = 0; i < nb; i++)
        pairs[i] = (u64)codes[i] << 32 | addrs[i];
    std::sort(pairs, pairs + nb);

    const u32 cc = (u32)(uintptr_t)CodeCache, fm_lo = wiiu_fm_pool_lo(), fm_hi = wiiu_fm_pool_hi();
    u32 c_jit = 0, c_stub = 0, c_tramp = 0, c_app = 0, c_os = 0;
    u32 n_blocks = 0, n_pcs = 0;
    for (u32 i = 0; i < n; )
    {
        const u32 pc = samples[i];
        u32 run = 0;
        while (i < n && samples[i] == pc)
        {
            i++;
            run++;
        }
        if (n_pcs < 65536)
            pcs[n_pcs++] = { pc, run };
        if (pc >= fm_lo && pc < fm_hi)
            c_tramp += run;
        else if (pc >= cc && pc < cc + CODE_SIZE)
        {
            const u64 key = (u64)(pc + 1) << 32;
            const u64 *it = std::lower_bound(pairs, pairs + nb, key);
            if (it == pairs)
                c_stub += run;
            else
            {
                const u32 sh4 = (u32)it[-1];
                c_jit += run;
                u32 k = 0;
                for (; k < n_blocks; k++)
                    if (blocks[k].key == sh4)
                        break;
                if (k == n_blocks && n_blocks < 65536)
                    blocks[n_blocks++] = { sh4, 0 };
                if (k < 65536)
                    blocks[k].n += run;
            }
        }
        else if (pc >= 0x02000000 && pc < 0x10000000)
            c_app += run;
        else
            c_os += run;
    }
    const auto pct = [n](u32 v) { return 100.0 * v / n; };
    printf("[samp] where: JIT blocks %.1f%%, stubs %.1f%%, trampolines %.1f%%, app C %.1f%%, OS %.1f%%\n", pct(c_jit), pct(c_stub),
           pct(c_tramp), pct(c_app), pct(c_os));
    const auto by_n = [](const Hit &a, const Hit &b) { return a.n > b.n; };
    std::sort(blocks, blocks + n_blocks, by_n);
    u32 sum = 0;
    for (u32 i = 0; i < n_blocks && i < 48; i++)
    {
        sum += blocks[i].n;
        char ops[16 * 5 + 1];
        for (u32 k = 0; k < 16; k++)
            snprintf(ops + k * 5, 6, " %04x", ReadMem16(blocks[i].key + k * 2));
        printf("[samp] block %08x %.2f%% (cum %.1f)%s\n", blocks[i].key, pct(blocks[i].n), pct(sum), ops);
        line_pause();
    }
    printf("[samp] %u blocks sampled\n", n_blocks);
    std::sort(pcs, pcs + n_pcs, by_n);
    for (u32 i = 0; i < n_pcs && i < 64; i++)
    {
        const u32 pc = pcs[i].key;
        const bool in_jit = pc >= cc && pc < cc + CODE_SIZE;
        const bool readable = in_jit || (pc >= 0x02000000 && pc < 0x10000000);
        printf("[samp] pc %08x %.2f%% insn %08x\n", pc, pct(pcs[i].n), readable ? *(u32 *)(uintptr_t)pc : 0);
        line_pause();
    }
    printf("[samp] done\n");
}
#endif   // WIIU_INSTRUMENT
