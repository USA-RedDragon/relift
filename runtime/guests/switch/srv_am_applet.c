/**
 * @file srv_am_applet.c
 * @brief ILibraryAppletCreator and ILibraryAppletAccessor: the library
 * applets an application starts (the software keyboard and others), with
 * their four storage channels, their events and their end.
 *
 * An application creates the applet, takes its state changed event, pushes
 * its arguments to the in channel, starts it, and waits for the state
 * changed event; then it reads the result (GetResult) and pops the out
 * channel. Interactive channels carry requests both ways while it runs.
 * relift runs the applets it implements (am_applet_impl) in the runtime; an
 * applet it does not implement ends at Start with LibraryAppletTerminated,
 * which the SDK's JoinLibraryApplet reports as a canceled applet.
 *
 * Command ids follow Ryujinx (MIT, Copyright (c) Ryujinx Team and
 * Contributors), libnx (ISC, Copyright (c) libnx Authors) and
 * switchbrew.org (Applet Manager services). The SDK 22 ids 90 and 170 are
 * named from what the SDK's nn::applet does with them.
 */
#include "srv_am_priv.h"

#include <pthread.h>
#include <stdlib.h>

/** @brief Storages a channel holds at most. */
#define CHANNEL_MAX 16u

enum { CH_IN, CH_OUT, CH_INTERACTIVE_IN, CH_INTERACTIVE_OUT, CH_COUNT };

typedef struct channel {
    rl_ipc_object *items[CHANNEL_MAX];
    uint32_t count;
} channel;

struct am_applet {
    pthread_mutex_t mu;
    uint32_t id;
    uint32_t mode;
    const am_applet_impl *impl;
    void *state;
    channel ch[CH_COUNT];
    hos_revent *state_changed;
    hos_revent *out_event;
    hos_revent *interactive_out_event;
    hos_revent *unknown170;
    uint64_t indirect_layer;
    rl_result result;
    bool started;
    bool completed;
    bool stopped;
};

void *am_applet_state(const am_applet *a) { return a->state; }

void am_applet_set_state(am_applet *a, void *state) { a->state = state; }

uint32_t am_applet_mode(const am_applet *a) { return a->mode; }

/** @brief Appends o (taking its reference) to channel c; false when the channel is full. Lock held. */
static bool channel_push_locked(channel *c, rl_ipc_object *o)
{
    if (c->count == CHANNEL_MAX)
        return false;
    c->items[c->count++] = o;
    return true;
}

/** @brief Removes the oldest storage of c, or NULL. Lock held. */
static rl_ipc_object *channel_pop_locked(channel *c)
{
    if (!c->count)
        return NULL;
    rl_ipc_object *o = c->items[0];
    memmove(&c->items[0], &c->items[1], sizeof c->items[0] * (c->count - 1));
    c->count--;
    return o;
}

static rl_ipc_object *pop(am_applet *a, uint32_t ch, hos_revent *ev)
{
    pthread_mutex_lock(&a->mu);
    rl_ipc_object *o = channel_pop_locked(&a->ch[ch]);
    if (ev && !a->ch[ch].count)
        rl_hos_event_clear(ev);
    pthread_mutex_unlock(&a->mu);
    return o;
}

static bool push(am_applet *a, uint32_t ch, hos_revent *ev, const void *data, uint64_t size)
{
    rl_ipc_object *o = am_storage_new(data, size);
    if (!o)
        return false;
    pthread_mutex_lock(&a->mu);
    bool ok = !a->stopped && channel_push_locked(&a->ch[ch], o);
    pthread_mutex_unlock(&a->mu);
    if (!ok) {
        rl_ipc_object_unref(o);
        return false;
    }
    rl_hos_event_signal(ev);
    return true;
}

rl_ipc_object *am_applet_pop_in(am_applet *a) { return pop(a, CH_IN, NULL); }

rl_ipc_object *am_applet_pop_interactive_in(am_applet *a) { return pop(a, CH_INTERACTIVE_IN, NULL); }

bool am_applet_push_out(am_applet *a, const void *data, uint64_t size)
{
    return push(a, CH_OUT, a->out_event, data, size);
}

bool am_applet_push_interactive_out(am_applet *a, const void *data, uint64_t size)
{
    return push(a, CH_INTERACTIVE_OUT, a->interactive_out_event, data, size);
}

void am_applet_exit(am_applet *a, rl_result result)
{
    pthread_mutex_lock(&a->mu);
    bool first = !a->completed;
    if (first) {
        a->completed = true;
        a->result = result;
    }
    pthread_mutex_unlock(&a->mu);
    if (first)
        rl_hos_event_signal(a->state_changed);
}

/** @brief Tells a running implementation to stop, once. */
static void stop_impl(am_applet *a)
{
    pthread_mutex_lock(&a->mu);
    bool run = a->started && !a->stopped;
    a->stopped = true;
    pthread_mutex_unlock(&a->mu);
    if (run && a->impl && a->impl->stop)
        a->impl->stop(a);
}

static void lac_destroy(void *state)
{
    am_applet *a = state;
    stop_impl(a);
    if (a->impl && a->impl->destroy)
        a->impl->destroy(a);
    for (uint32_t c = 0; c < CH_COUNT; c++)
        while (a->ch[c].count)
            rl_ipc_object_unref(channel_pop_locked(&a->ch[c]));
    hos_revent *events[] = { a->state_changed, a->out_event, a->interactive_out_event, a->unknown170 };
    for (uint32_t i = 0; i < sizeof events / sizeof events[0]; i++)
        if (events[i])
            hos_kobj_unref(&events[i]->obj);
    pthread_mutex_destroy(&a->mu);
    free(a);
}

static am_applet *lac(const rl_ipc_req *r) { return r->self->state; }

static rl_result lac_get_state_changed_event(rl_ipc_req *r) { return am_push_event(r, lac(r)->state_changed); }
static rl_result lac_get_pop_out_data_event(rl_ipc_req *r) { return am_push_event(r, lac(r)->out_event); }
static rl_result lac_get_pop_interactive_out_data_event(rl_ipc_req *r)
{
    return am_push_event(r, lac(r)->interactive_out_event);
}

static rl_result lac_is_completed(rl_ipc_req *r)
{
    am_applet *a = lac(r);
    pthread_mutex_lock(&a->mu);
    *(uint8_t *)rl_ipc_out(r, 1) = a->completed;
    pthread_mutex_unlock(&a->mu);
    return KR_SUCCESS;
}

static rl_result lac_start(rl_ipc_req *r)
{
    am_applet *a = lac(r);
    pthread_mutex_lock(&a->mu);
    bool first = !a->started && !a->completed;
    a->started = true;
    pthread_mutex_unlock(&a->mu);
    if (!first)
        return KR_SUCCESS;
    if (a->impl) {
        a->impl->start(a);
    } else {
        rl_log(RL_LOG_WARN, "am", "library applet 0x%x (mode %u) is not implemented; it ends as terminated", a->id,
               a->mode);
        am_applet_exit(a, AM_LIBRARY_APPLET_TERMINATED);
    }
    return KR_SUCCESS;
}

/** @brief RequestExit and Terminate: a running applet stops and ends as terminated; one not started just ends. */
static rl_result lac_request_exit(rl_ipc_req *r)
{
    am_applet *a = lac(r);
    stop_impl(a);
    am_applet_exit(a, AM_LIBRARY_APPLET_TERMINATED);
    return KR_SUCCESS;
}

static rl_result lac_get_result(rl_ipc_req *r)
{
    am_applet *a = lac(r);
    pthread_mutex_lock(&a->mu);
    rl_result rc = a->result;
    pthread_mutex_unlock(&a->mu);
    return rc;
}

static rl_result push_in(rl_ipc_req *r, uint32_t ch)
{
    am_applet *a = lac(r);
    if (r->n_in_objects != 1 || !am_storage_is(r->in_objects[0]))
        return SF_RESULT_INVALID_IN_OBJECT;
    rl_ipc_object_ref(r->in_objects[0]);
    pthread_mutex_lock(&a->mu);
    bool ok = channel_push_locked(&a->ch[ch], r->in_objects[0]);
    bool running = a->started && !a->completed && !a->stopped;
    pthread_mutex_unlock(&a->mu);
    if (!ok) {
        rl_ipc_object_unref(r->in_objects[0]);
        return AM_OUT_OF_BOUNDS;
    }
    if (ch == CH_INTERACTIVE_IN && running && a->impl && a->impl->interactive)
        a->impl->interactive(a);
    return KR_SUCCESS;
}

static rl_result lac_push_in_data(rl_ipc_req *r) { return push_in(r, CH_IN); }
static rl_result lac_push_interactive_in_data(rl_ipc_req *r) { return push_in(r, CH_INTERACTIVE_IN); }

static rl_result pop_out(rl_ipc_req *r, uint32_t ch, hos_revent *ev)
{
    rl_ipc_object *o = pop(lac(r), ch, ev);
    if (!o)
        return AM_NOT_AVAILABLE;
    return rl_ipc_push_object(r, o);
}

static rl_result lac_pop_out_data(rl_ipc_req *r) { return pop_out(r, CH_OUT, lac(r)->out_event); }
static rl_result lac_pop_interactive_out_data(rl_ipc_req *r)
{
    return pop_out(r, CH_INTERACTIVE_OUT, lac(r)->interactive_out_event);
}

static rl_result lac_stubbed(rl_ipc_req *r)
{
    (void)r;
    return AM_STUBBED;
}

/**
 * @brief 90: two flags the SDK keeps per applet handle from
 * nn::applet::SetStreamPlayShareEnabled and sends at StartLibraryApplet.
 * relift records no stream or play share, so it takes them and does nothing.
 */
static rl_result lac_set_stream_play_share_enabled(rl_ipc_req *r)
{
    AM_IN(uint16_t, flags);
    (void)flags;
    return KR_SUCCESS;
}

static rl_result lac_get_library_applet_info(rl_ipc_req *r)
{
    am_applet *a = lac(r);
    uint32_t *o = rl_ipc_out(r, 8);
    o[0] = a->id;
    o[1] = a->mode;
    return KR_SUCCESS;
}

static rl_result lac_get_indirect_layer_consumer_handle(rl_ipc_req *r)
{
    am_applet *a = lac(r);
    if (!a->indirect_layer)
        a->indirect_layer = 0x1000 + am_resource_user_id(rl_srv_pid(r));
    *(uint64_t *)rl_ipc_out(r, 8) = a->indirect_layer;
    return KR_SUCCESS;
}

/**
 * @brief 170 (SDK 22): a readable event. nn::applet::CreateLibraryApplet
 * attaches it to the handle, and StartLibraryApplet waits until it or
 * ISelfController's library applet launchable event is signaled before it
 * sends Start. What signals it on the console is not known; relift leaves
 * it clear, and the launchable event lets Start go.
 */
static rl_result lac_unknown170(rl_ipc_req *r) { return am_push_event(r, lac(r)->unknown170); }

static const rl_ipc_cmd lac_cmds[] = {
    { 0, "GetAppletStateChangedEvent", lac_get_state_changed_event },
    { 1, "IsCompleted", lac_is_completed },
    { 10, "Start", lac_start },
    { 20, "RequestExit", lac_request_exit },
    { 25, "Terminate", lac_request_exit },
    { 30, "GetResult", lac_get_result },
    RL_SRV_STUB(50, "SetOutOfFocusApplicationSuspendingEnabled"),
    RL_SRV_STUB(60, "PresetLibraryAppletGpuTimeSliceZero"),
    { 90, "SetStreamPlayShareEnabled", lac_set_stream_play_share_enabled },
    { 100, "PushInData", lac_push_in_data },
    { 101, "PopOutData", lac_pop_out_data },
    { 103, "PushInteractiveInData", lac_push_interactive_in_data },
    { 104, "PopInteractiveOutData", lac_pop_interactive_out_data },
    { 105, "GetPopOutDataEvent", lac_get_pop_out_data_event },
    { 106, "GetPopInteractiveOutDataEvent", lac_get_pop_interactive_out_data_event },
    { 110, "NeedsToExitProcess", lac_stubbed },
    { 120, "GetLibraryAppletInfo", lac_get_library_applet_info },
    { 150, "RequestForAppletToGetForeground", lac_stubbed },
    { 160, "GetIndirectLayerConsumerHandle", lac_get_indirect_layer_consumer_handle },
    { 170, "Unknown170", lac_unknown170 },
};

static const rl_ipc_class lac_class = {
    "nn::am::service::ILibraryAppletAccessor", lac_cmds, AM_N(lac_cmds), 0, lac_destroy
};

static const am_applet_impl *impl_for(uint32_t id)
{
    (void)id;
    return NULL;
}

/** @brief A new applet. The id's low byte names the applet; the SDK 22 sets bits above it (0x1020011 for the keyboard). */
static rl_ipc_object *lac_new(uint32_t raw_id, uint32_t mode)
{
    am_applet *a = calloc(1, sizeof *a);
    if (!a)
        return NULL;
    pthread_mutex_init(&a->mu, NULL);
    a->id = raw_id & 0xffu;
    a->mode = mode;
    a->impl = impl_for(a->id);
    a->state_changed = rl_hos_event_create();
    a->out_event = rl_hos_event_create();
    a->interactive_out_event = rl_hos_event_create();
    a->unknown170 = rl_hos_event_create();
    rl_ipc_object *o = rl_srv_object(&lac_class, a);
    if (!o)
        lac_destroy(a);
    return o;
}

static rl_result lc_create_library_applet(rl_ipc_req *r)
{
    struct {
        uint32_t applet_id;
        uint32_t mode;
    } q;
    if (!rl_srv_in(r, &q, sizeof q))
        return SF_RESULT_INVALID_HEADER;
    return am_push_object(r, lac_new(q.applet_id, q.mode));
}

static rl_result lc_create_library_applet_ex(rl_ipc_req *r)
{
    struct {
        uint32_t applet_id;
        uint32_t mode;
        uint64_t thread_id;
    } q;
    if (!rl_srv_in(r, &q, sizeof q))
        return SF_RESULT_INVALID_HEADER;
    return am_push_object(r, lac_new(q.applet_id, q.mode));
}

static rl_result lc_create_storage(rl_ipc_req *r)
{
    AM_IN(int64_t, size);
    if (size <= 0)
        return AM_OBJECT_INVALID;
    if (size > (int64_t)1 << 30)
        return AM_RESULT(600);
    return am_push_object(r, am_storage_new(NULL, (uint64_t)size));
}

static const rl_ipc_cmd lc_cmds[] = {
    { 0, "CreateLibraryApplet", lc_create_library_applet },
    { 3, "CreateLibraryAppletEx", lc_create_library_applet_ex },
    { 10, "CreateStorage", lc_create_storage },
    { 11, "CreateTransferMemoryStorage", am_create_transfer_memory_storage },
    { 12, "CreateHandleStorage", am_create_handle_storage },
};

static const rl_ipc_class lc_class = {
    "nn::am::service::ILibraryAppletCreator", lc_cmds, AM_N(lc_cmds), 0, NULL
};

rl_ipc_object *am_new_library_applet_creator(void) { return rl_srv_object(&lc_class, NULL); }
