/**
 * @file srv_vi_binder.c
 * @brief IHOSBinderDriver, the Android parcel format and the
 * IGraphicBufferProducer transactions.
 *
 * Ported from Ryujinx (MIT, Copyright (c) Ryujinx Team and Contributors),
 * Ryujinx.HLE/HOS/Services/SurfaceFlinger: IHOSBinderDriver, Parcel, IBinder
 * and IGraphicBufferProducer. The parcel layout follows libnx (ISC)
 * display/parcel.c.
 */
#include "vi.h"

#include <stdlib.h>

enum {
    TX_REQUEST_BUFFER = 1,
    TX_SET_BUFFER_COUNT,
    TX_DEQUEUE_BUFFER,
    TX_DETACH_BUFFER,
    TX_DETACH_NEXT_BUFFER,
    TX_ATTACH_BUFFER,
    TX_QUEUE_BUFFER,
    TX_CANCEL_BUFFER,
    TX_QUERY,
    TX_CONNECT,
    TX_DISCONNECT,
    TX_SET_SIDEBAND_STREAM,
    TX_ALLOCATE_BUFFERS,
    TX_SET_PREALLOCATED_BUFFER,
    TX_GET_BUFFER_HISTORY = 17
};

#define PARCEL_HDR 16u
#define QUEUE_IN_SIZE 0x54u
#define WINDOW_PAYLOAD 0x28u
#define WINDOW_OBJECTS 4u
#define WINDOW_PARCEL (PARCEL_HDR + WINDOW_PAYLOAD + WINDOW_OBJECTS)
#define HIST_ENTRY 28u

static const char token[] = "android.gui.IGraphicBufferProducer";

typedef struct vi_rd {
    const uint8_t *p;
    uint32_t size;
    uint32_t pos;
    bool err;
} vi_rd;

typedef struct vi_wr {
    uint8_t buf[VI_PARCEL_MAX];
    uint32_t pos;
    bool err;
} vi_wr;

static const uint8_t *rd_take(vi_rd *r, uint32_t n)
{
    uint32_t adv = (n + 3u) & ~3u;
    if (r->err || adv < n || r->size - r->pos < adv) {
        r->err = true;
        return NULL;
    }
    const uint8_t *p = r->p + r->pos;
    r->pos += adv;
    return p;
}

static uint32_t rd_u32(vi_rd *r)
{
    uint32_t v = 0;
    const uint8_t *p = rd_take(r, 4);
    if (p)
        memcpy(&v, p, 4);
    return v;
}

static int32_t rd_i32(vi_rd *r) { return (int32_t)rd_u32(r); }

static bool rd_flat(vi_rd *r, void *dst, uint32_t size)
{
    uint32_t len = rd_u32(r);
    uint32_t fds = rd_u32(r);
    if (r->err || len != size || fds != 0) {
        r->err = true;
        return false;
    }
    const uint8_t *p = rd_take(r, size);
    if (!p)
        return false;
    memcpy(dst, p, size);
    return true;
}

static bool rd_token(vi_rd *r)
{
    rd_u32(r);
    int32_t n = rd_i32(r);
    if (r->err || n < 0 || (uint32_t)n != sizeof token - 1)
        return false;
    const uint8_t *p = rd_take(r, ((uint32_t)n + 1u) * 2u);
    if (!p)
        return false;
    for (int32_t i = 0; i < n; i++) {
        uint16_t ch;
        memcpy(&ch, p + 2 * i, 2);
        if (ch != (uint8_t)token[i])
            return false;
    }
    return true;
}

static void wr_bytes(vi_wr *w, const void *src, uint32_t n)
{
    uint32_t adv = (n + 3u) & ~3u;
    if (w->err || adv < n || sizeof w->buf - w->pos < adv) {
        w->err = true;
        return;
    }
    memcpy(w->buf + w->pos, src, n);
    memset(w->buf + w->pos + n, 0, adv - n);
    w->pos += adv;
}

static void wr_u32(vi_wr *w, uint32_t v) { wr_bytes(w, &v, 4); }

static void wr_i32(vi_wr *w, int32_t v) { wr_bytes(w, &v, 4); }

static void wr_flat(vi_wr *w, const void *src, uint32_t size)
{
    wr_u32(w, size);
    wr_u32(w, 0);
    wr_bytes(w, src, size);
}

static void wr_gfx(vi_wr *w, const vi_gfx *g)
{
    wr_u32(w, g != NULL);
    if (g)
        wr_flat(w, g->raw, VI_GFX_SIZE);
}

static void wr_fence_ptr(vi_wr *w, const uint8_t f[VI_FENCE_SIZE])
{
    wr_u32(w, 1);
    wr_flat(w, f, VI_FENCE_SIZE);
}

static void wr_queue_out(vi_wr *w, const vi_queue_out *o)
{
    wr_u32(w, o->width);
    wr_u32(w, o->height);
    wr_u32(w, o->transform_hint);
    wr_u32(w, o->pending);
    if (o->transform_hint & 0x20u)
        wr_bytes(w, &o->frame_number, 8);
}

static bool rd_gfx(vi_rd *r, vi_gfx *g, bool *has)
{
    *has = rd_u32(r) != 0;
    if (!*has)
        return !r->err;
    if (!rd_flat(r, g->raw, VI_GFX_SIZE))
        return false;
    return vi_gfx_u32(g, VI_GFX_INTS_COUNT) == VI_GFX_INTS;
}

static void tx_request_buffer(vi_core *c, vi_rd *r, vi_wr *w)
{
    int slot = rd_i32(r);
    vi_gfx g;
    bool has;
    int st = vi_bq_request_buffer(c, slot, &g, &has);
    wr_gfx(w, has && st == VI_S_OK ? &g : NULL);
    wr_i32(w, st);
}

static void tx_dequeue(vi_core *c, vi_rd *r, vi_wr *w)
{
    bool async = rd_u32(r) != 0;
    uint32_t width = rd_u32(r), height = rd_u32(r), fmt = rd_u32(r), usage = rd_u32(r);
    int slot;
    uint8_t fence[VI_FENCE_SIZE];
    int st = r->err ? VI_S_NOT_ENOUGH_DATA : vi_bq_dequeue(c, &slot, fence, async, width, height, fmt, usage);
    if (r->err) {
        slot = -1;
        vi_fence_none(fence);
    }
    wr_i32(w, slot);
    wr_fence_ptr(w, fence);
    wr_i32(w, st);
}

static void tx_detach_next(vi_core *c, vi_wr *w)
{
    vi_gfx g;
    uint8_t fence[VI_FENCE_SIZE];
    int st = vi_bq_detach_next(c, &g, fence);
    wr_gfx(w, st == VI_S_OK ? &g : NULL);
    wr_fence_ptr(w, fence);
    wr_i32(w, st);
}

static void tx_attach(vi_core *c, vi_rd *r, vi_wr *w)
{
    vi_gfx g;
    bool has;
    int slot = -1;
    int st;
    if (!rd_gfx(r, &g, &has) || !has)
        st = VI_S_BAD_VALUE;
    else
        st = vi_bq_attach(c, &slot, &g);
    wr_i32(w, slot);
    wr_i32(w, st);
}

static void tx_queue(vi_core *c, vi_rd *r, vi_wr *w)
{
    int slot = rd_i32(r);
    uint8_t raw[QUEUE_IN_SIZE];
    vi_queue_out out;
    memset(&out, 0, sizeof out);
    int st;
    if (!rd_flat(r, raw, QUEUE_IN_SIZE)) {
        st = VI_S_BAD_VALUE;
    } else {
        vi_queue_in in;
        memset(&in, 0, sizeof in);
        memcpy(&in.timestamp, raw, 8);
        memcpy(&in.auto_timestamp, raw + 8, 4);
        memcpy(in.crop, raw + 12, 16);
        memcpy(&in.scaling, raw + 28, 4);
        memcpy(&in.transform, raw + 32, 4);
        memcpy(&in.sticky, raw + 36, 4);
        memcpy(&in.async, raw + 40, 4);
        memcpy(&in.swap_interval, raw + 44, 4);
        memcpy(in.fence, raw + 48, VI_FENCE_SIZE);
        st = vi_bq_queue(c, slot, &in, &out);
    }
    wr_queue_out(w, &out);
    wr_i32(w, st);
}

static void tx_cancel(vi_core *c, vi_rd *r, vi_wr *w)
{
    int slot = rd_i32(r);
    uint8_t fence[VI_FENCE_SIZE];
    if (rd_flat(r, fence, VI_FENCE_SIZE))
        vi_bq_cancel(c, slot, fence);
    wr_i32(w, VI_S_OK);
}

static void tx_query(vi_core *c, vi_rd *r, vi_wr *w)
{
    uint32_t what = rd_u32(r);
    int value = 0;
    int st = r->err ? VI_S_NOT_ENOUGH_DATA : vi_bq_query(c, what, &value);
    wr_i32(w, value);
    wr_i32(w, st);
}

static void tx_connect(vi_core *c, vi_rd *r, vi_wr *w)
{
    bool has_listener = rd_u32(r) != 0;
    int api = rd_i32(r);
    bool controlled = rd_u32(r) != 0;
    vi_queue_out out;
    memset(&out, 0, sizeof out);
    int st = r->err ? VI_S_NOT_ENOUGH_DATA : has_listener ? VI_S_BAD_VALUE : vi_bq_connect(c, api, controlled, &out);
    wr_queue_out(w, &out);
    wr_i32(w, st);
}

static void tx_prealloc(vi_core *c, vi_rd *r, vi_wr *w)
{
    int slot = rd_i32(r);
    vi_gfx g;
    bool has;
    int st;
    if (!rd_gfx(r, &g, &has))
        st = VI_S_BAD_VALUE;
    else
        st = vi_bq_set_preallocated(c, slot, has ? &g : NULL);
    wr_i32(w, st);
}

static void tx_history(vi_core *c, vi_rd *r, vi_wr *w)
{
    int count = rd_i32(r);
    vi_hist h[8];
    uint32_t n = 0;
    int st = r->err ? VI_S_NOT_ENOUGH_DATA : vi_bq_history(c, count, h, &n);
    wr_i32(w, st);
    wr_i32(w, (int32_t)n);
    for (uint32_t i = 0; i < n; i++) {
        uint8_t e[HIST_ENTRY];
        memcpy(e, &h[i].frame_number, 8);
        memcpy(e + 8, &h[i].queue_time, 8);
        memcpy(e + 16, &h[i].present_time, 8);
        memcpy(e + 24, &h[i].state, 4);
        wr_bytes(w, e, HIST_ENTRY);
    }
}

static void transact(vi_core *c, uint32_t code, vi_rd *r, vi_wr *w)
{
    switch (code) {
    case TX_REQUEST_BUFFER:
        tx_request_buffer(c, r, w);
        break;
    case TX_SET_BUFFER_COUNT: {
        int n = rd_i32(r);
        wr_i32(w, r->err ? VI_S_NOT_ENOUGH_DATA : vi_bq_set_buffer_count(c, n));
        break;
    }
    case TX_DEQUEUE_BUFFER:
        tx_dequeue(c, r, w);
        break;
    case TX_DETACH_BUFFER: {
        int slot = rd_i32(r);
        wr_i32(w, r->err ? VI_S_NOT_ENOUGH_DATA : vi_bq_detach(c, slot));
        break;
    }
    case TX_DETACH_NEXT_BUFFER:
        tx_detach_next(c, w);
        break;
    case TX_ATTACH_BUFFER:
        tx_attach(c, r, w);
        break;
    case TX_QUEUE_BUFFER:
        tx_queue(c, r, w);
        break;
    case TX_CANCEL_BUFFER:
        tx_cancel(c, r, w);
        break;
    case TX_QUERY:
        tx_query(c, r, w);
        break;
    case TX_CONNECT:
        tx_connect(c, r, w);
        break;
    case TX_DISCONNECT: {
        int api = rd_i32(r);
        wr_i32(w, r->err ? VI_S_NOT_ENOUGH_DATA : vi_bq_disconnect(c, api));
        break;
    }
    case TX_SET_SIDEBAND_STREAM:
        wr_i32(w, VI_S_OK);
        break;
    case TX_ALLOCATE_BUFFERS:
        break;
    case TX_SET_PREALLOCATED_BUFFER:
        tx_prealloc(c, r, w);
        break;
    case TX_GET_BUFFER_HISTORY:
        tx_history(c, r, w);
        break;
    default:
        wr_i32(w, VI_S_UNKNOWN_TRANSACTION);
        break;
    }
}

static rl_result do_transact(rl_ipc_req *r, rl_ipc_buf in, rl_ipc_buf out)
{
    struct {
        int32_t id;
        uint32_t code, flags;
    } a;
    if (!rl_srv_in(r, &a, sizeof a))
        return SF_RESULT_INVALID_HEADER;
    vi_layer *l = vi_layer_by_binder(a.id);
    if (!l)
        return VI_RESULT_INVALID_VALUE;

    uint8_t data[VI_PARCEL_MAX];
    uint64_t n = in.size < sizeof data ? in.size : sizeof data;
    if (n < PARCEL_HDR || !rl_ipc_buf_read(&in, data, n)) {
        vi_layer_unref(l);
        return KR_INVALID_MEM_STATE;
    }
    uint32_t hdr[4];
    memcpy(hdr, data, sizeof hdr);
    vi_rd rd = { data, (uint32_t)n, 0, false };
    if (hdr[1] > n || hdr[0] > n - hdr[1]) {
        vi_layer_unref(l);
        return SF_RESULT_INVALID_HEADER;
    }
    rd.p = data + hdr[1];
    rd.size = hdr[0];

    vi_wr *w = calloc(1, sizeof *w);
    if (!w) {
        vi_layer_unref(l);
        return KR_OUT_OF_MEMORY;
    }
    if (rd_token(&rd))
        transact(&l->core, a.code, &rd, w);
    vi_layer_unref(l);

    rl_result rc = KR_SUCCESS;
    if (w->err || PARCEL_HDR + w->pos > out.size) {
        rc = VI_RESULT_INVALID_ARGUMENTS;
    } else {
        uint32_t oh[4] = { w->pos, PARCEL_HDR, 0, PARCEL_HDR + w->pos };
        uint8_t *reply = malloc(PARCEL_HDR + w->pos);
        if (!reply) {
            rc = KR_OUT_OF_MEMORY;
        } else {
            memcpy(reply, oh, sizeof oh);
            memcpy(reply + PARCEL_HDR, w->buf, w->pos);
            if (!rl_ipc_buf_write(&out, reply, PARCEL_HDR + w->pos))
                rc = KR_INVALID_MEM_STATE;
            free(reply);
        }
    }
    free(w);
    return rc;
}

static rl_result binder_transact(rl_ipc_req *r)
{
    if (r->na < 1 || r->nb < 1)
        return SF_RESULT_INVALID_HEADER;
    return do_transact(r, r->a[0], r->b[0]);
}

static rl_result binder_transact_auto(rl_ipc_req *r)
{
    rl_ipc_buf in = rl_ipc_auto_in(r, 0), out = rl_ipc_auto_out(r, 0);
    if (!in.size || !out.size)
        return SF_RESULT_INVALID_HEADER;
    return do_transact(r, in, out);
}

static rl_result binder_adjust_refcount(rl_ipc_req *r)
{
    int32_t in[3];
    return rl_srv_in(r, in, sizeof in) ? KR_SUCCESS : SF_RESULT_INVALID_HEADER;
}

static rl_result binder_native_handle(rl_ipc_req *r)
{
    struct {
        int32_t id;
        uint32_t type;
    } a;
    if (!rl_srv_in(r, &a, sizeof a))
        return SF_RESULT_INVALID_HEADER;
    vi_layer *l = vi_layer_by_binder(a.id);
    if (!l)
        return VI_RESULT_INVALID_VALUE;
    rl_result rc = VI_RESULT_INVALID_VALUE;
    if (a.type == 0xF)
        rc = rl_ipc_push_copy(r, &l->core.free_ev->obj);
    vi_layer_unref(l);
    return rc;
}

static const rl_ipc_cmd binder_cmds[] = {
    { 0, "TransactParcel", binder_transact },
    { 1, "AdjustRefcount", binder_adjust_refcount },
    { 2, "GetNativeHandle", binder_native_handle },
    { 3, "TransactParcelAuto", binder_transact_auto },
};

static const rl_ipc_class binder_class = { "IHOSBinderDriver", binder_cmds, sizeof binder_cmds / sizeof binder_cmds[0], 0x1000,
                                           NULL };

rl_ipc_object *vi_binder_new(void) { return rl_srv_object(&binder_class, NULL); }

rl_result vi_native_window(int32_t binder_id, rl_ipc_buf out, uint64_t *len)
{
    uint8_t p[WINDOW_PARCEL];
    memset(p, 0, sizeof p);
    uint32_t hdr[4] = { WINDOW_PAYLOAD, PARCEL_HDR, WINDOW_OBJECTS, PARCEL_HDR + WINDOW_PAYLOAD };
    memcpy(p, hdr, sizeof hdr);
    int32_t type = 2;
    int64_t id = binder_id;
    memcpy(p + PARCEL_HDR, &type, 4);
    memcpy(p + PARCEL_HDR + 8, &id, 8);
    memcpy(p + PARCEL_HDR + 24, "dispdrv", 8);
    if (!rl_ipc_buf_write(&out, p, sizeof p))
        return KR_INVALID_MEM_STATE;
    *len = sizeof p;
    return KR_SUCCESS;
}
