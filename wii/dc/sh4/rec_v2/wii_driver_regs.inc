// wii_driver_regs.inc: static GPR allocation, FPU pinning, the shop_ifb register bracket, IFB_PROBE.
// Part of wii_driver.cpp, which #includes it: not compiled on its own.

// ============================================================================
// Static GPR allocation
//
// A fixed subset of SH4 integer registers is pinned to PPC callee-saved GPRs
// (r14..r28) for the whole emulation session. The mapping is block-invariant
// (no per-block colouring) — GetIntReg() defines it. SH4 r0..r15 (except r11,
// which is skipped because the recompiler needs a volatile temp window) map to
// r14..r28.
//
// Memory image vs register:
//   * The pinned PPC regs are the AUTHORITATIVE copy of the SH4 GPRs for the
//     whole JIT run. They are loaded once in the mainloop prologue and stay
//     live across blocks; Sh4cntx.r[] may be stale at any point.
//   * Sh4cntx.r[] is made coherent on demand only around code paths that read
//     or write context GPRs directly, each self-bracketing flush -> call ->
//     reload.
//
// Why we don't blanket flush/reload around ReadMem/WriteMem:
//   r14..r28 are callee-saved under the PPC EABI, so an ordinary C call
//   preserves them, and ReadMem/WriteMem touch only guest memory, not SH4
//   context GPRs.
//
// The flush/reload points are:
//   * shop_ifb              — interpreter handler reads/writes arbitrary GPRs
//   * canonical fallback    — e.g. sync_sr -> UpdateSR (SR.RB bank swap)
//   * BET_*Intr block end   — UpdateINTC -> Do_Interrupt (reads r[15], bank swap)
//   * mainloop UpdateSystem — CONDITIONAL: the split UpdateSystem_no_event()
//                             runs the GPR-free peripheral cascade + pending
//                             check unflushed every timeslice; only when it
//                             reports a pending interrupt do we flush, call
//                             UpdateSystem_handle_event(), and reload.
//
// STATIC_GPR_ALLOC (defined near the top of this file) can be set to 0 to
// disable the whole scheme (all accesses go through memory, as before) —
// useful for A/B debugging.
// ============================================================================

void reg_flush_all()
{
#if STATIC_GPR_ALLOC
	for (u32 i=reg_r0;i<=reg_r15;i++)
	{
		ppc_ireg ri=GetIntReg(i);
		if (ri!=ppc_rinvalid)
			ppc_stw(ri,ppc_contex,Sh4cntx.offset(i));
	}
#endif
}
void reg_reload_all()
{
#if STATIC_GPR_ALLOC
	for (u32 i=reg_r0;i<=reg_r15;i++)
	{
		ppc_ireg ri=GetIntReg(i);
		if (ri!=ppc_rinvalid)
			ppc_lwz(ri,ppc_contex,Sh4cntx.offset(i));
	}
#endif
}

// ============================================================================
// FPU_PIN preset (default OFF) — pins fr[0..15] to PPC f14..f29 for the whole
// session, the same scheme as STATIC_GPR_ALLOC but for the FPU file. xf[]
// stays memory-resident (see GetFloatReg's comment).
//
// Unlike GPR pinning, this is a RUNTIME preset (get_fpu_pin_preset()), not a
// compile-time #define: it's new and unproven, so it needs to be A/B-able on
// a shipped binary the way fastmem/bcache are, not just via recompile.
//
// Bracket points (mirrors reg_flush_all/reg_reload_all's list above, plus one
// FPU-specific case):
//   * shop_ifb              — interpreter handler reads/writes arbitrary FRs
//   * canonical fallback    — any un-natived shil op may touch fr[]/xf[]
//     directly (memory), including through ppc_sh_addr's &Sh4cntx.fr[n]
//     pointers handed to CPT_ptr canonical params — flushing here keeps
//     those pointers valid
//   * shop_sync_fpscr       — UpdateFPSCR() -> ChangeFP() swaps
//     fr_hex[i]<->xf_hex[i] IN MEMORY on an FPSCR.FR toggle (sh4_registers.cpp);
//     pinned fr must be flushed before the call (so the swap sees current
//     values) and reloaded after (fr[] now holds the new front bank)
//
// NOT bracketed: BET_*Intr (UpdateINTC -> Do_Exception). Real SH4 interrupts
// bank-swap GPRs (SR.RB) but never fr[]/xf[] — only an explicit FPSCR.FR
// write does that, which is shop_sync_fpscr's job above.
//
// The memory-bounce fix for shop_readm/writem/mov64 (which touch fr[] bit
// patterns through the generic int ppc_sh_load/ppc_sh_store, not these FPU
// helpers) lives centrally in those two functions — see their FPU_PIN
// comments earlier in this file.
// ============================================================================

void reg_flush_all_fpu()
{
	if (!get_fpu_pin_preset())
		return;
	for (u32 i=reg_fr_0;i<=reg_fr_15;i++)
	{
		ppc_freg rf=GetFloatReg(i);
		ppc_stfs(rf,ppc_contex,Sh4cntx.offset(i));
	}
}
void reg_reload_all_fpu()
{
	if (!get_fpu_pin_preset())
		return;
	for (u32 i=reg_fr_0;i<=reg_fr_15;i++)
	{
		ppc_freg rf=GetFloatReg(i);
		ppc_lfs(rf,ppc_contex,Sh4cntx.offset(i));
	}
}

// ============================================================================
// IFB_FLUSH preset + IFB_PROBE — narrowing the shop_ifb register bracket
// ============================================================================
// shop_ifb hands ONE 16-bit SH4 opcode to its interpreter handler
// (OpDesc[op]->oph). Because that handler could in principle read or write any
// guest register through Sh4cntx, the emitter has always bracketed the call
// with a FULL register-file spill: reg_flush_all() (15 stw) + reg_reload_all()
// (15 lwz), plus reg_flush_all_fpu()/reg_reload_all_fpu() (16 stfs + 16 lfs)
// when FPU_PIN is on. That is 30 memory ops — 62 with FPU_PIN — around an
// opcode that in practice touches two registers.
//
// Same shape as the fschg/sync_fpscr bug (a C call-out doing a whole-file
// spill for almost nothing) that was worth +12-13% on Wii, and it lands on a
// hot set. Only 28 opcodes reach ifb on this build, but they include div1,
// addc/subc/addv/subv, negc, rotcl/rotcr, cmp/str, xtrct, swap.b, mac.l/mac.w
// and tas.b. div1 is the expensive one: the SH4 has no divide instruction, so
// every 32-bit software divide is ~32x div1 (+ rotcl), i.e. on the order of
// 2000 memory ops of pure bracket per division under the full-spill code.
//
// What makes a narrower bracket safe: of the whole SH4 context, ONLY r0..r15
// (minus r11) and fr[0..15] are ever register-resident. sr, gbr, vbr, mac,
// fpul, fpscr, pr and the banked copies are always memory-authoritative, so a
// handler touching only those needs no bracket at all. The bracket therefore
// has to cover exactly the GPRs (and FPRs) that handler reads or writes.
//
// This is a CLOSED ALLOW-LIST, not a mask-derived guess: an opcode is narrowed
// only if it is named below with its exact register set. Everything else keeps
// the full bracket — trapa, sleep, illegal instructions, and every FPU op that
// fell back because FPSCR.PR selected double precision. Default-deny: an
// unlisted opcode is slow, never wrong, and a future opcode reaching ifb
// inherits the safe path automatically.
//
// TWO INDEPENDENT AXES. The bracket is not one decision but two — "which GPRs
// does this handler touch?" and "does it touch the float file at all?" — and
// they do not correlate. Collapsing them into a single "spill everything"
// sentinel (the first version of this code) made three groups overpay:
//
//   lds Rn,FPSCR      GPRs: just Rn      float: YES (ChangeFP swaps banks)
//   ldc Rn,SR         GPRs: ALL (bank)   float: no  (SR has no FP bank bit;
//                                                    SR.FD disables, not swaps)
//   fcnvds / fcnvsd   GPRs: none         float: YES
//
// An [IFB] probe run measured `lds Rn,FPSCR` at 265k/s during BIOS boot — 98.5%
// of all fallbacks in that phase — paying a 30-op GPR spill for a handler that
// writes no GPR at all. So ifb_bracket() returns the two axes separately.
//
// Entries are matched on the (mask, rez) PAIR from the opcode table rather
// than on a hand-written mask, so if sh4_opcode_list.cpp is ever retuned the
// pair simply stops matching and that opcode falls back to the full bracket.
//
// Exception caveat: a narrowed handler that raised an SH4 exception mid-op
// would bank-swap memory while our un-flushed pinned regs held the old values.
// That is why every opcode that can deliberately raise (trapa/sleep/illegal,
// the SR/FPSCR writers) stays on the full path. The remainder only fault the
// same way an ordinary shop_readm/shop_writem can, which this backend already
// assumes cannot touch context GPRs (see reg_flush_all()'s comment above).
// ============================================================================

// Opcode-table mask constants (private copies of the #defines in
// sh4_opcode_list.cpp, which are not exported in the header).
static const u32 IFB_M_NONE = 0xFFFF;
static const u32 IFB_M_N    = 0xF0FF;
static const u32 IFB_M_NM   = 0xF00F;
static const u32 IFB_M_IMM8 = 0xFF00;

// Every SH4 GPR — the mask a bank-swapping handler needs. r11 is included and
// simply drops out in reg_flush_mask(), since GetIntReg() never pins it.
static const u32 IFB_GPR_ALL = 0xFFFFu;

// What one ifb call-out has to spill, on the two independent axes.
struct IfbBracket
{
	u32  gprs;	// bitmask over SH4 r0..r15 (IFB_GPR_ALL = all of them)
	bool fpu;	// also spill fr[0..15] (only costs anything when FPU_PIN is on)
};

// The GPR mask is the UNION of the handler's read and write sets: flushing a
// register it only reads is required (it must see a live value), and reloading
// one it never wrote just re-reads what we stored, so one mask drives both
// directions.
static IfbBracket ifb_bracket(u16 sh4op)
{
	const sh4_opcodelistentry* d = OpDesc[sh4op];
	const u32 Rn = 1u << ((sh4op >> 8) & 0xF);
	const u32 Rm = 1u << ((sh4op >> 4) & 0xF);
	const u32 R0 = 1u << 0;

	#define IFB_IS(mm,rr) (d->mask==(mm) && d->rez==(rr))
	#define IFB_B(g,f)    { IfbBracket b={(g),(f)}; return b; }

	// --- touches no GPR and no float -------------------------------------
	if (IFB_IS(IFB_M_NONE,0x0028)) IFB_B(0,false)	// clrmac      -> MACH/MACL
	if (IFB_IS(IFB_M_NONE,0x0038)) IFB_B(0,false)	// ldtlb       -> PTEH/PTEL

	// --- Rn only ----------------------------------------------------------
	if (IFB_IS(IFB_M_N,0x0002)) IFB_B(Rn,false)	// stc SR,Rn      (reads sr only)
	if (IFB_IS(IFB_M_N,0x4003)) IFB_B(Rn,false)	// stc.l SR,@-Rn  (reads sr only)
	if (IFB_IS(IFB_M_N,0x4024)) IFB_B(Rn,false)	// rotcl Rn
	if (IFB_IS(IFB_M_N,0x4025)) IFB_B(Rn,false)	// rotcr Rn
	if (IFB_IS(IFB_M_N,0x401B)) IFB_B(Rn,false)	// tas.b @Rn

	// --- Rn and Rm --------------------------------------------------------
	if (IFB_IS(IFB_M_NM,0x000F)) IFB_B(Rn|Rm,false)	// mac.l @Rm+,@Rn+
	if (IFB_IS(IFB_M_NM,0x400F)) IFB_B(Rn|Rm,false)	// mac.w @Rm+,@Rn+
	if (IFB_IS(IFB_M_NM,0x200C)) IFB_B(Rn|Rm,false)	// cmp/str Rm,Rn
	if (IFB_IS(IFB_M_NM,0x200D)) IFB_B(Rn|Rm,false)	// xtrct Rm,Rn
	if (IFB_IS(IFB_M_NM,0x3004)) IFB_B(Rn|Rm,false)	// div1 Rm,Rn
	if (IFB_IS(IFB_M_NM,0x300A)) IFB_B(Rn|Rm,false)	// subc Rm,Rn
	if (IFB_IS(IFB_M_NM,0x300B)) IFB_B(Rn|Rm,false)	// subv Rm,Rn
	if (IFB_IS(IFB_M_NM,0x300E)) IFB_B(Rn|Rm,false)	// addc Rm,Rn
	if (IFB_IS(IFB_M_NM,0x300F)) IFB_B(Rn|Rm,false)	// addv Rm,Rn
	if (IFB_IS(IFB_M_NM,0x6008)) IFB_B(Rn|Rm,false)	// swap.b Rm,Rn
	if (IFB_IS(IFB_M_NM,0x600A)) IFB_B(Rn|Rm,false)	// negc Rm,Rn

	// --- implicit R0 (GBR-relative byte ops) ------------------------------
	if (IFB_IS(IFB_M_IMM8,0xCC00)) IFB_B(R0,false)	// tst.b #imm,@(R0,GBR)
	if (IFB_IS(IFB_M_IMM8,0xCD00)) IFB_B(R0,false)	// and.b #imm,@(R0,GBR)
	if (IFB_IS(IFB_M_IMM8,0xCE00)) IFB_B(R0,false)	// xor.b #imm,@(R0,GBR)
	if (IFB_IS(IFB_M_IMM8,0xCF00)) IFB_B(R0,false)	// or.b  #imm,@(R0,GBR)

	// --- FPSCR writers: Rn only, but the float file must be spilled -------
	// UpdateFPSCR -> ChangeFP() swaps fr_hex[i]<->xf_hex[i] IN MEMORY on an
	// FPSCR.FR toggle, so pinned fr[] must be written out before the call and
	// re-read after. Neither handler writes any GPR — it only sources the new
	// FPSCR value from Rn (post-incrementing it in the .l form).
	if (IFB_IS(IFB_M_N,0x406A)) IFB_B(Rn,true)	// lds Rn,FPSCR
	if (IFB_IS(IFB_M_N,0x4066)) IFB_B(Rn,true)	// lds.l @Rn+,FPSCR

	// --- SR writers: full GPR bank, but the float file is untouched -------
	// UpdateSR() bank-swaps r0..r7 <-> r0_bank..r7_bank in memory on an SR.RB
	// toggle, so every pinned GPR has to be coherent across the call. SR has no
	// FP bank bit — SR.FD disables the FPU, it does not swap fr[]/xf[] — so the
	// 32 float ops the old single-sentinel path emitted here were pure waste.
	if (IFB_IS(IFB_M_N,0x400E)) IFB_B(IFB_GPR_ALL,false)	// ldc Rn,SR
	if (IFB_IS(IFB_M_N,0x4007)) IFB_B(IFB_GPR_ALL,false)	// ldc.l @Rn+,SR

	// --- double-precision converts: float only, no GPR --------------------
	if (IFB_IS(IFB_M_N,0xF0BD)) IFB_B(0,true)	// fcnvds DRn,FPUL
	if (IFB_IS(IFB_M_N,0xF0AD)) IFB_B(0,true)	// fcnvsd FPUL,DRn

	#undef IFB_B
	#undef IFB_IS

	// Not on the list (trapa, sleep, illegal, double-precision FPU fallbacks):
	// the legacy everything-spill.
	IfbBracket full={IFB_GPR_ALL,true};
	return full;
}

// Memory ops one bracket costs, for the probe's reporting only.
static u32 ifb_bracket_memops(const IfbBracket& b)
{
	u32 n=0;
	for (u32 i=0;i<16;i++)
		if ((b.gprs&(1u<<i)) && GetIntReg(reg_r0+i)!=ppc_rinvalid)
			n+=2;					// one stw + one lwz
	if (b.fpu && get_fpu_pin_preset())
		n+=32;						// 16 stfs + 16 lfs
	return n;
}

// Flush/reload only the GPRs named in `gprs`. r11 is never pinned (GetIntReg
// returns invalid — the emitter keeps it as a volatile temp window), so it is
// already memory-coherent and drops out of the loop on its own.
static void reg_flush_mask(u32 gprs)
{
#if STATIC_GPR_ALLOC
	for (u32 i=0;i<16;i++)
	{
		if (!(gprs&(1u<<i)))
			continue;
		ppc_ireg ri=GetIntReg(reg_r0+i);
		if (ri!=ppc_rinvalid)
			ppc_stw(ri,ppc_contex,Sh4cntx.offset(reg_r0+i));
	}
#endif
}
static void reg_reload_mask(u32 gprs)
{
#if STATIC_GPR_ALLOC
	for (u32 i=0;i<16;i++)
	{
		if (!(gprs&(1u<<i)))
			continue;
		ppc_ireg ri=GetIntReg(reg_r0+i);
		if (ri!=ppc_rinvalid)
			ppc_lwz(ri,ppc_contex,Sh4cntx.offset(reg_r0+i));
	}
#endif
}

// ---------------------------------------------------------------------------
// IFB_PROBE — per-opcode execution counters
//
// Answers "is shop_ifb hot enough to be worth narrowing?" before committing to
// the A/B. Sites are keyed on the OpDesc[] ENTRY, not the raw opcode word, so
// all n/m variants of e.g. div1 collapse into one row and the table stays tiny
// (233 table entries exist in total; far fewer than 64 ever reach ifb).
//
// The counters live outside the code cache, so they survive recSh4_ClearCache()
// and keep accumulating across recompiles. ifb_probe_dump() is called once a
// second from the SPG stats block and resets them, so the numbers it prints are
// per-second rates.
// ---------------------------------------------------------------------------
#define IFB_SITE_MAX 64
struct IfbSite
{
	const sh4_opcodelistentry* desc;
	u32 hits;
	IfbBracket br;	// cached ifb_bracket() result, for the dump's cost columns
};
static IfbSite s_ifb_sites[IFB_SITE_MAX];
static u32 s_ifb_site_n = 0;
static u32 s_ifb_overflow = 0;

// Compile-time (not run-time) lookup: returns the counter this site should
// bump, or 0 if the registry is full.
static u32* ifb_probe_slot(u16 sh4op)
{
	const sh4_opcodelistentry* d = OpDesc[sh4op];

	for (u32 i=0;i<s_ifb_site_n;i++)
		if (s_ifb_sites[i].desc==d)
			return &s_ifb_sites[i].hits;

	if (s_ifb_site_n>=IFB_SITE_MAX)
	{
		s_ifb_overflow++;
		return 0;
	}

	IfbSite& s=s_ifb_sites[s_ifb_site_n];
	s.desc=d;
	s.hits=0;
	s.br=ifb_bracket(sh4op);
	return &s_ifb_sites[s_ifb_site_n++].hits;
}

extern "C" void ifb_probe_dump(double seconds)
{
	if (!get_ifb_probe_preset() || s_ifb_site_n==0 || seconds<=0.0)
		return;

	// Order by hit count, descending. Selection sort over <=64 entries once a
	// second is free, and it avoids dragging qsort into the build.
	u32 idx[IFB_SITE_MAX];
	u32 n=0;
	for (u32 i=0;i<s_ifb_site_n;i++)
		if (s_ifb_sites[i].hits)
			idx[n++]=i;

	if (n==0)
		return;

	for (u32 a=0;a<n;a++)
	{
		u32 best=a;
		for (u32 b=a+1;b<n;b++)
			if (s_ifb_sites[idx[b]].hits>s_ifb_sites[idx[best]].hits)
				best=b;
		u32 t=idx[a]; idx[a]=idx[best]; idx[best]=t;
	}

	// Cost of the legacy everything-spill, for the comparison column.
	IfbBracket full_br={IFB_GPR_ALL,true};
	const u32 full_cost = ifb_bracket_memops(full_br);

	u32 total=0, narrowed=0;
	double cost_full=0.0, cost_sel=0.0;
	for (u32 a=0;a<n;a++)
	{
		const IfbSite& s=s_ifb_sites[idx[a]];
		const u32 cost=ifb_bracket_memops(s.br);
		total += s.hits;
		cost_full += (double)s.hits*full_cost;
		cost_sel  += (double)s.hits*cost;
		if (cost<full_cost)
			narrowed += s.hits;
	}

	// Header states the preset actually in force. The old header did not, so a
	// log could not tell an ifb_flush=on run from an off one — which made the
	// first A/B unverifiable. Note both presets are read at CODEGEN time, so
	// this reflects what the compiled blocks were built with only if the preset
	// was set before the game launched.
	printf("[IFB] flush=%s fpu_pin=%s  %.2fs  total %u (%.0f/s)  narrowable %u (%.1f%%)  bracket memops/s: full %.0f -> sel %.0f (-%.1f%%)\n",
		get_ifb_flush_preset()?"ON":"OFF",
		get_fpu_pin_preset()?"ON":"OFF",
		seconds, total, total/seconds,
		narrowed, total?(narrowed*100.0/total):0.0,
		cost_full/seconds, cost_sel/seconds,
		cost_full>0.0?((cost_full-cost_sel)*100.0/cost_full):0.0);

	for (u32 a=0;a<n && a<12;a++)
	{
		const IfbSite& s=s_ifb_sites[idx[a]];
		u32 pop=0;
		for (u32 b=0;b<16;b++)
			if ((s.br.gprs&(1u<<b)) && GetIntReg(reg_r0+b)!=ppc_rinvalid)
				pop++;
		printf("[IFB]   %-34s %8u %8.0f/s  %2u gpr%s %3u ops (full %u)\n",
			s.desc->diss, s.hits, s.hits/seconds,
			pop, s.br.fpu?"+fpu":"    ",
			ifb_bracket_memops(s.br), full_cost);
	}

	if (s_ifb_overflow)
		printf("[IFB]   (%u distinct opcodes did not fit the %u-entry registry)\n",
			s_ifb_overflow, (u32)IFB_SITE_MAX);

	for (u32 i=0;i<s_ifb_site_n;i++)
		s_ifb_sites[i].hits=0;

	fflush(stdout);
}

