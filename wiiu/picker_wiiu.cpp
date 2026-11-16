// Game picker, shown before the emulator starts when wiiu.cfg names no file. The logic (config.txt, the scan of
// the home folder two levels deep, the menu, the help text) is picker/picker_core.c, structured like FBNeo's
// player/picker but kept separately; this file is only the UI for it: Roboto drawn into a 960x720 image shown
// through GX2 like the emulator's framebuffer, and the
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

#define NULLDC_WIIU_VERSION "v0.800-U1"   // this Wii U release (upstream alpha 0.800); README-WiiU.md

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
    static VPADStatus s_vs;
    static bool s_have_vs = false;
    static int s_stick_dir = 0;   // -1 up, 0 neutral, +1 down (with hysteresis)
    VPADStatus vs;
    VPADReadError ve = VPAD_READ_UNINITIALIZED;
    if (VPADRead(VPAD_CHAN_0, &vs, 1, &ve) == 1 && ve == VPAD_READ_SUCCESS)
    {
        s_vs = vs;
        s_have_vs = true;
    }
    if (s_have_vs)
    {
        const float ly = s_vs.leftStick.y;
        if (s_stick_dir == 0)
            s_stick_dir = (ly > 0.55f) ? -1 : (ly < -0.55f) ? 1 : 0;
        else if ((s_stick_dir == -1 && ly < 0.30f) || (s_stick_dir == 1 && ly > -0.30f))
            s_stick_dir = 0;

        const bool in_opts = picker_core_view()->options != 0;
        const u32 trig = s_vs.trigger;
        const u32 active = trig ? trig : s_vs.hold;

        if ((active & (VPAD_BUTTON_A | (in_opts ? (VPAD_BUTTON_RIGHT | VPAD_BUTTON_LEFT) : 0))))
            b |= PICKER_BTN_ACCEPT;
        else if (active & VPAD_BUTTON_B)
            b |= PICKER_BTN_BACK;
        else if (active & VPAD_BUTTON_X)
            b |= PICKER_BTN_OPTIONS;
        else if (active & VPAD_BUTTON_Y)
            b |= PICKER_BTN_HELP;
        else
        {
            if ((active & VPAD_BUTTON_UP) || s_stick_dir == -1)
                b |= PICKER_BTN_UP;
            if ((active & VPAD_BUTTON_DOWN) || s_stick_dir == 1)
                b |= PICKER_BTN_DOWN;
        }
        if ((s_vs.hold & (VPAD_BUTTON_PLUS | VPAD_BUTTON_MINUS)) == (VPAD_BUTTON_PLUS | VPAD_BUTTON_MINUS))
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
        if ((p.core & 0x0004) || (classic && (p.exp & 0x0040)))   // B: Wii Remote 0x0004, Classic/Pro 0x0040
            b |= PICKER_BTN_BACK;
        if (((p.core & 0x1010) == 0x1010) || (classic && (p.exp & 0x1400) == 0x1400))
            b |= PICKER_BTN_QUIT_COMBO;
    }
    return b;
}

const u32 W = 960, H = 720, MARGIN = 28;   // shown 4:3; Roboto from font_roboto.h (make_font.py)
const u32 BG = 0x102040FF, FG = 0xE0E0E0FF, HILITE = 0xFFE060FF, DIM = 0x9090A0FF;
const u32 ROW_SEL = 0x1E3A6EFF, LINE_COL = 0x2A4A80FF, WARN_COL = 0xFF8040FF;

void fill_rect(u32 *px, u32 rx, u32 ry, u32 rw, u32 rh, u32 color)
{
    for (u32 y = ry; y < ry + rh && y < H; y++)
        for (u32 x = rx; x < rx + rw && x < W; x++)
            px[y * W + x] = color;
}

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

void draw_header_status(u32 *px)
{
    text(px, MARGIN, 1, "nullDC for Wii U  " NULLDC_WIIU_VERSION, HILITE);
    const bool lgun = picker_core_option(1) == 1;
    const int clayout = picker_core_option(10);
    const char *status = lgun ? "[!] LIGHT GUN MODE ON" :
                         (clayout == 1 ? "Pad: Nintendo (A=A, B=B)" :
                          clayout == 2 ? "Pad: Custom (user_controls.cfg)" : "Pad: Dreamcast Pos (B=A)");
    const u32 sw = text_width(status);
    if (sw + MARGIN < W)
        text(px, W - MARGIN - sw, 1, status, lgun ? WARN_COL : DIM);
    fill_rect(px, MARGIN, 2 * FONT_LINE - 6, W - MARGIN * 2, 2, LINE_COL);
}

void draw(u32 *px)
{
    const picker_view &v = *picker_core_view();
    for (u32 i = 0; i < W * H; i++)
        px[i] = BG;
    draw_header_status(px);
    if (v.options)
    {
        text(px, MARGIN, 2, "Options & Controls   (A: change value   B: back to games)", DIM);
        const int n = options_count();
        const int max_vis = 11;
        int top = v.opt_sel - max_vis / 2;
        if (top > n - max_vis) top = n - max_vis;
        if (top < 0) top = 0;
        const int vis = (n < max_vis) ? n : max_vis;
        for (int k = 0; k < vis; k++)
        {
            const int i = top + k;
            const u32 r = 3 + k;
            if (i == v.opt_sel)
                fill_rect(px, MARGIN - 8, r * FONT_LINE + 4, W - (MARGIN - 8) * 2, FONT_LINE, ROW_SEL);
            char line[80];
            snprintf(line, sizeof(line), "%s%s: %s", i == v.opt_sel ? "> " : "   ", option_label(i),
                     picker_core_option_value(i));
            text(px, MARGIN, r, line, i == v.opt_sel ? HILITE : FG);
        }
        u32 row = 4 + vis;
        fill_rect(px, MARGIN, row * FONT_LINE - 4, W - MARGIN * 2, 2, LINE_COL);
        if (option_help(v.opt_sel))
            row += text_wrapped(px, MARGIN, row, option_help(v.opt_sel), FG);
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
        text(px, MARGIN, 2, "A: start   X: options & controls   Y: help   hold + and -: quit", DIM);
        for (int i = 0; i < v.visible && v.top + i < v.count; i++)
        {
            const int idx = v.top + i;
            const u32 r = 4 + i;
            if (idx == v.sel)
                fill_rect(px, MARGIN - 8, r * FONT_LINE + 4, W - (MARGIN - 8) * 2, FONT_LINE, ROW_SEL);
            char line[128];
            snprintf(line, sizeof(line), "%s%.90s", idx == v.sel ? "> " : "   ", picker_core_entry(idx)->name);
            text(px, MARGIN, r, line, idx == v.sel ? HILITE : FG);
        }
        char count[24];
        snprintf(count, sizeof(count), "%d / %d", v.sel + 1, v.count);
        text(px, MARGIN, 4 + v.visible + 1, count, DIM);
    }
    wiiu_gx2_show_ui(px, W, H);
}

void draw_lightgun_warning(u32 *px, const char *game_name)
{
    for (u32 i = 0; i < W * H; i++)
        px[i] = BG;
    draw_header_status(px);
    fill_rect(px, MARGIN, 4 * FONT_LINE, W - MARGIN * 2, 9 * FONT_LINE, ROW_SEL);
    text(px, MARGIN + 16, 5, "WARNING: Light Gun Mode is Enabled!", WARN_COL);
    text(px, MARGIN + 16, 6, game_name ? game_name : "", HILITE);
    text_wrapped(px, MARGIN + 16, 8,
                 "When Light Gun mode is ON, standard controllers (GamePad, Pro Controller, Classic Controller) "
                 "are disabled in-game so the Wii Remote IR pointer can act as the Dreamcast gun.", FG);
    text(px, MARGIN + 16, 11, "A: Launch with Light Gun     X: Switch to Standard Pad & Launch     B: Cancel", HILITE);
    wiiu_gx2_show_ui(px, W, H);
}

const char *const on_off[] = { "on", "off" }, *const off_on[] = { "off", "on" };
const char *const sizes[] = { "960x720", "640x480" }, *const slow_sound[] = { "slows down", "keeps pitch" };
const char *const sound_buf[] = { "100 ms", "46 ms" };
const char *const ctrl_layouts[] = { "Dreamcast pos (B=A)", "Nintendo (A=A, B=B)", "Custom (user_controls.cfg)" };
const char *const ratios[] = { "4:3 original", "16:9 fullscreen" };
const char *const cables[] = { "VGA (480p 60Hz)", "Composite", "RGB SCART" };
const char *const fskips[] = { "0 (off)", "1", "2", "auto", "auto max" };
const char *const rdelays[] = { "flycast (auto)", "on (hw-like)", "off (instant)" };
const char *const vmu_modes[] = { "off", "corner (bottom-left)", "gamepad dedicated" };
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
    { "gfx_core", "Graphics on own core",
      "Graphics on a core of its own, away from the emulated CPU: the biggest speedup (Soulcalibur fights 97% to "
      "131%). Next start.", on_off, 2, 0 },
    { "sound_core", "Sound on own core",
      "Sound mixing on a core of its own: about 12% faster. Turn it off if sounds or music go missing (the "
      "Dreamcast chime, streamed music). Next start.", on_off, 2, 0 },
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
    { "ctrl_layout", "Control layout",
      "Dreamcast pos: physical positions (B=A, A=B, Y=X, X=Y). Nintendo: matching letters (A=A, B=B, X=X, Y=Y). "
      "Custom: reads sd:/wiiu/nulldc/user_controls.cfg.",
      ctrl_layouts, 3, 0 },
    { "ratio", "Aspect ratio",
      "4:3 original keeps the Dreamcast proportion with side bars; 16:9 stretches to fill the screen.",
      ratios, 2, 0 },
    { "cable", "Video cable",
      "VGA forces 480p 60Hz progressive (best). Switch to Composite or RGB if a non-VGA game refuses to boot.",
      cables, 3, 0 },
    { "frameskip", "Frameskip",
      "Drops renders when behind real time so game speed stays at 100%. Use auto on heavy scenes.",
      fskips, 5, 0 },
    { "render_delay", "Render IRQ timing",
      "Flycast (default) paces render-done IRQs by scene size. Use 'on' for MvC2/CvS2 if framerate drops.",
      rdelays, 3, 0 },
    { "vmu_screen", "VMU on GamePad",
      "Off: hidden. Corner: small VMU in bottom-left of GamePad. Dedicated: full VMU + FPS on GamePad while TV shows the game.",
      vmu_modes, 3, 1 },
};
int options_count() { return (int)(sizeof(options) / sizeof(options[0])); }
const char *option_label(int i) { return options[i].label; }
const char *option_help(int i) { return i >= 0 && i < options_count() ? options[i].help : nullptr; }
} // namespace

static void setup(const char *home)
{
    static std::string data_dir;
    data_dir = home;
    while (data_dir.size() > 1 && data_dir.back() == '/')
        data_dir.pop_back();
    static const char *const exts[] = { "gdi", "cdi", "mds", "elf", nullptr };
    static const char *const skip[] = { "data", nullptr };
    static const picker_entry extras[] = { { "Dreamcast menu (no disc): memory cards, settings", "" } };
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
    bios_help = "Dreamcast BIOS: dc_boot.bin and dc_flash.bin in " + data_dir + "/data/. Memory cards A1-D2 are "
                "vmu_save_*.bin there too; the Dreamcast menu's File screen copies and deletes saves.";
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

static bool s_override_lightgun_off = false;

// the options' values from config.txt (written by the picker's options page), whether the picker runs or not
void wiiu_picker_options(const char *home, bool *fastmem, bool *lightgun, int *render_width, bool *keep_pitch)
{
    setup(home);
    *fastmem = picker_core_option(0) == 0;    // on_off: index 0 is "on"
    *lightgun = !s_override_lightgun_off && (picker_core_option(1) == 1);   // off_on: index 1 is "on"
    *render_width = picker_core_option(2) == 1 ? 640 : 960;
    *keep_pitch = picker_core_option(3) == 1;
}

int wiiu_picker_ctrl_layout(const char *home)
{
    setup(home);
    return picker_core_option(10);   // 0: Dreamcast pos, 1: Nintendo (A=A), 2: Custom (user_controls.cfg)
}

void wiiu_picker_extra_options(const char *home, int *ratio, int *cable, int *frameskip, int *render_delay)
{
    setup(home);
    *ratio = picker_core_option(11);          // 0: 4:3, 1: 16:9
    const int c = picker_core_option(12);     // 0: VGA(2), 1: Composite(0), 2: RGB(1)
    *cable = (c == 0) ? 2 : (c == 1) ? 0 : 1;
    *frameskip = picker_core_option(13);      // 0..4
    const int rd = picker_core_option(14);    // 0: flycast(2), 1: on(1), 2: off(0)
    *render_delay = (rd == 0) ? 2 : (rd == 1) ? 1 : 0;
}

int wiiu_picker_vmu_mode(const char *home)
{
    setup(home);
    return picker_core_option(15);   // 0: off, 1: corner (bottom-left), 2: gamepad dedicated
}

// the speed settings, read at startup (before the threads exist), from the same config.txt
void wiiu_picker_speed_options(const char *home, bool *gfx_core, bool *sound_core, bool *jit_fast, bool *smooth,
                               bool *code_check, int *sound_buffer_ms)
{
    setup(home);
    *gfx_core = picker_core_option(4) == 0;
    *sound_core = picker_core_option(5) == 0;
    *jit_fast = picker_core_option(6) == 0;
    *smooth = picker_core_option(7) == 0;
    *code_check = picker_core_option(8) == 0;
    *sound_buffer_ms = picker_core_option(9) == 0 ? 100 : 0;
}

// home: the data folder (ends in '/'), its config.txt and, by default, the content folder. path: the chosen file,
// empty for the BIOS. False when the user quit.
bool wiiu_pick_game(const char *home, char *path, size_t size)
{
    s_override_lightgun_off = false;
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
    bool in_lgun_warning = false;
    u32 warn_prev_btn = ~0u;
    picker_view shown = {};
    bool first = true;
    while (wiiu_app_running())
    {
        const u32 btns = read_buttons();
        if (in_lgun_warning)
        {
            const u32 pressed = btns & ~warn_prev_btn;
            warn_prev_btn = btns;
            if (pressed & PICKER_BTN_ACCEPT)
            {
                chosen = true;
                break;
            }
            if (pressed & PICKER_BTN_OPTIONS)
            {
                s_override_lightgun_off = true;
                chosen = true;
                break;
            }
            if (pressed & PICKER_BTN_BACK)
            {
                in_lgun_warning = false;
                first = true;
            }
            wiiu_gx2_frame();
            continue;
        }

        const int ev = picker_core_input(btns, (unsigned)OSTicksToMilliseconds(OSGetSystemTime()));
        if (ev == PICKER_QUIT)
            break;
        if (ev == PICKER_CHOSE)
        {
            if (picker_core_option(1) == 1)   // Light gun is ON -> warn user first
            {
                in_lgun_warning = true;
                warn_prev_btn = btns;
                const picker_entry *e = picker_core_selected();
                draw_lightgun_warning(px.data(), e ? e->name : "");
                wiiu_gx2_frame();
                continue;
            }
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
