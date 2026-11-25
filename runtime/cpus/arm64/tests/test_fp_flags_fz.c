/**
 * @file test_fp_flags_fz.c
 * @brief test_fp_flags.c with FPCR.FZ set: in flag-producing programs the inline operations must give the result and the
 * cumulative FPSR flags (IDC included) of the exact layer under FZ, on x86-64 through the host's default mode with the
 * exact path for denormals and tiny results (rl_fz_slow), and on arm64 through the host's own FZ.
 */
#define RL_FP_FLAGS 1
#pragma STDC FENV_ACCESS ON

#include "arm64.h"
#include "runner.h"

static uint64_t g_rng = 0x243F6A8885A308D3ull;

static uint64_t next(void)
{
    uint64_t z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* Operands that reach every flag: zeros, ones, infinities, NaNs of both
 * kinds, the smallest normals and subnormals, values near overflow, and
 * values whose products and quotients land at the smallest normal. */
static uint64_t pick64(void)
{
    static const uint64_t special[] = {
        0, 0x8000000000000000ull, 0x3FF0000000000000ull, 0xBFF0000000000000ull, 0x7FF0000000000000ull,
        0xFFF0000000000000ull, 0x7FF8000000000001ull, 0x7FF0000000000001ull, 0x0010000000000000ull,
        0x000FFFFFFFFFFFFFull, 0x0000000000000001ull, 0x7FEFFFFFFFFFFFFFull, 0x3FE0000000000000ull,
        0x0020000000000000ull, 0x001FFFFFFFFFFFFFull, 0x3FEFFFFFFFFFFFFFull, 0x3CA0000000000000ull,
        0x4000000000000000ull, 0x000FFFFFFFFFFFFEull,
    };
    uint64_t r = next();
    switch (r % 4) {
    case 0: return special[(r >> 8) % (sizeof special / sizeof special[0])];
    case 1: return special[(r >> 8) % (sizeof special / sizeof special[0])] ^ ((r >> 32) & 0xF);
    case 2: return (r & 0x800FFFFFFFFFFFFFull) | ((uint64_t)((r >> 52) % 4) << 52);
    default: return r;
    }
}

static uint32_t pick32(void)
{
    static const uint32_t special[] = {
        0, 0x80000000u, 0x3F800000u, 0xBF800000u, 0x7F800000u, 0xFF800000u, 0x7FC00001u, 0x7F800001u,
        0x00800000u, 0x007FFFFFu, 0x00000001u, 0x7F7FFFFFu, 0x3F000000u, 0x01000000u, 0x00FFFFFFu,
        0x3F7FFFFFu, 0x33800000u, 0x40000000u, 0x007FFFFEu,
    };
    uint64_t r = next();
    switch (r % 4) {
    case 0: return special[(r >> 8) % (sizeof special / sizeof special[0])];
    case 1: return special[(r >> 8) % (sizeof special / sizeof special[0])] ^ (uint32_t)((r >> 32) & 0xF);
    case 2: return ((uint32_t)r & 0x807FFFFFu) | ((uint32_t)((r >> 40) % 4) << 23);
    default: return (uint32_t)r;
    }
}

#define N 400000

#define CHECK_OP(name, fast, slow, T, pick)                                                              \
    TEST(name)                                                                                           \
    {                                                                                                    \
        rl_arm64_set_fp_flags(1);\
        rl_host_set_fpcr(RL_FPCR_FZ);                                                                          \
        unsigned bad = 0;                                                                                \
        for (unsigned i = 0; i < N; i++) {                                                               \
            T a = pick(), b = pick();                                                                    \
            uint32_t want = 0;                                                                           \
            T ref = slow(a, b, RL_FPCR_FZ, &want);                                                                \
            rl_fp_host_clear();                                                                          \
            T got = fast(a, b, RL_FPCR_FZ, 0);                                                                    \
            uint32_t flags = rl_fp_host_flags() & 0x9Fu;                                                 \
            if (got != ref || flags != want) {                                                           \
                if (bad++ < 5)                                                                           \
                    fprintf(stderr, "  %s(%#llx, %#llx): %#llx flags %#x, exact %#llx flags %#x\n", #fast, \
                            (unsigned long long)a, (unsigned long long)b, (unsigned long long)got, flags,  \
                            (unsigned long long)ref, want);                                              \
            }                                                                                            \
        }                                                                                                \
        rl_fp_host_clear();                                                                              \
        rl_arm64_set_fp_flags(0); \
        rl_host_set_fpcr(0);                                                                          \
        CHECK_EQ(bad, 0);                                                                                \
    }

CHECK_OP(fz_flags_host_add64, rl_fadd64, rl_fp64_add, uint64_t, pick64)
CHECK_OP(fz_flags_host_sub64, rl_fsub64, rl_fp64_sub, uint64_t, pick64)
CHECK_OP(fz_flags_host_mul64, rl_fmul64, rl_fp64_mul, uint64_t, pick64)
CHECK_OP(fz_flags_host_div64, rl_fdiv64, rl_fp64_div, uint64_t, pick64)
CHECK_OP(fz_flags_host_add32, rl_fadd32, rl_fp32_add, uint32_t, pick32)
CHECK_OP(fz_flags_host_sub32, rl_fsub32, rl_fp32_sub, uint32_t, pick32)
CHECK_OP(fz_flags_host_mul32, rl_fmul32, rl_fp32_mul, uint32_t, pick32)
CHECK_OP(fz_flags_host_div32, rl_fdiv32, rl_fp32_div, uint32_t, pick32)

/* Fused multiply-add: the addend is often the negated product of the
 * operands rounded, so the exact sum is tiny and its rounding cases are
 * reached, and one in four is a special value. */
#define CHECK_MULADD(name, fast, slow, T, pick, mul)                                                       \
    TEST(name)                                                                                           \
    {                                                                                                    \
        rl_arm64_set_fp_flags(1);\
        rl_host_set_fpcr(RL_FPCR_FZ);                                                                          \
        unsigned bad = 0;                                                                                \
        for (unsigned i = 0; i < N; i++) {                                                               \
            T a = pick(), b = pick(), c = pick();                                                        \
            if (i % 4 == 1)                                                                              \
                c = mul(a, b, 0, NULL) ^ ((T)1 << (sizeof(T) * 8 - 1)) ^ (T)(i & 2);                     \
            uint32_t want = 0;                                                                           \
            T ref = slow(c, a, b, RL_FPCR_FZ, &want);                                                             \
            rl_fp_host_clear();                                                                          \
            T got = fast(c, a, b, RL_FPCR_FZ, 0);                                                                 \
            uint32_t flags = rl_fp_host_flags() & 0x9Fu;                                                 \
            if (got != ref || flags != want) {                                                           \
                if (bad++ < 5)                                                                           \
                    fprintf(stderr, "  %s(%#llx, %#llx, %#llx): %#llx flags %#x, exact %#llx flags %#x\n", \
                            #fast, (unsigned long long)c, (unsigned long long)a, (unsigned long long)b,  \
                            (unsigned long long)got, flags, (unsigned long long)ref, want);              \
            }                                                                                            \
        }                                                                                                \
        rl_fp_host_clear();                                                                              \
        rl_arm64_set_fp_flags(0); \
        rl_host_set_fpcr(0);                                                                          \
        CHECK_EQ(bad, 0);                                                                                \
    }

CHECK_MULADD(fz_flags_host_muladd64, rl_fmuladd64, rl_fp64_muladd, uint64_t, pick64, rl_fp64_mul)
CHECK_MULADD(fz_flags_host_muladd32, rl_fmuladd32, rl_fp32_muladd, uint32_t, pick32, rl_fp32_mul)

#define CHECK_SQRT(name, fast, slow, T, pick)                                                            \
    TEST(name)                                                                                           \
    {                                                                                                    \
        rl_arm64_set_fp_flags(1);\
        rl_host_set_fpcr(RL_FPCR_FZ);                                                                          \
        unsigned bad = 0;                                                                                \
        for (unsigned i = 0; i < N; i++) {                                                               \
            T a = pick();                                                                                \
            uint32_t want = 0;                                                                           \
            T ref = slow(a, RL_FPCR_FZ, &want);                                                                   \
            rl_fp_host_clear();                                                                          \
            T got = fast(a, RL_FPCR_FZ, 0);                                                                       \
            uint32_t flags = rl_fp_host_flags() & 0x9Fu;                                                 \
            if (got != ref || flags != want) {                                                           \
                if (bad++ < 5)                                                                           \
                    fprintf(stderr, "  %s(%#llx): %#llx flags %#x, exact %#llx flags %#x\n", #fast,      \
                            (unsigned long long)a, (unsigned long long)got, flags,                       \
                            (unsigned long long)ref, want);                                              \
            }                                                                                            \
        }                                                                                                \
        rl_fp_host_clear();                                                                              \
        rl_arm64_set_fp_flags(0); \
        rl_host_set_fpcr(0);                                                                          \
        CHECK_EQ(bad, 0);                                                                                \
    }

CHECK_SQRT(fz_flags_host_sqrt64, rl_fsqrt64, rl_fp64_sqrt, uint64_t, pick64)
CHECK_SQRT(fz_flags_host_sqrt32, rl_fsqrt32, rl_fp32_sqrt, uint32_t, pick32)

