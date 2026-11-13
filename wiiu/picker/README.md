# picker_core — a content picker's logic, without UI

`picker_core.c` / `picker_core.h` are the logic of nullDC's game picker with no UI at all: plain C, only
stdio and dirent. `../picker_wiiu.cpp` is the UI: it draws what the core says with GX2 and a Roboto bitmap font, and
feeds it buttons from VPAD and KPAD.

It's structured like FBNeo's `player/picker` (a `picker_cfg`, `config.txt` with `content=` and `autoload=`, a
help / empty-folder screen, hold + and − to quit), but it's nullDC's own copy, maintained here; nothing is
shared with FBNeo. What it has beyond FBNeo's:

| Feature | FBNeo picker | picker_core |
|---|---|---|
| File types | one `ext` | a NULL-terminated `exts` list |
| Where | the content folder only | `scan_depth` folders down, `skip_dirs` never entered |
| What comes back | the name (host rebuilds the path) | the full `path` |
| Entries not on disk | — | `extras`, listed first (nullDC: "Dreamcast menu (no disc): memory cards, settings", path "") |
| Long names | over 47 characters skipped | any length (the UI truncates) |
| Order | `strcmp` | case-insensitive |
| Up/down | fixed 140 ms steps | a step, then repeats after 400 ms every 80 ms |
| Other config keys | fixed keys | `picker_core_config(key)` for any `key=value` |

## The UI's side

```c
picker_core_init(&cfg);            /* reads or creates <data_dir>/config.txt, makes the content folder */
if (*picker_core_autoload()) ...   /* boot straight into that */
picker_core_scan();
picker_core_set_visible(rows);     /* how many list rows the UI shows */
for (;;) {
    int ev = picker_core_input(buttons, ms);   /* PICKER_BTN_* held now, a millisecond clock */
    if (ev == PICKER_CHOSE) { picker_core_selected()->path; break; }
    if (ev == PICKER_QUIT) break;
    const picker_view *v = picker_core_view();
    if (v->help) draw picker_core_help_lines();
    else draw rows v->top .. v->top + v->visible - 1 (picker_core_entry(i)->name),
         highlight v->sel, and "sel+1 / count"
}
```

- Buttons are a set over all controllers: `UP`, `DOWN`, `ACCEPT`, `HELP`, `OPTIONS` (opens and closes the options
  page), `BACK` (leaves it), and `QUIT_COMBO`, which means the host's "leave" combination is held: + and − on any
  pad. The core needs it held 0.5 s, and released once after the picker opens.
- Buttons held when the picker opens only count once released, so a press left over from the Wii U Menu doesn't
  choose anything.

## The font

`../font_roboto.h` is Roboto (SIL Open Font License 1.1, `../Roboto-Regular.ttf`) rasterized by `../make_font.py`. The
generated header is checked in, so the build needs no TTF engine; to change the size, rerun make_font.py with the new
size.
