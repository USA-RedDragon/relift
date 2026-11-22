/**
 * @file nvn_obj.c
 * @brief relift's NVN2 objects: the state behind each device, queue, memory
 * pool, buffer, sync, texture, sampler, pool of descriptors and window the
 * game initializes, found by the guest address of its NVN object.
 *
 * The game owns the object memory; relift keeps its own state in a table and
 * never reads or writes the driver's layout of these objects. Builders stay
 * the driver's (nvn_driver.c), and relift reads them through the driver's
 * getters.
 */
#include "nvn_priv.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#define MAP_MIN 64u

#define GONE_RING 512u

static struct {
    pthread_mutex_t lock;
    nvn_obj **slots;
    uint32_t cap, used, filled;
    /** @brief The last objects removed: address, kind, how, guest thread and time, newest at gone_next - 1. */
    struct {
        uint64_t addr, tid;
        int64_t ns;
        uint32_t kind, how;
    } gone[GONE_RING];
    uint32_t gone_next;
} objs = { .lock = PTHREAD_MUTEX_INITIALIZER };

static void note_gone_locked(const nvn_obj *o, uint32_t how)
{
    hos_thread *h = hos_current();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    uint32_t i = objs.gone_next++ % GONE_RING;
    objs.gone[i].addr = o->addr;
    objs.gone[i].kind = o->kind;
    objs.gone[i].how = how;
    objs.gone[i].tid = h ? h->id : 0;
    objs.gone[i].ns = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

bool nvn_obj_gone(uint64_t addr, char *out, size_t n)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    bool found = false;
    pthread_mutex_lock(&objs.lock);
    for (uint32_t k = 0; k < GONE_RING && k < objs.gone_next; k++) {
        uint32_t i = (objs.gone_next - 1 - k) % GONE_RING;
        if (objs.gone[i].addr != addr)
            continue;
        snprintf(out, n, "kind %u %s by guest thread %llu %.3f ms ago", objs.gone[i].kind,
                 objs.gone[i].how ? "replaced" : "finalized", (unsigned long long)objs.gone[i].tid,
                 (double)(now - objs.gone[i].ns) / 1e6);
        found = true;
        break;
    }
    pthread_mutex_unlock(&objs.lock);
    return found;
}

static uint32_t hash(uint64_t a) { return (uint32_t)((a >> 3) * 0x9E3779B97F4A7C15ull >> 32); }

static void grow_locked(void)
{
    uint32_t cap = objs.cap ? objs.cap * 2 : MAP_MIN;
    nvn_obj **s = calloc(cap, sizeof *s);
    if (!s)
        return;
    for (uint32_t i = 0; i < objs.cap; i++) {
        nvn_obj *o = objs.slots[i];
        if (!o || o == NVN_TOMB)
            continue;
        uint32_t k = hash(o->addr) & (cap - 1);
        while (s[k])
            k = (k + 1) & (cap - 1);
        s[k] = o;
    }
    free(objs.slots);
    objs.slots = s;
    objs.cap = cap;
    objs.filled = objs.used;
}

nvn_obj *nvn_obj_new(uint64_t addr, uint32_t kind, size_t size)
{
    nvn_obj *o = calloc(1, size);
    if (!o)
        return NULL;
    o->addr = addr;
    o->kind = kind;
    o->refs = 1;
    return o;
}

nvn_obj *nvn_obj_publish(nvn_obj *o)
{
    if (!o)
        return NULL;
    uint64_t addr = o->addr;
    pthread_mutex_lock(&objs.lock);
    if ((objs.filled + 1) * 2 > objs.cap)
        grow_locked();
    if (!objs.cap) {
        pthread_mutex_unlock(&objs.lock);
        nvn_obj_release(o);
        return NULL;
    }
    nvn_obj *old = NULL;
    uint32_t k = hash(addr) & (objs.cap - 1), free_slot = UINT32_MAX;
    for (nvn_obj *e; (e = objs.slots[k]) != NULL; k = (k + 1) & (objs.cap - 1)) {
        if (e == NVN_TOMB) {
            if (free_slot == UINT32_MAX)
                free_slot = k;
        } else if (e->addr == addr) {
            old = e;
            break;
        }
    }
    if (old) {
        objs.slots[k] = o;
        note_gone_locked(old, 1);
    } else {
        if (free_slot != UINT32_MAX)
            k = free_slot;
        if (!objs.slots[k])
            objs.filled++;
        objs.slots[k] = o;
        objs.used++;
    }
    pthread_mutex_unlock(&objs.lock);
    if (old)
        nvn_obj_release(old);
    return o;
}

nvn_obj *nvn_obj_get(uint64_t addr, uint32_t kind)
{
    nvn_obj *found = NULL;
    pthread_mutex_lock(&objs.lock);
    if (objs.cap) {
        uint32_t k = hash(addr) & (objs.cap - 1);
        for (nvn_obj *o; (o = objs.slots[k]) != NULL; k = (k + 1) & (objs.cap - 1))
            if (o != NVN_TOMB && o->addr == addr) {
                found = o;
                break;
            }
    }
    pthread_mutex_unlock(&objs.lock);
    return found && (!kind || found->kind == kind) ? found : NULL;
}

nvn_obj *nvn_obj_acquire(uint64_t addr, uint32_t kind)
{
    nvn_obj *found = NULL;
    pthread_mutex_lock(&objs.lock);
    if (objs.cap) {
        uint32_t k = hash(addr) & (objs.cap - 1);
        for (nvn_obj *o; (o = objs.slots[k]) != NULL; k = (k + 1) & (objs.cap - 1))
            if (o != NVN_TOMB && o->addr == addr) {
                if (!kind || o->kind == kind) {
                    found = o;
                    found->refs++;
                }
                break;
            }
    }
    pthread_mutex_unlock(&objs.lock);
    return found;
}

void nvn_obj_release(nvn_obj *o)
{
    if (!o)
        return;
    pthread_mutex_lock(&objs.lock);
    bool last = --o->refs == 0;
    pthread_mutex_unlock(&objs.lock);
    if (!last)
        return;
    if (o->drop)
        o->drop(o);
    free(o);
}

void nvn_obj_free(nvn_obj *o)
{
    if (!o)
        return;
    bool unlinked = false;
    pthread_mutex_lock(&objs.lock);
    for (uint32_t k = hash(o->addr) & (objs.cap - 1); objs.slots[k]; k = (k + 1) & (objs.cap - 1))
        if (objs.slots[k] == o) {
            objs.slots[k] = NVN_TOMB;
            objs.used--;
            note_gone_locked(o, 0);
            unlinked = true;
            break;
        }
    pthread_mutex_unlock(&objs.lock);
    if (unlinked)
        nvn_obj_release(o);
}
