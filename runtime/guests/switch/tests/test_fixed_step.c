/**
 * @file test_fixed_step.c
 * @brief Fixed-step mode (clock.fixed_step): the guest clock moves only with
 * the guest's own progress, waits and sleeps end on it, a lone yield moves it
 * to the next deadline, timers run in deadline order, guest threads run one
 * at a time in a fixed order, and the entropy is a fixed sequence. Guest
 * thread entries are host functions with the lifted signature; they pass
 * preemption points the way lifted code does.
 */
#include "services.h"
#include "t.h"

#include <relift/settings.h>

#include <stdlib.h>

#define POINTS_PER_NOTE 1024
#define SLICES 3
/** @brief Iterations between the host yields inside the read-modify-write; a yield costs a host time slice on a loaded machine. */
#define YIELD_EVERY 64

static struct {
    uint32_t counter;
    char trace[64];
    int ntrace;
    int flag;
    uint64_t fired[16];
    char fired_by[16];
    int nfired;
} S;

static rl_ctx *boot_fixed(uint64_t *heap)
{
    setenv("RELIFT_FIXED_STEP", "1", 1);
    rl_switch_declare_settings();
    rl_ctx *c = t_boot(NULL);
    *heap = t_heap(c, 0x200000);
    memset(&S, 0, sizeof S);
    return c;
}

static uint64_t now_ns(rl_ctx *c)
{
    t_svc(c, SVC_GET_SYSTEM_TICK, 0, 0, 0, 0, 0, 0);
    return rl_ticks_to_ns(c->x[0]);
}

static void point(rl_ctx *c)
{
    if (rl_time_spent(&c->time))
        rl_preempt(c);
}

static uint64_t w_count(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    char me = (char)('a' + c->x[0]);
    for (uint32_t k = 0; k < SLICES * HOS_FS_SLICE; k++) {
        uint32_t v = __atomic_load_n(&S.counter, __ATOMIC_RELAXED);
        if (k % POINTS_PER_NOTE == 0 && S.ntrace < (int)sizeof S.trace - 1)
            S.trace[S.ntrace++] = me;
        if (k % YIELD_EVERY == 0)
            rl_host_yield();
        __atomic_store_n(&S.counter, v + 1, __ATOMIC_RELAXED);
        point(c);
    }
    return RL_OK;
}

static uint64_t w_sleep_flag(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 3000000;
    rl_svc(c, rl_mem_base(), SVC_SLEEP_THREAD);
    __atomic_store_n(&S.flag, 1, __ATOMIC_SEQ_CST);
    return RL_OK;
}

static uint32_t spawn(rl_ctx *c, uint64_t heap, uint32_t fn, int i, int32_t core)
{
    uint64_t stack = heap + 0x100000 + 0x10000u * (unsigned)i;
    if (t_svc(c, SVC_CREATE_THREAD, 0, t_code(fn), (uint64_t)i, stack, 44, (uint64_t)(int64_t)core) != 0)
        return 0;
    uint32_t h = (uint32_t)c->x[1];
    t_svc(c, SVC_START_THREAD, h, 0, 0, 0, 0, 0);
    return h;
}

static uint32_t join(rl_ctx *c, uint64_t heap, uint32_t h)
{
    *(uint32_t *)(void *)(t_mem() + heap + 0x800) = h;
    return t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, (uint64_t)-1, 0, 0);
}

static void setup_fns(void)
{
    static rl_fn const fns[2] = { w_count, w_sleep_flag };
    uint64_t addrs[2] = { t_code(0), t_code(1) };
    t_guest_fns(addrs, fns, 2);
}

TEST(the_guest_clock_moves_only_with_the_guest)
{
    uint64_t heap;
    rl_ctx *c = boot_fixed(&heap);
    CHECK(hos_fixed());
    uint64_t a = now_ns(c);
    rl_sleep_ns(20000000);
    uint64_t b = now_ns(c);
    CHECK(b - a < 1000);
    t_sleep(10000000);
    uint64_t d = now_ns(c);
    CHECK(d - b >= 10000000);
    CHECK(d - b < 10001000);
}

TEST(a_wait_times_out_on_the_guest_clock)
{
    uint64_t heap;
    rl_ctx *c = boot_fixed(&heap);
    CHECK_EQ(t_svc(c, SVC_CREATE_EVENT, 0, 0, 0, 0, 0, 0), 0);
    *(uint32_t *)(void *)(t_mem() + heap + 0x900) = (uint32_t)c->x[2];
    uint64_t a = now_ns(c);
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x900, 1, 5000000, 0, 0), 0xEA01u);
    uint64_t b = now_ns(c);
    CHECK(b - a >= 5000000);
    CHECK(b - a < 5001000);
}

TEST(a_lone_yield_moves_the_clock_to_the_next_deadline)
{
    uint64_t heap;
    rl_ctx *c = boot_fixed(&heap);
    setup_fns();
    uint64_t a = now_ns(c);
    uint32_t h = spawn(c, heap, 1, 0, 1);
    int yields = 0;
    while (!__atomic_load_n(&S.flag, __ATOMIC_SEQ_CST) && yields < 100000) {
        t_svc(c, SVC_SLEEP_THREAD, 0, 0, 0, 0, 0, 0);
        yields++;
    }
    CHECK_EQ(S.flag, 1);
    CHECK(yields < 10);
    uint64_t b = now_ns(c);
    CHECK(b - a >= 3000000);
    CHECK(b - a < 3001000);
    CHECK_EQ(join(c, heap, h), 0);
}

static void on_timer(void *ctx)
{
    if (S.nfired < 16) {
        S.fired[S.nfired] = hos_clock_ns();
        S.fired_by[S.nfired++] = *(const char *)ctx;
    }
}

TEST(timers_run_in_deadline_order_on_the_guest_clock)
{
    uint64_t heap;
    rl_ctx *c = boot_fixed(&heap);
    static const char a = 'a', b = 'b';
    uint64_t t0 = hos_clock_ns();
    void *ta = hos_fs_every(t0 + 5000000, 5000000, on_timer, (void *)&a);
    void *tb = hos_fs_every(t0 + 10000000, 7000000, on_timer, (void *)&b);
    CHECK(ta && tb);
    t_sleep(20000000);
    hos_fs_cancel(ta);
    hos_fs_cancel(tb);
    CHECK_EQ(S.nfired, 6);
    const char *want = "aababa";
    const uint64_t at[6] = { 5, 10, 10, 15, 17, 20 };
    for (int i = 0; i < 6 && i < S.nfired; i++) {
        CHECK_EQ(S.fired_by[i], want[i]);
        CHECK_EQ(S.fired[i] - t0, at[i] * 1000000u);
    }
    (void)c;
}

TEST(guest_threads_run_one_at_a_time_in_a_fixed_order)
{
    uint64_t heap;
    rl_ctx *c = boot_fixed(&heap);
    setup_fns();
    uint32_t h[3];
    for (int i = 0; i < 3; i++)
        h[i] = spawn(c, heap, 0, i, i);
    for (int i = 0; i < 3; i++)
        CHECK_EQ(join(c, heap, h[i]), 0);
    CHECK_EQ(S.counter, 3u * SLICES * HOS_FS_SLICE);
    const int per = HOS_FS_SLICE / POINTS_PER_NOTE;
    CHECK_EQ(S.ntrace, 3 * SLICES * per);
    const char order[3] = { 'b', 'c', 'a' };
    for (int k = 0; k < S.ntrace; k++)
        CHECK_EQ(S.trace[k], order[(k / per) % 3]);
}

TEST(entropy_is_a_fixed_sequence)
{
    uint64_t heap;
    rl_ctx *c = boot_fixed(&heap);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 11, 0, 0, 0, 0), 0);
    uint64_t e0 = c->x[1];
    hos_fs_init();
    uint64_t again;
    hos_fs_random(&again, sizeof again);
    CHECK_EQ(e0, again);
    CHECK(e0 != 0);
}
