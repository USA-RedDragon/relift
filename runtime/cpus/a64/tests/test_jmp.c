/**
 * @file test_jmp.c
 * @brief Guest setjmp and longjmp through hand-written functions that follow
 * the lifted-code ABI: registers in locals, the call boundary rule, a
 * pending check after every call, and a resume-point dispatch at the
 * setjmp call site (docs/abi.md sections 4, 7 and 7.1).
 */
#include "a64.h"
#include "runner.h"

static const rl_guest_desc jmp_guest = {
    .name = "a64-jmp-test",
    .page_size = 0x4000,
    .address_bits = 40,
    .tick_hz = 1000000,
};

static _Alignas(16) uint8_t g_mem[0x10000];

enum {
    JB_OUTER = 0x8000,
    JB_MIDDLE = 0x8400,
    OUTER = 0x1000,
    OUTER_AFTER_SETJMP = 0x1010,
    OUTER_AFTER_MIDDLE = 0x1018,
    MIDDLE = 0x2000,
    MIDDLE_AFTER_SETJMP = 0x2010,
    MIDDLE_AFTER_INNER = 0x2018,
    INNER = 0x3000,
    INNER_AFTER_LONGJMP = 0x3008,
    STACK_TOP = 0xF000,
    D8_VALUE = 0x4020000000000000ull,
};

static uint32_t g_val;
static int g_middle_setjmp;
static int g_middle_checks;
static int g_middle_ran_after;
static int g_inner_ran_after;
static int g_inner_exits;

static rl_ctx *boot(void)
{
    if (rl_core_init(&jmp_guest, rl_a64_cpu()) != 0)
        return NULL;
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return NULL;
    rl_thread_bind(t);
    memset(g_mem, 0, sizeof g_mem);
    g_val = 42;
    g_middle_setjmp = 0;
    g_middle_checks = 1;
    g_middle_ran_after = 0;
    g_inner_ran_after = 0;
    g_inner_exits = 0;
    t->ctx->sp = STACK_TOP;
    return t->ctx;
}

static RL_LIFTED(g_inner)
{
    uint64_t sp = c->sp - 16;
    uint64_t x30 = c->x[30];
    rl_st64(mem, sp + 8, x30);
    if (g_inner_exits)
        return rl_pend_exit_process(c, 3);
    c->x[21] = 0x2121;
    c->v[8].d[0] = 0;
    c->x[0] = JB_OUTER;
    c->x[1] = g_val;
    c->sp = sp;
    c->x[30] = INNER_AFTER_LONGJMP;
    int rc = rl_a64_longjmp(c, mem);
    if (RL_UNLIKELY(rc))
        return RL_PENDING;
    g_inner_ran_after++;
    c->x[30] = rl_ld64(mem, sp + 8);
    c->sp = sp + 16;
    return RL_OK;
}

static RL_LIFTED(g_middle)
{
    uint64_t sp = c->sp - 32;
    uint64_t x0;
    rl_st64(mem, sp + 8, c->x[30]);
    if (g_middle_setjmp) {
        c->x[0] = JB_MIDDLE;
        c->sp = sp;
        c->x[30] = MIDDLE_AFTER_SETJMP;
        int rc = rl_a64_setjmp(c, mem);
        if (RL_UNLIKELY(rc)) {
            if (c->pend.sp == sp)
                goto lp_dispatch;
            return RL_PENDING;
        }
        x0 = c->x[0];
    at_2010:
        if (x0 != 0)
            goto out;
    }
    c->x[20] = 0x2020;
    c->sp = sp;
    c->x[30] = MIDDLE_AFTER_INNER;
    int rc = g_inner(c, mem);
    if (g_middle_checks && RL_UNLIKELY(rc)) {
        if (g_middle_setjmp && c->pend.sp == sp)
            goto lp_dispatch;
        return RL_PENDING;
    }
    g_middle_ran_after++;
out:
    c->x[30] = rl_ld64(mem, sp + 8);
    c->sp = sp + 32;
    return RL_OK;
lp_dispatch:
    x0 = c->x[0];
    sp = c->sp;
    switch (c->pend.pc) {
    case MIDDLE_AFTER_SETJMP:
        c->pend.kind = RL_PEND_NONE;
        goto at_2010;
    default:
        rl_bad_resume(c, mem, MIDDLE);
    }
}

static RL_LIFTED(g_outer)
{
    uint64_t sp = c->sp - 48;
    uint64_t x0, x19 = 0x1919;
    rl_st64(mem, sp + 8, c->x[30]);
    c->x[19] = x19;
    c->v[8].d[0] = D8_VALUE;
    c->x[0] = JB_OUTER;
    c->sp = sp;
    c->x[30] = OUTER_AFTER_SETJMP;
    int rc = rl_a64_setjmp(c, mem);
    if (RL_UNLIKELY(rc)) {
        if (c->pend.sp == sp)
            goto lp_dispatch;
        return RL_PENDING;
    }
    x0 = c->x[0];
at_1010:
    if (x0 != 0)
        goto out;
    x19 = 0xBAD;
    c->x[19] = x19;
    c->sp = sp;
    c->x[30] = OUTER_AFTER_MIDDLE;
    rc = g_middle(c, mem);
    if (RL_UNLIKELY(rc)) {
        if (c->pend.sp == sp)
            goto lp_dispatch;
        return RL_PENDING;
    }
    x0 = 0xFFFF;
out:
    c->x[2] = x0;
    c->x[19] = x19;
    c->x[30] = rl_ld64(mem, sp + 8);
    c->sp = sp + 48;
    return RL_OK;
lp_dispatch:
    x0 = c->x[0];
    x19 = c->x[19];
    sp = c->sp;
    switch (c->pend.pc) {
    case OUTER_AFTER_SETJMP:
        c->pend.kind = RL_PEND_NONE;
        goto at_1010;
    default:
        rl_bad_resume(c, mem, OUTER);
    }
}

static RL_LIFTED(g_setjmp_and_return)
{
    c->x[0] = JB_OUTER;
    c->x[30] = OUTER_AFTER_SETJMP;
    return rl_a64_setjmp(c, mem);
}

TEST(jmp_longjmp_returns_to_the_setjmp_frame)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->x[30] = 0x9990;
    CHECK_EQ(g_outer(c, g_mem), RL_OK);
    CHECK_EQ(c->x[2], 42);
    CHECK_EQ(c->x[19], 0x1919);
    CHECK_EQ(c->v[8].d[0], D8_VALUE);
    CHECK_EQ(c->x[30], 0x9990);
    CHECK_EQ(c->sp, STACK_TOP);
    CHECK_EQ(c->pend.kind, RL_PEND_NONE);
    CHECK_EQ(g_middle_ran_after, 0);
    CHECK_EQ(g_inner_ran_after, 0);
}

TEST(jmp_buffer_holds_the_saved_registers)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    for (int i = 19; i < 31; i++)
        c->x[i] = 0x100u + (unsigned)i;
    for (int i = 8; i < 16; i++)
        c->v[i].d[0] = 0xD00u + (unsigned)i;
    c->sp = 0x7770;
    c->x[0] = JB_OUTER;
    CHECK_EQ(rl_a64_setjmp(c, g_mem), RL_OK);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(rl_ld64(g_mem, JB_OUTER + 0), 0x113);
    CHECK_EQ(rl_ld64(g_mem, JB_OUTER + 88), 0x11E);
    CHECK_EQ(rl_ld64(g_mem, JB_OUTER + 96), 0x7770);
    CHECK_EQ(rl_ld64(g_mem, JB_OUTER + 112), 0xD08);
    CHECK_EQ(rl_ld64(g_mem, JB_OUTER + 168), 0xD0F);
    CHECK_EQ(rl_ld64(g_mem, JB_OUTER + 104), 0);
}

TEST(jmp_zero_value_becomes_one)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    g_val = 0;
    CHECK_EQ(g_outer(c, g_mem), RL_OK);
    CHECK_EQ(c->x[2], 1);
    g_val = 0x100000000u - 1u;
    CHECK_EQ(g_outer(c, g_mem), RL_OK);
    CHECK_EQ(c->x[2], 0xFFFFFFFFu);
}

TEST(jmp_passes_a_frame_with_its_own_resume_point)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    g_middle_setjmp = 1;
    CHECK_EQ(g_outer(c, g_mem), RL_OK);
    CHECK_EQ(c->x[2], 42);
    CHECK_EQ(c->x[19], 0x1919);
    CHECK_EQ(g_middle_ran_after, 0);
    CHECK(rl_ld64(g_mem, JB_MIDDLE + 96) != rl_ld64(g_mem, JB_OUTER + 96));
}

TEST(jmp_to_a_returned_frame_reaches_the_thread_entry)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ(g_setjmp_and_return(c, g_mem), RL_OK);
    uint64_t saved_sp = rl_ld64(g_mem, JB_OUTER + 96);
    c->sp = STACK_TOP - 0x200;
    CHECK_EQ(g_middle(c, g_mem), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_UNWIND);
    CHECK_EQ(c->pend.sp, saved_sp);
    CHECK_EQ(c->pend.pc, OUTER_AFTER_SETJMP);
    CHECK_EQ(g_middle_ran_after, 0);
}

TEST(jmp_exit_passes_every_resume_point)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    g_middle_setjmp = 1;
    g_inner_exits = 1;
    CHECK_EQ(g_outer(c, g_mem), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->pend.code, 3);
    CHECK_EQ(c->pend.sp, RL_SP_NONE);
}

TEST(jmp_layout_is_the_guest_libcs)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    static const rl_a64_jmp_layout other = { .x19 = 16, .sp = 8, .d8 = 120, .size = 184 };
    rl_a64_set_jmp_layout(&other);
    CHECK(rl_a64_jmp_layout_current() == &other);
    c->x[19] = 0xAA;
    c->x[30] = 0xBB;
    c->sp = 0x6660;
    c->v[15].d[0] = 0xCC;
    c->x[0] = JB_OUTER;
    CHECK_EQ(rl_a64_setjmp(c, g_mem), RL_OK);
    CHECK_EQ(rl_ld64(g_mem, JB_OUTER + 16), 0xAA);
    CHECK_EQ(rl_ld64(g_mem, JB_OUTER + 104), 0xBB);
    CHECK_EQ(rl_ld64(g_mem, JB_OUTER + 8), 0x6660);
    CHECK_EQ(rl_ld64(g_mem, JB_OUTER + 176), 0xCC);
    c->x[0] = JB_OUTER;
    c->x[1] = 7;
    c->x[19] = 0;
    CHECK_EQ(rl_a64_longjmp(c, g_mem), RL_PENDING);
    CHECK_EQ(c->x[0], 7);
    CHECK_EQ(c->x[19], 0xAA);
    CHECK_EQ(c->sp, 0x6660);
    CHECK_EQ(c->pend.pc, 0xBB);
    CHECK_EQ(c->pend.sp, 0x6660);
    rl_a64_set_jmp_layout(NULL);
    CHECK(rl_a64_jmp_layout_current() == &rl_a64_jmp_newlib);
    CHECK_EQ(rl_a64_jmp_newlib.size, 176);
}

TEST(jmp_control_a_dropped_check_runs_code_longjmp_skipped)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    g_middle_checks = 0;
    CHECK_EQ(g_outer(c, g_mem), RL_OK);
    CHECK_EQ(g_middle_ran_after, 1);
    CHECK(c->x[2] != 42);
}
