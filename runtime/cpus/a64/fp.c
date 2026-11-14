/**
 * @file fp.c
 * @brief The exact slow paths of the FP cut layer (docs/abi.md section 10.1):
 * FPAdd, FPSub, FPMul, FPDiv, FPMulAdd, FPSqrt, FPRoundInt, FPRSqrtEstimate,
 * FPConvert, FPToFixed and FixedToFP with Arm's NaN rules, rounding modes,
 * FZ, FZ16, DN, AHP and FPSR flags.
 *
 * The arithmetic is integer only (128-bit intermediates), so results do not
 * depend on the host FPU, its rounding mode or its NaN conventions. The
 * rules follow the Arm Architecture Reference Manual pseudocode (FPUnpack,
 * FPProcessNaNs, FPRound, FPMulAdd, RecipSqrtEstimate); tininess is detected
 * before rounding, as Arm does.
 */
#include <relift/a64/abi.h>

#include "fp.h"

#define FPSR_IOC 0x01u
#define FPSR_DZC 0x02u
#define FPSR_OFC 0x04u
#define FPSR_UFC 0x08u
#define FPSR_IXC 0x10u
#define FPSR_IDC 0x80u
#define FPCR_FZ16 (1u << 19)

/** @brief An IEEE binary format. */
typedef struct fmt {
    unsigned bits;
    unsigned ebits;
    unsigned fbits;
    int bias;
} fmt;

static const fmt F16 = { 16, 5, 10, 15 };
static const fmt F32 = { 32, 8, 23, 127 };
static const fmt F64 = { 64, 11, 52, 1023 };

static const fmt *fmt_of(unsigned bits)
{
    switch (bits) {
    case 16: return &F16;
    case 32: return &F32;
    case 64: return &F64;
    default: return NULL;
    }
}

/** @brief The FPCR of one operation and the flags it raises. */
typedef struct fpenv {
    uint32_t fpcr;
    uint32_t flags;
    int dst_ahp;
} fpenv;

typedef enum kind { K_ZERO, K_NORM, K_INF, K_QNAN, K_SNAN } kind;

/** @brief An unpacked operand: a normal value is sig * 2^e with sig normalized to bit 63. */
typedef struct unp {
    kind k;
    unsigned sign;
    uint64_t sig;
    int e;
    uint64_t bits;
} unp;

static int fz_on(const fpenv *v, const fmt *f)
{
    return (v->fpcr & (f->bits == 16 ? FPCR_FZ16 : RL_FPCR_FZ)) != 0;
}

static int ahp_on(const fpenv *v, const fmt *f) { return f->bits == 16 && (v->fpcr & RL_FPCR_AHP) != 0; }

static unsigned rm_fpcr(const fpenv *v) { return (v->fpcr >> 22) & 3u; }

static unsigned rm_arg(const fpenv *v, uint32_t r)
{
    if (r == RL_RM_FPCR)
        return rm_fpcr(v);
    return r > RL_RM_AWAY ? (unsigned)RL_RM_NEAREST : r;
}

static unsigned msb128(rl_u128 x)
{
    uint64_t hi = (uint64_t)(x >> 64);
    if (hi)
        return 127u - (unsigned)__builtin_clzll(hi);
    return 63u - (unsigned)__builtin_clzll((uint64_t)x);
}

static unp unpack(fpenv *v, const fmt *f, uint64_t bits)
{
    unp u;
    uint64_t emask = (1ull << f->ebits) - 1, fmask = (1ull << f->fbits) - 1;
    uint64_t ex = (bits >> f->fbits) & emask, fr = bits & fmask;
    u.k = K_ZERO;
    u.sign = (unsigned)((bits >> (f->bits - 1)) & 1u);
    u.sig = 0;
    u.e = 0;
    u.bits = bits;
    if (ex == emask && !ahp_on(v, f)) {
        u.k = fr == 0 ? K_INF : (((fr >> (f->fbits - 1)) & 1u) ? K_QNAN : K_SNAN);
        return u;
    }
    if (ex == 0) {
        if (fr == 0)
            return u;
        if (fz_on(v, f)) {
            v->flags |= FPSR_IDC;
            return u;
        }
        u.sig = fr;
        u.e = 1 - f->bias - (int)f->fbits;
    } else {
        u.sig = fr | (1ull << f->fbits);
        u.e = (int)ex - f->bias - (int)f->fbits;
    }
    unsigned sh = (unsigned)__builtin_clzll(u.sig);
    u.sig <<= sh;
    u.e -= (int)sh;
    u.k = K_NORM;
    return u;
}

static uint64_t make_zero(const fmt *f, unsigned sign) { return (uint64_t)sign << (f->bits - 1); }

static uint64_t make_inf(const fmt *f, unsigned sign)
{
    return ((uint64_t)sign << (f->bits - 1)) | (((1ull << f->ebits) - 1) << f->fbits);
}

static uint64_t default_nan(const fmt *f) { return make_inf(f, 0) | (1ull << (f->fbits - 1)); }

static uint64_t invalid_nan(fpenv *v, const fmt *f)
{
    v->flags |= FPSR_IOC;
    return default_nan(f);
}

static int is_nan(const unp *u) { return u->k == K_QNAN || u->k == K_SNAN; }

static uint64_t nan_result(fpenv *v, const fmt *f, const unp *u)
{
    if (u->k == K_SNAN)
        v->flags |= FPSR_IOC;
    if (v->fpcr & RL_FPCR_DN)
        return default_nan(f);
    return u->bits | (1ull << (f->fbits - 1));
}

/** @brief FPProcessNaNs: the first signaling NaN, else the first quiet NaN, in argument order. */
static int process_nans(fpenv *v, const fmt *f, const unp *const *ops, unsigned n, uint64_t *res)
{
    for (unsigned i = 0; i < n; i++)
        if (ops[i]->k == K_SNAN) {
            *res = nan_result(v, f, ops[i]);
            return 1;
        }
    for (unsigned i = 0; i < n; i++)
        if (ops[i]->k == K_QNAN) {
            *res = nan_result(v, f, ops[i]);
            return 1;
        }
    return 0;
}

/** @brief Shifts m right by n bits and rounds the dropped bits per rm; *inexact is set when any were nonzero. */
static rl_u128 round_shift(rl_u128 m, unsigned n, unsigned sign, unsigned rm, int *inexact)
{
    rl_u128 q, rem;
    int cmp;
    int up;
    if (n >= 128) {
        q = 0;
        rem = m;
    } else {
        q = m >> n;
        rem = m & (((rl_u128)1 << n) - 1);
    }
    *inexact = rem != 0;
    if (!rem)
        return q;
    if (n > 128) {
        cmp = -1;
    } else {
        rl_u128 half = (rl_u128)1 << (n - 1);
        cmp = rem > half ? 1 : (rem == half ? 0 : -1);
    }
    switch (rm) {
    case RL_RM_NEAREST: up = cmp > 0 || (cmp == 0 && (q & 1)); break;
    case RL_RM_POSINF: up = !sign; break;
    case RL_RM_NEGINF: up = sign; break;
    case RL_RM_AWAY: up = cmp >= 0; break;
    default: up = 0; break;
    }
    return q + (rl_u128)(up ? 1 : 0);
}

/** @brief FPRound: packs sign * m * 2^q (m nonzero, exact up to its own low bits) into format f. */
static uint64_t round_pack(fpenv *v, const fmt *f, unsigned sign, rl_u128 m, int q, unsigned rm)
{
    int emin = 1 - f->bias;
    int e = q + (int)msb128(m);
    int tiny = e < emin;
    int lsb = (tiny ? emin : e) - (int)f->fbits;
    int drop = lsb - q;
    int inexact = 0;
    rl_u128 kept;
    uint64_t r, inf_bits;
    if (tiny && fz_on(v, f)) {
        v->flags |= FPSR_UFC;
        return make_zero(f, sign);
    }
    if (drop <= 0)
        kept = m << (unsigned)(-drop);
    else
        kept = round_shift(m, (unsigned)drop, sign, rm, &inexact);
    if (inexact && tiny)
        v->flags |= FPSR_UFC;
    r = (tiny ? 0ull : (uint64_t)(e + f->bias - 1) << f->fbits) + (uint64_t)kept;
    if (v->dst_ahp && f->bits == 16) {
        if (r >= 0x8000u) {
            v->flags |= FPSR_IOC;
            return ((uint64_t)sign << 15) | 0x7FFFu;
        }
    } else {
        inf_bits = ((1ull << f->ebits) - 1) << f->fbits;
        if (r >= inf_bits) {
            int to_inf = rm == RL_RM_NEAREST || rm == RL_RM_AWAY || (rm == RL_RM_POSINF && !sign) ||
                         (rm == RL_RM_NEGINF && sign);
            v->flags |= FPSR_OFC | FPSR_IXC;
            return ((uint64_t)sign << (f->bits - 1)) | (to_inf ? inf_bits : inf_bits - 1);
        }
    }
    if (inexact)
        v->flags |= FPSR_IXC;
    return ((uint64_t)sign << (f->bits - 1)) | r;
}

static uint64_t pack_unp(fpenv *v, const fmt *f, const unp *u, unsigned sign)
{
    return round_pack(v, f, sign, u->sig, u->e, rm_fpcr(v));
}

static rl_u128 shr_jam(rl_u128 m, unsigned n)
{
    if (n == 0)
        return m;
    if (n >= 128)
        return m != 0;
    return (m >> n) | ((m & (((rl_u128)1 << n) - 1)) != 0);
}

/** @brief Scales m * 2^q so that m has its top bit at bit 126 (a lowest bit lost is jammed into the sticky bit). */
static void norm126(rl_u128 *m, int *q)
{
    unsigned p = msb128(*m);
    if (p > 126u) {
        *m = shr_jam(*m, 1);
        *q += 1;
    } else {
        *m <<= 126u - p;
        *q -= (int)(126u - p);
    }
}

/** @brief Adds two nonzero values given as m * 2^q with signs; the result is rounded and packed. */
static uint64_t sum_pack(fpenv *v, const fmt *f, unsigned sa, rl_u128 ma, int qa, unsigned sb, rl_u128 mb, int qb)
{
    unsigned rm = rm_fpcr(v);
    rl_u128 s;
    unsigned sign;
    norm126(&ma, &qa);
    norm126(&mb, &qb);
    if (qa < qb) {
        rl_u128 tm = ma;
        int tq = qa;
        unsigned ts = sa;
        ma = mb;
        qa = qb;
        sa = sb;
        mb = tm;
        qb = tq;
        sb = ts;
    }
    mb = shr_jam(mb, (unsigned)(qa - qb));
    if (sa == sb) {
        s = ma + mb;
        sign = sa;
    } else if (ma >= mb) {
        s = ma - mb;
        sign = sa;
    } else {
        s = mb - ma;
        sign = sb;
    }
    if (s == 0)
        return make_zero(f, rm == RL_RM_NEGINF);
    return round_pack(v, f, sign, s, qa, rm);
}

static uint64_t fp_addsub(fpenv *v, const fmt *f, uint64_t a, uint64_t b, unsigned negate_b)
{
    unp ua = unpack(v, f, a), ub = unpack(v, f, b);
    const unp *ops[2] = { &ua, &ub };
    uint64_t res;
    unsigned sb = ub.sign ^ negate_b;
    if (process_nans(v, f, ops, 2, &res))
        return res;
    if (ua.k == K_INF || ub.k == K_INF) {
        if (ua.k == K_INF && ub.k == K_INF && ua.sign != sb)
            return invalid_nan(v, f);
        return make_inf(f, ua.k == K_INF ? ua.sign : sb);
    }
    if (ua.k == K_ZERO && ub.k == K_ZERO)
        return make_zero(f, ua.sign == sb ? ua.sign : rm_fpcr(v) == RL_RM_NEGINF);
    if (ua.k == K_ZERO)
        return pack_unp(v, f, &ub, sb);
    if (ub.k == K_ZERO)
        return pack_unp(v, f, &ua, ua.sign);
    return sum_pack(v, f, ua.sign, ua.sig, ua.e, sb, ub.sig, ub.e);
}

static uint64_t fp_mul(fpenv *v, const fmt *f, uint64_t a, uint64_t b)
{
    unp ua = unpack(v, f, a), ub = unpack(v, f, b);
    const unp *ops[2] = { &ua, &ub };
    uint64_t res;
    unsigned sign = ua.sign ^ ub.sign;
    if (process_nans(v, f, ops, 2, &res))
        return res;
    if ((ua.k == K_INF && ub.k == K_ZERO) || (ua.k == K_ZERO && ub.k == K_INF))
        return invalid_nan(v, f);
    if (ua.k == K_INF || ub.k == K_INF)
        return make_inf(f, sign);
    if (ua.k == K_ZERO || ub.k == K_ZERO)
        return make_zero(f, sign);
    return round_pack(v, f, sign, (rl_u128)ua.sig * ub.sig, ua.e + ub.e, rm_fpcr(v));
}

static uint64_t fp_div(fpenv *v, const fmt *f, uint64_t a, uint64_t b)
{
    unp ua = unpack(v, f, a), ub = unpack(v, f, b);
    const unp *ops[2] = { &ua, &ub };
    uint64_t res;
    unsigned sign = ua.sign ^ ub.sign;
    if (process_nans(v, f, ops, 2, &res))
        return res;
    if ((ua.k == K_INF && ub.k == K_INF) || (ua.k == K_ZERO && ub.k == K_ZERO))
        return invalid_nan(v, f);
    if (ua.k == K_INF || ub.k == K_ZERO) {
        if (ua.k != K_INF)
            v->flags |= FPSR_DZC;
        return make_inf(f, sign);
    }
    if (ua.k == K_ZERO || ub.k == K_INF)
        return make_zero(f, sign);
    rl_u128 num = (rl_u128)ua.sig << 64;
    rl_u128 quo = num / ub.sig;
    rl_u128 rem = num % ub.sig;
    return round_pack(v, f, sign, (quo << 1) | (rem != 0), ua.e - ub.e - 65, rm_fpcr(v));
}

static uint64_t fp_muladd(fpenv *v, const fmt *f, uint64_t addend, uint64_t a, uint64_t b)
{
    unp uc = unpack(v, f, addend), ua = unpack(v, f, a), ub = unpack(v, f, b);
    const unp *ops[3] = { &uc, &ua, &ub };
    uint64_t res;
    unsigned sp = ua.sign ^ ub.sign;
    int inf_p = ua.k == K_INF || ub.k == K_INF;
    int zero_p = ua.k == K_ZERO || ub.k == K_ZERO;
    int bad_p = (ua.k == K_INF && ub.k == K_ZERO) || (ua.k == K_ZERO && ub.k == K_INF);
    if (uc.k == K_QNAN && bad_p)
        return invalid_nan(v, f);
    if (process_nans(v, f, ops, 3, &res))
        return res;
    if (bad_p || (uc.k == K_INF && inf_p && uc.sign != sp))
        return invalid_nan(v, f);
    if (uc.k == K_INF)
        return make_inf(f, uc.sign);
    if (inf_p)
        return make_inf(f, sp);
    if (zero_p && uc.k == K_ZERO)
        return make_zero(f, sp == uc.sign ? sp : rm_fpcr(v) == RL_RM_NEGINF);
    if (zero_p)
        return pack_unp(v, f, &uc, uc.sign);
    rl_u128 prod = (rl_u128)ua.sig * ub.sig;
    if (uc.k == K_ZERO)
        return round_pack(v, f, sp, prod, ua.e + ub.e, rm_fpcr(v));
    return sum_pack(v, f, sp, prod, ua.e + ub.e, uc.sign, uc.sig, uc.e);
}

static rl_u128 isqrt128(rl_u128 n)
{
    rl_u128 res = 0, bit = (rl_u128)1 << 126;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= res + bit) {
            n -= res + bit;
            res = (res >> 1) + bit;
        } else {
            res >>= 1;
        }
        bit >>= 2;
    }
    return res;
}

static uint64_t fp_sqrt(fpenv *v, const fmt *f, uint64_t a)
{
    unp u = unpack(v, f, a);
    uint64_t res;
    const unp *ops[1] = { &u };
    if (process_nans(v, f, ops, 1, &res))
        return res;
    if (u.k == K_ZERO)
        return make_zero(f, u.sign);
    if (u.sign)
        return invalid_nan(v, f);
    if (u.k == K_INF)
        return make_inf(f, 0);
    rl_u128 n;
    int ex;
    if ((u.e & 1) == 0) {
        n = (rl_u128)u.sig << 62;
        ex = u.e - 62;
    } else {
        n = (rl_u128)u.sig << 63;
        ex = u.e - 63;
    }
    rl_u128 r = isqrt128(n);
    int inexact = n != r * r;
    return round_pack(v, f, 0, (r << 1) | (rl_u128)inexact, ex / 2 - 1, rm_fpcr(v));
}

static uint64_t fp_round_int(fpenv *v, const fmt *f, uint64_t a, uint32_t rmode, int exact)
{
    unp u = unpack(v, f, a);
    uint64_t res;
    const unp *ops[1] = { &u };
    unsigned rm = rm_arg(v, rmode);
    int inexact;
    if (process_nans(v, f, ops, 1, &res))
        return res;
    if (u.k == K_INF)
        return a;
    if (u.k == K_ZERO)
        return make_zero(f, u.sign);
    if (u.e >= 0)
        return pack_unp(v, f, &u, u.sign);
    rl_u128 m = round_shift(u.sig, (unsigned)(-u.e), u.sign, rm, &inexact);
    if (exact && inexact)
        v->flags |= FPSR_IXC;
    if (m == 0)
        return make_zero(f, u.sign);
    return round_pack(v, f, u.sign, m, 0, RL_RM_NEAREST);
}

static unsigned recip_sqrt_estimate(unsigned a)
{
    uint64_t b = 512, aa;
    if (a < 256) {
        aa = (uint64_t)a * 2 + 1;
    } else {
        aa = (uint64_t)((a >> 1) << 1);
        aa = (aa + 1) * 2;
    }
    while (aa * (b + 1) * (b + 1) < (1ull << 28))
        b++;
    return (unsigned)((b + 1) / 2);
}

static uint64_t fp_rsqrt_est(fpenv *v, const fmt *f, uint64_t a)
{
    unp u = unpack(v, f, a);
    uint64_t res, frac;
    const unp *ops[1] = { &u };
    int ex;
    unsigned scaled, est;
    if (process_nans(v, f, ops, 1, &res))
        return res;
    if (u.k == K_ZERO) {
        v->flags |= FPSR_DZC;
        return make_inf(f, u.sign);
    }
    if (u.sign)
        return invalid_nan(v, f);
    if (u.k == K_INF)
        return 0;
    if (f->bits == 32) {
        frac = (a & 0x7FFFFFull) << 29;
        ex = (int)((a >> 23) & 0xFF);
    } else {
        frac = a & 0xFFFFFFFFFFFFFull;
        ex = (int)((a >> 52) & 0x7FF);
    }
    if (ex == 0) {
        while (((frac >> 51) & 1u) == 0) {
            frac = (frac << 1) & 0xFFFFFFFFFFFFFull;
            ex--;
        }
        frac = (frac << 1) & 0xFFFFFFFFFFFFFull;
    }
    if ((ex & 1) == 0)
        scaled = 256u | (unsigned)((frac >> 44) & 0xFFu);
    else
        scaled = 128u | (unsigned)((frac >> 45) & 0x7Fu);
    est = recip_sqrt_estimate(scaled);
    if (f->bits == 32)
        return ((uint64_t)(((380 - ex) / 2) & 0xFF) << 23) | ((uint64_t)(est & 0xFFu) << 15);
    return ((uint64_t)(((3068 - ex) / 2) & 0x7FF) << 52) | ((uint64_t)(est & 0xFFu) << 44);
}

/** @brief RecipEstimate: a 9-bit estimate of 1/a for a in [256, 512). */
static unsigned recip_estimate(unsigned a)
{
    unsigned b = (1u << 19) / (a * 2 + 1);
    return (b + 1) / 2;
}

static uint64_t make_max_normal(const fmt *f, unsigned sign)
{
    return make_inf(f, sign) - 1;
}

/** @brief FPRecipEstimate (FRECPE). */
static uint64_t fp_recip_est(fpenv *v, const fmt *f, uint64_t a)
{
    unp u = unpack(v, f, a);
    uint64_t res;
    const unp *ops[1] = { &u };
    if (process_nans(v, f, ops, 1, &res))
        return res;
    if (u.k == K_INF)
        return make_zero(f, u.sign);
    if (u.k == K_ZERO) {
        v->flags |= FPSR_DZC;
        return make_inf(f, u.sign);
    }
    /* The value's exponent with the significand in [1, 2). */
    int vexp = u.e + 63;
    if (vexp < -(f->bias + 1)) {
        unsigned rm = rm_fpcr(v);
        int to_inf = rm == RL_RM_NEAREST || (rm == RL_RM_POSINF && !u.sign) || (rm == RL_RM_NEGINF && u.sign);
        v->flags |= FPSR_OFC | FPSR_IXC;
        return to_inf ? make_inf(f, u.sign) : make_max_normal(f, u.sign);
    }
    if (fz_on(v, f) && vexp >= f->bias - 1) {
        v->flags |= FPSR_UFC;
        return make_zero(f, u.sign);
    }
    /* The operand as a 52-bit fraction and biased exponent, as the
     * pseudocode scales it. */
    uint64_t frac;
    int ex;
    uint64_t fmask = (1ull << f->fbits) - 1;
    frac = (a & fmask) << (52 - f->fbits);
    ex = (int)((a >> f->fbits) & ((1ull << f->ebits) - 1));
    if (ex == 0) {
        if (((frac >> 51) & 1u) == 0) {
            ex = -1;
            frac = (frac & ((1ull << 50) - 1)) << 2;
        } else {
            frac = (frac & ((1ull << 51) - 1)) << 1;
        }
    }
    unsigned scaled = 256u | (unsigned)((frac >> 44) & 0xFFu);
    int rexp = 2 * f->bias - 1 - ex;
    unsigned est = recip_estimate(scaled);
    uint64_t rfrac = (uint64_t)(est & 0xFFu) << 44;
    if (rexp == 0) {
        rfrac = (1ull << 51) | (rfrac >> 1);
    } else if (rexp == -1) {
        rfrac = (1ull << 50) | (rfrac >> 2);
        rexp = 0;
    }
    return ((uint64_t)u.sign << (f->bits - 1)) | ((uint64_t)rexp << f->fbits) | (rfrac >> (52 - f->fbits));
}

/** @brief FPRecipStepFused (FRECPS, 2 - a * b) or FPRSqrtStepFused (FRSQRTS, (3 - a * b) / 2), one rounding. */
static uint64_t fp_step(fpenv *v, const fmt *f, uint64_t a, uint64_t b, int rsqrt)
{
    uint64_t na = a ^ (1ull << (f->bits - 1));
    unp ua = unpack(v, f, na), ub = unpack(v, f, b);
    const unp *ops[2] = { &ua, &ub };
    uint64_t res;
    /* 2.0 or 1.5 */
    uint64_t special = rsqrt ? ((uint64_t)f->bias << f->fbits) | (1ull << (f->fbits - 1))
                             : (uint64_t)(f->bias + 1) << f->fbits;
    if (process_nans(v, f, ops, 2, &res))
        return res;
    if ((ua.k == K_INF && ub.k == K_ZERO) || (ua.k == K_ZERO && ub.k == K_INF))
        return special;
    if (ua.k == K_INF || ub.k == K_INF)
        return make_inf(f, ua.sign ^ ub.sign);
    /* The constant 2 or 3 as m * 2^q, and the halving for FRSQRTS. */
    rl_u128 cm = rsqrt ? 3 : 1;
    int cq = rsqrt ? -1 : 1;
    if (ua.k == K_ZERO || ub.k == K_ZERO)
        return round_pack(v, f, 0, cm, cq, rm_fpcr(v));
    return sum_pack(v, f, ua.sign ^ ub.sign, (rl_u128)ua.sig * ub.sig, ua.e + ub.e - (rsqrt ? 1 : 0), 0, cm, cq);
}

/** @brief FPCompare: NZCV in bits 31 to 28. */
static uint32_t fp_compare(fpenv *v, const fmt *f, uint64_t a, uint64_t b, int signal_nans)
{
    unp ua = unpack(v, f, a), ub = unpack(v, f, b);
    if (is_nan(&ua) || is_nan(&ub)) {
        if (signal_nans || ua.k == K_SNAN || ub.k == K_SNAN)
            v->flags |= FPSR_IOC;
        return 0x30000000u;
    }
    int c = 0;
    /* Order by value: zeros are equal whatever their sign. */
    if (ua.k == K_ZERO && ub.k == K_ZERO) {
        c = 0;
    } else {
        int la = ua.k == K_ZERO ? 0 : (ua.sign ? -1 : 1), lb = ub.k == K_ZERO ? 0 : (ub.sign ? -1 : 1);
        if (la != lb) {
            c = la < lb ? -1 : 1;
        } else {
            /* Same sign, both nonzero: compare magnitudes. */
            int m;
            if (ua.k == K_INF || ub.k == K_INF)
                m = (ua.k == K_INF) - (ub.k == K_INF);
            else if (ua.e != ub.e)
                m = ua.e < ub.e ? -1 : 1;
            else
                m = ua.sig < ub.sig ? -1 : (ua.sig > ub.sig ? 1 : 0);
            c = la < 0 ? -m : m;
        }
    }
    if (c == 0)
        return 0x60000000u;
    return c < 0 ? 0x80000000u : 0x20000000u;
}

/** @brief FPCompareEQ (kind 0), FPCompareGE (1) or FPCompareGT (2). */
static int fp_compare_bool(fpenv *v, const fmt *f, uint64_t a, uint64_t b, int kind)
{
    uint32_t nzcv = fp_compare(v, f, a, b, kind != 0);
    switch (kind) {
    case 0: return nzcv == 0x60000000u;
    case 1: return nzcv == 0x60000000u || nzcv == 0x20000000u;
    default: return nzcv == 0x20000000u;
    }
}

/** @brief FPMax or FPMin (num 0), FPMaxNum or FPMinNum (num 1). */
static uint64_t fp_maxmin(fpenv *v, const fmt *f, uint64_t a, uint64_t b, int is_max, int num)
{
    unp ua = unpack(v, f, a), ub = unpack(v, f, b);
    if (num) {
        if (ua.k == K_QNAN && ub.k != K_QNAN) {
            a = make_inf(f, is_max);
            ua = unpack(v, f, a);
        } else if (ua.k != K_QNAN && ub.k == K_QNAN) {
            b = make_inf(f, is_max);
            ub = unpack(v, f, b);
        }
    }
    const unp *ops[2] = { &ua, &ub };
    uint64_t res;
    if (process_nans(v, f, ops, 2, &res))
        return res;
    if (ua.k == K_ZERO && ub.k == K_ZERO)
        return make_zero(f, is_max ? (ua.sign & ub.sign) : (ua.sign | ub.sign));
    fpenv quiet = *v;
    uint32_t nzcv = fp_compare(&quiet, f, a, b, 0);
    int first = is_max ? nzcv == 0x20000000u : nzcv == 0x80000000u;
    const unp *w = first ? &ua : &ub;
    if (w->k == K_INF)
        return make_inf(f, w->sign);
    if (w->k == K_ZERO)
        return make_zero(f, w->sign);
    return pack_unp(v, f, w, w->sign);
}

static uint64_t convert_nan(const fmt *from, const fmt *to, uint64_t a)
{
    uint64_t sign = (a >> (from->bits - 1)) & 1u;
    uint64_t frac = a & ((1ull << from->fbits) - 1);
    uint64_t out;
    if (to->fbits >= from->fbits)
        out = frac << (to->fbits - from->fbits);
    else
        out = frac >> (from->fbits - to->fbits);
    return (sign << (to->bits - 1)) | (((1ull << to->ebits) - 1) << to->fbits) | (1ull << (to->fbits - 1)) |
           (out & ((1ull << to->fbits) - 1));
}

static uint64_t fp_convert(fpenv *v, const fmt *from, const fmt *to, uint64_t a, uint32_t rmode)
{
    unp u = unpack(v, from, a);
    int dst_ahp = ahp_on(v, to);
    v->dst_ahp = dst_ahp;
    if (is_nan(&u)) {
        if (u.k == K_SNAN)
            v->flags |= FPSR_IOC;
        if (dst_ahp) {
            v->flags |= FPSR_IOC;
            return make_zero(to, u.sign);
        }
        if (v->fpcr & RL_FPCR_DN)
            return default_nan(to);
        return convert_nan(from, to, a);
    }
    if (u.k == K_INF) {
        if (dst_ahp) {
            v->flags |= FPSR_IOC;
            return ((uint64_t)u.sign << 15) | 0x7FFFu;
        }
        return make_inf(to, u.sign);
    }
    if (u.k == K_ZERO)
        return make_zero(to, u.sign);
    return round_pack(v, to, u.sign, u.sig, u.e, rm_arg(v, rmode));
}

static uint64_t fp_to_fixed(fpenv *v, const fmt *f, unsigned to_bits, unsigned fbits, int is_unsigned, uint32_t rmode,
                            uint64_t a)
{
    unp u = unpack(v, f, a);
    unsigned rm = rm_arg(v, rmode);
    rl_u128 mag = 0, limit;
    int inexact = 0, ovf = 0;
    uint64_t res, all = to_bits == 64 ? ~0ull : (1ull << to_bits) - 1;
    if (is_nan(&u)) {
        v->flags |= FPSR_IOC;
        return 0;
    }
    if (u.k == K_INF) {
        ovf = 1;
    } else if (u.k == K_NORM) {
        int ex = u.e + (int)fbits;
        if (ex >= 1)
            ovf = 1;
        else if (ex == 0)
            mag = u.sig;
        else
            mag = round_shift(u.sig, (unsigned)(-ex), u.sign, rm, &inexact);
    }
    if (!ovf) {
        if (is_unsigned)
            ovf = u.sign ? mag != 0 : mag > (rl_u128)all;
        else {
            limit = ((rl_u128)1 << (to_bits - 1)) - (u.sign ? 0 : 1);
            ovf = mag > limit;
        }
    }
    if (ovf) {
        v->flags |= FPSR_IOC;
        if (is_unsigned)
            return u.sign ? 0 : all;
        return u.sign ? (1ull << (to_bits - 1)) : (all >> 1);
    }
    if (inexact)
        v->flags |= FPSR_IXC;
    res = (uint64_t)mag;
    if (u.sign)
        res = 0 - res;
    return res & all;
}

static uint64_t fp_from_fixed(fpenv *v, const fmt *to, uint64_t a, unsigned from_bits, unsigned fbits, int is_unsigned,
                              uint32_t rmode)
{
    uint64_t mask = from_bits == 64 ? ~0ull : (1ull << from_bits) - 1;
    uint64_t x = a & mask;
    unsigned sign = 0;
    v->dst_ahp = ahp_on(v, to);
    if (!is_unsigned && ((x >> (from_bits - 1)) & 1u)) {
        sign = 1;
        x = (0 - x) & mask;
    }
    if (x == 0)
        return 0;
    return round_pack(v, to, sign, x, -(int)fbits, rm_arg(v, rmode));
}

static void commit(const fpenv *v, uint32_t *fpsr)
{
    if (fpsr)
        *fpsr |= v->flags;
}

#define ENV(fpcr) { (fpcr), 0, 0 }

uint32_t rl_fp32_add(uint32_t a, uint32_t b, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint32_t r = (uint32_t)fp_addsub(&v, &F32, a, b, 0);
    commit(&v, fpsr);
    return r;
}

uint32_t rl_fp32_sub(uint32_t a, uint32_t b, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint32_t r = (uint32_t)fp_addsub(&v, &F32, a, b, 1);
    commit(&v, fpsr);
    return r;
}

uint32_t rl_fp32_mul(uint32_t a, uint32_t b, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint32_t r = (uint32_t)fp_mul(&v, &F32, a, b);
    commit(&v, fpsr);
    return r;
}

uint32_t rl_fp32_div(uint32_t a, uint32_t b, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint32_t r = (uint32_t)fp_div(&v, &F32, a, b);
    commit(&v, fpsr);
    return r;
}

uint32_t rl_fp32_muladd(uint32_t addend, uint32_t a, uint32_t b, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint32_t r = (uint32_t)fp_muladd(&v, &F32, addend, a, b);
    commit(&v, fpsr);
    return r;
}

uint32_t rl_fp32_sqrt(uint32_t a, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint32_t r = (uint32_t)fp_sqrt(&v, &F32, a);
    commit(&v, fpsr);
    return r;
}

uint32_t rl_fp32_round_int(uint32_t a, uint32_t rmode, int exact, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint32_t r = (uint32_t)fp_round_int(&v, &F32, a, rmode, exact);
    commit(&v, fpsr);
    return r;
}

uint32_t rl_fp32_rsqrt_est(uint32_t a, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint32_t r = (uint32_t)fp_rsqrt_est(&v, &F32, a);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fp64_add(uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint64_t r = fp_addsub(&v, &F64, a, b, 0);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fp64_sub(uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint64_t r = fp_addsub(&v, &F64, a, b, 1);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fp64_mul(uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint64_t r = fp_mul(&v, &F64, a, b);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fp64_div(uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint64_t r = fp_div(&v, &F64, a, b);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fp64_muladd(uint64_t addend, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint64_t r = fp_muladd(&v, &F64, addend, a, b);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fp64_sqrt(uint64_t a, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint64_t r = fp_sqrt(&v, &F64, a);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fp64_round_int(uint64_t a, uint32_t rmode, int exact, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint64_t r = fp_round_int(&v, &F64, a, rmode, exact);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fp64_rsqrt_est(uint64_t a, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    uint64_t r = fp_rsqrt_est(&v, &F64, a);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fp_convert(uint64_t a, unsigned from_bits, unsigned to_bits, uint32_t rmode, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    const fmt *from = fmt_of(from_bits), *to = fmt_of(to_bits);
    uint64_t r;
    if (!from || !to)
        return 0;
    r = fp_convert(&v, from, to, a, rmode);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fp_to_fixed(uint64_t a, unsigned from_bits, unsigned to_bits, unsigned fbits, int is_unsigned,
                        uint32_t rmode, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    const fmt *from = fmt_of(from_bits);
    uint64_t r;
    if (!from || (to_bits != 32 && to_bits != 64))
        return 0;
    r = fp_to_fixed(&v, from, to_bits, fbits, is_unsigned, rmode, a);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fixed_to_fp(uint64_t a, unsigned from_bits, unsigned to_bits, unsigned fbits, int is_unsigned,
                        uint32_t rmode, uint32_t fpcr, uint32_t *fpsr)
{
    fpenv v = ENV(fpcr);
    const fmt *to = fmt_of(to_bits);
    uint64_t r;
    if (!to || (from_bits != 32 && from_bits != 64))
        return 0;
    r = fp_from_fixed(&v, to, a, from_bits, fbits, is_unsigned, rmode);
    commit(&v, fpsr);
    return r;
}

/* Width-generic entries (fp.h), for the slow paths. */

#define FPN_BEGIN(bits)                \
    fpenv v = ENV(fpcr);               \
    const fmt *f = fmt_of(bits);       \
    uint64_t r;                        \
    if (!f)                            \
        return 0;

uint64_t rl_fpn_add(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_addsub(&v, f, a, b, 0);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fpn_sub(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_addsub(&v, f, a, b, 1);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fpn_mul(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_mul(&v, f, a, b);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fpn_div(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_div(&v, f, a, b);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fpn_muladd(unsigned bits, uint64_t addend, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_muladd(&v, f, addend, a, b);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fpn_sqrt(unsigned bits, uint64_t a, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_sqrt(&v, f, a);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fpn_round_int(unsigned bits, uint64_t a, uint32_t rmode, int exact, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_round_int(&v, f, a, rmode, exact);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fpn_recip_est(unsigned bits, uint64_t a, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_recip_est(&v, f, a);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fpn_rsqrt_est(unsigned bits, uint64_t a, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_rsqrt_est(&v, f, a);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fpn_recip_step(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_step(&v, f, a, b, 0);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fpn_rsqrt_step(unsigned bits, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_step(&v, f, a, b, 1);
    commit(&v, fpsr);
    return r;
}

uint64_t rl_fpn_maxmin(unsigned bits, uint64_t a, uint64_t b, int is_max, int num, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_maxmin(&v, f, a, b, is_max, num);
    commit(&v, fpsr);
    return r;
}

uint32_t rl_fpn_compare(unsigned bits, uint64_t a, uint64_t b, int signal_nans, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = fp_compare(&v, f, a, b, signal_nans);
    commit(&v, fpsr);
    return (uint32_t)r;
}

int rl_fpn_compare_bool(unsigned bits, uint64_t a, uint64_t b, int kind, uint32_t fpcr, uint32_t *fpsr)
{
    FPN_BEGIN(bits)
    r = (uint64_t)fp_compare_bool(&v, f, a, b, kind);
    commit(&v, fpsr);
    return (int)r;
}
