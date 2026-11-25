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

# Quick Start Guide and Frequently Asked Questions (FAQ) — nullDC4WiiU

## 1. Exact Folder Structure on the SD Card

The most common mistake during installation is confusing the application folder (`apps`) with the data and games folder (`nulldc`). Your SD card must look **exactly** like this:

sd:/
├── wiiu/
│   ├── apps/
│   │   └── nulldc/
│   │       └── nulldc.wuhb              <-- The emulator (for Aroma only)
│   └── nulldc/
│       ├── data/
│       │   ├── dc_boot.bin              <-- Your Dreamcast BIOS (in lowercase)
│       │   ├── dc_flash.bin             <-- Your Dreamcast Flash (in lowercase)
│       │   └── vmu_save_A1.bin          <-- Created automatically during gameplay
│       ├── Sonic Adventure/
│       │   ├── Sonic Adventure.gdi
│       │   ├── track01.bin
│       │   └── track02.raw
│       └── Marvel vs Capcom 2/
│           └── MvC2.cdi

> **Golden Rule:** Do not create nested folders like `sd:/wiiu/nulldc/Games/Fighting/MvC2/`. The file selector only searches up to 2 levels deep within `sd:/wiiu/nulldc/`.

---

## 2. Frequently Asked Questions

### I get a "BIOS NOT FOUND" error, even though I’ve already copied the files
* Verify that they are located in `sd:/wiiu/nulldc/data/` and **not** inside `sd:/wiiu/apps/nulldc/data/`.
* Ensure the filenames are entirely in lowercase: `dc_boot.bin` and `dc_flash.bin` (and check that there are no double file extensions, such as `.bin.bin`). ### The emulator doesn't recognize my game (or shows "no disc")
* **Currently supported formats:** `.gdi`, `.cdi`, `.mds`, and `.elf` (uncompressed). `.chd`, `.zip`, `.7z`, and raw `.cue`/`.bin` files are **not** supported.
* **Lowercase file extensions:** If your file ends in uppercase `.GDI` or `.CDI`, rename it to use lowercase extensions.
* **`.gdi` files:** All `track*.bin` / `.raw` files must be in the same folder as the `.gdi` file, and their filenames must match those listed when opening the `.gdi` file in a text editor (like Notepad).
* If you converted a Redump dump (`.cue`/`.bin`), simply renaming the files is not enough; you must use a dedicated `.gdi` converter or obtain a native `.gdi`/`.cdi` dump.

### How do I play with 2 players? (Both controllers control Player 1)
Starting with version **`v0.800-U2.2`**, enable the option **`1st pad as Player 2: on`**:
* You can enable this in the **Options** menu (from the main menu) or while playing any game via **HOME → Quick settings**.
* **Off (default):** The GamePad and the first external controller both act as Player 1 (ideal for playing solo with a Pro Controller).
* **On:** The GamePad acts as Player 1, and your first external controller (Pro Controller, Classic Controller, or Wii Remote) becomes Player 2.

### How do multi-disc games (like Shenmue or Code Veronica) work?
* **For `.cdi` files:** Place all discs in the same folder and name them identically, changing only the disc identifier—e.g., `(Disc 1)`, `(Disc 2)`, etc. (e.g., `Shenmue (Disc 1).cdi`). When the game prompts you to change the disc, press **HOME → Change disc**.
* **For `.gdi` files:** If different discs contain tracks with identical filenames (e.g., `track01.bin`), place each disc in its own separate folder to prevent them from overwriting one another. When you finish a disc, save your progress to the VMU, then load the next disc via the selector (the VMU is shared across all discs).

### A game runs slowly or takes a long time to load (especially Atomiswave ports)
Try adjusting these settings under **Options**:
* **3D resolution:** Change it from `960x720` to **`640x480`** (the original Dreamcast resolution; this cuts the GPU workload in half).
* **Frameskip:** Set this to **`auto`** for demanding scenes.
* **Disc speed:** If black loading screens take too long (as seen in *KOF Neowave*), set this to **`instant`** (requires restarting the emulator).

### Why does the console freeze with certain games (e.g., *Sega Rally 2*, *Resident Evil 2*, *South Park*)?
These games were built using **Windows CE**, which is currently unsupported and causes the Wii U to freeze. Please check the list on [WiiUBrew](https://wiiubrew.org/wiki/NullDC4WiiU) before attempting to play them.

### Will there be support for `.chd` files, USB, or cheats?
* **`.chd`:** Along with performance improvements and split-screen support, this is a top priority for the next major core update (`U3`).
* **USB and Cheats:** These are noted as potential future enhancements; no release date has been confirmed yet.

### How can I report a bug in a truly helpful way? 1. At the exact moment the graphical glitch or slowdown occurs, press **HOME** and create a **Save State** in Slot 1.
2. Exit via **HOME → Quit to the Wii U Menu** so that `benchmarks.txt` is saved.
3. Open an issue on GitHub, specifying the game, region, and format (`.gdi`/`.cdi`), and attach the Save State from `sd:/wiiu/nulldc/`.

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
