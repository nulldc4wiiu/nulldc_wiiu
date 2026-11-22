<div align="center">
# nullDC (Wii U)

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

## 🚀 Quick Start

Downloads are available on the **[Releases](https://github.com/nulldc4wiiu/nulldc_wiiu/releases)** page. **No Dreamcast BIOS or game files are included** — you must provide your own dumps.

1. Download `nulldc.wuhb` from the latest release and place it on your SD card.
2. Copy your Dreamcast BIOS files (`dc_boot.bin` and `dc_flash.bin`) into the `data/` folder.
3. Place your games (`.gdi`, `.cdi`, `.mds`, or `.elf`) inside `sd:/wiiu/nulldc/` (at most one subfolder deep).

```text
sd:/
└── wiiu/
    ├── apps/
    │   └── nulldc/
    │       └── nulldc.wuhb
    └── nulldc/
        ├── data/
        │   ├── dc_boot.bin        (required)
        │   ├── dc_flash.bin       (required)
        │   └── vmu_save_*.bin     (created automatically)
        ├── Crazy Taxi/
        │   └── Crazy Taxi.gdi
        └── Skies of Arcadia/
            ├── Skies of Arcadia (Disc 1).gdi
            └── Skies of Arcadia (Disc 2).gdi
