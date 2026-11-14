/**
 * @file fiber_tests.c
 * @brief The fibers and the scheduler on them, in a test program that links
 * only those (and the crash handler), so it builds and runs on Windows too:
 * register preservation across switches, a ring of fibers, setjmp and
 * longjmp inside a fiber, the Windows stack fields of the TEB, the guard
 * page lookup, the FP mode check, and two guest CPUs handing the run back
 * and forth. Cases that must end the process run in a child.
 */
#include "child.h"
#include "fiber.h"

#include <relift/sched.h>

#include <fenv.h>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failed;
static int g_checks;

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failed++;                                                                                                \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                            \
        }                                                                                                              \
    } while (0)

static rl_fiber g_home, g_a, g_b;

typedef struct mix {
    uint64_t i[8];
    double d[8];
} mix;

__attribute__((noinline)) static void mix_step(mix *m, uint64_t k)
{
    for (int j = 0; j < 8; j++) {
        m->i[j] = m->i[j] * 6364136223846793005ull + k + (uint64_t)j;
        m->d[j] = m->d[j] * 0.5 + (double)(k + (uint64_t)j);
    }
}

static mix g_mix_a, g_mix_b;

typedef void switch_fn(rl_fiber *from, rl_fiber *to);

static void real_switch(rl_fiber *from, rl_fiber *to) { rl_fiber_switch(from, to); }

static void no_switch(rl_fiber *from, rl_fiber *to)
{
    (void)from;
    (void)to;
}

static void pressure(mix *out, uint64_t seed, switch_fn *sw, rl_fiber *self, rl_fiber *other)
{
    uint64_t i0 = seed, i1 = seed + 1, i2 = seed + 2, i3 = seed + 3, i4 = seed + 4, i5 = seed + 5;
    double d0 = (double)seed, d1 = d0 + 1, d2 = d0 + 2, d3 = d0 + 3, d4 = d0 + 4, d5 = d0 + 5, d6 = d0 + 6,
           d7 = d0 + 7;
    for (uint64_t k = 0; k < 1000; k++) {
        sw(self, other);
        i0 = i0 * 3 + k;
        i1 ^= i0;
        i2 += i1;
        i3 = i3 * 5 + i2;
        i4 ^= i3 >> 3;
        i5 += i4;
        d0 = d0 * 0.5 + 1;
        d1 = d1 * 0.25 + d0;
        d2 += d1 * 0.125;
        d3 = d3 * 0.5 + d2;
        d4 = d4 * 0.75 + d3;
        d5 += d4 * 0.0625;
        d6 = d6 * 0.5 + d5;
        d7 = d7 * 0.5 + d6;
    }
    uint64_t iv[6] = {i0, i1, i2, i3, i4, i5};
    double dv[8] = {d0, d1, d2, d3, d4, d5, d6, d7};
    memset(out, 0, sizeof *out);
    memcpy(out->i, iv, sizeof iv);
    memcpy(out->d, dv, sizeof dv);
}

static void pressure_a(void *arg)
{
    (void)arg;
    pressure(&g_mix_a, 11, real_switch, &g_a, &g_b);
    for (;;)
        rl_fiber_switch(&g_a, &g_b);
}

static void pressure_b(void *arg)
{
    (void)arg;
    pressure(&g_mix_b, 23, real_switch, &g_b, &g_a);
    for (;;)
        rl_fiber_switch(&g_b, &g_home);
}

static void test_registers_survive_switches(void)
{
    CHECK(rl_fiber_init(&g_a, 0, pressure_a, NULL) == 0);
    CHECK(rl_fiber_init(&g_b, 0, pressure_b, NULL) == 0);
    rl_fiber_switch(&g_home, &g_a);
    mix ra, rb;
    pressure(&ra, 11, no_switch, NULL, NULL);
    pressure(&rb, 23, no_switch, NULL, NULL);
    CHECK(memcmp(&ra, &g_mix_a, sizeof ra) == 0);
    CHECK(memcmp(&rb, &g_mix_b, sizeof rb) == 0);
    rl_fiber_free(&g_a);
    rl_fiber_free(&g_b);
}

enum { RING = 8, LAPS = 1000 };
static rl_fiber g_ring[RING];
static int g_ring_count[RING];
static mix g_ring_mix[RING];

static void ring_main(void *arg)
{
    int me = (int)(intptr_t)arg;
    mix m;
    for (int j = 0; j < 8; j++) {
        m.i[j] = (uint64_t)me;
        m.d[j] = me;
    }
    for (int lap = 0; lap < LAPS; lap++) {
        mix_step(&m, (uint64_t)lap);
        g_ring_count[me]++;
        rl_fiber_switch(&g_ring[me], me == RING - 1 ? &g_home : &g_ring[me + 1]);
    }
    g_ring_mix[me] = m;
    for (;;)
        rl_fiber_switch(&g_ring[me], me == RING - 1 ? &g_home : &g_ring[me + 1]);
}

static void test_a_ring_of_fibers(void)
{
    for (int i = 0; i < RING; i++)
        CHECK(rl_fiber_init(&g_ring[i], 64u << 10, ring_main, (void *)(intptr_t)i) == 0);
    for (int lap = 0; lap <= LAPS; lap++)
        rl_fiber_switch(&g_home, &g_ring[0]);
    for (int i = 0; i < RING; i++) {
        mix m;
        for (int j = 0; j < 8; j++) {
            m.i[j] = (uint64_t)i;
            m.d[j] = i;
        }
        for (int lap = 0; lap < LAPS; lap++)
            mix_step(&m, (uint64_t)lap);
        CHECK(g_ring_count[i] == LAPS);
        CHECK(memcmp(&m, &g_ring_mix[i], sizeof m) == 0);
        rl_fiber_free(&g_ring[i]);
    }
}

static jmp_buf g_jb;
static int g_jumps;

__attribute__((noinline)) static void jump_back(int v)
{
    rl_fiber_switch(&g_a, &g_b);
    longjmp(g_jb, v);
}

static void jumping_main(void *arg)
{
    (void)arg;
    volatile int round = 0;
    if (setjmp(g_jb) < 3) {
        round++;
        jump_back(round);
    }
    g_jumps = round;
    rl_fiber_switch(&g_a, &g_home);
}

static void bouncing_main(void *arg)
{
    (void)arg;
    for (;;)
        rl_fiber_switch(&g_b, &g_a);
}

static void test_longjmp_inside_a_fiber(void)
{
    CHECK(rl_fiber_init(&g_a, 0, jumping_main, NULL) == 0);
    CHECK(rl_fiber_init(&g_b, 0, bouncing_main, NULL) == 0);
    rl_fiber_switch(&g_home, &g_a);
    CHECK(g_jumps == 3);
    rl_fiber_free(&g_a);
    rl_fiber_free(&g_b);
}

#if defined(_WIN32)
static void *g_teb_seen[3];

__attribute__((noinline)) static void grow(void)
{
    volatile uint8_t big[128u << 10];
    for (size_t i = 0; i < sizeof big; i += 512)
        big[i] = (uint8_t)i;
}

static void teb_main(void *arg)
{
    (void)arg;
    volatile NT_TIB *tib = (volatile NT_TIB *)NtCurrentTeb();
    g_teb_seen[0] = tib->StackBase;
    g_teb_seen[1] = tib->StackLimit;
    grow();
    g_teb_seen[2] = tib->StackLimit;
    rl_fiber_switch(&g_a, &g_home);
}

static void test_windows_stack_fields_follow_the_fiber(void)
{
    NT_TIB *tib = (NT_TIB *)NtCurrentTeb();
    void *base = tib->StackBase, *limit = tib->StackLimit;
    CHECK(rl_fiber_init(&g_a, 1u << 20, teb_main, NULL) == 0);
    rl_fiber_switch(&g_home, &g_a);
    CHECK(g_teb_seen[0] == (void *)g_a.top);
    CHECK((uint8_t *)g_teb_seen[1] >= g_a.limit && (uint8_t *)g_teb_seen[1] < g_a.top);
    CHECK((uint8_t *)g_teb_seen[2] < (uint8_t *)g_teb_seen[1]);
    CHECK(tib->StackBase == base && tib->StackLimit == limit);
    rl_fiber_free(&g_a);
}
#endif

static uintptr_t g_local_addr;

static void where_main(void *arg)
{
    (void)arg;
    volatile int here = 0;
    g_local_addr = (uintptr_t)&here;
    rl_fiber_switch(&g_a, &g_home);
}

static void test_fibers_are_found_by_address(void)
{
    CHECK(rl_fiber_init(&g_a, 0, where_main, NULL) == 0);
    snprintf(g_a.label, sizeof g_a.label, "guest CPU 5");
    rl_fiber_switch(&g_home, &g_a);
    bool guard = true;
    const rl_fiber *f = rl_fiber_find(g_local_addr, &guard);
    CHECK(f == &g_a && !guard);
    f = rl_fiber_find((uintptr_t)g_a.alloc, &guard);
    CHECK(f == &g_a && guard);
    CHECK(rl_fiber_find((uintptr_t)&guard, NULL) == NULL);
    int listed = 0;
    for (uint32_t i = 0; i < RL_FIBER_SLOTS; i++)
        listed += rl_fiber_live[i] == &g_a;
    CHECK(listed == 1);
    rl_fiber_free(&g_a);
    CHECK(rl_fiber_find(g_local_addr, NULL) == NULL);
}

static void test_fp_mode_check(void)
{
    CHECK(rl_fiber_fp_mode_diff() == 0);
    volatile double x = 1.0, y = 3.0;
    volatile double q = x / y;
    (void)q;
    CHECK(rl_fiber_fp_mode_diff() == 0);
    CHECK(fesetround(FE_UPWARD) == 0);
    CHECK(rl_fiber_fp_mode_diff() != 0);
    CHECK(fesetround(FE_TONEAREST) == 0);
    CHECK(rl_fiber_fp_mode_diff() == 0);
}

typedef struct cpu_run {
    int64_t cost;
    int blocks;
    int at;
} cpu_run;

static uint32_t g_trace[8192];
static int g_ntrace;
static rl_sched_cpu *g_k[2];

static void trace_cpu(rl_sched_cpu *k, void *arg)
{
    cpu_run *r = arg;
    rl_time *t = rl_sched_clock(k);
    rl_sched_run(k);
    for (int i = 0; i < r->blocks; i++) {
        t->budget -= r->cost;
        if (t->budget <= 0)
            rl_sched_run(k);
        if (g_ntrace < 8192 && rl_sched_current(rl_sched_of(k)) == k)
            g_trace[g_ntrace++] = (uint32_t)((k == g_k[0] ? 0u : 1u) << 31 | (uint32_t)rl_sched_now(k));
        r->at = i;
    }
}

static int two_cpus(uint64_t quantum, int home_cpu)
{
    rl_sched_config cfg = {.quantum = quantum, .stack = 256u << 10};
    rl_sched *s = rl_sched_create(&cfg);
    g_k[0] = rl_sched_add_cpu(s, 1);
    g_k[1] = rl_sched_add_cpu(s, 2);
    g_ntrace = 0;
    cpu_run r[2] = {{3, 1500, 0}, {5, 1000, 0}};
    int rc = rl_sched_spawn(g_k[1], trace_cpu, &r[1]);
    if (home_cpu) {
        trace_cpu(g_k[0], &r[0]);
        rl_sched_leave(g_k[0]);
    } else {
        rc |= rl_sched_spawn(g_k[0], trace_cpu, &r[0]);
        rc |= rl_sched_main(s);
    }
    if (r[0].at != 1499 || r[1].at != 999 || rl_sched_current(s) != NULL)
        rc = -1;
    rl_sched_destroy(s);
    return rc;
}

static void test_two_cpus_hand_over_on_fibers(void)
{
    CHECK(two_cpus(64, 0) == 0);
    static uint32_t first[8192];
    int n = g_ntrace;
    memcpy(first, g_trace, sizeof first);
    CHECK(n > 2000);
    CHECK(two_cpus(64, 0) == 0);
    CHECK(g_ntrace == n && memcmp(first, g_trace, sizeof first) == 0);
    CHECK(two_cpus(64, 1) == 0);
    CHECK(g_ntrace == n && memcmp(first, g_trace, sizeof first) == 0);
}

static int g_inner_rc;

static void main_from_fiber(rl_sched_cpu *k, void *arg)
{
    (void)arg;
    g_inner_rc = rl_sched_main(rl_sched_of(k));
}

static void test_spawn_and_main_refuse_misuse(void)
{
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *a = rl_sched_add_cpu(s, 1);
    rl_sched_cpu *b = rl_sched_add_cpu(s, 1);
    CHECK(rl_sched_spawn(a, NULL, NULL) == -1);
    CHECK(rl_sched_spawn(a, main_from_fiber, NULL) == 0);
    CHECK(rl_sched_spawn(a, main_from_fiber, NULL) == -1);
    CHECK(rl_sched_main(s) == -1);
    CHECK(rl_sched_spawn(b, main_from_fiber, NULL) == 0);
    CHECK(rl_sched_main(s) == 0);
    CHECK(g_inner_rc == -1);
    CHECK(rl_sched_spawn(a, main_from_fiber, NULL) == -1);
    CHECK(rl_sched_main(s) == 0);
    rl_sched_destroy(s);
}

static void spin_upward(rl_sched_cpu *k, void *arg)
{
    (void)arg;
    rl_time *t = rl_sched_clock(k);
    rl_sched_run(k);
    if (k == g_k[1])
        fesetround(FE_UPWARD);
    for (int i = 0; i < 1000; i++) {
        t->budget -= 4;
        if (t->budget <= 0)
            rl_sched_run(k);
    }
}

static int child(const char *mode)
{
    if (!strcmp(mode, "fp-handover")) {
        rl_sched_config cfg = {.quantum = 16};
        rl_sched *s = rl_sched_create(&cfg);
        g_k[0] = rl_sched_add_cpu(s, 1);
        g_k[1] = rl_sched_add_cpu(s, 1);
        rl_sched_spawn(g_k[0], spin_upward, NULL);
        rl_sched_spawn(g_k[1], spin_upward, NULL);
        rl_sched_main(s);
        fprintf(stderr, "no abort\n");
        return 4;
    }
    return 2;
}

static void test_a_hand_over_checks_the_fp_mode(const char *self)
{
    child_run *r = calloc(1, sizeof *r);
    CHECK(r && child_spawn(self, "fp-handover", r) == 0);
    if (!r)
        return;
#if !defined(RL_CHECKS)
    CHECK(!child_aborted(r) && strstr(r->out, "no abort") != NULL);
#else
    CHECK(child_aborted(r));
    CHECK(strstr(r->out, "[relift sched] host FP control differs from the default") != NULL);
#endif
    free(r);
}

int main(int argc, char **argv)
{
    if (argc >= 3 && !strcmp(argv[1], "child"))
        return child(argv[2]);
    rl_fiber_thread_init();
    test_registers_survive_switches();
    test_a_ring_of_fibers();
    test_longjmp_inside_a_fiber();
#if defined(_WIN32)
    test_windows_stack_fields_follow_the_fiber();
#endif
    test_fibers_are_found_by_address();
    test_fp_mode_check();
    test_two_cpus_hand_over_on_fibers();
    test_spawn_and_main_refuse_misuse();
    test_a_hand_over_checks_the_fp_mode(argv[0]);
    printf("fiber_tests: %d checks, %d failed\n", g_checks, g_failed);
    return g_failed ? 1 : 0;
}
