// Diagnostics for problems seen on a console, where nothing but the UDP log is available.
#include <stdio.h>
#include "types.h"
#include "dc/mem/sh4_mem.h"
#include "dc/sh4/sh4_registers.h"
#include "dc/sh4/rec_v2/blockmanager.h"

// a game writing to the boot ROM (address 0) usually has a pointer that went to zero: once, log the SH4 state,
// the code around pc and pr (pr-0x40 on: the caller's setup), the stack, the DMA registers (MvC2's writes are
// aligned words, which a DMA with a zero destination would give), and which host code made the write: the
// return address _vmem_WriteMem* noted (walking the stack instead crashed: JIT code keeps no back chain), either
// in the JIT's code cache or, after subtracting (this function's address - its nm address), in nulldc.elf for
// addr2line. With the JIT, pc is where the current block started.
extern void *wiiu_last_writer;
extern u8 *CodeCache;
int wiiu_derail_dump;   // derail_dump=1: also save guest RAM

void wiiu_bootrom_write_dump()
{
    static bool dumped;
    if (dumped)
        return;
    dumped = true;
    // the register names are sh4_registers.h's macros (r, pr, gbr, ...) for Sh4cntx fields
    printf("[bootrom-write] pc %08x pr %08x gbr %08x vbr %08x spc %08x\n", (u32)next_pc, (u32)pr, (u32)gbr, (u32)vbr, (u32)spc);
    printf("[bootrom-write] host: dump fn %p, writer %p, SH4 code cache %p\n", (void *)&wiiu_bootrom_write_dump,
           wiiu_last_writer, (void *)CodeCache);
    const u32 block = bm_FindHost(wiiu_last_writer);
    printf("[bootrom-write] writer's block %08x\n", block);
    if (FILE *f = wiiu_derail_dump ? fopen("fs:/vol/external01/wiiu/nulldc/derail-ram.bin", "wb") : nullptr)
    {
        fwrite(mem_b.data, 1, RAM_SIZE, f);
        fclose(f);
        printf("[bootrom-write] RAM dumped\n");
    }
    for (int i = 0; i < 16; i += 4)
        printf("[bootrom-write] r%-2d %08x %08x %08x %08x\n", i, r[i], r[i + 1], r[i + 2], r[i + 3]);
    auto words16 = [](const char *what, u32 at, int n) {
        for (int k = 0; k < n; k += 16)
        {
            printf("[bootrom-write] %s %08x:", what, at + k * 2);
            for (int j = k; j < k + 16 && j < n; j++)
                printf(" %04x", ReadMem16(((at & ~1u) + j * 2)));
            printf("\n");
        }
    };
    auto words32 = [](const char *what, u32 at, int n) {
        for (int k = 0; k < n; k += 8)
        {
            printf("[bootrom-write] %s %08x:", what, at + k * 4);
            for (int j = k; j < k + 8 && j < n; j++)
                printf(" %08x", ReadMem32((at & ~3u) + j * 4));
            printf("\n");
        }
    };
    words16("code", (u32)next_pc - 16, 32);
    words16("code", (u32)pr - 0x40, 48);
    words32("stack", r[15], 16);
    words32("r14", r[14], 16);
    // GD-ROM DMA (start, length, direction, enable, start), SH4 DMAC channel 2 (SAR, DAR, DMATCR, CHCR) and 0
    printf("[bootrom-write] gd-dma %08x %08x %08x %08x %08x | dmac2 %08x %08x %08x %08x | dmac0 %08x %08x %08x %08x\n",
           ReadMem32(0xA05F7404), ReadMem32(0xA05F7408), ReadMem32(0xA05F740C), ReadMem32(0xA05F7414),
           ReadMem32(0xA05F7418), ReadMem32(0xFFA00020), ReadMem32(0xFFA00024), ReadMem32(0xFFA00028),
           ReadMem32(0xFFA0002C), ReadMem32(0xFFA00000), ReadMem32(0xFFA00004), ReadMem32(0xFFA00008),
           ReadMem32(0xFFA0000C));
}
