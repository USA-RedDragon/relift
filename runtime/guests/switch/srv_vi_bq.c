/**
 * @file srv_vi_bq.c
 * @brief The buffer queue behind a layer (producer and consumer sides), the
 * layer registry and the host consumer interface.
 *
 * Ported from Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors),
 * Ryujinx.HLE/HOS/Services/SurfaceFlinger: BufferQueueCore, BufferQueueProducer,
 * BufferQueueConsumer and SurfaceFlinger's layer table.
 */
#include "hos_mods.h"
#include "vi.h"

#include <relift/ports.h>

#include <stdlib.h>
#include <time.h>

static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;
static vi_layer *layers[VI_MAX_LAYERS];
static uint32_t nlayers;
static uint64_t next_layer_id = 1;
static int32_t next_binder_id = 1;
static uint64_t render_layer;

static pthread_mutex_t cb_lock = PTHREAD_MUTEX_INITIALIZER;
static void (*frame_cb)(uint64_t, void *);
static void *frame_cb_user;
static uint64_t frames_queued;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

void vi_fence_none(uint8_t f[VI_FENCE_SIZE])
{
    memset(f, 0, VI_FENCE_SIZE);
    uint32_t invalid = 0xFFFFFFFFu;
    memcpy(f + 4, &invalid, 4);
}

static void fence_clear_count(uint8_t f[VI_FENCE_SIZE]) { memset(f, 0, 4); }

static void signal_free(vi_core *c) { rl_hos_event_signal(c->free_ev); }
static void signal_frame(vi_core *c) { rl_hos_event_signal(c->frame_ev); }

static int min_undequeued(const vi_core *c, bool async)
{
    if (!c->use_async)
        return 0;
    if (c->dequeue_cannot_block || async)
        return c->max_acquired + 1;
    return c->max_acquired;
}

static int max_buffer_count(const vi_core *c, bool async)
{
    int min_max = min_undequeued(c, async);
    int count = c->default_max > min_max ? c->default_max : min_max;
    if (c->override_max != 0)
        return c->override_max;
    for (int s = count; s < VI_SLOTS; s++)
        if (c->slots[s].state == VI_QUEUED || c->slots[s].state == VI_DEQUEUED)
            count = s + 1;
    return count;
}

static void update_max_cached(vi_core *c, int slot)
{
    if (c->max_cached <= slot)
        c->max_cached = slot + 1;
}

static void check_events(vi_core *c, int max_count)
{
    bool need_free = false, need_frame = false;
    if (max_count > 1) {
        for (int i = 0; i < max_count && i < VI_SLOTS; i++) {
            if (c->slots[i].state == VI_QUEUED)
                need_frame = true;
            else if (c->slots[i].state == VI_FREE)
                need_free = true;
        }
    }
    if (need_free)
        signal_free(c);
    else
        rl_hos_event_clear(c->free_ev);
    if (need_frame)
        signal_frame(c);
    else
        rl_hos_event_clear(c->frame_ev);
}

static void free_buffer(vi_core *c, int slot)
{
    vi_slot *s = &c->slots[slot];
    s->has_buf = false;
    if (s->state == VI_ACQUIRED)
        s->needs_cleanup = true;
    s->state = VI_FREE;
    s->frame_number = 0xFFFFFFFFull;
    s->acquire_called = false;
    fence_clear_count(s->fence);
}

static void free_all(vi_core *c)
{
    c->has_been_queued = false;
    for (int s = 0; s < VI_SLOTS; s++)
        free_buffer(c, s);
}

static bool owned_by_producer(const vi_core *c, int slot) { return c->slots[slot].state == VI_DEQUEUED; }

static bool still_tracking(const vi_core *c, const vi_item *it)
{
    const vi_slot *s = &c->slots[it->slot];
    return s->has_buf && vi_gfx_u32(&s->buf, VI_GFX_SURF_HANDLE) == vi_gfx_u32(&it->buf, VI_GFX_SURF_HANDLE);
}

static void queue_remove(vi_core *c, uint32_t i)
{
    memmove(&c->queue[i], &c->queue[i + 1], (c->nqueue - i - 1) * sizeof c->queue[0]);
    c->nqueue--;
}

static int preallocated_count(const vi_core *c)
{
    int n = 0;
    for (int i = 0; i < VI_SLOTS; i++)
        n += c->slots[i].prealloc;
    return n;
}

int vi_bq_request_buffer(vi_core *c, int slot, vi_gfx *gb, bool *has)
{
    int st = VI_S_OK;
    *has = false;
    pthread_mutex_lock(&c->lock);
    if (c->abandoned) {
        st = VI_S_NO_INIT;
    } else if (slot < 0 || slot >= VI_SLOTS || !owned_by_producer(c, slot)) {
        st = VI_S_BAD_VALUE;
    } else {
        *has = c->slots[slot].has_buf;
        if (*has)
            *gb = c->slots[slot].buf;
        c->slots[slot].request_called = true;
    }
    pthread_mutex_unlock(&c->lock);
    return st;
}

int vi_bq_set_buffer_count(vi_core *c, int count)
{
    int st = VI_S_OK;
    pthread_mutex_lock(&c->lock);
    if (c->abandoned) {
        st = VI_S_NO_INIT;
        goto out;
    }
    if (count > VI_SLOTS || count < 0) {
        st = VI_S_BAD_VALUE;
        goto out;
    }
    for (int s = 0; s < VI_SLOTS; s++) {
        if (c->slots[s].state == VI_DEQUEUED) {
            st = VI_S_BAD_VALUE;
            goto out;
        }
    }
    if (count == 0) {
        c->override_max = 0;
        pthread_cond_broadcast(&c->cv);
        goto out;
    }
    if (count < min_undequeued(c, false)) {
        st = VI_S_BAD_VALUE;
        goto out;
    }
    int pre = preallocated_count(c);
    if (pre <= 0) {
        c->nqueue = 0;
        free_all(c);
    } else if (pre < count) {
        goto out;
    }
    c->override_max = count;
    pthread_cond_broadcast(&c->cv);
    signal_free(c);
out:
    pthread_mutex_unlock(&c->lock);
    return st;
}

static int wait_for_free_slot(vi_core *c, bool async, int *free_slot, int *flags)
{
    bool again = true;
    *free_slot = -1;
    *flags = VI_S_OK;
    while (again) {
        if (c->abandoned) {
            *free_slot = -1;
            return VI_S_NO_INIT;
        }
        int max_count = max_buffer_count(c, async);
        if (async && c->override_max != 0 && c->override_max < max_count) {
            *free_slot = -1;
            return VI_S_BAD_VALUE;
        }
        if (max_count < c->max_cached) {
            for (int s = max_count; s < c->max_cached; s++) {
                if (c->slots[s].state == VI_FREE && c->slots[s].has_buf && !c->slots[s].prealloc) {
                    free_buffer(c, s);
                    *flags |= VI_S_RELEASE_ALL_BUFFERS;
                }
            }
        }
        *free_slot = -1;
        int dequeued = 0, acquired = 0;
        for (int s = 0; s < max_count; s++) {
            switch (c->slots[s].state) {
            case VI_ACQUIRED:
                acquired++;
                break;
            case VI_DEQUEUED:
                dequeued++;
                break;
            case VI_FREE:
                if (*free_slot == -1 || c->slots[s].frame_number < c->slots[*free_slot].frame_number)
                    *free_slot = s;
                break;
            default:
                break;
            }
        }
        if (c->override_max == 0 && dequeued > 0)
            return VI_S_INVALID_OPERATION;
        if (c->has_been_queued) {
            int new_undequeued = max_count - (dequeued + 1);
            if (new_undequeued < min_undequeued(c, async))
                return VI_S_INVALID_OPERATION;
        }
        bool too_many = (int)c->nqueue > max_count;
        again = *free_slot == -1 || too_many;
        if (again) {
            if (async || (c->dequeue_cannot_block && acquired < c->max_acquired)) {
                check_events(c, max_count);
                return VI_S_WOULD_BLOCK;
            }
            if (c->active)
                pthread_cond_wait(&c->cv, &c->lock);
            if (!c->active)
                break;
        }
    }
    return VI_S_OK;
}

int vi_bq_dequeue(vi_core *c, int *slot, uint8_t fence[VI_FENCE_SIZE], bool async, uint32_t w, uint32_t h, uint32_t fmt,
                  uint32_t usage)
{
    *slot = -1;
    vi_fence_none(fence);
    if ((w == 0 && h != 0) || (h == 0 && w != 0))
        return VI_S_BAD_VALUE;
    int flags = VI_S_OK;
    bool attached;
    pthread_mutex_lock(&c->lock);
    if (fmt == 0)
        fmt = c->default_format;
    usage |= c->consumer_usage;
    int s;
    int st = wait_for_free_slot(c, async, &s, &flags);
    if (st != VI_S_OK) {
        pthread_mutex_unlock(&c->lock);
        return st;
    }
    if (s == -1) {
        pthread_mutex_unlock(&c->lock);
        return VI_S_BUSY;
    }
    vi_slot *sl = &c->slots[s];
    attached = sl->attached_by_consumer;
    if (w == 0 || h == 0) {
        w = (uint32_t)c->default_width;
        h = (uint32_t)c->default_height;
    }
    if (!sl->has_buf || vi_gfx_u32(&sl->buf, VI_GFX_WIDTH) != w || vi_gfx_u32(&sl->buf, VI_GFX_HEIGHT) != h ||
        vi_gfx_u32(&sl->buf, VI_GFX_FORMAT) != fmt || (vi_gfx_u32(&sl->buf, VI_GFX_USAGE) & usage) != usage) {
        st = sl->prealloc ? VI_S_NO_INIT : VI_S_NO_MEMORY;
        pthread_mutex_unlock(&c->lock);
        return st;
    }
    sl->state = VI_DEQUEUED;
    update_max_cached(c, s);
    memcpy(fence, sl->fence, VI_FENCE_SIZE);
    vi_fence_none(sl->fence);
    sl->queue_time = 0;
    sl->present_time = 0;
    check_events(c, max_buffer_count(c, async));
    *slot = s;
    pthread_mutex_unlock(&c->lock);
    return attached ? (flags | VI_S_BUFFER_NEEDS_REALLOCATION) : flags;
}

int vi_bq_detach(vi_core *c, int slot)
{
    int st = VI_S_OK;
    pthread_mutex_lock(&c->lock);
    if (c->abandoned)
        st = VI_S_NO_INIT;
    else if (slot < 0 || slot >= VI_SLOTS || !owned_by_producer(c, slot) || !c->slots[slot].request_called)
        st = VI_S_BAD_VALUE;
    else {
        free_buffer(c, slot);
        pthread_cond_broadcast(&c->cv);
    }
    pthread_mutex_unlock(&c->lock);
    return st;
}

int vi_bq_detach_next(vi_core *c, vi_gfx *gb, uint8_t fence[VI_FENCE_SIZE])
{
    int st = VI_S_OK;
    vi_fence_none(fence);
    pthread_mutex_lock(&c->lock);
    if (c->abandoned) {
        st = VI_S_NO_INIT;
        goto out;
    }
    int next = -1;
    for (int s = 0; s < VI_SLOTS; s++) {
        if (c->slots[s].state == VI_FREE && c->slots[s].has_buf &&
            (next == -1 || c->slots[s].frame_number < c->slots[next].frame_number))
            next = s;
    }
    if (next == -1) {
        st = VI_S_NO_MEMORY;
        goto out;
    }
    *gb = c->slots[next].buf;
    memcpy(fence, c->slots[next].fence, VI_FENCE_SIZE);
    free_buffer(c, next);
out:
    pthread_mutex_unlock(&c->lock);
    return st;
}

int vi_bq_attach(vi_core *c, int *slot, const vi_gfx *gb)
{
    int flags;
    *slot = -1;
    pthread_mutex_lock(&c->lock);
    int s;
    int st = wait_for_free_slot(c, false, &s, &flags);
    if (st == VI_S_OK && s == -1)
        st = VI_S_BUSY;
    if (st != VI_S_OK) {
        pthread_mutex_unlock(&c->lock);
        return st;
    }
    update_max_cached(c, s);
    c->slots[s].buf = *gb;
    c->slots[s].has_buf = true;
    c->slots[s].state = VI_DEQUEUED;
    vi_fence_none(c->slots[s].fence);
    c->slots[s].request_called = true;
    *slot = s;
    pthread_mutex_unlock(&c->lock);
    return flags;
}

void vi_frame_queued(vi_core *c)
{
    pthread_mutex_lock(&cb_lock);
    void (*fn)(uint64_t, void *) = frame_cb;
    void *user = frame_cb_user;
    uint64_t frame = ++frames_queued;
    pthread_mutex_unlock(&cb_lock);
    rl_ports_script_frame(frame);
    rl_hos_mods_frame();
    if (fn)
        fn(c->layer_id, user);
}

int vi_bq_queue(vi_core *c, int slot, const vi_queue_in *in, vi_queue_out *out)
{
    memset(out, 0, sizeof *out);
    if (in->scaling > 4)
        return VI_S_BAD_VALUE;
    pthread_mutex_lock(&c->lock);
    if (c->abandoned) {
        pthread_mutex_unlock(&c->lock);
        return VI_S_NO_INIT;
    }
    int max_count = max_buffer_count(c, in->async != 0);
    if ((in->async != 0 && c->override_max != 0 && c->override_max < max_count) || slot < 0 || slot >= VI_SLOTS ||
        !owned_by_producer(c, slot) || !c->slots[slot].request_called) {
        pthread_mutex_unlock(&c->lock);
        return VI_S_BAD_VALUE;
    }
    vi_slot *sl = &c->slots[slot];
    int32_t bw = (int32_t)vi_gfx_u32(&sl->buf, VI_GFX_WIDTH), bh = (int32_t)vi_gfx_u32(&sl->buf, VI_GFX_HEIGHT);
    int32_t cl = in->crop[0] > 0 ? in->crop[0] : 0, ct = in->crop[1] > 0 ? in->crop[1] : 0;
    int32_t cr = in->crop[2] < bw ? in->crop[2] : bw, cb = in->crop[3] < bh ? in->crop[3] : bh;
    if (cl != in->crop[0] || ct != in->crop[1] || cr != in->crop[2] || cb != in->crop[3]) {
        pthread_mutex_unlock(&c->lock);
        return VI_S_BAD_VALUE;
    }
    memcpy(sl->fence, in->fence, VI_FENCE_SIZE);
    sl->state = VI_QUEUED;
    c->frame_counter++;
    sl->frame_number = c->frame_counter;
    sl->queue_time = now_ns();
    sl->present_time = 0;

    vi_item item;
    memset(&item, 0, sizeof item);
    item.acquire_called = sl->acquire_called;
    memcpy(item.crop, in->crop, sizeof item.crop);
    item.transform = in->transform;
    item.scaling = in->scaling;
    item.timestamp = in->timestamp;
    item.auto_timestamp = in->auto_timestamp != 0;
    item.swap_interval = in->swap_interval;
    item.frame_number = c->frame_counter;
    item.slot = slot;
    memcpy(item.fence, in->fence, VI_FENCE_SIZE);
    item.droppable = c->dequeue_cannot_block || in->async != 0;
    item.buf = sl->buf;

    c->hist_pos = (c->hist_pos + 1) % 8;
    c->history[c->hist_pos].frame_number = c->frame_counter;
    c->history[c->hist_pos].queue_time = sl->queue_time;
    c->history[c->hist_pos].present_time = 0;
    c->history[c->hist_pos].state = VI_QUEUED;

    if (c->nqueue == 0) {
        c->queue[c->nqueue++] = item;
    } else if (c->queue[0].droppable) {
        if (still_tracking(c, &c->queue[0])) {
            c->slots[c->queue[0].slot].state = VI_FREE;
            c->slots[c->queue[0].slot].frame_number = 0;
        }
        c->queue[0] = item;
    } else if (c->nqueue < VI_SLOTS) {
        c->queue[c->nqueue++] = item;
    }
    c->has_been_queued = true;
    pthread_cond_broadcast(&c->cv);
    check_events(c, max_count);

    out->width = (uint32_t)c->default_width;
    out->height = (uint32_t)c->default_height;
    out->transform_hint = c->transform_hint;
    out->pending = c->nqueue;
    if (in->sticky & 8) {
        out->transform_hint |= 0x20;
        out->frame_number = sl->frame_number;
    }
    pthread_mutex_unlock(&c->lock);
    vi_frame_queued(c);
    return VI_S_OK;
}

void vi_bq_cancel(vi_core *c, int slot, const uint8_t fence[VI_FENCE_SIZE])
{
    pthread_mutex_lock(&c->lock);
    if (!c->abandoned && slot >= 0 && slot < VI_SLOTS && owned_by_producer(c, slot)) {
        c->slots[slot].state = VI_FREE;
        c->slots[slot].frame_number = 0;
        memcpy(c->slots[slot].fence, fence, VI_FENCE_SIZE);
        pthread_cond_broadcast(&c->cv);
        signal_free(c);
    }
    pthread_mutex_unlock(&c->lock);
}

int vi_bq_query(vi_core *c, uint32_t what, int *out)
{
    int st = VI_S_OK;
    *out = 0;
    pthread_mutex_lock(&c->lock);
    if (c->abandoned) {
        st = VI_S_NO_INIT;
    } else {
        switch (what) {
        case 0:
            *out = c->default_width;
            break;
        case 1:
            *out = c->default_height;
            break;
        case 2:
            *out = (int)c->default_format;
            break;
        case 3:
            *out = min_undequeued(c, false);
            break;
        case 9:
            *out = c->nqueue > 1 ? 1 : 0;
            break;
        case 10:
            *out = (int)c->consumer_usage;
            break;
        case 12:
            *out = max_buffer_count(c, true);
            break;
        default:
            st = VI_S_BAD_VALUE;
            break;
        }
    }
    pthread_mutex_unlock(&c->lock);
    return st;
}

int vi_bq_connect(vi_core *c, int api, bool producer_controlled, vi_queue_out *out)
{
    int st = VI_S_OK;
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&c->lock);
    if (c->abandoned || !c->consumer_connected) {
        st = VI_S_NO_INIT;
    } else if (c->connected_api != VI_API_NONE) {
        st = VI_S_BAD_VALUE;
    } else {
        c->has_been_queued = false;
        c->dequeue_cannot_block = c->consumer_controlled && producer_controlled;
        if (api >= VI_API_NVN && api <= VI_API_CAMERA) {
            c->connected_api = api;
            out->width = (uint32_t)c->default_width;
            out->height = (uint32_t)c->default_height;
            out->transform_hint = c->transform_hint;
            out->pending = c->nqueue;
        } else {
            st = VI_S_BAD_VALUE;
        }
    }
    pthread_mutex_unlock(&c->lock);
    return st;
}

int vi_bq_disconnect(vi_core *c, int api)
{
    int st = VI_S_BAD_VALUE;
    pthread_mutex_lock(&c->lock);
    if (c->abandoned) {
        st = VI_S_OK;
    } else if (api >= VI_API_NVN && api <= VI_API_CAMERA && c->connected_api == api) {
        c->nqueue = 0;
        free_all(c);
        pthread_cond_broadcast(&c->cv);
        c->connected_api = VI_API_NONE;
        signal_free(c);
        signal_frame(c);
        st = VI_S_OK;
    }
    pthread_mutex_unlock(&c->lock);
    return st;
}

int vi_bq_set_preallocated(vi_core *c, int slot, const vi_gfx *gb)
{
    if (slot < 0 || slot >= VI_SLOTS)
        return VI_S_BAD_VALUE;
    pthread_mutex_lock(&c->lock);
    vi_slot *s = &c->slots[slot];
    s->state = VI_FREE;
    vi_fence_none(s->fence);
    s->request_called = false;
    s->acquire_called = false;
    s->needs_cleanup = false;
    s->prealloc = gb != NULL;
    s->frame_number = 0;
    s->has_buf = gb != NULL;
    if (gb) {
        s->buf = *gb;
        vi_gfx_set_u32(&s->buf, VI_GFX_NV_USAGE, vi_gfx_u32(&s->buf, VI_GFX_NV_USAGE) & c->consumer_usage);
    }
    c->override_max = preallocated_count(c);
    c->use_async = false;
    if (gb) {
        c->default_width = (int)vi_gfx_u32(gb, VI_GFX_WIDTH);
        c->default_height = (int)vi_gfx_u32(gb, VI_GFX_HEIGHT);
        c->default_format = vi_gfx_u32(gb, VI_GFX_FORMAT);
    } else {
        bool all_free = true;
        for (int i = 0; i < VI_SLOTS; i++) {
            if (c->slots[i].has_buf) {
                all_free = false;
                break;
            }
        }
        if (all_free) {
            c->nqueue = 0;
            free_all(c);
            pthread_cond_broadcast(&c->cv);
            signal_free(c);
            signal_frame(c);
            pthread_mutex_unlock(&c->lock);
            return VI_S_OK;
        }
    }
    pthread_cond_broadcast(&c->cv);
    signal_free(c);
    pthread_mutex_unlock(&c->lock);
    return VI_S_OK;
}

int vi_bq_history(vi_core *c, int count, vi_hist *out, uint32_t *n)
{
    *n = 0;
    if (count <= 0)
        return VI_S_BAD_VALUE;
    pthread_mutex_lock(&c->lock);
    if (count > 8)
        count = 8;
    uint32_t pos = c->hist_pos;
    for (int i = 0; i < count; i++) {
        out[i] = c->history[pos % 8];
        pos--;
    }
    *n = (uint32_t)count;
    pthread_mutex_unlock(&c->lock);
    return VI_S_OK;
}

int vi_bq_acquire(vi_core *c, vi_item *item)
{
    int st;
    pthread_mutex_lock(&c->lock);
    int acquired = 0;
    for (int i = 0; i < c->max_cached; i++)
        acquired += c->slots[i].state == VI_ACQUIRED;
    if (acquired > c->max_acquired) {
        st = VI_S_INVALID_OPERATION;
        goto out;
    }
    st = VI_S_NO_BUFFER_AVAILABLE;
    while (c->nqueue > 0) {
        vi_item it = c->queue[0];
        queue_remove(c, 0);
        if (!still_tracking(c, &it))
            continue;
        vi_slot *s = &c->slots[it.slot];
        s->acquire_called = true;
        s->needs_cleanup = true;
        s->state = VI_ACQUIRED;
        vi_fence_none(s->fence);
        for (int i = 0; i < 8; i++) {
            if (c->history[i].frame_number == s->frame_number) {
                c->history[i].state = VI_ACQUIRED;
                break;
            }
        }
        *item = it;
        st = VI_S_OK;
        break;
    }
    check_events(c, max_buffer_count(c, true));
    pthread_cond_broadcast(&c->cv);
out:
    pthread_mutex_unlock(&c->lock);
    return st;
}

int vi_bq_release(vi_core *c, int slot, uint64_t frame_number)
{
    if (slot < 0 || slot >= VI_SLOTS)
        return VI_S_BAD_VALUE;
    int st = VI_S_OK;
    pthread_mutex_lock(&c->lock);
    vi_slot *s = &c->slots[slot];
    if (frame_number != UINT64_MAX && s->frame_number != frame_number) {
        st = VI_S_STALE_BUFFER_SLOT;
        goto out;
    }
    for (uint32_t i = 0; i < c->nqueue; i++) {
        if (c->queue[i].slot == slot) {
            st = VI_S_BAD_VALUE;
            goto out;
        }
    }
    if (s->state == VI_ACQUIRED) {
        s->state = VI_FREE;
        vi_fence_none(s->fence);
    } else if (s->needs_cleanup) {
        s->needs_cleanup = false;
        st = VI_S_STALE_BUFFER_SLOT;
        goto out;
    } else {
        st = VI_S_BAD_VALUE;
        goto out;
    }
    check_events(c, max_buffer_count(c, true));
    pthread_cond_broadcast(&c->cv);
out:
    pthread_mutex_unlock(&c->lock);
    return st;
}

static void core_init(vi_core *c, uint64_t layer_id)
{
    uint32_t w, h;
    vi_get_resolution(&w, &h);
    memset(c, 0, sizeof *c);
    pthread_mutex_init(&c->lock, NULL);
    pthread_cond_init(&c->cv, NULL);
    for (int i = 0; i < VI_SLOTS; i++)
        vi_fence_none(c->slots[i].fence);
    c->default_format = 1;
    c->default_width = (int)w;
    c->default_height = (int)h;
    c->default_max = 2;
    c->max_acquired = 1;
    c->consumer_connected = true;
    c->active = true;
    c->free_ev = rl_hos_event_create();
    c->frame_ev = rl_hos_event_create();
    c->layer_id = layer_id;
}

static void layer_free(vi_layer *l)
{
    hos_kobj_unref(&l->core.free_ev->obj);
    hos_kobj_unref(&l->core.frame_ev->obj);
    pthread_cond_destroy(&l->core.cv);
    pthread_mutex_destroy(&l->core.lock);
    free(l);
}

void vi_layer_unref(vi_layer *l)
{
    if (l && __atomic_sub_fetch(&l->refs, 1, __ATOMIC_ACQ_REL) == 0)
        layer_free(l);
}

static void layer_deactivate(vi_layer *l)
{
    pthread_mutex_lock(&l->core.lock);
    l->core.active = false;
    pthread_cond_broadcast(&l->core.cv);
    pthread_mutex_unlock(&l->core.lock);
}

rl_result vi_layer_create(uint64_t owner, int state, uint64_t *id)
{
    vi_layer *l = calloc(1, sizeof *l);
    if (!l)
        return KR_OUT_OF_MEMORY;
    pthread_mutex_lock(&reg_lock);
    if (nlayers >= VI_MAX_LAYERS) {
        pthread_mutex_unlock(&reg_lock);
        free(l);
        return KR_OUT_OF_RESOURCE;
    }
    l->refs = 1;
    l->id = next_layer_id++;
    l->state = state;
    l->binder_id = next_binder_id++;
    l->owner = owner;
    core_init(&l->core, l->id);
    layers[nlayers++] = l;
    render_layer = l->id;
    *id = l->id;
    pthread_mutex_unlock(&reg_lock);
    return KR_SUCCESS;
}

vi_layer *vi_layer_get(uint64_t id)
{
    vi_layer *r = NULL;
    pthread_mutex_lock(&reg_lock);
    for (uint32_t i = 0; i < nlayers; i++) {
        if (layers[i]->id == id) {
            r = layers[i];
            __atomic_add_fetch(&r->refs, 1, __ATOMIC_RELAXED);
            break;
        }
    }
    pthread_mutex_unlock(&reg_lock);
    return r;
}

vi_layer *vi_layer_by_binder(int32_t binder_id)
{
    vi_layer *r = NULL;
    pthread_mutex_lock(&reg_lock);
    for (uint32_t i = 0; i < nlayers; i++) {
        if (layers[i]->binder_id == binder_id) {
            r = layers[i];
            __atomic_add_fetch(&r->refs, 1, __ATOMIC_RELAXED);
            break;
        }
    }
    pthread_mutex_unlock(&reg_lock);
    return r;
}

int32_t vi_layer_binder(uint64_t id)
{
    vi_layer *l = vi_layer_get(id);
    if (!l)
        return 0;
    int32_t b = l->binder_id;
    vi_layer_unref(l);
    return b;
}

static void fix_render_locked(uint64_t id)
{
    if (render_layer == id)
        render_layer = nlayers ? layers[nlayers - 1]->id : 0;
}

rl_result vi_layer_open(uint64_t id, int32_t *binder)
{
    rl_result rc = VI_RESULT_INVALID_ARGUMENTS;
    pthread_mutex_lock(&reg_lock);
    for (uint32_t i = 0; i < nlayers; i++) {
        if (layers[i]->id == id && layers[i]->state == VI_LAYER_MANAGED_CLOSED) {
            layers[i]->state = VI_LAYER_MANAGED_OPENED;
            *binder = layers[i]->binder_id;
            render_layer = id;
            rc = KR_SUCCESS;
            break;
        }
    }
    pthread_mutex_unlock(&reg_lock);
    return rc;
}

rl_result vi_layer_close(uint64_t id)
{
    rl_result rc = VI_RESULT_INVALID_VALUE;
    pthread_mutex_lock(&reg_lock);
    for (uint32_t i = 0; i < nlayers; i++) {
        if (layers[i]->id == id) {
            fix_render_locked(id);
            if (layers[i]->state == VI_LAYER_MANAGED_OPENED)
                layers[i]->state = VI_LAYER_MANAGED_CLOSED;
            rc = KR_SUCCESS;
            break;
        }
    }
    pthread_mutex_unlock(&reg_lock);
    return rc;
}

static rl_result layer_destroy(uint64_t id, bool stray)
{
    rl_result rc = VI_RESULT_INVALID_VALUE;
    vi_layer *gone = NULL;
    pthread_mutex_lock(&reg_lock);
    for (uint32_t i = 0; i < nlayers; i++) {
        vi_layer *l = layers[i];
        if (l->id != id)
            continue;
        bool is_stray = l->state == VI_LAYER_STRAY;
        if (is_stray != stray) {
            rc = VI_RESULT_PERMISSION_DENIED;
            break;
        }
        memmove(&layers[i], &layers[i + 1], (nlayers - i - 1) * sizeof layers[0]);
        nlayers--;
        fix_render_locked(id);
        gone = l;
        rc = KR_SUCCESS;
        break;
    }
    pthread_mutex_unlock(&reg_lock);
    if (gone) {
        layer_deactivate(gone);
        vi_layer_unref(gone);
    }
    return rc;
}

rl_result vi_layer_destroy_managed(uint64_t id) { return layer_destroy(id, false); }

rl_result vi_layer_destroy_stray(uint64_t id) { return layer_destroy(id, true); }

void vi_layer_set_render(uint64_t id)
{
    pthread_mutex_lock(&reg_lock);
    render_layer = id;
    pthread_mutex_unlock(&reg_lock);
}

void vi_layers_deactivate(void)
{
    pthread_mutex_lock(&reg_lock);
    for (uint32_t i = 0; i < nlayers; i++)
        layer_deactivate(layers[i]);
    pthread_mutex_unlock(&reg_lock);
}

void vi_layers_reset(void)
{
    pthread_mutex_lock(&reg_lock);
    vi_layer *old[VI_MAX_LAYERS];
    uint32_t n = nlayers;
    memcpy(old, layers, n * sizeof old[0]);
    nlayers = 0;
    next_layer_id = 1;
    next_binder_id = 1;
    render_layer = 0;
    pthread_mutex_unlock(&reg_lock);
    for (uint32_t i = 0; i < n; i++) {
        layer_deactivate(old[i]);
        vi_layer_unref(old[i]);
    }
}

uint32_t rl_vi_layer_count(void)
{
    pthread_mutex_lock(&reg_lock);
    uint32_t n = nlayers;
    pthread_mutex_unlock(&reg_lock);
    return n;
}

bool rl_vi_layer_id(uint32_t index, uint64_t *id)
{
    bool ok = false;
    pthread_mutex_lock(&reg_lock);
    if (index < nlayers) {
        *id = layers[index]->id;
        ok = true;
    }
    pthread_mutex_unlock(&reg_lock);
    return ok;
}

bool rl_vi_layer_size(uint64_t id, uint32_t *width, uint32_t *height)
{
    vi_layer *l = vi_layer_get(id);
    if (!l)
        return false;
    vi_get_resolution(width, height);
    pthread_mutex_lock(&reg_lock);
    if (l->width && l->height) {
        *width = l->width;
        *height = l->height;
    }
    pthread_mutex_unlock(&reg_lock);
    vi_layer_unref(l);
    return true;
}

rl_result vi_layer_set_size(uint64_t id, uint64_t width, uint64_t height)
{
    if (!width || !height || width > 0x10000 || height > 0x10000)
        return VI_RESULT_INVALID_ARGUMENTS;
    vi_layer *l = vi_layer_get(id);
    if (!l)
        return VI_RESULT_INVALID_VALUE;
    pthread_mutex_lock(&reg_lock);
    l->width = (uint32_t)width;
    l->height = (uint32_t)height;
    pthread_mutex_unlock(&reg_lock);
    vi_layer_unref(l);
    return KR_SUCCESS;
}

bool rl_vi_render_layer(uint64_t *id)
{
    pthread_mutex_lock(&reg_lock);
    bool ok = render_layer != 0;
    *id = render_layer;
    pthread_mutex_unlock(&reg_lock);
    return ok;
}

void rl_vi_set_frame_callback(void (*fn)(uint64_t, void *), void *user)
{
    pthread_mutex_lock(&cb_lock);
    frame_cb = fn;
    frame_cb_user = user;
    pthread_mutex_unlock(&cb_lock);
}

rl_result rl_vi_acquire_frame(uint64_t layer, rl_vi_frame *out)
{
    vi_layer *l = vi_layer_get(layer);
    if (!l)
        return VI_RESULT_INVALID_VALUE;
    vi_item it;
    int st = vi_bq_acquire(&l->core, &it);
    if (st != VI_S_OK) {
        vi_layer_unref(l);
        return VI_RESULT_NO_FRAME;
    }
    memset(out, 0, sizeof *out);
    const vi_gfx *g = &it.buf;
    out->slot = (uint32_t)it.slot;
    out->nvmap_handle = vi_gfx_handle(g);
    out->nvmap_id = vi_gfx_u32(g, VI_GFX_NVMAP_ID);
    out->offset = vi_gfx_u32(g, VI_GFX_SURF_OFFSET);
    out->width = vi_gfx_u32(g, VI_GFX_WIDTH);
    out->height = vi_gfx_u32(g, VI_GFX_HEIGHT);
    out->format = vi_gfx_u32(g, VI_GFX_FORMAT);
    out->stride = vi_gfx_u32(g, VI_GFX_STRIDE);
    out->usage = vi_gfx_u32(g, VI_GFX_USAGE);
    out->pitch = vi_gfx_u32(g, VI_GFX_SURF_PITCH);
    out->layout = vi_gfx_u32(g, VI_GFX_SURF_LAYOUT);
    out->kind = vi_gfx_u32(g, VI_GFX_SURF_KIND);
    out->block_height_log2 = vi_gfx_u32(g, VI_GFX_SURF_BLOCK);
    out->color_format = vi_gfx_u64(g, VI_GFX_SURF_COLOR);
    out->size = vi_gfx_u64(g, VI_GFX_SURF_SIZE);
    memcpy(out->crop, it.crop, sizeof out->crop);
    out->transform = it.transform;
    out->scaling_mode = it.scaling;
    out->swap_interval = (uint32_t)it.swap_interval;
    uint32_t fc;
    memcpy(&fc, it.fence, 4);
    out->fence_count = fc > 4 ? 4 : fc;
    out->fence_present = out->fence_count > 0;
    for (uint32_t i = 0; i < out->fence_count; i++) {
        memcpy(&out->fence_id[i], it.fence + 4 + 8 * i, 4);
        memcpy(&out->fence_value[i], it.fence + 8 + 8 * i, 4);
    }
    out->frame_number = it.frame_number;
    out->timestamp = it.timestamp;
    vi_layer_unref(l);
    return KR_SUCCESS;
}

rl_result rl_vi_release_frame(uint64_t layer, uint32_t slot)
{
    vi_layer *l = vi_layer_get(layer);
    if (!l)
        return VI_RESULT_INVALID_VALUE;
    int st = vi_bq_release(&l->core, (int)slot, UINT64_MAX);
    vi_layer_unref(l);
    return st == VI_S_OK ? KR_SUCCESS : VI_RESULT_INVALID_VALUE;
}
