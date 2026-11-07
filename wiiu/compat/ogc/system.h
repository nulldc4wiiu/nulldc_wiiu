/* Wii U stand-in for libogc's <ogc/system.h>. The MEM2 arena is a block from the CafeOS heap
 * (wiiu/ogc_compat.cpp), interrupts map onto OSDisableInterrupts/OSRestoreInterrupts. */
#pragma once
#include <stdint.h>
#include <coreinit/interrupts.h>

#ifdef __cplusplus
extern "C" {
#endif
void *SYS_GetArena2Lo(void);
void *SYS_GetArena2Hi(void);
void SYS_SetArena2Lo(void *lo);
void SYS_SetArena2Hi(void *hi);
uint32_t SYS_GetArena2Size(void);
#ifdef __cplusplus
}
#endif

static inline uint32_t IRQ_Disable(void) { return (uint32_t)OSDisableInterrupts(); }
static inline void IRQ_Restore(uint32_t level) { OSRestoreInterrupts((BOOL)level); }
