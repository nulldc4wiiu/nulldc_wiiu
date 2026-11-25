# nullDC4WiiU - Dreamcast Emulator for Wii U

<img width="848" height="208" alt="4" src="https://github.com/user-attachments/assets/ed50330c-633b-46ba-b1a0-83d7eb753e6e" />

**A native Wii U (Aroma) port of the nullDC4Wii Sega Dreamcast emulator.**

[![Latest Release](https://img.shields.io/github/v/release/nulldc4wiiu/nulldc_wiiu?label=Release&color=orange)](https://github.com/nulldc4wiiu/nulldc_wiiu/releases)
[![Platform](https://img.shields.io/badge/Platform-Wii%20U%20(Aroma)-009AC7)](README-WiiU.md)
[![Compatibility](https://img.shields.io/badge/Wiki-Compatibility%20List-2EA44F)](https://wiiubrew.org/wiki/NullDC4WiiU)
[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)

[Downloads](https://github.com/nulldc4wiiu/nulldc_wiiu/releases) •
[Compatibility Wiki](https://wiiubrew.org/wiki/NullDC4WiiU) •
[Quick Start](#-quick-start) •
[Controls](#-controls) •
[Documentation](#-documentation) •
[Report an Issue](https://github.com/nulldc4wiiu/nulldc_wiiu/issues)

</div>

---

> [!IMPORTANT]
> **Independent Port Disclaimer**
>
> This is an independent Wii U port and is **not supported by** the `nullDC4Wii` project or its developers at this time.
> - **Do not report bugs in this port to `nullDC4Wii`** (not on their GitHub, Discord, or wiki). Problems here are most likely specific to the Wii U port.
> - Please report all Wii U bugs and regressions in this repository's **[Issues](https://github.com/nulldc4wiiu/nulldc_wiiu/issues)** tab.

---

## Key Features

- **Native GX2 & Multi-Core Architecture:** Hardware-accelerated PowerVR2 rendering on Wii U's GX2 API, SH-4 PowerPC JIT with fastmem, dual-core graphics intake/vertex conversion, and dedicated audio mixing with per-voice AICA low-pass filtering.
- **In-Game HOME Menu:** Pause at any time with `HOME` (or tap the top-right corner of the GamePad) to resume, manage save states, swap discs, adjust quick/per-game settings, remap buttons, or exit cleanly.
- **Save States & Multi-Disc Support:** 4 save state slots per game (with timestamps and *"Undo the last load"*), plus automatic grouping and live disc swapping for multi-disc titles named `... (Disc N) ...`.
- **GamePad VMU Display & Live FPS:** Choose between `Off`, `Corner (bottom-left)`, or `GamePad Dedicated` — displaying the live Dreamcast VMU LCD screen, real-time FPS counter, and a touch `[MENU]` badge on the GamePad while playing on the TV.
- **Light Gun & Rumble:** Automatic light gun support for *The House of the Dead 2*, *Confidential Mission*, and *Death Crimson OX* (Wii Remote IR pointer, GamePad touchscreen gun, or 2-player combinations), plus Puru Puru Pack rumble on GamePad and Pro Controller.
- **Customizable Controls & Fast-Forward:** Per-controller button remapping with turbo (saved globally or per game) and hold-to-fast-forward (`-`).
- **Expanded Display & Timing Options:** `4:3` / `16:9` aspect ratio, `VGA` / `Composite` / `RGB SCART` video cable modes, frameskip, render IRQ timing, and automatic session logging in `benchmarks.txt`.

---

## Compatibility List (WiiUBrew Wiki)

Before playing or reporting a game issue, check the community-maintained compatibility table on **WiiUBrew** for tested retail Dreamcast games, Atomiswave conversions, and recommended settings:

**[WiiUBrew — NullDC4WiiU Compatibility List](https://wiiubrew.org/wiki/NullDC4WiiU)**

Want to help expand the catalog? Every run automatically appends performance stats to `sd:/wiiu/nulldc/benchmarks.txt` so you can easily share accurate FPS results with the community.

---

> [!WARNING]
> **Critical Warnings & Known Limitations**
>
> - **Windows CE games are NOT supported (Hard Freeze):** Titles built on Sega's Windows CE SDK (e.g., *Sega Rally 2*, *South Park: Chef's Luv Shack*, *4x4 Evolution*, *Resident Evil 2*) currently trigger a **full Wii U system freeze requiring a hard reboot**. Avoid launching WinCE titles for now.
> - **Photosensitivity / Flicker:** *Magforce Racing* currently develops severe progressive screen flickering during gameplay.
> - See **[KNOWN-ISSUES-WiiU.md](wiiu/KNOWN-ISSUES-WiiU.md)** for the full list of known bugs.

---

## Quick Start

Downloads are available on the **[Releases](https://github.com/nulldc4wiiu/nulldc_wiiu/releases)** page. **No Dreamcast BIOS or game files are included** — you must provide your own dumps.

1. Download `nulldc.wuhb` from the latest release and place it on your SD card.
2. Copy your Dreamcast BIOS files (`dc_boot.bin` and `dc_flash.bin`) into the `data/` folder.
3. Place your games (`.gdi`, `.cdi`, `.mds`, or `.elf`) inside `sd:/wiiu/nulldc/` (at most one subfolder deep).

## Quick Setup & SD Card Folder Structure

The most common setup mistake is placing `data/` or games inside `sd:/wiiu/apps/nulldc/` instead of `sd:/wiiu/nulldc/`. Make sure your SD card matches this layout:

```text
sd:/
└── wiiu/
    ├── apps/
    │   └── nulldc/
    │       └── nulldc.wuhb              # Emulator binary (Aroma only)
    └── nulldc/
        ├── data/
        │   ├── dc_boot.bin              # Your Dreamcast BIOS (lowercase)
        │   ├── dc_flash.bin             # Your Dreamcast Flash (lowercase)
        │   └── vmu_save_A1.bin          # Created automatically on first boot
        ├── Sonic Adventure/
        │   ├── Sonic Adventure.gdi
        │   ├── track01.bin
        │   └── track02.raw
        └── Marvel vs Capcom 2/
            └── MvC2.cdi

```

> **Golden Rule:** Do not create nested folders like `sd:/wiiu/nulldc/Games/Fighting/MvC2/`. The file selector only searches up to 2 levels deep within `sd:/wiiu/nulldc/`.

---

## 2. Frequently Asked Questions

### BIOS Files

- Make sure `dc_boot.bin` and `dc_flash.bin` are inside **`sd:/wiiu/nulldc/data/`** — **not** inside `sd:/wiiu/apps/nulldc/data/`.
- Filenames must be **all lowercase** (`dc_boot.bin`, `dc_flash.bin`).
- If you are on Windows with file extensions hidden, verify they aren't accidentally named `dc_boot.bin.bin`.
- Since `v0.800-U2.1`, `fsca-table.bin` is bundled inside `nulldc.wuhb` — you do **not** need to supply it manually.

---

### Supported Game Formats & Game Detection

- **Supported formats:** `.gdi`, `.cdi`, `.mds`, and `.elf` (extracted). Compressed archives (`.zip`, `.7z`, `.rar`), `.chd`, and raw `.cue`/`.bin` dumps are **not** supported yet.
- **Lowercase extensions:** Rename uppercase extensions like `.GDI` or `.CDI` to lowercase (`.gdi`, `.cdi`).
- **Folder depth:** The game picker scans up to **2 levels** inside `sd:/wiiu/nulldc/`. Place your game folder directly under `sd:/wiiu/nulldc/`.
- **`.gdi` files:** All `track*.bin` / `track*.raw` files must sit in the exact same folder as the `.gdi` file, and their filenames must match the entries inside the `.gdi` text file.
- **Redump `.cue`/`.bin` sets:** Simply renaming `.cue`/`.bin` files to `.gdi` will fail with a "No disc" error. Use a proper Redump-to-GDI converter or a native `.gdi`/`.cdi` dump.

---

### Controller Configuration (Player 1 / Player 2)

Update to **`v0.800-U2.2`** or newer and enable **`1st pad as Player 2`**:

- You can toggle it permanently in the game picker under **Options**, or live mid-game via **HOME → Quick settings**.
- **`off` (default):** GamePad and the 1st external controller share Player 1 (best for solo play with a Pro/Classic Controller).
- **`on`:** GamePad is exclusively Player 1, and the 1st external controller (Pro Controller, Classic Controller, or Wii Remote) becomes Player 2 (`2nd = P3`, `3rd = P4`).

---

### Multi-Disc Games

- **Live disc swapping (best for `.cdi`):** Put all discs in the **same folder** with identical filenames differing only by ` (Disc N)` — for example:
  - `Shenmue (Disc 1).cdi`
  - `Shenmue (Disc 2).cdi`
  - `Shenmue (Disc 3).cdi`

  When prompted to swap discs, press **HOME → Change disc** and pick the next disc.

- **Multi-disc `.gdi` files:** Only put them in the same folder if their track filenames are unique. If each disc uses overlapping names (`track01.bin`, `track02.raw`), keep each disc in its own folder (`Shenmue 1/`, `Shenmue 2/`) so tracks don't overwrite each other. Save to the VMU at the end of Disc 1, then boot Disc 2 from the picker — VMU saves are shared across discs.

---

### Performance Settings

Try these settings in **Options**:

- **3D resolution → `640x480`:** Uses the Dreamcast's native resolution (half the GPU load of `960x720`), giving a big boost in heavy 3D titles like *Dead or Alive 2* and *Soulcalibur*.
- **Frameskip → `auto`:** Keeps game speed steady during heavy scenes.
- **Disc speed → `instant`:** Greatly shortens initial black screens and loading times in Atomiswave ports (such as *KOF Neowave*). Takes effect on next start.
- **Render IRQ timing → `on (hw-like)`:** Helpful if games like *Marvel vs. Capcom 2* or *CvS2* suffer framerate drops.

---

### Windows CE Games

Titles built on the **Windows CE SDK** are **not supported** and cause a hard console freeze requiring a power cycle.

Always check the [WiiU Compatibility List](https://wiiubrew.org/wiki/NullDC4WiiU) before testing unknown games.

---

### Blue Screen / Crashes in Homebrew Games

That blue screen is usually the homebrew port's own internal Dreamcast SH-4 exception handler (you will notice the VMU overlay is still active).

Modern KallistiOS homebrew ports sometimes conflict with emulator speed hacks.

Try turning **`Fast memory`** (`off`) and **`Faster CPU code`** (`off`) in **Options** for that title; if it still crashes, the port currently relies on behavior not yet supported by `nullDC`.

---

### Installation Requirements

No — `nullDC4WiiU` is distributed as a `.wuhb` package and requires **Aroma**.

---

### Planned Features & Future Improvements

- **`.chd` support, CPU optimizations, and split-screen flicker fixes (`#4`, `#6`):** Top priorities being investigated for the next major core update (`U3`).
- **USB storage (`#5`) and Cheat support:** Tracked as possible future enhancements, with no ETA yet.

---

### Reporting Bugs & Performance Issues

Save states are the fastest way to help us fix bugs and slowdowns:

1. Make sure you are on the latest release.
2. Right at the slowdown or graphical glitch, press **HOME** (or tap the top-right corner of the GamePad) and save a **Save State** to **Slot 1**.
3. Exit cleanly via **HOME → Quit to the Wii U Menu** so `sd:/wiiu/nulldc/benchmarks.txt` is written.
4. Open an issue on [GitHub Issues](https://github.com/nulldc4wiiu/nulldc_wiiu/issues) including the **game title, region, format** (`.gdi`/`.cdi`), any changed options, and attach the save state from `sd:/wiiu/nulldc/` (never upload BIOS files or game images).

## Documentation

README-WiiU.md	- Full Wii U guide: installation, controls, and features

RELEASE-NOTES-WiiU.md	- Changelog and highlights by release

BENCHES-WiiU.md	- Hardware speed benchmarks per game

KNOWN-ISSUES-WiiU.md - Known bugs and per-game issues

SETTINGS-WiiU.md - Reference for every menu and config setting

DESIGN-WiiU.md - Architecture notes, performance work, and roadmap

BUILDING-WiiU.md - Instructions for building from source with devkitPro / Docker

README-Wii.md - Original nullDC4Wii README kept for reference (Wii only, not Wii U)

## Credits & License

Core Wii U Port & Engine: perfcrash26

GamePad VMU, UI Enhancements, Release Management & Wiki: ClaudiWolf2056

Community Hardware Testing & Benchmarks: Zeox gamer, sunspot01, A$AP VENUM, David Hernandez

Original Emulator Base: The nullDC and nullDC4Wii teams (with Xale)
