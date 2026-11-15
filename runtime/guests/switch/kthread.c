/**
 * @file kthread.c
 * @brief Guest threads on host threads: creation, start, exit, priorities,
 * cores and the TLS slot behind tpidrro_el0.
 *
 * Argument checks follow Ryujinx (MIT) Syscall.CreateThread and friends.
 */
#include "hos.h"

#include <stdlib.h>
#include <string.h>

hos_thread *hos_current(void)
{
    struct rl_thread *rt = rl_thread_current();
    return rt ? rt->guest : NULL;
}

static void thread_destroy(hos_kobj *o)
{
    hos_thread *t = (hos_thread *)o;
    hos_lock();
    for (hos_thread **p = &hos.threads; *p; p = &(*p)->next_all)
        if (*p == t) {
            *p = t->next_all;
            break;
        }
    if (t->tls) {
        hos_tls_free(t->tls);
        t->tls = 0;
    }
    hos_unlock();
    pthread_cond_destroy(&t->cv);
    rl_thread_free(t->rt);
    free(t);
}

static hos_thread *new_thread(int32_t prio, int32_t core)
{
    hos_thread *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->rt = rl_thread_alloc();
    if (!t->rt) {
        free(t);
        return NULL;
    }
    t->tls = hos_tls_alloc();
    if (!t->tls) {
        rl_thread_free(t->rt);
        free(t);
        return NULL;
    }
    hos_kobj_init(&t->obj, KOBJ_THREAD, thread_destroy);
    pthread_cond_init(&t->cv, NULL);
#if !defined(__APPLE__)
    {
        pthread_condattr_t ca;
        pthread_condattr_init(&ca);
        pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
        pthread_cond_destroy(&t->cv);
        pthread_cond_init(&t->cv, &ca);
        pthread_condattr_destroy(&ca);
    }
#endif
    t->rt->guest = t;
    t->rt->ctx->tpidrro_el0 = t->tls;
    t->prio = prio;
    t->base_prio = prio;
    t->core = core;
    t->affinity = 1ull << core;
    t->active_core = core;
    t->cur_core = -1;
    t->id = hos.next_thread_id++;
    t->next_all = hos.threads;
    hos.threads = t;
    return t;
}

hos_thread *hos_thread_get(uint32_t h)
{
    return (hos_thread *)hos_handle_get(h, KOBJ_THREAD);
}

static bool prio_ok(int32_t prio)
{
    return prio >= 0 && prio <= 63 && (hos.cfg.priority_mask >> prio) & 1;
}

static bool core_ok(int32_t core)
{
    return core >= 0 && core < 64 && (hos.cfg.core_mask >> core) & 1;
}

rl_result hos_create_thread(uint32_t *out, uint64_t entry, uint64_t arg, uint64_t stack_top, int32_t prio, int32_t core,
                            uint32_t fpcr)
{
    if (core == -2)
        core = hos.cfg.main_core;
    if (!core_ok(core))
        return KR_INVALID_CPU_CORE;
    if (!prio_ok(prio))
        return KR_INVALID_PRIORITY;
    rl_result rc;
    hos_lock();
    if (hos.live_threads >= hos.cfg.max_threads) {
        hos_unlock();
        return KR_RES_LIMIT_EXCEEDED;
    }
    hos_thread *t = new_thread(prio, core);
    if (!t) {
        hos_unlock();
        return KR_OUT_OF_RESOURCE;
    }
    t->entry = entry;
    t->arg = arg;
    t->stack_top = stack_top;
    t->rt->ctx->fpcr = (t->rt->ctx->fpcr & ~RL_FPCR_GUEST) | (fpcr & RL_FPCR_GUEST);
    hos.live_threads++;
    rc = hos_handle_add(&t->obj, out);
    if (rc != KR_SUCCESS)
        hos.live_threads--;
    hos_unlock();
    hos_kobj_unref(&t->obj);
    return rc;
}

static int finish_guest(rl_ctx *c, int rc)
{
    if (rc != RL_PENDING)
        return rc;
    switch (c->pend.kind) {
    case RL_PEND_EXIT_PROCESS:
        rl_process_exit(c->pend.code);
        break;
    case RL_PEND_UNWIND:
        rl_log(RL_LOG_ERROR, "thread", "guest unwind to pc 0x%llx sp 0x%llx left the thread entry",
               (unsigned long long)c->pend.pc, (unsigned long long)c->pend.sp);
        rl_process_exit(RL_EXIT_SOFTWARE);
        break;
    default:
        break;
    }
    return rc;
}

/**
 * @brief Runs a thread entry the way the CPU would: every ret goes to its
 * target (libnx's crt0 tail-branches to main with x30 = exit, so main
 * returns into exit), and a return to 0, the x30 the thread starts with,
 * ends the thread (rl_call_guest).
 */
static int run_entry(rl_ctx *c, uint64_t entry) { return rl_call_guest(c, rl_mem_base(), entry); }

static void thread_body(struct rl_thread *rt)
{
    hos_thread *t = rt->guest;
    rl_ctx *c = rt->ctx;
    c->x[0] = t->arg;
    c->sp = t->stack_top;
    c->x[30] = 0;
    c->tpidrro_el0 = t->tls;
    hos_lock();
    hos_sched_run(t);
    hos_unlock();
    finish_guest(c, run_entry(c, t->entry));
    hos_thread_exited(t);
    rl_thread_bind(NULL);
    hos_kobj_unref(&t->obj);
}

rl_result hos_start_thread(uint32_t h)
{
    rl_result rc = KR_SUCCESS;
    hos_lock();
    hos_thread *t = hos_thread_get(h);
    if (!t) {
        rc = KR_INVALID_HANDLE;
    } else if (t->state != HT_CREATED) {
        rc = KR_INVALID_STATE;
    } else {
        t->state = HT_RUNNING;
        hos_kobj_ref(&t->obj);
        if (rl_thread_spawn(t->rt, thread_body) != 0) {
            t->state = HT_CREATED;
            hos_kobj_unref(&t->obj);
            rc = KR_OUT_OF_RESOURCE;
        } else {
            rl_thread_detach(t->rt);
            hos_sched_ready(t);
        }
    }
    hos_unlock();
    if (t)
        hos_kobj_unref(&t->obj);
    return rc;
}

void hos_thread_exited(hos_thread *t)
{
    hos_lock();
    hos_sched_unready(t);
    if (t->state != HT_EXITED) {
        t->state = HT_EXITED;
        hos.live_threads--;
        hos_sync_signal(&t->obj);
    }
    hos_unlock();
}

void hos_sleep_thread(int64_t ns)
{
    hos_thread *t = hos_current();
    if (ns <= 0) {
        hos_sched_yield(ns);
        return;
    }
    if (!t || hos.sched.free) {
        rl_sleep_ns((uint64_t)ns);
        return;
    }
    hos_lock();
    t->woken = false;
    t->wait_kind = HW_SLEEP;
    hos_block(t, ns);
    t->wait_kind = HW_NONE;
    t->woken = false;
    hos_unlock();
}

rl_result hos_get_thread_priority(uint32_t h, int32_t *out)
{
    hos_lock();
    hos_thread *t = hos_thread_get(h);
    if (t)
        *out = t->base_prio;
    hos_unlock();
    if (!t)
        return KR_INVALID_HANDLE;
    hos_kobj_unref(&t->obj);
    return KR_SUCCESS;
}

rl_result hos_set_thread_priority(uint32_t h, int32_t prio)
{
    if (!prio_ok(prio))
        return KR_INVALID_PRIORITY;
    hos_lock();
    hos_thread *t = hos_thread_get(h);
    if (t) {
        t->base_prio = prio;
        hos_update_inheritance(t);
    }
    hos_unlock();
    if (!t)
        return KR_INVALID_HANDLE;
    hos_kobj_unref(&t->obj);
    return KR_SUCCESS;
}

rl_result hos_get_thread_core_mask(uint32_t h, int32_t *core, uint64_t *mask)
{
    hos_lock();
    hos_thread *t = hos_thread_get(h);
    if (t) {
        *core = t->core;
        *mask = t->affinity;
    }
    hos_unlock();
    if (!t)
        return KR_INVALID_HANDLE;
    hos_kobj_unref(&t->obj);
    return KR_SUCCESS;
}

rl_result hos_set_thread_core_mask(uint32_t h, int32_t core, uint64_t mask)
{
    if (core == -2) {
        core = hos.cfg.main_core;
        mask = 1ull << core;
    } else {
        if ((mask | hos.cfg.core_mask) != hos.cfg.core_mask)
            return KR_INVALID_CPU_CORE;
        if (mask == 0)
            return KR_INVALID_COMBINATION;
        if (core < 0 || core >= HOS_CORES) {
            if (core != -1 && core != -3)
                return KR_INVALID_CPU_CORE;
        } else if (!((mask >> core) & 1)) {
            return KR_INVALID_COMBINATION;
        }
    }
    hos_lock();
    hos_thread *t = hos_thread_get(h);
    rl_result rc = t ? hos_sched_set_affinity(t, core, mask) : KR_INVALID_HANDLE;
    hos_unlock();
    if (t)
        hos_kobj_unref(&t->obj);
    return rc;
}

rl_result hos_get_thread_id(uint32_t h, uint64_t *out)
{
    hos_lock();
    hos_thread *t = hos_thread_get(h);
    if (t)
        *out = t->id;
    hos_unlock();
    if (!t)
        return KR_INVALID_HANDLE;
    hos_kobj_unref(&t->obj);
    return KR_SUCCESS;
}

rl_ctx *rl_hos_attach_thread(int32_t prio, int32_t core)
{
    hos_lock();
    hos_thread *t = new_thread(prio, core);
    if (t) {
        t->state = HT_RUNNING;
        t->attached = true;
        hos.live_threads++;
    }
    hos_unlock();
    if (!t)
        return NULL;
    rl_thread_bind(t->rt);
    hos_lock();
    hos_sched_ready(t);
    hos_sched_run(t);
    hos_unlock();
    return t->rt->ctx;
}

void rl_hos_detach_thread(void)
{
    hos_thread *t = hos_current();
    if (!t)
        return;
    hos_thread_exited(t);
    rl_thread_bind(NULL);
    hos_kobj_unref(&t->obj);
}

int rl_hos_run_main(uint64_t entry, uint64_t stack_top)
{
    hos_thread *t = hos_current();
    uint32_t h = 0;
    if (!t)
        return RL_PENDING;
    hos_lock();
    hos_handle_add(&t->obj, &h);
    hos_unlock();
    rl_ctx *c = t->rt->ctx;
    c->x[0] = 0;
    c->x[1] = h;
    c->sp = stack_top;
    c->x[30] = 0;
    return finish_guest(c, run_entry(c, entry));
}
