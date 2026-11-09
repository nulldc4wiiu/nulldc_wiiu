// Native Wii U entry point, replacing wii/main.cpp (the libogc menu and boot flow).
//
// Files live in sd:/wiiu/nulldc/: data/dc_boot.bin and data/dc_flash.bin (required by the core), nullDC.cfg,
// and wiiu.cfg (test builds carry their own wiiu.cfg in the WUHB content, which wins), read once at start:
//   file=<path>       disc image or .elf to boot (relative paths are under the home folder)
//   home=<path>       folder with data/ instead of sd:/wiiu/nulldc/, e.g. fs:/vol/content/; when it isn't on the
//                     SD card nothing is written there (no last_run.txt, no BIOS flash save)
//   g_<name>=<int>    override any int preset from wii/main.cpp, e.g. g_dynarec_preset=0 for the interpreter
//   probe=<hex addr>  log 4 words of emulated RAM from there every second (test programs report through it)
//   run_seconds=<n>   stop after n seconds and return to the Wii U Menu (unattended test runs)
//   thumb=<n>         after n seconds log an 80x60 thumbnail of the screen (compare a console run with Cemu)
//   reset_trace=1     log the blocks the JIT compiles after a game's soft reset (dc/sh4/ccn.cpp)
//   disc_verify=1     check the disc read-ahead cache against direct reads (disc_cache_wiiu.cpp)
//   gx2_priority=<n>  the main (GX2) thread's priority once the emulator runs (lower is more urgent; default 16)
//   rt_width=<n>      the 3D render target's width, 4:3 (default 960; 640 is the Dreamcast's own)
//   gx2_batch=0       draw each strip on its own instead of in runs of the same state
//   gx2_pipeline=1    convert the next 3D frame during the flip wait (off: stale textures in MvC2)
//   gpu_timing=1      measure the GPU's time for the 3D scene (waits for it: a test setting)
//   dump_test=<n>     after n seconds print the boot-ROM write dump once (debug_wiiu.cpp), to check it works
//   picker=0          boot the BIOS when no file= is given, instead of showing the game picker
// Output (stdout/stderr) goes to OSReport and to UDP tagged [nulldc] (Mac log out/hw/udplog.txt); the SD card
// gets one summary line per run in last_run.txt, plus the full log.txt only with sdlog=1 (it writes every line).
// The emulator runs on the codegen core, where the SH4 JIT's code has to execute.
#include <coreinit/codegen.h>
#include <coreinit/core.h>
#include <coreinit/debug.h>
#include <coreinit/exception.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
#include <whb/log.h>
#include <whb/log_udp.h>
#include <proc_ui/procui.h>
#include <whb/gfx.h>
#include <whb/proc.h>
#include <sysapp/launch.h>
#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string>
#include "types.h"
#include "dc/dc.h"
#include "dc/mem/_vmem.h"
#include "dc/mem/sh4_mem.h"
#include "plugins/plugin_manager.h"
#include "dc/sh4/sh4_registers.h"
#include "dc/sh4/rec_v2/ngen.h"
#include "wiiu_codegen.h"

#define NULLDC_HOME "fs:/vol/external01/wiiu/nulldc/"
// where the BIOS files (data/) and relative file= paths are: the SD card, or with home= in wiiu.cfg somewhere
// else, such as fs:/vol/content/ for a test build that carries everything and writes nothing to the card
static char home[256] = NULLDC_HOME;
static bool sd_writes = true;   // run summary and BIOS flash save; off when home isn't on the SD card

extern void SetApplicationPath(const char *path);
extern int EmuMain(int argc, char *argv[]);
extern u32 wiiu_vblanks, wiiu_renders, wiiu_ta_lists;
bool wiiu_gx2_init();
void wiiu_gx2_frame();
void wiiu_gx2_shutdown();
void wiiu_gx2_thumbnail();
void wiiu_audio_init();
bool wiiu_pick_game(const char *home, char *path, size_t size);
void wiiu_picker_speed_options(const char *home, bool *gfx_core, bool *sound_core, bool *jit_fast, bool *smooth,
                               bool *code_check, int *sound_buffer_ms);
void wiiu_picker_options(const char *home, bool *fastmem, bool *lightgun, int *render_width, bool *keep_pitch);
void wiiu_gx2_set_render_width(int width);   // present_gx2.cpp
extern float wiiu_audio_min_speed;          // audio_wiiu.cpp
static bool rt_width_set;                    // wiiu.cfg set rt_width: it wins over the picker's option
void wiiu_kpad_init();
extern volatile const char *wiiu_gx2_stage;
extern volatile u32 wiiu_gx2_frames_done;
extern "C" u32 wiiu_audio_queued();

// straight to UDP, not through stdio, whose lock a crashed or stuck thread may hold
#define WLOG(...) WHBLogPrintf("[nulldc] " __VA_ARGS__)

// ---- crash report ------------------------------------------------------------------------------------
// A fault in a CafeOS app normally stops the whole console on a black screen. These handlers note where it
// happened and park the faulting thread instead, so the watchdog (another core) can send the record over UDP.
static struct
{
    volatile u32 type;   // 0 none, else OS_EXCEPTION_TYPE_*
    u32 srr0, srr1, dar, dsisr, lr, ctr, cr, core, gpr[32];
    char thread[32];
} crash;

static void park_thread()
{
    for (;;)
        OSSleepTicks(OSSecondsToTicks(60));
}

static BOOL record_crash(OSContext *c, u32 type)
{
    if (!crash.type)
    {
        crash.srr0 = c->srr0;
        crash.srr1 = c->srr1;
        crash.dar = c->dar;
        crash.dsisr = c->dsisr;
        crash.lr = c->lr;
        crash.ctr = c->ctr;
        crash.cr = c->cr;
        crash.core = c->upir;
        for (int i = 0; i < 32; i++)
            crash.gpr[i] = c->gpr[i];
        const char *name = OSGetThreadName(OSGetCurrentThread());
        snprintf(crash.thread, sizeof(crash.thread), "%s", name ? name : "?");
        crash.type = type;
    }
    c->srr0 = (u32)(uintptr_t)park_thread;   // resume the thread in a sleep loop, with the system still alive
    return TRUE;
}
bool wiiu_fastmem_dsi(OSContext *c);   // fastmem_wiiu.cpp: a fastmem access, now patched; retry it
const char *wiiu_fastmem_status();     // fastmem_wiiu.cpp: " fastmem patched N" for the status line, or ""
extern "C" void wiiu_fastmem_failure(u32 *reason, u32 *pc, u32 *insn, u32 *dar);   // wii_driver.cpp
static BOOL on_dsi(OSContext *c) { return wiiu_fastmem_dsi(c) ? TRUE : record_crash(c, OS_EXCEPTION_TYPE_DSI); }
static BOOL on_isi(OSContext *c) { return record_crash(c, OS_EXCEPTION_TYPE_ISI); }
static BOOL on_program(OSContext *c) { return record_crash(c, OS_EXCEPTION_TYPE_PROGRAM); }

static void print_crash()
{
    static const char *names[] = { "", "", "DSI (data access)", "ISI (instruction fetch)", "", "", "PROGRAM (illegal instruction)" };
    void *cg = nullptr;
    uint32_t cg_size = 0;
    OSGetCodegenVirtAddrRange(&cg, &cg_size);
    const u32 cg0 = (u32)(uintptr_t)cg;
    auto where = [&](u32 a) { return a >= cg0 && a < cg0 + cg_size ? "JIT code" : a >= 0x02000000 && a < 0x10000000 ? "app code" : "?"; };
    WLOG("[crash] %s in thread '%s' on core %u", crash.type < 7 ? names[crash.type] : "?", crash.thread, crash.core);
    WLOG("[crash] srr0 %08x (%s)  lr %08x (%s)  ctr %08x  dar %08x  dsisr %08x  srr1 %08x  cr %08x", crash.srr0,
           where(crash.srr0), crash.lr, where(crash.lr), crash.ctr, crash.dar, crash.dsisr, crash.srr1, crash.cr);
    for (int i = 0; i < 32; i += 8)
        WLOG("[crash] r%-2d %08x %08x %08x %08x %08x %08x %08x %08x", i, crash.gpr[i], crash.gpr[i + 1], crash.gpr[i + 2],
               crash.gpr[i + 3], crash.gpr[i + 4], crash.gpr[i + 5], crash.gpr[i + 6], crash.gpr[i + 7]);
    if (crash.srr0 >= cg0 && crash.srr0 < cg0 + cg_size)
    {
        const u32 *p = (const u32 *)(uintptr_t)(crash.srr0 & ~3u);
        WLOG("[crash] code at srr0: %08x %08x [%08x] %08x %08x", p[-2], p[-1], p[0], p[1], p[2]);
    }
    WLOG("[crash] SH4 pc %08x", (u32)next_pc);
    u32 fm_reason, fm_pc, fm_insn, fm_dar;
    wiiu_fastmem_failure(&fm_reason, &fm_pc, &fm_insn, &fm_dar);
    if (fm_reason)   // 1 pc outside the code cache, 2 pool full, 3 shape not recognised, 4 branch out of range
        WLOG("[crash] fastmem refused a patch: reason %u at %08x insn %08x dar %08x", fm_reason, fm_pc, fm_insn, fm_dar);
}

// a second-by-second report from a thread on another core that waits on nothing the emulator or the GX2 thread
// use, so it still speaks when either of them hangs: what the GX2 thread is doing and since when, and whether the
// emulator moves (vblanks, renders, PC)
static int watchdog(int, const char **)
{
    const char *last_stage = nullptr;
    u32 last_frames = ~0u, last_vblanks = ~0u, same = 0;
    bool crash_printed = false;
    for (int t = 0; ; t++)
    {
        OSSleepTicks(OSSecondsToTicks(1));
        if (crash.type && !crash_printed)
        {
            print_crash();
            crash_printed = true;
        }
        const char *stage = (const char *)wiiu_gx2_stage;
        const u32 frames = wiiu_gx2_frames_done, vb = wiiu_vblanks;
        same = (frames == last_frames && stage == last_stage) ? same + 1 : 0;
        // every second for the first 30 s, then every 10 s unless something stopped
        if (t < 30 || t % 10 == 0 || same > 0 || vb == last_vblanks)
            WLOG("[watch] t=%d gx2: %s, %u frames%s | emu: vblanks %u renders %u pc %08x | audio queued %u", t, stage, frames,
                   same ? " (stuck)" : "", vb, wiiu_renders, (u32)next_pc, wiiu_audio_queued());
        last_stage = stage;
        last_frames = frames;
        last_vblanks = vb;
    }
    return 0;
}
void wiiu_audio_shutdown();
extern "C" int g_fastmem_preset, g_dynarec_preset, g_idle_shinobi_preset, g_controller_type;
extern "C" int g_bcache_preset, g_jit_align_preset, g_jit_sbp_preset;
extern "C" int g_jit_ccalls_preset;   // dc/sh4/rec_v2/shil.cpp: the [CC] call-out census
extern "C" int g_frame_prof_preset;   // plugs/drkPvr/frame_prof.cpp
u64 g_wiiu_prof[10];   // wiiu/prof_wiiu.h
extern u32 wiiu_ta_sq_blocks, wiiu_ta_dma_blocks;   // render_gx2.cpp: how the game feeds the TA
extern int g_exp_scan_thread, g_exp_dma_async, g_exp_sq_fast, g_exp_aica_thread;
extern int wiiu_exp_trans_min, wiiu_exp_screen_pos, wiiu_exp_mv_open, wiiu_exp_pcw_gouraud, wiiu_depth_stats, wiiu_exp_depth_drawn, wiiu_exp_mv_cull, wiiu_exp_canvas_regs, wiiu_exp_split_convert, g_exp_arm_thread, g_exp_fm_map, wiiu_exp_fix_inf, wiiu_exp_gx2_core, wiiu_exp_frame_queue;
extern u32 wiiu_bad_vertices;
#ifdef WIIU_INSTRUMENT
extern int wiiu_sampler_mode, wiiu_sampler_how, g_prof_links;
extern OSThread *wiiu_emu_thread, *wiiu_main_thread;
void wiiu_sampler_start();
void wiiu_sampler_segment(bool fight);
void wiiu_sampler_report();
extern u32 g_ocr_max;
extern u32 wiiu_sq_other[3];
extern u32 g_wr_census_on, g_wr_area[4096];
#endif
extern int wiiu_exp_gx2_dl, wiiu_no_draw, wiiu_derail_dump, wiiu_dump_render, wiiu_exp_halfz, wiiu_exp_sort_opaque;                         // present_gx2.cpp
extern "C" int g_wiiu_hot_dump_after, g_exp_ps_ftrv, g_exp_fuse, g_exp_fm_x;   // wii_driver.cpp, decoder.cpp
bool g_wiiu_prof_on;   // wiiu.cfg prof=1: time the emulator thread's C paths for the [sh4-prof] line

struct wiiu_preset { const char *name; int *value; };
extern const wiiu_preset wiiu_presets[];

char selectedFilePath[512] = "";

static FILE *log_file;
static u32 probe_addr;
static int fastmem = -1;   // wiiu.cfg fastmem=: 0 off, 1 on, 2 Cemu test mode; -1: the picker's option decides
static bool controller_type_set;   // wiiu.cfg set g_controller_type: it wins over the picker's light gun option
volatile bool wiiu_dump_request;
int wiiu_ram_mem1;
int wiiu_picker_quit_test;
extern int wiiu_tex_full_hash;   // texture_gx2.cpp
extern float wiiu_depth_cap;     // render_gx2.cpp
extern int g_aica_lag_test, g_aica_prio;   // aica.cpp
extern u32 g_aica_ahead;   // ram_mem1=1: Dreamcast RAM in MEM1 (_vmem.cpp; a test)
static int memlat;   // memlat=1: MEM1/MEM2 load latency at the bench start (sampler_wiiu.cpp)
void wiiu_memlat();   // GamePad combo (ogc_compat.cpp)
u16 wiiu_auto_mask[16], wiiu_auto_hold;
int wiiu_auto_n, wiiu_auto_every = 120, wiiu_auto_hold_rt, wiiu_gun_aim_x = -1, wiiu_gun_aim_y = -1;
u32 wiiu_auto_hold_from;
static int run_seconds, thumb_seconds, dump_test_seconds, gx2_priority;
extern int wiiu_gpu_timing, wiiu_gx2_pipeline, wiiu_rt_width, wiiu_gx2_batch;   // present_gx2.cpp
extern int wiiu_gx2_batch_check;   // present_gx2.cpp
void wiiu_bootrom_write_dump();   // debug_wiiu.cpp
void wiiu_disc_stats();           // disc_cache_wiiu.cpp
extern int wiiu_disc_verify;
int wiiu_reset_trace;   // ccn.cpp
// wiiu.cfg bench=<first>,<last>: a repeatable speed test. Real time from one emulated vblank to another, a line every
// 600 vblanks, then the app exits. Nothing paces the emulator meanwhile (it can pass 100%) and time() is a constant
// (__wrap_time), so with no input two runs of a game play the same frames: the render and TA block counts, and
// the guest RAM checksum on the RESULT line, show whether they did, which is what makes their times comparable.
u32 wiiu_bench_first, wiiu_bench_last;
static volatile bool bench_done;
extern bool wiiu_audio_no_pacing;
extern "C" time_t __real_time(time_t *t);
extern "C" time_t __wrap_time(time_t *t)
{
    if (!wiiu_bench_last)
        return __real_time(t);
    if (t)
        *t = 1800000000;
    return 1800000000;
}
// exp_gx2_core: HOME hung about 2 times in 5 with the emulator running flat out; the spike, with nothing else running,
// never did. So the emulator parks here, between frames, before the foreground goes
static volatile bool emu_pause_req, emu_paused;
void wiiu_emu_safe_point()
{
    if (emu_pause_req)
    {
        emu_paused = true;
        while (emu_pause_req)
            OSSleepTicks(OSMillisecondsToTicks(2));
        emu_paused = false;
    }
}
void wiiu_bench_vblank()
{
    static OSTime t_first, t_seg;
    static u32 r_first, sq_first, dma_first, r_seg, sq_seg, dma_seg;
    const u32 vb = wiiu_vblanks;
    if (vb < wiiu_bench_first || bench_done)
        return;
    const OSTime now = OSGetSystemTime();
    if (vb == wiiu_bench_first)
    {
        t_first = t_seg = now;
        r_first = r_seg = wiiu_renders;
        sq_first = sq_seg = wiiu_ta_sq_blocks;
        dma_first = dma_seg = wiiu_ta_dma_blocks;
        printf("[bench] start at vblank %u\n", vb);
#ifdef WIIU_INSTRUMENT
        if (memlat)
            wiiu_memlat();
        wiiu_sampler_start();
#endif
        return;
    }
    const auto percent = [](u32 vblanks, OSTime ticks) {
        return vblanks / 59.94 / (OSTicksToMicroseconds(ticks) / 1e6) * 100.0;
    };
    if ((vb - wiiu_bench_first) % 600 == 0)
    {
        printf("[bench] to vblank %u: %.1f%%, renders +%u, sq +%u, dma +%u\n", vb, percent(600, now - t_seg),
               wiiu_renders - r_seg, wiiu_ta_sq_blocks - sq_seg, wiiu_ta_dma_blocks - dma_seg);
#ifdef WIIU_INSTRUMENT
        wiiu_sampler_segment(wiiu_ta_sq_blocks - sq_seg > 7000000);
#endif
        t_seg = now;
        r_seg = wiiu_renders;
        sq_seg = wiiu_ta_sq_blocks;
        dma_seg = wiiu_ta_dma_blocks;
    }
    if (vb >= wiiu_bench_last)
    {
        u32 sum = 0;
        const u32 *ram = (const u32 *)mem_b.data;
        for (u32 i = 0; i < RAM_SIZE / 4; i++)
            sum = (sum << 1 | sum >> 31) ^ ram[i];
        printf("[bench] bad vertices so far: %u\n", wiiu_bad_vertices);
#ifdef WIIU_INSTRUMENT
        printf("[bench] store queue, not TA: RAM %u, VRAM %u, other %u\n", wiiu_sq_other[0], wiiu_sq_other[1], wiiu_sq_other[2]);
#endif
        printf("[bench] RESULT vblanks %u-%u: %.2f s, %.2f%%; renders %u, sq %u, dma %u, ram %08x\n", wiiu_bench_first, vb,
               OSTicksToMicroseconds(now - t_first) / 1e6, percent(vb - wiiu_bench_first, now - t_first),
               wiiu_renders - r_first, wiiu_ta_sq_blocks - sq_first, wiiu_ta_dma_blocks - dma_first, sum);
#ifdef WIIU_INSTRUMENT
        if (g_wr_census_on)
            printf("[wr] on-chip RAM: highest offset %08x\n", g_ocr_max);
        if (g_wr_census_on)
            for (u32 i = 0; i < 4096; i++)
                if (g_wr_area[i] > 100000)
                    printf("[wr] slow writes at %03x00000: %u\n", i, g_wr_area[i]);
#endif
#ifdef WIIU_INSTRUMENT
        wiiu_sampler_report();
#endif
        bench_done = true;
    }
}

static int wiiu_exp_fpscr_ni;
static int audio_buffer_ms, audio_glide;
void wiiu_audio_tune(int buffer_ms, int glide);
static bool picker = true;   // wiiu.cfg picker=0: no game picker even without file= (test runs of the BIOS)
static volatile bool emu_running, exit_requested, exit_requested_by_core;

// back to the Wii U Menu; ProcUI then reports the exit to the main loop
static void request_exit()
{
    if (exit_requested)
        return;
    exit_requested = true;
    SYSLaunchMenu();
}

extern "C" void dc_flash_save_at_exit(void);   // dc/dc.cpp

// stop the SH4 (its run loop checks the flag), then give the emulator thread time to unwind through EmuMain
static void stop_emulator(OSThread *thread)
{
    if (!emu_running)
        return;
    printf("[wiiu] stopping the emulator\n");
    if (sh4_cpu.Stop)
        sh4_cpu.Stop();
    for (int i = 0; i < 30 && emu_running; i++)
        OSSleepTicks(OSMillisecondsToTicks(100));
    if (emu_running)
        printf("[wiiu] emulator thread still running after 3 s, leaving anyway\n");
    else
    {
        OSJoinThread(thread, nullptr);
        // keep BIOS settings (date, language, sound) and game flash data for the next run
        if (sd_writes)
            dc_flash_save_at_exit();
    }
}

// ---- logging -----------------------------------------------------------------------------------------

// startup progress straight to UDP and the SD card, usable before stdout is redirected
// startup progress over UDP (the SD card only gets a one-line summary per run, see main)
static void marker(const char *where)
{
    WHBLogPrintf("[nulldc] reached %s", where);
}

__attribute__((constructor(101))) static void early_init()
{
    WHBLogUdpInit();
    marker("constructors");
}

static ssize_t log_write(struct _reent *reent, void *fd, const char *ptr, size_t len)
{
    static std::string line;
    for (size_t i = 0; i < len; i++)
    {
        if (ptr[i] != '\n')
        {
            line += ptr[i];
            continue;
        }
        OSReport("%s\n", line.c_str());
        WHBLogPrintf("[nulldc] %s", line.c_str());
        if (log_file)
        {
            fprintf(log_file, "%llu %s\n", (unsigned long long)OSTicksToMilliseconds(OSGetSystemTime()), line.c_str());
            fflush(log_file);
        }
        line.clear();
    }
    return len;
}

static devoptab_t log_devoptab = { .name = "log", .write_r = log_write };

// ---- platform hooks the core calls -------------------------------------------------------------------

int os_GetFile(char *szFileName, char *szParse, u32 flags)
{
    size_t len = strlen(selectedFilePath);
    if (len == 0 || (len > 4 && strcasecmp(selectedFilePath + len - 4, ".elf") == 0))
        return false;   // no disc, or an ELF (nullDC.cpp loads those itself)
    strcpy(szFileName, selectedFilePath);
    return true;
}

double os_GetSeconds()
{
    static OSTime t0 = 0;
    OSTime now = OSGetSystemTime();
    if (t0 == 0)
        t0 = now;
    return (double)OSTicksToMicroseconds(now - t0) / 1000000.0;
}

int os_msgbox(const wchar *text, unsigned int type)
{
    printf("OS_MSGBOX: %s\n", text);
    return 0;
}

extern "C" void wiiu_input_state(u32 *drc, u32 *wpad4);   // ogc_compat.cpp

extern "C" void WiiExitToLoader(void)
{
    // The core's exit combinations (Minus+Plus and the like) call this on every input poll while they're held.
    // A brush of two buttons quit the game on a console, so only a combination held for a second counts; HOME
    // and Close is the usual way out on a Wii U anyway. The main loop does the actual shutdown.
    static OSTime first, last;
    const OSTime now = OSGetSystemTime();
    if (!first || now - last > OSMillisecondsToTicks(200))
        first = now;   // a new press (polls come every frame or so while it's held)
    last = now;
    if (now - first < OSSecondsToTicks(1) || exit_requested_by_core)
        return;
    u32 drc, wpad[4];
    wiiu_input_state(&drc, wpad);
    printf("[wiiu] exit requested by the core (combination held 1 s): GamePad %08x, KPAD %08x %08x %08x %08x\n", drc,
           wpad[0], wpad[1], wpad[2], wpad[3]);
    exit_requested_by_core = true;
}

// ---- config ------------------------------------------------------------------------------------------

static void load_wiiu_cfg()
{
    // test builds carry their own cfg in the WUHB content, which wins; the plain build reads the SD card's
    const char *path = "fs:/vol/content/wiiu.cfg";
    FILE *f = fopen(path, "r");
    if (!f)
        f = fopen(path = NULLDC_HOME "wiiu.cfg", "r");
    if (!f)
        return;
    printf("[wiiu] settings from %s\n", path);
    char line[512];
    while (fgets(line, sizeof(line), f))
    {
        line[strcspn(line, "\r\n")] = 0;
        char *eq = strchr(line, '=');
        if (line[0] == '#' || !eq)
            continue;
        *eq = 0;
        const char *key = line, *value = eq + 1;
        if (!strcmp(key, "home"))
        {
            snprintf(home, sizeof(home), "%s", value);
            sd_writes = strncmp(home, "fs:/vol/external01/", 19) == 0;
        }
        else if (!strcmp(key, "file"))
            snprintf(selectedFilePath, sizeof(selectedFilePath), "%s%s", value[0] == '/' || strchr(value, ':') ? "" : home, value);
        else if (!strcmp(key, "probe"))
            probe_addr = strtoul(value, nullptr, 16);
        else if (!strcmp(key, "run_seconds"))
            run_seconds = atoi(value);
        else if (!strcmp(key, "thumb"))
            thumb_seconds = atoi(value);
        else if (!strcmp(key, "picker"))
            picker = atoi(value) != 0;
        else if (!strcmp(key, "bench"))
        {
            sscanf(value, "%u,%u", &wiiu_bench_first, &wiiu_bench_last);
            wiiu_audio_no_pacing = true;
        }
        else if (!strcmp(key, "exp_dma_async"))
            g_exp_dma_async = atoi(value);
        else if (!strcmp(key, "exp_scan_thread"))
            g_exp_scan_thread = atoi(value);
        else if (!strcmp(key, "derail_dump"))
            wiiu_derail_dump = atoi(value);
        else if (!strcmp(key, "dump_render"))
            wiiu_dump_render = atoi(value);
        else if (!strcmp(key, "exp_halfz"))
            wiiu_exp_halfz = atoi(value);
        else if (!strcmp(key, "exp_fpscr_ni"))
            wiiu_exp_fpscr_ni = atoi(value);
        else if (!strcmp(key, "exp_sq_fast"))
            g_exp_sq_fast = atoi(value);
        else if (!strcmp(key, "exp_aica_thread"))
            g_exp_aica_thread = atoi(value);
        else if (!strcmp(key, "exp_sort_opaque"))
            wiiu_exp_sort_opaque = atoi(value);
        else if (!strcmp(key, "exp_fm_map"))
            g_exp_fm_map = atoi(value);
#ifdef WIIU_INSTRUMENT
        else if (!strcmp(key, "sampler"))
            wiiu_sampler_mode = atoi(value);
        else if (!strcmp(key, "sampler_how"))
            wiiu_sampler_how = atoi(value);
        else if (!strcmp(key, "prof_links"))
            g_prof_links = atoi(value);
        else if (!strcmp(key, "wr_census"))
            g_wr_census_on = atoi(value);
#endif
        else if (!strcmp(key, "audio_buffer"))
            audio_buffer_ms = atoi(value);
        else if (!strcmp(key, "audio_glide"))
            audio_glide = atoi(value);
        else if (!strcmp(key, "exp_fix_inf"))
            wiiu_exp_fix_inf = atoi(value);
        else if (!strcmp(key, "exp_gx2_core"))
            wiiu_exp_gx2_core = atoi(value);
        else if (!strcmp(key, "exp_frame_queue"))
            wiiu_exp_frame_queue = atoi(value);
        else if (!strcmp(key, "exp_arm_thread"))
            g_exp_arm_thread = atoi(value);
        else if (!strcmp(key, "auto_press") || !strcmp(key, "auto_hold"))
        {
            // buttons by name, comma separated: auto_press=start,a presses them in turn every auto_every vblanks;
            // auto_hold=rt,a holds them from vblank auto_hold_from on
            const bool hold = key[5] == 'h';
            char list[128];
            snprintf(list, sizeof(list), "%s", value);
            for (char *t = strtok(list, ","); t; t = strtok(nullptr, ","))
            {
                static const struct { const char *n; u16 m; } names[] = { { "a", 1 << 2 }, { "b", 1 << 1 }, { "x", 1 << 10 },
                    { "y", 1 << 9 }, { "start", 1 << 3 }, { "up", 1 << 4 }, { "down", 1 << 5 }, { "left", 1 << 6 },
                    { "right", 1 << 7 }, { "none", 0 } };
                if (hold && !strcmp(t, "rt"))
                    wiiu_auto_hold_rt = 1;
                for (const auto &n : names)
                    if (!strcmp(t, n.n))
                    {
                        if (hold)
                            wiiu_auto_hold |= n.m;
                        else if (wiiu_auto_n < 16)
                            wiiu_auto_mask[wiiu_auto_n++] = n.m;
                    }
            }
        }
        else if (!strcmp(key, "depth_cap"))
            wiiu_depth_cap = atof(value);
        else if (!strcmp(key, "tex_full_hash"))
            wiiu_tex_full_hash = atoi(value);
        else if (!strcmp(key, "picker_quit_test"))
            wiiu_picker_quit_test = atoi(value);
        else if (!strcmp(key, "aica_ahead"))
            g_aica_ahead = atoi(value);
        else if (!strcmp(key, "aica_prio"))
            g_aica_prio = atoi(value);
        else if (!strcmp(key, "aica_lag_test"))
            g_aica_lag_test = atoi(value);
        else if (!strcmp(key, "ram_mem1"))
            wiiu_ram_mem1 = atoi(value);
        else if (!strcmp(key, "memlat"))
            memlat = atoi(value);
        else if (!strcmp(key, "depth_stats"))
            wiiu_depth_stats = atoi(value);
        else if (!strcmp(key, "exp_depth_drawn"))
            wiiu_exp_depth_drawn = atoi(value);
        else if (!strcmp(key, "exp_pcw_gouraud"))
            wiiu_exp_pcw_gouraud = atoi(value);
        else if (!strcmp(key, "exp_trans_min"))
            wiiu_exp_trans_min = atoi(value);
        else if (!strcmp(key, "exp_screen_pos"))
            wiiu_exp_screen_pos = atoi(value);
        else if (!strcmp(key, "exp_mv_open"))
            wiiu_exp_mv_open = atoi(value);
        else if (!strcmp(key, "exp_mv_cull"))
            wiiu_exp_mv_cull = atoi(value);
        else if (!strcmp(key, "gun_aim"))
            sscanf(value, "%d,%d", &wiiu_gun_aim_x, &wiiu_gun_aim_y);
        else if (!strcmp(key, "auto_every"))
            wiiu_auto_every = atoi(value) > 6 ? atoi(value) : 7;
        else if (!strcmp(key, "auto_hold_from"))
            wiiu_auto_hold_from = atoi(value);
        else if (!strcmp(key, "exp_canvas_regs"))
            wiiu_exp_canvas_regs = atoi(value);
        else if (!strcmp(key, "exp_split_convert"))
            wiiu_exp_split_convert = atoi(value);
        else if (!strcmp(key, "no_draw"))
            wiiu_no_draw = atoi(value);
        else if (!strcmp(key, "exp_gx2_dl"))
            wiiu_exp_gx2_dl = atoi(value);
        else if (!strcmp(key, "exp_fm_x"))
            g_exp_fm_x = atoi(value);
        else if (!strcmp(key, "exp_fuse"))
            g_exp_fuse = atoi(value);
        else if (!strcmp(key, "exp_ps_ftrv"))
            g_exp_ps_ftrv = atoi(value);
        else if (!strcmp(key, "hot_after"))
            g_wiiu_hot_dump_after = atoi(value);   // hotblocks: disassemble only blocks hot after this many seconds
        else if (!strcmp(key, "prof"))
            g_wiiu_prof_on = atoi(value) != 0;
        else if (!strcmp(key, "fastmem"))
            fastmem = atoi(value);
        else if (!strcmp(key, "jit_ccalls"))
            g_jit_ccalls_preset = atoi(value);
        else if (!strcmp(key, "reset_trace"))
            wiiu_reset_trace = atoi(value);   // log JIT compiles after a game turns its I-cache off (soft reset)
        else if (!strcmp(key, "disc_verify"))
            wiiu_disc_verify = atoi(value);   // also read each disc sector the old way and log differences
        else if (!strcmp(key, "gx2_priority"))
            gx2_priority = atoi(value);   // the main (GX2) thread's priority once the emulator thread runs
        else if (!strcmp(key, "rt_width"))
        {
            wiiu_rt_width = atoi(value);   // the 3D render target's width (4:3): 960 by default, 640 native
            rt_width_set = true;
        }
        else if (!strcmp(key, "gx2_batch_check"))
            wiiu_gx2_batch_check = atoi(value);   // compare batched and unbatched drawing every 60th frame
        else if (!strcmp(key, "gx2_batch"))
            wiiu_gx2_batch = atoi(value);   // 0: draw each strip on its own
        else if (!strcmp(key, "gx2_pipeline"))
            wiiu_gx2_pipeline = atoi(value);   // 0: convert each frame in its own vsync (no overlap with the flip)
        else if (!strcmp(key, "gpu_timing"))
            wiiu_gpu_timing = atoi(value);   // wait for the GPU after the 3D scene and report its time
        else if (!strcmp(key, "dump_test"))
            dump_test_seconds = atoi(value);   // run the boot-ROM write dump once after n seconds, to test it
        else if (!strcmp(key, "frame_prof"))
            g_frame_prof_preset = atoi(value);   // per-frame tail profiler (plugs/drkPvr/frame_prof.h): stutters
        else if (!strcmp(key, "sdlog") && atoi(value))
            log_file = fopen(NULLDC_HOME "log.txt", "w");   // full log on the SD card too (many small writes)
        else
        {
            bool found = false;
            for (const wiiu_preset *p = wiiu_presets; p->name; p++)
                if (!strcmp(p->name, key))
                {
                    *p->value = atoi(value);
                    found = true;
                }
            controller_type_set |= !strcmp(key, "g_controller_type");
            printf("[wiiu] cfg %s=%s%s\n", key, value, found ? "" : " (unknown key, ignored)");
        }
    }
    fclose(f);
}

// ---- end of run ----------------------------------------------------------------------------------------

static void write_summary(int seconds)
{
    printf("[wiiu] NULLDC_DONE after %d s\n", seconds);
    if (!sd_writes)
        return;
    if (FILE *f = fopen(NULLDC_HOME "last_run.txt", "w"))
    {
        fprintf(f, "file '%s' dynarec %d: %d s, vblanks %u renders %u", selectedFilePath, g_dynarec_preset, seconds,
                wiiu_vblanks, wiiu_renders);
        if (probe_addr && wiiu_vblanks && emu_running)
            fprintf(f, ", probe %08x %08x %08x %08x", _vmem_ReadMem32(probe_addr), _vmem_ReadMem32(probe_addr + 4),
                    _vmem_ReadMem32(probe_addr + 8), _vmem_ReadMem32(probe_addr + 12));
        fprintf(f, "\n");
        fclose(f);
    }
}

// ---- emulator thread ---------------------------------------------------------------------------------

static int emu_thread(int argc, const char **argv)
{
    static char arg0[] = "nulldc";
    static char *args[] = { arg0, nullptr };
    printf("[wiiu] EmuMain on core %u\n", OSGetCoreId());
    if (wiiu_exp_fpscr_ni)
        __asm__ volatile("mtfsb1 29");   // FPSCR NI: denormals flush to zero instead of trapping to slow handling
    int rv = EmuMain(1, args);
    printf("[wiiu] EmuMain returned %d\n", rv);
    emu_running = false;
    return rv;
}

// exp_gx2_core: WHB's foreground callbacks run on the main thread, away from GX2, and HOME hung that way. So they're
// dropped, and this thread shuts graphics down itself before the release and starts it again after
int wiiu_exp_gx2_core;
bool wiiu_gfx_down;   // shut down for the background: wiiu_gx2_shutdown leaves it
static volatile bool proc_running = true, gfx_want_pause, gfx_parked, app_done;
static uint32_t gfx_release(void *)
{
    emu_pause_req = true;
    for (int i = 0; i < 500 && !emu_paused && emu_running; i++)
        OSSleepTicks(OSMillisecondsToTicks(1));
    printf("[wiiu] foreground release: emulator %s\n", emu_paused ? "parked" : "not parked");
    gfx_want_pause = true;
    for (int i = 0; i < 2000 && !gfx_parked; i++)
        OSSleepTicks(OSMillisecondsToTicks(1));
    printf("[wiiu] foreground release: GX2 thread %s\n", gfx_parked ? "parked" : "not parked yet");
    return 0;
}
static uint32_t gfx_acquire(void *)
{
    gfx_want_pause = false;
    emu_pause_req = false;
    return 0;
}
static void gfx_callbacks()
{
    ProcUIClearCallbacks();
    ProcUIRegisterCallback(PROCUI_CALLBACK_RELEASE, gfx_release, nullptr, 200);
    ProcUIRegisterCallback(PROCUI_CALLBACK_ACQUIRE, gfx_acquire, nullptr, 50);
}
// instead of WHBProcIsRunning() wherever GX2 is driven
bool wiiu_app_running()
{
    if (!wiiu_exp_gx2_core)
        return WHBProcIsRunning();
    if (gfx_want_pause)
    {
        WHBGfxShutdown();
        wiiu_gfx_down = true;
        gfx_parked = true;
        printf("[wiiu] GX2 thread parked\n");
        while (gfx_want_pause && proc_running)
            OSSleepTicks(OSMillisecondsToTicks(5));
        if (!proc_running)
            emu_pause_req = false;   // closing: the emulator has to run to stop
        if (proc_running)
        {
            WHBGfxInit();
            wiiu_gfx_down = false;
            gfx_callbacks();
            printf("[wiiu] GX2 back\n");
        }
        gfx_parked = false;
    }
    return proc_running;
}
static int app_main();
static int app_thread(int, const char **) { return app_main(); }

int main(int argc, char **argv)
{
    marker("main");
    WHBProcInit();
    marker("procui");
    devoptab_list[STD_OUT] = &log_devoptab;
    devoptab_list[STD_ERR] = &log_devoptab;
    setvbuf(stdout, nullptr, _IONBF, 0);
    // the Shinobi idle-loop rule stalls the BIOS on its boot logo with the JIT (seen in Cemu); wiiu.cfg can
    // turn it back on with g_idle_shinobi_preset=1
    g_idle_shinobi_preset = 0;
    // console-tested defaults (Soulcalibur fights 86% -> 141% in bench, 99% in real play); wiiu.cfg can still turn any of
    // them off. The GX2 and sound threads go on a core the JIT isn't on: 0 on a console, 2 in Cemu, whose codegen core is 0
    // The options page can turn them off (config.txt, read here because the threads start before the picker runs).
    {
        bool gfx_core = true, sound_core = true, jit_fast = true, smooth = true, code_check = true;
        int buffer = 100;
        wiiu_picker_speed_options(home, &gfx_core, &sound_core, &jit_fast, &smooth, &code_check, &buffer);
        const int other = OSGetCodegenCore() == 0 ? 2 : 1;   // 1: core 0, 2: core 2
        wiiu_exp_gx2_core = gfx_core ? other : 0;
        wiiu_exp_split_convert = gfx_core;
        g_exp_aica_thread = sound_core ? other : 0;
        g_exp_fm_x = g_exp_ps_ftrv = g_exp_sq_fast = g_bcache_preset = jit_fast;
        g_jit_align_preset = jit_fast ? 0 : 1;
        wiiu_exp_frame_queue = smooth;
        g_jit_sbp_preset = code_check ? 3 : 1;   // 3: MvC2's module cache too
        audio_buffer_ms = buffer;
        audio_glide = 1;
        printf("[wiiu] speed options: graphics core %d, sound core %d, faster code %d, smooth frames %d, code check %d, "
               "sound buffer %d\n", gfx_core, sound_core, jit_fast, smooth, code_check, buffer ? buffer : 46);
    }
    load_wiiu_cfg();
    if (wiiu_exp_gx2_core)
    {
        // exp_gx2_core (1: core 0, 2: core 2): GX2 lives on another thread, which does its init too, and this thread
        // only runs ProcUI. It takes the GX2 thread's share off core 1, the JIT's core (scratch/gx2core-spike)
        static OSThread app;
        const u32 size = 1024 * 1024;
        u8 *stack = (u8 *)memalign(16, size);
        OSCreateThread(&app, app_thread, 0, nullptr, stack + size, size, OSGetThreadPriority(OSGetCurrentThread()),
                       wiiu_exp_gx2_core == 2 ? OS_THREAD_ATTRIB_AFFINITY_CPU2 : OS_THREAD_ATTRIB_AFFINITY_CPU0);
        OSSetThreadName(&app, "nullDC GX2");
        OSResumeThread(&app);
        while (WHBProcIsRunning())
            OSSleepTicks(OSMillisecondsToTicks(10));
        proc_running = false;
        printf("[wiiu] exit: ProcUI says exit\n");
        // no OSJoinThread: the GX2 thread's exit hung once (console, 2026-11-06), and that left the app stuck on the way
        // back to the menu. Wait a while, then leave regardless
        for (int i = 0; i < 3000 && !app_done; i++)
            OSSleepTicks(OSMillisecondsToTicks(1));
        printf("[wiiu] exit: GX2 thread %s\n", app_done ? "finished" : "NOT FINISHED, leaving anyway");
        WHBProcShutdown();
        return 0;
    }
    return app_main();
}

static int app_main()
{
    bool video = wiiu_gx2_init();
    if (wiiu_exp_gx2_core)
        gfx_callbacks();
    marker(video ? "gx2" : "gx2 failed");
    // no game named: let the user choose one
    if (video && !selectedFilePath[0] && picker)
    {
        wiiu_kpad_init();
        if (!wiiu_pick_game(home, selectedFilePath, sizeof(selectedFilePath)))
        {
            // quit from the picker (+ and -) or HOME, Close: ask for the Wii U Menu if nobody has yet, then keep serving
            // ProcUI (the HOME and release handling need GX2 up) until it says exit. Shutting GX2 down at once, with
            // nothing asking for the menu, left the app frozen and HOME dead (console, 2026-11-08)
            printf("[wiiu] exit: left the picker, asking for the Wii U Menu\n");
            if (wiiu_app_running())
                request_exit();
            while (wiiu_app_running())
                OSSleepTicks(OSMillisecondsToTicks(10));
            wiiu_gx2_shutdown();
            app_done = true;
            if (!wiiu_exp_gx2_core)
                WHBProcShutdown();
            return 0;
        }
    }
    wiiu_audio_tune(audio_buffer_ms, audio_glide);
    wiiu_audio_init();

    void *codegen_area = nullptr;
    uint32_t codegen_size = 0;
    OSGetCodegenVirtAddrRange(&codegen_area, &codegen_size);
    printf("[wiiu] nullDC4Wii native Wii U build; codegen %p size 0x%x core %u\n", codegen_area, codegen_size, OSGetCodegenCore());

    marker("logging");
    extern u8 *CodeCache;
    CodeCache = (u8 *)wiiu_codegen_alloc(CODE_SIZE, "sh4 jit");
    if (sd_writes)
    {
        mkdir("fs:/vol/external01/wiiu", 0777);
        mkdir(NULLDC_HOME, 0777);
        mkdir(NULLDC_HOME "data", 0777);
    }
    SetApplicationPath(home);
    printf("[wiiu] home %s%s\n", home, sd_writes ? "" : " (nothing written to the SD card)");
    {
        extern u32 GetRTC_now();
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        printf("[wiiu] clock: time %lld = %04d-%02d-%02d %02d:%02d, isdst %d, DC RTC %08x\n", (long long)now,
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_isdst, GetRTC_now());
    }
    // the picker's options (config.txt), unless wiiu.cfg (test builds) set them: fastmem (fastmem_wiiu.cpp; 2 is the
    // Cemu test mode) and the light gun (drkMapleLightGun.cpp) on every port
    {
        bool opt_fastmem = true, opt_lightgun = false, opt_keep_pitch = true;
        int opt_width = 960;
        if (sd_writes)
            wiiu_picker_options(home, &opt_fastmem, &opt_lightgun, &opt_width, &opt_keep_pitch);
        g_fastmem_preset = fastmem >= 0 ? fastmem : opt_fastmem;
        if (!controller_type_set)
            g_controller_type = opt_lightgun ? 1 : 0;
        if (!rt_width_set)
            wiiu_gx2_set_render_width(opt_width);
        wiiu_audio_min_speed = opt_keep_pitch ? 0.97f : 0.60f;
        printf("[wiiu] options: fastmem %d%s, light gun %s%s, 3D %dx%d%s, sound when slow: %s\n", g_fastmem_preset,
               fastmem >= 0 ? " (wiiu.cfg)" : "", g_controller_type == 1 ? "on" : "off",
               controller_type_set ? " (wiiu.cfg)" : "", rt_width_set ? wiiu_rt_width : opt_width,
               (rt_width_set ? wiiu_rt_width : opt_width) * 3 / 4, rt_width_set ? " (wiiu.cfg)" : "",
               opt_keep_pitch ? "keeps pitch" : "slows down");
    }
    printf("[wiiu] file '%s', probe 0x%08x, run_seconds %d\n", selectedFilePath, probe_addr, run_seconds);

    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_DSI, on_dsi);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_ISI, on_isi);
    OSSetExceptionCallbackEx(OS_EXCEPTION_MODE_GLOBAL_ALL_CORES, OS_EXCEPTION_TYPE_PROGRAM, on_program);
    // the watchdog runs on a core neither the emulator nor (normally) the GX2 thread uses
    {
        static OSThread watch;
        const u32 wsize = 64 * 1024;
        u8 *wstack = (u8 *)memalign(16, wsize);
        const u32 other = OSGetCodegenCore() == 2 ? 0 : 2;
        if (OSCreateThread(&watch, watchdog, 0, nullptr, wstack + wsize, wsize, 10,
                           other == 0 ? OS_THREAD_ATTRIB_AFFINITY_CPU0 : OS_THREAD_ATTRIB_AFFINITY_CPU2))
        {
            OSSetThreadName(&watch, "nullDC watchdog");
            OSResumeThread(&watch);
        }
    }

    marker("emulator thread");
    // the JIT's code may only run on the codegen core, so the whole emulator runs there
    static OSThread thread;
#ifdef WIIU_INSTRUMENT
    wiiu_emu_thread = &thread;
    wiiu_main_thread = OSGetCurrentThread();
#endif
    const u32 stack_size = 4 * 1024 * 1024;
    u8 *stack = (u8 *)memalign(16, stack_size);
    u32 core = OSGetCodegenCore();
    OSThreadAttributes affinity = core == 0 ? OS_THREAD_ATTRIB_AFFINITY_CPU0 :
                                  core == 2 ? OS_THREAD_ATTRIB_AFFINITY_CPU2 : OS_THREAD_ATTRIB_AFFINITY_CPU1;
    emu_running = true;
    // one step below the main thread, which shares the codegen core on a console: the emulator never yields,
    // and CafeOS doesn't time-slice threads of equal priority, so at the same priority the main thread (video,
    // status, HOME) never ran again. The main thread mostly waits for VSync, so the emulator still gets the core.
    const int emu_priority = OSGetThreadPriority(OSGetCurrentThread()) + 1;
    printf("[wiiu] emulator thread priority %d (main %d)\n", emu_priority, emu_priority - 1);
    if (!OSCreateThread(&thread, emu_thread, 0, nullptr, stack + stack_size, stack_size, emu_priority, affinity))
    {
        printf("[wiiu] could not create the emulator thread\n");
        emu_running = false;
    }
    else
    {
        OSSetThreadName(&thread, "nullDC");
        OSResumeThread(&thread);
        if (gx2_priority)
        {
            OSSetThreadPriority(OSGetCurrentThread(), gx2_priority);
            printf("[wiiu] main (GX2) thread priority now %d\n", OSGetThreadPriority(OSGetCurrentThread()));
        }
    }
    OSTime start = OSGetSystemTime();
    int last_report = -1;
    bool done = false;
    while (wiiu_app_running())
    {
        if (video)
            wiiu_gx2_frame();   // waits for VSync
        else
            OSSleepTicks(OSMillisecondsToTicks(100));
        int seconds = (int)OSTicksToSeconds(OSGetSystemTime() - start);
        if (seconds != last_report && !done)
        {
            last_report = seconds;
            // the memory map exists once the emulator runs, which the first VBlank shows
            if (probe_addr && emu_running && wiiu_vblanks)
                printf("[wiiu] t=%d vblanks %u renders %u probe %08x %08x %08x %08x\n", seconds, wiiu_vblanks, wiiu_renders,
                       _vmem_ReadMem32(probe_addr), _vmem_ReadMem32(probe_addr + 4), _vmem_ReadMem32(probe_addr + 8),
                       _vmem_ReadMem32(probe_addr + 12));
            else
                printf("[wiiu] t=%d vblanks %u renders %u ta lists %u (sq %u dma %u) pc %08x%s\n", seconds, wiiu_vblanks,
                       wiiu_renders, wiiu_ta_lists, wiiu_ta_sq_blocks, wiiu_ta_dma_blocks, (u32)next_pc, wiiu_fastmem_status());
                if (seconds % 5 == 0)
                    wiiu_disc_stats();
#ifdef WIIU_INSTRUMENT
                if (seconds % 5 == 0 && g_prof_links)
                {
                    // prof_links: block ends per emulated second, by kind, and lookups that missed into C
                    extern u32 g_cnt_exit[8], g_cnt_getcode, g_cnt_dyn;
                    static u32 last[10], last_vb;
                    const u32 now[10] = { g_cnt_exit[0], g_cnt_exit[1], g_cnt_exit[2], g_cnt_exit[3], g_cnt_exit[4],
                                          g_cnt_exit[5], g_cnt_getcode, g_cnt_dyn, g_cnt_exit[6], g_cnt_exit[7] };
                    const float s = (wiiu_vblanks - last_vb) / 59.94f;
                    if (s > 0.5f)
                        printf("[exits] per emulated s: cond %.0fK, static %.0fK, dyn call %.0fK, dyn jump %.0fK, ret %.0fK, intr %.0fK; "
                               "dispatch %.0fK (past inline cache: calls+jumps %.0fK, returns %.0fK), table misses %.0fK\n", (now[0] - last[0]) / s / 1e3f, (now[1] - last[1]) / s / 1e3f,
                               (now[2] - last[2]) / s / 1e3f, (now[3] - last[3]) / s / 1e3f, (now[4] - last[4]) / s / 1e3f,
                               (now[5] - last[5]) / s / 1e3f, (now[7] - last[7]) / s / 1e3f, (now[8] - last[8]) / s / 1e3f,
                               (now[9] - last[9]) / s / 1e3f, (now[6] - last[6]) / s / 1e3f);
                    memcpy(last, now, sizeof(last));
                    last_vb = wiiu_vblanks;
                }
#endif
        }
        if (video && wiiu_dump_request)
        {
            wiiu_dump_request = false;
            printf("[wiiu] frame dump requested at vblank %u\n", wiiu_vblanks);
            wiiu_dump_render = wiiu_renders + 1;
            wiiu_gx2_thumbnail();
        }
        if (video && thumb_seconds && seconds >= thumb_seconds)
        {
            wiiu_gx2_thumbnail();
            thumb_seconds = 0;
        }
        if (dump_test_seconds && seconds >= dump_test_seconds)
        {
            wiiu_bootrom_write_dump();   // from this thread, so the SH4 state is only roughly current
            dump_test_seconds = 0;
        }
        if (!done && (!emu_running || exit_requested_by_core || bench_done || (run_seconds && seconds >= run_seconds)))
        {
            done = true;
            write_summary(seconds);
            stop_emulator(&thread);
            request_exit();
        }
    }
    // HOME -> Close (or the request above) ends up here
    if (!done)
        write_summary((int)OSTicksToSeconds(OSGetSystemTime() - start));
    stop_emulator(&thread);
    printf("[wiiu] exit: emulator stopped\n");
    wiiu_audio_shutdown();
    printf("[wiiu] exit: audio down\n");
    wiiu_gx2_shutdown();
    printf("[wiiu] exit: graphics down\n");
    if (log_file)
        fclose(log_file);
    app_done = true;
    if (!wiiu_exp_gx2_core)
        WHBProcShutdown();
    return 0;
}
