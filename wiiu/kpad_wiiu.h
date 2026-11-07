// Wii Remotes and their extensions, Classic and Pro Controllers through KPAD (kpad_wiiu.cpp), reported in libogc's
// terms for ogc_compat.cpp's WPAD functions. wut's padscore headers and the libogc-style compat/wiiuse/wpad.h
// use the same names for different values, so they never meet in one file.
#pragma once
#include <stdint.h>

struct WiiuPad
{
    bool connected;
    int ext;            // 0 none, 1 Nunchuk, 2 Classic or Pro Controller (libogc's WPAD_EXP_*)
    uint32_t core;      // Wii Remote buttons, libogc's WPAD_BUTTON_* values
    uint32_t exp;       // extension buttons: libogc's WPAD_CLASSIC_BUTTON_* / WPAD_NUNCHUK_BUTTON_* >> 16
    float lx, ly;       // the extension's (left) stick, -1..1, up positive
    bool pointing;      // the Wii Remote's pointer (sensor bar seen): px, py are valid
    float px, py;       // pointer position, -1..1 across the screen, (-1,-1) top left
};

void wiiu_kpad_init();
// the newest state of channel 0..3; keeps the last one when there is no new sample
void wiiu_kpad_read(int chan, WiiuPad *pad);
// the pointer (KPAD's "DPD"), for the light gun
void wiiu_kpad_pointer(int chan, bool on);
