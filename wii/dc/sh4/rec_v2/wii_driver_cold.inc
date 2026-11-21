// wii_driver_cold.inc: out-of-line slow paths (ColdFrag, FlushCold).
// Part of wii_driver.cpp, which #includes it: not compiled on its own.

// =====================
// OPERATION COMPILATION
// =====================

// ── Cold-out-of-line slow paths for memory ops ──────────────────────────────
// The mem fast path is the common case (direct RAM/VRAM access). To keep it a
// straight fall-through with a SINGLE not-taken branch, we emit:
//
//     <addr/ptr setup>
//     cmpi ptr,0
//     beq  cold        ; forward, PREDICTED NOT-TAKEN -> fast path falls through
//     <fast access>
//   done:              ; (next op continues here, no branch on the fast path)
//     ...rest of block...
//   ngen_End
//   cold:              ; emitted out-of-line, AFTER the block tail (never
//     <slow MMIO call>   ;  fall-through reached; only via the beq)
//     b done           ; backward unconditional, jumps into the normal stream
//
// Each fast path registers a ColdFrag describing its slow body; FlushCold()
// emits them after ngen_End and back-patches the forward beq (16-bit) to the
// cold entry. The `b done` is a backward branch with a known target, emitted
// directly via ppc_bx(void*,LK) (no patch needed).
struct ColdFrag
{
	ppc_label* beq;     // the forward beq site to patch to the cold entry
	void*      done;    // join point in the main stream (backward target)
	u8         kind;    // 0=read scalar, 1=read pair(64), 2=write scalar, 3=write pair,
	                    // 4=store queue in C (wiiu_sqw_cold, rarg0=dst), 5=TA block parse (wiiu_ta_scan, r6=slot)
	u8         sz;      // 1/2/4 (scalar)
	u8         rdreg;   // read: destination reg (rrv0 or pinned)
	u8         datareg; // write: data reg (rarg1 or pinned)
	u8         areg;    // address source reg (rarg0 or pinned)
	// FPU_PIN Phase B: when the read dest / write source is a pinned FR the
	// fast path uses lfs/stfs straight to/from the FPR (no GPR bounce). The
	// slow C path still deals in ints, so it bounces through the FR's own
	// context slot: read = ReadMem->rrvN->stw fofs->lfs fdreg; write =
	// stfs fdreg->fofs->lwz->WriteMem. fdreg==0xFF marks the plain int path
	// (must be set on EVERY registration — s_cold is reused, stale otherwise).
	// For pairs, the second FR is fdreg+1 and its slot is fofs+4 (fr[] is
	// contiguous and GetFloatReg maps fr[n]->f14+n).
	u8         fdreg;   // pinned FPR for the float bounce, or 0xFF = int path
	u16        fofs;    // context byte offset of the FR's memory slot
	void*      fuct;    // slow C function (ReadMem*/WriteMem*); 0 -> default by sz
};
// Sized so it cannot overflow within one block: ngen_Compile bails when
// emit_FreeSpace() < 16KB, so a block emits < 4096 PPC insns; each mem op's
// fast path is >=~8 insns, capping mem ops well under 1024.
static ColdFrag s_cold[1024];
static u32      s_cold_n;
#if defined(__WIIU__)
static void j2_cold(const ColdFrag& c);   // sq_guard (below the fastmem helpers)
#endif

static void ColdReset() { s_cold_n = 0; }

#if defined(__WIIU__)
extern "C" { int g_exp_entry_cold = 1; }   // entry_cold (declared in wii_driver_emit.inc); on by default
// ngen_Begin with entry_cold: the out-of-cycles path of this block's entry check (ColdFrag kind 7, pc in fuct)
static void cold_entry_update(ppc_label* blt_site,u32 pc)
{
	verify(s_cold_n < 1024);
	ColdFrag& c=s_cold[s_cold_n++];
	c.beq=blt_site; c.done=0; c.kind=7; c.sz=0; c.rdreg=0; c.datareg=0; c.areg=0;
	c.fdreg=0xFF; c.fofs=0; c.fuct=(void*)(uintptr_t)pc;
}
#endif

static void FlushCold()
{
	for (u32 i = 0; i < s_cold_n; i++)
	{
		ColdFrag& c = s_cold[i];
		// The forward beq uses a 16-bit (B-form) displacement. Cold fragments sit
		// after the block tail; for a normal (<32KB) block this is in range, but
		// guard against silent truncation -> wrong target on a pathologically
		// large block. (ngen_Compile bails blocks needing >16KB, so this holds.)
		snat beq_disp = (u8*)emit_GetCCPtr() - (u8*)c.beq;
		verify(beq_disp >= 0 && beq_disp < 32768);
		c.beq->MarkLabel();              // patch the forward beq -> here (cold entry)

		if (c.kind==0)                   // --- read scalar: ReadMem(addr)->rrv0 ---
		{
			if (c.areg!=ppc_rarg0) ppc_ori(ppc_rarg0,c.areg,0);   // mr rarg0,areg
			void* f=c.fuct;
			if (!f) f=(c.sz==1)?(void*)ReadMem8:(c.sz==2)?(void*)ReadMem16:(void*)ReadMem32;
			ppc_call(f);
			if (c.fdreg!=0xFF)
			{
				// FPU_PIN: float dest (sz==4). Bounce rrv0 -> pinned FPR via its
				// context slot (no direct GPR->FPR move on PPC750).
				ppc_stw(ppc_rrv0,ppc_contex,c.fofs);
				ppc_lfs(c.fdreg,ppc_contex,c.fofs);
			}
			else if (c.sz==1)      ppc_extsbx(c.rdreg,ppc_rrv0,0);
			else if (c.sz==2) ppc_extshx(c.rdreg,ppc_rrv0,0);
			else if (c.rdreg!=ppc_rrv0) ppc_ori(c.rdreg,ppc_rrv0,0);
		}
		else if (c.kind==1)              // --- read pair (64): ReadMem64 -> rrv0:rrv1 ---
		{
			void* f=c.fuct? c.fuct : (void*)ReadMem64;
			ppc_call(f);
			if (c.fdreg!=0xFF)
			{
				// FPU_PIN: float pair. rrv0=word[addr]->fdreg, rrv1=word[addr+4]
				// ->fdreg+1, each bounced through its own context slot.
				ppc_stw(ppc_rrv0,ppc_contex,c.fofs);
				ppc_lfs(c.fdreg,ppc_contex,c.fofs);
				ppc_stw(ppc_rrv1,ppc_contex,c.fofs+4);
				ppc_lfs(c.fdreg+1,ppc_contex,c.fofs+4);
			}
		}
		else if (c.kind==2)              // --- write scalar: WriteMem(addr,data) ---
		{
			if (c.areg!=ppc_rarg0)    ppc_ori(ppc_rarg0,c.areg,0);
			if (c.fdreg!=0xFF)
			{
				// FPU_PIN: float source (sz==4). Bounce pinned FPR -> rarg1 via
				// its context slot, then the normal WriteMem32.
				ppc_stfs(c.fdreg,ppc_contex,c.fofs);
				ppc_lwz(ppc_rarg1,ppc_contex,c.fofs);
				ppc_call(&WriteMem32);
			}
			else
			{
				if (c.datareg!=ppc_rarg1) ppc_ori(ppc_rarg1,c.datareg,0);
				if (c.sz==1)      { ppc_andi(ppc_rarg1,ppc_rarg1,0xFF);   ppc_call(&WriteMem8); }
				else if (c.sz==2) { ppc_andi(ppc_rarg1,ppc_rarg1,0xFFFF); ppc_call(&WriteMem16); }
				else                ppc_call(&WriteMem32);
			}
		}
#if defined(__WIIU__)
		else if (c.kind==4)              // --- sq_inline: not TA, or the frame is full: the C path ---
		{
			if (g_exp_sq_ram)
			{
				// sq_ram (J1b): when this queue's QACR area is RAM, g_sq_ram[queue] is mem_b's base: copy the 32
				// bytes from sq_both[dst & 0x20] to RAM + (dst & 0xFFFFE0) here, as sqw_c would, with no call
				verify(RAM_MASK==0x00FFFFFF);
				const u32 tab=(u32)(uintptr_t)g_sq_ram, sq=(u32)(uintptr_t)sq_both;
				ppc_rlwinmx(ppc_r9,ppc_rarg0,29,29,29,0);			// (dst >> 3) & 4: the queue's entry
				ppc_addis(ppc_r10,ppc_r0,(tab-(u32)(s32)(s16)tab)>>16);
				ppc_addi(ppc_r10,ppc_r10,(u32)(s32)(s16)tab);
				ppc_lwzx(ppc_r10,ppc_r10,ppc_r9);
				ppc_cmpwi(ppc_r10,0);
				ppc_label* ccall=ppc_CreateLabel();
				ppc_bcx(BO_TRUE,BI_CR0_EQ,0,0,0);			// beq: not RAM (or J1b off for it): C
				ppc_rlwinmx(ppc_r9,ppc_rarg0,0,8,26,0);			// dst & 0x00FFFFE0
				ppc_addx(ppc_r10,ppc_r10,ppc_r9,0,0);
				ppc_rlwinmx(ppc_r9,ppc_rarg0,0,26,26,0);			// dst & 0x20: which queue
				ppc_addis(ppc_r8,ppc_r0,(sq-(u32)(s32)(s16)sq)>>16);
				ppc_addi(ppc_r8,ppc_r8,(u32)(s32)(s16)sq);
				ppc_addx(ppc_r8,ppc_r8,ppc_r9,0,0);
				for (u32 k=0;k<4;k++)
					ppc_lfd(ppc_f0+k,ppc_r8,k*8);
				for (u32 k=0;k<4;k++)
					ppc_stfd(ppc_f0+k,ppc_r10,k*8);
				ppc_bx(c.done,0);
				ccall->MarkLabel();
			}
			ppc_call(&wiiu_sqw_cold);    // rarg0 still holds dst
		}
		else if (c.kind==5)              // --- sq_inline: the block (already in the frame) needs parsing ---
		{
			ppc_ori(ppc_rarg0,ppc_r6,0);
			ppc_call(&wiiu_ta_scan);
		}
		else if (c.kind==6)              // --- sq_guard: the other side of the store-queue test ---
			j2_cold(c);
		else if (c.kind==7)              // --- entry_cold: out of cycles at block entry ---
		{
			ppc_li(ppc_next_pc,(u32)(uintptr_t)c.fuct);   // the block's start pc
			ppc_jump(loop_do_update_write);
			continue;                    // no join: the update path re-dispatches
		}
#endif
		else if (c.kind==3)              // --- write pair (64): WriteMem64 ---
		{
			if (c.fdreg!=0xFF)
			{
				// FPU_PIN: float pair. fdreg=high->rarg2, fdreg+1=low->rarg3
				// (the WriteMem64(u32,u64) EABI regs), each via its context slot.
				ppc_stfs(c.fdreg,ppc_contex,c.fofs);
				ppc_lwz(ppc_rarg2,ppc_contex,c.fofs);
				ppc_stfs(c.fdreg+1,ppc_contex,c.fofs+4);
				ppc_lwz(ppc_rarg3,ppc_contex,c.fofs+4);
			}
			ppc_call(&WriteMem64);
		}

		ppc_bx(c.done,0);                // backward unconditional b -> done (join)
	}
	s_cold_n = 0;
}

