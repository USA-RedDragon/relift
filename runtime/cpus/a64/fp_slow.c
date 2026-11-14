/**
 * @file fp_slow.c
 * @brief rl_fp_slow: decodes one A64 FP or AdvSIMD FP instruction and runs
 * it through the exact FP layer (fp.c) with the context's FPCR.
 *
 * Each class decodes its fields as the Arm ARM encoding tables give them and
 * applies one element operation per lane: scalar forms work on element 0 and
 * clear the rest of the register, vector forms on every lane (64 or 128
 * bits), pairwise and across-lanes forms in the pseudocode's order.
 */
#include "a64.h"
#include "fp.h"

#include <relift/a64/fp_slow.h>

#include <string.h>

static int unsupported(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    return rl_guest_fatal(c, "guest", "no FPCR slow path for instruction 0x%08x at guest pc 0x%llx (FPCR 0x%x)", insn,
                          (unsigned long long)pc, c->fpcr);
}

static uint64_t sign_bit(unsigned w) { return 1ull << (w - 1); }

static uint64_t wmask(unsigned w) { return w == 64 ? ~0ull : (1ull << w) - 1; }

/* Element operations. */
enum {
    E_ADD,
    E_SUB,
    E_MUL,
    E_DIV,
    E_MAX,
    E_MIN,
    E_MAXNM,
    E_MINNM,
    E_ABD,
    E_NMUL,
    E_CMEQ,
    E_CMGE,
    E_CMGT,
    E_ACGE,
    E_ACGT,
    E_RECPS,
    E_RSQRTS,
};

/**
 * @brief The FPCR an operation on width w sees. AHP selects the alternative
 * half precision format for FCVT only; every other half precision operation
 * unpacks and rounds as if AHP were clear (FPUnpack and FPRound clear it).
 */
static uint32_t fpcr_for(const rl_ctx *c, unsigned w) { return w == 16 ? c->fpcr & ~RL_FPCR_AHP : c->fpcr; }

static uint64_t elem2(rl_ctx *c, unsigned w, int op, uint64_t a, uint64_t b)
{
    uint32_t f = fpcr_for(c, w), *s = &c->fpsr;
    switch (op) {
    case E_ADD: return rl_fpn_add(w, a, b, f, s);
    case E_SUB: return rl_fpn_sub(w, a, b, f, s);
    case E_MUL: return rl_fpn_mul(w, a, b, f, s);
    case E_DIV: return rl_fpn_div(w, a, b, f, s);
    case E_MAX: return rl_fpn_maxmin(w, a, b, 1, 0, f, s);
    case E_MIN: return rl_fpn_maxmin(w, a, b, 0, 0, f, s);
    case E_MAXNM: return rl_fpn_maxmin(w, a, b, 1, 1, f, s);
    case E_MINNM: return rl_fpn_maxmin(w, a, b, 0, 1, f, s);
    case E_ABD: return rl_fpn_sub(w, a, b, f, s) & ~sign_bit(w);
    case E_NMUL: return rl_fpn_mul(w, a, b, f, s) ^ sign_bit(w);
    case E_CMEQ: return rl_fpn_compare_bool(w, a, b, 0, f, s) ? wmask(w) : 0;
    case E_CMGE: return rl_fpn_compare_bool(w, a, b, 1, f, s) ? wmask(w) : 0;
    case E_CMGT: return rl_fpn_compare_bool(w, a, b, 2, f, s) ? wmask(w) : 0;
    case E_ACGE: return rl_fpn_compare_bool(w, a & ~sign_bit(w), b & ~sign_bit(w), 1, f, s) ? wmask(w) : 0;
    case E_ACGT: return rl_fpn_compare_bool(w, a & ~sign_bit(w), b & ~sign_bit(w), 2, f, s) ? wmask(w) : 0;
    case E_RECPS: return rl_fpn_recip_step(w, a, b, f, s);
    default: return rl_fpn_rsqrt_step(w, a, b, f, s);
    }
}

static uint64_t lane_get(const rl_v128 *v, unsigned w, unsigned i)
{
    switch (w) {
    case 16: return v->h[i];
    case 32: return v->s[i];
    default: return v->d[i];
    }
}

static void lane_set(rl_v128 *v, unsigned w, unsigned i, uint64_t x)
{
    switch (w) {
    case 16: v->h[i] = (uint16_t)x; break;
    case 32: v->s[i] = (uint32_t)x; break;
    default: v->d[i] = x; break;
    }
}

/** @brief Writes a scalar result: element 0, the rest of the register zero. */
static void write_scalar(rl_ctx *c, unsigned w, unsigned r, uint64_t x)
{
    c->v[r].q = 0;
    lane_set(&c->v[r], w, 0, x);
}

static uint64_t read_scalar(const rl_ctx *c, unsigned w, unsigned r) { return lane_get(&c->v[r], w, 0); }

static uint64_t read_gpr(const rl_ctx *c, unsigned r) { return r == 31 ? 0 : c->x[r]; }

static void write_gpr(rl_ctx *c, unsigned r, uint64_t v)
{
    if (r != 31)
        c->x[r] = v;
}

static int cond_holds(uint32_t nzcv, unsigned cond)
{
    int n = (nzcv >> 31) & 1, z = (nzcv >> 30) & 1, cy = (nzcv >> 29) & 1, v = (nzcv >> 28) & 1, r;
    switch (cond >> 1) {
    case 0: r = z; break;
    case 1: r = cy; break;
    case 2: r = n; break;
    case 3: r = v; break;
    case 4: r = cy && !z; break;
    case 5: r = n == v; break;
    case 6: r = !z && n == v; break;
    default: r = 1; break;
    }
    if ((cond & 1) && cond != 15)
        r = !r;
    return r;
}

static unsigned type_bits(unsigned type)
{
    switch (type) {
    case 0: return 32;
    case 1: return 64;
    case 3: return 16;
    default: return 0;
    }
}

/** @brief True for the widths the slow path handles: half, single and double. */
static int fp_width(unsigned w) { return w == 16 || w == 32 || w == 64; }

static uint64_t expand_imm(unsigned w, uint32_t imm8)
{
    uint64_t sign = (imm8 >> 7) & 1u, b = (imm8 >> 6) & 1u, rest = imm8 & 0x3Fu;
    if (w == 16)
        return (sign << 15) | ((b ^ 1u) << 14) | (b ? 3ull << 12 : 0) | (rest << 6);
    if (w == 32)
        return (sign << 31) | ((b ^ 1u) << 30) | (b ? 0x1Full << 25 : 0) | (rest << 19);
    return (sign << 63) | ((b ^ 1u) << 62) | (b ? 0xFFull << 54 : 0) | (rest << 48);
}

/* ---- Scalar FP (floating-point data processing) ---- */

static int scalar_dp1(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned type = (insn >> 22) & 3u, op = (insn >> 15) & 0x3Fu, rn = (insn >> 5) & 31u, rd = insn & 31u;
    unsigned w = type_bits(type);
    if (op >= 0x04 && op <= 0x07 && (op & 3u) != 2u) {
        unsigned to = type_bits(op & 3u);
        if (!w || !to || to == w)
            return unsupported(c, pc, insn);
        write_scalar(c, to, rd, rl_fp_convert(read_scalar(c, w, rn), w, to, RL_RM_FPCR, c->fpcr, &c->fpsr));
        return RL_OK;
    }
    if (!fp_width(w))
        return unsupported(c, pc, insn);
    uint64_t a = read_scalar(c, w, rn), r;
    uint32_t f = fpcr_for(c, w), *s = &c->fpsr;
    switch (op) {
    case 0x00: r = a; break;
    case 0x01: r = a & ~sign_bit(w); break;
    case 0x02: r = a ^ sign_bit(w); break;
    case 0x03: r = rl_fpn_sqrt(w, a, f, s); break;
    case 0x08: r = rl_fpn_round_int(w, a, RL_RM_NEAREST, 0, f, s); break;
    case 0x09: r = rl_fpn_round_int(w, a, RL_RM_POSINF, 0, f, s); break;
    case 0x0A: r = rl_fpn_round_int(w, a, RL_RM_NEGINF, 0, f, s); break;
    case 0x0B: r = rl_fpn_round_int(w, a, RL_RM_ZERO, 0, f, s); break;
    case 0x0C: r = rl_fpn_round_int(w, a, RL_RM_AWAY, 0, f, s); break;
    case 0x0E: r = rl_fpn_round_int(w, a, RL_RM_FPCR, 1, f, s); break;
    case 0x0F: r = rl_fpn_round_int(w, a, RL_RM_FPCR, 0, f, s); break;
    case 0x10: /* FRINT32Z, FRINT32X, FRINT64Z, FRINT64X */
    case 0x11:
    case 0x12:
    case 0x13:
        if (w == 16)
            return unsupported(c, pc, insn);
        r = rl_fpn_round_int_n(w, a, (op & 1u) ? RL_RM_FPCR : RL_RM_ZERO, (op & 2u) ? 64 : 32, f, s);
        break;
    default: return unsupported(c, pc, insn);
    }
    write_scalar(c, w, rd, r);
    return RL_OK;
}

static int scalar_dp2(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u), op = (insn >> 12) & 15u;
    unsigned rm = (insn >> 16) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    static const int ops[9] = { E_MUL, E_DIV, E_ADD, E_SUB, E_MAX, E_MIN, E_MAXNM, E_MINNM, E_NMUL };
    if (!fp_width(w) || op > 8)
        return unsupported(c, pc, insn);
    write_scalar(c, w, rd, elem2(c, w, ops[op], read_scalar(c, w, rn), read_scalar(c, w, rm)));
    return RL_OK;
}

static int scalar_dp3(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u), o1 = (insn >> 21) & 1u, o0 = (insn >> 15) & 1u;
    unsigned rm = (insn >> 16) & 31u, ra = (insn >> 10) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    if (!fp_width(w))
        return unsupported(c, pc, insn);
    uint64_t a = read_scalar(c, w, ra), n = read_scalar(c, w, rn), m = read_scalar(c, w, rm);
    if (o1)
        a ^= sign_bit(w);
    if (o0 != o1)
        n ^= sign_bit(w);
    write_scalar(c, w, rd, rl_fpn_muladd(w, a, n, m, fpcr_for(c, w), &c->fpsr));
    return RL_OK;
}

static int scalar_cmp(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u), rm = (insn >> 16) & 31u, rn = (insn >> 5) & 31u;
    unsigned opc2 = insn & 31u;
    if (!fp_width(w) || (opc2 & 7u) || ((insn >> 14) & 3u))
        return unsupported(c, pc, insn);
    uint64_t b = (opc2 & 8u) ? 0 : read_scalar(c, w, rm);
    c->nzcv = rl_fpn_compare(w, read_scalar(c, w, rn), b, (opc2 & 16u) != 0, fpcr_for(c, w), &c->fpsr);
    return RL_OK;
}

static int scalar_ccmp(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u), rm = (insn >> 16) & 31u, rn = (insn >> 5) & 31u;
    unsigned cond = (insn >> 12) & 15u;
    if (!fp_width(w))
        return unsupported(c, pc, insn);
    if (cond_holds(c->nzcv, cond))
        c->nzcv = rl_fpn_compare(w, read_scalar(c, w, rn), read_scalar(c, w, rm), (insn >> 4) & 1u, fpcr_for(c, w), &c->fpsr);
    else
        c->nzcv = (insn & 15u) << 28;
    return RL_OK;
}

static int scalar_sel(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u), rm = (insn >> 16) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    if (!fp_width(w))
        return unsupported(c, pc, insn);
    write_scalar(c, w, rd, read_scalar(c, w, cond_holds(c->nzcv, (insn >> 12) & 15u) ? rn : rm));
    return RL_OK;
}

static int scalar_imm(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u);
    if (!fp_width(w))
        return unsupported(c, pc, insn);
    write_scalar(c, w, insn & 31u, expand_imm(w, (insn >> 13) & 0xFFu));
    return RL_OK;
}

static int scalar_int(rl_ctx *c, uint64_t pc, uint32_t insn, int fixed)
{
    unsigned sf = insn >> 31, type = (insn >> 22) & 3u, rmode = (insn >> 19) & 3u, opcode = (insn >> 16) & 7u;
    unsigned scale = (insn >> 10) & 63u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    unsigned w = type_bits(type), ibits = sf ? 64 : 32, fbits = 0;
    if (fixed) {
        fbits = 64 - scale;
        if (!sf && scale < 32)
            return unsupported(c, pc, insn);
    }
    if (!fixed && rmode == 1 && (opcode == 6 || opcode == 7) && sf && type == 2) {
        if (opcode == 6)
            write_gpr(c, rd, c->v[rn].d[1]);
        else
            c->v[rd].d[1] = read_gpr(c, rn);
        return RL_OK;
    }
    if (!fp_width(w))
        return unsupported(c, pc, insn);
    if (!fixed && rmode == 0 && (opcode == 6 || opcode == 7)) {
        if (w != 16 && w != ibits)
            return unsupported(c, pc, insn);
        if (opcode == 6)
            write_gpr(c, rd, read_scalar(c, w, rn));
        else
            write_scalar(c, w, rd, read_gpr(c, rn) & wmask(w));
        return RL_OK;
    }
    if (rmode == 0 && (opcode == 2 || opcode == 3)) {
        uint64_t a = read_gpr(c, rn) & wmask(ibits);
        write_scalar(c, w, rd, rl_fixed_to_fp(a, ibits, w, fbits, opcode == 3, RL_RM_FPCR, fpcr_for(c, w), &c->fpsr));
        return RL_OK;
    }
    unsigned rm;
    if (fixed) {
        if (rmode != 3 || opcode > 1)
            return unsupported(c, pc, insn);
        rm = RL_RM_ZERO;
    } else if (opcode <= 1) {
        rm = rmode;
    } else if (opcode <= 5 && rmode == 0 && opcode >= 4) {
        rm = RL_RM_AWAY;
    } else {
        return unsupported(c, pc, insn);
    }
    uint64_t r = rl_fp_to_fixed(read_scalar(c, w, rn), w, ibits, fbits, opcode & 1u, rm, fpcr_for(c, w), &c->fpsr);
    write_gpr(c, rd, r & wmask(ibits));
    return RL_OK;
}

/* ---- AdvSIMD FP ---- */

/** @brief The element operation of a three-same FP opcode (U, a = bit 23, opcode<4:0>), or -1. */
static int same_op(unsigned u, unsigned a, unsigned opc, int *pair, int *fma)
{
    *pair = 0;
    *fma = 0;
    switch ((u << 6) | (a << 5) | opc) {
    case 0x18: return E_MAXNM;
    case 0x19: *fma = 1; return E_ADD;
    case 0x1A: return E_ADD;
    case 0x1C: return E_CMEQ;
    case 0x1E: return E_MAX;
    case 0x1F: return E_RECPS;
    case 0x38: return E_MINNM;
    case 0x39: *fma = 2; return E_ADD;
    case 0x3A: return E_SUB;
    case 0x3E: return E_MIN;
    case 0x3F: return E_RSQRTS;
    case 0x58: *pair = 1; return E_MAXNM;
    case 0x5A: *pair = 1; return E_ADD;
    case 0x5B: return E_MUL;
    case 0x5C: return E_CMGE;
    case 0x5D: return E_ACGE;
    case 0x5E: *pair = 1; return E_MAX;
    case 0x5F: return E_DIV;
    case 0x78: *pair = 1; return E_MINNM;
    case 0x7A: return E_ABD;
    case 0x7C: return E_CMGT;
    case 0x7D: return E_ACGT;
    case 0x7E: *pair = 1; return E_MIN;
    default: return -1;
    }
}

/* Three registers of the same type, vector (asimdsame) or scalar (asisdsame). */
static int simd_same(rl_ctx *c, uint64_t pc, uint32_t insn, int scalar, int half)
{
    unsigned q = (insn >> 30) & 1u, u = (insn >> 29) & 1u, a = (insn >> 23) & 1u, sz = (insn >> 22) & 1u;
    unsigned opc = (insn >> 11) & 31u, rm = (insn >> 16) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    /* The half precision class has a 3-bit opcode in bits 13 to 11 that
     * matches the low bits of the single and double opcodes (bits 15 and 14
     * are zero, bit 22 is one). */
    if (half)
        opc = 0x18u | (opc & 7u);
    unsigned w = half ? 16 : (sz ? 64 : 32), lanes = scalar ? 1 : (q ? 128 : 64) / w;
    int pair, fma, op = same_op(u, a, opc, &pair, &fma);
    if (op < 0 || (!half && !scalar && sz && !q) || (scalar && (pair || fma)))
        return unsupported(c, pc, insn);
    rl_v128 n = c->v[rn], m = c->v[rm], d = c->v[rd], r;
    r.q = 0;
    for (unsigned i = 0; i < lanes; i++) {
        uint64_t x;
        if (pair) {
            unsigned j = 2 * i;
            uint64_t p = j < lanes ? lane_get(&n, w, j) : lane_get(&m, w, j - lanes);
            uint64_t s = j + 1 < lanes ? lane_get(&n, w, j + 1) : lane_get(&m, w, j + 1 - lanes);
            x = elem2(c, w, op, p, s);
        } else if (fma) {
            uint64_t e = lane_get(&n, w, i);
            if (fma == 2)
                e ^= sign_bit(w);
            x = rl_fpn_muladd(w, lane_get(&d, w, i), e, lane_get(&m, w, i), fpcr_for(c, w), &c->fpsr);
        } else {
            x = elem2(c, w, op, lane_get(&n, w, i), lane_get(&m, w, i));
        }
        lane_set(&r, w, i, x);
    }
    c->v[rd] = r;
    return RL_OK;
}

enum { M_CVT, M_SCVTF, M_ABS, M_NEG, M_SQRT, M_RINT, M_RINTN, M_CMP0, M_RECPE, M_RSQRTE, M_CVTN, M_CVTL };

/* Two-register miscellaneous, vector (asimdmisc) or scalar (asisdmisc). */
static int simd_misc(rl_ctx *c, uint64_t pc, uint32_t insn, int scalar, int half)
{
    unsigned q = (insn >> 30) & 1u, u = (insn >> 29) & 1u, a = (insn >> 23) & 1u, sz = (insn >> 22) & 1u;
    unsigned opc = (insn >> 12) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    unsigned w = half ? 16 : (sz ? 64 : 32), lanes = scalar ? 1 : (q ? 128 : 64) / w;
    int kind, exact = 0, cmp = 0, nbits = 32;
    uint32_t rm = RL_RM_NEAREST;
    uint32_t f = fpcr_for(c, w), *s = &c->fpsr;
    switch ((u << 6) | (a << 5) | opc) {
    case 0x16: kind = M_CVTN; break;
    case 0x17: kind = M_CVTL; break;
    case 0x18: kind = M_RINT; rm = RL_RM_NEAREST; break;
    case 0x19: kind = M_RINT; rm = RL_RM_NEGINF; break;
    case 0x1A: kind = M_CVT; rm = RL_RM_NEAREST; break;
    case 0x1B: kind = M_CVT; rm = RL_RM_NEGINF; break;
    case 0x1C: kind = M_CVT; rm = RL_RM_AWAY; break;
    case 0x1D: kind = M_SCVTF; break;
    case 0x1E: kind = M_RINTN; rm = RL_RM_ZERO; break;
    case 0x1F: kind = M_RINTN; rm = RL_RM_ZERO; nbits = 64; break;
    case 0x2C: kind = M_CMP0; cmp = E_CMGT; break;
    case 0x2D: kind = M_CMP0; cmp = E_CMEQ; break;
    case 0x2E: kind = M_CMP0; cmp = -E_CMGT; break;
    case 0x2F: kind = M_ABS; break;
    case 0x38: kind = M_RINT; rm = RL_RM_POSINF; break;
    case 0x39: kind = M_RINT; rm = RL_RM_ZERO; break;
    case 0x3A: kind = M_CVT; rm = RL_RM_POSINF; break;
    case 0x3B: kind = M_CVT; rm = RL_RM_ZERO; break;
    case 0x3D: kind = M_RECPE; break;
    case 0x58: kind = M_RINT; rm = RL_RM_AWAY; break;
    case 0x59: kind = M_RINT; rm = RL_RM_FPCR; exact = 1; break;
    case 0x5A: kind = M_CVT; rm = RL_RM_NEAREST; break;
    case 0x5B: kind = M_CVT; rm = RL_RM_NEGINF; break;
    case 0x5C: kind = M_CVT; rm = RL_RM_AWAY; break;
    case 0x5D: kind = M_SCVTF; break;
    case 0x5E: kind = M_RINTN; rm = RL_RM_FPCR; break;
    case 0x5F: kind = M_RINTN; rm = RL_RM_FPCR; nbits = 64; break;
    case 0x6C: kind = M_CMP0; cmp = E_CMGE; break;
    case 0x6D: kind = M_CMP0; cmp = -E_CMGE; break;
    case 0x6F: kind = M_NEG; break;
    case 0x79: kind = M_RINT; rm = RL_RM_FPCR; break;
    case 0x7A: kind = M_CVT; rm = RL_RM_POSINF; break;
    case 0x7B: kind = M_CVT; rm = RL_RM_ZERO; break;
    case 0x7D: kind = M_RSQRTE; break;
    case 0x7F: kind = M_SQRT; break;
    default: return unsupported(c, pc, insn);
    }
    if (half && (kind == M_CVTN || kind == M_CVTL))
        return unsupported(c, pc, insn);
    if ((half || scalar) && kind == M_RINTN)
        return unsupported(c, pc, insn);
    if (kind == M_CVTN || kind == M_CVTL) {
        /* sz 0: half and single; sz 1: single and double. */
        unsigned wide = sz ? 64 : 32, narrow = sz ? 32 : 16, cnt = 64 / narrow;
        if (scalar || u)
            return unsupported(c, pc, insn);
        rl_v128 src = c->v[rn], r = c->v[rd];
        if (kind == M_CVTN) {
            /* FCVTN writes the low half and clears the high one; FCVTN2
             * writes the high half and keeps the low one. */
            if (!q)
                r.q = 0;
            for (unsigned i = 0; i < cnt; i++)
                lane_set(&r, narrow, q * cnt + i, rl_fp_convert(lane_get(&src, wide, i), wide, narrow, RL_RM_FPCR, f, s));
        } else {
            /* FCVTL reads the low half, FCVTL2 the high one. */
            r.q = 0;
            for (unsigned i = 0; i < cnt; i++)
                lane_set(&r, wide, i, rl_fp_convert(lane_get(&src, narrow, q * cnt + i), narrow, wide, RL_RM_FPCR, f, s));
        }
        c->v[rd] = r;
        return RL_OK;
    }
    if (!half && !scalar && sz && !q)
        return unsupported(c, pc, insn);
    rl_v128 n = c->v[rn], r;
    r.q = 0;
    for (unsigned i = 0; i < lanes; i++) {
        uint64_t e = lane_get(&n, w, i), x;
        switch (kind) {
        case M_CVT: x = rl_fp_to_fixed(e, w, w, 0, u, rm, f, s); break;
        case M_SCVTF: x = rl_fixed_to_fp(e, w, w, 0, u, RL_RM_FPCR, f, s); break;
        case M_ABS: x = e & ~sign_bit(w); break;
        case M_NEG: x = e ^ sign_bit(w); break;
        case M_SQRT: x = rl_fpn_sqrt(w, e, f, s); break;
        case M_RECPE: x = rl_fpn_recip_est(w, e, f, s); break;
        case M_RSQRTE: x = rl_fpn_rsqrt_est(w, e, f, s); break;
        case M_CMP0: x = cmp > 0 ? elem2(c, w, cmp, e, 0) : elem2(c, w, -cmp, 0, e); break;
        case M_RINTN: x = rl_fpn_round_int_n(w, e, rm, (unsigned)nbits, f, s); break;
        default: x = rl_fpn_round_int(w, e, rm, exact, f, s); break;
        }
        lane_set(&r, w, i, x);
    }
    c->v[rd] = r;
    return RL_OK;
}

/** @brief Reduce: the pseudocode's recursive halving over lanes lo .. lo + n - 1. */
static uint64_t reduce(rl_ctx *c, unsigned w, int op, const rl_v128 *v, unsigned lo, unsigned n)
{
    if (n == 1)
        return lane_get(v, w, lo);
    uint64_t a = reduce(c, w, op, v, lo, n / 2), b = reduce(c, w, op, v, lo + n / 2, n / 2);
    return elem2(c, w, op, a, b);
}

static int pair_op(unsigned u, unsigned a, unsigned opc)
{
    switch ((u << 6) | (a << 5) | opc) {
    case 0x0C:
    case 0x4C: return E_MAXNM;
    case 0x2C:
    case 0x6C: return E_MINNM;
    case 0x0D:
    case 0x4D: return E_ADD;
    case 0x0F:
    case 0x4F: return E_MAX;
    case 0x2F:
    case 0x6F: return E_MIN;
    default: return -1;
    }
}

/* Scalar pairwise (asisdpair): two elements of Vn; U is 1 for the single and
 * double forms and 0 for the half precision ones (bit 22 is zero then). */
static int scalar_pair(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned u = (insn >> 29) & 1u, a = (insn >> 23) & 1u, sz = (insn >> 22) & 1u, opc = (insn >> 12) & 31u;
    unsigned rn = (insn >> 5) & 31u, rd = insn & 31u, w = u ? (sz ? 64 : 32) : 16;
    int op = pair_op(1, a, opc);
    if (op < 0 || (!u && sz))
        return unsupported(c, pc, insn);
    rl_v128 n = c->v[rn];
    write_scalar(c, w, rd, reduce(c, w, op, &n, 0, 2));
    return RL_OK;
}

/* Across lanes (asimdall): FMAXNMV, FMINNMV, FMAXV, FMINV on 4S (U is 1) or
 * on 4H and 8H (U is 0, bit 22 is zero). */
static int across(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1u, u = (insn >> 29) & 1u, a = (insn >> 23) & 1u, sz = (insn >> 22) & 1u;
    unsigned opc = (insn >> 12) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    int op = pair_op(1, a, opc);
    if (op < 0 || op == E_ADD || sz || (u && !q))
        return unsupported(c, pc, insn);
    rl_v128 n = c->v[rn];
    if (u) {
        write_scalar(c, 32, rd, reduce(c, 32, op, &n, 0, 4));
    } else {
        write_scalar(c, 16, rd, reduce(c, 16, op, &n, 0, q ? 8 : 4));
    }
    return RL_OK;
}

/* Shift by immediate: the fixed-point conversions, vector (asimdshf) or scalar (asisdshf). */
static int simd_shift(rl_ctx *c, uint64_t pc, uint32_t insn, int scalar)
{
    unsigned q = (insn >> 30) & 1u, u = (insn >> 29) & 1u, immh = (insn >> 19) & 15u, immb = (insn >> 16) & 7u;
    unsigned opc = (insn >> 11) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    unsigned w;
    if (immh & 8u)
        w = 64;
    else if (immh & 4u)
        w = 32;
    else if (immh & 2u)
        w = 16;
    else
        return unsupported(c, pc, insn);
    unsigned fbits = 2 * w - ((immh << 3) | immb), lanes = scalar ? 1 : (q ? 128 : 64) / w;
    if ((opc != 0x1C && opc != 0x1F) || (!scalar && w == 64 && !q))
        return unsupported(c, pc, insn);
    rl_v128 n = c->v[rn], r;
    r.q = 0;
    for (unsigned i = 0; i < lanes; i++) {
        uint64_t e = lane_get(&n, w, i), x;
        if (opc == 0x1C)
            x = rl_fixed_to_fp(e, w, w, fbits, u, RL_RM_FPCR, fpcr_for(c, w), &c->fpsr);
        else
            x = rl_fp_to_fixed(e, w, w, fbits, u, RL_RM_ZERO, fpcr_for(c, w), &c->fpsr);
        lane_set(&r, w, i, x);
    }
    c->v[rd] = r;
    return RL_OK;
}

/* By element: FMLA, FMLS, FMUL, vector (asimdelem) or scalar (asisdelem). */
static int simd_elem(rl_ctx *c, uint64_t pc, uint32_t insn, int scalar)
{
    unsigned q = (insn >> 30) & 1u, u = (insn >> 29) & 1u, size = (insn >> 22) & 3u, l = (insn >> 21) & 1u;
    unsigned mm = (insn >> 16) & 31u, opc = (insn >> 12) & 15u, h = (insn >> 11) & 1u;
    unsigned rn = (insn >> 5) & 31u, rd = insn & 31u;
    unsigned w = size == 3 ? 64 : (size == 0 ? 16 : 32), index = size == 3 ? h : (h << 1) | l;
    if (size == 0) {
        /* Half precision: Rm has four bits and bit 20 (M) extends the index. */
        index = (h << 2) | (l << 1) | ((insn >> 20) & 1u);
        mm &= 15u;
    }
    if (size == 1 || (size == 3 && l) || u || (opc != 1 && opc != 5 && opc != 9) || (!scalar && w == 64 && !q))
        return unsupported(c, pc, insn);
    unsigned lanes = scalar ? 1 : (q ? 128 : 64) / w;
    uint64_t e2 = lane_get(&c->v[mm], w, index);
    rl_v128 n = c->v[rn], d = c->v[rd], r;
    r.q = 0;
    for (unsigned i = 0; i < lanes; i++) {
        uint64_t e1 = lane_get(&n, w, i), x;
        if (opc == 9)
            x = rl_fpn_mul(w, e1, e2, fpcr_for(c, w), &c->fpsr);
        else
            x = rl_fpn_muladd(w, lane_get(&d, w, i), opc == 5 ? e1 ^ sign_bit(w) : e1, e2, fpcr_for(c, w), &c->fpsr);
        lane_set(&r, w, i, x);
    }
    c->v[rd] = r;
    return RL_OK;
}

/*
 * BFMLALB and BFMLALT (vector and by element): four single precision lanes,
 * each the accumulator plus the product of two bfloat16 values (the even
 * or the odd half of each source lane, picked by Q). A bfloat16 widens to
 * single exactly by shifting it left 16 bits, so BFMulAddH is a single
 * precision FPMulAdd with the FPCR.
 */
static int bfmlal(rl_ctx *c, uint32_t insn, int by_element)
{
    unsigned sel = (insn >> 30) & 1u, rn = (insn >> 5) & 31u, rd = insn & 31u, rm = (insn >> 16) & 31u;
    unsigned index = 0;
    if (by_element) {
        rm &= 15u;
        index = (((insn >> 11) & 1u) << 2) | (((insn >> 21) & 1u) << 1) | ((insn >> 20) & 1u);
    }
    rl_v128 n = c->v[rn], m = c->v[rm], d = c->v[rd], r;
    r.q = 0;
    for (unsigned e = 0; e < 4; e++) {
        uint64_t a = (uint64_t)lane_get(&n, 16, 2 * e + sel) << 16;
        uint64_t b = (uint64_t)lane_get(&m, 16, by_element ? index : 2 * e + sel) << 16;
        lane_set(&r, 32, e, rl_fpn_muladd(32, lane_get(&d, 32, e), a, b, c->fpcr, &c->fpsr));
    }
    c->v[rd] = r;
    return RL_OK;
}

/*
 * Complex arithmetic (FCADD, FCMLA, vector and by element): each pair of
 * lanes is (real, imaginary). The rotation picks which operand lanes feed
 * the two FPAdd or FPMulAdd steps and which of them are negated (FPNeg is
 * a sign flip that raises nothing). mode 0 is FCADD, 1 is FCMLA, 2 is the
 * by-element FCMLA.
 */
static int simd_complex(rl_ctx *c, uint64_t pc, uint32_t insn, int mode)
{
    unsigned q = (insn >> 30) & 1u, size = (insn >> 22) & 3u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    unsigned rm = (insn >> 16) & 31u, rot = mode == 0 ? (insn >> 12) & 1u : (insn >> (mode == 1 ? 11 : 13)) & 3u;
    unsigned w = 8u << size, index = 0;
    if (size == 0 || (q == 0 && size == 3))
        return unsupported(c, pc, insn);
    if (mode == 2) {
        unsigned h = (insn >> 11) & 1u, l = (insn >> 21) & 1u;
        rm |= ((insn >> 20) & 1u) << 4;
        rm &= 0x1Fu;
        if (size == 3 || (size == 2 && (l || !q)) || (size == 1 && h && !q))
            return unsupported(c, pc, insn);
        index = size == 1 ? (h << 1) | l : h;
    }
    unsigned pairs = (q ? 128 : 64) / w / 2;
    uint32_t f = fpcr_for(c, w);
    uint64_t sb = sign_bit(w);
    rl_v128 n = c->v[rn], m = c->v[rm], d = c->v[rd], r;
    r.q = 0;
    for (unsigned e = 0; e < pairs; e++) {
        uint64_t m0 = lane_get(&m, w, mode == 2 ? index * 2 : e * 2);
        uint64_t m1 = lane_get(&m, w, mode == 2 ? index * 2 + 1 : e * 2 + 1);
        uint64_t n0 = lane_get(&n, w, e * 2), n1 = lane_get(&n, w, e * 2 + 1);
        uint64_t x0, x1;
        if (mode == 0) {
            uint64_t e1 = rot ? m1 : m1 ^ sb, e3 = rot ? m0 ^ sb : m0;
            x0 = rl_fpn_add(w, n0, e1, f, &c->fpsr);
            x1 = rl_fpn_add(w, n1, e3, f, &c->fpsr);
        } else {
            uint64_t e1, e2, e3, e4;
            switch (rot) {
            case 0: e1 = m0, e2 = n0, e3 = m1, e4 = n0; break;
            case 1: e1 = m1 ^ sb, e2 = n1, e3 = m0, e4 = n1; break;
            case 2: e1 = m0 ^ sb, e2 = n0, e3 = m1 ^ sb, e4 = n0; break;
            default: e1 = m1, e2 = n1, e3 = m0 ^ sb, e4 = n1; break;
            }
            x0 = rl_fpn_muladd(w, lane_get(&d, w, e * 2), e2, e1, f, &c->fpsr);
            x1 = rl_fpn_muladd(w, lane_get(&d, w, e * 2 + 1), e4, e3, f, &c->fpsr);
        }
        lane_set(&r, w, e * 2, x0);
        lane_set(&r, w, e * 2 + 1, x1);
    }
    c->v[rd] = r;
    return RL_OK;
}

int rl_fp_slow(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn)
{
    (void)mem;
    if ((insn & 0x7F000000u) == 0x1E000000u) {
        if (!(insn & 0x00200000u))
            return scalar_int(c, pc, insn, 1);
        if (((insn >> 10) & 63u) == 0)
            return scalar_int(c, pc, insn, 0);
        if (insn >> 31)
            return unsupported(c, pc, insn);
        if ((insn & 0x00007C00u) == 0x00004000u)
            return scalar_dp1(c, pc, insn);
        if ((insn & 0x00000C00u) == 0x00000800u)
            return scalar_dp2(c, pc, insn);
        if ((insn & 0x00000C00u) == 0x00000C00u)
            return scalar_sel(c, pc, insn);
        if ((insn & 0x00000C00u) == 0x00000400u)
            return scalar_ccmp(c, pc, insn);
        if ((insn & 0x00003C00u) == 0x00002000u)
            return scalar_cmp(c, pc, insn);
        if ((insn & 0x00001FE0u) == 0x00001000u)
            return scalar_imm(c, pc, insn);
        return unsupported(c, pc, insn);
    }
    if ((insn & 0xFF000000u) == 0x1F000000u)
        return scalar_dp3(c, pc, insn);
    if ((insn & 0x9F200400u) == 0x0E200400u)
        return simd_same(c, pc, insn, 0, 0);
    if ((insn & 0xDF200400u) == 0x5E200400u)
        return simd_same(c, pc, insn, 1, 0);
    if ((insn & 0x9F60C400u) == 0x0E400400u)
        return simd_same(c, pc, insn, 0, 1);
    if ((insn & 0xDF60C400u) == 0x5E400400u)
        return simd_same(c, pc, insn, 1, 1);
    if ((insn & 0x9F7E0C00u) == 0x0E780800u)
        return simd_misc(c, pc, insn, 0, 1);
    if ((insn & 0xDF7E0C00u) == 0x5E780800u)
        return simd_misc(c, pc, insn, 1, 1);
    if ((insn & 0x9F3E0C00u) == 0x0E200800u)
        return simd_misc(c, pc, insn, 0, 0);
    if ((insn & 0xDF3E0C00u) == 0x5E200800u)
        return simd_misc(c, pc, insn, 1, 0);
    if ((insn & 0xDF3E0C00u) == 0x5E300800u)
        return scalar_pair(c, pc, insn);
    if ((insn & 0x9F3E0C00u) == 0x0E300800u)
        return across(c, pc, insn);
    if ((insn & 0xBFE0FC00u) == 0x2EC0FC00u)
        return bfmlal(c, insn, 0);
    if ((insn & 0xBFC0F400u) == 0x0FC0F000u)
        return bfmlal(c, insn, 1);
    if ((insn & 0xBF20EC00u) == 0x2E00E400u)
        return simd_complex(c, pc, insn, 0);
    if ((insn & 0xBF20E400u) == 0x2E00C400u)
        return simd_complex(c, pc, insn, 1);
    if ((insn & 0xBF009400u) == 0x2F001000u)
        return simd_complex(c, pc, insn, 2);
    if ((insn & 0x9F800400u) == 0x0F000400u && (insn & 0x00780000u))
        return simd_shift(c, pc, insn, 0);
    if ((insn & 0xDF800400u) == 0x5F000400u && (insn & 0x00780000u))
        return simd_shift(c, pc, insn, 1);
    if ((insn & 0x9F000400u) == 0x0F000000u)
        return simd_elem(c, pc, insn, 0);
    if ((insn & 0xDF000400u) == 0x5F000000u)
        return simd_elem(c, pc, insn, 1);
    return unsupported(c, pc, insn);
}
