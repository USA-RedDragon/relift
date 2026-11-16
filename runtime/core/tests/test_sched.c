/**
 * @file test_sched.c
 * @brief The device scheduler against synthetic devices: a timer that raises
 * a line at a tick, a counter whose read value is the tick it is read at,
 * observers, event order, rates, halting, two CPUs on their fibers and
 * the lifted-code entries rl_sync and rl_io_read.
 */
#include "runner.h"

#include <relift/sched.h>

#include <stdlib.h>

/** @brief Charges n cycles the way lifted code does: one subtraction, one compare. */
static int spend(rl_sched_cpu *k, int64_t n)
{
    rl_time *t = rl_sched_clock(k);
    t->budget -= n;
    if (t->budget <= 0)
        return rl_sched_run(k);
    return 0;
}

/** @brief A timer: an event that raises the device's line. */
typedef struct timer {
    /** @brief The device. */
    rl_device dev;
    /** @brief Its event. */
    rl_event ev;
    /** @brief Tick the line was raised at, 0 before. */
    uint64_t raised_at;
    /** @brief The device's now when the event fired. */
    uint64_t dev_now;
} timer;

static void timer_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    timer *t = e->opaque;
    t->raised_at = when;
    t->dev_now = t->dev.now;
    rl_device_raise(&t->dev);
}

static void timer_init(rl_sched *s, timer *t, uint32_t line)
{
    memset(t, 0, sizeof *t);
    t->dev.name = "timer";
    t->dev.line = line;
    t->ev.fire = timer_fire;
    t->ev.dev = &t->dev;
    t->ev.opaque = t;
    rl_sched_add_device(s, &t->dev);
}

TEST(sched_timer_raises_its_line_on_its_tick)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    timer t;
    timer_init(s, &t, 3);
    rl_sched_set_mask(k, 1u << 3);
    rl_sched_post(s, &t.ev, 1000);
    CHECK_EQ(rl_sched_run(k), 0);
    CHECK_EQ(rl_sched_clock(k)->budget, 1000);
    int blocks = 0, got = 0;
    while (!got && blocks < 1000) {
        got = spend(k, 7);
        blocks++;
    }
    CHECK_EQ(got, 1);
    CHECK_EQ(blocks, 143);
    CHECK_EQ(t.raised_at, 1000);
    CHECK_EQ(t.dev_now, 1000);
    CHECK_EQ(rl_sched_now(k), 1001);
    CHECK_EQ(rl_sched_cycles(k), 1001);
    CHECK_EQ(rl_sched_pending(k), 1u << 3);
    CHECK_EQ(rl_sched_get_stats(s)->events, 1);
    CHECK_EQ(rl_sched_get_stats(s)->runs, 2);
    rl_sched_destroy(s);
}

/** @brief A counter of ticks, advanced only by catch-up. */
typedef struct counter {
    /** @brief The device. */
    rl_device dev;
    /** @brief Ticks counted. */
    uint64_t ticks;
} counter;

static void counter_catch_up(rl_device *d, uint64_t from, uint64_t to)
{
    counter *c = d->opaque;
    c->ticks += to - from;
}

static void counter_catch_up_short(rl_device *d, uint64_t from, uint64_t to)
{
    counter *c = d->opaque;
    c->ticks += to - from - 1;
}

static uint64_t counter_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now)
{
    (void)addr;
    (void)size;
    (void)now;
    return ((counter *)d->opaque)->ticks;
}

static void counter_write(rl_device *d, uint64_t addr, uint32_t size, uint64_t value, uint64_t now)
{
    (void)addr;
    (void)size;
    (void)now;
    ((counter *)d->opaque)->ticks = value;
}

static void counter_init(rl_sched *s, counter *c, rl_device_catch_up *cu)
{
    memset(c, 0, sizeof *c);
    c->dev.name = "counter";
    c->dev.catch_up = cu;
    c->dev.read = counter_read;
    c->dev.write = counter_write;
    c->dev.opaque = c;
    c->dev.line = RL_LINE_NONE;
    rl_sched_add_device(s, &c->dev);
}

/** @brief Runs blocks of pseudo-random cost at the given rate and checks every read of the counter against the CPU's tick. */
static int counter_mismatches(rl_device_catch_up *cu, uint32_t ratio)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, ratio);
    counter c;
    counter_init(s, &c, cu);
    rl_sched_map(s, 0xFF04, 1, &c.dev, 0);
    uint32_t seed = 12345;
    int bad = 0;
    rl_sched_run(k);
    for (int i = 0; i < 2000; i++) {
        seed = seed * 1103515245u + 12345u;
        spend(k, 1 + (seed >> 16) % 24);
        if (rl_sched_read(k, 0xFF04, 1) != rl_sched_now(k))
            bad++;
        if (rl_sched_now(k) != rl_sched_cycles(k) * ratio)
            bad++;
    }
    rl_sched_destroy(s);
    return bad;
}

TEST(sched_counter_read_is_exact_to_the_tick)
{
    CHECK_EQ(counter_mismatches(counter_catch_up, 1), 0);
    CHECK_EQ(counter_mismatches(counter_catch_up, 4), 0);
}

TEST(sched_counter_check_fails_a_short_catch_up)
{
    CHECK(counter_mismatches(counter_catch_up_short, 1) > 1000);
}

TEST(sched_catch_up_is_lazy)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    counter c;
    counter_init(s, &c, counter_catch_up);
    rl_sched_map(s, 0x10, 4, &c.dev, 0);
    rl_sched_run(k);
    for (int i = 0; i < 100; i++)
        spend(k, 10);
    CHECK_EQ(c.dev.now, 0);
    CHECK_EQ(rl_sched_get_stats(s)->catch_ups, 0);
    CHECK_EQ(rl_sched_read(k, 0x12, 1), 1000);
    CHECK_EQ(rl_sched_read(k, 0x12, 1), 1000);
    CHECK_EQ(rl_sched_get_stats(s)->catch_ups, 1);
    rl_sched_write(k, 0x10, 1, 5);
    spend(k, 3);
    CHECK_EQ(rl_sched_read(k, 0x10, 1), 8);
    rl_sched_destroy(s);
}

/** @brief Catch-up log shared by the observer devices. */
static char g_order[16];
static int g_norder;

static void logged_catch_up(rl_device *d, uint64_t from, uint64_t to)
{
    counter_catch_up(d, from, to);
    if (g_norder < (int)sizeof g_order - 1)
        g_order[g_norder++] = d->name[0];
}

static uint64_t peer_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now)
{
    (void)addr;
    (void)size;
    (void)now;
    return ((counter *)d->opaque)->ticks;
}

TEST(sched_access_catches_up_the_named_devices_in_order)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    counter b, a, c;
    counter_init(s, &b, logged_catch_up);
    counter_init(s, &a, logged_catch_up);
    counter_init(s, &c, logged_catch_up);
    b.dev.name = "b";
    a.dev.name = "a";
    c.dev.name = "c";
    CHECK_EQ(rl_sched_map(s, 0x100, 0x10, &a.dev, rl_device_bit(&b.dev)), 0);
    rl_sched_run(k);
    spend(k, 100);
    rl_sched_read(k, 0x105, 1);
    CHECK_EQ(a.dev.now, 100);
    CHECK_EQ(b.dev.now, 100);
    CHECK_EQ(c.dev.now, 0);
    CHECK_EQ(c.ticks, 0);
    g_order[g_norder] = 0;
    CHECK(strcmp(g_order, "ba") == 0);
    rl_sched_destroy(s);
}

TEST(sched_an_observer_is_exact_only_when_named)
{
    for (int named = 0; named < 2; named++) {
        rl_sched *s = rl_sched_create(NULL);
        rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
        counter src, view;
        counter_init(s, &src, counter_catch_up);
        counter_init(s, &view, NULL);
        view.dev.read = peer_read;
        view.dev.opaque = &src;
        rl_sched_map(s, 0x20, 1, &view.dev, named ? rl_device_bit(&src.dev) : 0);
        rl_sched_run(k);
        spend(k, 77);
        uint64_t v = rl_sched_read(k, 0x20, 1);
        if (named)
            CHECK_EQ(v, 77);
        else
            CHECK_EQ(v, 0);
        rl_sched_destroy(s);
    }
}

/** @brief Fire log of the order test. */
static int g_fired[16];
static int g_nfired;
static uint64_t g_fired_when[16];

static void log_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    g_fired_when[g_nfired] = when;
    g_fired[g_nfired++] = (int)(intptr_t)e->opaque;
}

TEST(sched_events_fire_by_tick_then_post_order)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    rl_event e[8];
    memset(e, 0, sizeof e);
    for (int i = 0; i < 8; i++) {
        e[i].fire = log_fire;
        e[i].opaque = (void *)(intptr_t)i;
    }
    rl_sched_post(s, &e[1], 50);
    rl_sched_post(s, &e[2], 20);
    rl_sched_post(s, &e[3], 50);
    rl_sched_post(s, &e[4], 20);
    rl_sched_post(s, &e[5], 30);
    rl_sched_post(s, &e[6], 10);
    rl_sched_post(s, &e[7], 40);
    rl_sched_cancel(s, &e[5]);
    CHECK(!rl_event_queued(&e[5]));
    rl_sched_post(s, &e[6], 50);
    CHECK(rl_event_queued(&e[6]));
    rl_sched_run(k);
    CHECK_EQ(rl_sched_clock(k)->budget, 20);
    for (int i = 0; i < 20; i++)
        spend(k, 5);
    static const int want[] = {2, 4, 7, 1, 3, 6};
    CHECK_EQ(g_nfired, 6);
    for (int i = 0; i < 6; i++)
        CHECK_EQ(g_fired[i], want[i]);
    CHECK_EQ(g_fired_when[2], 40);
    CHECK_EQ(rl_sched_time(s), 50);
    CHECK_EQ(rl_sched_get_stats(s)->events, 6);
    rl_sched_destroy(s);
}

/** @brief Reposts itself every 64 ticks and records the gap to the CPU's tick. */
static uint64_t g_late_max;

static void periodic_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    rl_sched_cpu *k = rl_sched_current(s);
    uint64_t late = rl_sched_now(k) - when;
    if (late > g_late_max)
        g_late_max = late;
    rl_sched_post(s, e, when + 64);
}

TEST(sched_posting_cuts_the_budget)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    rl_sched_run(k);
    CHECK_EQ(rl_sched_clock(k)->budget, (int64_t)RL_SCHED_SLICE_DEFAULT);
    spend(k, 30);
    rl_event e = {.fire = periodic_fire};
    rl_sched_post(s, &e, 40);
    CHECK_EQ(rl_sched_clock(k)->budget, 10);
    for (int i = 0; i < 1000; i++)
        spend(k, 4);
    CHECK_EQ(rl_sched_get_stats(s)->events, (4030 - 40) / 64 + 1);
    CHECK(g_late_max < 4);
    rl_sched_destroy(s);
}

/** @brief Order log of the access test. */
static char g_seen[8];
static int g_nseen;

static void mark_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)e;
    (void)when;
    g_seen[g_nseen++] = 'e';
}

static uint64_t mark_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now)
{
    (void)d;
    (void)addr;
    (void)size;
    g_seen[g_nseen++] = 'r';
    return now;
}

TEST(sched_an_access_runs_the_events_due_first)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    rl_device d = {.name = "mark", .read = mark_read, .line = RL_LINE_NONE};
    rl_sched_add_device(s, &d);
    rl_sched_map(s, 0x40, 1, &d, 0);
    rl_event e = {.fire = mark_fire};
    rl_sched_run(k);
    rl_sched_post(s, &e, 50);
    rl_sched_stall(k, 60);
    CHECK_EQ(rl_sched_now(k), 60);
    CHECK_EQ(rl_sched_read(k, 0x40, 1), 60);
    CHECK(strcmp(g_seen, "er") == 0);
    rl_sched_post(s, &e, 10);
    CHECK_EQ(rl_sched_clock(k)->budget, 0);
    CHECK_EQ(rl_sched_read(k, 0x40, 1), 60);
    CHECK(strcmp(g_seen, "erer") == 0);
    rl_sched_destroy(s);
}

TEST(sched_rates_share_one_timeline)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 2);
    timer t;
    timer_init(s, &t, 0);
    rl_sched_set_mask(k, 1);
    rl_sched_post(s, &t.ev, 1001);
    rl_sched_run(k);
    CHECK_EQ(rl_sched_clock(k)->budget, 501);
    int got = 0;
    for (int i = 0; i < 2000 && !got; i++)
        got = spend(k, 1);
    CHECK_EQ(got, 1);
    CHECK_EQ(rl_sched_cycles(k), 501);
    CHECK_EQ(rl_sched_now(k), 1002);
    CHECK_EQ(t.raised_at, 1001);
    rl_sched_set_ratio(k, 1);
    CHECK_EQ(rl_sched_clock(k)->budget, 0);
    spend(k, 10);
    CHECK_EQ(rl_sched_cycles(k), 511);
    CHECK_EQ(rl_sched_now(k), 1012);
    rl_sched_destroy(s);
}

TEST(sched_masked_lines_wait_for_the_mask)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    rl_sched_run(k);
    spend(k, 5);
    int64_t before = rl_sched_clock(k)->budget;
    rl_sched_raise(s, 2);
    CHECK_EQ(rl_sched_pending(k), 0);
    CHECK_EQ(rl_sched_clock(k)->budget, before);
    rl_sched_set_mask(k, 1u << 2);
    CHECK_EQ(rl_sched_clock(k)->budget, 0);
    CHECK_EQ(rl_sched_now(k), 5);
    CHECK_EQ(spend(k, 1), 1);
    rl_sched_lower(s, 2);
    CHECK_EQ(rl_sched_pending(k), 0);
    CHECK_EQ(spend(k, (int64_t)RL_SCHED_SLICE_DEFAULT), 0);
    rl_sched_raise(s, 2);
    CHECK_EQ(rl_sched_clock(k)->budget, 0);
    rl_sched_destroy(s);
}

/** @brief Accesses a logging device saw. */
static struct {
    uint64_t addr[16];
    uint32_t size[16];
    uint64_t value[16];
    uint64_t at[16];
    int n;
} gl;

static uint64_t log_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now)
{
    (void)d;
    gl.addr[gl.n] = addr;
    gl.size[gl.n] = size;
    gl.at[gl.n] = now;
    return gl.value[gl.n++] = 0x10 + (addr & 0xf);
}

static void log_store(rl_device *d, uint64_t addr, uint32_t size, uint64_t value, uint64_t now)
{
    (void)d;
    gl.addr[gl.n] = addr;
    gl.size[gl.n] = size;
    gl.value[gl.n] = value;
    gl.at[gl.n++] = now;
}

TEST(sched_an_access_that_straddles_ranges_goes_byte_by_byte)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    rl_device a = {.name = "a", .read = log_read, .write = log_store, .line = RL_LINE_NONE};
    rl_device b = {.name = "b", .read = log_read, .write = log_store, .line = RL_LINE_NONE};
    rl_sched_add_device(s, &a);
    rl_sched_add_device(s, &b);
    CHECK_EQ(rl_sched_map(s, 0x100, 4, &a, 0), 0);
    CHECK_EQ(rl_sched_map(s, 0x104, 4, &b, 0), 0);
    CHECK_EQ(rl_sched_map(s, 0x109, 1, &b, 0), 0);
    rl_sched_run(k);
    memset(&gl, 0, sizeof gl);
    CHECK_EQ(rl_sched_read(k, 0x100, 4), 0x10);
    CHECK_EQ(gl.n, 1);
    CHECK_EQ(gl.size[0], 4);
    memset(&gl, 0, sizeof gl);
    CHECK_EQ(rl_sched_read(k, 0x102, 4), 0x15141312ull);
    CHECK_EQ(gl.n, 4);
    for (int i = 0; i < 4; i++) {
        CHECK_EQ(gl.addr[i], 0x102 + i);
        CHECK_EQ(gl.size[i], 1);
        CHECK_EQ(gl.at[i], 0);
    }
    memset(&gl, 0, sizeof gl);
    rl_sched_write(k, 0x106, 4, 0x44332211);
    CHECK_EQ(gl.n, 3);
    CHECK_EQ(gl.addr[0], 0x106);
    CHECK_EQ(gl.value[0], 0x11);
    CHECK_EQ(gl.addr[1], 0x107);
    CHECK_EQ(gl.value[1], 0x22);
    CHECK_EQ(gl.addr[2], 0x109);
    CHECK_EQ(gl.value[2], 0x44);
    memset(&gl, 0, sizeof gl);
    CHECK_EQ(rl_sched_read(k, 0x107, 4), 0xff19ff17ull);
    rl_sched_destroy(s);
}

TEST(sched_an_access_that_leaves_the_mapped_ranges_mixes_in_ones)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    rl_device a = {.name = "a", .read = log_read, .write = log_store, .line = RL_LINE_NONE};
    rl_sched_add_device(s, &a);
    CHECK_EQ(rl_sched_map(s, 0x100, 2, &a, 0), 0);
    rl_sched_run(k);
    memset(&gl, 0, sizeof gl);
    CHECK_EQ(rl_sched_read(k, 0x100, 4), 0xffff1110ull);
    CHECK_EQ(gl.n, 2);
    CHECK_EQ(rl_sched_read(k, 0xfe, 4), 0x1110ffffull);
    CHECK_EQ(rl_sched_read(k, 0xf0, 8), UINT64_MAX);
    rl_sched_destroy(s);
}

TEST(sched_halt_skips_to_the_next_interrupt)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    timer t;
    timer_init(s, &t, 1);
    rl_event other = {.fire = log_fire, .opaque = (void *)(intptr_t)9};
    rl_sched_post(s, &other, 3000);
    rl_sched_post(s, &t.ev, 5000);
    rl_sched_set_mask(k, 1u << 1);
    rl_sched_run(k);
    spend(k, 12);
    CHECK_EQ(rl_sched_halt(k), 1);
    CHECK_EQ(rl_sched_now(k), 5000);
    CHECK_EQ(g_nfired, 1);
    rl_sched_lower(s, 1);
    CHECK_EQ(rl_sched_halt(k), 0);
    rl_sched_destroy(s);
}

TEST(sched_map_refuses_overlaps)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    counter a, b;
    counter_init(s, &a, counter_catch_up);
    counter_init(s, &b, counter_catch_up);
    rl_device stray = {.name = "stray"};
    CHECK_EQ(rl_sched_map(s, 0x100, 0x10, &a.dev, 0), 0);
    CHECK_EQ(rl_sched_map(s, 0x10F, 1, &b.dev, 0), -1);
    CHECK_EQ(rl_sched_map(s, 0xF0, 0x11, &b.dev, 0), -1);
    CHECK_EQ(rl_sched_map(s, 0xF0, 0x10, &b.dev, 0), 0);
    CHECK_EQ(rl_sched_map(s, 0x110, 0, &b.dev, 0), -1);
    CHECK_EQ(rl_sched_map(s, UINT64_MAX - 1, 3, &b.dev, 0), -1);
    CHECK_EQ(rl_sched_map(s, UINT64_MAX - 1, 2, &b.dev, 0), 0);
    CHECK_EQ(rl_sched_map(s, 0x200, 1, &stray, 0), -1);
    rl_sched_run(k);
    spend(k, 9);
    CHECK_EQ(rl_sched_read(k, 0x10F, 1), 9);
    CHECK_EQ(rl_sched_read(k, 0xF0, 1), 9);
    CHECK_EQ(rl_sched_read(k, UINT64_MAX, 1), 9);
    CHECK_EQ(rl_sched_read(k, 0x110, 1), 0xFF);
    CHECK_EQ(rl_sched_read(k, 0x110, 2), 0xFFFF);
    CHECK_EQ(rl_sched_read(k, 0x110, 8), UINT64_MAX);
    rl_sched_write(k, 0x110, 1, 1);
    CHECK_EQ(rl_sched_get_stats(s)->unmapped, 4);
    rl_sched_destroy(s);
}

/** @brief One guest CPU of the two-CPU test. */
typedef struct cpu_run {
    /** @brief Its clock. */
    rl_sched_cpu *k;
    /** @brief Cycles per block. */
    int64_t cost;
    /** @brief Blocks to run. */
    int blocks;
} cpu_run;

/** @brief State of the two-CPU test. */
static struct {
    rl_sched_cpu *k[2];
    bool active[2];
    uint64_t skew_max;
    uint64_t late_events;
    uint32_t log[4096];
    int nlog;
    int main_rc;
} g2;

static uint64_t skew_of(void)
{
    if (!g2.active[0] || !g2.active[1])
        return 0;
    uint64_t a = rl_sched_now(g2.k[0]), b = rl_sched_now(g2.k[1]);
    return a > b ? a - b : b - a;
}

static void log_write(rl_device *d, uint64_t addr, uint32_t size, uint64_t value, uint64_t now)
{
    (void)d;
    (void)addr;
    (void)size;
    uint64_t sk = skew_of();
    if (sk > g2.skew_max)
        g2.skew_max = sk;
    if (g2.nlog < 4096)
        g2.log[g2.nlog++] = (uint32_t)(value << 24 | (now & 0xFFFFFF));
}

static void frontier_fire(rl_sched *s, rl_event *e, uint64_t when)
{
    for (int i = 0; i < 2; i++)
        if (g2.active[i] && rl_sched_now(g2.k[i]) < when)
            g2.late_events++;
    rl_sched_post(s, e, when + 100);
}

static void cpu_main(rl_sched_cpu *k, void *p)
{
    cpu_run *r = p;
    (void)k;
    rl_sched_run(r->k);
    for (int i = 0; i < r->blocks; i++) {
        spend(r->k, r->cost);
        rl_sched_write(r->k, 0x50, 1, r->k == g2.k[0] ? 0 : 1);
    }
    g2.active[r->k == g2.k[0] ? 0 : 1] = false;
}

/** @brief Runs two CPUs (rates 1 and 2) on their fibers; fills g2. */
static void run_two(uint64_t quantum)
{
    memset(&g2, 0, sizeof g2);
    rl_sched_config cfg = {.quantum = quantum};
    rl_sched *s = rl_sched_create(&cfg);
    g2.k[0] = rl_sched_add_cpu(s, 1);
    g2.k[1] = rl_sched_add_cpu(s, 2);
    g2.active[0] = g2.active[1] = true;
    rl_device d = {.name = "log", .write = log_write, .line = RL_LINE_NONE};
    rl_sched_add_device(s, &d);
    rl_sched_map(s, 0x50, 1, &d, 0);
    rl_event e = {.fire = frontier_fire};
    rl_sched_post(s, &e, 100);
    cpu_run r[2] = {{g2.k[0], 3, 1500}, {g2.k[1], 5, 1000}};
    rl_sched_spawn(g2.k[1], cpu_main, &r[1]);
    rl_sched_spawn(g2.k[0], cpu_main, &r[0]);
    g2.main_rc = rl_sched_main(s);
    rl_sched_destroy(s);
}

TEST(sched_two_cpus_stay_within_the_quantum_and_repeat)
{
    run_two(64);
    CHECK_EQ(g2.main_rc, 0);
    CHECK_EQ(g2.nlog, 2500);
    CHECK(g2.skew_max <= 64 + 10 + 2);
    CHECK(g2.skew_max > 32);
    CHECK_EQ(g2.late_events, 0);
    static uint32_t first[4096];
    memcpy(first, g2.log, sizeof first);
    for (int round = 0; round < 3; round++) {
        run_two(64);
        CHECK(memcmp(first, g2.log, sizeof first) == 0);
    }
    run_two(4096);
    CHECK(g2.skew_max > 64 + 10 + 2);
    CHECK(g2.skew_max <= 100 + 10 + 2);
}

/** @brief The ordered-mode runs: the accesses each CPU logged, in the order they happened. */
static struct {
    rl_sched_cpu *k[2];
    uint64_t at[4096];
    uint8_t who[4096];
    int n;
    int steps[2];
    int64_t cost[2];
    int halt_cpu;
    int wake_step;
    uint64_t woke_at;
    uint64_t waits_while_halted;
    uint64_t waits_at_halt;
} go;

static void go_log(int id)
{
    if (go.n < 4096) {
        go.at[go.n] = rl_sched_now(go.k[id]);
        go.who[go.n++] = (uint8_t)id;
    }
}

static void go_main(rl_sched_cpu *k, void *p)
{
    int id = (int)(intptr_t)p;
    rl_sched_run(k);
    if (id == go.halt_cpu) {
        go.waits_at_halt = rl_sched_get_stats(rl_sched_of(k))->order_waits;
        CHECK_EQ(rl_sched_halt(k), 1);
        go.woke_at = rl_sched_now(k);
        rl_sched_lower(rl_sched_of(k), 0);
    }
    for (int i = 0; i < go.steps[id]; i++) {
        spend(k, go.cost[id] + (i * 7 + id) % 3);
        rl_sched_order(k);
        go_log(id);
        if (id != go.halt_cpu && go.halt_cpu >= 0 && i == go.wake_step) {
            go.waits_while_halted = rl_sched_get_stats(rl_sched_of(k))->order_waits;
            rl_sched_cpu *h = go.k[go.halt_cpu];
            uint64_t now = rl_sched_now(k), hn = rl_sched_now(h);
            rl_sched_stall(h, (now + 2 - hn + 1) / 2);
            rl_sched_raise(rl_sched_of(k), 0);
        }
    }
}

/** @brief Runs two CPUs (ratios 1 and 2) in ordered mode; rank1 is CPU 1's rank. */
static rl_sched_stats go_run(uint32_t rank1, int halt_cpu)
{
    go.n = 0;
    go.halt_cpu = halt_cpu;
    go.woke_at = 0;
    rl_sched_config cfg = {.ordered = true, .stack = 256u << 10};
    rl_sched *s = rl_sched_create(&cfg);
    go.k[0] = rl_sched_add_cpu(s, 1);
    go.k[1] = rl_sched_add_cpu(s, 2);
    rl_sched_set_rank(go.k[1], rank1);
    rl_sched_set_rank(go.k[0], rank1 ? 0 : 1);
    rl_sched_route(s, 0, 1);
    rl_sched_set_mask(go.k[1], 1);
    rl_sched_spawn(go.k[0], go_main, (void *)(intptr_t)0);
    rl_sched_spawn(go.k[1], go_main, (void *)(intptr_t)1);
    rl_sched_main(s);
    rl_sched_stats st = *rl_sched_get_stats(s);
    rl_sched_destroy(s);
    return st;
}

/** @brief True when the log is in (tick, rank, index) order for the ranks given. */
static bool go_in_order(uint32_t rank1)
{
    for (int i = 1; i < go.n; i++) {
        uint64_t a = go.at[i - 1], b = go.at[i];
        if (a > b)
            return false;
        if (a == b && go.who[i - 1] != go.who[i]) {
            if (go.who[i - 1] != (rank1 ? 0 : 1))
                return false;
        }
    }
    return true;
}

TEST(sched_ordered_accesses_follow_the_tick_then_the_rank)
{
    go.steps[0] = 1500;
    go.steps[1] = 700;
    go.cost[0] = 2;
    go.cost[1] = 1;
    rl_sched_stats st = go_run(1, -1);
    CHECK_EQ(go.n, 2200);
    CHECK(go_in_order(1));
    CHECK(st.order_waits > 100);
    CHECK_EQ(st.orders, 2200);
    static uint64_t first[4096];
    memcpy(first, go.at, sizeof first);
    go_run(1, -1);
    CHECK(memcmp(first, go.at, sizeof first) == 0);
    int ties = 0, cpu1_first = 0;
    go_run(0, -1);
    for (int i = 1; i < go.n; i++)
        if (go.at[i - 1] == go.at[i] && go.who[i - 1] != go.who[i]) {
            ties++;
            cpu1_first += go.who[i - 1] == 1;
        }
    CHECK(ties > 10);
    CHECK_EQ(cpu1_first, ties);
}

TEST(sched_ordered_halt_leaves_the_order_until_a_raise)
{
    go.steps[0] = 400;
    go.steps[1] = 100;
    go.cost[0] = 5;
    go.cost[1] = 3;
    go.wake_step = 200;
    rl_sched_stats st = go_run(1, 1);
    CHECK_EQ(go.waits_while_halted, go.waits_at_halt);
    CHECK(go.woke_at > 1000);
    CHECK_EQ(go.woke_at % 2, 0);
    int k = 0;
    while (k < go.n && go.who[k] == 0)
        k++;
    CHECK(k >= 201);
    CHECK(go.at[k] > go.woke_at);
    CHECK(go.woke_at >= go.at[200] + 2);
    CHECK(go.woke_at <= go.at[200] + 3);
    CHECK(go_in_order(1));
    CHECK(st.order_waits > 0);
}

static void alone_main(rl_sched_cpu *k, void *p)
{
    (void)p;
    rl_sched_run(k);
    go.woke_at = (uint64_t)rl_sched_halt(k) << 32 | rl_sched_now(k);
    rl_sched_lower(rl_sched_of(k), 1);
    go.steps[0] = rl_sched_halt(k);
}

static void leave_main(rl_sched_cpu *k, void *p)
{
    (void)p;
    rl_sched_run(k);
    spend(k, 40);
}

TEST(sched_ordered_halt_alone_runs_to_the_next_event)
{
    rl_sched_config cfg = {.ordered = true, .stack = 256u << 10};
    rl_sched *s = rl_sched_create(&cfg);
    rl_sched_cpu *a = rl_sched_add_cpu(s, 1), *b = rl_sched_add_cpu(s, 2);
    timer t;
    timer_init(s, &t, 1);
    rl_sched_route(s, 1, 1);
    rl_sched_set_mask(b, 1u << 1);
    rl_sched_post(s, &t.ev, 501);
    go.steps[0] = 7;
    rl_sched_spawn(a, leave_main, NULL);
    rl_sched_spawn(b, alone_main, NULL);
    CHECK_EQ(rl_sched_main(s), 0);
    CHECK_EQ(go.woke_at >> 32, 1);
    CHECK_EQ(go.woke_at & 0xFFFFFFFFu, 502);
    CHECK_EQ(t.raised_at, 501);
    CHECK_EQ(go.steps[0], 0);
    rl_sched_destroy(s);
}

/** @brief A fake CPU with a clock in its context. */
struct rl_ctx {
    /** @brief Four registers. */
    uint32_t r[4];
    /** @brief The core's block. */
    rl_ctx_common k;
    /** @brief The clock. */
    rl_time time;
};

static const rl_cpu_desc clock_cpu = {
    .name = "clocked",
    .ctx_size = sizeof(struct rl_ctx),
    .ctx_align = 8,
    .common_offset = offsetof(struct rl_ctx, k),
    .time_offset = offsetof(struct rl_ctx, time),
};

static const rl_guest_desc clock_guest = {
    .name = "clocked",
    .page_size = 0x1000,
    .address_bits = 16,
    .tick_hz = 4194304,
};

/** @brief State of the rl_sync test. */
static struct {
    rl_sched_cpu *k;
    uint64_t next[4];
    int entries;
    int handler_runs;
    int handler_result;
} gs;

static uint64_t irq_hook(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    (void)mem;
    if (!rl_sched_pending(gs.k))
        return RL_NO_IRQ;
    rl_sched_lower(rl_sched_of(gs.k), 0);
    gs.next[gs.entries++ & 3] = next;
    c->time.budget -= 20;
    return 0x2000;
}

static uint64_t irq_handler(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    gs.handler_runs++;
    c->r[0] = (uint32_t)(c->time.deadline - (uint64_t)c->time.budget);
    return gs.handler_result;
}

static int boot_clocked(struct rl_thread **out, rl_sched **sout)
{
    if (rl_core_init(&clock_guest, &clock_cpu) != 0)
        return -1;
    static const rl_fn_entry fns[] = {{0x2000, 4, 0, irq_handler}};
    static rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(struct rl_ctx);
    u.fn_count = 1;
    u.fns = fns;
    u.name = "clocked";
    const rl_unit *list[] = {&u};
    if (rl_units_register(list, 1) != 0)
        return -1;
    struct rl_thread *t = rl_thread_alloc();
    rl_sched *s = rl_sched_create(NULL);
    gs.k = rl_sched_add_cpu(s, 1);
    if (!t || rl_sched_attach(gs.k, t) != 0)
        return -1;
    rl_sched_set_irq(gs.k, irq_hook);
    rl_sched_set_mask(gs.k, 1);
    *out = t;
    *sout = s;
    return 0;
}

TEST(sched_sync_enters_the_interrupt_and_returns_to_next)
{
    struct rl_thread *t;
    rl_sched *s;
    CHECK_EQ(boot_clocked(&t, &s), 0);
    rl_ctx *c = t->ctx;
    CHECK(rl_sched_clock(gs.k) == &c->time);
    timer tm;
    timer_init(s, &tm, 0);
    rl_sched_post(s, &tm.ev, 400);
    counter cn;
    counter_init(s, &cn, counter_catch_up);
    rl_sched_map(s, 0xFF04, 1, &cn.dev, 0);
    int syncs = 0;
    for (uint64_t pc = 0x100; pc < 0x100 + 200; pc++) {
        int64_t budget = c->time.budget;
        budget -= 4;
        if (budget <= 0) {
            c->time.budget = budget;
            syncs++;
            CHECK_EQ(rl_sync(c, NULL, pc), RL_OK);
            budget = c->time.budget;
        }
        c->time.budget = budget;
        if (pc == 0x180)
            CHECK_EQ(rl_io_read(c, NULL, 0xFF04, 1), 0x81 * 4 + 20);
    }
    CHECK_EQ(gs.handler_runs, 1);
    CHECK_EQ(gs.entries, 1);
    CHECK_EQ(gs.next[0], 0x100 + 99);
    CHECK_EQ(c->r[0], 400 + 20);
    CHECK_EQ(rl_sched_get_stats(s)->irqs, 1);
    CHECK_EQ(syncs, 2);
    CHECK_EQ(rl_sched_cycles(gs.k), 200 * 4 + 20);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sched_sync_propagates_a_pending_handler)
{
    struct rl_thread *t;
    rl_sched *s;
    CHECK_EQ(boot_clocked(&t, &s), 0);
    gs.handler_result = RL_PENDING;
    rl_sched_raise(s, 0);
    CHECK_EQ(rl_sync(t->ctx, NULL, 0x300), RL_PENDING);
    CHECK_EQ(gs.handler_runs, 1);
    gs.handler_result = RL_OK;
    rl_sched_raise(s, 0);
    CHECK_EQ(rl_sync(t->ctx, NULL, 0x300), RL_OK);
    CHECK_EQ(gs.handler_runs, 2);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

/** @brief State of the redirect tests: where the synthetic handler says it returned. */
static struct {
    uint64_t returns_to;
    int hook_calls;
    int landed;
    uint64_t landed_pc;
    int tasks;
} rd;

static uint64_t ret_hook(rl_ctx *c, uint8_t *mem, uint64_t expected)
{
    (void)c;
    (void)mem;
    (void)expected;
    rd.hook_calls++;
    return rd.returns_to;
}

static uint64_t task_fn(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    rd.landed++;
    rd.landed_pc = 0x3000;
    c->r[1] = 77;
    return RL_OK;
}

static int boot_redirect(struct rl_thread **out, rl_sched **sout, uint32_t depth)
{
    if (rl_core_init(&clock_guest, &clock_cpu) != 0)
        return -1;
    rl_units_reset();
    static const rl_fn_entry fns[] = {{0x2000, 4, 0, irq_handler}, {0x3000, 4, 0, task_fn}};
    static rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(struct rl_ctx);
    u.fn_count = 2;
    u.fns = fns;
    u.name = "redirect";
    const rl_unit *list[] = {&u};
    if (rl_units_register(list, 1) != 0)
        return -1;
    rl_sched_config cfg = {.irq_depth = depth};
    struct rl_thread *t = rl_thread_alloc();
    rl_sched *s = rl_sched_create(&cfg);
    gs.k = rl_sched_add_cpu(s, 1);
    if (!t || rl_sched_attach(gs.k, t) != 0)
        return -1;
    gs.entries = 0;
    gs.handler_runs = 0;
    gs.handler_result = RL_OK;
    memset(&rd, 0, sizeof rd);
    rl_sched_set_irq(gs.k, irq_hook);
    rl_sched_set_irq_return(gs.k, ret_hook);
    rl_sched_set_mask(gs.k, 1);
    *out = t;
    *sout = s;
    return 0;
}

TEST(sched_sync_returning_to_next_does_not_redirect)
{
    struct rl_thread *t;
    rl_sched *s;
    CHECK_EQ(boot_redirect(&t, &s, 0), 0);
    rd.returns_to = 0x300;
    rl_sched_raise(s, 0);
    CHECK_EQ(rl_sync(t->ctx, NULL, 0x300), RL_OK);
    CHECK_EQ(rd.hook_calls, 1);
    CHECK_EQ(rl_sched_get_stats(s)->redirects, 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sched_sync_redirects_a_handler_that_returns_elsewhere)
{
    struct rl_thread *t;
    rl_sched *s;
    CHECK_EQ(boot_redirect(&t, &s, 0), 0);
    rd.returns_to = 0x3000;
    rl_sched_raise(s, 0);
    CHECK_EQ(rl_sync(t->ctx, NULL, 0x300), RL_PENDING);
    CHECK_EQ(rl_common(t->ctx)->pend.kind, RL_PEND_REDIRECT);
    CHECK_EQ(rl_common(t->ctx)->pend.pc, 0x3000);
    CHECK_EQ(rl_common(t->ctx)->pend.sp, RL_SP_NONE);
    CHECK_EQ(rl_sched_get_stats(s)->redirects, 1);
    rl_common(t->ctx)->pend.kind = RL_PEND_NONE;
    rl_sched_destroy(s);
    rl_thread_free(t);
}

static uint64_t nest_fn(rl_ctx *c, uint8_t *mem)
{
    rd.tasks++;
    rl_sched_raise(rl_sched_of(gs.k), 0);
    return rl_sync(c, mem, 0x500);
}

TEST(sched_sync_nested_entries_are_bounded)
{
    struct rl_thread *t;
    rl_sched *s;
    CHECK_EQ(boot_redirect(&t, &s, 8), 0);
    rl_fn_entry e = {0x2000, 4, 0, nest_fn};
    rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(struct rl_ctx);
    u.fn_count = 1;
    u.fns = &e;
    u.name = "nest";
    const rl_unit *list[] = {&u};
    rl_units_reset();
    CHECK_EQ(rl_units_register(list, 1), 0);
    rd.returns_to = 0x500;
    rl_sched_raise(s, 0);
    CHECK_EQ(rl_sync(t->ctx, NULL, 0x500), RL_PENDING);
    CHECK_EQ(rl_common(t->ctx)->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(rd.tasks, 8);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

static uintptr_t stack_lo, stack_hi;

static void note_stack(void)
{
    int here;
    uintptr_t p = (uintptr_t)&here;
    if (!stack_lo || p < stack_lo)
        stack_lo = p;
    if (p > stack_hi)
        stack_hi = p;
}

static uint64_t switch_fn(rl_ctx *c, uint8_t *mem)
{
    note_stack();
    if (rd.landed++ < 1000) {
        rl_sched_raise(rl_sched_of(gs.k), 0);
        return rl_sync(c, mem, 0x3100);
    }
    return RL_OK;
}

TEST(sched_sync_run_from_keeps_the_host_stack_flat_across_redirects)
{
    struct rl_thread *t;
    rl_sched *s;
    CHECK_EQ(boot_redirect(&t, &s, 0), 0);
    rl_fn_entry e[] = {{0x2000, 4, 0, irq_handler}, {0x3000, 4, 0, switch_fn}};
    rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(struct rl_ctx);
    u.fn_count = 2;
    u.fns = e;
    u.name = "switch";
    const rl_unit *list[] = {&u};
    rl_units_reset();
    CHECK_EQ(rl_units_register(list, 1), 0);
    rd.returns_to = 0x3000;
    stack_lo = stack_hi = 0;
    CHECK_EQ(rl_run_from(t->ctx, NULL, 0x3000), RL_OK);
    CHECK_EQ(rd.landed, 1001);
    CHECK_EQ(gs.handler_runs, 1000);
    CHECK_EQ(rl_sched_get_stats(s)->redirects, 1000);
    CHECK_EQ(rl_common(t->ctx)->pend.kind, RL_PEND_NONE);
    CHECK(stack_hi - stack_lo < 256);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

static uint64_t sync_block(rl_ctx *c, uint8_t *mem)
{
    rl_sched_raise(rl_sched_of(gs.k), 0);
    return rl_sync(c, mem, 0x1100);
}

TEST(sched_sync_run_from_restarts_at_the_redirect_target)
{
    struct rl_thread *t;
    rl_sched *s;
    CHECK_EQ(boot_redirect(&t, &s, 0), 0);
    rl_fn_entry e[] = {{0x1000, 4, 0, sync_block}, {0x2000, 4, 0, irq_handler}, {0x3000, 4, 0, task_fn}};
    rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(struct rl_ctx);
    u.fn_count = 3;
    u.fns = e;
    u.name = "blocks";
    const rl_unit *list[] = {&u};
    rl_units_reset();
    CHECK_EQ(rl_units_register(list, 1), 0);
    rd.returns_to = 0x3000;
    CHECK_EQ(rl_run_from(t->ctx, NULL, 0x1000), RL_OK);
    CHECK_EQ(gs.handler_runs, 1);
    CHECK_EQ(rd.landed, 1);
    CHECK_EQ(t->ctx->r[1], 77);
    CHECK_EQ(rl_common(t->ctx)->pend.kind, RL_PEND_NONE);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sched_set_irq_mask_from_the_cpu_layer_gates_the_entry)
{
    struct rl_thread *t;
    rl_sched *s;
    CHECK_EQ(boot_redirect(&t, &s, 0), 0);
    rd.returns_to = 0x300;
    rl_set_irq_mask(t->ctx, 0);
    rl_sched_raise(s, 0);
    CHECK_EQ(rl_sync(t->ctx, NULL, 0x300), RL_OK);
    CHECK_EQ(gs.handler_runs, 0);
    rl_set_irq_mask(t->ctx, 1);
    CHECK(rl_ctx_time(t->ctx)->budget <= 0);
    CHECK_EQ(rl_sync(t->ctx, NULL, 0x300), RL_OK);
    CHECK_EQ(gs.handler_runs, 1);
    rl_set_irq_mask(t->ctx, 0);
    rl_sched_destroy(s);
    rl_thread_free(t);
}

TEST(sched_core_init_checks_the_clock_offset)
{
    rl_cpu_desc bad = clock_cpu;
    bad.time_offset = 3;
    CHECK_EQ(rl_core_init(&clock_guest, &bad), -1);
    bad.time_offset = (uint32_t)sizeof(struct rl_ctx) - 8;
    CHECK_EQ(rl_core_init(&clock_guest, &bad), -1);
    CHECK_EQ(rl_core_init(&clock_guest, &clock_cpu), 0);
    struct rl_thread *t = rl_thread_alloc();
    CHECK(rl_ctx_time(t->ctx) == &t->ctx->time);
    rl_thread_free(t);
}

static int preempt_calls;

static void count_preempt(rl_ctx *c)
{
    preempt_calls++;
    rl_preempt_clear(c);
}

TEST(sched_preempt_runs_the_hook_or_clears_the_budget)
{
    CHECK_EQ(rl_core_init(&clock_guest, &clock_cpu), 0);
    struct rl_thread *t = rl_thread_alloc();
    rl_preempt_clear(t->ctx);
    CHECK(!rl_time_spent(rl_ctx_time(t->ctx)));
    rl_preempt_request(t->ctx);
    CHECK(rl_time_spent(rl_ctx_time(t->ctx)));
    preempt_calls = 0;
    rl_set_preempt_hook(count_preempt);
    rl_preempt(t->ctx);
    CHECK_EQ(preempt_calls, 1);
    CHECK(!rl_time_spent(rl_ctx_time(t->ctx)));
    rl_set_preempt_hook(NULL);
    rl_preempt_request(t->ctx);
    rl_preempt(t->ctx);
    CHECK_EQ(preempt_calls, 1);
    CHECK(!rl_time_spent(rl_ctx_time(t->ctx)));
    rl_thread_free(t);
}
