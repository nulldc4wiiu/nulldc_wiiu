# nullDC (Wii U): settings

The options page and `wiiu.cfg`. Installing and controls: [README-WiiU.md](../README-WiiU.md).

**Options page** (X in the picker; saved in `config.txt`). Settings marked "next start" apply the next time nullDC
starts.

| Option (`config.txt` key) | Values (default first) | Effect |
|---|---|---|
| Fast memory (`fastmem`) | on, off | Dreamcast memory mapped straight into the Wii U's; turn it off if a game crashes |
| Force gun on (`lightgun`) | off, on | the light gun on every port, for a gun game that doesn't turn it on by itself; this run only, never saved |
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
| `home_menu`, `save_states`, `fast_forward`, `rumble`, `gun_v2` | 1, 0 | U2's HOME menu, its save state rows, fast-forward, the rumble packs, the touch gun and gun games turning the gun on; each on by default |
| `ffwd` | 2, 3, 0 | fast-forward speed (0: as fast as it goes) |
| `gun_cross` | 1, 0 | the Wii Remote guns' crosshairs |
| `remote_pad` | 1, 0 | `0`: a Wii Remote alone (or with a Nunchuk) is only a gun or pointer, not a controller |
| `remap_gamepad`, `remap_classic`, `turbo`, `stick_dpad_swap` | lists | what the HOME menu's Remap page saves (also per game in `gamecfg/<game>.txt`) |
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
