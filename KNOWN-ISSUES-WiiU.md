# nullDC (Wii U): known issues

By release, the latest first. See [README-WiiU.md](README-WiiU.md) for installing and settings, and [COMPATIBILITY-WiiU.md](COMPATIBILITY-WiiU.md) for speed.

## v0.800-U2

- **Dead or Alive 2** fights run at about 96% (87% in U1): the emulated CPU's core is still the limit.
- **Street Fighter III: 3rd Strike** is noticeably slow in fights (its attract runs at full speed) and shows many
  visual artifacts.
- **Sound:** the sound chip's effects processor (DSP) isn't emulated, so there's no reverb or effects. Some scenes
  distort (The House of the Dead 2's intro, Shenmue's rain and thunder), and there's faint background
  scratchiness. Shenmue screeches briefly between its splash screens and the attract. Marvel vs. Capcom 2's
  startup logo sound is distorted while the "MVC" and "2" fall into place. In many games the SEGA and game logo
  screens are silent.
- **Rendering:**
  - Shenmue's hair draws badly, and Ryo's brow or chin flashes dark for a frame as he turns.
  - The House of the Dead 2's attract and menu backgrounds flash (textures come and go); gameplay is fine.
  - In Power Stone 2, the elevator's green lettering flashes during play.
  - Dead or Alive 2: in the doppelganger intro (after losing the first match), polygons of the fighter and then the
    walls and floor blink out one by one as the camera zooms out; a white box flickers on the character select.
  - Marvel vs. Capcom 2's intro frame spills past 4:3; a few grey shards at the bottom left of Jet Grind Radio's
    title; a striped band on Metropolis Street Racer's car options screen; translucent modifier volumes aren't
    drawn.
- **Fast-forward** doesn't hold its 2x or 3x setting: light games run several times faster (Soulcalibur 3 to 5.5
  times at "2x").
- **Light gun** shots land slightly to the side of the crosshair. Gun games are recognised by the disc's file
  name, so a renamed disc needs "Force gun on".
- **Rumble** plays on the GamePad and Pro Controller only, not on Wii Remotes.
- **Save states** don't include the memory cards (VMUs), which keep their contents as a real card would, and load
  only in the same nullDC build with the same controllers.
- **Change disc** during a game's boot, rather than at its own disc prompt, sends the Dreamcast to its BIOS menu,
  as on a real Dreamcast.
- **Gunbird 2:** the left stick doesn't move the ship; use the D-pad.
- **Not yet tried on a console:** button remapping, Restart the game, the GamePad corner tap, how the rumble feels,
  "Force gun on" resetting after a quit to the game list, and the + and − quit without a fast-forward burst. Save
  states, Change disc, Quick settings, fast-forward, the HOME menu and the light gun were tested on a console.

Fixed since U1: HOME now opens nullDC's menu over the paused game; Gunbird 2's speed drops while loading weren't
seen in U2's test run.

## v0.800-U1

- **Dead or Alive 2** fights run at about 87%: the emulated CPU's core is the limit.
- **Sound:** the sound chip's effects processor (DSP) isn't emulated, so there's no reverb or effects. Some scenes
  distort (The House of the Dead 2's intro, Shenmue's rain and thunder), and there's faint background
  scratchiness.
- **Rendering:** Marvel vs. Capcom 2's intro frame spills past 4:3; a few grey shards at the bottom left of Jet
  Grind Radio's title; a striped band on Metropolis Street Racer's car options screen; translucent modifier
  volumes aren't drawn.
- **Light gun** shots land slightly to the side of the crosshair.
- **Gunbird 2:** the left stick doesn't move the ship; use the D-pad. Speed drops for a few seconds while it
  loads (seen in each test run).
- **HOME** doesn't suspend the game; closing works.
