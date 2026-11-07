/* Wii U stand-in for libogc's <gccore.h>: just the parts nullDC's core uses, mapped onto CafeOS (wut). */
#pragma once
#include <stdint.h>
#include <unistd.h>
#include <coreinit/cache.h>
#include "ogc/system.h"
#include "ogc/lwp_watchdog.h"
#include "ogc/video.h"
#include "ogc/pad.h"
#include "ogc/gx.h"
#include "ogc/ios.h"
#include "ogc/usb.h"
#include "ogc/machine/processor.h"

// libogc's gctypes.h; the same types plugins/plugin_header.h declares
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
typedef float f32;
typedef double f64;
typedef volatile uint8_t vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;
typedef volatile uint64_t vu64;
typedef volatile int32_t vs32;
