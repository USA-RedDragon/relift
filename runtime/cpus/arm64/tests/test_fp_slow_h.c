/**
 * @file test_fp_slow_h.c
 * @brief rl_fp_slow, binary16 forms: hand-encoded half precision FP and
 * AdvSIMD instructions run with non-default FPCR values (FZ16, DN, AHP,
 * rounding modes), checked bit for bit with the flags they raise.
 *
 * The instruction words come from an assembler (fullfp16); the expected
 * values are worked out from the Arm ARM pseudocode: FZ16 flushes half
 * precision inputs silently and results with UFC, AHP changes FCVT only.
 */
#include "arm64.h"
#include "runner.h"

#include <relift/arm64/fp_slow.h>

#define RZ ((uint32_t)RL_RM_ZERO << 22)
#define RP ((uint32_t)RL_RM_POSINF << 22)
#define FZ16 (1u << 19)
#define FZ RL_FPCR_FZ
#define DN RL_FPCR_DN
#define AHP RL_FPCR_AHP

#define IOC 0x01u
#define DZC 0x02u
#define OFC 0x04u
#define UFC 0x08u
#define IXC 0x10u

static const rl_guest_desc test_guest = {
    .name = "arm64-fp-slow-half-test",
    .page_size = 0x4000,
    .address_bits = 40,
    .tick_hz = 1000000,
};

static rl_ctx *boot(uint32_t fpcr)
{
    if (rl_core_init(&test_guest, rl_arm64_cpu()) != 0)
        return NULL;
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return NULL;
    rl_thread_bind(t);
    t->ctx->fpcr = fpcr;
    return t->ctx;
}

static int run(rl_ctx *c, uint32_t insn) { return rl_fp_slow(c, rl_mem_base(), 0x10000, insn); }

TEST(fp_slow_half_dp2_rounding_and_clear)
{
    rl_ctx *c = boot(0);
    CHECK(c != NULL);
    c->v[1].h[0] = 0x3C00;
    c->v[2].h[0] = 0x4000;
    c->v[0].q = ~(rl_u128)0;
    CHECK_EQ(run(c, 0x1EE22820u), RL_OK); /* fadd h0, h1, h2 */
    CHECK_EQ(c->v[0].h[0], 0x4200);
    CHECK_EQ(c->v[0].h[1], 0);
    CHECK_EQ(c->v[0].d[1], 0);
    CHECK_EQ(c->fpsr, 0);
    /* 1/3 rounded toward plus infinity is the half above 0x3555. */
    c->fpcr = RP;
    c->v[2].h[0] = 0x4200;
    CHECK_EQ(run(c, 0x1EE21820u), RL_OK); /* fdiv h0, h1, h2 */
    CHECK_EQ(c->v[0].h[0], 0x3556);
    CHECK_EQ(c->fpsr, IXC);
    c->fpcr = 0;
    CHECK_EQ(run(c, 0x1EE21820u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0x3555);
}

TEST(fp_slow_half_fz16_flush)
{
    rl_ctx *c = boot(FZ16);
    CHECK(c != NULL);
    /* A half subnormal input is read as zero, without Input Denormal. */
    c->v[1].h[0] = 0x0200;
    c->v[2].h[0] = 0x4000;
    CHECK_EQ(run(c, 0x1EE20820u), RL_OK); /* fmul h0, h1, h2 */
    CHECK_EQ(c->v[0].h[0], 0);
    CHECK_EQ(c->fpsr, 0);
    /* FZ alone does not flush half precision. */
    c->fpcr = FZ;
    CHECK_EQ(run(c, 0x1EE20820u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0x0400);
    CHECK_EQ(c->fpsr, 0);
    /* A subnormal result: 2^-14 * 0.5 is exact as 0x0200 but flushed with UFC. */
    c->v[1].h[0] = 0x0400;
    c->v[2].h[0] = 0x3800;
    c->fpcr = 0;
    CHECK_EQ(run(c, 0x1EE20820u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0x0200);
    CHECK_EQ(c->fpsr, 0);
    c->fpcr = FZ16;
    CHECK_EQ(run(c, 0x1EE20820u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0);
    CHECK_EQ(c->fpsr, UFC);
    /* Conversions ignore FZ16 (FPUnpackCV, FPRoundCV): 2^-24 converts
     * exactly both ways. */
    c->fpsr = 0;
    c->v[0].h[0] = 0x0001;
    CHECK_EQ(run(c, 0x1EE24001u), RL_OK); /* fcvt s1, h0 */
    CHECK_EQ(c->v[1].s[0], 0x33800000u);
    CHECK_EQ(run(c, 0x1E23C022u), RL_OK); /* fcvt h2, s1 */
    CHECK_EQ(c->v[2].h[0], 0x0001);
    CHECK_EQ(c->fpsr, 0);
}

TEST(fp_slow_half_default_nan_and_ahp_ignored)
{
    rl_ctx *c = boot(DN);
    CHECK(c != NULL);
    c->v[1].h[0] = 0x7E01;
    c->v[2].h[0] = 0x3C00;
    CHECK_EQ(run(c, 0x1EE22820u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0x7E00);
    CHECK_EQ(c->fpsr, 0);
    c->fpcr = 0;
    CHECK_EQ(run(c, 0x1EE22820u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0x7E01);
    c->v[1].h[0] = 0x7C01;
    CHECK_EQ(run(c, 0x1EE22820u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0x7E01);
    CHECK_EQ(c->fpsr, IOC);
    /* AHP affects FCVT only: an arithmetic overflow still gives infinity. */
    c->fpcr = AHP;
    c->fpsr = 0;
    c->v[1].h[0] = 0x7BFF;
    c->v[2].h[0] = 0x7BFF;
    CHECK_EQ(run(c, 0x1EE22820u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0x7C00);
    CHECK_EQ(c->fpsr, OFC | IXC);
    /* FCVT S0, H1 reads 0x7C00 as the AHP value 2^16. */
    c->v[1].h[0] = 0x7C00;
    c->fpsr = 0;
    CHECK_EQ(run(c, 0x1EE24020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x47800000u);
    CHECK_EQ(c->fpsr, 0);
}

TEST(fp_slow_half_dp1_and_dp3)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].h[0] = 0x4400;
    CHECK_EQ(run(c, 0x1EE1C020u), RL_OK); /* fsqrt h0, h1 */
    CHECK_EQ(c->v[0].h[0], 0x4000);
    c->v[0].q = ~(rl_u128)0;
    c->v[1].h[0] = 0x3C00;
    c->v[2].h[0] = 0x4000;
    c->v[3].h[0] = 0x4200;
    CHECK_EQ(run(c, 0x1FC20C20u), RL_OK); /* fmadd h0, h1, h2, h3 */
    CHECK_EQ(c->v[0].h[0], 0x4500);
    CHECK_EQ(c->v[0].h[1], 0);
}

TEST(fp_slow_half_compare)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].h[0] = 0x3C00;
    c->v[2].h[0] = 0x4000;
    CHECK_EQ(run(c, 0x1EE22020u), RL_OK); /* fcmp h1, h2 */
    CHECK_EQ(rl_nzcv(c), 0x80000000u);
    CHECK_EQ(run(c, 0x1EE02038u), RL_OK); /* fcmpe h1, #0.0 */
    CHECK_EQ(rl_nzcv(c), 0x20000000u);
    c->v[1].h[0] = 0x7E00;
    CHECK_EQ(run(c, 0x1EE22020u), RL_OK);
    CHECK_EQ(rl_nzcv(c), 0x30000000u);
    CHECK_EQ(c->fpsr, 0);
    CHECK_EQ(run(c, 0x1EE02038u), RL_OK);
    CHECK_EQ(rl_nzcv(c), 0x30000000u);
    CHECK_EQ(c->fpsr, IOC);
    /* fccmp h1, h2, #0, eq: the condition decides between the compare and #0. */
    c->v[1].h[0] = 0x3C00;
    rl_set_nzcv(c, 0);
    CHECK_EQ(run(c, 0x1EE20420u), RL_OK);
    CHECK_EQ(rl_nzcv(c), 0);
    rl_set_nzcv(c, 0x40000000u);
    CHECK_EQ(run(c, 0x1EE20420u), RL_OK);
    CHECK_EQ(rl_nzcv(c), 0x80000000u);
}

TEST(fp_slow_half_int_and_fixed)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].h[0] = 0x4100; /* 2.5 */
    CHECK_EQ(run(c, 0x1EF80020u), RL_OK); /* fcvtzs w0, h1 */
    CHECK_EQ(c->x[0], 2);
    CHECK_EQ(c->fpsr, IXC);
    CHECK_EQ(run(c, 0x9EE40020u), RL_OK); /* fcvtas x0, h1 */
    CHECK_EQ(c->x[0], 3);
    c->x[1] = 3;
    CHECK_EQ(run(c, 0x1EE20020u), RL_OK); /* scvtf h0, w1 */
    CHECK_EQ(c->v[0].h[0], 0x4200);
    c->x[1] = 24;
    CHECK_EQ(run(c, 0x1EC2F020u), RL_OK); /* scvtf h0, w1, #4 */
    CHECK_EQ(c->v[0].h[0], 0x3E00);
    c->v[1].h[0] = 0x3E00;
    CHECK_EQ(run(c, 0x1ED8F020u), RL_OK); /* fcvtzs w0, h1, #4 */
    CHECK_EQ(c->x[0], 24);
    /* Infinity saturates; AHP does not make 0x7C00 a finite number here. */
    c->fpcr = AHP;
    c->fpsr = 0;
    c->v[1].h[0] = 0x7C00;
    CHECK_EQ(run(c, 0x1EF80020u), RL_OK);
    CHECK_EQ(c->x[0], 0x7FFFFFFFu);
    CHECK_EQ(c->fpsr, IOC);
}

TEST(fp_slow_half_vector_same)
{
    rl_ctx *c = boot(FZ16);
    CHECK(c != NULL);
    const uint16_t a[8] = { 0x3C00, 0x4000, 0x0200, 0x4200, 0x3C00, 0x3C00, 0x3C00, 0x3C00 };
    const uint16_t b[8] = { 0x4000, 0x4000, 0x4000, 0x7E00, 1, 1, 1, 1 };
    for (unsigned i = 0; i < 8; i++) {
        c->v[1].h[i] = a[i];
        c->v[2].h[i] = b[i];
    }
    c->v[0].q = ~(rl_u128)0;
    CHECK_EQ(run(c, 0x0E421420u), RL_OK); /* fadd v0.4h, v1.4h, v2.4h */
    CHECK_EQ(c->v[0].h[0], 0x4200);
    CHECK_EQ(c->v[0].h[1], 0x4400);
    CHECK_EQ(c->v[0].h[2], 0x4000); /* the subnormal input is zero */
    CHECK_EQ(c->v[0].h[3], 0x7E00);
    CHECK_EQ(c->v[0].d[1], 0);
    CHECK_EQ(run(c, 0x6E421C20u), RL_OK); /* fmul v0.8h, v1.8h, v2.8h */
    CHECK_EQ(c->v[0].h[0], 0x4000);
    CHECK_EQ(c->v[0].h[2], 0);
    CHECK_EQ(c->v[0].h[4], 0); /* the input 2^-24 is a subnormal, flushed */
    CHECK_EQ(c->fpsr, 0);
    CHECK_EQ(run(c, 0x2E421420u), RL_OK); /* faddp v0.4h, v1.4h, v2.4h */
    CHECK_EQ(c->v[0].h[0], 0x4200); /* 1 + 2 */
    CHECK_EQ(c->v[0].h[1], 0x4200); /* 0 + 3 */
    CHECK_EQ(c->v[0].h[2], 0x4400); /* 2 + 2 */
    CHECK_EQ(c->v[0].h[3], 0x7E00); /* 2 + NaN */
}

TEST(fp_slow_half_scalar_same_and_pair)
{
    rl_ctx *c = boot(0);
    CHECK(c != NULL);
    c->v[1].h[0] = 0x4000;
    c->v[1].h[1] = 0x3C00;
    c->v[2].h[0] = 0x3C00;
    c->v[0].q = ~(rl_u128)0;
    CHECK_EQ(run(c, 0x7EC21420u), RL_OK); /* fabd h0, h1, h2 */
    CHECK_EQ(c->v[0].h[0], 0x3C00);
    CHECK_EQ(c->v[0].h[1], 0);
    CHECK_EQ(run(c, 0x5E422420u), RL_OK); /* fcmeq h0, h1, h2 */
    CHECK_EQ(c->v[0].h[0], 0);
    c->v[2].h[0] = 0x4000;
    CHECK_EQ(run(c, 0x5E422420u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0xFFFF);
    CHECK_EQ(run(c, 0x5E30D820u), RL_OK); /* faddp h0, v1.2h */
    CHECK_EQ(c->v[0].h[0], 0x4200);
    CHECK_EQ(c->v[0].h[1], 0);
}

TEST(fp_slow_half_vector_misc)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].h[0] = 0x0000;
    c->v[1].h[1] = 0x8000;
    c->v[1].h[2] = 0x3C00;
    c->v[1].h[3] = 0x7E00;
    CHECK_EQ(run(c, 0x0EF8D820u), RL_OK); /* fcmeq v0.4h, v1.4h, #0.0 */
    CHECK_EQ(c->v[0].h[0], 0xFFFF);
    CHECK_EQ(c->v[0].h[1], 0xFFFF);
    CHECK_EQ(c->v[0].h[2], 0);
    CHECK_EQ(c->v[0].h[3], 0);
    /* Estimates of zero, a negative number, infinity and a quiet NaN. */
    c->v[1].h[0] = 0x0000;
    c->v[1].h[1] = 0xBC00;
    c->v[1].h[2] = 0x7C00;
    c->v[1].h[3] = 0x7E00;
    c->fpsr = 0;
    CHECK_EQ(run(c, 0x2EF9D820u), RL_OK); /* frsqrte v0.4h, v1.4h */
    CHECK_EQ(c->v[0].h[0], 0x7C00);
    CHECK_EQ(c->v[0].h[1], 0x7E00);
    CHECK_EQ(c->v[0].h[2], 0);
    CHECK_EQ(c->v[0].h[3], 0x7E00);
    CHECK_EQ(c->fpsr, DZC | IOC);
    /* Conversions to integer round toward zero here and saturate. */
    c->v[1].h[0] = 0x4100;
    c->v[1].h[1] = 0xC100;
    c->v[1].h[2] = 0x7C00;
    c->v[1].h[3] = 0x7E00;
    c->fpsr = 0;
    CHECK_EQ(run(c, 0x0EF9B820u), RL_OK); /* fcvtzs v0.4h, v1.4h */
    CHECK_EQ(c->v[0].h[0], 2);
    CHECK_EQ(c->v[0].h[1], 0xFFFE);
    CHECK_EQ(c->v[0].h[2], 0x7FFF);
    CHECK_EQ(c->v[0].h[3], 0);
    CHECK_EQ(c->fpsr, IOC | IXC);
    /* 2049 is not a half: round toward plus infinity gives 2050. */
    c->fpcr = RP;
    c->v[1].h[0] = 1;
    c->v[1].h[1] = 2049;
    c->v[1].h[2] = 0xFFFF;
    c->v[1].h[3] = 0;
    CHECK_EQ(run(c, 0x4E79D820u), RL_OK); /* scvtf v0.8h, v1.8h */
    CHECK_EQ(c->v[0].h[0], 0x3C00);
    CHECK_EQ(c->v[0].h[1], 0x6801);
    CHECK_EQ(c->v[0].h[2], 0xBC00);
    /* frintx rounds with the FPCR mode and signals inexact. */
    c->fpcr = RZ;
    c->fpsr = 0;
    c->v[1].h[0] = 0x4100;
    c->v[1].h[1] = 0xC100;
    c->v[1].h[2] = 0x3C00;
    c->v[1].h[3] = 0x0001;
    CHECK_EQ(run(c, 0x2E799820u), RL_OK); /* frintx v0.4h, v1.4h */
    CHECK_EQ(c->v[0].h[0], 0x4000);
    CHECK_EQ(c->v[0].h[1], 0xC000);
    CHECK_EQ(c->v[0].h[2], 0x3C00);
    CHECK_EQ(c->v[0].h[3], 0);
    CHECK_EQ(c->fpsr, IXC);
}

TEST(fp_slow_half_across_lanes)
{
    rl_ctx *c = boot(0);
    CHECK(c != NULL);
    const uint16_t a[8] = { 0x3C00, 0x4200, 0x4000, 0x7E00, 0x4400, 0x4100, 0x3C00, 0x3C00 };
    for (unsigned i = 0; i < 8; i++)
        c->v[1].h[i] = a[i];
    c->v[0].q = ~(rl_u128)0;
    CHECK_EQ(run(c, 0x0E30F820u), RL_OK); /* fmaxv h0, v1.4h: a NaN wins */
    CHECK_EQ(c->v[0].h[0], 0x7E00);
    CHECK_EQ(c->v[0].h[1], 0);
    CHECK_EQ(c->v[0].d[1], 0);
    CHECK_EQ(run(c, 0x4E30C820u), RL_OK); /* fmaxnmv h0, v1.8h: a NaN is ignored */
    CHECK_EQ(c->v[0].h[0], 0x4400);
}

TEST(fp_slow_half_shift_and_element)
{
    rl_ctx *c = boot(0);
    CHECK(c != NULL);
    c->v[1].h[0] = 8;
    c->v[1].h[1] = 12;
    c->v[1].h[2] = 0xFFF8;
    c->v[1].h[3] = 0;
    CHECK_EQ(run(c, 0x0F1DE420u), RL_OK); /* scvtf v0.4h, v1.4h, #3 */
    CHECK_EQ(c->v[0].h[0], 0x3C00);
    CHECK_EQ(c->v[0].h[1], 0x3E00);
    CHECK_EQ(c->v[0].h[2], 0xBC00);
    c->v[1].h[0] = 0x3E00;
    c->v[0].q = ~(rl_u128)0;
    CHECK_EQ(run(c, 0x5F1DFC20u), RL_OK); /* fcvtzs h0, h1, #3 */
    CHECK_EQ(c->v[0].h[0], 12);
    CHECK_EQ(c->v[0].h[1], 0);
    /* fmul v0.4h, v1.4h, v2.h[3] */
    c->v[1].h[0] = 0x3C00;
    c->v[1].h[1] = 0x4000;
    c->v[1].h[2] = 0x4200;
    c->v[1].h[3] = 0x3800;
    c->v[2].h[3] = 0x4000;
    CHECK_EQ(run(c, 0x0F329020u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0x4000);
    CHECK_EQ(c->v[0].h[1], 0x4400);
    CHECK_EQ(c->v[0].h[2], 0x4600);
    CHECK_EQ(c->v[0].h[3], 0x3C00);
    /* fmla h0, h1, v2.h[7]: 1 + 2 * 3 */
    c->v[0].h[0] = 0x3C00;
    c->v[1].h[0] = 0x4000;
    c->v[2].h[7] = 0x4200;
    CHECK_EQ(run(c, 0x5F321820u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0x4700);
    CHECK_EQ(c->v[0].h[1], 0);
}

TEST(fp_slow_complex_and_bfloat)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    /* fcmla v0.4s, v1.4s, v2.4s, #90: re gets n.im * -m.im, im gets n.im * m.re */
    c->v[1].s[0] = 0x3F800000u;
    c->v[1].s[1] = 0x40000000u;
    c->v[1].s[2] = 0;
    c->v[1].s[3] = 0;
    c->v[2].s[0] = 0x40400000u;
    c->v[2].s[1] = 0x40800000u;
    c->v[2].s[2] = 0;
    c->v[2].s[3] = 0;
    c->v[0].s[0] = 0;
    c->v[0].s[1] = 0;
    c->v[0].s[2] = 0x3F800000u;
    c->v[0].s[3] = 0;
    CHECK_EQ(run(c, 0x6E82CC20u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xC1000000u);
    CHECK_EQ(c->v[0].s[1], 0x40C00000u);
    CHECK_EQ(c->v[0].s[2], 0x3F800000u);
    /* fcadd v0.2d, v1.2d, v2.2d, #270: (n.re + m.im, n.im - m.re) */
    c->v[1].d[0] = 0x3FF0000000000000ull;
    c->v[1].d[1] = 0x4000000000000000ull;
    c->v[2].d[0] = 0x4008000000000000ull;
    c->v[2].d[1] = 0x4010000000000000ull;
    CHECK_EQ(run(c, 0x6EC2F420u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0x4014000000000000ull);
    CHECK_EQ(c->v[0].d[1], 0xBFF0000000000000ull);
    /* bfmlalb v0.4s, v1.8h, v2.8h: the even bfloat16 lanes, 0.5 + 1 * 3 and 2 * 4 */
    c->v[1].q = 0;
    c->v[2].q = 0;
    c->v[0].q = 0;
    c->v[1].h[0] = 0x3F80;
    c->v[1].h[1] = 0x4100;
    c->v[1].h[2] = 0x4000;
    c->v[2].h[0] = 0x4040;
    c->v[2].h[2] = 0x4080;
    c->v[0].s[0] = 0x3F000000u;
    CHECK_EQ(run(c, 0x2EC2FC20u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40600000u);
    CHECK_EQ(c->v[0].s[1], 0x41000000u);
    /* bfmlalt v0.4s, v1.8h, v2.h[3]: the odd lanes times element 3 */
    c->v[1].q = 0;
    c->v[2].q = 0;
    c->v[0].q = 0;
    c->v[1].h[1] = 0x3F80;
    c->v[1].h[3] = 0x4000;
    c->v[2].h[3] = 0x4040;
    CHECK_EQ(run(c, 0x4FF2F020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40400000u);
    CHECK_EQ(c->v[0].s[1], 0x40C00000u);
}
