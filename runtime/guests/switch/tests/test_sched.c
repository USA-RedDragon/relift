/**
 * @file test_sched.c
 * @brief The scheduler: equal-priority threads of one core never interleave,
 * a higher-priority thread that becomes ready runs at the next svc and a
 * lower one waits, cores run in parallel within affinity masks, threads
 * migrate on svcSetThreadCoreMask and to idle cores, the three yields of
 * svcSleepThread, priority inheritance through a mutex, and a thread
 * blocked in a host wait leaves its core to the others. Guest thread entries
 * are host functions with the lifted signature.
 */
#include "t.h"

#include <stdlib.h>

#define HAS_LISTENERS 0x40000000u
#define SLEEP_FLAG_NS 10000000ull

static struct {
    uint32_t handles[8];
    int flag[8];
    int32_t cpu[8];
    int ok[8];
    uint32_t counter;
    char trace[64];
    int ntrace;
    uint64_t event;
    uint64_t mutex;
    int iters;
} S;

static uint32_t svc_c(rl_ctx *c, uint32_t imm, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    rl_svc(c, rl_mem_base(), imm);
    return (uint32_t)c->x[0];
}

static int32_t cpu_now(rl_ctx *c)
{
    svc_c(c, SVC_GET_CURRENT_PROCESSOR_NUMBER, 0, 0, 0, 0);
    return (int32_t)c->x[0];
}

static void note(char ch)
{
    int n = __atomic_fetch_add(&S.ntrace, 1, __ATOMIC_SEQ_CST);
    if (n < (int)sizeof S.trace - 1)
        S.trace[n] = ch;
}

static int get(int *p) { return __atomic_load_n(p, __ATOMIC_SEQ_CST); }

static void set(int *p, int v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }

static bool host_wait_flag(int *p)
{
    while (!get(p)) {
    }
    return true;
}

static int idx_of(rl_ctx *c) { return (int)c->x[0]; }

static int w_count(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    for (int k = 0; k < S.iters; k++) {
        uint32_t v = __atomic_load_n(&S.counter, __ATOMIC_RELAXED);
        if ((k & 1023) == 0) {
            rl_host_yield();
            note((char)('a' + i));
        }
        __atomic_store_n(&S.counter, v + 1, __ATOMIC_RELAXED);
    }
    return RL_OK;
}

static int w_flag(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    S.cpu[i] = cpu_now(c);
    set(&S.flag[i], 1);
    return RL_OK;
}

static int w_wait_event(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    S.ok[i] = svc_c(c, SVC_WAIT_SYNCHRONIZATION, 0, S.event, 1, (uint64_t)-1) == 0;
    set(&S.flag[i], 1);
    return RL_OK;
}

static int w_ping(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    set(&S.flag[i], 1);
    S.ok[i] = host_wait_flag(&S.flag[i + 1]);
    S.cpu[i] = cpu_now(c);
    return RL_OK;
}

static int w_pong(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    S.ok[i] = host_wait_flag(&S.flag[i - 1]);
    S.cpu[i] = cpu_now(c);
    set(&S.flag[i], 1);
    return RL_OK;
}

static int w_migrate(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    S.cpu[i] = cpu_now(c);
    S.ok[i] = svc_c(c, SVC_SET_THREAD_CORE_MASK, HOS_CUR_THREAD, 2, 0x4, 0) == 0;
    S.cpu[i + 1] = cpu_now(c);
    S.ok[i + 1] = svc_c(c, SVC_SET_THREAD_CORE_MASK, HOS_CUR_THREAD, (uint64_t)-3, 0x6, 0) == 0;
    S.cpu[i + 2] = cpu_now(c);
    return RL_OK;
}

static int w_yield(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    for (int k = 0; k < 3; k++) {
        note('w');
        svc_c(c, SVC_SLEEP_THREAD, 0, 0, 0, 0);
    }
    return RL_OK;
}

static int w_flag_then_sleep(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    S.cpu[i] = cpu_now(c);
    set(&S.flag[i], 1);
    svc_c(c, SVC_SLEEP_THREAD, 50000000, 0, 0, 0);
    return RL_OK;
}

static int w_sleep_then_flag(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    svc_c(c, SVC_SLEEP_THREAD, SLEEP_FLAG_NS, 0, 0, 0);
    S.cpu[i] = cpu_now(c);
    set(&S.flag[i], 1);
    return RL_OK;
}

static int w_ret_twice(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    uint64_t ra = c->x[30];
    if (ra)
        set(&S.flag[0], 1);
    c->x[30] = ra ? 0 : c->pc;
    return rl_arm64_ret_far(c, c->x[30], c->pc);
}

static int w_host_wait(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    hos_sched_leave();
    while (!get(&S.flag[i + 1]))
        rl_sleep_ns(1000000);
    hos_sched_return();
    S.ok[i] = get(&S.flag[i + 1]);
    return RL_OK;
}

static int w_lock(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    uint32_t tag = S.handles[i];
    uint32_t *m = (uint32_t *)(void *)(rl_mem_base() + S.mutex);
    uint32_t v = __atomic_load_n(m, __ATOMIC_ACQUIRE);
    if (__atomic_compare_exchange_n(m, &v, v | HAS_LISTENERS, 0, __ATOMIC_SEQ_CST, __ATOMIC_RELAXED))
        S.ok[i] = svc_c(c, SVC_ARBITRATE_LOCK, v, S.mutex, tag, 0) == 0 && (*m & ~HAS_LISTENERS) == tag;
    set(&S.flag[i], 1);
    return RL_OK;
}

enum { F_COUNT, F_FLAG, F_WAIT_EVENT, F_PING, F_PONG, F_MIGRATE, F_YIELD, F_FLAG_SLEEP, F_HOST_WAIT, F_LOCK, F_SLEEP_FLAG, F_RET_TWICE, F_N };

static rl_ctx *setup(uint64_t *heap)
{
    rl_ctx *c = t_boot(NULL);
    static rl_fn const fns[F_N] = { w_count,     w_flag, w_wait_event,     w_ping,     w_pong, w_migrate,
                                    w_yield,     w_flag_then_sleep, w_host_wait, w_lock, w_sleep_then_flag, w_ret_twice };
    uint64_t addrs[F_N];
    for (uint32_t k = 0; k < F_N; k++)
        addrs[k] = t_code(k);
    t_guest_fns(addrs, fns, F_N);
    *heap = t_heap(c, 0x200000);
    memset(&S, 0, sizeof S);
    S.event = *heap + 0x100;
    S.mutex = *heap + 0x200;
    return c;
}

static uint32_t spawn(rl_ctx *c, uint64_t heap, uint32_t fn, int i, int32_t prio, int32_t core)
{
    uint64_t stack = heap + 0x100000 + 0x10000u * (unsigned)i;
    if (t_svc(c, SVC_CREATE_THREAD, 0, t_code(fn), (uint64_t)i, stack, (uint64_t)(int64_t)prio, (uint64_t)(int64_t)core) != 0)
        return 0;
    S.handles[i] = (uint32_t)c->x[1];
    return S.handles[i];
}

static uint32_t start(rl_ctx *c, uint32_t h) { return t_svc(c, SVC_START_THREAD, h, 0, 0, 0, 0, 0); }

static uint32_t join(rl_ctx *c, uint64_t heap, uint32_t h)
{
    *(uint32_t *)(void *)(t_mem() + heap + 0x800) = h;
    return t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, (uint64_t)-1, 0, 0);
}

static hos_thread *thread_of(uint32_t h)
{
    hos_lock();
    hos_thread *t = hos_thread_get(h);
    hos_unlock();
    if (t)
        hos_kobj_unref(&t->obj);
    return t;
}

TEST(equal_priority_threads_of_one_core_never_interleave)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    S.iters = 20000;
    uint32_t a = spawn(c, heap, F_COUNT, 0, 44, 0), b = spawn(c, heap, F_COUNT, 1, 44, 0);
    CHECK_EQ(start(c, a), 0);
    CHECK_EQ(start(c, b), 0);
    CHECK_EQ(S.ntrace, 0);
    CHECK_EQ(join(c, heap, a), 0);
    CHECK_EQ(join(c, heap, b), 0);
    CHECK_EQ(S.counter, 2u * 20000u);
    int switches = 0;
    for (int k = 1; k < S.ntrace && k < (int)sizeof S.trace - 1; k++)
        switches += S.trace[k] != S.trace[k - 1];
    CHECK_EQ(S.trace[0], 'a');
    CHECK_EQ(switches, 1);
}

TEST(higher_priority_runs_at_the_next_svc_and_lower_waits)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t h = spawn(c, heap, F_FLAG, 0, 30, 0);
    CHECK_EQ(start(c, h), 0);
    CHECK_EQ(get(&S.flag[0]), 1);
    CHECK_EQ(S.cpu[0], 0);
    CHECK_EQ(join(c, heap, h), 0);
    uint32_t l = spawn(c, heap, F_FLAG, 1, 50, 0);
    CHECK_EQ(start(c, l), 0);
    rl_sleep_ns(20000000);
    t_svc(c, SVC_GET_SYSTEM_TICK, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(get(&S.flag[1]), 0);
    CHECK_EQ(join(c, heap, l), 0);
    CHECK_EQ(get(&S.flag[1]), 1);
    CHECK_EQ(t_svc(c, SVC_CREATE_EVENT, 0, 0, 0, 0, 0, 0), 0);
    uint32_t w = (uint32_t)c->x[1];
    *(uint32_t *)(void *)(t_mem() + S.event) = (uint32_t)c->x[2];
    uint32_t e = spawn(c, heap, F_WAIT_EVENT, 2, 30, 0);
    CHECK_EQ(start(c, e), 0);
    CHECK_EQ(get(&S.flag[2]), 0);
    CHECK_EQ(t_svc(c, SVC_SIGNAL_EVENT, w, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(get(&S.flag[2]), 1);
    CHECK_EQ(S.ok[2], 1);
    CHECK_EQ(join(c, heap, e), 0);
}

TEST(a_thread_that_wakes_preempts_a_lower_one_at_its_next_preemption_point)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t h = 0;
    bool timely = false;
    for (int attempt = 0; attempt < 100 && !timely; attempt++) {
        set(&S.flag[0], 0);
        h = spawn(c, heap, F_SLEEP_FLAG, 0, 30, 0);
        uint64_t t_start = rl_clock_ns();
        CHECK_EQ(start(c, h), 0);
        int woke = get(&S.flag[0]);
        timely = rl_clock_ns() - t_start < SLEEP_FLAG_NS;
        if (!timely) {
            CHECK_EQ(join(c, heap, h), 0);
            continue;
        }
        CHECK_EQ(woke, 0);
    }
    CHECK(timely);
    int points = 0;
    while (!get(&S.flag[0])) {
        if (rl_time_spent(&c->time)) {
            points++;
            rl_preempt(c);
        }
    }
    CHECK_EQ(get(&S.flag[0]), 1);
    CHECK_EQ(points, 1);
    CHECK_EQ(S.cpu[0], 0);
    CHECK(!rl_time_spent(&c->time));
    CHECK_EQ(join(c, heap, h), 0);
}

TEST(a_thread_entry_that_returns_with_x30_set_goes_on_there)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t h = spawn(c, heap, F_RET_TWICE, 0, 30, 0);
    CHECK_EQ(start(c, h), 0);
    CHECK_EQ(join(c, heap, h), 0);
    CHECK_EQ(get(&S.flag[0]), 1);
}

TEST(cores_run_in_parallel_within_their_masks)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t p = spawn(c, heap, F_PING, 0, 44, 1), q = spawn(c, heap, F_PONG, 1, 44, 2);
    CHECK_EQ(start(c, p), 0);
    CHECK_EQ(start(c, q), 0);
    CHECK_EQ(join(c, heap, p), 0);
    CHECK_EQ(join(c, heap, q), 0);
    CHECK_EQ(S.ok[0], 1);
    CHECK_EQ(S.ok[1], 1);
    CHECK_EQ(S.cpu[0], 1);
    CHECK_EQ(S.cpu[1], 2);
}

TEST(set_thread_core_mask_migrates_the_thread)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t m = spawn(c, heap, F_MIGRATE, 0, 44, 1);
    CHECK_EQ(start(c, m), 0);
    CHECK_EQ(join(c, heap, m), 0);
    CHECK_EQ(S.cpu[0], 1);
    CHECK_EQ(S.ok[0], 1);
    CHECK_EQ(S.cpu[1], 2);
    CHECK_EQ(S.ok[1], 1);
    CHECK_EQ(S.cpu[2], 2);
    CHECK_EQ(t_svc(c, SVC_GET_THREAD_CORE_MASK, 0, 0, m, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], 2);
    CHECK_EQ(c->x[2], 0x6);
    CHECK_EQ(t_svc(c, SVC_SET_THREAD_CORE_MASK, m, (uint64_t)-3, 0x1, 0, 0, 0), KR_INVALID_COMBINATION);
    CHECK_EQ(t_svc(c, SVC_SET_THREAD_CORE_MASK, m, 5, 0x1, 0, 0, 0), KR_INVALID_CPU_CORE);
    CHECK_EQ(t_svc(c, SVC_SET_THREAD_CORE_MASK, m, (uint64_t)-1, 0x3, 0, 0, 0), 0);
}

TEST(a_ready_thread_moves_to_an_idle_core_of_its_mask)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t t = spawn(c, heap, F_FLAG, 0, 44, 0);
    CHECK_EQ(t_svc(c, SVC_SET_THREAD_CORE_MASK, t, 0, 0x3, 0, 0, 0), 0);
    CHECK_EQ(start(c, t), 0);
    CHECK(host_wait_flag(&S.flag[0]));
    CHECK_EQ(S.cpu[0], 1);
    CHECK_EQ(join(c, heap, t), 0);
}

TEST(yield_takes_turns_with_equal_priority_only)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t w = spawn(c, heap, F_YIELD, 0, 44, 0);
    CHECK_EQ(start(c, w), 0);
    for (int k = 0; k < 3; k++) {
        note('m');
        t_svc(c, SVC_SLEEP_THREAD, 0, 0, 0, 0, 0, 0);
    }
    CHECK_EQ(join(c, heap, w), 0);
    S.trace[S.ntrace] = 0;
    CHECK(strcmp(S.trace, "mwmwmw") == 0);
    uint32_t l = spawn(c, heap, F_FLAG, 1, 50, 0);
    CHECK_EQ(start(c, l), 0);
    t_svc(c, SVC_SLEEP_THREAD, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(get(&S.flag[1]), 0);
    CHECK_EQ(join(c, heap, l), 0);
}

TEST(yield_to_any_thread_runs_a_lower_priority_one)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t l = spawn(c, heap, F_FLAG_SLEEP, 0, 50, 0);
    CHECK_EQ(start(c, l), 0);
    CHECK_EQ(get(&S.flag[0]), 0);
    t_svc(c, SVC_SLEEP_THREAD, (uint64_t)-2, 0, 0, 0, 0, 0);
    CHECK_EQ(get(&S.flag[0]), 1);
    CHECK_EQ(S.cpu[0], 0);
    CHECK_EQ(t_svc(c, SVC_GET_CURRENT_PROCESSOR_NUMBER, 0, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(join(c, heap, l), 0);
}

TEST(yield_with_migration_pulls_a_waiting_thread_over)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t x = spawn(c, heap, F_PING, 0, 44, 1);
    uint32_t w = spawn(c, heap, F_PONG, 1, 44, 1);
    CHECK_EQ(t_svc(c, SVC_SET_THREAD_CORE_MASK, w, 1, 0x3, 0, 0, 0), 0);
    CHECK_EQ(start(c, x), 0);
    CHECK(host_wait_flag(&S.flag[0]));
    CHECK_EQ(start(c, w), 0);
    hos_thread *wt = thread_of(w);
    CHECK_EQ(wt->active_core, 1);
    t_svc(c, SVC_SLEEP_THREAD, (uint64_t)-1, 0, 0, 0, 0, 0);
    CHECK_EQ(wt->active_core, 0);
    CHECK_EQ(join(c, heap, w), 0);
    CHECK_EQ(join(c, heap, x), 0);
    CHECK_EQ(S.ok[0], 1);
    CHECK_EQ(S.ok[1], 1);
    CHECK_EQ(S.cpu[1], 0);
}

TEST(a_mutex_owner_inherits_its_waiters_priority)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t me = 0;
    hos_lock();
    hos_handle_add(&hos_current()->obj, &me);
    hos_unlock();
    *(uint32_t *)(void *)(t_mem() + S.mutex) = me;
    hos_thread *self = hos_current();
    uint32_t h = spawn(c, heap, F_LOCK, 0, 30, 1);
    CHECK_EQ(start(c, h), 0);
    T_WAIT_UNTIL(__atomic_load_n(&self->prio, __ATOMIC_SEQ_CST) == 30);
    CHECK_EQ(self->prio, 30);
    CHECK_EQ(t_svc(c, SVC_GET_THREAD_PRIORITY, 0, HOS_CUR_THREAD, 0, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], 44);
    CHECK_EQ(t_svc(c, SVC_ARBITRATE_UNLOCK, S.mutex, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(self->prio, 44);
    CHECK_EQ(join(c, heap, h), 0);
    CHECK_EQ(S.ok[0], 1);
}

TEST(a_thread_in_a_host_wait_leaves_its_core_to_the_others)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t a = spawn(c, heap, F_HOST_WAIT, 0, 44, 0), b = spawn(c, heap, F_FLAG, 1, 44, 0);
    CHECK_EQ(start(c, a), 0);
    CHECK_EQ(start(c, b), 0);
    CHECK_EQ(join(c, heap, a), 0);
    CHECK_EQ(join(c, heap, b), 0);
    CHECK_EQ(S.ok[0], 1);
    CHECK_EQ(S.cpu[1], 0);
}
