/**
 * @file thread.c
 * @brief Threads: clone with L_CLONE_THREAD over host threads, thread exit and
 * the futex.
 *
 * A thread is a host thread with a context of its own over the one guest
 * address space. The child of a clone starts at the instruction after the
 * `svc`, which the build made a function entry (guests/linux lift.rs), with
 * the parent's registers, x0 = 0 and the stack and thread pointer clone
 * gave it. A clone without the flags of a thread is a fork (process.c).
 */
#include "arch.h"
#include "hostio.h"
#include "linux_priv.h"

#include <stdlib.h>
#include <string.h>


typedef struct waiter {
    uint64_t addr;
    uint32_t bits;
    bool woken;
    struct waiter *next;
} waiter;

lx_mutex lx_big = LX_MUTEX_INIT;
lx_cond lx_ended = LX_COND_INIT;
static lx_mutex fx_lock = LX_MUTEX_INIT;
static lx_cond fx_cond = LX_COND_INIT;
static waiter *fx_waiters;
static uint64_t g_child_gen;

void lx_child_event(void)
{
    lx_mutex_lock(&fx_lock);
    g_child_gen++;
    lx_cond_broadcast(&fx_cond);
    lx_mutex_unlock(&fx_lock);
}

uint64_t lx_child_gen(void)
{
    lx_mutex_lock(&fx_lock);
    uint64_t g = g_child_gen;
    lx_mutex_unlock(&fx_lock);
    return g;
}

bool lx_child_wait(uint64_t gen)
{
    lx_mutex_lock(&fx_lock);
    while (g_child_gen == gen && !lx_signal_ready())
        lx_cond_wait(&fx_cond, &fx_lock, -1);
    bool sig = g_child_gen == gen;
    lx_mutex_unlock(&fx_lock);
    return sig;
}

void lx_threads_fork(int step)
{
    static const lx_mutex fresh_mutex = LX_MUTEX_INIT;
    static const lx_cond fresh_cond = LX_COND_INIT;
    switch (step) {
    case RL_FORK_PREPARE:
        lx_mutex_lock(&lx_big);
        lx_mutex_lock(&fx_lock);
        break;
    case RL_FORK_PARENT:
        lx_mutex_unlock(&fx_lock);
        lx_mutex_unlock(&lx_big);
        break;
    default: {
        lx_big = fresh_mutex;
        fx_lock = fresh_mutex;
        fx_cond = fresh_cond;
        lx_ended = fresh_cond;
        fx_waiters = NULL;
        lx_thread *me = lx_cur();
        for (lx_thread *t = lx.threads, *next; t; t = next) {
            next = t->next;
            if (t == me)
                continue;
            hio_waker_free(t->waker);
            free(t);
        }
        hio_waker_free(me->waker);
        me->waker = hio_waker_new();
        me->next = NULL;
        me->main = true;
        for (int s = 0; s <= L_NSIG; s++)
            me->pending[s].queued = false;
        lx.threads = me;
        lx.live = 1;
        break;
    }
    }
}

void lx_thread_kick(lx_thread *t)
{
    lx_mutex_lock(&lx_big);
    if (t->waker)
        hio_waker_kick(t->waker);
    lx_mutex_unlock(&lx_big);
    lx_mutex_lock(&fx_lock);
    lx_cond_broadcast(&fx_cond);
    lx_mutex_unlock(&fx_lock);
}

int64_t lx_wait(int h, uint32_t events, int64_t timeout_ns)
{
    lx_thread *t = lx_cur();
    int64_t end = timeout_ns < 0 ? 0 : (int64_t)rl_clock_ns() + timeout_ns;
    for (;;) {
        int64_t left = -1;
        if (timeout_ns >= 0) {
            left = end - (int64_t)rl_clock_ns();
            if (left < 0)
                left = 0;
        }
        int64_t r = hio_wait(h, events, left, t->waker);
        if (r != -L_EINTR)
            return r;
        hio_waker_clear(t->waker);
        if (lx_signal_ready())
            return -L_EINTR;
    }
}

int64_t lx_epoll_wait(int ep, hio_epoll_event *out, int max, int64_t timeout_ns)
{
    lx_thread *t = lx_cur();
    int64_t end = timeout_ns < 0 ? 0 : (int64_t)rl_clock_ns() + timeout_ns;
    for (;;) {
        int64_t left = -1;
        if (timeout_ns >= 0) {
            left = end - (int64_t)rl_clock_ns();
            if (left < 0)
                left = 0;
        }
        int64_t r = hio_epoll_wait(ep, out, max, left, t->waker);
        if (r != -L_EINTR)
            return r;
        hio_waker_clear(t->waker);
        if (lx_signal_ready())
            return -L_EINTR;
    }
}

int lx_futex_wake(uint64_t a, uint32_t n, uint32_t bits)
{
    int woke = 0;
    lx_mutex_lock(&fx_lock);
    for (waiter *w = fx_waiters; w && (uint32_t)woke < n; w = w->next)
        if (!w->woken && w->addr == a && (w->bits & bits)) {
            w->woken = true;
            woke++;
        }
    if (woke)
        lx_cond_broadcast(&fx_cond);
    lx_mutex_unlock(&fx_lock);
    return woke;
}

int64_t lx_futex_requeue(uint64_t a, uint32_t nwake, uint64_t a2, uint32_t nrequeue)
{
    int64_t done = 0;
    uint32_t moved = 0;
    lx_mutex_lock(&fx_lock);
    for (waiter *w = fx_waiters; w; w = w->next) {
        if (w->woken || w->addr != a)
            continue;
        if ((uint32_t)done < nwake) {
            w->woken = true;
            done++;
        } else if (moved < nrequeue) {
            w->addr = a2;
            moved++;
            done++;
        }
    }
    if (done)
        lx_cond_broadcast(&fx_cond);
    lx_mutex_unlock(&fx_lock);
    return done;
}

int64_t lx_futex_wait(uint64_t a, uint32_t val, int64_t timeout_ns, uint32_t bits)
{
    if (!lx_ok(a, 4, RL_VM_R) || (a & 3))
        return -L_EFAULT;
    waiter me = { .addr = a, .bits = bits };
    lx_mutex_lock(&fx_lock);
    uint32_t cur = __atomic_load_n((uint32_t *)(rl_mem_base() + a), __ATOMIC_SEQ_CST);
    if (cur != val) {
        lx_mutex_unlock(&fx_lock);
        return -L_EAGAIN;
    }
    me.next = fx_waiters;
    fx_waiters = &me;
    int64_t end = timeout_ns < 0 ? 0 : (int64_t)rl_clock_ns() + timeout_ns;
    int64_t rc = 0;
    while (!me.woken) {
        if (lx_signal_ready()) {
            rc = timeout_ns >= 0 ? -L_ERESTART_RESTARTBLOCK : -L_ERESTARTSYS;
            break;
        }
        int64_t left = -1;
        if (timeout_ns >= 0) {
            left = end - (int64_t)rl_clock_ns();
            if (left <= 0) {
                rc = -L_ETIMEDOUT;
                break;
            }
        }
        lx_cond_wait(&fx_cond, &fx_lock, left);
    }
    for (waiter **p = &fx_waiters; *p; p = &(*p)->next)
        if (*p == &me) {
            *p = me.next;
            break;
        }
    lx_mutex_unlock(&fx_lock);
    return rc;
}

void lx_thread_end(lx_thread *t)
{
    if (t->clear_child_tid && lx_ok(t->clear_child_tid, 4, RL_VM_W)) {
        __atomic_store_n((uint32_t *)(rl_mem_base() + t->clear_child_tid), 0, __ATOMIC_SEQ_CST);
        lx_futex_wake(t->clear_child_tid, 1, ~0u);
    }
    lx_mutex_lock(&lx_big);
    for (lx_thread **p = &lx.threads; *p; p = &(*p)->next)
        if (*p == t) {
            *p = t->next;
            break;
        }
    hio_waker_free(t->waker);
    t->waker = NULL;
    lx.live--;
    lx_cond_broadcast(&lx_ended);
    lx_mutex_unlock(&lx_big);
}

void lx_wait_others(void)
{
    lx_mutex_lock(&lx_big);
    while (lx.live > 0)
        lx_cond_wait(&lx_ended, &lx_big, -1);
    lx_mutex_unlock(&lx_big);
}

static void thread_body(struct rl_thread *rt)
{
    lx_thread *t = rt->guest;
    rl_ctx *c = rt->ctx;
    int rc = lx_arch_call_guest(c, rl_mem_base(), t->start);
    if (rc == RL_PENDING && c->pend.kind == RL_PEND_EXIT_PROCESS)
        lx_exit_process(c->pend.code);
    if (rc == RL_PENDING && c->pend.kind == RL_PEND_UNWIND) {
        rl_log(RL_LOG_ERROR, "thread", "guest unwind to pc 0x%llx left the thread", (unsigned long long)c->pend.pc);
        rl_process_exit(RL_EXIT_SOFTWARE);
    }
    lx_thread_end(t);
}

int64_t lx_sys_clone(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)mem;
    uint64_t flags = a[0];
    if (!(flags & L_CLONE_THREAD))
        return lx_fork(c, a);
    if ((flags & L_CLONE_THREAD_SET) != L_CLONE_THREAD_SET || (flags & 0xff)) {
        rl_report_unknown(0x4c58434c4f4eull, "linux", "clone with flags 0x%llx is neither a thread nor a fork", (unsigned long long)flags);
        return -L_EINVAL;
    }
    uint64_t site = lx_arch_after_trap(c);
    if (!rl_dispatch_find_ctx(c, site))
        return rl_guest_fatal(c, "linux", "clone at 0x%llx: nothing was lifted at the instruction after it", (unsigned long long)site), LX_PENDING;
    lx_thread *t = calloc(1, sizeof *t);
    struct rl_thread *rt = rl_thread_alloc();
    if (!t || !rt) {
        free(t);
        return -L_ENOMEM;
    }
    rl_ctx *n = rt->ctx;
    int ar = lx_arch_clone_regs(n, c, flags, a);
    if (ar != 0) {
        rl_thread_free(rt);
        free(t);
        return ar;
    }
    t->waker = hio_waker_new();
    if (!t->waker) {
        rl_thread_free(rt);
        free(t);
        return -L_EMFILE;
    }
    t->rt = rt;
    t->start = site;
    t->blocked = lx_cur()->blocked;
    rt->guest = t;
    lx_mutex_lock(&lx_big);
    t->tid = lx.next_tid++;
    t->next = lx.threads;
    lx.threads = t;
    lx.live++;
    lx_mutex_unlock(&lx_big);
    if (flags & L_CLONE_PARENT_SETTID)
        lx_put(a[2], &t->tid, 4);
    if (flags & L_CLONE_CHILD_SETTID)
        lx_put(a[lx_arch_clone_ctid_arg()], &t->tid, 4);
    if (flags & L_CLONE_CHILD_CLEARTID)
        t->clear_child_tid = a[lx_arch_clone_ctid_arg()];
    if (rl_thread_spawn(rt, thread_body) != 0) {
        lx_thread_end(t);
        rl_thread_free(rt);
        free(t);
        return -L_EAGAIN;
    }
    rl_thread_detach(rt);
    return t->tid;
}

void lx_exit_process(uint32_t code)
{
    if (lx.death_signal)
        hio_die_by_signal(lx.death_signal);
    rl_process_exit(code);
}
