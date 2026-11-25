/**
 * @file nvn_probe.c
 * @brief The inputs and checks of the device probes (probe.comp) that the
 * runtime runs before it writes device.json: whether the device's
 * single-precision NaNs are the SM86 canonical NaN, and whether its
 * half-precision arithmetic and conversions are exact binary16. The
 * Vulkan side is in nvn_vk.c; these parts need no device.
 */
#include "nvn_priv.h"

#include <math.h>
#include <string.h>

/** @brief Single-precision inputs of the NaN probe: payloads, signaling NaNs, infinities, zeros and denormals. */
static const uint32_t nan_vals[] = { 0x7fc00000, 0xffc00000, 0x7f800001, 0x7fbfffff, 0x7fc12345, 0xffd00001,
                                     0xff800001, 0x7fffffff, 0x7f800000, 0xff800000, 0x00000000, 0x80000000,
                                     0x3f800000, 0xbf800000, 0x00000001, 0x80000001, 0x7f7fffff };
#define NAN_VALS (uint32_t)(sizeof nan_vals / sizeof nan_vals[0])

#define GFMA_ROWS 8192u

uint32_t nvn_probe_gfma_rows(void) { return GFMA_ROWS; }

void nvn_probe_gfma_input(uint32_t *w)
{
    uint32_t x = 0x9e3779b9u;
    for (uint32_t i = 0; i < GFMA_ROWS; i++) {
        uint32_t r[3];
        for (int k = 0; k < 3; k++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            r[k] = x;
        }
        /* Normal numbers of moderate exponents, with c about the size of the product so the rounding of a * b shows. */
        w[3 * i] = (r[0] & 0x807fffffu) | (126u + r[0] % 5) << 23;
        w[3 * i + 1] = (r[1] & 0x807fffffu) | (126u + r[1] % 5) << 23;
        w[3 * i + 2] = (r[2] & 0x807fffffu) | (125u + r[2] % 7) << 23;
    }
}

bool nvn_probe_gfma_check(const uint32_t *w)
{
    uint32_t differ = 0;
    for (uint32_t i = 0; i < GFMA_ROWS; i++) {
        float a, b, c, got;
        memcpy(&a, w + 3 * i, 4);
        memcpy(&b, w + 3 * i + 1, 4);
        memcpy(&c, w + 3 * i + 2, 4);
        memcpy(&got, w + 3 * GFMA_ROWS + i, 4);
        volatile float p = a * b;
        float unfused = p + c, fused = fmaf(a, b, c);
        if (memcmp(&got, &fused, 4))
            return false;
        differ += memcmp(&unfused, &fused, 4) != 0;
    }
    return differ >= 64;
}

uint32_t nvn_probe_nan_rows(void) { return NAN_VALS * NAN_VALS * 2; }

void nvn_probe_nan_input(uint32_t *w)
{
    uint32_t rows = nvn_probe_nan_rows();
    for (uint32_t i = 0; i < rows; i++) {
        uint32_t k = i % (NAN_VALS * NAN_VALS);
        w[3 * i] = nan_vals[k / NAN_VALS];
        w[3 * i + 1] = nan_vals[k % NAN_VALS];
        w[3 * i + 2] = i < NAN_VALS * NAN_VALS ? 0x00000000u : nan_vals[(k * 7 + 3) % NAN_VALS];
    }
}

static bool is_nan32(uint32_t v) { return (v & 0x7fffffffu) > 0x7f800000u; }

bool nvn_probe_nan_check(const uint32_t *w)
{
    uint32_t rows = nvn_probe_nan_rows(), nans = 0;
    for (uint32_t i = 0; i < rows; i++) {
        const uint32_t *in = w + 3 * i, *out = w + 3 * rows + 5 * i;
        bool any = is_nan32(in[0]) || is_nan32(in[1]);
        for (uint32_t k = 0; k < 5; k++) {
            uint32_t v = out[k];
            if (is_nan32(v)) {
                nans++;
                if (v != 0x7fffffffu)
                    return false;
            } else if (k < 3 && any) {
                return false;
            }
        }
    }
    return nans > 0;
}

/** @brief Single-precision inputs of the denormal probe: zeros, denormals, the smallest normals, and values whose sums and products land near the normal range. */
static const uint32_t den_vals[] = { 0x00000000, 0x80000000, 0x00000001, 0x80000001, 0x00000002, 0x00400000,
                                     0x80400000, 0x007fffff, 0x807fffff, 0x00800000, 0x80800000, 0x00800001,
                                     0x00ffffff, 0x01000000, 0x3f800000, 0xbf800000, 0x3f800001, 0x3f000000,
                                     0x0d800000, 0x8d800000, 0x73800000, 0x7f7fffff, 0x40000000, 0x34000000 };
#define DEN_VALS (uint32_t)(sizeof den_vals / sizeof den_vals[0])

#define DEN_RANDOM 16384u

uint32_t nvn_probe_denorm_rows(void) { return DEN_VALS * DEN_VALS + DEN_RANDOM; }

void nvn_probe_denorm_input(uint32_t *w)
{
    for (uint32_t i = 0; i < DEN_VALS * DEN_VALS; i++) {
        w[2 * i] = den_vals[i / DEN_VALS];
        w[2 * i + 1] = den_vals[i % DEN_VALS];
    }
    nvn_probe_denorm_random(w + 2 * DEN_VALS * DEN_VALS, DEN_RANDOM, 0);
}

/**
 * @brief Writes rows random inputs of the denormal probe to w (two words each): bits from the whole range, from
 * the exponents at and just above zero (denormals and the smallest normals), and in pairs whose exponents sum to
 * about the smallest normal exponent (products at the edge of the normal range, a quarter of them with the mantissas that put the product just below the smallest normal number), from the seed.
 */
void nvn_probe_denorm_random(uint32_t *w, uint32_t rows, uint32_t seed)
{
    uint32_t x = seed * 2654435761u + 1;
    for (uint32_t i = 0; i < rows; i++) {
        uint32_t r[4];
        for (int k = 0; k < 4; k++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            r[k] = x;
        }
        uint32_t kind = r[0] % 4, a = r[1], b = r[2];
        if (kind == 1) {
            a = (a & 0x807fffffu) | (r[3] % 4) << 23;
            b = (b & 0x807fffffu) | (r[3] >> 4) % 4 << 23;
        } else if (kind >= 2) {
            uint32_t ea = 1 + r[3] % 127;
            int eb = 128 - (int)ea + (int)(r[3] >> 8) % 5 - 2;
            eb = eb < 0 ? 0 : eb;
            a = (a & 0x807fffffu) | ea << 23;
            b = (b & 0x807fffffu) | (uint32_t)eb << 23;
            if (kind == 3) {
                a |= 0x007fffffu;
                b &= 0xff800000u;
            }
            if (r[3] >> 12 & 1) {
                uint32_t t = a;
                a = b;
                b = t;
            }
        }
        w[2 * i] = a;
        w[2 * i + 1] = b;
    }
}

static float bits_float(uint32_t v)
{
    float f;
    memcpy(&f, &v, 4);
    return f;
}

static uint32_t float_bits(float f)
{
    uint32_t v;
    memcpy(&v, &f, 4);
    return v;
}

/** @brief x with a denormal read as the zero of its sign. */
static double flush32(float x)
{
    return fabsf(x) < 1.17549435e-38f ? copysign(0.0, (double)x) : (double)x;
}

/** @brief The SM86 `.FTZ` result bits for the exact double v: rounded to 24 bits with an unbounded exponent, then a result below the normal range is a signed zero. */
static uint32_t ftz_bits(double v)
{
    if (v != 0.0 && isfinite(v)) {
        int e;
        double m = frexp(v, &e);
        v = ldexp(nearbyint(ldexp(m, 24)), e - 24);
        if (fabs(v) < ldexp(1.0, -126))
            v = copysign(0.0, v);
    }
    return float_bits((float)v);
}

/** @brief The result of rounding v to the denormal grid and then flushing a denormal result to the zero of its sign. */
static uint32_t after_bits(double v)
{
    uint32_t r = float_bits((float)v);
    return (r & 0x7f800000u) == 0 ? r & 0x80000000u : r;
}

/** @brief The result bits of the multiplication or addition (op 1 or 0) of the bits a and b under denormal model (bit 0 flush before, 1 keep, 2 flush after). */
static uint32_t den_model(uint32_t model, uint32_t op, uint32_t a, uint32_t b)
{
    float fa = bits_float(a), fb = bits_float(b);
    double x = model == 1 ? (double)fa : flush32(fa), y = model == 1 ? (double)fb : flush32(fb);
    double v = op ? x * y : x + y;
    return model == 0 ? ftz_bits(v) : model == 1 ? float_bits((float)v) : after_bits(v);
}

/**
 * @brief What a translator correction of a multiplication result res (bits of a * b on the device) does: fix bit 0
 * gives a zero result the sign of the exclusive or of the operands' signs; bit 1 turns a result of exactly the
 * smallest normal number into zero when the product, rounded to 24 bits with an unbounded exponent, is below it (a
 * device that rounds to the denormal grid before it flushes). The translator computes that product as (a * 2^64) * b,
 * normal whenever the device's result is that number, and flushes every result whose scaled product is below
 * 2^-62, which on a device this fix suits changes only those.
 */
static uint32_t den_fix(uint32_t fix, uint32_t res, uint32_t a, uint32_t b)
{
    uint32_t sign = (a ^ b) & 0x80000000u;
    if (fix & 2 && (res & 0x7fffffffu) == 0x00800000u) {
        double p = ldexp(flush32(bits_float(a)) * flush32(bits_float(b)), 64);
        if (fabsf((float)p) < ldexpf(1.0f, -62))
            res = sign;
    }
    if (fix & 1 && (res & 0x7fffffffu) == 0)
        res = sign;
    return res;
}

/** @brief Whether the result bits got and want agree: equal, or both a NaN (the payload is no part of what this probe asks). */
static bool den_same(uint32_t got, uint32_t want) { return got == want || (is_nan32(got) && is_nan32(want)); }

uint32_t nvn_probe_denorm_check(const uint32_t *w, uint32_t rows, nvn_denorm_class *out)
{
    uint32_t add = 7, mul = 7, fixes = 0xf;
    for (uint32_t i = 0; i < rows; i++) {
        uint32_t a = w[2 * i], b = w[2 * i + 1];
        const uint32_t *o = w + 2 * rows + 2 * i;
        for (uint32_t m = 0; m < 3; m++) {
            if (!den_same(o[0], den_model(m, 0, a, b)))
                add &= ~(1u << m);
            if (!den_same(o[1], den_model(m, 1, a, b)))
                mul &= ~(1u << m);
        }
        uint32_t want = den_model(0, 1, a, b);
        for (uint32_t f = 0; f < 4; f++)
            if (!den_same(den_fix(f, o[1], a, b), want))
                fixes &= ~(1u << f);
    }
    out->add_models = add;
    out->mul_models = mul;
    out->mul_fix = fixes ? (uint32_t)__builtin_ctz(fixes) : 255;
    out->mul_fix_ok = fixes;
    return add & mul & 1 ? 1 : add & mul & 2 ? 2 : 0;
}

double nvn_half_value(uint16_t h)
{
    int e = h >> 10 & 31, m = h & 0x3ff;
    double v = e ? ldexp(1024 + m, e - 25) : ldexp(m, -24);
    if (e == 31)
        v = m ? NAN : INFINITY;
    return h & 0x8000 ? -v : v;
}

uint16_t nvn_half_of(double x)
{
    uint16_t s = signbit(x) ? 0x8000 : 0;
    double a = fabs(x);
    if (isnan(x))
        return 0x7e00;
    if (a >= 65520.0)
        return s | 0x7c00;
    if (a < ldexp(1.0, -14))
        return s | (uint16_t)nearbyint(ldexp(a, 24));
    int e;
    double f = frexp(a, &e);
    double m = nearbyint(ldexp(f, 11));
    if (m == 2048.0) {
        m = 1024.0;
        e++;
    }
    if (e + 14 >= 31)
        return s | 0x7c00;
    return s | (uint16_t)((e + 14) << 10 | ((uint32_t)m - 1024));
}

/** @brief Half-precision inputs: zeros, the smallest and largest subnormals and normals, ties and infinities. */
static const uint16_t half_vals[] = { 0x0000, 0x8000, 0x0001, 0x8001, 0x03ff, 0x0400, 0x0401, 0x3c00, 0xbc00,
                                      0x3c01, 0x3555, 0x1400, 0x0200, 0x7bff, 0xfbff, 0x5bff, 0x7c00, 0xfc00,
                                      0x3e00, 0x4248, 0x2e66, 0x8401, 0x1001, 0x6800 };
#define HALF_VALS (uint32_t)(sizeof half_vals / sizeof half_vals[0])
#define HALF_RANDOM 4096u

static uint32_t mix(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    return x ^ x >> 16;
}

static uint16_t half_random(uint32_t i)
{
    uint16_t h = (uint16_t)mix(i);
    return (h & 0x7c00) == 0x7c00 ? (uint16_t)(h ^ 0x4000) : h;
}

uint32_t nvn_probe_f16_rows(void) { return (HALF_VALS * HALF_VALS + HALF_RANDOM + 1) / 2; }

uint32_t nvn_probe_f16_convs(void) { return 4096; }

uint32_t nvn_probe_f16_words(void) { return 6 * nvn_probe_f16_rows() + 2 * nvn_probe_f16_convs() + 3 * 32768; }

/** @brief Lane j of the half arithmetic probe: a, b and c. */
static void half_lane(uint32_t j, uint16_t *a, uint16_t *b, uint16_t *c)
{
    if (j < HALF_VALS * HALF_VALS) {
        *a = half_vals[j / HALF_VALS];
        *b = half_vals[j % HALF_VALS];
        *c = half_vals[(j * 5 + 1) % HALF_VALS];
    } else {
        *a = half_random(3 * j);
        *b = half_random(3 * j + 1);
        *c = half_random(3 * j + 2);
    }
}

/** @brief Single-precision inputs of the conversion to half: around the subnormal, rounding and overflow edges. */
static uint32_t conv_input(uint32_t i)
{
    static const uint32_t edge[] = { 0x00000000, 0x80000000, 0x00000001, 0x807fffff, 0x33000000, 0x33000001,
                                     0x337fffff, 0x33800000, 0x33c00000, 0x387fc000, 0x387fe000, 0x38800000,
                                     0x3f801000, 0x3f802000, 0x3f803000, 0x477fefff, 0x477ff000, 0x477fe000,
                                     0x47800000, 0x7f800000, 0xff800000, 0x7f7fffff, 0x3eaaaaab, 0xc2f6e979 };
    enum { EDGES = sizeof edge / sizeof edge[0] };
    if (i < EDGES)
        return edge[i];
    uint32_t r = mix(i * 0x9e3779b9u);
    uint32_t e = 102 + (r >> 24) % 42;
    return (r & 0x80000000u) | e << 23 | (r & 0x7fffffu);
}

void nvn_probe_f16_input(uint32_t *w)
{
    uint32_t n = nvn_probe_f16_rows(), m = nvn_probe_f16_convs();
    for (uint32_t i = 0; i < n; i++) {
        uint16_t a[2], b[2], c[2];
        half_lane(2 * i, &a[0], &b[0], &c[0]);
        half_lane(2 * i + 1, &a[1], &b[1], &c[1]);
        w[3 * i] = a[0] | (uint32_t)a[1] << 16;
        w[3 * i + 1] = b[0] | (uint32_t)b[1] << 16;
        w[3 * i + 2] = c[0] | (uint32_t)c[1] << 16;
    }
    for (uint32_t i = 0; i < m; i++)
        w[6 * n + i] = conv_input(i);
    for (uint32_t i = 0; i < 32768; i++)
        w[6 * n + 2 * m + i] = 2 * i | (2 * i + 1) << 16;
}

static bool is_nan16(uint16_t h) { return (h & 0x7fff) > 0x7c00; }

/**
 * @brief Whether the device half `got` is the binary16 rounding of the
 * exact `x`; when x is a NaN, any NaN (the translator writes the guest's
 * NaN itself).
 */
static bool half_ok(uint16_t got, double x)
{
    if (isnan(x))
        return is_nan16(got);
    return got == nvn_half_of(x);
}

/** @brief a * b + c exactly in a double, or false when it does not fit one (the lane is skipped). */
static bool fma_exact(double a, double b, double c, double *r)
{
    double p = a * b, s = p + c;
    if (!isfinite(p) || !isfinite(c)) {
        *r = s;
        return true;
    }
    double bp = s - c, bc = s - bp;
    *r = s;
    return (p - bp) + (c - bc) == 0.0;
}

uint32_t nvn_probe_f16_check(const uint32_t *w)
{
    uint32_t n = nvn_probe_f16_rows(), m = nvn_probe_f16_convs(), bad = 0;
    const uint32_t *out = w + 3 * n;
    for (uint32_t i = 0; i < 2 * n; i++) {
        uint16_t a, b, c;
        half_lane(i, &a, &b, &c);
        double x = nvn_half_value(a), y = nvn_half_value(b), z = nvn_half_value(c), f;
        unsigned sh = (i & 1) * 16;
        const uint32_t *o = out + 3 * (i / 2);
        bad += !half_ok((uint16_t)(o[0] >> sh), x + y);
        bad += !half_ok((uint16_t)(o[1] >> sh), x * y);
        if (fma_exact(x, y, z, &f))
            bad += !half_ok((uint16_t)(o[2] >> sh), f);
    }
    for (uint32_t i = 0; i < m; i++) {
        uint32_t v = w[6 * n + i];
        float x;
        memcpy(&x, &v, sizeof x);
        bad += !half_ok((uint16_t)w[6 * n + m + i], (double)x);
    }
    for (uint32_t h = 0; h < 65536; h++) {
        uint32_t got = w[6 * n + 2 * m + 32768 + h];
        double want = nvn_half_value((uint16_t)h);
        if (isnan(want)) {
            bad += !is_nan32(got);
            continue;
        }
        float wf = (float)want;
        uint32_t wb;
        memcpy(&wb, &wf, sizeof wb);
        bad += got != wb;
    }
    return bad;
}

/** @brief Exponent the HMMA model gives a step without a nonzero term. */
#define HMMA_NO_EXP (-64)

/** @brief Sign, biased exponent (1 for zeros and denormals) and significand with the implicit bit of a half. */
typedef struct hmma_half {
    uint32_t sign, sig;
    int32_t exp;
    bool nan, inf, zero;
} hmma_half;

static hmma_half hmma_parts(uint32_t h)
{
    uint32_t e = h >> 10 & 31, m = h & 1023;
    return (hmma_half){ .sign = h >> 15 & 1,
                        .sig = e ? m | 1024 : m,
                        .exp = e ? (int32_t)e : 1,
                        .nan = e == 31 && m,
                        .inf = e == 31 && !m,
                        .zero = !(h & 0x7fff) };
}

static uint32_t umin32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static int32_t msb32(uint32_t v)
{
    int32_t l = -1;
    for (; v; v >>= 1)
        l++;
    return l;
}

/**
 * @brief The exact sum `sum` (two's complement) of a step's terms on the
 * grid 24 bits below exponent `big`, rounded to half precision to nearest
 * even, a zero being +0: the translator's dot8_round, shift clamps included.
 */
static uint32_t hmma_round(uint32_t sum, int32_t big)
{
    bool neg = (int32_t)sum < 0;
    uint32_t mag = neg ? 0u - sum : sum, q;
    int32_t ev = msb32(mag) + big - 24, eq = ev > 1 ? ev : 1, sh = eq - big + 14;
    if (sh > 0) {
        uint32_t rs = umin32((uint32_t)sh, 31), one = 1u << rs, rem = mag & (one - 1), qd = mag >> rs;
        q = qd + (rem > one >> 1 || (rem == one >> 1 && (qd & 1)));
    } else {
        q = mag << umin32(0u - (uint32_t)sh, 31);
    }
    if (!q)
        return 0;
    uint32_t bits = umin32(((uint32_t)(eq - 1) << 10) + q, 0x7c00);
    return neg ? bits | 0x8000 : bits;
}

uint32_t nvn_hmma_step(uint32_t acc, const uint16_t a[8], const uint16_t b[8])
{
    bool nan = false, pos = false, neg = false;
    int32_t big = HMMA_NO_EXP, exp[9], lead[9];
    uint32_t sign[9], sig[9];
    for (int k = 0; k < 8; k++) {
        hmma_half x = hmma_parts(a[k]), y = hmma_parts(b[k]);
        bool pn = x.nan || y.nan || (x.inf && y.zero) || (x.zero && y.inf);
        bool pi = (x.inf || y.inf) && !pn;
        sign[k] = x.sign ^ y.sign;
        exp[k] = x.exp + y.exp - 15;
        sig[k] = x.sig * y.sig;
        lead[k] = 4;
        nan |= pn;
        pos |= pi && !sign[k];
        neg |= pi && sign[k];
        if (sig[k] && exp[k] > big)
            big = exp[k];
    }
    hmma_half c = hmma_parts(acc);
    nan |= c.nan;
    pos |= c.inf && !c.sign;
    neg |= c.inf && c.sign;
    nan |= pos && neg;
    if (c.sig && c.exp > big)
        big = c.exp;
    sign[8] = c.sign, exp[8] = c.exp, sig[8] = c.sig, lead[8] = 14;
    uint32_t sum = 0;
    for (int k = 0; k < 9; k++) {
        int32_t left = lead[k] - (big - exp[k]);
        uint32_t right = 0u - (uint32_t)left, v;
        if (left > 0)
            v = sig[k] << umin32((uint32_t)left, 31);
        else
            v = right >= 32 ? 0 : sig[k] >> right;
        sum += sign[k] ? 0u - v : v;
    }
    uint32_t r = hmma_round(sum, big);
    if (pos)
        r = 0x7c00;
    if (neg)
        r = 0xfc00;
    return nan ? 0x7fff : r;
}

void nvn_hmma_warp(bool k16, const uint32_t *a, const uint32_t *b, const uint32_t *c, uint32_t *d)
{
    for (int l = 0; l < 32; l++) {
        int g = l >> 2, q = l & 3;
        for (int row = 0; row < 2; row++) {
            uint32_t out = 0;
            for (int h = 0; h < 2; h++) {
                int col = 2 * q + h;
                uint32_t r = c[2 * l + row] >> (16 * h) & 0xffff;
                for (int s = 0; s < (k16 ? 2 : 1); s++) {
                    uint16_t av[8], bv[8];
                    for (int j = 0; j < 4; j++) {
                        uint32_t aw = a[4 * (4 * g + j) + row + 2 * s], bw = b[2 * (4 * col + j) + s];
                        av[2 * j] = (uint16_t)aw;
                        av[2 * j + 1] = (uint16_t)(aw >> 16);
                        bv[2 * j] = (uint16_t)bw;
                        bv[2 * j + 1] = (uint16_t)(bw >> 16);
                    }
                    r = nvn_hmma_step(r, av, bv);
                }
                out |= r << (16 * h);
            }
            d[2 * l + row] = out;
        }
    }
}

/** @brief Tiles of each shape in the HMMA probe. */
#define HMMA_TILES 64u

uint32_t nvn_probe_hmma_tiles(void) { return HMMA_TILES; }

uint32_t nvn_probe_hmma_words(void) { return 256 * HMMA_TILES + 160 * HMMA_TILES + 64 * 2 * HMMA_TILES; }

/**
 * @brief Half k of the HMMA probe's inputs: normal halves between 2^-14
 * and 2^8 (even tiles) or 2^-4 and 2^6 (odd tiles), so that cancellation,
 * the grid cut and denormal results all show, with about one in 97 a
 * zero, a denormal, an infinity, a NaN or the largest half.
 */
static uint16_t hmma_input(uint32_t tile, uint32_t k)
{
    static const uint16_t special[] = { 0x0000, 0x8000, 0x0001, 0x83ff, 0x7c00, 0xfc00, 0x7e00, 0x7bff };
    uint32_t r = mix(tile * 0x10001u + k * 0x9e3779b9u + 0x1234567u);
    if (r % 97 == 0)
        return special[r >> 8 & 7];
    uint32_t e = tile & 1 ? 11 + (r >> 24) % 10 : 1 + (r >> 24) % 22;
    return (uint16_t)((r >> 8 & 0x83ff) | e << 10);
}

void nvn_probe_hmma_input(uint32_t *w)
{
    for (uint32_t t = 0; t < 2 * HMMA_TILES; t++) {
        uint32_t per = t < HMMA_TILES ? 8 : 5, base = t < HMMA_TILES ? 256 * t : 256 * HMMA_TILES + 160 * (t - HMMA_TILES);
        for (uint32_t k = 0; k < 32 * per; k++)
            w[base + k] = hmma_input(t, 2 * k) | (uint32_t)hmma_input(t, 2 * k + 1) << 16;
    }
}

uint32_t nvn_probe_hmma_check(const uint32_t *w)
{
    uint32_t bad = 0;
    const uint32_t *out = w + 256 * HMMA_TILES + 160 * HMMA_TILES;
    for (uint32_t t = 0; t < 2 * HMMA_TILES; t++) {
        bool k16 = t < HMMA_TILES;
        uint32_t per = k16 ? 8 : 5;
        const uint32_t *in = w + (k16 ? 256 * t : 256 * HMMA_TILES + 160 * (t - HMMA_TILES));
        uint32_t a[128] = { 0 }, b[64], c[64], d[64];
        for (int l = 0; l < 32; l++) {
            const uint32_t *x = in + per * l;
            for (int r = 0; r < (k16 ? 4 : 2); r++)
                a[4 * l + r] = x[r];
            b[2 * l] = x[k16 ? 4 : 2];
            b[2 * l + 1] = k16 ? x[5] : 0;
            c[2 * l] = x[k16 ? 6 : 3];
            c[2 * l + 1] = x[k16 ? 7 : 4];
        }
        nvn_hmma_warp(k16, a, b, c, d);
        for (int k = 0; k < 64; k++)
            bad += out[64 * t + k] != d[k];
    }
    return bad;
}
