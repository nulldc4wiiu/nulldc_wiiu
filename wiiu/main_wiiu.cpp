// Native Wii U entry point, replacing wii/main.cpp (the libogc menu and boot flow).
//
// Files live in sd:/wiiu/nulldc/: data/dc_boot.bin and data/dc_flash.bin (required by the core), nullDC.cfg,
// config.txt (the picker's options) and wiiu.cfg (optional), read once at start:
//   file=<path>       disc image or .elf to boot instead of showing the game picker (relative to sd:/wiiu/nulldc/)
//   picker=0          boot the BIOS (the Dreamcast menu) when no file= is given, instead of showing the game picker
//   fastmem=0|1       fast memory access, overriding the options page
//   rt_width=<n>      the 3D render target's width, 4:3 (default 960; 640 is the Dreamcast's own)
//   audio_buffer=<ms> the sound queue's target, overriding the options page
//   exp_*             the speed switches the options page sets: exp_gx2_core and exp_aica_thread take a core
//                     (0: the emulator's own, 1: core 0, 2: core 2), the others 0|1 (see README-WiiU.md)
//   g_<name>=<int>    override any int preset from wii/main.cpp, e.g. g_dynarec_preset=0 for the interpreter
//   sdlog=1           write the log to sd:/wiiu/nulldc/log.txt as well
// Output (stdout/stderr) goes to OSReport; the SD card gets one summary line per run in last_run.txt.
// The emulator runs on the codegen core, where the SH4 JIT's code has to execute.
#include <coreinit/codegen.h>
#include <coreinit/core.h>
#include <coreinit/debug.h>
#include <coreinit/exception.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sys/iosupport.h>
#include <sys/stat.h>
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
static const char home[] = NULLDC_HOME;   // the BIOS files (data/) and relative file= paths

extern void SetApplicationPath(const char *path);
extern int EmuMain(int argc, char *argv[]);
extern u32 wiiu_vblanks, wiiu_renders, wiiu_ta_lists;
bool wiiu_gx2_init();
void wiiu_gx2_frame();
void wiiu_gx2_shutdown();
void wiiu_audio_init();
bool wiiu_pick_game(const char *home, char *path, size_t size);
void wiiu_picker_speed_options(const char *home, bool *gfx_core, bool *sound_core, bool *jit_fast, bool *smooth,
                               bool *code_check, int *sound_buffer_ms, int *disc_kbps);
void wiiu_picker_options(const char *home, bool *fastmem, bool *lightgun, int *render_width, bool *keep_pitch);
void wiiu_gx2_set_render_width(int width);   // present_gx2.cpp
extern float wiiu_audio_min_speed;          // audio_wiiu.cpp
static bool rt_width_set;                    // wiiu.cfg set rt_width: it wins over the picker's option
void wiiu_kpad_init();
extern volatile const char *wiiu_gx2_stage;
extern volatile u32 wiiu_gx2_frames_done;
extern "C" u32 wiiu_audio_queued();

// straight to OSReport, not through stdio, whose lock a crashed or stuck thread may hold
#define WLOG(fmt, ...) OSReport("[nulldc] " fmt "\n", ##__VA_ARGS__)

// ---- crash report ------------------------------------------------------------------------------------
// A fault in a CafeOS app normally stops the whole console on a black screen. These handlers note where it
// happened and park the faulting thread instead, so the watchdog (another core) can report it.
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
extern "C" int g_gdrom_delay;   // presets_generated.cpp: GD-DMA pacing in KB/s (dc/gdrom/gdromv3.cpp), the options page's Disc speed
extern u32 wiiu_ta_sq_blocks, wiiu_ta_dma_blocks;   // render_gx2.cpp: how the game feeds the TA
extern int g_exp_sq_fast, g_exp_aica_thread;
extern int wiiu_mv_bbox, wiiu_mv_bbox_check;   // present_gx2.cpp
extern int wiiu_exp_tr_depth;   // present_gx2.cpp: auto-sorted translucent polygons with depth >= and no depth writes
extern int wiiu_exp_split_convert, wiiu_exp_gx2_core, wiiu_exp_frame_queue;
extern "C" int g_exp_ps_ftrv, g_exp_fm_x, g_exp_jit_tlive, g_exp_jit_fconst, g_exp_fipr_split;   // wii_driver.cpp
extern "C" int g_exp_sq_inline;   // render_gx2.cpp (J1: the JIT's inline TA store-queue write)

struct wiiu_preset { const char *name; int *value; };
extern const wiiu_preset wiiu_presets[];

char selectedFilePath[512] = "";

static FILE *log_file;
static int fastmem = -1;   // wiiu.cfg fastmem=: 0 off, 1 on; -1: the picker's option decides
static bool controller_type_set;   // wiiu.cfg set g_controller_type: it wins over the picker's light gun option
extern int wiiu_rt_width;   // present_gx2.cpp
void wiiu_disc_stats();     // disc_cache_wiiu.cpp
// exp_gx2_core: the emulator parks here, between frames, before the foreground is released, so GX2 can shut down
// cleanly (with the emulator running flat out, HOME could hang otherwise)
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
static int audio_buffer_ms;
void wiiu_audio_tune(int buffer_ms);
static bool picker = true;   // wiiu.cfg picker=0: no game picker even without file= (boots the BIOS)
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
    void wiiu_disc_quiesce();
    wiiu_disc_quiesce();
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
        dc_flash_save_at_exit();
    }
}

// ---- logging -----------------------------------------------------------------------------------------

// startup progress straight to OSReport, usable before stdout is redirected
static void marker(const char *where)
{
    OSReport("[nulldc] reached %s\n", where);
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
    const char *path = NULLDC_HOME "wiiu.cfg";
    FILE *f = fopen(path, "r");
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
        if (!strcmp(key, "file"))
            snprintf(selectedFilePath, sizeof(selectedFilePath), "%s%s", value[0] == '/' || strchr(value, ':') ? "" : home, value);
        else if (!strcmp(key, "picker"))
            picker = atoi(value) != 0;
#ifdef WIIU_INSTRUMENT
        else if (!strcmp(key, "mv_bbox_check"))
            wiiu_mv_bbox_check = atoi(value);   // mv_bbox on/off compared pixel for pixel (1: 1:1, 2: render-target scale)
#endif
        else if (!strcmp(key, "mv_bbox"))
            wiiu_mv_bbox = atoi(value);   // modifier volume stencil quads bounded to each volume's box (present_gx2.cpp)
        else if (!strcmp(key, "tr_depth"))
            wiiu_exp_tr_depth = atoi(value);   // translucent depth as Flycast in auto-sort mode (DOA2 character select)
        // frame-rate work (console fr1/fr2): intake on cores 2 and 0
        else if (!strcmp(key, "depth_inline"))
            { extern int wiiu_exp_depth_inline; wiiu_exp_depth_inline = atoi(value); }   // depth scale gathered per strip
        else if (!strcmp(key, "ta_dcbz"))
            { extern int wiiu_exp_ta_dcbz; wiiu_exp_ta_dcbz = atoi(value); }   // zero the decoded vertices' new cache lines
        else if (!strcmp(key, "ta_keyed"))
            { extern int wiiu_exp_ta_keyed; wiiu_exp_ta_keyed = atoi(value); }   // a render draws its own buffer's list (DOA2)
        else if (!strcmp(key, "jit_tlive"))
            g_exp_jit_tlive = atoi(value);   // Review 3 K1: T kept in r31 until a delay-slot branch reads it
        else if (!strcmp(key, "jit_fconst"))
            g_exp_jit_fconst = atoi(value);   // Review 3 K2: float immediates from a constant pool, not stw+lfs
        else if (!strcmp(key, "fipr_split"))
            g_exp_fipr_split = atoi(value);   // P3: fipr as two product chains (shorter dependency chain)
        else if (!strcmp(key, "sq_guard"))
            { extern int g_exp_sq_guard; g_exp_sq_guard = atoi(value); }   // J2: compile-time store-queue test for pair-float stores (1, 2)
        else if (!strcmp(key, "sq_inline"))
            g_exp_sq_inline = atoi(value);   // J1: TA store-queue blocks written by the JIT (2: vertex starts counted inline too)
        else if (!strcmp(key, "gd_async"))
            { extern int g_exp_gd_async; g_exp_gd_async = atoi(value); }   // H1 v2: paced GD-DMA waits for the SD read-ahead (with g_gdrom_delay)
        else if (!strcmp(key, "sq_ram"))
            { extern int g_exp_sq_ram; g_exp_sq_ram = atoi(value); }   // J1b: store-queue blocks to RAM copied by the JIT, no C call
        else if (!strcmp(key, "fastmem"))
            fastmem = atoi(value) != 0;
        else if (!strcmp(key, "rt_width"))
        {
            wiiu_rt_width = atoi(value);   // the 3D render target's width (4:3): 960 by default, 640 native
            rt_width_set = true;
        }
        else if (!strcmp(key, "audio_buffer"))
            audio_buffer_ms = atoi(value);
        else if (!strcmp(key, "exp_sq_fast"))
            g_exp_sq_fast = atoi(value);
        else if (!strcmp(key, "exp_aica_thread"))
            g_exp_aica_thread = atoi(value);
        else if (!strcmp(key, "exp_gx2_core"))
            wiiu_exp_gx2_core = atoi(value);
        else if (!strcmp(key, "exp_frame_queue"))
            wiiu_exp_frame_queue = atoi(value);
        else if (!strcmp(key, "exp_split_convert"))
            wiiu_exp_split_convert = atoi(value);
        else if (!strcmp(key, "exp_fm_x"))
            g_exp_fm_x = atoi(value);
        else if (!strcmp(key, "exp_ps_ftrv"))
            g_exp_ps_ftrv = atoi(value);
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
    printf("[wiiu] run ended after %d s\n", seconds);
    if (FILE *f = fopen(NULLDC_HOME "last_run.txt", "w"))
    {
        fprintf(f, "file '%s' dynarec %d: %d s, vblanks %u renders %u", selectedFilePath, g_dynarec_preset, seconds,
                wiiu_vblanks, wiiu_renders);
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
    int rv = EmuMain(1, args);
    printf("[wiiu] EmuMain returned %d\n", rv);
    emu_running = false;
    return rv;
}

// exp_gx2_core: ProcUI's foreground callbacks would run on the main thread, away from GX2, so they're dropped, and the
// GX2 thread shuts graphics down itself before the release and starts it again after
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
    // Threads and cores (console; in Cemu the codegen core is 0, so cores 0 and 2 swap):
    //   core 1, the codegen core: the emulator (SH4 JIT), and the main thread, which only runs ProcUI
    //   core 0: the GX2 thread, the AICA mixer, the vertex convert helper and the disc reader
    //   core 2: TA intake (decode, convert, textures), the GX2 worker and the watchdog
    // GX2 work is kept off the JIT core because it took a large share of it. These are the speed options' defaults;
    // the options page can turn them off (config.txt, read here because the threads start before the picker runs),
    // and wiiu.cfg's exp_* keys set them one by one.
    {
        bool gfx_core = true, sound_core = true, jit_fast = true, smooth = true, code_check = true;
        int buffer = 100, disc_kbps = 7200;
        wiiu_picker_speed_options(home, &gfx_core, &sound_core, &jit_fast, &smooth, &code_check, &buffer, &disc_kbps);
        const int other = OSGetCodegenCore() == 0 ? 2 : 1;   // 1: core 0, 2: core 2
        wiiu_exp_gx2_core = gfx_core ? other : 0;
        wiiu_exp_split_convert = gfx_core;
        g_exp_aica_thread = sound_core ? other : 0;
        g_exp_fm_x = g_exp_ps_ftrv = g_exp_sq_fast = g_bcache_preset = jit_fast;
        g_jit_align_preset = jit_fast ? 0 : 1;
        wiiu_exp_frame_queue = smooth;
        g_jit_sbp_preset = code_check ? 3 : 1;   // 3: MvC2's module cache too
        audio_buffer_ms = buffer;
        g_gdrom_delay = disc_kbps;   // H1: the drive paced (dc/gdrom/gdromv3.cpp), 4x a real one by default
        printf("[wiiu] speed options: graphics core %d, sound core %d, faster code %d, smooth frames %d, code check %d, "
               "sound buffer %d, disc %d KB/s\n", gfx_core, sound_core, jit_fast, smooth, code_check, buffer ? buffer : 46,
               disc_kbps);
    }
    load_wiiu_cfg();
    if (wiiu_exp_gx2_core)
    {
        // exp_gx2_core (1: core 0, 2: core 2): GX2 lives on another thread, which does its init too, and this thread
        // only runs ProcUI. It takes the GX2 thread's share off core 1, the JIT's core
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
        // no OSJoinThread: if the GX2 thread doesn't finish, leave after 3 s anyway rather than stay stuck on the way
        // back to the menu
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
            // nothing asking for the menu, would leave the app frozen and HOME dead
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
    wiiu_audio_tune(audio_buffer_ms);
    wiiu_audio_init();

    void *codegen_area = nullptr;
    uint32_t codegen_size = 0;
    OSGetCodegenVirtAddrRange(&codegen_area, &codegen_size);
    printf("[wiiu] nullDC4Wii native Wii U build; codegen %p size 0x%x core %u\n", codegen_area, codegen_size, OSGetCodegenCore());

    marker("codegen");
    extern u8 *CodeCache;
    CodeCache = (u8 *)wiiu_codegen_alloc(CODE_SIZE, "sh4 jit");
    mkdir("fs:/vol/external01/wiiu", 0777);
    mkdir(NULLDC_HOME, 0777);
    mkdir(NULLDC_HOME "data", 0777);
    SetApplicationPath(home);
    printf("[wiiu] home %s\n", home);
    {
        extern u32 GetRTC_now();
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        printf("[wiiu] clock: time %lld = %04d-%02d-%02d %02d:%02d, isdst %d, DC RTC %08x\n", (long long)now,
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_isdst, GetRTC_now());
    }
    // the picker's options (config.txt), unless wiiu.cfg sets them: fastmem (fastmem_wiiu.cpp) and the light gun
    // (drkMapleLightGun.cpp) on every port
    {
        bool opt_fastmem = true, opt_lightgun = false, opt_keep_pitch = true;
        int opt_width = 960;
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
    printf("[wiiu] file '%s'\n", selectedFilePath);

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
    const u32 stack_size = 4 * 1024 * 1024;
    u8 *stack = (u8 *)memalign(16, stack_size);
    u32 core = OSGetCodegenCore();
    OSThreadAttributes affinity = core == 0 ? OS_THREAD_ATTRIB_AFFINITY_CPU0 :
                                  core == 2 ? OS_THREAD_ATTRIB_AFFINITY_CPU2 : OS_THREAD_ATTRIB_AFFINITY_CPU1;
    emu_running = true;
    // one step below the thread starting it (the GX2 thread, or with the graphics core off the main thread on the
    // codegen core): the emulator never yields, and CafeOS doesn't time-slice threads of equal priority, so at the same
    // priority a thread sharing its core would never run again. The others mostly wait, so the emulator gets the core.
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
            printf("[wiiu] t=%d vblanks %u renders %u ta lists %u (sq %u dma %u) pc %08x%s\n", seconds, wiiu_vblanks,
                   wiiu_renders, wiiu_ta_lists, wiiu_ta_sq_blocks, wiiu_ta_dma_blocks, (u32)next_pc, wiiu_fastmem_status());
            if (seconds % 5 == 0)
                wiiu_disc_stats();
        }
        if (!done && (!emu_running || exit_requested_by_core))
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
