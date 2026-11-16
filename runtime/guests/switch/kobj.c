/**
 * @file kobj.c
 * @brief Kernel objects, the handle table, events and the blocking primitive.
 *
 * Handle encoding and event rules follow Ryujinx (MIT) KHandleTable,
 * KReadableEvent and KWritableEvent.
 */
#include "hos.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

hos_process hos;

void hos_lock(void) { pthread_mutex_lock(&hos.lock); }
void hos_unlock(void) { pthread_mutex_unlock(&hos.lock); }

void hos_kobj_init(hos_kobj *o, uint32_t type, void (*destroy)(hos_kobj *))
{
    o->type = type;
    o->refs = 1;
    o->destroy = destroy;
}

void hos_kobj_ref(hos_kobj *o)
{
    __atomic_add_fetch(&o->refs, 1, __ATOMIC_RELAXED);
}

void hos_kobj_unref(hos_kobj *o)
{
    if (o && __atomic_sub_fetch(&o->refs, 1, __ATOMIC_ACQ_REL) == 0 && o->destroy)
        o->destroy(o);
}

rl_result hos_handle_add(hos_kobj *o, uint32_t *out)
{
    for (uint32_t i = 0; i < HOS_HANDLES; i++) {
        if (hos.handles[i])
            continue;
        if (++hos.next_handle_id >= 0x8000 || hos.next_handle_id == 0)
            hos.next_handle_id = 1;
        hos.handles[i] = o;
        hos.handle_ids[i] = hos.next_handle_id;
        hos_kobj_ref(o);
        *out = ((uint32_t)hos.next_handle_id << 15) | i;
        return KR_SUCCESS;
    }
    return KR_HANDLE_TABLE_FULL;
}

static int slot_of(uint32_t h)
{
    uint32_t i = h & 0x7FFF, id = (h >> 15) & 0x7FFF;
    if ((h >> 30) || i >= HOS_HANDLES || id == 0 || !hos.handles[i] || hos.handle_ids[i] != id)
        return -1;
    return (int)i;
}

hos_kobj *hos_handle_get(uint32_t h, uint32_t type)
{
    hos_kobj *o = NULL;
    if (h == HOS_CUR_THREAD) {
        hos_thread *t = hos_current();
        o = t ? &t->obj : NULL;
    } else {
        int s = slot_of(h);
        if (s >= 0)
            o = hos.handles[s];
    }
    if (!o || (type && o->type != type))
        return NULL;
    hos_kobj_ref(o);
    return o;
}

hos_kobj *hos_handle_remove(uint32_t h)
{
    int s = slot_of(h);
    if (s < 0)
        return NULL;
    hos_kobj *o = hos.handles[s];
    hos.handles[s] = NULL;
    hos.handle_ids[s] = 0;
    return o;
}

uint32_t hos_handle_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < HOS_HANDLES; i++)
        n += hos.handles[i] != NULL;
    return n;
}

static void revent_destroy(hos_kobj *o) { free(o); }

static void wevent_destroy(hos_kobj *o)
{
    hos_wevent *w = (hos_wevent *)o;
    hos_kobj_unref(&w->r->obj);
    free(w);
}

hos_revent *rl_hos_event_create(void)
{
    hos_revent *e = calloc(1, sizeof *e);
    if (e)
        hos_kobj_init(&e->obj, KOBJ_READABLE_EVENT, revent_destroy);
    return e;
}

void rl_hos_event_signal(hos_revent *e)
{
    hos_lock();
    if (!e->signaled) {
        e->signaled = true;
        hos_sync_signal(&e->obj);
    }
    hos_unlock();
}

void rl_hos_event_clear(hos_revent *e)
{
    hos_lock();
    e->signaled = false;
    hos_unlock();
}

rl_result hos_create_event(uint32_t *wh, uint32_t *rh)
{
    hos_revent *r = rl_hos_event_create();
    hos_wevent *w = calloc(1, sizeof *w);
    if (!r || !w) {
        free(r);
        free(w);
        return KR_OUT_OF_RESOURCE;
    }
    hos_kobj_init(&w->obj, KOBJ_WRITABLE_EVENT, wevent_destroy);
    w->r = r;
    hos_lock();
    rl_result rc = hos_handle_add(&w->obj, wh);
    if (rc == KR_SUCCESS) {
        rc = hos_handle_add(&r->obj, rh);
        if (rc != KR_SUCCESS) {
            hos_handle_remove(*wh);
            hos_kobj_unref(&w->obj);
        }
    }
    hos_unlock();
    hos_kobj_unref(&w->obj);
    return rc;
}

rl_result hos_signal_event(uint32_t h)
{
    hos_lock();
    hos_kobj *o = hos_handle_get(h, KOBJ_WRITABLE_EVENT);
    if (!o) {
        hos_unlock();
        return KR_INVALID_HANDLE;
    }
    hos_revent *r = ((hos_wevent *)o)->r;
    if (!r->signaled) {
        r->signaled = true;
        hos_sync_signal(&r->obj);
    }
    hos_unlock();
    hos_kobj_unref(o);
    return KR_SUCCESS;
}

static hos_revent *event_of(hos_kobj *o)
{
    if (o->type == KOBJ_WRITABLE_EVENT)
        return ((hos_wevent *)o)->r;
    if (o->type == KOBJ_READABLE_EVENT)
        return (hos_revent *)o;
    return NULL;
}

rl_result hos_clear_event(uint32_t h)
{
    hos_lock();
    hos_kobj *o = hos_handle_get(h, 0);
    hos_revent *r = o ? event_of(o) : NULL;
    if (r)
        r->signaled = false;
    hos_unlock();
    if (o)
        hos_kobj_unref(o);
    return r ? KR_SUCCESS : KR_INVALID_HANDLE;
}

rl_result hos_reset_signal(uint32_t h)
{
    rl_result rc;
    hos_lock();
    hos_kobj *o = hos_handle_get(h, KOBJ_READABLE_EVENT);
    if (!o) {
        rc = KR_INVALID_HANDLE;
    } else if (!((hos_revent *)o)->signaled) {
        rc = KR_INVALID_STATE;
    } else {
        ((hos_revent *)o)->signaled = false;
        rc = KR_SUCCESS;
    }
    hos_unlock();
    if (o)
        hos_kobj_unref(o);
    return rc;
}

static bool is_signaled(hos_kobj *o)
{
    switch (o->type) {
    case KOBJ_READABLE_EVENT:
        return ((hos_revent *)o)->signaled;
    case KOBJ_THREAD:
        return ((hos_thread *)o)->state == HT_EXITED;
    default:
        return false;
    }
}

void hos_sync_signal(hos_kobj *o)
{
    if (!is_signaled(o))
        return;
    for (hos_thread *t = hos.threads; t; t = t->next_all) {
        if (t->wait_kind != HW_SYNC || t->woken)
            continue;
        for (int32_t i = 0; i < t->wait_count; i++)
            if (t->wait_objs[i] == o) {
                t->sync_index = i;
                hos_wake(t, KR_SUCCESS);
                break;
            }
    }
}

void hos_wake(hos_thread *t, rl_result r)
{
    t->wait_result = r;
    t->woken = true;
    t->deadline = 0;
    pthread_cond_signal(&t->cv);
    hos_sched_ready(t);
}

static bool wait_woken(hos_thread *t, int64_t timeout_ns)
{
    if (hos.sched.fixed) {
        hos_fs_set_deadline(t, timeout_ns);
        while (!t->woken && !t->expired)
            pthread_cond_wait(&t->cv, &hos.lock);
        t->deadline = 0;
        return t->woken;
    }
    if (timeout_ns < 0) {
        while (!t->woken)
            pthread_cond_wait(&t->cv, &hos.lock);
        return true;
    }
    uint64_t deadline = rl_clock_ns() + (uint64_t)timeout_ns;
    while (!t->woken) {
        uint64_t now = rl_clock_ns();
        if (now >= deadline)
            return false;
#if defined(__APPLE__)
        uint64_t rel = deadline - now;
        struct timespec ts = { (time_t)(rel / 1000000000ull), (long)(rel % 1000000000ull) };
        pthread_cond_timedwait_relative_np(&t->cv, &hos.lock, &ts);
#else
        struct timespec ts = { (time_t)(deadline / 1000000000ull), (long)(deadline % 1000000000ull) };
        pthread_cond_timedwait(&t->cv, &hos.lock, &ts);
#endif
    }
    return true;
}

bool hos_block(hos_thread *t, int64_t timeout_ns)
{
    hos_sched_unready(t);
    bool woken = wait_woken(t, timeout_ns);
    hos_sched_ready(t);
    hos_sched_run(t);
    return woken;
}

rl_result hos_wait_sync(int32_t *index, uint64_t handles_addr, int32_t count, int64_t timeout)
{
    hos_thread *t = hos_current();
    uint32_t hs[HOS_WAIT_MAX];
    *index = 0;
    if (count < 0 || count > HOS_WAIT_MAX)
        return KR_MAXIMUM_EXCEEDED;
    if (count && (handles_addr & 3))
        return KR_USER_COPY_FAILED;
    hos_lock();
    for (int32_t i = 0; i < count; i++)
        if (!hos_read_u32(handles_addr + 4u * (uint32_t)i, &hs[i])) {
            hos_unlock();
            return KR_USER_COPY_FAILED;
        }
    int32_t n = 0;
    rl_result rc = KR_SUCCESS;
    for (; n < count; n++) {
        hos_kobj *o = hos_handle_get(hs[n], 0);
        if (!o || (o->type != KOBJ_READABLE_EVENT && o->type != KOBJ_THREAD)) {
            if (o)
                hos_kobj_unref(o);
            rc = KR_INVALID_HANDLE;
            break;
        }
        t->wait_objs[n] = o;
    }
    if (rc == KR_SUCCESS) {
        t->wait_count = n;
        if (t->cancel_pending) {
            t->cancel_pending = false;
            rc = KR_CANCELLED;
        } else {
            int32_t ready = -1;
            for (int32_t i = 0; i < n && ready < 0; i++)
                if (is_signaled(t->wait_objs[i]))
                    ready = i;
            if (ready >= 0) {
                *index = ready;
            } else if (timeout == 0) {
                rc = KR_TIMED_OUT;
            } else {
                t->woken = false;
                t->wait_kind = HW_SYNC;
                t->wait_result = KR_TIMED_OUT;
                hos_block(t, timeout);
                t->wait_kind = HW_NONE;
                rc = t->woken ? t->wait_result : KR_TIMED_OUT;
                if (rc == KR_SUCCESS)
                    *index = t->sync_index;
                else if (rc == KR_CANCELLED)
                    *index = -1;
                t->woken = false;
            }
        }
    }
    t->wait_count = 0;
    hos_unlock();
    for (int32_t i = 0; i < n; i++)
        hos_kobj_unref(t->wait_objs[i]);
    return rc;
}

rl_result hos_cancel_sync(uint32_t h)
{
    hos_lock();
    hos_thread *t = hos_thread_get(h);
    if (!t) {
        hos_unlock();
        return KR_INVALID_HANDLE;
    }
    if (t->wait_kind == HW_SYNC && !t->woken)
        hos_wake(t, KR_CANCELLED);
    else
        t->cancel_pending = true;
    hos_unlock();
    hos_kobj_unref(&t->obj);
    return KR_SUCCESS;
}

bool hos_read_u32(uint64_t addr, uint32_t *v)
{
    if ((addr & 3) || !rl_vm_contains(&hos.vm, addr, 4))
        return false;
    rl_vm_block b = rl_vm_query(&hos.vm, addr);
    if (!((uint32_t)b.tag & MS_MAPPED) || !(b.perm & RL_VM_R))
        return false;
    *v = __atomic_load_n((uint32_t *)(void *)(hos.vm.base + addr), __ATOMIC_SEQ_CST);
    return true;
}

bool hos_writable_u32(uint64_t addr)
{
    if ((addr & 3) || !rl_vm_contains(&hos.vm, addr, 4))
        return false;
    rl_vm_block b = rl_vm_query(&hos.vm, addr);
    return ((uint32_t)b.tag & MS_MAPPED) && (b.perm & RL_VM_W);
}

bool hos_write_u32(uint64_t addr, uint32_t v)
{
    if (!hos_writable_u32(addr))
        return false;
    __atomic_store_n((uint32_t *)(void *)(hos.vm.base + addr), v, __ATOMIC_SEQ_CST);
    return true;
}

bool hos_cas_u32(uint64_t addr, uint32_t *expected, uint32_t desired)
{
    if (!hos_writable_u32(addr))
        return false;
    return __atomic_compare_exchange_n((uint32_t *)(void *)(hos.vm.base + addr), expected, desired, 0, __ATOMIC_SEQ_CST,
                                       __ATOMIC_SEQ_CST);
}
