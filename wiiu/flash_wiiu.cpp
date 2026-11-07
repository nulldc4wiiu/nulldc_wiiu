// BIOS flash fix-up at boot. The BIOS keeps the date it was last set to in a system-config block of the
// flash user partition, and asks for the date again when the RTC is far past it. The RTC starts from the
// Wii U clock (RTC_HOST), so without this every boot stops on the Set Date/Time screen. Like Flycast, write
// the RTC into the newest system-config block. Changes nothing when the flash has no such block yet (the
// BIOS asks once, and FLASH_SAVE keeps the block it writes).
#include <stdio.h>
#include "types.h"

static const u32 USER_PARTITION = 0x1C000, USER_SIZE = 0x4000, BLOCK = 64, SYSCFG_ID = 5;

static u16 crc16(const u8 *p, u32 len)
{
    u32 n = 0xFFFF;
    for (u32 i = 0; i < len; i++)
    {
        n ^= p[i] << 8;
        for (int b = 0; b < 8; b++)
            n = n & 0x8000 ? (n << 1) ^ 0x1021 : n << 1;
    }
    return ~n & 0xFFFF;
}

// flash: the 128 KB flash image in file order; rtc: seconds since 1950, as the AICA RTC counts
void wiiu_flash_sync_time(u8 *flash, u32 rtc)
{
    u8 *syscfg = nullptr;
    // block 0 is the partition header, the last blocks hold the allocation bitmap; ids and CRCs are little-endian
    for (u32 off = USER_PARTITION + BLOCK; off < USER_PARTITION + USER_SIZE - BLOCK; off += BLOCK)
    {
        u8 *b = flash + off;
        if ((b[0] | b[1] << 8) == SYSCFG_ID && crc16(b, BLOCK - 2) == (b[BLOCK - 2] | b[BLOCK - 1] << 8))
            syscfg = b;
    }
    if (!syscfg)
    {
        printf("[wiiu] flash: no system-config block, the BIOS will ask for the date\n");
        return;
    }
    const u32 old = syscfg[2] | syscfg[3] << 8 | syscfg[4] << 16 | (u32)syscfg[5] << 24;
    syscfg[2] = rtc & 0xFF;   // time_lo, then time_hi
    syscfg[3] = rtc >> 8 & 0xFF;
    syscfg[4] = rtc >> 16 & 0xFF;
    syscfg[5] = rtc >> 24;
    const u16 crc = crc16(syscfg, BLOCK - 2);
    syscfg[BLOCK - 2] = crc & 0xFF;
    syscfg[BLOCK - 1] = crc >> 8;
    printf("[wiiu] flash: system-config time %08x -> %08x\n", old, rtc);
}
