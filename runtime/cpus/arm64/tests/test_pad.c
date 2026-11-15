/**
 * @file test_pad.c
 * @brief A computed jump to a landing pad that no function enters (an
 * unwinder's install step, in a shape that is not any one library's)
 * reaches the table's rl_pad_resume entry, and the host stack unwinds to
 * the frame whose landing-pad dispatch owns the pad (docs/abi.md section 7).
 */
#include "arm64.h"
#include "runner.h"

static const rl_guest_desc pad_guest = {
    .name = "arm64-pad-test",
    .page_size = 0x4000,
    .address_bits = 40,
    .tick_hz = 1000000,
};

static _Alignas(16) uint8_t g_mem[0x10000];

enum {
    OUTER = 0x1000,
    OUTER_PAD = 0x1010,
    MIDDLE = 0x1100,
    MIDDLE_PAD = 0x1110,
    INSTALL = 0x2000,
    STACK_TOP = 0xF000,
};

static uint64_t g_target_sp;
static uint64_t g_target_pad;
static int g_middle_ran_after;
static int g_outer_ran_after;
static int g_outer_caught;
static uint64_t g_outer_x0;

static RL_LIFTED(g_install)
{
    c->x[0] = 0x77;
    c->sp = g_target_sp;
    return rl_indirect(c, g_target_pad)(c, mem);
}

static RL_LIFTED(g_middle)
{
    uint64_t sp = c->sp - 32;
    c->sp = sp;
    c->x[30] = 0x1108;
    int rc = g_install(c, mem);
    if (RL_UNLIKELY(rc)) {
        if (c->pend.sp == sp)
            goto lp_dispatch;
        return RL_PENDING;
    }
    g_middle_ran_after++;
    c->sp = sp + 32;
    return RL_OK;
lp_dispatch:
    sp = c->sp;
    switch (c->pend.pc) {
    case MIDDLE_PAD:
        c->pend.kind = RL_PEND_NONE;
        g_middle_ran_after += 100;
        c->sp = sp + 32;
        return RL_OK;
    default:
        rl_bad_resume(c, mem, MIDDLE);
    }
}

static RL_LIFTED(g_outer)
{
    uint64_t sp = c->sp - 48;
    c->sp = sp;
    c->x[30] = 0x1008;
    int rc = g_middle(c, mem);
    if (RL_UNLIKELY(rc)) {
        if (c->pend.sp == sp)
            goto lp_dispatch;
        return RL_PENDING;
    }
    g_outer_ran_after++;
    c->sp = sp + 48;
    return RL_OK;
lp_dispatch:
    sp = c->sp;
    switch (c->pend.pc) {
    case OUTER_PAD:
        c->pend.kind = RL_PEND_NONE;
        g_outer_caught++;
        g_outer_x0 = c->x[0];
        c->sp = sp + 48;
        return RL_OK;
    default:
        rl_bad_resume(c, mem, OUTER);
    }
}

static rl_ctx *boot(uint64_t pad, uint64_t sp)
{
    static const rl_fn_entry fns[] = {
        { OUTER, 0x20, RL_FN_MAY_UNWIND | RL_FN_LANDING_PADS, g_outer },
        { OUTER_PAD, 0, RL_FN_MAY_UNWIND | RL_FN_FRAGMENT, rl_pad_resume },
        { MIDDLE, 0x20, RL_FN_MAY_UNWIND | RL_FN_LANDING_PADS, g_middle },
        { MIDDLE_PAD, 0, RL_FN_MAY_UNWIND | RL_FN_FRAGMENT, rl_pad_resume },
        { INSTALL, 0x20, RL_FN_MAY_UNWIND, g_install },
    };
    rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(rl_ctx);
    u.fn_count = sizeof fns / sizeof fns[0];
    u.fns = fns;
    u.name = "pad-test";
    const rl_unit *list[] = { &u };
    if (rl_core_init(&pad_guest, rl_arm64_cpu()) != 0 || rl_units_register(list, 1) != 0)
        return NULL;
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return NULL;
    rl_thread_bind(t);
    memset(g_mem, 0, sizeof g_mem);
    g_target_pad = pad;
    g_target_sp = sp;
    g_middle_ran_after = 0;
    g_outer_ran_after = 0;
    g_outer_caught = 0;
    g_outer_x0 = 0;
    t->ctx->sp = STACK_TOP;
    return t->ctx;
}

TEST(pad_jump_resumes_the_frame_that_owns_the_pad)
{
    rl_ctx *c = boot(OUTER_PAD, STACK_TOP - 48);
    CHECK(c != NULL);
    CHECK_EQ(g_outer(c, g_mem), RL_OK);
    CHECK_EQ(g_outer_caught, 1);
    CHECK_EQ(g_outer_x0, 0x77);
    CHECK_EQ(g_middle_ran_after, 0);
    CHECK_EQ(g_outer_ran_after, 0);
    CHECK_EQ(c->sp, STACK_TOP);
    CHECK_EQ(c->pend.kind, RL_PEND_NONE);
}

TEST(pad_jump_stops_at_the_nearest_frame_with_that_sp)
{
    rl_ctx *c = boot(MIDDLE_PAD, STACK_TOP - 48 - 32);
    CHECK(c != NULL);
    CHECK_EQ(g_outer(c, g_mem), RL_OK);
    CHECK_EQ(g_middle_ran_after, 100);
    CHECK_EQ(g_outer_caught, 0);
    CHECK_EQ(g_outer_ran_after, 1);
}

TEST(pad_jump_with_no_frame_reaches_the_thread_entry)
{
    rl_ctx *c = boot(OUTER_PAD, 0x1230);
    CHECK(c != NULL);
    CHECK_EQ(g_outer(c, g_mem), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_UNWIND);
    CHECK_EQ(c->pend.pc, OUTER_PAD);
    CHECK_EQ(c->pend.sp, 0x1230);
    CHECK_EQ(g_outer_caught, 0);
}
