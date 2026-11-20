// Button remapping (features plan, phase 7). No Aroma plugin remaps a GamePad or a Pro Controller for an app
// (Bloopair remaps only the controllers it connects itself), so nullDC does it in its own input path:
//
// - The GamePad and the Classic/Pro Controller both reach the Dreamcast mapping (plugs/drkMapleDevices/
//   drkMapleDevices.cpp MapButtons) as Classic Controller bits. Each controller kind has a table: for each physical
//   button, the Classic bit it acts as, i.e. its Dreamcast function (B is Dreamcast A, A is B, Y is X, X is Y, + is
//   Start, L/ZL the left trigger, R/ZR the right, the D-pad). The default table is the identity: nothing changes.
// - stick_dpad_swap=1: the left stick works the D-pad and the D-pad the stick.
// - turbo=a,b,...: those Dreamcast buttons repeat while held (3 frames on, 3 off).
// - remote_pad=0: a Wii Remote's own buttons and a Nunchuk are no pad (the frontend rule: a Remote is a gun or a
//   pointer). Default 1 keeps the old behaviour until a console run confirms the rule's change.
//
// Settings: remap_gamepad= and remap_classic= list the changed buttons ("a=b,zl=none": GamePad A acts as Dreamcast
// B, ZL as nothing), in config.txt (all games) or gamecfg/<game>.txt (this game, over the other). The HOME menu's
// "Remap buttons" page edits them (home_menu_wiiu.cpp).
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "compat/wiiuse/wpad.h"
#define RIGHT_BIT ((uint32_t)WPAD_CLASSIC_BUTTON_RIGHT)   // 0x8000 << 16: past an int

std::string wiiu_settings_get(const std::string &path, const char *key);   // picker_wiiu.cpp
std::string wiiu_game_settings_path(const char *disc);
std::string wiiu_config_path();
extern char selectedFilePath[512];

int wiiu_remote_pad = 1, wiiu_stick_dpad_swap;

// the buttons a table covers, in Classic bits; their names in settings and menus
const int WIIU_REMAP_N = 13;
static const uint32_t phys_bit[WIIU_REMAP_N] = {
    WPAD_CLASSIC_BUTTON_A, WPAD_CLASSIC_BUTTON_B, WPAD_CLASSIC_BUTTON_X, WPAD_CLASSIC_BUTTON_Y,
    WPAD_CLASSIC_BUTTON_PLUS, WPAD_CLASSIC_BUTTON_FULL_L, WPAD_CLASSIC_BUTTON_ZL, WPAD_CLASSIC_BUTTON_FULL_R,
    WPAD_CLASSIC_BUTTON_ZR, WPAD_CLASSIC_BUTTON_UP, WPAD_CLASSIC_BUTTON_DOWN, WPAD_CLASSIC_BUTTON_LEFT, RIGHT_BIT };
static const char *const phys_key[WIIU_REMAP_N] = { "a", "b", "x", "y", "plus", "l", "zl", "r", "zr", "up", "down",
                                                    "left", "right" };
static const char *const phys_label[WIIU_REMAP_N] = { "A", "B", "X", "Y", "+", "L", "ZL", "R", "ZR", "Up", "Down",
                                                      "Left", "Right" };
// the Dreamcast functions, as the Classic bit that gives each one; index WIIU_REMAP_FN-1 is "nothing"
const int WIIU_REMAP_FN = 12;
static const uint32_t fn_bit[WIIU_REMAP_FN] = {
    WPAD_CLASSIC_BUTTON_B, WPAD_CLASSIC_BUTTON_A, WPAD_CLASSIC_BUTTON_Y, WPAD_CLASSIC_BUTTON_X,
    WPAD_CLASSIC_BUTTON_PLUS, WPAD_CLASSIC_BUTTON_FULL_L, WPAD_CLASSIC_BUTTON_FULL_R, WPAD_CLASSIC_BUTTON_UP,
    WPAD_CLASSIC_BUTTON_DOWN, WPAD_CLASSIC_BUTTON_LEFT, RIGHT_BIT, 0 };
static const char *const fn_key[WIIU_REMAP_FN] = { "a", "b", "x", "y", "start", "lt", "rt", "up", "down", "left",
                                                   "right", "none" };
static const char *const fn_label[WIIU_REMAP_FN] = { "Dreamcast A", "Dreamcast B", "Dreamcast X", "Dreamcast Y",
                                                     "Start", "Left trigger", "Right trigger", "D-pad up",
                                                     "D-pad down", "D-pad left", "D-pad right", "nothing" };

static int table[2][WIIU_REMAP_N];   // [0 GamePad, 1 Classic/Pro][physical] = function
static uint32_t turbo_mask;          // Classic bits of the turbo functions
static bool remapped[2];             // a table isn't the default

static int default_fn(int p)
{
    for (int f = 0; f < WIIU_REMAP_FN - 1; f++)
        if (fn_bit[f] == phys_bit[p])
            return f;
    // ZL and ZR: the triggers, as L and R
    return phys_bit[p] == WPAD_CLASSIC_BUTTON_ZL ? 5 : 6;
}

int wiiu_remap_count() { return WIIU_REMAP_N; }
int wiiu_remap_fn_count() { return WIIU_REMAP_FN; }
const char *wiiu_remap_phys_label(int p) { return phys_label[p]; }
const char *wiiu_remap_fn_label(int f) { return fn_label[f]; }
int wiiu_remap_get(int ctl, int p) { return table[ctl][p]; }

static void refresh(int ctl)
{
    remapped[ctl] = false;
    for (int p = 0; p < WIIU_REMAP_N; p++)
        remapped[ctl] = remapped[ctl] || table[ctl][p] != default_fn(p);
}
void wiiu_remap_set(int ctl, int p, int f)
{
    table[ctl][p] = f;
    refresh(ctl);
}
void wiiu_remap_reset(int ctl)
{
    for (int p = 0; p < WIIU_REMAP_N; p++)
        table[ctl][p] = default_fn(p);
    remapped[ctl] = false;
}

// the table as its setting: only the changed buttons
std::string wiiu_remap_string(int ctl)
{
    std::string s;
    for (int p = 0; p < WIIU_REMAP_N; p++)
        if (table[ctl][p] != default_fn(p))
            s += (s.empty() ? "" : ",") + std::string(phys_key[p]) + "=" + fn_key[table[ctl][p]];
    return s;
}
static void parse(int ctl, const std::string &v)
{
    wiiu_remap_reset(ctl);
    size_t i = 0;
    while (i < v.size())
    {
        size_t e = v.find(',', i);
        if (e == std::string::npos)
            e = v.size();
        const std::string item = v.substr(i, e - i);
        const size_t eq = item.find('=');
        if (eq != std::string::npos)
        {
            const std::string k = item.substr(0, eq), f = item.substr(eq + 1);
            for (int p = 0; p < WIIU_REMAP_N; p++)
                if (k == phys_key[p])
                    for (int n = 0; n < WIIU_REMAP_FN; n++)
                        if (f == fn_key[n])
                            table[ctl][p] = n;
        }
        i = e + 1;
    }
    refresh(ctl);
}

// the turbo setting's Dreamcast buttons as Classic bits
static uint32_t parse_turbo(const std::string &v)
{
    uint32_t m = 0;
    for (int f = 0; f < WIIU_REMAP_FN - 1; f++)
    {
        const std::string k = fn_key[f];
        size_t at = 0;
        while ((at = v.find(k, at)) != std::string::npos)
        {
            const bool start = at == 0 || v[at - 1] == ',', end = at + k.size() == v.size() || v[at + k.size()] == ',';
            if (start && end)
                m |= fn_bit[f];
            at += k.size();
        }
    }
    return m;
}
uint32_t wiiu_turbo_mask() { return turbo_mask; }
void wiiu_turbo_set(uint32_t m) { turbo_mask = m; }
std::string wiiu_turbo_string(uint32_t m)
{
    std::string s;
    for (int f = 0; f < WIIU_REMAP_FN - 1; f++)
        if (m & fn_bit[f])
            s += (s.empty() ? "" : ",") + std::string(fn_key[f]);
    return s;
}
uint32_t wiiu_turbo_fn_bit(int f) { return fn_bit[f]; }

// at a game's start (main_wiiu.cpp): the settings, this game's over all games'
void wiiu_remap_load()
{
    const std::string all = wiiu_config_path(), game = wiiu_game_settings_path(selectedFilePath);
    auto get = [&](const char *key) {
        const std::string g = wiiu_settings_get(game, key);
        return g.empty() ? wiiu_settings_get(all, key) : g;
    };
    parse(0, get("remap_gamepad"));
    parse(1, get("remap_classic"));
    turbo_mask = parse_turbo(get("turbo"));
    const std::string swap = get("stick_dpad_swap");
    if (!swap.empty())
        wiiu_stick_dpad_swap = atoi(swap.c_str());
    const std::string rp = get("remote_pad");
    if (!rp.empty())
        wiiu_remote_pad = atoi(rp.c_str());
    if (remapped[0] || remapped[1] || turbo_mask || wiiu_stick_dpad_swap || !wiiu_remote_pad)
        printf("[remap] GamePad \"%s\", Classic/Pro \"%s\", turbo \"%s\", stick/D-pad swap %d, Remote as a pad %d\n",
               wiiu_remap_string(0).c_str(), wiiu_remap_string(1).c_str(), wiiu_turbo_string(turbo_mask).c_str(),
               wiiu_stick_dpad_swap, wiiu_remote_pad);
}

// a controller's Classic bits through its table (ctl 0 the GamePad, 1 a Classic or Pro Controller)
uint32_t wiiu_remap(int ctl, uint32_t bits)
{
    if (!remapped[ctl] || !bits)
        return bits;
    uint32_t out = bits & ~(WPAD_CLASSIC_BUTTON_A | WPAD_CLASSIC_BUTTON_B | WPAD_CLASSIC_BUTTON_X | WPAD_CLASSIC_BUTTON_Y |
                            WPAD_CLASSIC_BUTTON_PLUS | WPAD_CLASSIC_BUTTON_FULL_L | WPAD_CLASSIC_BUTTON_ZL |
                            WPAD_CLASSIC_BUTTON_FULL_R | WPAD_CLASSIC_BUTTON_ZR | WPAD_CLASSIC_BUTTON_UP |
                            WPAD_CLASSIC_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_RIGHT);
    for (int p = 0; p < WIIU_REMAP_N; p++)
        if (bits & phys_bit[p])
            out |= fn_bit[table[ctl][p]];
    return out;
}

// after the controllers are read, before the Dreamcast mapping: the Remote rule, the stick/D-pad swap, turbo
void wiiu_input_post(uint32_t port, uint32_t *wii, uint32_t *nunchuk, uint32_t *classic, int32_t *sx, int32_t *sy,
                     bool nunchuk_attached)
{
    if (!wiiu_remote_pad)
    {
        *wii &= 0xFFFF0000u;   // the Remote's own buttons (the Classic's are the high half)
        if (nunchuk_attached)
        {
            *wii = 0;          // with a Nunchuk the high half is the Nunchuk's
            *nunchuk = 0;
            *sx = *sy = 0;
        }
    }
    if (wiiu_stick_dpad_swap)
    {
        const uint32_t dpad = WPAD_CLASSIC_BUTTON_UP | WPAD_CLASSIC_BUTTON_DOWN | WPAD_CLASSIC_BUTTON_LEFT | WPAD_CLASSIC_BUTTON_RIGHT;
        const uint32_t d = *classic & dpad;
        const int32_t x = *sx, y = *sy;
        *classic &= ~dpad;
        if (x > 64) *classic |= WPAD_CLASSIC_BUTTON_RIGHT;
        if (x < -64) *classic |= WPAD_CLASSIC_BUTTON_LEFT;
        if (y > 64) *classic |= WPAD_CLASSIC_BUTTON_UP;
        if (y < -64) *classic |= WPAD_CLASSIC_BUTTON_DOWN;
        *sx = (d & WPAD_CLASSIC_BUTTON_RIGHT ? 127 : 0) - (d & WPAD_CLASSIC_BUTTON_LEFT ? 127 : 0);
        *sy = (d & WPAD_CLASSIC_BUTTON_UP ? 127 : 0) - (d & WPAD_CLASSIC_BUTTON_DOWN ? 127 : 0);
    }
    if (turbo_mask && (*classic & turbo_mask))
    {
        static uint32_t polls[4];
        if (port < 4 && (polls[port]++ / 3) & 1)
            *classic &= ~turbo_mask;   // the off half of the repeat
    }
}
