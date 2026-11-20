/**
 * @file srv_gsv_syncpt.c
 * @brief The Switch 2 sync point session (nn::syncpt, gsv 202): counters, and the slot events that fire when a
 * counter reaches a value.
 *
 * A SyncCounterId is the bitwise NOT of a sync point id. Ids below NV_SYNCPOINTS are the nvhost sync points the
 * GPU emulation owns; ids from NV_SYNCPOINTS up are counters of the supervisor's own services (the video
 * decoders count their finished frames). Command ids and layouts follow the lifted nn::syncpt SDK and
 * Eglantine's description (values only).
 */
#include "nv.h"
#include "services.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define COUNTERS_MAX 64u
#define SLOTS_MAX 256u

static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_counter[COUNTERS_MAX];
static bool g_counter_used[COUNTERS_MAX];

typedef struct slot {
    hos_revent *ev;
    bool pending;
    uint32_t id;
    uint64_t value;
    bool wide;
} slot;

typedef struct syncpt_session {
    struct syncpt_session *next;
    slot slots[SLOTS_MAX];
} syncpt_session;

static syncpt_session *g_sessions;

static bool reached(uint32_t id, uint64_t value, bool wide, bool nv_held)
{
    if (id >= NV_SYNCPOINTS) {
        if (id - NV_SYNCPOINTS >= COUNTERS_MAX)
            return false;
        uint64_t have = g_counter[id - NV_SYNCPOINTS];
        return wide ? have >= value : (int32_t)((uint32_t)have - (uint32_t)value) >= 0;
    }
    if (!nv_held)
        nv_lock();
    bool ok = nv_sync_valid(id) && nv_sync_expired(id, (uint32_t)value);
    if (!nv_held)
        nv_unlock();
    return ok;
}

static void fire_pending(bool nv_held)
{
    for (syncpt_session *s = g_sessions; s; s = s->next)
        for (uint32_t i = 0; i < SLOTS_MAX; i++) {
            slot *t = &s->slots[i];
            if (t->pending && reached(t->id, t->value, t->wide, nv_held)) {
                t->pending = false;
                rl_hos_event_signal(t->ev);
            }
        }
}

void rl_gsv_syncpt_changed(void)
{
    pthread_mutex_lock(&g_mu);
    fire_pending(true);
    pthread_mutex_unlock(&g_mu);
}

uint32_t rl_gsv_counter_new(void)
{
    pthread_mutex_lock(&g_mu);
    uint32_t id = UINT32_MAX;
    for (uint32_t i = 0; i < COUNTERS_MAX; i++)
        if (!g_counter_used[i]) {
            g_counter_used[i] = true;
            g_counter[i] = 0;
            id = NV_SYNCPOINTS + i;
            break;
        }
    pthread_mutex_unlock(&g_mu);
    return id;
}

void rl_gsv_counter_free(uint32_t id)
{
    if (id < NV_SYNCPOINTS || id - NV_SYNCPOINTS >= COUNTERS_MAX)
        return;
    pthread_mutex_lock(&g_mu);
    g_counter_used[id - NV_SYNCPOINTS] = false;
    pthread_mutex_unlock(&g_mu);
}

uint64_t rl_gsv_counter_get(uint32_t id)
{
    if (id < NV_SYNCPOINTS) {
        nv_lock();
        uint64_t v = nv_sync_valid(id) ? nv_sync_min(id) : 0;
        nv_unlock();
        return v;
    }
    if (id - NV_SYNCPOINTS >= COUNTERS_MAX)
        return 0;
    pthread_mutex_lock(&g_mu);
    uint64_t v = g_counter[id - NV_SYNCPOINTS];
    pthread_mutex_unlock(&g_mu);
    return v;
}

void rl_gsv_counter_signal(uint32_t id, uint64_t value)
{
    if (id < NV_SYNCPOINTS || id - NV_SYNCPOINTS >= COUNTERS_MAX)
        return;
    pthread_mutex_lock(&g_mu);
    if (g_counter[id - NV_SYNCPOINTS] < value)
        g_counter[id - NV_SYNCPOINTS] = value;
    fire_pending(false);
    pthread_mutex_unlock(&g_mu);
}

/** @brief Ping (0): answers the u64 it gets (the SDK sends the system tick). */
static rl_result syncpt_ping(rl_ipc_req *r)
{
    uint64_t v;
    if (!rl_srv_in(r, &v, 8))
        return SF_RESULT_INVALID_HEADER;
    uint64_t *out = rl_ipc_out(r, 8);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = v;
    return KR_SUCCESS;
}

/** @brief Command 1 (unnamed): a u32 below 0x400 that the SDK passes back, capped at 63, to command 21. */
static rl_result syncpt_cmd_1(rl_ipc_req *r)
{
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    return KR_SUCCESS;
}

/** @brief Command 21 (unnamed): takes the u32 from command 1. */
static rl_result syncpt_cmd_21(rl_ipc_req *r)
{
    uint32_t v;
    return rl_srv_in(r, &v, 4) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

static slot *slot_of(rl_ipc_req *r, uint32_t index)
{
    syncpt_session *s = r->self->state;
    if (index >= SLOTS_MAX)
        return NULL;
    slot *t = &s->slots[index];
    if (!t->ev)
        t->ev = rl_hos_event_create();
    return t->ev ? t : NULL;
}

/** @brief Command 22 (GetSlotEvent): a u32 slot; one copied handle, the slot's event (NvOsEventPool multi-waits on them). */
static rl_result syncpt_get_slot_event(rl_ipc_req *r)
{
    uint32_t index;
    if (!rl_srv_in(r, &index, 4))
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&g_mu);
    slot *t = slot_of(r, index);
    pthread_mutex_unlock(&g_mu);
    if (!t)
        return SF_RESULT_INVALID_HEADER;
    return rl_ipc_push_copy(r, &t->ev->obj);
}

/** @brief Command 103 (QueryCounter): a u32 counter; its u64 value and a u32 zero. */
static rl_result syncpt_query_counter(rl_ipc_req *r)
{
    uint32_t counter;
    if (!rl_srv_in(r, &counter, 4))
        return SF_RESULT_INVALID_HEADER;
    uint64_t v = rl_gsv_counter_get(~counter);
    uint32_t *out = rl_ipc_out(r, 12);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    out[0] = (uint32_t)v;
    out[1] = (uint32_t)(v >> 32);
    return KR_SUCCESS;
}

/**
 * @brief Command 201 (RequestSlotEvent): a u32 slot, a u32 counter, a u64 value and a u32 mode (0 compares 32 bits
 * wrapping, 1 compares 64 bits). The slot's event is cleared, then signaled once the counter reaches the value.
 */
static rl_result syncpt_request_slot_event(rl_ipc_req *r)
{
    struct {
        uint32_t slot, counter;
        uint64_t value;
        uint32_t mode;
    } in;
    if (!rl_srv_in(r, &in, 20))
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&g_mu);
    slot *t = slot_of(r, in.slot);
    if (t) {
        rl_hos_event_clear(t->ev);
        t->id = ~in.counter;
        t->value = in.value;
        t->wide = in.mode == 1;
        t->pending = true;
        fire_pending(false);
    }
    pthread_mutex_unlock(&g_mu);
    return t ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

/** @brief Command 202 (CancelSlotEvent): a u32 slot. The SDK waits on the event after the reply, so a cancel signals it. */
static rl_result syncpt_cancel_slot_event(rl_ipc_req *r)
{
    uint32_t index;
    if (!rl_srv_in(r, &index, 4))
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&g_mu);
    slot *t = slot_of(r, index);
    if (t) {
        t->pending = false;
        rl_hos_event_signal(t->ev);
    }
    pthread_mutex_unlock(&g_mu);
    return t ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

static void syncpt_destroy(void *state)
{
    syncpt_session *s = state;
    pthread_mutex_lock(&g_mu);
    for (syncpt_session **p = &g_sessions; *p; p = &(*p)->next)
        if (*p == s) {
            *p = s->next;
            break;
        }
    pthread_mutex_unlock(&g_mu);
    for (uint32_t i = 0; i < SLOTS_MAX; i++)
        if (s->slots[i].ev)
            hos_kobj_unref(&s->slots[i].ev->obj);
    free(s);
}

static const rl_ipc_cmd syncpt_cmds[] = {
    { 0, "Ping", syncpt_ping },
    { 1, "Cmd1", syncpt_cmd_1 },
    { 21, "Cmd21", syncpt_cmd_21 },
    { 22, "GetSlotEvent", syncpt_get_slot_event },
    { 103, "QueryCounter", syncpt_query_counter },
    { 201, "RequestSlotEvent", syncpt_request_slot_event },
    { 202, "CancelSlotEvent", syncpt_cancel_slot_event },
};

static const rl_ipc_class syncpt_class = { "nn::gsv::SyncptSession", syncpt_cmds,
                                           sizeof syncpt_cmds / sizeof syncpt_cmds[0], 0, syncpt_destroy };

rl_result rl_gsv_open_syncpt_session(rl_ipc_req *r)
{
    syncpt_session *s = calloc(1, sizeof *s);
    if (!s)
        return KR_OUT_OF_RESOURCE;
    rl_ipc_object *o = rl_srv_object(&syncpt_class, s);
    if (!o) {
        free(s);
        return KR_OUT_OF_RESOURCE;
    }
    pthread_mutex_lock(&g_mu);
    s->next = g_sessions;
    g_sessions = s;
    pthread_mutex_unlock(&g_mu);
    uint32_t h;
    rl_result rc = rl_ipc_session_open(o, &h);
    rl_ipc_object_unref(o);
    if (rc == KR_SUCCESS)
        r->out_move[r->n_out_move++] = h;
    return rc;
}
