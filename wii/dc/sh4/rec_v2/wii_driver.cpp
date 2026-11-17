/*
	wii dynarec
	based on the mips one !

	PPC/Wii calling rules:
	Registers:
		32 32-bit gprs
			r0     volatile, temp
			r1     stack pointer, grows down
			r2     TOC (what ?) Pointer (who cares)
		    r3:10  volatile, first 8 params.r3 is also return value
		    r11    volatile (used as 'environment' pointers for calls by ptr .. what?)
		    r12    volatile (used for ming (mingw ?) & magic as well as linking)
		    r13:31 preserved (19 of em)
		
		32 64-bit fprs (single, vector or double)
		    f0     volatile, scratch
			f1:4   volatile, params, return
			f5:13  volatile, params
			f14:31 preserved (18 of em)
		LR  Link 
		XER (exception register)
		FPSCR
		CR (CR0:1;CR5:7 volatile, CR2:4 preserved)

	When calling 

	Call stack: 
		Return is stored on the link register and its saved at a specific location on function entry
		lr is stored on sp+4
		stack grows towards zero

	Improvements over original:
	  - BUG FIX: GPR restore loop in ngen_mainloop used ppc_r14+i instead of ppc_r13+i,
	             causing r13 to never be restored and r32 (invalid) to be written. Fixed.
	  - BUG FIX: ppc_lip<T> template overload was missing the destination register 'D'
	             parameter — it silently dropped it. Fixed signature to ppc_lip(u32 D, T*).
	  - Use static_cast<> instead of C-style casts for ppc_finvalid/ppc_rinvalid sentinels.
	  - Use snprintf instead of sprintf to guard against buffer overrun in dynarec filename.
	  - Removed redundant fflush(f) before fclose(f) (fclose already flushes).
	  - Fixed BET_StaticCall/BET_StaticJump: added proper braces around debug-log block
	    so indentation reflects actual control flow.
	  - Minor formatting/whitespace consistency improvements; no logic changes.
*/
#include "types.h"
#include <stddef.h>	// offsetof — used for jit_scratch context slot addressing
#include <math.h>	// sqrtf — fsqrt/fsrra native call targets
#include <stdlib.h>	// malloc — JIT_HOTBLOCKS lazily allocates its block table
#include "dc/sh4/sh4_opcode_list.h"
#include "dc/sh4/sh4_interpreter.h"	// sh4_GetTimeslice — preset-latched timeslice

#include "dc/sh4/sh4_registers.h"
#include "dc/sh4/ccn.h"
#include "dc/sh4/rec_v2/ngen.h"
#include "dc/mem/sh4_mem.h"
#include "dc/mem/sh4_internal_reg.h"	// sq_both — fastmem SQ trampoline fast path
#include "dc/sh4/rec_v2/ccall_census.h"	// C call-out census (JIT_CCALLS preset)
#include "wii/wii_fastmem.h"		// PPC-MMU fastmem (FASTMEM preset)
#include "emitter/PPCEmit/ppc_emitter.h"

// wii_driver.cpp defines its own higher-level wrappers for these names.
// Undefine any macros from ppc_emitter.h that would clash with the
// local function definitions below.
#ifdef ppc_li
#  undef ppc_li
#endif
#ifdef ppc_lis
#  undef ppc_lis
#endif
#ifdef ppc_li32
#  undef ppc_li32
#endif
#ifdef ppc_mr
#  undef ppc_mr
#endif
#ifdef ppc_mov
#  undef ppc_mov
#endif
#ifdef ppc_nop
#  undef ppc_nop
#endif
#ifdef ppc_blr
#  undef ppc_blr
#endif
#ifdef ppc_bctr
#  undef ppc_bctr
#endif
#ifdef ppc_bctrl
#  undef ppc_bctrl
#endif
#ifdef ppc_b
#  undef ppc_b
#endif
#ifdef ppc_bl
#  undef ppc_bl
#endif
// These are defined as real functions below; ppc_bx is also a real function
// (overload of the auto-generated one) so guard it too.
#ifdef ppc_bx
#  undef ppc_bx
#endif

// Define "invalid" value for non-mapped-registery
const ppc_freg ppc_finvalid = static_cast<ppc_freg>(-1);  // sentinel: out-of-range (valid regs are 0..31)
const ppc_ireg ppc_rinvalid = static_cast<ppc_ireg>(-1);  // sentinel: out-of-range (valid regs are 0..31)

// This is defined in main.cpp
extern "C" int get_debug_loop();
extern "C" int get_bcache_preset();	// main.cpp: 0=off (default), 1=flat dispatch cache
extern "C" int get_dyn_ic_preset();	// main.cpp: 0=off (default), 1=call+jump, 2=+rts
extern "C" int get_fpu_pin_preset();	// main.cpp: 0=off (default), 1=pin fr[0..15] to f14..f29
extern "C" int g_exp_jit_tlive, g_exp_jit_fconst;	// below: Review 3 K1, K2
extern "C" int g_exp_fipr_split;
extern u32 g_cnt_fipr;
extern "C" int get_jit_align_preset();	// main.cpp: 0=off (default), 1=32-byte-align block entries
extern "C" int get_ifb_flush_preset();	// main.cpp: 0=off (default, full bracket), 1=selective
extern "C" int get_ifb_probe_preset();	// main.cpp: 0=off (default), 1=count ifb sites per opcode

// ppc_li: Loads a 32-bit immediate value into a PowerPC register.
void ppc_li(u32 D,u32 imm)
{
	if (is_s16(imm))
	{
		ppc_addi(D,0,imm);
		return;
	}
	else
	{
		ppc_addis(D,0,imm>>16);
		if ((u16)imm != 0) {
			ppc_ori(D,D,(u16)imm);
		}
	}
}


// The rest of the PPC back end, in this one translation unit (inlining and statics as before): wii_driver_*.inc
#include "wii_driver_emit.inc"
#include "wii_driver_ops.inc"
#include "wii_driver_exit.inc"
#include "wii_driver_regs.inc"
#include "wii_driver_probes.inc"
#include "wii_driver_cold.inc"
#include "wii_driver_fastmem.inc"
#include "wii_driver_compile.inc"
#include "wii_driver_link.inc"
