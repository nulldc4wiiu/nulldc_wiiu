/*
    SH4 Intermediate Language (SHIL) — analysis helpers and canonical
    opcode instantiation.

    Changes vs original:
    - Fixed dead-write detection logic (was comparing write-ord >= read-ord
      which is backwards; a dead write happens when the *next* write comes
      before any read of the previously written register).
    - Fixed the liveness sentinel: the "never read" marker was 0xFFFFFFFF
      compared as u32, which reads as the *newest* possible read and hid the
      most common dead write of all (written twice, never read). Arrays are
      now s32 with a -1 sentinel.
    - Dead writes are only removed for ops on a purity whitelist
      (shil_op_is_pure). readm can hit MMIO where the read itself has*/
      //       hardware side effects; pref/writem/sync_*/ftrv/fsca all do work the
      /*
      register liveness cannot see.
    - AnalyseBlock is now ACTIVE by default behind the SHIL_DCE switch; set
      SHIL_DCE to 0 to restore the original disabled behaviour for A/B.
    - Per-block printf removed (it flooded the console and skewed the very
      speed comparison it was meant to support); optional periodic summary
      behind SHIL_DCE_STATS, off by default.
*/

#include "types.h"
#include "shil.h"
#include "decoder.h"
#include "ccall_census.h"
#include <stdio.h>
#include <string.h>
#include <vector>

// ---------------------------------------------------------------------------
// Dead-code elimination master switch (A/B kill switch).
//   1 = pass active
//   0 = pass compiled out, identical to the original shipping behaviour
// Bisect a regression by setting this back to 0 and rebuilding.
// ---------------------------------------------------------------------------
#define SHIL_DCE 0

// Set to 1 to print a periodic "blocks / ops removed" summary. Off by default
// so a speed A/B runs with zero console output (printf on the compile path
// would itself skew the comparison).
#define SHIL_DCE_STATS 0

// ---------------------------------------------------------------------------
// Per-register liveness tracking — only compiled when DCE is active.
// ---------------------------------------------------------------------------
#if SHIL_DCE

// SIGNED, with -1 meaning "never read"/"never written". The original used u32
// with a 0xFFFFFFFF sentinel, which made "never read" compare as the *newest*
// possible read (0xFFFFFFFF < prevW is always false) — so a register that was
// written twice and never read, the single most common dead write, was never
// detected. That sentinel bug is why the pass looked useless enough to leave
// switched off.
static s32 RegisterWrite[sh4_reg_count];
static s32 RegisterRead [sh4_reg_count];

static u32 fallback_blocks = 0;
static u32 total_blocks    = 0;
static u32 REMOVED_OPS     = 0;

// Only ops that are pure register->register computation may be deleted.
// Everything else can carry effects this register-liveness analysis cannot
// see, and deleting one would be silent corruption rather than a speedup:
//   readm              - may target MMIO, where the *read* has hardware effects
//   writem / pref      - store to guest memory / trigger a store-queue burst
//   sync_sr/sync_fpscr - call UpdateSR/UpdateFPSCR, can switch register banks
//   ifb                - interpreter fallback, arbitrary effects
//   jdyn / jcond       - control flow
//   ftrv / fsca        - write their destination through a pointer, so the
//                        rd liveness above does not describe what they touch
//   div32u/s/p2        - multi-register division helpers
//   adc/sbc/negc/      - two-output ops: RegWriteInfo() decides deadness one
//   addv/subv            register at a time, so a dead T with a LIVE value
//                        would still mark the whole op dead. Kept off the
//                        whitelist until that is fixed (the same latent
//                        hazard applies to div32u/s and mul_u64/s64, which
//                        predate this).
//   div1               - also read-modify-writes SR.Q, which no shil param
//                        describes
// Whitelist rather than blacklist on purpose: a future shil op defaults to
// "not deletable" instead of silently becoming a correctness bug.
static bool shil_op_is_pure(shilop op)
{
    switch (op)
    {
    case shop_mov32:     case shop_mov64:
    case shop_and:       case shop_or:        case shop_xor:
    case shop_not:       case shop_add:       case shop_sub:
    case shop_neg:       case shop_shl:       case shop_shr:
    case shop_sar:       case shop_ror:       case shop_shld:
    case shop_shad:      case shop_ext_s8:    case shop_ext_s16:
    case shop_mul_u16:   case shop_mul_s16:   case shop_mul_i32:
    case shop_mul_u64:   case shop_mul_s64:
    case shop_cvt_f2i_t: case shop_cvt_i2f_n: case shop_cvt_i2f_z:
    case shop_test:      case shop_seteq:     case shop_setge:
    case shop_setgt:     case shop_setae:     case shop_setab:
    case shop_cmpstr:    case shop_swaplb:
    case shop_fadd:      case shop_fsub:      case shop_fmul:
    case shop_fdiv:      case shop_fabs:      case shop_fneg:
    case shop_fsqrt:     case shop_fipr:      case shop_fmac:
    case shop_fsrra:     case shop_fseteq:    case shop_fsetgt:
        return true;
    default:
        return false;
    }
}

// Record the ordinal of the instruction that *reads* each sub-register of p.
static void RegReadInfo(const shil_param& p, s32 ord)
{
    if (!p.is_reg())
        return;
    const int n = p.count();
    for (int i = 0; i < n; ++i)
        RegisterRead[p._reg + i] = ord;
}

// Record the ordinal of the instruction that *writes* each sub-register of p.
// BUG FIX vs original: condition was inverted.
// A write is dead when no read occurred between the previous write and this
// new write, i.e. RegisterRead[r] < RegisterWrite[r].
static void RegWriteInfo(shil_opcode* ops, const shil_param& p, s32 ord)
{
    if (!p.is_reg())
        return;
    const int n = p.count();
    for (int i = 0; i < n; ++i)
    {
        const u32 r     = p._reg + i;
        const s32 prevW = RegisterWrite[r];
        const s32 prevR = RegisterRead[r];

        // prevW < 0        -> no earlier write, nothing can be dead
        // prevR < prevW    -> no read since that write (prevR == -1, i.e.
        //                     never read at all, is correctly covered here)
        if (prevW >= 0 && prevR < prevW && shil_op_is_pure(ops[prevW].op))
        {
            // Per-opcode logging removed: fires on every dead write found,
            // which floods console output across thousands of blocks and
            // would itself skew any speed comparison.
            ops[prevW].Flow = 1;
        }
        RegisterWrite[r] = ord;
    }
}

#endif // SHIL_DCE

// ---------------------------------------------------------------------------
// Dead-code elimination pass (Write-after-Write without intervening Read).
//
// Scope and assumptions (both inherited from the original design):
//   * Block-local only. The last write to a register within a block is never
//     removed, so state handed to the next block stays correct.
//   * Assumes no precise mid-block exceptions — if an op faults partway
//     through a block, a removed earlier write would leave stale state
//     visible to the handler. Consistent with how the rest of rec_v2 works.
// Controlled by SHIL_DCE above.
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// exp_ssa: block optimiser after Flycast's SSAOptimizer (core/hw/sh4/dyna/ssa.cpp), cut down to what this PPC
// backend accepts. Every register write gets a version; constants are propagated only into the operand slots
// the backend takes immediates in (the second operand of integer binops, the only operand of mov32 and integer
// unops) and whole ops with known inputs become mov32 rd,imm; then dead pure ops are removed, simple identities
// rewritten (x+0, x&0, x^x...), constant shifts of one register combined and mov rX,rX dropped. Ops with
// register effects their operands don't show (barriers below) end constant knowledge and keep every earlier
// write live. Not ported: Flycast's alias pass (tied to its register allocator), its branch-target pass (needs
// read-only code pages) and memory address folding.
// ---------------------------------------------------------------------------
int g_exp_ssa;
static u32 ssa_blocks, ssa_ops_in, ssa_ops_out, ssa_consts, ssa_folded, ssa_dead, ssa_simple, ssa_shifts, ssa_idmov;
static u16 ssa_ver[sh4_reg_count];

static bool ssa_barrier(shilop op)
{
    switch (op)
    {
    case shop_ifb:      case shop_sync_sr:  case shop_sync_fpscr: case shop_div1:
    case shop_div32u:   case shop_div32s:   case shop_div32p2:    case shop_mac_l:
    case shop_mac_w:    case shop_ftrv:     case shop_fsca:
        return true;
    default:
        return false;
    }
}

static void ssa_use(shil_param& p)
{
    if (p.is_reg())
        for (int i = 0; i < p.count(); i++)
            p.version[i] = ssa_ver[p._reg + i];
}
static void ssa_def(shil_param& p)
{
    if (p.is_reg())
        for (int i = 0; i < p.count(); i++)
            p.version[i] = ++ssa_ver[p._reg + i];
}
static void ssa_version_pass(DecodedBlock* blk)
{
    memset(ssa_ver, 0, sizeof(ssa_ver));
    for (shil_opcode& op : blk->oplist)
    {
        ssa_use(op.rs1);
        ssa_use(op.rs2);
        ssa_use(op.rs3);
        if (ssa_barrier(op.op))
            for (u32 r = 0; r < sh4_reg_count; r++)
                ssa_ver[r]++;   // hidden writes: nothing read later is the value read before
        ssa_def(op.rd);
        ssa_def(op.rd2);
    }
}

static void ssa_mov_imm(shil_opcode& op, u32 v)
{
    op.op = shop_mov32;
    op.rs1 = shil_param(FMT_IMM, v);
    op.rs2 = shil_param();
    op.rs3 = shil_param();
}
static void ssa_mov_rs1(shil_opcode& op)
{
    op.op = shop_mov32;
    op.rs2 = shil_param();
    op.rs3 = shil_param();
}

// integer ops this pass may evaluate at compile time: one int result, no second result, no third source
static bool ssa_eval(shilop op, u32 a, u32 b, bool has_b, u32& v)
{
    switch (op)
    {
    case shop_add:     v = a + b; return has_b;
    case shop_sub:     v = a - b; return has_b;
    case shop_and:     v = a & b; return has_b;
    case shop_or:      v = a | b; return has_b;
    case shop_xor:     v = a ^ b; return has_b;
    case shop_shl:     v = a << b; return has_b && b < 32;
    case shop_shr:     v = a >> b; return has_b && b < 32;
    case shop_sar:     v = (u32)((s32)a >> b); return has_b && b < 32;
    case shop_mul_i32: v = a * b; return has_b;
    case shop_mul_u16: v = (u32)(u16)a * (u32)(u16)b; return has_b;
    case shop_mul_s16: v = (u32)((s32)(s16)a * (s32)(s16)b); return has_b;
    case shop_test:    v = (a & b) == 0; return has_b;
    case shop_seteq:   v = a == b; return has_b;
    case shop_setge:   v = (s32)a >= (s32)b; return has_b;
    case shop_setgt:   v = (s32)a > (s32)b; return has_b;
    case shop_setae:   v = a >= b; return has_b;
    case shop_setab:   v = a > b; return has_b;
    case shop_not:     v = ~a; return !has_b;
    case shop_neg:     v = 0u - a; return !has_b;
    case shop_ext_s8:  v = (u32)(s32)(s8)a; return !has_b;
    case shop_ext_s16: v = (u32)(s32)(s16)a; return !has_b;
    case shop_swaplb:  v = (a & 0xFFFF0000u) | ((a & 0xFFu) << 8) | ((a >> 8) & 0xFFu); return !has_b;
    default: return false;
    }
}
// second operand may be an immediate (bop_resolve_b, binop_start, emit_cmp_into_cr0)
static bool ssa_imm_rs2(shilop op, u32 v)
{
    switch (op)
    {
    case shop_add: case shop_sub: case shop_and: case shop_or: case shop_xor: case shop_mul_i32:
    case shop_mul_u16: case shop_mul_s16: case shop_test: case shop_seteq: case shop_setge:
    case shop_setgt: case shop_setae: case shop_setab:
        return true;
    case shop_shl: case shop_shr: case shop_sar:
        return v < 32;
    default:
        return false;
    }
}
static bool ssa_commutes(shilop op)
{
    return op == shop_add || op == shop_and || op == shop_or || op == shop_xor || op == shop_mul_i32 ||
           op == shop_mul_u16 || op == shop_mul_s16 || op == shop_test || op == shop_seteq;
}

static struct { u16 ver; bool valid; u32 val; } ssa_const[sh4_reg_count];
static bool ssa_known(const shil_param& p, u32& v)
{
    if (p.is_imm())
    {
        v = p._imm;
        return true;
    }
    if (p.is_r32i() && ssa_const[p._reg].valid && ssa_const[p._reg].ver == p.version[0])
    {
        v = ssa_const[p._reg].val;
        return true;
    }
    return false;
}
static void ssa_constprop_pass(DecodedBlock* blk)
{
    memset(ssa_const, 0, sizeof(ssa_const));
    for (shil_opcode& op : blk->oplist)
    {
        if (ssa_barrier(op.op))
        {
            memset(ssa_const, 0, sizeof(ssa_const));
            continue;
        }
        if (!op.rd.is_r32i() || !op.rd2.is_null() || !op.rs3.is_null())
            continue;
        u32 a = 0, b = 0, v;
        const bool ka = ssa_known(op.rs1, a), kb = !op.rs2.is_null() && ssa_known(op.rs2, b);
        if (op.op == shop_mov32)
        {
            if (ka)
            {
                if (!op.rs1.is_imm())
                    ssa_consts++;
                op.rs1 = shil_param(FMT_IMM, a);
                ssa_const[op.rd._reg] = { op.rd.version[0], true, a };
            }
            continue;
        }
        if (ka && (op.rs2.is_null() || kb) && ssa_eval(op.op, a, b, !op.rs2.is_null(), v))
        {
            ssa_mov_imm(op, v);
            ssa_const[op.rd._reg] = { op.rd.version[0], true, v };
            ssa_folded++;
            continue;
        }
        if ((op.op == shop_shld || op.op == shop_shad) && kb && op.rs1.is_r32i())
        {
            // as Flycast: a known shift amount makes a plain shift (amount 0 stays for the backend's own path)
            if (b != 0)
            {
                if ((b & 0x1F) == 0)
                {
                    if (op.op == shop_shld)
                        ssa_mov_imm(op, 0);
                    else
                    {
                        op.op = shop_sar;
                        op.rs2 = shil_param(FMT_IMM, 31);
                    }
                }
                else if (!(b & 0x80000000))
                {
                    op.op = shop_shl;
                    op.rs2 = shil_param(FMT_IMM, b & 0x1F);
                }
                else
                {
                    op.op = op.op == shop_shad ? shop_sar : shop_shr;
                    op.rs2 = shil_param(FMT_IMM, (~b & 0x1F) + 1);
                }
                ssa_folded++;
            }
            continue;
        }
        if (op.rs1.is_reg() && kb && !op.rs2.is_imm() && ssa_imm_rs2(op.op, b))
        {
            op.rs2 = shil_param(FMT_IMM, b);
            ssa_consts++;
        }
        else if (ka && op.rs1.is_reg() && op.rs2.is_r32i() && ssa_commutes(op.op))
        {
            shil_param t = op.rs2;
            op.rs2 = shil_param(FMT_IMM, a);
            op.rs1 = t;
            ssa_consts++;
        }
    }
}

// pure integer/float ops whose only effect is their rd (and rd2); everything else stays
static bool ssa_removable(shilop op)
{
    switch (op)
    {
    case shop_mov32:     case shop_mov64:     case shop_and:       case shop_or:        case shop_xor:
    case shop_not:       case shop_add:       case shop_sub:       case shop_neg:       case shop_shl:
    case shop_shr:       case shop_sar:       case shop_ror:       case shop_shld:      case shop_shad:
    case shop_ext_s8:    case shop_ext_s16:   case shop_mul_u16:   case shop_mul_s16:   case shop_mul_i32:
    case shop_mul_u64:   case shop_mul_s64:   case shop_test:      case shop_seteq:     case shop_setge:
    case shop_setgt:     case shop_setae:     case shop_setab:     case shop_cmpstr:    case shop_swaplb:
    case shop_fadd:      case shop_fsub:      case shop_fmul:      case shop_fdiv:      case shop_fabs:
    case shop_fneg:      case shop_fsqrt:     case shop_fipr:      case shop_fmac:      case shop_fsrra:
    case shop_fseteq:    case shop_fsetgt:    case shop_cvt_f2i_t: case shop_cvt_i2f_n: case shop_cvt_i2f_z:
        return true;
    default:
        return false;
    }
}
static void ssa_dead_pass(DecodedBlock* blk)
{
    // backwards: a write is dead when a later write of the same register exists and nothing read this version
    static u32 last[sh4_reg_count];
    static std::vector<u32> uses;   // (reg << 16 | version) read by a later live op
    memset(last, 0xFF, sizeof(last));
    uses.clear();
    const auto used = [](u32 reg, u32 ver) {
        const u32 key = reg << 16 | ver;
        for (u32 k : uses)
            if (k == key)
                return true;
        return false;
    };
    const auto use = [](const shil_param& p) {
        if (p.is_reg())
            for (int i = 0; i < p.count(); i++)
                uses.push_back((u32)(p._reg + i) << 16 | p.version[i]);
    };
    for (int n = (int)blk->oplist.size() - 1; n >= 0; n--)
    {
        shil_opcode& op = blk->oplist[n];
        if (ssa_barrier(op.op))
        {
            memset(last, 0xFF, sizeof(last));   // everything before may be read by it
            use(op.rs1); use(op.rs2); use(op.rs3);
            continue;
        }
        bool dead = ssa_removable(op.op) && (op.rd.is_reg() || op.rd2.is_reg());
        const shil_param *defs[2] = { &op.rd, &op.rd2 };
        for (const shil_param *d : defs)
        {
            if (!d->is_reg())
                continue;
            for (int i = 0; i < d->count(); i++)
            {
                const u32 r = d->_reg + i;
                if (last[r] == 0xFFFFFFFF)
                {
                    last[r] = d->version[i];   // the block's final value of r: always kept
                    dead = false;
                }
                else if (used(r, d->version[i]))
                    dead = false;
            }
        }
        if (dead)
        {
            blk->oplist.erase(blk->oplist.begin() + n);
            ssa_dead++;
        }
        else
        {
            use(op.rs1);
            use(op.rs2);
            use(op.rs3);
        }
    }
}

static void ssa_simplify_pass(DecodedBlock* blk)
{
    for (shil_opcode& op : blk->oplist)
    {
        if (!op.rd.is_r32i() || !op.rd2.is_null() || !op.rs3.is_null() || !op.rs1.is_r32i())
            continue;
        if (op.rs2.is_imm())
        {
            const u32 b = op.rs2._imm;
            if (b == 0 && (op.op == shop_and || op.op == shop_mul_i32 || op.op == shop_mul_s16 || op.op == shop_mul_u16))
                ssa_mov_imm(op, 0), ssa_simple++;
            else if (b == 0 && (op.op == shop_add || op.op == shop_sub || op.op == shop_or || op.op == shop_xor ||
                                op.op == shop_shl || op.op == shop_shr || op.op == shop_sar))
                ssa_mov_rs1(op), ssa_simple++;
            else if (b == 1 && op.op == shop_mul_i32)
                ssa_mov_rs1(op), ssa_simple++;
        }
        else if (op.rs2.is_r32i() && op.rs1._reg == op.rs2._reg && op.rs1.version[0] == op.rs2.version[0])
        {
            if (op.op == shop_add)
                op.op = shop_shl, op.rs2 = shil_param(FMT_IMM, 1), ssa_simple++;
            else if (op.op == shop_xor || op.op == shop_sub)
                ssa_mov_imm(op, 0), ssa_simple++;
            else if (op.op == shop_and || op.op == shop_or)
                ssa_mov_rs1(op), ssa_simple++;
        }
    }
}

static void ssa_shift_pass(DecodedBlock* blk)
{
    for (size_t n = 0; n + 1 < blk->oplist.size(); n++)
    {
        shil_opcode& op = blk->oplist[n];
        shil_opcode& next = blk->oplist[n + 1];
        // rX = rY << a; rX = rX << b  ->  rX = rY; rX = rX << (a + b)
        if (op.op == next.op && (op.op == shop_shl || op.op == shop_shr || op.op == shop_sar) && op.rd.is_r32i() &&
            op.rs1.is_r32i() && op.rs2.is_imm() && next.rs2.is_imm() && next.rs1.is_r32i() && next.rd.is_r32i() &&
            next.rs1._reg == op.rd._reg && next.rd._reg == op.rd._reg && op.rs2._imm + next.rs2._imm <= 31)
        {
            next.rs2._imm += op.rs2._imm;
            ssa_mov_rs1(op);
            ssa_shifts++;
        }
    }
}

static void ssa_idmov_pass(DecodedBlock* blk)
{
    for (size_t n = 0; n < blk->oplist.size(); n++)
    {
        const shil_opcode& op = blk->oplist[n];
        if (op.op == shop_mov32 && op.rs1.is_r32() && op.rd.is_r32() && op.rd._reg == op.rs1._reg && op.rd.type == op.rs1.type)
        {
            blk->oplist.erase(blk->oplist.begin() + n);
            n--;
            ssa_idmov++;
        }
    }
}

static void ssa_optimize(DecodedBlock* blk)
{
    ssa_ops_in += blk->oplist.size();
    ssa_version_pass(blk);
    ssa_constprop_pass(blk);
    ssa_dead_pass(blk);
    ssa_simplify_pass(blk);
    ssa_shift_pass(blk);
    ssa_idmov_pass(blk);
    ssa_ops_out += blk->oplist.size();
    if (++ssa_blocks % 4096 == 0)
        printf("[ssa] %u blocks: ops %u -> %u (-%.1f%%); constants %u, folded %u, dead %u, simplified %u, shifts %u, "
               "identity moves %u\n", ssa_blocks, ssa_ops_in, ssa_ops_out,
               100.0 * (ssa_ops_in - ssa_ops_out) / (ssa_ops_in ? ssa_ops_in : 1), ssa_consts, ssa_folded, ssa_dead,
               ssa_simple, ssa_shifts, ssa_idmov);
}

void AnalyseBlock(DecodedBlock* blk)
{
    if (g_exp_ssa)
    {
        ssa_optimize(blk);
        return;
    }
#if !SHIL_DCE
    return;
#else
    memset(RegisterWrite, 0xFF, sizeof(RegisterWrite));  // -1 as s32
    memset(RegisterRead,  0xFF, sizeof(RegisterRead));   // -1 as s32

    ++total_blocks;

    const s32 nOps = static_cast<s32>(blk->oplist.size());

    for (s32 i = 0; i < nOps; ++i)
    {
        shil_opcode* op = &blk->oplist[i];
        op->Flow = 0;

        // An interpreter fallback can read and write any register, including
        // ones this analysis believes are dead. Bail on the whole block —
        // the purity whitelist only stops us deleting the ifb itself, not
        // the writes feeding it.
        if (op->op == shop_ifb)
        {
            ++fallback_blocks;
            for (s32 j = 0; j < i; ++j)
                blk->oplist[j].Flow = 0;
            return;
        }

        RegReadInfo(op->rs1, i);
        RegReadInfo(op->rs2, i);
        RegReadInfo(op->rs3, i);

        // shop_div1 reads AND writes SR.Q/SR.M through sr_status, which no
        // shil param carries (rs1/rs2/rs3 are already Rn/Rm/T). Declare the
        // read explicitly or the div0s that set Q/M up looks dead as soon as
        // a second division in the same block writes sr_status again.
        if (op->op == shop_div1)
            RegReadInfo(shil_param(reg_sr_status), i);

        // Same shape for mac.l/mac.w: rd/rd2 are MACL/MACH, but they are
        // ACCUMULATORS — read as well as written — and no param says so. Two
        // macs in a row would otherwise make the first one's write look dead.
        if (op->op == shop_mac_l || op->op == shop_mac_w)
        {
            RegReadInfo(shil_param(reg_macl), i);
            RegReadInfo(shil_param(reg_mach), i);
        }

        RegWriteInfo(&blk->oplist[0], op->rd,  i);
        RegWriteInfo(&blk->oplist[0], op->rd2, i);
    }

    for (int i = static_cast<int>(blk->oplist.size()) - 1; i >= 0; --i)
    {
        if (blk->oplist[i].Flow)
        {
            blk->oplist.erase(blk->oplist.begin() + i);
            ++REMOVED_OPS;
        }
    }

#if SHIL_DCE_STATS
    // Periodic, not per-block: a printf on every compiled block floods the
    // console and skews the very measurement it is meant to support.
    if ((total_blocks & 8191) == 0)
    {
        printf("[DCE] blocks=%u ifb-skipped=%u ops-removed=%u\n",
               total_blocks, fallback_blocks, REMOVED_OPS);
    }
#endif
#endif // SHIL_DCE
}

// ---------------------------------------------------------------------------
// Forward declarations expected by canonical implementations.
// ---------------------------------------------------------------------------
void FASTCALL do_sqw_mmu(u32 dst);
void FASTCALL do_sqw_nommu(u32 dst);
void UpdateFPSCR();
bool UpdateSR();

#include "dc/sh4/ccn.h"
#include "ngen.h"
#include "dc/sh4/sh4_registers.h"

// ---------------------------------------------------------------------------
// mac.l / mac.w — the single implementation, declared in shil.h. Both the
// canonical shil bodies and the Wii backend's rare S==1 path call these, so
// there is exactly one copy of the saturation rules.
//
// Transcribed from the SH-4 manual pseudocode. Note what it does NOT do: the
// old interpreter's mac.l had `verify(sr.S == 0)` (a hard die) and its mac.w
// printed a line and then skipped the instruction entirely, post-increments
// included.
// ---------------------------------------------------------------------------
extern "C" void sh4_mac_l(u32 a, u32 b, u32 oldl, u32 oldh, u32 srs)
{
    CCALL(CC_MAC_SAT);
    const u64 mul  = (u64)((s64)(s32)a * (s64)(s32)b);
    u32       res0 = oldl + (u32)mul;
    u32       res2 = (u32)(mul >> 32) + (oldl > res0 ? 1u : 0u);

    if (srs & 2)
    {
        // 48-bit saturation: only the low 16 bits of MACH take part, and the
        // result is clamped to [0xFFFF800000000000, 0x00007FFFFFFFFFFF].
        res2 += (oldh & 0x0000FFFFu);
        if ((s32)res2 < 0 && res2 < 0xFFFF8000u) { res2 = 0xFFFF8000u; res0 = 0x00000000u; }
        if ((s32)res2 > 0 && res2 > 0x00007FFFu) { res2 = 0x00007FFFu; res0 = 0xFFFFFFFFu; }
    }
    else
        res2 += oldh;

    macl = res0;
    mach = res2;
}

extern "C" void sh4_mac_w(u32 a, u32 b, u32 oldl, u32 oldh, u32 srs)
{
    CCALL(CC_MAC_SAT);
    const u32 prod = (u32)((s32)a * (s32)b);
    u32       res0 = oldl + prod;
    u32       res2 = oldh;

    if (srs & 2)
    {
        // 32-bit saturation. MACH is NOT touched (the "MACH LSB is an overflow
        // flag" behaviour belongs to SH-1/SH-DSP, not this core). Overflow only
        // when both addends share a sign and the sum flips it.
        const u32 dest = (s32)oldl < 0 ? 1u : 0u;
        const u32 src  = ((s32)prod < 0 ? 1u : 0u) + dest;
        const u32 ans  = ((s32)res0 < 0 ? 1u : 0u) + dest;
        if (ans == 1)
        {
            if (src == 0) res0 = 0x7FFFFFFFu;
            if (src == 2) res0 = 0x80000000u;
        }
    }
    else
    {
        const u32 sext = (u32)((s32)prod >> 31);        // 0 or 0xFFFFFFFF
        res2 += sext + (oldl > res0 ? 1u : 0u);         // + carry out of the low add
    }

    macl = res0;
    mach = res2;
}

// ---------------------------------------------------------------------------
// C call-out census (JIT_CCALLS preset). See ccall_census.h for the rationale.
// Storage lives here rather than in wii/main.cpp so that dc/ stays linkable on
// its own; the Wii menu and game_presets write g_jit_ccalls_preset directly.
// ---------------------------------------------------------------------------
extern "C"
{
	u32 g_ccall[CC_COUNT] = { 0 };
	u32 g_ccall_area[2][CCA_COUNT] = { { 0 } };
	u32 g_ccall_src[2][CCS_COUNT] = { { 0 } };
	const u8* g_ccall_tramp_lo = 0;
	const u8* g_ccall_tramp_hi = 0;
	const u8* g_ccall_cc_lo = 0;
	const u8* g_ccall_cc_hi = 0;
	int g_jit_ccalls_preset = 0;
}

// SH4 physical areas. 1 is PVR VRAM, 3 is system RAM, 4 is the TA / texture
// upload FIFO; the rest are rare enough that their names are only here so an
// unexpected hit is recognisable rather than a bare number.
// Which side of the JIT boundary a call-out came from. "tramp" is a
// back-patched fastmem trampoline, "codecache" is ordinary emitted code, and
// "other" is the emulator's own C code -- which is where the bulk of the
// system-RAM writes turned out to live (block transfers, not the dynarec).
static const char* const s_ccall_src_name[CCS_COUNT] =
{
	"from:tramp", "from:codecache", "from:other",
};

static const char* const s_ccall_area_name[CCA_COUNT] =
{
	"a0:boot/reg", "a1:VRAM", "a2", "a3:RAM",
	"a4:TA", "a5", "a6", "a7",
};

static const char* const s_ccall_name[CC_COUNT] =
{
	"timeslice",   // UpdateSystem_no_event
	"interrupt",   // UpdateSystem_handle_event / UpdateINTC
	"getcode",     // bm_GetCode
	"sqw",         // do_sqw  (store queue -> TA)
	"sync_sr",     // UpdateSR
	"sync_fpscr",  // UpdateFPSCR (full path)
	"fsqrt",       // rec_fsqrt  -> libm
	"fsrra",       // rec_fsrra  -> libm
	"readmem",     // _vmem_ReadMem*
	"writemem",    // _vmem_WriteMem*
	"mac_sat",     // sh4_mac_l/w saturation
};

extern "C" void ccall_census_dump(double seconds, double vbs)
{
	if (!g_jit_ccalls_preset || seconds <= 0.0)
		return;

	// Sort by rate, descending. CC_COUNT is tiny, so a selection sort once a
	// second costs nothing and keeps qsort out of the build.
	u32 idx[CC_COUNT];
	u32 n = 0;
	for (u32 i = 0; i < CC_COUNT; i++)
		if (g_ccall[i])
			idx[n++] = i;

	if (n == 0)
	{
		// Say so explicitly: a silent probe is indistinguishable from one that
		// was never switched on, and that ambiguity has cost a test run before.
		printf("[CC] %.2fs  no C call-outs counted\n", seconds);
		return;
	}

	for (u32 a = 0; a < n; a++)
	{
		u32 best = a;
		for (u32 b = a + 1; b < n; b++)
			if (g_ccall[idx[b]] > g_ccall[idx[best]])
				best = b;
		u32 t = idx[a]; idx[a] = idx[best]; idx[best] = t;
	}

	double total = 0.0;
	for (u32 i = 0; i < CC_COUNT; i++)
		total += (double)g_ccall[i];

	printf("[CC] %.2fs  %.0f call-outs/s total  (%.1f vbl/s)\n",
	       seconds, total / seconds, vbs);
#ifdef __WIIU__
	{
		extern u32 g_fpscr_bank_swaps;
		printf("[CC]   (fpscr syncs that swap FPU banks: %.0f/s)\n", g_fpscr_bank_swaps / seconds);
		g_fpscr_bank_swaps = 0;
	}
#endif

	for (u32 a = 0; a < n; a++)
	{
		const u32 i = idx[a];
		const double per_s = (double)g_ccall[i] / seconds;
		const double per_f = (vbs > 0.0) ? per_s / vbs : 0.0;
		printf("[CC]   %-11s %10.0f/s  %9.1f/frame  %5.1f%%\n",
		       s_ccall_name[i], per_s, per_f,
		       total > 0.0 ? 100.0 * (double)g_ccall[i] / total : 0.0);

		// readmem/writemem are the only two with a per-area split, and it is
		// the split -- not the total -- that says whether anything can be done.
		if (i == CC_READMEM || i == CC_WRITEMEM)
		{
			const u32 rw = (i == CC_WRITEMEM) ? 1u : 0u;
			for (u32 k = 0; k < CCA_COUNT; k++)
			{
				if (!g_ccall_area[rw][k])
					continue;
				printf("[CC]     %-11s %10.0f/s  %9.1f/frame  %5.1f%% of that\n",
				       s_ccall_area_name[k],
				       (double)g_ccall_area[rw][k] / seconds,
				       (vbs > 0.0) ? (double)g_ccall_area[rw][k] / seconds / vbs : 0.0,
				       g_ccall[i] ? 100.0 * (double)g_ccall_area[rw][k] / (double)g_ccall[i] : 0.0);
			}
			for (u32 k = 0; k < CCS_COUNT; k++)
			{
				if (!g_ccall_src[rw][k])
					continue;
				printf("[CC]     %-14s %10.0f/s  %9.1f/frame  %5.1f%% of that\n",
				       s_ccall_src_name[k],
				       (double)g_ccall_src[rw][k] / seconds,
				       (vbs > 0.0) ? (double)g_ccall_src[rw][k] / seconds / vbs : 0.0,
				       g_ccall[i] ? 100.0 * (double)g_ccall_src[rw][k] / (double)g_ccall[i] : 0.0);
			}
		}
	}

	for (u32 i = 0; i < CC_COUNT; i++)
		g_ccall[i] = 0;
	for (u32 rw = 0; rw < 2; rw++)
	{
		for (u32 k = 0; k < CCA_COUNT; k++)
			g_ccall_area[rw][k] = 0;
		for (u32 k = 0; k < CCS_COUNT; k++)
			g_ccall_src[rw][k] = 0;
	}
}

// Instantiate canonical (portable C) implementations (SHIL_MODE 1).
#define SHIL_MODE 1
#include "shil_canonical.h"

// Instantiate the dispatch table (SHIL_MODE 3).
#define SHIL_MODE 3
#include "shil_canonical.h"
