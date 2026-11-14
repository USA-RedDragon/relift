/**
 * @file fibers.c
 * @brief Two fibers for the gdb fiber commands test: guest CPU 1 parks
 * inside guest::main::deep(int) twelve frames down, then guest CPU 0 runs
 * guest::main::foo(), where the test stops.
 */
#include "fiber.h"

#include <stdio.h>

int _ZN5guest4main3fooEv(void);
int _ZN5guest4main4deepEi(int n);

static rl_fiber g_home, g_a, g_b;

/** @brief The frame the test stops in, on guest CPU 0. */
__attribute__((noinline)) int _ZN5guest4main3fooEv(void)
{
    static volatile int calls;
    return ++calls;
}

/** @brief Recurses n frames deep, then parks guest CPU 1. */
__attribute__((noinline)) int _ZN5guest4main4deepEi(int n)
{
    if (n == 0) {
        rl_fiber_switch(&g_b, &g_home);
        return 0;
    }
    return _ZN5guest4main4deepEi(n - 1) + 1;
}

static void cpu1(void *arg)
{
    (void)arg;
    _ZN5guest4main4deepEi(11);
    for (;;)
        rl_fiber_switch(&g_b, &g_home);
}

static void cpu0(void *arg)
{
    (void)arg;
    printf("%d\n", _ZN5guest4main3fooEv());
    for (;;)
        rl_fiber_switch(&g_a, &g_home);
}

int main(void)
{
    static rl_fiber idle;
    if (rl_fiber_init(&g_a, 0, cpu0, NULL) || rl_fiber_init(&g_b, 0, cpu1, NULL) ||
        rl_fiber_init(&idle, 0, cpu0, NULL))
        return 1;
    snprintf(g_a.label, sizeof g_a.label, "guest CPU 0");
    snprintf(g_b.label, sizeof g_b.label, "guest CPU 1");
    snprintf(idle.label, sizeof idle.label, "guest CPU 2");
    rl_fiber_switch(&g_home, &g_b);
    rl_fiber_switch(&g_home, &g_a);
    return 0;
}
