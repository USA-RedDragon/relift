/**
 * @file thread.c
 * @brief Threads: clone with L_CLONE_THREAD over host threads, thread exit and
 * the futex.
 *
 * A thread is a host thread with a context of its own over the one guest
 * address space. The child of a clone starts at the instruction after the
 * `svc`, which the build made a function entry (guests/linux lift.rs), with
 * the parent's registers, x0 = 0 and the stack and thread pointer clone
 * gave it. A clone without the flags of a thread (a fork) is not supported.
 */
#include "arm64.h"
#include "linux_priv.h"

#include <stdlib.h>
#include <string.h>

enum {
    L_CLONE_VM = 0x100,
    L_CLONE_FS = 0x200,
    L_CLONE_FILES = 0x400,
    L_CLONE_SIGHAND = 0x800,
    L_CLONE_THREAD = 0x10000,
    L_CLONE_SYSVSEM = 0x40000,
    L_CLONE_SETTLS = 0x80000,
    L_CLONE_PARENT_SETTID = 0x100000,
    L_CLONE_CHILD_CLEARTID = 0x200000,
    L_CLONE_DETACHED = 0x400000,
    L_CLONE_CHILD_SETTID = 0x1000000,
    L_CLONE_THREAD_SET = L_CLONE_VM | L_CLONE_FS | L_CLONE_FILES | L_CLONE_SIGHAND | L_CLONE_THREAD | L_CLONE_SYSVSEM
};

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
    int rc = rl_call_guest(c, rl_mem_base(), t->start);
    if (rc == RL_PENDING && c->pend.kind == RL_PEND_EXIT_PROCESS)
        rl_process_exit(c->pend.code);
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
    if ((flags & L_CLONE_THREAD_SET) != L_CLONE_THREAD_SET || (flags & 0xff)) {
        rl_report_unknown(0x4c58434c4f4eull, "linux", "clone with flags 0x%llx is not a thread (processes are not supported)",
                          (unsigned long long)flags);
        return -L_ENOSYS;
    }
    uint64_t site = c->pc;
    if (!rl_dispatch_find_ctx(c, site + 4))
        return rl_guest_fatal(c, "linux", "clone at 0x%llx: nothing was lifted at the instruction after it", (unsigned long long)site), LX_PENDING;
    lx_thread *t = calloc(1, sizeof *t);
    struct rl_thread *rt = rl_thread_alloc();
    if (!t || !rt) {
        free(t);
        return -L_ENOMEM;
    }
    rl_ctx *n = rt->ctx;
    memcpy(n->x, c->x, sizeof n->x);
    memcpy(n->v, c->v, sizeof n->v);
    n->fpcr = c->fpcr;
    n->fpsr = c->fpsr;
    n->tpidr_el0 = (flags & L_CLONE_SETTLS) ? a[3] : c->tpidr_el0;
    n->x[0] = 0;
    n->x[30] = 0;
    n->sp = a[1] ? a[1] : c->sp;
    t->rt = rt;
    t->start = site + 4;
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
        lx_put(a[4], &t->tid, 4);
    if (flags & L_CLONE_CHILD_CLEARTID)
        t->clear_child_tid = a[4];
    if (rl_thread_spawn(rt, thread_body) != 0) {
        lx_thread_end(t);
        rl_thread_free(rt);
        free(t);
        return -L_EAGAIN;
    }
    rl_thread_detach(rt);
    return t->tid;
}
