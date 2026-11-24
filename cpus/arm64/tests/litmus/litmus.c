/*
 * A64 litmus tests for the RCpc acquire load. Each test runs two threads on
 * fresh words, 200000 rounds, and counts the outcomes.
 *
 *   sb_pc   T0: STLR x=1; LDAPR r0=y    T1: STLR y=1; LDAPR r1=x
 *           r0=0 r1=0 is allowed: RCpc lets a load pass an earlier STLR to
 *           another address.
 *   sb_sc   the same with LDAR: r0=0 r1=0 is FORBIDDEN (RCsc).
 *   mp_pc   T0: STR data=1; STLR flag=1   T1: LDAPR f=flag; LDR d=data
 *           f=1 d=0 is FORBIDDEN (LDAPR is still an acquire).
 *
 * Natively on a CPU the counts are the hardware's allowed set; lifted, they
 * must contain no outcome the architecture forbids (the lines marked
 * FORBIDDEN stay at 0). The thread bodies are the guest's functions: the
 * lifted run calls them from a host harness (run.c) on host threads.
 */
#include <stdint.h>

volatile uint32_t lt_x, lt_y, lt_data, lt_flag;
volatile uint32_t lt_r0, lt_r1;

#define NOINLINE __attribute__((noinline))

NOINLINE void sb_pc_t0(void)
{
    uint32_t r;
    __asm__ volatile("mov w9, #1\n\tstlr w9, [%1]\n\tldapr %w0, [%2]" : "=&r"(r) : "r"(&lt_x), "r"(&lt_y) : "w9", "memory");
    lt_r0 = r;
}
NOINLINE void sb_pc_t1(void)
{
    uint32_t r;
    __asm__ volatile("mov w9, #1\n\tstlr w9, [%1]\n\tldapr %w0, [%2]" : "=&r"(r) : "r"(&lt_y), "r"(&lt_x) : "w9", "memory");
    lt_r1 = r;
}
NOINLINE void sb_sc_t0(void)
{
    uint32_t r;
    __asm__ volatile("mov w9, #1\n\tstlr w9, [%1]\n\tldar %w0, [%2]" : "=&r"(r) : "r"(&lt_x), "r"(&lt_y) : "w9", "memory");
    lt_r0 = r;
}
NOINLINE void sb_sc_t1(void)
{
    uint32_t r;
    __asm__ volatile("mov w9, #1\n\tstlr w9, [%1]\n\tldar %w0, [%2]" : "=&r"(r) : "r"(&lt_y), "r"(&lt_x) : "w9", "memory");
    lt_r1 = r;
}
NOINLINE void mp_pc_t0(void)
{
    __asm__ volatile("mov w9, #1\n\tstr w9, [%0]\n\tstlr w9, [%1]" : : "r"(&lt_data), "r"(&lt_flag) : "w9", "memory");
}
NOINLINE void mp_pc_t1(void)
{
    uint32_t f, d;
    __asm__ volatile("ldapr %w0, [%2]\n\tldr %w1, [%3]" : "=&r"(f), "=&r"(d) : "r"(&lt_flag), "r"(&lt_data) : "memory");
    lt_r0 = f;
    lt_r1 = d;
}

#ifndef LITMUS_NO_MAIN
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define ROUNDS 200000

static volatile int go[2], done[2];
static volatile int stop;
static void (*fn[2])(void);

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    for (;;) {
        while (!go[id]) {
            if (stop) return 0;
        }
        go[id] = 0;
        fn[id]();
        __atomic_store_n(&done[id], 1, __ATOMIC_RELEASE);
    }
}

static unsigned counts[4];

static void run(const char *name, void (*t0)(void), void (*t1)(void), int mp, const char *forbidden)
{
    fn[0] = t0;
    fn[1] = t1;
    counts[0] = counts[1] = counts[2] = counts[3] = 0;
    for (int i = 0; i < ROUNDS; i++) {
        lt_x = lt_y = lt_data = lt_flag = 0;
        lt_r0 = lt_r1 = 9;
        done[0] = done[1] = 0;
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        go[0] = go[1] = 1;
        while (!__atomic_load_n(&done[0], __ATOMIC_ACQUIRE) || !__atomic_load_n(&done[1], __ATOMIC_ACQUIRE)) {}
        counts[(lt_r0 & 1) * 2 + (lt_r1 & 1)]++;
    }
    printf("%s (%s):\n", name, mp ? "f, d" : "r0, r1");
    for (int k = 0; k < 4; k++)
        printf("  %d %d : %u%s\n", k >> 1, k & 1, counts[k], (forbidden && k == forbidden[0] - '0' && counts[k]) ? "  FORBIDDEN" : "");
    fflush(stdout);
}

int main(void)
{
    pthread_t t[2];
    for (long i = 0; i < 2; i++) pthread_create(&t[i], 0, worker, (void *)i);
    run("sb_pc", sb_pc_t0, sb_pc_t1, 0, 0);
    run("sb_sc", sb_sc_t0, sb_sc_t1, 0, "0");
    run("mp_pc", mp_pc_t0, mp_pc_t1, 1, "2");
    stop = 1;
    pthread_join(t[0], 0);
    pthread_join(t[1], 0);
    return 0;
}
#endif
