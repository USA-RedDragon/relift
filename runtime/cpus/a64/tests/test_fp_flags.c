/**
 * @file test_fp_flags.c
 * @brief FPSR flags on host hardware (abi.h, RL_FP_FLAGS): the inline host
 * operations lifted code uses must give the result and the cumulative flags
 * of the exact layer (fp.c, itself checked against an Apple M2), on x86-64
 * through MXCSR with the tininess fix-up and on arm64 natively.
 */
#define RL_FP_FLAGS 1
#pragma STDC FENV_ACCESS ON

#include "a64.h"
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
        rl_a64_set_fp_flags(1);                                                                          \
        unsigned bad = 0;                                                                                \
        for (unsigned i = 0; i < N; i++) {                                                               \
            T a = pick(), b = pick();                                                                    \
            uint32_t want = 0;                                                                           \
            T ref = slow(a, b, 0, &want);                                                                \
            rl_fp_host_clear();                                                                          \
            T got = fast(a, b, 0, 0);                                                                    \
            uint32_t flags = rl_fp_host_flags() & 0x1Fu;                                                 \
            if (got != ref || flags != want) {                                                           \
                if (bad++ < 5)                                                                           \
                    fprintf(stderr, "  %s(%#llx, %#llx): %#llx flags %#x, exact %#llx flags %#x\n", #fast, \
                            (unsigned long long)a, (unsigned long long)b, (unsigned long long)got, flags,  \
                            (unsigned long long)ref, want);                                              \
            }                                                                                            \
        }                                                                                                \
        rl_fp_host_clear();                                                                              \
        rl_a64_set_fp_flags(0);                                                                          \
        CHECK_EQ(bad, 0);                                                                                \
    }

CHECK_OP(fp_flags_host_add64, rl_fadd64, rl_fp64_add, uint64_t, pick64)
CHECK_OP(fp_flags_host_sub64, rl_fsub64, rl_fp64_sub, uint64_t, pick64)
CHECK_OP(fp_flags_host_mul64, rl_fmul64, rl_fp64_mul, uint64_t, pick64)
CHECK_OP(fp_flags_host_div64, rl_fdiv64, rl_fp64_div, uint64_t, pick64)
CHECK_OP(fp_flags_host_add32, rl_fadd32, rl_fp32_add, uint32_t, pick32)
CHECK_OP(fp_flags_host_sub32, rl_fsub32, rl_fp32_sub, uint32_t, pick32)
CHECK_OP(fp_flags_host_mul32, rl_fmul32, rl_fp32_mul, uint32_t, pick32)
CHECK_OP(fp_flags_host_div32, rl_fdiv32, rl_fp32_div, uint32_t, pick32)

TEST(fp_flags_exact_layer_raises_host_flags_only_in_flag_mode)
{
    rl_fp_host_clear();
    (void)rl_fp64_sqrt(0xBFF0000000000000ull, 0, 0);
    CHECK_EQ(rl_fp_host_flags() & 0x1Fu, 0);
    rl_a64_set_fp_flags(1);
    (void)rl_fp64_sqrt(0xBFF0000000000000ull, 0, 0);
    CHECK_EQ(rl_fp_host_flags() & 0x1Fu, 1u);
    rl_fp_host_clear();
    rl_a64_set_fp_flags(0);
}

TEST(fp_flags_mrs_and_msr_fold_and_clear_the_host)
{
    rl_fp_host_clear();
    rl_fp_host_raise(0x10u);
    CHECK_EQ(rl_mrs_fpsr_flags(0x08000000u), 0x08000010u);
    rl_msr_fpsr_flags();
    CHECK_EQ(rl_mrs_fpsr_flags(0), 0);
    rl_fp_raise(0x3u);
    CHECK_EQ(rl_mrs_fpsr_flags(0), 0x3u);
    rl_fp_host_clear();
}

TEST(fp_flags_host_raise_sets_exactly_the_flags_asked)
{
    unsigned bad = 0;
    for (uint32_t f = 0; f < 0x20u; f++) {
        rl_fp_host_clear();
        rl_fp_host_raise(f);
        uint32_t got = rl_fp_host_flags() & 0x1Fu;
        if (got != f && bad++ < 5)
            fprintf(stderr, "  raise %#x gave %#x\n", f, got);
    }
    rl_fp_host_clear();
    CHECK_EQ(bad, 0);
}

TEST(fp_flags_runtime_entries_keep_the_guest_flags)
{
    rl_a64_set_fp_flags(1);
    rl_fp_host_clear();
    rl_fp_host_raise(0x2u);
    uint32_t saved = rl_a64_flags_enter();
    CHECK_EQ(rl_fp_host_flags() & 0x1Fu, 0);
    rl_fp_host_raise(0x10u); /* the runtime's own inexact work */
    rl_a64_flags_leave(saved);
    CHECK_EQ(rl_fp_host_flags() & 0x1Fu, 0x2u);
    rl_fp_host_clear();
    rl_a64_set_fp_flags(0);
}

/* Arm detects tininess before rounding, x86 after: these products round up
 * to the smallest normal, and an Apple M2 sets UFC and IXC (0x18). */
TEST(fp_flags_tiny_result_rounded_to_min_normal_sets_ufc)
{
    rl_a64_set_fp_flags(1);
    rl_fp_host_clear();
    CHECK_EQ(rl_fmul64(0x000FFFFFFFFFFFFFull, 0x3FF0000000000001ull, 0, 0), 0x0010000000000000ull);
    CHECK_EQ(rl_fp_host_flags() & 0x1Fu, 0x18u);
    rl_fp_host_clear();
    CHECK_EQ(rl_fmul32(0x007FFFFFu, 0x3F800001u, 0, 0), 0x00800000u);
    CHECK_EQ(rl_fp_host_flags() & 0x1Fu, 0x18u);
    rl_fp_host_clear();
    rl_a64_set_fp_flags(0);
}
