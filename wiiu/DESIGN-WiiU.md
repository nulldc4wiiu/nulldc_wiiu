# nullDC (Wii U): how it's fast, and what's next

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

## Future work

The goal is full speed in every game.

1. **Graphics intake on two cores** — decoding the next frame on one core while another converts the current one.
   In tests it made Dead or Alive 2's heavy fights about 13% faster; it needs wider testing before it ships.
2. **Faster translated code** — the emulated CPU's core spends most of its time in translated code, much of it
   waiting on memory; keeping emulated registers in host registers across blocks, and keeping loops inside
   translated code, are the next steps.
3. **Sound** — emulate the DSP, using the spare time on the sound core.
