/**
 * @file test_sm83.c
 * @brief The SM83 CPU layer over the core and the device scheduler with a
 * synthetic machine (an interrupt request and enable byte, a timer) that is
 * no real console: context layout, interrupt entry and return, the redirect
 * path, EI delay, HALT and crash report text. This binary links the core and
 * the SM83 layer only.
 */
#include "runner.h"
#include "sm83.h"

#include <stdlib.h>
#include <unistd.h>

static const rl_guest_desc test_guest = {
    .name = "sm83-test",
    .page_size = 0x1000,
    .address_bits = 16,
    .tick_hz = 4194304,
};

/** @brief The synthetic machine: IF and IE, and the acknowledged lines. */
static struct {
    uint8_t iflag;
    uint8_t ie;
    uint32_t acks[8];
    int ack_count;
    rl_sched *sched;
    int stop_calls;
    rl_sm83_stop_info stop;
} mc;

static uint8_t mc_pending(void *o)
{
    (void)o;
    return mc.iflag & mc.ie;
}

static void mc_ack(void *o, uint32_t line)
{
    (void)o;
    mc.iflag = (uint8_t)(mc.iflag & ~(1u << line));
    rl_sched_lower(mc.sched, line);
    mc.acks[mc.ack_count++ & 7] = line;
}

static void mc_stop(void *o, rl_sm83_stop_info *r)
{
    (void)o;
    mc.stop_calls++;
    *r = mc.stop;
}

static const rl_sm83_ints ints = {.pending = mc_pending, .ack = mc_ack, .stop = mc_stop};

static uint8_t ram[0x10000];

/** @brief What the handlers and tasks saw. */
static struct {
    int runs;
    uint16_t sp_in;
    uint8_t pushed_lo;
    uint8_t pushed_hi;
    int64_t budget_in;
    uint8_t ime_in;
    uint16_t order[8];
    int landed;
    uint16_t rewrite_to;
    int reenable;
} hs;

static void raise_line(uint32_t line)
{
    mc.iflag = (uint8_t)(mc.iflag | (1u << line));
    rl_sched_raise(mc.sched, line);
}

/** @brief A handler: records its entry state and executes RET (or RETI). */
static int handler(rl_ctx *c, uint8_t *mem)
{
    hs.sp_in = c->sp;
    hs.pushed_lo = mem[c->sp];
    hs.pushed_hi = mem[(uint16_t)(c->sp + 1u)];
    hs.budget_in = c->time.budget;
    hs.ime_in = c->ime;
    hs.order[hs.runs & 7] = c->pc;
    hs.runs++;
    if (hs.rewrite_to) {
        mem[c->sp] = (uint8_t)hs.rewrite_to;
        mem[(uint16_t)(c->sp + 1u)] = (uint8_t)(hs.rewrite_to >> 8);
    }
    c->sp = (uint16_t)(c->sp + 2u);
    if (hs.reenable)
        rl_sm83_reti(c);
    return RL_OK;
}

static int task(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    hs.landed++;
    c->b = 77;
    return RL_OK;
}

/** @brief A lifted-looking block: a sync point whose next pc is 0x0150. */
static int block(rl_ctx *c, uint8_t *mem)
{
    c->time.budget = 0;
    return rl_sync(c, mem, 0x0150);
}

static int vec_fn_runs[5];

#define VEC(n)                                                                  \
    static int vec##n(rl_ctx *c, uint8_t *mem)                                  \
    {                                                                           \
        vec_fn_runs[n]++;                                                       \
        c->pc = (uint16_t)RL_SM83_IRQ_VECTOR(n);                                \
        return handler(c, mem);                                                 \
    }
VEC(0)
VEC(1)
VEC(2)
VEC(3)
VEC(4)

static const rl_fn_entry fns[] = {
    {0x0040, 8, 0, vec0}, {0x0048, 8, 0, vec1}, {0x0050, 8, 0, vec2}, {0x0058, 8, 0, vec3},
    {0x0060, 8, 0, vec4}, {0x0100, 8, 0, block}, {0x3000, 4, 0, task},
};

static int boot(struct rl_thread **out, rl_sched **sout, rl_sched_cpu **kout)
{
    if (rl_core_init(&test_guest, rl_sm83_cpu()) != 0)
        return -1;
    rl_units_reset();
    static rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(rl_ctx);
    u.fn_count = (uint32_t)(sizeof fns / sizeof fns[0]);
    u.fns = fns;
    u.name = "sm83-test";
    const rl_unit *list[] = {&u};
    if (rl_units_register(list, 1) != 0)
        return -1;
    memset(&mc, 0, sizeof mc);
    memset(&hs, 0, sizeof hs);
    memset(vec_fn_runs, 0, sizeof vec_fn_runs);
    memset(ram, 0, sizeof ram);
    struct rl_thread *t = rl_thread_alloc();
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    if (!t || !s || !k || rl_sm83_attach(k, t, &ints) != 0)
        return -1;
    mc.sched = s;
    t->ctx->sp = 0xFFFE;
    rl_sched_run(k);
    *out = t;
    *sout = s;
    *kout = k;
    return 0;
}

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

TEST(sm83_descriptor_matches_the_context)
{
    const rl_cpu_desc *d = rl_sm83_cpu();
    CHECK_EQ(d->ctx_size, sizeof(rl_ctx));
    CHECK_EQ(d->ctx_size, 104);
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
    CHECK_EQ(c->ime, 0);
    rl_thread_free(t);
}

TEST(sm83_flags_are_the_high_nibble_of_f)
{
    char buf[8];
    CHECK(!strcmp(rl_sm83_flags_str(0xB0, buf), "Z-HC"));
    CHECK(!strcmp(rl_sm83_flags_str(0x00, buf), "----"));
    CHECK(!strcmp(rl_sm83_flags_str(0x50, buf), "-N-C"));
}

TEST(sm83_interrupt_entry_pushes_pc_and_jumps_to_the_vector)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x1F);
    mc.ie = 0x1F;
    rl_sm83_reti(c);
    raise_line(2);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(vec_fn_runs[2], 1);
    CHECK_EQ(hs.sp_in, 0xFFFC);
    CHECK_EQ(hs.pushed_lo, 0x50);
    CHECK_EQ(hs.pushed_hi, 0x01);
    CHECK_EQ(hs.ime_in, 0);
    CHECK_EQ(mc.ack_count, 1);
    CHECK_EQ(mc.acks[0], 2);
    CHECK_EQ(mc.iflag, 0);
    CHECK_EQ(c->sp, 0xFFFE);
    CHECK_EQ(c->ime, 0);
    CHECK_EQ(hs.budget_in, (int64_t)RL_SCHED_SLICE_DEFAULT - RL_SM83_IRQ_CYCLES);
    CHECK_EQ(rl_sched_cycles(k), RL_SM83_IRQ_CYCLES);
    CHECK_EQ(rl_sched_get_stats(s)->irqs, 1);
    CHECK_EQ(rl_sched_get_stats(s)->redirects, 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_ime_clear_blocks_the_entry)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x1F);
    mc.ie = 0x1F;
    raise_line(0);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 0);
    CHECK_EQ(mc.iflag, 1);
    rl_sm83_reti(c);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(vec_fn_runs[0], 1);
    rl_sm83_di(c);
    CHECK_EQ(c->ime, 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_a_disabled_line_does_not_enter)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x04);
    mc.ie = 0x04;
    rl_sm83_reti(c);
    raise_line(1);
    CHECK_EQ(rl_sched_pending(k), 0);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 0);
    CHECK_EQ(c->sp, 0xFFFE);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_lowest_line_first_and_each_handler_reenables)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x1F);
    mc.ie = 0x1F;
    rl_sm83_reti(c);
    raise_line(4);
    raise_line(1);
    raise_line(3);
    hs.reenable = 1;
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 3);
    CHECK_EQ(hs.order[0], 0x48);
    CHECK_EQ(hs.order[1], 0x58);
    CHECK_EQ(hs.order[2], 0x60);
    CHECK_EQ(c->sp, 0xFFFE);
    CHECK_EQ(c->ime, 1);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_without_reenable_the_second_line_waits)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x1F);
    mc.ie = 0x1F;
    rl_sm83_reti(c);
    raise_line(0);
    raise_line(1);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(mc.iflag, 2);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_ei_withholds_one_entry_and_ends_the_budget)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x1F);
    mc.ie = 0x1F;
    raise_line(0);
    rl_sm83_ei(c);
    CHECK_EQ(c->ime, 1);
    CHECK_EQ(c->ei_delay, 1);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 0);
    CHECK_EQ(c->ei_delay, 0);
    CHECK(rl_sched_clock(k)->budget <= 0);
    CHECK_EQ(rl_sync(c, ram, 0x0152), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(hs.pushed_lo, 0x52);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_ei_commit_makes_the_entry_exact)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x1F);
    mc.ie = 0x1F;
    raise_line(3);
    rl_sm83_ei(c);
    rl_sm83_ei_commit(c);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_ie_write_while_ime_set_enters_a_raised_line)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_reti(c);
    mc.ie = 0x01;
    raise_line(0);
    CHECK_EQ(rl_sched_pending(k), 0);
    rl_sm83_set_ie(c, 0x01);
    CHECK_EQ(rl_sched_pending(k), 1);
    CHECK(rl_sched_clock(k)->budget <= 0);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_a_handler_that_returns_elsewhere_redirects)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x1F);
    mc.ie = 0x1F;
    rl_sm83_reti(c);
    raise_line(0);
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

TEST(sm83_run_lands_on_the_redirect_target)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x1F);
    mc.ie = 0x1F;
    rl_sm83_reti(c);
    raise_line(4);
    hs.rewrite_to = 0x3000;
    CHECK_EQ(rl_sm83_run(c, ram, 0x0100), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(hs.landed, 1);
    CHECK_EQ(c->b, 77);
    CHECK_EQ(c->pend.kind, RL_PEND_NONE);
    CHECK_EQ(rl_sched_get_stats(s)->redirects, 1);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_run_without_an_interrupt_returns_ok)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    CHECK_EQ(rl_sm83_run(t->ctx, ram, 0x0100), RL_OK);
    CHECK_EQ(hs.landed, 0);
    CHECK_EQ(t->ctx->pc, 0x0100);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_dispatch_lookup_and_miss)
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
    CHECK(rl_lookup(c, 0x10000 + 0x3000) == rl_dispatch_miss);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

/** @brief A timer: raises its line at a tick. */
static void timer_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)when;
    raise_line((uint32_t)(uintptr_t)e->opaque);
}

TEST(sm83_halt_waits_for_a_line_with_ime_clear)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x01);
    mc.ie = 0x01;
    rl_device dev = {.name = "timer", .line = RL_LINE_NONE};
    rl_sched_add_device(s, &dev);
    rl_event ev = {.fire = timer_fire, .dev = &dev, .opaque = (void *)(uintptr_t)0};
    rl_sched_post(s, &ev, 5000);
    CHECK_EQ(rl_sm83_halt(c, ram), RL_OK);
    CHECK(rl_sched_now(k) >= 5000);
    CHECK_EQ(c->halted, 0);
    CHECK_EQ(hs.runs, 0);
    CHECK_EQ(mc.iflag, 1);
    CHECK_EQ(c->sp, 0xFFFE);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_halt_with_ime_set_enters_after_waking)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x02);
    mc.ie = 0x02;
    rl_sm83_reti(c);
    rl_device dev = {.name = "timer", .line = RL_LINE_NONE};
    rl_sched_add_device(s, &dev);
    rl_event ev = {.fire = timer_fire, .dev = &dev, .opaque = (void *)(uintptr_t)1};
    rl_sched_post(s, &ev, 3000);
    CHECK_EQ(rl_sm83_halt(c, ram), RL_OK);
    CHECK_EQ(rl_sync(c, ram, 0x0150), RL_OK);
    CHECK_EQ(hs.runs, 1);
    CHECK_EQ(vec_fn_runs[1], 1);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_halt_returns_at_once_when_a_line_is_already_pending)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x01);
    mc.ie = 0x01;
    raise_line(0);
    uint64_t now = rl_sched_now(k);
    CHECK_EQ(rl_sm83_halt(c, ram), RL_OK);
    CHECK_EQ(rl_sched_now(k), now);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_halt_with_nothing_to_wake_it_stops_the_guest)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x01);
    mc.ie = 0x01;
    c->pc = 0x0150;
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_sm83_halt(c, ram);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "HALT with no interrupt that can wake the CPU") != NULL);
    CHECK_EQ(c->halted, 0);
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
    c->sp = 0xFFF0;
    c->pc = 0x0150;
    c->ime = 1;
}

TEST(sm83_undefined_report_names_the_registers)
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
    int rc = rl_undefined(c, ram, 0x0150, 0xD3);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->pend.code, RL_EXIT_SOFTWARE);
    CHECK(strstr(out,
                 "undefined instruction 0xd3 at guest pc 0x0150 (af 12b0 Z-HC, bc 3456, de 789a, hl c0de, sp fff0, pc 0150, ime 1)") !=
          NULL);
    c->pend.kind = RL_PEND_NONE;
    saved = capture_begin(fds);
    rc = rl_undefined(c, ram, 0x0150, 0xCB10);
    capture_end(fds, saved, out, sizeof out);
    CHECK(strstr(out, "undefined instruction 0xcb10 at guest pc 0x0150") != NULL);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_trap_report_and_halted_marker)
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
    CHECK(strstr(out, "trap 0x10 (af 12b0 Z-HC,") != NULL);
    CHECK(strstr(out, "ime 1, halted)") != NULL);
    c->pend.kind = RL_PEND_NONE;
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_a_miss_reports_the_target)
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

TEST(sm83_stop_with_a_button_held_and_a_line_pending_is_a_nop)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    mc.ie = 0x04;
    mc.stop = (rl_sm83_stop_info){.kind = RL_SM83_STOP_HALT};
    raise_line(2);
    int64_t b = c->time.budget;
    CHECK_EQ(rl_sm83_stop(c, ram), 1);
    CHECK_EQ(mc.stop_calls, 1);
    CHECK_EQ(c->halted, 0);
    CHECK_EQ(c->time.budget, b);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_stop_mode_with_a_line_pending_waits_for_the_joypad_and_is_one_byte)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    mc.ie = 0x01;
    mc.stop = (rl_sm83_stop_info){.kind = RL_SM83_STOP_MODE};
    raise_line(0);
    rl_device dev = {.name = "joypad", .line = RL_LINE_NONE};
    rl_sched_add_device(s, &dev);
    rl_event ev = {.fire = timer_fire, .dev = &dev, .opaque = (void *)(uintptr_t)4};
    rl_sched_post(s, &ev, 6000);
    CHECK_EQ(rl_sm83_stop(c, ram), 1);
    CHECK_EQ(mc.stop_calls, 1);
    CHECK(rl_sched_now(k) >= 6000);
    CHECK_EQ(c->halted, 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_stop_speed_switch_with_a_line_pending_switches_and_is_one_byte)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    mc.ie = 0x04;
    mc.stop = (rl_sm83_stop_info){.kind = RL_SM83_STOP_SWITCH, .ratio = 2, .run_operand = 0};
    raise_line(2);
    CHECK_EQ(rl_sm83_stop(c, ram), 1);
    CHECK_EQ(mc.stop_calls, 1);
    CHECK_EQ(c->time.budget, -RL_SM83_STOP_STALL);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_stop_speed_switch_changes_the_ratio_and_stalls)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    mc.stop = (rl_sm83_stop_info){.kind = RL_SM83_STOP_SWITCH, .ratio = 2, .run_operand = 0};
    CHECK_EQ(rl_sm83_stop(c, ram), 0);
    CHECK_EQ(mc.stop_calls, 1);
    CHECK_EQ(c->time.budget, -RL_SM83_STOP_STALL);
    uint64_t t0 = rl_sched_now(k);
    c->time.budget -= 100;
    CHECK_EQ(rl_sched_now(k) - t0, 200u);
    mc.stop = (rl_sm83_stop_info){.kind = RL_SM83_STOP_SWITCH, .ratio = 4, .run_operand = 1};
    CHECK_EQ(rl_sm83_stop(c, ram), 1);
    t0 = rl_sched_now(k);
    c->time.budget -= 10;
    CHECK_EQ(rl_sched_now(k) - t0, 40u);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_stop_with_a_button_held_halts_like_halt)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    rl_sm83_set_ie(c, 0x01);
    mc.ie = 0x01;
    mc.stop = (rl_sm83_stop_info){.kind = RL_SM83_STOP_HALT};
    rl_device dev = {.name = "timer", .line = RL_LINE_NONE};
    rl_sched_add_device(s, &dev);
    rl_event ev = {.fire = timer_fire, .dev = &dev, .opaque = (void *)(uintptr_t)0};
    rl_sched_post(s, &ev, 4000);
    CHECK_EQ(rl_sm83_stop(c, ram), 0);
    CHECK(rl_sched_now(k) >= 4000);
    CHECK_EQ(c->halted, 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_stop_mode_waits_for_a_line_even_when_it_is_not_enabled)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    mc.ie = 0;
    mc.stop = (rl_sm83_stop_info){.kind = RL_SM83_STOP_MODE};
    rl_device dev = {.name = "joypad", .line = RL_LINE_NONE};
    rl_sched_add_device(s, &dev);
    rl_event ev = {.fire = timer_fire, .dev = &dev, .opaque = (void *)(uintptr_t)4};
    rl_sched_post(s, &ev, 6000);
    int rc = rl_sm83_stop(c, ram);
    CHECK_EQ(rc, 0);
    CHECK(rl_sched_now(k) >= 6000);
    CHECK_EQ(c->halted, 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sm83_stop_with_nothing_to_wake_it_stops_the_guest)
{
    struct rl_thread *t;
    rl_sched *s;
    rl_sched_cpu *k;
    CHECK_EQ(boot(&t, &s, &k), 0);
    rl_ctx *c = t->ctx;
    mc.stop = (rl_sm83_stop_info){.kind = RL_SM83_STOP_MODE};
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_sm83_stop(c, ram);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK(strstr(out, "STOP with no joypad line") != NULL);
    c->pend.kind = RL_PEND_NONE;
    rl_sched_destroy(s);
    rl_thread_free(t);
}
