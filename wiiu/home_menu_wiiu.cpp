// The HOME menu (features plan, phase 1; home_menu=1 in wiiu.cfg, off until confirmed on a console).
//
// With it on, nullDC owns the HOME button while a game runs: a press opens this menu over the paused game, and a
// press held for 0.8 s opens the Wii U's own HOME Menu (its overlay, as HOME did before). The main loop calls
// wiiu_home_menu_tick() once a frame on the GX2 thread. HOME comes from the emulator thread's own input reads
// (wiiu_home_held, ogc_compat.cpp); while the menu is open the emulator is parked, so the menu reads the controllers
// itself, keeping the last sample when a read has none (polled faster than a controller reports, NO_SAMPLES would
// otherwise look like every button released: the pcsx port's menu flashed open and shut on a console).
#include <coreinit/systeminfo.h>   // OSEnableHomeButtonMenu
#include <coreinit/thread.h>
#include <coreinit/time.h>
#include <padscore/kpad.h>
#include <sysapp/switch.h>         // _SYSSwitchToHBMWithMode
#include <vpad/input.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <vector>
#include <string>
#include <stdint.h>

extern "C" bool wiiu_home_held();                                   // ogc_compat.cpp
void wiiu_menu_draw(uint32_t *px, const char *title, const char *const *rows, int n, int sel, const char *help);   // picker_wiiu.cpp
void wiiu_menu_size(uint32_t *w, uint32_t *h);
void wiiu_gx2_show_menu(const uint32_t *px, uint32_t w, uint32_t h);               // present_gx2.cpp
void wiiu_gx2_hide_ui();
void wiiu_gx2_frame();
void wiiu_emu_pause(bool pause);                                     // main_wiiu.cpp
extern char selectedFilePath[512];
void wiiu_disc_swap_request(const char *path);
void wiiu_rumble_stop();                          // rumble_wiiu.cpp   // main_wiiu.cpp: done on the emulator thread
// settings (picker_wiiu.cpp)
int wiiu_option_count();
const char *wiiu_option_key(int i);
const char *wiiu_option_label(int i);
const char *wiiu_option_help(int i);
int wiiu_option_values(int i);
const char *wiiu_option_value_name(int i, int v);
int wiiu_option_value(int i);
std::string wiiu_game_name(const char *path);
std::string wiiu_game_settings_path(const char *game_path);
std::string wiiu_config_path();
std::string wiiu_settings_get(const std::string &path, const char *key);
bool wiiu_settings_set(const std::string &path, const char *key, const char *value);
bool wiiu_app_running();

int wiiu_home_menu;                 // home_menu=1: this menu owns HOME (off: HOME opens the Wii U's HOME Menu, as before)
static bool owns_home, hbm_open;
#ifdef WIIU_INSTRUMENT
// home_test=N (test setting, Cemu has no HOME to press): N s into the game a short HOME press opens the menu, then
// a fixed script drives it (see read_menu_buttons). Each step is logged ("[home] test: ...")
int wiiu_home_test, wiiu_home_script = 1;
static int test_frame = -1;   // frames since the test's HOME press, -1: not started
static OSTime test_t0;
#endif

// menu buttons, from every controller
enum { MB_UP = 1, MB_DOWN = 2, MB_ACCEPT = 4, MB_BACK = 8, MB_HOME = 16 };
static uint32_t vpad_last, kpad_last[4];
static uint32_t read_menu_buttons_real();
static uint32_t read_menu_buttons()
{
    uint32_t b = read_menu_buttons_real();
#ifdef WIIU_INSTRUMENT
    if (test_frame >= 0)
    {
        // DOWN to "Settings for this game", A into it, A steps the first option, B back, B to the game
        struct Step { int frame; uint32_t button; const char *name; };
        // home_script=1: settings (DOWN, A, A steps fast memory, B, B); 2: DOWN x3, A (restart the game);
        // 3: DOWN x4, A (quit to the game list); 4: HOME held for a second instead of pressed
        static const Step settings[] = { { 120, MB_DOWN, "DOWN" }, { 180, MB_ACCEPT, "A" }, { 240, MB_ACCEPT, "A" },
                                         { 300, MB_BACK, "B" }, { 360, MB_BACK, "B" } };
        static const Step restart[] = { { 120, MB_DOWN, "DOWN" }, { 150, MB_DOWN, "DOWN" }, { 180, MB_DOWN, "DOWN" },
                                        { 240, MB_ACCEPT, "A" } };
        static const Step picker[] = { { 120, MB_DOWN, "DOWN" }, { 150, MB_DOWN, "DOWN" }, { 180, MB_DOWN, "DOWN" },
                                       { 210, MB_DOWN, "DOWN" }, { 270, MB_ACCEPT, "A" } };
        // 5: a disc set: DOWN to "Change disc", A, DOWN to the next disc, A (the swap)
        static const Step disc[] = { { 120, MB_DOWN, "DOWN" }, { 180, MB_ACCEPT, "A" }, { 240, MB_DOWN, "DOWN" },
                                     { 300, MB_ACCEPT, "A" } };
        const Step *script = wiiu_home_script == 2 ? restart : wiiu_home_script == 3 ? picker
                           : wiiu_home_script == 5 ? disc : settings;
        const int steps = wiiu_home_script == 2 ? 4 : wiiu_home_script == 3 ? 5 : wiiu_home_script == 5 ? 4 : 5;
        test_frame++;
        for (int k = 0; k < steps; k++)
        {
            const Step &e = script[k];
            if (test_frame >= e.frame && test_frame < e.frame + 6)
                b |= e.button;
            if (test_frame == e.frame)
                printf("[home] test: %s\n", e.name);
        }
    }
#endif
    return b;
}
static uint32_t read_menu_buttons_real()
{
    VPADStatus vs;
    VPADReadError ve;
    if (VPADRead(VPAD_CHAN_0, &vs, 1, &ve) == 1 && ve == VPAD_READ_SUCCESS)
    {
        uint32_t b = 0;
        if (vs.hold & (VPAD_BUTTON_UP | VPAD_STICK_L_EMULATION_UP))
            b |= MB_UP;
        if (vs.hold & (VPAD_BUTTON_DOWN | VPAD_STICK_L_EMULATION_DOWN))
            b |= MB_DOWN;
        if (vs.hold & VPAD_BUTTON_A)
            b |= MB_ACCEPT;
        if (vs.hold & VPAD_BUTTON_B)
            b |= MB_BACK;
        if (vs.hold & VPAD_BUTTON_HOME)
            b |= MB_HOME;
        vpad_last = b;
    }
    else if (ve != VPAD_READ_NO_SAMPLES)
        vpad_last = 0;   // a real error: nothing held (no sample: keep the last one)
    uint32_t all = vpad_last;
    for (int c = 0; c < 4; c++)
    {
        KPADStatus st;
        KPADError err;
        if (KPADReadEx((KPADChan)c, &st, 1, &err) >= 1)
        {
            uint32_t b = 0;
            const uint32_t h = st.hold;
            if (h & WPAD_BUTTON_UP) b |= MB_UP;
            if (h & WPAD_BUTTON_DOWN) b |= MB_DOWN;
            if (h & WPAD_BUTTON_A) b |= MB_ACCEPT;
            if (h & WPAD_BUTTON_B) b |= MB_BACK;
            if (h & WPAD_BUTTON_HOME) b |= MB_HOME;
            uint32_t x = 0;
            if (st.extensionType == WPAD_EXT_CLASSIC || st.extensionType == WPAD_EXT_MPLUS_CLASSIC)
                x = st.classic.hold;
            else if (st.extensionType == WPAD_EXT_PRO_CONTROLLER)
                x = st.pro.hold;
            if (x & WPAD_CLASSIC_BUTTON_UP) b |= MB_UP;
            if (x & WPAD_CLASSIC_BUTTON_DOWN) b |= MB_DOWN;
            if (x & WPAD_CLASSIC_BUTTON_A) b |= MB_ACCEPT;
            if (x & WPAD_CLASSIC_BUTTON_B) b |= MB_BACK;
            if (x & WPAD_CLASSIC_BUTTON_HOME) b |= MB_HOME;
            kpad_last[c] = b;
        }
        else if (err != KPAD_ERROR_NO_SAMPLES)
            kpad_last[c] = 0;
        all |= kpad_last[c];
    }
    return all;
}

// one page of rows: returns the chosen row, or -1 for back (B or HOME). `rows` may change while it runs (settings):
// draw() rebuilds them on every change
template <typename Draw>
static int run_page(std::vector<uint32_t> &px, uint32_t w, uint32_t h, int n, int &sel, Draw draw, bool *home)
{
    uint32_t held = read_menu_buttons() | MB_HOME | MB_ACCEPT | MB_BACK;   // presses from before count once released
    int drawn = -1;
    while (wiiu_app_running())
    {
        const uint32_t b = read_menu_buttons(), pressed = b & ~held;
        held = b;
        if (pressed & MB_UP)
            sel = (sel + n - 1) % n;
        if (pressed & MB_DOWN)
            sel = (sel + 1) % n;
        if (pressed & (MB_BACK | MB_HOME))
        {
            if (home)
                *home = (pressed & MB_HOME) != 0;
            return -1;
        }
        if (pressed & MB_ACCEPT)
            return sel;
        if (sel != drawn)
        {
            draw(px.data(), sel);
            wiiu_gx2_show_menu(px.data(), w, h);
            drawn = sel;
        }
        wiiu_gx2_frame();   // the game's last picture with the menu over it; waits for VSync
    }
    return -1;
}

// a settings page: every option (all games: config.txt) or the ones a game can have (this game: gamecfg/<game>.txt,
// "as all games" first). A steps the value and saves at once; most take effect at the next start (the help says)
static void settings_page(std::vector<uint32_t> &px, uint32_t w, uint32_t h, bool game, bool *home)
{
    const std::string path = game ? wiiu_game_settings_path(selectedFilePath) : wiiu_config_path();
    const int n = game ? 4 : wiiu_option_count();   // WIIU_GAME_OPTIONS in picker_wiiu.cpp
    const std::string title = game ? "Settings for " + wiiu_game_name(selectedFilePath) : std::string("Settings for all games");
    int sel = 0;
    for (;;)
    {
        std::vector<std::string> lines(n);
        std::vector<const char *> rows(n);
        auto value_of = [&](int i) -> std::string {
            const std::string v = wiiu_settings_get(path, wiiu_option_key(i));
            if (!v.empty())
                return v;
            if (game)
                return "";   // as all games
            return wiiu_option_value_name(i, wiiu_option_value(i));
        };
        for (int i = 0; i < n; i++)
        {
            const std::string v = value_of(i);
            lines[i] = std::string(wiiu_option_label(i)) + ":  " + (v.empty() ? std::string("as all games (") + wiiu_option_value_name(i, wiiu_option_value(i)) + ")" : v);
            rows[i] = lines[i].c_str();
        }
        const int r = run_page(px, w, h, n, sel, [&](uint32_t *p, int s) {
            wiiu_menu_draw(p, title.c_str(), rows.data(), n, s, wiiu_option_help(s));
        }, home);
        if (r < 0)
            return;
        // step the value: this game cycles "as all games" -> each value -> back; all games cycles the values
        const std::string cur = value_of(r);
        const int nv = wiiu_option_values(r);
        int idx = -1;
        for (int v = 0; v < nv; v++)
            if (cur == wiiu_option_value_name(r, v))
                idx = v;
        std::string next;
        if (game)
            next = idx + 1 < nv ? wiiu_option_value_name(r, idx + 1) : "";
        else
            next = wiiu_option_value_name(r, (idx + 1) % nv);
        if (!wiiu_settings_set(path, wiiu_option_key(r), next.c_str()))
            printf("[home] couldn't save %s\n", path.c_str());
        else
            printf("[home] %s: %s=%s\n", path.c_str(), wiiu_option_key(r), next.empty() ? "(as all games)" : next.c_str());
    }
}

// the discs of a set: the files in the game's folder whose names differ from its own only in " (Disc N)", by N.
// One disc (or no " (Disc N)" in the name): just that one
static std::vector<std::pair<int, std::string>> disc_set(const std::string &path)
{
    std::vector<std::pair<int, std::string>> set;
    const size_t slash = path.find_last_of('/');
    const std::string dir = slash == std::string::npos ? "." : path.substr(0, slash), file = path.substr(slash + 1);
    const size_t at = file.find(" (Disc ");
    if (at != std::string::npos)
    {
        const size_t close = file.find(')', at);
        const std::string head = file.substr(0, at), tail = close == std::string::npos ? "" : file.substr(close + 1);
        if (DIR *d = opendir(dir.c_str()))
        {
            while (struct dirent *e = readdir(d))
            {
                const std::string n = e->d_name;
                if (n.size() > head.size() + tail.size() + 8 && !n.compare(0, head.size(), head) &&
                    !n.compare(head.size(), 7, " (Disc ") && n.size() >= tail.size() &&
                    !n.compare(n.size() - tail.size(), tail.size(), tail))
                    set.push_back({ atoi(n.c_str() + head.size() + 7), dir + "/" + n });
            }
            closedir(d);
        }
        std::sort(set.begin(), set.end());
    }
    if (set.empty())
        set.push_back({ 1, path });
    return set;
}

// the disc page: each disc of the set, the one in the drive marked; A puts the chosen one in (the menu then closes, the
// lid opens and closes on the emulator thread). Returns true when a swap was asked for
static bool disc_page(std::vector<uint32_t> &px, uint32_t w, uint32_t h, bool *home)
{
    const auto set = disc_set(selectedFilePath);
    std::vector<std::string> lines;
    std::vector<const char *> rows;
    int sel = 0;
    for (size_t i = 0; i < set.size(); i++)
    {
        const bool in = set[i].second == selectedFilePath;
        lines.push_back("Disc " + std::to_string(set[i].first) + (in ? "  (in the drive)" : ""));
        if (in)
            sel = (int)i;
    }
    for (const auto &l : lines)
        rows.push_back(l.c_str());
    const int r = run_page(px, w, h, (int)rows.size(), sel, [&](uint32_t *p, int s) {
        wiiu_menu_draw(p, "Change disc", rows.data(), (int)rows.size(), s,
                       "A: open the lid, put this disc in and close it again (about 1.5 s), as when a game asks for "
                       "the next disc. B: back.");
    }, home);
    if (r < 0 || set[r].second == selectedFilePath)
        return false;
    printf("[home] disc swap: %s\n", set[r].second.c_str());
    wiiu_disc_swap_request(set[r].second.c_str());
    return true;
}

// the menu: returns what the main loop should do (1 the game list, 2 the Wii U Menu, 3 restart the game)
enum { ROW_RESUME, ROW_DISC, ROW_GAME_SETTINGS, ROW_ALL_SETTINGS, ROW_RESTART, ROW_PICKER, ROW_QUIT };
static int run_menu()
{
    struct Row { int id; const char *label, *help; };
    std::vector<Row> rows = {
        { ROW_RESUME, "Resume", "Back to the game. B or HOME does the same; HOME held opens the Wii U HOME Menu." },
        { ROW_GAME_SETTINGS, "Settings for this game",
          "This game's own settings, over the ones for all games. Most take effect at the next start." },
        { ROW_ALL_SETTINGS, "Settings for all games",
          "The settings every game starts with (the game list's options page). Most take effect at the next start." },
        { ROW_RESTART, "Restart the game", "Starts this game again from the Dreamcast's boot, as if switched off and on." },
        { ROW_PICKER, "Quit to the game list", "Stops the game and goes back to the list of games." },
        { ROW_QUIT, "Quit to the Wii U Menu", "Stops the game and goes back to the Wii U Menu." } };
    const auto set = disc_set(selectedFilePath);
    std::string disc_label;
    if (set.size() > 1)
    {
        int in = 1;
        for (const auto &d : set)
            if (d.second == selectedFilePath)
                in = d.first;
        disc_label = "Change disc (disc " + std::to_string(in) + " of " + std::to_string(set.size()) + " in the drive)";
        rows.insert(rows.begin() + 1, { ROW_DISC, disc_label.c_str(), "Opens the lid and puts another disc of this game in." });
    }
    std::vector<const char *> labels;
    for (const auto &r : rows)
        labels.push_back(r.label);
    uint32_t w, h;
    wiiu_menu_size(&w, &h);
    std::vector<uint32_t> px(w * h);
    printf("[home] menu open\n");
    wiiu_emu_pause(true);
    wiiu_rumble_stop();
    int sel = 0, result = ROW_RESUME;
    for (;;)
    {
        bool home = false;
        const int r = run_page(px, w, h, (int)rows.size(), sel, [&](uint32_t *p, int s) {
            wiiu_menu_draw(p, "nullDC", labels.data(), (int)labels.size(), s, rows[s].help);
        }, &home);
        const int id = r < 0 ? ROW_RESUME : rows[r].id;
        if (id == ROW_GAME_SETTINGS || id == ROW_ALL_SETTINGS)
        {
            settings_page(px, w, h, id == ROW_GAME_SETTINGS, &home);
            if (home)
                break;   // HOME leaves the menu from any page
            continue;
        }
        if (id == ROW_DISC)
        {
            if (disc_page(px, w, h, &home) || home)
                break;   // a swap is under way: back to the game to see it
            continue;
        }
        result = id;
        break;
    }
    wiiu_gx2_hide_ui();
    if (result == ROW_RESTART)
    {
        if (FILE *f = fopen("fs:/vol/external01/wiiu/nulldc/restart.txt", "w"))
        {
            fprintf(f, "%s\n", selectedFilePath);
            fclose(f);
        }
    }
    wiiu_emu_pause(false);
    printf("[home] menu closed (%d)\n", result);
    return result == ROW_PICKER ? 1 : result == ROW_QUIT ? 2 : result == ROW_RESTART ? 3 : 0;
}

// at a game's start, when home_menu is on
void wiiu_home_menu_start()
{
    if (!wiiu_home_menu)
        return;
    OSEnableHomeButtonMenu(FALSE);
    owns_home = true;
    printf("[home] HOME opens nullDC's menu; held 0.8 s, the Wii U HOME Menu\n");
}

// ProcUI gave the foreground back (main_wiiu.cpp's acquire callback): after the Wii U HOME Menu, HOME is ours again
void wiiu_home_menu_acquired()
{
    if (owns_home && hbm_open)
    {
        OSEnableHomeButtonMenu(FALSE);
        hbm_open = false;
    }
}

// once a frame on the GX2 thread while a game runs: 0 nothing, else HOME_MENU_PICKER or HOME_MENU_QUIT
int wiiu_home_menu_tick()
{
    if (!owns_home)
        return 0;
    static bool was, long_done, wait_release;
    static OSTime down_at;
    bool down = wiiu_home_held();
    const OSTime now = OSGetSystemTime();
#ifdef WIIU_INSTRUMENT
    if (wiiu_home_test)
    {
        if (!test_t0)
            test_t0 = now;
        static int fake;   // frames of a held HOME, from the test
        if (test_frame < 0 && now - test_t0 > OSSecondsToTicks(wiiu_home_test))
        {
            printf("[home] test: HOME pressed\n");
            test_frame = 0;
            fake = wiiu_home_script == 4 ? 60 : 6;   // 4: HOME held a second (the Wii U HOME Menu)
        }
        if (fake > 0)
        {
            fake--;
            down = true;
        }
    }
#endif
    if (wait_release)
    {
        // the menu closed with HOME, maybe still held: nothing until it's released
        if (!down)
            wait_release = false;
        was = false;
        return 0;
    }
    if (down && !was)
    {
        down_at = now;
        long_done = false;
    }
    int action = 0;
    if (down && !long_done && now - down_at > OSMillisecondsToTicks(800))
    {
        // held: the Wii U's HOME Menu. ProcUI then releases the foreground as before (main_wiiu.cpp parks the
        // emulator and GX2), and the acquire callback takes HOME back
        long_done = true;
        printf("[home] HOME held: the Wii U HOME Menu\n");
        hbm_open = true;
        OSEnableHomeButtonMenu(TRUE);
        _SYSSwitchToHBMWithMode(0);
    }
    else if (!down && was && !long_done)
    {
        action = run_menu();
        wait_release = true;
    }
    was = down;
    return action;
}
