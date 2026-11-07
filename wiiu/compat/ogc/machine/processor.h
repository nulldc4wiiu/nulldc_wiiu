/* Wii U stand-in for libogc's <ogc/machine/processor.h>. */
#pragma once
#include <stdint.h>
#include <coreinit/interrupts.h>
#include <coreinit/time.h>
static inline uint32_t PPCMftb(void) { uint32_t t; __asm__ volatile("mftb %0" : "=r"(t)); return t; }
static inline uint64_t PPCUsToTicks(uint64_t us) { return (uint64_t)OSMicrosecondsToTicks(us); }
#define _CPU_ISR_Disable(level) do { (level) = (uint32_t)OSDisableInterrupts(); } while (0)
#define _CPU_ISR_Restore(level) OSRestoreInterrupts((BOOL)(level))
