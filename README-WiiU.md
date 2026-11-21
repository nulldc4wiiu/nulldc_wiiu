# nullDC (Wii U)

A native Wii U port of **nullDC4Wii** that runs most games at or close to full speed.

nullDC4Wii is the Dreamcast emulator for the Wii (itself a fork of skmp's nullDCe). This port runs as a Wii U app
instead of in vWii mode, so it gets the Wii U's three CPU cores, its memory, and its GPU (GX2) with HD output.

This project only exists because of nullDC4Wii's effort to resurrect the project.

**Version v0.800-U1** (based on nullDC4Wii alpha 0.800)

See: AI / Claude Disclosure below

## What's new in v0.800-U1

The first pass of the Wii U port, focused on making better use of the Wii U's hardware. Not much effort has gone
into optimizing JIT execution (yet).

This release:

- runs as a native Wii U app (Aroma), not in vWii, using all three CPU cores and the GX2 GPU
- a new GX2 renderer: 960x720 or 640x480 3D, fog, modifier-volume shadows, translucent sorting, render to texture
- the SH-4 JIT in the Wii U's code area, with fast memory mapped into the Wii U's address space
- graphics, sound and disc reading on their own cores
- a game picker with help pages and an options page, and a Dreamcast menu entry for memory cards and settings
- up to four controllers (GamePad, Pro Controller, Classic Controller, Wii Remote), and the light gun with the Wii
  Remote's pointer

## Compatibility & speed

Measured on a Wii U console with this release's default settings, over 80-90 s of each game's demo or play
(100% = Dreamcast speed):

| Game | Speed | Notes |
|---|---|---|
| Soulcalibur | 99.7% | |
| Marvel vs. Capcom 2 | 98.4% | |
| Crazy Taxi | 99.5% | |
| Metropolis Street Racer | 100% | |
| Jet Grind Radio | 100% | |
| Shenmue | 100% | sound distorts in the rain and thunder scenes |
| Dead or Alive 2 | 94.5% demo, **87.4% in fights** | not full speed |
| The House of the Dead 2 | 100% | light gun with the Wii Remote |
| AeroWings 2 | 100% | |
| Power Stone 2 | 100% | |
| Spider-Man | 100% | |
| Sonic Adventure | 98.7% | |
| Gunbird 2 | 96.5% | dips for a few seconds while loading |
| Street Fighter III: 3rd Strike | ~99% | |

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
| `config.txt` | the options page's settings; also `content=<folder>` to look for games somewhere else and `autoload=<file>` to start a game without the picker |
| `wiiu.cfg` | optional settings (below) |
| `nullDC.cfg` | the emulator core's own settings file |
| `last_run.txt` | a one-line summary of the last run |

## Settings

**Options page** (X in the picker; saved in `config.txt`). Settings marked "next start" apply the next time nullDC
starts.

| Option (`config.txt` key) | Values (default first) | Effect |
|---|---|---|
| Fast memory (`fastmem`) | on, off | Dreamcast memory mapped straight into the Wii U's; turn it off if a game crashes |
| Light gun (`lightgun`) | off, on | the Wii Remote's pointer is the gun on every port; for light-gun games only |
| 3D resolution (`render`) | 960x720, 640x480 | 640x480 halves the GPU work |
| Sound when slow (`slow_sound`) | keeps pitch, slows down | below full speed: short gaps, or slower and lower sound |
| Graphics on own core (`gfx_core`) | on, off | graphics away from the emulated CPU's core (next start) |
| Sound on own core (`sound_core`) | on, off | sound mixing on another core (next start) |
| Faster CPU code (`jit_fast`) | on, off | speedups in the translated code (next start) |
| Smooth frames (`smooth`) | on, off | two finished frames may wait to be shown; one frame more delay (next start) |
| Reloaded code check (`code_check`) | on, off | catches games that load new code over old (Marvel vs. Capcom 2) (next start) |
| Sound buffer (`sound_buf`) | 100 ms, 46 ms | fewer gaps, or less delay between picture and sound (next start) |
| Disc speed (`disc_speed`) | 4x drive, real drive, instant | how fast the emulated drive reads; 4x has no load pauses (next start) |

`config.txt` also takes `content=<folder>` (where to look for games) and `autoload=<file>` (start that game without
the picker).

**`wiiu.cfg`** (optional, one `key=value` per line; overrides the options page). The settings from `tr_depth` down
are on by default; each turns off with 0, for testing a game that misbehaves:

| Key | Values | Effect |
|---|---|---|
| `file` | path, relative to `sd:/wiiu/nulldc/` | boot this disc or `.elf` without the picker |
| `picker` | 1, 0 | `0`: boot the Dreamcast menu when no `file=` is given |
| `fastmem` | 0, 1 | fast memory off or on |
| `rt_width` | 960, 640 | 3D resolution (4:3) |
| `audio_buffer` | milliseconds | the sound queue's target |
| `sdlog` | 0, 1 | `1`: also write the log to `sd:/wiiu/nulldc/log.txt` |
| `exp_gx2_core` | 0, 1, 2 | graphics thread on the emulator's core (0), core 0 (1) or core 2 (2) |
| `exp_split_convert` | 0, 1 | vertex conversion split over two cores |
| `exp_aica_thread` | 0, 1, 2 | sound mixing on the emulator's core (0), core 0 (1) or core 2 (2) |
| `exp_sq_fast`, `exp_fm_x`, `exp_ps_ftrv` | 0, 1 | the parts of "Faster CPU code": store-queue writes, memory addressing, vector maths |
| `exp_frame_queue` | 0, 1 | Smooth frames |
| `tr_depth` | 2, 1, 0 | auto-sorted translucent polygons drawn with depth >=; 2: each writes depth by its own bit, transparent pixels discarded (Shenmue's faces); 1: no depth writes |
| `ta_keyed` | 1, 0 | a render draws the TA list in the buffer it points at (DOA2's doppelganger scenes) |
| `gd_async` | 1, 0 | a disc read that isn't in the read-ahead cache yet waits in emulated time, not on the SD card |
| `mv_bbox` | 1, 0 | modifier volume passes cover only each volume's screen box |
| `jit_tlive`, `jit_fconst`, `fipr_split` | 1, 0 | translated-code speedups: the T bit kept in a register, float constants from a pool, FIPR in two chains |
| `sq_inline` | 2, 1, 0 | store-queue writes to the TA done by the translated code (2: vertex starts counted there too) |
| `sq_guard` | 2, 1, 0 | store-queue stores tested when compiled, not trapped |
| `sq_ram` | 1, 0 | store-queue writes to RAM done by the translated code |
| `entry_cold` | 1, 0 | a translated block's cycle check falls through to the block |
| `depth_inline`, `ta_dcbz` | 1, 0 | TA decode: depth scale gathered as strips are kept; output cache lines zeroed rather than read |
| `conv_split2` | 1, 0 | the helper core also converts the modifier-volume vertices |
| `conv_dcbz` | 2, 1, 0 | vertex conversion's output lines zeroed rather than read (2: the index list too) |
| `conv_prio` | number | the conversion helper's thread priority (16: below the sound mixer; 0: as created) |
| `g_<preset>` | number | any of nullDC4Wii's presets, e.g. `g_dynarec_preset=0` for the interpreter |

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

- **Dead or Alive 2** fights run at about 87%: the emulated CPU's core is the limit.
- **Sound:** the sound chip's effects processor (DSP) isn't emulated, so there's no reverb or effects. Some scenes
  distort (The House of the Dead 2's intro, Shenmue's rain and thunder), and there's faint background
  scratchiness. Shenmue screeches briefly between its splash screens and the attract.
- **Rendering:** Marvel vs. Capcom 2's intro frame spills past 4:3; a few grey shards at the bottom left of Jet
  Grind Radio's title; a striped band on Metropolis Street Racer's car options screen; translucent modifier
  volumes aren't drawn. Shenmue's hair draws badly. The House of the Dead 2's attract and menu backgrounds flash
  (textures come and go; gameplay is fine). In Power Stone 2, the elevator's green lettering flashes during play.
- **Light gun** shots land slightly to the side of the crosshair.
- **Street Fighter III: 3rd Strike** is noticeably slow in fights (its attract runs at full speed) and shows many
  visual artifacts.
- **Gunbird 2:** the left stick doesn't move the ship; use the D-pad. Speed drops for a few seconds while it
  loads (seen in each test run).
- **HOME** doesn't suspend the game; closing works.

## Future work

The goal is full speed in every game.

1. **Graphics intake on two cores** — decoding the next frame on one core while another converts the current one.
   In tests it made Dead or Alive 2's heavy fights about 13% faster; it needs wider testing before it ships.
2. **Faster translated code** — the emulated CPU's core spends most of its time in translated code, much of it
   waiting on memory; keeping emulated registers in host registers across blocks, and keeping loops inside
   translated code, are the next steps.
3. **Sound** — emulate the DSP, using the spare time on the sound core.

## AI / Claude Disclosure

This Wii U port — the GX2 renderer, the platform layer, the multi-core layout, fast memory, and the
build and profiling tooling — was developed by perfcrash26 working with **Claude (Anthropic's AI coding
assistant, via Claude Code)**. Claude wrote and edited the code, ran the build-and-test cycles, and analyzed the
profiling data; every change was directed, reviewed, and tested on real hardware. The nullDC4Wii emulator is the
work of its developers (see Credits).
