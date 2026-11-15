/**
 * @file test_math.c
 * @brief Div, DivArm, Sqrt, ArcTan and ArcTan2 against the console's
 * results, the division by zero cases, and the affine set calls.
 */
#include "gbat.h"
#include "vectors_math.h"

TEST(math_calls_match_the_console)
{
    gbat *g = gbat_new();
    CHECK(sizeof gbat_math / sizeof gbat_math[0] > 500);
    for (size_t i = 0; i < sizeof gbat_math / sizeof gbat_math[0]; i++) {
        const gbat_vec *v = &gbat_math[i];
        rl_gba_swi_cpu c = gbat_cpu(v->in[0], v->in[1], v->in[2], v->in[3]);
        CHECK_EQ(gbat_swi(g, v->swi, &c), RL_GBA_SWI_OK);
        CHECK_EQ(c.r[0], v->out[0]);
        CHECK_EQ(c.r[1], v->out[1]);
        CHECK_EQ(c.r[2], v->in[2]);
        CHECK_EQ(c.r[3], v->out[2]);
    }
}

TEST(div_by_zero_returns_only_for_small_numerators)
{
    gbat *g = gbat_new();
    static const uint32_t ok[] = {0, 1, 0xFFFFFFFFu};
    for (unsigned i = 0; i < 3; i++) {
        rl_gba_swi_cpu c = gbat_cpu(ok[i], 0, 0, 0);
        CHECK_EQ(gbat_swi(g, 0x06, &c), RL_GBA_SWI_OK);
    }
    rl_gba_swi_cpu c = gbat_cpu(0, 0, 0, 0);
    gbat_swi(g, 0x06, &c);
    CHECK_EQ(c.r[0], 1);
    CHECK_EQ(c.r[1], 0);
    CHECK_EQ(c.r[3], 1);
    c = gbat_cpu(0xFFFFFFFFu, 0, 0, 0);
    gbat_swi(g, 0x06, &c);
    CHECK_EQ(c.r[0], 0xFFFFFFFFu);
    CHECK_EQ(c.r[1], 0xFFFFFFFFu);
    CHECK_EQ(c.r[3], 1);
    static const uint32_t hang[] = {2, 5, 0x80000000u, 0xFFFFFFFEu, 0x7FFFFFFFu};
    for (unsigned i = 0; i < 5; i++) {
        c = gbat_cpu(hang[i], 0, 0, 0);
        CHECK_EQ(gbat_swi(g, 0x06, &c), RL_GBA_SWI_HANG);
        c = gbat_cpu(0, hang[i], 0, 0);
        CHECK_EQ(gbat_swi(g, 0x07, &c), RL_GBA_SWI_HANG);
    }
}

TEST(sqrt_takes_the_input_as_unsigned)
{
    gbat *g = gbat_new();
    rl_gba_swi_cpu c = gbat_cpu(0xFFFFFFFFu, 0, 0, 0);
    gbat_swi(g, 0x08, &c);
    CHECK_EQ(c.r[0], 0xFFFF);
    c = gbat_cpu(0x80000000u, 0, 0, 0);
    gbat_swi(g, 0x08, &c);
    CHECK_EQ(c.r[0], 0xB504);
}

TEST(bg_affine_set_writes_the_matrix_and_start)
{
    gbat *g = gbat_new();
    gbat_w32(g, 0x02000000u, 0x00010000u);
    gbat_w32(g, 0x02000004u, 0x00020000u);
    gbat_w16(g, 0x02000008u, 8);
    gbat_w16(g, 0x0200000Au, 4);
    gbat_w16(g, 0x0200000Cu, 0x0100);
    gbat_w16(g, 0x0200000Eu, 0x0200);
    gbat_w16(g, 0x02000010u, 0x4000);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 1, 0);
    CHECK_EQ(gbat_swi(g, 0x0E, &c), RL_GBA_SWI_OK);
    CHECK_EQ(gbat_r16(g, 0x02001000u), 0);
    CHECK_EQ(gbat_r16(g, 0x02001002u), 0xFF00);
    CHECK_EQ(gbat_r16(g, 0x02001004u), 0x0200);
    CHECK_EQ(gbat_r16(g, 0x02001006u), 0);
    CHECK_EQ(gbat_r32(g, 0x02001008u), 0x00010000u + 0x0100u * 4u);
    CHECK_EQ(gbat_r32(g, 0x0200100Cu), 0x00020000u - 0x0200u * 8u);
    CHECK_EQ(c.r[0], 0x02000014u);
    CHECK_EQ(c.r[1], 0x02001010u);
    c = gbat_cpu(0x02000000u, 0x02002000u, 0, 0x1234);
    gbat_swi(g, 0x0E, &c);
    CHECK_EQ(c.r[3], 0x1234);
    CHECK_EQ(gbat_r32(g, 0x02002000u), 0);
}

TEST(obj_affine_set_steps_by_r3)
{
    gbat *g = gbat_new();
    gbat_w16(g, 0x02000000u, 0x0100);
    gbat_w16(g, 0x02000002u, 0x0080);
    gbat_w16(g, 0x02000004u, 0);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x07000006u, 1, 8);
    gbat_swi(g, 0x0F, &c);
    CHECK_EQ(gbat_r16(g, 0x07000006u), 0x0100);
    CHECK_EQ(gbat_r16(g, 0x0700000Eu), 0);
    CHECK_EQ(gbat_r16(g, 0x07000016u), 0);
    CHECK_EQ(gbat_r16(g, 0x0700001Eu), 0x0080);
    CHECK_EQ(c.r[0], 0x02000008u);
    CHECK_EQ(c.r[1], 0x07000026u);
    CHECK_EQ(c.r[3], 8);
}
