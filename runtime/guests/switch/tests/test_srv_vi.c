/**
 * @file test_srv_vi.c
 * @brief Tests of the vi services, the binder driver and the buffer queue,
 * driven through IPC the way libnx drives them.
 */
#include "tc.h"
#include "vi.h"
#include "srv_link.h"

#include <relift/ports.h>

#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

#define PARCEL_IN 0x10000u
#define PARCEL_OUT 0x12000u
#define WIN_BUF 0x14000u
#define LIST_BUF 0x15000u
#define GFX_USAGE 0xB00u

typedef struct fx {
    rl_ctx *c;
    uint64_t heap;
    tc_obj root, disp, relay;
    uint64_t layer;
    int32_t binder;
} fx;

typedef struct pb {
    uint8_t d[0x800];
    uint32_t n;
} pb;

typedef struct rep {
    uint32_t rc;
    uint8_t p[0x800];
    uint32_t n;
} rep;

static uint8_t *gm(uint64_t a) { return t_mem() + a; }

static void pb_put(pb *b, const void *p, uint32_t n)
{
    memcpy(b->d + b->n, p, n);
    memset(b->d + b->n + n, 0, ((n + 3u) & ~3u) - n);
    b->n += (n + 3u) & ~3u;
}

static void pb_u32(pb *b, uint32_t v) { pb_put(b, &v, 4); }

static void pb_flat(pb *b, const void *p, uint32_t n)
{
    pb_u32(b, n);
    pb_u32(b, 0);
    pb_put(b, p, n);
}

static pb pb_new(void)
{
    static const char tok[] = "android.gui.IGraphicBufferProducer";
    pb b;
    memset(&b, 0, sizeof b);
    pb_u32(&b, 0x100);
    pb_u32(&b, sizeof tok - 1);
    for (uint32_t i = 0; i < sizeof tok; i++) {
        uint16_t ch = (uint8_t)tok[i];
        memcpy(b.d + b.n + 2 * i, &ch, 2);
    }
    b.n += (((uint32_t)sizeof tok * 2u) + 3u) & ~3u;
    return b;
}

static int32_t rep_i32(const rep *r, uint32_t off)
{
    int32_t v;
    memcpy(&v, r->p + off, 4);
    return v;
}

static int32_t rep_status(const rep *r) { return rep_i32(r, r->n - 4); }

static uint32_t get_display(rl_ctx *c, tc_obj root, uint32_t cmd, uint32_t mode, tc_obj *out)
{
    tc_req q = tc_new(cmd);
    tc_put32(&q, mode);
    return tc_sub(c, root, q, out);
}

static uint32_t sub0(rl_ctx *c, tc_obj o, uint32_t cmd, tc_obj *out)
{
    return tc_sub(c, o, tc_new(cmd), out);
}

static uint32_t u64cmd(rl_ctx *c, tc_obj o, uint32_t cmd, uint64_t v)
{
    tc_req q = tc_new(cmd);
    tc_put64(&q, v);
    return tc_send(c, o, q).result;
}

static bool fx_setup(fx *f, bool with_layer)
{
    memset(f, 0, sizeof *f);
    f->c = t_boot(NULL);
    f->heap = t_heap(f->c, 0x200000);
    if (!f->heap)
        return false;
    f->root = tc_open_service(f->c, f->heap, "vi:u");
    if (!f->root.handle || get_display(f->c, f->root, 0, 0, &f->disp) != 0)
        return false;
    if (sub0(f->c, f->disp, 100, &f->relay) != 0)
        return false;
    tc_res r = tc_send(f->c, f->disp, tc_new(1011));
    if (r.result != 0 || tc_u64(&r, 0) != 0)
        return false;
    if (!with_layer)
        return true;
    if (!rl_srv_vi_layer_hook || rl_srv_vi_layer_hook(&f->layer) != 0)
        return false;
    tc_req q = tc_new(2020);
    uint8_t name[0x40] = "Default";
    tc_put(&q, name, sizeof name);
    tc_put64(&q, f->layer);
    tc_put64(&q, 0);
    q.send_pid = true;
    q.nb = 1;
    q.b_addr[0] = f->heap + WIN_BUF;
    q.b_size[0] = 0x100;
    r = tc_send(f->c, f->disp, q);
    if (r.result != 0 || tc_u64(&r, 0) != 0x3C)
        return false;
    uint32_t w[3];
    memcpy(w, gm(f->heap + WIN_BUF + 16), 12);
    f->binder = (int32_t)w[2];
    return f->binder > 0;
}

static rep tx(fx *f, int32_t id, uint32_t code, const pb *in, int mode)
{
    rep out;
    memset(&out, 0, sizeof out);
    uint32_t total = 16 + in->n;
    uint32_t hdr[4] = { in->n, 16, 0, total };
    memcpy(gm(f->heap + PARCEL_IN), hdr, 16);
    memcpy(gm(f->heap + PARCEL_IN + 16), in->d, in->n);
    memset(gm(f->heap + PARCEL_OUT), 0, 0x800);
    tc_req q = tc_new(mode == 0 ? 0 : 3);
    tc_put32(&q, (uint32_t)id);
    tc_put32(&q, code);
    tc_put32(&q, 0);
    if (mode == 2) {
        q.nx = 1;
        q.x_addr[0] = f->heap + PARCEL_IN;
        q.x_size[0] = total;
        q.nc = 1;
        q.c_addr[0] = f->heap + PARCEL_OUT;
        q.c_size[0] = 0x800;
    } else {
        q.na = 1;
        q.a_addr[0] = f->heap + PARCEL_IN;
        q.a_size[0] = total;
        q.nb = 1;
        q.b_addr[0] = f->heap + PARCEL_OUT;
        q.b_size[0] = 0x800;
    }
    tc_res r = tc_send(f->c, f->relay, q);
    out.rc = r.svc_rc ? r.svc_rc : r.result;
    if (out.rc == 0) {
        uint32_t oh[4];
        memcpy(oh, gm(f->heap + PARCEL_OUT), 16);
        out.n = oh[0] <= sizeof out.p ? oh[0] : 0;
        memcpy(out.p, gm(f->heap + PARCEL_OUT + oh[1]), out.n);
    }
    return out;
}

static void gfx_make(uint8_t g[VI_GFX_SIZE], uint32_t w, uint32_t h, uint32_t id, uint32_t handle)
{
    vi_gfx v;
    memset(&v, 0, sizeof v);
    vi_gfx_set_u32(&v, 0, 0x47424652);
    vi_gfx_set_u32(&v, VI_GFX_WIDTH, w);
    vi_gfx_set_u32(&v, VI_GFX_HEIGHT, h);
    vi_gfx_set_u32(&v, VI_GFX_STRIDE, w);
    vi_gfx_set_u32(&v, VI_GFX_FORMAT, 1);
    vi_gfx_set_u32(&v, VI_GFX_USAGE, GFX_USAGE);
    vi_gfx_set_u32(&v, 0x18, 42);
    vi_gfx_set_u32(&v, VI_GFX_INTS_COUNT, VI_GFX_INTS);
    vi_gfx_set_u32(&v, 0x28, 0xFFFFFFFFu);
    vi_gfx_set_u32(&v, VI_GFX_NVMAP_ID, id);
    vi_gfx_set_u32(&v, 0x34, 0xDAFFCAFFu);
    vi_gfx_set_u32(&v, 0x38, 42);
    vi_gfx_set_u32(&v, VI_GFX_NV_USAGE, GFX_USAGE);
    vi_gfx_set_u32(&v, 0x44, 1);
    vi_gfx_set_u32(&v, 0x48, 1);
    vi_gfx_set_u32(&v, 0x4C, w);
    vi_gfx_set_u32(&v, 0x50, w * h * 4);
    vi_gfx_set_u32(&v, 0x54, 1);
    vi_gfx_set_u32(&v, VI_GFX_SURF0, w);
    vi_gfx_set_u32(&v, VI_GFX_SURF0 + 4, h);
    vi_gfx_set_u32(&v, VI_GFX_SURF_COLOR, 0x0B);
    vi_gfx_set_u32(&v, VI_GFX_SURF_LAYOUT, 2);
    vi_gfx_set_u32(&v, VI_GFX_SURF_PITCH, w * 4);
    vi_gfx_set_u32(&v, VI_GFX_SURF_HANDLE, handle);
    vi_gfx_set_u32(&v, VI_GFX_SURF_OFFSET, 0x100);
    vi_gfx_set_u32(&v, VI_GFX_SURF_KIND, 0xFE);
    vi_gfx_set_u32(&v, VI_GFX_SURF_BLOCK, 4);
    vi_gfx_set_u32(&v, VI_GFX_SURF_SIZE, w * h * 4);
    memcpy(g, v.raw, VI_GFX_SIZE);
}

static int32_t t_connect(fx *f, int mode, int api, rep *out)
{
    pb b = pb_new();
    pb_u32(&b, 0);
    pb_u32(&b, (uint32_t)api);
    pb_u32(&b, 0);
    *out = tx(f, f->binder, 10, &b, mode);
    return out->rc ? (int32_t)out->rc : rep_status(out);
}

static int32_t t_prealloc(fx *f, int mode, int slot, uint32_t w, uint32_t h, uint32_t handle)
{
    pb b = pb_new();
    pb_u32(&b, (uint32_t)slot);
    pb_u32(&b, 1);
    uint8_t g[VI_GFX_SIZE];
    gfx_make(g, w, h, handle + 100, handle);
    pb_flat(&b, g, sizeof g);
    rep r = tx(f, f->binder, 14, &b, mode);
    return r.rc ? (int32_t)r.rc : rep_status(&r);
}

static int32_t t_dequeue(fx *f, int mode, bool async, int *slot)
{
    pb b = pb_new();
    pb_u32(&b, async);
    pb_u32(&b, 1280);
    pb_u32(&b, 720);
    pb_u32(&b, 1);
    pb_u32(&b, GFX_USAGE);
    rep r = tx(f, f->binder, 3, &b, mode);
    if (r.rc)
        return (int32_t)r.rc;
    *slot = rep_i32(&r, 0);
    return rep_status(&r);
}

static int32_t t_request(fx *f, int mode, int slot, rep *out)
{
    pb b = pb_new();
    pb_u32(&b, (uint32_t)slot);
    *out = tx(f, f->binder, 1, &b, mode);
    return out->rc ? (int32_t)out->rc : rep_status(out);
}

static int32_t t_queue(fx *f, int mode, int slot, const int32_t crop[4], uint32_t async, uint32_t *pending)
{
    uint8_t in[0x54];
    memset(in, 0, sizeof in);
    int32_t one = 1;
    uint64_t ts = 77;
    memcpy(in, &ts, 8);
    memcpy(in + 12, crop, 16);
    memcpy(in + 40, &async, 4);
    memcpy(in + 44, &one, 4);
    pb b = pb_new();
    pb_u32(&b, (uint32_t)slot);
    pb_flat(&b, in, sizeof in);
    rep r = tx(f, f->binder, 7, &b, mode);
    if (r.rc)
        return (int32_t)r.rc;
    uint32_t p;
    memcpy(&p, r.p + 12, 4);
    if (pending)
        *pending = p;
    return rep_status(&r);
}

static bool ev_signaled(uint32_t h)
{
    hos_lock();
    hos_kobj *o = hos_handle_get(h, KOBJ_READABLE_EVENT);
    bool s = o && ((hos_revent *)o)->signaled;
    hos_unlock();
    if (o)
        hos_kobj_unref(o);
    return s;
}

static void ev_clear(uint32_t h)
{
    hos_lock();
    hos_kobj *o = hos_handle_get(h, KOBJ_READABLE_EVENT);
    hos_unlock();
    if (o) {
        rl_hos_event_clear((hos_revent *)o);
        hos_kobj_unref(o);
    }
}

TEST(vi_display_basic)
{
    fx f;
    CHECK(fx_setup(&f, false));
    rl_ctx *c = f.c;

    tc_req q = tc_new(1000);
    q.nb = 1;
    q.b_addr[0] = f.heap + LIST_BUF;
    q.b_size[0] = 0x60 * 6;
    tc_res r = tc_send(c, f.disp, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 1);
    CHECK(strcmp((const char *)gm(f.heap + LIST_BUF), "Default") == 0);
    uint64_t w, h;
    memcpy(&w, gm(f.heap + LIST_BUF + 0x50), 8);
    memcpy(&h, gm(f.heap + LIST_BUF + 0x58), 8);
    CHECK_EQ(w, 1280);
    CHECK_EQ(h, 720);

    tc_req o = tc_new(1010);
    uint8_t name[0x40] = "Internal";
    tc_put(&o, name, sizeof name);
    r = tc_send(c, f.disp, o);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 3);
    r = tc_send(c, f.disp, o);
    CHECK_EQ(r.result, VI_RESULT_ALREADY_OPENED);
    r = tc_send(c, f.disp, tc_new(1011));
    CHECK_EQ(r.result, VI_RESULT_ALREADY_OPENED);

    memset(name, 0, sizeof name);
    tc_req e = tc_new(1010);
    tc_put(&e, name, sizeof name);
    CHECK_EQ(tc_send(c, f.disp, e).result, VI_RESULT_INVALID_VALUE);
    memcpy(name, "Bogus", 6);
    tc_req b = tc_new(1010);
    tc_put(&b, name, sizeof name);
    CHECK_EQ(tc_send(c, f.disp, b).result, VI_RESULT_INVALID_VALUE);

    CHECK_EQ(u64cmd(c, f.disp, 1020, 0), 0);
    CHECK_EQ(u64cmd(c, f.disp, 1020, 0), VI_RESULT_INVALID_VALUE);
    CHECK_EQ(u64cmd(c, f.disp, 1020, 3), 0);
    CHECK_EQ(u64cmd(c, f.disp, 1020, 99), VI_RESULT_INVALID_VALUE);

    r = tc_send(c, f.disp, tc_new(1011));
    CHECK_EQ(r.result, 0);
    tc_req res = tc_new(1102);
    tc_put64(&res, 0);
    r = tc_send(c, f.disp, res);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 1280);
    CHECK_EQ(tc_u64(&r, 8), 720);
    rl_vi_set_resolution(1920, 1080);
    r = tc_send(c, f.disp, res);
    CHECK_EQ(tc_u64(&r, 0), 1920);
    CHECK_EQ(tc_u64(&r, 8), 1080);
    CHECK_EQ(u64cmd(c, f.disp, 1101, 0), 0);
}

TEST(vi_permissions)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj u = tc_open_service(c, heap, "vi:u");
    tc_obj s = tc_open_service(c, heap, "vi:s");
    tc_obj m = tc_open_service(c, heap, "vi:m");
    CHECK(u.handle && s.handle && m.handle);
    tc_obj d;
    CHECK_EQ(get_display(c, u, 0, 1, &d), VI_RESULT_PERMISSION_DENIED);
    CHECK_EQ(get_display(c, u, 0, 2, &d), VI_RESULT_PERMISSION_DENIED);
    CHECK_EQ(get_display(c, m, 2, 0, &d), VI_RESULT_PERMISSION_DENIED);
    CHECK_EQ(get_display(c, m, 2, 2, &d), VI_RESULT_PERMISSION_DENIED);
    CHECK_EQ(get_display(c, s, 1, 0, &d), VI_RESULT_PERMISSION_DENIED);
    CHECK_EQ(get_display(c, u, 99, 0, &d), SF_RESULT_UNKNOWN_COMMAND);

    tc_obj ud, sd, md, x;
    CHECK_EQ(get_display(c, u, 0, 0, &ud), 0);
    CHECK_EQ(get_display(c, s, 1, 1, &sd), 0);
    CHECK_EQ(get_display(c, s, 1, 2, &x), 0);
    CHECK_EQ(get_display(c, m, 2, 1, &md), 0);

    CHECK_EQ(sub0(c, ud, 100, &x), 0);
    CHECK_EQ(sub0(c, ud, 101, &x), VI_RESULT_PERMISSION_DENIED);
    CHECK_EQ(sub0(c, ud, 102, &x), VI_RESULT_PERMISSION_DENIED);
    CHECK_EQ(sub0(c, ud, 103, &x), VI_RESULT_PERMISSION_DENIED);

    tc_obj relay, sys, mgr, ind;
    CHECK_EQ(sub0(c, md, 100, &relay), 0);
    CHECK_EQ(sub0(c, md, 101, &sys), 0);
    CHECK_EQ(sub0(c, md, 102, &mgr), 0);
    CHECK_EQ(sub0(c, md, 103, &ind), 0);

    tc_req mode = tc_new(3200);
    tc_put64(&mode, 0);
    tc_res r = tc_send(c, sys, mode);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u32(&r, 0), 1280);
    CHECK_EQ(tc_u32(&r, 4), 720);

    tc_req ml = tc_new(2010);
    tc_put32(&ml, 0);
    tc_put32(&ml, 0);
    tc_put64(&ml, 0);
    tc_put64(&ml, 0);
    r = tc_send(c, mgr, ml);
    CHECK_EQ(r.result, 0);
    uint64_t id = tc_u64(&r, 0);
    CHECK(id != 0);
    CHECK_EQ(rl_vi_layer_count(), 1);
    uint32_t lw = 0, lh = 0;
    CHECK(rl_vi_layer_size(id, &lw, &lh));
    CHECK_EQ(lw, 1280);
    CHECK_EQ(lh, 720);
    tc_req size = tc_new(2203);
    tc_put64(&size, id);
    tc_put64(&size, 1920);
    tc_put64(&size, 1080);
    CHECK_EQ(tc_send(c, sys, size).result, 0);
    CHECK(rl_vi_layer_size(id, &lw, &lh));
    CHECK_EQ(lw, 1920);
    CHECK_EQ(lh, 1080);
    tc_req bad = tc_new(2203);
    tc_put64(&bad, id + 100);
    tc_put64(&bad, 640);
    tc_put64(&bad, 480);
    CHECK_EQ(tc_send(c, sys, bad).result, VI_RESULT_INVALID_VALUE);
    CHECK(!rl_vi_layer_size(id + 100, &lw, &lh));
    CHECK_EQ(u64cmd(c, mgr, 2011, id), 0);
    CHECK_EQ(u64cmd(c, mgr, 2011, id), VI_RESULT_INVALID_VALUE);
    CHECK_EQ(rl_vi_layer_count(), 0);

    tc_req stray = tc_new(2012);
    tc_put32(&stray, 0);
    tc_put32(&stray, 0);
    tc_put64(&stray, 0);
    stray.nb = 1;
    stray.b_addr[0] = heap + WIN_BUF;
    stray.b_size[0] = 0x100;
    r = tc_send(c, mgr, stray);
    CHECK_EQ(r.result, 0);
    uint64_t sid = tc_u64(&r, 0);
    CHECK_EQ(tc_u64(&r, 8), 0x3C);
    CHECK_EQ(u64cmd(c, mgr, 2011, sid), VI_RESULT_PERMISSION_DENIED);
    CHECK_EQ(u64cmd(c, ud, 2031, sid), 0);
}

TEST(vi_layer_parcel)
{
    fx f;
    CHECK(fx_setup(&f, true));
    uint32_t hdr[4];
    memcpy(hdr, gm(f.heap + WIN_BUF), 16);
    CHECK_EQ(hdr[0], 0x28);
    CHECK_EQ(hdr[1], 0x10);
    CHECK_EQ(hdr[2], 4);
    CHECK_EQ(hdr[3], 0x38);
    uint32_t p[4];
    memcpy(p, gm(f.heap + WIN_BUF + 16), 16);
    CHECK_EQ(p[0], 2);
    CHECK_EQ(p[1], 0);
    CHECK_EQ((int32_t)p[2], f.binder);
    CHECK(memcmp(gm(f.heap + WIN_BUF + 16 + 24), "dispdrv\0", 8) == 0);

    tc_req q = tc_new(2020);
    uint8_t name[0x40] = "Default";
    tc_put(&q, name, sizeof name);
    tc_put64(&q, f.layer);
    tc_put64(&q, 0);
    q.send_pid = true;
    q.nb = 1;
    q.b_addr[0] = f.heap + WIN_BUF;
    q.b_size[0] = 0x100;
    CHECK_EQ(tc_send(f.c, f.disp, q).result, VI_RESULT_INVALID_ARGUMENTS);
    CHECK_EQ(u64cmd(f.c, f.disp, 2021, f.layer), 0);
    CHECK_EQ(tc_send(f.c, f.disp, q).result, 0);
    CHECK_EQ(u64cmd(f.c, f.disp, 2021, f.layer), 0);

    tc_req un = tc_new(2020);
    tc_put(&un, name, sizeof name);
    tc_put64(&un, 9999);
    tc_put64(&un, 0);
    un.nb = 1;
    un.b_addr[0] = f.heap + WIN_BUF;
    un.b_size[0] = 0x100;
    CHECK_EQ(tc_send(f.c, f.disp, un).result, VI_RESULT_INVALID_ARGUMENTS);
    CHECK_EQ(u64cmd(f.c, f.disp, 2021, 9999), VI_RESULT_INVALID_VALUE);
    CHECK_EQ(u64cmd(f.c, f.disp, 2031, f.layer), VI_RESULT_PERMISSION_DENIED);
    CHECK_EQ(u64cmd(f.c, f.disp, 2031, 9999), VI_RESULT_INVALID_VALUE);

    tc_req sq = tc_new(2030);
    tc_put32(&sq, 0);
    tc_put32(&sq, 0);
    tc_put64(&sq, 0);
    sq.nb = 1;
    sq.b_addr[0] = f.heap + WIN_BUF;
    sq.b_size[0] = 0x100;
    tc_res r = tc_send(f.c, f.disp, sq);
    CHECK_EQ(r.result, 0);
    uint64_t sid = tc_u64(&r, 0);
    CHECK(sid != f.layer);
    CHECK_EQ(tc_u64(&r, 8), 0x3C);
    CHECK_EQ(rl_vi_layer_count(), 2);
    uint64_t render;
    CHECK(rl_vi_render_layer(&render));
    CHECK_EQ(render, sid);
    CHECK_EQ(u64cmd(f.c, f.disp, 2031, sid), 0);
    CHECK_EQ(u64cmd(f.c, f.disp, 2031, sid), VI_RESULT_INVALID_VALUE);
    CHECK_EQ(rl_vi_layer_count(), 1);
    CHECK(rl_vi_render_layer(&render));
    CHECK_EQ(render, f.layer);

    tc_req small = tc_new(2030);
    tc_put32(&small, 0);
    tc_put32(&small, 0);
    tc_put64(&small, 0);
    CHECK(tc_send(f.c, f.disp, small).result != 0);
    CHECK_EQ(rl_vi_layer_count(), 1);
}

TEST(vi_producer_sequence)
{
    fx f;
    CHECK(fx_setup(&f, true));

    tc_req nh = tc_new(2);
    tc_put32(&nh, (uint32_t)f.binder);
    tc_put32(&nh, 0xF);
    tc_res r = tc_send(f.c, f.relay, nh);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.ncopy, 1);
    uint32_t ev = r.copy[0];
    CHECK(!ev_signaled(ev));

    tc_req badtype = tc_new(2);
    tc_put32(&badtype, (uint32_t)f.binder);
    tc_put32(&badtype, 7);
    CHECK_EQ(tc_send(f.c, f.relay, badtype).result, VI_RESULT_INVALID_VALUE);

    tc_req adj = tc_new(1);
    tc_put32(&adj, (uint32_t)f.binder);
    tc_put32(&adj, 1);
    tc_put32(&adj, 0);
    CHECK_EQ(tc_send(f.c, f.relay, adj).result, 0);

    rep rp;
    CHECK_EQ(t_connect(&f, 1, 2, &rp), 0);
    uint32_t cw, ch;
    memcpy(&cw, rp.p, 4);
    memcpy(&ch, rp.p + 4, 4);
    CHECK_EQ(cw, 1280);
    CHECK_EQ(ch, 720);

    CHECK_EQ(t_prealloc(&f, 2, 0, 1280, 720, 0x10), 0);
    CHECK_EQ(t_prealloc(&f, 0, 1, 1280, 720, 0x11), 0);
    CHECK(ev_signaled(ev));

    pb qb = pb_new();
    pb_u32(&qb, 0);
    rep q = tx(&f, f.binder, 9, &qb, 1);
    CHECK_EQ(q.rc, 0);
    CHECK_EQ(rep_i32(&q, 0), 1280);
    CHECK_EQ(rep_status(&q), 0);

    const int32_t crop[4] = { 0, 0, 1280, 720 };
    uint64_t frames_seen = 0;
    for (int i = 0; i < 6; i++) {
        int mode = i % 3;
        int slot = -1;
        CHECK_EQ(t_dequeue(&f, mode, false, &slot), 0);
        CHECK_EQ(slot, 0);
        CHECK_EQ(t_request(&f, mode, slot, &rp), 0);
        uint32_t has, size;
        memcpy(&has, rp.p, 4);
        memcpy(&size, rp.p + 4, 4);
        CHECK_EQ(has, 1);
        CHECK_EQ(size, VI_GFX_SIZE);
        uint32_t nvid, handle;
        memcpy(&nvid, rp.p + 12 + VI_GFX_NVMAP_ID, 4);
        memcpy(&handle, rp.p + 12 + VI_GFX_SURF_HANDLE, 4);
        CHECK_EQ(handle, 0x10 + (uint32_t)slot);
        CHECK_EQ(nvid, 0x10 + (uint32_t)slot + 100);

        uint32_t pending = 0;
        CHECK_EQ(t_queue(&f, mode, slot, crop, 0, &pending), 0);
        CHECK_EQ(pending, 1);

        int blocked = -1;
        CHECK_EQ(t_dequeue(&f, mode, true, &blocked), 0);
        CHECK_EQ(blocked, 1);
        CHECK_EQ(t_request(&f, mode, blocked, &rp), 0);
        CHECK_EQ(t_queue(&f, mode, blocked, crop, 0, &pending), 0);
        CHECK_EQ(pending, 2);
        int none = -1;
        CHECK_EQ(t_dequeue(&f, mode, true, &none), -11);

        rl_vi_frame fr;
        CHECK_EQ(rl_vi_acquire_frame(f.layer, &fr), 0);
        CHECK_EQ(fr.slot, (uint32_t)slot);
        CHECK_EQ(fr.nvmap_handle, 0x10 + (uint32_t)slot);
        CHECK_EQ(fr.nvmap_id, 0x10 + (uint32_t)slot + 100);
        CHECK_EQ(fr.offset, 0x100);
        CHECK_EQ(fr.width, 1280);
        CHECK_EQ(fr.height, 720);
        CHECK_EQ(fr.format, 1);
        CHECK_EQ(fr.stride, 1280);
        CHECK_EQ(fr.usage, GFX_USAGE);
        CHECK_EQ(fr.pitch, 5120);
        CHECK_EQ(fr.layout, 2);
        CHECK_EQ(fr.kind, 0xFE);
        CHECK_EQ(fr.block_height_log2, 4);
        CHECK_EQ(fr.crop[2], 1280);
        CHECK_EQ(fr.crop[3], 720);
        CHECK_EQ(fr.swap_interval, 1);
        CHECK_EQ(fr.timestamp, 77);
        CHECK(!fr.fence_present);
        CHECK_EQ(fr.frame_number, frames_seen + 1);
        frames_seen += 2;

        rl_vi_frame fr2;
        CHECK_EQ(rl_vi_acquire_frame(f.layer, &fr2), 0);
        CHECK_EQ(fr2.slot, (uint32_t)(1 - slot));
        CHECK_EQ(fr2.frame_number, fr.frame_number + 1);
        CHECK_EQ(rl_vi_acquire_frame(f.layer, &fr), VI_RESULT_NO_FRAME);

        CHECK_EQ(t_dequeue(&f, mode, true, &none), -11);
        CHECK_EQ(rl_vi_release_frame(f.layer, (uint32_t)slot), 0);
        CHECK(ev_signaled(ev));
        CHECK_EQ(rl_vi_release_frame(f.layer, (uint32_t)slot), VI_RESULT_INVALID_VALUE);
        CHECK_EQ(rl_vi_release_frame(f.layer, (uint32_t)(1 - slot)), 0);
    }

    int slot = -1;
    CHECK_EQ(t_dequeue(&f, 0, false, &slot), 0);
    CHECK_EQ(t_request(&f, 0, slot, &rp), 0);
    pb cb = pb_new();
    pb_u32(&cb, (uint32_t)slot);
    uint8_t fence[VI_FENCE_SIZE];
    vi_fence_none(fence);
    pb_flat(&cb, fence, sizeof fence);
    rep cr = tx(&f, f.binder, 8, &cb, 1);
    CHECK_EQ(cr.rc, 0);
    CHECK_EQ(rep_status(&cr), 0);
    int again = -1;
    CHECK_EQ(t_dequeue(&f, 0, false, &again), 0);

    pb dis = pb_new();
    pb_u32(&dis, 2);
    rep dr = tx(&f, f.binder, 11, &dis, 1);
    CHECK_EQ(dr.rc, 0);
    CHECK_EQ(rep_status(&dr), 0);
    rl_vi_frame none_fr;
    CHECK_EQ(rl_vi_acquire_frame(f.layer, &none_fr), VI_RESULT_NO_FRAME);
}

TEST(vi_binder_errors)
{
    fx f;
    CHECK(fx_setup(&f, true));
    rep rp;
    pb b = pb_new();
    pb_u32(&b, 0);
    rep r = tx(&f, 777, 9, &b, 1);
    CHECK_EQ(r.rc, VI_RESULT_INVALID_VALUE);

    tc_req nh = tc_new(2);
    tc_put32(&nh, 777);
    tc_put32(&nh, 0xF);
    CHECK_EQ(tc_send(f.c, f.relay, nh).result, VI_RESULT_INVALID_VALUE);

    pb badtok;
    memset(&badtok, 0, sizeof badtok);
    pb_u32(&badtok, 0);
    pb_u32(&badtok, 3);
    pb_put(&badtok, "a\0b\0c\0\0\0", 8);
    r = tx(&f, f.binder, 9, &badtok, 0);
    CHECK_EQ(r.rc, 0);
    CHECK_EQ(r.n, 0);

    pb unk = pb_new();
    r = tx(&f, f.binder, 99, &unk, 1);
    CHECK_EQ(r.rc, 0);
    CHECK_EQ(rep_status(&r), -74);

    pb q9 = pb_new();
    pb_u32(&q9, 55);
    r = tx(&f, f.binder, 9, &q9, 2);
    CHECK_EQ(rep_status(&r), -22);

    int slot = -1;
    CHECK_EQ(t_connect(&f, 1, 7, &rp), -22);
    CHECK_EQ(t_connect(&f, 1, 2, &rp), 0);
    CHECK_EQ(t_connect(&f, 1, 2, &rp), -22);

    pb withl = pb_new();
    pb_u32(&withl, 1);
    pb_u32(&withl, 2);
    pb_u32(&withl, 0);
    r = tx(&f, f.binder, 10, &withl, 1);
    CHECK_EQ(rep_status(&r), -22);

    CHECK_EQ(t_request(&f, 1, 0, &rp), -22);
    CHECK_EQ(t_request(&f, 1, 70, &rp), -22);
    const int32_t crop[4] = { 0, 0, 1280, 720 };
    CHECK_EQ(t_queue(&f, 1, 0, crop, 0, NULL), -22);

    CHECK_EQ(t_prealloc(&f, 1, 0, 1280, 720, 0x10), 0);
    CHECK_EQ(t_prealloc(&f, 1, 1, 1280, 720, 0x11), 0);
    CHECK_EQ(t_prealloc(&f, 1, 99, 1280, 720, 0x11), -22);

    pb bad_dq = pb_new();
    pb_u32(&bad_dq, 0);
    pb_u32(&bad_dq, 1280);
    pb_u32(&bad_dq, 0);
    pb_u32(&bad_dq, 1);
    pb_u32(&bad_dq, GFX_USAGE);
    r = tx(&f, f.binder, 3, &bad_dq, 1);
    CHECK_EQ(rep_i32(&r, 0), -1);
    CHECK_EQ(rep_status(&r), -22);

    pb mism = pb_new();
    pb_u32(&mism, 0);
    pb_u32(&mism, 640);
    pb_u32(&mism, 480);
    pb_u32(&mism, 1);
    pb_u32(&mism, GFX_USAGE);
    r = tx(&f, f.binder, 3, &mism, 1);
    CHECK_EQ(rep_status(&r), -19);

    CHECK_EQ(t_dequeue(&f, 1, false, &slot), 0);
    int32_t bad_crop[4] = { 0, 0, 2000, 720 };
    CHECK_EQ(t_queue(&f, 1, slot, bad_crop, 0, NULL), -22);
    CHECK_EQ(t_request(&f, 1, slot, &rp), 0);
    CHECK_EQ(t_queue(&f, 1, slot, bad_crop, 0, NULL), -22);
    CHECK_EQ(t_queue(&f, 1, slot, crop, 0, NULL), 0);
    CHECK_EQ(t_queue(&f, 1, slot, crop, 0, NULL), -22);

    pb badscale = pb_new();
    pb_u32(&badscale, (uint32_t)slot);
    uint8_t in[0x54];
    memset(in, 0, sizeof in);
    uint32_t scale = 9;
    memcpy(in + 28, &scale, 4);
    pb_flat(&badscale, in, sizeof in);
    r = tx(&f, f.binder, 7, &badscale, 1);
    CHECK_EQ(rep_status(&r), -22);

    pb shortq = pb_new();
    pb_u32(&shortq, 0);
    pb_u32(&shortq, 10);
    pb_u32(&shortq, 0);
    pb_put(&shortq, in, 12);
    r = tx(&f, f.binder, 7, &shortq, 1);
    CHECK_EQ(rep_status(&r), -22);

    pb dis = pb_new();
    pb_u32(&dis, 3);
    r = tx(&f, f.binder, 11, &dis, 1);
    CHECK_EQ(rep_status(&r), -22);

    pb sc = pb_new();
    pb_u32(&sc, 65);
    r = tx(&f, f.binder, 2, &sc, 1);
    CHECK_EQ(rep_status(&r), -22);

    tc_req tiny = tc_new(0);
    tc_put32(&tiny, (uint32_t)f.binder);
    tc_put32(&tiny, 9);
    tc_put32(&tiny, 0);
    tiny.na = 1;
    tiny.a_addr[0] = f.heap + PARCEL_IN;
    tiny.a_size[0] = 8;
    tiny.nb = 1;
    tiny.b_addr[0] = f.heap + PARCEL_OUT;
    tiny.b_size[0] = 0x100;
    CHECK(tc_send(f.c, f.relay, tiny).result != 0);

    tc_req nobuf = tc_new(3);
    tc_put32(&nobuf, (uint32_t)f.binder);
    tc_put32(&nobuf, 9);
    tc_put32(&nobuf, 0);
    CHECK_EQ(tc_send(f.c, f.relay, nobuf).result, SF_RESULT_INVALID_HEADER);

    CHECK_EQ(u64cmd(f.c, f.disp, 2021, f.layer), 0);
    CHECK_EQ(u64cmd(f.c, f.disp, 2031, 12345), VI_RESULT_INVALID_VALUE);
}

TEST(vi_destroyed_layer)
{
    fx f;
    CHECK(fx_setup(&f, true));
    tc_obj mgr;
    tc_obj md;
    tc_obj mroot = tc_open_service(f.c, f.heap, "vi:m");
    CHECK(mroot.handle);
    CHECK_EQ(get_display(f.c, mroot, 2, 1, &md), 0);
    CHECK_EQ(sub0(f.c, md, 102, &mgr), 0);
    rep rp;
    CHECK_EQ(t_connect(&f, 1, 2, &rp), 0);
    CHECK_EQ(u64cmd(f.c, mgr, 2011, f.layer), 0);
    CHECK_EQ(t_connect(&f, 1, 2, &rp), (int32_t)VI_RESULT_INVALID_VALUE);
    uint64_t id;
    CHECK(!rl_vi_render_layer(&id));
    rl_vi_frame fr;
    CHECK_EQ(rl_vi_acquire_frame(f.layer, &fr), VI_RESULT_INVALID_VALUE);
    CHECK_EQ(rl_vi_release_frame(f.layer, 0), VI_RESULT_INVALID_VALUE);
}

TEST(vi_droppable_and_history)
{
    fx f;
    CHECK(fx_setup(&f, true));
    rep rp;
    CHECK_EQ(t_connect(&f, 1, 1, &rp), 0);
    CHECK_EQ(t_prealloc(&f, 1, 0, 1280, 720, 0x10), 0);
    CHECK_EQ(t_prealloc(&f, 1, 1, 1280, 720, 0x11), 0);
    const int32_t crop[4] = { 0, 0, 1280, 720 };

    int slot = -1;
    CHECK_EQ(t_dequeue(&f, 1, false, &slot), 0);
    CHECK_EQ(t_request(&f, 1, slot, &rp), 0);
    uint32_t pending;
    CHECK_EQ(t_queue(&f, 1, slot, crop, 1, &pending), 0);
    CHECK_EQ(pending, 1);
    int slot2 = -1;
    CHECK_EQ(t_dequeue(&f, 1, true, &slot2), 0);
    CHECK_EQ(t_request(&f, 1, slot2, &rp), 0);
    CHECK_EQ(t_queue(&f, 1, slot2, crop, 1, &pending), 0);
    CHECK_EQ(pending, 1);

    rl_vi_frame fr;
    CHECK_EQ(rl_vi_acquire_frame(f.layer, &fr), 0);
    CHECK_EQ(fr.slot, (uint32_t)slot2);
    CHECK_EQ(fr.frame_number, 2);
    CHECK_EQ(rl_vi_acquire_frame(f.layer, &fr), VI_RESULT_NO_FRAME);

    pb hb = pb_new();
    pb_u32(&hb, 3);
    rep h = tx(&f, f.binder, 17, &hb, 1);
    CHECK_EQ(h.rc, 0);
    CHECK_EQ(rep_i32(&h, 0), 0);
    CHECK_EQ(rep_i32(&h, 4), 3);
    uint64_t newest;
    memcpy(&newest, h.p + 8, 8);
    CHECK_EQ(newest, 2);
    int32_t state;
    memcpy(&state, h.p + 8 + 24, 4);
    CHECK_EQ(state, 3);
}

TEST(vi_vsync_timeline)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    int64_t now = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
    int64_t a = vi_vsync_after(now);
    CHECK(a > now);
    CHECK(a - now <= VI_VSYNC_NS);
    CHECK_EQ(vi_vsync_after(a - 1), a);
    CHECK_EQ(vi_vsync_after(a), a + VI_VSYNC_NS);
    CHECK_EQ(vi_vsync_after(a + 10 * VI_VSYNC_NS + 5), a + 11 * VI_VSYNC_NS);
}

typedef struct sim_display {
    double period_ns;
    uint32_t every;
    uint64_t rng;
} sim_display;

static double sim_rand(sim_display *d)
{
    d->rng = d->rng * 6364136223846793005ull + 1442695040888963407ull;
    return (double)(d->rng >> 11) / (double)(1ull << 53);
}

/**
 * @brief Ten minutes of a guest that renders one frame per vi vsync (5-7 ms
 * of work) on a FIFO host display of the given refresh, which shows each
 * frame at the first refresh 2 ms after it is ready and reports the flip up
 * to a few ms late. Returns the frames the display showed for other than
 * one refresh (`every` refreshes for a 120 Hz panel at 60 frames/s).
 */
static uint32_t sim_slips(double host_hz, uint32_t every, bool lock, int64_t *last_phase)
{
    sim_display d = { 1e9 / host_hz, every, 7 };
    vi_clock c;
    vi_clock_init(&c, 0, lock);
    double step = d.period_ns * every, prev = -1;
    int64_t vs = 0;
    uint32_t slips = 0;
    for (uint32_t i = 0; i < 600u * 60u; i++) {
        vs = vi_clock_after(&c, vs);
        double ready = (double)vs + 5e6 + sim_rand(&d) * 2e6 + 2e6;
        double shown = ceil(ready / step) * step;
        if (prev >= 0 && shown <= prev)
            shown = prev + step;
        if (prev >= 0 && llround((shown - prev) / step) != 1)
            slips++;
        if (shown - (double)vs > 3.0 * VI_VSYNC_NS)
            vs = (int64_t)(shown - 3.0 * VI_VSYNC_NS);
        prev = shown;
        double late = -log(1.0 - sim_rand(&d)) * 5e5;
        if (sim_rand(&d) < 0.01)
            late += sim_rand(&d) * 1e7;
        vi_clock_flip(&c, (int64_t)(shown + late));
    }
    *last_phase = c.phase;
    return slips;
}

TEST(vi_vsync_follows_a_display_off_60_hz)
{
    static const struct {
        double hz;
        uint32_t every;
    } displays[] = { { 60.0 * (1 - 68e-6), 1 }, { 59.94, 1 }, { 60.02, 1 }, { 120.0 * (1 - 68e-6), 2 } };
    for (size_t i = 0; i < sizeof displays / sizeof displays[0]; i++) {
        int64_t phase = 0;
        uint32_t free_slips = sim_slips(displays[i].hz, displays[i].every, false, &phase);
        uint32_t locked_slips = sim_slips(displays[i].hz, displays[i].every, true, &phase);
        fprintf(stderr, "  %.4f Hz: %u slips free, %u locked (phase %lld us)\n", displays[i].hz, free_slips,
                locked_slips, (long long)(phase / 1000));
        CHECK(free_slips >= 1);
        CHECK_EQ(locked_slips, 0);
        CHECK(llabs(phase) < 1000000);
    }
}

TEST(vi_vsync_runs_free_on_a_display_not_near_60_hz)
{
    static const double rates[] = { 90.0, 144.0, 165.0, 175.0 };
    for (size_t r = 0; r < sizeof rates / sizeof rates[0]; r++) {
        vi_clock c;
        vi_clock_init(&c, 0, true);
        double step = 1e9 / rates[r];
        int64_t moved = 0, vs = 0;
        for (uint32_t i = 0; i < 600u * 60u; i++) {
            vs = vi_clock_after(&c, vs);
            int64_t t = (int64_t)(ceil(((double)vs + 6e6) / step) * step);
            int64_t m = vi_clock_flip(&c, t);
            moved += m < 0 ? -m : m;
        }
        fprintf(stderr, "  %.0f Hz: the grid moved %lld us in 10 minutes\n", rates[r], (long long)(moved / 1000));
        CHECK(moved < 20000000);
    }
}

TEST(vi_vsync_is_not_dragged_by_a_host_that_shows_on_arrival)
{
    vi_clock c;
    vi_clock_init(&c, 0, true);
    int64_t vs = 0, moved = 0;
    for (uint32_t i = 0; i < 600u * 60u; i++) {
        vs = vi_clock_after(&c, vs);
        int64_t m = vi_clock_flip(&c, vs + 6000000 + (int64_t)(i % 7) * 100000);
        moved += m;
    }
    fprintf(stderr, "  grid moved %lld us in 10 minutes\n", (long long)(moved / 1000));
    CHECK(llabs(moved) < 60ll * 1000000);
}

TEST(vi_vsync_grid_after)
{
    vi_clock c;
    vi_clock_init(&c, 1000, false);
    CHECK_EQ(vi_clock_after(&c, 999), 1000);
    CHECK_EQ(vi_clock_after(&c, 1000), 1000 + VI_VSYNC_NS);
    CHECK_EQ(vi_clock_after(&c, 1000 - VI_VSYNC_NS), 1000);
    CHECK_EQ(vi_clock_after(&c, 1000 - VI_VSYNC_NS - 1), 1000 - VI_VSYNC_NS);
    CHECK_EQ(vi_clock_after(&c, 1000 + 5 * VI_VSYNC_NS + 3), 1000 + 6 * VI_VSYNC_NS);
}

static int g_fail_threads;

/** @brief Makes pthread_create fail while g_fail_threads is set, as a host at its task limit does. */
int pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
    static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (g_fail_threads)
        return EAGAIN;
    if (!real)
        *(void **)&real = dlsym(RTLD_NEXT, "pthread_create");
    return real(t, a, fn, arg);
}

TEST(vi_vsync_event_errors_when_its_thread_cannot_start)
{
    fx f;
    CHECK(fx_setup(&f, false));
    rl_ctx *c = f.c;
    tc_req q = tc_new(5202);
    tc_put64(&q, 0);
    g_fail_threads = 1;
    tc_res r = tc_send(c, f.disp, q);
    g_fail_threads = 0;
    CHECK_EQ(r.result, KR_OUT_OF_RESOURCE);
    CHECK_EQ(r.ncopy, 0);
    r = tc_send(c, f.disp, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.ncopy, 1);
    rl_vi_stop();
}

TEST(vi_vsync_signal)
{
    fx f;
    CHECK(fx_setup(&f, false));
    rl_ctx *c = f.c;
    CHECK_EQ(u64cmd(c, f.disp, 5202, 4), VI_RESULT_INVALID_VALUE);
    tc_req q = tc_new(5202);
    tc_put64(&q, 0);
    tc_res r = tc_send(c, f.disp, q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.ncopy, 1);
    uint32_t ev = r.copy[0];
    CHECK_EQ(tc_send(c, f.disp, q).result, VI_RESULT_PERMISSION_DENIED);
    tc_req dbg = tc_new(5203);
    tc_put64(&dbg, 0);
    r = tc_send(c, f.disp, dbg);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.ncopy, 1);

    ev_clear(ev);
    CHECK(!ev_signaled(ev));
    rl_vi_vsync_now();
    CHECK(ev_signaled(ev));
    ev_clear(ev);
    while (!ev_signaled(ev)) {
        struct timespec ts = { 0, 10 * 1000000L };
        nanosleep(&ts, NULL);
    }
    rl_vi_stop();
    rl_vi_stop();
    ev_clear(ev);
    CHECK(!ev_signaled(ev));
}

static uint64_t press_seen_at(uint32_t vsyncs_per_present)
{
    rl_ports_script_reset();
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t first = vi_vsync_after((int64_t)now.tv_sec * 1000000000ll + now.tv_nsec);
    uint64_t n0 = vi_vsync_tick(first);
    char text[64];
    snprintf(text, sizeof text, "%llu+3:p1.a", (unsigned long long)(n0 + 10));
    if (!rl_ports_set_script(text))
        return 0;
    uint64_t seen = 0;
    for (uint32_t i = 1; i < 40; i++) {
        uint64_t n = vi_vsync_tick(first + (int64_t)i * VI_VSYNC_NS);
        if (n != n0 + i)
            return 0;
        if (i % vsyncs_per_present == 0)
            vi_guest_frame();
        float v;
        if (!seen && rl_ports_script_value("p1", "a", &v))
            seen = n - n0;
    }
    rl_ports_script_reset();
    return seen;
}

TEST(vi_input_script_follows_vsyncs_not_presents)
{
    fx f;
    CHECK(fx_setup(&f, false));
    rl_vi_stop();
    uint64_t fast = press_seen_at(1);
    uint64_t slow = press_seen_at(3);
    CHECK_EQ(fast, 10);
    CHECK_EQ(slow, 10);
}

TEST(vi_vsync_index_numbers_the_grid)
{
    vi_clock c;
    vi_clock_init(&c, 1000, false);
    CHECK_EQ(vi_clock_index(&c, 1000), 0);
    CHECK_EQ(vi_clock_index(&c, 1000 + 7 * VI_VSYNC_NS), 7);
    CHECK_EQ(vi_clock_index(&c, 1000 + 7 * VI_VSYNC_NS + 50000), 7);
    CHECK_EQ(vi_clock_index(&c, 1000 - 2 * VI_VSYNC_NS), -2);
    c.epoch += 40000;
    CHECK_EQ(vi_clock_index(&c, 1000 + 7 * VI_VSYNC_NS), 7);
}

TEST(vi_scaling_and_memory)
{
    fx f;
    CHECK(fx_setup(&f, false));
    rl_ctx *c = f.c;
    uint32_t modes[] = { 0, 1, 2, 3, 4, 5 };
    uint32_t want_rc[] = { VI_RESULT_INVALID_SCALING_MODE, VI_RESULT_INVALID_SCALING_MODE, 0,
                           VI_RESULT_INVALID_SCALING_MODE, 0, VI_RESULT_INVALID_ARGUMENTS };
    for (int i = 0; i < 6; i++) {
        tc_req q = tc_new(2102);
        tc_put32(&q, modes[i]);
        tc_res r = tc_send(c, f.disp, q);
        CHECK_EQ(r.result, want_rc[i]);
        if (modes[i] == 2)
            CHECK_EQ(tc_u64(&r, 0), 1);
        if (modes[i] == 4)
            CHECK_EQ(tc_u64(&r, 0), 4);
    }
    tc_req mi = tc_new(2460);
    tc_put64(&mi, 1280);
    tc_put64(&mi, 720);
    tc_res r = tc_send(c, f.disp, mi);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(tc_u64(&r, 0), 3932160);
    CHECK_EQ(tc_u64(&r, 8), 0x1000);
    tc_req neg = tc_new(2460);
    tc_put64(&neg, 0xFFFFFFFFull);
    tc_put64(&neg, 720);
    CHECK_EQ(tc_send(c, f.disp, neg).result, VI_RESULT_INVALID_LAYER_SIZE);
    tc_req ss = tc_new(2101);
    tc_put32(&ss, 2);
    tc_put32(&ss, 0);
    tc_put64(&ss, 1);
    CHECK_EQ(tc_send(c, f.disp, ss).result, 0);
}

static uint64_t cb_layer;
static int cb_count;

static void on_frame(uint64_t layer, void *user)
{
    cb_layer = layer;
    cb_count += *(int *)user;
}

TEST(vi_frame_callback_and_layers)
{
    fx f;
    CHECK(fx_setup(&f, true));
    int inc = 1;
    rl_vi_set_frame_callback(on_frame, &inc);
    rep rp;
    CHECK_EQ(t_connect(&f, 1, 2, &rp), 0);
    CHECK_EQ(t_prealloc(&f, 1, 0, 1280, 720, 0x10), 0);
    CHECK_EQ(t_prealloc(&f, 1, 1, 1280, 720, 0x11), 0);
    int slot = -1;
    CHECK_EQ(t_dequeue(&f, 1, false, &slot), 0);
    CHECK_EQ(t_request(&f, 1, slot, &rp), 0);
    const int32_t crop[4] = { 0, 0, 1280, 720 };
    CHECK_EQ(t_queue(&f, 1, slot, crop, 0, NULL), 0);
    CHECK_EQ(cb_count, 1);
    CHECK_EQ(cb_layer, f.layer);
    rl_vi_set_frame_callback(NULL, NULL);
    CHECK_EQ(rl_vi_layer_count(), 1);
    uint64_t id;
    CHECK(rl_vi_layer_id(0, &id));
    CHECK_EQ(id, f.layer);
    CHECK(!rl_vi_layer_id(1, &id));
}

TEST(vi_frame_trace_kinds_name_the_lines_it_writes)
{
    CHECK(rl_trace_kind_listed("", 'c'));
    CHECK(rl_trace_kind_listed("NUS", 'N'));
    CHECK(!rl_trace_kind_listed("NUS", 'c'));
    CHECK(!rl_trace_kind_listed("NUS", 'C'));
    CHECK(!rl_trace_kind_listed("NUS", 0));
}
