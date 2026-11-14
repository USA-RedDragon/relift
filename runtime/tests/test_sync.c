/**
 * @file test_sync.c
 * @brief Threads, events, svcWaitSynchronization, mutexes and condition
 * variables under contention, the address arbiter, TLS registers, FPCR,
 * the system tick and the svc error paths. Guest thread entries are host
 * functions with the lifted signature, registered in the dispatch table.
 */
#include "t.h"

#include <stdlib.h>

#define HAS_LISTENERS 0x40000000u

static uint32_t self_handle(void)
{
    uint32_t h = 0;
    hos_lock();
    hos_handle_add(&hos_current()->obj, &h);
    hos_unlock();
    return h;
}

static uint32_t svc_c(rl_ctx *c, uint32_t imm, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3)
{
    c->x[0] = a0;
    c->x[1] = a1;
    c->x[2] = a2;
    c->x[3] = a3;
    rl_svc(c, rl_mem_base(), imm);
    return (uint32_t)c->x[0];
}

static uint32_t *word(uint64_t a) { return (uint32_t *)(void *)(rl_mem_base() + a); }

static void mutex_lock(rl_ctx *c, uint64_t m, uint32_t tag)
{
    uint32_t v = __atomic_load_n(word(m), __ATOMIC_ACQUIRE);
    for (;;) {
        if (v == 0) {
            if (__atomic_compare_exchange_n(word(m), &v, tag, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED))
                return;
            continue;
        }
        if (!(v & HAS_LISTENERS)) {
            if (!__atomic_compare_exchange_n(word(m), &v, v | HAS_LISTENERS, 0, __ATOMIC_RELAXED, __ATOMIC_RELAXED))
                continue;
        }
        svc_c(c, SVC_ARBITRATE_LOCK, v & ~HAS_LISTENERS, m, tag, 0);
        v = __atomic_load_n(word(m), __ATOMIC_ACQUIRE);
        if ((v & ~HAS_LISTENERS) == tag)
            return;
    }
}

static void mutex_unlock(rl_ctx *c, uint64_t m, uint32_t tag)
{
    uint32_t v = __atomic_load_n(word(m), __ATOMIC_RELAXED);
    for (;;) {
        if (v != tag)
            break;
        if (__atomic_compare_exchange_n(word(m), &v, 0, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return;
    }
    if (v & HAS_LISTENERS)
        svc_c(c, SVC_ARBITRATE_UNLOCK, m, 0, 0, 0);
}

/** @brief State shared by the worker threads of a test. */
static struct {
    uint64_t mutex, counter, cv, flag, arb;
    uint32_t handles[8];
    uint64_t tls[8];
    uint64_t tpidr[8];
    uint32_t fpcr_seen[8];
    int iters;
    int done;
} S;

static int idx_of(rl_ctx *c) { return (int)c->x[0]; }

static int worker_mutex(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    uint32_t tag = S.handles[i];
    for (int k = 0; k < S.iters; k++) {
        mutex_lock(c, S.mutex, tag);
        uint32_t v = *word(S.counter);
        if ((k & 15) == 0)
            rl_host_yield();
        *word(S.counter) = v + 1;
        mutex_unlock(c, S.mutex, tag);
    }
    return RL_OK;
}

static int worker_tls(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    S.tls[i] = c->tpidrro_el0;
    rl_msr_tpidr_el0(c, 0x1000u + (unsigned)i);
    rl_host_yield();
    S.tpidr[i] = rl_mrs_tpidr_el0(c);
    S.fpcr_seen[i] = rl_host_get_fpcr();
    *word(c->tpidrro_el0) = 0xC0DE0000u + (uint32_t)i;
    return RL_OK;
}

static int worker_exit(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    __atomic_store_n(&S.done, 1, __ATOMIC_SEQ_CST);
    return rl_svc(c, mem, SVC_EXIT_THREAD);
}

static int worker_condvar(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int i = idx_of(c);
    uint32_t tag = S.handles[i];
    mutex_lock(c, S.mutex, tag);
    while (*word(S.flag) == 0) {
        uint32_t rc = svc_c(c, SVC_WAIT_PROCESS_WIDE_KEY_ATOMIC, S.mutex, S.cv, tag, (uint64_t)-1);
        if (rc == KR_TIMED_OUT)
            mutex_lock(c, S.mutex, tag);
    }
    *word(S.counter) += 1;
    mutex_unlock(c, S.mutex, tag);
    return RL_OK;
}

static int worker_arb(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    uint32_t rc = svc_c(c, SVC_WAIT_FOR_ADDRESS, S.arb, 0, 1, (uint64_t)-1);
    __atomic_add_fetch(word(S.counter), rc == 0 ? 1u : 0x100u, __ATOMIC_SEQ_CST);
    return RL_OK;
}

static int worker_wait_event(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    int32_t idx;
    uint32_t rc = svc_c(c, SVC_WAIT_SYNCHRONIZATION, 0, S.flag, 1, (uint64_t)-1);
    idx = (int32_t)c->x[1];
    S.handles[7] = rc;
    S.tpidr[7] = (uint64_t)(int64_t)idx;
    return RL_OK;
}

static rl_ctx *setup(uint64_t *heap)
{
    rl_ctx *c = t_boot(NULL);
    static const uint64_t addrs[] = { 0x8000000, 0x8000100, 0x8000200, 0x8000300, 0x8000400, 0x8000500 };
    static rl_fn const fns[] = { worker_mutex, worker_tls, worker_exit, worker_condvar, worker_arb, worker_wait_event };
    t_guest_fns(addrs, fns, 6);
    *heap = t_heap(c, 0x200000);
    memset(&S, 0, sizeof S);
    S.mutex = *heap + 0x100;
    S.counter = *heap + 0x200;
    S.cv = *heap + 0x300;
    S.flag = *heap + 0x400;
    S.arb = *heap + 0x500;
    return c;
}

static uint32_t spawn(rl_ctx *c, uint64_t entry, int i, uint64_t heap)
{
    uint64_t stack = heap + 0x100000 + 0x10000u * (unsigned)i;
    if (t_svc(c, SVC_CREATE_THREAD, 0, entry, (uint64_t)i, stack, 44, 0) != 0)
        return 0;
    uint32_t h = (uint32_t)c->x[1];
    S.handles[i] = h;
    return h;
}

static uint32_t start(rl_ctx *c, uint32_t h) { return t_svc(c, SVC_START_THREAD, h, 0, 0, 0, 0, 0); }

static uint32_t join(rl_ctx *c, uint64_t heap, uint32_t h)
{
    *word(heap + 0x800) = h;
    return t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, 10000000000ull, 0, 0);
}

TEST(mutex_contention)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t me = self_handle();
    S.iters = 3000;
    uint32_t hs[4];
    for (int i = 0; i < 4; i++) {
        hs[i] = spawn(c, 0x8000000, i, heap);
        CHECK(hs[i]);
    }
    mutex_lock(c, S.mutex, me);
    for (int i = 0; i < 4; i++)
        CHECK_EQ(start(c, hs[i]), 0);
    rl_sleep_ns(20000000);
    CHECK_EQ(*word(S.mutex), me | HAS_LISTENERS);
    mutex_unlock(c, S.mutex, me);
    for (int i = 0; i < 4; i++)
        CHECK_EQ(join(c, heap, hs[i]), 0);
    CHECK_EQ(*word(S.counter), 4 * 3000);
    CHECK_EQ(*word(S.mutex), 0);
    CHECK_EQ(t_svc(c, SVC_START_THREAD, hs[0], 0, 0, 0, 0, 0), KR_INVALID_STATE);
    for (int i = 0; i < 4; i++)
        CHECK_EQ(t_svc(c, SVC_CLOSE_HANDLE, hs[i], 0, 0, 0, 0, 0), 0);
}

TEST(condvar_broadcast)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t me = self_handle();
    uint32_t hs[3];
    for (int i = 0; i < 3; i++) {
        hs[i] = spawn(c, 0x8000300, i, heap);
        CHECK_EQ(start(c, hs[i]), 0);
    }
    for (int spin = 0; spin < 500; spin++) {
        int n = 0;
        hos_lock();
        for (hos_thread *t = hos.cv_waiters; t; t = t->next_wait)
            n++;
        hos_unlock();
        if (n == 3)
            break;
        rl_sleep_ns(1000000);
    }
    CHECK_EQ(*word(S.cv), 1);
    mutex_lock(c, S.mutex, me);
    *word(S.flag) = 1;
    t_svc(c, SVC_SIGNAL_PROCESS_WIDE_KEY, S.cv, 1, 0, 0, 0, 0);
    t_svc(c, SVC_SIGNAL_PROCESS_WIDE_KEY, S.cv, (uint64_t)-1, 0, 0, 0, 0);
    CHECK_EQ(*word(S.cv), 0);
    mutex_unlock(c, S.mutex, me);
    for (int i = 0; i < 3; i++)
        CHECK_EQ(join(c, heap, hs[i]), 0);
    CHECK_EQ(*word(S.counter), 3);
    CHECK_EQ(*word(S.mutex), 0);
}

TEST(condvar_timeout)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t me = self_handle();
    mutex_lock(c, S.mutex, me);
    uint64_t t0 = rl_clock_ns();
    CHECK_EQ(t_svc(c, SVC_WAIT_PROCESS_WIDE_KEY_ATOMIC, S.mutex, S.cv, me, 20000000, 0, 0), KR_TIMED_OUT);
    CHECK(rl_clock_ns() - t0 >= 20000000);
    CHECK_EQ(*word(S.mutex), 0);
    CHECK_EQ(t_svc(c, SVC_WAIT_PROCESS_WIDE_KEY_ATOMIC, S.mutex + 1, S.cv, me, 0, 0, 0), KR_INVALID_ADDRESS);
}

TEST(events_and_wait_synchronization)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    CHECK_EQ(t_svc(c, SVC_CREATE_EVENT, 0, 0, 0, 0, 0, 0), 0);
    uint32_t w = (uint32_t)c->x[1], r = (uint32_t)c->x[2];
    CHECK_EQ(t_svc(c, SVC_CREATE_EVENT, 0, 0, 0, 0, 0, 0), 0);
    uint32_t w2 = (uint32_t)c->x[1], r2 = (uint32_t)c->x[2];
    uint32_t *hl = word(heap + 0x800);
    hl[0] = r;
    hl[1] = r2;
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 2, 0, 0, 0), KR_TIMED_OUT);
    uint64_t t0 = rl_clock_ns();
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 2, 30000000, 0, 0), KR_TIMED_OUT);
    uint64_t dt = rl_clock_ns() - t0;
    CHECK(dt >= 30000000);
    CHECK(dt < 2000000000);
    CHECK_EQ(t_svc(c, SVC_SIGNAL_EVENT, w2, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 2, (uint64_t)-1, 0, 0), 0);
    CHECK_EQ(c->x[1], 1);
    CHECK_EQ(t_svc(c, SVC_RESET_SIGNAL, r2, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_RESET_SIGNAL, r2, 0, 0, 0, 0, 0), KR_INVALID_STATE);
    CHECK_EQ(t_svc(c, SVC_SIGNAL_EVENT, r, 0, 0, 0, 0, 0), KR_INVALID_HANDLE);
    hl[0] = 0x12345;
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 1, 0, 0, 0), KR_INVALID_HANDLE);
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x800, 65, 0, 0, 0), KR_MAXIMUM_EXCEEDED);
    *word(heap + 0x900) = r;
    S.flag = heap + 0x900;
    uint32_t th = spawn(c, 0x8000500, 0, heap);
    CHECK_EQ(start(c, th), 0);
    rl_sleep_ns(20000000);
    CHECK_EQ(S.handles[7], 0);
    CHECK_EQ(t_svc(c, SVC_SIGNAL_EVENT, w, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(join(c, heap, th), 0);
    CHECK_EQ(S.handles[7], 0);
    CHECK_EQ(S.tpidr[7], 0);
    CHECK_EQ(t_svc(c, SVC_CLEAR_EVENT, w, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_CLOSE_HANDLE, w, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_CLOSE_HANDLE, w, 0, 0, 0, 0, 0), KR_INVALID_HANDLE);
    CHECK_EQ(t_svc(c, SVC_CLOSE_HANDLE, r, 0, 0, 0, 0, 0), 0);
    (void)w2;
}

TEST(cancel_synchronization)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    CHECK_EQ(t_svc(c, SVC_CREATE_EVENT, 0, 0, 0, 0, 0, 0), 0);
    *word(heap + 0x900) = (uint32_t)c->x[2];
    S.flag = heap + 0x900;
    uint32_t th = spawn(c, 0x8000500, 0, heap);
    S.handles[7] = 0xFFFF;
    CHECK_EQ(start(c, th), 0);
    rl_sleep_ns(20000000);
    CHECK_EQ(t_svc(c, SVC_CANCEL_SYNCHRONIZATION, th, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(join(c, heap, th), 0);
    CHECK_EQ(S.handles[7], KR_CANCELLED);
    uint32_t me = self_handle();
    CHECK_EQ(t_svc(c, SVC_CANCEL_SYNCHRONIZATION, me, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x900, 1, (uint64_t)-1, 0, 0), KR_CANCELLED);
    CHECK_EQ(t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, heap + 0x900, 1, 0, 0, 0), KR_TIMED_OUT);
}

TEST(threads_tls_and_registers)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    rl_msr_fpcr(c, 1u << 24);
    uint32_t hs[4];
    for (int i = 0; i < 4; i++) {
        hs[i] = spawn(c, 0x8000100, i, heap);
        CHECK_EQ(start(c, hs[i]), 0);
    }
    for (int i = 0; i < 4; i++)
        CHECK_EQ(join(c, heap, hs[i]), 0);
    for (int i = 0; i < 4; i++) {
        CHECK(S.tls[i] != 0);
        CHECK(S.tls[i] != c->tpidrro_el0);
        CHECK_EQ(S.tls[i] & 0x1FF, 0);
        CHECK_EQ(S.tpidr[i], 0x1000u + (unsigned)i);
        CHECK_EQ(S.fpcr_seen[i], 0);
        for (int k = 0; k < i; k++)
            CHECK(S.tls[i] != S.tls[k]);
        CHECK_EQ(rl_vm_query(&hos.vm, S.tls[i]).tag & 0xFF, MS_THREAD_LOCAL & 0xFF);
    }
#if defined(__aarch64__)
    CHECK_EQ(rl_host_get_fpcr(), 1u << 24);
#endif
    rl_msr_fpcr(c, 0);
    CHECK_EQ(t_svc(c, SVC_GET_THREAD_PRIORITY, 0, hs[0], 0, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], 44);
    CHECK_EQ(t_svc(c, SVC_SET_THREAD_PRIORITY, hs[0], 30, 0, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_GET_THREAD_PRIORITY, 0, hs[0], 0, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], 30);
    CHECK_EQ(t_svc(c, SVC_SET_THREAD_PRIORITY, hs[0], 10, 0, 0, 0, 0), KR_INVALID_PRIORITY);
    CHECK_EQ(t_svc(c, SVC_GET_THREAD_PRIORITY, 0, HOS_CUR_THREAD, 0, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], 44);
    CHECK_EQ(t_svc(c, SVC_GET_CURRENT_PROCESSOR_NUMBER, 0, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_SET_THREAD_CORE_MASK, HOS_CUR_THREAD, 2, 0x6, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_GET_CURRENT_PROCESSOR_NUMBER, 0, 0, 0, 0, 0, 0), 2);
    CHECK_EQ(t_svc(c, SVC_GET_THREAD_CORE_MASK, 0, 0, HOS_CUR_THREAD, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], 2);
    CHECK_EQ(c->x[2], 0x6);
    CHECK_EQ(t_svc(c, SVC_SET_THREAD_CORE_MASK, HOS_CUR_THREAD, 3, 0x8, 0, 0, 0), KR_INVALID_CPU_CORE);
    CHECK_EQ(t_svc(c, SVC_CREATE_THREAD, 0, 0x8000100, 0, heap, 44, 5), KR_INVALID_CPU_CORE);
    CHECK_EQ(t_svc(c, SVC_CREATE_THREAD, 0, 0x8000100, 0, heap, 64, 0), KR_INVALID_PRIORITY);
    CHECK_EQ(t_svc(c, SVC_GET_THREAD_ID, 0, hs[1], 0, 0, 0, 0), 0);
    uint64_t id1 = c->x[1];
    CHECK_EQ(t_svc(c, SVC_GET_THREAD_ID, 0, hs[2], 0, 0, 0, 0), 0);
    CHECK(id1 != c->x[1]);
    CHECK_EQ(t_svc(c, SVC_GET_PROCESS_ID, 0, HOS_CUR_PROCESS, 0, 0, 0, 0), 0);
    CHECK(c->x[1] != 0);
}

TEST(exit_thread)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    uint32_t th = spawn(c, 0x8000200, 0, heap);
    CHECK_EQ(start(c, th), 0);
    CHECK_EQ(join(c, heap, th), 0);
    CHECK_EQ(S.done, 1);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 24, HOS_CUR_PROCESS, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], hos.cfg.max_threads - 1);
}

TEST(wait_for_address)
{
    uint64_t heap;
    rl_ctx *c = setup(&heap);
    *word(S.arb) = 0;
    uint32_t hs[3];
    for (int i = 0; i < 3; i++) {
        hs[i] = spawn(c, 0x8000400, i, heap);
        CHECK_EQ(start(c, hs[i]), 0);
    }
    for (int spin = 0; spin < 500; spin++) {
        int n = 0;
        hos_lock();
        for (hos_thread *t = hos.arb_waiters; t; t = t->next_wait)
            n++;
        hos_unlock();
        if (n == 3)
            break;
        rl_sleep_ns(1000000);
    }
    CHECK_EQ(t_svc(c, SVC_SIGNAL_TO_ADDRESS, S.arb, 0, 0, 1, 0, 0), 0);
    rl_sleep_ns(20000000);
    CHECK_EQ(__atomic_load_n(word(S.counter), __ATOMIC_SEQ_CST), 1);
    CHECK_EQ(t_svc(c, SVC_SIGNAL_TO_ADDRESS, S.arb, 1, 5, 1, 0, 0), KR_INVALID_STATE);
    CHECK_EQ(t_svc(c, SVC_SIGNAL_TO_ADDRESS, S.arb, 1, 0, 1, 0, 0), 0);
    CHECK_EQ(*word(S.arb), 1);
    CHECK_EQ(t_svc(c, SVC_SIGNAL_TO_ADDRESS, S.arb, 0, 0, -1, 0, 0), 0);
    for (int i = 0; i < 3; i++)
        CHECK_EQ(join(c, heap, hs[i]), 0);
    CHECK_EQ(*word(S.counter), 3);
    *word(S.arb) = 7;
    CHECK_EQ(t_svc(c, SVC_WAIT_FOR_ADDRESS, S.arb, 2, 7, 10000000, 0, 0), KR_TIMED_OUT);
    CHECK_EQ(t_svc(c, SVC_WAIT_FOR_ADDRESS, S.arb, 2, 8, 10000000, 0, 0), KR_INVALID_STATE);
    CHECK_EQ(t_svc(c, SVC_WAIT_FOR_ADDRESS, S.arb, 1, 9, 0, 0, 0), KR_TIMED_OUT);
    CHECK_EQ(*word(S.arb), 6);
    CHECK_EQ(t_svc(c, SVC_WAIT_FOR_ADDRESS, S.arb, 0, 1, 0, 0, 0), KR_INVALID_STATE);
    CHECK_EQ(t_svc(c, SVC_WAIT_FOR_ADDRESS, S.arb, 3, 1, 0, 0, 0), KR_INVALID_ENUM_VALUE);
    CHECK_EQ(t_svc(c, SVC_WAIT_FOR_ADDRESS, S.arb + 2, 0, 1, 0, 0, 0), KR_INVALID_ADDRESS);
    CHECK_EQ(t_svc(c, SVC_WAIT_FOR_ADDRESS, hos.lay.heap_start + 0x400000, 0, 1, 0, 0, 0), KR_INVALID_MEM_STATE);
}

TEST(system_tick_and_debug)
{
    rl_ctx *c = t_boot(NULL);
    t_svc(c, SVC_GET_SYSTEM_TICK, 0, 0, 0, 0, 0, 0);
    uint64_t a = c->x[0];
    rl_sleep_ns(10000000);
    t_svc(c, SVC_GET_SYSTEM_TICK, 0, 0, 0, 0, 0, 0);
    uint64_t d = c->x[0] - a;
    CHECK(d >= 192000);
    CHECK(d < 19200000);
    CHECK_EQ(rl_mrs_cntfrq_el0(), 19200000);
    uint64_t heap = t_heap(c, 0x200000);
    memcpy(t_mem() + heap, "hello from the guest\n", 21);
    CHECK_EQ(t_svc(c, SVC_OUTPUT_DEBUG_STRING, heap, 21, 0, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_BREAK, 0x80000000u, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(t_svc_rc, RL_OK);
}

TEST(svc_break_and_exit_pend)
{
    rl_ctx *c = t_boot(NULL);
    t_svc(c, SVC_BREAK, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(t_svc_rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->pend.code, RL_EXIT_SOFTWARE);
    t_svc(c, SVC_EXIT_PROCESS, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(t_svc_rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->pend.code, 0);
}

TEST(unknown_svc)
{
    rl_ctx *c = t_boot(NULL);
    CHECK_EQ(t_svc(c, 0x7F, 0, 0, 0, 0, 0, 0), KR_NOT_IMPLEMENTED);
    CHECK_EQ(t_svc_rc, RL_OK);
    CHECK_EQ(t_svc(c, 0x7F, 0, 0, 0, 0, 0, 0), KR_NOT_IMPLEMENTED);
    rl_cfg()->strict = true;
    t_svc(c, 0x7E, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(t_svc_rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
}

TEST(exclusive_monitor_cleared_by_svc)
{
    rl_ctx *c = t_boot(NULL);
    c->excl.size = 4;
    t_svc(c, SVC_GET_SYSTEM_TICK, 0, 0, 0, 0, 0, 0);
    CHECK_EQ(c->excl.size, 0);
}

TEST(handle_table)
{
    rl_ctx *c = t_boot(NULL);
    uint32_t hs[2 * 200];
    for (int i = 0; i < 200; i++) {
        CHECK_EQ(t_svc(c, SVC_CREATE_EVENT, 0, 0, 0, 0, 0, 0), 0);
        hs[2 * i] = (uint32_t)c->x[1];
        hs[2 * i + 1] = (uint32_t)c->x[2];
    }
    uint32_t base = hos_handle_count();
    for (int i = 0; i < 400; i++)
        CHECK_EQ(t_svc(c, SVC_CLOSE_HANDLE, hs[i], 0, 0, 0, 0, 0), 0);
    CHECK_EQ(hos_handle_count(), base - 400);
    CHECK_EQ(t_svc(c, SVC_CREATE_EVENT, 0, 0, 0, 0, 0, 0), 0);
    CHECK((uint32_t)c->x[1] != hs[0]);
    CHECK_EQ(t_svc(c, SVC_CLOSE_HANDLE, hs[0], 0, 0, 0, 0, 0), KR_INVALID_HANDLE);
}

/** @brief A lifted-style function that calls a missing guest function through the dispatch table. */
static int call_missing(rl_ctx *c, uint8_t *mem)
{
    c->pc = 0x9000000;
    return rl_lookup(c, 0x9000000)(c, mem);
}

TEST(dispatch_miss_reports)
{
    rl_ctx *c = t_boot(NULL);
    static const uint64_t addrs[] = { 0x8000000 };
    static rl_fn const fns[] = { call_missing };
    t_guest_fns(addrs, fns, 1);
    CHECK_EQ(rl_call_guest(c, t_mem(), 0x8000000), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->pend.code, RL_EXIT_SOFTWARE);
}
