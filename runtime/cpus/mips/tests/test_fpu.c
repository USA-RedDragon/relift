/**
 * @file test_fpu.c
 * @brief The VR4300 FPU slow path and the fast path's FCR31 intrinsics:
 * libdragon known answers recorded on ares (spike 21 testroms/REPORT.md),
 * every operation on special values (zeros, subnormals, both NaN kinds,
 * infinities, range edges), the rounding modes, the enables, flush to zero,
 * the FR views, and the soft float against the host's round-to-nearest
 * arithmetic on random operands.
 */
#include "mips.h"

#include "fpu.h"
#include "runner.h"
#include "softfloat.h"

#include <math.h>

enum { S = 0x10, D = 0x11, W = 0x14, L = 0x15 };

#define STATUS_FR1 UINT32_C(0x34000000)
#define SENTINEL UINT64_C(0xDEADBEEFCAFEF00D)

static uint32_t cop1(uint32_t fmt, uint32_t ft, uint32_t fs, uint32_t fd, uint32_t funct)
{
    return 0x11u << 26 | fmt << 21 | ft << 16 | fs << 11 | fd << 6 | funct;
}

/** @brief One operation's outcome. */
typedef struct res {
    bool exc;
    uint32_t fcr;
    uint64_t fd;
} res;

/** @brief funct of fmt with fs = f2 = a, ft = f4 = b, fd = f6, FR = 1. */
static res op(uint32_t fmt, uint32_t funct, uint32_t fcr, uint64_t a, uint64_t b)
{
    uint64_t f[32];
    for (int i = 0; i < 32; i++)
        f[i] = 0x1111111122222222u;
    f[2] = a;
    f[4] = b;
    f[6] = SENTINEL;
    res r;
    r.fcr = fcr;
    r.exc = rl_mips_fpu_eval(cop1(fmt, 4, 2, 6, funct), STATUS_FR1, &r.fcr, f);
    r.fd = f[6];
    return r;
}

/** @brief An unimplemented-operation exception: cause E, destination untouched. */
#define CHECK_UNIMPL(r, fcr_in)                                                                                        \
    do {                                                                                                               \
        res r_ = (r);                                                                                                  \
        CHECK(r_.exc);                                                                                                 \
        CHECK_EQ(r_.fcr, ((fcr_in) & ~(uint32_t)RL_MIPS_FCR_CAUSE) | RL_MIPS_FCR_E);                                   \
        CHECK_EQ(r_.fd, SENTINEL);                                                                                     \
    } while (0)

#define CHECK_RES(r, want_fd, want_fcr)                                                                                \
    do {                                                                                                               \
        res r_ = (r);                                                                                                  \
        CHECK(!r_.exc);                                                                                                \
        CHECK_EQ(r_.fd, (want_fd));                                                                                    \
        CHECK_EQ(r_.fcr, (want_fcr));                                                                              \
    } while (0)

/** @brief The COP1 word of funct in format fmt, registers 0. */
static uint32_t op1(uint32_t fmt, uint32_t funct) { return 0x11u << 26 | fmt << 21 | funct; }

TEST(fpu_cycles_match_the_lifted_templates)
{
    CHECK_EQ(rl_mips_fpu_cycles(op1(0x11, 0x02)), 7u);
    CHECK_EQ(rl_mips_fpu_cycles(op1(0x10, 0x02)), 4u);
    CHECK_EQ(rl_mips_fpu_cycles(op1(0x11, 0x03)), 57u);
    CHECK_EQ(rl_mips_fpu_cycles(op1(0x10, 0x04)), 28u);
    CHECK_EQ(rl_mips_fpu_cycles(op1(0x10, 0x00)), 2u);
    CHECK_EQ(rl_mips_fpu_cycles(op1(0x11, 0x09)), 4u);
    CHECK_EQ(rl_mips_fpu_cycles(op1(0x11, 0x20)), 1u);
    CHECK_EQ(rl_mips_fpu_cycles(op1(0x14, 0x21)), 4u);
    CHECK_EQ(rl_mips_fpu_cycles(op1(0x11, 0x06)), 0u);
    CHECK_EQ(rl_mips_fpu_cycles(op1(0x11, 0x3C)), 2u);
}

TEST(fpu_known_answers_add_sqrt_div)
{
    CHECK_RES(op(S, 0, 0, 0x3F800000, 0x3F800000), 0x40000000, 0);
    CHECK_RES(op(S, 0, 0, 0x3F800000, 0xBF800000), 0, 0);
    CHECK_RES(op(S, 0, 0, 0x7FC00000, 0x3F800000), 0x7FBFFFFF, 0x00010040);
    CHECK_RES(op(S, 0, 0, 0x3F800000, 0x7FC12345), 0x7FBFFFFF, 0x00010040);
    CHECK_RES(op(D, 0, 0, UINT64_C(0x7FF8000000000000), UINT64_C(0x3FF0000000000000)), UINT64_C(0x7FF7FFFFFFFFFFFF),
              0x00010040);
    CHECK_RES(op(S, 0, 1, 0x7FC00000, 0x3F800000), 0x7FBFFFFF, 0x00010041);
    CHECK_RES(op(S, 0, 3, 0x7FC00000, 0x3F800000), 0x7FBFFFFF, 0x00010043);
    CHECK_UNIMPL(op(S, 0, 0, 0x7FA00000, 0x3F800000), 0);
    CHECK_UNIMPL(op(S, 32 + 1, 0, 0xFFA00001, 0), 0);
    CHECK_RES(op(S, 4, 0, 0, 0), 0, 0);
    CHECK_RES(op(S, 4, 0, 0x80000000, 0), 0x80000000, 0);
    CHECK_RES(op(S, 4, 0, 0x3F800000, 0), 0x3F800000, 0);
    CHECK_RES(op(S, 4, 0, 0x00800000, 0), 0x20000000, 0);
    CHECK_RES(op(S, 4, 0, 0x7F7FFFFF, 0), 0x5F7FFFFF, 0x1004);
    CHECK_RES(op(S, 4, 0, 0x7F800000, 0), 0x7F800000, 0);
    CHECK_RES(op(S, 4, 0, 0xBF800000, 0), 0x7FBFFFFF, 0x00010040);
    CHECK_RES(op(S, 4, 0, 0xFF800000, 0), 0x7FBFFFFF, 0x00010040);
    CHECK_RES(op(S, 3, 0, 0x3F800000, 0), 0x7F800000, 0x8020);
    CHECK_RES(op(S, 3, 0, 0xBF800000, 0), 0xFF800000, 0x8020);
    CHECK_RES(op(S, 3, 0, 0, 0x3F800000), 0, 0);
    CHECK_RES(op(S, 3, 0, 0, 0), 0x7FBFFFFF, 0x00010040);
    CHECK_RES(op(S, 3, 0, 0x7F800000, 0x7F800000), 0x7FBFFFFF, 0x00010040);
    CHECK_RES(op(S, 3, 0, 0x7F800000, 0), 0x7F800000, 0);
    CHECK_RES(op(S, 1, 0, 0x7F800000, 0x7F800000), 0x7FBFFFFF, 0x00010040);
    CHECK_RES(op(S, 2, 0, 0x7F800000, 0), 0x7FBFFFFF, 0x00010040);
    CHECK_RES(op(D, 3, 0, UINT64_C(0x3FF0000000000000), 0), UINT64_C(0x7FF0000000000000), 0x8020);
}

TEST(fpu_subnormal_inputs_are_unimplemented_with_and_without_flush)
{
    static const uint32_t dn[] = {0x00000001, 0x007FFFFF, 0x80000001};
    for (unsigned i = 0; i < 3; i++) {
        for (uint32_t fcr = 0; fcr <= RL_MIPS_FCR_FS; fcr += RL_MIPS_FCR_FS) {
            CHECK_UNIMPL(op(S, 0, fcr, dn[i], 0x3F800000), fcr);
            CHECK_UNIMPL(op(S, 0, fcr, 0x3F800000, dn[i]), fcr);
            CHECK_UNIMPL(op(S, 2, fcr, dn[i], 0x3F800000), fcr);
            CHECK_UNIMPL(op(S, 3, fcr, 0x3F800000, dn[i]), fcr);
            CHECK_UNIMPL(op(S, 4, fcr, dn[i], 0), fcr);
            CHECK_UNIMPL(op(S, 5, fcr, dn[i], 0), fcr);
            CHECK_UNIMPL(op(S, 7, fcr, dn[i], 0), fcr);
            CHECK_UNIMPL(op(S, 33, fcr, dn[i], 0), fcr);
            CHECK_UNIMPL(op(S, 36, fcr, dn[i], 0), fcr);
        }
    }
    CHECK_UNIMPL(op(D, 0, 0, 1, UINT64_C(0x3FF0000000000000)), 0);
}

TEST(fpu_underflow_traps_or_flushes_by_rounding_mode)
{
    CHECK_UNIMPL(op(S, 2, 0, 0x00800000, 0x3F000000), 0);
    CHECK_RES(op(S, 2, RL_MIPS_FCR_FS, 0x00800000, 0x3F000000), 0, RL_MIPS_FCR_FS | 0x3000u | 0x000Cu);
    CHECK_RES(op(S, 2, RL_MIPS_FCR_FS | 2u, 0x00800000, 0x3F000000), 0x00800000,
              RL_MIPS_FCR_FS | 2u | 0x3000u | 0x000Cu);
    CHECK_RES(op(S, 2, RL_MIPS_FCR_FS | 3u, 0x80800000, 0x3F000000), 0x80800000,
              RL_MIPS_FCR_FS | 3u | 0x3000u | 0x000Cu);
    CHECK_RES(op(S, 2, RL_MIPS_FCR_FS | 1u, 0x80800000, 0x3F000000), 0x80000000,
              RL_MIPS_FCR_FS | 1u | 0x3000u | 0x000Cu);
    CHECK_UNIMPL(op(S, 2, RL_MIPS_FCR_FS | 1u << 8, 0x00800000, 0x3F000000), RL_MIPS_FCR_FS | 1u << 8);
    CHECK_UNIMPL(op(S, 2, RL_MIPS_FCR_FS | 1u << 7, 0x00800000, 0x3F000000), RL_MIPS_FCR_FS | 1u << 7);
    CHECK_RES(op(S, 2, RL_MIPS_FCR_FS, 0x00800000, 0x00800000), 0, RL_MIPS_FCR_FS | 0x3000u | 0x000Cu);
    CHECK_UNIMPL(op(S, 2, 0, 0x00800000, 0x00800000), 0);
    CHECK_UNIMPL(op(D, 32, 0, UINT64_C(0x3690000000000000), 0), 0);
}

TEST(fpu_overflow_follows_the_rounding_mode)
{
    CHECK_RES(op(S, 2, 0, 0x7F7FFFFF, 0x40000000), 0x7F800000, 0x5014);
    CHECK_RES(op(S, 2, 1, 0x7F7FFFFF, 0x40000000), 0x7F7FFFFF, 0x5015);
    CHECK_RES(op(S, 2, 3, 0x7F7FFFFF, 0x40000000), 0x7F7FFFFF, 0x5017);
    CHECK_RES(op(S, 2, 3, 0xFF7FFFFF, 0x40000000), 0xFF800000, 0x5017);
    CHECK_RES(op(S, 2, 2, 0xFF7FFFFF, 0x40000000), 0xFF7FFFFF, 0x5016);
    CHECK_RES(op(D, 32, 0, UINT64_C(0x47EFFFFFE0000000), 0), 0x7F7FFFFF, 0);
    CHECK_RES(op(D, 32, 0, UINT64_C(0x47F0000000000000), 0), 0x7F800000, 0x5014);
}

TEST(fpu_enabled_exceptions_trap_and_keep_the_destination)
{
    res r = op(S, 3, 1u << 10, 0x3F800000, 0);
    CHECK(r.exc);
    CHECK_EQ(r.fcr, (1u << 10) | 0x8000u);
    CHECK_EQ(r.fd, SENTINEL);
    r = op(S, 0, 1u << 7, 0x3F800000, 0x33800000);
    CHECK(r.exc);
    CHECK_EQ(r.fcr, (1u << 7) | 0x1000u);
    r = op(S, 2, 1u << 9, 0x7F7FFFFF, 0x40000000);
    CHECK(r.exc);
    CHECK_EQ(r.fcr, (1u << 9) | 0x5000u | 0x4u);
    r = op(S, 0, 1u << 11, 0x7FC00000, 0x3F800000);
    CHECK(r.exc);
    CHECK_EQ(r.fcr, (1u << 11) | 0x10000u);
    CHECK_RES(op(S, 0, 0x0001F07Cu, 0x3F800000, 0x3F800000), 0x40000000, 0x7Cu);
}

TEST(fpu_conversions_to_integers)
{
    CHECK_RES(op(S, 36, 0, 0x3F800000, 0), 1, 0);
    CHECK_RES(op(S, 36, 0, 0xBF800000, 0), 0xFFFFFFFF, 0);
    CHECK_RES(op(S, 36, 0, 0x3F000000, 0), 0, 0x1004);
    CHECK_RES(op(S, 36, 0, 0x3FC00000, 0), 2, 0x1004);
    CHECK_RES(op(S, 36, 0, 0x40200000, 0), 2, 0x1004);
    CHECK_RES(op(S, 36, 0, 0xCF000000, 0), 0x80000000, 0);
    CHECK_RES(op(S, 36, 0, 0x4EFFFFFF, 0), 0x7FFFFF80, 0);
    CHECK_UNIMPL(op(S, 36, 0, 0x4F000000, 0), 0);
    CHECK_UNIMPL(op(S, 36, 0, 0x4F800000, 0), 0);
    CHECK_UNIMPL(op(S, 36, 0, 0xDF000000, 0), 0);
    CHECK_UNIMPL(op(S, 36, 0, 0x7F800000, 0), 0);
    CHECK_UNIMPL(op(S, 36, 0, 0x7FC00000, 0), 0);
    CHECK_UNIMPL(op(D, 36, 0, UINT64_C(0x41E0000000000000), 0), 0);
    CHECK_RES(op(D, 36, 0, UINT64_C(0xC1E0000000000000), 0), 0x80000000, 0);
    CHECK_UNIMPL(op(D, 36, 0, UINT64_C(0x41DFFFFFFFE00000), 0), 0);
    CHECK_RES(op(S, 37, 0, 0x4F000000, 0), 0x80000000, 0);
    CHECK_RES(op(S, 37, 0, 0xCF000000, 0), UINT64_C(0xFFFFFFFF80000000), 0);
    CHECK_RES(op(S, 37, 0, 0x4F800000, 0), UINT64_C(0x100000000), 0);
    CHECK_UNIMPL(op(S, 37, 0, 0x5F000000, 0), 0);
    CHECK_UNIMPL(op(S, 37, 0, 0x5A000000, 0), 0);
    CHECK_UNIMPL(op(S, 37, 0, 0xDA000000, 0), 0);
    CHECK_RES(op(S, 37, 0, 0xBF800000, 0), UINT64_C(0xFFFFFFFFFFFFFFFF), 0);
    CHECK_RES(op(S, 36, 1, 0x3FC00000, 0), 1, 0x1005);
    CHECK_RES(op(S, 36, 2, 0xBFC00000, 0), 0xFFFFFFFF, 0x1006);
    CHECK_RES(op(S, 36, 2, 0x3FC00000, 0), 2, 0x1006);
    CHECK_RES(op(S, 36, 3, 0x3FC00000, 0), 1, 0x1007);
    CHECK_RES(op(S, 12, 3, 0x3FC00000, 0), 2, 0x1007);
    CHECK_RES(op(S, 13, 0, 0xBFC00000, 0), 0xFFFFFFFF, 0x1004);
    CHECK_RES(op(S, 14, 0, 0x3FC00000, 0), 2, 0x1004);
    CHECK_RES(op(S, 15, 0, 0xBFC00000, 0), 0xFFFFFFFE, 0x1004);
    CHECK_RES(op(S, 14, 0, 0xBF000000, 0), 0, 0x1004);
    CHECK_RES(op(S, 8, 0, 0x40200000, 0), 2, 0x1004);
    CHECK_RES(op(D, 9, 0, UINT64_C(0xC004000000000000), 0), UINT64_C(0xFFFFFFFFFFFFFFFE), 0x1004);
    CHECK_RES(op(D, 10, 0, UINT64_C(0x4004000000000000), 0), 3, 0x1004);
    CHECK_RES(op(D, 11, 0, UINT64_C(0xC004000000000000), 0), UINT64_C(0xFFFFFFFFFFFFFFFD), 0x1004);
    CHECK_UNIMPL(op(D, 9, 0, UINT64_C(0xC340000000000000), 0), 0);
    CHECK_RES(op(D, 9, 0, UINT64_C(0xC33FFFFFFFFFFFFF), 0), UINT64_C(0xFFE0000000000001), 0);
    res r = op(S, 12, 1u << 7, 0x3FC00000, 0);
    CHECK(r.exc);
    CHECK_EQ(r.fcr, (1u << 7) | 0x1000u);
}

TEST(fpu_conversions_between_formats)
{
    CHECK_RES(op(S, 33, 0, 0x3F800000, 0), UINT64_C(0x3FF0000000000000), 0);
    CHECK_RES(op(S, 33, 0, 0x7F800000, 0), UINT64_C(0x7FF0000000000000), 0);
    CHECK_RES(op(S, 33, 0, 0x7FC00000, 0), UINT64_C(0x7FF7FFFFFFFFFFFF), 0x00010040);
    CHECK_RES(op(D, 32, 0, UINT64_C(0x7FF8000000000000), 0), 0x7FBFFFFF, 0x00010040);
    CHECK_RES(op(D, 32, 0, UINT64_C(0x3FF0000000000001), 0), 0x3F800000, 0x1004);
    CHECK_RES(op(D, 32, 2, UINT64_C(0x3FF0000000000001), 0), 0x3F800001, 0x1006);
    CHECK_RES(op(W, 32, 0, 0x01000001, 0), 0x4B800000, 0x1004);
    CHECK_RES(op(W, 32, 0, 0xFFFFFFFF, 0), 0xBF800000, 0);
    CHECK_RES(op(W, 33, 0, 0x80000000, 0), UINT64_C(0xC1E0000000000000), 0);
    CHECK_RES(op(L, 33, 0, UINT64_C(0x0020000000000001), 0), UINT64_C(0x4340000000000000), 0x1004);
    CHECK_RES(op(L, 32, 0, UINT64_C(0x007FFFFFFFFFFFFF), 0), 0x5B000000, 0x1004);
    CHECK_UNIMPL(op(L, 32, 0, UINT64_C(0x0080000000000000), 0), 0);
    CHECK_UNIMPL(op(L, 33, 0, UINT64_C(0xFF7FFFFFFFFFFFFF), 0), 0);
    CHECK_RES(op(L, 33, 0, UINT64_C(0xFF80000000000000), 0), UINT64_C(0xC360000000000000), 0);
    CHECK_UNIMPL(op(S, 32, 0, 0x3F800000, 0), 0);
    CHECK_UNIMPL(op(D, 33, 0, 0, 0), 0);
    CHECK_UNIMPL(op(W, 36, 0, 1, 0), 0);
    CHECK_UNIMPL(op(L, 37, 0, 1, 0), 0);
    CHECK_UNIMPL(op(W, 0, 0, 1, 1), 0);
    CHECK_UNIMPL(op(S, 20, 0, 1, 1), 0);
}

TEST(fpu_abs_and_neg_on_special_values)
{
    CHECK_RES(op(S, 5, 0, 0x80000000, 0), 0, 0);
    CHECK_RES(op(S, 7, 0, 0, 0), 0x80000000, 0);
    CHECK_RES(op(S, 5, 0, 0xFF800000, 0), 0x7F800000, 0);
    CHECK_RES(op(S, 7, 0, 0x7F800000, 0), 0xFF800000, 0);
    CHECK_RES(op(S, 7, 0, 0xFFC00000, 0), 0x7FBFFFFF, 0x00010040);
    CHECK_UNIMPL(op(S, 5, 0, 0x7FBFFFFF, 0), 0);
    CHECK_RES(op(D, 5, 0, UINT64_C(0xBFF0000000000000), 0), UINT64_C(0x3FF0000000000000), 0);
}

TEST(fpu_a_word_result_clears_the_high_half)
{
    uint64_t f[32] = {0};
    f[2] = 0x3F800000;
    f[4] = 0x3F800000;
    f[6] = SENTINEL;
    uint32_t fcr = 0;
    CHECK(!rl_mips_fpu_eval(cop1(S, 4, 2, 6, 0), STATUS_FR1, &fcr, f));
    CHECK_EQ(f[6], 0x40000000);
    f[2] = UINT64_C(0x123456783F800000);
    CHECK(!rl_mips_fpu_eval(cop1(S, 4, 2, 6, 36), STATUS_FR1, &fcr, f));
    CHECK_EQ(f[6], 1);
}

TEST(fpu_fr0_reads_fs_from_the_even_register)
{
    uint64_t f[32] = {0};
    f[2] = 0x40000000;
    f[3] = 0x3F800000;
    f[4] = 0x3F800000;
    uint32_t fcr = 0;
    CHECK(!rl_mips_fpu_eval(cop1(S, 4, 3, 7, 0), 0x20000000u, &fcr, f));
    CHECK_EQ(f[7], 0x40400000);
    CHECK(!rl_mips_fpu_eval(cop1(S, 4, 3, 7, 0), STATUS_FR1, &fcr, f));
    CHECK_EQ(f[7], 0x40000000);
    CHECK(!rl_mips_fpu_eval(cop1(D, 0, 3, 9, 6), 0x20000000u, &fcr, f));
    CHECK_EQ(f[9], 0x40000000);
}

TEST(fpu_compares_and_their_nan_rules)
{
    uint64_t f[32] = {0};
    uint32_t fcr;
    f[2] = 0x7FC00000;
    f[4] = 0x3F800000;
    fcr = 0;
    CHECK(!rl_mips_fpu_eval(cop1(S, 4, 2, 0, 0x32), STATUS_FR1, &fcr, f));
    CHECK_EQ(fcr, 0x00010040);
    fcr = 0;
    CHECK(!rl_mips_fpu_eval(cop1(S, 4, 2, 0, 0x39), STATUS_FR1, &fcr, f));
    CHECK_EQ(fcr, 0x00810040);
    fcr = RL_MIPS_FCR_FS;
    CHECK(!rl_mips_fpu_eval(cop1(S, 4, 2, 0, 0x32), STATUS_FR1, &fcr, f));
    CHECK_EQ(fcr, 0x01010040);
    fcr = RL_MIPS_FCR_FS;
    CHECK(!rl_mips_fpu_eval(cop1(S, 4, 2, 0, 0x39), STATUS_FR1, &fcr, f));
    CHECK_EQ(fcr, 0x01810040);
    f[2] = 0x7FA00000;
    fcr = RL_MIPS_FCR_C;
    CHECK(!rl_mips_fpu_eval(cop1(S, 4, 2, 0, 0x32), STATUS_FR1, &fcr, f));
    CHECK_EQ(fcr, 0);
    fcr = 0;
    CHECK(!rl_mips_fpu_eval(cop1(S, 4, 2, 0, 0x33), STATUS_FR1, &fcr, f));
    CHECK_EQ(fcr, RL_MIPS_FCR_C);
    fcr = 0;
    CHECK(!rl_mips_fpu_eval(cop1(S, 4, 2, 0, 0x3A), STATUS_FR1, &fcr, f));
    CHECK_EQ(fcr, 0x00010040);
    fcr = 1u << 11;
    CHECK(rl_mips_fpu_eval(cop1(S, 4, 2, 0, 0x3A), STATUS_FR1, &fcr, f));
    CHECK_EQ(fcr, (1u << 11) | 0x10000u);
    static const struct {
        uint32_t a, b;
        uint32_t lt, eq;
    } pairs[] = {
        {0x3F800000, 0x40000000, 1, 0}, {0x40000000, 0x3F800000, 0, 0}, {0x80000000, 0, 0, 1},
        {0x00000001, 0x00000002, 1, 0}, {0xFF800000, 0x7F800000, 1, 0}, {0x7F800000, 0x7F800000, 0, 1},
        {0xBF800000, 0xC0000000, 0, 0},
    };
    for (unsigned i = 0; i < sizeof pairs / sizeof pairs[0]; i++) {
        f[2] = pairs[i].a;
        f[4] = pairs[i].b;
        for (uint32_t cond = 0; cond < 16; cond++) {
            fcr = 0x0003F000u;
            CHECK(!rl_mips_fpu_eval(cop1(S, 4, 2, 0, 0x30 | cond), STATUS_FR1, &fcr, f));
            bool want = ((cond & 4u) && pairs[i].lt) || ((cond & 2u) && pairs[i].eq);
            CHECK_EQ(fcr, want ? RL_MIPS_FCR_C : 0u);
            CHECK_EQ(rl_mips_fcmp(cond, S, 0x0003F000u, pairs[i].a, pairs[i].b), want ? RL_MIPS_FCR_C : 0u);
        }
    }
}

TEST(fpu_mov_copies_without_touching_fcr31)
{
    uint64_t f[32] = {0};
    f[3] = UINT64_C(0x7FF0000000000001);
    uint32_t fcr = 0x0003F000u;
    CHECK(!rl_mips_fpu_eval(cop1(S, 0, 3, 8, 6), STATUS_FR1, &fcr, f));
    CHECK_EQ(f[8], UINT64_C(0x7FF0000000000001));
    CHECK_EQ(fcr, 0x0003F000u);
}

static uint64_t rng_state = UINT64_C(0x9E3779B97F4A7C15);

static uint64_t rng(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/** @brief A random normal binary32 with an exponent near 1 (scale 0) or anywhere in range. */
static uint32_t rand_f32(int spread)
{
    uint32_t e = spread ? 1u + (uint32_t)(rng() % 253u) : 112u + (uint32_t)(rng() % 32u);
    return (uint32_t)(rng() & 0x80000000u) | e << 23 | (uint32_t)(rng() & 0x7FFFFFu);
}

static uint64_t rand_f64(int spread)
{
    uint64_t e = spread ? 1u + rng() % 2045u : 1008u + rng() % 32u;
    return (rng() & (UINT64_C(1) << 63)) | e << 52 | (rng() & UINT64_C(0xFFFFFFFFFFFFF));
}

static uint64_t host_f32(uint32_t funct, uint32_t a, uint32_t b)
{
    float x, y, r = 0;
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    switch (funct) {
    case 0:
        r = x + y;
        break;
    case 1:
        r = x - y;
        break;
    case 2:
        r = x * y;
        break;
    case 3:
        r = x / y;
        break;
    default:
        r = sqrtf(fabsf(x));
        break;
    }
    uint32_t o;
    memcpy(&o, &r, 4);
    return o;
}

static uint64_t host_f64(uint32_t funct, uint64_t a, uint64_t b)
{
    double x, y, r = 0;
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    switch (funct) {
    case 0:
        r = x + y;
        break;
    case 1:
        r = x - y;
        break;
    case 2:
        r = x * y;
        break;
    case 3:
        r = x / y;
        break;
    default:
        r = sqrt(fabs(x));
        break;
    }
    uint64_t o;
    memcpy(&o, &r, 8);
    return o;
}

TEST(fpu_soft_float_matches_the_host_and_the_fast_path_flags)
{
    unsigned compared = 0;
    for (int i = 0; i < 200000; i++) {
        uint32_t funct = (uint32_t)(rng() % 5u);
        int spread = (int)(rng() & 1u);
        bool dbl = rng() & 1u;
        uint64_t a = dbl ? rand_f64(spread) : rand_f32(spread);
        uint64_t b = dbl ? rand_f64(spread) : rand_f32(spread);
        if (funct == 4)
            a &= dbl ? ~(UINT64_C(1) << 63) : ~(UINT64_C(1) << 31);
        res r = op(dbl ? D : S, funct, 0, a, b);
        uint64_t want = dbl ? host_f64(funct, a, b) : host_f32(funct, (uint32_t)a, (uint32_t)b);
        rl_sf_class cls = rl_sf_classify(dbl, want);
        if (cls == RL_SF_SUBNORMAL || (cls == RL_SF_ZERO && (funct == 2 || funct == 3))) {
            CHECK(r.exc);
            continue;
        }
        if (cls != RL_SF_NORMAL && cls != RL_SF_ZERO)
            continue;
        CHECK(!r.exc);
        CHECK_EQ(r.fd, want);
        CHECK_EQ(r.fcr, rl_mips_fcr31(funct, dbl ? D : S, 0x0003F000u, a, b));
        compared++;
    }
    CHECK(compared > 150000);
}

TEST(fpu_fast_path_flags_for_conversions)
{
    for (int i = 0; i < 50000; i++) {
        uint32_t a = rand_f32(0);
        uint64_t d = rand_f64(0);
        static const uint32_t to_int[] = {8, 9, 10, 11, 12, 13, 14, 15, 36, 37};
        uint32_t funct = to_int[rng() % 10u];
        res r = op(S, funct, 0, a, 0);
        CHECK(!r.exc);
        CHECK_EQ(r.fcr, rl_mips_fcr31(funct, S, 0, a, 0));
        r = op(D, funct, 0, d, 0);
        CHECK(!r.exc);
        CHECK_EQ(r.fcr, rl_mips_fcr31(funct, D, 0, d, 0));
        r = op(D, 32, 0, d, 0);
        if (!r.exc)
            CHECK_EQ(r.fcr, rl_mips_fcr31(32, D, 0, d, 0));
        uint64_t w = (uint32_t)rng();
        r = op(W, 32, 0, w, 0);
        CHECK_EQ(r.fcr, rl_mips_fcr31(32, W, 0, w, 0));
        uint64_t l = rng() >> (9 + rng() % 40u);
        r = op(L, 32, 0, l, 0);
        CHECK(!r.exc);
        CHECK_EQ(r.fcr, rl_mips_fcr31(32, L, 0, l, 0));
        r = op(L, 33, 0, l, 0);
        CHECK_EQ(r.fcr, rl_mips_fcr31(33, L, 0, l, 0));
    }
}

TEST(soft_float_rounds_every_mode)
{
    unsigned fl = 0;
    CHECK_EQ(rl_sf_add(false, 0x3F800000, 0x33800000, RL_SF_RN, &fl), 0x3F800000);
    CHECK_EQ(rl_sf_add(false, 0x3F800000, 0x33800001, RL_SF_RN, &fl), 0x3F800001);
    CHECK_EQ(rl_sf_add(false, 0x3F800001, 0x33800000, RL_SF_RN, &fl), 0x3F800002);
    CHECK_EQ(rl_sf_add(false, 0x3F800000, 0x33800000, RL_SF_RP, &fl), 0x3F800001);
    CHECK_EQ(rl_sf_add(false, 0xBF800000, 0xB3800000, RL_SF_RP, &fl), 0xBF800000);
    CHECK_EQ(rl_sf_add(false, 0xBF800000, 0xB3800000, RL_SF_RM, &fl), 0xBF800001);
    CHECK_EQ(rl_sf_add(false, 0x3F800000, 0x33800000, RL_SF_RZ, &fl), 0x3F800000);
    CHECK_EQ(rl_sf_sub(false, 0x3F800000, 0x3F800000, RL_SF_RM, &fl), 0x80000000);
    CHECK_EQ(rl_sf_sub(false, 0x3F800000, 0x3F800000, RL_SF_RN, &fl), 0);
    CHECK_EQ(rl_sf_add(false, 0x80000000, 0x80000000, RL_SF_RN, &fl), 0x80000000);
    CHECK_EQ(rl_sf_sqrt(true, UINT64_C(0x4000000000000000), RL_SF_RN, &fl), UINT64_C(0x3FF6A09E667F3BCD));
    CHECK_EQ(rl_sf_sqrt(true, UINT64_C(0x4000000000000000), RL_SF_RP, &fl), UINT64_C(0x3FF6A09E667F3BCD));
    CHECK_EQ(rl_sf_sqrt(true, UINT64_C(0x4000000000000000), RL_SF_RZ, &fl), UINT64_C(0x3FF6A09E667F3BCC));
    CHECK_EQ(rl_sf_div(false, 0x3F800000, 0x40400000, RL_SF_RN, &fl), 0x3EAAAAAB);
    CHECK_EQ(rl_sf_div(false, 0x3F800000, 0x40400000, RL_SF_RZ, &fl), 0x3EAAAAAA);
    fl = 0;
    CHECK_EQ(rl_sf_mul(false, 0x00800000, 0x3F7FFFFF, RL_SF_RN, &fl), 0x00800000);
    CHECK_EQ(fl, RL_SF_I | RL_SF_U);
    fl = 0;
    CHECK_EQ(rl_sf_mul(false, 0x00800000, 0x3F000000, RL_SF_RN, &fl), 0x00400000);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_sf_to_int(false, 0xC0200000, RL_SF_RN, &fl), -2);
    CHECK_EQ(rl_sf_to_int(false, 0xC0600000, RL_SF_RN, &fl), -4);
    CHECK_EQ(rl_sf_to_int(false, 0x3E800000, RL_SF_RP, &fl), 1);
    CHECK_EQ(rl_sf_to_int(false, 0xBE800000, RL_SF_RM, &fl), -1);
    CHECK_EQ(rl_sf_from_int(true, INT64_MIN, RL_SF_RN, &fl), UINT64_C(0xC3E0000000000000));
}
