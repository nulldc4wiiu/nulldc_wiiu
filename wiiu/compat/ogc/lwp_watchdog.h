/* Wii U stand-in for libogc's <ogc/lwp_watchdog.h>: time base reads and tick conversions.
 * The Wii U time base runs at 62.15625 MHz (libogc's Wii value is 60.75 MHz); OSTime is the same counter. */
#pragma once
#include <stdint.h>
#include <coreinit/time.h>
#include "machine/processor.h"

static inline uint64_t gettime(void) { return (uint64_t)OSGetSystemTime(); }
// The time base read inline, for the profiling counters that run per sample: OSGetSystemTime is a library call
// (and in Cemu a slow HLE one: 176,000 a second slowed the BIOS to 18%). Only differences mean anything.
static inline uint64_t gettime_fast(void)
{
    uint32_t hi, lo, hi2;
    do
    {
        __asm__ volatile("mftbu %0; mftb %1; mftbu %2" : "=r"(hi), "=r"(lo), "=r"(hi2));
    } while (hi != hi2);
    return ((uint64_t)hi << 32) | lo;
}
static inline uint32_t gettick(void) { return (uint32_t)OSGetSystemTick(); }
static inline uint64_t ticks_to_secs(uint64_t t) { return (uint64_t)OSTicksToSeconds((OSTime)t); }
static inline uint64_t ticks_to_millisecs(uint64_t t) { return (uint64_t)OSTicksToMilliseconds((OSTime)t); }
static inline uint64_t ticks_to_microsecs(uint64_t t) { return (uint64_t)OSTicksToMicroseconds((OSTime)t); }
static inline uint64_t ticks_to_nanosecs(uint64_t t) { return (uint64_t)OSTicksToNanoseconds((OSTime)t); }
static inline uint64_t diff_ticks(uint64_t a, uint64_t b) { return b - a; }
static inline uint64_t secs_to_ticks(uint64_t s) { return (uint64_t)OSSecondsToTicks(s); }
static inline uint64_t millisecs_to_ticks(uint64_t ms) { return (uint64_t)OSMillisecondsToTicks(ms); }
static inline uint64_t microsecs_to_ticks(uint64_t us) { return (uint64_t)OSMicrosecondsToTicks(us); }
#define TB_TIMER_CLOCK ((uint32_t)(OSTimerClockSpeed / 1000))
