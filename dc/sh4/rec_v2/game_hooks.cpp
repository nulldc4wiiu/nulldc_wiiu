// Game-specific facts for the JIT (see game_hooks.h). Add a game here, not in the general JIT code.
#include "game_hooks.h"

bool game_hook_smc_known(u32 pc)
{
	switch (pc & 0xFFFFFF)
	{
	case 0x3DAFC6:   // Dead or Alive 2 (LE)
	case 0x3C83F8:   // Dead or Alive 2 (LE)
	case 0x348000:   // Shenmue II
	case 0x41860E:   // Shenmue
		return true;
	default:
		return false;
	}
}

bool game_hook_module_cache(u32 pc)
{
	return (pc & 0xFFFFFF) >= 0xE30000 && (pc & 0xFFFFFF) < 0xE60000;   // Marvel vs. Capcom 2
}
