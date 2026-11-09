// Game picker, shown before the emulator starts when wiiu.cfg names no file. The logic (config.txt, the scan of
// the home folder two levels deep, the menu, the help text) is picker/picker_core.c, structured like FBNeo's
// player/picker but kept separately; this file is only the UI for it: Roboto drawn into a 960x720 image shown
// through GX2 like the emulator's framebuffer (OSScreen before GX2 left the GamePad black on a console), and the
// GamePad, Wii Remote, Classic and Pro Controller read through VPAD and KPAD.
// Up/down to choose, A to start, Y (or 1) for help, hold + and - (or HOME) to quit.
#include <coreinit/time.h>
#include <vpad/input.h>
#include <whb/proc.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <vector>
#include "types.h"
#include "kpad_wiiu.h"
#include "font_roboto.h"
#include "picker/picker_core.h"

void wiiu_gx2_show_ui(const u32 *px, u32 w, u32 h);
void wiiu_gx2_hide_ui();
void wiiu_gx2_frame();
bool wiiu_app_running();   // main_wiiu.cpp

namespace
{
// the core's buttons, as a set over all controllers
u32 read_buttons()
{
    u32 b = 0;
    VPADStatus vs;
    VPADReadError ve;
    if (VPADRead(VPAD_CHAN_0, &vs, 1, &ve) == 1 && ve == VPAD_READ_SUCCESS)
    {
        if (vs.hold & (VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP))
            b |= PICKER_BTN_UP;
        if (vs.hold & (VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN))
            b |= PICKER_BTN_DOWN;
        if (vs.hold & VPAD_BUTTON_A)
            b |= PICKER_BTN_ACCEPT;
        if (vs.hold & VPAD_BUTTON_Y)
            b |= PICKER_BTN_HELP;
        if (vs.hold & VPAD_BUTTON_X)
            b |= PICKER_BTN_OPTIONS;
        if ((vs.hold & (VPAD_BUTTON_PLUS | VPAD_BUTTON_MINUS)) == (VPAD_BUTTON_PLUS | VPAD_BUTTON_MINUS))
            b |= PICKER_BTN_QUIT_COMBO;
    }
    for (int c = 0; c < 4; c++)
    {
        static WiiuPad pad[4];
        wiiu_kpad_read(c, &pad[c]);
        const WiiuPad &p = pad[c];
        if (!p.connected)
            continue;
        // libogc bits: Wii Remote up 0x0800, down 0x0400, A 0x0008, 1 0x0002, plus 0x1000, minus 0x0010;
        // Classic/Pro up 0x0001, down 0x4000, A 0x0010, Y 0x0020, X 0x0008, plus 0x0400, minus 0x1000; Wii Remote 2 0x0001
        const bool classic = p.ext == 2;
        if ((p.core & 0x0800) || (classic && (p.exp & 0x0001)) || p.ly > 0.6f)
            b |= PICKER_BTN_UP;
        if ((p.core & 0x0400) || (classic && (p.exp & 0x4000)) || p.ly < -0.6f)
            b |= PICKER_BTN_DOWN;
        if ((p.core & 0x0008) || (classic && (p.exp & 0x0010)))
            b |= PICKER_BTN_ACCEPT;
        if ((p.core & 0x0002) || (classic && (p.exp & 0x0020)))
            b |= PICKER_BTN_HELP;
        if ((p.core & 0x0001) || (classic && (p.exp & 0x0008)))
            b |= PICKER_BTN_OPTIONS;
        if (((p.core & 0x1010) == 0x1010) || (classic && (p.exp & 0x1400) == 0x1400))
            b |= PICKER_BTN_QUIT_COMBO;
    }
    return b;
}

const u32 W = 960, H = 720, MARGIN = 28;   // shown 4:3; Roboto from font_roboto.h (make_font.py)
const u32 BG = 0x102040FF, FG = 0xE0E0E0FF, HILITE = 0xFFE060FF, DIM = 0x9090A0FF;

u32 text_width(const char *s)
{
    u32 w = 0;
    for (; *s; s++)
        w += *s >= 32 && *s < 127 ? font_glyphs[*s - 32].advance : 0;
    return w;
}

// one line at pixel x, on text row `row`; cut off at the right margin
void text(u32 *px, u32 x, u32 row, const char *s, u32 color)
{
    const u32 top = row * FONT_LINE, cr = color >> 24, cg = (color >> 16) & 255, cb = (color >> 8) & 255;
    for (; *s; s++)
    {
        if (*s < 32 || *s >= 127)
            continue;
        const FontGlyph &g = font_glyphs[*s - 32];
        if (x + g.advance > W - MARGIN)
            break;
        const unsigned char *a = &font_alpha[g.offset];
        for (int y = 0; y < g.h; y++)
            for (int i = 0; i < g.w; i++)
            {
                const u32 c = *a++;
                const int dx = (int)x + g.x + i, dy = (int)top + g.y + y;
                if (!c || dx < 0 || dy < 0 || dx >= (int)W || dy >= (int)H)
                    continue;
                u32 &d = px[dy * W + dx];
                const u32 dr = d >> 24, dg = (d >> 16) & 255, db = (d >> 8) & 255;
                d = (dr + (cr - dr) * c / 255) << 24 | (dg + (cg - dg) * c / 255) << 16 | (db + (cb - db) * c / 255) << 8 | 255;
            }
        x += g.advance;
    }
}

// a long line over as many rows as it takes (paths), returns the rows used
u32 text_wrapped(u32 *px, u32 x, u32 row, const char *s, u32 color)
{
    const u32 width = W - MARGIN - x;
    u32 rows = 0;
    std::string rest = s;
    do
    {
        // the most that fits, back to the last space; mid-word when a word (a path) is longer than the row
        size_t n = 0, space = 0;
        u32 w = 0;
        while (n < rest.size())
        {
            const u32 adv = rest[n] >= 32 && rest[n] < 127 ? font_glyphs[rest[n] - 32].advance : 0;
            if (w + adv > width)
                break;
            w += adv;
            if (rest[n] == ' ')
                space = n;
            n++;
        }
        if (n < rest.size() && space)
            n = space;
        if (!n)
            n = 1;
        text(px, x, row + rows++, rest.substr(0, n).c_str(), color);
        rest.erase(0, n);
        while (!rest.empty() && rest[0] == ' ')
            rest.erase(0, 1);
    } while (!rest.empty());
    return rows;
}

int options_count();
const char *option_label(int i);
const char *option_help(int i);

void draw(u32 *px)
{
    const picker_view &v = *picker_core_view();
    for (u32 i = 0; i < W * H; i++)
        px[i] = BG;
    text(px, MARGIN, 1, "nullDC for Wii U", HILITE);
    if (v.options)
    {
        text(px, MARGIN, 3, "Options", HILITE);
        const int n = options_count();
        for (int i = 0; i < n; i++)
        {
            char line[64];
            snprintf(line, sizeof(line), "%s%s: %s", i == v.opt_sel ? "> " : "   ", option_label(i),
                     picker_core_option_value(i));
            text(px, MARGIN, 5 + i, line, i == v.opt_sel ? HILITE : FG);
        }
        u32 row = 6 + n;
        if (option_help(v.opt_sel))
            row += text_wrapped(px, MARGIN, row, option_help(v.opt_sel), FG);
        text(px, MARGIN, row + 1, "A change   X back   (all games)", DIM);
    }
    else if (v.help)
    {
        const char *lines[16];
        const int n = picker_core_help_lines(lines, 16);
        u32 row = 3;
        for (int i = 0; i < n; i++)
            row += text_wrapped(px, MARGIN, row, lines[i], i == 0 ? HILITE : FG) + (i == 0);
        char foot[64];
        const int pages = picker_core_help_pages();
        snprintf(foot, sizeof(foot), "%s%s", v.help < pages ? "Y: next page  " : "Y: back  ",
                 v.count ? "A: list" : "A: quit");
        text(px, MARGIN, row + 1, foot, DIM);
    }
    else
    {
        text(px, MARGIN, 2, "A start   Y help   X options   hold + and - to quit", DIM);
        for (int i = 0; i < v.visible && v.top + i < v.count; i++)
        {
            const int idx = v.top + i;
            const char *label = picker_core_label(idx);
            char line[128];
            snprintf(line, sizeof(line), "%s%.90s%s%s", idx == v.sel ? "> " : "   ", picker_core_entry(idx)->name,
                     *label ? " " : "", label);
            text(px, MARGIN, 4 + i, line, idx == v.sel ? HILITE : FG);
        }
        char count[24];
        snprintf(count, sizeof(count), "%d / %d", v.sel + 1, v.count);
        text(px, MARGIN, 4 + v.visible + 1, count, DIM);
    }
    wiiu_gx2_show_ui(px, W, H);
}
// Options page (picker_core); main_wiiu.cpp reads them with wiiu_picker_options, picker shown or not
const char *const on_off[] = { "on", "off" }, *const off_on[] = { "off", "on" };
const char *const sizes[] = { "960x720", "640x480" }, *const slow_sound[] = { "slows down", "keeps pitch" };
const char *const sound_buf[] = { "100 ms", "46 ms" };
const picker_option options[] = {
    { "fastmem", "Fast memory",
      "Dreamcast memory mapped straight into the Wii U's: about 12% faster in 3D games. Turn it off if a game "
      "crashes.", on_off, 2, 0 },
    { "lightgun", "Light gun",
      "For light-gun games only: the Wii Remote's pointer aims, B shoots, A reloads, - is Start. It replaces "
      "every controller, so other games need it off.", off_on, 2, 0 },
    { "render", "3D resolution",
      "640x480 is the Dreamcast's own: half the GPU work, so heavy 3D games (Soulcalibur) show more frames, a "
      "little softer.", sizes, 2, 0 },
    { "slow_sound", "Sound when slow",
      "When a game runs below full speed, its sound either slows down with it (smooth, lower pitch) or keeps "
      "its pitch (short gaps).", slow_sound, 2, 1 },
    // the console-tested speed settings (main_wiiu.cpp's defaults); wiiu.cfg still overrides any of them
    { "cores", "Use all CPU cores",
      "Graphics and sound mixing on their own cores, away from the emulated CPU: the biggest speedup (Soulcalibur "
      "fights 97% to 140%). Next start.", on_off, 2, 0 },
    { "jit_fast", "Faster CPU code",
      "Speedups in the emulated CPU's translated code (memory addressing, vector maths, block lookup, store "
      "queue). Turn it off if a game behaves oddly. Next start.", on_off, 2, 0 },
    { "smooth", "Smooth frames",
      "Lets two finished frames wait to be shown, so fewer are dropped when frames arrive in bursts. Adds one "
      "frame of delay. Next start.", on_off, 2, 0 },
    { "code_check", "Reloaded code check",
      "Catches games that load new code over old (fixes Marvel vs. Capcom 2's crash in attract mode). Costs "
      "nothing in other games. Next start.", on_off, 2, 0 },
    { "sound_buf", "Sound buffer",
      "100 ms means fewer gaps when a game slows down; 46 ms means less delay between picture and sound. Next start.",
      sound_buf, 2, 0 },
};
int options_count() { return (int)(sizeof(options) / sizeof(options[0])); }
const char *option_label(int i) { return options[i].label; }
const char *option_help(int i) { return i >= 0 && i < options_count() ? options[i].help : nullptr; }
} // namespace

// home: the data folder (ends in '/'), its config.txt and, by default, the content folder. path: the chosen file,
// empty for the BIOS. False when the user quit.
static void setup(const char *home)
{
    static std::string data_dir;
    data_dir = home;
    while (data_dir.size() > 1 && data_dir.back() == '/')
        data_dir.pop_back();
    static const char *const exts[] = { "gdi", "cdi", "mds", "elf", nullptr };
    static const char *const skip[] = { "data", nullptr };
    static const picker_entry extras[] = { { "Dreamcast BIOS (no disc)", "" } };
    static picker_cfg cfg;
    cfg.app_name = "nullDC";
    cfg.content_noun = "disc";
    cfg.data_dir = data_dir.c_str();
    cfg.content_default = data_dir.c_str();   // discs sit in folders of their own under the data folder
    cfg.exts = exts;
    cfg.scan_depth = 2;
    cfg.skip_dirs = skip;
    cfg.extras = extras;
    cfg.n_extras = 1;
    // the Dreamcast BIOS is what other emulators call the core's system files: without it nothing boots
    static std::string bios_help, warning;
    bios_help = "Dreamcast BIOS: dc_boot.bin and dc_flash.bin in " + data_dir + "/data/";
    cfg.help_extra = bios_help.c_str();
    std::string boot = data_dir + "/data/dc_boot.bin";
    if (FILE *f = fopen(boot.c_str(), "rb"))
        fclose(f);
    else
        warning = "BIOS NOT FOUND: nothing will boot until dc_boot.bin and dc_flash.bin are in data/";
    cfg.warning = warning.empty() ? nullptr : warning.c_str();
    // the core's mapping (plugs/drkMapleDevices): by position, the bottom button is the Dreamcast's A
    static const char *const controls[] = {
        "Controls",
        "GamePad, Pro Controller, Classic:",
        "  B A Y X = Dreamcast A B X Y",
        "  + = Start   L/ZL R/ZR = triggers",
        "  D-pad, left stick = D-pad, stick",
        "Wii Remote alone:",
        "  A B 2 1 = A B X Y   - + = triggers",
        "  HOME = Start (also the HOME menu)",
        "Quit: hold + and - for a second,",
        "  or HOME, then Close",
        "Player 1: GamePad or 1st controller,",
        "  players 2-4: controllers 2-4",
        nullptr,
    };
    cfg.controls = controls;
    cfg.options = options;
    cfg.n_options = options_count();
    picker_core_init(&cfg);
}

// the options' values from config.txt (written by the picker's options page), whether the picker runs or not
void wiiu_picker_options(const char *home, bool *fastmem, bool *lightgun, int *render_width, bool *keep_pitch)
{
    setup(home);
    *fastmem = picker_core_option(0) == 0;    // on_off: index 0 is "on"
    *lightgun = picker_core_option(1) == 1;   // off_on: index 1 is "on"
    *render_width = picker_core_option(2) == 1 ? 640 : 960;
    *keep_pitch = picker_core_option(3) == 1;
}

// the speed settings, read at startup (before the threads exist), from the same config.txt
void wiiu_picker_speed_options(const char *home, bool *cores, bool *jit_fast, bool *smooth, bool *code_check,
                               int *sound_buffer_ms)
{
    setup(home);
    *cores = picker_core_option(4) == 0;
    *jit_fast = picker_core_option(5) == 0;
    *smooth = picker_core_option(6) == 0;
    *code_check = picker_core_option(7) == 0;
    *sound_buffer_ms = picker_core_option(8) == 0 ? 100 : 0;
}

bool wiiu_pick_game(const char *home, char *path, size_t size)
{
    setup(home);

    // config.txt autoload=: straight into that file (relative to the content folder unless it's a full path)
    if (*picker_core_autoload())
    {
        const char *a = picker_core_autoload();
        if (strchr(a, ':'))
            snprintf(path, size, "%s", a);
        else
            snprintf(path, size, "%s/%s", picker_core_content_dir(), a);
        printf("[picker] autoload '%s'\n", path);
        return true;
    }

    picker_core_scan();
    picker_core_set_visible((int)(H / FONT_LINE) - 7);   // under the header, leaving a margin at the bottom
    printf("[picker] %d choices under %s\n", picker_core_view()->count, picker_core_content_dir());

    std::vector<u32> px(W * H);
    bool chosen = false;
    picker_view shown = {};
    bool first = true;
    while (wiiu_app_running())
    {
        const int ev = picker_core_input(read_buttons(), (unsigned)OSTicksToMilliseconds(OSGetSystemTime()));
        if (ev == PICKER_QUIT)
            break;
        if (ev == PICKER_CHOSE)
        {
            chosen = true;
            break;
        }
        const picker_view &v = *picker_core_view();
        if (first || memcmp(&v, &shown, sizeof(v)))
        {
            draw(px.data());
            shown = v;
            first = false;
        }
        wiiu_gx2_frame();   // shows the image and waits for VSync
    }
    if (chosen)
    {
        const picker_entry *e = picker_core_selected();
        snprintf(path, size, "%s", e->path);
        printf("[picker] chose '%s'\n", e->name);
    }
    wiiu_gx2_hide_ui();   // the framebuffer path, black until the game's first frame
    return chosen;
}
