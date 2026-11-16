/**
 * @file fpu.c
 * @brief The VR4300 FPU rules (fpu.h) and the fast path's FCR31 intrinsics
 * (rl_mips_fcr31, rl_mips_fcmp).
 *
 * The order of the checks (input classes, then the operation's exceptions,
 * then the result class), the unimplemented-operation cases, the default NaN
 * results, the flush rule and the integer conversion ranges are ported from
 * ares's VR4300 FPU interpreter (ares/n64/cpu/interpreter-fpu.cpp), whose
 * notice follows. The arithmetic is relift's own soft float (softfloat.c),
 * not the host's.
 *
 * Copyright (c) 2004-2025 ares team, Near et al
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "fpu.h"

#include "softfloat.h"

#include <relift/mips/abi.h>

#include <math.h>

enum {
    FMT_S = 0x10,
    FMT_D = 0x11,
    FMT_W = 0x14,
    FMT_L = 0x15,
};

/** @brief The default NaN results of the VR4300 (a quiet NaN has the top fraction bit clear). */
#define NAN_S UINT64_C(0x7FBFFFFF)
#define NAN_D UINT64_C(0x7FF7FFFFFFFFFFFF)

/** @brief One operation's view of FCR31. */
typedef struct fpst {
    uint32_t fcr;
} fpst;

static unsigned rm_of(const fpst *s) { return s->fcr & RL_MIPS_FCR_RM; }
static bool enabled(const fpst *s, unsigned k) { return (s->fcr >> 7 & k) != 0; }
static bool flush(const fpst *s) { return (s->fcr & RL_MIPS_FCR_FS) != 0; }

/** @brief Sets cause k; sets flag k too unless k is enabled. Returns whether k raises the exception. */
static bool signal(fpst *s, unsigned k)
{
    s->fcr |= k << 12;
    if (enabled(s, k))
        return true;
    s->fcr |= k << 2;
    return false;
}

/** @brief The unimplemented-operation cause; it always raises the exception. */
static bool unimplemented(fpst *s)
{
    s->fcr |= RL_MIPS_FCR_E;
    return true;
}

static bool is_nan(bool dbl, uint64_t x) { return rl_sf_classify(dbl, x) == RL_SF_NAN; }
/** @brief A NaN the VR4300 treats as signalling: the top fraction bit set. */
static bool is_snan(bool dbl, uint64_t x) { return is_nan(dbl, x) && rl_sf_frac_msb(dbl, x); }

/** @brief The input check of a one-operand float operation. Returns true when the exception is raised. */
static bool check_input(fpst *s, bool dbl, uint64_t x)
{
    switch (rl_sf_classify(dbl, x)) {
    case RL_SF_SUBNORMAL:
        return unimplemented(s);
    case RL_SF_NAN:
        return is_snan(dbl, x) ? signal(s, RL_SF_V) : unimplemented(s);
    default:
        return false;
    }
}

/** @brief The input check of a two-operand float operation. */
static bool check_inputs(fpst *s, bool dbl, uint64_t x, uint64_t y)
{
    rl_sf_class cx = rl_sf_classify(dbl, x), cy = rl_sf_classify(dbl, y);
    if ((cx == RL_SF_NAN && !is_snan(dbl, x)) || (cy == RL_SF_NAN && !is_snan(dbl, y)))
        return unimplemented(s);
    if (cx == RL_SF_SUBNORMAL || cy == RL_SF_SUBNORMAL)
        return unimplemented(s);
    if (is_snan(dbl, x) || is_snan(dbl, y))
        return signal(s, RL_SF_V);
    return false;
}

/** @brief Whether |x| is at least the power of two 2^p (an exact comparison of finite x). */
static bool at_least_pow2(bool dbl, uint64_t x, int p)
{
    int bias = dbl ? 1023 : 127;
    unsigned mbits = dbl ? 52u : 23u;
    uint64_t mag = x & (dbl ? UINT64_C(0x7FFFFFFFFFFFFFFF) : UINT64_C(0x7FFFFFFF));
    return mag >= ((uint64_t)(p + bias) << mbits);
}

/**
 * @brief The input check of a conversion to an integer of 32 (wide false)
 * or 64 bits: subnormal, infinite and NaN inputs and inputs outside
 * [-2^31, 2^31) or (-2^53, 2^53) are unimplemented.
 */
static bool check_input_conv(fpst *s, bool dbl, uint64_t x, bool wide)
{
    rl_sf_class c = rl_sf_classify(dbl, x);
    if (c == RL_SF_SUBNORMAL || c == RL_SF_INF || c == RL_SF_NAN)
        return unimplemented(s);
    bool neg = rl_sf_sign(dbl, x);
    if (wide) {
        if (at_least_pow2(dbl, x, 53))
            return unimplemented(s);
    } else if (at_least_pow2(dbl, x, 31)) {
        uint64_t m31 = dbl ? UINT64_C(0xC1E0000000000000) : UINT64_C(0xCF000000);
        if (!(neg && x == m31))
            return unimplemented(s);
    }
    return false;
}

/**
 * @brief The exceptions an operation raised (flags), as the VR4300 takes
 * them: for a conversion to a 32-bit integer an invalid result is
 * unimplemented; an underflow is unimplemented unless FS is set with the
 * underflow and inexact enables clear; the others set their cause and flag.
 * Returns true when the exception is raised.
 */
static bool check_exceptions(fpst *s, unsigned flags, bool conv)
{
    if (!flags)
        return false;
    if (conv && (flags & RL_SF_V))
        return unimplemented(s);
    if ((flags & RL_SF_U) && (!flush(s) || enabled(s, RL_SF_U) || enabled(s, RL_SF_I)))
        return unimplemented(s);
    bool raise = false;
    if (flags & RL_SF_Z)
        raise |= signal(s, RL_SF_Z);
    if (flags & RL_SF_I)
        raise |= signal(s, RL_SF_I);
    if (flags & RL_SF_U)
        raise |= signal(s, RL_SF_U);
    if (flags & RL_SF_O)
        raise |= signal(s, RL_SF_O);
    if (flags & RL_SF_V)
        raise |= signal(s, RL_SF_V);
    return raise;
}

/**
 * @brief The result check of a float result *r: a subnormal is
 * unimplemented, or with FS flushed by the rounding mode with underflow and
 * inexact; a NaN becomes the default NaN.
 */
static bool check_output(fpst *s, bool dbl, uint64_t *r)
{
    switch (rl_sf_classify(dbl, *r)) {
    case RL_SF_SUBNORMAL: {
        if (!flush(s) || enabled(s, RL_SF_U) || enabled(s, RL_SF_I))
            return unimplemented(s);
        signal(s, RL_SF_U);
        signal(s, RL_SF_I);
        bool neg = rl_sf_sign(dbl, *r);
        uint64_t sign = neg ? (dbl ? UINT64_C(1) << 63 : UINT64_C(1) << 31) : 0;
        uint64_t min = dbl ? UINT64_C(0x0010000000000000) : UINT64_C(0x00800000);
        unsigned rm = rm_of(s);
        bool up = (rm == RL_SF_RP && !neg) || (rm == RL_SF_RM && neg);
        *r = sign | (up ? min : 0);
        return false;
    }
    case RL_SF_NAN:
        *r = dbl ? NAN_D : NAN_S;
        return false;
    default:
        return false;
    }
}

/** @brief The format of the result of funct on fmt, or 0 when the pair is no VR4300 operation (as the overlay's fp_res_fmt). */
static uint32_t result_fmt(uint32_t funct, uint32_t fmt)
{
    bool fl = fmt == FMT_S || fmt == FMT_D;
    if (funct <= 7)
        return fl ? fmt : 0;
    if (funct <= 11)
        return fl ? FMT_L : 0;
    if (funct <= 15)
        return fl ? FMT_W : 0;
    if (funct == 32)
        return fmt != FMT_S ? FMT_S : 0;
    if (funct == 33)
        return fmt != FMT_D ? FMT_D : 0;
    if (funct == 36)
        return fl ? FMT_W : 0;
    if (funct == 37)
        return fl ? FMT_L : 0;
    return 0;
}

bool rl_mips_fpu_is_arith(uint32_t insn) { return insn >> 26 == 0x11 && (insn >> 21 & 31) >= 0x10; }

/** @brief The rounding mode of a conversion to an integer: ROUND, TRUNC, CEIL, FLOOR, or FCR31's for CVT. */
static unsigned int_rm(const fpst *s, uint32_t funct)
{
    if (funct >= 36)
        return rm_of(s);
    switch (funct & 3u) {
    case 0:
        return RL_SF_RN;
    case 1:
        return RL_SF_RZ;
    case 2:
        return RL_SF_RP;
    default:
        return RL_SF_RM;
    }
}

/** @brief A float to integer conversion; writes the integer to *out. */
static bool to_int(fpst *s, uint32_t funct, bool dbl, uint64_t x, bool wide, uint64_t *out)
{
    if (check_input_conv(s, dbl, x, wide))
        return true;
    unsigned flags = 0;
    int64_t v = rl_sf_to_int(dbl, x, int_rm(s, funct), &flags);
    if (!wide && (v < INT32_MIN || v > INT32_MAX))
        flags |= RL_SF_V;
    if (check_exceptions(s, flags, !wide))
        return true;
    bool explicit_inexact = funct < 36 && (funct & 3u) < 2;
    if (explicit_inexact && (flags & RL_SF_I) && signal(s, RL_SF_I))
        return true;
    *out = wide ? (uint64_t)v : (uint64_t)(uint32_t)(int32_t)v;
    return false;
}

/** @brief C.cond.fmt; returns true when the invalid-operation exception is raised. */
static bool compare(fpst *s, uint32_t cond, bool dbl, uint64_t a, uint64_t b)
{
    bool na = is_nan(dbl, a), nb = is_nan(dbl, b);
    bool c;
    if (na || nb) {
        bool quiet = cond < 8;
        if (na && (!quiet || is_snan(dbl, a)) && signal(s, RL_SF_V))
            return true;
        if (nb && (!quiet || is_snan(dbl, b)) && signal(s, RL_SF_V))
            return true;
        c = cond & 1u;
    } else {
        rl_sf_order o = rl_sf_compare(dbl, a, b);
        c = ((cond & 4u) && o == RL_SF_LESS) || ((cond & 2u) && o == RL_SF_EQUAL);
    }
    s->fcr = c ? s->fcr | RL_MIPS_FCR_C : s->fcr & ~(uint32_t)RL_MIPS_FCR_C;
    return false;
}

/** @brief A float operation's result: the exception check, then the result check. */
static bool finish(fpst *s, bool dbl, unsigned flags, uint64_t *r)
{
    if (check_exceptions(s, flags, false))
        return true;
    return check_output(s, dbl, r);
}

/** @brief Runs funct of fmt on a and b; writes the result in format rf to *out. */
static bool run(fpst *s, uint32_t funct, uint32_t fmt, uint32_t rf, uint64_t a, uint64_t b, uint64_t *out)
{
    bool dbl = fmt == FMT_D;
    unsigned rm = rm_of(s), flags = 0;
    uint64_t r;
    switch (funct) {
    case 0:
    case 1:
    case 2:
    case 3:
        if (check_inputs(s, dbl, a, b))
            return true;
        if (funct == 0)
            r = rl_sf_add(dbl, a, b, rm, &flags);
        else if (funct == 1)
            r = rl_sf_sub(dbl, a, b, rm, &flags);
        else if (funct == 2)
            r = rl_sf_mul(dbl, a, b, rm, &flags);
        else
            r = rl_sf_div(dbl, a, b, rm, &flags);
        break;
    case 4:
        if (check_input(s, dbl, a))
            return true;
        r = rl_sf_sqrt(dbl, a, rm, &flags);
        break;
    case 5:
    case 7: {
        if (check_input(s, dbl, a))
            return true;
        uint64_t sign = dbl ? UINT64_C(1) << 63 : UINT64_C(1) << 31;
        r = funct == 5 ? a & ~sign : a ^ sign;
        break;
    }
    case 32:
    case 33: {
        bool to_dbl = funct == 33;
        if (fmt == FMT_W || fmt == FMT_L) {
            int64_t v = fmt == FMT_W ? (int64_t)(int32_t)(uint32_t)a : (int64_t)a;
            if (fmt == FMT_L && (v >= INT64_C(0x0080000000000000) || v < -INT64_C(0x0080000000000000)))
                return unimplemented(s);
            r = rl_sf_from_int(to_dbl, v, rm, &flags);
        } else {
            if (check_input(s, dbl, a))
                return true;
            r = rl_sf_convert(to_dbl, a, rm, &flags);
        }
        dbl = to_dbl;
        break;
    }
    default:
        return to_int(s, funct, dbl, a, rf == FMT_L, out);
    }
    if (finish(s, dbl, flags, &r))
        return true;
    *out = r;
    return false;
}

bool rl_mips_fpu_eval(uint32_t insn, uint32_t status, uint32_t *fcr31, uint64_t f[32])
{
    uint32_t fmt = insn >> 21 & 31, ft = insn >> 16 & 31, fs = insn >> 11 & 31, fd = insn >> 6 & 31;
    uint32_t funct = insn & 63;
    bool fr = (status & RL_MIPS_SR_FR) != 0;
    bool word = fmt == FMT_S || fmt == FMT_W;
    uint64_t a = fr ? f[fs] : f[fs & ~1u];
    uint64_t b = f[ft];
    if (word) {
        a = (uint32_t)a;
        b = (uint32_t)b;
    }
    if (funct == 6 && (fmt == FMT_S || fmt == FMT_D)) {
        f[fd] = fr ? f[fs] : f[fs & ~1u];
        return false;
    }
    fpst s = {*fcr31 & ~(uint32_t)RL_MIPS_FCR_CAUSE};
    bool raised;
    if (funct >= 48) {
        raised = (fmt == FMT_S || fmt == FMT_D) ? compare(&s, funct & 15u, fmt == FMT_D, a, b) : unimplemented(&s);
    } else {
        uint32_t rf = result_fmt(funct, fmt);
        uint64_t r = 0;
        raised = rf ? run(&s, funct, fmt, rf, a, b, &r) : unimplemented(&s);
        if (!raised)
            f[fd] = (rf == FMT_S || rf == FMT_W) ? (uint32_t)r : r;
    }
    *fcr31 = s.fcr;
    return raised;
}

/**
 * @brief Whether binary64 operation op (2 MUL, 3 DIV, 4 SQRT) of a and b
 * rounds, by the soft float: the host's fused multiply-add misses the
 * residual of a result near the bottom of the normal range.
 */
static bool soft_inexact(uint32_t op, uint64_t a, uint64_t b)
{
    unsigned flags = 0;
    if (op == 2)
        rl_sf_mul(true, a, b, RL_SF_RN, &flags);
    else if (op == 3)
        rl_sf_div(true, a, b, RL_SF_RN, &flags);
    else
        rl_sf_sqrt(true, a, RL_SF_RN, &flags);
    return (flags & RL_SF_I) != 0;
}

/** @brief Whether binary64 x is below 2^-900, where a residual can fall under the subnormal range. */
static bool tiny_d(uint64_t x) { return (x >> 52 & 0x7FFu) < 1023u - 900u; }

/** @brief Whether the host result of the fast path's operation was rounded (the host rounds to nearest). */
static bool fast_inexact(uint32_t op, uint32_t fmt, uint64_t a, uint64_t b)
{
    float fa, fb;
    double da, db;
    uint32_t a32 = (uint32_t)a, b32 = (uint32_t)b;
    memcpy(&fa, &a32, 4);
    memcpy(&fb, &b32, 4);
    memcpy(&da, &a, 8);
    memcpy(&db, &b, 8);
    bool s = fmt == FMT_S;
    switch (op) {
    case 0:
    case 1: {
        if (s) {
            float y = op ? -fb : fb, r = fa + y, bv = r - fa;
            return (fa - (r - bv)) + (y - bv) != 0.0f;
        }
        double y = op ? -db : db, r = da + y, bv = r - da;
        return (da - (r - bv)) + (y - bv) != 0.0;
    }
    case 2:
        if (s) {
            double p = (double)fa * (double)fb;
            return (double)(float)p != p;
        }
        if (fabs(da * db) < 0x1p-900 || tiny_d(a) || tiny_d(b))
            return soft_inexact(op, a, b);
        return fma(da, db, -(da * db)) != 0.0;
    case 3:
        if (s) {
            float q = fa / fb;
            return (double)q * (double)fb != (double)fa;
        } else {
            double q = da / db;
            if (fabs(q) < 0x1p-900 || tiny_d(a) || tiny_d(b))
                return soft_inexact(op, a, b);
            return fma(-q, db, da) != 0.0;
        }
    case 4:
        if (s) {
            float r = sqrtf(fa);
            return (double)r * (double)r != (double)fa;
        } else {
            double r = sqrt(da);
            if (tiny_d(a))
                return soft_inexact(op, a, b);
            return fma(-r, r, da) != 0.0;
        }
    case 32:
        if (fmt == FMT_D)
            return (double)(float)da != da;
        if (fmt == FMT_W)
            return (int64_t)(float)(int32_t)a32 != (int32_t)a32;
        if (fmt == FMT_L) {
            float r = (float)(int64_t)a;
            return r >= 0x1p63f || (int64_t)r != (int64_t)a;
        }
        return false;
    case 33:
        if (fmt == FMT_L) {
            double r = (double)(int64_t)a;
            return r >= 0x1p63 || (int64_t)r != (int64_t)a;
        }
        return false;
    case 5:
    case 6:
    case 7:
        return false;
    default:
        if (op >= 8 && op <= 37)
            return s ? truncf(fa) != fa : trunc(da) != da;
        return false;
    }
}

uint32_t rl_mips_fcr31(uint32_t op, uint32_t fmt, uint32_t fcr31, uint64_t a, uint64_t b)
{
    uint32_t r = fcr31 & ~(uint32_t)RL_MIPS_FCR_CAUSE;
    if (fast_inexact(op, fmt, a, b))
        r |= (uint32_t)RL_SF_I << 12 | (uint32_t)RL_SF_I << 2;
    return r;
}

uint32_t rl_mips_fcmp(uint32_t cond, uint32_t fmt, uint32_t fcr31, uint64_t a, uint64_t b)
{
    fpst s = {fcr31 & ~(uint32_t)RL_MIPS_FCR_CAUSE};
    bool dbl = fmt == FMT_D;
    if (!dbl) {
        a = (uint32_t)a;
        b = (uint32_t)b;
    }
    compare(&s, cond & 15u, dbl, a, b);
    return s.fcr;
}
