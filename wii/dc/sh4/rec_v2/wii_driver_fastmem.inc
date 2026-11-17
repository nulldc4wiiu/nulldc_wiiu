// wii_driver_fastmem.inc: fastmem shapes, the DSI trampolines, sq_guard (J2).
// Part of wii_driver.cpp, which #includes it: not compiled on its own.

// ============================================================================
// FASTMEM (PPC-MMU) — JIT side.  MMU/page-table/DSI-handler side is in
// wii/wii_fastmem.cpp; the FASTMEM preset (default OFF) gates everything.
//
// When active, shop_readm/shop_writem emit BRANCHLESS direct accesses
// through the low-EA window instead of the inline _vmem-table lookup:
//
//     rlwinm  rEA, rADDR, 0,3,31      ; EA = sh4_addr & 0x1FFFFFFF
//     [xori   rEA, rEA, 4-sz]         ; BE sub-word swizzle (sz<4)
//     l/st    ..., 0(rEA)             ; ONE access, no compare, no branch
//
// RAM (4 mirrors), VRAM (64-bit path) and AICA wave RAM are page-mapped, so
// those never fault. Anything else (MMIO, SQ-after-masking, BIOS, VRAM
// 32-bit path, TA FIFO, OC-RAM...) takes a DSI; rec_fastmem_patch() below
// decodes the faulting site and overwrites the access instruction with a
// `b trampoline` into s_fm_pool, then the DSI handler RETRIES the PC. The
// slow path therefore always runs in normal context (interrupts enabled) —
// MMIO with side effects such as GD-ROM DMA file I/O stays safe. After the
// one-time patch a site costs the same as the legacy cold path, while
// never-faulting sites stay at 2-4 instructions.
//
// The shapes are FIXED and the EA scratch register doubles as the shape
// discriminator for the fault-time decoder — any change to the emission in
// shop_readm/shop_writem MUST be mirrored here:
//
//   scalar read : rlwinm r4 / [xori r4] / lbz|lha|lwz rD,0(r4) [/ extsb rD]
//   pair   read : rlwinm r5 / lwz r3,0(r5) / lwz r4,4(r5)
//   scalar write: rlwinm r5 / [xori r5] / stb|sth|stw rS,0(r5)
//   pair   write: rlwinm r4 / stw r5,0(r4) / stw r6,4(r4)
//
// JIT_FMOV adds four FLOAT variants of the same four shapes, used when the
// value's SH4 register is a pinned FR (FPU_PIN) so the access can name the
// FPR directly instead of bouncing through a GPR and the context slot:
//
//   scalar read : rlwinm r4 / lfs  fD,0(r4)
//   pair   read : rlwinm r5 / lfs  fD,0(r5)  / lfs  fD+1,4(r5)
//   scalar write: rlwinm r5 / stfs fS,0(r5)
//   pair   write: rlwinm r4 / stfs fS,0(r4)  / stfs fS+1,4(r4)
//
// Scratch registers and displacements are identical to the integer shapes —
// only the opcode differs (lfs 48 / stfs 52 vs lwz 32 / stw 36) — so the
// decoder's existing read/write and scalar/pair discrimination is untouched.
// Float shapes are always 4 bytes wide, hence no xori and no andi mask.
//
// The rlwinm never clobbers the address register, so the trampoline can
// pass the ORIGINAL SH4 address (recovered from the decoded rlwinm source
// field) to the generic ReadMem/WriteMem dispatchers — this is what keeps
// P4/SQ correct even though masking folds 0xE0000000 onto 0x00000000.
//
// Store-queue writes are the geometry hot path and used to hit the inline
// table lookup, so a write site whose first fault lands in the area-0
// image (DAR < 0x04000000 — where masked SQ addresses land) gets an
// inlined "SQ? -> direct sq_both store" fast path ahead of the generic
// call. Every trampoline ends in a generic path, so polymorphic sites are
// always correct, merely slower.
//
// LIMITATION (harmless): if only the SECOND word of a pair faults (a
// non-8-aligned fmov.d straddling a region edge), the original address is
// gone and the trampoline dispatches on the MASKED EA+4. That is identical
// for every normal region and can only differ for P4/SQ — which cannot
// straddle-fault, because SQ is fully unmapped and the FIRST word always
// faults there.
// ============================================================================

#if defined(__WIIU__)
extern u8* CodeCache;
#else
extern u8 CodeCache[];
#endif
extern "C" int get_fastmem_preset();	// main.cpp: 0=off (default), 1=on

static inline bool fastmem_on()
{
	return g_wii_fastmem_active && get_fastmem_preset();
}

// Trampoline pool. MEM1 .bss like CodeCache, so `b` reach (±32 MB) always
// holds (checked at emit time anyway). Reset together with the block cache:
// patched sites die with their blocks, so their trampolines must too.
#if defined(__WIIU__)
// Wii U (wiiu/fastmem_wiiu.cpp): the pool has to be in the codegen area to execute, and the window is a 512 MB-aligned
// block of virtual space, not EA 0, so every shape ORs its base in after the mask (fm_ea). fastmem=2 (Cemu, which can
// neither alias memory nor raise DSIs) gives every site its trampoline at compile time, as if each had faulted:
// a much bigger pool, and blocks stop compiling (cache clear) before it runs out.
static u8* s_fm_pool = 0;
static u32 FM_POOL_SIZE = 512*1024;	// fastmem=2: 4 MB (the console's codegen area is 32 MB, Cemu's 16)
extern u32 g_wiiu_fm_window;
extern int g_wiiu_fm_prepatch;
#define FM_WINDOW g_wiiu_fm_window
#define FM_SITES_MAX 512
static u32* s_fm_sites[FM_SITES_MAX];	// this block's fault sites, for fastmem=2
static u32 s_fm_sites_n;
#else
#define FM_POOL_SIZE (128*1024)
static u8  s_fm_pool[FM_POOL_SIZE] __attribute__((aligned(32)));
#define FM_WINDOW 0u
#endif
static u32 s_fm_pool_used   = 0;

#if defined(__WIIU__)
#define FM_ACCESS(indexed,dform) do { if (g_exp_fm_x) { indexed; } else { dform; } } while (0)
#else
#define FM_ACCESS(indexed,dform) dform
#endif

// The address part of every fastmem shape: rD = window + (addr & 0x1FFFFFFF). `before` is the number of instructions
// the caller emits between this and the access (the sub-word xori), so fastmem=2 knows where the fault site lands.
static void fm_ea(u32 rd,u32 rs,u32 before,bool pair=false)
{
	ppc_rlwinmx(rd,rs,0,3,31,0);
#if defined(__WIIU__)
	if (pair || !g_exp_fm_x)	// a pair's second word needs a displacement, which indexed forms don't have
		ppc_oris(rd,rd,FM_WINDOW>>16);
	if (g_wiiu_fm_prepatch && s_fm_sites_n < FM_SITES_MAX)
		s_fm_sites[s_fm_sites_n++] = (u32*)emit_GetCCPtr() + before;
#endif
}
static u32 s_fm_patch_count = 0;

// Exception-context diagnostics. printf/stdio is FORBIDDEN anywhere in the
// resumable DSI path (hardware lesson from the 2026-07-19 MMU experiment:
// newlib's stdout lock can invoke the thread scheduler with EE=0 and
// deadlock after the line is emitted). Failures leave a record here for
// the panic screen / post-mortem instead, and the patch ring below can be
// inspected from a RAM dump; the cumulative count is printed from NORMAL
// context on each cache clear (rec_fastmem_reset_pool).
static volatile u32 s_fm_fail_pc, s_fm_fail_insn, s_fm_fail_dar;
static volatile u32 s_fm_fail_reason;	// 1=pc outside cache 2=pool full 3=undecodable 4=branch range
#define FM_RING 32
static u32 s_fm_ring[FM_RING];		// last FM_RING patched site PCs
static u32 s_fm_ring_n;

// JIT_FMOV tripwire. The preset needs BOTH fpu_pin and fastmem to be on to do
// anything, and silently emits the legacy GPR bounce otherwise — exactly the
// way to waste a hardware A/B run on a switch that never engaged. This counts
// float shapes actually emitted and announces the first one, so the log says
// plainly whether the thing under test is in the build that ran.
static u32 s_fm_float_sites = 0;
static void fm_note_float_site();
#if defined(__WIIU__)
// sq_guard (J2-1): a fastmem float store tests at compile time whether its address is the store queue
// (E0000000-E3FFFFFF: top 6 bits 0x38), instead of faulting into a trampoline on first use and detouring there
// forever. DOA2 fills the store queue with fmov.d (4 pair stores per 32-byte block, ~27K blocks a frame).
//   1: store queue inline (rlwinm, cmpwi, bne, rlwinm, addis, stfs x2), other targets in a cold fragment that holds
//      the normal fastmem shape (RAM direct; anything else still faults and patches as before)
//   2: the normal fastmem shape inline behind a beq (+3 instructions for a RAM store), the store queue in the cold
//      fragment (2 taken branches, no trampoline)
//   +4: single-float stores (fmov.s) too, not only pairs
// The EA scratch is the shape's own (rarg1 for pairs, rarg2 for scalars): free, as the data is in FPRs.
int g_exp_sq_guard = 2;   // on: 2 (console r3a: DOA2 fights +1.1%); 0 turns it off
u32 g_cnt_sqguard;   // compiled guards
// s = &sq_both[a & 0x3F] minus the returned displacement (folded into the stores when it fits)
static s32 j2_sq_ea(u32 s,u32 a,u32 extra)
{
	ppc_rlwinmx(s,a,0,26,31,0);
	const u32 sq=(u32)(uintptr_t)sq_both;
	const s32 lo=(s32)(s16)sq;
	ppc_addis(s,s,(sq-(u32)lo)>>16);
	if (lo+(s32)extra>32767)
	{
		ppc_addi(s,s,(u32)lo);
		return 0;
	}
	return lo;
}
static void j2_sq_store(u32 fs,u32 s,s32 d,bool pair)
{
	ppc_stfs(fs,s,(u32)d);
	if (pair)
		ppc_stfs(fs+1,s,(u32)(d+4));
}
// the normal fastmem float shape (fault site(s) for anything but RAM, as without the guard)
static void j2_fm_shape(u32 fs,u32 areg,bool pair)
{
	if (pair)
	{
		fm_ea(ppc_rarg1,ppc_rarg0,0,true);
		fm_note_float_site();
		ppc_stfs(fs,ppc_rarg1,0);
		ppc_stfs(fs+1,ppc_rarg1,4);
	}
	else
	{
		fm_ea(ppc_rarg2,areg,0);
		fm_note_float_site();
		FM_ACCESS(ppc_stfsx(fs,ppc_r12,ppc_rarg2),ppc_stfs(fs,ppc_rarg2,0));
	}
}
// shop_writem: emits the guarded store and its cold fragment; false = not used here (emit the plain shape)
static bool j2_emit(u32 fs,u32 areg,bool pair)
{
	const int mode=g_exp_sq_guard&3;
	if (mode!=1 && mode!=2)
		return false;
	if ((!pair && !(g_exp_sq_guard&4)) || s_cold_n>=1024)
		return false;
	const u32 s=pair?(u32)ppc_rarg1:(u32)ppc_rarg2;
	const u32 a=pair?(u32)ppc_rarg0:areg;
	ppc_rlwinmx(s,a,6,26,31,0);			// the address's top 6 bits
	ppc_cmpwi(s,0x38);
	ppc_label* cold=ppc_CreateLabel();
	if (mode==1)
	{
		ppc_bcx(BO_FALSE,BI_CR0_EQ,0,0,0);	// bne: not the store queue
		j2_sq_store(fs,s,j2_sq_ea(s,a,pair?4:0),pair);
	}
	else
	{
		ppc_bcx(BO_TRUE,BI_CR0_EQ,0,0,0);	// beq: the store queue
		j2_fm_shape(fs,areg,pair);
	}
	ColdFrag& c=s_cold[s_cold_n++];
	c.beq=cold; c.done=emit_GetCCPtr(); c.kind=6; c.sz=pair?8:4; c.rdreg=(u8)mode;
	c.datareg=0; c.areg=(u8)a; c.fdreg=(u8)fs; c.fofs=0; c.fuct=0;
	g_cnt_sqguard++;
	return true;
}
static void j2_cold(const ColdFrag& c)
{
	const bool pair=c.sz==8;
	if (c.rdreg==1)
		j2_fm_shape(c.fdreg,c.areg,pair);
	else
	{
		const u32 s=pair?(u32)ppc_rarg1:(u32)ppc_rarg2;
		j2_sq_store(c.fdreg,s,j2_sq_ea(s,c.areg,pair?4:0),pair);
	}
}
#endif
static void fm_note_float_site()
{
	// Compile time, normal context: printf is OK here (never in the DSI path).
	// Muted: the counter dies with the pool on every cache clear, so the "first"
	// site announces itself again a few times a second. Uncomment to confirm on
	// hardware that a JIT_FMOV build really emitted float shapes.
	s_fm_float_sites++;
	//if (s_fm_float_sites == 1)
	//	printf("[fmov] JIT_FMOV active: emitting lfs/stfs fastmem shapes\n");
}

// Float sites that actually FAULTED and got a trampoline built. Emitted-vs-
// patched is the distinction that matters: emitting a float shape exercises
// nothing but the emitter, while a patch is the only proof that the DSI
// decoder and its trampolines — the exception-context code, where a mis-decode
// is a panic rather than a slow path — have been down the float path at all.
// A log showing thousands of float shapes emitted and ZERO patched means the
// risky half of JIT_FMOV is still untested, however well the game ran.
static u32 s_fm_float_patches = 0;

static void rec_fastmem_reset_pool()
{
	// Runs in normal context (bm_Reset / recSh4_ClearCache) — printf is OK.
	// Muted: one line per cache clear is a log flood. Uncomment when validating
	// fastmem — "float shapes emitted / float sites patched" is the line that
	// says whether the DSI float path has been exercised at all.
	//if (s_fm_patch_count)
	//	printf("[fastmem] cache clear: %u sites were patched (%u/%u B pool),"
	//	       " %u float shapes emitted / %u float sites patched\n",
	//	       s_fm_patch_count, s_fm_pool_used, FM_POOL_SIZE,
	//	       s_fm_float_sites, s_fm_float_patches);
	// Tell the [CC] census where a call-out's return address has to land to
	// count as "came from a back-patched trampoline".
	g_ccall_tramp_lo = s_fm_pool;
	g_ccall_tramp_hi = s_fm_pool + FM_POOL_SIZE;
	g_ccall_cc_lo    = CodeCache;
	g_ccall_cc_hi    = CodeCache + CODE_SIZE;
	s_fm_pool_used     = 0;
	s_fm_patch_count   = 0;
	s_fm_ring_n        = 0;
	s_fm_float_sites   = 0;	// the sites die with the cache, like the patches
	s_fm_float_patches = 0;
}

// --- minimal raw-word emitter --------------------------------------------
// Deliberately independent of the global emit_ptr/LastAddr state: this runs
// INSIDE the DSI handler and must not disturb an in-progress compilation.
// (Also: integer code only — MSR.FP is off in the exception context.)
static u32* fm_p;
static void fm_w(u32 i)                            { *fm_p++ = i; }
static u32  fm_dform(u32 op6,u32 rt,u32 ra,u32 dd) { return (op6<<26)|(rt<<21)|(ra<<16)|(dd&0xFFFF); }
static void fm_mr(u32 dst,u32 src)                 { if (dst!=src) fm_w(0x7C000378|(src<<21)|(dst<<16)|(src<<11)); }
static bool fm_branch(void* target,u32 lk)
{
#if defined(__WIIU__)
	// calls into the app: on a console its code sits ~170 MB from the codegen area, past bl's +-32 MB, so call
	// through r12/CTR (both volatile across the C call anyway)
	if (lk)
	{
		const u32 a=(u32)(uintptr_t)target;
		fm_w(0x3D800000|(a>>16));		// lis   r12,a@h
		fm_w(0x618C0000|(a&0xFFFF));		// ori   r12,r12,a@l
		fm_w(0x7D8903A6);			// mtctr r12
		fm_w(0x4E800421);			// bctrl
		if (g_exp_fm_x)
			fm_w(0x3D800000|(FM_WINDOW>>16));	// lis r12,window
		return true;
	}
#endif
	snat disp=(u8*)target-(u8*)fm_p;
	if (disp>=33554432 || disp<-33554432)
		return false;
	fm_w(0x48000000|((u32)disp&0x03FFFFFC)|lk);
	return true;
}

// SQ fast-path address: reg = &sq_both[(areg & 0x3F) ^ swz]  (same layout as
// the _vmem direct block: BE u32 words, sub-word swizzle folded into addr).
// (NB: the scratch register parameter must not be called `r` —
// sh4_registers.h #defines that to Sh4cntx.r.)
static void fm_sq_addr(u32 reg,u32 areg,u32 swz)
{
	fm_w(0x54000000|(areg<<21)|(reg<<16)|(0<<11)|(26<<6)|(31<<1));	// rlwinm reg,areg,0,26,31
	if (swz)
		fm_w(0x68000000|(reg<<21)|(reg<<16)|swz);		// xori reg,reg,swz
	u32 sq=(u32)(uintptr_t)sq_both;
	s32 lo=(s32)(s16)sq;
	u32 hi=((u32)(sq-(u32)lo))>>16;
	fm_w(fm_dform(15,reg,reg,hi));					// addis reg,reg,hi
	fm_w(fm_dform(14,reg,reg,(u32)lo));				// addi  reg,reg,lo
}

// JIT_FMOV: context byte offset of the pinned FR named by a float shape's
// register field. GetFloatReg maps fr[n] -> f14+n and fr[] is contiguous, so
// the slot follows from the register number alone — which matters because this
// runs in exception context, where Sh4Context::offset() (a switch through
// Sh4_int_GetRegisterPtr plus verify()) is not callable. `pair` additionally
// requires fD+1 to be inside the pinned bank, so a mis-decode can never make
// the trampoline write past fr[15]. Returns -1 when the register is outside
// the bank (no emitted shape does that; this is the belt-and-braces check).
static int fm_fr_ofs(u32 fd,bool pair)
{
	const u32 lastf = pair ? (u32)(ppc_f14+14) : (u32)(ppc_f14+15);
	if (fd < (u32)ppc_f14 || fd > lastf)
		return -1;
	return (int)(SH4CTX_OFS_FR + (fd-(u32)ppc_f14)*4);
}

// Locate the governing `rlwinm rEA, rADDR, 0,3,31` within the 3 insns
// before the fault site; returns rADDR (which still holds the ORIGINAL
// SH4 address — the shapes never clobber it) or -1 if absent (not ours).
static int fm_find_areg(u32* site,u32 ea_reg)
{
	for (int back=1;back<=3;back++)
	{
		u32 i=site[-back];
		if ((i&0xFC1FFFFF)==(0x54000000|(ea_reg<<16)|0x00FE))
			return (int)((i>>21)&31);
	}
	return -1;
}

// Decode the faulting fastmem site at pc, build its trampoline, patch the
// site. Returns 0 on success; nonzero makes the DSI handler chain to the
// libogc panic screen. Called from WiiFastmem_HandleDSI (exception context:
// interrupts off, FPU off — keep this integer-only and printf-free except
// on the failure/debug paths).
static int rec_fastmem_patch_dar(unsigned int pc,u32 dar);
#if !defined(__WIIU__)
int rec_fastmem_patch(unsigned int pc)
{
	u32 dar;
	asm volatile("mfspr %0,19" : "=r"(dar));	// DAR = faulting (masked) EA
	return rec_fastmem_patch_dar(pc,dar);
}
#else
// wiiu/fastmem_wiiu.cpp's DSI callback (DAR comes from the exception context: mfspr DAR is privileged there)
extern "C" int wiiu_fastmem_patch(unsigned int pc,u32 dar) { return rec_fastmem_patch_dar(pc,dar); }
// for the crash report (normal context): why the last patch was refused
extern "C" void wiiu_fastmem_failure(u32* reason,u32* pc,u32* insn,u32* dar)
{
	*reason=s_fm_fail_reason; *pc=s_fm_fail_pc; *insn=s_fm_fail_insn; *dar=s_fm_fail_dar;
}
#endif

// rD = (window EA & 0x1FFFFFFF) + 4: the masked Dreamcast address of a pair's second word
static void fm_ea_plus4(u32 rd,u32 rea)
{
#if defined(__WIIU__)
	fm_w(0x54000000|(rea<<21)|(rd<<16)|(3<<6)|(31<<1));	// rlwinm rd,rea,0,3,31
	fm_w(fm_dform(14,rd,rd,4));				// addi rd,rd,4
#else
	fm_w(fm_dform(14,rd,rea,4));				// addi rd,rea,4
#endif
}

static int rec_fastmem_patch_dar(unsigned int pc,u32 dar)
{
	const u32 dca=dar-FM_WINDOW;	// the masked Dreamcast address that faulted

	// Only faults inside the code cache can be fastmem sites.
	if (pc < (u32)(uintptr_t)&CodeCache[0] || pc >= (u32)(uintptr_t)&CodeCache[0]+CODE_SIZE)
	{
		s_fm_fail_pc=pc; s_fm_fail_insn=0; s_fm_fail_dar=dar; s_fm_fail_reason=1;
		return 1;
	}

	u32* site=(u32*)pc;
	const u32 insn=*site;
	u32 op6=insn>>26;
	const u32 rt =(insn>>21)&31;	// rD (loads) / rS (stores)
	u32 ra =(insn>>16)&31;	// base = EA scratch = shape discriminator
	u32 dd =insn&0xFFFF;
#if defined(__WIIU__)
	// exp_fm_x: an indexed access off r12 is the same site as its D-form; rB holds the masked address
	if (op6==31 && ra==12)
	{
		const u32 xo=(insn>>1)&0x3FF;
		op6= xo==23?32: xo==87?34: xo==343?42: xo==535?48: xo==151?36: xo==215?38: xo==407?44: xo==663?52: 0;
		ra=(insn>>11)&31;
		dd=0;
	}
#endif

	if (s_fm_pool_used + 32*sizeof(u32) > FM_POOL_SIZE)
	{
		s_fm_fail_pc=pc; s_fm_fail_insn=insn; s_fm_fail_dar=dar; s_fm_fail_reason=2;
		return 1;
	}

#if defined(__WIIU__)
	wiiu_codegen_write_begin();	// the pool is in the codegen area
#endif
	fm_p=(u32*)&s_fm_pool[s_fm_pool_used];
	u32* const tramp=fm_p;
	bool ok=false;
#if defined(__WIIU__)
	// fastmem=2 never runs the window accesses, so prove r12 here: trap unless it holds the window base
	if (g_wiiu_fm_prepatch && g_exp_fm_x)
	{
		fm_w(0x3C000000|(FM_WINDOW>>16));	// lis  r0,window
		fm_w(0x7C0C0000);			// cmpw r12,r0
		fm_w(0x41820008);			// beq  +8
		fm_w(0x7FE00008);			// trap
	}
#endif

	if ((op6==32||op6==42||op6==34) && ra==ppc_rarg1 && dd==0)
	{
		// --- scalar read: lbz/lha/lwz rD,0(r4). Replicates ColdFrag kind 0.
		int areg=fm_find_areg(site,ra);
		if (areg>=0)
		{
			ok=true;
			fm_mr(ppc_rarg0,(u32)areg);
			if (op6==34)		// lbz — ReadMem8, then extsb rD,r3
			{
				ok&=fm_branch((void*)ReadMem8,1);
				fm_w(0x7C000774|(ppc_rrv0<<21)|(rt<<16));
			}
			else if (op6==42)	// lha — ReadMem16, then extsh rD,r3
			{
				ok&=fm_branch((void*)ReadMem16,1);
				fm_w(0x7C000734|(ppc_rrv0<<21)|(rt<<16));
			}
			else			// lwz — ReadMem32
			{
				ok&=fm_branch((void*)ReadMem32,1);
				fm_mr(rt,ppc_rrv0);
			}
			// Resume after the load; a trailing in-line extsb (sz==1
			// shape) re-runs on the already-extended value — idempotent.
			ok&=fm_branch(site+1,0);
		}
	}
	else if (op6==32 && ra==ppc_rarg2 && rt==ppc_rrv0 && dd==0)
	{
		// --- pair read, first word: lwz r3,0(r5). Address still in r3
		// (the pair shape always sources rarg0), so ReadMem64 needs no
		// setup and returns r3:r4 = high:low exactly as the shape does.
		if (fm_find_areg(site,ra)==ppc_rarg0)
		{
			ok=fm_branch((void*)ReadMem64,1);
			ok&=fm_branch(site+2,0);	// skip the second lwz too
		}
	}
	else if (op6==32 && ra==ppc_rarg2 && rt==ppc_rrv1 && dd==4)
	{
		// --- pair read, second word alone (straddling pair; see LIMITATION).
		if (fm_find_areg(site,ra)>=0)
		{
			ok=true;
			fm_mr(ppc_rarg3,ppc_rrv0);			// save word[addr]
			fm_ea_plus4(ppc_rarg0,ppc_rarg2);		// r3 = masked EA+4
			ok&=fm_branch((void*)ReadMem32,1);
			fm_mr(ppc_rrv1,ppc_rrv0);
			fm_mr(ppc_rrv0,ppc_rarg3);
			ok&=fm_branch(site+1,0);
		}
	}
	else if ((op6==36||op6==44||op6==38) && ra==ppc_rarg2 && dd==0)
	{
		// --- scalar write: stb/sth/stw rS,0(r5). Replicates ColdFrag kind 2,
		// with an inlined SQ fast path for area-0-image faults.
		int areg=fm_find_areg(site,ra);
		if (areg>=0)
		{
			const u32 sz=(op6==38)?1:(op6==44)?2:4;
			ok=true;
			if (dca < 0x04000000)
			{
				fm_w(0x54000000|((u32)areg<<21)|(ppc_rarg2<<16)|(6<<11)|(26<<6)|(31<<1));	// rlwinm r5,areg,6,26,31 (addr>>26)
				fm_w(0x2C000000|(ppc_rarg2<<16)|0x38);		// cmpwi r5,0x38 — SQ region?
				u32* bne=fm_p;
				fm_w(0x40820000);				// bne -> generic (patched below)
				fm_sq_addr(ppc_rarg2,(u32)areg,(sz<4)?(4-sz):0);
				fm_w(fm_dform(op6,rt,ppc_rarg2,0));		// st{b,h,w} rS,0(r5)
				ok&=fm_branch(site+1,0);
				*bne |= ((u32)((u8*)fm_p-(u8*)bne))&0xFFFC;	// resolve bne -> generic
			}
			fm_mr(ppc_rarg0,(u32)areg);
			fm_mr(ppc_rarg1,rt);
			if (sz==1)      fm_w(0x70000000|(ppc_rarg1<<21)|(ppc_rarg1<<16)|0xFF);	// andi. r4,r4,0xFF
			else if (sz==2) fm_w(0x70000000|(ppc_rarg1<<21)|(ppc_rarg1<<16)|0xFFFF);
			ok&=fm_branch((sz==1)?(void*)WriteMem8:(sz==2)?(void*)WriteMem16:(void*)WriteMem32,1);
			ok&=fm_branch(site+1,0);
		}
	}
	else if (op6==36 && ra==ppc_rarg1 && rt==ppc_rarg2 && dd==0)
	{
		// --- pair write, first word: stw r5,0(r4). Address still in r3,
		// data in r5:r6 — the exact WriteMem64(u32,u64) EABI registers.
		if (fm_find_areg(site,ra)==ppc_rarg0)
		{
			ok=true;
			if (dca < 0x04000000)
			{
				// fmov.d bursts into the SQ are THE geometry path.
				fm_w(0x54000000|(ppc_rarg0<<21)|(ppc_rarg1<<16)|(6<<11)|(26<<6)|(31<<1));	// rlwinm r4,r3,6,26,31
				fm_w(0x2C000000|(ppc_rarg1<<16)|0x38);		// cmpwi r4,0x38
				u32* bne=fm_p;
				fm_w(0x40820000);				// bne -> generic
				fm_sq_addr(ppc_rarg1,ppc_rarg0,0);		// 8-aligned: no swizzle
				fm_w(fm_dform(36,ppc_rarg2,ppc_rarg1,0));	// stw r5,0(r4)
				fm_w(fm_dform(36,ppc_rarg3,ppc_rarg1,4));	// stw r6,4(r4)
				ok&=fm_branch(site+2,0);
				*bne |= ((u32)((u8*)fm_p-(u8*)bne))&0xFFFC;
			}
			ok&=fm_branch((void*)WriteMem64,1);
			ok&=fm_branch(site+2,0);
		}
	}
	else if (op6==36 && ra==ppc_rarg1 && rt==ppc_rarg3 && dd==4)
	{
		// --- pair write, second word alone (straddling pair; see LIMITATION).
		if (fm_find_areg(site,ra)>=0)
		{
			ok=true;
			fm_ea_plus4(ppc_rarg0,ppc_rarg1);		// r3 = masked EA+4
			fm_mr(ppc_rarg1,ppc_rarg3);
			ok&=fm_branch((void*)WriteMem32,1);
			ok&=fm_branch(site+1,0);
		}
	}
	// ---- JIT_FMOV float shapes (lfs op6=48 / stfs op6=52) -------------------
	// Same EA scratch registers and displacements as the integer shapes above
	// — the opcode alone tells them apart — so the read/write and scalar/pair
	// discrimination is unchanged. rt is an FPR number here, not a GPR.
	//
	// These cases are deliberately NOT gated on get_jit_fmov_preset(): a site
	// is only ever EMITTED with the preset on, but if the preset were flipped
	// off while blocks compiled under it are still in the cache, gating here
	// would turn a perfectly valid site into an undecodable one — a panic
	// instead of a slow path. A decoder must accept everything the emitter can
	// produce, unconditionally.
	//
	// The slow C dispatchers deal in integers, so each trampoline bounces
	// through the FR's OWN context slot (fm_fr_ofs) — the one place that is
	// safe to clobber, since it is the slot the FR would flush to anyway.
	// f14..f31 are callee-saved under the PPC EABI, so a pinned FR survives
	// the ReadMem/WriteMem call; that is why the pair straddle cases need none
	// of the save/restore their integer counterparts do.
	else if (op6==48 && ra==ppc_rarg1 && dd==0)
	{
		// --- scalar float read: lfs fD,0(r4)  (fmov.s, always 4 bytes).
		int areg=fm_find_areg(site,ra);
		int fofs=fm_fr_ofs(rt,false);
		if (areg>=0 && fofs>=0)
		{
			ok=true;
			fm_mr(ppc_rarg0,(u32)areg);
			ok&=fm_branch((void*)ReadMem32,1);
			fm_w(fm_dform(36,ppc_rrv0,ppc_contex,(u32)fofs));	// stw r3,fofs(r30)
			fm_w(fm_dform(48,rt,ppc_contex,(u32)fofs));		// lfs fD,fofs(r30)
			ok&=fm_branch(site+1,0);
		}
	}
	else if (op6==48 && ra==ppc_rarg2 && dd==0)
	{
		// --- pair float read, first word: lfs fD,0(r5). Address is still in
		// r3 (the pair shape always sources rarg0), so ReadMem64 needs no
		// setup; it returns r3:r4 = word[addr]:word[addr+4] -> fD:fD+1.
		int fofs=fm_fr_ofs(rt,true);
		if (fm_find_areg(site,ra)==ppc_rarg0 && fofs>=0)
		{
			ok=fm_branch((void*)ReadMem64,1);
			fm_w(fm_dform(36,ppc_rrv0,ppc_contex,(u32)fofs));	// stw r3,fofs(r30)
			fm_w(fm_dform(36,ppc_rrv1,ppc_contex,(u32)fofs+4));	// stw r4,fofs+4(r30)
			fm_w(fm_dform(48,rt,ppc_contex,(u32)fofs));		// lfs fD,  fofs(r30)
			fm_w(fm_dform(48,rt+1,ppc_contex,(u32)fofs+4));		// lfs fD+1,fofs+4(r30)
			ok&=fm_branch(site+2,0);				// skip the second lfs
		}
	}
	else if (op6==48 && ra==ppc_rarg2 && dd==4)
	{
		// --- pair float read, second word alone (straddling; see LIMITATION).
		// The first word is already in fD-1, which the call cannot disturb.
		int fofs=fm_fr_ofs(rt,false);
		if (fm_find_areg(site,ra)>=0 && fofs>=0)
		{
			ok=true;
			fm_ea_plus4(ppc_rarg0,ppc_rarg2);			// r3 = masked EA+4
			ok&=fm_branch((void*)ReadMem32,1);
			fm_w(fm_dform(36,ppc_rrv0,ppc_contex,(u32)fofs));	// stw r3,fofs(r30)
			fm_w(fm_dform(48,rt,ppc_contex,(u32)fofs));		// lfs fD,fofs(r30)
			ok&=fm_branch(site+1,0);
		}
	}
	else if (op6==52 && ra==ppc_rarg2 && dd==0)
	{
		// --- scalar float write: stfs fS,0(r5), with the same inlined SQ
		// fast path the integer scalar write gets. sz is always 4 here, so
		// there is no sub-word swizzle and no andi mask.
		int areg=fm_find_areg(site,ra);
		int fofs=fm_fr_ofs(rt,false);
		if (areg>=0 && fofs>=0)
		{
			ok=true;
			if (dca < 0x04000000)
			{
				fm_w(0x54000000|((u32)areg<<21)|(ppc_rarg2<<16)|(6<<11)|(26<<6)|(31<<1));	// rlwinm r5,areg,6,26,31
				fm_w(0x2C000000|(ppc_rarg2<<16)|0x38);		// cmpwi r5,0x38 — SQ region?
				u32* bne=fm_p;
				fm_w(0x40820000);				// bne -> generic (patched below)
				fm_sq_addr(ppc_rarg2,(u32)areg,0);
				fm_w(fm_dform(52,rt,ppc_rarg2,0));		// stfs fS,0(r5)
				ok&=fm_branch(site+1,0);
				*bne |= ((u32)((u8*)fm_p-(u8*)bne))&0xFFFC;	// resolve bne -> generic
			}
			fm_mr(ppc_rarg0,(u32)areg);				// before r4 is clobbered below
			fm_w(fm_dform(52,rt,ppc_contex,(u32)fofs));		// stfs fS,fofs(r30)
			fm_w(fm_dform(32,ppc_rarg1,ppc_contex,(u32)fofs));	// lwz  r4,fofs(r30)
			ok&=fm_branch((void*)WriteMem32,1);
			ok&=fm_branch(site+1,0);
		}
	}
	else if (op6==52 && ra==ppc_rarg1 && dd==0)
	{
		// --- pair float write, first word: stfs fS,0(r4). Address still in
		// r3; WriteMem64(u32,u64) wants r3 + r5:r6, so only the data moves.
		// fmov.d bursts into the SQ are THE geometry path, hence the inline.
		int fofs=fm_fr_ofs(rt,true);
		if (fm_find_areg(site,ra)==ppc_rarg0 && fofs>=0)
		{
			ok=true;
			if (dca < 0x04000000)
			{
				fm_w(0x54000000|(ppc_rarg0<<21)|(ppc_rarg1<<16)|(6<<11)|(26<<6)|(31<<1));	// rlwinm r4,r3,6,26,31
				fm_w(0x2C000000|(ppc_rarg1<<16)|0x38);		// cmpwi r4,0x38
				u32* bne=fm_p;
				fm_w(0x40820000);				// bne -> generic
				fm_sq_addr(ppc_rarg1,ppc_rarg0,0);		// 8-aligned: no swizzle
				fm_w(fm_dform(52,rt,ppc_rarg1,0));		// stfs fS,  0(r4)
				fm_w(fm_dform(52,rt+1,ppc_rarg1,4));		// stfs fS+1,4(r4)
				ok&=fm_branch(site+2,0);
				*bne |= ((u32)((u8*)fm_p-(u8*)bne))&0xFFFC;
			}
			fm_w(fm_dform(52,rt,ppc_contex,(u32)fofs));		// stfs fS,  fofs(r30)
			fm_w(fm_dform(52,rt+1,ppc_contex,(u32)fofs+4));		// stfs fS+1,fofs+4(r30)
			fm_w(fm_dform(32,ppc_rarg2,ppc_contex,(u32)fofs));	// lwz  r5,fofs(r30)
			fm_w(fm_dform(32,ppc_rarg3,ppc_contex,(u32)fofs+4));	// lwz  r6,fofs+4(r30)
			ok&=fm_branch((void*)WriteMem64,1);
			ok&=fm_branch(site+2,0);				// skip the second stfs
		}
	}
	else if (op6==52 && ra==ppc_rarg1 && dd==4)
	{
		// --- pair float write, second word alone (straddling; see LIMITATION).
		int fofs=fm_fr_ofs(rt,false);
		if (fm_find_areg(site,ra)>=0 && fofs>=0)
		{
			ok=true;
			fm_ea_plus4(ppc_rarg0,ppc_rarg1);			// r3 = masked EA+4 (before r4 dies)
			fm_w(fm_dform(52,rt,ppc_contex,(u32)fofs));		// stfs fS,fofs(r30)
			fm_w(fm_dform(32,ppc_rarg1,ppc_contex,(u32)fofs));	// lwz  r4,fofs(r30)
			ok&=fm_branch((void*)WriteMem32,1);
			ok&=fm_branch(site+1,0);
		}
	}

	if (!ok)
	{
		s_fm_fail_pc=pc; s_fm_fail_insn=insn; s_fm_fail_dar=dar; s_fm_fail_reason=3;
		return 1;
	}

	const u32 bytes=(u32)((u8*)fm_p-(u8*)tramp);
	s_fm_pool_used += (bytes+31)&~31u;
	make_address_range_executable(tramp,bytes);

	// Patch the faulting access with an unconditional branch to the trampoline.
	snat disp=(u8*)tramp-(u8*)site;
	if (disp>=33554432 || disp<-33554432)
	{
		s_fm_fail_pc=pc; s_fm_fail_insn=insn; s_fm_fail_dar=dar; s_fm_fail_reason=4;
		return 1;
	}
#if defined(__WIIU__)
	wiiu_codegen_write_begin();
#endif
	*site=0x48000000|((u32)disp&0x03FFFFFC);
	make_address_range_executable(site,sizeof(u32));

	s_fm_ring[s_fm_ring_n++ & (FM_RING-1)]=pc;
	s_fm_patch_count++;
	if (op6==48 || op6==52)		// lfs / stfs — a JIT_FMOV float shape faulted
		s_fm_float_patches++;
	return 0;
}
