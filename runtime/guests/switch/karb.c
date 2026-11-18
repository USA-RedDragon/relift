/**
 * @file karb.c
 * @brief User mutexes, condition variables and the address arbiter:
 * svcArbitrateLock/Unlock, svcWaitProcessWideKeyAtomic,
 * svcSignalProcessWideKey, svcWaitForAddress and svcSignalToAddress.
 *
 * A port of the logic of Ryujinx (MIT) KAddressArbiter and the KThread mutex
 * waiter list. The mutex word holds the owner's handle tag, with bit 30 set
 * while other threads wait.
 */
#include "hos.h"

#define HAS_LISTENERS 0x40000000u

static void insert_waiter(hos_thread *owner, hos_thread *t)
{
    hos_thread **p = &owner->mutex_waiters;
    while (*p && (*p)->prio <= t->prio)
        p = &(*p)->next_mutex_waiter;
    t->next_mutex_waiter = *p;
    *p = t;
    t->mutex_owner = owner;
}

static void unlink_waiter(hos_thread *owner, hos_thread *t)
{
    for (hos_thread **p = &owner->mutex_waiters; *p; p = &(*p)->next_mutex_waiter)
        if (*p == t) {
            *p = t->next_mutex_waiter;
            break;
        }
    t->next_mutex_waiter = NULL;
    t->mutex_owner = NULL;
}

void hos_update_inheritance(hos_thread *t)
{
    for (;;) {
        int32_t p = t->base_prio;
        if (t->mutex_waiters && t->mutex_waiters->prio < p)
            p = t->mutex_waiters->prio;
        if (p == t->prio)
            return;
        hos_sched_set_prio(t, p);
        hos_thread *owner = t->mutex_owner;
        if (!owner)
            return;
        unlink_waiter(owner, t);
        insert_waiter(owner, t);
        t = owner;
    }
}

static void add_mutex_waiter(hos_thread *owner, hos_thread *t)
{
    insert_waiter(owner, t);
    hos_update_inheritance(owner);
}

static void remove_mutex_waiter(hos_thread *t)
{
    hos_thread *owner = t->mutex_owner;
    if (!owner)
        return;
    unlink_waiter(owner, t);
    hos_update_inheritance(owner);
}

static hos_thread *relinquish(hos_thread *cur, uint64_t addr, uint32_t *count)
{
    hos_thread *next = NULL;
    *count = 0;
    hos_thread **p = &cur->mutex_waiters;
    while (*p) {
        hos_thread *w = *p;
        if (w->mutex_addr != addr) {
            p = &w->next_mutex_waiter;
            continue;
        }
        *p = w->next_mutex_waiter;
        w->next_mutex_waiter = NULL;
        w->mutex_owner = NULL;
        (*count)++;
        if (!next)
            next = w;
        else
            insert_waiter(next, w);
    }
    if (next) {
        hos_update_inheritance(cur);
        hos_update_inheritance(next);
    }
    return next;
}

static uint32_t mutex_unlock(hos_thread *cur, uint64_t addr, hos_thread **new_owner)
{
    uint32_t count;
    hos_thread *next = relinquish(cur, addr, &count);
    uint32_t value = 0;
    if (next) {
        value = next->mutex_tag;
        if (count >= 2)
            value |= HAS_LISTENERS;
        hos_wake(next, KR_SUCCESS);
    }
    if (new_owner)
        *new_owner = next;
    return value;
}

static void list_add(hos_thread **head, hos_thread *t)
{
    t->next_wait = NULL;
    while (*head)
        head = &(*head)->next_wait;
    *head = t;
}

static bool list_remove(hos_thread **head, hos_thread *t)
{
    for (; *head; head = &(*head)->next_wait)
        if (*head == t) {
            *head = t->next_wait;
            t->next_wait = NULL;
            return true;
        }
    return false;
}

static hos_thread *list_best(hos_thread *head, int kind, uint64_t addr)
{
    hos_thread *best = NULL;
    for (hos_thread *t = head; t; t = t->next_wait) {
        uint64_t a = kind == HW_CONDVAR ? t->cv_key : t->mutex_addr;
        if (a == addr && (!best || t->prio < best->prio))
            best = t;
    }
    return best;
}

rl_result hos_arbitrate_lock(uint32_t owner_h, uint64_t addr, uint32_t tag)
{
    if (addr & 3)
        return KR_INVALID_ADDRESS;
    hos_thread *t = hos_current();
    uint32_t value;
    hos_lock();
    if (!hos_read_u32(addr, &value)) {
        hos_unlock();
        return KR_INVALID_MEM_STATE;
    }
    if (value != (owner_h | HAS_LISTENERS)) {
        hos_unlock();
        return KR_SUCCESS;
    }
    hos_thread *owner = hos_thread_get(owner_h);
    if (!owner) {
        hos_unlock();
        return KR_INVALID_HANDLE;
    }
    t->mutex_addr = addr;
    t->mutex_tag = tag;
    t->woken = false;
    t->wait_result = KR_SUCCESS;
    t->wait_kind = HW_MUTEX;
    add_mutex_waiter(owner, t);
    hos_block(t, -1);
    remove_mutex_waiter(t);
    t->wait_kind = HW_NONE;
    t->woken = false;
    rl_result rc = t->wait_result;
    hos_unlock();
    hos_kobj_unref(&owner->obj);
    return rc;
}

rl_result hos_arbitrate_unlock(uint64_t addr)
{
    if (addr & 3)
        return KR_INVALID_ADDRESS;
    hos_thread *t = hos_current();
    hos_thread *next;
    rl_result rc = KR_SUCCESS;
    hos_lock();
    uint32_t value = mutex_unlock(t, addr, &next);
    if (!hos_write_u32(addr, value)) {
        rc = KR_INVALID_MEM_STATE;
        if (next)
            next->wait_result = rc;
    }
    hos_unlock();
    return rc;
}

rl_result hos_wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout)
{
    if (mutex & 3)
        return KR_INVALID_ADDRESS;
    hos_thread *t = hos_current();
    hos_lock();
    t->woken = false;
    t->wait_result = KR_TIMED_OUT;
    uint32_t value = mutex_unlock(t, mutex, NULL);
    hos_write_u32(key, 1);
    if (!hos_write_u32(mutex, value)) {
        hos_unlock();
        return KR_INVALID_MEM_STATE;
    }
    t->mutex_addr = mutex;
    t->mutex_tag = tag;
    t->cv_key = key;
    t->wait_kind = HW_CONDVAR;
    list_add(&hos.cv_waiters, t);
    if (timeout != 0)
        hos_block(t, timeout);
    remove_mutex_waiter(t);
    list_remove(&hos.cv_waiters, t);
    rl_result rc = t->woken ? t->wait_result : KR_TIMED_OUT;
    t->wait_kind = HW_NONE;
    t->woken = false;
    hos_unlock();
    return rc;
}

static void try_acquire(hos_thread *t)
{
    uint32_t cur;
    if (!hos_writable_u32(t->mutex_addr) || !hos_read_u32(t->mutex_addr, &cur)) {
        hos_wake(t, KR_INVALID_MEM_STATE);
        return;
    }
    for (;;) {
        uint32_t want = cur ? (cur | HAS_LISTENERS) : t->mutex_tag;
        if (hos_cas_u32(t->mutex_addr, &cur, want))
            break;
        if (!hos_read_u32(t->mutex_addr, &cur)) {
            hos_wake(t, KR_INVALID_MEM_STATE);
            return;
        }
    }
    if (cur == 0) {
        hos_wake(t, KR_SUCCESS);
        return;
    }
    hos_thread *owner = hos_thread_get(cur & ~HAS_LISTENERS);
    if (owner) {
        t->wait_kind = HW_MUTEX;
        add_mutex_waiter(owner, t);
        hos_kobj_unref(&owner->obj);
    } else {
        hos_wake(t, KR_INVALID_HANDLE);
    }
}

void hos_signal_process_wide_key(uint64_t key, int32_t count)
{
    hos_lock();
    for (int32_t n = 0; count <= 0 || n < count; n++) {
        hos_thread *t = list_best(hos.cv_waiters, HW_CONDVAR, key);
        if (!t)
            break;
        list_remove(&hos.cv_waiters, t);
        try_acquire(t);
    }
    if (!list_best(hos.cv_waiters, HW_CONDVAR, key))
        hos_write_u32(key, 0);
    hos_unlock();
}

static void wake_arbiter(uint64_t addr, int32_t count)
{
    for (int32_t n = 0; count <= 0 || n < count; n++) {
        hos_thread *t = list_best(hos.arb_waiters, HW_ARBITER, addr);
        if (!t)
            break;
        list_remove(&hos.arb_waiters, t);
        hos_wake(t, KR_SUCCESS);
    }
}

/**
 * @brief Type 3, a Switch 2 kernel's: wait while the 64-bit word at addr
 * equals value. Verified from a Switch 2 SDK's nn::os::WaitForAddress(const
 * long *, long), which passes type 3 with the whole value in X2 and takes
 * InvalidState as a changed word; the 8-byte alignment is inferred.
 */
#define ARB_WAIT_IF_EQUAL64 3u

rl_result hos_wait_for_address(uint64_t addr, uint32_t type, int64_t value, int64_t timeout)
{
    bool wide = type == ARB_WAIT_IF_EQUAL64 && hos_switch2();
    if (addr & (wide ? 7u : 3u))
        return KR_INVALID_ADDRESS;
    if (type > 2 && !wide)
        return KR_INVALID_ENUM_VALUE;
    hos_thread *t = hos_current();
    uint32_t cur = 0;
    uint64_t cur64 = 0;
    hos_lock();
    if (wide ? !hos_read_u64(addr, &cur64) : !hos_read_u32(addr, &cur)) {
        hos_unlock();
        return KR_INVALID_MEM_STATE;
    }
    if (type == 1) {
        if (!hos_writable_u32(addr)) {
            hos_unlock();
            return KR_INVALID_MEM_STATE;
        }
        for (;;) {
            if (hos_cas_u32(addr, &cur, cur - 1))
                break;
            if (!hos_read_u32(addr, &cur)) {
                hos_unlock();
                return KR_INVALID_MEM_STATE;
            }
        }
    }
    bool wait = wide        ? cur64 == (uint64_t)value
                : type == 2 ? (int32_t)cur == (int32_t)value
                            : (int32_t)cur < (int32_t)value;
    rl_result rc;
    if (!wait) {
        rc = KR_INVALID_STATE;
    } else if (timeout == 0) {
        rc = KR_TIMED_OUT;
    } else {
        t->woken = false;
        t->wait_result = KR_TIMED_OUT;
        t->mutex_addr = addr;
        t->wait_kind = HW_ARBITER;
        list_add(&hos.arb_waiters, t);
        hos_block(t, timeout);
        list_remove(&hos.arb_waiters, t);
        rc = t->woken ? t->wait_result : KR_TIMED_OUT;
        t->wait_kind = HW_NONE;
        t->woken = false;
    }
    hos_unlock();
    return rc;
}

rl_result hos_signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count)
{
    if (addr & 3)
        return KR_INVALID_ADDRESS;
    if (type > 2)
        return KR_INVALID_ENUM_VALUE;
    rl_result rc = KR_SUCCESS;
    hos_lock();
    if (type == 0) {
        wake_arbiter(addr, count);
        hos_unlock();
        return KR_SUCCESS;
    }
    int32_t addend = 1;
    if (type == 2) {
        int32_t waiting = 0;
        for (hos_thread *t = hos.arb_waiters; t; t = t->next_wait)
            if (t->mutex_addr == addr && ++waiting >= count)
                break;
        if (waiting > 0)
            addend = count <= 0 ? -2 : (waiting < count ? -1 : 0);
    }
    uint32_t cur;
    if (!hos_writable_u32(addr) || !hos_read_u32(addr, &cur)) {
        rc = KR_INVALID_MEM_STATE;
    } else {
        for (;;) {
            if ((int32_t)cur != value) {
                rc = KR_INVALID_STATE;
                break;
            }
            if (hos_cas_u32(addr, &cur, (uint32_t)((int32_t)cur + addend)))
                break;
            if (!hos_read_u32(addr, &cur)) {
                rc = KR_INVALID_MEM_STATE;
                break;
            }
        }
    }
    if (rc == KR_SUCCESS)
        wake_arbiter(addr, count);
    hos_unlock();
    return rc;
}
