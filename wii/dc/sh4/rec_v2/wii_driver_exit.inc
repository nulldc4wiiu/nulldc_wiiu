// wii_driver_exit.inc: ngen_End, block exit code generation.
// Part of wii_driver.cpp, which #includes it: not compiled on its own.

// ====================================
// ngen_End: Block Exit Code Generation
// ====================================

void ngen_End(DecodedBlock* block)
{
	// No blanket GPR flush here: the pinned PPC registers (r14..r28) are the
	// authoritative copy of the SH4 GPRs and stay live across straight-line
	// block transitions (static/dynamic jumps just flow into the next block).
	// The exception is the interrupt path (BET_*Intr) below, which calls
	// UpdateINTC -> Do_Exception (reads r[15], swaps the register bank); that
	// case brackets the call with reg_flush_all()/reg_reload_all() locally.
	// shop_ifb and the canonical fallback similarly self-bracket.
	switch(block->BlockType)
	{
	case BET_Cond_0:
	case BET_Cond_1:
		{
			//printf("COND %d\n",block->BlockType&1);
			//die("not supported");
			// JIT_CR0: the block's last op left its condition in CR0 and
			// emitted nothing. Branch on that bit, and write sr_T as the
			// constant each path implies.
			//
			//   T == (BlockType&1)  =>  BranchBlock
			//   T = cr0_invert ? !bit : bit
			// so BranchBlock is taken when
			//   bit == (cr0_invert ? !want : want)
			// and on that path T is `want`, on the other it is `!want`.
			if (compile_state.cr0_live && !compile_state.has_jcond)
			{
				const u32  want      = block->BlockType & 1;
				const bool bit_taken = compile_state.cr0_invert ? (want==0) : (want!=0);

				ppc_label* jtrue=ppc_CreateLabel();
				ppc_bcx(bit_taken?BO_TRUE:BO_FALSE,compile_state.cr0_bit,0,0,0);

				// fall-through: BranchBlock not taken, so T is the complement.
				ppc_li(ppc_rarg0,want^1);
				ppc_sh_store(ppc_rarg0,reg_sr_T);
				DoStatic(block->NextBlock);

				jtrue->MarkLabel();
				ppc_li(ppc_rarg0,want);
				ppc_sh_store(ppc_rarg0,reg_sr_T);
				DoStatic(block->BranchBlock);
				break;
			}

			u32 reg;
			if (compile_state.has_jcond)
			{
				reg=ppc_djump;
			}
			else if (compile_state.t_in_rarg0)
			{
				// The block's last op was the compare that produced T, and
				// nothing has run since — it is still in rarg0. Skip reloading
				// the value we stored two instructions ago (see t_in_rarg0).
				reg=ppc_rarg0;
			}
			else
			{
				// cmpi reads any reg: if T is pinned, compare it in place.
				reg=src_or_load(reg_sr_T,ppc_rarg0);
			}

			ppc_cmpi(ppc_cr0,reg,block->BlockType&1,0);
	
			ppc_label* jtrue=ppc_CreateLabel();
			ppc_bcx(BO_TRUE,BI_CR0_EQ,0,0,0);

			DoStatic(block->NextBlock);
			jtrue->MarkLabel();
			DoStatic(block->BranchBlock);
		}
		break;

	case BET_DynamicCall:
	case BET_DynamicJump:
	case BET_DynamicRet:
		//printf("Dynamic !\n");
		//mov reg,djump
		ppc_ori(ppc_rarg0,ppc_djump,0);  // mr rarg0, djump

		// ---- DYN_IC preset: per-site monomorphic inline cache ----------------
		// Both dispatch paths below end in `bctr` through a table entry: ~9
		// instructions and two dependent loads, with an indirect branch the
		// 750CL cannot predict. But most dynamic exits are monomorphic — the
		// SH4 `JSR @Rn` to a fixed callee, and especially the JSR->RTS;NOP
		// trampoline idiom that dominates 3D inner loops. For those, the target
		// never changes, so the table lookup re-derives a constant every time.
		//
		// So we put a 4-instruction guard in front of it, self-patched with the
		// target this site actually took the first time it ran:
		//
		//     xoris  rarg1, djump, hi(pc)   ; rarg1 = djump ^ (hi<<16)
		//     cmplwi cr0, rarg1, lo(pc)     ; ==0 iff djump == pc  (32-bit cmp,
		//     bne    generic                ;  two insns, no immediate load)
		//     b      target                 ; direct, statically predicted
		//   generic:
		//     <bcache / cache[] path, unchanged>
		//
		// Until it is patched, all four slots branch to `generic`, so a site is
		// always correct, just slow. Slot 0 is `bl` to the fill stub, which
		// compiles the target, rewrites the four words, and jumps into it.
		//
		// Staleness: the baked `b target` is exactly as safe as the direct `b`
		// DoStatic() patches in, and for the same reason — every invalidation
		// path is a FULL cache clear (see rdv_BlockCheckFail in driver.cpp),
		// which destroys these sites along with the code they point at.
		//
		// Monomorphic only: a site is patched once and never re-patched, so a
		// polymorphic site (typically BET_DynamicRet — one callee returning to
		// many callers) settles into paying 3 extra ALU/branch ops in front of
		// the normal lookup. That is why mode 1 leaves RTS alone; mode 2 opts
		// it in for A/B.
		{
			const int ic = get_dyn_ic_preset();
			const bool ic_here = ngen_LinkBlock_Dynamic_IC_stub &&
			                     ((ic >= 2) ||
			                      (ic == 1 && block->BlockType != BET_DynamicRet));
			if (ic_here)
			{
				// 4 words. `generic` is site+16, so each unpatched slot is a
				// plain forward branch onto it (LI is in instructions).
				ppc_call(ngen_LinkBlock_Dynamic_IC_stub);	// +0  -> xoris
				ppc_bx(3,0,0);					// +4  -> cmplwi
				ppc_bx(2,0,0);					// +8  -> bne generic
				ppc_bx(1,0,0);					// +12 -> b target
			}
		}
		// generic path starts here (the IC's fall-through target)

		if (get_bcache_preset())
		{
			// ---- BCACHE preset: flat single-cache-line dispatch ----
			// bm_bcache[] holds {addr, code} pairs value-mirrored from cache[]
			// (blockmanager.cpp), so the hit path touches ONE data cache line
			// (the 8-byte entry) instead of two (cache[] pointer + the heap
			// DynarecBlock it points at) and drops the lookups++ read-modify-
			// write. Policy note: the flat path does not feed the replacement
			// counters, so a bucket-colliding block steals the slot after +3
			// slow-path hits — the two then alternate, each getting the fast
			// path most of the time, instead of one paying bm_GetCode forever.
			//
			//   off   = ((addr>>2)&16383)*8 = (addr<<1) & 0x1FFF8  (one rlwinm)
			//   entry = &bm_bcache[off/8];
			//   if (entry->addr == addr) goto entry->code;
			//   else goto loop_no_update;             // full bm_GetCode
			//
			// Invariant (blockmanager.h): addr != 0xFFFFFFFF => code valid, so
			// no separate code==0 test. addr stays in rarg0 for the miss path.
			verify(sizeof(BlockCacheFlatEntry)==8 && BM_BLOCKLIST_COUNT==16384);

			ppc_rlwinmx(ppc_rarg1,ppc_rarg0,1,15,28,0);	// rarg1 = (addr<<1) & 0x1FFF8
			u32 lo=ppc_addr_high(ppc_rarg2,(void*)&bm_bcache[0]);
			ppc_addx(ppc_rarg2,ppc_rarg2,ppc_rarg1,0,0);	// rarg2 = &bm_bcache[idx] - lo
			ppc_lwz(ppc_rarg3,ppc_rarg2,lo);		// entry.addr
			ppc_cmp(ppc_cr0,ppc_rarg3,ppc_rarg0,0);		// == addr ?
			ppc_label* miss=ppc_CreateLabel();
			ppc_bcx(BO_FALSE,BI_CR0_EQ,0,0,0);		// bne miss
			ppc_lwz(ppc_rarg3,ppc_rarg2,lo+4);		// entry.code
			ppc_mtctr(ppc_rarg3);
			ppc_bcctrx(BO_ALWAYS,BI_CR0_EQ,0);		// bctr -> cached code
			miss->MarkLabel();
		}
		else
		{
			// Inline bm_GetCode's fast path (bm_CheckCache) before falling back
			// to the loop_no_update machinery:
			//   idx    = (addr>>2) & (16384-1);
			//   cached = cache[idx];                  // DynarecBlock*
			//   if (cached->addr==addr && cached->code) { cached->lookups++;
			//       goto cached->code; }
			//   else goto loop_no_update;             // full bm_GetCode
			//
			// Byte offset into cache[] = idx*4 = ((addr>>2)&16383)<<2
			//                          = addr & 0xFFFC  (single rlwinm).
			// DynarecBlock layout (PPC32): code@0, addr@4, lookups@8.
			// addr stays in rarg0 for the miss path (bm_GetCode arg).
			ppc_rlwinmx(ppc_rarg1,ppc_rarg0,0,16,29,0);	// rarg1 = addr & 0xFFFC
			u32 lo=ppc_addr_high(ppc_rarg2,(void*)&cache[0]);
			ppc_addi(ppc_rarg2,ppc_rarg2,lo);		// rarg2 = &cache
			ppc_lwzx(ppc_rarg2,ppc_rarg2,ppc_rarg1);	// rarg2 = cache[idx]

			ppc_lwz(ppc_rarg3,ppc_rarg2,offsetof(DynarecBlock,addr));
			ppc_cmp(ppc_cr0,ppc_rarg3,ppc_rarg0,0);	// cached->addr == addr ?
			ppc_label* miss1=ppc_CreateLabel();
			ppc_bcx(BO_FALSE,BI_CR0_EQ,0,0,0);		// bne miss

			ppc_lwz(ppc_rarg3,ppc_rarg2,offsetof(DynarecBlock,code));
			ppc_cmpi(ppc_cr0,ppc_rarg3,0,0);		// code == 0 ? (empty_block)
			ppc_label* miss2=ppc_CreateLabel();
			ppc_bcx(BO_TRUE,BI_CR0_EQ,0,0,0);		// beq miss

			// hit: lookups++ (keeps bm_GetCode's cache-replacement policy fed)
			// NOTE: must NOT use r0 as the addi target/source -- `addi rD,r0,imm`
			// treats rA=r0 as literal 0 (it's how ppc_li is built), so
			// `addi r0,r0,1` would emit `r0 = 0 + 1 = 1`, never incrementing the
			// loaded value. Use rarg1 (dead after the cache[idx] load above).
			ppc_lwz(ppc_rarg1,ppc_rarg2,offsetof(DynarecBlock,lookups));
			ppc_addi(ppc_rarg1,ppc_rarg1,1);
			ppc_stw(ppc_rarg1,ppc_rarg2,offsetof(DynarecBlock,lookups));

			ppc_mtctr(ppc_rarg3);
			ppc_bcctrx(BO_ALWAYS,BI_CR0_EQ,0);		// bctr -> cached code

			miss1->MarkLabel();
			miss2->MarkLabel();
		}
		//jmp no update
		ppc_jump(loop_no_update);
		break;

	case BET_StaticIntr:
	case BET_DynamicIntr:
		// Gated like the BET_StaticJump trace below: printf goes to /ndclog.txt
		// on the SD card, and JIT_NEWOPS makes BET_StaticIntr common (every
		// ldc/ldc.l to SR now ends a block this way instead of falling back).
		if (get_debug_loop() == 1)
		{
			printf("BET: Interrupt !\n");
		}
		{
			u32 reg;
			if (block->BlockType==BET_StaticIntr)
			{
				ppc_li(ppc_rarg0,block->BranchBlock);
				reg=ppc_rarg0;
			}
			else
			{
				reg=ppc_djump;
			}
			ppc_sh_store(reg,reg_nextpc);
			// UpdateINTC -> Do_Exception reads r[15] (sgr=r[15]) and swaps the
			// register bank (sr.RB=1; UpdateSR). Pinned GPRs must be coherent in
			// memory for the read, and re-read afterwards to pick up the swap.
			reg_flush_all();
			ppc_call(&UpdateINTC);
			reg_reload_all();

			ppc_sh_load(ppc_next_pc,reg_nextpc);
			ppc_jump(loop_no_update);
		}
		break;

	case BET_StaticCall:
	case BET_StaticJump:
		if (get_debug_loop() == 1)
		{
			printf("Static 0x%08X!\n", block->BranchBlock);
		}
		DoStatic(block->BranchBlock);
		break;

	default:
		printf("END TYPE: %d\n",block->BlockType);
		die("wtfh end type\n");
	}
}

// f14:f29 — fr[0..15] only. xf[] is NOT pinned: fr[16] already claims all 16
// of the PPC EABI's callee-saved FPRs available after reserving f0 (scratch)
// and f1-f13 (call-arg/return volatiles used throughout this file); only
// f30/f31 are left, nowhere near enough for another 16-wide bank. See the
// FPU_PIN comment block above reg_flush_all_fpu() for the full rationale.
ppc_freg GetFloatReg(u32 reg)
{
	if (reg>=reg_fr_0 && reg<=reg_fr_15)
	{
		return (ppc_freg)((reg-reg_fr_0)+ppc_f14);
	}

	return ppc_finvalid;
}

//r14:r28 (shr11 missing)
ppc_ireg GetIntReg(u32 reg)
{
	if (reg>=reg_r0 && reg<=reg_r15 && reg!=reg_r11)
	{
		if (reg>=reg_r11) reg--;

		return (ppc_ireg)((reg-reg_r0 )+ppc_r14);
	}

	return ppc_rinvalid;
}
