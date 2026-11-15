/**
 * @file test_arm32.c
 * @brief The ARMv4T CPU layer over the core with hand-written lifted
 * functions: context layout, the CPSR and the mode banks, exception entry
 * and return, the I bit's interrupt mask, the return address stack, the run
 * loop's redirects, code fetch costs and the crash report text. This binary
 * links the core and the ARM32 layer only.
 */
#include "arm32.h"
#include "runner.h"

#include <relift/sched.h>

static const rl_guest_desc test_guest = {
    .name = "arm32-test",
    .page_size = 0x1000,
    .address_bits = 32,
    .tick_hz = 16777216,
};

static struct {
    int leaf_runs;
    int tail_runs;
    uint32_t depth_in_leaf;
    int after_call;
} hs;

/** @brief A leaf: r0 += 1, then BX LR (a computed jump to LR). */
static int leaf(rl_ctx *c, uint8_t *mem)
{
    hs.leaf_runs++;
    hs.depth_in_leaf = c->depth;
    c->r[0] += 1;
    RL_MUSTTAIL return rl_indirect(c, c->r[14] & ~1u)(c, mem);
}

/** @brief Jumps on to the leaf (a tail call: B leaf). */
static int tail(rl_ctx *c, uint8_t *mem)
{
    hs.tail_runs++;
    RL_MUSTTAIL return rl_indirect(c, 0x08000100u)(c, mem);
}

/** @brief Calls the leaf twice (BL leaf; BL tail), then stops by jumping to 0x08000400. */
static int caller(rl_ctx *c, uint8_t *mem)
{
    c->r[14] = 0x08000204u;
    rl_arm32_call(c, 0x08000204u);
    int rc = rl_indirect(c, 0x08000100u)(c, mem);
    if (rc)
        return RL_PENDING;
    c->r[14] = 0x08000208u;
    rl_arm32_call(c, 0x08000208u);
    rc = rl_indirect(c, 0x08000180u)(c, mem);
    if (rc)
        return RL_PENDING;
    hs.after_call = 1;
    RL_MUSTTAIL return rl_indirect(c, 0x08000400u)(c, mem);
}

/** @brief Returns to an address no frame waits for: the leaf's LR was rewritten. */
static int liar(rl_ctx *c, uint8_t *mem)
{
    c->r[14] = 0x08000300u;
    RL_MUSTTAIL return rl_indirect(c, 0x08000300u)(c, mem);
}

/** @brief BL liar, expecting to come back to 0x0800020C. */
static int fooled(rl_ctx *c, uint8_t *mem)
{
    rl_arm32_call(c, 0x0800020Cu);
    int rc = rl_indirect(c, 0x08000280u)(c, mem);
    if (rc)
        return RL_PENDING;
    return rl_guest_fatal(c, "test", "came back to a frame the guest left");
}

/** @brief Where the liar went: stops the run with r1 = 0x5A. */
static int landing(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->r[1] = 0x5A;
    return rl_pend_exit_process(c, 0);
}

/** @brief The end of caller's run. */
static int stop(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return rl_pend_exit_process(c, 0);
}

/** @brief Leaves every frame and goes on at 0x08000300. */
static int bounce(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->depth = 5;
    return rl_arm32_redirect(c, 0x08000300u);
}

static const rl_fn_entry fns[] = {
    {0x08000100u, 4, 0, leaf},    {0x08000180u, 4, 0, tail},    {0x08000200u, 16, 0, caller},
    {0x08000280u, 4, 0, liar},    {0x08000300u, 4, 0, landing}, {0x08000400u, 4, 0, stop},
    {0x08000500u, 16, 0, fooled}, {0x08000600u, 4, 0, bounce},
};

static rl_ctx *boot(void)
{
    if (rl_core_init(&test_guest, rl_arm32_cpu()) != 0)
        return NULL;
    rl_units_reset();
    static rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(rl_ctx);
    u.fn_count = (uint32_t)(sizeof fns / sizeof fns[0]);
    u.fns = fns;
    u.name = "arm32-test";
    const rl_unit *list[] = {&u};
    if (rl_units_register(list, 1) != 0)
        return NULL;
    memset(&hs, 0, sizeof hs);
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return NULL;
    rl_thread_bind(t);
    return t->ctx;
}

TEST(arm32_descriptor_matches_the_context)
{
    const rl_cpu_desc *d = rl_arm32_cpu();
    CHECK_EQ(d->ctx_size, sizeof(rl_ctx));
    CHECK_EQ(d->common_offset, offsetof(rl_ctx, pend));
    CHECK_EQ(d->time_offset, offsetof(rl_ctx, time));
    CHECK_EQ(d->code_shift, 1);
    CHECK(!d->big_endian);
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ(c->m, RL_ARM32_SVC);
    CHECK_EQ(c->i, 1);
    CHECK_EQ(c->f, 1);
    CHECK_EQ(rl_arm32_cpsr(c), 0xD3);
}

TEST(arm32_mode_changes_swap_the_banks)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    rl_arm32_set_cpsr(c, RL_ARM32_SYS);
    for (unsigned n = 0; n < 15; n++)
        c->r[n] = 0x100 + n;
    rl_arm32_set_mode(c, RL_ARM32_IRQ);
    CHECK_EQ(c->r[12], 0x10C);
    CHECK_EQ(c->bank_usr[5], 0x10D);
    CHECK_EQ(c->bank_usr[6], 0x10E);
    c->r[13] = 0x03007FA0u;
    rl_arm32_set_mode(c, RL_ARM32_FIQ);
    CHECK_EQ(c->bank_irq[0], 0x03007FA0u);
    CHECK_EQ(c->bank_usr[0], 0x108);
    c->r[8] = 0xF8;
    rl_arm32_set_mode(c, RL_ARM32_USR);
    CHECK_EQ(c->r[8], 0x108);
    CHECK_EQ(c->r[13], 0x10D);
    CHECK_EQ(c->bank_fiq[0], 0xF8);
    CHECK_EQ(*rl_arm32_banked(c, RL_ARM32_IRQ, 13), 0x03007FA0u);
    CHECK_EQ(*rl_arm32_banked(c, RL_ARM32_SYS, 13), 0x10D);
    CHECK(rl_arm32_banked(c, RL_ARM32_SYS, 13) == &c->r[13]);
    CHECK_EQ(*rl_arm32_banked(c, RL_ARM32_FIQ, 8), 0xF8);
}

TEST(arm32_exception_entry_and_return_restore_the_caller)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    rl_arm32_set_cpsr(c, 0x6000003Fu);
    c->r[13] = 0x03007F00u;
    c->r[14] = 0x08000111u;
    c->bank_svc[0] = 0x03007FE0u;
    rl_arm32_exception(c, RL_ARM32_SVC, 0x08000124u);
    CHECK_EQ(c->m, RL_ARM32_SVC);
    CHECK_EQ(c->t, 0);
    CHECK_EQ(c->i, 1);
    CHECK_EQ(c->r[13], 0x03007FE0u);
    CHECK_EQ(c->r[14], 0x08000124u);
    CHECK_EQ(c->spsr[2], 0x6000003Fu);
    CHECK_EQ(c->bank_usr[6], 0x08000111u);
    rl_arm32_restore(c);
    CHECK_EQ(rl_arm32_cpsr(c), 0x6000003Fu);
    CHECK_EQ(c->r[13], 0x03007F00u);
    CHECK_EQ(c->r[14], 0x08000111u);
    CHECK_EQ(c->bank_svc[1], 0x08000124u);
}

TEST(arm32_the_i_bit_masks_the_irq_lines)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    CHECK(rl_sched_attach(k, rl_common(c)->thread) == 0);
    rl_arm32_set_lines(c, 1u, 4u);
    rl_sched_run(k);
    rl_sched_raise(s, 0);
    CHECK_EQ(rl_sched_run(k), 0);
    rl_arm32_set_i(c, 0);
    CHECK(c->time.budget <= 0);
    CHECK(rl_sched_run(k) != 0);
    rl_arm32_set_cpsr(c, 0x9F);
    CHECK_EQ(rl_sched_run(k), 0);
    rl_sched_raise(s, 2);
    CHECK(rl_sched_run(k) != 0);
    rl_sched_destroy(s);
}

TEST(arm32_returns_close_host_frames_and_tail_calls_keep_them)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    int r = rl_arm32_run(c, NULL, 0x08000200u);
    CHECK_EQ(r, RL_PENDING);
    CHECK_EQ(rl_common(c)->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(hs.leaf_runs, 2);
    CHECK_EQ(hs.tail_runs, 1);
    CHECK_EQ(hs.depth_in_leaf, 1);
    CHECK_EQ(hs.after_call, 1);
    CHECK_EQ(c->r[0], 2);
    CHECK_EQ(c->depth, 0);
}

TEST(arm32_a_return_elsewhere_goes_on_through_dispatch)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    int r = rl_arm32_run(c, NULL, 0x08000500u);
    CHECK_EQ(r, RL_PENDING);
    CHECK_EQ(rl_common(c)->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->r[1], 0x5A);
    CHECK_EQ(c->depth, 1);
}

TEST(arm32_a_redirect_restarts_with_no_open_frame)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint64_t before = rl_arm32_get_stats()->redirects;
    CHECK_EQ(rl_arm32_run(c, NULL, 0x08000600u), RL_PENDING);
    CHECK_EQ(rl_common(c)->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(rl_arm32_get_stats()->redirects, before + 1);
    CHECK_EQ(c->depth, 0);
    CHECK_EQ(c->r[1], 0x5A);
}

TEST(arm32_a_nested_guest_call_returns_at_its_address)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->r[14] = 0x00000138u;
    CHECK_EQ(rl_arm32_call_guest(c, NULL, 0x08000100u, 0x00000138u), RL_OK);
    CHECK_EQ(c->r[0], 1);
    CHECK_EQ(c->depth, 0);
}

TEST(arm32_fetches_cost_the_region_wait_states)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    static uint8_t waits[16 * 6];
    for (unsigned i = 0; i < sizeof waits; i++)
        waits[i] = 1;
    waits[8 * 6 + 1 * 2 + 0] = 5;
    waits[8 * 6 + 1 * 2 + 1] = 3;
    waits[8 * 6 + 2 * 2 + 0] = 8;
    waits[8 * 6 + 2 * 2 + 1] = 6;
    c->waits = waits;
    c->time.budget = 100;
    rl_arm32_fetch(c, 0x08000000u, 1);
    CHECK_EQ(c->time.budget, 97);
    rl_arm32_fetch(c, 0x08000000u, 0);
    CHECK_EQ(c->time.budget, 91);
    rl_arm32_fetch(c, 0x03000000u, 0);
    CHECK_EQ(c->time.budget, 90);
    rl_arm32_refill(c, 0x08000010u, 1, 1);
    CHECK_EQ(c->time.budget, 82);
    rl_arm32_refill(c, 0x08000010u, 1, 0);
    CHECK_EQ(c->time.budget, 82);
    c->fast_fetch = 1u << 8;
    rl_arm32_refill(c, 0x08000010u, 1, 1);
    CHECK_EQ(c->time.budget, 76);
    CHECK_EQ(rl_arm32_cond_fail(c, NULL, 0x08000020u, 0), RL_OK);
    CHECK_EQ(c->time.budget, 75);
}

TEST(arm32_data_accesses_are_sequential_only_within_an_instruction)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ(rl_arm32_data_seq(c, 0x03000000u, 4), 0);
    CHECK_EQ(rl_arm32_data_seq(c, 0x03000004u, 4), 1);
    CHECK_EQ(rl_arm32_data_seq(c, 0x03000010u, 4), 0);
    rl_arm32_fetch(c, 0x08000000u, 0);
    CHECK_EQ(rl_arm32_data_seq(c, 0x03000014u, 4), 0);
}

TEST(arm32_crash_reports_name_the_mode_and_registers)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    rl_arm32_set_cpsr(c, 0x2000003Fu);
    c->r[3] = 0xCAFEF00Du;
    char buf[512];
    rl_arm32_cpu()->describe(c, buf, sizeof buf);
    CHECK(strstr(buf, "cpsr 2000003f (--C- --T sys)") != NULL);
    CHECK(strstr(buf, "r3 cafef00d") != NULL);
}
