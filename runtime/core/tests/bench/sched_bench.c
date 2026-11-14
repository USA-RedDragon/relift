/**
 * @file sched_bench.c
 * @brief Microbenchmark of the device scheduler (make sched-bench): the
 * budget check of lifted code, the scheduler call, event firing, device
 * catch-up on access, rl_sync through a context and the hand-over between two
 * CPUs on two host threads. Prints nanoseconds per operation.
 */
#include "core.h"

#include <relift/sched.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/** @brief Blocks per budget run. */
#define BLOCKS 200000000ll
/** @brief Operations per slow-path run. */
#define OPS 20000000ll

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static void report(const char *what, double secs, long long n)
{
    printf("%-44s %8.2f ns\n", what, secs * 1e9 / (double)n);
}

/** @brief A lifted-code stand-in: per block one add of work and, when checked, the budget step; the budget lives in a register. */
static int64_t blocks(rl_sched_cpu *k, long long n, int64_t cost, int check, uint64_t *work)
{
    rl_time *t = rl_sched_clock(k);
    int64_t budget = t->budget;
    uint64_t w = *work;
    for (long long i = 0; i < n; i++) {
        w = w * 6364136223846793005ull + 1;
        __asm__ volatile("" : "+r"(w));
        if (check) {
            budget -= cost;
            __asm__ volatile("" : "+r"(budget));
            if (RL_UNLIKELY(budget <= 0)) {
                t->budget = budget;
                rl_sched_run(k);
                budget = t->budget;
            }
        }
    }
    t->budget = budget;
    *work = w;
    return budget;
}

static void bench_budget(void)
{
    uint64_t w = 1;
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    rl_sched_run(k);
    blocks(k, BLOCKS / 10, 4, 1, &w);
    double t0 = now_s();
    blocks(k, BLOCKS, 4, 0, &w);
    double t1 = now_s();
    blocks(k, BLOCKS, 4, 1, &w);
    double t2 = now_s();
    report("block, no budget check (baseline)", t1 - t0, BLOCKS);
    report("block, budget check", t2 - t1, BLOCKS);
    printf("%-44s %8.2f ns  (%llu scheduler runs)\n", "budget check alone", ((t2 - t1) - (t1 - t0)) * 1e9 / BLOCKS,
           (unsigned long long)rl_sched_get_stats(s)->runs);
    rl_sched_destroy(s);
}

static void bench_run(int queued)
{
    rl_sched_config cfg = {.slice = 1};
    rl_sched *s = rl_sched_create(&cfg);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    rl_event *ev = calloc((size_t)queued + 1, sizeof *ev);
    for (int i = 0; i < queued; i++)
        rl_sched_post(s, &ev[i], UINT64_MAX - (uint64_t)i);
    rl_time *t = rl_sched_clock(k);
    double t0 = now_s();
    for (long long i = 0; i < OPS; i++) {
        t->budget -= 1;
        rl_sched_run(k);
    }
    double t1 = now_s();
    char what[64];
    snprintf(what, sizeof what, "rl_sched_run, nothing due, %d queued", queued);
    report(what, t1 - t0, OPS);
    free(ev);
    rl_sched_destroy(s);
}

static void tick_fire(rl_sched *s, rl_event *e, uint64_t when) { rl_sched_post(s, e, when + 1); }

static void bench_events(int queued)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    rl_event *ev = calloc((size_t)queued + 1, sizeof *ev);
    for (int i = 0; i < queued; i++)
        rl_sched_post(s, &ev[i], UINT64_MAX - (uint64_t)i);
    rl_event e = {.fire = tick_fire};
    rl_sched_post(s, &e, 1);
    rl_sched_run(k);
    rl_time *t = rl_sched_clock(k);
    double t0 = now_s();
    for (long long i = 0; i < OPS; i++) {
        t->budget -= 1;
        if (t->budget <= 0)
            rl_sched_run(k);
    }
    double t1 = now_s();
    char what[64];
    snprintf(what, sizeof what, "event fire + repost + run, %d queued", queued);
    report(what, t1 - t0, (long long)rl_sched_get_stats(s)->events);
    free(ev);
    rl_sched_destroy(s);
}

static void count_up(rl_device *d, uint64_t from, uint64_t to) { *(uint64_t *)d->opaque += to - from; }

static uint64_t count_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now)
{
    (void)addr;
    (void)size;
    (void)now;
    return *(uint64_t *)d->opaque;
}

static void bench_access(int observers, int ranges)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    uint64_t cnt[8] = {0};
    rl_device d[8];
    memset(d, 0, sizeof d);
    uint64_t obs = 0;
    for (int i = 0; i <= observers; i++) {
        d[i].name = "bench";
        d[i].catch_up = count_up;
        d[i].read = count_read;
        d[i].opaque = &cnt[i];
        d[i].line = RL_LINE_NONE;
        rl_sched_add_device(s, &d[i]);
        if (i)
            obs |= rl_device_bit(&d[i]);
    }
    for (int r = 0; r < ranges; r++)
        rl_sched_map(s, 0x1000 + (uint64_t)r * 0x10, 0x10, &d[0], obs);
    rl_sched_run(k);
    rl_time *t = rl_sched_clock(k);
    uint64_t sum = 0;
    double t0 = now_s();
    for (long long i = 0; i < OPS; i++) {
        t->budget -= 4;
        sum += rl_sched_read(k, 0x1000 + (uint64_t)(i % ranges) * 0x10, 1);
    }
    double t1 = now_s();
    char what[64];
    snprintf(what, sizeof what, "read: catch-up %d device(s), %d range(s)", observers + 1, ranges);
    report(what, t1 - t0, OPS);
    if (sum == 42)
        puts("");
    rl_sched_destroy(s);
}

/** @brief A context with a clock for the rl_sync benchmark. */
struct rl_ctx {
    /** @brief The core's block. */
    rl_ctx_common k;
    /** @brief The clock. */
    rl_time time;
};

static uint64_t no_irq(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    (void)c;
    (void)mem;
    (void)next;
    return RL_NO_IRQ;
}

static void bench_sync(void)
{
    static const rl_cpu_desc cpu = {
        .name = "bench",
        .ctx_size = sizeof(struct rl_ctx),
        .ctx_align = 8,
        .common_offset = offsetof(struct rl_ctx, k),
        .time_offset = offsetof(struct rl_ctx, time),
    };
    static const rl_guest_desc guest = {.name = "bench", .page_size = 0x1000, .address_bits = 16, .tick_hz = 1000000};
    if (rl_core_init(&guest, &cpu) != 0)
        return;
    struct rl_thread *th = rl_thread_alloc();
    rl_sched_config cfg = {.slice = 1};
    rl_sched *s = rl_sched_create(&cfg);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    rl_sched_attach(k, th);
    rl_sched_set_irq(k, no_irq);
    rl_ctx *c = th->ctx;
    double t0 = now_s();
    for (long long i = 0; i < OPS; i++) {
        c->time.budget -= 1;
        rl_sync(c, NULL, 0x100);
    }
    double t1 = now_s();
    report("rl_sync through a context, no interrupt", t1 - t0, OPS);
    rl_sched_destroy(s);
    rl_thread_free(th);
}

/** @brief Blocks per CPU in the hand-over benchmark. */
#define SWITCH_BLOCKS 2000000ll

static void *switch_main(void *p)
{
    rl_sched_cpu *k = p;
    rl_sched_enter(k);
    rl_time *t = rl_sched_clock(k);
    rl_sched_run(k);
    for (long long i = 0; i < SWITCH_BLOCKS; i++) {
        t->budget -= 4;
        if (t->budget <= 0)
            rl_sched_run(k);
    }
    rl_sched_leave(k);
    return NULL;
}

static void bench_switch(uint64_t quantum)
{
    rl_sched_config cfg = {.quantum = quantum};
    rl_sched *s = rl_sched_create(&cfg);
    rl_sched_cpu *a = rl_sched_add_cpu(s, 1);
    rl_sched_cpu *b = rl_sched_add_cpu(s, 1);
    pthread_t ta, tb;
    double t0 = now_s();
    pthread_create(&tb, NULL, switch_main, b);
    pthread_create(&ta, NULL, switch_main, a);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    double t1 = now_s();
    unsigned long long sw = (unsigned long long)rl_sched_get_stats(s)->switches;
    char what[64];
    snprintf(what, sizeof what, "hand-over between 2 CPUs, quantum %llu", (unsigned long long)quantum);
    printf("%-44s %8.2f ns  (%llu hand-overs, %.2f ns per block)\n", what, (t1 - t0) * 1e9 / (double)sw, sw,
           (t1 - t0) * 1e9 / (double)(2 * SWITCH_BLOCKS));
    rl_sched_destroy(s);
}

int main(void)
{
    bench_budget();
    bench_run(0);
    bench_run(16);
    bench_events(0);
    bench_events(64);
    bench_access(0, 1);
    bench_access(3, 1);
    bench_access(0, 8);
    bench_sync();
    bench_switch(64);
    bench_switch(4096);
    return 0;
}
