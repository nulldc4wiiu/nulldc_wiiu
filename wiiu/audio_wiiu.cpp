// Sound on the Wii U: the AICA's 44.1 kHz stereo output (wii_audio_push_sample, emulator thread) played by two
// AX voices, left and right, each looping over a ring of 16-bit samples that AX resamples to 48 kHz and sends
// to the TV and the GamePad. The emulator writes ahead of the play position in small cache-flushed blocks.
// The AX frame callback (every 3 ms) tracks the play position and steers the resampling ratio by how much is
// queued, so playback follows the rate the emulator really produces: below full speed the sound runs a little
// slower and lower instead of stuttering (stop-and-restart at ~76% speed was unbearable on a console). Only a
// real stall (a disc load) empties the ring; the voices then stop and start again once some sound is back.
// A full ring makes the emulator wait, which is what holds it to real speed (on a console it otherwise ran at
// about 170%); it never waits long, so a stalled voice can't hang it.
#include <coreinit/cache.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <sndcore2/core.h>
#include <sndcore2/device.h>
#include <sndcore2/voice.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "dc/dc.h"
#include "wii/wii_audio.h"

static const u32 RING = 8192;            // samples per channel in each voice's loop (186 ms), a power of two
static u32 MAX_QUEUED = 3072;      // the emulator keeps at most this much unplayed (70 ms)
static u32 TARGET_QUEUED = 2048;   // what the rate steering aims for (46 ms)
static const u32 MIN_QUEUED = 256;       // below this (6 ms, two AX frames) the voices stop
static u32 START_QUEUED = 768;     // and start again from this much (17 ms)
static const float BASE_RATIO = 44100.0f / 48000.0f;
// playback speed as a fraction of nominal: from 60% (the emulator that slow still sounds continuous) to just
// over 100% (when the emulator runs ahead the full ring paces it, and this keeps the queue from sitting full)
// The picker's "Sound when slow" option sets the floor: 0.97 keeps the pitch and lets the queue run dry instead (gaps)
float wiiu_audio_min_speed = 0.60f;
static const float MAX_SPEED = 1.005f;
static const u32 BLOCK = 64;             // samples written and flushed at a time

static s16 *ring[2];
static AXVoice *voice[2];
static volatile u32 write_pos, play_pos;  // in samples, modulo RING
static volatile bool playing, ax_ready, aica_ready;
static s16 block[2][BLOCK];
static u32 block_fill;
static float speed_glide = 0.02f;        // audio_glide=1: 0.005, speed follows the queue over about 600 ms
static float speed = 1.0f;               // the voices' current playback speed (frame callback only)
static volatile float speed_sum;         // for the log: summed over frame callbacks
static volatile u32 speed_n;
static u32 consumed;                     // samples the voices played (frame callback), and over the last second
static volatile bool output_stalled;     // under 20000 a second while playing: don't pace on it

// once a second in the log: peak level, fill, and what went wrong
static u32 stat_samples, stat_peak, stat_drops, stat_waits, stat_underruns, stat_stalls;

static inline u32 queued() { return (write_pos - play_pos) & (RING - 1); }
extern "C" u32 wiiu_audio_queued() { return ax_ready ? queued() : 0; }

static void start_voices();
static volatile bool want_start;
u32 wiiu_keyons, wiiu_keyons_zero;   // sgc_if.cpp KEY_ON
extern u32 g_aica_lag_max, g_aica_lag_sum, g_aica_lag_n;   // aica.cpp   // exp_aica_thread: restart the voices from the frame callback
static void frame_callback()
{
    if (!ax_ready)
        return;
    AXVoiceOffsets o;
    AXGetVoiceOffsets(voice[0], &o);
    const u32 pos = o.currentOffset & (RING - 1);
    consumed += (pos - play_pos) & (RING - 1);
    static u32 frames, playing_frames;
    playing_frames += playing;
    if (++frames == 333)   // AX frames are 3 ms: about a second
    {
        output_stalled = playing_frames > 300 && consumed < 20000;
        frames = playing_frames = consumed = 0;
    }
    // a voice that plays but no longer moves (seen once in Cemu, emulator stuck waiting for room): count it as
    // an underrun, so the writer restarts both voices
    static u32 still;
    still = playing && pos == play_pos ? still + 1 : 0;
    play_pos = pos;
    const u32 q = queued();
    if (still >= 30)
        stat_stalls++;
    if (playing && (q < MIN_QUEUED || still >= 30))
    {
        AXSetVoiceState(voice[0], AX_VOICE_STATE_STOPPED);
        AXSetVoiceState(voice[1], AX_VOICE_STATE_STOPPED);
        playing = false;
        stat_underruns++;
        return;
    }
    if (!playing)
    {
        if (want_start)
        {
            want_start = false;
            start_voices();
        }
        return;
    }
    // proportional steering toward TARGET_QUEUED, smoothed over ~150 ms so the pitch glides rather than wobbles:
    // an emulator at 76% settles with the queue near 1000 samples and the voices at 76%
    float want = 1.0f + 0.5f * ((float)q - (float)TARGET_QUEUED) / (float)TARGET_QUEUED;
    want = want < wiiu_audio_min_speed ? wiiu_audio_min_speed : want > MAX_SPEED ? MAX_SPEED : want;
    // speed always moves; the voices only hear about it once it has moved a little (a threshold on each step
    // instead left playback stuck at 98% with the emulator paced to match)
    static float applied = 1.0f;
    speed += (want - speed) * speed_glide;
    if (speed - applied > 0.0005f || applied - speed > 0.0005f)
    {
        applied = speed;
        for (int ch = 0; ch < 2; ch++)
        {
            AXVoiceBegin(voice[ch]);
            AXSetVoiceSrcRatio(voice[ch], BASE_RATIO * speed);
            AXVoiceEnd(voice[ch]);
        }
    }
    speed_sum = speed_sum + speed;
    speed_n = speed_n + 1;
}

// wiiu.cfg: audio_buffer=<ms> sets the queue the steering aims for, audio_glide=1 slows its speed changes
void wiiu_audio_tune(int buffer_ms, int glide)
{
    if (buffer_ms)
    {
        TARGET_QUEUED = (u32)buffer_ms * 441 / 10;
        MAX_QUEUED = TARGET_QUEUED * 3 / 2;
        START_QUEUED = TARGET_QUEUED * 3 / 8;
    }
    if (glide)
        speed_glide = 0.005f;
}

// called from main before the emulator starts
void wiiu_audio_init()
{
    AXInitParams init = {};
    init.renderer = AX_INIT_RENDERER_48KHZ;
    AXInitWithParams(&init);
    for (int ch = 0; ch < 2; ch++)
    {
        ring[ch] = (s16 *)memalign(0x100, RING * sizeof(s16));
        voice[ch] = AXAcquireVoice(31, nullptr, nullptr);
        if (!ring[ch] || !voice[ch])
        {
            printf("[audio] no AX voice or buffer, no sound\n");
            return;
        }
        memset(ring[ch], 0, RING * sizeof(s16));
        DCFlushRange(ring[ch], RING * sizeof(s16));

        AXVoiceBegin(voice[ch]);
        AXSetVoiceType(voice[ch], 0);
        AXVoiceVeData ve = { 0x8000, 0 };
        AXSetVoiceVe(voice[ch], &ve);
        // left voice to channel 0, right to channel 1, on the TV and the GamePad
        AXVoiceDeviceMixData mix[6] = {};
        mix[ch].bus[0].volume = 0x8000;
        AXSetVoiceDeviceMix(voice[ch], AX_DEVICE_TYPE_TV, 0, mix);
        AXSetVoiceDeviceMix(voice[ch], AX_DEVICE_TYPE_DRC, 0, mix);
        AXVoiceOffsets o = {};
        o.dataType = AX_VOICE_FORMAT_LPCM16;
        o.loopingEnabled = AX_VOICE_LOOP_ENABLED;
        o.loopOffset = 0;
        o.endOffset = RING - 1;
        o.currentOffset = 0;
        o.data = ring[ch];
        AXSetVoiceOffsets(voice[ch], &o);
        AXSetVoiceSrcType(voice[ch], AX_VOICE_SRC_TYPE_LINEAR);
        AXSetVoiceSrcRatio(voice[ch], 44100.0f / 48000.0f);
        AXSetVoiceState(voice[ch], AX_VOICE_STATE_STOPPED);
        AXVoiceEnd(voice[ch]);
    }
    AXRegisterAppFrameCallback(frame_callback);
    ax_ready = true;
    printf("[audio] AX: two voices, %u-sample rings, 44.1 -> 48 kHz\n", RING);
}

// called from main after the emulator stopped
void wiiu_audio_shutdown()
{
    if (!ax_ready)
        return;
    ax_ready = false;
    AXDeregisterAppFrameCallback(frame_callback);
    for (int ch = 0; ch < 2; ch++)
    {
        AXSetVoiceState(voice[ch], AX_VOICE_STATE_STOPPED);
        AXFreeVoice(voice[ch]);
    }
    AXQuit();
    for (int ch = 0; ch < 2; ch++)
        free(ring[ch]);
}

void wii_audio_aica_ready() { aica_ready = true; }
void wii_audio_term() { aica_ready = false; }

static void start_voices()
{
    // both voices from the same place, so left and right stay together
    const u32 p = play_pos;
    for (int ch = 0; ch < 2; ch++)
    {
        AXVoiceBegin(voice[ch]);
        AXSetVoiceCurrentOffset(voice[ch], p);
        AXSetVoiceState(voice[ch], AX_VOICE_STATE_PLAYING);
        AXVoiceEnd(voice[ch]);
    }
    playing = true;
}

// with exp_aica_thread the writer is on another core: voices restart from the frame callback, where they stop
extern int g_exp_aica_thread;
static inline void request_start()
{
    if (g_exp_aica_thread)
        want_start = true;
    else
        start_voices();
}

static void write_block()
{
    if (!playing && queued() >= START_QUEUED)
        request_start();   // a stall stops the voices with the ring full, and nothing below would restart them
    if (queued() + BLOCK > MAX_QUEUED)
    {
        // wait for room, a frame callback at a time, but never long: a stopped voice frees nothing, and an
        // output that drains far below the sample rate (Cemu's, when its audio device stalls; the steering never
        // goes below 60%) isn't waited for at all, rather than holding the emulator at a crawl
        for (int i = 0; i < 50 && playing && !output_stalled && queued() + BLOCK > MAX_QUEUED; i++)
        {
            OSSleepTicks(OSMillisecondsToTicks(1));
            stat_waits++;
        }
        if (queued() + BLOCK > MAX_QUEUED)
        {
            stat_drops++;
            return;
        }
    }
    const u32 w = write_pos;
    for (int ch = 0; ch < 2; ch++)
    {
        // BLOCK divides RING, so a block never wraps
        memcpy(&ring[ch][w], block[ch], BLOCK * sizeof(s16));
        DCFlushRange(&ring[ch][w], BLOCK * sizeof(s16));
    }
    write_pos = (w + BLOCK) & (RING - 1);
    if (!playing && queued() >= START_QUEUED)
        request_start();
}

void wii_audio_push_sample(s16 l, s16 r)
{
    if (!ax_ready || !aica_ready)
        return;
    block[0][block_fill] = l;
    block[1][block_fill] = r;
    const u32 al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
    if (al > stat_peak)
        stat_peak = al;
    if (ar > stat_peak)
        stat_peak = ar;
    if (++block_fill == BLOCK)
    {
        block_fill = 0;
        write_block();
    }
    if (++stat_samples == 44100)
    {
        const u32 n = speed_n;
        printf("[audio] 1 s of samples: peak %u, queued %u, playing %d, underruns %u, dropped blocks %u, waits %u ms, "
               "playback speed %.0f%%, stalls %u; key-ons %u (%u on zero data); mixer lag avg %u max %u\n", stat_peak, queued(),
               playing, stat_underruns, stat_drops, stat_waits, n ? speed_sum / n * 100.0f : 100.0f, stat_stalls, wiiu_keyons,
               wiiu_keyons_zero, g_aica_lag_n ? g_aica_lag_sum / g_aica_lag_n : 0, g_aica_lag_max);
        g_aica_lag_sum = g_aica_lag_n = g_aica_lag_max = 0;
        wiiu_keyons = wiiu_keyons_zero = 0;
        stat_samples = stat_peak = stat_drops = stat_waits = stat_underruns = stat_stalls = 0;
        speed_sum = 0.0f;
        speed_n = 0;
    }
}
