// libogc functions the core still calls, on CafeOS. Input is the GamePad through VPAD and Wii Remotes, Classic and
// Pro Controllers through KPAD, the MEM2 arena is one block from the default heap, and VSync is a timed sleep.
#include <malloc.h>
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <vpad/input.h>
#include <gccore.h>
#include <wiiuse/wpad.h>
#include <stdarg.h>
#include "types.h"
#include "plugs/libwiidrc/wiidrc.h"
#include "plugs/libsicksaxis/libsicksaxis/sicksaxis.h"
#include "kpad_wiiu.h"

// _vmem_reserve() bump-allocates the Dreamcast's RAM, VRAM and ARAM (plus optional caches) from here.
static const u32 ARENA2_SIZE = 64 * 1024 * 1024;
static u8 *arena2_lo, *arena2_hi;

static void arena2_init()
{
    if (arena2_lo)
        return;
    // 128 KB: fastmem maps the RAM and VRAM carved from here into its window (OSMapMemory, fastmem_wiiu.cpp)
    arena2_lo = (u8 *)memalign(0x20000, ARENA2_SIZE);
    arena2_hi = arena2_lo ? arena2_lo + ARENA2_SIZE : nullptr;
    printf("[wiiu] arena: %u MB at %p\n", ARENA2_SIZE >> 20, arena2_lo);
}

extern "C" void *SYS_GetArena2Lo(void) { arena2_init(); return arena2_lo; }
extern "C" void *SYS_GetArena2Hi(void) { arena2_init(); return arena2_hi; }
extern "C" void SYS_SetArena2Lo(void *lo) { arena2_lo = (u8 *)lo; }
extern "C" void SYS_SetArena2Hi(void *hi) { arena2_hi = (u8 *)hi; }
extern "C" u32 SYS_GetArena2Size(void) { arena2_init(); return (u32)(arena2_hi - arena2_lo); }

extern "C" void VIDEO_WaitVSync(void) { OSSleepTicks(OSMicrosecondsToTicks(16683)); }
extern "C" VIRetraceCallback VIDEO_SetPostRetraceCallback(VIRetraceCallback callback) { return nullptr; }

// Wii Remotes, Classic and Pro Controllers: KPAD (kpad_wiiu.cpp) filling libogc's WPAD data
extern "C" int get_controller_type(void);   // 1: the light gun
extern "C" { int wiiu_pad_shift = 0; }      // 0: GamePad+Pad1 share P1; 1: GamePad=P1, Pad1=P2, Pad2=P3, Pad3=P4
static WPADData wpad[4], no_wpad;
static uint32_t wpad_held[4];

static int map_wpad_chan(int chan)
{
    if (wiiu_pad_shift && get_controller_type() != 1)
        return chan - 1;
    return chan;
}
static uint32_t ir_w[4] = { 640, 640, 640, 640 }, ir_h[4] = { 480, 480, 480, 480 };   // WPAD_SetVRes
static bool kpad_ready;

static void stick(joystick_t &js, float x, float y)
{
    auto axis = [](float v) { int p = 128 + (int)(v * 127.0f); return (uint8_t)(p < 0 ? 0 : p > 255 ? 255 : p); };
    js.min = { 0, 0 };
    js.center = { 128, 128 };
    js.max = { 255, 255 };
    js.pos = { axis(x), axis(y) };
}

extern "C" int32_t WPAD_Init(void) { return 0; }
extern "C" int32_t WPAD_ScanPads(void)
{
    if (!kpad_ready)
    {
        wiiu_kpad_init();
        kpad_ready = true;
    }
    no_wpad = {};
    no_wpad.err = -1;
    for (int c = 0; c < 4; c++)
    for (int c = 0; c < 4; c++)
    {
        static WiiuPad pad[4];
        wiiu_kpad_read(c, &pad[c]);
        WPADData &d = wpad[c];
        d = {};
        d.err = pad[c].connected ? 0 : -1;
        d.data_present = pad[c].connected;
        d.exp.type = pad[c].ext;
        if (pad[c].ext == WPAD_EXP_NUNCHUK)
        {
            d.exp.nunchuk.btns = d.exp.nunchuk.btns_held = pad[c].exp << 16;
            stick(d.exp.nunchuk.js, pad[c].lx, pad[c].ly);
        }
        else if (pad[c].ext == WPAD_EXP_CLASSIC)
        {
            d.exp.classic.btns = d.exp.classic.btns_held = pad[c].exp << 16;
            stick(d.exp.classic.ljs, pad[c].lx, pad[c].ly);
            stick(d.exp.classic.rjs, 0.0f, 0.0f);
        }
        d.btns_h = wpad_held[c] = pad[c].core | (pad[c].exp << 16);
        // the pointer, as libogc's IR in the WPAD_SetVRes resolution (the light gun, drkMapleLightGun.cpp)
        if (pad[c].pointing && pad[c].px >= -1.0f && pad[c].px < 1.0f && pad[c].py >= -1.0f && pad[c].py < 1.0f)
        {
            d.ir.valid = 1;
            d.ir.x = (pad[c].px + 1.0f) * 0.5f * ir_w[c];
            d.ir.y = (pad[c].py + 1.0f) * 0.5f * ir_h[c];
        }
    }
    return 0;
}
extern "C" uint32_t WPAD_ButtonsHeld(int chan)
{
    const int c = map_wpad_chan(chan);
    return c >= 0 && c < 4 ? wpad_held[c] : 0;
}
extern "C" WPADData *WPAD_Data(int chan)
{
    const int c = map_wpad_chan(chan);
    return c >= 0 && c < 4 ? &wpad[c] : &no_wpad;
}
extern "C" int32_t WPAD_SetDataFormat(int chan, int fmt)
{
    // a format with IR turns the pointer on
    if (chan >= 0 && chan < 4)
    {
        if (!kpad_ready)
        {
            wiiu_kpad_init();
            kpad_ready = true;
        }
        wiiu_kpad_pointer(chan, fmt == WPAD_FMT_BTNS_ACC_IR);
    }
    return 0;
}
extern "C" int32_t WPAD_SetVRes(int chan, uint32_t xres, uint32_t yres)
{
    if (chan >= 0 && chan < 4 && xres && yres)
    {
        ir_w[chan] = xres;
        ir_h[chan] = yres;
    }
    return 0;
}

extern "C" uint32_t PAD_Init(void) { return 0; }
extern "C" uint32_t PAD_ScanPads(void) { return 0; }
extern "C" uint16_t PAD_ButtonsHeld(int pad) { return 0; }
extern "C" int8_t PAD_StickX(int pad) { return 0; }
extern "C" int8_t PAD_StickY(int pad) { return 0; }
extern "C" int8_t PAD_SubStickX(int pad) { return 0; }
extern "C" int8_t PAD_SubStickY(int pad) { return 0; }
extern "C" uint8_t PAD_TriggerL(int pad) { return 0; }
extern "C" uint8_t PAD_TriggerR(int pad) { return 0; }

// libwiidrc drives the GamePad in vWii mode; here VPAD reads it and reports it in libwiidrc's terms, so the
// controller plugin's GamePad mapping works unchanged
static u32 drc_buttons;
static s16 drc_lx, drc_ly;
static volatile bool drc_home;
static volatile bool drc_touch;
static volatile u16 drc_tx, drc_ty;
#ifdef WIIU_INSTRUMENT
int wiiu_touch_test_x = -1, wiiu_touch_test_y, wiiu_touch_test_at;   // touch_test=x,y,s: taps there every 3 s from s seconds
extern u32 wiiu_vblanks;
#endif
// gun_v2 (drkMapleLightGun.cpp): both sensor bars powered, so a Remote aims at the TV or the GamePad
extern "C" void WPADSetSensorBar(int32_t on);
extern "C" void wiiu_sensor_bars_on()
{
    static bool on;
    if (on)
        return;
    WPADSetSensorBar(1);
    VPADSetSensorBar(VPAD_CHAN_0, TRUE);
    on = true;
    printf("[LightGun] sensor bars on (TV and GamePad)\n");
}
// the GamePad's touch: true and the point (854x480) while touched
extern "C" bool WiiDRC_Touch(u16 *x, u16 *y)
{
#ifdef WIIU_INSTRUMENT
    if (wiiu_touch_test_x >= 0 && wiiu_vblanks > (u32)wiiu_touch_test_at * 60 && wiiu_vblanks % 180 < 4)
    {
        *x = (u16)wiiu_touch_test_x;
        *y = (u16)wiiu_touch_test_y;
        return true;
    }
#endif
    *x = drc_tx;
    *y = drc_ty;
    return drc_touch;
}

bool WiiDRC_Inited() { return true; }
bool WiiDRC_ScanPads()
{
    VPADStatus st;
    VPADReadError err;
    if (VPADRead(VPAD_CHAN_0, &st, 1, &err) != 1 || err != VPAD_READ_SUCCESS)
        return false;   // no new sample; keep the last state
    static const struct { u32 vpad, drc; } map[] = {
        { VPAD_BUTTON_A, WIIDRC_BUTTON_A },         { VPAD_BUTTON_B, WIIDRC_BUTTON_B },
        { VPAD_BUTTON_X, WIIDRC_BUTTON_X },         { VPAD_BUTTON_Y, WIIDRC_BUTTON_Y },
        { VPAD_BUTTON_LEFT, WIIDRC_BUTTON_LEFT },   { VPAD_BUTTON_RIGHT, WIIDRC_BUTTON_RIGHT },
        { VPAD_BUTTON_UP, WIIDRC_BUTTON_UP },       { VPAD_BUTTON_DOWN, WIIDRC_BUTTON_DOWN },
        { VPAD_BUTTON_ZL, WIIDRC_BUTTON_ZL },       { VPAD_BUTTON_ZR, WIIDRC_BUTTON_ZR },
        { VPAD_BUTTON_L, WIIDRC_BUTTON_L },         { VPAD_BUTTON_R, WIIDRC_BUTTON_R },
        { VPAD_BUTTON_PLUS, WIIDRC_BUTTON_PLUS },   { VPAD_BUTTON_MINUS, WIIDRC_BUTTON_MINUS },
    };
    u32 b = 0;
    for (const auto &m : map)
        if (st.hold & m.vpad)
            b |= m.drc;
    drc_buttons = b;
    drc_home = (st.hold & VPAD_BUTTON_HOME) != 0;
    {
        // the touch screen, in GamePad pixels (854x480), for the touch gun (drkMapleLightGun.cpp)
        VPADTouchData tp;
        VPADGetTPCalibratedPointEx(VPAD_CHAN_0, VPAD_TP_854X480, &tp, &st.tpNormal);
        drc_touch = st.tpNormal.touched && tp.validity == VPAD_VALID;
        drc_tx = tp.x;
        drc_ty = tp.y;
    }
    // libwiidrc's stick reads about +-73; the plugin scales that by 7/4 to the Dreamcast's +-128
    drc_lx = (s16)(st.leftStick.x * 73.0f);
    drc_ly = (s16)(st.leftStick.y * 73.0f);
    return true;
}
bool WiiDRC_ShutdownRequested() { return false; }
u32 WiiDRC_ButtonsHeld() { return drc_buttons; }
s16 WiiDRC_lStickX() { return drc_lx; }
s16 WiiDRC_lStickY() { return drc_ly; }

// the buttons the core sees, for the log when it asks to exit (main_wiiu.cpp)
// the HOME menu (home_menu_wiiu.cpp): HOME held on the GamePad, a Wii Remote, or a Classic or Pro Controller (its
// HOME in the extension bits), as the emulator thread last read them
extern "C" bool wiiu_home_held()
{
    if (drc_home)
        return true;
    for (int c = 0; c < 4; c++)
        if (wpad_held[c] & (0x0080u | (0x0800u << 16)))   // HOME: 0x0080 in the libogc core bits (kpad_wiiu.cpp), Classic and Pro HOME (0x0800) in the extension
            return true;
    return false;
}
// fast-forward (home_menu_wiiu.cpp): − held alone on the GamePad, a Classic or a Pro Controller (with + it's quit; a
// Wii Remote's own − is Start for the light gun, so it doesn't count)
extern "C" bool wiiu_ffwd_held()
{
    if ((drc_buttons & WIIDRC_BUTTON_MINUS) && !(drc_buttons & WIIDRC_BUTTON_PLUS))
        return true;
    for (int c = 0; c < 4; c++)
        if ((wpad_held[c] & (0x1000u << 16)) && !(wpad_held[c] & (0x0400u << 16)))   // Classic/Pro − without +
            return true;
    return false;
}
// + and − held together on the GamePad, a Classic or a Pro Controller: the quit combo (fast-forward stays off)
extern "C" bool wiiu_quit_combo_held()
{
    const u32 both = WIIDRC_BUTTON_MINUS | WIIDRC_BUTTON_PLUS;
    if ((drc_buttons & both) == both)
        return true;
    for (int c = 0; c < 4; c++)
        if ((wpad_held[c] & (0x1400u << 16)) == (0x1400u << 16))   // Classic/Pro − and +
            return true;
    return false;
}
// a tap in the GamePad's top right corner (80x80 of its 854x480): opens the HOME menu, outside gun games
extern "C" bool wiiu_corner_tapped()
{
    return drc_touch && drc_tx >= 854 - 80 && drc_ty < 80;
}
extern "C" void wiiu_input_state(u32 *drc, u32 *wpad4)
{
    *drc = drc_buttons;
    for (int c = 0; c < 4; c++)
        wpad4[c] = wpad_held[c];
}

// libsicksaxis needs the Wii's USB stack (IOS58); no DualShock 3 here
int ss_open(struct ss_device *dev) { return -1; }
int ss_is_connected(struct ss_device *dev) { return 0; }
int ss_start_reading(struct ss_device *dev) { return -1; }

extern "C" void wii_boot_log(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}
