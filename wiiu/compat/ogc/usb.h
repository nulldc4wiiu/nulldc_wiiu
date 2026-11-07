/* Wii U stand-in for libogc's <ogc/usb.h>: no Wii USB stack here. */
#pragma once
#include <stdint.h>
static inline int32_t USB_Initialize(void) { return -1; }
