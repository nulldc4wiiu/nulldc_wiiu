/* Wii U stand-in for libogc's <ogc/pad.h> (GameCube pads); values match libogc, functions report none. */
#pragma once
#include <stdint.h>
#define PAD_CHAN0           0
#define PAD_BUTTON_LEFT     0x0001
#define PAD_BUTTON_RIGHT    0x0002
#define PAD_BUTTON_DOWN     0x0004
#define PAD_BUTTON_UP       0x0008
#define PAD_TRIGGER_Z       0x0010
#define PAD_TRIGGER_R       0x0020
#define PAD_TRIGGER_L       0x0040
#define PAD_BUTTON_A        0x0100
#define PAD_BUTTON_B        0x0200
#define PAD_BUTTON_X        0x0400
#define PAD_BUTTON_Y        0x0800
#define PAD_BUTTON_MENU     0x1000
#define PAD_BUTTON_START    0x1000
#ifdef __cplusplus
extern "C" {
#endif
uint32_t PAD_Init(void);
uint32_t PAD_ScanPads(void);
uint16_t PAD_ButtonsHeld(int pad);
int8_t PAD_StickX(int pad);
int8_t PAD_StickY(int pad);
int8_t PAD_SubStickX(int pad);
int8_t PAD_SubStickY(int pad);
uint8_t PAD_TriggerL(int pad);
uint8_t PAD_TriggerR(int pad);
#ifdef __cplusplus
}
#endif
