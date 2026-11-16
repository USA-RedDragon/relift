/**
 * @file test_z80.c
 * @brief The Z80 CPU layer over the core and the device scheduler with a
 * synthetic machine (an INT level, an NMI edge latch, ports, a timer) that is
 * no real computer: context layout, the interrupt entry through the entry
 * blocks, the redirect path, the EI delay, HALT, ports and crash report
 * text. This binary links the core and the Z80 layer only.
 */
#include "runner.h"
#include "z80.h"

#include <stdlib.h>
#include <unistd.h>

static const rl_guest_desc test_guest = {
    .name = "z80-test",
    .page_size = 0x1000,
    .address_bits = 18,
    .tick_hz = 3579545,
};

/** @brief The synthetic machine: the INT level, the NMI latch, a port file and what the CPU acknowledged. */
static struct {
    uint8_t level;
    uint8_t nmi;
    uint32_t acks[8];
    int ack_count;
    rl_sched *sched;
    uint8_t ports[0x10000];
    uint16_t last_in;
    uint16_t last_out;
    uint8_t last_out_value;
    int outs;
    int release_on_ack;
} mc;

static uint8_t mc_pending(void *o)
{
    (void)o;
    return (uint8_t)((mc.level ? RL_Z80_PEND_INT : 0u) | (mc.nmi ? RL_Z80_PEND_NMI : 0u));
}

static void mc_ack(void *o, uint32_t bit)
{
    (void)o;
    mc.acks[mc.ack_count++ & 7] = bit;
    if (bit == RL_Z80_PEND_NMI) {
        mc.nmi = 0;
        rl_sched_lower(mc.sched, RL_Z80_LINE_NMI);
    } else if (mc.release_on_ack) {
        mc.level = 0;
        rl_sched_lower(mc.sched, RL_Z80_LINE_INT);
    }
}

static uint8_t mc_in(void *o, uint16_t port)
{
    (void)o;
    mc.last_in = port;
    return mc.ports[port];
}

static void mc_out(void *o, uint16_t port, uint8_t v)
{
    (void)o;
    mc.last_out = port;
    mc.last_out_value = v;
    mc.outs++;
}

static const rl_z80_ints ints = {.pending = mc_pending, .ack = mc_ack, .in = mc_in, .out = mc_out};
static const rl_z80_ints bare = {.pending = mc_pending};

static uint8_t ram[0x10000];

static void set_iff1(rl_ctx *c, uint8_t v)
{
    c->iff1 = v;
    rl_z80_iff(c, v);
}

static void raise_int(void)
{
    mc.level = 1;
    rl_sched_raise(mc.sched, RL_Z80_LINE_INT);
}

static void lower_int(void)
{
    mc.level = 0;
    rl_sched_lower(mc.sched, RL_Z80_LINE_INT);
}

static void raise_nmi(void)
{
    mc.nmi = 1;
    rl_sched_raise(mc.sched, RL_Z80_LINE_NMI);
}

/** @brief What the entry blocks saw. */
static struct {
    int runs;
    uint32_t kind;
    uint16_t ret;
    uint16_t sp_in;
    int64_t budget_in;
    uint8_t iff1_in;
    uint16_t rewrite_to;
    int landed;
    int reenable;
} hs;

/** @brief An entry block as lifted code behaves: pushes ret, clears IFF1, charges and "runs" the handler, whose RET pops. */
static int entry(rl_ctx *c, uint8_t *mem, uint32_t kind)
{
    hs.kind = kind;
    hs.ret = c->ret;
    hs.sp_in = c->sp;
    hs.budget_in = c->time.budget;
    hs.iff1_in = c->iff1;
    hs.runs++;
    c->sp = (uint16_t)(c->sp - 2u);
    mem[c->sp] = (uint8_t)c->ret;
    mem[(uint16_t)(c->sp + 1u)] = (uint8_t)(c->ret >> 8);
    c->iff1 = 0;
    c->time.budget -= 13;
    if (hs.rewrite_to) {
        mem[c->sp] = (uint8_t)hs.rewrite_to;
        mem[(uint16_t)(c->sp + 1u)] = (uint8_t)(hs.rewrite_to >> 8);
    }
    c->sp = (uint16_t)(c->sp + 2u);
    if (hs.reenable)
        set_iff1(c, 1);
    return RL_OK;
}

static int entry_nmi(rl_ctx *c, uint8_t *mem) { return entry(c, mem, RL_Z80_ENTRY_NMI); }
static int entry_im0(rl_ctx *c, uint8_t *mem) { return entry(c, mem, RL_Z80_ENTRY_IM0); }
static int entry_im1(rl_ctx *c, uint8_t *mem) { return entry(c, mem, RL_Z80_ENTRY_IM1); }
static int entry_im2(rl_ctx *c, uint8_t *mem) { return entry(c, mem, RL_Z80_ENTRY_IM2); }

static int task(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    hs.landed++;
    c->b = 77;
    return RL_OK;
}

static const rl_fn_entry fns[] = {
    {0x3000, 4, 0, task},
    {RL_Z80_ENTRY + RL_Z80_ENTRY_NMI, 4, 0, entry_nmi},
    {RL_Z80_ENTRY + RL_Z80_ENTRY_IM0, 4, 0, entry_im0},
    {RL_Z80_ENTRY + RL_Z80_ENTRY_IM1, 4, 0, entry_im1},
    {RL_Z80_ENTRY + RL_Z80_ENTRY_IM2, 4, 0, entry_im2},
};

static int boot_with(const rl_z80_ints *machine, struct rl_thread **out, rl_sched **sout, rl_sched_cpu **kout)
{
    if (rl_core_init(&test_guest, rl_z80_cpu()) != 0)
        return -1;
    rl_units_reset();
    static rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(rl_ctx);
    u.fn_count = (uint32_t)(sizeof fns / sizeof fns[0]);
    u.fns = fns;
    u.name = "z80-test";
    const rl_unit *list[] = {&u};
    if (rl_units_register(list, 1) != 0)
        return -1;
    memset(&mc, 0, sizeof mc);
    memset(&hs, 0, sizeof hs);
    memset(ram, 0, sizeof ram);
    struct rl_thread *t = rl_thread_alloc();
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    if (!t || !s || !k || rl_z80_attach(k, t, machine) != 0)
        return -1;
    mc.sched = s;
    t->ctx->sp = 0xFFFE;
    rl_sched_run(k);
    *out = t;
    *sout = s;
    *kout = k;
    return 0;
}

static int boot(struct rl_thread **out, rl_sched **sout, rl_sched_cpu **kout) { return boot_with(&ints, out, sout, kout); }

static int capture_begin(int fds[2])
{
    fflush(stderr);
    if (pipe(fds) != 0)
        return -1;
    int saved = dup(2);
    dup2(fds[1], 2);
    return saved;
}

static void capture_end(int fds[2], int saved, char *buf, size_t n)
{
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(fds[1]);
    ssize_t got = read(fds[0], buf, n - 1);
    buf[got > 0 ? got : 0] = 0;
    close(fds[0]);
}

/** @brief A timer: raises the INT line at a tick. */
static void timer_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)e;
    (void)when;
    raise_int();
}

static void nmi_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)e;
    (void)when;
    raise_nmi();
}

TEST(z80_descriptor_matches_the_context)
{
    const rl_cpu_desc *d = rl_z80_cpu();
    CHECK_EQ(d->ctx_size, sizeof(rl_ctx));
    CHECK_EQ(d->ctx_size, 120);
    CHECK_EQ(d->ctx_align, 8);
    CHECK_EQ(d->common_offset, offsetof(rl_ctx, pend));
    CHECK_EQ(d->time_offset, offsetof(rl_ctx, time));
    CHECK_EQ(d->code_shift, 0);
    CHECK(!d->big_endian);
    CHECK(rl_core_init(&test_guest, d) == 0);
    struct rl_thread *t = rl_thread_alloc();
    CHECK(t != NULL);
    rl_ctx *c = t->ctx;
    CHECK(rl_ctx_time(c) == &c->time);
    CHECK((void *)rl_common(c) == (void *)&c->pend);
    CHECK(rl_common(c)->thread == t);
    CHECK_EQ(c->iff1, 0);
    CHECK_EQ(c->im, 0);
    CHECK_EQ(c->sp, 0xFFFF);
    rl_thread_free(t);
}

TEST(z80_flags_are_the_bits_of_f)
{
    char buf[9];
    CHECK(!strcmp(rl_z80_flags_str(0xFF, buf), "SZYHXPNC"));
    CHECK(!strcmp(rl_z80_flags_str(0x00, buf), "--------"));
    CHECK(!strcmp(rl_z80_flags_str(0x82, buf), "S-----N-"));
    CHECK(!strcmp(rl_z80_flags_str(0x45, buf), "-Z---P-C"));
}

TEST(z80_int_runs_the_entry_of_the_mode_with_the_return_address)
{
    for (uint8_t im = 0; im < 3; im++) {
        struct rl_thread *t;
        rl_sched *s;
        rl_sched_cpu *k;
        CHECK_EQ(boot(&t, &s, &k), 0);
        rl_ctx *c = t->ctx;
        set_iff1(c, 1);
        c->im = im;
        raise_int();
        CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
        CHECK_EQ(hs.runs, 1);
        CHECK_EQ(hs.kind, RL_Z80_ENTRY_IM0 + im);
        CHECK_EQ(hs.ret, 0x0150);
        CHECK_EQ(hs.sp_in, 0xFFFE);
        CHECK_EQ(hs.iff1_in, 1);
        CHECK_EQ(c->iff1, 0);
        CHECK_EQ(c->sp, 0xFFFE);
        CHECK_EQ(hs.budget_in, (int64_t)RL_SCHED_SLICE_DEFAULT);
        CHECK_EQ(rl_sched_cycles(k), 13);
        CHECK_EQ(rl_sched_get_stats(s)->irqs, 1);
        CHECK_EQ(rl_sched_get_stats(s)->redirects, 0);
        CHECK_EQ(mc.ack_count, 1);
        CHECK_EQ(mc.acks[0], RL_Z80_PEND_INT);
        rl_sched_destroy(s);
        rl_thread_free(t);
    }
}

TEST(z80_iff1_clear_blocks_int_but_not_nmi)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    raise_int();
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 0);
    raise_nmi();
    CHECK_EQ(rl_sync(c, ram, 0x0152), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(hs.kind, RL_Z80_ENTRY_NMI);
    CHECK_EQ(hs.ret, 0x0152);
    CHECK_EQ(mc.nmi, 0);
    CHECK_EQ(rl_sched_pending(k), 0);
    set_iff1(c, 1);
    CHECK_EQ(rl_sched_pending(k), 1u << RL_Z80_LINE_INT);
    CHECK_EQ(rl_sync(c, ram, 0x0154), RL_OK);
    CHECK_EQ(hs.runs, 2);
    CHECK_EQ(hs.kind, RL_Z80_ENTRY_IM0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_nmi_comes_before_int)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    set_iff1(c, 1);
    c->im = 1;
    mc.release_on_ack = 1;
    raise_int();
    raise_nmi();
    hs.reenable = 1;
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 2);
    CHECK_EQ(mc.acks[0], RL_Z80_PEND_NMI);
    CHECK_EQ(mc.acks[1], RL_Z80_PEND_INT);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_an_int_that_stays_asserted_is_taken_again_after_the_handler_reenables)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    set_iff1(c, 1);
    c->im = 1;
    raise_int();
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(mc.level, 1);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    set_iff1(c, 1);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 2);
    lower_int();
    CHECK_EQ(rl_sched_pending(k), 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_int_raised_with_iff1_clear_leaves_the_budget_and_ei_ends_it)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    CHECK_EQ(rl_sched_pending(k), 0);
    int64_t b = rl_sched_clock(k)->budget;
    CHECK(b > 0);
    raise_int();
    CHECK_EQ(rl_sched_clock(k)->budget, b);
    CHECK_EQ(rl_sched_pending(k), 0);
    rl_z80_iff(c, 1);
    CHECK(rl_sched_clock(k)->budget <= 0);
    CHECK_EQ(rl_sched_pending(k), 1u << RL_Z80_LINE_INT);
    rl_z80_iff(c, 0);
    CHECK_EQ(rl_sched_pending(k), 0);
    raise_nmi();
    CHECK(rl_sched_clock(k)->budget <= 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_ei_withholds_one_entry_and_ends_the_budget)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    raise_int();
    set_iff1(c, 1);
    c->iff2 = 1;
    c->ei = 1;
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 0);
    CHECK(rl_sched_clock(k)->budget <= 0);
    c->ei = 0;
    CHECK_EQ(rl_sync(c, ram, 0x0152), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(hs.ret, 0x0152);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_ei_does_not_delay_an_nmi)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    set_iff1(c, 1);
    c->ei = 1;
    raise_nmi();
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(hs.kind, RL_Z80_ENTRY_NMI);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_a_machine_without_an_ack_still_enters)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot_with(&bare, &t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    set_iff1(c, 1);
    c->im = 2;
    raise_int();
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(hs.kind, RL_Z80_ENTRY_IM2);
    CHECK_EQ(mc.ack_count, 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_a_handler_that_returns_elsewhere_redirects)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    set_iff1(c, 1);
    c->im = 1;
    raise_int();
    hs.rewrite_to = 0x3000;
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_REDIRECT);
    CHECK_EQ(c->pend.pc, 0x3000);
    CHECK_EQ(c->pend.sp, RL_SP_NONE);
    CHECK_EQ(rl_sched_get_stats(s)->redirects, 1);
    c->pend.kind = RL_PEND_NONE;
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_run_lands_on_the_redirect_target)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    raise_nmi();
    hs.rewrite_to = 0x3000;
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_PENDING);
    c->pend.kind = RL_PEND_NONE;
    CHECK_EQ(rl_z80_run(c, ram, 0x3000), RL_OK);
    CHECK_EQ(hs.landed, 1);
    CHECK_EQ(c->b, 77);
    CHECK_EQ(c->pc, 0x3000);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_dispatch_lookup_and_miss)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    CHECK(rl_lookup(c, 0x3000) == task);
    CHECK(rl_indirect(c, 0x3000) == task);
    CHECK_EQ(c->pc, 0x3000);
    CHECK(rl_lookup(c, 0x3001) == rl_dispatch_miss);
    CHECK(rl_lookup(c, RL_Z80_HALF + 0x3000) == rl_dispatch_miss);
    CHECK(rl_lookup(c, RL_Z80_ENTRY + RL_Z80_ENTRY_IM1) == entry_im1);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_ports_reach_the_machine)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    mc.ports[0x12BF] = 0x5A;
    CHECK_EQ(rl_z80_in(c, 0x12BF), 0x5A);
    CHECK_EQ(mc.last_in, 0x12BF);
    rl_z80_out(c, 0x007F, 0xE1);
    CHECK_EQ(mc.outs, 1);
    CHECK_EQ(mc.last_out, 0x007F);
    CHECK_EQ(mc.last_out_value, 0xE1);
    rl_sched_destroy(s);
    rl_thread_free(t);
    CHECK_EQ(boot_with(&bare, &t, &s, &k), 0);
    CHECK_EQ(rl_z80_in(t->ctx, 0x00FE), 0xFF);
    rl_z80_out(t->ctx, 0x00FE, 1);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_halt_waits_for_int_with_iff1_set_and_counts_nops_in_r)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_device dev = {.name = "timer", .line = RL_LINE_NONE};
    rl_sched_add_device(s, &dev);
    rl_event ev = {.fire = timer_fire, .dev = &dev};
    rl_sched_post(s, &ev, 5002);
    uint64_t before = rl_sched_cycles(k);
    uint8_t r = rl_z80_halt(c, ram, 1, 0xFE);
    uint64_t spent = rl_sched_cycles(k) - before;
    CHECK(spent >= 5002 && spent < 5002 + 4);
    CHECK_EQ(spent % 4, 0);
    CHECK_EQ(c->halted, 0);
    CHECK_EQ(r, (uint8_t)(0x80 | ((0x7E + spent / 4) & 0x7F)));
    set_iff1(c, 1);
    CHECK(rl_sched_clock(k)->budget <= 0);
    c->im = 1;
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(hs.ret, 0x0150);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_halt_with_iff1_clear_ignores_int_and_wakes_on_nmi)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_device dev = {.name = "timer", .line = RL_LINE_NONE};
    rl_sched_add_device(s, &dev);
    rl_event e1 = {.fire = timer_fire, .dev = &dev};
    rl_event e2 = {.fire = nmi_fire, .dev = &dev};
    rl_sched_post(s, &e1, 3000);
    rl_sched_post(s, &e2, 9000);
    CHECK(rl_z80_halt(c, ram, 0, 0x21) > 0x21);
    CHECK(rl_sched_now(k) >= 9000);
    CHECK_EQ(mc.level, 1);
    CHECK_EQ(mc.nmi, 1);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(hs.kind, RL_Z80_ENTRY_NMI);
    CHECK_EQ(c->halted, 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_halt_returns_at_once_when_an_interrupt_can_be_taken)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    raise_int();
    uint64_t now = rl_sched_now(k);
    CHECK_EQ(rl_z80_halt(c, ram, 1, 5), 5);
    CHECK_EQ(rl_sched_now(k), now);
    CHECK(rl_sched_clock(k)->budget <= 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_halt_with_nothing_to_wake_it_stops_the_guest)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    c->pc = 0x0150;
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    (void)rl_z80_halt(c, ram, 1, 0);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "HALT with no interrupt that can wake the CPU") != NULL);
    CHECK_EQ(c->halted, 0);
    c->pend.kind = RL_PEND_NONE;
    rl_sched_destroy(s);
    rl_thread_free(t);
}

static void set_regs(rl_ctx *c)
{
    c->a = 0x12;
    c->f = 0xB0;
    c->b = 0x34;
    c->c = 0x56;
    c->d = 0x78;
    c->e = 0x9A;
    c->h = 0xC0;
    c->l = 0xDE;
    c->ix = 0x1234;
    c->iy = 0x5678;
    c->sp = 0xFFF0;
    c->pc = 0x0150;
    c->i = 0x3F;
    c->r = 0x21;
    c->iff1 = 1;
    c->im = 1;
}

TEST(z80_undefined_report_names_the_registers)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    set_regs(c);
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_undefined(c, ram, 0x0150, 0xED000000u);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->pend.code, RL_EXIT_SOFTWARE);
    CHECK(strstr(out, "kind: undefined instruction\nsummary: undefined instruction 0xed000000 at guest pc 0x0150\n") != NULL);
    CHECK(strstr(out, "\nregisters:\n  af 12b0 S-YH----, bc 3456, de 789a, hl c0de, ix 1234, iy 5678, sp fff0, pc 0150, i 3f "
                      "r 21, iff 10 im 1\n") != NULL);
    c->pend.kind = RL_PEND_NONE;
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_trap_report_and_halted_marker)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    set_regs(c);
    c->halted = 1;
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_trap(c, ram, 0x10);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "kind: trap\nsummary: trap 0x10\n") != NULL);
    CHECK(strstr(out, "\nregisters:\n  af 12b0 ") != NULL);
    CHECK(strstr(out, "im 1, halted\n") != NULL);
    c->pend.kind = RL_PEND_NONE;
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(z80_a_miss_reports_the_target)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    set_regs(c);
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_indirect(c, 0x4321)(c, ram);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "0x4321") != NULL);
    c->pend.kind = RL_PEND_NONE;
    rl_sched_destroy(s);
    rl_thread_free(t);
}

static int custom_miss(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->b = 9;
    return RL_OK;
}

TEST(z80_a_guest_layer_can_take_over_the_miss_path)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_z80_set_miss(custom_miss);
    CHECK_EQ(rl_indirect(c, 0x4321)(c, ram), RL_OK);
    CHECK_EQ(c->b, 9);
    rl_z80_set_miss(NULL);
    rl_sched_destroy(s);
    rl_thread_free(t);
}
