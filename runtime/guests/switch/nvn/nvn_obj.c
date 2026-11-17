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
#include <stdlib.h>

#define MAP_MIN 64u

static struct {
    pthread_mutex_t lock;
    nvn_obj **slots;
    uint32_t cap, used, filled;
} objs = { .lock = PTHREAD_MUTEX_INITIALIZER };

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
    nvn_obj_free(nvn_obj_get(addr, 0));
    nvn_obj *o = calloc(1, size);
    if (!o)
        return NULL;
    o->addr = addr;
    o->kind = kind;
    pthread_mutex_lock(&objs.lock);
    if ((objs.filled + 1) * 2 > objs.cap)
        grow_locked();
    if (!objs.cap) {
        pthread_mutex_unlock(&objs.lock);
        free(o);
        return NULL;
    }
    uint32_t k = hash(addr) & (objs.cap - 1);
    while (objs.slots[k] && objs.slots[k] != NVN_TOMB)
        k = (k + 1) & (objs.cap - 1);
    if (!objs.slots[k])
        objs.filled++;
    objs.slots[k] = o;
    objs.used++;
    pthread_mutex_unlock(&objs.lock);
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

void nvn_obj_free(nvn_obj *o)
{
    if (!o)
        return;
    pthread_mutex_lock(&objs.lock);
    for (uint32_t k = hash(o->addr) & (objs.cap - 1); objs.slots[k]; k = (k + 1) & (objs.cap - 1))
        if (objs.slots[k] == o) {
            objs.slots[k] = NVN_TOMB;
            objs.used--;
            break;
        }
    pthread_mutex_unlock(&objs.lock);
    if (o->drop)
        o->drop(o);
    free(o);
}
