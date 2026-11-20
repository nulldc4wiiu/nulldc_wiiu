/*
 * picker_core - the logic of nullDC's content picker, without any UI: config, a content scan, the menu's state
 * machine and the help text. The UI (../picker_wiiu.cpp: GX2, a Roboto bitmap font, VPAD and KPAD) draws what it says
 * and feeds it buttons. Plain C, no SDL, no wut UI: it only needs stdio and dirent.
 *
 * Structured like FBNeo's player/picker (picker_cfg, config.txt with content= and autoload=, help/empty screen,
 * hold +/- to quit), but maintained here on its own; it adds what nullDC needed: several extensions, a scan into subfolders (Redump sets sit in
 * a folder of their own) with folders to skip, full paths back (not names to rebuild), extra entries ahead of
 * the scan (nullDC's "BIOS, no disc"), names of any length, case-insensitive order, and key repeat.
 *
 * A host's loop:
 *     picker_core_init(&cfg);                 // reads or creates <data_dir>/config.txt
 *     picker_core_scan();
 *     for (;;) {
 *         int ev = picker_core_input(buttons_now, ms_now);
 *         if (ev == PICKER_CHOSE) { use picker_core_selected()->path; break; }
 *         if (ev == PICKER_QUIT) break;
 *         draw: picker_core_view() for the list (or the help lines when view->help)
 *     }
 */
#ifndef PICKER_CORE_H
#define PICKER_CORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;   /* shown in the list */
    const char *path;   /* passed back on a choice ("" for an entry with no file) */
} picker_entry;

/* A setting on the options page, saved in config.txt as key=value (values[0..n_values-1], def when absent). */
typedef struct {
    const char *key;            /* config.txt key, e.g. "fastmem" */
    const char *label;          /* shown in the list, e.g. "Fast memory" */
    const char *help;           /* one line about it, shown under the list, or NULL */
    const char *const *values;  /* e.g. {"on", "off"} */
    int n_values, def;
} picker_option;

typedef struct {
    const char *app_name;       /* header text, e.g. "nullDC" */
    const char *content_noun;   /* help text, e.g. "disc" (shown as "discs") */
    const char *data_dir;       /* holds config.txt, e.g. "fs:/vol/external01/wiiu/nulldc" (no trailing '/') */
    const char *content_default;   /* content folder when config.txt names none; NULL: <data_dir>/<noun>s */
    const char *const *exts;    /* extensions to list, without the dot, NULL-terminated, e.g. {"gdi", "cdi", NULL} */
    int scan_depth;             /* folders below the content folder to look in (0: only the folder itself) */
    const char *const *skip_dirs;   /* folder names never entered, NULL-terminated, or NULL */
    const picker_entry *extras; /* entries listed first, before the scan, or NULL */
    int n_extras;
    const char *help_extra;     /* optional extra help line (e.g. where the BIOS or cores go), or NULL */
    const char *warning;        /* optional problem to show first (e.g. "BIOS not found"), or NULL: opens help */
    const char *const *controls;   /* optional second help page: the emulator's controls, NULL-terminated */
    const picker_option *options;  /* the options page (PICKER_BTN_OPTIONS), or NULL */
    int n_options;
} picker_cfg;

/* Buttons the host reports each frame, as a set (any controller). */
enum {
    PICKER_BTN_UP = 1, PICKER_BTN_DOWN = 2, PICKER_BTN_ACCEPT = 4, PICKER_BTN_HELP = 8,
    PICKER_BTN_QUIT_COMBO = 16,   /* the UI's "leave" combination is held (nullDC: + and -) */
    PICKER_BTN_OPTIONS = 32,      /* opens and closes the options page */
    PICKER_BTN_BACK = 64,         /* leaves the options page */
};

/* What picker_core_input reports. */
enum { PICKER_NONE, PICKER_CHOSE, PICKER_QUIT };

typedef struct {
    int help;          /* 0: the list; else the help page shown (1: files and folders, 2: controls) */
    int empty;         /* the scan found nothing (the help screen opens; extras can still be chosen) */
    int count, sel, top, visible;   /* the list, the selected row, and the first of `visible` rows shown */
    int options;       /* the options page is shown (help is 0 then) */
    int opt_sel;       /* its selected row */
    int opt_changes;   /* counts option changes, so a UI that redraws on view changes sees them */
} picker_view;

/* Reads <data_dir>/config.txt, or creates it with the defaults; makes sure the content folder exists. */
void picker_core_init(const picker_cfg *cfg);

const char *picker_core_content_dir(void);       /* content=, or the default */
const char *picker_core_autoload(void);          /* autoload=, or "" */
const char *picker_core_config(const char *key); /* any other key=value line of config.txt, or NULL */

/* Lists the extras and the content folder; the selection goes to the first scanned item when there is one.
 * Returns the number of entries. */
int picker_core_scan(void);
const picker_entry *picker_core_entry(int i);   /* 0..count-1 */

/* Rows of the list on screen at once (the host's layout); 15 by default. */
void picker_core_set_visible(int rows);

/* Feed the buttons held now and a millisecond clock; call every frame. Buttons held when the picker starts
 * only count once released (a press left over from the Wii U Menu). */
int picker_core_input(unsigned buttons, unsigned ms);
const picker_view *picker_core_view(void);
const picker_entry *picker_core_selected(void);

/* The lines of the help page being shown (page 1: where files go and how to change the
 * folder; page 2: cfg->controls), for any UI. HELP steps through the pages, ACCEPT goes back to the list. */
int picker_core_help_lines(const char **lines, int max);
int picker_core_help_pages(void);

/* Options: the current value of option i (an index into its values; config.txt or its default). On the options
 * page, up/down choose, ACCEPT steps the value and saves config.txt, OPTIONS or HELP goes back. */
int picker_core_option(int i);
const char *picker_core_option_value(int i);
/* Sets option i to value index v in memory only (config.txt is unchanged): a per-game layer read after config.txt. */
void picker_core_set_option(int i, int v);

#ifdef __cplusplus
}
#endif

#endif /* PICKER_CORE_H */
