// drkMapleLightGun.cpp
// Light gun device emulation for Dreamcast on Wii
// Uses Wiimote IR pointer for aiming, Wiimote buttons for shooting.
//
// Supported games (Dreamcast Maple light gun / "Stunner"):
//   - House of the Dead 2
//   - Confidential Mission
//   - Death Crimson OX
//   - Virtua Cop 2
//   - Starship Troopers
//
// Wii controls:
//   IR pointer      -> Gun aim (screen X/Y coordinates)
//   B (trigger)     -> Shoot
//   A               -> Reload / offscreen shot trick
//   MINUS           -> Start
//   PLUS            -> Coin / insert
//   UP/DOWN/LEFT/RIGHT -> D-Pad (menus)
//   MINUS + PLUS    -> Exit emulator
//
// 2-player: Player 1 = Wiimote 0, Player 2 = Wiimote 1
// Both Wiimotes must point at the screen independently.
//
// Notes on Dreamcast light gun protocol:
//   The Dreamcast Maple light gun reports a screen X/Y coordinate
//   (0-639 horizontal, 0-479 vertical for NTSC) and a button mask.
//   The Wii IR camera gives dot positions in 0-1023 (X) and 0-767 (Y).
//   We scale those to Dreamcast coordinates here.
//   When the gun is pointed off-screen, we report (0xFFFF, 0xFFFF) which
//   is the conventional "missed" signal used by all DC gun games.
//
// Player count is read from get_player_count() defined in main.cpp.

#include "plugins/plugin_header.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <wiiuse/wpad.h>
#include "dc/dc.h"
#include "wii/wii_exit.h"  // ordered shutdown for the exit combo (EXIT FIX preset)
#include "plugs/libwiidrc/wiidrc.h"  // the GamePad (WiiDRC) drives the gun too

// Dreamcast controller button definitions (same as drkMapleDevices.cpp)
#define key_CONT_C          (1 << 0)
#define key_CONT_B          (1 << 1)
#define key_CONT_A          (1 << 2)
#define key_CONT_START      (1 << 3)
#define key_CONT_DPAD_UP    (1 << 4)
#define key_CONT_DPAD_DOWN  (1 << 5)
#define key_CONT_DPAD_LEFT  (1 << 6)
#define key_CONT_DPAD_RIGHT (1 << 7)
#define key_CONT_Z          (1 << 8)
#define key_CONT_Y          (1 << 9)
#define key_CONT_X          (1 << 10)
#define key_CONT_D          (1 << 11)

// Dreamcast screen resolution (NTSC)
#define DC_SCREEN_W  640
#define DC_SCREEN_H  480

// Wii IR camera resolution
#define WII_IR_W  1024
#define WII_IR_H   768

// Sentinel value for off-screen / IR not visible
#define DC_OFFSCREEN 0xFFFF

// Maximum players
#define MAX_LG_PLAYERS 2

// Controller state arrays — shared with maple_cfg / maple_devs via plugin_header externs
extern u16 kcode[4];
extern u32 vks[4];
extern s8  joyx[4];
extern s8  joyy[4];
extern u8  rt[4];
extern u8  lt[4];

// Gun position reported to the Dreamcast (per port)
// These need to be read by maple_devs.cpp / maple_cfg.cpp when the
// Maple device type is set to MAPLE_DEVICE_LIGHTGUN.
u16 gun_x[MAX_LG_PLAYERS] = {DC_OFFSCREEN, DC_OFFSCREEN};
u16 gun_y[MAX_LG_PLAYERS] = {DC_OFFSCREEN, DC_OFFSCREEN};

// Expose gun position to rest of emulator
extern "C"
{
    u16 GetLightGunX(int port) { return (port < MAX_LG_PLAYERS) ? gun_x[port] : DC_OFFSCREEN; }
    u16 GetLightGunY(int port) { return (port < MAX_LG_PLAYERS) ? gun_y[port] : DC_OFFSCREEN; }
}

extern "C" int get_player_count(void);
#ifdef __WIIU__
// gun_v2=1 (off until confirmed on a console; features plan phase 4, the frontend's light gun rules): the GamePad's
// touch screen is a gun too (a tap aims and fires, a tap on the black bars beside the picture reloads), gun 1 when no
// Wii Remote points at the screen, else gun 2; both sensor bars are powered; a crosshair for each Remote gun (not for
// touch, where the finger is the aim). gun_cross=0 hides the crosshairs
extern "C" bool WiiDRC_Touch(u16 *x, u16 *y);   // wiiu/ogc_compat.cpp
extern "C" { int wiiu_gun_v2, wiiu_gun_cross = 1; }
static int gun_src[MAX_LG_PLAYERS];               // what aimed each gun this frame: 0 nothing, 1 a Remote, 2 touch
static u32 remote_seen[MAX_LG_PLAYERS];           // frames since a Remote last pointed at the screen, per port
static int touch_fire;                            // polls the touch shot stays down (a tap is shorter than a poll)
static u16 touch_x = DC_OFFSCREEN, touch_y = DC_OFFSCREEN;
extern "C" int wiiu_gun_source(int port) { return port >= 0 && port < MAX_LG_PLAYERS ? gun_src[port] : 0; }
extern "C" void wiiu_sensor_bars_on();   // wiiu/ogc_compat.cpp: TV and GamePad bars
// the touch gun: the port it plays, and its aim and trigger this poll; false when it isn't this port's
static bool touch_gun(u32 port, bool *fire)
{
    const u32 touch_port = remote_seen[0] < 120 ? 1 : 0;   // a Remote aimed lately: it's gun 1, touch is gun 2
    if (port != touch_port || port >= (u32)get_player_count())
        return false;
    u16 x, y;
    if (WiiDRC_Touch(&x, &y))
    {
        // the 4:3 picture fills the GamePad's 854x480 between x 107 and 747; the bars beside it are the edge of the
        // screen: a shot there is off-screen, the reload
        if (x >= 107 && x < 747)
        {
            touch_x = (u16)((x - 107) * DC_SCREEN_W / 640);
            touch_y = (u16)(y * DC_SCREEN_H / 480);
        }
        else
            touch_x = touch_y = DC_OFFSCREEN;
        if (touch_fire <= 0)
        {
            static int logged;
            if (logged++ < 20)
                printf("[LightGun] touch shot at %u,%u: gun %u at %u,%u\n", x, y, port + 1, touch_x, touch_y);
        }
        touch_fire = 3;
    }
    if (touch_fire <= 0)
        return false;
    touch_fire--;
    gun_x[port] = touch_x;
    gun_y[port] = touch_y;
    *fire = true;
    return true;
}
#endif

/**
 * Scales a Wii IR coordinate to a Dreamcast screen coordinate.
 * Returns DC_OFFSCREEN if the IR dot is not visible.
 *
 * @param wiiVal   Raw IR value (0 to wiiMax-1), or negative if not visible
 * @param wiiMax   Range of Wii IR axis (1024 for X, 768 for Y)
 * @param dcMax    Range of DC screen axis (640 for X, 480 for Y)
 */
static u16 ScaleIRCoord(s32 wiiVal, s32 wiiMax, s32 dcMax)
{
    if (wiiVal < 0 || wiiVal >= wiiMax)
        return DC_OFFSCREEN;

    // Simple linear scale
    s32 scaled = (wiiVal * dcMax) / wiiMax;

    // Clamp just in case of rounding edge
    if (scaled < 0)      scaled = 0;
    if (scaled >= dcMax) scaled = dcMax - 1;

    return (u16)scaled;
}

// IR data format set lazily on first update call (WPAD must be fully ready)
static bool s_ir_format_set[MAX_LG_PLAYERS] = {false, false};

/**
 * Updates light gun input state for one player port.
 * Call once per frame per active port.
 *
 * @param port  Controller port (0 = player 1, 1 = player 2)
 */
void UpdateLightGunState(u32 port)
{
    if (port >= (u32)get_player_count() || port >= MAX_LG_PLAYERS)
        return;

    // Enable IR tracking lazily — WPAD must be initialised and running
    // before SetDataFormat is called, which is guaranteed by the time the
    // emulator starts polling for input.
    if (!s_ir_format_set[port])
    {
        WPAD_SetDataFormat(port, WPAD_FMT_BTNS_ACC_IR);
        // ir.valid / ir.x / ir.y only work after WPAD_SetVRes sets the virtual
        // screen resolution; without it vres defaults to 0 and ir.valid is
        // always 0.  Set it to match WII_IR_W x WII_IR_H so the scaling in
        // ScaleIRCoord is consistent with the vres range.
        WPAD_SetVRes(port, WII_IR_W, WII_IR_H);
        s_ir_format_set[port] = true;
        printf("[LightGun] IR data format + vres set for port %d\n", port);
    }

    WPAD_ScanPads();
#ifdef __WIIU__
    if (wiiu_gun_v2)
        wiiu_sensor_bars_on();
#endif

    // --- IR aiming ---
    WPADData *data = WPAD_Data(port);

    if (data && data->ir.valid)
    {
#ifdef __WIIU__
        remote_seen[port] = 0;
        gun_src[port] = 1;
#endif
        // ir.x is 0..(WII_IR_W-1), ir.y is 0..(WII_IR_H-1) after SetVRes.
        // The Wii IR camera origin is top-left; DC expects same origin.
        gun_x[port] = ScaleIRCoord((s32)data->ir.x, WII_IR_W, DC_SCREEN_W);
        gun_y[port] = ScaleIRCoord((s32)data->ir.y, WII_IR_H, DC_SCREEN_H);
    }
    else
    {
        // IR not visible — report off-screen shot
        gun_x[port] = DC_OFFSCREEN;
        gun_y[port] = DC_OFFSCREEN;
#ifdef __WIIU__
        if (remote_seen[port] < 0xFFFF)
            remote_seen[port]++;
        gun_src[port] = 0;
#endif
    }

    // --- Buttons ---
    u32 wiiButtons = WPAD_ButtonsHeld(port);

    // Reset to all-released
    kcode[port] = 0xFFFF;

    // Shoot — B trigger (most natural for a gun)
    if (wiiButtons & WPAD_BUTTON_B)
        kcode[port] &= ~key_CONT_A;
#ifdef __WIIU__
    {
        bool fire = false;
        if (wiiu_gun_v2 && touch_gun(port, &fire))
        {
            gun_src[port] = 2;
            if (fire)
                kcode[port] &= ~key_CONT_A;
        }
    }
#endif

    // Off-screen reload trick — A button fires with gun_x/y set to off-screen
    // Some games (HotD2) use an off-screen shot to reload.
    if (wiiButtons & WPAD_BUTTON_A)
    {
        kcode[port] &= ~key_CONT_A;
        // Force off-screen so the game treats it as a reload
        gun_x[port] = DC_OFFSCREEN;
        gun_y[port] = DC_OFFSCREEN;
    }

#ifdef __WIIU__
    // the GamePad works alongside the gun on player 1: Start (+) and the D-pad for menus, A/ZR as the trigger
    if (port == 0 && WiiDRC_Inited())
    {
        WiiDRC_ScanPads();
        const u32 drc = WiiDRC_ButtonsHeld();
        if (drc & WIIDRC_BUTTON_PLUS)                     kcode[0] &= ~key_CONT_START;
        if (drc & (WIIDRC_BUTTON_A | WIIDRC_BUTTON_ZR))   kcode[0] &= ~key_CONT_A;
        if (drc & WIIDRC_BUTTON_UP)                       kcode[0] &= ~key_CONT_DPAD_UP;
        if (drc & WIIDRC_BUTTON_DOWN)                     kcode[0] &= ~key_CONT_DPAD_DOWN;
        if (drc & WIIDRC_BUTTON_LEFT)                     kcode[0] &= ~key_CONT_DPAD_LEFT;
        if (drc & WIIDRC_BUTTON_RIGHT)                    kcode[0] &= ~key_CONT_DPAD_RIGHT;
    }
#endif

    // Start / coin
    if (wiiButtons & WPAD_BUTTON_MINUS)
        kcode[port] &= ~key_CONT_START;

    // key_CONT_C (bit 0) is always forced to "released" by the 0xF901 mask in
    // MapleConfigMap::GetInput(), so we map PLUS to START instead so it
    // actually registers.  Both MINUS and PLUS now act as Start / coin.
    if (wiiButtons & WPAD_BUTTON_PLUS)
        kcode[port] &= ~key_CONT_START;

    // D-Pad (menu navigation in attract mode / config screens)
    if (wiiButtons & WPAD_BUTTON_UP)    kcode[port] &= ~key_CONT_DPAD_UP;
    if (wiiButtons & WPAD_BUTTON_DOWN)  kcode[port] &= ~key_CONT_DPAD_DOWN;
    if (wiiButtons & WPAD_BUTTON_LEFT)  kcode[port] &= ~key_CONT_DPAD_LEFT;
    if (wiiButtons & WPAD_BUTTON_RIGHT) kcode[port] &= ~key_CONT_DPAD_RIGHT;

    // Exit combination: MINUS + PLUS simultaneously
    if ((wiiButtons & WPAD_BUTTON_MINUS) && (wiiButtons & WPAD_BUTTON_PLUS))
        WiiExitToLoader();
}

/**
 * Initializes light gun ports to safe default state.
 * WPAD_SetDataFormat is called lazily on the first update, not here,
 * because WPAD may not be fully ready during mcfg_CreateDevices().
 */
void InitLightGun(void)
{
    for (int i = 0; i < MAX_LG_PLAYERS; i++)
    {
        kcode[i]           = 0xFFFF;
        gun_x[i]           = DC_OFFSCREEN;
        gun_y[i]           = DC_OFFSCREEN;
        joyx[i]            = 0;
        joyy[i]            = 0;
        rt[i]              = 0;
        lt[i]              = 0;
        s_ir_format_set[i] = false;
    }
    printf("[LightGun] Initialized (IR format will be set on first poll)\n");
}
