/**
 * @file test_fp16.c
 * @brief The binary16 helpers: exhaustive round trips through binary32 and
 * binary64, arithmetic against the host's binary64 arithmetic rounded once
 * with rl_fp16_from_f64 (exact for binary16 sums, products and quotients
 * because binary64 has more than 2p + 2 bits), square roots and
 * round-to-integral exhaustively, fused multiply-add against a TwoSum
 * reference, and integer conversion edges.
 */
#include "runner.h"

#include <relift/arm64/fp16.h>

static double dbl(uint64_t b) {
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}

static uint64_t bits64(double d) {
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}

static float flt(uint32_t b) {
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}

static double h2d(uint16_t h) {
    return dbl(rl_fp16_to_f64(h));
}

static int nan16(uint16_t h) {
    return (h & 0x7fffu) > 0x7c00u;
}

static uint64_t rng = 0x2545f4914f6cdd1dull;

static uint16_t rnd16(void) {
    rng ^= rng << 13;
    rng ^= rng >> 7;
    rng ^= rng << 17;
    return (uint16_t)(rng >> 24);
}

TEST(fp16_round_trips_exhaustive)
{
    for (uint32_t i = 0; i < 0x10000; i++) {
        uint16_t h = (uint16_t)i;
        uint32_t f = rl_fp16_to_f32(h);
        uint64_t d = rl_fp16_to_f64(h);
        uint16_t want = nan16(h) ? (uint16_t)(h | 0x200) : h;
        CHECK_EQ(rl_fp16_from_f32(f), want);
        CHECK_EQ(rl_fp16_from_f64(d), want);
        if (nan16(h)) {
            /* Sign, payload and quietness carried over. */
            CHECK_EQ(f, ((uint32_t)(h & 0x8000) << 16) | 0x7f800000u | ((uint32_t)(h & 0x3ff) << 13));
            CHECK_EQ(d, ((uint64_t)(h & 0x8000) << 48) | 0x7ff0000000000000ull | ((uint64_t)(h & 0x3ff) << 42));
        } else {
            /* The binary32 and binary64 values agree (float to double is exact). */
            CHECK_EQ(bits64((double)flt(f)), d);
        }
    }
    /* Known values. */
    CHECK_EQ(rl_fp16_to_f32(0x3c00), 0x3f800000u);
    CHECK_EQ(rl_fp16_to_f32(0x0001), 0x33800000u); /* 2^-24 */
    CHECK_EQ(rl_fp16_to_f32(0x7bff), 0x477fe000u); /* 65504 */
    CHECK_EQ(rl_fp16_to_f64(0xc000), 0xc000000000000000ull);
#if defined(__FLT16_MAX__)
    for (uint32_t i = 0; i < 0x10000; i++) {
        _Float16 x;
        uint16_t h = (uint16_t)i;
        memcpy(&x, &h, 2);
        if (!nan16(h))
            CHECK_EQ(bits64((double)x), rl_fp16_to_f64(h));
    }
#endif
}

TEST(fp16_from_wider_rounding)
{
    CHECK_EQ(rl_fp16_from_f64(bits64(65504.0)), 0x7bff);
    CHECK_EQ(rl_fp16_from_f64(bits64(65519.99)), 0x7bff);
    CHECK_EQ(rl_fp16_from_f64(bits64(65520.0)), 0x7c00);
    CHECK_EQ(rl_fp16_from_f64(bits64(-1e300)), 0xfc00);
    CHECK_EQ(rl_fp16_from_f64(bits64(0x1p-25)), 0);              /* tie to even zero */
    CHECK_EQ(rl_fp16_from_f64(bits64(0x1p-25 * 1.0001)), 1);
    CHECK_EQ(rl_fp16_from_f64(bits64(3 * 0x1p-25)), 2);
    CHECK_EQ(rl_fp16_from_f64(bits64(-0x1p-26)), 0x8000);
    CHECK_EQ(rl_fp16_from_f64(1), 0);                             /* smallest binary64 subnormal */
    CHECK_EQ(rl_fp16_from_f64(bits64(0x1.ffcp-15)), 0x0400);      /* rounds up into the normals */
    /* A value just above a binary16 tie that a double-to-float step would round onto the tie. */
    double x = 1.0 + 0x1p-11 + 0x1p-40;
    CHECK_EQ(rl_fp16_from_f64(bits64(x)), 0x3c01);
    CHECK_EQ(rl_fp16_from_f32(0x3f801000u), 0x3c00);             /* 1 + 2^-11: tie to even */
    CHECK_EQ(rl_fp16_from_f32(0x3f801001u), 0x3c01);
    CHECK_EQ(rl_fp16_from_f32(0x7fc00001u), 0x7e00);
    CHECK_EQ(rl_fp16_from_f32(0xff800001u), 0xfe00);             /* signaling NaN quieted, sign kept */
    CHECK_EQ(rl_fp16_from_f64(0x7ff8000000000000ull | (0x155ull << 42)), 0x7f55);
#if defined(__FLT16_MAX__)
    /* The compiler's own conversions as a second reference, on random binary32 and binary64 values. */
    for (unsigned i = 0; i < 2000000; i++) {
        rng ^= rng << 13;
        rng ^= rng >> 7;
        rng ^= rng << 17;
        uint32_t f = (uint32_t)rng;
        /* Bias exponents toward binary16's range. */
        f = (f & 0x807fffffu) | ((uint32_t)(100 + (rng >> 40) % 60) << 23);
        _Float16 hf = (_Float16)flt(f);
        uint16_t hb;
        memcpy(&hb, &hf, 2);
        CHECK_EQ(rl_fp16_from_f32(f), hb);
        uint64_t d = (rng & 0x800fffffffffffffull) | ((uint64_t)(1000 + (rng >> 44) % 50) << 52);
        _Float16 hd = (_Float16)dbl(d);
        memcpy(&hb, &hd, 2);
        CHECK_EQ(rl_fp16_from_f64(d), hb);
    }
#endif
}

/** @brief The expected NaN of a two-operand operation, or 0 when neither operand is a NaN. */
static uint16_t want_nan(uint16_t a, uint16_t b) {
    if (nan16(a))
        return a | 0x200;
    if (nan16(b))
        return b | 0x200;
    return 0;
}

/** @brief The reference of op (0 add, 1 sub, 2 mul, 3 div) in binary64, rounded once. */
static uint16_t ref2(int op, uint16_t a, uint16_t b) {
    uint16_t n = want_nan(a, b);
    if (n)
        return n;
    double x = h2d(a), y = h2d(b), r;
    switch (op) {
    case 0: r = x + y; break;
    case 1: r = x - y; break;
    case 2: r = x * y; break;
    default: r = x / y; break;
    }
    if (r != r)
        return RL_FP16_DEFAULT_NAN;
    return rl_fp16_from_f64(bits64(r));
}

static uint16_t run2(int op, uint16_t a, uint16_t b) {
    switch (op) {
    case 0: return rl_fp16_add(a, b);
    case 1: return rl_fp16_sub(a, b);
    case 2: return rl_fp16_mul(a, b);
    default: return rl_fp16_div(a, b);
    }
}

/** @brief Second operands tried against every first operand. */
static const uint16_t edge16[] = {
    0x0000, 0x8000, 0x0001, 0x8001, 0x0002, 0x03ff, 0x83ff, 0x0400, 0x8400, 0x0401, 0x07ff, 0x3555,
    0x3bff, 0x3c00, 0xbc00, 0x3c01, 0x3e00, 0x4000, 0xc000, 0x4248, 0x5640, 0x6400, 0x77ff, 0x7800,
    0x7bfe, 0x7bff, 0xfbff, 0x7c00, 0xfc00, 0x7e00, 0x7d01, 0xff3f, 0x1000, 0x2000, 0x0c00, 0x1400,
    0x2e66, 0xb4cd, 0x47ff, 0x5bff,
};
#define NEDGE16 (sizeof edge16 / sizeof edge16[0])

TEST(fp16_arithmetic_against_binary64)
{
    unsigned long long n = 0;
    for (int op = 0; op < 4; op++) {
        for (uint32_t i = 0; i < 0x10000; i++)
            for (unsigned j = 0; j < NEDGE16; j++) {
                uint16_t a = (uint16_t)i, b = edge16[j];
                CHECK_EQ(run2(op, a, b), ref2(op, a, b));
                CHECK_EQ(run2(op, b, a), ref2(op, b, a));
                n += 2;
            }
        for (unsigned k = 0; k < 3000000; k++) {
            uint16_t a = rnd16(), b = rnd16();
            CHECK_EQ(run2(op, a, b), ref2(op, a, b));
            n++;
        }
    }
    /* Signs of exact zeros. */
    CHECK_EQ(rl_fp16_add(0x3c00, 0xbc00), 0x0000);
    CHECK_EQ(rl_fp16_add(0x8000, 0x8000), 0x8000);
    CHECK_EQ(rl_fp16_sub(0x8000, 0x0000), 0x8000);
    CHECK_EQ(rl_fp16_sub(0x7e01, 0x3c00), 0x7e01);
    CHECK_EQ(rl_fp16_sub(0x3c00, 0x7d01), 0x7f01); /* the NaN keeps its own sign */
    CHECK_EQ(rl_fp16_mul(0x7c00, 0x0000), RL_FP16_DEFAULT_NAN);
    printf("  %llu binary16 add/sub/mul/div cases matched\n", n);
    fflush(stdout);
}

/** @brief The binary16 value halfway between h and the next one up in magnitude (65536 past the largest). */
static double mid_up(uint16_t h) {
    double lo = h2d(h & 0x7fff);
    double hi = (h & 0x7fff) == 0x7bff ? 65536.0 : h2d((uint16_t)((h & 0x7fff) + 1));
    return (lo + hi) / 2;
}

TEST(fp16_sqrt_exhaustive)
{
    for (uint32_t i = 0; i < 0x10000; i++) {
        uint16_t a = (uint16_t)i, r = rl_fp16_sqrt(a);
        if (nan16(a)) {
            CHECK_EQ(r, a | 0x200);
        } else if (a == 0x8000 || a == 0 || a == 0x7c00) {
            CHECK_EQ(r, a);
        } else if (a & 0x8000) {
            CHECK_EQ(r, RL_FP16_DEFAULT_NAN);
        } else {
            /* r is the rounded root iff x lies strictly between the squares of the midpoints around r (an exact tie is impossible). */
            double x = h2d(a);
            CHECK(r < 0x7c00);
            double above = mid_up(r);
            double below = r == 0 ? 0.0 : mid_up((uint16_t)(r - 1));
            CHECK(below * below < x);
            CHECK(x < above * above);
        }
    }
    CHECK_EQ(rl_fp16_sqrt(0x4400), 0x4000); /* sqrt(4) */
    CHECK_EQ(rl_fp16_sqrt(0x0001), 0x0c00); /* sqrt(2^-24) = 2^-12 */
}

/** @brief An exact reference for round-to-integral: x is a binary16 value as a double. */
static double ref_rint(double x, int mode) {
    double ax = x < 0 ? -x : x;
    if (ax >= 1024)
        return x; /* already integral */
    double t = (double)(long long)ax; /* truncation */
    double fr = ax - t;
    int neg = x < 0;
    switch (mode) {
    case RL_RM_POSINF: t += (fr > 0 && !neg); break;
    case RL_RM_NEGINF: t += (fr > 0 && neg); break;
    case RL_RM_ZERO: break;
    case RL_RM_AWAY: t += fr >= 0.5; break;
    default: t += fr > 0.5 || (fr == 0.5 && ((long long)t & 1)); break;
    }
    return neg ? -t : t;
}

TEST(fp16_round_int_exhaustive)
{
    static const int modes[] = {RL_RM_NEAREST, RL_RM_POSINF, RL_RM_NEGINF, RL_RM_ZERO, RL_RM_AWAY};
    for (unsigned mi = 0; mi < 5; mi++)
        for (uint32_t i = 0; i < 0x10000; i++) {
            uint16_t a = (uint16_t)i, r = rl_fp16_round_int(a, modes[mi]);
            if (nan16(a)) {
                CHECK_EQ(r, a | 0x200);
                continue;
            }
            uint16_t want = rl_fp16_from_f64(bits64(ref_rint(h2d(a), modes[mi])));
            if ((want & 0x7fff) == 0)
                want = a & 0x8000; /* a zero result keeps the operand's sign */
            CHECK_EQ(r, want);
        }
    CHECK_EQ(rl_fp16_round_int(0x3800, RL_RM_NEAREST), 0x0000); /* 0.5 */
    CHECK_EQ(rl_fp16_round_int(0x3e00, RL_RM_NEAREST), 0x4000); /* 1.5 */
    CHECK_EQ(rl_fp16_round_int(0x4100, RL_RM_NEAREST), 0x4000); /* 2.5 */
    CHECK_EQ(rl_fp16_round_int(0xb800, RL_RM_AWAY), 0xbc00);    /* -0.5 */
    CHECK_EQ(rl_fp16_round_int(0xb400, RL_RM_POSINF), 0x8000);  /* -0.25 to -0 */
}

/** @brief The double after (up) or before x, for finite x. */
static double step(double x, int up) {
    uint64_t b = bits64(x);
    if (x == 0)
        return up ? dbl(1) : dbl(0x8000000000000001ull);
    if ((x > 0) == (up != 0))
        b++;
    else
        b--;
    return dbl(b);
}

/** @brief a * b + c for finite operands: the product is exact in binary64, TwoSum gives s + t exactly, and t breaks a tie at s. */
static uint16_t ref_fma(uint16_t a, uint16_t b, uint16_t c) {
    double p = h2d(a) * h2d(b), q = h2d(c);
    double s = p + q;
    double bp = s - q, bq = s - bp;
    double t = (p - bp) + (q - bq);
    if (s == 0 && t == 0) {
        /* An exact zero: -0 only when both terms are -0. */
        int np = (int)(bits64(p) >> 63), nq = (int)(bits64(q) >> 63);
        return (np && nq) ? 0x8000 : 0;
    }
    uint16_t r = rl_fp16_from_f64(bits64(s));
    if (t != 0) {
        /* If s is exactly a binary16 rounding tie, the sign of t decides. */
        double mag = s < 0 ? -s : s;
        uint16_t h = rl_fp16_from_f64(bits64(mag));
        int tie = mag == mid_up(h) || (h > 0 && mag == mid_up((uint16_t)(h - 1)));
        if (tie)
            r = rl_fp16_from_f64(bits64(step(s, t > 0)));
    }
    return r;
}

static int finite16(uint16_t h) {
    return (h & 0x7c00u) != 0x7c00u;
}

TEST(fp16_muladd_one_rounding)
{
    uint16_t a = rl_fp16_from_f64(bits64(683.0)), b = rl_fp16_from_f64(bits64(3.0));
    /* 683 * 3 = 2049 is a tie between 2048 and 2050; the addend's sign breaks it. */
    CHECK_EQ(h2d(rl_fp16_muladd(a, b, 0)), 2048.0);
    CHECK_EQ(h2d(rl_fp16_muladd(a, b, 0x0001)), 2050.0);
    CHECK_EQ(h2d(rl_fp16_muladd(a, b, 0x8001)), 2048.0);
    CHECK_EQ(rl_fp16_muladd(0x3c00, 0x3c00, 0x0001), 0x3c00);
    CHECK_EQ(rl_fp16_muladd(0x3c00, 0x3c00, 0xbc00), 0x0000);
    CHECK_EQ(rl_fp16_muladd(0x8000, 0x3c00, 0x8000), 0x8000);
    CHECK_EQ(rl_fp16_muladd(0x0000, 0x3c00, 0x8000), 0x0000);
    CHECK_EQ(rl_fp16_muladd(0x7c00, 0x0000, 0x3c00), RL_FP16_DEFAULT_NAN);
    CHECK_EQ(rl_fp16_muladd(0x7c00, 0x3c00, 0xfc00), RL_FP16_DEFAULT_NAN);
    CHECK_EQ(rl_fp16_muladd(0x3c00, 0x7d00, 0x7e01), 0x7f00);
    CHECK_EQ(rl_fp16_muladd(0x3c00, 0x3c00, 0xfc00), 0xfc00);
    /* Overflow of the product cancelled by the addend is still exact. */
    CHECK_EQ(rl_fp16_muladd(0x7bff, 0x4000, 0xfbff), 0x7bff);
    unsigned long long n = 0;
    for (unsigned k = 0; k < 4000000; k++) {
        uint16_t x = rnd16(), y = rnd16(), z = rnd16();
        if (k & 1) {
            /* Put the addend near the product so cancellation and ties occur. */
            int ez = ((x >> 10) & 0x1f) + ((y >> 10) & 0x1f) - 15 - (int)(k >> 1) % 13;
            if (ez >= 1 && ez <= 30)
                z = (uint16_t)((z & 0x83ff) | (ez << 10));
        }
        if (!finite16(x) || !finite16(y) || !finite16(z))
            continue;
        CHECK_EQ(rl_fp16_muladd(x, y, z), ref_fma(x, y, z));
        n++;
    }
    /*
     * Products that are exact binary16 ties, plus addends far below them:
     * only the addend's sign may break the tie, which a binary64 fma
     * followed by a second rounding gets wrong.
     */
    static const uint16_t odd[] = {0x4200, 0x4500, 0x4700, 0x3e00, 0x3d00, 0x3a00, 0x4880, 0x4980, 0x4a80};
    unsigned long long ties = 0;
    for (uint32_t i = 0; i < 0x7c00; i++)
        for (unsigned j = 0; j < sizeof odd / sizeof odd[0]; j++) {
            uint16_t x = (uint16_t)i, y = odd[j];
            double p = h2d(x) * h2d(y);
            uint16_t h = rl_fp16_from_f64(bits64(p));
            if (h >= 0x7c00 || !(p == mid_up(h) || (h > 0 && p == mid_up((uint16_t)(h - 1)))))
                continue;
            for (unsigned s = 0; s < 4; s++) {
                uint16_t tiny = (uint16_t)((s & 1 ? 0x8000 : 0) | (s & 2 ? 0x0001 : 0x0400));
                uint16_t xs = (uint16_t)(x | (rnd16() & 0x8000));
                CHECK_EQ(rl_fp16_muladd(xs, y, tiny), ref_fma(xs, y, tiny));
                ties++;
            }
        }
    CHECK(ties > 1000);
    printf("  %llu binary16 fused multiply-add cases matched, %llu on ties\n", n + ties, ties);
    fflush(stdout);
}

TEST(fp16_from_int_edges)
{
    CHECK_EQ(rl_fp16_from_int(65519, 32, 0), 0x7bff);
    CHECK_EQ(rl_fp16_from_int(65520, 32, 0), 0x7c00);
    CHECK_EQ(rl_fp16_from_int(65504, 64, 1), 0x7bff);
    CHECK_EQ(rl_fp16_from_int(0xffffffffu, 32, 0), 0xbc00); /* signed 32-bit -1 */
    CHECK_EQ(rl_fp16_from_int(0xffffffffu, 32, 1), 0x7c00);
    CHECK_EQ(rl_fp16_from_int(0xffffffffffffffffull, 64, 0), 0xbc00);
    CHECK_EQ(rl_fp16_from_int(0xffffffffffffffffull, 64, 1), 0x7c00);
    CHECK_EQ(rl_fp16_from_int(0x8000000000000000ull, 64, 0), 0xfc00);
    CHECK_EQ(rl_fp16_from_int(0x80000000u, 32, 0), 0xfc00);
    CHECK_EQ(rl_fp16_from_int(0x1234567800000000ull, 32, 0), 0x0000); /* high bits ignored */
    CHECK_EQ(rl_fp16_from_int(0, 64, 0), 0x0000);
    CHECK_EQ(rl_fp16_from_int(2049, 32, 0), 0x6800); /* tie to even 2048 */
    CHECK_EQ(rl_fp16_from_int(2051, 32, 0), 0x6802); /* tie to even 2052 */
    CHECK_EQ(rl_fp16_from_int((uint64_t)-2049, 64, 0), 0xe800);
    CHECK_EQ(rl_fp16_from_int(0x0000000100000801ull, 64, 1), 0x7c00);
    for (int64_t v = -70000; v <= 70000; v++) {
        uint16_t want = rl_fp16_from_f64(bits64((double)v));
        CHECK_EQ(rl_fp16_from_int((uint64_t)v, 64, 0), want);
        CHECK_EQ(rl_fp16_from_int((uint64_t)v, 32, 0), want);
        if (v >= 0)
            CHECK_EQ(rl_fp16_from_int((uint64_t)v, 32, 1), want);
    }
}

TEST(fp16_to_int_saturates)
{
    static const int modes[] = {RL_RM_NEAREST, RL_RM_POSINF, RL_RM_NEGINF, RL_RM_ZERO, RL_RM_AWAY};
    static const unsigned widths[] = {32, 64};
    /* Exhaustive against the FPCR-aware FPToFixed slow path in the default mode. */
    for (unsigned mi = 0; mi < 5; mi++)
        for (unsigned wi = 0; wi < 2; wi++)
            for (int u = 0; u < 2; u++)
                for (uint32_t i = 0; i < 0x10000; i++) {
                    uint16_t h = (uint16_t)i;
                    unsigned w = widths[wi];
                    uint64_t mask = w == 64 ? ~0ull : 0xffffffffull;
                    uint64_t want = rl_fp_to_fixed(h, 16, w, 0, u, (uint32_t)modes[mi], 0, NULL) & mask;
                    CHECK_EQ(rl_fp16_to_int(h, w, u, modes[mi]), want);
                }
    CHECK_EQ(rl_fp16_to_int(0x7e00, 32, 0, RL_RM_NEAREST), 0);
    CHECK_EQ(rl_fp16_to_int(0xfe00, 64, 1, RL_RM_NEAREST), 0);
    CHECK_EQ(rl_fp16_to_int(0x7c00, 32, 0, RL_RM_ZERO), 0x7fffffffull);
    CHECK_EQ(rl_fp16_to_int(0xfc00, 32, 0, RL_RM_ZERO), 0x80000000ull);
    CHECK_EQ(rl_fp16_to_int(0x7c00, 32, 1, RL_RM_ZERO), 0xffffffffull);
    CHECK_EQ(rl_fp16_to_int(0xfc00, 32, 1, RL_RM_ZERO), 0);
    CHECK_EQ(rl_fp16_to_int(0x7c00, 64, 0, RL_RM_ZERO), 0x7fffffffffffffffull);
    CHECK_EQ(rl_fp16_to_int(0xfc00, 64, 0, RL_RM_ZERO), 0x8000000000000000ull);
    CHECK_EQ(rl_fp16_to_int(0x7c00, 64, 1, RL_RM_ZERO), ~0ull);
    CHECK_EQ(rl_fp16_to_int(0x7bff, 32, 0, RL_RM_ZERO), 65504);
    CHECK_EQ(rl_fp16_to_int(0xfbff, 32, 0, RL_RM_ZERO), (uint64_t)(uint32_t)-65504);
    CHECK_EQ(rl_fp16_to_int(0xfbff, 64, 0, RL_RM_ZERO), (uint64_t)-65504);
    CHECK_EQ(rl_fp16_to_int(0xfbff, 64, 1, RL_RM_ZERO), 0);
    CHECK_EQ(rl_fp16_to_int(0xbc00, 64, 1, RL_RM_NEAREST), 0);           /* -1 unsigned saturates to 0 */
    CHECK_EQ(rl_fp16_to_int(0xb800, 32, 1, RL_RM_ZERO), 0);              /* -0.5 truncates to 0 */
    CHECK_EQ(rl_fp16_to_int(0xb800, 32, 0, RL_RM_AWAY), 0xffffffffull);  /* -0.5 away is -1 */
    CHECK_EQ(rl_fp16_to_int(0x3e00, 32, 0, RL_RM_NEAREST), 2);           /* 1.5 */
    CHECK_EQ(rl_fp16_to_int(0x4100, 32, 0, RL_RM_NEAREST), 2);           /* 2.5 */
    CHECK_EQ(rl_fp16_to_int(0x3c01, 32, 0, RL_RM_POSINF), 2);
    CHECK_EQ(rl_fp16_to_int(0xbc01, 32, 0, RL_RM_NEGINF), (uint64_t)(uint32_t)-2);
}
