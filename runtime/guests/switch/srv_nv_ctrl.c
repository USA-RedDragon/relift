/**
 * @file srv_nv_ctrl.c
 * @brief Syncpoints and /dev/nvhost-ctrl: reading and incrementing
 * syncpoints, waiting for them through kernel events, and the event slots.
 *
 * Ioctl numbers, argument layouts, the event state machine and the results
 * follow Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors)
 * NvHostCtrlDeviceFile, NvHostEvent and NvHostSyncpt. The counters live in
 * memory; with no GPU a counter moves when the guest increments it or when
 * work submitted to a channel is marked complete.
 */
#include "nv.h"

#include <stdlib.h>
#include <string.h>

#define VBLANK0_SYNCPOINT 26
#define VBLANK1_SYNCPOINT 27
#define CONFIG_SIZE (0x41 + 0x41 + 0x101)

enum ev_state { EV_AVAILABLE = 0, EV_WAITING = 1, EV_CANCELLING = 2, EV_SIGNALING = 3, EV_SIGNALED = 4, EV_CANCELLED = 5 };

typedef struct ctrl_event {
    uint32_t state;
    nv_fence fence;
    hos_revent *ev;
} ctrl_event;

typedef struct ctrl_priv {
    ctrl_event events[NV_EVENTS];
} ctrl_priv;

static uint32_t sp_min[NV_SYNCPOINTS];
static uint32_t sp_max[NV_SYNCPOINTS];
static bool sp_client[NV_SYNCPOINTS];
static bool sp_assigned[NV_SYNCPOINTS];
static bool sp_inited;
static nv_file *ctrl_files;

static void sp_init(void)
{
    if (sp_inited)
        return;
    sp_inited = true;
    memset(sp_min, 0, sizeof sp_min);
    memset(sp_max, 0, sizeof sp_max);
    memset(sp_client, 0, sizeof sp_client);
    memset(sp_assigned, 0, sizeof sp_assigned);
    sp_assigned[VBLANK0_SYNCPOINT] = sp_client[VBLANK0_SYNCPOINT] = true;
    sp_assigned[VBLANK1_SYNCPOINT] = sp_client[VBLANK1_SYNCPOINT] = true;
}

void nv_sync_reset(void)
{
    sp_inited = false;
}

bool nv_sync_valid(uint32_t id)
{
    return id < NV_SYNCPOINTS;
}

uint32_t nv_sync_alloc(bool client_managed)
{
    sp_init();
    for (uint32_t i = 1; i < NV_SYNCPOINTS; i++) {
        if (!sp_assigned[i]) {
            sp_assigned[i] = true;
            sp_client[i] = client_managed;
            return i;
        }
    }
    return 0;
}

void nv_sync_release(uint32_t id)
{
    sp_init();
    if (id == 0 || id >= NV_SYNCPOINTS || !sp_assigned[id])
        return;
    sp_assigned[id] = false;
    sp_client[id] = false;
    sp_max[id] = sp_min[id];
}

uint32_t nv_sync_min(uint32_t id)
{
    sp_init();
    return sp_min[id];
}

uint32_t nv_sync_max(uint32_t id)
{
    sp_init();
    return sp_max[id];
}

uint32_t nv_sync_max_add(uint32_t id, uint32_t n)
{
    sp_init();
    sp_max[id] += n;
    return sp_max[id];
}

bool nv_sync_expired(uint32_t id, uint32_t threshold)
{
    sp_init();
    uint32_t min_diff = sp_min[id] - threshold;
    uint32_t max_diff = sp_max[id] - threshold;
    if (sp_client[id])
        return (int32_t)min_diff >= 0;
    return max_diff >= min_diff;
}

static void ctrl_wake(uint32_t id)
{
    for (nv_file *f = ctrl_files; f; f = f->next_ctrl) {
        ctrl_priv *c = f->priv;
        for (uint32_t i = 0; i < NV_EVENTS; i++) {
            ctrl_event *e = &c->events[i];
            if (e->ev && e->state == EV_WAITING && e->fence.id == id && nv_sync_expired(id, e->fence.value)) {
                e->state = EV_SIGNALED;
                rl_hos_event_signal(e->ev);
            }
        }
    }
}

void nv_sync_complete(uint32_t id)
{
    sp_init();
    sp_min[id] = sp_max[id];
    ctrl_wake(id);
}

static int32_t event_unregister(ctrl_priv *c, uint32_t id)
{
    if (id >= NV_EVENTS)
        return NVE_INVALID_INPUT;
    ctrl_event *e = &c->events[id];
    if (!e->ev)
        return NVE_OK;
    if (e->state == EV_AVAILABLE || e->state == EV_CANCELLED || e->state == EV_SIGNALED) {
        hos_kobj_unref(&e->ev->obj);
        memset(e, 0, sizeof *e);
        return NVE_OK;
    }
    return NVE_BUSY;
}

static int32_t event_register(ctrl_priv *c, uint32_t id)
{
    int32_t rc = event_unregister(c, id);
    if (rc != NVE_OK)
        return rc;
    ctrl_event *e = &c->events[id];
    e->ev = rl_hos_event_create();
    if (!e->ev)
        return NVE_OUT_OF_MEMORY;
    e->state = EV_AVAILABLE;
    e->fence.id = 0;
    return NVE_OK;
}

static bool event_reusable(const ctrl_event *e)
{
    return e->state == EV_AVAILABLE || e->state == EV_SIGNALED || e->state == EV_CANCELLED;
}

static ctrl_event *event_free_slot(ctrl_priv *c, uint32_t id, uint32_t *index)
{
    uint32_t found = NV_EVENTS, empty = NV_EVENTS;
    for (uint32_t i = 0; i < NV_EVENTS; i++) {
        ctrl_event *e = &c->events[i];
        if (e->ev) {
            if (event_reusable(e)) {
                found = i;
                if (e->fence.id == id) {
                    *index = i;
                    return e;
                }
            }
        } else if (empty == NV_EVENTS) {
            empty = i;
        }
    }
    if (empty < NV_EVENTS) {
        *index = empty;
        if (event_register(c, empty) != NVE_OK)
            return NULL;
        return &c->events[empty];
    }
    if (found < NV_EVENTS) {
        *index = found;
        return &c->events[found];
    }
    return NULL;
}

static int32_t event_wait(ctrl_priv *c, nv_fence *fence, uint32_t *value, int32_t timeout, bool async, bool wait_cmd)
{
    if (fence->id >= NV_SYNCPOINTS)
        return NVE_INVALID_INPUT;
    if (nv_sync_expired(fence->id, fence->value)) {
        *value = nv_sync_min(fence->id);
        return NVE_OK;
    }
    if (timeout == 0)
        return NVE_TRY_AGAIN;
    if (!async)
        *value = 0;
    uint32_t index = NV_EVENTS;
    ctrl_event *e;
    if (async) {
        index = *value;
        if (index >= NV_EVENTS)
            return NVE_INVALID_INPUT;
        e = c->events[index].ev ? &c->events[index] : NULL;
    } else {
        e = event_free_slot(c, fence->id, &index);
    }
    if (!e || !event_reusable(e))
        return NVE_INVALID_INPUT;
    e->fence = *fence;
    e->state = EV_WAITING;
    rl_hos_event_clear(e->ev);
    *value = wait_cmd ? (((fence->id & 0xFFF) << 16) | 0x10000000u) : (fence->id << 4);
    *value |= index;
    return NVE_TRY_AGAIN;
}

typedef struct wait_args {
    nv_fence fence;
    int32_t timeout;
} wait_args;

typedef struct wait_ex_args {
    wait_args input;
    uint32_t value;
} wait_ex_args;

typedef struct event_wait_args {
    nv_fence fence;
    int32_t timeout;
    uint32_t value;
} event_wait_args;

_Static_assert(sizeof(wait_args) == 12, "syncpt wait");
_Static_assert(sizeof(wait_ex_args) == 16, "syncpt wait ex");
_Static_assert(sizeof(event_wait_args) == 16, "event wait");

static ctrl_priv *priv_of(nv_file *f) { return f->priv; }

static int32_t ctrl_syncpt_read(nv_file *f, nv_args *a)
{
    (void)f;
    nv_fence *p = (nv_fence *)(void *)a->data;
    if (p->id >= NV_SYNCPOINTS)
        return NVE_INVALID_INPUT;
    p->value = nv_sync_min(p->id);
    return NVE_OK;
}

static int32_t ctrl_syncpt_read_max(nv_file *f, nv_args *a)
{
    (void)f;
    nv_fence *p = (nv_fence *)(void *)a->data;
    if (p->id >= NV_SYNCPOINTS)
        return NVE_INVALID_INPUT;
    p->value = nv_sync_max(p->id);
    return NVE_OK;
}

static int32_t ctrl_syncpt_incr(nv_file *f, nv_args *a)
{
    (void)f;
    uint32_t id;
    memcpy(&id, a->data, 4);
    if (id >= NV_SYNCPOINTS)
        return NVE_INVALID_INPUT;
    sp_init();
    if (sp_client[id])
        nv_sync_max_add(id, 1);
    sp_min[id]++;
    ctrl_wake(id);
    return NVE_OK;
}

static int32_t ctrl_syncpt_wait(nv_file *f, nv_args *a)
{
    wait_args *p = (wait_args *)(void *)a->data;
    uint32_t dummy = 0;
    return event_wait(priv_of(f), &p->fence, &dummy, p->timeout, false, false);
}

static int32_t ctrl_syncpt_wait_ex(nv_file *f, nv_args *a)
{
    wait_ex_args *p = (wait_ex_args *)(void *)a->data;
    return event_wait(priv_of(f), &p->input.fence, &p->value, p->input.timeout, false, false);
}

static int32_t ctrl_get_config(nv_file *f, nv_args *a)
{
    (void)f;
    (void)a;
    return NVE_INVALID_INPUT;
}

static int32_t ctrl_event_signal(nv_file *f, nv_args *a)
{
    uint32_t v;
    memcpy(&v, a->data, 4);
    uint32_t id = v & 0xFFFF;
    if (id >= NV_EVENTS)
        return NVE_INVALID_INPUT;
    ctrl_event *e = &priv_of(f)->events[id];
    if (!e->ev)
        return NVE_INVALID_INPUT;
    e->state = EV_CANCELLED;
    rl_hos_event_clear(e->ev);
    return NVE_OK;
}

static int32_t ctrl_event_wait(nv_file *f, nv_args *a)
{
    event_wait_args *p = (event_wait_args *)(void *)a->data;
    return event_wait(priv_of(f), &p->fence, &p->value, p->timeout, false, true);
}

static int32_t ctrl_event_wait_async(nv_file *f, nv_args *a)
{
    event_wait_args *p = (event_wait_args *)(void *)a->data;
    return event_wait(priv_of(f), &p->fence, &p->value, p->timeout, true, false);
}

static int32_t ctrl_event_register(nv_file *f, nv_args *a)
{
    uint32_t id;
    memcpy(&id, a->data, 4);
    return event_register(priv_of(f), id);
}

static int32_t ctrl_event_unregister(nv_file *f, nv_args *a)
{
    uint32_t id;
    memcpy(&id, a->data, 4);
    return event_unregister(priv_of(f), id);
}

static int32_t ctrl_event_kill(nv_file *f, nv_args *a)
{
    uint64_t mask;
    memcpy(&mask, a->data, 8);
    int32_t result = NVE_OK;
    for (uint32_t i = 0; i < NV_EVENTS; i++) {
        if (mask & (1ull << i)) {
            int32_t rc = event_unregister(priv_of(f), i);
            if (rc != NVE_OK)
                result = rc;
        }
    }
    return result;
}

static int32_t ctrl_open(nv_file *f)
{
    ctrl_priv *c = calloc(1, sizeof *c);
    if (!c)
        return NVE_OUT_OF_MEMORY;
    f->priv = c;
    f->next_ctrl = ctrl_files;
    ctrl_files = f;
    return NVE_OK;
}

static void ctrl_close(nv_file *f)
{
    ctrl_priv *c = f->priv;
    for (nv_file **p = &ctrl_files; *p; p = &(*p)->next_ctrl) {
        if (*p == f) {
            *p = f->next_ctrl;
            break;
        }
    }
    for (uint32_t i = 0; i < NV_EVENTS; i++)
        if (c->events[i].ev)
            hos_kobj_unref(&c->events[i].ev->obj);
    free(c);
    f->priv = NULL;
}

static int32_t ctrl_query_event(nv_file *f, uint32_t id, hos_revent **out)
{
    ctrl_priv *c = priv_of(f);
    uint32_t slot, syncpt;
    if ((id >> 28) == 1) {
        slot = id & 0xFFFF;
        syncpt = (id >> 16) & 0xFFF;
    } else {
        slot = id & 0xF;
        syncpt = id >> 4;
    }
    if (slot >= NV_EVENTS || !c->events[slot].ev || c->events[slot].fence.id != syncpt)
        return NVE_INVALID_INPUT;
    *out = c->events[slot].ev;
    return NVE_OK;
}

#define NC(nr, stub, min, name, fn) { NV_MAGIC_HOST_CUSTOM, (nr), NV_V_IOCTL, (stub), (min), (name), (fn) }
static const nv_ioctl_entry ctrl_ioctls[] = {
    NC(0x14, false, 8, "SyncptRead", ctrl_syncpt_read),
    NC(0x15, false, 4, "SyncptIncr", ctrl_syncpt_incr),
    NC(0x16, false, 12, "SyncptWait", ctrl_syncpt_wait),
    NC(0x19, false, 16, "SyncptWaitEx", ctrl_syncpt_wait_ex),
    NC(0x1A, false, 8, "SyncptReadMax", ctrl_syncpt_read_max),
    NC(0x1B, true, CONFIG_SIZE, "GetConfig", ctrl_get_config),
    NC(0x1C, false, 4, "EventSignal", ctrl_event_signal),
    NC(0x1D, false, 16, "EventWait", ctrl_event_wait),
    NC(0x1E, false, 16, "EventWaitAsync", ctrl_event_wait_async),
    NC(0x1F, false, 4, "EventRegister", ctrl_event_register),
    NC(0x20, false, 4, "EventUnregister", ctrl_event_unregister),
    NC(0x21, false, 8, "EventKill", ctrl_event_kill),
};
#undef NC

const nv_dev_class nv_dev_ctrl = {
    .path = "/dev/nvhost-ctrl",
    .ioctls = ctrl_ioctls,
    .nioctls = sizeof ctrl_ioctls / sizeof ctrl_ioctls[0],
    .open = ctrl_open,
    .close = ctrl_close,
    .query_event = ctrl_query_event,
};
