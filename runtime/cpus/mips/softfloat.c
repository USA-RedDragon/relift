/**
 * @file softfloat.c
 * @brief IEEE 754 binary32 and binary64 arithmetic in integer code
 * (softfloat.h). An unpacked finite value is sig / 2^62 * 2^exp with the
 * leading one of sig at bit 62 (bit 63 takes a carry) and every bit shifted
 * out folded into bit 0 (sticky), which keeps at least nine bits below a
 * binary64 significand for rounding.
 */
#include "softfloat.h"

typedef struct fmt {
    unsigned mbits;
    unsigned ebits;
    int bias;
    int emax;
} fmt;

static const fmt F32 = {23, 8, 127, 254};
static const fmt F64 = {52, 11, 1023, 2046};

static const fmt *fmt_of(bool dbl) { return dbl ? &F64 : &F32; }

typedef struct unpacked {
    bool sign;
    rl_sf_class cls;
    int exp;
    uint64_t sig;
} unpacked;

static uint64_t frac_mask(const fmt *f) { return (UINT64_C(1) << f->mbits) - 1u; }
static uint64_t exp_field(const fmt *f, uint64_t x) { return x >> f->mbits & ((UINT64_C(1) << f->ebits) - 1u); }
static uint64_t sign_bit(const fmt *f) { return UINT64_C(1) << (f->mbits + f->ebits); }

rl_sf_class rl_sf_classify(bool dbl, uint64_t x)
{
    const fmt *f = fmt_of(dbl);
    uint64_t e = exp_field(f, x), m = x & frac_mask(f);
    if (e == 0)
        return m ? RL_SF_SUBNORMAL : RL_SF_ZERO;
    if (e == (UINT64_C(1) << f->ebits) - 1u)
        return m ? RL_SF_NAN : RL_SF_INF;
    return RL_SF_NORMAL;
}

bool rl_sf_sign(bool dbl, uint64_t x) { return (x & sign_bit(fmt_of(dbl))) != 0; }

bool rl_sf_frac_msb(bool dbl, uint64_t x) { return x >> (fmt_of(dbl)->mbits - 1u) & 1u; }

static int clz64(uint64_t x)
{
    int n = 0;
    if (!x)
        return 64;
    while (!(x & (UINT64_C(1) << 63))) {
        x <<= 1;
        n++;
    }
    return n;
}

/** @brief x >> n with every bit shifted out folded into bit 0. */
static uint64_t shift_sticky(uint64_t x, unsigned n)
{
    if (n == 0)
        return x;
    if (n >= 64)
        return x != 0;
    return x >> n | ((x & ((UINT64_C(1) << n) - 1u)) != 0);
}

static unpacked unpack(const fmt *f, uint64_t x)
{
    unpacked u = {.sign = (x & sign_bit(f)) != 0, .cls = RL_SF_ZERO, .exp = 0, .sig = 0};
    uint64_t e = exp_field(f, x), m = x & frac_mask(f);
    if (e == (UINT64_C(1) << f->ebits) - 1u) {
        u.cls = m ? RL_SF_NAN : RL_SF_INF;
        return u;
    }
    if (e == 0) {
        if (!m)
            return u;
        u.cls = RL_SF_SUBNORMAL;
        int s = clz64(m) - 1;
        u.sig = m << s;
        u.exp = 1 - f->bias - (s - (int)(62 - f->mbits));
        return u;
    }
    u.cls = RL_SF_NORMAL;
    u.sig = (m | UINT64_C(1) << f->mbits) << (62 - f->mbits);
    u.exp = (int)e - f->bias;
    return u;
}

static uint64_t inf_bits(const fmt *f, bool sign)
{
    return (sign ? sign_bit(f) : 0) | ((UINT64_C(1) << f->ebits) - 1u) << f->mbits;
}

static uint64_t zero_bits(const fmt *f, bool sign) { return sign ? sign_bit(f) : 0; }

static uint64_t nan_bits(const fmt *f) { return inf_bits(f, false) | UINT64_C(1) << (f->mbits - 1u); }

/** @brief x >> shift rounded by rm for a value of sign; sets *inexact when bits were dropped. */
static uint64_t round_shift(uint64_t x, unsigned shift, unsigned rm, bool sign, bool *inexact)
{
    uint64_t q = x >> shift, rem = x & ((UINT64_C(1) << shift) - 1u);
    *inexact = rem != 0;
    if (!rem)
        return q;
    uint64_t half = UINT64_C(1) << (shift - 1u);
    switch (rm) {
    case RL_SF_RN:
        if (rem > half || (rem == half && (q & 1u)))
            q++;
        break;
    case RL_SF_RP:
        if (!sign)
            q++;
        break;
    case RL_SF_RM:
        if (sign)
            q++;
        break;
    default:
        break;
    }
    return q;
}

/** @brief Whether rm rounds a value of sign away from zero on overflow (to infinity). */
static bool overflow_to_inf(unsigned rm, bool sign)
{
    return rm == RL_SF_RN || (rm == RL_SF_RP && !sign) || (rm == RL_SF_RM && sign);
}

/** @brief Rounds sig / 2^62 * 2^exp (sig normalized to bit 62, sticky in bit 0) into format f. */
static uint64_t round_pack(const fmt *f, bool sign, int exp, uint64_t sig, unsigned rm, unsigned *flags)
{
    if (!sig)
        return zero_bits(f, sign);
    unsigned shift = 62 - f->mbits;
    int e = exp + f->bias;
    bool inexact;
    uint64_t r = round_shift(sig, shift, rm, sign, &inexact);
    int er = e;
    if (r >> (f->mbits + 1u)) {
        r >>= 1;
        er++;
    }
    if (e >= 1) {
        if (er > f->emax) {
            *flags |= RL_SF_O | RL_SF_I;
            if (overflow_to_inf(rm, sign))
                return inf_bits(f, sign);
            return zero_bits(f, sign) | ((uint64_t)f->emax << f->mbits) | frac_mask(f);
        }
        if (inexact)
            *flags |= RL_SF_I;
        return zero_bits(f, sign) | ((uint64_t)er << f->mbits) | (r & frac_mask(f));
    }
    bool tiny = er < 1;
    uint64_t d = shift_sticky(sig, (unsigned)(1 - e) > 63u ? 64u : (unsigned)(1 - e));
    r = round_shift(d, shift, rm, sign, &inexact);
    if (inexact) {
        *flags |= RL_SF_I;
        if (tiny)
            *flags |= RL_SF_U;
    }
    return zero_bits(f, sign) | r;
}

/** @brief Moves a carry in bit 63 down to bit 62. */
static void normalize_carry(int *exp, uint64_t *sig)
{
    if (*sig >> 63) {
        *sig = shift_sticky(*sig, 1);
        (*exp)++;
    }
}

/** @brief Moves the leading one of a nonzero sig up to bit 62. */
static void normalize_up(int *exp, uint64_t *sig)
{
    int s = clz64(*sig) - 1;
    if (s > 0) {
        *sig <<= s;
        *exp -= s;
    }
}

static uint64_t add_signed(const fmt *f, uint64_t a, uint64_t b, bool negate_b, unsigned rm, unsigned *flags)
{
    unpacked x = unpack(f, a), y = unpack(f, b);
    if (negate_b)
        y.sign = !y.sign;
    if (x.cls == RL_SF_NAN || y.cls == RL_SF_NAN)
        return nan_bits(f);
    if (x.cls == RL_SF_INF || y.cls == RL_SF_INF) {
        if (x.cls == RL_SF_INF && y.cls == RL_SF_INF && x.sign != y.sign) {
            *flags |= RL_SF_V;
            return nan_bits(f);
        }
        return inf_bits(f, x.cls == RL_SF_INF ? x.sign : y.sign);
    }
    if (x.cls == RL_SF_ZERO && y.cls == RL_SF_ZERO)
        return zero_bits(f, x.sign == y.sign ? x.sign : rm == RL_SF_RM);
    if (x.cls == RL_SF_ZERO)
        return round_pack(f, y.sign, y.exp, y.sig, rm, flags);
    if (y.cls == RL_SF_ZERO)
        return round_pack(f, x.sign, x.exp, x.sig, rm, flags);
    if (x.exp < y.exp || (x.exp == y.exp && x.sig < y.sig)) {
        unpacked t = x;
        x = y;
        y = t;
    }
    unsigned d = (unsigned)(x.exp - y.exp);
    uint64_t ys = shift_sticky(y.sig, d > 63u ? 64u : d);
    int exp = x.exp;
    uint64_t sig;
    if (x.sign == y.sign) {
        sig = x.sig + ys;
        normalize_carry(&exp, &sig);
    } else {
        sig = x.sig - ys;
        if (!sig)
            return zero_bits(f, rm == RL_SF_RM);
        normalize_up(&exp, &sig);
    }
    return round_pack(f, x.sign, exp, sig, rm, flags);
}

uint64_t rl_sf_add(bool dbl, uint64_t a, uint64_t b, unsigned rm, unsigned *flags)
{
    return add_signed(fmt_of(dbl), a, b, false, rm, flags);
}

uint64_t rl_sf_sub(bool dbl, uint64_t a, uint64_t b, unsigned rm, unsigned *flags)
{
    return add_signed(fmt_of(dbl), a, b, true, rm, flags);
}

/** @brief The 128-bit product of a and b. */
static void mul64(uint64_t a, uint64_t b, uint64_t *hi, uint64_t *lo)
{
    uint64_t al = a & 0xFFFFFFFFu, ah = a >> 32, bl = b & 0xFFFFFFFFu, bh = b >> 32;
    uint64_t ll = al * bl, lh = al * bh, hl = ah * bl, hh = ah * bh;
    uint64_t mid = (ll >> 32) + (lh & 0xFFFFFFFFu) + (hl & 0xFFFFFFFFu);
    *lo = (ll & 0xFFFFFFFFu) | mid << 32;
    *hi = hh + (lh >> 32) + (hl >> 32) + (mid >> 32);
}

uint64_t rl_sf_mul(bool dbl, uint64_t a, uint64_t b, unsigned rm, unsigned *flags)
{
    const fmt *f = fmt_of(dbl);
    unpacked x = unpack(f, a), y = unpack(f, b);
    bool sign = x.sign != y.sign;
    if (x.cls == RL_SF_NAN || y.cls == RL_SF_NAN)
        return nan_bits(f);
    if (x.cls == RL_SF_INF || y.cls == RL_SF_INF) {
        if (x.cls == RL_SF_ZERO || y.cls == RL_SF_ZERO) {
            *flags |= RL_SF_V;
            return nan_bits(f);
        }
        return inf_bits(f, sign);
    }
    if (x.cls == RL_SF_ZERO || y.cls == RL_SF_ZERO)
        return zero_bits(f, sign);
    uint64_t hi, lo;
    mul64(x.sig, y.sig, &hi, &lo);
    uint64_t sig = hi << 2 | lo >> 62 | ((lo & ((UINT64_C(1) << 62) - 1u)) != 0);
    int exp = x.exp + y.exp;
    normalize_carry(&exp, &sig);
    return round_pack(f, sign, exp, sig, rm, flags);
}

uint64_t rl_sf_div(bool dbl, uint64_t a, uint64_t b, unsigned rm, unsigned *flags)
{
    const fmt *f = fmt_of(dbl);
    unpacked x = unpack(f, a), y = unpack(f, b);
    bool sign = x.sign != y.sign;
    if (x.cls == RL_SF_NAN || y.cls == RL_SF_NAN)
        return nan_bits(f);
    if (x.cls == RL_SF_INF) {
        if (y.cls == RL_SF_INF) {
            *flags |= RL_SF_V;
            return nan_bits(f);
        }
        return inf_bits(f, sign);
    }
    if (y.cls == RL_SF_INF)
        return zero_bits(f, sign);
    if (y.cls == RL_SF_ZERO) {
        if (x.cls == RL_SF_ZERO) {
            *flags |= RL_SF_V;
            return nan_bits(f);
        }
        *flags |= RL_SF_Z;
        return inf_bits(f, sign);
    }
    if (x.cls == RL_SF_ZERO)
        return zero_bits(f, sign);
    int exp = x.exp - y.exp;
    uint64_t r = x.sig, q = 0;
    if (r < y.sig) {
        r <<= 1;
        exp--;
    }
    for (int i = 0; i < 63; i++) {
        q <<= 1;
        if (r >= y.sig) {
            r -= y.sig;
            q |= 1u;
        }
        r <<= 1;
    }
    return round_pack(f, sign, exp, q | (r != 0), rm, flags);
}

uint64_t rl_sf_sqrt(bool dbl, uint64_t a, unsigned rm, unsigned *flags)
{
    const fmt *f = fmt_of(dbl);
    unpacked x = unpack(f, a);
    if (x.cls == RL_SF_NAN)
        return nan_bits(f);
    if (x.cls == RL_SF_ZERO)
        return a;
    if (x.sign) {
        *flags |= RL_SF_V;
        return nan_bits(f);
    }
    if (x.cls == RL_SF_INF)
        return a;
    unsigned extra = (x.exp & 1) ? 63u : 62u;
    int exp = (x.exp - (x.exp & 1)) / 2;
    uint64_t rh = extra == 63u ? x.sig >> 1 : x.sig >> 2;
    uint64_t rl = x.sig << extra;
    uint64_t res = 0, remh = 0, reml = 0;
    for (int i = 62; i >= 0; i--) {
        unsigned two = (unsigned)(i >= 32 ? rh >> ((i - 32) * 2) : rl >> (i * 2)) & 3u;
        remh = remh << 2 | reml >> 62;
        reml = reml << 2 | two;
        uint64_t th = res >> 62, tl = res << 2 | 1u;
        if (remh > th || (remh == th && reml >= tl)) {
            uint64_t borrow = reml < tl;
            reml -= tl;
            remh -= th + borrow;
            res = res << 1 | 1u;
        } else {
            res <<= 1;
        }
    }
    return round_pack(f, false, exp, res | (remh != 0 || reml != 0), rm, flags);
}

uint64_t rl_sf_convert(bool to_dbl, uint64_t a, unsigned rm, unsigned *flags)
{
    const fmt *from = fmt_of(!to_dbl), *to = fmt_of(to_dbl);
    unpacked x = unpack(from, a);
    switch (x.cls) {
    case RL_SF_NAN:
        return nan_bits(to);
    case RL_SF_INF:
        return inf_bits(to, x.sign);
    case RL_SF_ZERO:
        return zero_bits(to, x.sign);
    default:
        return round_pack(to, x.sign, x.exp, x.sig, rm, flags);
    }
}

uint64_t rl_sf_from_int(bool dbl, int64_t v, unsigned rm, unsigned *flags)
{
    const fmt *f = fmt_of(dbl);
    if (!v)
        return 0;
    bool sign = v < 0;
    uint64_t m = sign ? 0u - (uint64_t)v : (uint64_t)v;
    int exp = 62;
    uint64_t sig = m;
    if (sig >> 63) {
        sig = shift_sticky(sig, 1);
        exp++;
    }
    normalize_up(&exp, &sig);
    return round_pack(f, sign, exp, sig, rm, flags);
}

int64_t rl_sf_to_int(bool dbl, uint64_t a, unsigned rm, unsigned *flags)
{
    unpacked x = unpack(fmt_of(dbl), a);
    if (x.cls == RL_SF_ZERO || x.cls == RL_SF_NAN || x.cls == RL_SF_INF)
        return 0;
    uint64_t q;
    bool inexact = false;
    if (x.exp >= 62) {
        q = x.sig << (x.exp - 62 > 1 ? 1 : x.exp - 62);
    } else if (62 - x.exp >= 64) {
        inexact = true;
        bool up = (rm == RL_SF_RP && !x.sign) || (rm == RL_SF_RM && x.sign);
        q = up ? 1u : 0u;
    } else {
        q = round_shift(x.sig, (unsigned)(62 - x.exp), rm, x.sign, &inexact);
    }
    if (inexact)
        *flags |= RL_SF_I;
    return x.sign ? (int64_t)(0u - q) : (int64_t)q;
}

rl_sf_order rl_sf_compare(bool dbl, uint64_t a, uint64_t b)
{
    const fmt *f = fmt_of(dbl);
    if (rl_sf_classify(dbl, a) == RL_SF_NAN || rl_sf_classify(dbl, b) == RL_SF_NAN)
        return RL_SF_UNORDERED;
    uint64_t mag = sign_bit(f) - 1u;
    if ((a & mag) == 0 && (b & mag) == 0)
        return RL_SF_EQUAL;
    bool sa = (a & sign_bit(f)) != 0, sb = (b & sign_bit(f)) != 0;
    if (sa != sb)
        return sa ? RL_SF_LESS : RL_SF_GREATER;
    if (a == b)
        return RL_SF_EQUAL;
    bool below = (a & mag) < (b & mag);
    if (sa)
        below = !below;
    return below ? RL_SF_LESS : RL_SF_GREATER;
}
