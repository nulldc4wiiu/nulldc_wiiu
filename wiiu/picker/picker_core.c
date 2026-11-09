/* picker_core - the logic of a Wii U content picker, without UI. See picker_core.h. */
#include "picker_core.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#define MAX_ITEMS 1024
#define MAX_KEYS 32

typedef struct {
    char *name, *path;
    char label[32];
} item;

static const picker_cfg *g_cfg;
static char g_content[512];
static char g_autoload[512];
static char g_keys[MAX_KEYS][32], g_vals[MAX_KEYS][256];
static int g_nkeys;
static char g_config_path[512];
#define MAX_OPTIONS 16
static int g_opt[MAX_OPTIONS];

static item g_items[MAX_ITEMS];
static int g_count, g_first_scanned;
static picker_view g_view = { 0, 0, 0, 0, 0, 15 };
static picker_entry g_entry;

/* input state */
static unsigned g_last = ~0u;   /* everything "held" at the start: a leftover press only counts once released */
static unsigned g_repeat_at;
static unsigned g_quit_since;
static int g_quit_armed;

/* ---- helpers ---- */

static void mkdirs(const char *path)
{
    char tmp[512];
    size_t i;
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (i = 1; tmp[i]; i++)
        if (tmp[i] == '/' && tmp[i - 1] != ':') {   /* not the "fs:" device separator */
            tmp[i] = 0;
            mkdir(tmp, 0777);
            tmp[i] = '/';
        }
    mkdir(tmp, 0777);
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = (char *)malloc(n);
    if (d)
        memcpy(d, s, n);
    return d;
}

static int has_ext(const char *name, const char **ext_out)
{
    const char *dot = strrchr(name, '.');
    const char *const *e;
    if (!dot || dot == name)
        return 0;
    for (e = g_cfg->exts; e && *e; e++)
        if (!strcasecmp(dot + 1, *e)) {
            if (ext_out)
                *ext_out = dot;
            return 1;
        }
    return 0;
}

static int skipped(const char *dir)
{
    const char *const *s;
    for (s = g_cfg->skip_dirs; s && *s; s++)
        if (!strcasecmp(dir, *s))
            return 1;
    return 0;
}

/* ---- config ---- */

void picker_core_init(const picker_cfg *cfg)
{
    char line[800];
    FILE *f;
    g_cfg = cfg;
    g_nkeys = 0;
    g_autoload[0] = 0;
    if (cfg->content_default)
        snprintf(g_content, sizeof(g_content), "%s", cfg->content_default);
    else
        snprintf(g_content, sizeof(g_content), "%s/%ss", cfg->data_dir, cfg->content_noun);
    snprintf(g_config_path, sizeof(g_config_path), "%s/config.txt", cfg->data_dir);
    f = fopen(g_config_path, "r");
    if (f) {
        while (fgets(line, sizeof(line), f)) {
            char *eq;
            line[strcspn(line, "\r\n")] = 0;
            if (line[0] == '#' || !(eq = strchr(line, '=')))
                continue;
            *eq = 0;
            /* "content=" is the generic key; "roms=" accepted as an alias (FBNeo's older configs) */
            if ((!strcmp(line, "content") || !strcmp(line, "roms")) && eq[1])
                snprintf(g_content, sizeof(g_content), "%s", eq + 1);
            else if (!strcmp(line, "autoload"))
                snprintf(g_autoload, sizeof(g_autoload), "%s", eq + 1);
            else if (g_nkeys < MAX_KEYS) {
                snprintf(g_keys[g_nkeys], sizeof(g_keys[0]), "%s", line);
                snprintf(g_vals[g_nkeys], sizeof(g_vals[0]), "%s", eq + 1);
                g_nkeys++;
            }
        }
        fclose(f);
    } else {
        mkdirs(cfg->data_dir);
        f = fopen(g_config_path, "w");
        if (f) {
            fprintf(f,
                    "# %s config\n"
                    "content=%s\n"
                    "# autoload=PATH   # boot straight into one %s\n",
                    cfg->app_name, g_content, cfg->content_noun);
            fclose(f);
        }
    }
    mkdirs(g_content);
    {
        int i, v;
        for (i = 0; i < cfg->n_options && i < MAX_OPTIONS; i++) {
            const char *set = picker_core_config(cfg->options[i].key);
            g_opt[i] = cfg->options[i].def;
            for (v = 0; set && v < cfg->options[i].n_values; v++)
                if (!strcasecmp(set, cfg->options[i].values[v]))
                    g_opt[i] = v;
        }
    }
}

int picker_core_option(int i) { return i >= 0 && i < g_cfg->n_options && i < MAX_OPTIONS ? g_opt[i] : 0; }
const char *picker_core_option_value(int i) { return g_cfg->options[i].values[picker_core_option(i)]; }

/* config.txt with option i's line replaced (or added); the other lines kept as they are */
static void save_option(int i)
{
    static char text[8192];
    char line[800];
    size_t n = 0;
    int found = 0;
    const picker_option *o = &g_cfg->options[i];
    const size_t klen = strlen(o->key);
    FILE *f = fopen(g_config_path, "r");
    text[0] = 0;
    if (f) {
        while (fgets(line, sizeof(line), f) && n + strlen(line) + 64 < sizeof(text)) {
            if (!strncmp(line, o->key, klen) && line[klen] == '=') {
                n += (size_t)snprintf(text + n, sizeof(text) - n, "%s=%s\n", o->key, o->values[g_opt[i]]);
                found = 1;
            } else
                n += (size_t)snprintf(text + n, sizeof(text) - n, "%s", line);
        }
        fclose(f);
    }
    if (!found)
        snprintf(text + n, sizeof(text) - n, "%s=%s\n", o->key, o->values[g_opt[i]]);
    f = fopen(g_config_path, "w");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}

const char *picker_core_content_dir(void) { return g_content; }
const char *picker_core_autoload(void) { return g_autoload; }

const char *picker_core_config(const char *key)
{
    int i;
    for (i = 0; i < g_nkeys; i++)
        if (!strcmp(g_keys[i], key))
            return g_vals[i];
    return NULL;
}

/* ---- scan ---- */

static void add(const char *name, const char *path)
{
    if (g_count >= MAX_ITEMS)
        return;
    g_items[g_count].name = dup_str(name);
    g_items[g_count].path = dup_str(path);
    g_items[g_count].label[0] = 0;
    if (g_items[g_count].name && g_items[g_count].path)
        g_count++;
}

static void scan_dir(const char *dir, int depth)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    if (!d)
        return;
    while ((e = readdir(d)) != NULL && g_count < MAX_ITEMS) {
        char path[768];
        const char *ext;
        if (e->d_name[0] == '.')
            continue;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (e->d_type == DT_DIR) {
            if (depth > 0 && !skipped(e->d_name))
                scan_dir(path, depth - 1);
        } else if (has_ext(e->d_name, &ext)) {
            char name[256];
            size_t n = (size_t)(ext - e->d_name);
            if (n >= sizeof(name))
                n = sizeof(name) - 1;
            memcpy(name, e->d_name, n);
            name[n] = 0;
            add(name, path);
        }
    }
    closedir(d);
}

static int cmp_items(const void *a, const void *b)
{
    return strcasecmp(((const item *)a)->name, ((const item *)b)->name);
}

int picker_core_scan(void)
{
    int i;
    for (i = 0; i < g_count; i++) {
        free(g_items[i].name);
        free(g_items[i].path);
    }
    g_count = 0;
    for (i = 0; i < g_cfg->n_extras; i++)
        add(g_cfg->extras[i].name, g_cfg->extras[i].path);
    g_first_scanned = g_count;
    scan_dir(g_content, g_cfg->scan_depth);
    qsort(g_items + g_first_scanned, (size_t)(g_count - g_first_scanned), sizeof(item), cmp_items);
    if (g_cfg->label_for)
        for (i = 0; i < g_count; i++) {
            const char *s = g_cfg->label_for(g_items[i].name, g_items[i].path);
            snprintf(g_items[i].label, sizeof(g_items[i].label), "%s", s ? s : "");
        }
    g_view.count = g_count;
    g_view.empty = g_count == g_first_scanned;   /* only the extras: nothing was found */
    g_view.help = g_view.empty || (g_cfg->warning && *g_cfg->warning);
    g_view.sel = g_count > g_first_scanned ? g_first_scanned : 0;
    g_view.top = 0;
    g_last = ~0u;
    g_quit_armed = 0;
    return g_count;
}

const picker_entry *picker_core_entry(int i)
{
    if (i < 0 || i >= g_count)
        return NULL;
    g_entry.name = g_items[i].name;
    g_entry.path = g_items[i].path;
    return &g_entry;
}

const char *picker_core_label(int i) { return (i >= 0 && i < g_count) ? g_items[i].label : ""; }

/* ---- the menu ---- */

void picker_core_set_visible(int rows) { g_view.visible = rows > 0 ? rows : 1; }

int picker_core_input(unsigned b, unsigned ms)
{
    const unsigned pressed = b & ~g_last;
    int move = 0;
    g_last = b;

    /* hold the quit combination ~0.5 s, once it was released after the picker started */
    if (b & PICKER_BTN_QUIT_COMBO) {
        if (!g_quit_since)
            g_quit_since = ms ? ms : 1;
        if (g_quit_armed && ms - g_quit_since >= 500)
            return PICKER_QUIT;
    } else {
        g_quit_since = 0;
        g_quit_armed = 1;
    }

    if (g_view.options) {
        const int n = g_cfg->n_options < MAX_OPTIONS ? g_cfg->n_options : MAX_OPTIONS;
        if (pressed & (PICKER_BTN_BACK | PICKER_BTN_OPTIONS | PICKER_BTN_HELP))
            g_view.options = 0;
        else if (n && (pressed & PICKER_BTN_UP))
            g_view.opt_sel = (g_view.opt_sel + n - 1) % n;
        else if (n && (pressed & PICKER_BTN_DOWN))
            g_view.opt_sel = (g_view.opt_sel + 1) % n;
        else if (n && (pressed & PICKER_BTN_ACCEPT)) {
            const int i = g_view.opt_sel;
            g_opt[i] = (g_opt[i] + 1) % g_cfg->options[i].n_values;
            save_option(i);
            g_view.opt_changes++;
        }
        return PICKER_NONE;
    }
    if ((pressed & PICKER_BTN_OPTIONS) && g_cfg->n_options) {
        g_view.options = 1;
        g_view.help = 0;
        return PICKER_NONE;
    }
    if (g_view.help) {
        /* help steps through the pages and back to the list; accept goes back to the list (with nothing at all to
           list, it leaves, as FBNeo's picker returns NULL) */
        if (pressed & PICKER_BTN_HELP)
            g_view.help = g_view.help < picker_core_help_pages() ? g_view.help + 1 : (g_count ? 0 : 1);
        else if (pressed & PICKER_BTN_ACCEPT) {
            if (!g_count)
                return PICKER_QUIT;
            g_view.help = 0;
        }
        return PICKER_NONE;
    }
    if (pressed & PICKER_BTN_HELP) {
        g_view.help = 1;
        return PICKER_NONE;
    }
    if (g_count == 0)
        return PICKER_NONE;

    /* up/down: a step on the press, then repeats after 400 ms every 80 ms */
    if (pressed & (PICKER_BTN_UP | PICKER_BTN_DOWN)) {
        move = (pressed & PICKER_BTN_UP) ? -1 : 1;
        g_repeat_at = ms + 400;
    } else if ((b & (PICKER_BTN_UP | PICKER_BTN_DOWN)) && (int)(ms - g_repeat_at) >= 0) {
        move = (b & PICKER_BTN_UP) ? -1 : 1;
        g_repeat_at = ms + 80;
    }
    if (move) {
        g_view.sel = (g_view.sel + move + g_count) % g_count;
        if (g_view.sel < g_view.top)
            g_view.top = g_view.sel;
        if (g_view.sel >= g_view.top + g_view.visible)
            g_view.top = g_view.sel - g_view.visible + 1;
    }
    if (pressed & PICKER_BTN_ACCEPT)
        return PICKER_CHOSE;
    return PICKER_NONE;
}

const picker_view *picker_core_view(void) { return &g_view; }
const picker_entry *picker_core_selected(void) { return picker_core_entry(g_view.sel); }

/* ---- text ---- */

const char *picker_core_header(void)
{
    static char h[160];
    snprintf(h, sizeof(h), "%s - pick a %s", g_cfg->app_name, g_cfg->content_noun);
    return h;
}

int picker_core_help_pages(void) { return g_cfg->controls && g_cfg->controls[0] ? 2 : 1; }

int picker_core_help_lines(const char **lines, int max)
{
    static char l[10][300];
    int n = 0;
    if (g_view.help == 2 && g_cfg->controls) {
        const char *const *c;
        for (c = g_cfg->controls; *c && n < max; c++)
            lines[n++] = *c;
        return n;
    }
    const char *const *e;
    char exts[96] = "";
    for (e = g_cfg->exts; e && *e; e++) {
        size_t len = strlen(exts);
        snprintf(exts + len, sizeof(exts) - len, "%s.%s", len ? " " : "", *e);
    }
    snprintf(l[n++], sizeof(l[0]), "%s - how to add %ss", g_cfg->app_name, g_cfg->content_noun);
    if (g_cfg->warning && *g_cfg->warning)
        snprintf(l[n++], sizeof(l[0]), "%s", g_cfg->warning);
    if (g_view.empty)
        snprintf(l[n++], sizeof(l[0]), "No %ss found.", g_cfg->content_noun);
    snprintf(l[n++], sizeof(l[0]), "Put %s files (%s) in, or in folders inside:", g_cfg->content_noun, exts);
    snprintf(l[n++], sizeof(l[0]), "  %s", g_content);
    snprintf(l[n++], sizeof(l[0]), "(fs:/vol/external01 is the SD card)");
    snprintf(l[n++], sizeof(l[0]), "Another folder: content=<path> in %s", g_config_path);
    if (g_cfg->help_extra && *g_cfg->help_extra)
        snprintf(l[n++], sizeof(l[0]), "%s", g_cfg->help_extra);
    {
        int i;
        for (i = 0; i < n && i < max; i++)
            lines[i] = l[i];
    }
    return n < max ? n : max;
}

int picker_core_hold_to_leave(int combo_held)
{
    static int frames;
    if (!combo_held) {
        frames = 0;
        return 0;
    }
    return ++frames >= 30;
}
