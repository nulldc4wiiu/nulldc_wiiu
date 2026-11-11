# nullDC (Wii U)

A native Wii U port of **nullDC4Wii**, the Dreamcast emulator for the Wii (itself a fork of skmp's nullDCe). It
runs as a Wii U app instead of in vWii mode, so it gets the Wii U's three CPU cores, its memory, and its GPU (GX2)
with HD output.

**Version v0.800-U1** (based on nullDC4Wii alpha 0.800)

See: AI / Claude Disclosure below

## Compatibility & speed

Measured on a Wii U console in real play:

| Game | Speed |
|---|---|
| Marvel vs. Capcom 2 | full speed |
| Soulcalibur | ~99% (about 55 frames shown per second at 960x720) |
| Crazy Taxi | ~99% |
| Metropolis Street Racer | ~100% |
| Jet Grind Radio | plays |
| The House of the Dead 2 | plays, with the light gun |
| Shenmue | plays (sound distorts in the rain and thunder scenes) |
| Dead or Alive 2 | **not full speed**: heavy fights run at about 83% |

Other games may work; these are the ones tested on a console. Discs: `.gdi` (tested), `.cdi` and `.mds`. You need
your own Dreamcast BIOS (`dc_boot.bin` and `dc_flash.bin`); it is not included.

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
| `config.txt` | the options page's settings |
| `wiiu.cfg` | optional settings (below) |
| `last_run.txt` | a one-line summary of the last run |

`wiiu.cfg` settings (one `key=value` per line, all optional): `file=<path>` boots that disc without the picker,
`picker=0` boots the Dreamcast menu when no `file=` is given, `fastmem=0|1` and `rt_width=960|640` override the
options page, `sdlog=1` writes a log to `sd:/wiiu/nulldc/log.txt`, and `g_<preset>=<n>` sets any of nullDC4Wii's
presets. The `exp_*` keys override the options page's speed settings individually.

## Optimizations

What makes the Wii U build fast:

- **Graphics and sound on their own cores.** The emulated SH-4 runs on the core where JIT code may execute; the
  GX2 renderer, vertex conversion and sound mixing run on the other two. Moving graphics off the emulator's core
  was the biggest single gain (Soulcalibur fights from 97% to 131% in a benchmark).
- **SH-4 JIT in the Wii U's code area.** nullDC4Wii's PowerPC JIT runs from the Wii U's code-generation memory,
  with block linking, faster translated code (memory addressing, vector maths, store-queue writes) and a check
  for games that load new code over old.
- **Fast memory.** Dreamcast memory is mapped into the Wii U's address space, so the JIT's loads and stores go
  straight to it; accesses that hit hardware registers fault once and are patched to call the handler.
- **A new GX2 renderer.** The Wii's GX renderer is replaced by one for the Wii U GPU, with precompiled shaders:
  960x720 or 640x480 3D, fog, modifier volumes (shadows), translucent sorting, render-to-texture and interlaced
  2D modes.
- **Disc read-ahead.** Disc images are read in chunks by a background thread, which stopped the sound stutter at
  every load.

## Features

- **Game picker** with on-screen help pages (adding discs, the BIOS, controls)
- **Options page** (X in the picker): fast memory, light gun, 3D resolution, sound when slow, and the speed
  settings (graphics core, sound core, faster CPU code, smooth frames, reloaded code check, sound buffer)
- **Dreamcast menu** entry (first in the picker) for the BIOS menu: memory cards, date, settings
- **Up to four players**: GamePad, Pro Controller, Classic Controller or Wii Remote
- **Light gun** with the Wii Remote's pointer (needs the sensor bar)

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
| Quit | hold + and − | hold + and − for a second, or HOME then Close |

Player 1 is the GamePad or the first controller; players 2–4 use controllers 2–4. **Light gun** (options page):
the Wii Remote's pointer aims, B shoots, A reloads, − is Start; on player 1 the GamePad still gives Start (+), the
D-pad and the trigger (A/ZR). Turn the light gun off for other games. HOME doesn't suspend the game; use it to
close nullDC.

## Building for Wii U

```
docker build -t nulldc-wiiu wiiu
docker run --rm -v "$PWD":/src nulldc-wiiu wiiu/build-wiiu.sh
```

Run both from the repo root. The build files (`Dockerfile`, `build-wiiu.sh`) live in `wiiu/`. Output:
`build-wiiu/nulldc.wuhb`. Install it as described in [Installing](#installing).

The build also writes `wiiu/release/`, the files to publish: the wuhb, `LICENSE-nullDC4Wii.txt`,
`THIRD-PARTY-NOTICES.txt` and `SHA256SUMS`. Both license files are also bundled inside the wuhb.
`wiiu/make-notices.sh` generates them; it fails the build if a license text is missing.

The shaders are checked in compiled (`wiiu/shaders/*.gsh`). After editing a `.vert` or `.frag`, rebuild them with
`wiiu/shaders/build.sh /path/to/glslcompiler` (CafeGLSL).

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

## Known issues

- **Dead or Alive 2** heavy fights run at about 83%: the emulated CPU's core is the limit.
- **Sound:** the sound chip's effects processor (DSP) isn't emulated, so there's no reverb or effects. Some scenes
  distort (The House of the Dead 2's intro, Shenmue's rain and thunder), and there's faint background
  scratchiness.
- **Rendering:** Marvel vs. Capcom 2's intro frame spills past 4:3; a few grey shards at the bottom left of Jet
  Grind Radio's title; a striped band on Metropolis Street Racer's car options screen; translucent modifier
  volumes aren't drawn.
- **Light gun** shots land slightly to the side of the crosshair.
- **Gunbird 2:** the left stick doesn't move the ship; use the D-pad.
- **HOME** doesn't suspend the game; closing works.

## Future work

Profiling on the console shows every game spends a fifth to a half of its translated-code time entering and
leaving small code blocks. Next steps:

1. **Region compiler** — keep loops inside translated code instead of returning to the dispatcher between blocks.
   Estimated 7–10% on Dead or Alive 2, enough to bring its heavy fights near full speed.
2. **GPU instrumentation** — per-pass GPU timing and a timeline across the cores, to decide what can move to the
   GPU (texture and vertex decoding) and what makes the GPU itself faster.
3. **Sound** — emulate the DSP, using the spare time on the sound core.

## AI / Claude Disclosure

The Wii U port in this fork — the GX2 renderer, the platform layer, the multi-core layout, fast memory, and the
build and profiling tooling — was developed by perfcrash26 working with **Claude (Anthropic's AI coding
assistant, via Claude Code)**. Claude wrote and edited the code, ran the build-and-test cycles, and analyzed the
profiling data; every change was directed, reviewed, and tested on real hardware. The nullDC4Wii emulator is the
work of its developers (see Credits).
