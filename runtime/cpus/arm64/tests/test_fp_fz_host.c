/**
 * @file test_fp_fz_host.c
 * @brief Whether the host FPU, with its own flush-to-zero modes, gives the results the exact slow path (rl_fp_slow, itself
 * checked against Arm hardware) gives under FPCR.FZ: a game that runs with FZ set could then keep lifted FP code on the
 * fast path.
 *
 * The A64 rule: FZ flushes a denormal input to zero, and a result whose exponent is below the smallest normal before rounding
 * to zero. x86's MXCSR.DAZ flushes denormal inputs and MXCSR.FTZ a tiny result, with tininess detected after rounding, which
 * differs exactly where a result below the smallest normal rounds up to it. The operands here cluster at that boundary on
 * purpose (exponents near the smallest normal, mantissas of all ones, all zeros and random), for add, subtract, multiply,
 * divide, square root and fused multiply-add in single and double precision.
 */
#include "arm64.h"
#include "runner.h"

#include <relift/arm64/fp_slow.h>

#if defined(__x86_64__)
#include <immintrin.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MXCSR_FTZ 0x8000u
#define MXCSR_DAZ 0x0040u

static const rl_guest_desc test_guest = {
    .name = "arm64-fp-fz-host-test",
    .page_size = 0x4000,
    .address_bits = 40,
    .tick_hz = 1000000,
};

static uint64_t g_rng = 0x9e3779b97f4a7c15ull;

static uint64_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

/** @brief A single-precision pattern whose exponent is near the smallest normal (biased 1), or special. */
static uint32_t pick_s(void)
{
    uint32_t sign = (uint32_t)(rnd() & 1) << 31;
    uint32_t mant;
    switch (rnd() % 4) {
    case 0: mant = 0; break;
    case 1: mant = 0x7FFFFFu; break;
    case 2: mant = 0x7FFFFFu - (uint32_t)(rnd() % 4); break;
    default: mant = (uint32_t)rnd() & 0x7FFFFFu;
    }
    uint32_t e = (uint32_t)(rnd() % 14);
    if (rnd() % 6 == 0)
        e = 0x7E + (uint32_t)(rnd() % 4);
    if (rnd() % 8 == 0)
        e = 0x80 + (uint32_t)(rnd() % 40);
    return sign | e << 23 | mant;
}

static uint64_t pick_d(void)
{
    uint64_t sign = (rnd() & 1) << 63;
    uint64_t mant;
    switch (rnd() % 4) {
    case 0: mant = 0; break;
    case 1: mant = 0xFFFFFFFFFFFFFull; break;
    case 2: mant = 0xFFFFFFFFFFFFFull - (rnd() % 4); break;
    default: mant = rnd() & 0xFFFFFFFFFFFFFull;
    }
    uint64_t e = rnd() % 14;
    if (rnd() % 6 == 0)
        e = 0x3FE + rnd() % 4;
    if (rnd() % 8 == 0)
        e = 0x400 + rnd() % 400;
    return sign | e << 52 | mant;
}

static float f32(uint32_t b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static uint32_t u32(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

static double f64(uint64_t b)
{
    double f;
    memcpy(&f, &b, 8);
    return f;
}

static uint64_t u64(double f)
{
    uint64_t b;
    memcpy(&b, &f, 8);
    return b;
}

static bool nan_s(uint32_t b) { return (b & 0x7F800000u) == 0x7F800000u && (b & 0x7FFFFFu); }
static bool nan_d(uint64_t b) { return (b & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (b & 0xFFFFFFFFFFFFFull); }

__attribute__((target("fma"))) static uint32_t fma_s(uint32_t a, uint32_t b, uint32_t c)
{
    return u32(_mm_cvtss_f32(_mm_fmadd_ss(_mm_set_ss(f32(a)), _mm_set_ss(f32(b)), _mm_set_ss(f32(c)))));
}

__attribute__((target("fma"))) static uint64_t fma_d(uint64_t a, uint64_t b, uint64_t c)
{
    return u64(_mm_cvtsd_f64(_mm_fmadd_sd(_mm_set_sd(f64(a)), _mm_set_sd(f64(b)), _mm_set_sd(f64(c)))));
}

static rl_ctx *boot(void)
{
    if (rl_core_init(&test_guest, rl_arm64_cpu()) != 0)
        return NULL;
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return NULL;
    rl_thread_bind(t);
    t->ctx->fpcr = RL_FPCR_FZ;
    return t->ctx;
}

enum { OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_SQRT, OP_FMA, OPS };
static const char *const op_name[OPS] = { "fadd", "fsub", "fmul", "fdiv", "fsqrt", "fmadd" };
static const uint32_t op_insn_s[OPS] = { 0x1E222820u, 0x1E223820u, 0x1E220820u, 0x1E221820u, 0x1E21C020u, 0x1F020C20u };

/** @brief The host's result of op on a, b and k (single or double bits) in the host mode csr. */
static uint64_t host_op(int op, bool dbl, uint64_t a, uint64_t b, uint64_t k, uint32_t csr)
{
    uint32_t save = _mm_getcsr();
    _mm_setcsr(csr);
    uint64_t got;
    if (dbl) {
        double x = f64(a), y = f64(b);
        switch (op) {
        case OP_ADD: got = u64(x + y); break;
        case OP_SUB: got = u64(x - y); break;
        case OP_MUL: got = u64(x * y); break;
        case OP_DIV: got = u64(x / y); break;
        case OP_SQRT: got = u64(_mm_cvtsd_f64(_mm_sqrt_sd(_mm_setzero_pd(), _mm_set_sd(x)))); break;
        default: got = fma_d(a, b, k); break;
        }
    } else {
        float x = f32((uint32_t)a), y = f32((uint32_t)b);
        switch (op) {
        case OP_ADD: got = u32(x + y); break;
        case OP_SUB: got = u32(x - y); break;
        case OP_MUL: got = u32(x * y); break;
        case OP_DIV: got = u32(x / y); break;
        case OP_SQRT: got = u32(_mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x)))); break;
        default: got = fma_s((uint32_t)a, (uint32_t)b, (uint32_t)k); break;
        }
    }
    _mm_setcsr(save);
    return got;
}

/**
 * @brief The host's result with the fix-up that makes it A64's: when a multiply, divide or fused multiply-add gives exactly the
 * smallest normal, the exact result may have been below it before rounding (A64 flushes that to zero, the host rounded it up
 * first); the same operation rounded toward zero tells (it then comes out below the smallest normal, flushed to a signed zero).
 */
static uint64_t host_fixed(int op, bool dbl, uint64_t a, uint64_t b, uint64_t k, uint32_t csr, bool fix)
{
    uint64_t got = host_op(op, dbl, a, b, k, csr | MXCSR_FTZ | MXCSR_DAZ);
    uint64_t min_normal_abs = dbl ? 0x0010000000000000ull : 0x00800000ull, abs_mask = dbl ? 0x7FFFFFFFFFFFFFFFull : 0x7FFFFFFFull;
    if (fix && (op == OP_MUL || op == OP_DIV || op == OP_FMA) && (got & abs_mask) == min_normal_abs) {
        uint64_t rz = host_op(op, dbl, a, b, k, (csr & ~0x6000u) | 0x6000u | MXCSR_FTZ | MXCSR_DAZ);
        if ((rz & abs_mask) < min_normal_abs)
            got = rz;
    }
    return got;
}

/** @brief A pair whose exact product or quotient lies just under the smallest normal, to land on the rounding boundary. */
static void boundary_pair(bool dbl, int op, uint64_t *a, uint64_t *b)
{
    if (dbl) {
        double x = f64(0x3FE0000000000000ull | (rnd() & 0xFFFFFFFFFFFFFull)); /* [0.5, 1) */
        double target = 0x1p-1022;
        double y = op == OP_MUL ? target / x : target * x;
        uint64_t yb = u64(y) + (rnd() % 5) - 2;
        *a = u64(x);
        *b = op == OP_MUL ? yb : yb;
        if (op == OP_DIV) {
            *a = yb; /* a / b with b = x: a ~ target * x */
            *b = u64(x);
        }
    } else {
        float x = f32(0x3F000000u | ((uint32_t)rnd() & 0x7FFFFFu));
        float target = 0x1p-126f;
        float y = op == OP_MUL ? target / x : target * x;
        uint32_t yb = u32(y) + (uint32_t)(rnd() % 5) - 2;
        *a = u32(x);
        *b = yb;
        if (op == OP_DIV) {
            *a = yb;
            *b = u32(x);
        }
    }
}

/** @brief Counts the cases where the host in FTZ and DAZ (with the fix-up when fix is set) differs from rl_fp_slow under FZ. */
static uint64_t compare(rl_ctx *c, int op, bool dbl, uint64_t cases, bool fix, uint64_t *first_a, uint64_t *first_b)
{
    uint64_t bad = 0;
    uint32_t csr = _mm_getcsr();
    uint32_t insn = op_insn_s[op] | (dbl ? 0x00400000u : 0);
    for (uint64_t n = 0; n < cases; n++) {
        uint64_t a = dbl ? pick_d() : pick_s(), b = dbl ? pick_d() : pick_s(), k = dbl ? pick_d() : pick_s();
        if ((op == OP_MUL || op == OP_DIV) && n % 2)
            boundary_pair(dbl, op, &a, &b);
        if (dbl ? (nan_d(a) || nan_d(b) || nan_d(k)) : (nan_s(a) || nan_s(b) || nan_s(k)))
            continue;
        c->v[1].q = 0;
        c->v[2].q = 0;
        c->v[3].q = 0;
        c->v[0].q = 0;
        if (dbl) {
            c->v[1].d[0] = a;
            c->v[2].d[0] = b;
            c->v[3].d[0] = k;
        } else {
            c->v[1].s[0] = (uint32_t)a;
            c->v[2].s[0] = (uint32_t)b;
            c->v[3].s[0] = (uint32_t)k;
        }
        if (rl_fp_slow(c, rl_mem_base(), 0x10000, insn) != RL_OK)
            return ~0ull;
        uint64_t want = dbl ? c->v[0].d[0] : c->v[0].s[0];
        uint64_t got = host_fixed(op, dbl, a, b, k, csr, fix);
        bool nan = dbl ? nan_d(got) && nan_d(want) : nan_s((uint32_t)got) && nan_s((uint32_t)want);
        if (got != want && !nan) {
            if (!bad) {
                *first_a = a;
                *first_b = b;
            }
            if (bad < 6 && getenv("RELIFT_FZ_TEST_VERBOSE"))
                printf("    %s %s a=0x%llx b=0x%llx k=0x%llx: exact path 0x%llx, host 0x%llx\n", op_name[op], dbl ? "d" : "s",
                       (unsigned long long)a, (unsigned long long)b, (unsigned long long)k, (unsigned long long)want,
                       (unsigned long long)got);
            bad++;
        }
    }
    return bad;
}

TEST(host_flush_to_zero_differs_from_a64_fz_only_where_a_tiny_result_rounds_up_to_the_smallest_normal)
{
    if (!__builtin_cpu_supports("fma")) {
        printf("  skipped: no host FMA\n");
        return;
    }
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint64_t plain = 0, fixed_total = 0;
    for (int dbl = 0; dbl < 2; dbl++)
        for (int op = 0; op < OPS; op++) {
            uint64_t a = 0, b = 0;
            uint64_t bad_plain = compare(c, op, dbl, 400000, false, &a, &b);
            uint64_t bad_fixed = compare(c, op, dbl, 400000, true, &a, &b);
            printf("  %s %s: %llu differ with the host's own flush, %llu with the fix-up\n", op_name[op], dbl ? "double" : "single",
                   (unsigned long long)bad_plain, (unsigned long long)bad_fixed);
            plain += bad_plain == ~0ull ? 1 : bad_plain;
            fixed_total += bad_fixed == ~0ull ? 1 : bad_fixed;
        }
    /* the host alone is wrong somewhere (the claim this test rests on); with the fix-up it is never wrong */
    CHECK(plain > 0);
    CHECK_EQ(fixed_total, 0u);
}
#endif
