// Codegen area management for the SH4 and ARM7 JITs (see compat/wiiu_codegen.h).
#include <coreinit/cache.h>
#include <coreinit/codegen.h>
#include <coreinit/core.h>
#include <stdio.h>
#include "wiiu_codegen.h"

volatile int wiiu_codegen_rw;
static uint8_t *area, *next, *end;
static unsigned switch_failures;

void *wiiu_codegen_alloc(uint32_t size, const char *who)
{
    if (!area)
    {
        void *a = nullptr;
        uint32_t s = 0;
        OSGetCodegenVirtAddrRange(&a, &s);
        area = next = (uint8_t *)a;
        end = area + s;
        // the area starts out writable (sec mode 0) on hardware
        wiiu_codegen_rw = 1;
    }
    next = (uint8_t *)(((uintptr_t)next + 31) & ~(uintptr_t)31);
    if (!area || next + size > end)
    {
        printf("[wiiu] codegen: no room for %s (%u bytes; area %p, %u bytes left)\n", who, size, area,
               area ? (unsigned)(end - next) : 0);
        return nullptr;
    }
    void *p = next;
    next += size;
    printf("[wiiu] codegen: %s gets %u KB at %p\n", who, size / 1024, p);
    return p;
}

static void switch_mode(OSCodegenSecMode mode)
{
    // nonzero means success; real hardware returns a pointer-like value, not 1
    if (!OSSwitchSecCodeGenMode(mode) && switch_failures++ < 10)
        printf("[wiiu] codegen: switch to %s failed on core %u (codegen core %u)\n",
               mode == CODEGEN_RW_ ? "RW" : "RX", OSGetCoreId(), OSGetCodegenCore());
}

void wiiu_codegen_writable(void)
{
    switch_mode(CODEGEN_RW_);
    wiiu_codegen_rw = 1;
}

void wiiu_codegen_executable(const void *addr, uint32_t size)
{
    uintptr_t start = (uintptr_t)addr & ~(uintptr_t)31;
    uint32_t len = (uint32_t)(((uintptr_t)addr + size + 31) & ~(uintptr_t)31) - start;
    if (len)
        DCFlushRange((void *)start, len);
    if (wiiu_codegen_rw)
    {
        switch_mode(CODEGEN_R_X);
        wiiu_codegen_rw = 0;
    }
    if (len)
        ICInvalidateRange((void *)start, len);
}

// one 16-byte veneer per far target, created on first use
static const int VENEER_MAX = 1024;
static uint32_t *veneers;
static void *veneer_targets[VENEER_MAX];
static int veneer_count;

void *wiiu_codegen_veneer(void *target)
{
    for (int i = 0; i < veneer_count; i++)
        if (veneer_targets[i] == target)
            return veneers + i * 4;
    if (!veneers)
        veneers = (uint32_t *)wiiu_codegen_alloc(VENEER_MAX * 16, "far-branch veneers");
    if (!veneers || veneer_count == VENEER_MAX)
    {
        printf("[wiiu] codegen: out of veneers for %p\n", target);
        return nullptr;
    }
    uint32_t a = (uint32_t)(uintptr_t)target;
    uint32_t *v = veneers + veneer_count * 4;
    wiiu_codegen_write_begin();
    v[0] = 0x3D800000 | (a >> 16);              // lis   r12,target@h
    v[1] = 0x618C0000 | (a & 0xFFFF);           // ori   r12,r12,target@l
    v[2] = 0x7D8903A6;                          // mtctr r12
    v[3] = 0x4E800420;                          // bctr
    wiiu_codegen_executable(v, 16);
    veneer_targets[veneer_count++] = target;
    printf("[wiiu] codegen: veneer %d at %p -> %p\n", veneer_count, v, target);
    return v;
}
