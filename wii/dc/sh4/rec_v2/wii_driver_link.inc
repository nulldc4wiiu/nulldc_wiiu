// wii_driver_link.inc: block reset, static/dynamic linking, ngen_mainloop, features.
// Part of wii_driver.cpp, which #includes it: not compiled on its own.




void ngen_ResetBlocks()
{
	// Block-cache clear: every fastmem-patched site just died with its
	// block, so drop their trampolines too (bm_Reset calls this from
	// recSh4_ClearCache and bm_Init).
	rec_fastmem_reset_pool();
}

// The three words DoStatic() emits for `pc` at `site`:
//   addis rarg0,0,pc>>16 ; ori rarg0,rarg0,pc&0xFFFF ; bl ngen_LinkBlock_Static_stub
static inline bool static_site_wants(const u32* site, u32 pc)
{
	const u32 rg = (u32)ppc_rarg0;
	snat offs = ((u8*)ngen_LinkBlock_Static_stub - (u8*)(site+2)) >> 2;
	return site[0] == (0x3C000000u | (rg<<21) | (pc>>16))
	    && site[1] == (0x60000000u | (rg<<21) | (rg<<16) | (pc&0xFFFFu))
	    && site[2] == (0x48000000u | ((0xffffffu & (u32)offs) << 2) | 1u);
}

// Static exits whose site no longer asked for this link when the patch came due.
static u32 s_link_stale=0;

// JIT_LINK_GUARD. rdv_FindOrCompile() can clear the whole code cache before it
// returns (CACHE_MIN_FREE / CACHE_FLUSH_THRESHOLD in rdv_CompileBlock), and a
// guest CCR.ICI write (ccn.cpp, jit_ici=0) clears it from INSIDE the running
// block, which then still reaches this exit. Either way LastAddr went back to
// the start and the target was just compiled there -- possibly right on top of
// `patch`. Patching then writes `b target` into the middle of an unrelated new
// block: a freeze that comes and goes with code layout (flycastPS3, Looney
// Tunes BIOS hang, 2026-10-08). Checking all three words matters: the PS3
// checked the `bl` alone, and the new block had its own link `bl` there.
// A site that still holds the same three words for the same pc is a genuine
// link request at that address, so patching it is correct either way.
void* FASTCALL ngen_LinkBlock_Static(u32 pc,u32* patch)
{
	next_pc=pc;

	DynarecCodeEntry* rv=rdv_FindOrCompile();

	if (!static_site_wants(patch,pc))
	{
		const int guard=get_jit_link_guard_preset();
		s_link_stale++;
		// Bounded: the first 8, then a running total every 256th. Flushed,
		// because with the guard off the next thing may be a freeze.
		if (s_link_stale<=8 || (s_link_stale&255)==0)
		{
			printf("[LINK] stale static site #%u at %p for pc=%08X (%08X %08X %08X): %s\n",
			       s_link_stale, patch, pc, patch[0], patch[1], patch[2],
			       guard ? "skipped" : "PATCHED ANYWAY (guard off)");
			fflush(stdout);
		}
		if (guard)
			return (void*)rv;
	}

	emit_ptr=patch;
	{
		ppc_jump(rv);
	}
	emit_ptr=0;

	make_address_range_executable(patch, 1*sizeof(u32));

	return (void*)rv;
}

// The `bl` word ngen_End() emitted into slot 0 of an IC site, recomputed for a
// given site address. Used to prove the site still exists before patching it.
static inline u32 ic_site_bl_word(const u32* site)
{
	snat offs = ((u8*)ngen_LinkBlock_Dynamic_IC_stub - (u8*)site) >> 2;
	return 0x48000000u | ((0xffffffu & (u32)offs) << 2) | 1u;
}

// DYN_IC preset: fill a dynamic exit's inline cache.
//
// Reached by the `bl` in slot 0 of the site emitted in ngen_End(). `pc` is the
// target the site actually branched to (rarg0 = djump, set just before the
// site); `site` is the address of slot 0, recovered from LR by the stub.
//
// We compile/find the target, overwrite the four slots with the guard shown in
// ngen_End(), and return the target so the stub's `bctr` jumps into it — this
// call's dispatch is not wasted.
//
// Called at most once per site: after the rewrite slot 0 is no longer a `bl`,
// so a target change just falls through to the generic path forever after.
void* FASTCALL ngen_LinkBlock_Dynamic_IC(u32 pc,u32* site)
{
	next_pc=pc;

	DynarecCodeEntry* rv=rdv_FindOrCompile();

	// rdv_FindOrCompile may have cleared and refilled the code cache (a full
	// clear resets LastAddr and recompiles from scratch). That destroys every
	// patched site, this one included — `site` would now point into code that
	// has been overwritten by an unrelated block. Detect it and patch nothing;
	// the site is gone anyway and the caller is returning into fresh code.
	if (*site != ic_site_bl_word(site))
		return (void*)rv;

	emit_ptr=site;
	{
		ppc_xoris(ppc_rarg1,ppc_djump,pc>>16);		// rarg1 = djump ^ (hi<<16)
		ppc_cmpli(ppc_cr0,ppc_rarg1,(u16)pc,0);		// cmplwi rarg1, lo
		ppc_bcx(BO_FALSE,BI_CR0_EQ,2,0,0);		// bne +8 -> generic (site+16)
		ppc_jump(rv);					// b target
	}
	emit_ptr=0;

	make_address_range_executable(site, 4*sizeof(u32));

	// [DYN_IC] engagement counter — served its purpose (Wii-measured
	// 2026-09-07, see dyn-ic-preset memory) and is quieted now that this
	// preset ships default-on. Re-enable if the fill path is ever suspect
	// again: costs one increment per SITE (not per traversal), never on
	// the hot path.
#if 0
	{
		static u32 ic_patched=0;
		if ((++ic_patched % 1000)==0)
			printf("[DYN_IC] %u sites patched\n", ic_patched);
	}
#endif

	return (void*)rv;
}

// =========
// MAIN LOOP
// =========

void ngen_mainloop()
{
	// FASTMEM preset: set up the MMU window + DSI handler before the first
	// block compiles. Idempotent, checks the preset itself, and leaves
	// g_wii_fastmem_active=0 on any failure (emission then stays legacy).
	// Called here (not recSh4_Init) so the game preset file has been
	// applied and _vmem_reserve() has placed the guest arrays.
	WiiFastmem_Init();
#if defined(__WIIU__)
	if (g_wii_fastmem_active && !s_fm_pool)
	{
		if (g_wiiu_fm_prepatch)
			FM_POOL_SIZE = 4*1024*1024;
		s_fm_pool = (u8*)wiiu_codegen_alloc(FM_POOL_SIZE, "fastmem trampolines");
		if (!s_fm_pool)
			g_wii_fastmem_active = 0;
		printf("[fastmem] trampoline pool %p, %u KB%s\n", s_fm_pool, FM_POOL_SIZE/1024, s_fm_pool ? "" : ": fastmem off");
		rec_fastmem_reset_pool();
	}
#endif

	printf("[LINK] static-link site check ARMED (jit_link_guard=%d)\n", get_jit_link_guard_preset());
	fflush(stdout);

	if (loop_code==0)
	{

		loop_code=(void(*)())emit_GetCCPtr();
		{
			/*
			create stack frame, push regsters, etc ..
			*/
#if defined(__WIIU__)
			// the Wii U build returns from here when the CPU is stopped (exit path below), so the non-volatile
			// FPRs the JIT pins SH4 registers in are saved too: f14-f31 at sp+8, the GPRs at the top as before
			u32 stac_alloc_size=(8+18*8+20*4+15)&~15u;
#else
			u32 stac_alloc_size=8+20*4;
#endif
			ppc_mfspr(ppc_r0,ppc_spr_lr);
			ppc_addi(ppc_sp,ppc_sp,-stac_alloc_size);
			
			//store link register
			ppc_stw(ppc_r0,ppc_sp,stac_alloc_size+4);

			//store gprs r13..r31 (19 preserved regs per ABI)
			// Layout: sp+[stac_alloc_size-4] = r13, sp+[stac_alloc_size-8] = r14, ...
			for (int i=0;i<19;i++)
			{
				ppc_stw(ppc_r13+i,ppc_sp,stac_alloc_size-4-i*4);
			}
#if defined(__WIIU__)
			for (int i=0;i<18;i++)
				ppc_stfd(ppc_f14+i,ppc_sp,8+i*8);
#endif

			/*
			pre load registers/counters etc ..
			*/

			//cntx base
			ppc_lip(ppc_contex,&Sh4cntx);

			// Load the pinned SH4 GPRs ONCE here, after ppc_contex is valid and
			// before the loop_no_update re-entry point below. They then stay live
			// in r14..r28 for the entire JIT run (no per-block reload/flush); the
			// only resync points are shop_ifb and the canonical fallback.
			reg_reload_all();
			// Same, for the FPU_PIN preset's fr[0..15] -> f14..f29 (no-op if off).
			reg_reload_all_fpu();

			// Cycle budget per timeslice. Latched from the accuracy preset
			// (FAST=1792 / BALANCED=896 / ACCURATE=448) rather than the fixed
			// SH4_TIMESLICE: recSh4_Run() applies the preset before this code
			// is emitted, and UpdateSystem_no_event() advances TMU/PVR by the
			// same s_timeslice, so the two stay consistent. Baked in at emit
			// time — the mainloop is generated once per session.
			const s32 jit_timeslice = sh4_GetTimeslice();

			//cycles
			ppc_li(ppc_cycles,jit_timeslice);

			//and pc!
			ppc_sh_load(ppc_next_pc,reg_nextpc);

			//no_update
			loop_no_update=emit_GetCCPtr();

			//handy function !
			ppc_call_and_jump(bm_GetCode);

			//do_update_write
			loop_do_update_write=emit_GetCCPtr();


			//next_pc _MUST_ be on ram since update system uses it for interrupt processing
			ppc_sh_store(ppc_next_pc,reg_nextpc);
			ppc_addi(ppc_cycles,ppc_cycles,jit_timeslice);	//add cycles (preset-latched timeslice)

			// Split UpdateSystem: the GPR-free peripheral cascade + interrupt
			// pending-check runs every timeslice WITHOUT flushing the pinned
			// GPRs. Only if it reports a pending interrupt (rv != 0) do we flush,
			// run the GPR-touching handler (Do_Interrupt / bank swap), and reload.
			ppc_call(UpdateSystem_no_event);
			ppc_cmpi(ppc_cr0,ppc_rrv0,0,0);			// rv == 0 ?
			ppc_label* no_intr=ppc_CreateLabel();
			ppc_bcx(BO_TRUE,BI_CR0_EQ,0,0,0);		// beq no_intr (skip handler)
			{
				reg_flush_all();
				ppc_call(UpdateSystem_handle_event);
				reg_reload_all();

				// IDLE_SHINOBI (decoder.cpp idle_task_rule; other_fix.md 2.2):
				// a dispatched interrupt drops what is left of a wait-loop
				// debt, so the task it made ready runs at once. r29 is still
				// negative here (after the add above) only while such a debt
				// is being paid -- no other path drives it below -max_cycles
				// -- so this is a no-op without one, and is emitted always
				// (the mainloop is emitted once; the decoder is the gate).
				// The PS3 loads 0 here; on this layout the slice is already
				// added above, so 0 would leave one more empty slice before
				// the handler runs. A fresh slice is the no-debt state.
				ppc_cmpi(ppc_cr0,ppc_cycles,0,0);
				ppc_label* paid=ppc_CreateLabel();
				ppc_bcx(BO_FALSE,BI_CR0_LT,0,0,0);	// bge paid
				ppc_li(ppc_cycles,jit_timeslice);
				paid->MarkLabel();
			}
			no_intr->MarkLabel();
#if defined(__WIIU__)
			// leave the mainloop at a timeslice boundary once the CPU is stopped (recSh4_Stop clears
			// sh4_int_bCpuRun), so the emulator thread can unwind and exit; upstream never returns from here
			{
				u32 lo=ppc_addr_high(ppc_rarg0,(void*)&sh4_int_bCpuRun);
				ppc_lbz(ppc_rarg0,ppc_rarg0,lo);
				ppc_cmpi(ppc_cr0,ppc_rarg0,0,0);
				ppc_label* stop=ppc_CreateLabel();
				ppc_bcx(BO_TRUE,BI_CR0_EQ,0,0,0);		// beq stop
				ppc_sh_load(ppc_next_pc,reg_nextpc);
				ppc_jump(loop_no_update);

				stop->MarkLabel();
				reg_flush_all();
				reg_flush_all_fpu();
				for (int i=0;i<18;i++)
					ppc_lfd(ppc_f14+i,ppc_sp,8+i*8);
				for (int i=0;i<19;i++)
					ppc_lwz(ppc_r13+i,ppc_sp,stac_alloc_size-4-i*4);
				ppc_lwz(ppc_r0,ppc_sp,stac_alloc_size+4);
				ppc_mtlr(ppc_r0);
				ppc_addi(ppc_sp,ppc_sp,stac_alloc_size);
				ppc_bclrx(BO_ALWAYS,BI_CR0_EQ,0);		// blr
			}
#else
			ppc_sh_load(ppc_next_pc,reg_nextpc);
			//
			ppc_jump(loop_no_update);
#endif
			//right

      /*
      Claude AI says it's dead end after looking in dump dynarec_XXX.bin
			ppc_lbz(ppc_rarg0,ppc_rarg0,ppc_addr_high(ppc_rarg0,(void*)&sh4_int_bCpuRun));
			ppc_sh_load(ppc_next_pc,reg_nextpc);

			ppc_cmpi(ppc_cr0,ppc_rarg0,1,0);	//set flags

			//does this even work ?
			//ppc_bcx(BO_TRUE,BI_CR0_EQ,ppc_jdiff(loop_no_update),0,0);
			

			//write back registers and stuff ...

			//cleanup
			
			Clean up the stack frame and return ...
			

			//restore link register
			ppc_lwz(ppc_r0,ppc_sp,stac_alloc_size+4);
			ppc_mtlr(ppc_r0);

			//restore gprs 13 .. 31
			for (int i=0;i<19;i++)
			{
				ppc_lwz(ppc_r13+i,ppc_sp,stac_alloc_size-4-i*4);
			}

			

			//destroy stack frame
			ppc_addi(ppc_sp,ppc_sp,stac_alloc_size);

			//return
			ppc_bclrx(BO_ALWAYS,BI_CR0_EQ,0);  // blr
      */

		} //that was mainloop


		//ngen_FailedToFindBlock
		ngen_FailedToFindBlock=(void(*)())emit_GetCCPtr();
		{
			ppc_call_and_jump(&rdv_FailedToFindBlock);
		}

    // ====================
    // STATIC BLOCK LINKING
    // ====================

		ngen_LinkBlock_Static_stub=emit_GetCCPtr();
		{
			//not used for now
			ppc_mfspr(ppc_rarg1,ppc_spr_lr);
			
			ppc_addi(ppc_rarg1,ppc_rarg1,(u32)-12);
			ppc_call_and_jump(&ngen_LinkBlock_Static);
		}

		ngen_LinkBlock_Dynamic_1st_stub=emit_GetCCPtr();
		{
			//not used for now
		}

		ngen_LinkBlock_Dynamic_2nd_stub=emit_GetCCPtr();
		{
			//not used for now
		}

		// DYN_IC preset: inline-cache fill stub. Entered by the `bl` in slot 0
		// of an unpatched site, so LR = site+4 and rarg0 = the target PC (the
		// `mr rarg0,djump` that precedes every site). Hand both to the patcher
		// and jump into whatever it returns.
		ngen_LinkBlock_Dynamic_IC_stub=emit_GetCCPtr();
		{
			ppc_mfspr(ppc_rarg1,ppc_spr_lr);
			ppc_addi(ppc_rarg1,ppc_rarg1,(u32)-4);
			ppc_call_and_jump(&ngen_LinkBlock_Dynamic_IC);
		}

		ngen_BlockCheckFail_stub=emit_GetCCPtr();
		{
			ppc_call_and_jump(&rdv_BlockCheckFail);
		}

		//Make _SURE_ this code is not overwriten !
		emit_SetBaseAddr();
		
#if !defined(__WIIU__)	// a debug dump on every boot; the Wii U build keeps SD writes down
		char file[512];
		snprintf(file, sizeof(file), "dynarec_%08X.bin", (u32)(uintptr_t)loop_code);
		char* path=GetEmuPath(file);

		FILE* f = fopen(path, "wb");
		free(path);

		if (f)
		{
			fwrite((void*)loop_code, 1, CODE_SIZE - emit_FreeSpace(), f);
			fclose(f);  // fclose flushes; explicit fflush is redundant
		}
#endif
		
    // CACHE COHERENCY
    make_address_range_executable((u8*)loop_code, (u8*)emit_GetCCPtr()-(u8*)loop_code);
	}

	loop_code();
}

void ngen_GetFeatures(ngen_features* dst)
{
	dst->InterpreterFallback=false;
	dst->OnlyDynamicEnds=false;
}
