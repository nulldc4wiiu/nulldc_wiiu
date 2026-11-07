// Fastmem on Wii U: the Wii's FASTMEM (wii/wii_fastmem.cpp builds its own page table and DSI handler, which needs
// supervisor mode) redone with what a user-mode Cafe OS app has, checked on a console by scratch/fastmem-spike:
//   * a 512 MB, 512 MB-aligned block of virtual space (OSAllocVirtAddr) stands for the Dreamcast's 29-bit physical
//     space; the JIT turns an address into window + (addr & 0x1FFFFFFF) (fm_ea in wii_driver.cpp)
//   * the guest RAM and VRAM arrays are mapped into it with OSMapMemory at their Dreamcast offsets, mirrors included,
//     as aliases of the same physical memory (main RAM x4 at 0x0C000000, VRAM 64-bit x2 at 0x04000000 and 0x06000000)
//   * everything else stays unmapped, area 0 above all (store queue addresses mask onto it, see wii_fastmem.h); an
//     access there takes a DSI, and main_wiiu.cpp's DSI callback hands it to wiiu_fastmem_dsi, which has
//     rec_fastmem_patch rewrite the site into a branch to a slow-path trampoline and retries it
// fastmem=1 in wiiu.cfg turns it on; it self-tests the window and stays off if anything fails. fastmem=2 is the Cemu
// test mode (Cemu neither aliases mapped memory nor raises DSIs): no window, and every site gets its trampoline as it
// is compiled, so the decoder and trampolines run on every memory access the game makes.
#include <coreinit/context.h>
#include <coreinit/memorymap.h>
#include <malloc.h>
#include <stdio.h>
#include <string.h>
#include "types.h"
#include "dc/mem/sh4_mem.h"
#include "dc/mem/sh4_internal_reg.h"
#include "dc/pvr/pvr_if.h"
#include "wii/wii_fastmem.h"

extern "C" int get_fastmem_preset();
extern "C" int wiiu_fastmem_patch(unsigned int pc, u32 dar);   // wii_driver.cpp

int g_wii_fastmem_active = 0;
u32 g_wiiu_fm_window = 0;
int g_wiiu_fm_prepatch = 0;
static const u32 WINDOW_SIZE = 0x20000000;
static volatile int probing, probe_faulted;

static u32 patched;   // sites patched after a fault since boot
int g_exp_fm_map;
extern VArray2 aica_ram;
extern Array<u8> OnChipRAM;

int WiiFastmem_ReserveHtab(void) { return 1; }   // no page table to reserve

const char *wiiu_fastmem_status()
{
    static char s[48];
    if (!g_wii_fastmem_active)
        return "";
    snprintf(s, sizeof(s), " fastmem patched %u", patched);
    return s;
}

// From the DSI callback: true when the fault was a fastmem access and the site is patched (retry it)
bool wiiu_fastmem_dsi(OSContext *c)
{
    if (!g_wiiu_fm_window || c->dar - g_wiiu_fm_window >= WINDOW_SIZE)
        return false;
    if (probing)
    {
        probe_faulted = 1;
        c->srr0 += 4;   // skip the probing load
        return true;
    }
    if (!g_wii_fastmem_active || wiiu_fastmem_patch(c->srr0, c->dar) != 0)
        return false;
    patched++;
    return true;
}

static bool map(u32 dc, u32 size, void *host)
{
    const u32 phys = OSEffectiveToPhysical((u32)(uintptr_t)host);
    const BOOL ok = phys && OSMapMemory(g_wiiu_fm_window + dc, phys, size, OS_MAP_MEMORY_READ_WRITE);
    if (!ok)
        printf("[fastmem] mapping %08x (+%08x) to %p (phys %08x) failed\n", dc, size, host, phys);
    return ok;
}

// one aliased word, both ways (wii_fastmem.cpp test_alias)
static bool alias_ok(u32 dc, void *host_ptr)
{
    volatile u32 *win = (volatile u32 *)(uintptr_t)(g_wiiu_fm_window + dc), *host = (volatile u32 *)host_ptr;
    const u32 save = *host;
    *win = 0xA55A0000 | (dc >> 16);
    bool ok = *host == (0xA55A0000 | (dc >> 16));
    *host = 0x5AA50000 | (dc & 0xFFFF);
    ok &= *win == (0x5AA50000 | (dc & 0xFFFF));
    *host = save;
    if (!ok)
        printf("[fastmem] self-test failed at %08x\n", dc);
    return ok;
}

void WiiFastmem_Init(void)
{
    static bool tried;
    if (tried)
        return;
    tried = true;
    const int mode = get_fastmem_preset();
    if (mode == 2)
    {
        g_wiiu_fm_window = 0xC0000000;   // only ORed into the shapes, which never run unpatched in this mode
        g_wiiu_fm_prepatch = 1;
        g_wii_fastmem_active = 1;
        printf("[fastmem] test mode: every site is patched to its trampoline at compile time\n");
        return;
    }
    if (mode != 1)
        return;
    if (!mem_b.data || !vram.data)
    {
        printf("[fastmem] guest memory not reserved yet: off\n");
        return;
    }
    u32 va = 0, vs = 0;
    OSGetMapVirtAddrRange(&va, &vs);
    if (!vs)
    {
        printf("[fastmem] no mappable virtual range (Cemu?): off\n");
        return;
    }
    u32 w = OSAllocVirtAddr(0, WINDOW_SIZE, WINDOW_SIZE);
    if (!w)
        w = OSAllocVirtAddr(0, WINDOW_SIZE, 0x20000);
    if (!w || (w & (WINDOW_SIZE - 1)))
    {
        printf("[fastmem] no 512 MB-aligned window (got %08x): off\n", w);
        if (w)
            OSFreeVirtAddr(w, WINDOW_SIZE);
        return;
    }
    g_wiiu_fm_window = w;
    bool ok = true;
    for (u32 m = 0; m < 4; m++)
        ok &= map(0x0C000000 + m * RAM_SIZE, RAM_SIZE, mem_b.data);
    ok &= map(0x04000000, VRAM_SIZE, vram.data) && map(0x04800000, VRAM_SIZE, vram.data);
    ok &= map(0x06000000, VRAM_SIZE, vram.data) && map(0x06800000, VRAM_SIZE, vram.data);
    if (ok)
    {
        ok &= alias_ok(0x0C000010, mem_b.data + 0x10) && alias_ok(0x0D000010, mem_b.data + 0x10);
        ok &= alias_ok(0x0E000010, mem_b.data + 0x10) && alias_ok(0x0F000010, mem_b.data + 0x10);
        ok &= alias_ok(0x0CFFF00C, mem_b.data + 0xFFF00C);
        ok &= alias_ok(0x04000020, vram.data + 0x20) && alias_ok(0x04800020, vram.data + 0x20);
        ok &= alias_ok(0x06000020, vram.data + 0x20);
    }
    if (ok)
    {
        // and an unmapped page must fault into our callback
        probing = 1;
        (void)*(volatile u32 *)(uintptr_t)(w + 0x00700000);
        probing = 0;
        if (!probe_faulted)
            printf("[fastmem] an unmapped window page did not fault\n");
        ok = probe_faulted;
    }
    if (!ok)
    {
        g_wiiu_fm_window = 0;   // the mappings stay, unused
        printf("[fastmem] self-test failed: off\n");
        return;
    }
    g_wii_fastmem_active = 1;
    printf("[fastmem] active: window %08x, RAM %p, VRAM %p\n", w, mem_b.data, vram.data);
    // exp_fm_map: bit 0 sound RAM, bit 1 on-chip RAM. Each stays on its handlers if it can't be mapped.
    if ((g_exp_fm_map & 1) && map(0x00800000, 2 * 1024 * 1024, aica_ram.data) && alias_ok(0x00800010, aica_ram.data + 0x10))
        printf("[fastmem] sound RAM mapped\n");
    if (g_exp_fm_map & 2)
    {
        // a whole 128 KB map unit for 8 KB: offsets past 0x1FFF don't mirror, which the census found no game writes
        u8 *ocr = (u8 *)memalign(0x20000, 0x20000);
        if (ocr)
        {
            memcpy(ocr, OnChipRAM.data, OnChipRAM_SIZE);
            if (map(0x1E000000, 0x20000, ocr) && alias_ok(0x1E000010, ocr + 0x10))
            {
                OnChipRAM.data = ocr;
                printf("[fastmem] on-chip RAM mapped\n");
            }
        }
    }
}
