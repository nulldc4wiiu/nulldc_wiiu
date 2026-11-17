// wii_driver_compile.inc: ngen_Compile, the per-op code generator.
// Part of wii_driver.cpp, which #includes it: not compiled on its own.


// ngen_CompileBlock: Main Block Compilation Loop
DynarecCodeEntry* ngen_Compile(DecodedBlock* block,bool force_checks)
{
	// Bail out early if there isn't enough space for a worst-case block
	if (emit_FreeSpace() < 16384) // 16*1024
		return 0;
#if defined(__WIIU__)
	// nor in the trampoline pool: a site that can't get a trampoline when it faults is a crash, so clear the cache
	// (and the pool with it) first; fastmem=2 gives a whole block's sites trampolines at once
	if (fastmem_on() && s_fm_pool_used + (g_wiiu_fm_prepatch ? FM_SITES_MAX * 128u : 32768u) > FM_POOL_SIZE)
		return 0;
	s_fm_sites_n = 0;
#endif

	// JIT_ALIGN preset: pad the entry to the next 32-byte L1 line so every
	// block start (hence every branch/link target landing on one) begins on a
	// clean cache-line boundary. CodeCache itself is 32-byte aligned, so the
	// low 5 bits of emit_GetCCPtr() equal the intra-line offset. The padding
	// sits between the previous block's tail — which ALWAYS ends in an
	// unconditional branch (ngen_End) or a cold fragment's `b done`, never a
	// fall-through — and this entry, so the nops are never executed; they also
	// fall within the line the previous block's flush already rounds up to, so
	// no extra I-cache handling is needed. <=7 nops/block against a 6 MB cache.
	if (get_jit_align_preset())
	{
		while (((u32)(uintptr_t)emit_GetCCPtr() & 31) != 0)
			ppc_emit(0x60000000);	// ppc nop (ori r0,r0,0)
	}

	DynarecCodeEntry* rv=(DynarecCodeEntry*)emit_GetCCPtr();

	ColdReset();          // mem-op cold (slow) fragments deferred to block end
	ngen_Begin(block,force_checks);

	// JIT_HOTBLOCKS: bump this block's execution counter. Emitted AFTER
	// ngen_Begin so it counts real body executions — past the SMC check and the
	// cycle-underflow exit — at a point where rarg0/rarg1 are still scratch
	// (same reasoning as the block-check guard inside ngen_Begin).
	const s32 hb = hotblocks_slot(block);
	if (hb>=0)
	{
		u32 lo = ppc_addr_high(ppc_rarg1,(void*)&s_hb_hits[hb]);
		ppc_lwz(ppc_rarg0,ppc_rarg1,lo);
		ppc_addi(ppc_rarg0,ppc_rarg0,1);
		ppc_stw(ppc_rarg0,ppc_rarg1,lo);
	}

	// jit_tlive: where the block's jcond is, if any (T stores before it are mirrored into r31)
	size_t jcond_at = (size_t)-1;
	if (g_exp_jit_tlive)
		for (size_t k = 0; k < block->oplist.size(); k++)
			if (block->oplist[k].op == shop_jcond)
			{
				jcond_at = k;
				break;
			}

	for (size_t i = 0; i < block->oplist.size(); i++)
	{
		shil_opcode* op=&block->oplist[i];

		// jit_tlive: every op that writes T or SR (or may change r31) ends the copy; a ppc_sh_store of T within
		// the op renews it
		{
			const auto writes = [](const shil_param &p) {
				return p.is_reg() && (p._reg == reg_sr_T || p._reg == reg_sr_status);
			};
			if (op->op == shop_ifb || op->op == shop_sync_sr || op->op == shop_jdyn || writes(op->rd) || writes(op->rd2))
				compile_state.t_in_djump = false;
			compile_state.t_copy_ok = g_exp_jit_tlive && jcond_at != (size_t)-1 && i < jcond_at;
		}

		// T-FORWARDING: latch whether the PREVIOUS op left sr_T in rarg0, then
		// clear, so this op starts clean and only its own binop_end can set the
		// flag again. Anything that runs between a producer and a consumer
		// therefore invalidates the forward automatically.
		const bool t_prev = compile_state.t_in_rarg0;
		compile_state.t_in_rarg0 = false;

		// JIT_CR0: may THIS op hand its condition to the block exit in CR0?
		// Only when it is the last op (nothing can run in between and clobber
		// CR0), the block exits on sr_T, and this op is the one writing sr_T.
		// has_jcond means the exit tests a computed value in ppc_djump instead,
		// so the CR0 path must stay out of its way.
		compile_state.t_exit_cr0 =
			get_jit_cr0_preset() &&
			!compile_state.has_jcond &&
			(i + 1 == block->oplist.size()) &&
			(block->BlockType == BET_Cond_0 || block->BlockType == BET_Cond_1) &&
			op->rd.is_reg() && op->rd._reg == reg_sr_T;

		switch(op->op)
		{

		case shop_readm:
			{
				void* fuct=0;
				bool isram=false;
				verify(op->rs1.is_imm() || op->rs1.is_r32i());

				// Land the loaded value DIRECTLY in rd's pinned PPC register when
				// possible, so the trailing ppc_sh_store(rrv0,rd) becomes a
				// store-to-self (elided) instead of an `ori rN,r3,0` move. The slow
				// MMIO call returns in rrv0, so that path copies rrv0->rdreg once.
				// 64-bit pair loads keep the rrv0:rrv1 ABI (handled below).
				u32 rdreg = ppc_rrv0;
#if STATIC_GPR_ALLOC
				if (op->flags!=8)
				{
					ppc_ireg rp=GetIntReg(op->rd._reg);
					if (rp!=ppc_rinvalid)
						rdreg=(u32)rp;
				}
#endif
				// Address source for the runtime fast path (set in the rs1-reg
				// branch below). Defaults to rarg0 (the const/imm + pair paths).
				u32 areg = ppc_rarg0;

				// FPU_PIN Phase B: when the destination is a pinned FR, load the
				// value STRAIGHT into the FPR (lfs/lfsx) instead of into a GPR and
				// then bouncing it through memory. Only for RUNTIME addresses
				// (rs1 reg): a compile-time-known address is either resolved to a
				// direct RAM pointer or dispatched through a C call that returns
				// in a GPR, so neither wants a float shape.
				// fmov.s = flags 4 scalar (rd float); fmov.d = flags 8 pair
				// (rd, rd+1 both pinned fr). xf-based pairs (unpinned) fall back.
				//
				// JIT_FMOV extends this to the fastmem path. Those shapes are
				// FIXED — rec_fastmem_patch() decodes them at DSI time — so the
				// float variants only get EMITTED while the preset is on, but
				// the decoder accepts them unconditionally (see the comment on
				// its float cases: a site outlives the switch that made it).
				bool fdirect=false;
				u32  fd0=0, fd1=0, fofs=0;
				if (get_fpu_pin_preset() && op->rs1.is_reg()
				    && (!fastmem_on() || get_jit_fmov_preset()))
				{
					if (op->flags==8)
					{
						ppc_freg a=GetFloatReg(op->rd._reg);
						ppc_freg b=GetFloatReg(op->rd._reg+1);
						if (a!=ppc_finvalid && b!=ppc_finvalid)
						{ fdirect=true; fd0=(u32)a; fd1=(u32)b; fofs=Sh4cntx.offset(op->rd._reg); }
					}
					else if (op->flags==4 && op->rd.is_r32f())
					{
						ppc_freg a=GetFloatReg(op->rd._reg);
						if (a!=ppc_finvalid)
						{ fdirect=true; fd0=(u32)a; fofs=Sh4cntx.offset(op->rd._reg); }
					}
				}

				if (op->rs1.is_imm())
				{
					void* ptr=_vmem_read_const(op->rs1._imm,isram,op->flags);
					if (isram)
					{
						if (op->flags==1)
						{
							ppc_lbz(rdreg,ppc_r4,ppc_addr_high(ppc_r4,ptr));
							ppc_extsbx(rdreg,rdreg,0);
						}
						else if (op->flags==2)
							ppc_lha(rdreg,ppc_r4,ppc_addr_high(ppc_r4,ptr));
						else if (op->flags==4)
							ppc_lwz(rdreg,ppc_r4,ppc_addr_high(ppc_r4,ptr));
						else
						{
							die("Invalid mem read size");
						}
					}
					else
					{
						ppc_li(ppc_rarg0,op->rs1._imm);
						fuct=ptr;
					}
				}
				else
				{
					// `areg` is the address SOURCE for the fast-path index/mask ops.
					// For a plain @Rn (rs3 null) with rs1 pinned, source the address
					// straight from its pinned reg -- no `ori rarg0,pinned,0` mov.
					// The fast path only READS areg (writing scratch), and the slow
					// path materializes rarg0 from it. Any offset (imm/reg) needs a
					// mutated address, so fall back to pre-loading rarg0 as before.
					areg = ppc_rarg0;
					if (op->rs3.is_imm())
					{
						verify(op->rs3.is_imm_s16());
						// addr = rs1 + imm -> addi can read pinned rs1 in place and
						// write scratch rarg0, skipping the `mr rarg0,rs1` load.
						u32 a1=src_or_load(op->rs1,ppc_rarg0);
						ppc_addi(ppc_rarg0,a1,op->rs3._imm);
					}
					else if (op->rs3.is_r32i())
					{
						// addr = rs1 + rs3 -> add reads both sources in place.
						u32 a1=src_or_load(op->rs1,ppc_rarg0);
						u32 a3=src_or_load(op->rs3,ppc_rarg1);
						ppc_addx(ppc_rarg0,a1,a3,0,0);
					}
					else if (op->rs3.is_null())
					{
#if STATIC_GPR_ALLOC
						// Only the scalar fast path threads areg; the 64-bit pair
						// path still reads rarg0, so keep pre-loading it there.
						ppc_ireg ap=(op->flags!=8 && op->rs1.is_reg())
						              ?GetIntReg(op->rs1._reg):ppc_rinvalid;
						if (ap!=ppc_rinvalid)
							areg=(u32)ap;		// address lives in pinned reg
						else
#endif
							ppc_sh_load(ppc_rarg0,op->rs1);
					}
					else
					{
						die("invalid rs3");
					}
				}

				if (!isram && fastmem_on())
				{
					// ---- FASTMEM: branchless access through the MMU window.
					// Shapes are FIXED — rec_fastmem_patch() decodes them at
					// DSI time (the EA scratch register is the shape
					// discriminator), so keep emission and decoder in sync.
					if (op->rs1.is_imm())
					{
						// Compile-time-known MMIO (fuct set above, address
						// already materialized in rarg0): call the handler
						// directly — never emit a faulting access for a
						// constant MMIO address. This is strictly cheaper
						// than the legacy always-miss 0x8C compare + cold
						// branch pair (think SB_ISTNRM poll loops).
						void* f=fuct;
						if (!f) f=(op->flags==1)?(void*)ReadMem8:(op->flags==2)?(void*)ReadMem16:(void*)ReadMem32;
						ppc_call(f);
						if (op->flags==1)      ppc_extsbx(rdreg,ppc_rrv0,0);
						else if (op->flags==2) ppc_extshx(rdreg,ppc_rrv0,0);
						else if (rdreg!=ppc_rrv0) ppc_ori(rdreg,ppc_rrv0,0);
					}
					else if (op->flags==8)
					{
						// Pair (fmov.d): address is always pre-loaded in
						// rarg0. EA scratch MUST be rarg2. word[addr]->rrv0
						// (high), word[addr+4]->rrv1 (low), matching the
						// ReadMem64 slow path register-for-register.
						fm_ea(ppc_rarg2,ppc_rarg0,0,true);	// EA = addr & 0x1FFFFFFF
						if (fdirect)
						{
							// JIT_FMOV: straight into the pinned FPRs. Same EA
							// scratch (rarg2) and same displacements, so the
							// decoder tells this from the integer pair only by
							// the opcode (lfs vs lwz).
							fm_note_float_site();
							ppc_lfs(fd0,ppc_rarg2,0);		// fault site A
							ppc_lfs(fd1,ppc_rarg2,4);		// fault site B
						}
						else
						{
							ppc_lwz(ppc_rrv0,ppc_rarg2,0);		// fault site A
							ppc_lwz(ppc_rrv1,ppc_rarg2,4);		// fault site B
						}
					}
					else
					{
						const u32 sz=op->flags;
						verify(sz==1||sz==2||sz==4);
						// Scalar: EA scratch MUST be rarg1. areg (pinned or
						// rarg0) still holds the original SH4 address at the
						// load — the fault decoder relies on that.
						fm_ea(ppc_rarg1,areg,sz<4);	// EA = addr & 0x1FFFFFFF
						if (sz<4)
							ppc_xori(ppc_rarg1,ppc_rarg1,4-sz);	// BE sub-word swizzle
						if (fdirect)
						{
							fm_note_float_site();
							FM_ACCESS(ppc_lfsx(fd0,ppc_r12,ppc_rarg1),ppc_lfs(fd0,ppc_rarg1,0));		// fault site (sz==4 only)
						}
						else if (sz==1)
						{
							FM_ACCESS(ppc_lbzx(rdreg,ppc_r12,ppc_rarg1),ppc_lbz(rdreg,ppc_rarg1,0));		// fault site
							ppc_extsbx(rdreg,rdreg,0);
						}
						else if (sz==2)
							FM_ACCESS(ppc_lhax(rdreg,ppc_r12,ppc_rarg1),ppc_lha(rdreg,ppc_rarg1,0));		// fault site
						else
							FM_ACCESS(ppc_lwzx(rdreg,ppc_r12,ppc_rarg1),ppc_lwz(rdreg,ppc_rarg1,0));		// fault site
					}
				}
				else if (!isram)
				{
					// Inline the _vmem_readt fast path (direct RAM/VRAM) for
					// runtime addresses. The MMIO path falls back to the C
					// dispatcher.
					//
					//   iirf = _vmem_MemInfo_ptr[addr>>24];
					//   ptr  = iirf & ~0x1F;
					//   if (ptr) { sh=iirf&0x1F; a=(addr<<sh)>>sh;
					//              if (sz<4) a^=4-sz; rv=*(T*)(ptr+a); }
					//   else  rv = ReadMem<sz>(addr);   // slow
					//
					// addr is in rarg0 on entry. Scratch: r0, rarg1, rarg2.
					if (op->flags==8)
					{
						// 64-bit pair load (fmov.d / sz=1 pair fmov). Replicates
						// the BE *(u64*)p load of _vmem_readt<u64>: the u64 is
						// returned in r3:r4 = high:low, so word[addr] -> rrv0
						// (-> rd) and word[addr+4] -> rrv1 (-> rd+1), matching
						// the ReadMem64 slow path register-for-register.
						ppc_rlwinmx(ppc_r0,ppc_rarg0,10,22,29,0);	// (addr>>24)*4
						u32 lo=ppc_addr_high(ppc_rarg1,(void*)&_vmem_MemInfo_ptr[0]);
						if (lo) ppc_addi(ppc_rarg1,ppc_rarg1,lo);
						ppc_lwzx(ppc_rarg1,ppc_rarg1,ppc_r0);		// rarg1 = iirf
						ppc_rlwinmx(ppc_rarg2,ppc_rarg1,0,0,26,0);	// ptr (≠r0)
						ppc_cmpli(ppc_cr0,ppc_rarg1,0x20,0);		// iirf < 0x20 ? (MMIO)

						// blt -> cold (forward, not-taken: fast path falls through).
						ppc_label* cold=ppc_CreateLabel();
						ppc_bcx(BO_TRUE,BI_CR0_LT,0,0,0);		// blt cold (MMIO)

						// --- fast direct path (fall-through) ---
						// shift count = iirf directly (ptr >=0x40-aligned -> iirf's
						// low 6 bits, which slw/srw use, equal the 5-bit shift field).
						ppc_slwx(ppc_rarg0,ppc_rarg0,ppc_rarg1,0);
						ppc_srwx(ppc_rarg0,ppc_rarg0,ppc_rarg1,0);	// mirror mask
						if (fdirect)
						{
							// FPU_PIN: load both words STRAIGHT into the pinned FPRs
							// (word[addr]->fd0, word[addr+4]->fd1); no GPR/rrv bounce.
							ppc_lfsx(fd0,ppc_rarg2,ppc_rarg0);	// word[addr]   -> fd0
							ppc_addi(ppc_rarg0,ppc_rarg0,4);
							ppc_lfsx(fd1,ppc_rarg2,ppc_rarg0);	// word[addr+4] -> fd1
						}
						else
						{
							ppc_lwzx(ppc_rarg3,ppc_rarg2,ppc_rarg0);	// word[addr]
							ppc_addi(ppc_rarg0,ppc_rarg0,4);
							ppc_lwzx(ppc_rrv1,ppc_rarg2,ppc_rarg0);	// word[addr+4] -> r4
							ppc_ori(ppc_rrv0,ppc_rarg3,0);			// r3 = word[addr]
						}

						ColdFrag& c=s_cold[s_cold_n++];
						c.beq=cold; c.done=emit_GetCCPtr();
						c.kind=1; c.fuct=fuct;
						c.fdreg=fdirect?(u8)fd0:0xFF; c.fofs=(u16)fofs;
					}
					else
					{
						const u32 sz=op->flags;
						verify(sz==1||sz==2||sz==4);

						// Fast path for the CACHED main-RAM window only (P1, top byte
						// 0x8C -> 0x8C000000..0x8CFFFFFF = 16 MB of mem_b). This is the
						// overwhelmingly common data region; MMIO, uncached mirrors and
						// every other region fall to the cold path.
						//
						//   if ((addr>>24)==0x8C) {
						//       native = addr + (mem_b.data - 0x8C000000);   // 1 addis
						//       if (sz<4) native ^= 4-sz;                    // BE swizzle
						//       rv = *(T*)native;                            // 1 load
						//   } else rv = ReadMem<sz>(addr);                   // cold
						//
						// mem_b.data is 64 KB-aligned and set by _vmem_reserve() during
						// init (BEFORE any block compiles), and 0x8C000000 is 64 KB-
						// aligned too, so the delta's low 16 bits are 0 -> a single
						// addis (adds delta>>16 << 16) forms the exact native address.
						const u32 delta   = (u32)(uintptr_t)mem_b.data - 0x8C000000u;
						const u32 delta_hi= (delta>>16)&0xFFFF;		// low 16 of delta are 0

						// top byte == 0x8C ?  (rlwinm r0,areg,8,24,31 == srwi 24)
						ppc_rlwinmx(ppc_r0,areg,8,24,31,0);		// r0 = addr>>24
						ppc_cmpi(ppc_cr0,ppc_r0,0x8C,0);		// == 0x8C ?

						// bne -> cold (forward, not-taken: RAM path falls through).
						ppc_label* cold=ppc_CreateLabel();
						ppc_bcx(BO_FALSE,BI_CR0_EQ,0,0,0);		// bne cold (not RAM)

						// --- fast direct RAM path (fall-through) ---
						// native = addr + delta  (single addis). rarg0 = native addr.
						ppc_addis(ppc_rarg0,areg,delta_hi);
						// big-endian sub-word swizzle folded into the address
						// (fdirect is float sz==4 only, so it never swizzles)
						if (sz<4)
							ppc_xori(ppc_rarg0,ppc_rarg0,4-sz);
						// rv = *(T*)native   (d-form, base=rarg0 != r0)
						if (fdirect)
							ppc_lfs(fd0,ppc_rarg0,0);	// FPU_PIN: straight into the FPR
						else if (sz==1)
						{
							ppc_lbz(rdreg,ppc_rarg0,0);
							ppc_extsbx(rdreg,rdreg,0);
						}
						else if (sz==2)
						{
							ppc_lhz(rdreg,ppc_rarg0,0);
							ppc_extshx(rdreg,rdreg,0);
						}
						else
							ppc_lwz(rdreg,ppc_rarg0,0);

						// done = join point; cold path (kind=0) rematerialises rarg0
						// from areg and calls ReadMem<sz>, then branches here.
						ColdFrag& c=s_cold[s_cold_n++];
						c.beq=cold; c.done=emit_GetCCPtr();
						c.kind=0; c.sz=(u8)sz; c.rdreg=(u8)rdreg;
						c.areg=(u8)areg; c.fuct=fuct;
						c.fdreg=fdirect?(u8)fd0:0xFF; c.fofs=(u16)fofs;
					}
				}

				// FPU_PIN direct path already landed the value in the pinned
				// FPR(s) via lfs/lfsx, so skip the GPR->context stores entirely.
				// (fdirect is only ever set on the legacy runtime path above; the
				// fastmem / const-imm / int paths keep rdreg and store normally.)
				if (!fdirect)
				{
					ppc_sh_store(rdreg,op->rd);

					if (op->flags==8)
					{
						ppc_sh_store(ppc_rrv1,op->rd._reg+1);
					}
				}
			}
			break;

		case shop_writem:
			{
				// Compute the FULL effective address first, THEN load the data.
				// (The old order loaded data first; the rs3 register-index path
				// uses rarg3 as scratch, which CLOBBERED the 64-bit data low
				// word for indexed pair stores like "fmov.d FRm,@(R0,Rn)".)
				//
				// `areg` is the address SOURCE for the scalar fast-path index/mask
				// ops. For a plain @Rn (rs3 null) with rs1 pinned we source it
				// straight from the pinned reg (no `ori rarg0,pinned,0` mov); the
				// fast path only reads areg, the slow path materializes rarg0.
				u32 areg = ppc_rarg0;
				if (op->rs3.is_imm())
				{
					verify(op->rs3.is_imm_s16());
					// addr = rs1 + imm: addi reads pinned rs1 in place -> rarg0.
					u32 a1=src_or_load(op->rs1,ppc_rarg0);
					ppc_addi(ppc_rarg0,a1,op->rs3._imm);
				}
				else if (op->rs3.is_r32i())
				{
					// addr = rs1 + rs3: add reads both sources in place -> rarg0.
					u32 a1=src_or_load(op->rs1,ppc_rarg0);
					u32 a3=src_or_load(op->rs3,ppc_rarg3);
					ppc_addx(ppc_rarg0,a1,a3,0,0);
				}
				else if (op->rs3.is_null())
				{
#if STATIC_GPR_ALLOC
					// Scalar only; the 64-bit pair path still reads rarg0.
					ppc_ireg ap=(op->flags!=8 && op->rs1.is_reg())
					              ?GetIntReg(op->rs1._reg):ppc_rinvalid;
					if (ap!=ppc_rinvalid)
						areg=(u32)ap;
					else
#endif
						ppc_sh_load(ppc_rarg0,op->rs1);
				}
				else
				{
					printf("rs3: %08X\n",op->rs3.type);
					die("invalid rs3");
				}

				// FPU_PIN Phase B (symmetric to shop_readm): when the data source
				// is a pinned FR, store STRAIGHT from the FPR (stfs/stfsx) instead
				// of bouncing it into a GPR first. fmov.s = flags 4 scalar,
				// fmov.d = flags 8 pair. JIT_FMOV extends it to fastmem, whose
				// float shapes rec_fastmem_patch() learned — see the shop_readm
				// gate above. Unlike the read side there is no rs1 check: a
				// store's address is always a runtime register here.
				bool fdirect=false;
				u32  fs0=0, fs1=0, fofs=0;
				if (get_fpu_pin_preset() && (!fastmem_on() || get_jit_fmov_preset()))
				{
					if (op->flags==8)
					{
						ppc_freg a=GetFloatReg(op->rs2._reg);
						ppc_freg b=GetFloatReg(op->rs2._reg+1);
						if (a!=ppc_finvalid && b!=ppc_finvalid)
						{ fdirect=true; fs0=(u32)a; fs1=(u32)b; fofs=Sh4cntx.offset(op->rs2._reg); }
					}
					else if (op->flags==4 && op->rs2.is_r32f())
					{
						ppc_freg a=GetFloatReg(op->rs2._reg);
						if (a!=ppc_finvalid)
						{ fdirect=true; fs0=(u32)a; fofs=Sh4cntx.offset(op->rs2._reg); }
					}
				}

				// Scalar data operand: if rs2 is pinned, the fast path can store
				// DIRECTLY from its pinned PPC reg (the fast path only reads the
				// data reg, never clobbers it), so we skip the pre-split
				// ppc_sh_load(rarg1,rs2) mov and load rarg1 only on the slow MMIO
				// path (where the WriteMem ABI requires data in rarg1). The 64-bit
				// pair keeps the rarg2:rarg3 ABI as before.
				u32 datareg = ppc_rarg1;
				if (fdirect)
				{
					// data stays in the pinned FPR(s) fs0[/fs1]; no GPR preload.
				}
				else if (op->flags==8)
				{
					ppc_sh_load(ppc_rarg2,op->rs2);
					ppc_sh_load(ppc_rarg3,op->rs2._reg+1);
				}
				else
				{
#if STATIC_GPR_ALLOC
					ppc_ireg dp=op->rs2.is_reg()?GetIntReg(op->rs2._reg):ppc_rinvalid;
					if (dp!=ppc_rinvalid)
						datareg=(u32)dp;		// store straight from pinned reg
					else
#endif
						ppc_sh_load(ppc_rarg1,op->rs2);	// scratch: load now
				}

				if (fastmem_on())
				{
					// ---- FASTMEM: branchless store through the MMU window.
					// Shapes are FIXED — rec_fastmem_patch() decodes them at
					// DSI time (EA scratch register = shape discriminator).
					// areg/datareg (or rarg0 + rarg2:rarg3 for pairs) still
					// hold the original values at the store — the fault
					// decoder and trampolines rely on that.
					if (op->flags==8)
					{
						// Pair: address in rarg0, data in rarg2:rarg3 =
						// high:low. EA scratch MUST be rarg1.
#if defined(__WIIU__)
						if (fdirect && fs1==fs0+1 && j2_emit(fs0,ppc_rarg0,true))
							;	// sq_guard: the store-queue test and both paths
						else
#endif
						{
						fm_ea(ppc_rarg1,ppc_rarg0,0,true);	// EA = addr & 0x1FFFFFFF
						if (fdirect)
						{
							// JIT_FMOV: straight from the pinned FPRs; the
							// rarg2:rarg3 preload above was skipped.
							fm_note_float_site();
							ppc_stfs(fs0,ppc_rarg1,0);		// fault site A
							ppc_stfs(fs1,ppc_rarg1,4);		// fault site B
						}
						else
						{
							ppc_stw(ppc_rarg2,ppc_rarg1,0);		// fault site A
							ppc_stw(ppc_rarg3,ppc_rarg1,4);		// fault site B
						}
						}
					}
#if defined(__WIIU__)
					else if (fdirect && op->flags==4 && j2_emit(fs0,areg,false))
						;	// sq_guard +4: the same for a single-float store
#endif
					else
					{
						const u32 sz=op->flags;
						verify(sz==1||sz==2||sz==4);
						// Scalar: EA scratch MUST be rarg2. No data masking
						// needed on the direct path — stb/sth store the low
						// byte/halfword by definition (the trampoline's C
						// call path re-applies the andi like ColdFrag does).
						fm_ea(ppc_rarg2,areg,sz<4);	// EA = addr & 0x1FFFFFFF
						if (sz<4)
							ppc_xori(ppc_rarg2,ppc_rarg2,4-sz);	// BE sub-word swizzle
						if (fdirect)
						{
							fm_note_float_site();
							FM_ACCESS(ppc_stfsx(fs0,ppc_r12,ppc_rarg2),ppc_stfs(fs0,ppc_rarg2,0));		// fault site (sz==4 only)
						}
						else if (sz==1)
							FM_ACCESS(ppc_stbx(datareg,ppc_r12,ppc_rarg2),ppc_stb(datareg,ppc_rarg2,0));		// fault site
						else if (sz==2)
							FM_ACCESS(ppc_sthx(datareg,ppc_r12,ppc_rarg2),ppc_sth(datareg,ppc_rarg2,0));		// fault site
						else
							FM_ACCESS(ppc_stwx(datareg,ppc_r12,ppc_rarg2),ppc_stw(datareg,ppc_rarg2,0));		// fault site
					}
				}
				else
				// Inline the _vmem_writet fast path (direct RAM/VRAM) for runtime
				// addresses. MMIO falls back to the C call.
				//
				//   iirf = _vmem_MemInfo_ptr[addr>>24];
				//   ptr  = iirf & ~0x1F;
				//   if (ptr) { sh=iirf&0x1F; a=(addr<<sh)>>sh;
				//              if (sz<4) a^=4-sz; *(T*)(ptr+a)=data; }
				//   else  WriteMem<sz>(addr,data);   // slow
				//
				// On entry rarg0=addr, rarg1=data (or rarg2:rarg3 = high:low for
				// 64-bit). addr/data MUST survive to the slow call.
				if (op->flags==8)
				{
					// 64-bit pair store (fmov.d / sz=1 pair fmov). Data is in
					// rarg2:rarg3 (r5:r6) = high:low — exactly the EABI registers
					// WriteMem64(u32,u64) wants, so the slow path needs no moves.
					// Direct path replicates the BE *(u64*)p store of
					// _vmem_writet<u64>: word[addr]=high, word[addr+4]=low.
					// Lookup may only use r0/rarg1 as scratch.
					ppc_rlwinmx(ppc_r0,ppc_rarg0,10,22,29,0);	// (addr>>24)*4
					u32 lo=ppc_addr_high(ppc_rarg1,(void*)&_vmem_MemInfo_ptr[0]);
					if (lo) ppc_addi(ppc_rarg1,ppc_rarg1,lo);
					ppc_lwzx(ppc_rarg1,ppc_rarg1,ppc_r0);		// rarg1 = iirf
					// Extract shift first. NOTE: ppc_andi is the RECORD form (andi.)
					// and clobbers CR0, so the MMIO compare MUST come AFTER it (and
					// before the rlwinm overwrites iirf in rarg1).
					ppc_andi(ppc_r0,ppc_rarg1,0x1F);		// r0 = shift (clobbers CR0)
					ppc_cmpli(ppc_cr0,ppc_rarg1,0x20,0);		// iirf < 0x20 ? (sets CR0 last)
					ppc_rlwinmx(ppc_rarg1,ppc_rarg1,0,0,26,0);	// rarg1 = ptr (≠r0; Rc=0)

					// blt -> cold (forward, not-taken: fast path falls through).
					ppc_label* cold=ppc_CreateLabel();
					ppc_bcx(BO_TRUE,BI_CR0_LT,0,0,0);		// blt cold (MMIO)

					// --- fast direct path (fall-through; addr masked here only) ---
					ppc_slwx(ppc_rarg0,ppc_rarg0,ppc_r0,0);
					ppc_srwx(ppc_rarg0,ppc_rarg0,ppc_r0,0);	// mirror mask
					if (fdirect)
					{
						// FPU_PIN: store both words STRAIGHT from the pinned FPRs
						// (fs0=high->word[addr], fs1=low->word[addr+4]).
						ppc_stfsx(fs0,ppc_rarg1,ppc_rarg0);	// word[addr]   = fs0
						ppc_addi(ppc_rarg0,ppc_rarg0,4);
						ppc_stfsx(fs1,ppc_rarg1,ppc_rarg0);	// word[addr+4] = fs1
					}
					else
					{
						ppc_stwx(ppc_rarg2,ppc_rarg1,ppc_rarg0);	// word[addr]   = high
						ppc_addi(ppc_rarg0,ppc_rarg0,4);
						ppc_stwx(ppc_rarg3,ppc_rarg1,ppc_rarg0);	// word[addr+4] = low
					}

					ColdFrag& c=s_cold[s_cold_n++];
					c.beq=cold; c.done=emit_GetCCPtr();
					c.kind=3;
					c.fdreg=fdirect?(u8)fs0:0xFF; c.fofs=(u16)fofs;
				}
				else
				{
					const u32 sz=op->flags;
					verify(sz==1||sz==2||sz==4);

					// r0 = (addr>>24)*4   (byte index into the void* table).
					// areg is the address source (rarg0, or rs1's pinned reg for a
					// plain @Rn); the index + mirror-mask READ it, writing scratch.
					ppc_rlwinmx(ppc_r0,areg,10,22,29,0);
					// rarg2 = &_vmem_MemInfo_ptr
					u32 lo=ppc_addr_high(ppc_rarg2,(void*)&_vmem_MemInfo_ptr[0]);
					if (lo) ppc_addi(ppc_rarg2,ppc_rarg2,lo);
					ppc_lwzx(ppc_rarg2,ppc_rarg2,ppc_r0);		// rarg2 = iirf
					// rarg3 = ptr = iirf & ~0x1F  (ptr NOT in r0: store-indexed
					// treats base r0 as literal zero, dropping the pointer)
					ppc_rlwinmx(ppc_rarg3,ppc_rarg2,0,0,26,0);
					// MMIO test on iirf directly (iirf<0x20 <=> ptr==0); keeps iirf
					// in rarg2 alive as the shift count below.
					ppc_cmpli(ppc_cr0,ppc_rarg2,0x20,0);		// iirf < 0x20 ?

					// blt -> cold (forward, not-taken: fast path falls through).
					ppc_label* cold=ppc_CreateLabel();
					ppc_bcx(BO_TRUE,BI_CR0_LT,0,0,0);		// blt cold (MMIO)

					// --- fast direct path (fall-through) ---
					// eff = (addr<<sh)>>sh into r0 (free after the index above);
					// shift count is iirf in rarg2 directly (ptr >=0x40-aligned ->
					// iirf's low 6 bits == the 5-bit field, no andi needed).
					ppc_slwx(ppc_r0,areg,ppc_rarg2,0);		// r0 = addr<<sh (reads areg)
					ppc_srwx(ppc_r0,ppc_r0,ppc_rarg2,0);		// r0 = (addr<<sh)>>sh
					if (sz<4)
						ppc_xori(ppc_r0,ppc_r0,4-sz);		// big-endian sub-word swizzle
					// *(T*)(ptr+eff) = data  (ptr=rarg3, eff=r0, data=datareg).
					// datareg is rs2's pinned reg (read-only here) or rarg1.
					// (fdirect is float sz==4 only, so it never swizzles.)
					if (fdirect)
						ppc_stfsx(fs0,ppc_rarg3,ppc_r0);	// FPU_PIN: straight from the FPR
					else if (sz==1)
						ppc_stbx(datareg,ppc_rarg3,ppc_r0);
					else if (sz==2)
						ppc_sthx(datareg,ppc_rarg3,ppc_r0);
					else
						ppc_stwx(datareg,ppc_rarg3,ppc_r0);

					ColdFrag& c=s_cold[s_cold_n++];
					c.beq=cold; c.done=emit_GetCCPtr();
					c.kind=2; c.sz=(u8)sz; c.datareg=(u8)datareg; c.areg=(u8)areg;
					c.fdreg=fdirect?(u8)fs0:0xFF; c.fofs=(u16)fofs;
				}
			}
			break;

		case shop_ifb:
			{
				const u16 sh4op=(u16)op->rs3._imm;

				// IFB_PROBE: bump this opcode's counter. rarg0/rarg1 are
				// scratch at a shil-op boundary, and this runs before the
				// bracket so it cannot disturb a flushed value.
				if (get_ifb_probe_preset())
				{
					u32* slot=ifb_probe_slot(sh4op);
					if (slot)
					{
						u32 lo=ppc_addr_high(ppc_rarg1,(void*)slot);
						ppc_lwz(ppc_rarg0,ppc_rarg1,lo);
						ppc_addi(ppc_rarg0,ppc_rarg0,1);
						ppc_stw(ppc_rarg0,ppc_rarg1,lo);
					}
				}

				// IFB_FLUSH: spill only what this opcode's handler actually
				// touches, on both axes independently — the GPRs named by its
				// allow-list entry, and the float file only if it can disturb
				// fr[]/xf[]. Preset off => the legacy everything-spill, which
				// is also what an unlisted opcode gets.
				IfbBracket br={IFB_GPR_ALL,true};
				if (get_ifb_flush_preset())
					br=ifb_bracket(sh4op);

				reg_flush_mask(br.gprs);
				if (br.fpu)
					reg_flush_all_fpu();		// self-gates on FPU_PIN

				if (op->rs1._imm)
				{
					ppc_li(ppc_rarg0,op->rs2._imm);
					ppc_sh_store(ppc_rarg0,reg_nextpc);
				}
				ppc_li(ppc_rarg0,op->rs3._imm);
				ppc_call(OpDesc[op->rs3._imm]->oph);

				if (br.fpu)
					reg_reload_all_fpu();
				reg_reload_mask(br.gprs);
			}
			break;
			
		case shop_jdyn:
			{
				if (op->rs2.is_imm())
				{
					// djump = rs1 + imm: read pinned rs1 in place into the addi/add,
					// so the `mr djump,rs1` load is folded into the arithmetic.
					u32 a1=src_or_load(op->rs1,ppc_djump);
					if (op->rs2.is_imm_s16())
					{
						ppc_addi(ppc_djump,a1,op->rs2._imm);
					}
					else
					{
						ppc_li(ppc_rarg0,op->rs2._imm);
						ppc_addx(ppc_djump,a1,ppc_rarg0,0,0);
					}
				}
				else
				{
					// djump = rs1 (no offset): a plain move is unavoidable.
					ppc_sh_load(ppc_djump,op->rs1);
				}
			}
			break;
			
		case shop_jcond:
			{
				compile_state.has_jcond=true;
				// Delay-slot branches capture T here, before the delay slot runs.
				// When the compare that produced it is the op right before this
				// one, it is still in rarg0 — take it from there instead of
				// reloading the value we just stored.
				if (compile_state.t_in_djump && op->rs1.is_reg() && op->rs1._reg==reg_sr_T)
					g_cnt_tlive++;	// jit_tlive: every T store since the producer also went to r31
				else if (t_prev && op->rs1.is_reg() && op->rs1._reg==reg_sr_T)
					ppc_ori(ppc_djump,ppc_rarg0,0);		// mr djump,rarg0
				else
					ppc_sh_load(ppc_djump,op->rs1);
				compile_state.t_in_djump=false;
			}
			break;
			
		case shop_mov64:
			{
				verify(op->rd.is_r64());
				verify(op->rs1.is_r64());

				// mov64 is always a float pair (FMT_F64: fmov.d / DR<->XD moves).
				// Under FPU_PIN move each word register-to-register (fmr) instead
				// of bouncing both words through GPRs + memory. fmove_reg emits
				// the minimal form per word and, with the preset off, degenerates
				// to the legacy lfs f0/stfs f0 scratch bounce (no GPR round-trip,
				// which is also strictly fewer insns than the old code).
				fmove_reg(op->rd._reg,   op->rs1._reg);
				fmove_reg(op->rd._reg+1, op->rs1._reg+1);
			}
			break;

		case shop_mov32:
			{
				verify(op->rd.is_r32());

				// fmov FRm,FRn (both float): move register-to-register. fmove_reg
				// picks fmr / stfs / lfs / scratch-bounce by how each end is
				// allocated. The MIXED cases (flds FRm,FPUL = float->int, fsts
				// FPUL,FRn = int->float, or imm->float) fall through to the GPR
				// path below, where the pinning-aware ppc_sh_load/ppc_sh_store
				// already bounce a pinned FR through its memory slot correctly.
				if (op->rd.is_r32f() && op->rs1.is_r32f())
				{
					fmove_reg(op->rd._reg,op->rs1._reg);
					break;
				}

				// jit_fconst (Review 3 K2): an immediate into a pinned FR (fldi0/fldi1, ~15% of DOA2's hottest block)
				// went li; stw slot; lfs fr,slot: the lfs hits the store it follows. The context slot still gets the
				// value, but the FPR loads it from a constant pool instead (append-only, never reset, so it outlives
				// code cache flushes)
				if (g_exp_jit_fconst && op->rs1.is_imm() && op->rd.is_r32f() && get_fpu_pin_preset())
				{
					const ppc_freg rf=GetFloatReg(op->rd._reg);
					const u32 *k=rf!=ppc_finvalid ? jit_fconst(op->rs1._imm) : nullptr;
					if (k)
					{
						ppc_li(ppc_rarg0,op->rs1._imm);
						ppc_stw(ppc_rarg0,ppc_contex,Sh4cntx.offset(op->rd._reg));
						const u32 lo=ppc_addr_high(ppc_rarg1,(void*)k);
						ppc_lfs(rf,ppc_rarg1,lo);
						g_cnt_fconst++;
						break;
					}
				}

				// Resolve dest: a pinned (integer) rd is written in place; a
				// non-pinned or float-typed rd uses rarg0 + a context store.
				ppc_ireg rdr = op->rd.is_r32i() ? GetIntReg(op->rd._reg) : ppc_rinvalid;
				u32 dst = (rdr!=ppc_rinvalid) ? (u32)rdr : (u32)ppc_rarg0;

				if (op->rs1.is_imm())
				{
					ppc_li(dst,op->rs1._imm);
				}
				else if (op->rs1.is_r32())
				{
					// reg -> reg. If rs1 is pinned, move/keep it directly.
					ppc_ireg rsr = op->rs1.is_r32i() ? GetIntReg(op->rs1._reg) : ppc_rinvalid;
					if (rsr!=ppc_rinvalid)
					{
						if ((u32)rsr!=dst)
							ppc_ori(dst,rsr,0);	// mr dst, rsr  (skipped if same reg)
					}
					else
					{
						ppc_sh_load(dst,op->rs1);
					}
				}
				else
				{
					die("Invalid mov32 size");
				}

				// Store back only when we used the scratch reg (non-pinned rd).
				if (dst==ppc_rarg0 && rdr==ppc_rinvalid)
					ppc_sh_store(ppc_rarg0,op->rd);
			}
			break;

		// Single-instruction binops — read/write pinned regs directly (binop3).
		// Single-instruction binops. When rs2 is a small immediate we fold it
		// straight into the PPC immediate-form instruction, skipping the
		// ppc_li(rarg1,imm) materialisation. addi/subi use a SIGNED 16-bit
		// field; andi./ori/xori use UNSIGNED 16-bit.
		case shop_add:
			bop_resolve_a_d(op);
			if (op->rs2.is_imm_s16())
				ppc_addi(bop_d,bop_a,op->rs2._imm);		// d = a + imm
			else
				{ bop_resolve_b(op); ppc_addx(bop_d,bop_a,bop_b,0,0); }
			binop3_end(op);
			break;
		case shop_sub:
			// SH4 sub = rs1 - rs2; fold imm as a + (-imm) when -imm fits s16.
			bop_resolve_a_d(op);
			if (op->rs2.is_imm() && is_s16(0u-op->rs2._imm))
				ppc_addi(bop_d,bop_a,0u-op->rs2._imm);		// d = a - imm
			else
				{ bop_resolve_b(op); ppc_subfx(bop_d,bop_b,bop_a,0,0); }
			binop3_end(op);
			break;

		case shop_or:
			bop_resolve_a_d(op);
			if (op->rs2.is_imm_u16())
				ppc_ori(bop_d,bop_a,op->rs2._imm);
			else
				{ bop_resolve_b(op); ppc_orx(bop_d,bop_a,bop_b,0); }
			binop3_end(op);
			break;
		case shop_and:
			bop_resolve_a_d(op);
			if (op->rs2.is_imm_u16())
				ppc_andi(bop_d,bop_a,op->rs2._imm);		// andi. (Rc=1, harmless)
			else
				{ bop_resolve_b(op); ppc_andx(bop_d,bop_a,bop_b,0); }
			binop3_end(op);
			break;
		case shop_xor:
			bop_resolve_a_d(op);
			if (op->rs2.is_imm_u16())
				ppc_xori(bop_d,bop_a,op->rs2._imm);
			else
				{ bop_resolve_b(op); ppc_xorx(bop_d,bop_a,bop_b,0); }
			binop3_end(op);
			break;

		// Shifts: fold a constant shift amount (0..31) into rlwinm/srawi.
		case shop_shl:
			bop_resolve_a_d(op);
			if (op->rs2.is_imm())
			{
				u32 s=op->rs2._imm&0x1F;
				ppc_rlwinmx(bop_d,bop_a,s,0,31-s,0);		// slwi: rotl s, mask [0..31-s]
			}
			else
				{ bop_resolve_b(op); ppc_slwx(bop_d,bop_a,bop_b,0); }
			binop3_end(op);
			break;
		case shop_shr:
			bop_resolve_a_d(op);
			if (op->rs2.is_imm())
			{
				u32 s=op->rs2._imm&0x1F;
				ppc_rlwinmx(bop_d,bop_a,(32-s)&31,s,31,0);	// srwi: rotl 32-s, mask [s..31]
			}
			else
				{ bop_resolve_b(op); ppc_srwx(bop_d,bop_a,bop_b,0); }
			binop3_end(op);
			break;
		case shop_sar:
			bop_resolve_a_d(op);
			if (op->rs2.is_imm())
				ppc_srawix(bop_d,bop_a,op->rs2._imm&0x1F,0);	// srawi
			else
				{ bop_resolve_b(op); ppc_srawx(bop_d,bop_a,bop_b,0); }
			binop3_end(op);
			break;
		case shop_mul_i32:
			bop_resolve_a_d(op);
			if (op->rs2.is_imm_s16())
				ppc_mulli(bop_d,bop_a,op->rs2._imm);		// d = a * imm
			else
				{ bop_resolve_b(op); ppc_mullwx(bop_d,bop_a,bop_b,0,0); }
			binop3_end(op);
			break;


		// Single-instruction FP binops: with FPU_PIN, fbop_resolve reads pinned
		// fr operands in place and writes a pinned fr dest in place, collapsing
		// the whole op to ONE instruction (was lfs/lfs/op/stfs). Preset off ->
		// fop_a/fop_b are farg0/farg1 scratch loads and fbop_end stores back,
		// exactly the legacy binop_start_fpu/binop_end_fpu sequence.
		case shop_fadd: fbop_resolve(op); ppc_faddsx(fop_d,fop_a,fop_b,0); fbop_end(op); break;
		case shop_fsub: fbop_resolve(op); ppc_fsubsx(fop_d,fop_a,fop_b,0); fbop_end(op); break;
		case shop_fmul: fbop_resolve(op); ppc_fmulsx(fop_d,fop_a,fop_b,0); fbop_end(op); break;
		case shop_fdiv: fbop_resolve(op); ppc_fdivsx(fop_d,fop_a,fop_b,0); fbop_end(op); break;

		// --- Additional native integer binops ---------------------------------
		case shop_mul_u16:
			// rd = (u16)r1 * (u16)r2  (low 32 bits). Zero-extend both, mullw.
			binop_start(op);
			ppc_rlwinmx(ppc_rarg0,ppc_rarg0,0,16,31,0);	// clrlwi rarg0,rarg0,16
			ppc_rlwinmx(ppc_rarg1,ppc_rarg1,0,16,31,0);	// clrlwi rarg1,rarg1,16
			ppc_mullwx(ppc_rarg0,ppc_rarg0,ppc_rarg1,0,0);
			binop_end(op);
			break;
		case shop_mul_s16:
			// rd = (s16)r1 * (s16)r2  (low 32 bits). Sign-extend both, mullw.
			binop_start(op);
			ppc_extshx(ppc_rarg0,ppc_rarg0,0);
			ppc_extshx(ppc_rarg1,ppc_rarg1,0);
			ppc_mullwx(ppc_rarg0,ppc_rarg0,ppc_rarg1,0,0);
			binop_end(op);
			break;

		// --- 64-bit multiply: low -> rd (macl) / high -> rd2 (mach) -----------
		// NOTE: the decoder puts the high word in op->rd2 (reg_mach), NOT in
		// op->rd._reg+1. reg_mach actually PRECEDES reg_macl in the enum, so
		// rd._reg+1 == reg_pr — the old code corrupted PR and never wrote mach,
		// which broke booting.
		case shop_mul_u64:
			// 64-bit unsigned product of two 32-bit values.
			binop_start(op);
			// high word first (mulhwu) since low word overwrites an input reg.
			ppc_mulhwux(ppc_rarg2,ppc_rarg0,ppc_rarg1,0);	// hi = mulhwu(r1,r2)
			ppc_mullwx(ppc_rarg0,ppc_rarg0,ppc_rarg1,0,0);	// lo = mullw(r1,r2)
			ppc_sh_store(ppc_rarg0,op->rd);
			ppc_sh_store(ppc_rarg2,op->rd2);
			break;
		case shop_mul_s64:
			// 64-bit signed product of two 32-bit values.
			binop_start(op);
			ppc_mulhwx(ppc_rarg2,ppc_rarg0,ppc_rarg1,0);	// hi = mulhw(r1,r2)
			ppc_mullwx(ppc_rarg0,ppc_rarg0,ppc_rarg1,0,0);	// lo = mullw(r1,r2)
			ppc_sh_store(ppc_rarg0,op->rd);
			ppc_sh_store(ppc_rarg2,op->rd2);
			break;

		// --- 32-bit division (matched ROTCL/DIV1 idiom): quo->rd, rem->rd2 ----
		// Broadway has hardware divide; remainder = dividend - quo*divisor.
		// Division by zero gives an undefined register result on PPC (no trap),
		// which is fine — matched sequences are compiler-generated divisions.
		// DIV32_FIX: the decoder emitted the 64/32 one's-complement forms (rs3 =
		// the high word, T bit 31 = the quotient sign). Broadway has no 64-bit
		// divide, so call the canonical C bodies -- directly, NOT through the
		// default: case: there reg_flush_all/reg_reload_all bracket the call,
		// and the results ppc_sh_store'd into pinned registers (a bare `mr`)
		// would be overwritten by the reload with stale context values. These
		// three are pure functions and pinned GPRs are callee-saved, so no
		// bracket is needed. Rare ops (matched compiler divides), so the call
		// costs nothing measurable against the 64 guest ops it replaces.
		// With DIV32_FIX off: the legacy native 32/32 cases below.
		case shop_div32u:
			if (get_div32_fix_preset()) { shil_chf[op->op](op); break; }
			binop_start(op);				// rarg0=dividend, rarg1=divisor
			ppc_divwux(ppc_rarg2,ppc_rarg0,ppc_rarg1,0,0);	// quo = divwu
			ppc_mullwx(ppc_rarg3,ppc_rarg2,ppc_rarg1,0,0);	// quo*divisor
			ppc_subfx(ppc_rarg3,ppc_rarg3,ppc_rarg0,0,0);	// rem = dividend - quo*divisor
			ppc_sh_store(ppc_rarg2,op->rd);
			ppc_sh_store(ppc_rarg3,op->rd2);
			break;
		case shop_div32s:
			if (get_div32_fix_preset()) { shil_chf[op->op](op); break; }
			// divw truncates toward zero (legacy 32/32 form).
			binop_start(op);
			ppc_divwx(ppc_rarg2,ppc_rarg0,ppc_rarg1,0,0);	// quo = divw
			ppc_mullwx(ppc_rarg3,ppc_rarg2,ppc_rarg1,0,0);
			ppc_subfx(ppc_rarg3,ppc_rarg3,ppc_rarg0,0,0);
			ppc_sh_store(ppc_rarg2,op->rd);
			ppc_sh_store(ppc_rarg3,op->rd2);
			break;
		case shop_div32p2:
			// DIV32_FIX: T may carry the quotient sign in bit 31, which the
			// T-1 mask below cannot handle -- canonical body.
			if (get_div32_fix_preset()) { shil_chf[op->op](op); break; }
			// rd = T ? a : a-b  (non-restoring remainder fixup).
			// PRECONDITION: T (rs3) is 0/1 — guaranteed because the decoder
			// emits "and T,quo,1" immediately before this op. mask = T-1 maps
			// 0 -> 0xFFFFFFFF (apply b) and 1 -> 0 (keep a). Branchless.
			binop_start(op);				// rarg0=a, rarg1=b
			ppc_sh_load(ppc_rarg2,op->rs3);			// T
			ppc_addi(ppc_rarg2,ppc_rarg2,-1);		// mask = T-1
			ppc_andx(ppc_rarg1,ppc_rarg1,ppc_rarg2,0);	// b &= mask
			ppc_subfx(ppc_rarg0,ppc_rarg1,ppc_rarg0,0,0);	// a -= (T ? 0 : b)
			binop_end(op);
			break;

		// --- Shifts with dynamic SH4 semantics (branchless) -------------------
		case shop_ror:
			// rd = rotr(r1, r2&31). PPC rotates left; rotl by (32-(r2&31)).
			// rlwnm rotate amount is taken from rB[27:31] (mod 32), so passing
			// 32 when (r2&31)==0 is harmless (32 mod 32 == 0).
			binop_start(op);
			ppc_rlwinmx(ppc_rarg1,ppc_rarg1,0,27,31,0);	// rarg1 = r2 & 0x1F
			ppc_subfic(ppc_rarg1,ppc_rarg1,32);			// rarg1 = 32 - (r2&31)
			ppc_rlwnmx(ppc_rarg0,ppc_rarg0,ppc_rarg1,0,31,0);	// rotl, full mask
			binop_end(op);
			break;
#if defined(__WIIU__)
		// SH4 dynamic shifts with a branch on the amount's sign, which is the same every time at most sites. Going right
		// the amount is (~r2 & 0x1F) + 1, 1..32, and srw/sraw treat 32 properly (0 / the sign), so the zero case the
		// branchless forms below spend ten instructions on needs nothing.
		case shop_shld:
		case shop_shad:
			{
				binop_start(op);				// rarg0=r1, rarg1=r2
				ppc_cmpi(ppc_cr0,ppc_rarg1,0,0);
				ppc_label* right=ppc_CreateLabel();
				ppc_bcx(BO_TRUE,BI_CR0_LT,0,0,0);		// blt right
				ppc_rlwinmx(ppc_rarg2,ppc_rarg1,0,27,31,0);	// r2 & 0x1F
				ppc_slwx(ppc_rarg0,ppc_rarg0,ppc_rarg2,0);
				ppc_label* done=ppc_CreateLabel();
				ppc_bcx(BO_ALWAYS,BI_CR0_LT,0,0,0);		// b done
				right->MarkLabel();
				ppc_norx(ppc_rarg2,ppc_rarg1,ppc_rarg1,0);	// ~r2
				ppc_rlwinmx(ppc_rarg2,ppc_rarg2,0,27,31,0);
				ppc_addi(ppc_rarg2,ppc_rarg2,1);		// 1..32
				if (op->op==shop_shld)
					ppc_srwx(ppc_rarg0,ppc_rarg0,ppc_rarg2,0);
				else
					ppc_srawx(ppc_rarg0,ppc_rarg0,ppc_rarg2,0);
				done->MarkLabel();
				binop_end(op);
			}
			break;
#else
		case shop_shld:
			// SH4 logical dynamic shift (branchless):
			//   left  = r1 << (r2 & 0x1F)
			//   rcnt  = (-r2) & 0x1F ;  if rcnt==0 force 32 so srw yields 0
			//   right = r1 >> rcnt
			//   res   = (r2 >= 0) ? left : right
			// (When r2<0 and (r2&31)==0 SH4 yields 0; srw by 32 gives 0.)
			{
				binop_start(op);				// rarg0=r1, rarg1=r2
				// left = r1 << (r2 & 0x1F)
				ppc_rlwinmx(ppc_rarg2,ppc_rarg1,0,27,31,0);	// rarg2 = r2 & 0x1F
				ppc_slwx(ppc_rarg3,ppc_rarg0,ppc_rarg2,0);	// rarg3 = left
				// rcnt = (-r2) & 0x1F
				ppc_negx(ppc_rarg2,ppc_rarg1,0,0);		// -r2
				ppc_rlwinmx(ppc_rarg2,ppc_rarg2,0,27,31,0);	// rcnt = (-r2)&0x1F
				// force 32 when rcnt==0:  isZero=(rcnt-1)>>31 ; rcnt += isZero*32
				ppc_addi(ppc_r0,ppc_rarg2,-1);			// r0 = rcnt-1
				ppc_rlwinmx(ppc_r0,ppc_r0,1,31,31,0);		// r0 = (rcnt==0)?1:0  (sign bit -> bit31)
				ppc_rlwinmx(ppc_r0,ppc_r0,5,0,31,0);		// r0 <<= 5  -> 32 when zero
				ppc_orx(ppc_rarg2,ppc_rarg2,ppc_r0,0);		// rcnt |= 32 when zero
				ppc_srwx(ppc_rarg0,ppc_rarg0,ppc_rarg2,0);	// rarg0 = right
				// select: mask = (r2<0)? ~0 : 0
				ppc_srawix(ppc_r0,ppc_rarg1,31,0);
				ppc_andx(ppc_rarg0,ppc_rarg0,ppc_r0,0);		// right & mask
				ppc_andcx(ppc_rarg3,ppc_rarg3,ppc_r0,0);	// left & ~mask
				ppc_orx(ppc_rarg0,ppc_rarg0,ppc_rarg3,0);
				binop_end(op);
			}
			break;
		case shop_shad:
			// SH4 arithmetic dynamic shift (branchless):
			//   left  = r1 << (r2 & 0x1F)
			//   rcnt  = (-r2) & 0x1F ;  if rcnt==0 force 31 (sign fill)
			//   right = (s32)r1 >> rcnt
			//   res   = (r2 >= 0) ? left : right
			{
				binop_start(op);				// rarg0=r1, rarg1=r2
				ppc_rlwinmx(ppc_rarg2,ppc_rarg1,0,27,31,0);	// r2 & 0x1F
				ppc_slwx(ppc_rarg3,ppc_rarg0,ppc_rarg2,0);	// rarg3 = left
				// rcnt = (-r2) & 0x1F
				ppc_negx(ppc_rarg2,ppc_rarg1,0,0);
				ppc_rlwinmx(ppc_rarg2,ppc_rarg2,0,27,31,0);	// rcnt
				// force 31 when rcnt==0:  isZero=(rcnt-1)>>31 ; rcnt += isZero*31
				ppc_addi(ppc_r0,ppc_rarg2,-1);
				ppc_rlwinmx(ppc_r0,ppc_r0,1,31,31,0);		// (rcnt==0)?1:0
				ppc_mulli(ppc_r0,ppc_r0,31);			// 31 when zero else 0
				ppc_addx(ppc_rarg2,ppc_rarg2,ppc_r0,0,0);	// rcnt = 31 in zero-case
				ppc_srawx(ppc_rarg0,ppc_rarg0,ppc_rarg2,0);	// rarg0 = right (arithmetic)
				// select on sign of r2
				ppc_srawix(ppc_r0,ppc_rarg1,31,0);
				ppc_andx(ppc_rarg0,ppc_rarg0,ppc_r0,0);
				ppc_andcx(ppc_rarg3,ppc_rarg3,ppc_r0,0);
				ppc_orx(ppc_rarg0,ppc_rarg0,ppc_rarg3,0);
				binop_end(op);
			}
			break;
#endif

		// --- Carry / overflow arithmetic (JIT_CARRY preset) -------------------
		// All of these used to be shop_ifb call-outs because SHIL had no way to
		// say "produce a value AND the new T bit". Scratch budget: sources go
		// to rarg0/rarg1, the T<->XER shuffle uses rarg2, the dest is a pinned
		// GPR or rarg3 — no two of those can alias.

		case shop_adc:		// addc Rm,Rn : Rn = Rn + Rm + T ; T = carry
			{
				u32 a=src_or_load(op->rs1,ppc_rarg0);
				u32 b=src_or_load(op->rs2,ppc_rarg1);
				u32 d=carry_dest(op);
				emit_T_to_ca(ppc_rarg2,false);
				ppc_addex(d,a,b,0,0);			// adde d,a,b
				emit_ca_to_T(ppc_rarg2,false);
				carry_dest_store(op,d);
			}
			break;

		case shop_sbc:		// subc Rm,Rn : Rn = Rn - Rm - T ; T = borrow
			{
				// subfe d,b,a computes ~b + a + CA = a - b - 1 + CA, so CA
				// carries the INVERTED T going in and comes back as "no
				// borrow" — hence invert on both ends.
				u32 a=src_or_load(op->rs1,ppc_rarg0);
				u32 b=src_or_load(op->rs2,ppc_rarg1);
				u32 d=carry_dest(op);
				emit_T_to_ca(ppc_rarg2,true);
				ppc_subfex(d,b,a,0,0);			// subfe d,b,a
				emit_ca_to_T(ppc_rarg2,true);
				carry_dest_store(op,d);
			}
			break;

		case shop_negc:		// negc Rm,Rn : Rn = 0 - Rm - T ; T = borrow
			{
				// rs1 is Rm here (DM_NEGC), rs2 is the T input which
				// emit_T_to_ca reads straight from the context.
				u32 a=src_or_load(op->rs1,ppc_rarg0);
				u32 d=carry_dest(op);
				emit_T_to_ca(ppc_rarg2,true);
				ppc_subfzex(d,a,0,0);			// subfze d,a  = ~a + CA
				emit_ca_to_T(ppc_rarg2,true);
				carry_dest_store(op,d);
			}
			break;

		case shop_addv:		// addv Rm,Rn : Rn = Rn + Rm ; T = signed overflow
			{
				u32 a=src_or_load(op->rs1,ppc_rarg0);
				u32 b=src_or_load(op->rs2,ppc_rarg1);
				u32 d=carry_dest(op);
				ppc_addx(d,a,b,1,0);			// addo d,a,b
				emit_ov_to_T(ppc_rarg2);
				carry_dest_store(op,d);
			}
			break;

		case shop_subv:		// subv Rm,Rn : Rn = Rn - Rm ; T = signed overflow
			{
				u32 a=src_or_load(op->rs1,ppc_rarg0);
				u32 b=src_or_load(op->rs2,ppc_rarg1);
				u32 d=carry_dest(op);
				ppc_subfx(d,b,a,1,0);			// subfo d,b,a = a - b
				emit_ov_to_T(ppc_rarg2);
				carry_dest_store(op,d);
			}
			break;

		case shop_cmpstr:	// cmp/str Rm,Rn : T = any byte lane is equal
			{
				// Classic has-zero-byte test on (Rn ^ Rm):
				//   x = (t - 0x01010101) & ~t ; rotl 1 ; & 0x01010101 ; != 0
				// The mask has its MSB clear, so the `and.` result can never be
				// negative and CR0[GT] is exactly "non-zero".
				u32 a=src_or_load(op->rs1,ppc_rarg0);
				u32 b=src_or_load(op->rs2,ppc_rarg1);

				ppc_xorx(ppc_rarg2,a,b,0);			// rarg2 = t
				ppc_li(ppc_rarg3,0x01010101);
				ppc_subfx(ppc_r0,ppc_rarg3,ppc_rarg2,0,0);	// r0 = t - const
				ppc_andcx(ppc_r0,ppc_r0,ppc_rarg2,0);		// r0 &= ~t
				ppc_rlwinmx(ppc_r0,ppc_r0,1,0,31,0);		// rotl 1
				ppc_andx(ppc_r0,ppc_r0,ppc_rarg3,1);		// and. -> CR0
				emit_cr0_bit_to_rarg0(BI_CR0_GT);
				binop_end(op);					// rd is sr_T
			}
			break;

		case shop_swaplb:	// swap.b Rm,Rn : swap the two LOW bytes
			{
				// rlwimi rotate n places source bit (i+n)%32 into dest bit i:
				// bits 16..23 come from 24..31 (n=8), 24..31 from 16..23 (n=24).
				// Built in scratch so a dest that aliases the source is safe.
				u32 s=src_or_load(op->rs1,ppc_rarg0);
				ppc_rlwinmx(ppc_rarg1,s,0,0,15,0);	// keep the upper half
				ppc_rlwimix(ppc_rarg1,s,8,16,23,0);
				ppc_rlwimix(ppc_rarg1,s,24,24,31,0);
				ppc_sh_store(ppc_rarg1,op->rd);
			}
			break;

		case shop_div1:		// div1 Rm,Rn : one non-restoring division step
			{
				// q_new = q ^ oldQ ^ CA ^ 1, where q = MSB(Rn) before the
				// shift and CA is the carry out of the unified add:
				//     sel  = oldQ ^ M          (1 => add Rm, 0 => subtract)
				//     rmx  = sel ? Rm : ~Rm    (mask = sel-1)
				//     cin  = !sel
				//     Rn   = ((Rn<<1)|T) + rmx + cin
				// The M term cancels out of the usual `q ^ M ^ carry ^ !sel`
				// because sel already carries it. T = (q_new == M).
				//
				// Rm is captured into rmx BEFORE Rn is shifted, so `div1 Rn,Rn`
				// behaves like the hardware.
				const u32 t0=ppc_rarg0;		// SR.status, live to the end
				const u32 t1=ppc_rarg1;		// M, live to the T test
				const u32 t2=ppc_rarg2;		// oldQ, then general scratch
				const u32 t3=ppc_rarg3;		// q accumulator
				const u32 t4=ppc_r0;		// rmx
				const u32 t5=ppc_rarg4;		// sel

				// Rn is updated in place when it is pinned (rd == rs1 always
				// holds for DM_ADC), else through rarg7.
				ppc_ireg dpin=GetIntReg(op->rd._reg);
				u32 A;
				bool a_scratch=false;
				if (dpin!=ppc_rinvalid && op->rs1.is_reg() && op->rs1._reg==op->rd._reg)
					A=(u32)dpin;
				else
				{
					ppc_sh_load(ppc_rarg7,op->rs1);
					A=ppc_rarg7;
					a_scratch=true;
				}
				u32 B=src_or_load(op->rs2,ppc_rarg6);

				ppc_sh_load(t0,reg_sr_status);
				ppc_rlwinmx(t1,t0,23,31,31,0);		// M    = SR value bit 9
				ppc_rlwinmx(t2,t0,24,31,31,0);		// oldQ = SR value bit 8
				ppc_xorx(t5,t2,t1,0);			// sel  = oldQ ^ M
				ppc_addi(t4,t5,-1);			// mask = ~0 when sel==0
				ppc_xorx(t4,B,t4,0);			// rmx  = Rm ^ mask
				ppc_xori(t5,t5,1);			// cin  = !sel
				ppc_rlwinmx(t5,t5,29,2,2,0);		// -> XER[CA]
				ppc_mtxer(t5);				// (nothing below touches XER
									//  until the adde)

				ppc_rlwinmx(t3,A,1,31,31,0);		// q = MSB(Rn), pre-shift
				ppc_xorx(t3,t3,t2,0);			// q ^= oldQ

				ppc_sh_load(t2,reg_sr_T);
				ppc_rlwinmx(A,A,1,0,30,0);		// Rn <<= 1
				ppc_orx(A,A,t2,0);			// Rn |= T
				ppc_addex(A,A,t4,0,0);			// Rn += rmx + cin

				ppc_mfxer(t2);
				ppc_rlwinmx(t2,t2,3,31,31,0);		// CA
				ppc_xorx(t3,t3,t2,0);			// q ^= CA
				ppc_xori(t3,t3,1);			// q_new = q ^ oldQ ^ CA ^ 1
				ppc_xorx(t2,t3,t1,0);
				ppc_xori(t2,t2,1);			// T = (q_new == M)
				ppc_sh_store(t2,reg_sr_T);
				ppc_rlwimix(t0,t3,8,23,23,0);		// SR.Q (value bit 8) = q_new
				ppc_sh_store(t0,reg_sr_status);

				if (a_scratch)
					ppc_sh_store(A,op->rd);
			}
			break;

		// --- Multiply-accumulate (JIT_MAC preset) -----------------------------
		// mac.l / mac.w. The two @Rm+/@Rn+ reads are NOT here — the decoder
		// issues them as ordinary shop_readm ops (DM_MAC), which is most of the
		// win: on the old shop_ifb path they went through the interpreter's
		// ReadMem and never saw fastmem. What is left is the 64-bit accumulate.
		//
		// Shape, identical for both widths:
		//     r3..r7 = a, b, old MACL, old MACH, SR.status   (kept live)
		//     r8..r11 = the S==0 result, stored unconditionally
		//     then, only if SR.S is set, call sh4_mac_l/w with r3..r7 to redo
		//     it properly from the ORIGINAL accumulator and overwrite.
		// Passing the old accumulator to the helper instead of letting it read
		// the context is what lets the store happen before the test, so the
		// common path needs one conditional branch and no unconditional one.
		case shop_mac_l:
		case shop_mac_w:
			{
				const bool is_l = (op->op==shop_mac_l);

				ppc_sh_load(ppc_rarg0,op->rs1);			// a
				ppc_sh_load(ppc_rarg1,op->rs2);			// b
				ppc_sh_load(ppc_rarg2,reg_macl);		// old MACL
				ppc_sh_load(ppc_rarg3,reg_mach);		// old MACH
				ppc_sh_load(ppc_rarg4,op->rs3);			// SR.status

				// Product, sign-extended to 64 bits. mac.w's operands are
				// already sign-extended 16-bit values (shop_readm size 2 emits
				// lha), so mullw gives the whole signed product and the high
				// word is just its sign.
				ppc_mullwx(ppc_rarg5,ppc_rarg0,ppc_rarg1,0,0);
				if (is_l)
					ppc_mulhwx(ppc_rarg6,ppc_rarg0,ppc_rarg1,0);
				else
					ppc_srawix(ppc_rarg6,ppc_rarg5,31,0);

				ppc_addcx(ppc_rarg7,ppc_rarg2,ppc_rarg5,0,0);	// MACL + lo
				ppc_addex(ppc_r11,ppc_rarg3,ppc_rarg6,0,0);	// MACH + hi + CA
				ppc_sh_store(ppc_rarg7,op->rd);			// -> MACL
				ppc_sh_store(ppc_r11,op->rd2);			// -> MACH

				// SR.S is value bit 1. Saturation mode is vanishingly rare —
				// rare enough that the old interpreter died on it — so this is
				// a predicted-not-taken forward branch over a call.
				ppc_andi(ppc_r0,ppc_rarg4,2);
				ppc_label* done=ppc_CreateLabel();
				ppc_bcx(BO_TRUE,BI_CR0_EQ,0,0,0);		// beq -> done
				if (is_l)
					ppc_call(sh4_mac_l);
				else
					ppc_call(sh4_mac_w);
				done->MarkLabel();
			}
			break;

		// --- Integer comparisons / test ---------------------------------------
		// (set*/test do their own operand load + immediate folding internally.)
		case shop_test:	// rd = (r1 & r2) == 0
			{
				// and./andi. read any reg and write a scratch dest (rarg0), so we
				// can source pinned rs1/rs2 in place and skip the loading mr's. The
				// result (CR0) goes through mfcr->rarg0; rarg0 as the and dest is
				// dead-on-arrival so clobbering a pinned source is impossible.
				u32 a1=src_or_load(op->rs1,ppc_rarg0);

				if (op->rs2.is_imm_u16())
					ppc_andi(ppc_rarg0,a1,op->rs2._imm);		// andi. sets CR0
				else
				{
					u32 b;
					if (op->rs2.is_imm()) { ppc_li(ppc_rarg1,op->rs2._imm); b=ppc_rarg1; }
					else b=src_or_load(op->rs2,ppc_rarg1);
					ppc_andx(ppc_rarg0,a1,b,1);			// and. sets CR0
				}
				// JIT_CR0: the and./andi. above already set CR0.EQ.
				if (emit_t_to_cr0(BI_CR0_EQ,false))
					break;
				emit_cr0_bit_to_rarg0(BI_CR0_EQ);
				binop_end(op);
			}
			break;
		case shop_seteq: emit_setcc_signed(op,BI_CR0_EQ,false); break;
		case shop_setgt: emit_setcc_signed(op,BI_CR0_GT,false); break;
		case shop_setge: emit_setcc_signed(op,BI_CR0_LT,true);  break; // >= == !<
		case shop_setab: emit_setcc_unsigned(op,BI_CR0_GT,false); break;
		case shop_setae: emit_setcc_unsigned(op,BI_CR0_LT,true);  break; // >= == !<

		// --- Unary integer ----------------------------------------------------
		case shop_neg:    unop3_start(op); ppc_negx(bop_d,bop_a,0,0);    unop3_end(op); break;
		case shop_not:    unop3_start(op); ppc_norx(bop_d,bop_a,bop_a,0); unop3_end(op); break;
		case shop_ext_s8: unop3_start(op); ppc_extsbx(bop_d,bop_a,0);    unop3_end(op); break;
		case shop_ext_s16:unop3_start(op); ppc_extshx(bop_d,bop_a,0);    unop3_end(op); break;

		// --- Unary float (single instruction -> in-place under FPU_PIN) -------
		case shop_fabs: fuop_resolve(op); ppc_fabsx(fop_d,fop_a,0); fbop_end(op); break;
		case shop_fneg: fuop_resolve(op); ppc_fnegx(fop_d,fop_a,0); fbop_end(op); break;

		// --- Float comparisons (rd is an integer 0/1) -------------------------
		// fcmpu reads any FPR, so pinned operands compare in place; only rd
		// (an int reg) goes through rarg0 as before.
		case shop_fseteq:
			{
				u32 a=fsrc_or_load(op->rs1,ppc_farg0);
				u32 b=fsrc_or_load(op->rs2,ppc_farg1);
				ppc_fcmpu(ppc_cr0,a,b);
				emit_cr0_bit_to_rarg0(BI_CR0_EQ);
				ppc_sh_store(ppc_rarg0,op->rd);
			}
			break;
		case shop_fsetgt:
			{
				u32 a=fsrc_or_load(op->rs1,ppc_farg0);
				u32 b=fsrc_or_load(op->rs2,ppc_farg1);
				ppc_fcmpu(ppc_cr0,a,b);
				emit_cr0_bit_to_rarg0(BI_CR0_GT);
				ppc_sh_store(ppc_rarg0,op->rd);
			}
			break;

		// --- Float<->int conversion -------------------------------------------
		// All conversions bounce integer<->double bit patterns through the
		// per-context jit_scratch slot (8 bytes, 8-aligned), addressed via
		// ppc_contex. This avoids relying on a stack red zone (the devkitPPC
		// EABI does not guarantee one below sp).
		case shop_cvt_f2i_t:	// (s32)truncate(f)
			{
				// rs1 float read in place (pinned) or into farg0; rd is int.
				u32 a=fsrc_or_load(op->rs1,ppc_farg0);
				u32 so = (u32)offsetof(Sh4Context, jit_scratch);
				ppc_fctiwzx(ppc_f0,a,0);			// f0[low32] = (s32)trunc(a)
				ppc_stfd(ppc_f0,ppc_contex,so);			// spill the double
				ppc_lwz(ppc_rarg0,ppc_contex,so+4);		// low word (BE) = integer result
				ppc_sh_store(ppc_rarg0,op->rd);
			}
			break;
		case shop_cvt_i2f_n:	// (float)(s32)  round-to-nearest
		case shop_cvt_i2f_z:	// (float)(s32)  (s32->f32 is identical for both modes here)
			// Classic PPC s32->double magic-constant trick:
			//   d = bitcast(0x43300000_00000000 | (u32)(r1 ^ 0x80000000)) - 0x4330000080000000.0
			// d is the exact (double)(s32)r1; frsp narrows to single.
			{
				unop_start(op);					// rarg0 = r1
				u32 so = (u32)offsetof(Sh4Context, jit_scratch);
				// Build the biased value double: hi=0x43300000, lo=r1^0x80000000
				ppc_xoris(ppc_rarg0,ppc_rarg0,0x8000);		// flip sign bit
				ppc_addis(ppc_rarg1,0,0x4330);			// rarg1 = 0x43300000 (lis)
				ppc_stw(ppc_rarg1,ppc_contex,so);		// scratch.hi
				ppc_stw(ppc_rarg0,ppc_contex,so+4);		// scratch.lo
				ppc_lfd(ppc_f0,ppc_contex,so);			// f0 = magic|value (double)
				// Build the subtrahend 0x4330000080000000 in the same slot.
				ppc_addis(ppc_rarg1,0,0x4330);
				ppc_stw(ppc_rarg1,ppc_contex,so);
				ppc_addis(ppc_rarg1,0,0x8000);
				ppc_stw(ppc_rarg1,ppc_contex,so+4);
				ppc_lfd(ppc_farg1,ppc_contex,so);		// farg1 = 0x4330000080000000
				ppc_fsubx(ppc_f0,ppc_f0,ppc_farg1,0);		// f0 = (double)(s32)r1
				// Narrow straight into the pinned rd (no trailing fmr/stfs).
				u32 d=fdst_reg(op->rd,ppc_farg0);
				ppc_frspx(d,ppc_f0,0);				// narrow to single
				fdst_store(op->rd,d,ppc_farg0);
			}
			break;

		// --- FMAC: rd = rs1 + rs2 * rs3  (scalar) -----------------------------
		// fmadds(D,A,B,C) = A*C + B  ->  rs2*rs3 + rs1.  Single instruction, so
		// under FPU_PIN read all three pinned sources in place and write the
		// pinned dest in place (was 3 lfs + op + stfs). NOTE for SH4 fmac, rd
		// aliases rs3 (== FR0-accumulate form is different; here rd is a real
		// fr), and fmadds reads A,B,C before writing D, so aliasing is safe.
		case shop_fmac:
			{
				u32 a=fsrc_or_load(op->rs1,ppc_f0);	// addend (rs1)
				u32 b=fsrc_or_load(op->rs2,ppc_f1);
				u32 c=fsrc_or_load(op->rs3,ppc_f2);
				u32 d=fdst_reg(op->rd,ppc_f0);
				// fmaddsx(D,A,B,C) = A*C + B. Want d = rs2*rs3 + rs1 = b*c + a,
				// so A=b, C=c, B=a -> fmaddsx(d, b, a, c). (Matches the legacy
				// fmaddsx(f0,f1,f0,f2) = f1*f2 + f0 arg pattern D,rs2,rs1,rs3.)
				ppc_fmaddsx(d,b,a,c,0);			// d = b*c + a
				fdst_store(op->rd,d,ppc_f0);
			}
			break;

		// --- FIPR: rd = dot(v1, v2) over 4 elements ---------------------------
		// rs1,rs2 are FV4 vectors (base fr); rd is the 4th element slot.
		case shop_fipr:
			{
				const bool dp=get_jit_fipr_preset()!=0;
#if defined(__WIIU__)
				if (g_exp_fipr_split && get_fpu_pin_preset())
				{
					// fipr_split: pinned operands read in place (no fmr copies), the last step writing the pinned
					// result directly. rd is always an element of an operand (FR[n+3] of FVn); it is written only by
					// the final instruction.
					u32 a[4],b[4];
					for (u32 i=0;i<4;i++)
					{
						ppc_freg ra=GetFloatReg(op->rs1._reg+i), rb=GetFloatReg(op->rs2._reg+i);
						if (ra!=ppc_finvalid) a[i]=(u32)ra; else { ppc_lfs(ppc_f1+i,ppc_contex,ppc_fvec_ofs(op->rs1,i)); a[i]=ppc_f1+i; }
						if (rb!=ppc_finvalid) b[i]=(u32)rb; else { ppc_lfs(ppc_f5+i,ppc_contex,ppc_fvec_ofs(op->rs2,i)); b[i]=ppc_f5+i; }
					}
					const ppc_freg rf=GetFloatReg(op->rd._reg);
					const u32 out=rf!=ppc_finvalid ? (u32)rf : (u32)ppc_f0;
					// Two chains, (a0*b0 + a2*b2) + (a1*b1 + a3*b3), in f0 and f9 (scratch; the
					// operands are pinned FRs or f1..f8): the dependent chain is mul, madd, add instead of mul and
					// three madds. Same precision as the serial form; the sum order differs, so with the double
					// sum the single result can differ only where two double roundings land differently (rare)
					if (dp) { ppc_fmulx (ppc_f0,a[0],b[0],0); ppc_fmulx (ppc_f9,a[1],b[1],0); }
					else    { ppc_fmulsx(ppc_f0,a[0],b[0],0); ppc_fmulsx(ppc_f9,a[1],b[1],0); }
					if (dp) { ppc_fmaddx (ppc_f0,a[2],ppc_f0,b[2],0); ppc_fmaddx (ppc_f9,a[3],ppc_f9,b[3],0); }
					else    { ppc_fmaddsx(ppc_f0,a[2],ppc_f0,b[2],0); ppc_fmaddsx(ppc_f9,a[3],ppc_f9,b[3],0); }
					if (dp) { ppc_faddx(ppc_f0,ppc_f0,ppc_f9,0); ppc_frspx(out,ppc_f0,0); }
					else    ppc_faddsx(out,ppc_f0,ppc_f9,0);
					if (rf==ppc_finvalid) ppc_stfs(ppc_f0,ppc_contex,Sh4cntx.offset(op->rd._reg));
					g_cnt_fipr++;
					break;
				}
#endif
				ppc_fvec_load(ppc_f1,op->rs1,0);
				ppc_fvec_load(ppc_f2,op->rs2,0);
				if (dp) ppc_fmulx (ppc_f0,ppc_f1,ppc_f2,0);	// acc = a0*b0 (exact in double)
				else    ppc_fmulsx(ppc_f0,ppc_f1,ppc_f2,0);	// acc = a0*b0
				for (u32 i=1;i<4;i++)
				{
					ppc_fvec_load(ppc_f1,op->rs1,i);
					ppc_fvec_load(ppc_f2,op->rs2,i);
					if (dp) ppc_fmaddx (ppc_f0,ppc_f1,ppc_f0,ppc_f2,0);	// acc = ai*bi + acc
					else    ppc_fmaddsx(ppc_f0,ppc_f1,ppc_f0,ppc_f2,0);	// acc = ai*bi + acc
				}
				// The double sum MUST be rounded here: stfs truncates instead of
				// rounding, and the FPU_PIN store is a bare fmr into the pinned
				// FPR, which would carry the extra precision into later ops.
				if (dp) ppc_frspx(ppc_f0,ppc_f0,0);
				ppc_sh_store_f32(ppc_f0,op->rd);
			}
			break;

		// --- FTRV: rd(vec4) = matrix(rs2) x vec(rs1) --------------------------
		// rs1 = FV4 vector (fr base), rs2 = XMTRX (xf base), rd = FV4 (aliases rs1).
		//   out[i] = m[i]*v[0] + m[4+i]*v[1] + m[8+i]*v[2] + m[12+i]*v[3]
		// Results go to f4..f7 first because rd aliases rs1 (must finish reads
		// before any store).
		case shop_ftrv:
			{
				const bool dp=get_jit_fipr_preset()!=0;
#if defined(__WIIU__)
				// the input vector where it is: pinned FRs are read in place (rd aliases it, but the results only
				// leave f4..f7 after the last read)
				u32 vin[4];
				for (u32 j=0;j<4;j++)
				{
					const ppc_freg rf=get_fpu_pin_preset() ? GetFloatReg(op->rs1._reg+j) : ppc_finvalid;
					if (rf!=ppc_finvalid)
						vin[j]=(u32)rf;
					else
					{
						ppc_fvec_load(ppc_f8+j,op->rs1,j);
						vin[j]=ppc_f8+j;
					}
				}
#else
				// Load the input vector once into f8..f11.
				for (u32 j=0;j<4;j++)
					ppc_fvec_load(ppc_f8+j,op->rs1,j);
#endif

#if defined(__WIIU__)
				if (g_exp_ps_ftrv)
				{
					// paired singles: the matrix rows two floats at a time, out[0,1] in f4 and out[2,3] in f5. Single
					// precision throughout, so not bit-identical to the default (jit_fipr keeps double). GQR0 must be
					// 0, plain floats; the 12-bit displacement holds because xf[] sits at the start of the context.
					const u32 mo=ppc_fvec_ofs(op->rs2,0);
					for (u32 j=0;j<4;j+=2)
					{
						ppc_psq_l(ppc_f0,ppc_contex,0,0,mo+j*16);
						ppc_psq_l(ppc_f1,ppc_contex,0,0,mo+j*16+8);
						ppc_psq_l(ppc_f2,ppc_contex,0,0,mo+j*16+16);
						ppc_psq_l(ppc_f3,ppc_contex,0,0,mo+j*16+24);
						if (j==0)
						{
							ppc_ps_muls0(ppc_f4,ppc_f0,vin[0],0);
							ppc_ps_muls0(ppc_f5,ppc_f1,vin[0],0);
						}
						else
						{
							ppc_ps_madds0(ppc_f4,ppc_f0,ppc_f4,vin[j],0);
							ppc_ps_madds0(ppc_f5,ppc_f1,ppc_f5,vin[j],0);
						}
						ppc_ps_madds0(ppc_f4,ppc_f2,ppc_f4,vin[j+1],0);
						ppc_ps_madds0(ppc_f5,ppc_f3,ppc_f5,vin[j+1],0);
					}
					ppc_ps_merge11(ppc_f6,ppc_f4,ppc_f4,0);
					ppc_ps_merge11(ppc_f7,ppc_f5,ppc_f5,0);
					ppc_fvec_store(ppc_f4,op->rd,0);
					ppc_fvec_store(ppc_f6,op->rd,1);
					ppc_fvec_store(ppc_f5,op->rd,2);
					ppc_fvec_store(ppc_f7,op->rd,3);
					break;
				}
				// The four output chains interleaved (accumulators f4..f7, this step's matrix column in f0..f3): each
				// fmadd depends on the one four instructions back instead of the previous one, so the FPU pipeline
				// stays full. Every chain still does the same operations in the same order, so the results are
				// bit-identical to the serial form below, and the stash move is gone (f4..f7 are the stash).
				for (u32 j=0;j<4;j++)
				{
					for (u32 i=0;i<4;i++)
						ppc_fvec_load(ppc_f0+i,op->rs2,j*4+i);	// m[j*4+i]
					for (u32 i=0;i<4;i++)
					{
						if (j==0)
						{
							if (dp) ppc_fmulx (ppc_f4+i,ppc_f0+i,vin[0],0);	// out[i] = m[i]*v0
							else    ppc_fmulsx(ppc_f4+i,ppc_f0+i,vin[0],0);
						}
						else
						{
							if (dp) ppc_fmaddx (ppc_f4+i,ppc_f0+i,ppc_f4+i,vin[j],0);	// out[i] += m*vj
							else    ppc_fmaddsx(ppc_f4+i,ppc_f0+i,ppc_f4+i,vin[j],0);
						}
					}
				}
				for (u32 i=0;i<4;i++)
				{
					// round(out[i]) (JIT_FIPR) straight into a pinned result
					const ppc_freg rf=get_fpu_pin_preset() ? GetFloatReg(op->rd._reg+i) : ppc_finvalid;
					if (dp && rf!=ppc_finvalid)
						ppc_frspx((u32)rf,ppc_f4+i,0);
					else
					{
						if (dp)
							ppc_frspx(ppc_f4+i,ppc_f4+i,0);
						ppc_fvec_store(ppc_f4+i,op->rd,i);
					}
				}
#else
				for (u32 i=0;i<4;i++)
				{
					ppc_fvec_load(ppc_f1,op->rs2,i);		// m[i]
					if (dp) ppc_fmulx (ppc_f0,ppc_f1,ppc_f8,0);	// out = m[i]*v0
					else    ppc_fmulsx(ppc_f0,ppc_f1,ppc_f8,0);	// out = m[i]*v0
					for (u32 j=1;j<4;j++)
					{
						ppc_fvec_load(ppc_f1,op->rs2,j*4+i);	// m[j*4+i]
						if (dp) ppc_fmaddx (ppc_f0,ppc_f1,ppc_f0,ppc_f8+j,0);	// out += m*vj
						else    ppc_fmaddsx(ppc_f0,ppc_f1,ppc_f0,ppc_f8+j,0);	// out += m*vj
					}
					// JIT_FIPR: the rounding to single replaces the stash move,
					// so FTRV costs no extra instruction (see shop_fipr).
					if (dp) ppc_frspx(ppc_f4+i,ppc_f0,0);		// stash round(out[i])
					else    ppc_fmrx (ppc_f4+i,ppc_f0,0);		// stash out[i]
				}
				for (u32 i=0;i<4;i++)
					ppc_fvec_store(ppc_f4+i,op->rd,i);
#endif
			}
			break;

		// fsqrt / fsrra are handled below. Both are the libm call unless
		// JIT_FSQRT is on, which inlines both (frsqrte + Newton).

		// --- FSCA: rd[0]=sin_table[idx], rd[1]=sin_table[idx+0x4000] ----------
		// idx = rs1 & 0xFFFF. Table entries are f32 (4 bytes).
		case shop_fsca:
			{
				// idx*4 = (fpul & 0xFFFF)*4: rlwinm reads pinned rs1 in place -> rarg0.
				u32 fp=src_or_load(op->rs1,ppc_rarg0);
				ppc_rlwinmx(ppc_rarg0,fp,2,14,29,0);		// rarg0 = (idx & 0xFFFF) * 4
				// rarg1 = &sin_table  (split hi/lo via lis+addi pattern)
				u32 lo=ppc_addr_high(ppc_rarg1,(void*)&sin_table[0]);
				ppc_addi(ppc_rarg1,ppc_rarg1,lo);		// rarg1 = base of sin_table
				ppc_lfsx(ppc_f0,ppc_rarg1,ppc_rarg0);		// sin_table[idx]
				ppc_fvec_store(ppc_f0,op->rd,0);
				// +0x4000 entries * 4 bytes = +0x10000 (doesn't fit addi s16;
				// use addis to add 1<<16).
				ppc_addis(ppc_rarg0,ppc_rarg0,1);		// rarg0 += 0x10000
				ppc_lfsx(ppc_f1,ppc_rarg1,ppc_rarg0);		// sin_table[idx+0x4000]
				ppc_fvec_store(ppc_f1,op->rd,1);
			}
			break;

		// --- SR / FPSCR sync, prefetch, sqrt: direct native calls ------------
		// sync_sr: write SR side-effects. UpdateSR() -> ChangeGPR() swaps
		// r[]<->r_bank[] IN MEMORY on an SR.RB change, so pinned GPRs must be
		// flushed before and reloaded after. The bool return (interrupt pending)
		// is intentionally ignored here: every SH4 op that emits sync_sr ends its
		// block with BET_*Intr, whose native handler runs UpdateINTC and
		// dispatches. (This is pseudo-core: keep it self-contained, do not rely
		// on the generic fallback.)
		case shop_sync_sr:
			reg_flush_all();
			ppc_call(&UpdateSR);
			reg_reload_all();
			break;

		// sync_fpscr: UpdateFPSCR() (rounding mode + possible FR bank swap).
		// No GPR flush needed (this never touches r[]). FPU_PIN: UpdateFPSCR()
		// -> ChangeFP() swaps fr_hex[i]<->xf_hex[i] IN MEMORY on an FPSCR.FR
		// toggle (sh4_registers.cpp), so pinned fr must be flushed before the
		// call (so the swap sees current values, not stale ones) and reloaded
		// after (fr[] now holds the new front bank). reg_flush/reload_all_fpu
		// are no-ops when the preset is off.
		case shop_sync_fpscr:
			// JIT_FSCHG: SYNC_FPSCR_SZ_ONLY is set only by the `fschg` decoder,
			// which toggles FPSCR.SZ (bit 20) and nothing else. FR (bit 21)
			// cannot have changed, so ChangeFP() is impossible; RM/DN are
			// untouched, so SetFloatStatusReg() has nothing to do (and on Wii
			// its body is x86-only anyway). All that is left of UpdateFPSCR()
			// is keeping old_fpscr in step -- two instructions.
			//
			// This matters far more than it looks. Dreamcast T&L brackets its
			// vertex loop with fschg to get pair-width fmov, so this op fires
			// ~2.3 times per transformed vertex; with FPU_PIN on, the legacy
			// path below is 16 stfs + a C call + 16 lfs, every time, to change
			// one bit that the FPU state does not depend on.
			//
			// old_fpscr is plain context memory (never pinned), so a raw stw is
			// safe; fpscr itself may live in a pinned GPR, hence ppc_sh_load
			// rather than a bare lwz. rarg0 is scratch here (shop_pref uses it
			// the same way) and no GPR flush is needed -- this never touches
			// r[]. The offset comes from SH4CTX_OFS_OLD_FPSCR rather than
			// offsetof() because sh4_registers.h later does
			// "#define old_fpscr Sh4cntx.old_fpscr", which would rewrite the
			// member name inside the offsetof and fail to compile.
			if ((op->flags & SYNC_FPSCR_SZ_ONLY) && get_jit_fschg_preset())
			{
				ppc_sh_load(ppc_rarg0, reg_fpscr);
				ppc_stw(ppc_rarg0, ppc_contex, (u32)SH4CTX_OFS_OLD_FPSCR);
			}
			else
			{
				reg_flush_all_fpu();
				ppc_call(&UpdateFPSCR);
				reg_reload_all_fpu();
			}
			break;

		// pref: store-queue prefetch. Only addresses in the SQ region trigger a
		// store-queue flush — the canonical guards with `if ((addr>>26)==0x38)`
		// BEFORE calling do_sqw (do_sqw itself assumes an SQ address). For any
		// other address pref is a no-op. The MMU vs no-MMU variant is chosen at
		// COMPILE time from MMUCR.AT (mmucr_at(): the bitfield reads the wrong
		// bit on big endian, see ccn.h), and ccn.cpp clears the cache on an AT
		// edge so no block keeps the old choice. addr in rarg0.
		case shop_pref:
			{
				ppc_sh_load(ppc_rarg0,op->rs1);
				ppc_rlwinmx(ppc_rarg1,ppc_rarg0,6,26,31,0);	// rarg1 = addr >> 26
				ppc_cmpi(ppc_cr0,ppc_rarg1,0x38,0);		// SQ region?
				ppc_label* skip=ppc_CreateLabel();
				ppc_bcx(BO_FALSE,BI_CR0_EQ,0,0,0);		// bne skip (not SQ -> no-op)
				if (mmucr_at())
					ppc_call(&do_sqw_mmu);			// do_sqw reloads rarg0=addr arg
				else
#if defined(__WIIU__)
				if (g_exp_sq_inline)
				{
					// sq_inline (J1): a TA block goes straight into the frame being built, no call. rarg0 = dst
					// stays intact for the cold paths; scratch r4-r8, r11, f0-f3 (op-local); r12 is not touched.
					const u32 lo=ppc_addr_high(ppc_r11,(void*)g_ta_fast);	// lis r11; lo+field fits (32-aligned)
					ppc_rlwinmx(ppc_r4,ppc_rarg0,29,29,29,0);		// (dst & 0x20) >> 3: this queue's mask
					ppc_add(ppc_r4,ppc_r4,ppc_r11);
					ppc_lwz(ppc_r5,ppc_r4,lo+TAF_MASK);
					ppc_and_(ppc_r5,ppc_r5,ppc_rarg0);			// 0: QACR area is the TA, dst below 0x800000
					ppc_label* cold_c1=ppc_CreateLabel();
					ppc_bcx(BO_FALSE,BI_CR0_EQ,0,0,0);			// bne: C path
					ppc_lwz(ppc_r6,ppc_r11,lo+TAF_NEXT);
					ppc_lwz(ppc_r7,ppc_r11,lo+TAF_END);
					ppc_cmplw(ppc_r6,ppc_r7);
					ppc_label* cold_c2=ppc_CreateLabel();
					ppc_bcx(BO_FALSE,BI_CR0_LT,0,0,0);			// bge: frame full, C counts the drop
					ppc_rlwinmx(ppc_r4,ppc_rarg0,0,26,26,0);		// dst & 0x20: which queue
					ppc_oris(ppc_r4,ppc_r4,(u32)(uintptr_t)sq_both>>16);	// sq_both is 256-aligned
					ppc_ori(ppc_r4,ppc_r4,(u32)(uintptr_t)sq_both&0xFFFF);
					ppc_dcbz(0,ppc_r6);					// only after the bound check
					ppc_lfd(ppc_f0,ppc_r4,0);  ppc_lfd(ppc_f1,ppc_r4,8);
					ppc_lfd(ppc_f2,ppc_r4,16); ppc_lfd(ppc_f3,ppc_r4,24);
					ppc_stfd(ppc_f0,ppc_r6,0);  ppc_stfd(ppc_f1,ppc_r6,8);
					ppc_stfd(ppc_f2,ppc_r6,16); ppc_stfd(ppc_f3,ppc_r6,24);
					ppc_addi(ppc_r7,ppc_r6,32);
					ppc_stw(ppc_r7,ppc_r11,lo+TAF_NEXT);
					ppc_lwz(ppc_r8,ppc_r11,lo+TAF_SKIP);
					ppc_addic(ppc_r8,ppc_r8,(u32)-1,1);			// addic. skip-1
					ppc_label* colds[4]={cold_c1,cold_c2,0,0};
					int ncold=3;
					if (g_exp_sq_inline>=2)
					{
						// skip was 0: a vertex (ParaType 7, PCW bits 31-29) in a polygon list is only counted by
						// the scanner: skip = vfast, nvert++, nstrip += EndOfStrip (bit 28). Anything else: C
						ppc_label* cont=ppc_CreateLabel();
						ppc_bcx(BO_FALSE,BI_CR0_LT,0,0,0);		// bge: skip was > 0, a continuation
						ppc_lwz(ppc_r9,ppc_r4,0);			// the PCW, from the store queue
						ppc_rlwinmx(ppc_r10,ppc_r9,3,29,31,0);
						ppc_cmpwi(ppc_r10,7);
						colds[2]=ppc_CreateLabel();
						ppc_bcx(BO_FALSE,BI_CR0_EQ,0,0,0);		// bne: not a vertex
						ppc_lwz(ppc_r8,ppc_r11,lo+TAF_VFAST);
						ppc_cmpwi(ppc_r8,0);
						colds[3]=ppc_CreateLabel();
						ppc_bcx(BO_TRUE,BI_CR0_LT,0,0,0);		// blt: sprite, modifier volume or no list
						ncold=4;
						ppc_lwz(ppc_r5,ppc_r11,lo+TAF_NVERT);
						ppc_addi(ppc_r5,ppc_r5,1);
						ppc_stw(ppc_r5,ppc_r11,lo+TAF_NVERT);
						ppc_rlwinmx(ppc_r9,ppc_r9,4,31,31,0);
						ppc_lwz(ppc_r5,ppc_r11,lo+TAF_NSTRIP);
						ppc_add(ppc_r5,ppc_r5,ppc_r9);
						ppc_stw(ppc_r5,ppc_r11,lo+TAF_NSTRIP);
						cont->MarkLabel();
					}
					else
					{
						colds[2]=ppc_CreateLabel();
						ppc_bcx(BO_TRUE,BI_CR0_LT,0,0,0);		// skip was 0: parse it in C
					}
					ppc_stw(ppc_r8,ppc_r11,lo+TAF_SKIP);
					void* done=emit_GetCCPtr();
					for (int k=0;k<ncold;k++)
					{
						ColdFrag& c=s_cold[s_cold_n++];
						c.beq=colds[k]; c.done=done; c.kind=k<2?4:5;
						c.fdreg=0xFF; c.fuct=0;
					}
				}
				else
					ppc_call(&wiiu_sqw_fast);		// wiiu/render_gx2.cpp: TA data in one call
#else
					ppc_call(&do_sqw_nommu);
#endif
				skip->MarkLabel();
			}
			break;

		// fsqrt / fsrra: single f32->f32 calls (accurate libm path).
		// arg in farg0 (f1), result in frv0 (f1) per PPC FP calling convention.
		case shop_fsqrt:
			if (get_jit_fsqrt_preset())
			{
				// Say so once, so a log can prove the inline path was emitted
				// at all -- [fmov] does the same for its fastmem shapes.
				static bool announced=false;
				if (!announced)
				{
					announced=true;
					printf("[fsqrt] JIT_FSQRT active: inlining frsqrte+Newton (no libm call)\n");
					fflush(stdout);
				}

				// rd and rs1 are the same FR for `fsqrt FRn`, so nothing may
				// write d until the last read of a (the residual step).
				u32 a=fsrc_or_load(op->rs1,ppc_f0);
				u32 d=fdst_reg(op->rd,ppc_f0);
				const u32 fC=ppc_f2,fY=ppc_f3,fH=ppc_f4,fT=ppc_f5;
				const u32 fE=ppc_f6,fS=ppc_f7,fR=ppc_f8,fHY=ppc_f9;

				// Hoisted above the guard so the load has landed by the time
				// the first multiply wants it.
				u32 lo=ppc_addr_high(ppc_rarg0,(void*)&s_fsqrt_half);
				ppc_lfs(fC,ppc_rarg0,lo);		// 0.5

				// Guard: fast path only for x > 0. fsubs(x,x) is +0.0 for any
				// finite x and NaN for +-Inf, so this one compare rejects 0,
				// -0, negatives, Inf and NaN together -- NaN and Inf compare
				// unordered, which is not GT.
				ppc_fsubs(fT,a,a);
				ppc_fcmpu(ppc_cr0,a,fT);
				ppc_label* to_cold=ppc_CreateLabel();
				ppc_bcx(BO_FALSE,BI_CR0_GT,0,0,0);	// bng -> cold

				ppc_fmul(fH,a,fC);			// h = 0.5*x
				ppc_frsqrte(fY,a);			// y ~ 1/sqrt(x), >=5 bits
				for (u32 i=0;i<3;i++)
				{
					ppc_fmul(fT,fY,fY);		// t = y*y
					ppc_fnmsub(fE,fH,fT,fC);	// e = 0.5 - h*t
					ppc_fmadd(fY,fY,fE,fY);		// y = y*e + y
				}
				ppc_fmul(fS,a,fY);			// s = x*y ~ sqrt(x)
				ppc_fmul(fHY,fY,fC);			// hy = 0.5*y
				ppc_fnmsub(fR,fS,fS,a);			// r = x - s*s  (exact: fused)
				ppc_fmadd(fS,fR,fHY,fS);		// s = r*hy + s
				ppc_frsp(d,fS);				// -> single, correctly rounded

				ppc_label* to_done=ppc_CreateLabel();
				ppc_bx(0,0,0);				// b done (patched below)
				to_cold->MarkLabel();
				// Cold: 0, -0, negative, Inf, NaN. Hand them to the same libm
				// call the legacy path uses so the semantics are identical.
				if (a!=ppc_farg0)
					ppc_fmr(ppc_farg0,a);
				ppc_call(&rec_fsqrt);
				if (d!=ppc_frv0)
					ppc_fmr(d,ppc_frv0);
				to_done->MarkLabelLong();

				fdst_store(op->rd,d,ppc_f0);
			}
			else
			{
				ppc_sh_load_f32(ppc_farg0,op->rs1);
				ppc_call(&rec_fsqrt);
				ppc_sh_store_f32(ppc_frv0,op->rd);
			}
			break;
		// FSRRA(x) = 1/sqrt(x), which is exactly what shop_fsqrt computes on
		// its way to sqrt -- the Newton refinement below IS that code, stopping
		// one step earlier. It sits in ChuChu's per-vertex T&L block
		// (8C107502, inside the 250 MB/s 8C1074EE run), so it ran ~600k times a
		// second as a libm call until this existed.
		case shop_fsrra:
			if (get_jit_fsqrt_preset())
			{
				// Announced separately from fsqrt's line: the two are gated on
				// the same preset but are different emitters, and a log that
				// only proves fsqrt was inlined says nothing about this one --
				// which is exactly how fsrra stayed a call-out unnoticed.
				static bool announced=false;
				if (!announced)
				{
					announced=true;
					printf("[fsqrt] JIT_FSQRT also inlining fsrra (frsqrte+Newton, no libm call)\n");
					fflush(stdout);
				}

				u32 a=fsrc_or_load(op->rs1,ppc_f0);
				u32 d=fdst_reg(op->rd,ppc_f0);
				const u32 fC=ppc_f2,fY=ppc_f3,fH=ppc_f4,fT=ppc_f5,fE=ppc_f6;

				u32 lo=ppc_addr_high(ppc_rarg0,(void*)&s_fsqrt_half);
				ppc_lfs(fC,ppc_rarg0,lo);		// 0.5

				// Same one-compare guard as fsqrt: fsubs(x,x) is +0.0 for any
				// finite x and NaN for +-Inf, so this rejects 0, -0, negatives,
				// Inf and NaN together (NaN/Inf compare unordered, not GT).
				// Every one of those has a defined fsrra result (+Inf, NaN, 0)
				// that the cold call reproduces exactly.
				ppc_fsubs(fT,a,a);
				ppc_fcmpu(ppc_cr0,a,fT);
				ppc_label* to_cold=ppc_CreateLabel();
				ppc_bcx(BO_FALSE,BI_CR0_GT,0,0,0);	// bng -> cold

				ppc_fmul(fH,a,fC);			// h = 0.5*x
				ppc_frsqrte(fY,a);			// y ~ 1/sqrt(x), >=5 bits
				// Each step roughly doubles the accurate bits: 5 -> 10 -> 20 ->
				// 40, comfortably past single precision's 24. Done in DOUBLE
				// (ppc_fmul/fnmsub/fmadd are the double forms) so the rounding
				// error of the iteration itself stays below the final frsp.
				for (u32 i=0;i<3;i++)
				{
					ppc_fmul(fT,fY,fY);		// t = y*y
					ppc_fnmsub(fE,fH,fT,fC);	// e = 0.5 - h*t
					ppc_fmadd(fY,fY,fE,fY);		// y = y*e + y
				}
				// y IS the result. fsqrt continues with s = x*y and a residual
				// correction to land sqrt correctly rounded; neither applies
				// here, which is why this path is shorter than that one.
				ppc_frsp(d,fY);				// -> single

				ppc_label* to_done=ppc_CreateLabel();
				ppc_bx(0,0,0);				// b done (patched below)
				to_cold->MarkLabel();
				if (a!=ppc_farg0)
					ppc_fmr(ppc_farg0,a);
				ppc_call(&rec_fsrra);
				if (d!=ppc_frv0)
					ppc_fmr(d,ppc_frv0);
				to_done->MarkLabelLong();

				fdst_store(op->rd,d,ppc_f0);
			}
			else
			{
				ppc_sh_load_f32(ppc_farg0,op->rs1);
				ppc_call(&rec_fsrra);
				ppc_sh_store_f32(ppc_frv0,op->rd);
			}
			break;

		default:
			//canonical fallback ~
      if (!shil_chf[op->op]) {
          printf("OH CRAP %d\n", op->op);
          die("Recompiler doesn't know about that opcode");
      }
			// Bracket the canonical fallback with flush/reload: some fallback
			// handlers (notably sync_sr -> UpdateSR) mutate context GPRs directly
			// (e.g. SR.RB bank switch), so pinned regs must be coherent in memory
			// across the call and re-read afterwards. The CC param marshalling
			// itself is register-aware and unaffected. FPU_PIN: any un-natived
			// float op reads/writes fr[]/xf[] directly too (including via
			// ppc_sh_addr's &Sh4cntx.fr[n] pointers for CPT_ptr canonical
			// params), so pinned fr needs the same bracket.
			reg_flush_all();
			reg_flush_all_fpu();
			shil_chf[op->op](op);
			reg_reload_all();
			reg_reload_all_fpu();
			break;
      
		}
	}

	ngen_End(block);

	// Split point for JIT_HOTBLOCKS: everything up to here is the block's HOT
	// body, everything FlushCold() adds after it is out-of-line mem slow paths
	// that usually never execute. Lumping them together makes a block with big
	// cold fragments look like bad codegen when its hot path may be fine, so
	// the two are reported separately.
	u8 *const _hb_cold_start = (u8*)emit_GetCCPtr();

	FlushCold();          // emit the out-of-line mem slow paths after the block tail
#if defined(__WIIU__)
	// fastmem=2: every site gets its trampoline now, as if it had faulted at a Dreamcast address in area 0 (so writes
	// also build the store queue path)
	if (g_wiiu_fm_prepatch)
		for (u32 i = 0; i < s_fm_sites_n; i++)
			if (rec_fastmem_patch_dar((unsigned)(uintptr_t)s_fm_sites[i], FM_WINDOW) != 0)
				printf("[fastmem] pre-patch failed at %p: reason %u insn %08x\n", s_fm_sites[i],
				       (unsigned)s_fm_fail_reason, (unsigned)s_fm_fail_insn);
#endif
	make_address_range_executable((u8*)rv, (u8*)emit_GetCCPtr()-(u8*)rv);

	// Hot body and cold fragments recorded separately — B/op is built on the HOT
	// size only, since that is what actually executes.
	if (hb>=0)
	{
		u32 hot = (u32)(_hb_cold_start-(u8*)rv);
		// Discount the probe's own 4-instruction counter so the B/op figure
		// describes the codegen, not the measurement. On a 3-op block those
		// 16 bytes are 5 B/op of pure observer effect.
		s_hb[hb].ppc_start = (u8*)rv;
		s_hb[hb].ppc_size  = hot>16 ? hot-16 : hot;
		s_hb[hb].ppc_cold  = (u32)((u8*)emit_GetCCPtr()-_hb_cold_start);
	}

	return rv;
}
