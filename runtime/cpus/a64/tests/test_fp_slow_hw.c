/**
 * @file test_fp_slow_hw.c
 * @brief rl_fp_slow on the forms the FPCR slow paths gained (estimates and
 * steps, pairwise and across-lanes, by element, compares with zero, FCVTN
 * and FCVTL, vector fixed point) and FZ compares. Every expected value,
 * NZCV and FPSR was read from an Apple M2 running the same instruction on
 * the same state (check/oracle/harness.c).
 */
#include "a64.h"
#include "runner.h"

#include <relift/a64/fp_slow.h>

static const rl_guest_desc test_guest = {
    .name = "a64-fp-slow-hw-test",
    .page_size = 0x4000,
    .address_bits = 40,
    .tick_hz = 1000000,
};

static rl_ctx *boot(void)
{
    static rl_ctx *c;
    if (c)
        return c;
    if (rl_core_init(&test_guest, rl_a64_cpu()) != 0)
        return NULL;
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return NULL;
    rl_thread_bind(t);
    c = t->ctx;
    return c;
}

#define Q(hi, lo) (((rl_u128)(uint64_t)(hi) << 64) | (uint64_t)(lo))

/* Runs insn with v0, v1, v2 and fpcr from zero FPSR and NZCV. */
static rl_ctx *run(uint32_t insn, uint32_t fpcr, rl_u128 v0, rl_u128 v1, rl_u128 v2)
{
    rl_ctx *c = boot();
    if (!c)
        return NULL;
    memset(c->v, 0, sizeof c->v);
    c->v[0].q = v0;
    c->v[1].q = v1;
    c->v[2].q = v2;
    c->fpcr = fpcr;
    c->fpsr = 0;
    c->nzcv = 0;
    if (rl_fp_slow(c, rl_mem_base(), 0x10000, insn) != RL_OK)
        return NULL;
    return c;
}

TEST(fp_slow_hw_frecpe_scalar)
{
    rl_ctx *c = run(0x5ea1d820u, 0x0u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x0000000000000000u, 0x0000000040400000u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x0000000000000000u, 0x000000003eaa8000u));
    CHECK_EQ(c->fpsr, 0x0u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_frecpe_denormal_overflows)
{
    rl_ctx *c = run(0x5ea1d820u, 0x0u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x0000000000000000u, 0x0000000000000001u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x0000000000000000u, 0x000000007f800000u));
    CHECK_EQ(c->fpsr, 0x14u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_frecpe_denormal_rz_gives_max)
{
    rl_ctx *c = run(0x5ea1d820u, 0xc00000u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x0000000000000000u, 0x0000000080000001u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x0000000000000000u, 0x00000000ff7fffffu));
    CHECK_EQ(c->fpsr, 0x14u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_frecpe_fz_flushes_input)
{
    rl_ctx *c = run(0x5ea1d820u, 0x1000000u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x0000000000000000u, 0x0000000000000001u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x0000000000000000u, 0x000000007f800000u));
    CHECK_EQ(c->fpsr, 0x82u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_frecpe_tiny_result_fz)
{
    rl_ctx *c = run(0x5ea1d820u, 0x1000000u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x0000000000000000u, 0x000000007e800000u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK_EQ(c->fpsr, 0x8u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_frecps_vector)
{
    rl_ctx *c = run(0x4e22fc20u, 0x2000000u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x3f80000000000000u, 0x7f80000040000000u), Q(0x7fa000007f800000u, 0x000000003f000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x7fc0000040000000u, 0x400000003f800000u));
    CHECK_EQ(c->fpsr, 0x1u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_frsqrts_vector)
{
    rl_ctx *c = run(0x4ea2fc20u, 0x1000000u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x3f80000000800000u, 0x7f80000040000000u), Q(0x3f80000100800000u, 0x800000003f000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x3f7fffff3fc00000u, 0x3fc000003f800000u));
    CHECK_EQ(c->fpsr, 0x10u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_fmaxnmv)
{
    rl_ctx *c = run(0x6e30c820u, 0x0u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x40000000bf800000u, 0x3f8000007fc00000u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x0000000000000000u, 0x0000000040000000u));
    CHECK_EQ(c->fpsr, 0x0u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_fminv_nan)
{
    rl_ctx *c = run(0x6eb0f820u, 0x0u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x40000000bf800000u, 0x3f8000007f800001u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x0000000000000000u, 0x000000007fc00001u));
    CHECK_EQ(c->fpsr, 0x1u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_faddp_scalar_rp)
{
    rl_ctx *c = run(0x7e30d820u, 0x400000u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x0000000000000000u, 0x338000013f800000u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x0000000000000000u, 0x000000003f800001u));
    CHECK_EQ(c->fpsr, 0x10u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_fmla_by_element)
{
    rl_ctx *c = run(0x4fa21020u, 0x800000u, Q(0xff80000000800000u, 0x400000003f800000u), Q(0x3f80000080800000u, 0x3f8000003f800000u), Q(0x0000000000000000u, 0x3f80000100000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0xff80000080000001u, 0x4040000040000000u));
    CHECK_EQ(c->fpsr, 0x10u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_fcmlt_zero_fz)
{
    rl_ctx *c = run(0x4ea0e820u, 0x1000000u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0xbf8000007fc00000u, 0x3f80000080000001u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0xffffffff00000000u, 0x0000000000000000u));
    CHECK_EQ(c->fpsr, 0x81u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_fcvtn_rz)
{
    rl_ctx *c = run(0x0e216820u, 0xc00000u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x7f80000133000001u, 0x477ff0003f800001u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x0000000000000000u, 0x7e0000007bff3c00u));
    CHECK_EQ(c->fpsr, 0x19u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_fcvtl_ahp)
{
    rl_ctx *c = run(0x0e217820u, 0x4000000u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x0000000000000000u, 0xfc0000017e017c00u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0xc780000033800000u, 0x47c0200047800000u));
    CHECK_EQ(c->fpsr, 0x0u);
    CHECK_EQ(c->nzcv, 0x0u);
}

TEST(fp_slow_hw_fcmp_zero_fz_denormal)
{
    rl_ctx *c = run(0x1e202028u, 0x1000000u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x0000000000000000u, 0x0000000000000001u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK_EQ(c->fpsr, 0x80u);
    CHECK_EQ(c->nzcv, 0x60000000u);
}

TEST(fp_slow_hw_scvtf_fixed_vector)
{
    rl_ctx *c = run(0x4f3de420u, 0x800000u, Q(0x0000000000000000u, 0x0000000000000000u), Q(0x000000037fffffffu, 0xffffffff00000001u), Q(0x0000000000000000u, 0x0000000000000000u));
    CHECK(c != NULL);
    CHECK(c->v[0].q == Q(0x3ec000004d7fffffu, 0xbe0000003e000000u));
    CHECK_EQ(c->fpsr, 0x10u);
    CHECK_EQ(c->nzcv, 0x0u);
}
