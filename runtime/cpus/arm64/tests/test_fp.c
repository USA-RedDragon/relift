/**
 * @file test_fp.c
 * @brief The FP cut layer slow paths: NaN propagation order, default NaNs,
 * FPSR flags, rounding modes, FZ and DN, the estimate table, conversions and
 * saturation, with known bit patterns, plus a differential run against the
 * host FPU for non-NaN round-to-nearest results.
 */
#include "runner.h"

#include <relift/arm64/abi.h>

#define IOC 0x01u
#define DZC 0x02u
#define OFC 0x04u
#define UFC 0x08u
#define IXC 0x10u
#define IDC 0x80u

#define FZ RL_FPCR_FZ
#define DN RL_FPCR_DN
#define RMODE(m) ((uint32_t)(m) << 22)

#define ONE 0x3F800000u
#define TWO 0x40000000u
#define THREE 0x40400000u
#define INF 0x7F800000u
#define NINF 0xFF800000u
#define QNAN 0x7FC00000u
#define NEG0 0x80000000u

TEST(fp32_basic_arithmetic)
{
    uint32_t fl = 0;
    CHECK_EQ(rl_fp32_add(ONE, TWO, 0, &fl), THREE);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp32_sub(THREE, TWO, 0, &fl), ONE);
    CHECK_EQ(rl_fp32_mul(TWO, THREE, 0, &fl), 0x40C00000u);
    CHECK_EQ(rl_fp32_div(THREE, TWO, 0, &fl), 0x3FC00000u);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp32_add(0x3DCCCCCDu, 0x3E4CCCCDu, 0, &fl), 0x3E99999Au);
    CHECK_EQ(fl, IXC);
    CHECK_EQ(rl_fp32_add(ONE, 0, 0, NULL), ONE);
    CHECK_EQ(rl_fp32_add(NEG0, NEG0, 0, NULL), NEG0);
    CHECK_EQ(rl_fp32_add(NEG0, 0, 0, NULL), 0);
    CHECK_EQ(rl_fp32_add(NEG0, 0, RMODE(RL_RM_NEGINF), NULL), NEG0);
    CHECK_EQ(rl_fp32_sub(ONE, ONE, 0, NULL), 0);
    CHECK_EQ(rl_fp32_sub(ONE, ONE, RMODE(RL_RM_NEGINF), NULL), NEG0);
}

TEST(fp32_invalid_and_nan_propagation)
{
    uint32_t fl = 0;
    CHECK_EQ(rl_fp32_add(INF, NINF, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_sub(INF, INF, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_mul(0, INF, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_div(0, 0, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_div(INF, INF, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_add(0x7FC00123u, 0x7FC00456u, 0, &fl), 0x7FC00123u);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp32_add(ONE, 0x7FC00456u, 0, &fl), 0x7FC00456u);
    CHECK_EQ(rl_fp32_add(0x7FC00123u, 0x7F800001u, 0, &fl), 0x7FC00001u);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_mul(0x7F800002u, 0x7F800001u, 0, &fl), 0x7FC00002u);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_add(0xFFC00123u, ONE, 0, &fl), 0xFFC00123u);
    CHECK_EQ(rl_fp32_add(0xFFC00123u, ONE, DN, &fl), QNAN);
    CHECK_EQ(rl_fp32_sub(ONE, 0xFFC00123u, 0, &fl), 0xFFC00123u);
    CHECK_EQ(fl, 0);
}

TEST(fp32_divide_by_zero)
{
    uint32_t fl = 0;
    CHECK_EQ(rl_fp32_div(ONE, 0, 0, &fl), INF);
    CHECK_EQ(fl, DZC);
    fl = 0;
    CHECK_EQ(rl_fp32_div(ONE, NEG0, 0, &fl), NINF);
    CHECK_EQ(fl, DZC);
    fl = 0;
    CHECK_EQ(rl_fp32_div(INF, 0, 0, &fl), INF);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp32_div(ONE, INF, 0, &fl), 0);
    CHECK_EQ(rl_fp32_div(0, ONE, 0, &fl), 0);
}

TEST(fp32_rounding_modes)
{
    uint32_t fl = 0;
    uint32_t tiny = 0x33800000u;
    CHECK_EQ(rl_fp32_add(ONE, tiny, RMODE(RL_RM_NEAREST), &fl), ONE);
    CHECK_EQ(fl, IXC);
    CHECK_EQ(rl_fp32_add(ONE, tiny, RMODE(RL_RM_POSINF), NULL), 0x3F800001u);
    CHECK_EQ(rl_fp32_add(ONE, tiny, RMODE(RL_RM_NEGINF), NULL), ONE);
    CHECK_EQ(rl_fp32_add(ONE, tiny, RMODE(RL_RM_ZERO), NULL), ONE);
    CHECK_EQ(rl_fp32_add(0xBF800000u, 0xB3800000u, RMODE(RL_RM_NEGINF), NULL), 0xBF800001u);
    CHECK_EQ(rl_fp32_add(0x3F800001u, tiny, 0, NULL), 0x3F800002u);
    fl = 0;
    CHECK_EQ(rl_fp32_add(0x7F7FFFFFu, 0x7F7FFFFFu, 0, &fl), INF);
    CHECK_EQ(fl, OFC | IXC);
    fl = 0;
    CHECK_EQ(rl_fp32_add(0x7F7FFFFFu, 0x7F7FFFFFu, RMODE(RL_RM_ZERO), &fl), 0x7F7FFFFFu);
    CHECK_EQ(fl, OFC | IXC);
    CHECK_EQ(rl_fp32_add(0xFF7FFFFFu, 0xFF7FFFFFu, RMODE(RL_RM_POSINF), NULL), 0xFF7FFFFFu);
    CHECK_EQ(rl_fp32_add(0xFF7FFFFFu, 0xFF7FFFFFu, RMODE(RL_RM_NEGINF), NULL), NINF);
}

TEST(fp32_underflow_and_flush)
{
    uint32_t fl = 0;
    CHECK_EQ(rl_fp32_mul(0x00000001u, 0x3F000000u, 0, &fl), 0);
    CHECK_EQ(fl, UFC | IXC);
    fl = 0;
    CHECK_EQ(rl_fp32_mul(0x00000003u, 0x3F000000u, 0, &fl), 0x00000002u);
    CHECK_EQ(fl, UFC | IXC);
    fl = 0;
    CHECK_EQ(rl_fp32_mul(0x00000004u, 0x3F000000u, 0, &fl), 0x00000002u);
    CHECK_EQ(fl, 0);
    fl = 0;
    CHECK_EQ(rl_fp32_mul(0x00800000u, 0x3F7FFFFFu, 0, &fl), 0x00800000u);
    CHECK_EQ(fl, UFC | IXC);
    fl = 0;
    CHECK_EQ(rl_fp32_add(0x00000001u, ONE, FZ, &fl), ONE);
    CHECK_EQ(fl, IDC);
    fl = 0;
    CHECK_EQ(rl_fp32_add(0x80000001u, 0, FZ, &fl), 0);
    CHECK_EQ(fl, IDC);
    fl = 0;
    CHECK_EQ(rl_fp32_mul(0x00800000u, 0x3F000000u, FZ, &fl), 0);
    CHECK_EQ(fl, UFC);
    fl = 0;
    CHECK_EQ(rl_fp32_mul(0x80800000u, 0x3F000000u, FZ, &fl), NEG0);
    CHECK_EQ(fl, UFC);
    fl = 0;
    CHECK_EQ(rl_fp32_mul(0x00800000u, 0x3F800000u, FZ, &fl), 0x00800000u);
    CHECK_EQ(fl, 0);
    fl = 0;
    CHECK_EQ(rl_fp32_mul(0x00000003u, TWO, 0, &fl), 0x00000006u);
    CHECK_EQ(fl, 0);
}

TEST(fp32_muladd)
{
    uint32_t fl = 0;
    CHECK_EQ(rl_fp32_muladd(ONE, TWO, THREE, 0, &fl), 0x40E00000u);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp32_muladd(0xBF801000u, 0x3F800800u, 0x3F800800u, 0, &fl), 0x33800000u);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp32_muladd(0x7FC00001u, 0x7F800002u, QNAN, 0, &fl), 0x7FC00002u);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_muladd(0x7FC00001u, 0x7FC00002u, QNAN, 0, &fl), 0x7FC00001u);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp32_muladd(ONE, 0x7FC00002u, 0x7FC00003u, 0, &fl), 0x7FC00002u);
    CHECK_EQ(rl_fp32_muladd(0x7FC00001u, 0, INF, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_muladd(ONE, 0, INF, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_muladd(NINF, INF, ONE, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_muladd(INF, INF, ONE, 0, &fl), INF);
    CHECK_EQ(rl_fp32_muladd(NEG0, 0, ONE, 0, NULL), 0);
    CHECK_EQ(rl_fp32_muladd(NEG0, NEG0, ONE, 0, NULL), NEG0);
    CHECK_EQ(rl_fp32_muladd(NEG0, NEG0, 0xBF800000u, 0, NULL), 0);
}

TEST(fp32_sqrt)
{
    uint32_t fl = 0;
    CHECK_EQ(rl_fp32_sqrt(0x40800000u, 0, &fl), TWO);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp32_sqrt(TWO, 0, &fl), 0x3FB504F3u);
    CHECK_EQ(fl, IXC);
    fl = 0;
    CHECK_EQ(rl_fp32_sqrt(0xBF800000u, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_sqrt(NEG0, 0, &fl), NEG0);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp32_sqrt(INF, 0, &fl), INF);
    CHECK_EQ(rl_fp32_sqrt(NINF, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_sqrt(0x7F800001u, 0, &fl), 0x7FC00001u);
    CHECK_EQ(fl, IOC);
    CHECK_EQ(rl_fp32_sqrt(0x3F800001u, RMODE(RL_RM_POSINF), NULL), 0x3F800001u);
    CHECK_EQ(rl_fp32_sqrt(0x3F800001u, RMODE(RL_RM_ZERO), NULL), ONE);
}

TEST(fp32_round_int)
{
    uint32_t fl = 0;
    uint32_t v25 = 0x40200000u;
    CHECK_EQ(rl_fp32_round_int(v25, RL_RM_NEAREST, 0, 0, &fl), TWO);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp32_round_int(v25, RL_RM_NEAREST, 1, 0, &fl), TWO);
    CHECK_EQ(fl, IXC);
    CHECK_EQ(rl_fp32_round_int(v25, RL_RM_AWAY, 0, 0, NULL), THREE);
    CHECK_EQ(rl_fp32_round_int(v25, RL_RM_POSINF, 0, 0, NULL), THREE);
    CHECK_EQ(rl_fp32_round_int(v25, RL_RM_NEGINF, 0, 0, NULL), TWO);
    CHECK_EQ(rl_fp32_round_int(v25, RL_RM_ZERO, 0, 0, NULL), TWO);
    CHECK_EQ(rl_fp32_round_int(v25 | 0x80000000u, RL_RM_AWAY, 0, 0, NULL), 0xC0400000u);
    CHECK_EQ(rl_fp32_round_int(v25, RL_RM_FPCR, 0, RMODE(RL_RM_POSINF), NULL), THREE);
    CHECK_EQ(rl_fp32_round_int(0xBE99999Au, RL_RM_NEAREST, 0, 0, NULL), NEG0);
    CHECK_EQ(rl_fp32_round_int(0x3F000000u, RL_RM_NEAREST, 0, 0, NULL), 0);
    CHECK_EQ(rl_fp32_round_int(0x3F000000u, RL_RM_AWAY, 0, 0, NULL), ONE);
    CHECK_EQ(rl_fp32_round_int(0x4B000001u, RL_RM_NEAREST, 1, 0, &fl), 0x4B000001u);
    CHECK_EQ(rl_fp32_round_int(NINF, RL_RM_NEAREST, 1, 0, NULL), NINF);
    CHECK_EQ(rl_fp32_round_int(0x7F800001u, RL_RM_NEAREST, 0, 0, &fl), 0x7FC00001u);
    CHECK_EQ(rl_fp32_round_int(0x00000001u, RL_RM_POSINF, 0, 0, NULL), ONE);
    fl = 0;
    CHECK_EQ(rl_fp32_round_int(0x00000001u, RL_RM_POSINF, 0, FZ, &fl), 0);
    CHECK_EQ(fl, IDC);
}

TEST(fp_rsqrt_estimate)
{
    uint32_t fl = 0;
    CHECK_EQ(rl_fp32_rsqrt_est(ONE, 0, &fl), 0x3F7F8000u);
    CHECK_EQ(rl_fp32_rsqrt_est(TWO, 0, &fl), 0x3F348000u);
    CHECK_EQ(rl_fp32_rsqrt_est(0x40800000u, 0, &fl), 0x3EFF8000u);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp32_rsqrt_est(0, 0, &fl), INF);
    CHECK_EQ(fl, DZC);
    fl = 0;
    CHECK_EQ(rl_fp32_rsqrt_est(NEG0, 0, &fl), NINF);
    CHECK_EQ(fl, DZC);
    fl = 0;
    CHECK_EQ(rl_fp32_rsqrt_est(0xBF800000u, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_rsqrt_est(INF, 0, &fl), 0);
    CHECK_EQ(rl_fp32_rsqrt_est(0x7F800001u, 0, &fl), 0x7FC00001u);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp32_rsqrt_est(0x00000001u, 0, &fl), 0x64B48000u);
    CHECK_EQ(rl_fp32_rsqrt_est(0x00000001u, FZ, &fl), INF);
    CHECK_EQ(fl, IDC | DZC);
    CHECK_EQ(rl_fp64_rsqrt_est(0x3FF0000000000000ull, 0, NULL), 0x3FEFF00000000000ull);
    CHECK_EQ(rl_fp64_rsqrt_est(0x4000000000000000ull, 0, NULL), 0x3FE6900000000000ull);
}

TEST(fp64_basic)
{
    uint32_t fl = 0;
    uint64_t one = 0x3FF0000000000000ull, two = 0x4000000000000000ull;
    CHECK_EQ(rl_fp64_add(one, two, 0, &fl), 0x4008000000000000ull);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp64_mul(0x3FB999999999999Aull, two, 0, &fl), 0x3FC999999999999Aull);
    CHECK_EQ(rl_fp64_div(one, 0x4008000000000000ull, 0, &fl), 0x3FD5555555555555ull);
    CHECK_EQ(fl, IXC);
    fl = 0;
    CHECK_EQ(rl_fp64_div(one, 0, 0, &fl), 0x7FF0000000000000ull);
    CHECK_EQ(fl, DZC);
    fl = 0;
    CHECK_EQ(rl_fp64_sub(0x7FF0000000000000ull, 0x7FF0000000000000ull, 0, &fl), 0x7FF8000000000000ull);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp64_add(0x7FF0000000000001ull, 0x7FF8000000000002ull, 0, &fl), 0x7FF8000000000001ull);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp64_sqrt(0xBFF0000000000000ull, 0, &fl), 0x7FF8000000000000ull);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp64_sqrt(two, 0, &fl), 0x3FF6A09E667F3BCDull);
    CHECK_EQ(fl, IXC);
    fl = 0;
    CHECK_EQ(rl_fp64_add(0x7FEFFFFFFFFFFFFFull, 0x7FEFFFFFFFFFFFFFull, 0, &fl), 0x7FF0000000000000ull);
    CHECK_EQ(fl, OFC | IXC);
    fl = 0;
    CHECK_EQ(rl_fp64_mul(0x0000000000000001ull, 0x3FE0000000000000ull, 0, &fl), 0);
    CHECK_EQ(fl, UFC | IXC);
    fl = 0;
    CHECK_EQ(rl_fp64_muladd(0xBFF0000004000000ull, 0x3FF0000002000000ull,
                            0x3FF0000002000000ull, 0, &fl),
             0x3C90000000000000ull);
    CHECK_EQ(rl_fp64_round_int(0x4004000000000000ull, RL_RM_AWAY, 0, 0, NULL), 0x4008000000000000ull);
    CHECK_EQ(rl_fp64_round_int(0x3FD3333333333333ull, RL_RM_NEGINF, 1, 0, &fl), 0);
    CHECK_EQ(fl, IXC);
}

TEST(fp_convert_formats)
{
    uint32_t fl = 0;
    CHECK_EQ(rl_fp_convert(ONE, 32, 16, RL_RM_FPCR, 0, &fl), 0x3C00u);
    CHECK_EQ(rl_fp_convert(0x3C00u, 16, 32, RL_RM_FPCR, 0, &fl), ONE);
    CHECK_EQ(rl_fp_convert(0x0001u, 16, 32, RL_RM_FPCR, 0, &fl), 0x33800000u);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp_convert(0x0001u, 16, 32, RL_RM_FPCR, RL_FPCR_FZ, &fl), 0x33800000u);
    CHECK_EQ(rl_fp_convert(0x3F800000ull, 32, 64, RL_RM_FPCR, 0, &fl), 0x3FF0000000000000ull);
    CHECK_EQ(rl_fp_convert(0x3FF0000000000001ull, 64, 32, RL_RM_FPCR, 0, &fl), ONE);
    CHECK_EQ(fl, IXC);
    fl = 0;
    CHECK_EQ(rl_fp_convert(0x3FF0000000000001ull, 64, 32, RL_RM_FPCR, RMODE(RL_RM_POSINF), NULL), 0x3F800001u);
    CHECK_EQ(rl_fp_convert(0x47F0000000000000ull, 64, 32, RL_RM_FPCR, 0, &fl), INF);
    CHECK_EQ(fl, OFC | IXC);
    fl = 0;
    CHECK_EQ(rl_fp_convert(0x47EFFFFFE0000000ull, 64, 32, RL_RM_FPCR, 0, &fl), 0x7F7FFFFFu);
    CHECK_EQ(rl_fp_convert(0x40F0000000000000ull, 64, 16, RL_RM_FPCR, 0, &fl), 0x7C00u);
    CHECK_EQ(fl, OFC | IXC);
    fl = 0;
    CHECK_EQ(rl_fp_convert(0x7FF0000000000001ull, 64, 32, RL_RM_FPCR, 0, &fl), QNAN);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp_convert(0x7FC12345u, 32, 64, RL_RM_FPCR, 0, &fl), 0x7FF0000000000000ull | ((uint64_t)0x412345 << 29));
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp_convert(0x7FC12345u, 32, 64, RL_RM_FPCR, DN, &fl), 0x7FF8000000000000ull);
    CHECK_EQ(rl_fp_convert(0xFF800000u, 32, 64, RL_RM_FPCR, 0, &fl), 0xFFF0000000000000ull);
    CHECK_EQ(rl_fp_convert(0x7C00u, 16, 32, RL_RM_FPCR, 0, &fl), INF);
    CHECK_EQ(rl_fp_convert(0x7E01u, 16, 32, RL_RM_FPCR, 0, &fl), 0x7FC02000u);
    CHECK_EQ(rl_fp_convert(0x7C01u, 16, 32, RL_RM_FPCR, 0, &fl), 0x7FC02000u);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp_convert(INF, 32, 16, RL_RM_FPCR, RL_FPCR_AHP, &fl), 0x7FFFu);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp_convert(0x7C00u, 16, 32, RL_RM_FPCR, RL_FPCR_AHP, &fl), 0x47800000u);
}

TEST(fp_to_fixed_saturates)
{
    uint32_t fl = 0;
    uint32_t v15 = 0x3FC00000u;
    CHECK_EQ(rl_fp_to_fixed(v15, 32, 32, 0, 0, RL_RM_NEAREST, 0, &fl), 2);
    CHECK_EQ(fl, IXC);
    fl = 0;
    CHECK_EQ(rl_fp_to_fixed(v15, 32, 32, 0, 0, RL_RM_ZERO, 0, &fl), 1);
    CHECK_EQ(rl_fp_to_fixed(v15, 32, 32, 0, 0, RL_RM_POSINF, 0, NULL), 2);
    CHECK_EQ(rl_fp_to_fixed(v15, 32, 32, 0, 0, RL_RM_NEGINF, 0, NULL), 1);
    fl = 0;
    CHECK_EQ(rl_fp_to_fixed(v15, 32, 32, 1, 0, RL_RM_ZERO, 0, &fl), 3);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp_to_fixed(0xC0200000u, 32, 32, 0, 0, RL_RM_AWAY, 0, NULL), 0xFFFFFFFDu);
    CHECK_EQ(rl_fp_to_fixed(0xC0200000u, 32, 32, 0, 0, RL_RM_NEAREST, 0, NULL), 0xFFFFFFFEu);
    CHECK_EQ(rl_fp_to_fixed(0x4F000000u, 32, 32, 0, 0, RL_RM_ZERO, 0, &fl), 0x7FFFFFFFu);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp_to_fixed(0xCF000000u, 32, 32, 0, 0, RL_RM_ZERO, 0, &fl), 0x80000000u);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fp_to_fixed(0xCF000001u, 32, 32, 0, 0, RL_RM_ZERO, 0, &fl), 0x80000000u);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp_to_fixed(0xBF800000u, 32, 32, 0, 1, RL_RM_ZERO, 0, &fl), 0);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp_to_fixed(0xBE800000u, 32, 32, 0, 1, RL_RM_ZERO, 0, &fl), 0);
    CHECK_EQ(fl, IXC);
    fl = 0;
    CHECK_EQ(rl_fp_to_fixed(0x4F800000u, 32, 32, 0, 1, RL_RM_ZERO, 0, &fl), 0xFFFFFFFFu);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp_to_fixed(0x7FC00000u, 32, 32, 0, 0, RL_RM_ZERO, 0, &fl), 0);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp_to_fixed(INF, 32, 64, 0, 0, RL_RM_ZERO, 0, &fl), 0x7FFFFFFFFFFFFFFFull);
    CHECK_EQ(fl, IOC);
    fl = 0;
    CHECK_EQ(rl_fp_to_fixed(NINF, 32, 64, 0, 0, RL_RM_ZERO, 0, &fl), 0x8000000000000000ull);
    CHECK_EQ(rl_fp_to_fixed(0x7149F2CAull, 32, 64, 0, 0, RL_RM_ZERO, 0, NULL), 0x7FFFFFFFFFFFFFFFull);
    CHECK_EQ(rl_fp_to_fixed(0x43E0000000000000ull, 64, 64, 0, 1, RL_RM_ZERO, 0, NULL), 0x8000000000000000ull);
    CHECK_EQ(rl_fp_to_fixed(0x43EFFFFFFFFFFFFFull, 64, 64, 0, 1, RL_RM_ZERO, 0, NULL), 0xFFFFFFFFFFFFF800ull);
    CHECK_EQ(rl_fp_to_fixed(0x43F0000000000000ull, 64, 64, 0, 1, RL_RM_ZERO, 0, NULL), 0xFFFFFFFFFFFFFFFFull);
    CHECK_EQ(rl_fp_to_fixed(0x7FEFFFFFFFFFFFFFull, 64, 64, 0, 1, RL_RM_ZERO, 0, &fl), 0xFFFFFFFFFFFFFFFFull);
    CHECK_EQ(rl_fp_to_fixed(0x3C00u, 16, 32, 0, 0, RL_RM_ZERO, 0, NULL), 1);
    CHECK_EQ(rl_fp_to_fixed(0x00000001u, 32, 32, 0, 0, RL_RM_POSINF, 0, NULL), 1);
    CHECK_EQ(rl_fp_to_fixed(0x00000001u, 32, 32, 0, 0, RL_RM_POSINF, FZ, &fl), 0);
}

TEST(fp_from_fixed)
{
    uint32_t fl = 0;
    CHECK_EQ(rl_fixed_to_fp(0xFFFFFFFFull, 32, 32, 0, 0, RL_RM_FPCR, 0, &fl), 0xBF800000u);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fixed_to_fp(0xFFFFFFFFull, 32, 32, 0, 1, RL_RM_FPCR, 0, &fl), 0x4F800000u);
    CHECK_EQ(fl, IXC);
    fl = 0;
    CHECK_EQ(rl_fixed_to_fp(3, 32, 32, 1, 0, RL_RM_FPCR, 0, &fl), 0x3FC00000u);
    CHECK_EQ(fl, 0);
    CHECK_EQ(rl_fixed_to_fp(0, 32, 32, 3, 0, RL_RM_FPCR, 0, &fl), 0);
    CHECK_EQ(rl_fixed_to_fp(0xFFFFFFFFFFFFFFFFull, 64, 32, 0, 1, RL_RM_FPCR, 0, &fl), 0x5F800000u);
    CHECK_EQ(rl_fixed_to_fp(0x8000000000000000ull, 64, 64, 0, 0, RL_RM_FPCR, 0, NULL), 0xC3E0000000000000ull);
    CHECK_EQ(rl_fixed_to_fp(0x8000000000000000ull, 64, 64, 0, 1, RL_RM_FPCR, 0, NULL), 0x43E0000000000000ull);
    CHECK_EQ(rl_fixed_to_fp(0x1000001ull, 32, 32, 0, 0, RL_RM_FPCR, 0, NULL), 0x4B800000u);
    CHECK_EQ(rl_fixed_to_fp(0x1000001ull, 32, 32, 0, 0, RL_RM_FPCR, RMODE(RL_RM_POSINF), NULL), 0x4B800001u);
    CHECK_EQ(rl_fixed_to_fp(100000, 32, 16, 0, 0, RL_RM_FPCR, 0, &fl), 0x7C00u);
    fl = 0;
    CHECK_EQ(rl_fixed_to_fp(0x7FFFFFFFull, 32, 64, 0, 0, RL_RM_FPCR, 0, &fl), 0x41DFFFFFFFC00000ull);
    CHECK_EQ(fl, 0);
}

static uint64_t g_rng = 0x9E3779B97F4A7C15ull;

static uint64_t rnd(void)
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return g_rng;
}

static uint32_t rnd32(void)
{
    uint64_t r = rnd();
    uint32_t x = (uint32_t)(r >> 16);
    switch (r & 7) {
    case 0: x = (x & 0x807FFFFFu) | ((127u - 8 + (uint32_t)(r >> 40) % 16) << 23); break;
    case 1: x &= 0x807FFFFFu; break;
    case 2: x = (x & 0x80000000u) | ((100u + (uint32_t)(r >> 40) % 56) << 23) | (x & 0x7Fu); break;
    default: break;
    }
    return x;
}

static uint64_t rnd64(void)
{
    uint64_t r = rnd();
    uint64_t x = rnd();
    switch (r & 7) {
    case 0: x = (x & 0x800FFFFFFFFFFFFFull) | ((1023ull - 8 + (r >> 40) % 16) << 52); break;
    case 1: x &= 0x800FFFFFFFFFFFFFull; break;
    case 2: x = (x & 0x8000000000000000ull) | ((900ull + (r >> 40) % 250) << 52) | (x & 0xFFu); break;
    default: break;
    }
    return x;
}

static float f32_of(uint32_t b)
{
    float f;
    memcpy(&f, &b, 4);
    return f;
}

static uint32_t bits_of_f32(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return b;
}

static double f64_of(uint64_t b)
{
    double f;
    memcpy(&f, &b, 8);
    return f;
}

static uint64_t bits_of_f64(double f)
{
    uint64_t b;
    memcpy(&b, &f, 8);
    return b;
}

static int is_nan32(uint32_t b) { return (b & 0x7FFFFFFFu) > 0x7F800000u; }
static int is_nan64(uint64_t b) { return (b & 0x7FFFFFFFFFFFFFFFull) > 0x7FF0000000000000ull; }

TEST(fp32_matches_host_round_to_nearest)
{
    for (int i = 0; i < 300000; i++) {
        volatile float x = f32_of(rnd32()), y = f32_of(rnd32());
        uint32_t xb = bits_of_f32(x), yb = bits_of_f32(y);
        uint32_t want[5] = { bits_of_f32(x + y), bits_of_f32(x - y), bits_of_f32(x * y), bits_of_f32(x / y),
                             bits_of_f32(__builtin_elementwise_sqrt(x)) };
        uint32_t got[5] = { rl_fp32_add(xb, yb, 0, NULL), rl_fp32_sub(xb, yb, 0, NULL), rl_fp32_mul(xb, yb, 0, NULL),
                            rl_fp32_div(xb, yb, 0, NULL), rl_fp32_sqrt(xb, 0, NULL) };
        for (int k = 0; k < 5; k++) {
            if (is_nan32(want[k])) {
                CHECK(is_nan32(got[k]) || is_nan32(xb) || is_nan32(yb));
                continue;
            }
            if (got[k] != want[k]) {
                fprintf(stderr, "  op %d x=%08x y=%08x got=%08x want=%08x\n", k, xb, yb, got[k], want[k]);
                CHECK_EQ(got[k], want[k]);
            }
        }
    }
}

TEST(fp64_matches_host_round_to_nearest)
{
    for (int i = 0; i < 300000; i++) {
        volatile double x = f64_of(rnd64()), y = f64_of(rnd64());
        uint64_t xb = bits_of_f64(x), yb = bits_of_f64(y);
        uint64_t want[5] = { bits_of_f64(x + y), bits_of_f64(x - y), bits_of_f64(x * y), bits_of_f64(x / y),
                             bits_of_f64(__builtin_elementwise_sqrt(x)) };
        uint64_t got[5] = { rl_fp64_add(xb, yb, 0, NULL), rl_fp64_sub(xb, yb, 0, NULL), rl_fp64_mul(xb, yb, 0, NULL),
                            rl_fp64_div(xb, yb, 0, NULL), rl_fp64_sqrt(xb, 0, NULL) };
        for (int k = 0; k < 5; k++) {
            if (is_nan64(want[k])) {
                CHECK(is_nan64(got[k]) || is_nan64(xb) || is_nan64(yb));
                continue;
            }
            if (got[k] != want[k]) {
                fprintf(stderr, "  op %d x=%016llx y=%016llx got=%016llx want=%016llx\n", k, (unsigned long long)xb,
                        (unsigned long long)yb, (unsigned long long)got[k], (unsigned long long)want[k]);
                CHECK_EQ(got[k], want[k]);
            }
        }
    }
}

TEST(fp_conversions_match_host)
{
    for (int i = 0; i < 300000; i++) {
        uint32_t xb = rnd32();
        uint64_t yb = rnd64();
        volatile float x = f32_of(xb);
        volatile double y = f64_of(yb);
        if (!is_nan32(xb))
            CHECK_EQ(rl_fp_convert(xb, 32, 64, RL_RM_FPCR, 0, NULL), bits_of_f64((double)x));
        if (!is_nan64(yb))
            CHECK_EQ(rl_fp_convert(yb, 64, 32, RL_RM_FPCR, 0, NULL), bits_of_f32((float)y));
        if (xb < 0x4F000000u || (xb & 0x80000000u && (xb & 0x7FFFFFFFu) < 0x4F000000u)) {
            long long t = (long long)x;
            CHECK_EQ(rl_fp_to_fixed(xb, 32, 64, 0, 0, RL_RM_ZERO, 0, NULL), (uint64_t)t);
        }
        int64_t si = (int64_t)rnd();
        if (i & 1)
            si >>= (rnd() & 31);
        CHECK_EQ(rl_fixed_to_fp((uint64_t)si, 64, 32, 0, 0, RL_RM_FPCR, 0, NULL), bits_of_f32((float)si));
        CHECK_EQ(rl_fixed_to_fp((uint64_t)si, 64, 64, 0, 1, RL_RM_FPCR, 0, NULL), bits_of_f64((double)(uint64_t)si));
    }
}
