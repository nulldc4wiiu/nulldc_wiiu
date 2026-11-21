# nullDC (Wii U)

A native Wii U port of **nullDC4Wii** that runs most games at or close to full speed.

nullDC4Wii is the Dreamcast emulator for the Wii (itself a fork of skmp's nullDCe). This port runs as a Wii U app
instead of in vWii mode, so it gets the Wii U's three CPU cores, its memory, and its GPU (GX2) with HD output.

This project only exists because of nullDC4Wii's effort to resurrect the project.

**Version v0.800-U2** (based on nullDC4Wii alpha 0.800)

See: AI / Claude Disclosure below

## What's new in v0.800-U2

Faster, with the front-end features U1 lacked. Every game tested runs at full speed except Dead or Alive 2's
fights (96%, up from 87%) and Street Fighter III's fights (see [known issues](KNOWN-ISSUES-WiiU.md)).

The full list, and U1's: [RELEASE-NOTES-WiiU.md](RELEASE-NOTES-WiiU.md).

## Compatibility & speed

All 14 games tested on a console run at full speed in U2, except Dead or Alive 2's fights (96%) and Street Fighter
III: 3rd Strike's fights (noticeably slow). The per-game tables, for each release, are in
[COMPATIBILITY-WiiU.md](COMPATIBILITY-WiiU.md).

Discs: `.gdi` (tested), `.cdi` and `.mds`. You need your own Dreamcast BIOS (`dc_boot.bin` and `dc_flash.bin`); it
is not included.

## Installing

Requires a Wii U running the Aroma homebrew environment.

1. Copy `nulldc.wuhb` to `sd:/wiiu/apps/nulldc/`.
2. Put the BIOS files `dc_boot.bin` and `dc_flash.bin` in `sd:/wiiu/nulldc/data/`.
3. Put each game in a folder of its own under `sd:/wiiu/nulldc/`, for example
   `sd:/wiiu/nulldc/soulcalibur/Soulcalibur (USA).gdi` with its track files beside it.
4. Launch **nullDC** from the Wii U Menu and pick a game.

Files under `sd:/wiiu/nulldc/`:

| Path | Contents |
|---|---|
| `data/dc_boot.bin`, `data/dc_flash.bin` | the Dreamcast BIOS and its flash (you provide them) |
| `data/vmu_save_A1.bin` ... `D2.bin` | memory cards, one per controller port and slot; created when first used |
| `<game>/` | one folder per game |
| `config.txt` | the options page's settings; also `content=<folder>` to look for games somewhere else and `autoload=<file>` to start a game without the picker |
| `wiiu.cfg` | optional settings ([SETTINGS-WiiU.md](SETTINGS-WiiU.md)) |
| `nullDC.cfg` | the emulator core's own settings file |
| `last_run.txt` | a one-line summary of the last run |

## Settings

X in the game list opens the options page; most settings are there, and the HOME menu's settings pages set them
for one game or all. Every option and `wiiu.cfg` key: [SETTINGS-WiiU.md](SETTINGS-WiiU.md).

## Features

- **Game picker** with on-screen help pages (adding discs, the BIOS, controls); multi-disc games as one entry
- **Options page** (X in the picker): fast memory, Force gun on, 3D resolution, sound when slow, and the speed
  settings (graphics core, sound core, faster CPU code, smooth frames, reloaded code check, sound buffer, disc
  speed)
- **HOME menu** in game: save states, change disc, quick settings, remap buttons (experimental), settings,
  restart (experimental), quit
- **Fast-forward** (− held) and **rumble** (experimental); experimental means not yet tested on a console, see
  the [release notes](RELEASE-NOTES-WiiU.md#experimental)
- **Dreamcast menu** entry (first in the picker) for the BIOS menu: memory cards, date, settings
- **Up to four players**: GamePad, Pro Controller, Classic Controller or Wii Remote
- **Light gun** with the Wii Remote's pointer (needs the sensor bar) or the GamePad's touch screen

## Controls

| Action | Picker | In game |
|---|---|---|
| Move | D-pad or left stick | D-pad, left stick = Dreamcast D-pad, stick |
| Start a game | A | — |
| Help | Y | — |
| Options | X (B goes back) | — |
| Dreamcast A B X Y | — | B A Y X (GamePad, Pro, Classic) · A B 2 1 (Wii Remote alone) |
| Start | — | + (Wii Remote alone: HOME) |
| Triggers | — | L/ZL, R/ZR (Wii Remote alone: − and +) |
| HOME menu | — | HOME (held: the Wii U HOME Menu), or a tap in the GamePad's top right corner |
| Fast-forward | — | hold − (GamePad, Classic, Pro) |
| Quit | hold + and − | hold + and − for a second, or HOME then Quit to the Wii U Menu |

Player 1 is the GamePad or the first controller; players 2–4 use controllers 2–4. **Light gun** (gun games turn it
on by themselves; "Force gun on" on the options page for others): the Wii Remote's pointer aims, B shoots, A
reloads, − is Start; a tap on the GamePad's picture shoots there, and a tap beside it reloads. On player 1 the
GamePad still gives Start (+), the D-pad and the trigger (A/ZR).

## Building for Wii U

See [BUILDING-WiiU.md](BUILDING-WiiU.md).

## Credits

- **nullDC** — skmp (Stefanos Kornilios Mitsis Poiitidis), the original nullDC creator, and the nullDC
  contributors.
- **nullDC4Wii**, the Wii port this one is built on:
  - Xale00 (Benoit Adam), lead developer, who revived nullDC on the Wii in 2026;
  - skmp, who also works on nullDC4Wii itself (SH-4 JIT and memory fast paths, the ARM7, audio);
  - GatoWare (packaging fixes) and Jilou04 (testing on the Wii U);
  - Joseph Jordan (libiso), and nullDC4Wii's testers and the people helping on its Discord and the WiiBrew wiki.
  - Without the nullDC4Wii team's effort to resurrect the Wii version, this release would not exist.
- **Other nullDC ports** — gligli (Xbox 360), Xeihro (PSP).
- **Flycast** (flyinghead) and **Reicast** — used for knowledge of the Dreamcast hardware; the Wii U port's code
  is our own, written into nullDC.
- **wut** (Wii U toolchain & library) — the devkitPro / wut team.
- **CafeGLSL** — the shader compiler used to build the GX2 shaders.
- **Roboto font** — The Roboto Project Authors; see `THIRD-PARTY-NOTICES.txt`.
- **Wii U port & optimizations** — perfcrash26, with Claude (see below).
- **Release, support & testing** — ClaudiWolf2056: owns the release, general support, onboarding and testing.

## Known issues

See [KNOWN-ISSUES-WiiU.md](KNOWN-ISSUES-WiiU.md), by release, the latest first.

## More

- [RELEASE-NOTES-WiiU.md](RELEASE-NOTES-WiiU.md): what's new, by release
- [COMPATIBILITY-WiiU.md](COMPATIBILITY-WiiU.md): speed per game, by release
- [KNOWN-ISSUES-WiiU.md](KNOWN-ISSUES-WiiU.md): known issues, by release
- [SETTINGS-WiiU.md](SETTINGS-WiiU.md): every setting
- [DESIGN-WiiU.md](DESIGN-WiiU.md): what makes the Wii U build fast, and what's next
- [BUILDING-WiiU.md](BUILDING-WiiU.md): building from source

## AI / Claude Disclosure

This Wii U port — the GX2 renderer, the platform layer, the multi-core layout, fast memory, and the
build and profiling tooling — was developed by perfcrash26 working with **Claude (Anthropic's AI coding
assistant, via Claude Code)**. Claude wrote and edited the code, ran the build-and-test cycles, and analyzed the
profiling data; every change was directed, reviewed, and tested on real hardware. The nullDC4Wii emulator is the
work of its developers (see Credits).
