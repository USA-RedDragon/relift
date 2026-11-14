/**
 * @file test_fp_slow.c
 * @brief rl_fp_slow: hand-encoded FP instructions run with non-default FPCR
 * values (round toward zero, round toward plus infinity, FZ, DN), checked
 * bit for bit, plus NZCV for compares and the unknown-encoding report.
 */
#include "a64.h"
#include "runner.h"

#include <relift/a64/fp_slow.h>

#define RZ ((uint32_t)RL_RM_ZERO << 22)
#define RP ((uint32_t)RL_RM_POSINF << 22)
#define FZ RL_FPCR_FZ
#define DN RL_FPCR_DN

#define IOC 0x01u
#define OFC 0x04u
#define UFC 0x08u
#define IXC 0x10u
#define IDC 0x80u

#define DBL 0x00400000u
#define W64 0x80000000u

static const rl_guest_desc test_guest = {
    .name = "a64-fp-slow-test",
    .page_size = 0x4000,
    .address_bits = 40,
    .tick_hz = 1000000,
};

static rl_ctx *boot(uint32_t fpcr)
{
    if (rl_core_init(&test_guest, rl_a64_cpu()) != 0)
        return NULL;
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return NULL;
    rl_thread_bind(t);
    t->ctx->fpcr = fpcr;
    return t->ctx;
}

static int run(rl_ctx *c, uint32_t insn) { return rl_fp_slow(c, rl_mem_base(), 0x10000, insn); }

TEST(fp_slow_div_rounds_per_fpcr)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x3F800000u;
    c->v[2].s[0] = 0x40400000u;
    c->v[0].q = ~(rl_u128)0;
    CHECK_EQ(run(c, 0x1E221820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x3EAAAAAAu);
    CHECK_EQ(c->v[0].s[1], 0);
    CHECK_EQ(c->v[0].d[1], 0);
    CHECK_EQ(c->fpsr, IXC);
    c->fpcr = 0;
    CHECK_EQ(run(c, 0x1E221820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x3EAAAAABu);
}

TEST(fp_slow_double_arithmetic)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].d[0] = 0x3FF0000000000000ull;
    c->v[2].d[0] = 0x4008000000000000ull;
    CHECK_EQ(run(c, 0x1E621820u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0x3FD5555555555555ull);
    CHECK_EQ(c->v[0].d[1], 0);
    CHECK_EQ(run(c, 0x1E622820u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0x4010000000000000ull);
    CHECK_EQ(run(c, 0x1E623820u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0xC000000000000000ull);
    CHECK_EQ(run(c, 0x1E620820u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0x4008000000000000ull);
    CHECK_EQ(run(c, 0x1E628820u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0xC008000000000000ull);
}

TEST(fp_slow_single_arithmetic_and_minmax)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x3F800000u;
    c->v[2].s[0] = 0x40000000u;
    CHECK_EQ(run(c, 0x1E222820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40400000u);
    CHECK_EQ(run(c, 0x1E223820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xBF800000u);
    CHECK_EQ(run(c, 0x1E220820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40000000u);
    CHECK_EQ(run(c, 0x1E228820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xC0000000u);
    CHECK_EQ(run(c, 0x1E224820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40000000u);
    CHECK_EQ(run(c, 0x1E225820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x3F800000u);
    c->v[1].s[0] = 0x7FC00001u;
    CHECK_EQ(run(c, 0x1E224820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x7FC00001u);
    CHECK_EQ(run(c, 0x1E226820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40000000u);
    CHECK_EQ(run(c, 0x1E227820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40000000u);
    CHECK_EQ(c->fpsr, 0);
    c->v[1].s[0] = 0x7F800001u;
    CHECK_EQ(run(c, 0x1E226820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x7FC00001u);
    CHECK_EQ(c->fpsr, IOC);
}

TEST(fp_slow_zero_signs_in_minmax)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x80000000u;
    c->v[2].s[0] = 0;
    CHECK_EQ(run(c, 0x1E224820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0);
    CHECK_EQ(run(c, 0x1E225820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x80000000u);
}

TEST(fp_slow_flush_to_zero_and_default_nan)
{
    rl_ctx *c = boot(FZ);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x00000001u;
    c->v[2].s[0] = 0;
    CHECK_EQ(run(c, 0x1E222820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0);
    CHECK_EQ(c->fpsr, IDC);
    c->fpsr = 0;
    c->v[2].s[0] = 0x80000000u;
    CHECK_EQ(run(c, 0x1E224820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0);
    CHECK_EQ(c->fpsr, IDC);
    c->fpcr = DN;
    c->v[1].s[0] = 0x7FC00123u;
    c->v[2].s[0] = 0x3F800000u;
    CHECK_EQ(run(c, 0x1E222820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x7FC00000u);
}

TEST(fp_slow_one_source_ops)
{
    rl_ctx *c = boot(RP);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x40000000u;
    CHECK_EQ(run(c, 0x1E21C020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x3FB504F4u);
    c->fpcr = RZ;
    CHECK_EQ(run(c, 0x1E21C020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x3FB504F3u);
    c->v[1].s[0] = 0xC0200000u;
    CHECK_EQ(run(c, 0x1E20C020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40200000u);
    CHECK_EQ(run(c, 0x1E214020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40200000u);
    CHECK_EQ(run(c, 0x1E204020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xC0200000u);
}

TEST(fp_slow_round_to_integral)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x40200000u;
    CHECK_EQ(run(c, 0x1E244020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40000000u);
    CHECK_EQ(run(c, 0x1E24C020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40400000u);
    CHECK_EQ(run(c, 0x1E25C020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40000000u);
    CHECK_EQ(run(c, 0x1E264020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40400000u);
    CHECK_EQ(c->fpsr, 0);
    c->fpcr = RP;
    CHECK_EQ(run(c, 0x1E27C020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40400000u);
    CHECK_EQ(c->fpsr, 0);
    CHECK_EQ(run(c, 0x1E274020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40400000u);
    CHECK_EQ(c->fpsr, IXC);
}

TEST(fp_slow_fcvt_between_precisions)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].d[0] = 0x3FD5555555555555ull;
    CHECK_EQ(run(c, 0x1E624020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x3EAAAAAAu);
    CHECK_EQ(c->v[0].d[0] >> 32, 0);
    c->fpcr = 0;
    CHECK_EQ(run(c, 0x1E624020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x3EAAAAABu);
    c->v[1].s[0] = 0x3F800000u;
    CHECK_EQ(run(c, 0x1E22C020u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0x3FF0000000000000ull);
    CHECK_EQ(run(c, 0x1E23C020u), RL_OK);
    CHECK_EQ(c->v[0].h[0], 0x3C00);
    CHECK_EQ(c->v[0].s[0] >> 16, 0);
}

TEST(fp_slow_fused_multiply_add)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x40000000u;
    c->v[2].s[0] = 0x40400000u;
    c->v[3].s[0] = 0x3F800000u;
    CHECK_EQ(run(c, 0x1F020C20u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40E00000u);
    CHECK_EQ(run(c, 0x1F028C20u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xC0A00000u);
    CHECK_EQ(run(c, 0x1F220C20u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xC0E00000u);
    CHECK_EQ(run(c, 0x1F228C20u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40A00000u);
    c->v[1].d[0] = 0x4000000000000000ull;
    c->v[2].d[0] = 0x4008000000000000ull;
    c->v[3].d[0] = 0x3FF0000000000000ull;
    CHECK_EQ(run(c, 0x1F420C20u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0x401C000000000000ull);
    CHECK_EQ(run(c, 0x1F428C20u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0xC014000000000000ull);
    CHECK_EQ(run(c, 0x1F620C20u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0xC01C000000000000ull);
    CHECK_EQ(run(c, 0x1F628C20u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0x4014000000000000ull);
}

TEST(fp_slow_fused_multiply_add_rounds_once)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x3F800001u;
    c->v[2].s[0] = 0x3F800001u;
    c->v[3].s[0] = 0xBF800000u;
    CHECK_EQ(run(c, 0x1F020C20u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x34800000u);
}

TEST(fp_slow_compare_sets_nzcv)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x3F800000u;
    c->v[2].s[0] = 0x40000000u;
    CHECK_EQ(run(c, 0x1E222020u), RL_OK);
    CHECK_EQ(c->nzcv, 0x80000000u);
    c->v[2].s[0] = 0x3F800000u;
    CHECK_EQ(run(c, 0x1E222020u), RL_OK);
    CHECK_EQ(c->nzcv, 0x60000000u);
    c->v[2].s[0] = 0;
    CHECK_EQ(run(c, 0x1E222020u), RL_OK);
    CHECK_EQ(c->nzcv, 0x20000000u);
    CHECK_EQ(run(c, 0x1E202028u), RL_OK);
    CHECK_EQ(c->nzcv, 0x20000000u);
    c->v[2].s[0] = 0x7FC00000u;
    CHECK_EQ(run(c, 0x1E222020u), RL_OK);
    CHECK_EQ(c->nzcv, 0x30000000u);
    CHECK_EQ(c->fpsr, 0);
    CHECK_EQ(run(c, 0x1E222030u), RL_OK);
    CHECK_EQ(c->nzcv, 0x30000000u);
    CHECK_EQ(c->fpsr, IOC);
    c->fpsr = 0;
    c->v[2].s[0] = 0x7F800001u;
    CHECK_EQ(run(c, 0x1E222020u), RL_OK);
    CHECK_EQ(c->fpsr, IOC);
}

TEST(fp_slow_compare_double_and_flush)
{
    rl_ctx *c = boot(FZ);
    CHECK(c != NULL);
    c->v[1].d[0] = 1;
    c->v[2].d[0] = 0;
    CHECK_EQ(run(c, 0x1E622020u), RL_OK);
    CHECK_EQ(c->nzcv, 0x60000000u);
    CHECK_EQ(c->fpsr, IDC);
    c->v[1].d[0] = 0x4000000000000000ull;
    c->v[2].d[0] = 0x3FF0000000000000ull;
    CHECK_EQ(run(c, 0x1E622020u), RL_OK);
    CHECK_EQ(c->nzcv, 0x20000000u);
}

TEST(fp_slow_conditional_compare_and_select)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x3F800000u;
    c->v[2].s[0] = 0x40000000u;
    c->nzcv = 0x40000000u;
    CHECK_EQ(run(c, 0x1E220425u), RL_OK);
    CHECK_EQ(c->nzcv, 0x80000000u);
    c->nzcv = 0;
    CHECK_EQ(run(c, 0x1E220425u), RL_OK);
    CHECK_EQ(c->nzcv, 0x50000000u);
    CHECK_EQ(run(c, 0x1E221C20u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40000000u);
    c->nzcv = 0;
    CHECK_EQ(run(c, 0x1E221C20u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x3F800000u);
}

TEST(fp_slow_move_immediate)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[0].q = ~(rl_u128)0;
    CHECK_EQ(run(c, 0x1E2E1000u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x3F800000u);
    CHECK_EQ(c->v[0].s[1], 0);
    CHECK_EQ(run(c, 0x1E701000u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0xC000000000000000ull);
    CHECK_EQ(run(c, 0x1E6E1000u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0x3FF0000000000000ull);
}

TEST(fp_slow_float_to_integer)
{
    rl_ctx *c = boot(RP);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x40200000u;
    CHECK_EQ(run(c, 0x1E200020u), RL_OK);
    CHECK_EQ(c->x[0], 2);
    CHECK_EQ(run(c, 0x1E240020u), RL_OK);
    CHECK_EQ(c->x[0], 3);
    CHECK_EQ(run(c, 0x1E380020u), RL_OK);
    CHECK_EQ(c->x[0], 2);
    CHECK_EQ(run(c, 0x1E280020u), RL_OK);
    CHECK_EQ(c->x[0], 3);
    CHECK_EQ(run(c, 0x1E300020u), RL_OK);
    CHECK_EQ(c->x[0], 2);
    c->v[1].s[0] = 0xC0200000u;
    CHECK_EQ(run(c, 0x1E300020u), RL_OK);
    CHECK_EQ(c->x[0], 0xFFFFFFFDu);
    CHECK_EQ(run(c, 0x9E380020u), RL_OK);
    CHECK_EQ(c->x[0], 0xFFFFFFFFFFFFFFFEull);
    CHECK_EQ(c->fpsr, IXC);
    c->fpsr = 0;
    c->v[1].s[0] = 0xBF800000u;
    CHECK_EQ(run(c, 0x1E390020u), RL_OK);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(c->fpsr, IOC);
    c->v[1].d[0] = 0x4024000000000000ull;
    CHECK_EQ(run(c, 0x9E790020u), RL_OK);
    CHECK_EQ(c->x[0], 10);
    c->v[1].d[0] = 0x7FF8000000000000ull;
    CHECK_EQ(run(c, 0x1E780020u), RL_OK);
    CHECK_EQ(c->x[0], 0);
    c->v[1].d[0] = 0x4290000000000000ull;
    CHECK_EQ(run(c, 0x1E780020u), RL_OK);
    CHECK_EQ(c->x[0], 0x7FFFFFFFu);
}

TEST(fp_slow_integer_to_float)
{
    rl_ctx *c = boot(RP);
    CHECK(c != NULL);
    c->x[1] = 16777217;
    CHECK_EQ(run(c, 0x1E220020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x4B800001u);
    c->fpcr = RZ;
    CHECK_EQ(run(c, 0x1E220020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x4B800000u);
    c->x[1] = 0xFFFFFFFFull;
    CHECK_EQ(run(c, 0x1E230020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x4F7FFFFFu);
    CHECK_EQ(run(c, 0x1E220020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xBF800000u);
    c->x[1] = 3;
    CHECK_EQ(run(c, 0x9E620020u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0x4008000000000000ull);
}

TEST(fp_slow_move_between_general_and_fp)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[0].q = ~(rl_u128)0;
    c->x[1] = 0xAAAAAAAA12345678ull;
    CHECK_EQ(run(c, 0x1E270020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x12345678u);
    CHECK_EQ(c->v[0].s[1], 0);
    CHECK_EQ(c->v[0].d[1], 0);
    CHECK_EQ(run(c, 0x9E670020u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0xAAAAAAAA12345678ull);
    c->v[1].d[0] = 0x1122334455667788ull;
    c->v[1].d[1] = 0x99AABBCCDDEEFF00ull;
    CHECK_EQ(run(c, 0x9E660022u), RL_OK);
    CHECK_EQ(c->x[2], 0x1122334455667788ull);
    CHECK_EQ(run(c, 0x1E260022u), RL_OK);
    CHECK_EQ(c->x[2], 0x55667788u);
    CHECK_EQ(run(c, 0x9EAE0022u), RL_OK);
    CHECK_EQ(c->x[2], 0x99AABBCCDDEEFF00ull);
    CHECK_EQ(run(c, 0x9EAF0022u), RL_OK);
    CHECK_EQ(c->v[2].d[1], c->x[1]);
}

TEST(fp_slow_fixed_point_conversions)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->x[1] = 256;
    CHECK_EQ(run(c, 0x1E02E020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x3F800000u);
    c->v[1].s[0] = 0x3FC00000u;
    CHECK_EQ(run(c, 0x1E18F020u), RL_OK);
    CHECK_EQ(c->x[0], 24);
    CHECK_EQ(run(c, 0x1E19F020u), RL_OK);
    CHECK_EQ(c->x[0], 24);
    c->x[1] = 3;
    CHECK_EQ(run(c, 0x1E03F020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x3E400000u);
    CHECK_EQ(run(c, 0x1E024020u), RL_PENDING);
}

TEST(fp_slow_vector_arithmetic)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    for (int i = 0; i < 4; i++) {
        c->v[1].s[i] = 0x3F800000u;
        c->v[2].s[i] = 0x40400000u;
    }
    CHECK_EQ(run(c, 0x6E22FC20u), RL_OK);
    for (int i = 0; i < 4; i++)
        CHECK_EQ(c->v[0].s[i], 0x3EAAAAAAu);
    c->v[0].q = ~(rl_u128)0;
    CHECK_EQ(run(c, 0x2E22FC20u), RL_OK);
    CHECK_EQ(c->v[0].s[1], 0x3EAAAAAAu);
    CHECK_EQ(c->v[0].d[1], 0);
    c->v[1].d[0] = c->v[1].d[1] = 0x3FF0000000000000ull;
    c->v[2].d[0] = c->v[2].d[1] = 0x4008000000000000ull;
    CHECK_EQ(run(c, 0x6E62FC20u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0x3FD5555555555555ull);
    CHECK_EQ(c->v[0].d[1], 0x3FD5555555555555ull);
    CHECK_EQ(run(c, 0x0E62D420u), RL_PENDING);
}

TEST(fp_slow_vector_add_sub_mul_fma)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    for (int i = 0; i < 4; i++) {
        c->v[1].s[i] = 0x40000000u;
        c->v[2].s[i] = 0x40400000u;
    }
    CHECK_EQ(run(c, 0x4E22D420u), RL_OK);
    CHECK_EQ(c->v[0].s[3], 0x40A00000u);
    CHECK_EQ(run(c, 0x4EA2D420u), RL_OK);
    CHECK_EQ(c->v[0].s[2], 0xBF800000u);
    CHECK_EQ(run(c, 0x6E22DC20u), RL_OK);
    CHECK_EQ(c->v[0].s[1], 0x40C00000u);
    for (int i = 0; i < 4; i++)
        c->v[0].s[i] = 0x3F800000u;
    CHECK_EQ(run(c, 0x4E22CC20u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40E00000u);
    for (int i = 0; i < 4; i++)
        c->v[0].s[i] = 0x3F800000u;
    CHECK_EQ(run(c, 0x4EA2CC20u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xC0A00000u);
    CHECK_EQ(run(c, 0x4E22F420u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40400000u);
    CHECK_EQ(run(c, 0x4EA2F420u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40000000u);
}

TEST(fp_slow_vector_pairwise_add)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    for (int i = 0; i < 4; i++) {
        c->v[1].s[i] = 0x3F800000u + 0x00800000u * (uint32_t)i;
        c->v[2].s[i] = 0x40A00000u;
    }
    c->v[1].s[0] = 0x3F800000u;
    c->v[1].s[1] = 0x40000000u;
    c->v[1].s[2] = 0x40400000u;
    c->v[1].s[3] = 0x40800000u;
    CHECK_EQ(run(c, 0x6E22D420u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40400000u);
    CHECK_EQ(c->v[0].s[1], 0x40E00000u);
    CHECK_EQ(c->v[0].s[2], 0x41200000u);
    CHECK_EQ(c->v[0].s[3], 0x41200000u);
}

TEST(fp_slow_vector_convert)
{
    rl_ctx *c = boot(RP);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x40200000u;
    c->v[1].s[1] = 0xC0200000u;
    c->v[1].s[2] = 0x501502F9u;
    c->v[1].s[3] = 0x7FC00000u;
    CHECK_EQ(run(c, 0x4EA1B820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 2);
    CHECK_EQ(c->v[0].s[1], 0xFFFFFFFEu);
    CHECK_EQ(c->v[0].s[2], 0x7FFFFFFFu);
    CHECK_EQ(c->v[0].s[3], 0);
    CHECK_EQ(c->fpsr, IOC | IXC);
    CHECK_EQ(run(c, 0x4E21A820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 2);
    c->v[1].s[0] = 16777217;
    c->v[1].s[1] = 16777217;
    CHECK_EQ(run(c, 0x4E21D820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x4B800001u);
    c->fpcr = RZ;
    CHECK_EQ(run(c, 0x4E21D820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x4B800000u);
}

TEST(fp_slow_fcvtxn_rounds_to_odd)
{
    /* Round to odd ignores FPCR.RMode: truncate, then set the lowest bit
     * when inexact; overflow gives the largest normal. */
    rl_ctx *c = boot(RP);
    CHECK(c != NULL);
    c->v[1].d[0] = 0xBFF0000000000001ull;
    c->v[1].d[1] = 0x3FF0000000000000ull;
    c->v[0].q = ~(rl_u128)0;
    CHECK_EQ(run(c, 0x2E616820u), RL_OK); /* FCVTXN v0.2s, v1.2d */
    CHECK_EQ(c->v[0].s[0], 0xBF800001u);
    CHECK_EQ(c->v[0].s[1], 0x3F800000u);
    CHECK_EQ(c->v[0].d[1], 0);
    CHECK_EQ(c->fpsr, IXC);
    c->fpsr = 0;
    c->v[1].d[0] = 0x47EFFFFFF0000000ull;
    c->v[1].d[1] = 0x47F0000000000000ull;
    CHECK_EQ(run(c, 0x6E616820u), RL_OK); /* FCVTXN2 v0.4s, v1.2d */
    CHECK_EQ(c->v[0].s[0], 0xBF800001u);
    CHECK_EQ(c->v[0].s[2], 0x7F7FFFFFu);
    CHECK_EQ(c->v[0].s[3], 0x7F7FFFFFu);
    CHECK_EQ(c->fpsr, OFC | IXC);
    c->fpsr = 0;
    c->v[1].d[0] = 0x47EFFFFFF0000000ull;
    CHECK_EQ(run(c, 0x7E616820u), RL_OK); /* FCVTXN s0, d1 */
    CHECK_EQ(c->v[0].s[0], 0x7F7FFFFFu);
    CHECK_EQ(c->v[0].s[1], 0);
    CHECK_EQ(c->v[0].d[1], 0);
    CHECK_EQ(c->fpsr, IXC);
    c->fpsr = 0;
    c->v[1].d[0] = 1;
    CHECK_EQ(run(c, 0x7E616820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 1);
    CHECK_EQ(c->fpsr, UFC | IXC);
}

TEST(fp_slow_vector_one_source)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    c->v[1].s[0] = 0x40200000u;
    c->v[1].s[1] = 0x3F800000u;
    c->v[1].s[2] = 0x40800000u;
    c->v[1].s[3] = 0x41100000u;
    CHECK_EQ(run(c, 0x4EA19820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40000000u);
    CHECK_EQ(run(c, 0x6EA0F820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xC0200000u);
    CHECK_EQ(run(c, 0x4EA0F820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40200000u);
    CHECK_EQ(run(c, 0x6EA1F820u), RL_OK);
    CHECK_EQ(c->v[0].s[1], 0x3F800000u);
    CHECK_EQ(c->v[0].s[2], 0x40000000u);
    CHECK_EQ(c->v[0].s[3], 0x40400000u);
}

TEST(fp_slow_frint32_frint64_scalar)
{
    rl_ctx *c = boot(RP);
    CHECK(c != NULL);
    /* FRINT32X S0, S1 rounds per FPCR (toward plus infinity here). */
    c->v[1].s[0] = 0x3FC00000u;
    CHECK_EQ(run(c, 0x1E28C020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40000000u);
    CHECK_EQ(c->fpsr, IXC);
    /* Out of the 32-bit range: the most negative integer and Invalid only. */
    c->fpsr = 0;
    c->v[1].s[0] = 0x4F32D05Eu;
    CHECK_EQ(run(c, 0x1E28C020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xCF000000u);
    CHECK_EQ(c->fpsr, IOC);
    /* FRINT32Z: -2^31 itself fits; the next value down does not. */
    c->fpsr = 0;
    c->v[1].s[0] = 0xCF000000u;
    CHECK_EQ(run(c, 0x1E284020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xCF000000u);
    CHECK_EQ(c->fpsr, 0);
    c->v[1].s[0] = 0xCF000001u;
    CHECK_EQ(run(c, 0x1E284020u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0xCF000000u);
    CHECK_EQ(c->fpsr, IOC);
    /* FRINT64X D0, D1 of a quiet NaN. */
    c->fpsr = 0;
    c->v[1].d[0] = 0x7FF8000000000001ull;
    CHECK_EQ(run(c, 0x1E69C020u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0xC3E0000000000000ull);
    CHECK_EQ(c->v[0].d[1], 0);
    CHECK_EQ(c->fpsr, IOC);
}

TEST(fp_slow_frint32_frint64_vector)
{
    rl_ctx *c = boot(RZ | FZ);
    CHECK(c != NULL);
    /* FRINT32X V0.4S, V1.4S toward zero with FZ. */
    c->v[1].s[0] = 0x40200000u;
    c->v[1].s[1] = 0xBF000000u;
    c->v[1].s[2] = 0x501502F9u;
    c->v[1].s[3] = 0x00000001u;
    CHECK_EQ(run(c, 0x6E21E820u), RL_OK);
    CHECK_EQ(c->v[0].s[0], 0x40000000u);
    CHECK_EQ(c->v[0].s[1], 0x80000000u);
    CHECK_EQ(c->v[0].s[2], 0xCF000000u);
    CHECK_EQ(c->v[0].s[3], 0);
    CHECK_EQ(c->fpsr, IXC | IOC | IDC);
    /* FRINT64Z V0.2D, V1.2D rounds toward zero whatever the FPCR says. */
    c->fpcr = RP;
    c->fpsr = 0;
    c->v[1].d[0] = 0x43E0000000000000ull;
    c->v[1].d[1] = 0xC00599999999999Aull;
    CHECK_EQ(run(c, 0x4E61F820u), RL_OK);
    CHECK_EQ(c->v[0].d[0], 0xC3E0000000000000ull);
    CHECK_EQ(c->v[0].d[1], 0xC000000000000000ull);
    CHECK_EQ(c->fpsr, IOC | IXC);
    /* FRINT64Z with 2D lanes needs Q. */
    CHECK_EQ(run(c, 0x0E61F820u), RL_PENDING);
}

TEST(fp_slow_unknown_encoding_stops)
{
    rl_ctx *c = boot(RZ);
    CHECK(c != NULL);
    CHECK_EQ(run(c, 0x00000000u), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
}
