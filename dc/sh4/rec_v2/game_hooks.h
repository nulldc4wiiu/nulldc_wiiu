#pragma once
#include "types.h"

// Game-specific facts the JIT acts on, kept in this one place (game_hooks.cpp) so that general JIT code holds no game
// addresses. Which mechanism a game uses is chosen per game in apps/discs/game_presets.cfg (e.g. jit_sbp); the facts
// here only say where. Everything is decided when a block is compiled, never on a hot path at run time.

// Addresses (PC & 0xFFFFFF) where a game is known to overwrite its own code: DoCheck guards blocks starting there
// (jit_sbp=1, the default, and 3)
bool game_hook_smc_known(u32 pc);

// MvC2's character-module cache 0CE30000..0CE5FFFF: its modules share head and tail instructions, so jit_sbp=3
// guards every block there and compares every instruction (ngen_Begin)
bool game_hook_module_cache(u32 pc);
