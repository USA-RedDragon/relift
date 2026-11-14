/**
 * @file fp16.c
 * @brief IEEE binary16 helpers (relift/a64/fp16.h). Every operation forms
 * its exact result as an integer magnitude times a power of two and rounds
 * it once to nearest even, the same algorithm as core/ir/src/half.rs:
 * sums are exact at 2^-24, products at 2^-48, and quotients and square
 * roots carry a sticky bit below the rounding point.
 */
#include <relift/a64/fp16.h>

#define SIGN 0x8000u
#define INF 0x7c00u
#define QUIET 0x0200u

static int is_nan(uint16_t h) {
    return (h & 0x7fffu) > INF;
}

static int is_inf(uint16_t h) {
    return (h & 0x7fffu) == INF;
}

static int is_zero(uint16_t h) {
    return (h & 0x7fffu) == 0;
}

/** @brief The index of the highest set bit of x (x != 0). */
static int top_bit(rl_u128 x) {
    uint64_t hi = (uint64_t)(x >> 64);
    return hi ? 127 - __builtin_clzll(hi) : 63 - __builtin_clzll((uint64_t)x);
}

/** @brief mag * 2^exp with the sign neg, rounded to nearest even binary16. */
static uint16_t round16(int neg, rl_u128 mag, int exp) {
    uint16_t s = neg ? SIGN : 0;
    if (mag == 0)
        return s;
    int e = top_bit(mag) + exp;
    if (e > 15)
        return s | INF;
    /* The unit of the last place: 2^(e - 10) for normals, 2^-24 below. */
    int ulp = (e > -14 ? e : -14) - 10;
    int shift = ulp - exp;
    rl_u128 q;
    if (shift <= 0) {
        q = mag << -shift;
    } else if (shift >= 128) {
        q = 0;
    } else {
        q = mag >> shift;
        rl_u128 rem = mag & (((rl_u128)1 << shift) - 1);
        rl_u128 half = (rl_u128)1 << (shift - 1);
        if (rem > half || (rem == half && (q & 1)))
            q++;
    }
    /* q is the significand at the unit ulp; a carry into the next binade is absorbed by the encoding. */
    uint64_t bits = ulp == -24 ? (uint64_t)q : (uint64_t)q - 1024 + ((uint64_t)(ulp + 25) << 10);
    if (bits >= INF)
        return s | INF;
    return s | (uint16_t)bits;
}

/** @brief The magnitude and exponent of a finite h: |h| = *mag * 2^*exp. */
static void parts(uint16_t h, uint64_t *mag, int *exp) {
    int e = (h >> 10) & 0x1f;
    uint64_t f = h & 0x3ffu;
    if (e == 0) {
        *mag = f;
        *exp = -24;
    } else {
        *mag = f | 0x400;
        *exp = e - 25;
    }
}

/** @brief The NaN result of a two-operand operation where a or b is a NaN. */
static uint16_t nan2(uint16_t a, uint16_t b) {
    return is_nan(a) ? a | QUIET : b | QUIET;
}

uint16_t rl_fp16_add(uint16_t a, uint16_t b) {
    if (is_nan(a) || is_nan(b))
        return nan2(a, b);
    int na = a >> 15, nb = b >> 15;
    if (is_inf(a) || is_inf(b)) {
        if (is_inf(a) && is_inf(b) && na != nb)
            return RL_FP16_DEFAULT_NAN;
        return is_inf(a) ? a : b;
    }
    uint64_t ma, mb;
    int ea, eb;
    parts(a, &ma, &ea);
    parts(b, &mb, &eb);
    /* Exact at 2^-24: at most 11 + 29 bits. */
    uint64_t p = ma << (ea + 24), q = mb << (eb + 24);
    if (na == nb)
        return round16(na, p + q, -24);
    if (p == q)
        return 0; /* x + -x is +0 in round to nearest; -0 + -0 took the branch above */
    return p > q ? round16(na, p - q, -24) : round16(nb, q - p, -24);
}

uint16_t rl_fp16_sub(uint16_t a, uint16_t b) {
    if (is_nan(a) || is_nan(b))
        return nan2(a, b);
    return rl_fp16_add(a, b ^ SIGN);
}

uint16_t rl_fp16_mul(uint16_t a, uint16_t b) {
    if (is_nan(a) || is_nan(b))
        return nan2(a, b);
    int neg = (a ^ b) >> 15;
    if (is_inf(a) || is_inf(b)) {
        if (is_zero(a) || is_zero(b))
            return RL_FP16_DEFAULT_NAN;
        return (uint16_t)((neg ? SIGN : 0) | INF);
    }
    uint64_t ma, mb;
    int ea, eb;
    parts(a, &ma, &ea);
    parts(b, &mb, &eb);
    return round16(neg, (rl_u128)(ma * mb), ea + eb);
}

uint16_t rl_fp16_div(uint16_t a, uint16_t b) {
    if (is_nan(a) || is_nan(b))
        return nan2(a, b);
    uint16_t s = ((a ^ b) & SIGN) ? SIGN : 0;
    if (is_inf(a))
        return is_inf(b) ? RL_FP16_DEFAULT_NAN : (uint16_t)(s | INF);
    if (is_inf(b))
        return s;
    if (is_zero(b))
        return is_zero(a) ? RL_FP16_DEFAULT_NAN : (uint16_t)(s | INF);
    if (is_zero(a))
        return s;
    uint64_t ma, mb;
    int ea, eb;
    parts(a, &ma, &ea);
    parts(b, &mb, &eb);
    /* At least 29 quotient bits, then a sticky bit for any remainder. */
    uint64_t num = ma << 40;
    uint64_t q = num / mb, r = num % mb;
    return round16(s != 0, ((rl_u128)q << 1) | (r != 0), ea - eb - 41);
}

/** @brief The integer square root of x. */
static uint64_t isqrt64(uint64_t x) {
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > x)
        bit >>= 2;
    while (bit) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

uint16_t rl_fp16_sqrt(uint16_t a) {
    if (is_nan(a))
        return a | QUIET;
    if (is_zero(a))
        return a;
    if (a & SIGN)
        return RL_FP16_DEFAULT_NAN;
    if (is_inf(a))
        return a;
    uint64_t m;
    int e;
    parts(a, &m, &e);
    if (e & 1) {
        m <<= 1;
        e--;
    }
    /* sqrt(m * 2^40) has at least 20 bits; a sticky bit marks an inexact root. */
    uint64_t big = m << 40;
    uint64_t r = isqrt64(big);
    return round16(0, ((rl_u128)r << 1) | (r * r != big), e / 2 - 20 - 1);
}

uint16_t rl_fp16_muladd(uint16_t a, uint16_t b, uint16_t c) {
    if (is_nan(a))
        return a | QUIET;
    if (is_nan(b))
        return b | QUIET;
    if (is_nan(c))
        return c | QUIET;
    int np = (a ^ b) >> 15, nc = c >> 15;
    if (is_inf(a) || is_inf(b)) {
        if (is_zero(a) || is_zero(b))
            return RL_FP16_DEFAULT_NAN;
        if (is_inf(c) && nc != np)
            return RL_FP16_DEFAULT_NAN;
        return (uint16_t)((np ? SIGN : 0) | INF);
    }
    if (is_inf(c))
        return c;
    uint64_t ma, mb, mc;
    int ea, eb, ec;
    parts(a, &ma, &ea);
    parts(b, &mb, &eb);
    parts(c, &mc, &ec);
    /* Exact at 2^-48: products up to 22 + 58 bits, the addend up to 11 + 53. */
    rl_u128 p = (rl_u128)(ma * mb) << (ea + eb + 48);
    rl_u128 q = (rl_u128)mc << (ec + 48);
    if (np == nc)
        return round16(np, p + q, -48);
    if (p == q)
        return 0; /* exact cancellation, or +0 + -0: +0 */
    return p > q ? round16(np, p - q, -48) : round16(nc, q - p, -48);
}

uint16_t rl_fp16_round_int(uint16_t a, int rmode) {
    if (is_nan(a))
        return a | QUIET;
    if (is_inf(a) || is_zero(a))
        return a;
    uint64_t m;
    int e;
    parts(a, &m, &e);
    if (e >= 0)
        return a;
    int neg = a >> 15;
    uint64_t ip = -e >= 64 ? 0 : m >> -e;
    uint64_t rem = m - (ip << -e);
    uint64_t half = 1ull << (-e - 1);
    int up;
    switch (rmode) {
    case RL_RM_POSINF: up = rem != 0 && !neg; break;
    case RL_RM_NEGINF: up = rem != 0 && neg; break;
    case RL_RM_ZERO: up = 0; break;
    case RL_RM_AWAY: up = rem >= half; break;
    default: up = rem > half || (rem == half && (ip & 1)); break;
    }
    return round16(neg, ip + (uint64_t)up, 0);
}

uint32_t rl_fp16_to_f32(uint16_t h) {
    uint32_t s = (uint32_t)(h & SIGN) << 16;
    uint32_t e = (h >> 10) & 0x1f, f = h & 0x3ffu;
    if (e == 0x1f)
        return s | 0x7f800000u | (f << 13);
    if (e == 0) {
        if (f == 0)
            return s;
        /* Normalize: f * 2^-24 with the top bit at 2^(t - 24). */
        int t = 31 - __builtin_clz(f);
        return s | ((uint32_t)(t - 24 + 127) << 23) | ((f << (23 - t)) & 0x7fffffu);
    }
    return s | ((e - 15 + 127) << 23) | (f << 13);
}

uint64_t rl_fp16_to_f64(uint16_t h) {
    uint64_t s = (uint64_t)(h & SIGN) << 48;
    uint64_t e = (h >> 10) & 0x1f, f = h & 0x3ffu;
    if (e == 0x1f)
        return s | 0x7ff0000000000000ull | (f << 42);
    if (e == 0) {
        if (f == 0)
            return s;
        int t = 63 - __builtin_clzll(f);
        return s | ((uint64_t)(t - 24 + 1023) << 52) | ((f << (52 - t)) & 0xfffffffffffffull);
    }
    return s | ((e - 15 + 1023) << 52) | (f << 42);
}

uint16_t rl_fp16_from_f32(uint32_t x) {
    int neg = x >> 31;
    uint16_t s = neg ? SIGN : 0;
    uint32_t e = (x >> 23) & 0xff, f = x & 0x7fffffu;
    if (e == 0xff)
        return f ? (uint16_t)(s | INF | QUIET | (f >> 13)) : (uint16_t)(s | INF);
    if (e == 0)
        return round16(neg, f, -149);
    return round16(neg, f | 0x800000u, (int)e - 150);
}

uint16_t rl_fp16_from_f64(uint64_t x) {
    int neg = (int)(x >> 63);
    uint16_t s = neg ? SIGN : 0;
    int e = (int)((x >> 52) & 0x7ff);
    uint64_t f = x & 0xfffffffffffffull;
    if (e == 0x7ff)
        return f ? (uint16_t)(s | INF | QUIET | (f >> 42)) : (uint16_t)(s | INF);
    if (e == 0)
        return round16(neg, f, -1074);
    return round16(neg, f | (1ull << 52), e - 1075);
}

uint64_t rl_fp16_to_int(uint16_t h, unsigned bits, int is_unsigned, int rmode) {
    if (is_nan(h))
        return 0;
    if (bits == 0 || bits > 64)
        bits = 64;
    uint64_t mask = bits < 64 ? (1ull << bits) - 1 : ~0ull;
    int neg = h >> 15;
    /* The largest magnitude allowed on each side of zero. */
    uint64_t pos_max = is_unsigned ? mask : mask >> 1;
    uint64_t neg_max = is_unsigned ? 0 : (mask >> 1) + 1;
    uint64_t mag;
    if (is_inf(h)) {
        mag = ~0ull;
    } else {
        uint16_t r = rl_fp16_round_int(h, rmode);
        uint64_t m;
        int e;
        parts(r, &m, &e);
        mag = e >= 0 ? m << e : m >> -e; /* r is integral, so the shift is exact */
    }
    if (neg) {
        if (mag > neg_max)
            mag = neg_max;
        return (0 - mag) & mask;
    }
    return (mag > pos_max ? pos_max : mag) & mask;
}

uint16_t rl_fp16_from_int(uint64_t x, unsigned bits, int is_unsigned) {
    if (bits < 64)
        x &= (1ull << bits) - 1;
    int neg = !is_unsigned && bits && ((x >> (bits - 1)) & 1);
    uint64_t mag = x;
    if (neg)
        mag = bits < 64 ? ((~x + 1) & ((1ull << bits) - 1)) : ~x + 1;
    return round16(neg, mag, 0);
}
