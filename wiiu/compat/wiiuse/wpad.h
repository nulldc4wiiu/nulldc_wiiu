/* Wii U stand-in for libogc's <wiiuse/wpad.h>: the types and constants nullDC's controller code uses.
 * The functions (wiiu/ogc_compat.cpp) fill them from KPAD (wiiu/kpad_wiiu.cpp).
 * Button and expansion values match libogc. */
#pragma once
#include <stdint.h>

#define WPAD_CHAN_ALL               -1
#define WPAD_FMT_BTNS               0
#define WPAD_FMT_BTNS_ACC           1
#define WPAD_FMT_BTNS_ACC_IR        2

#define WPAD_BUTTON_2               0x0001
#define WPAD_BUTTON_1               0x0002
#define WPAD_BUTTON_B               0x0004
#define WPAD_BUTTON_A               0x0008
#define WPAD_BUTTON_MINUS           0x0010
#define WPAD_BUTTON_HOME            0x0080
#define WPAD_BUTTON_LEFT            0x0100
#define WPAD_BUTTON_RIGHT           0x0200
#define WPAD_BUTTON_DOWN            0x0400
#define WPAD_BUTTON_UP              0x0800
#define WPAD_BUTTON_PLUS            0x1000

#define WPAD_NUNCHUK_BUTTON_Z       (0x0001 << 16)
#define WPAD_NUNCHUK_BUTTON_C       (0x0002 << 16)

#define WPAD_CLASSIC_BUTTON_UP      (0x0001 << 16)
#define WPAD_CLASSIC_BUTTON_LEFT    (0x0002 << 16)
#define WPAD_CLASSIC_BUTTON_ZR      (0x0004 << 16)
#define WPAD_CLASSIC_BUTTON_X       (0x0008 << 16)
#define WPAD_CLASSIC_BUTTON_A       (0x0010 << 16)
#define WPAD_CLASSIC_BUTTON_Y       (0x0020 << 16)
#define WPAD_CLASSIC_BUTTON_B       (0x0040 << 16)
#define WPAD_CLASSIC_BUTTON_ZL      (0x0080 << 16)
#define WPAD_CLASSIC_BUTTON_FULL_R  (0x0200 << 16)
#define WPAD_CLASSIC_BUTTON_PLUS    (0x0400 << 16)
#define WPAD_CLASSIC_BUTTON_HOME    (0x0800 << 16)
#define WPAD_CLASSIC_BUTTON_MINUS   (0x1000 << 16)
#define WPAD_CLASSIC_BUTTON_FULL_L  (0x2000 << 16)
#define WPAD_CLASSIC_BUTTON_DOWN    (0x4000 << 16)
#define WPAD_CLASSIC_BUTTON_RIGHT   (0x8000 << 16)

#define WPAD_EXP_NONE               0
#define WPAD_EXP_NUNCHUK            1
#define WPAD_EXP_CLASSIC            2

typedef struct vec2b_t { uint8_t x, y; } vec2b_t;
typedef struct vec3w_t { uint16_t x, y, z; } vec3w_t;
typedef struct joystick_t { vec2b_t max, min, center, pos; float ang, mag; } joystick_t;
typedef struct nunchuk_t { uint32_t btns, btns_held, btns_released; joystick_t js; } nunchuk_t;
typedef struct classic_ctrl_t {
    uint32_t btns, btns_held, btns_released;
    float r_shoulder, l_shoulder;
    joystick_t ljs, rjs;
    uint8_t type;
} classic_ctrl_t;
typedef struct expansion_t { int type; nunchuk_t nunchuk; classic_ctrl_t classic; } expansion_t;
typedef struct ir_t { int valid; float x, y; } ir_t;
typedef struct WPADData {
    int16_t err;
    uint32_t data_present, battery_level;
    uint32_t btns_h, btns_l, btns_d, btns_u;
    ir_t ir;
    vec3w_t accel;
    expansion_t exp;
} WPADData;

#ifdef __cplusplus
extern "C" {
#endif
int32_t WPAD_Init(void);
int32_t WPAD_ScanPads(void);
uint32_t WPAD_ButtonsHeld(int chan);
WPADData *WPAD_Data(int chan);
int32_t WPAD_SetDataFormat(int chan, int fmt);
int32_t WPAD_SetVRes(int chan, uint32_t xres, uint32_t yres);
#ifdef __cplusplus
}
#endif
