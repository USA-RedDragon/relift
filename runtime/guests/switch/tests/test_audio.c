/**
 * @file test_audio.c
 * @brief audren:u, audout:u and hwopus driven through sm: and svcSendSyncRequest
 * the way a guest does. The renderer tests send update data in the layout
 * libnx writes (revision 7), read the status back, count frames against the
 * 5 ms timer and check the mixed samples a circular sink writes to guest
 * memory. The client encoder follows libnx (ISC) sf/hipc.h and sf/cmif.h.
 */
#include "t.h"

#include "hos_audio.h"

#include <stdlib.h>
#include <time.h>

#define REV7 0x37564552u
#define AR_UNUSED_MIX_FOR_TEST 0x7FFFFFFFu

typedef struct msg {
    uint32_t cmd;
    uint8_t data[0x200];
    uint32_t data_size;
    uint64_t a_addr[2], a_size[2];
    uint32_t na;
    uint64_t b_addr[2], b_size[2];
    uint32_t nb;
    uint32_t copy[2];
    uint32_t ncopy;
    bool send_pid;
} msg;

typedef struct reply {
    uint32_t svc_rc, result;
    uint8_t data[0x40];
    uint32_t copy[4], move[4];
    uint32_t ncopy, nmove;
} reply;

static void w3(uint32_t *w, uint32_t *pos, uint64_t addr, uint64_t size)
{
    w[(*pos)++] = (uint32_t)size;
    w[(*pos)++] = (uint32_t)addr;
    w[(*pos)++] = (uint32_t)(((addr >> 36) & 0x3FFFFF) << 2) | (uint32_t)(((size >> 32) & 0xF) << 24) |
                  (uint32_t)(((addr >> 32) & 0xF) << 28);
}

static reply call(rl_ctx *c, uint32_t h, const msg *q)
{
    reply r;
    uint32_t w[RL_IPC_MSG / 4];
    uint32_t pos = 2;
    memset(&r, 0, sizeof r);
    memset(w, 0, sizeof w);
    if (q->ncopy || q->send_pid) {
        w[pos++] = (q->send_pid ? 1u : 0u) | (q->ncopy << 1);
        if (q->send_pid)
            pos += 2;
        for (uint32_t i = 0; i < q->ncopy; i++)
            w[pos++] = q->copy[i];
    }
    for (uint32_t i = 0; i < q->na; i++)
        w3(w, &pos, q->a_addr[i], q->a_size[i]);
    for (uint32_t i = 0; i < q->nb; i++)
        w3(w, &pos, q->b_addr[i], q->b_size[i]);
    uint32_t data_start = pos;
    uint8_t *b = (uint8_t *)w;
    uint32_t off = (pos * 4 + 15) & ~15u;
    uint32_t hdr[4] = { CMIF_IN_MAGIC, 0, q->cmd, 0 };
    memcpy(b + off, hdr, 16);
    memcpy(b + off + 16, q->data, q->data_size);
    off += 16 + q->data_size + 16;
    uint32_t ndata = (off - data_start * 4 + 3) / 4;
    w[0] = HIPC_REQUEST | (q->na << 20) | (q->nb << 24);
    w[1] = ndata | (q->ncopy || q->send_pid ? 1u << 31 : 0);
    uint32_t *tls = (uint32_t *)(void *)(t_mem() + c->tpidrro_el0);
    memcpy(tls, w, RL_IPC_MSG);
    r.svc_rc = t_svc(c, SVC_SEND_SYNC_REQUEST, h, 0, 0, 0, 0, 0);
    if (r.svc_rc)
        return r;
    pos = 2;
    if (tls[1] >> 31) {
        uint32_t sp = tls[pos++];
        r.ncopy = (sp >> 1) & 0xF;
        r.nmove = (sp >> 5) & 0xF;
        if (sp & 1)
            pos += 2;
        for (uint32_t i = 0; i < r.ncopy; i++)
            r.copy[i] = tls[pos++];
        for (uint32_t i = 0; i < r.nmove; i++)
            r.move[i] = tls[pos++];
    }
    const uint8_t *tb = (const uint8_t *)tls;
    off = (pos * 4 + 15) & ~15u;
    memcpy(hdr, tb + off, 16);
    r.result = hdr[0] == CMIF_OUT_MAGIC ? hdr[2] : 0xBADBAD;
    memcpy(r.data, tb + off + 16, sizeof r.data);
    return r;
}

static uint32_t rd32(const reply *r, uint32_t off) { return le32(r->data + off); }

static msg mk(uint32_t cmd);
static uint32_t open_service(rl_ctx *c, uint64_t scratch, const char *name)
{
    rl_ipc_reset_services();
    rl_hos_audio_register();
    memcpy(t_mem() + scratch, "sm:", 4);
    if (t_svc(c, SVC_CONNECT_TO_NAMED_PORT, 0, scratch, 0, 0, 0, 0) != 0)
        return 0;
    uint32_t sm = (uint32_t)c->x[1];
    uint64_t zero = 0;
    msg q = mk(0);
    memcpy(q.data, &zero, 8);
    q.data_size = 8;
    q.send_pid = true;
    if (call(c, sm, &q).result != 0)
        return 0;
    q.cmd = 1;
    uint64_t n = rl_ipc_name(name);
    memcpy(q.data, &n, 8);
    reply r = call(c, sm, &q);
    return r.svc_rc || r.result || r.nmove != 1 ? 0 : r.move[0];
}

static uint32_t wait_event(rl_ctx *c, uint64_t scratch, uint32_t h, uint64_t ns)
{
    *(uint32_t *)(void *)(t_mem() + scratch) = h;
    return t_svc(c, SVC_WAIT_SYNCHRONIZATION, 0, scratch, 1, ns, 0, 0);
}

static void sleep_ms(long ms)
{
    struct timespec ts = { 0, ms * 1000000L };
    nanosleep(&ts, NULL);
}

static msg mk(uint32_t cmd)
{
    msg q;
    memset(&q, 0, sizeof q);
    q.cmd = cmd;
    return q;
}

static void add_u32(msg *q, uint32_t v)
{
    memcpy(q->data + q->data_size, &v, 4);
    q->data_size += 4;
}

static void add_pad(msg *q, uint32_t n) { q->data_size += n; }

TEST(hwopus_sizes_and_errors)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = open_service(c, heap, "hwopus");
    CHECK(h);
    msg q = mk(1);
    add_u32(&q, 48000);
    add_u32(&q, 2);
    reply r = call(c, h, &q);
    CHECK_EQ(r.result, 0);
    CHECK(rd32(&r, 0) > 0x1000);
    uint32_t stereo = rd32(&r, 0);
    q = mk(1);
    add_u32(&q, 48000);
    add_u32(&q, 1);
    r = call(c, h, &q);
    CHECK(rd32(&r, 0) < stereo);
    q = mk(1);
    add_u32(&q, 44100);
    add_u32(&q, 2);
    r = call(c, h, &q);
    CHECK_EQ(r.result, CODEC_INVALID_SAMPLE_RATE);
    q = mk(1);
    add_u32(&q, 48000);
    add_u32(&q, 3);
    r = call(c, h, &q);
    CHECK_EQ(r.result, CODEC_INVALID_CHANNEL_COUNT);
    q = mk(0);
    add_u32(&q, 12000);
    add_u32(&q, 2);
    add_u32(&q, 0x10000);
    r = call(c, h, &q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
}

TEST(hwopus_decodes_a_packet)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = open_service(c, heap, "hwopus");
    CHECK(h);
    msg q = mk(0);
    add_u32(&q, 48000);
    add_u32(&q, 2);
    add_u32(&q, 0x10000);
    reply r = call(c, h, &q);
    CHECK_EQ(r.result, 0);
    uint32_t dec = r.move[0];
    uint8_t *in = t_mem() + heap + 0x1000;
    memset(in, 0, 16);
    in[3] = 1;
    in[8] = 0xF8;
    int16_t *pcm = (int16_t *)(void *)(t_mem() + heap + 0x2000);
    for (int i = 0; i < 960 * 2; i++)
        pcm[i] = 0x1234;
    q = mk(0);
    q.na = 1;
    q.a_addr[0] = heap + 0x1000;
    q.a_size[0] = 16;
    q.nb = 1;
    q.b_addr[0] = heap + 0x2000;
    q.b_size[0] = 960 * 2 * 2;
    r = call(c, dec, &q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(rd32(&r, 0), 9);
    CHECK_EQ(rd32(&r, 4), 960);
    int nonzero = 0;
    for (int i = 0; i < 960 * 2; i++)
        nonzero |= pcm[i] == 0x1234;
    CHECK(!nonzero);
    q.b_size[0] = 100;
    r = call(c, dec, &q);
    CHECK_EQ(r.result, CODEC_INVALID_LENGTH);
    in[3] = 200;
    q.b_size[0] = 960 * 2 * 2;
    r = call(c, dec, &q);
    CHECK_EQ(r.result, CODEC_INVALID_LENGTH);
}

TEST(audout_releases_buffers_as_they_play)
{
    rl_audio_set_backend(rl_audio_null_backend());
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint32_t h = open_service(c, heap, "audout:u");
    CHECK(h);
    msg q = mk(0);
    q.nb = 1;
    q.b_addr[0] = heap + 0x100;
    q.b_size[0] = 0x100;
    reply r = call(c, h, &q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(rd32(&r, 0), 1);
    CHECK(memcmp(t_mem() + heap + 0x100, "DeviceOut", 9) == 0);
    q = mk(1);
    add_u32(&q, 48000);
    add_u32(&q, 2);
    add_pad(&q, 8);
    q.na = 1;
    q.a_addr[0] = heap + 0x100;
    q.a_size[0] = 0x100;
    q.nb = 1;
    q.b_addr[0] = heap + 0x200;
    q.b_size[0] = 0x100;
    r = call(c, h, &q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(rd32(&r, 0), 48000);
    CHECK_EQ(rd32(&r, 4), 2);
    CHECK_EQ(r.nmove, 1);
    uint32_t out = r.move[0];
    r = call(c, out, &(msg){ .cmd = 4 });
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.ncopy, 1);
    uint32_t ev = r.copy[0];
    uint8_t *ab = t_mem() + heap + 0x1000;
    memset(ab, 0, 0x28);
    uint64_t sample = heap + 0x2000, cap = 4800 * 4, size = 4800 * 4;
    memcpy(ab + 8, &sample, 8);
    memcpy(ab + 0x10, &cap, 8);
    memcpy(ab + 0x18, &size, 8);
    q = mk(3);
    uint64_t tag = 0xABCD1234;
    memcpy(q.data, &tag, 8);
    q.data_size = 8;
    q.na = 1;
    q.a_addr[0] = heap + 0x1000;
    q.a_size[0] = 0x28;
    CHECK_EQ(call(c, out, &q).result, 0);
    q = mk(5);
    q.nb = 1;
    q.b_addr[0] = heap + 0x3000;
    q.b_size[0] = 8 * 4;
    memset(t_mem() + heap + 0x3000, 0xEE, 8 * 4);
    r = call(c, out, &q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(rd32(&r, 0), 0);
    CHECK_EQ(le64(t_mem() + heap + 0x3000), 0);
    CHECK_EQ(call(c, out, &(msg){ .cmd = 1 }).result, 0);
    uint64_t t0 = rl_clock_ns();
    CHECK_EQ(wait_event(c, heap + 0x800, ev, (uint64_t)-1), 0);
    uint64_t dt = rl_clock_ns() - t0;
    CHECK(dt >= 4800ull * 1000000000ull / 48000 - AUDIO_FRAME_NS);
    q = mk(5);
    q.nb = 1;
    q.b_addr[0] = heap + 0x3000;
    q.b_size[0] = 8 * 4;
    r = call(c, out, &q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(rd32(&r, 0), 1);
    uint64_t got;
    memcpy(&got, t_mem() + heap + 0x3000, 8);
    CHECK_EQ(got, tag);
    r = call(c, out, &(msg){ .cmd = 10 });
    CHECK_EQ(le64(r.data), 4800);
}

typedef struct upd {
    uint8_t in[0x2000];
    uint32_t size;
} upd;

static void put_f(uint8_t *p, float v) { memcpy(p, &v, 4); }

static uint32_t g_fmt = 2, g_end = 480;
static uint64_t g_wave_size = 960, g_ds;

static void build_update(upd *u, bool first, uint64_t wave, uint64_t sink_buf, uint32_t sink_size, bool sent)
{
    memset(u, 0, sizeof *u);
    uint8_t *p = u->in + 0x40;
    uint32_t sec_behavior = 0x10, sec_pools = 4 * 0x20, sec_res = 0x70, sec_voices = 0x170, sec_mixes = 0x20 + 0x930;
    uint32_t sec_sinks = 0x140, sec_perf = 0x10;
    put32(p, REV7);
    p += sec_behavior + sec_pools;
    put32(p, 0);
    put_f(p + 4, 1.0f);
    p += sec_res;
    p[8] = first;
    p[9] = 1;
    p[0xA] = 0;
    p[0xB] = (uint8_t)g_fmt;
    put32(p + 0xC, 48000);
    put32(p + 0x18, 1);
    put_f(p + 0x1C, 1.0f);
    put_f(p + 0x20, 1.0f);
    uint8_t *w0 = p + 0x3C;
    put32(w0 + 0x1C, 0);
    put64(w0 + 0xC, g_ds);
    put64(w0 + 0x14, g_ds ? 32 : 0);
    uint8_t *wb = w0 + 0x24;
    put64(wb, wave);
    put64(wb + 8, g_wave_size);
    put32(wb + 0x10, 0);
    put32(wb + 0x14, g_end);
    wb[0x1A] = sent;
    p += sec_voices;
    put32(p + 4, 1);
    p += 0x20;
    put_f(p, 1.0f);
    put32(p + 4, 48000);
    put32(p + 8, 2);
    p[0xC] = 1;
    put32(p + 0x10, 0);
    put32(p + 0x924, AR_UNUSED_MIX_FOR_TEST);
    p += 0x930;
    p[0] = 2;
    p[1] = 1;
    uint8_t *sd = p + 0x20;
    put64(sd, sink_buf);
    put32(sd + 8, sink_size);
    put32(sd + 0xC, 1);
    put32(sd + 0x10, 240);
    sd[0x1C] = 0;
    p += sec_sinks + sec_perf;
    uint32_t total = (uint32_t)(p - u->in);
    u->size = total;
    put32(u->in, REV7);
    put32(u->in + 4, sec_behavior);
    put32(u->in + 8, sec_pools);
    put32(u->in + 0xC, sec_voices);
    put32(u->in + 0x10, sec_res);
    put32(u->in + 0x14, 0);
    put32(u->in + 0x18, sec_mixes);
    put32(u->in + 0x1C, sec_sinks);
    put32(u->in + 0x20, sec_perf);
    put32(u->in + 0x3C, total);
}

static uint32_t open_renderer(rl_ctx *c, uint64_t heap, uint32_t h)
{
    uint32_t cfg[13] = { 48000, 240, 2, 0, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
    cfg[12] = 0;
    memcpy(&cfg[12], &(uint32_t){ REV7 }, 4);
    msg q = mk(1);
    memcpy(q.data, cfg, 0x34);
    q.data_size = 0x34;
    reply r = call(c, h, &q);
    if (r.result)
        return 0;
    uint64_t size = le64(r.data);
    q = mk(0);
    memcpy(q.data, cfg, 0x34);
    memcpy(q.data + 0x38, &size, 8);
    q.data_size = 0x48;
    r = call(c, h, &q);
    (void)heap;
    return r.result ? 0 : r.move[0];
}

TEST(audren_update_frames_and_mixing)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x400000);
    rl_cfg()->trace_os = false;
    uint32_t h = open_service(c, heap, "audren:u");
    CHECK(h);
    uint32_t rend = open_renderer(c, heap, h);
    CHECK(rend);
    reply r = call(c, rend, &(msg){ .cmd = 0 });
    CHECK_EQ(rd32(&r, 0), 48000);
    r = call(c, rend, &(msg){ .cmd = 1 });
    CHECK_EQ(rd32(&r, 0), 240);
    r = call(c, rend, &(msg){ .cmd = 2 });
    CHECK_EQ(rd32(&r, 0), 2);
    r = call(c, rend, &(msg){ .cmd = 7 });
    CHECK_EQ(r.result, 0);
    uint32_t ev = r.copy[0];
    CHECK_EQ(wait_event(c, heap + 0x800, ev, 50000000ull), KR_TIMED_OUT);

    int16_t *wave = (int16_t *)(void *)(t_mem() + heap + 0x10000);
    for (int i = 0; i < 480; i++)
        wave[i] = 1000;
    uint64_t sink = heap + 0x20000;
    uint32_t sink_size = 480 * 50;
    upd *u = calloc(1, sizeof *u);
    build_update(u, true, heap + 0x10000, sink, sink_size, true);
    memcpy(t_mem() + heap + 0x30000, u->in, u->size);
    msg q = mk(4);
    q.na = 1;
    q.a_addr[0] = heap + 0x30000;
    q.a_size[0] = u->size;
    q.nb = 1;
    q.b_addr[0] = heap + 0x40000;
    q.b_size[0] = 0x1000;
    r = call(c, rend, &q);
    CHECK_EQ(r.result, 0);
    const uint8_t *o = t_mem() + heap + 0x40000;
    CHECK_EQ(le32(o), REV7);
    CHECK_EQ(le32(o + 0xC), 0x10);
    CHECK_EQ(le32(o + 0x1C), 0x20);
    CHECK_EQ(le32(o + 4), 0xB0);
    uint64_t t_start = rl_clock_ns();
    CHECK_EQ(call(c, rend, &(msg){ .cmd = 5 }).result, 0);
    CHECK_EQ(wait_event(c, heap + 0x800, ev, (uint64_t)-1), 0);
    build_update(u, false, heap + 0x10000, sink, sink_size, false);
    memcpy(t_mem() + heap + 0x30000, u->in, u->size);
    uint64_t t_update;
    do {
        sleep_ms(50);
        r = call(c, rend, &q);
        t_update = rl_clock_ns();
    } while (r.result == 0 && le64(o + le32(o + 0x3C) - 0x10) < 30);
    CHECK_EQ(r.result, 0);
    uint32_t used = le32(o + 0x3C);
    uint32_t voices_at = 0x40 + 4 * 0x10;
    uint64_t played = le64(o + voices_at);
    uint32_t consumed = le32(o + voices_at + 8);
    uint32_t sinks_at = voices_at + 0x10;
    CHECK_EQ(played, 480);
    CHECK_EQ(consumed, 1);
    uint64_t elapsed = le64(o + used - 0x10);
    CHECK(elapsed >= 30);
    CHECK(elapsed <= (t_update - t_start) / AUDIO_FRAME_NS + 2);
    uint32_t wrote = le32(o + sinks_at);
    CHECK_EQ(wrote % 480, 0);
    CHECK(wrote < sink_size);
    const int16_t *s = (const int16_t *)(const void *)(t_mem() + sink);
    for (int i = 0; i < 480; i++)
        CHECK_EQ(s[i], 1000);
    for (int i = 480; i < 960; i++)
        CHECK_EQ(s[i], 0);
    CHECK_EQ(call(c, rend, &(msg){ .cmd = 6 }).result, 0);
    free(u);
}

TEST(audren_rejects_bad_updates)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x400000);
    uint32_t h = open_service(c, heap, "audren:u");
    uint32_t rend = open_renderer(c, heap, h);
    CHECK(rend);
    upd *u = calloc(1, sizeof *u);
    build_update(u, true, heap + 0x10000, heap + 0x20000, 4800, true);
    put32(u->in + 0xC, 0x170 * 2);
    memcpy(t_mem() + heap + 0x30000, u->in, u->size);
    msg q = mk(4);
    q.na = 1;
    q.a_addr[0] = heap + 0x30000;
    q.a_size[0] = u->size;
    q.nb = 1;
    q.b_addr[0] = heap + 0x40000;
    q.b_size[0] = 0x1000;
    CHECK_EQ(call(c, rend, &q).result, AUDIO_INVALID_UPDATE_INFO);
    build_update(u, true, heap + 0x10000, heap + 0x20000, 4800, true);
    put32(u->in, REV7 + 0x01000000);
    memcpy(t_mem() + heap + 0x30000, u->in, u->size);
    CHECK_EQ(call(c, rend, &q).result, AUDIO_INVALID_UPDATE_INFO);
    q.b_size[0] = 0x80;
    build_update(u, true, heap + 0x10000, heap + 0x20000, 4800, true);
    memcpy(t_mem() + heap + 0x30000, u->in, u->size);
    CHECK_EQ(call(c, rend, &q).result, AUDIO_INVALID_UPDATE_INFO);
    free(u);
}

TEST(audren_mixes_adpcm_voices)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x400000);
    uint32_t h = open_service(c, heap, "audren:u");
    uint32_t rend = open_renderer(c, heap, h);
    CHECK(rend);
    reply r = call(c, rend, &(msg){ .cmd = 7 });
    CHECK_EQ(r.result, 0);
    uint8_t *data = t_mem() + heap + 0x10000;
    memset(data, 0, 8);
    const uint8_t pairs[7] = { 0x12, 0x3F, 0x7E, 0x00, 0x57, 0x6A, 0x21 };
    memcpy(data + 1, pairs, 7);
    memset(t_mem() + heap + 0x11000, 0, 32);
    g_fmt = 6;
    g_end = 14;
    g_wave_size = 8;
    g_ds = heap + 0x11000;
    uint64_t sink = heap + 0x20000;
    upd *u = calloc(1, sizeof *u);
    build_update(u, true, heap + 0x10000, sink, 480 * 50, true);
    memcpy(t_mem() + heap + 0x30000, u->in, u->size);
    msg q = mk(4);
    q.na = 1;
    q.a_addr[0] = heap + 0x30000;
    q.a_size[0] = u->size;
    q.nb = 1;
    q.b_addr[0] = heap + 0x40000;
    q.b_size[0] = 0x1000;
    CHECK_EQ(call(c, rend, &q).result, 0);
    uint32_t ev = r.copy[0];
    CHECK_EQ(call(c, rend, &(msg){ .cmd = 5 }).result, 0);
    CHECK_EQ(wait_event(c, heap + 0x800, ev, (uint64_t)-1), 0);
    const int16_t want[14] = { 1, 2, 3, -1, 7, -2, 0, 0, 5, 7, 6, -6, 2, 1 };
    const int16_t *s = (const int16_t *)(const void *)(t_mem() + sink);
    while (memcmp(s, want, sizeof want) != 0)
        sleep_ms(5);
    CHECK_EQ(call(c, rend, &(msg){ .cmd = 6 }).result, 0);
    for (int i = 0; i < 14; i++)
        CHECK_EQ(s[i], want[i]);
    for (int i = 14; i < 240; i++)
        CHECK_EQ(s[i], 0);
    free(u);
}
