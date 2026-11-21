# nullDC (Wii U): known issues

By release, the latest first. See [README-WiiU.md](README-WiiU.md) for installing, settings and speed.

## v0.800-U2

- **Dead or Alive 2** fights run at about 96% (87% in U1): the emulated CPU's core is still the limit.
- **Street Fighter III: 3rd Strike** is noticeably slow in fights (its attract runs at full speed) and shows many
  visual artifacts.
- **Sound:** the sound chip's effects processor (DSP) isn't emulated, so there's no reverb or effects. Some scenes
  distort (The House of the Dead 2's intro, Shenmue's rain and thunder), and there's faint background
  scratchiness. Shenmue screeches briefly between its splash screens and the attract.
- **Rendering:**
  - Shenmue's hair draws badly.
  - The House of the Dead 2's attract and menu backgrounds flash (textures come and go); gameplay is fine.
  - In Power Stone 2, the elevator's green lettering flashes during play.
  - Marvel vs. Capcom 2's intro frame spills past 4:3; a few grey shards at the bottom left of Jet Grind Radio's
    title; a striped band on Metropolis Street Racer's car options screen; translucent modifier volumes aren't
    drawn.
- **Fast-forward** doesn't hold its 2x or 3x setting: light games run several times faster (Soulcalibur 3 to 5.5
  times at "2x").
- **Light gun** shots land slightly to the side of the crosshair.
- **Gunbird 2:** the left stick doesn't move the ship; use the D-pad.

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
