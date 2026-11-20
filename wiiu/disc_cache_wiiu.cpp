// GD-ROM image reads on the Wii U, through a cache of 64-sector chunks that a thread on core 0 fills ahead of
// the game. On a console the emulator read the image a sector at a time (one seek and one read each, about
// 450 SD round trips in a 120 ms menu frame), which is what made the sound stutter at every load. Now a miss
// reads its whole chunk in one go, and every chunk the game touches queues the next two for the reader thread,
// which has its own file handles. Raw sectors are cached; iso9660.cpp converts them as before.
#include <coreinit/event.h>
#include <coreinit/fastmutex.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "types.h"


volatile int wiiu_disc_reader_state;   // exit-hang diagnosis: 0 waiting for work, 1 opening a track, 2 reading
int wiiu_disc_ctx;   // who is reading (dc/gdrom/gdromv3.cpp sets it): 0 DMA, 1 PIO, 2 CD audio; for the miss counts

namespace
{
const u32 CHUNK = 64, SLOTS = 32, MAX_SS = 2352, MAX_TRACKS = 101, QUEUE = 16;
enum { EMPTY, LOADING, READY };

struct Slot
{
    int track;
    u32 chunk, sectors, used, gen;   // gen: the disc it was loaded for (disc_gen at claim)
    volatile int state;
    u8 *data;
};

struct Track
{
    char path[512];
    FILE *own;   // the reader thread's handle, opened when it first needs it
    u32 ss;
    s32 offset;
};

Slot slots[SLOTS];
Track tracks[MAX_TRACKS];
u32 use_clock;
u32 disc_gen;   // bumped for each new disc: a chunk still loading for the old one is never found for the new
OSFastMutex lock;
bool ready;

struct Request
{
    int track;
    u32 chunk;
};
Request queue[QUEUE];
u32 q_head, q_tail;
OSEvent wake;
OSThread reader;

u32 stat_hits, stat_misses, stat_waits, stat_prefetched, stat_async, stat_async_timeouts, stat_miss_by[3];

int find(int track, u32 chunk)
{
    for (u32 i = 0; i < SLOTS; i++)
        if (slots[i].state != EMPTY && slots[i].gen == disc_gen && slots[i].track == track && slots[i].chunk == chunk)
            return (int)i;
    return -1;
}

// an empty slot, or the least recently used ready one (never one being loaded); under the lock
int claim(int track, u32 chunk)
{
    int best = -1;
    for (u32 i = 0; i < SLOTS; i++)
    {
        if (slots[i].state == EMPTY)
        {
            best = (int)i;
            break;
        }
        if (slots[i].state == READY && (best < 0 || slots[i].used < slots[best].used))
            best = (int)i;
    }
    if (best >= 0)
    {
        slots[best].track = track;
        slots[best].chunk = chunk;
        slots[best].gen = disc_gen;
        slots[best].sectors = 0;
        slots[best].used = ++use_clock;   // new, so newest: it kept its old occupant's stamp, and read-ahead evicted
                                          // the chunks it had just loaded before the game reached them
        slots[best].state = LOADING;
    }
    return best;
}

// outside the lock: the slot is ours while it's LOADING
void load(Slot &s, FILE *f)
{
    const Track &t = tracks[s.track];
    u32 got = 0;
    if (f && !fseek(f, (long)(t.offset + (s64)s.chunk * CHUNK * t.ss), SEEK_SET))
        got = (u32)fread(s.data, t.ss, CHUNK, f);
    s.sectors = got;
    __sync_synchronize();
    s.state = READY;
}

volatile bool stopping;   // wiiu_disc_quiesce: the emulator is stopping, no more read-ahead

void request(int track, u32 chunk)
{
    if (stopping || find(track, chunk) >= 0)
        return;
    for (u32 i = q_head; i != q_tail; i = (i + 1) % QUEUE)
        if (queue[i].track == track && queue[i].chunk == chunk)
            return;
    if ((q_tail + 1) % QUEUE == q_head)
        return;   // full: the reader is behind, and a miss still reads it
    queue[q_tail] = { track, chunk };
    q_tail = (q_tail + 1) % QUEUE;
    OSSignalEvent(&wake);
}

// H2.1: a chunk the paced drive is waiting for goes to the front of the queue, ahead of read-ahead that may never
// be used (behind up to 16 of those it waited 100-300 ms on the console, h2a). When the queue is full the newest
// read-ahead request makes room.
void request_now(int track, u32 chunk)
{
    if (stopping || find(track, chunk) >= 0)
        return;
    for (u32 i = q_head; i != q_tail; i = (i + 1) % QUEUE)
        if (queue[i].track == track && queue[i].chunk == chunk)
        {
            const Request r = queue[i];
            queue[i] = queue[q_head];
            queue[q_head] = r;
            OSSignalEvent(&wake);
            return;
        }
    if ((q_tail + 1) % QUEUE == q_head)
        q_tail = (q_tail + QUEUE - 1) % QUEUE;
    q_head = (q_head + QUEUE - 1) % QUEUE;
    queue[q_head] = { track, chunk };
    OSSignalEvent(&wake);
}

int reader_main(int, const char **)
{
    for (;;)
    {
        OSWaitEvent(&wake);
        for (;;)
        {
            OSFastMutex_Lock(&lock);
            if (q_head == q_tail)
            {
                OSFastMutex_Unlock(&lock);
                break;
            }
            const Request r = queue[q_head];
            q_head = (q_head + 1) % QUEUE;
            const int s = find(r.track, r.chunk) >= 0 ? -1 : claim(r.track, r.chunk);
            OSFastMutex_Unlock(&lock);
            if (s < 0)
                continue;
            Track &t = tracks[r.track];
            wiiu_disc_reader_state = 1;
            if (!t.own)
                t.own = fopen(t.path, "rb");
            wiiu_disc_reader_state = 2;
            load(slots[s], t.own);
            wiiu_disc_reader_state = 0;
            stat_prefetched++;
        }
    }
    return 0;
}
} // namespace

// load_gdi (iso9660.cpp), for each track with a file; track 0 starts a new disc
void wiiu_disc_cache_track(int track, const char *path, u32 ss, s32 offset)
{
    if (track < 0 || track >= (int)MAX_TRACKS || ss == 0 || ss > MAX_SS)
        return;
    if (!ready)
    {
        OSFastMutex_Init(&lock, "nulldc disc cache");
        OSInitEvent(&wake, FALSE, OS_EVENT_MODE_AUTO);
        bool memory = true;
        for (u32 i = 0; i < SLOTS; i++)
        {
            slots[i].track = -1;
            memory = (slots[i].data = (u8 *)malloc(CHUNK * MAX_SS)) && memory;
        }
        if (!memory)
        {
            printf("[disc] no memory for the read-ahead cache, reading as before\n");
            return;   // ready stays false: every read falls back
        }
        static u8 stack[32 * 1024] __attribute__((aligned(16)));
        if (OSCreateThread(&reader, reader_main, 0, nullptr, stack + sizeof(stack), sizeof(stack), 16,
                           OS_THREAD_ATTRIB_AFFINITY_CPU0))
        {
            OSSetThreadName(&reader, "nullDC disc reader");
            OSResumeThread(&reader);
        }
        ready = true;
        printf("[disc] read-ahead cache: %u chunks of %u sectors\n", SLOTS, CHUNK);
    }
    OSFastMutex_Lock(&lock);
    if (track == 0)
    {
        // a new disc (the first one, or a swap from the HOME menu): forget the old one. A load still running finishes
        // under the old generation, which find() never matches
        for (u32 i = 0; i < SLOTS; i++)
            if (slots[i].state == READY)
                slots[i].state = EMPTY;
        q_head = q_tail = 0;
        disc_gen++;
    }
    for (u32 i = 0; i < SLOTS; i++)
        if (slots[i].track == track && slots[i].state == READY)
            slots[i].state = EMPTY;
    Track &t = tracks[track];
    if (t.own)
        fclose(t.own);
    t.own = nullptr;
    snprintf(t.path, sizeof(t.path), "%s", path);
    t.ss = ss;
    t.offset = offset;
    OSFastMutex_Unlock(&lock);
}

// the raw sector index-th of the track into out (its sector size); false: read it the old way.
// f is the emulator's own handle for the track, used on a miss.
bool wiiu_disc_read(int track, u32 index, u8 *out, FILE *f)
{
    if (!ready || track < 0 || track >= (int)MAX_TRACKS || !tracks[track].ss || tracks[track].offset < 0)
        return false;
    const u32 chunk = index / CHUNK, at = index % CHUNK, ss = tracks[track].ss;
    for (int tries = 0;; tries++)
    {
        OSFastMutex_Lock(&lock);
        int s = find(track, chunk);
        if (s < 0)
        {
            s = claim(track, chunk);
            OSFastMutex_Unlock(&lock);
            if (s < 0)
                return false;
            stat_misses++;
            stat_miss_by[wiiu_disc_ctx < 3 ? wiiu_disc_ctx : 0]++;
            load(slots[s], f);
            OSFastMutex_Lock(&lock);
        }
        if (slots[s].state == LOADING)
        {
            OSFastMutex_Unlock(&lock);
            if (tries == 0)
                stat_waits++;
            if (tries > 20000)
                return false;   // ~2 s: something is wrong with the reader
            OSSleepTicks(OSMicrosecondsToTicks(100));
            continue;
        }
        const bool ok = at < slots[s].sectors;
        if (ok)
            memcpy(out, slots[s].data + at * ss, ss);
        slots[s].used = ++use_clock;
        stat_hits++;
        request(track, chunk + 1);
        request(track, chunk + 2);
        OSFastMutex_Unlock(&lock);
        return ok;
    }
}

// H1 v2: whether the chunks holding sectors [index, index + count) of the track are loaded, without reading:
// missing ones are queued for the reader and the paced GD-DMA (gdromv3.cpp) waits in emulated time instead of the
// emulator thread waiting on the SD card. After 300 ms of host time it says yes anyway, and the read takes the
// synchronous miss path as before, so a reader that can't keep up (full queue, no free slot) never hangs a game.
bool wiiu_disc_ready(int track, u32 index, u32 count)
{
    if (!ready || stopping || track < 0 || track >= (int)MAX_TRACKS || !tracks[track].ss || tracks[track].offset < 0 ||
        !count)
        return true;
    static OSTime wait_since;
    const u32 c0 = index / CHUNK, c1 = (index + count - 1) / CHUNK;
    bool all = true;
    OSFastMutex_Lock(&lock);
    for (u32 c = c0; c <= c1; c++)
    {
        const int s = find(track, c);
        if (s < 0)
            request_now(track, c);
        else
            slots[s].used = ++use_clock;   // wanted now: keep it until the read below
        if (s < 0 || slots[s].state != READY)
            all = false;
    }
    if (all)
        request(track, c1 + 1);   // keep the reader a chunk ahead of the drive
    OSFastMutex_Unlock(&lock);
    if (all)
    {
        wait_since = 0;
        return true;
    }
    const OSTime now = OSGetSystemTime();
    if (!wait_since)
    {
        wait_since = now;
        stat_async++;
    }
    else if (now - wait_since > (OSTime)OSMillisecondsToTicks(300))
    {
        wait_since = 0;
        stat_async_timeouts++;
        return true;
    }
    return false;
}

OSThread *wiiu_disc_reader_thread() { return ready ? &reader : nullptr; }

// before the emulator stops (main_wiiu.cpp stop_emulator): drop the queued read-ahead and let a read in flight finish,
// so the exit's SD writes (settings, flash, the run summary) don't queue behind up to 16 chunks of reads
void wiiu_disc_quiesce()
{
    if (!ready)
        return;
    stopping = true;
    OSFastMutex_Lock(&lock);
    q_head = q_tail = 0;
    OSFastMutex_Unlock(&lock);
    for (int i = 0; i < 200 && wiiu_disc_reader_state != 0; i++)
        OSSleepTicks(OSMillisecondsToTicks(10));
    printf("[disc] read-ahead stopped%s\n", wiiu_disc_reader_state ? ", a read still running after 2 s" : "");
}

// for the log once in a while
void wiiu_disc_stats()
{
    if (!ready || !stat_hits)
        return;
    printf("[disc] sectors %u, chunk misses %u (emulator read; dma %u pio %u cdda %u), waits %u, prefetched %u, "
           "drive waits %u (timeouts %u)\n", stat_hits, stat_misses, stat_miss_by[0], stat_miss_by[1], stat_miss_by[2],
           stat_waits, stat_prefetched, stat_async, stat_async_timeouts);
    stat_hits = stat_misses = stat_waits = stat_prefetched = stat_async = stat_async_timeouts = 0;
    stat_miss_by[0] = stat_miss_by[1] = stat_miss_by[2] = 0;
}
