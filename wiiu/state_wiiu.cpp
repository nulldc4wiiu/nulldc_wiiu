// Save states (features plan, phase 3): the sections (wiiu/state_wiiu.h) into one buffer, written to
// sd:/wiiu/nulldc/states/<game>.s<slot> by a thread of its own so play resumes at once; loads read the file back.
//
// Everything that touches machine state runs on the emulator thread, parked in wiiu_emu_safe_point while the HOME
// menu is open (main_wiiu.cpp calls wiiu_state_service from the park loop): the menu asks and waits. A load first
// keeps the running state in memory, so "Undo the load" can go back to it.
#include "state_wiiu.h"
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <algorithm>
#include <string>
#include <vector>

std::string wiiu_game_name(const char *disc);   // picker_wiiu.cpp
extern char selectedFilePath[512];
extern "C" bool wiiu_disc_lid_close(const char *path);   // plugs/ImgReader/common.cpp
void wiiu_state_after_load();                    // below: the caches a load throws away
void wiiu_state_before();                        // below: other threads done with machine state
extern "C" int get_controller_type(void);
extern "C" int get_player_count(void);
extern "C" int wiiu_rumble;

int wiiu_save_states;   // save_states=1: the HOME menu's save and load rows
static void trace_start();

static StateSection *sections;
StateSection::StateSection(const char *n, StateFn f) : name(n), fn(f), next(sections) { sections = this; }

void StateIO::io(void *p, size_t n)
{
    if (!ok)
        return;
    if (buf)
    {
        if (pos + n > cap)
        {
            ok = false;
            return;
        }
        if (loading)
            memcpy(p, buf + pos, n);
        else
            memcpy(buf + pos, p, n);
    }
    pos += n;
}

namespace
{
const char MAGIC[8] = { 'N', 'D', 'C', 'S', 'T', 'A', 'T', '1' };
struct Header
{
    char magic[8];
    char build[64];      // a state loads only into the build that wrote it
    char disc[512];      // the disc in the drive
    int64_t saved_at;    // time()
    uint32_t size;       // the whole file
    uint32_t sections;
    uint32_t layout;     // the Maple devices: controller type, players, rumble packs (they must match)
};
uint32_t layout() { return (uint32_t)get_controller_type() | (uint32_t)get_player_count() << 8 | (wiiu_rumble ? 1u : 0u) << 16; }
struct SectionHead
{
    char name[24];
    uint32_t size;
};
const char *build_id() { return __DATE__ " " __TIME__; }

std::vector<StateSection *> sorted()
{
    std::vector<StateSection *> v;
    for (StateSection *s = sections; s; s = s->next)
        v.push_back(s);
    std::sort(v.begin(), v.end(), [](StateSection *a, StateSection *b) { return strcmp(a->name, b->name) < 0; });
    return v;
}

// the whole state into buf (null: just its size)
size_t serialize(uint8_t *buf, size_t cap)
{
    const auto v = sorted();
    size_t pos = sizeof(Header);
    for (StateSection *sec : v)
    {
        StateIO m = { nullptr, 0, 0, false, true };
        sec->fn(m);   // measure
        if (buf)
        {
            if (pos + sizeof(SectionHead) + m.pos > cap)
                return 0;
            SectionHead h = {};
            snprintf(h.name, sizeof(h.name), "%s", sec->name);
            h.size = (uint32_t)m.pos;
            memcpy(buf + pos, &h, sizeof(h));
            StateIO w = { buf + pos + sizeof(h), 0, m.pos, false, true };
            sec->fn(w);
        }
        pos += sizeof(SectionHead) + m.pos;
    }
    if (buf)
    {
        Header h = {};
        memcpy(h.magic, MAGIC, 8);
        snprintf(h.build, sizeof(h.build), "%s", build_id());
        snprintf(h.disc, sizeof(h.disc), "%s", selectedFilePath);
        h.saved_at = (int64_t)time(nullptr);
        h.size = (uint32_t)pos;
        h.sections = (uint32_t)v.size();
        h.layout = layout();
        memcpy(buf, &h, sizeof(h));
    }
    return pos;
}

// checks the whole file before touching anything; returns an error, or null after loading
const char *apply(const uint8_t *buf, size_t size)
{
    Header h;
    if (size < sizeof(h))
        return "the file is too short";
    memcpy(&h, buf, sizeof(h));
    if (memcmp(h.magic, MAGIC, 8) || h.size != size)
        return "not a nullDC save state";
    if (strncmp(h.build, build_id(), sizeof(h.build)))
        return "made by another nullDC build";
    if (h.layout != layout())
        return "saved with other controllers (light gun, players or rumble)";
    const auto v = sorted();
    if (h.sections != v.size())
        return "made by another nullDC build (sections)";
    size_t pos = sizeof(h);
    for (StateSection *sec : v)
    {
        SectionHead sh;
        if (pos + sizeof(sh) > size)
            return "the file is cut short";
        memcpy(&sh, buf + pos, sizeof(sh));
        StateIO m = { nullptr, 0, 0, false, true };
        sec->fn(m);
        if (strncmp(sh.name, sec->name, sizeof(sh.name)) || sh.size != m.pos || pos + sizeof(sh) + sh.size > size)
            return "made by another nullDC build (a section differs)";
        pos += sizeof(sh) + sh.size;
    }
    // valid: the disc first (its state is in the sections), then everything
    if (strncmp(h.disc, selectedFilePath, sizeof(h.disc)))
    {
        char disc[512];
        snprintf(disc, sizeof(disc), "%s", h.disc);
        if (!wiiu_disc_lid_close(disc))
            return "its disc isn't there";
        snprintf(selectedFilePath, 512, "%s", disc);
    }
    pos = sizeof(h);
    for (StateSection *sec : v)
    {
        SectionHead sh;
        memcpy(&sh, buf + pos, sizeof(sh));
        StateIO r = { (uint8_t *)buf + pos + sizeof(sh), 0, sh.size, true, true };
        sec->fn(r);
        pos += sizeof(sh) + sh.size;
    }
    wiiu_state_after_load();
    return nullptr;
}

std::string dir() { return "fs:/vol/external01/wiiu/nulldc/states"; }
std::string slot_path(int slot)
{
    return dir() + "/" + wiiu_game_name(selectedFilePath) + ".s" + std::to_string(slot);
}

// the buffers: the state being written, and the running state kept by a load for undo
uint8_t *save_buf, *undo_buf;
size_t save_cap, undo_cap, undo_size;
bool ensure(uint8_t *&b, size_t &cap, size_t need)
{
    if (cap >= need)
        return true;
    free(b);
    b = (uint8_t *)malloc(need);
    cap = b ? need : 0;
    return b != nullptr;
}

// the writer thread
OSThread writer;
volatile bool writing;
size_t write_size;
char write_path[600];
const char *volatile status = "";   // for the menu: the last result (static strings: the writer thread sets it)
int writer_main(int, const char **)
{
    const OSTime t0 = OSGetSystemTime();
    mkdir(dir().c_str(), 0777);
    const std::string tmp = std::string(write_path) + ".new";
    bool ok = false;
    if (FILE *f = fopen(tmp.c_str(), "wb"))
    {
        ok = fwrite(save_buf, 1, write_size, f) == write_size;
        ok = fclose(f) == 0 && ok;
    }
    if (ok)
    {
        remove(write_path);
        ok = rename(tmp.c_str(), write_path) == 0;
    }
    printf("[state] %s %s: %u bytes in %u ms\n", ok ? "wrote" : "COULDN'T write", write_path, (unsigned)write_size,
           (unsigned)OSTicksToMilliseconds(OSGetSystemTime() - t0));
    status = ok ? "Saved." : "Couldn't write the save state to the SD card.";
    writing = false;
    return 0;
}

// a request from the menu, carried out on the emulator thread
volatile int req_op, req_slot;   // op 1 save, 2 load, 3 undo the load
volatile bool req_done;
const char *req_error;

void do_save(int slot)
{
    while (writing)
        OSSleepTicks(OSMillisecondsToTicks(10));   // the previous save still writing: its buffer is in use
    const OSTime t0 = OSGetSystemTime();
    const size_t need = serialize(nullptr, 0);
    if (!ensure(save_buf, save_cap, need) || serialize(save_buf, save_cap) != need)
    {
        req_error = "Not enough memory for a save state.";
        return;
    }
    printf("[state] slot %d: %u bytes gathered in %u ms\n", slot, (unsigned)need,
           (unsigned)OSTicksToMilliseconds(OSGetSystemTime() - t0));
    write_size = need;
    snprintf(write_path, sizeof(write_path), "%s", slot_path(slot).c_str());
    writing = true;
    status = "Saving...";
    static uint8_t stack[16 * 1024] __attribute__((aligned(16)));
    if (!OSCreateThread(&writer, writer_main, 0, nullptr, stack + sizeof(stack), sizeof(stack), 20,
                        OS_THREAD_ATTRIB_AFFINITY_CPU0))
    {
        writer_main(0, nullptr);   // no thread: write it here
        return;
    }
    OSSetThreadName(&writer, "nullDC state writer");
    OSDetachThread(&writer);
    OSResumeThread(&writer);
}

void do_load(int slot)
{
    std::vector<uint8_t> file;
    const std::string path = slot_path(slot);
    const OSTime t0 = OSGetSystemTime();
    if (FILE *f = fopen(path.c_str(), "rb"))
    {
        fseek(f, 0, SEEK_END);
        const long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (n > 0)
        {
            file.resize((size_t)n);
            if (fread(file.data(), 1, file.size(), f) != file.size())
                file.clear();
        }
        fclose(f);
    }
    if (file.empty())
    {
        req_error = "That slot is empty.";
        return;
    }
    // the running state, for undo
    const size_t need = serialize(nullptr, 0);
    undo_size = ensure(undo_buf, undo_cap, need) && serialize(undo_buf, undo_cap) == need ? need : 0;
    req_error = apply(file.data(), file.size());
    printf("[state] slot %d load: %s (%u ms)\n", slot, req_error ? req_error : "ok",
           (unsigned)OSTicksToMilliseconds(OSGetSystemTime() - t0));
}

void do_undo()
{
    if (!undo_size)
    {
        req_error = "There's no load to undo.";
        return;
    }
    req_error = apply(undo_buf, undo_size);
    if (!req_error)
        undo_size = 0;
    printf("[state] undo: %s\n", req_error ? req_error : "ok");
}
}

bool wiiu_state_op_pending() { return req_op != 0; }

// main_wiiu.cpp: the emulator thread, CPU stopped (wiiu_emu_after_run)
void wiiu_state_service()
{
    const int op = req_op;
    if (!op)
        return;
    req_error = nullptr;
    wiiu_state_before();
    if (op == 1)
        do_save(req_slot);
    else if (op == 2)
        do_load(req_slot);
    else
        do_undo();
    if (!req_error)
    {
        trace_start();
    }
    req_op = 0;
    __sync_synchronize();
    req_done = true;
}

// the HOME menu (GX2 thread, emulator parked): does it and waits; returns an error or null
const char *wiiu_state_run(int op, int slot)
{
    req_done = false;
    req_slot = slot;
    __sync_synchronize();
    req_op = op;
    for (int i = 0; i < 3000 && !req_done; i++)   // a load reads ~30 MB from the SD card
        OSSleepTicks(OSMillisecondsToTicks(10));
    if (!req_done)
        return "The emulator didn't answer.";
    return req_error;
}

// the menu's slot lines: the save time from the file's header (Cemu's file times are wrong)
std::string wiiu_state_slot_info(int slot)
{
    Header h;
    FILE *f = fopen(slot_path(slot).c_str(), "rb");
    if (!f)
        return "empty";
    const bool ok = fread(&h, sizeof(h), 1, f) == 1 && !memcmp(h.magic, MAGIC, 8);
    fclose(f);
    if (!ok)
        return "not a save state";
    time_t t = (time_t)h.saved_at;
    struct tm tmv;
    localtime_r(&t, &tmv);
    char b[48];
    strftime(b, sizeof(b), "%Y-%m-%d %H:%M", &tmv);
    return strncmp(h.build, build_id(), sizeof(h.build)) ? std::string(b) + ", another build" : std::string(b);
}
bool wiiu_state_can_undo() { return undo_size != 0; }
const char *wiiu_state_status() { return writing ? "Saving..." : status; }
bool wiiu_state_writing() { return writing; }

// what other threads must finish before the state is read or replaced, and what a load throws away
void aica_state_drain();               // plugs/nullAICA/aica.cpp
void aica_state_after_load();
void wiiu_rtt_wait();                  // present_gx2.cpp
void wiiu_textures_after_load();
void wiiu_render_after_load();         // render_gx2.cpp
void recSh4_ClearCache();              // dc/sh4/rec_v2/driver.cpp
void arm_Run_Cached(uint32_t CycleCount, bool reset);   // plugs/vbaARM/arm7_cached.cpp
void arm_jit_on_reset();               // plugs/vbaARM/arm7_jit.cpp
void wiiu_state_before()
{
    aica_state_drain();
    wiiu_rtt_wait();
}
void wiiu_state_after_load()
{
    recSh4_ClearCache();       // the SH-4 code was compiled from the old RAM
    arm_Run_Cached(0, true);   // both ARM7 engines' code, from the old sound RAM
    arm_jit_on_reset();
    aica_state_after_load();
    wiiu_render_after_load();
    wiiu_textures_after_load();
}

#ifdef WIIU_INSTRUMENT
// state_trace=N (test): after a save or a load, a hash of the whole state (all sections, not the header) every N
// emulated vblanks counted from it, 12 times: a run that saved and a run that loaded that state must print the same
// hashes when the emulation is exact (bench mode, aica_sync_reads=1, no inputs after the save)
int wiiu_state_trace;
static int trace_left = -1;
static uint32_t trace_vb;
static uint32_t fnv(const uint8_t *p, size_t n)
{
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 16777619u;
    return h;
}
// the state's hash; for the first samples each section's too, to find the ones that part ways
void aica_state_drain();
static void trace_sample(int k)
{
    aica_state_drain();   // as a save does: the mixer thread's channels at a defined point
    static uint8_t *buf;
    static size_t cap;
    const size_t need = serialize(nullptr, 0);
    if (!ensure(buf, cap, need) || serialize(buf, cap) != need)
        return;
    printf("[state] trace %d: %08x\n", k, fnv(buf + sizeof(Header), need - sizeof(Header)));
    if (k > 2)
        return;
    size_t pos = sizeof(Header);
    std::string line;
    while (pos + sizeof(SectionHead) <= need)
    {
        SectionHead sh;
        memcpy(&sh, buf + pos, sizeof(sh));
        char b[64];
        snprintf(b, sizeof(b), " %s=%08x", sh.name, fnv(buf + pos + sizeof(sh), sh.size));
        line += b;
        pos += sizeof(sh) + sh.size;
    }
    printf("[state] trace %d sections:%s\n", k, line.c_str());
}
void wiiu_state_trace_vblank()
{
    if (!wiiu_state_trace || trace_left <= 0 || ++trace_vb % (uint32_t)wiiu_state_trace)
        return;
    trace_sample(13 - trace_left);
    trace_left--;
}
static void trace_start()
{
    if (!wiiu_state_trace)
        return;
    trace_sample(0);   // at the save or load itself: the two runs must agree here
    trace_left = 12;
    trace_vb = 0;
}
#else
static void trace_start() {}
#endif
