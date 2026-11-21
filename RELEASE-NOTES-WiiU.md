# nullDC (Wii U): release notes

By release, the latest first. Speed per game: [COMPATIBILITY-WiiU.md](COMPATIBILITY-WiiU.md); known issues: [KNOWN-ISSUES-WiiU.md](KNOWN-ISSUES-WiiU.md).

## v0.800-U2

Faster, with the front-end features U1 lacked. Every game tested runs at full speed except Dead or Alive 2's
fights (96%, up from 87%) and Street Fighter III's fights (see [known issues](KNOWN-ISSUES-WiiU.md)).

- **HOME menu** over the paused game: resume, save states, change disc, quick settings, remap buttons, settings
  for this game or all games, restart, quit to the game list or the Wii U Menu. HOME held opens the Wii U HOME
  Menu; a tap in the GamePad's top right corner opens nullDC's menu too.
- **Save states:** 4 slots per game, with the save time, and "Undo the last load".
- **Multi-disc games** (files named "... (Disc N) ...") are one entry in the game list; change discs from the
  HOME menu.
- **Fast-forward:** hold − (GamePad, Classic or Pro Controller), or latch it on in Quick settings.
- **Rumble:** a Puru Puru Pack in each controller's second slot, on the GamePad's and Pro Controller's motors.
- **Button remapping** per controller kind, with turbo, for one game or all games.
- **Light gun:** House of the Dead 2, Confidential Mission and Death Crimson OX turn the gun on by themselves; the
  GamePad's touch screen is a gun (tap beside the picture to reload), and a Wii Remote and touch can be two guns.
  "Force gun on" (options page) is for another gun game, for that run only.
- **Speed:** faster translated CPU code (store-queue writes, the T bit and float constants in registers, FIPR),
  graphics intake and vertex conversion work spread over two cores, GD-ROM reads that wait in emulated time, and a
  faster 2D framebuffer path (pause screens, logos).
- **Fixes:** Shenmue's faces, Dead or Alive 2's select screens and doppelganger scenes, sound samples at
  unaligned addresses, a per-voice low-pass filter, and sound reads timed against the mixer.

### Experimental

In U2 but not yet tested on a console (they worked in an emulator):

- **Rumble:** the games find the packs, but the motors haven't been felt on a console yet.
- **Button remapping** (HOME menu → Remap buttons).
- **Restart the game** (HOME menu).
- **The GamePad corner tap** that opens the HOME menu.
- **"Force gun on"** turning itself off again after a quit to the game list.
- **Quitting with + and −** without a moment of fast-forward first.

Tested on a console: the HOME menu, save states, Change disc, Quick settings, fast-forward, the light gun and the
speed work.

## v0.800-U1

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
