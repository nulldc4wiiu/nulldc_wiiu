/* JIT code memory on the Wii U. Generated code may only run from the codegen area and only on the codegen
 * core, and the area is either writable or executable, never both (OSSwitchSecCodeGenMode). Writers call
 * wiiu_codegen_write_begin() before storing code; wiiu_codegen_executable() then flushes the data cache,
 * switches the area back to executable and invalidates the instruction cache for the range.
 * Checked on a real console: OSCodegenCopy is refused there. */
#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
void *wiiu_codegen_alloc(uint32_t size, const char *who);
void wiiu_codegen_writable(void);
void wiiu_codegen_executable(const void *addr, uint32_t size);
// a stub in the codegen area that jumps to target (lis/ori r12, mtctr, bctr), for branches whose target is
// beyond the +-32 MB reach of b/bl, such as system library functions on real hardware
void *wiiu_codegen_veneer(void *target);
extern volatile int wiiu_codegen_rw;
#ifdef __cplusplus
}
#endif
static inline void wiiu_codegen_write_begin(void)
{
    if (!wiiu_codegen_rw)
        wiiu_codegen_writable();
}
