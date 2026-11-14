/**
 * @file fp_slow.c
 * @brief rl_fp_slow: decodes one A64 FP instruction and runs it through the
 * exact FP cut layer (fp.c) with the context's FPCR.
 */
#include "a64.h"

#include <relift/a64/fp_slow.h>

#include <string.h>

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MAX, OP_MIN, OP_MAXNM, OP_MINNM, OP_ABD };

static int unsupported(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    return rl_guest_fatal(c, "guest", "no FPCR slow path for instruction 0x%08x at guest pc 0x%llx (FPCR 0x%x)", insn,
                          (unsigned long long)pc, c->fpcr);
}

static uint64_t sign_bit(unsigned w) { return 1ull << (w - 1); }

static uint64_t wmask(unsigned w) { return w == 64 ? ~0ull : 0xFFFFFFFFull; }

static int is_nan(unsigned w, uint64_t x)
{
    return w == 32 ? (x & 0x7FFFFFFFu) > 0x7F800000u : (x & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull;
}

static int is_snan(unsigned w, uint64_t x)
{
    uint64_t q = w == 32 ? 0x00400000ull : 0x0008000000000000ull;
    return is_nan(w, x) && !(x & q);
}

static uint64_t flush_in(rl_ctx *c, unsigned w, uint64_t x)
{
    uint64_t emask = w == 32 ? 0x7F800000ull : 0x7FF0000000000000ull;
    uint64_t fmask = w == 32 ? 0x007FFFFFull : 0x000FFFFFFFFFFFFFull;
    if ((c->fpcr & RL_FPCR_FZ) && !(x & emask) && (x & fmask)) {
        c->fpsr |= 0x80u;
        return x & sign_bit(w);
    }
    return x;
}

static uint64_t arith(rl_ctx *c, unsigned w, int op, uint64_t a, uint64_t b)
{
    uint32_t f = c->fpcr, *s = &c->fpsr;
    if (w == 32) {
        uint32_t x = (uint32_t)a, y = (uint32_t)b;
        switch (op) {
        case OP_ADD: return rl_fp32_add(x, y, f, s);
        case OP_SUB: return rl_fp32_sub(x, y, f, s);
        case OP_MUL: return rl_fp32_mul(x, y, f, s);
        case OP_DIV: return rl_fp32_div(x, y, f, s);
        default: break;
        }
    } else {
        switch (op) {
        case OP_ADD: return rl_fp64_add(a, b, f, s);
        case OP_SUB: return rl_fp64_sub(a, b, f, s);
        case OP_MUL: return rl_fp64_mul(a, b, f, s);
        case OP_DIV: return rl_fp64_div(a, b, f, s);
        default: break;
        }
    }
    return 0;
}

static uint64_t fmuladd(rl_ctx *c, unsigned w, uint64_t addend, uint64_t a, uint64_t b)
{
    if (w == 32)
        return rl_fp32_muladd((uint32_t)addend, (uint32_t)a, (uint32_t)b, c->fpcr, &c->fpsr);
    return rl_fp64_muladd(addend, a, b, c->fpcr, &c->fpsr);
}

static uint64_t fsqrt(rl_ctx *c, unsigned w, uint64_t a)
{
    if (w == 32)
        return rl_fp32_sqrt((uint32_t)a, c->fpcr, &c->fpsr);
    return rl_fp64_sqrt(a, c->fpcr, &c->fpsr);
}

static uint64_t fround(rl_ctx *c, unsigned w, uint64_t a, uint32_t rmode, int exact)
{
    if (w == 32)
        return rl_fp32_round_int((uint32_t)a, rmode, exact, c->fpcr, &c->fpsr);
    return rl_fp64_round_int(a, rmode, exact, c->fpcr, &c->fpsr);
}

static int less_than(unsigned w, uint64_t a, uint64_t b)
{
    if (w == 32) {
        float x, y;
        uint32_t p = (uint32_t)a, q = (uint32_t)b;
        memcpy(&x, &p, 4);
        memcpy(&y, &q, 4);
        return x < y;
    }
    double x, y;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    return x < y;
}

static uint64_t inf_bits(unsigned w, unsigned neg)
{
    return (w == 32 ? 0x7F800000ull : 0x7FF0000000000000ull) | (neg ? sign_bit(w) : 0);
}

static uint64_t minmax(rl_ctx *c, unsigned w, int op, uint64_t a, uint64_t b)
{
    int is_max = op == OP_MAX || op == OP_MAXNM;
    int num = op == OP_MAXNM || op == OP_MINNM;
    if (num) {
        uint64_t qa = is_nan(w, a) && !is_snan(w, a), qb = is_nan(w, b) && !is_snan(w, b);
        if (qa && !is_nan(w, b))
            a = inf_bits(w, is_max);
        else if (qb && !is_nan(w, a))
            b = inf_bits(w, is_max);
    }
    if (is_nan(w, a) || is_nan(w, b))
        return arith(c, w, OP_ADD, a, b);
    a = flush_in(c, w, a);
    b = flush_in(c, w, b);
    if (!((a | b) & ~sign_bit(w) & wmask(w)))
        return is_max ? (a & b) : (a | b);
    if (is_max)
        return less_than(w, a, b) ? b : a;
    return less_than(w, b, a) ? b : a;
}

static uint64_t binop(rl_ctx *c, unsigned w, int op, uint64_t a, uint64_t b)
{
    switch (op) {
    case OP_MAX:
    case OP_MIN:
    case OP_MAXNM:
    case OP_MINNM: return minmax(c, w, op, a, b);
    case OP_ABD: return arith(c, w, OP_SUB, a, b) & ~sign_bit(w);
    default: return arith(c, w, op, a, b);
    }
}

static uint64_t read_fp(const rl_ctx *c, unsigned w, unsigned r)
{
    return w == 32 ? c->v[r].s[0] : c->v[r].d[0];
}

static void write_fp(rl_ctx *c, unsigned w, unsigned r, uint64_t val)
{
    c->v[r].q = 0;
    if (w == 32)
        c->v[r].s[0] = (uint32_t)val;
    else if (w == 64)
        c->v[r].d[0] = val;
    else
        c->v[r].h[0] = (uint16_t)val;
}

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

static uint32_t compare(rl_ctx *c, unsigned w, uint64_t a, uint64_t b, int signaling)
{
    if (is_nan(w, a) || is_nan(w, b)) {
        if (signaling || is_snan(w, a) || is_snan(w, b))
            c->fpsr |= 0x01u;
        return 0x30000000u;
    }
    a = flush_in(c, w, a);
    b = flush_in(c, w, b);
    if (less_than(w, a, b))
        return 0x80000000u;
    if (less_than(w, b, a))
        return 0x20000000u;
    return 0x60000000u;
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

static uint64_t expand_imm(unsigned w, uint32_t imm8)
{
    uint64_t sign = (imm8 >> 7) & 1u, b = (imm8 >> 6) & 1u, rest = imm8 & 0x3Fu;
    if (w == 32)
        return (sign << 31) | ((b ^ 1u) << 30) | (b ? 0x1Full << 25 : 0) | (rest << 19);
    return (sign << 63) | ((b ^ 1u) << 62) | (b ? 0xFFull << 54 : 0) | (rest << 48);
}

static int scalar_dp1(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned type = (insn >> 22) & 3u, op = (insn >> 15) & 0x3Fu, rn = (insn >> 5) & 31u, rd = insn & 31u;
    unsigned w = type_bits(type);
    if (op >= 0x04 && op <= 0x07 && (op & 3u) != 2u) {
        unsigned to = type_bits(op & 3u);
        if (!w || !to || to == w)
            return unsupported(c, pc, insn);
        uint64_t src = w == 16 ? c->v[rn].h[0] : read_fp(c, w, rn);
        write_fp(c, to, rd, rl_fp_convert(src, w, to, RL_RM_FPCR, c->fpcr, &c->fpsr));
        return RL_OK;
    }
    if (w != 32 && w != 64)
        return unsupported(c, pc, insn);
    uint64_t a = read_fp(c, w, rn), r;
    switch (op) {
    case 0x00: r = a; break;
    case 0x01: r = a & ~sign_bit(w); break;
    case 0x02: r = a ^ sign_bit(w); break;
    case 0x03: r = fsqrt(c, w, a); break;
    case 0x08: r = fround(c, w, a, RL_RM_NEAREST, 0); break;
    case 0x09: r = fround(c, w, a, RL_RM_POSINF, 0); break;
    case 0x0A: r = fround(c, w, a, RL_RM_NEGINF, 0); break;
    case 0x0B: r = fround(c, w, a, RL_RM_ZERO, 0); break;
    case 0x0C: r = fround(c, w, a, RL_RM_AWAY, 0); break;
    case 0x0E: r = fround(c, w, a, RL_RM_FPCR, 1); break;
    case 0x0F: r = fround(c, w, a, RL_RM_FPCR, 0); break;
    default: return unsupported(c, pc, insn);
    }
    write_fp(c, w, rd, r);
    return RL_OK;
}

static int scalar_dp2(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u), op = (insn >> 12) & 15u;
    unsigned rm = (insn >> 16) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    static const int ops[9] = { OP_MUL, OP_DIV, OP_ADD, OP_SUB, OP_MAX, OP_MIN, OP_MAXNM, OP_MINNM, OP_MUL };
    if ((w != 32 && w != 64) || op > 8)
        return unsupported(c, pc, insn);
    uint64_t r = binop(c, w, ops[op], read_fp(c, w, rn), read_fp(c, w, rm));
    if (op == 8)
        r ^= sign_bit(w);
    write_fp(c, w, rd, r);
    return RL_OK;
}

static int scalar_dp3(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u), o1 = (insn >> 21) & 1u, o0 = (insn >> 15) & 1u;
    unsigned rm = (insn >> 16) & 31u, ra = (insn >> 10) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    if (w != 32 && w != 64)
        return unsupported(c, pc, insn);
    uint64_t a = read_fp(c, w, ra), n = read_fp(c, w, rn), m = read_fp(c, w, rm);
    if (o1)
        a ^= sign_bit(w);
    if (o0 != o1)
        n ^= sign_bit(w);
    write_fp(c, w, rd, fmuladd(c, w, a, n, m));
    return RL_OK;
}

static int scalar_cmp(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u), rm = (insn >> 16) & 31u, rn = (insn >> 5) & 31u;
    unsigned opc2 = insn & 31u;
    if ((w != 32 && w != 64) || (opc2 & 7u) || ((insn >> 14) & 3u))
        return unsupported(c, pc, insn);
    uint64_t b = (opc2 & 8u) ? 0 : read_fp(c, w, rm);
    c->nzcv = compare(c, w, read_fp(c, w, rn), b, (opc2 & 16u) != 0);
    return RL_OK;
}

static int scalar_ccmp(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u), rm = (insn >> 16) & 31u, rn = (insn >> 5) & 31u;
    unsigned cond = (insn >> 12) & 15u;
    if (w != 32 && w != 64)
        return unsupported(c, pc, insn);
    if (cond_holds(c->nzcv, cond))
        c->nzcv = compare(c, w, read_fp(c, w, rn), read_fp(c, w, rm), (insn >> 4) & 1u);
    else
        c->nzcv = (insn & 15u) << 28;
    return RL_OK;
}

static int scalar_sel(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u), rm = (insn >> 16) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    if (w != 32 && w != 64)
        return unsupported(c, pc, insn);
    write_fp(c, w, rd, read_fp(c, w, cond_holds(c->nzcv, (insn >> 12) & 15u) ? rn : rm));
    return RL_OK;
}

static int scalar_imm(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned w = type_bits((insn >> 22) & 3u);
    if (w != 32 && w != 64)
        return unsupported(c, pc, insn);
    write_fp(c, w, insn & 31u, expand_imm(w, (insn >> 13) & 0xFFu));
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
        if (opcode == 6) {
            write_gpr(c, rd, c->v[rn].d[1]);
        } else {
            c->v[rd].d[1] = read_gpr(c, rn);
        }
        return RL_OK;
    }
    if (w != 32 && w != 64)
        return unsupported(c, pc, insn);
    if (!fixed && rmode == 0 && (opcode == 6 || opcode == 7)) {
        if (w != ibits)
            return unsupported(c, pc, insn);
        if (opcode == 6)
            write_gpr(c, rd, read_fp(c, w, rn));
        else
            write_fp(c, w, rd, read_gpr(c, rn) & wmask(w));
        return RL_OK;
    }
    if (rmode == 0 && (opcode == 2 || opcode == 3)) {
        uint64_t a = read_gpr(c, rn) & wmask(ibits);
        write_fp(c, w, rd, rl_fixed_to_fp(a, ibits, w, fbits, opcode == 3, RL_RM_FPCR, c->fpcr, &c->fpsr));
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
    uint64_t r = rl_fp_to_fixed(read_fp(c, w, rn), w, ibits, fbits, opcode & 1u, rm, c->fpcr, &c->fpsr);
    write_gpr(c, rd, r & wmask(ibits));
    return RL_OK;
}

static uint64_t lane_get(const rl_v128 *v, unsigned w, unsigned i) { return w == 32 ? v->s[i] : v->d[i]; }

static void lane_set(rl_v128 *v, unsigned w, unsigned i, uint64_t x)
{
    if (w == 32)
        v->s[i] = (uint32_t)x;
    else
        v->d[i] = x;
}

static int vec_same(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1u, u = (insn >> 29) & 1u, hi = (insn >> 23) & 1u, sz = (insn >> 22) & 1u;
    unsigned opc = (insn >> 11) & 31u, rm = (insn >> 16) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    unsigned w = sz ? 64 : 32, lanes = (q ? 128 : 64) / w;
    int op = -1, pair = 0, fma = 0;
    if (sz && !q)
        return unsupported(c, pc, insn);
    switch ((u << 6) | (hi << 5) | opc) {
    case 0x1A: op = OP_ADD; break;
    case 0x3A: op = OP_SUB; break;
    case 0x5A: op = OP_ADD; pair = 1; break;
    case 0x7A: op = OP_ABD; break;
    case 0x5B: op = OP_MUL; break;
    case 0x5F: op = OP_DIV; break;
    case 0x19: fma = 1; break;
    case 0x39: fma = 2; break;
    case 0x1E: op = OP_MAX; break;
    case 0x3E: op = OP_MIN; break;
    case 0x5E: op = OP_MAX; pair = 1; break;
    case 0x7E: op = OP_MIN; pair = 1; break;
    case 0x18: op = OP_MAXNM; break;
    case 0x38: op = OP_MINNM; break;
    case 0x58: op = OP_MAXNM; pair = 1; break;
    case 0x78: op = OP_MINNM; pair = 1; break;
    default: return unsupported(c, pc, insn);
    }
    rl_v128 n = c->v[rn], m = c->v[rm], d = c->v[rd], r;
    r.q = 0;
    for (unsigned i = 0; i < lanes; i++) {
        uint64_t x;
        if (pair) {
            unsigned j = 2 * i;
            uint64_t a = j < lanes ? lane_get(&n, w, j) : lane_get(&m, w, j - lanes);
            uint64_t b = j + 1 < lanes ? lane_get(&n, w, j + 1) : lane_get(&m, w, j + 1 - lanes);
            x = binop(c, w, op, a, b);
        } else if (fma) {
            uint64_t a = lane_get(&n, w, i);
            if (fma == 2)
                a ^= sign_bit(w);
            x = fmuladd(c, w, lane_get(&d, w, i), a, lane_get(&m, w, i));
        } else {
            x = binop(c, w, op, lane_get(&n, w, i), lane_get(&m, w, i));
        }
        lane_set(&r, w, i, x);
    }
    c->v[rd] = r;
    return RL_OK;
}

enum { M_NONE, M_CVT, M_SCVTF, M_ABS, M_NEG, M_SQRT, M_RINT };

static int vec_misc(rl_ctx *c, uint64_t pc, uint32_t insn)
{
    unsigned q = (insn >> 30) & 1u, u = (insn >> 29) & 1u, hi = (insn >> 23) & 1u, sz = (insn >> 22) & 1u;
    unsigned opc = (insn >> 12) & 31u, rn = (insn >> 5) & 31u, rd = insn & 31u;
    unsigned w = sz ? 64 : 32, lanes = (q ? 128 : 64) / w;
    int kind = M_NONE, exact = 0;
    uint32_t rm = RL_RM_NEAREST;
    if (sz && !q)
        return unsupported(c, pc, insn);
    switch ((u << 6) | (hi << 5) | opc) {
    case 0x1A: kind = M_CVT; rm = RL_RM_NEAREST; break;
    case 0x3A: kind = M_CVT; rm = RL_RM_POSINF; break;
    case 0x1B: kind = M_CVT; rm = RL_RM_NEGINF; break;
    case 0x3B: kind = M_CVT; rm = RL_RM_ZERO; break;
    case 0x1C: kind = M_CVT; rm = RL_RM_AWAY; break;
    case 0x5A: kind = M_CVT; rm = RL_RM_NEAREST; break;
    case 0x7A: kind = M_CVT; rm = RL_RM_POSINF; break;
    case 0x5B: kind = M_CVT; rm = RL_RM_NEGINF; break;
    case 0x7B: kind = M_CVT; rm = RL_RM_ZERO; break;
    case 0x5C: kind = M_CVT; rm = RL_RM_AWAY; break;
    case 0x1D:
    case 0x5D: kind = M_SCVTF; break;
    case 0x2F: kind = M_ABS; break;
    case 0x6F: kind = M_NEG; break;
    case 0x7F: kind = M_SQRT; break;
    case 0x18: kind = M_RINT; rm = RL_RM_NEAREST; break;
    case 0x38: kind = M_RINT; rm = RL_RM_POSINF; break;
    case 0x19: kind = M_RINT; rm = RL_RM_NEGINF; break;
    case 0x39: kind = M_RINT; rm = RL_RM_ZERO; break;
    case 0x58: kind = M_RINT; rm = RL_RM_AWAY; break;
    case 0x59: kind = M_RINT; rm = RL_RM_FPCR; exact = 1; break;
    case 0x79: kind = M_RINT; rm = RL_RM_FPCR; break;
    default: return unsupported(c, pc, insn);
    }
    rl_v128 n = c->v[rn], r;
    r.q = 0;
    for (unsigned i = 0; i < lanes; i++) {
        uint64_t a = lane_get(&n, w, i), x = 0;
        switch (kind) {
        case M_CVT: x = rl_fp_to_fixed(a, w, w, 0, u, rm, c->fpcr, &c->fpsr); break;
        case M_SCVTF: x = rl_fixed_to_fp(a, w, w, 0, u, RL_RM_FPCR, c->fpcr, &c->fpsr); break;
        case M_ABS: x = a & ~sign_bit(w); break;
        case M_NEG: x = a ^ sign_bit(w); break;
        case M_SQRT: x = fsqrt(c, w, a); break;
        default: x = fround(c, w, a, rm, exact); break;
        }
        lane_set(&r, w, i, x);
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
        return vec_same(c, pc, insn);
    if ((insn & 0x9F3E0C00u) == 0x0E200800u)
        return vec_misc(c, pc, insn);
    return unsupported(c, pc, insn);
}
