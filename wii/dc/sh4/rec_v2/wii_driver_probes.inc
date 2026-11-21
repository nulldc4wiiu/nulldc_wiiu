// wii_driver_probes.inc: JIT_HOTBLOCKS probe, the exp_* JIT switches, native call targets (fsqrt/fsrra).
// Part of wii_driver.cpp, which #includes it: not compiled on its own.

// ---------------------------------------------------------------------------
// JIT_HOTBLOCKS probe (wii/main.cpp preset)
//
// Counts executions per compiled block, and once a second prints the hottest
// ones with their codegen density — PPC bytes emitted per SH4 opcode — then
// dumps the SH4 source and the generated PPC of the top block so the real
// hot-path codegen can be READ instead of guessed at.
//
// Costs 4 instructions at each block entry, so it is a diagnostic: leave it off
// for timing runs. The table is allocated lazily, so nothing costs MEM1 while
// the preset is off, and it is dropped whenever the code cache is flushed —
// every recorded ppc_start points into the cache that just went away.
//
// OBSERVER EFFECT: the counters live in their OWN dense u32 array, not inside
// the descriptor struct. ChuChu runs 5-10M block entries/s, so the emitted
// load/increment/store is touched millions of times a second; with the counter
// embedded in a 24-byte struct the working set was HOT_MAX*24 = 384 KB, past
// Broadway's 256 KB L2, and a good share of those accesses went to main memory.
// Split out, the hot array is HOT_MAX*4 = 64 KB and eight counters share a
// 32-byte line, so consecutively-compiled blocks (which tend to run together)
// share lines. The descriptors stay fat but are only touched at compile time
// and once a second by the dump. This matters because SPEED% in a hotblocks-on
// log is only worth comparing if the probe's own cost is small and stable.
// ---------------------------------------------------------------------------
extern "C" int get_hotblocks_preset();

struct HotBlock
{
	u32 sh4_pc;
	u32 sh4_size;	// bytes of SH4 source the block consumed
	u32 sh4_ops;
	u8* ppc_start;
	u32 ppc_size;	// bytes of HOT PPC body (filled in at the end of ngen_Compile)
	u32 ppc_cold;	// bytes of out-of-line mem slow paths FlushCold() appended
};

// ChuChu compiles ~10k blocks between cache clears; at 2048 the first run left
// 7894 untracked, which biases the census toward whatever compiled first.
// Descriptors 16384 * 20 B = 320 KB + counters 64 KB, allocated only when the
// preset is on.
#define HOT_MAX 16384
static HotBlock* s_hb = 0;
static u32* s_hb_hits = 0;	// dense counter array, indexed in step with s_hb
static u32 s_hb_n = 0;
static u32 s_hb_overflow = 0;
// Blocks already disassembled this session, so each distinct hot shape is
// dumped exactly once no matter which scene it turns hot in. Deliberately NOT
// cleared by hotblocks_reset(): a cache clear recompiles the same pc, and
// re-dumping identical code would just bury the new shapes.
#define HOT_DUMP_MAX 64
static u32 s_hb_dumped[HOT_DUMP_MAX];
static u32 s_hb_dumped_n = 0;

// Called from recSh4_ClearCache (dc/sh4/rec_v2/driver.cpp): the compiled code
// these entries describe no longer exists.
extern "C" void hotblocks_reset()
{
	s_hb_n = 0;
	s_hb_overflow = 0;
}

extern "C" { int g_exp_jit_tlive = 1, g_exp_jit_fconst = 1; }   // jit_tlive (T in r31 until the jcond), jit_fconst (float immediates): on (DOA2 fights +0.8% on a console)
extern "C" { int g_exp_fipr_split = 1; }   // P3: fipr as two chains, pinned operands read in place; on (console r3a: DOA2 fights +2.4%)
u32 g_cnt_tlive, g_cnt_fconst, g_cnt_fipr;   // compiled sites using them (sampler counters line)
// jit_fconst: float immediates the JIT loads into pinned FRs, written once each, never stored to again; full: nullptr
static u32 jit_fconst_pool[256] __attribute__((aligned(32)));
static u32 jit_fconst_n;
static const u32 *jit_fconst(u32 bits)
{
	for (u32 i = 0; i < jit_fconst_n; i++)
		if (jit_fconst_pool[i] == bits)
			return &jit_fconst_pool[i];
	if (jit_fconst_n == 256)
		return nullptr;
	jit_fconst_pool[jit_fconst_n] = bits;
	return &jit_fconst_pool[jit_fconst_n++];
}
// Compile time: claim a slot for this block. Returns its index, or -1 if
// unavailable — the caller needs the index to reach both the descriptor and
// the counter, which live in separate arrays.
static s32 hotblocks_slot(DecodedBlock* block)
{
	if (!get_hotblocks_preset())
		return -1;

	if (!s_hb)
	{
		s_hb = (HotBlock*)malloc(sizeof(HotBlock)*HOT_MAX);
		s_hb_hits = (u32*)malloc(sizeof(u32)*HOT_MAX);
		if (!s_hb || !s_hb_hits)
		{
			free(s_hb);      s_hb = 0;
			free(s_hb_hits); s_hb_hits = 0;
			return -1;
		}
	}

	if (s_hb_n >= HOT_MAX)
	{
		s_hb_overflow++;
		return -1;
	}

	const s32 idx = (s32)s_hb_n++;
	HotBlock* h = &s_hb[idx];
	h->sh4_pc    = block->start;
	h->sh4_size  = block->sh4_code_size;
	h->sh4_ops   = block->opcodes;
	h->ppc_start = 0;
	h->ppc_size  = 0;
	h->ppc_cold  = 0;
	s_hb_hits[idx] = 0;
	return idx;
}

static void hotblocks_disasm(const HotBlock& h)
{
	printf("[HOT] --- disasm pc=%08X : %u SH4 ops / %u B  ->  %u B hot + %u B cold ---\n",
		h.sh4_pc, h.sh4_ops, h.sh4_size, h.ppc_size, h.ppc_cold);

	printf("[HOT] SH4:\n");
	u32 n = h.sh4_size/2;
	if (n > 64) n = 64;
	for (u32 i=0;i<n;i++)
	{
		u32 a = h.sh4_pc + i*2;
		u16 o = ReadMem16(a);
		printf("[HOT]   %08X: %04X  %s\n", a, o, OpDesc[o]->diss);
	}
	if (h.sh4_size/2 > n)
		printf("[HOT]   ... (%u more)\n", h.sh4_size/2 - n);

	printf("[HOT] PPC hot body (%u B):\n", h.ppc_size);
	if (h.ppc_start)
	{
		const u32* w = (const u32*)h.ppc_start;
		u32 wn = h.ppc_size/4;
		if (wn > 160) wn = 160;
		for (u32 i=0;i<wn;i+=4)
		{
			printf("[HOT]   +%04X:", i*4);
			for (u32 j=0;j<4 && (i+j)<wn;j++)
				printf(" %08X", w[i+j]);
			printf("\n");
		}
		if (h.ppc_size/4 > wn)
			printf("[HOT]   ... (%u more words)\n", h.ppc_size/4 - wn);
	}
}

// Disassemble this block unless its pc has already been dumped. Returns true
// if it actually emitted a dump.
static bool hotblocks_dump_once(const HotBlock& h)
{
	if (s_hb_dumped_n >= HOT_DUMP_MAX)
	{
		// Say so, once. A silent budget exhaustion looks identical to "the
		// selector never picked anything", which is the failure this tool has
		// already produced three times -- and the reader cannot tell the two
		// apart from the log. If this ever prints, raise HOT_DUMP_MAX.
		static bool said = false;
		if (!said)
		{
			said = true;
			printf("[HOT] dump budget exhausted (%u distinct blocks on file) --"
			       " raise HOT_DUMP_MAX to disassemble any more\n", (u32)HOT_DUMP_MAX);
		}
		return false;
	}
	for (u32 i=0;i<s_hb_dumped_n;i++)
		if (s_hb_dumped[i]==h.sh4_pc)
			return false;

	s_hb_dumped[s_hb_dumped_n++] = h.sh4_pc;
	hotblocks_disasm(h);
	return true;
}

extern "C" void hotblocks_dump(double seconds)
{
	if (!get_hotblocks_preset() || !s_hb || s_hb_n==0 || seconds<=0.0)
		return;

	// Totals first, so a block's share of all executed blocks is visible.
	double all_hits = 0.0, all_ppc = 0.0, all_ops = 0.0;
	for (u32 i=0;i<s_hb_n;i++)
	{
		all_hits += s_hb_hits[i];
		all_ppc  += (double)s_hb_hits[i] * s_hb[i].ppc_size;
		all_ops  += (double)s_hb_hits[i] * s_hb[i].sh4_ops;
	}

	if (all_hits <= 0.0)
		return;

	printf("[HOT] %.2fs  %u blocks tracked, %.0f entries/s, %.1f PPC B per SH4 op (execution-weighted)\n",
		seconds, s_hb_n, all_hits/seconds, all_ops>0.0?(all_ppc/all_ops):0.0);

	// Top 8 by hits. Repeated max-scan — 8 passes over <=HOT_MAX entries once a
	// second is free and avoids dragging a sort in.
	u32 top[8];
	u32 tn = 0;
	for (u32 a=0;a<8;a++)
	{
		u32 best = 0xFFFFFFFF;
		for (u32 i=0;i<s_hb_n;i++)
		{
			if (!s_hb_hits[i])
				continue;
			bool taken = false;
			for (u32 k=0;k<tn;k++)
				if (top[k]==i) { taken=true; break; }
			if (taken)
				continue;
			if (best==0xFFFFFFFF || s_hb_hits[i] > s_hb_hits[best])
				best = i;
		}
		if (best==0xFFFFFFFF)
			break;
		top[tn++] = best;
	}
	for (u32 a=0;a<tn;a++)
	{
		const HotBlock& h = s_hb[top[a]];
		const u32 hits = s_hb_hits[top[a]];
		// MB/s of PPC code actually executed = hits * block size. This, not
		// B/op and not the hit count, is what a block COSTS: ChuChu's
		// 8C1074EE is only #2 by hits but burns 3x the bytes of #1.
		printf("[HOT]   %u: pc=%08X %8u (%7.0f/s) %5.1f%%  sh4 %3u ops  hot %5u B  %5.1f B/op  %6.1f MB/s  cold %5u B\n",
			a+1, h.sh4_pc, hits, hits/seconds,
			hits*100.0/all_hits,
			h.sh4_ops, h.ppc_size,
			h.sh4_ops?((double)h.ppc_size/h.sh4_ops):0.0,
			(double)hits * h.ppc_size / seconds / 1e6,
			h.ppc_cold);
	}

	if (s_hb_overflow)
		printf("[HOT]   (%u blocks did not fit the %u-entry table)\n", s_hb_overflow, (u32)HOT_MAX);

	// Disassemble the costliest block not yet on file, one per second, ranked
	// by PPC BYTES EXECUTED (hits * ppc_size).
	//
	// NO GATE. Three different gates were tried here and all three locked the
	// hunt out before it reached the scene worth reading:
	//   * "#1 by hits, budget 4" -- spent during boot.
	//   * "worst B/op, must beat the record" -- B/op is a RATE, so a cold 1-op
	//     block with a fat prologue outranked a routine burning 250 MB/s.
	//   * "top by bytes, must beat the record by 25%" -- ChuChu's own light
	//     scene set the record at 212 MB/s and mania peaks at 252, which is
	//     only 19% clear, so the record was never beaten again.
	// The premise all three shared -- that the budget is precious and must be
	// spent on the right second -- was the actual mistake. The top-8 set is
	// SMALL and STABLE (the same 8 blocks in every row of a mania log, light
	// and heavy alike), so simply working through it one per second reaches
	// every block that matters within a few seconds and needs no cleverness.
	// HOT_DUMP_MAX is sized so boot and the menus cannot exhaust it first.
	u32 idx[8], left = tn;
	for (u32 i=0;i<tn;i++) idx[i] = top[i];
	while (left)
	{
		u32 best = 0; double best_bytes = -1.0;
		for (u32 i=0;i<left;i++)
		{
			const double b = (double)s_hb_hits[idx[i]] * s_hb[idx[i]].ppc_size;
			if (b > best_bytes) { best_bytes = b; best = i; }
		}
		if (hotblocks_dump_once(s_hb[idx[best]]))
			break;               // dumped one; that is this second's share
		idx[best] = idx[--left]; // already on file, try the next costliest
	}
	for (u32 i=0;i<s_hb_n;i++)
		s_hb_hits[i] = 0;

	fflush(stdout);
}

void FASTCALL do_sqw_mmu(u32 dst);
void FASTCALL do_sqw_nommu(u32 dst);
#if defined(__WIIU__)
void FASTCALL wiiu_sqw_fast(u32 dst);
// sq_inline (J1): shop_pref writes TA store-queue blocks into the frame itself, from g_ta_fast
// (wiiu/render_gx2.cpp, one cache line: next, end, sq_mask[2], skip, vfast, nvert, nstrip); the parse and the rest
// stay in C. sq_inline=2 also counts a vertex start inline when vfast >= 0
extern "C" { extern u32 g_ta_fast[8], g_sq_ram[2]; extern int g_exp_sq_inline, g_exp_sq_ram; void wiiu_ta_scan(const u32 *b);
             void FASTCALL wiiu_sqw_cold(u32 dst); }
enum { TAF_NEXT = 0, TAF_END = 4, TAF_MASK = 8, TAF_SKIP = 16, TAF_VFAST = 20, TAF_NVERT = 24, TAF_NSTRIP = 28 };
#endif

// Native call targets for fsqrt/fsrra. Broadway has no hardware fsqrt and its
// frsqrte estimate (~5-bit) is too imprecise (it distorted the BIOS swirl), so
// these route to the accurate libm path via a single f32->f32 call. Matches the
// canonical UN_OP_F(sqrtf) / UN_OP_F(1.0f/sqrtf) semantics exactly.
static f32 rec_fsqrt(f32 x)  { CCALL(CC_FSQRT); return sqrtf(x); }

// ---- JIT_FSQRT -------------------------------------------------------------
// That libm call is far more expensive than "no hardware fsqrt" suggests.
// newlib's __ieee754_sqrtf is a 25-iteration shift-and-subtract bit loop
// (li r9,25 / mtctr / bdnz) wrapped in TWO stack round-trips -- stfs f1,8(r1)
// followed immediately by lwz r10,8(r1) to get at the bit pattern, and the
// mirror image on the way out. Each of those is a load-hit-store stall on
// Broadway. Call it ~300 cycles a pop, and the [CC] census measured 167,906
// fsqrt/s in Crazy Taxi, i.e. several percent of the whole CPU.
//
// Broadway does have frsqrte (reciprocal sqrt estimate). The obvious move --
// used by an earlier attempt here -- is to emit it bare and multiply back up.
// That is wrong: the PowerPC spec only guarantees the estimate to 1/32, about
// 5 bits, and it visibly distorted the BIOS swirl. The estimate is a SEED, not
// an answer.
//
// So refine it. Newton-Raphson for y = 1/sqrt(x), written to need exactly one
// constant (0.5) rather than the usual two:
//
//     t = y*y ; e = 0.5 - (0.5*x)*t ; y = y*e + y        (quadratic)
//
// Three iterations take the spec-minimum 5 bits to ~36. Then form s = x*y and
// take one residual step -- r = x - s*s is EXACT because fnmsub is fused, so
// s = r*(0.5*y) + s lands within a double ulp:
//
//     seed   2^-5  ->  2^-9  ->  2^-18  ->  2^-36  ->  refine  ->  2^-53
//
// Rounding that to single is then provably the correctly-rounded float result:
// sqrt of a float is never exactly halfway between two floats (if it were,
// squaring would need more mantissa bits than a float has), and the closest it
// ever comes to a tie is 2^-50 -- comfortably outside the double's error.
//
// Verified rather than argued: simulating this sequence with exact fused
// semantics over 2.07 M cases -- 230 K inputs (random normals, denormals, every
// power of two, perfect squares, FLT_MIN/FLT_MAX) crossed with adversarial
// seeds at the full +-1/32 spec limit -- reproduced sqrtf bit-for-bit every
// time. The 16 ops are also MINIMAL: dropping to 2 iterations, or keeping 3 but
// skipping the residual step, both start missing (0.004% of cases) and 2
// iterations with no refinement is wrong 78% of the time.
//
// Denormal inputs need no special case: lfs widens an f32 denormal into a
// perfectly normal double, so the whole sequence runs in range.
//
// 0.5f is exact in single precision, so one lfs materialises it as a double --
// no 8-byte pool, no second load.
static const f32 s_fsqrt_half = 0.5f;
static f32 rec_fsrra(f32 x)  { CCALL(CC_FSRRA); return 1.0f / sqrtf(x); }

