// wii_driver_ops.inc: calling convention adapter, binary/unary/compare operations.
// Part of wii_driver.cpp, which #includes it: not compiled on its own.

// ==========================
// CALLING CONVENTION ADAPTER
// ==========================

struct CC_PS
{
	CanonicalParamType type;
	shil_param* par;
};
vector<CC_PS> CC_pars;
void ngen_CC_Start(shil_opcode* op) 
{ 
	CC_pars.clear();
}
void ngen_CC_Param(shil_opcode* op,shil_param* par,CanonicalParamType tp) 
{
	switch(tp)
	{
		case CPT_f32rv:
			ppc_sh_store_f32(ppc_frv0, *par);
			break;

		case CPT_u32rv:
		case CPT_u64rvL:
			ppc_sh_store(ppc_rrv0, *par);
			break;

		case CPT_u64rvH:
			ppc_sh_store(ppc_rrv1, *par);
			break;

		case CPT_u32:
		case CPT_ptr:
		case CPT_f32:
			{
				CC_PS t={tp,par};
				CC_pars.push_back(t);
			}
			break;

		default:
			die("invalid tp");
	}
}
void ngen_CC_Call(shil_opcode*op,void* function) 
{
	u32 rd_fp=ppc_farg0;
	u32 rd_gpr=ppc_rarg0;
	for (int i=CC_pars.size();i-->0;)
	{
		if (CC_pars[i].type==CPT_ptr)
		{
			ppc_sh_addr(rd_gpr,*CC_pars[i].par);
		}
		else
		{
			if (CC_pars[i].par->is_reg())
			{
				if (CC_pars[i].type==CPT_f32)
				{
					ppc_sh_load_f32(rd_fp,*CC_pars[i].par);
					rd_fp++;
				}
				else
				{
					ppc_sh_load(rd_gpr,*CC_pars[i].par);
				}
			}
			else
				ppc_li(rd_gpr,CC_pars[i].par->_imm);
		}
		rd_gpr++;
	}
	//printf("used reg r0 to r%d, %d params, calling %08X\n",rd-1,CC_pars.size(),function);
	ppc_call(function);
}

// =================
// BINARY OPERATIONS
// =================

void binop_start(shil_opcode* op)
{
	verify(!op->rs1.is_null() && !op->rs2.is_null() && !op->rd.is_null());

	verify(op->rs1.is_reg());
	//verify(!op->rs2.is_imm() || op->rs2.is_imm_s16());
	
	ppc_sh_load(ppc_rarg0,op->rs1);

	if (op->rs2.is_imm())
	{
		ppc_li(ppc_rarg1,op->rs2._imm);
	}
	else if (op->rs2.is_reg())
	{
		ppc_sh_load(ppc_rarg1,op->rs2);
	}
}

void binop_end(shil_opcode* op)
{
	ppc_sh_store(ppc_rarg0,op->rd);

	// ppc_sh_store reads rarg0, never writes it, so for an unpinned dest like
	// sr_T the value is still sitting in rarg0 for the next op to use. Gating
	// the flag HERE gates the whole optimisation: with JIT_TFWD off it never
	// gets set, and both consumers fall back to their original reload.
	compile_state.t_in_rarg0 = get_jit_tfwd_preset() &&
		op->rd.is_reg() && op->rd._reg==reg_sr_T;
}

// ---------------------------------------------------------------------------
// Shadow-register-aware operand resolution (for SINGLE-instruction ops only).
//
// With static GPR allocation, an operand that is a pinned SH4 reg already lives
// in a PPC register, so the op can read/write it in place instead of bouncing
// through rarg0/rarg1. binop3_start() resolves rs1/rs2/rd into the PPC regs the
// op should use:
//   bop_a = source reg for rs1   (pinned reg, or rarg0 holding a loaded/imm value)
//   bop_b = source reg for rs2   (pinned reg, or rarg1 holding a loaded/imm value)
//   bop_d = dest reg for rd      (pinned reg, or rarg0 scratch)
//
// SAFETY: only valid for ops emitted as a SINGLE PPC instruction (add, sub,
// and, or, xor, shl, shr, sar, mul_i32). Such an instruction reads both sources
// then writes the dest atomically, so bop_d may safely alias bop_a or bop_b.
// Multi-instruction ops (mul_u16/64, ror, shld, shad, set*, conversions) must
// NOT use this — they clobber scratch mid-sequence; they keep using
// binop_start()/binop_end() with the rarg0/rarg1 scratch window.
// ---------------------------------------------------------------------------
u32 bop_d, bop_a, bop_b;

// Resolve rs1 -> bop_a (source) and rd -> bop_d (dest). Shared by the plain
// register form and the immediate-folded form.
void bop_resolve_a_d(shil_opcode* op)
{
	verify(!op->rs1.is_null() && !op->rd.is_null());
	verify(op->rs1.is_reg());

	ppc_ireg a=GetIntReg(op->rs1._reg);
	if (a!=ppc_rinvalid)
		bop_a=a;
	else
	{
		ppc_sh_load(ppc_rarg0,op->rs1);
		bop_a=ppc_rarg0;
	}

	ppc_ireg d=GetIntReg(op->rd._reg);
	bop_d = (d!=ppc_rinvalid) ? (u32)d : (u32)ppc_rarg0;
}

// Resolve rs2 -> bop_b (source). Materialises an immediate into rarg1.
void bop_resolve_b(shil_opcode* op)
{
	if (op->rs2.is_imm())
	{
		ppc_li(ppc_rarg1,op->rs2._imm);
		bop_b=ppc_rarg1;
	}
	else
	{
		ppc_ireg b=GetIntReg(op->rs2._reg);
		if (b!=ppc_rinvalid)
			bop_b=b;
		else
		{
			ppc_sh_load(ppc_rarg1,op->rs2);
			bop_b=ppc_rarg1;
		}
	}
}

void binop3_start(shil_opcode* op)
{
	verify(!op->rs2.is_null());
	bop_resolve_a_d(op);
	bop_resolve_b(op);
}

void binop3_end(shil_opcode* op)
{
	// If rd is pinned the op already wrote it in place; only a scratch dest
	// needs storing back to the context.
	if (bop_d==ppc_rarg0 && GetIntReg(op->rd._reg)==ppc_rinvalid)
		ppc_sh_store(ppc_rarg0,op->rd);
}

void binop_start_fpu(shil_opcode* op)
{
	verify(!op->rs1.is_null() && !op->rs2.is_null() && !op->rd.is_null());

	verify(op->rs1.is_reg());
	verify(op->rs2.is_reg());
	
	ppc_sh_load_f32(ppc_farg0,op->rs1);
	ppc_sh_load_f32(ppc_farg1,op->rs2);
}

void binop_end_fpu(shil_opcode* op)
{
	ppc_sh_store_f32(ppc_farg0,op->rd);
}

// =================
// UNARY OPERATIONS
// =================

// Unary integer: loads rs1 -> rarg0, operation writes rarg0, stores rarg0 -> rd
void unop_start(shil_opcode* op)
{
	verify(!op->rs1.is_null() && !op->rd.is_null());
	verify(op->rd.is_reg());

	if (op->rs1.is_imm())
		ppc_li(ppc_rarg0,op->rs1._imm);
	else
	{
		verify(op->rs1.is_reg());
		ppc_sh_load(ppc_rarg0,op->rs1);
	}
}

void unop_end(shil_opcode* op)
{
	ppc_sh_store(ppc_rarg0,op->rd);
}

// Shadow-register-aware unary resolution (SINGLE-instruction ops only:
// neg, not, ext_s8, ext_s16). Same safety rule as binop3_*: the op must be one
// PPC instruction so bop_d may alias bop_a. Sets bop_a (source) / bop_d (dest).
void unop3_start(shil_opcode* op)
{
	verify(!op->rs1.is_null() && !op->rd.is_null());
	verify(op->rd.is_reg());

	if (op->rs1.is_imm())
	{
		ppc_li(ppc_rarg0,op->rs1._imm);
		bop_a=ppc_rarg0;
	}
	else
	{
		verify(op->rs1.is_reg());
		ppc_ireg a=GetIntReg(op->rs1._reg);
		if (a!=ppc_rinvalid)
			bop_a=a;
		else
		{
			ppc_sh_load(ppc_rarg0,op->rs1);
			bop_a=ppc_rarg0;
		}
	}

	ppc_ireg d=GetIntReg(op->rd._reg);
	bop_d = (d!=ppc_rinvalid) ? (u32)d : (u32)ppc_rarg0;
}

void unop3_end(shil_opcode* op)
{
	if (bop_d==ppc_rarg0 && GetIntReg(op->rd._reg)==ppc_rinvalid)
		ppc_sh_store(ppc_rarg0,op->rd);
}

// Unary fpu: loads rs1 -> farg0, operation writes farg0, stores farg0 -> rd
void unop_start_fpu(shil_opcode* op)
{
	verify(!op->rs1.is_null() && !op->rd.is_null());
	verify(op->rs1.is_reg());
	verify(op->rd.is_reg());

	ppc_sh_load_f32(ppc_farg0,op->rs1);
}

void unop_end_fpu(shil_opcode* op)
{
	ppc_sh_store_f32(ppc_farg0,op->rd);
}

// ---------------------------------------------------------------------------
// FPU_PIN: shadow-register-aware FLOAT operand resolution, mirroring the
// integer binop3_*/unop3_* scheme. Only for ops emitted as a SINGLE PPC FP
// instruction (fadds/fsubs/fmuls/fdivs/fabs/fneg/fmadds): such an instruction
// reads all sources then writes the dest atomically, so fop_d may safely
// alias a source. All of fr[0..15] is pinned when the preset is on, so the
// scalar arithmetic ops (operands always fr) collapse to ONE instruction;
// fpul/xf operands resolve to a scratch load exactly as before. With the
// preset off this degenerates to the legacy lfs/lfs/op/stfs sequence.
// ---------------------------------------------------------------------------
u32 fop_d, fop_a, fop_b;

// Resolve a float source: its pinned FPR (read in place, no copy), or load
// into scratch D. Never emits the fmr that ppc_sh_load_f32 would.
static u32 fsrc_or_load_i(u32 sh4_reg,u32 D)
{
	if (get_fpu_pin_preset())
	{
		ppc_freg rf=GetFloatReg(sh4_reg);
		if (rf!=ppc_finvalid)
			return (u32)rf;
	}
	ppc_lfs(D,ppc_contex,Sh4cntx.offset(sh4_reg));
	return D;
}
static u32 fsrc_or_load(shil_param prm,u32 D)
{
	verify(prm.is_reg());
	return fsrc_or_load_i(prm._reg,D);
}

static void fbop_resolve(shil_opcode* op)
{
	verify(!op->rs1.is_null() && !op->rs2.is_null() && !op->rd.is_null());
	fop_a=fsrc_or_load(op->rs1,ppc_farg0);
	fop_b=fsrc_or_load(op->rs2,ppc_farg1);
	ppc_freg d=get_fpu_pin_preset()?GetFloatReg(op->rd._reg):ppc_finvalid;
	fop_d=(d!=ppc_finvalid)?(u32)d:(u32)ppc_farg0;
}
static void fuop_resolve(shil_opcode* op)
{
	verify(!op->rs1.is_null() && !op->rd.is_null());
	fop_a=fsrc_or_load(op->rs1,ppc_farg0);
	ppc_freg d=get_fpu_pin_preset()?GetFloatReg(op->rd._reg):ppc_finvalid;
	fop_d=(d!=ppc_finvalid)?(u32)d:(u32)ppc_farg0;
}
static void fbop_end(shil_opcode* op)
{
	// A pinned rd was written in place; only the scratch dest needs a store.
	// (Pinned regs are f14+, so fop_d==farg0 <=> rd is not pinned.)
	if (fop_d==ppc_farg0)
		ppc_sh_store_f32(ppc_farg0,op->rd);
}

// Resolve a float DEST to the FPR an op should write: the pinned FPR if rd is
// pinned (write in place, no trailing store), else `scratch`. Pair with
// fdst_store() which stores only when scratch was used.
static u32 fdst_reg(shil_param rd,u32 scratch)
{
	if (get_fpu_pin_preset())
	{
		ppc_freg rf=GetFloatReg(rd._reg);
		if (rf!=ppc_finvalid)
			return (u32)rf;
	}
	return scratch;
}
static void fdst_store(shil_param rd,u32 reg,u32 scratch)
{
	if (reg==scratch)
		ppc_sh_store_f32(scratch,rd);
}

// Minimal float register->register move between two SH4 float regs (fmov
// FRm,FRn and each word of fmov.d / DR<->XD). Emits the fewest instructions
// for however the two ends are allocated:
//   both pinned  -> fmr        (or nothing when identical)
//   src pinned   -> stfs       (dst is xf/memory)
//   dst pinned   -> lfs        (src is xf/memory)
//   both memory  -> lfs f0 + stfs f0   (== legacy path; also the preset-off case)
static void fmove_reg(u32 dst_reg,u32 src_reg)
{
	ppc_freg sd=get_fpu_pin_preset()?GetFloatReg(src_reg):ppc_finvalid;
	ppc_freg dd=get_fpu_pin_preset()?GetFloatReg(dst_reg):ppc_finvalid;

	if (sd!=ppc_finvalid && dd!=ppc_finvalid)
	{
		if (sd!=dd)
			ppc_fmrx(dd,sd,0);
	}
	else if (sd!=ppc_finvalid)
		ppc_stfs(sd,ppc_contex,Sh4cntx.offset(dst_reg));
	else if (dd!=ppc_finvalid)
		ppc_lfs(dd,ppc_contex,Sh4cntx.offset(src_reg));
	else
	{
		ppc_lfs(ppc_f0,ppc_contex,Sh4cntx.offset(src_reg));
		ppc_stfs(ppc_f0,ppc_contex,Sh4cntx.offset(dst_reg));
	}
}

// ===================================================
// COMPARE -> BOOLEAN (branchless CR0 bit extraction)
// ===================================================
//
// SH4 set* ops produce a full 0/1 u32 in rd. We compare rarg0,rarg1 into CR0
// (signed cmp or unsigned cmpl) then extract the desired CR0 bit into rarg0:
//
//   mfcr  rarg0          ; rarg0[CR0 field] = LT GT EQ SO at bits 0..3
//   rlwinm rarg0,rarg0,(bit+1),31,31  ; rotate wanted bit to position 31, mask to 1
//
// CR0 occupies the top 4 bits of CR (bit 0=LT,1=GT,2=EQ,3=SO). After mfcr the
// LT bit is in word-bit 0, so the bit for BI_CR0_xx index 'b' sits at word-bit
// 'b'. A left-rotate of (b+1) brings it to bit 31; mask MB=ME=31 keeps just it.
static void emit_cr0_bit_to_rarg0(u32 cr0_bit_index)
{
	ppc_mfcr(ppc_rarg0);
	ppc_rlwinmx(ppc_rarg0,ppc_rarg0,cr0_bit_index+1,31,31,0);
}

// JIT_CR0: hand a block-ending compare's condition straight to ngen_End.
//
// The SH4's commonest shape is a flag producer immediately followed by a
// conditional branch on it. The compare has already set CR0 correctly, and the
// legacy path then spends mfcr + rlwinm (+ xori) + stw + cmpi rebuilding that
// same bit so ngen_End can compare it against 0/1 — five or six instructions,
// one of them the CR-reading mfcr, to recover something the CPU already knew.
//
// When this returns true the caller has emitted NOTHING beyond its compare and
// must NOT call binop_end(): ngen_End does both halves of the job instead. It
// branches on the CR0 bit directly, and writes sr_T as a per-path CONSTANT —
// which is exact, not an assumption: on the path that takes BranchBlock T is
// (BlockType&1) by definition, and on the other path it is the complement. So
// the architectural T a later block might read is still written, and no
// cross-block liveness analysis is needed to justify dropping the mfcr.
static bool emit_t_to_cr0(u32 cr0_bit_index,bool invert)
{
	if (!compile_state.t_exit_cr0)
		return false;

	compile_state.cr0_live   = true;
	compile_state.cr0_bit    = cr0_bit_index;
	compile_state.cr0_invert = invert;
	return true;
}

// ---------------------------------------------------------------------------
// JIT_CARRY: SH4 T bit <-> PPC XER.
//
// TWO BIT NUMBERINGS ARE IN PLAY HERE, and mixing them is the easy mistake.
// PowerPC documents SPR and CR fields in IBM numbering (bit 0 = MSB), so
// XER[SO]=0, XER[OV]=1, XER[CA]=2, CR0[LT,GT,EQ,SO]=0..3. rlwinm's SH/MB/ME
// are IBM-numbered too. The SH4 flags we are moving them to/from are described
// in ordinary value numbering: sr_T is value bit 0, SR.Q is value bit 8, SR.M
// is value bit 9.
//
// To land value bit `b` in value bit 0:      rlwinm rD,rS,32-b,31,31
// To take value bit 0 back up to value bit b: rlwimi rD,rS,b,31-b,31-b
//
// So XER[CA] (IBM 2 = value 29) extracts with SH=3 and is written with SH=29,
// which is also what emit_cr0_bit_to_rarg0 does for CR0 (index k is IBM, so
// value bit 31-k, so SH = k+1). SR.Q (value 8) needs SH=24 / SH=8 + MB=ME=23 —
// NOT 9 and 8. Getting that pair wrong is silent: the value comes out right
// and only Q/T are corrupted.
//
// These are the sequences Evoca uses in seta-gx's SH2 recompiler, retargeted:
// there SR (T included) is pinned in a GPR so the T traffic is register-only,
// here sr_T is always memory-resident, which costs the lwz/stw pair below.
// ---------------------------------------------------------------------------

// XER[CA] = sr_T (optionally inverted, for the borrow-polarity ops). `t` is a
// scratch GPR that must not alias either operand of the following adde/subfe.
static void emit_T_to_ca(u32 t,bool invert)
{
	ppc_sh_load(t,reg_sr_T);
	if (invert)
		ppc_xori(t,t,1);
	ppc_rlwinmx(t,t,29,2,2,0);	// value bit 0 -> XER[CA]
	ppc_mtxer(t);
}

// sr_T = XER[CA] (optionally inverted). SH4 subc/negc define T as a BORROW,
// PPC defines CA as "no borrow", hence the inversion on those.
static void emit_ca_to_T(u32 t,bool invert)
{
	ppc_mfxer(t);
	ppc_rlwinmx(t,t,3,31,31,0);	// XER[CA] -> value bit 0
	if (invert)
		ppc_xori(t,t,1);
	ppc_sh_store(t,reg_sr_T);
}

// sr_T = XER[OV], set by the OE=1 form of add/subf (addv/subv).
static void emit_ov_to_T(u32 t)
{
	ppc_mfxer(t);
	ppc_rlwinmx(t,t,2,31,31,0);	// XER[OV] -> value bit 0
	ppc_sh_store(t,reg_sr_T);
}

// Resolve the destination of a carry op. rd is always an SH4 GPR here, so it
// is pinned unless it is r11. Safe for the single-instruction shapes below
// (adde/subfe/subfze/addo/subfo read both sources then write the dest), which
// is why the dest may alias a source.
static u32 carry_dest(shil_opcode* op)
{
	ppc_ireg d=GetIntReg(op->rd._reg);
	return (d!=ppc_rinvalid) ? (u32)d : (u32)ppc_rarg3;
}

static void carry_dest_store(shil_opcode* op,u32 d)
{
	if (d==(u32)ppc_rarg3)
		ppc_sh_store(ppc_rarg3,op->rd);
}

// Compare rs1 against rs2 into CR0. cmp/cmpl/cmpi/cmpli can read ANY register,
// so when an operand already lives in a pinned PPC reg we compare it in place
// and skip the move into rarg0/rarg1 — only spilled operands (or immediates
// that don't fit the cmp-immediate forms) need a scratch load. Folds a small
// immediate rs2 into cmpi/cmpli (signed uses cmpi+s16, unsigned cmpli+u16).
//
// The follow-on emit_cr0_bit_to_rarg0 does mfcr rarg0, which clobbers ONLY
// rarg0 (scratch) and never the pinned source regs, so comparing in place is
// safe regardless of which operands were elided.
static void emit_cmp_into_cr0(shil_opcode* op,bool is_signed)
{
	// rs1 in a pinned reg -> use it directly; else load into rarg0 scratch.
	u32 a=src_or_load(op->rs1,ppc_rarg0);

	if (op->rs2.is_imm() && (is_signed ? op->rs2.is_imm_s16() : op->rs2.is_imm_u16()))
	{
		if (is_signed)
			ppc_cmpi(ppc_cr0,a,op->rs2._imm,0);
		else
			ppc_cmpli(ppc_cr0,a,op->rs2._imm,0);
		return;
	}

	// rs2 in a pinned reg -> use directly; immediate -> li into rarg1; else load.
	u32 b;
	if (op->rs2.is_imm())
		{ ppc_li(ppc_rarg1,op->rs2._imm); b=ppc_rarg1; }
	else
		b=src_or_load(op->rs2,ppc_rarg1);

	if (is_signed)
		ppc_cmp(ppc_cr0,a,b,0);
	else
		ppc_cmpl(ppc_cr0,a,b,0);
}

// rd = (signed) rs1 <cond> rs2  -> 0/1
static void emit_setcc_signed(shil_opcode* op,u32 cr0_bit_index,bool invert)
{
	emit_cmp_into_cr0(op,true);
	if (emit_t_to_cr0(cr0_bit_index,invert))
		return;		// JIT_CR0: ngen_End branches on CR0 and stores T itself
	emit_cr0_bit_to_rarg0(cr0_bit_index);
	if (invert)
		ppc_xori(ppc_rarg0,ppc_rarg0,1);
	binop_end(op);
}

// rd = (unsigned) rs1 <cond> rs2 -> 0/1
static void emit_setcc_unsigned(shil_opcode* op,u32 cr0_bit_index,bool invert)
{
	emit_cmp_into_cr0(op,false);
	if (emit_t_to_cr0(cr0_bit_index,invert))
		return;		// JIT_CR0: ngen_End branches on CR0 and stores T itself
	emit_cr0_bit_to_rarg0(cr0_bit_index);
	if (invert)
		ppc_xori(ppc_rarg0,ppc_rarg0,1);
	binop_end(op);
}


void ngen_CC_Finish(shil_opcode* op) 
{ 
	CC_pars.clear(); 
}
void DoStatic(u32 pc)
{
	// MUST be exactly 3 instructions (12 bytes): ngen_LinkBlock_Static_stub
	// computes its patch site as LR-12, i.e. the START of this sequence, and
	// overwrites it with a direct `b compiled_target`. ppc_li is variable
	// length (1 insn when the value fits s16 OR its low half is 0 — e.g. a
	// block at 0x8C010000), which would make the stub patch the instruction
	// BEFORE the sequence — in a BET_Cond end, that is the other exit path's
	// branch. So emit the fixed addis+ori pair unconditionally.
	ppc_addis(ppc_rarg0,0,pc>>16);
	ppc_ori(ppc_rarg0,ppc_rarg0,(u16)pc);
	ppc_call(ngen_LinkBlock_Static_stub);
}

