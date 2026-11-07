// Time spent in the emulator thread's main C paths, for the [sh4-prof] line next to the speed report (SPG.cpp).
// What's left over is the translated SH-4 code itself. One inline time base read on each side of a call; the
// counters are only reset, never locked, so they're for a rough split, not exact accounting.
#pragma once
#ifdef __WIIU__
#include "types.h"
#include <ogc/lwp_watchdog.h>

enum { PROF_TIMESLICE, PROF_INTERRUPT, PROF_SQW, PROF_GETCODE, PROF_READMEM, PROF_WRITEMEM,
       PROF_TS_MEDIUM, PROF_TS_TMU, PROF_TS_PVR,   // these three: parts of PROF_TIMESLICE
       PROF_TA_DECODE, PROF_COUNT };                  // part of PROF_SQW (and DMA): the TA state machine
extern u64 g_wiiu_prof[PROF_COUNT];
extern bool g_wiiu_prof_on;   // prof=1 in wiiu.cfg (the test apps): these wrap every slow call, so off otherwise

struct WiiuProfScope
{
    u32 site;
    u64 t0;
    WiiuProfScope(u32 s) : site(s), t0(g_wiiu_prof_on ? gettime_fast() : 0) {}
    ~WiiuProfScope()
    {
        if (g_wiiu_prof_on)
            g_wiiu_prof[site] += gettime_fast() - t0;
    }
};
#define WIIU_PROF(site) WiiuProfScope wiiu_prof_scope_(site)
#else
#define WIIU_PROF(site) do { } while (0)
#endif
