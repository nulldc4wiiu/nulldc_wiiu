// wii_driver_emit.inc: jump offsets, block begin, memory access helpers.
// Part of wii_driver.cpp, which #includes it: not compiled on its own.

// =======================
// JUMP OFFSET CALCULATION
// =======================

#if defined(__WIIU__)
#include "wiiu_codegen.h"
#endif
snat ppc_jdiff_raw(void* dst)
{
	return (u8*)dst-(u8*)emit_GetCCPtr();
}
snat ppc_jdiff(void* dst)
{
	return ppc_jdiff_raw(dst)>>2;
}

void ppc_bx(void* dst,u32 LK)
{
	snat offs = ppc_jdiff_raw(dst);
#if defined(__WIIU__)
	// on the Wii U, system library code can be out of reach of the codegen area: go through a veneer
	if (!(offs<33554432 && offs>-33554432))
	{
		void* veneer = wiiu_codegen_veneer(dst);
		if (veneer)
			offs = ppc_jdiff_raw(veneer);
	}
#endif
	//offs must fit in 24 bits
	verify(offs<33554432 && offs>-33554432);
	offs>>=2;
	// does this work ? //
	ppc_bx(offs,0,LK);
}

#if defined(__WIIU__)
// exp_fm_x (on with the "faster code" option): the fastmem window base lives in r12, so scalar accesses are rlwinm +
// an indexed load/store. r12 is volatile and nothing in the JIT uses it, so it is reloaded after every call into C.
// Not after calls to the JIT's own stubs: their sites have a fixed layout, and the stubs reach C through ppc_call.
extern "C" { int g_exp_fm_x; }
// exp_ps_ftrv (on with the "faster code" option): FTRV with paired singles, in single precision (shop_ftrv below)
extern "C" { int g_exp_ps_ftrv; }
extern u32 g_wiiu_fm_window;
extern u8* CodeCache;
#endif
void ppc_call(void* funct)
{
	ppc_bx(funct,1);
#if defined(__WIIU__)
	if (g_exp_fm_x && g_wii_fastmem_active && !((u8*)funct>=CodeCache && (u8*)funct<CodeCache+CODE_SIZE))
		ppc_addis(ppc_r12,ppc_r0,g_wiiu_fm_window>>16);	// lis r12,window
#endif
}
template<typename T> void ppc_call(T* dst) { return ppc_call((void*)dst); }

void ppc_jump(void* funct)
{
	ppc_bx(funct,0);
}
template<typename T> void ppc_jump(T* dst) { return ppc_jump((void*)dst); }
void ppc_call_and_jump(void* funct)
{
	ppc_call(funct);
	ppc_mtctr(ppc_r3);
	ppc_bcctrx(BO_ALWAYS,BI_CR0_EQ,0);  // bctr
}
template<typename T> void ppc_call_and_jump(T* dst) { return ppc_call_and_jump((void*)dst); }
void make_address_range_executable(void* addr, u32 size)
{
#if defined(__WIIU__)
	wiiu_codegen_executable(addr, size);
	return;
#endif
	//what gives?
	DCFlushRange(addr, size);
	ICInvalidateRange(addr, size);
}

void ppc_lip(u32 D,void* ptr)
{
	ppc_li(D,(u8*)ptr-(u8*)0);
}
template<typename T> void ppc_lip(u32 D, T* ptr) { return ppc_lip(D, (void*)ptr); }

ppc_ireg ppc_cycles = ppc_r29;
ppc_ireg ppc_contex = ppc_r30;
ppc_ireg ppc_djump = ppc_r31;
ppc_ireg ppc_next_pc = ppc_rarg0;

void ppc_emit(u32 insn)
{
	emit_Write32(insn);
}

void* loop_no_update;
void* ngen_LinkBlock_Static_stub;
void* ngen_LinkBlock_Dynamic_1st_stub;
void* ngen_LinkBlock_Dynamic_2nd_stub;
void* ngen_LinkBlock_Dynamic_IC_stub;	// DYN_IC preset: inline-cache fill
void* ngen_BlockCheckFail_stub;
void* loop_do_update_write;
void (*loop_code)() ;
void (*ngen_FailedToFindBlock)();

// Defined below — emits the addis half of an absolute address into rD and
// returns the low 16-bit displacement. Forward-declared so the block-check
// guard in ngen_Begin() can use it.
u32 ppc_addr_high(u32 rD,void* ptr);

struct
{
	bool has_jcond;

	// T-FORWARDING. The SH4's commonest shape by far is a flag producer
	// (cmp/tst/dt) immediately followed by a consumer (bt/bf, or the shop_jcond
	// of a delay-slot branch). sr_T is not pinned — GetIntReg only covers
	// r0..r15 minus r11 — so the producer stored T to the context and the
	// consumer loaded it straight back:
	//     stw r3,0x118(r30)      <- producer (value still in rarg0!)
	//     lwz r3,0x118(r30)      <- consumer, same address, same register
	// which is a load-hit-store stall on Broadway to recover a value that never
	// left the register. Set true by binop_end when it has just written sr_T,
	// and cleared before EVERY shil op, so it can only ever describe the
	// immediately preceding one — no cross-op liveness reasoning needed, and no
	// way for an intervening op to have clobbered rarg0 behind our back.
	// The store stays: sr_T is architectural state a later block may read.
	bool t_in_rarg0;

	// JIT_CR0. Set before each shil op: true only when this op is the block's
	// LAST, the block exits on sr_T (BET_Cond_0/1), and this op is what writes
	// sr_T. A T producer seeing it leaves its condition in CR0 and emits
	// nothing, and ngen_End branches on that CR0 bit directly.
	//
	// "Last op" is the whole safety argument: the only code that can run
	// between the compare and the branch is ngen_End's own, so no emitter can
	// clobber CR0 behind our back and there is no liveness to track. A version
	// that let the condition survive intervening ops would need every CR0
	// writer in this file to invalidate it, and missing one is a silent
	// wrong-branch — deliberately not attempted here.
	bool t_exit_cr0;     // this op may hand its condition to ngen_End
	bool cr0_live;       // it did; cr0_bit/cr0_invert describe it
	u32  cr0_bit;        // BI_CR0_xx index holding the condition
	bool cr0_invert;     // sr_T is the INVERSE of that bit (e.g. >= is !<)

	// jit_tlive (Review 3 K1): blocks ending in a delay-slot branch capture T in shop_jcond, several ops after
	// the compare that produced it (dt; mov.l; bt.s), so T-FORWARDING above can't help and the jcond reloads the T
	// the producer stored a few instructions earlier: a load-hit-store, 28% of DOA2's hottest block. ppc_djump (r31)
	// is free until that jcond (the block's own jcond, jdyn and exit write it; C calls preserve it), so every T store before the
	// jcond also copies the value there, and the jcond finds it in place. t_copy_ok: a jcond follows this op;
	// t_in_djump: r31 holds the current T. Every op that writes T or SR clears it; ppc_sh_store of T sets it again.
	bool t_copy_ok;
	bool t_in_djump;

	void Reset()
	{
		has_jcond=false;
		t_copy_ok=false;
		t_in_djump=false;
		t_in_rarg0=false;
		t_exit_cr0=false;
		cr0_live=false;
		cr0_bit=0;
		cr0_invert=false;
	}
} compile_state;
u32 last_block;

// JIT_TFWD preset (main.cpp): forward sr_T from rarg0 to the branch that
// consumes it instead of storing then immediately reloading it. See the
// t_in_rarg0 comment in compile_state.
extern "C" int get_jit_tfwd_preset();

// JIT_FSQRT preset (main.cpp): inline fsqrt as frsqrte + Newton-Raphson
// instead of calling libm. See the comment block above rec_fsqrt.
extern "C" int get_jit_fsqrt_preset();

// JIT_FIPR preset (main.cpp): accumulate fipr/ftrv in double and round to
// single once, as the SH4 does, instead of rounding after every fmadds.
extern "C" int get_jit_fipr_preset();

// JIT_CR0 preset (main.cpp): let a block-ending compare leave its answer in
// CR0 and have the conditional exit branch on it directly, instead of moving
// the bit into a GPR only to compare it back to 0/1. See emit_t_to_cr0().
extern "C" int get_jit_cr0_preset();

// JIT_LINK_GUARD preset (main.cpp): patch a static exit only if the site is
// still the one that asked for the link. See ngen_LinkBlock_Static().
extern "C" int get_jit_link_guard_preset();

// DIV32_FIX preset (main.cpp): the matched divide is 64/32 one's complement;
// shop_div32u/s/p2 call their canonical bodies. See case shop_div32u.
extern "C" int get_div32_fix_preset();

// JIT_FSCHG preset (main.cpp): `fschg` toggles FPSCR.SZ only, so UpdateFPSCR()
// has nothing to do but keep old_fpscr in step. Lets shop_sync_fpscr skip the
// C call and the pinned-FPU spill/reload that brackets it. See shop_sync_fpscr.
extern "C" int get_jit_fschg_preset();

// JIT_FMOV preset (main.cpp): extend FPU_PIN "Phase B" (direct lfs/stfs to a
// pinned FR, no GPR bounce) to the FASTMEM shapes in shop_readm/shop_writem.
// Needs the matching float shapes in rec_fastmem_patch() below, which is why
// it is a separate switch from fpu_pin rather than part of it.
extern "C" int get_jit_fmov_preset();

// Forward decls: GPR/FPR allocation maps + flush/reload (defined later in this file).
ppc_ireg GetIntReg(u32 reg);
ppc_freg GetFloatReg(u32 reg);
void reg_flush_all();
void reg_reload_all();
void reg_flush_all_fpu();
void reg_reload_all_fpu();

// =======================
// BLOCK BEGIN/END
// =======================

extern "C" int get_jit_sbp_preset();
void ngen_Begin(DecodedBlock* block,bool force_checks)
{
	compile_state.Reset();

	// ---------------------------------------------------------------------
	// Block-check guard (self-modifying code protection)
	//
	// force_checks comes from DoCheck() in driver.cpp, which is gated on the
	// JIT_SBP preset. When set, re-read this block's SH4 source at entry and
	// compare it against what was there at compile time. A mismatch means the
	// game overwrote its own code, so the translation is stale — bail to
	// rdv_BlockCheckFail, which clears the cache (dropping the statically
	// patched links along with it) and recompiles from live memory.
	//
	// WHAT IS COMPARED, AND WHY IT IS NO LONGER ONE BYTE
	//
	// This used to load a single BYTE. That is far too weak for the case the
	// guard exists to catch. Marvel vs Capcom 2 loads code overlays into a
	// six-slot module cache at 0CE30000..0CE58000 and jumps into them; the next
	// stage reloads DIFFERENT modules to the SAME six addresses. Every block
	// compiled from stage 1's module is then stale, and this compare is the
	// only thing between the game and executing stage 1's translation of stage
	// 2's code.
	//
	// A block starts at a function entry far more often than not, and SH4
	// prologues are stereotyped — 2FE6 / 2FD6 / 2F32 (mov.l Rm,@-R15) and 4F22
	// (sts.l pr,@-R15) dominate. Two unrelated modules agree on the first BYTE
	// of a block entry very often, so the guard passed and the stale block ran.
	// The byte compare was close to useless exactly where it mattered.
	//
	// Now: the whole first INSTRUCTION (16 bits, 256x stronger) and the whole
	// LAST instruction of the block. Both are halfword loads against a 16-bit
	// immediate, so the cost over the old version is one lhz + cmpli + branch —
	// no 32-bit constant to materialise. Checking the tail as well as the head
	// is what catches a replacement sharing a prologue but diverging later,
	// which is the normal case for two modules of the same engine.
	//
	// Emitted BEFORE the cycle decrement so a stale block never bills cycles.
	// rarg0/rarg1 are scratch at block entry (rarg0 is next_pc, which the
	// cycle-underflow path below overwrites anyway), so both are free here.
	// ---------------------------------------------------------------------
	if (force_checks && ngen_BlockCheckFail_stub)
	{
		const u32 code_sz = block->sh4_code_size ? block->sh4_code_size : 2;
		u8* ptr = GetMemPtr(block->start, code_sz);

		// jit_sbp=3 (MvC2's module cache: its modules share head and tail
		// instructions, see DoCheck): every instruction. RAM holds words natively, so an aligned
		// word is two instructions; a lone halfword at either end sits at addr^2.
		if (ptr && get_jit_sbp_preset()==3 && code_sz<=4096)
		{
			static ppc_label* fails[2048+2];
			u32 n=0;
			u32 a=(u32)(uintptr_t)ptr;
			const u32 end=a+code_sz;
			ppc_li(ppc_rarg0,a&~3u);
			const u32 base=a&~3u;
			for (; a<end; )
			{
				if ((a&2) || end-a<4)
				{
					const u32 w=a^2;
					ppc_lhz(ppc_rarg1,ppc_rarg0,w-base);
					ppc_cmpli(ppc_cr0,ppc_rarg1,*(u16*)w,0);
					a+=2;
				}
				else
				{
					const u32 v=*(u32*)a;
					ppc_lwz(ppc_rarg1,ppc_rarg0,a-base);
					ppc_xoris(ppc_rarg1,ppc_rarg1,v>>16);
					ppc_cmpli(ppc_cr0,ppc_rarg1,v&0xFFFF,0);
					a+=4;
				}
				fails[n++]=ppc_CreateLabel();
				ppc_bcx(BO_FALSE,BI_CR0_EQ,0,0,0);	// bne -> fail
			}
			ppc_label* all_ok=ppc_CreateLabel();
			ppc_bcx(BO_ALWAYS,BI_CR0_LT,0,0,0);		// b all_ok
			for (u32 i=0;i<n;i++)
				fails[i]->MarkLabel();
			ppc_li(ppc_rarg0,block->start);
			ppc_jump(ngen_BlockCheckFail_stub);
			all_ok->MarkLabel();
		}
		// DoCheck already probed this, but the mapping is the thing that makes
		// the compare safe — never emit a load we cannot justify.
		else if (ptr)
		{
			// Only worth a second compare when the block is longer than one
			// instruction; otherwise the tail IS the head.
			const bool check_tail = (code_sz >= 4);
			ppc_label* fail = 0;

			// --- first instruction --------------------------------------------
			{
				const u16 first = *(u16*)ptr;
				u32 lo = ppc_addr_high(ppc_rarg1,(void*)ptr);
				ppc_lhz(ppc_rarg1,ppc_rarg1,lo);
				ppc_cmpli(ppc_cr0,ppc_rarg1,first,0);
			}

			if (check_tail)
			{
				fail = ppc_CreateLabel();
				ppc_bcx(BO_FALSE,BI_CR0_EQ,0,0,0);	// bne -> fail

				// --- last instruction ------------------------------------------
				u8* tail = ptr + code_sz - 2;
				const u16 last = *(u16*)tail;
				u32 lo = ppc_addr_high(ppc_rarg1,(void*)tail);
				ppc_lhz(ppc_rarg1,ppc_rarg1,lo);
				ppc_cmpli(ppc_cr0,ppc_rarg1,last,0);
			}

			ppc_label* check_ok=ppc_CreateLabel();
			ppc_bcx(BO_TRUE,BI_CR0_EQ,0,0,0);		// beq -> check_ok

			if (fail)
				fail->MarkLabel();					// the bne above lands here
			ppc_li(ppc_rarg0,block->start);			// rdv_BlockCheckFail(pc)
			ppc_jump(ngen_BlockCheckFail_stub);

			check_ok->MarkLabel();
		}
	}

	ppc_addic(ppc_cycles,ppc_cycles,-block->cycles,1);
	
	ppc_label* jdst=ppc_CreateLabel();
	ppc_bcx(BO_FALSE,BI_CR0_LT,0,0,0);

	ppc_li(ppc_next_pc,block->start);
	ppc_jump(loop_do_update_write);

	jdst->MarkLabel();

	// IDLE_KAMUI / wait-loop rules (decoder.cpp idle_hash): AFTER the cycle
	// check above, so this block itself still runs (before it, the block's own
	// exit to the update path would be lost). r29 = (1 - idle_hash) * timeslice:
	// 0 ends the slice at the next block's check; a negative value is a debt of
	// that many further slices (IDLE_SHINOBI: 16 -> 15, dropped by the next
	// dispatched interrupt in the mainloop). Emulated time still advances a
	// full slice per UpdateSystem, so a wait loop just reaches its event in
	// fewer host turns.
	if (block->idle_hash)
		ppc_li(ppc_cycles, (u32)((1 - (s32)block->idle_hash) * sh4_GetTimeslice()));

	// No GPR reload here: pinned regs (r14..r28) hold the authoritative SH4 GPR
	// values continuously across blocks, so there is nothing to re-read. They are
	// loaded once in the mainloop prologue and only resynced with memory around
	// shop_ifb / the canonical fallback.
}

// =====================
// MEMORY ACCESS HELPERS
// =====================

// Static GPR allocation master switch. Full rationale at reg_flush_all().
// Must be defined before the first use below; set to 0 for all-memory mode.
#define STATIC_GPR_ALLOC 1

//1 opcode (2 if bouncing a pinned FR through memory — see FPU_PIN below)
void ppc_sh_load(u32 D,u32 sh4_reg)
{
#if STATIC_GPR_ALLOC
	ppc_ireg ri=GetIntReg(sh4_reg);
	if (ri!=ppc_rinvalid)
	{
		// Value already lives in a pinned PPC register; just move it.
		if ((u32)ri!=D)
			ppc_ori(D,ri,0);	// mr D, ri
		return;
	}
#endif
	// FPU_PIN landmine: shop_readm/writem/mov64 read/write ANY sh4_reg's raw
	// bit pattern through this generic helper, including FR-typed ones (e.g.
	// the fmov.s/fmov.d memory paths, which bounce the value through a GPR
	// since PPC750 has no direct GPR<->FPR move). If fr[] is pinned, the
	// authoritative bits live in a PPC FPR, not Sh4cntx.fr[] memory — so a
	// bare lwz here would read stale data. Fix: flush the pinned FPR to its
	// memory slot first, THEN read it back as an integer. One extra insn,
	// only on this int<->pinned-float crossing (the pure-float fast path
	// via ppc_sh_load_f32 never pays it).
	if (get_fpu_pin_preset())
	{
		ppc_freg rf=GetFloatReg(sh4_reg);
		if (rf!=ppc_finvalid)
		{
			u32 ofs=Sh4cntx.offset(sh4_reg);
			ppc_stfs(rf,ppc_contex,ofs);
			ppc_lwz(D,ppc_contex,ofs);
			return;
		}
	}
	ppc_lwz(D,ppc_contex,Sh4cntx.offset(sh4_reg));
}
void ppc_sh_load(u32 D,shil_param prm)
{
	verify(prm.is_reg());
	ppc_sh_load(D,prm._reg);
}
// Resolve a register source to the PPC reg an instruction can read directly:
// the pinned reg if `prm` is statically allocated, else load it into scratch
// `D` and return D. Lets single-instruction ops (cmp, and, add, ...) read
// pinned sources in place and skip the redundant `mr D,pinned` move. ONLY for
// ops whose dest is a scratch reg (or that don't write the source) — never when
// the op would clobber the pinned reg it just read.
static u32 src_or_load(u32 sh4_reg,u32 D)
{
#if STATIC_GPR_ALLOC
	ppc_ireg ri=GetIntReg(sh4_reg);
	if (ri!=ppc_rinvalid)
		return (u32)ri;
#endif
	ppc_sh_load(D,sh4_reg);
	return D;
}
static u32 src_or_load(shil_param prm,u32 D)
{
	verify(prm.is_reg());
	return src_or_load(prm._reg,D);
}
void ppc_sh_load_f32(u32 D,u32 sh4_reg)
{
	if (get_fpu_pin_preset())
	{
		ppc_freg rf=GetFloatReg(sh4_reg);
		if (rf!=ppc_finvalid)
		{
			// Value already lives in a pinned PPC FPR; just move it.
			if ((u32)rf!=D)
				ppc_fmrx(D,rf,0);
			return;
		}
	}
	ppc_lfs(D,ppc_contex,Sh4cntx.offset(sh4_reg));
}
void ppc_sh_load_f32(u32 D,shil_param prm)
{
	verify(prm.is_reg());
	ppc_sh_load_f32(D,prm._reg);
}
void ppc_sh_load_u16(u32 D,u32 sh4_reg)
{
	ppc_lhz(D,ppc_contex,Sh4cntx.offset(sh4_reg));
}
void ppc_sh_load_u16(u32 D,shil_param prm)
{
	verify(prm.is_reg());
	ppc_sh_load_u16(D,prm._reg);
}
//1 opcode
void ppc_sh_addr(u32 D,u32 sh4_reg)
{
	ppc_addi(D,ppc_contex,Sh4cntx.offset(sh4_reg));
}
void ppc_sh_addr(u32 D,shil_param prm)
{
	verify(prm.is_reg());
	ppc_sh_addr(D,prm._reg);
}
//1 opcode (2 if bouncing a pinned FR through memory — see FPU_PIN, ppc_sh_load)
void ppc_sh_store(u32 D,u32 sh4_reg)
{
#if STATIC_GPR_ALLOC
	ppc_ireg ri=GetIntReg(sh4_reg);
	if (ri!=ppc_rinvalid)
	{
		// Destination is a pinned PPC register; update it in place.
		if ((u32)ri!=D)
			ppc_ori(ri,D,0);	// mr ri, D
		return;
	}
#endif
	// FPU_PIN: symmetric fix to ppc_sh_load above. Write the raw bits to
	// memory as before, then reload the pinned FPR from that same slot so
	// it picks up the new value instead of going stale.
	if (get_fpu_pin_preset())
	{
		ppc_freg rf=GetFloatReg(sh4_reg);
		if (rf!=ppc_finvalid)
		{
			u32 ofs=Sh4cntx.offset(sh4_reg);
			ppc_stw(D,ppc_contex,ofs);
			ppc_lfs(rf,ppc_contex,ofs);
			return;
		}
	}
	ppc_stw(D,ppc_contex,Sh4cntx.offset(sh4_reg));
	// jit_tlive: a jcond later in this block will read T: keep a copy in r31 (see compile_state.t_copy_ok)
	if (sh4_reg==reg_sr_T && compile_state.t_copy_ok)
	{
		ppc_ori(ppc_djump,D,0);		// mr r31, D
		compile_state.t_in_djump=true;
	}
}
void ppc_sh_store(u32 D,shil_param prm)
{
	verify(prm.is_reg());
	ppc_sh_store(D,prm._reg);
}
void ppc_sh_store_f32(u32 D,u32 sh4_reg)
{
	if (get_fpu_pin_preset())
	{
		ppc_freg rf=GetFloatReg(sh4_reg);
		if (rf!=ppc_finvalid)
		{
			// Destination is a pinned PPC FPR; update it in place.
			if ((u32)rf!=D)
				ppc_fmrx(rf,D,0);
			return;
		}
	}
	ppc_stfs(D,ppc_contex,Sh4cntx.offset(sh4_reg));
}
u32 ppc_addr_high(u32 rD,void* ptr)
{
	unat diff=(u8*)ptr-(u8*)0;
	u32 rv=(s32)(s16)diff;
	diff-=rv;
	ppc_addis(rD,0,diff>>16);

	return rv;
}

void ppc_sh_store_f32(u32 D,shil_param prm)
{
	verify(prm.is_reg());
	ppc_sh_store_f32(D,prm._reg);
}

// --- Vector float element access -------------------------------------------
// For a float param (scalar FMT_F32, or vector FMT_V4/FMT_V16), prm._reg/_imm
// holds the BASE fr/xf register index. Element i lives at offset(base)+i*4 in
// the context, and reg_fr_0..reg_fr_15 / reg_xf_0..reg_xf_15 are contiguous
// (sh4_if.h), so prm._reg+i is always the correct absolute register index.
// These load/store the i-th single-precision element.
//
// FPU_PIN preset: only fr[] is pinned (GetFloatReg returns invalid for the
// xf[] range — there aren't enough non-volatile PPC FPRs left to pin both
// banks), so an FV4/FV16 base in xf (e.g. ftrv's XMTRX operand) naturally
// falls through to memory here, unaffected.
u32 ppc_fvec_ofs(shil_param prm,u32 i)
{
	return Sh4cntx.offset(prm._reg) + i*4;
}
void ppc_fvec_load(u32 fd,shil_param prm,u32 i)
{
	if (get_fpu_pin_preset())
	{
		ppc_freg rf=GetFloatReg(prm._reg+i);
		if (rf!=ppc_finvalid)
		{
			if ((u32)rf!=fd)
				ppc_fmrx(fd,rf,0);
			return;
		}
	}
	ppc_lfs(fd,ppc_contex,ppc_fvec_ofs(prm,i));
}
void ppc_fvec_store(u32 fs,shil_param prm,u32 i)
{
	if (get_fpu_pin_preset())
	{
		ppc_freg rf=GetFloatReg(prm._reg+i);
		if (rf!=ppc_finvalid)
		{
			if ((u32)rf!=fs)
				ppc_fmrx(rf,fs,0);
			return;
		}
	}
	ppc_stfs(fs,ppc_contex,ppc_fvec_ofs(prm,i));
}

