/**
 * @file audren.c
 * @brief audren:u: IAudioRendererManager, IAudioRenderer and IAudioDevice,
 * and RequestUpdate: parsing the update data the guest sends and writing the
 * status it reads back.
 *
 * Layouts, section order, revision gates and results follow Ryujinx (MIT)
 * Ryujinx.Audio/Renderer (StateUpdater, VoiceInfo, PoolMapper, BehaviourInfo)
 * and libnx (ISC) audren.h. Rendering is in audren_dsp.c.
 *
 * Input sections in order: header (0x40), behavior (0x10), memory pools
 * (0x20 each), voice channel resources (0x70), voices (0x170, or 0x188 from
 * REV15), effects, splitter, mixes (0x930, with a 0x20 prefix from REV7),
 * sinks (0x140) and the performance parameter (0x10). Output sections: header,
 * memory pools (0x10), voices (0x10), effects (0x10, or 0x90 from REV9),
 * sinks (0x20), performance (0x10), behavior errors (0xB0) and, from REV5,
 * renderer info (0x10).
 *
 * Deviations from the real service: effects and splitters are accepted and
 * their status is written, but they do not process audio; the work buffer
 * size is this runtime's own estimate; wave buffers are read from guest memory
 * directly instead of through the attached memory pools.
 */
#include "audren_priv.h"

#include <stdlib.h>
#include <string.h>

#define HDR_SIZE 0x40u
#define BEHAVIOR_SIZE 0x10u
#define POOL_IN 0x20u
#define POOL_OUT 0x10u
#define CHRES_IN 0x70u
#define VOICE_OUT 0x10u
#define MIX_IN 0x930u
#define MIX_DIRTY_PREFIX 0x20u
#define SINK_IN 0x140u
#define SINK_OUT 0x20u
#define PERF_IN 0x10u
#define PERF_OUT 0x10u
#define ERRORS_OUT 0xB0u
#define RENDER_INFO_OUT 0x10u
#define WAVE_IN 0x38u
#define REV_BASE 0x30564552u
#define REV_LAST 15

static int rev_number(int32_t revision)
{
    if (((uint32_t)revision & 0x00FFFFFFu) != (REV_BASE & 0x00FFFFFFu))
        return -1;
    int n = (int)(((uint32_t)revision - REV_BASE) >> 24);
    return n >= 0 && n <= REV_LAST ? n : -1;
}

/** @brief The renderer configuration (AudioRendererParameterInternal, 0x34 bytes). */
typedef struct ar_config {
    uint32_t rate, sample_count, mix_bufs, submix, voices, sinks, effects, perf_frames;
    uint32_t splitters, splitter_dests;
    int32_t revision;
} ar_config;

static bool read_config(const uint8_t *p, ar_config *c)
{
    c->rate = le32(p);
    c->sample_count = le32(p + 4);
    c->mix_bufs = le32(p + 8);
    c->submix = le32(p + 0xC);
    c->voices = le32(p + 0x10);
    c->sinks = le32(p + 0x14);
    c->effects = le32(p + 0x18);
    c->perf_frames = le32(p + 0x1C);
    c->splitters = le32(p + 0x24);
    c->splitter_dests = le32(p + 0x28);
    c->revision = (int32_t)le32(p + 0x30);
    return true;
}

static uint64_t work_buffer_size(const ar_config *c)
{
    uint64_t pools = c->effects + 4ull * c->voices;
    uint64_t size = 0x4000;
    size += 4ull * c->sample_count * (AR_VOICE_CH + c->mix_bufs) * 2;
    size += 4ull * 240 * (AR_VOICE_CH + c->mix_bufs) * (c->sinks + c->submix);
    size += (uint64_t)c->voices * 0x400;
    size += (uint64_t)(c->submix + 1) * 0x200;
    size += pools * 0x40;
    size += (uint64_t)c->effects * 0x400;
    size += (uint64_t)c->sinks * 0x100;
    size += (uint64_t)c->splitters * 0x40 + (uint64_t)c->splitter_dests * 0x200;
    if (c->perf_frames)
        size += (uint64_t)(c->perf_frames + 1) * 0x1000;
    return (size + 0xFFF) & ~0xFFFull;
}

static void audren_destroy(void *p)
{
    audren *ar = p;
    audio_tick_remove(ar->tick);
    rl_audio_close(ar->host);
    if (ar->event)
        hos_kobj_unref(&ar->event->obj);
    if (ar->mixes)
        for (uint32_t i = 0; i < ar->mix_count; i++)
            free(ar->mixes[i].buf);
    free(ar->voices);
    free(ar->res);
    free(ar->pools);
    free(ar->mixes);
    free(ar->sinks);
    free(ar->tmp);
    pthread_mutex_destroy(&ar->lock);
    free(ar);
}

static void frame_tick(void *ctx)
{
    audren *ar = ctx;
    pthread_mutex_lock(&ar->lock);
    bool run = ar->started;
    if (run)
        audren_render_frame(ar);
    pthread_mutex_unlock(&ar->lock);
    if (run)
        rl_hos_event_signal(ar->event);
}

static rl_result r_get_u32(rl_ipc_req *r, uint32_t v)
{
    put32(rl_ipc_out(r, 4), v);
    return KR_SUCCESS;
}

static rl_result r_sample_rate(rl_ipc_req *r) { return r_get_u32(r, ((audren *)r->self->state)->rate); }
static rl_result r_sample_count(rl_ipc_req *r) { return r_get_u32(r, ((audren *)r->self->state)->sample_count); }
static rl_result r_mix_buffer_count(rl_ipc_req *r) { return r_get_u32(r, ((audren *)r->self->state)->mix_buf_count); }

static rl_result r_get_state(rl_ipc_req *r)
{
    audren *ar = r->self->state;
    pthread_mutex_lock(&ar->lock);
    uint32_t s = ar->started ? 0 : 1;
    pthread_mutex_unlock(&ar->lock);
    return r_get_u32(r, s);
}

static rl_result r_start(rl_ipc_req *r)
{
    audren *ar = r->self->state;
    pthread_mutex_lock(&ar->lock);
    if (!ar->started) {
        ar->started = true;
        if (!ar->tick)
            ar->tick = audio_tick_add(frame_tick, ar);
        rl_audio_pause(ar->host, false);
    }
    pthread_mutex_unlock(&ar->lock);
    return KR_SUCCESS;
}

static rl_result r_stop(rl_ipc_req *r)
{
    audren *ar = r->self->state;
    pthread_mutex_lock(&ar->lock);
    if (ar->started) {
        ar->started = false;
        rl_audio_pause(ar->host, true);
    }
    pthread_mutex_unlock(&ar->lock);
    return KR_SUCCESS;
}

static rl_result r_query_event(rl_ipc_req *r)
{
    audren *ar = r->self->state;
    return rl_ipc_push_copy(r, &ar->event->obj);
}

static rl_result r_set_time_limit(rl_ipc_req *r)
{
    audren *ar = r->self->state;
    int32_t v;
    if (!rl_ipc_in(r, &v, 4))
        return SF_RESULT_INVALID_HEADER;
    ar->time_limit = v;
    return KR_SUCCESS;
}

static rl_result r_get_time_limit(rl_ipc_req *r) { return r_get_u32(r, (uint32_t)((audren *)r->self->state)->time_limit); }

static rl_result r_execute(rl_ipc_req *r)
{
    (void)r;
    return AUDIO_UNSUPPORTED_OPERATION;
}

static rl_result r_set_drop(rl_ipc_req *r)
{
    audren *ar = r->self->state;
    float v;
    if (!rl_ipc_in(r, &v, 4))
        return SF_RESULT_INVALID_HEADER;
    ar->voice_drop = v;
    return KR_SUCCESS;
}

static rl_result r_get_drop(rl_ipc_req *r)
{
    audren *ar = r->self->state;
    __builtin_memcpy(rl_ipc_out(r, 4), &ar->voice_drop, 4);
    return KR_SUCCESS;
}

/** @brief Cursor over the input data with bounds checks. */
typedef struct cursor {
    const uint8_t *p;
    uint32_t size, pos;
} cursor;

static const uint8_t *take(cursor *c, uint32_t n)
{
    if (n > c->size - c->pos)
        return NULL;
    const uint8_t *r = c->p + c->pos;
    c->pos += n;
    return r;
}

static rl_result update_pools(audren *ar, cursor *in, uint32_t sz, uint8_t **out)
{
    if ((uint64_t)ar->pool_count * POOL_IN != sz)
        return AUDIO_INVALID_UPDATE_INFO;
    const uint8_t *p = take(in, sz);
    if (!p)
        return AUDIO_INVALID_UPDATE_INFO;
    for (uint32_t i = 0; i < ar->pool_count; i++, p += POOL_IN, *out += POOL_OUT) {
        uint64_t addr = le64(p), size = le64(p + 8);
        uint32_t state = le32(p + 0x10);
        ar_pool *pool = &ar->pools[i];
        if (state != POOL_REQUEST_ATTACH && state != POOL_REQUEST_DETACH)
            continue;
        if (!addr || (addr & 0xFFF) || !size || (size & 0xFFF))
            return AUDIO_INVALID_UPDATE_INFO;
        if (state == POOL_REQUEST_ATTACH) {
            hos_lock();
            bool ok = hos_user_range(addr, size, HOS_PERM_R);
            hos_unlock();
            if (!ok) {
                memset(pool, 0, sizeof *pool);
                continue;
            }
            pool->addr = addr;
            pool->size = size;
            pool->attached = true;
            put32(*out, POOL_ATTACHED);
        } else {
            if (pool->addr != addr || pool->size != size)
                return AUDIO_INVALID_UPDATE_INFO;
            pool->attached = false;
            put32(*out, POOL_DETACHED);
        }
    }
    return KR_SUCCESS;
}

static void voice_reset(ar_voice *v)
{
    memset(v, 0, sizeof *v);
    v->play = VP_STOPPED;
    v->mix_id = AR_UNUSED_MIX;
    for (int i = 0; i < AR_WAVEBUFS; i++)
        v->wb[i].sent_to_dsp = true;
}

static bool sample_offsets_valid(uint32_t format, const ar_wave *w)
{
    if (w->start > w->end && w->size)
        return false;
    uint64_t need;
    switch (format) {
    case AR_FMT_PCM16:
        need = (uint64_t)w->end * 2;
        return need <= w->size || w->size == 0;
    case AR_FMT_PCMF32:
        need = (uint64_t)w->end * 4;
        return need <= w->size || w->size == 0;
    default: {
        uint32_t s = w->end;
        need = 8ull * (s / 14) + (s % 14 ? (s % 14) / 2 + 1 + (s % 2) : 0);
        return need <= w->size || w->size == 0;
    }
    }
}

static void flush_wave_buffers(ar_voice *v, uint32_t index, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        ar_wave *w = &v->wb[index];
        if (w->valid || !w->sent_to_dsp) {
            v->wb_index = (v->wb_index + 1) % AR_WAVEBUFS;
            v->wb_consumed++;
            w->valid = false;
        }
        w->sent_to_dsp = true;
        index = (index + 1) % AR_WAVEBUFS;
    }
}

static void update_wave(uint32_t format, ar_wave *w, const uint8_t *in)
{
    if (!w->valid && w->sent_to_dsp && w->addr) {
        w->addr = 0;
        w->size = 0;
    }
    if (!in[0x1A]) {
        w->await_clear = false;
        return;
    }
    if (w->await_clear || w->valid || !w->sent_to_dsp)
        return;
    ar_wave n = *w;
    n.addr = le64(in);
    n.size = le64(in + 8);
    n.start = le32(in + 0x10);
    n.end = le32(in + 0x14);
    n.loop = in[0x18] != 0;
    n.eos = in[0x19] != 0;
    n.loop_count = (int32_t)le32(in + 0x1C);
    n.ctx_addr = le64(in + 0x20);
    n.ctx_size = le64(in + 0x28);
    n.loop_first = le32(in + 0x30);
    n.loop_last = le32(in + 0x34);
    if (!sample_offsets_valid(format, &n))
        return;
    n.sent_to_dsp = false;
    n.valid = false;
    *w = n;
}

static rl_result update_voices(audren *ar, cursor *in, uint32_t sz, int rev, uint8_t **out)
{
    bool v2 = rev >= 15;
    uint32_t vs = v2 ? 0x188 : 0x170;
    uint32_t bq = v2 ? 0x30 : 0x18;
    uint32_t w0 = 0x24 + bq;
    if ((uint64_t)ar->voice_count * vs != sz)
        return AUDIO_INVALID_UPDATE_INFO;
    const uint8_t *p = take(in, sz);
    if (!p)
        return AUDIO_INVALID_UPDATE_INFO;
    for (uint32_t i = 0; i < ar->voice_count; i++)
        ar->voices[i].in_use = false;
    for (uint32_t i = 0; i < ar->voice_count; i++, p += vs, *out += VOICE_OUT) {
        ar_voice *v = &ar->voices[i];
        bool is_new = p[8] != 0, in_use = p[9] != 0;
        if (!in_use)
            continue;
        if (is_new)
            voice_reset(v);
        v->in_use = true;
        uint8_t user_play = p[0xA];
        uint8_t old = v->play;
        if (user_play == 0)
            v->play = VP_STARTED;
        else if (user_play == 1) {
            if (old != VP_STOPPED)
                v->play = VP_STOPPING;
        } else if (user_play == 2)
            v->play = VP_PAUSED;
        v->format = p[0xB];
        v->rate = le32(p + 0xC);
        v->nch = le32(p + 0x18);
        if (v->nch > AR_VOICE_CH)
            v->nch = AR_VOICE_CH;
        v->pitch = lef32(p + 0x1C);
        v->volume = lef32(p + 0x20);
        v->ds_addr = le64(p + w0 + 0xC);
        v->ds_size = le64(p + w0 + 0x14);
        v->mix_id = (int32_t)le32(p + w0 + 0x1C);
        for (int c = 0; c < AR_VOICE_CH; c++)
            v->res_id[c] = le32(p + w0 + 0x104 + 4 * c);
        uint32_t flush = rev >= 5 ? p[w0 + 0x104 + 0x19] : 0;
        if (flush)
            flush_wave_buffers(v, le32(p + w0 + 4) % AR_WAVEBUFS, flush > AR_WAVEBUFS ? AR_WAVEBUFS : flush);
        for (int b = 0; b < AR_WAVEBUFS; b++)
            update_wave(v->format, &v->wb[b], p + w0 + 0x24 + b * WAVE_IN);
        if (is_new || v->is_new) {
            v->is_new = true;
        } else {
            put64(*out, v->played);
            put32(*out + 8, v->wb_consumed);
        }
    }
    return KR_SUCCESS;
}

static rl_result update_mixes(audren *ar, cursor *in, uint32_t sz, int rev)
{
    uint32_t count = ar->mix_count;
    uint32_t prefix = 0;
    bool dirty_only = rev >= 7;
    const uint8_t *p;
    if (dirty_only) {
        const uint8_t *h = take(in, MIX_DIRTY_PREFIX);
        if (!h)
            return AUDIO_INVALID_UPDATE_INFO;
        count = le32(h + 4);
        prefix = MIX_DIRTY_PREFIX;
    }
    if ((uint64_t)count * MIX_IN + prefix != sz || count > ar->mix_count)
        return AUDIO_INVALID_UPDATE_INFO;
    p = take(in, count * MIX_IN);
    if (!p)
        return AUDIO_INVALID_UPDATE_INFO;
    uint32_t total_bufs = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *m = p + (uint64_t)i * MIX_IN;
        if (m[0xC]) {
            int32_t dest = (int32_t)le32(m + 0x924), id = (int32_t)le32(m + 0x10);
            if (dest != AR_UNUSED_MIX && (uint32_t)dest > ar->mix_count && id != 0)
                return AUDIO_INVALID_UPDATE_INFO;
            total_bufs += le32(m + 8);
        }
    }
    if (total_bufs > ar->mix_buf_count)
        return AUDIO_INVALID_UPDATE_INFO;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *m = p + (uint64_t)i * MIX_IN;
        uint32_t id = dirty_only ? le32(m + 0x10) : i;
        if (id >= ar->mix_count)
            return AUDIO_INVALID_UPDATE_INFO;
        ar_mix *mx = &ar->mixes[id];
        mx->used = m[0xC] != 0;
        if (!mx->used)
            continue;
        mx->volume = lef32(m);
        mx->rate = le32(m + 4);
        mx->buf_count = le32(m + 8);
        if (mx->buf_count > AR_MIXBUFS)
            mx->buf_count = AR_MIXBUFS;
        mx->dest_id = (int32_t)le32(m + 0x924);
        memcpy(mx->matrix, m + 0x24, sizeof mx->matrix);
    }
    return KR_SUCCESS;
}

static rl_result update_sinks(audren *ar, cursor *in, uint32_t sz, uint8_t **out)
{
    if ((uint64_t)ar->sink_count * SINK_IN != sz)
        return AUDIO_INVALID_UPDATE_INFO;
    const uint8_t *p = take(in, sz);
    if (!p)
        return AUDIO_INVALID_UPDATE_INFO;
    for (uint32_t i = 0; i < ar->sink_count; i++, p += SINK_IN, *out += SINK_OUT) {
        ar_sink *s = &ar->sinks[i];
        uint8_t type = p[0];
        s->used = p[1] != 0;
        if (type != s->type) {
            memset(s, 0, sizeof *s);
            s->type = type;
            s->used = p[1] != 0;
        }
        const uint8_t *d = p + 0x20;
        if (type == SINK_DEVICE) {
            s->input_count = le32(d + 0x100);
            if (s->input_count > AR_VOICE_CH)
                s->input_count = AR_VOICE_CH;
            memcpy(s->inputs, d + 0x104, AR_VOICE_CH);
        } else if (type == SINK_CIRCULAR) {
            s->addr = le64(d);
            s->size = le32(d + 8);
            s->input_count = le32(d + 0xC);
            if (s->input_count > AR_VOICE_CH)
                s->input_count = AR_VOICE_CH;
            memcpy(s->inputs, d + 0x1C, AR_VOICE_CH);
            if (s->last_write >= s->size)
                s->last_write = 0;
        }
        put32(*out, s->last_write);
    }
    return KR_SUCCESS;
}

static rl_result update(audren *ar, const uint8_t *inb, uint32_t in_size, uint8_t *outb, uint32_t out_size)
{
    cursor in = { inb, in_size, 0 };
    memset(outb, 0, out_size);
    const uint8_t *h = take(&in, HDR_SIZE);
    if (!h)
        return AUDIO_INVALID_UPDATE_INFO;
    uint32_t sec_behavior = le32(h + 4), sec_pools = le32(h + 8), sec_voices = le32(h + 0xC), sec_res = le32(h + 0x10);
    uint32_t sec_effects = le32(h + 0x14), sec_mixes = le32(h + 0x18), sec_sinks = le32(h + 0x1C), sec_perf = le32(h + 0x20);
    uint32_t total = le32(h + 0x3C);
    int rev = rev_number(ar->revision);
    uint64_t known = (uint64_t)HDR_SIZE + sec_behavior + sec_pools + sec_voices + sec_res + sec_effects + sec_mixes +
                     sec_sinks + sec_perf;
    if (total > in_size || known > total)
        return AUDIO_INVALID_UPDATE_INFO;
    uint32_t sec_splitter = total - (uint32_t)known;

    const uint8_t *b = take(&in, sec_behavior);
    if (!b || sec_behavior != BEHAVIOR_SIZE || (int32_t)le32(b) != ar->revision || (int32_t)le32(h) != ar->revision)
        return AUDIO_INVALID_UPDATE_INFO;

    uint32_t effect_out = rev >= 9 ? 0x90 : 0x10;
    uint32_t need_out = HDR_SIZE + ar->pool_count * POOL_OUT + ar->voice_count * VOICE_OUT +
                        ar->effect_count * effect_out + ar->sink_count * SINK_OUT + PERF_OUT + ERRORS_OUT +
                        (rev >= 5 ? RENDER_INFO_OUT : 0);
    if (out_size < need_out)
        return AUDIO_INVALID_UPDATE_INFO;

    uint8_t *o = outb + HDR_SIZE;
    uint8_t *pools_out = o;
    rl_result rc = update_pools(ar, &in, sec_pools, &o);
    if (rc != KR_SUCCESS)
        return rc;
    o = pools_out + ar->pool_count * POOL_OUT;

    if ((uint64_t)ar->voice_count * CHRES_IN != sec_res)
        return AUDIO_INVALID_UPDATE_INFO;
    const uint8_t *rp = take(&in, sec_res);
    if (!rp)
        return AUDIO_INVALID_UPDATE_INFO;
    for (uint32_t i = 0; i < ar->voice_count; i++, rp += CHRES_IN) {
        memcpy(ar->res[le32(rp) < ar->voice_count ? le32(rp) : i].mix, rp + 4, sizeof ar->res[0].mix);
        ar->res[le32(rp) < ar->voice_count ? le32(rp) : i].used = rp[0x64] != 0;
    }

    uint8_t *voices_out = o;
    rc = update_voices(ar, &in, sec_voices, rev, &o);
    if (rc != KR_SUCCESS)
        return rc;
    o = voices_out + ar->voice_count * VOICE_OUT;

    if (take(&in, sec_effects) == NULL)
        return AUDIO_INVALID_UPDATE_INFO;
    uint8_t *effects_out = o;
    if (ar->effect_count && sec_effects) {
        uint32_t es = sec_effects / ar->effect_count;
        const uint8_t *e = in.p + in.pos - sec_effects;
        for (uint32_t i = 0; i < ar->effect_count; i++) {
            uint8_t type = e[(uint64_t)i * es];
            bool enabled = e[(uint64_t)i * es + 2] != 0;
            if (type)
                effects_out[(uint64_t)i * effect_out] = enabled && ar->started ? 3 : 4;
        }
    }
    o = effects_out + ar->effect_count * effect_out;

    if (take(&in, sec_splitter) == NULL)
        return AUDIO_INVALID_UPDATE_INFO;
    rc = update_mixes(ar, &in, sec_mixes, rev);
    if (rc != KR_SUCCESS)
        return rc;

    uint8_t *sinks_out = o;
    rc = update_sinks(ar, &in, sec_sinks, &o);
    if (rc != KR_SUCCESS)
        return rc;
    o = sinks_out + ar->sink_count * SINK_OUT;

    if (sec_perf != PERF_IN || !take(&in, PERF_IN))
        return AUDIO_INVALID_UPDATE_INFO;
    o += PERF_OUT;
    o += ERRORS_OUT;

    put32(outb, (uint32_t)ar->revision);
    put32(outb + 4, ERRORS_OUT);
    put32(outb + 8, ar->pool_count * POOL_OUT);
    put32(outb + 0xC, ar->voice_count * VOICE_OUT);
    put32(outb + 0x14, ar->effect_count * effect_out);
    put32(outb + 0x1C, ar->sink_count * SINK_OUT);
    put32(outb + 0x20, PERF_OUT);
    uint32_t used = HDR_SIZE + ar->pool_count * POOL_OUT + ar->voice_count * VOICE_OUT + ar->effect_count * effect_out +
                    ar->sink_count * SINK_OUT + PERF_OUT + ERRORS_OUT;
    if (rev >= 5) {
        put64(o, ar->elapsed_frames);
        put32(outb + 0x28, RENDER_INFO_OUT);
        used += RENDER_INFO_OUT;
    }
    put32(outb + 0x3C, used);
    if (in.pos != total)
        return AUDIO_INVALID_UPDATE_INFO;
    return KR_SUCCESS;
}

static rl_result r_request_update(rl_ipc_req *r)
{
    audren *ar = r->self->state;
    rl_ipc_buf out = rl_ipc_auto_out(r, 0), inb = rl_ipc_auto_in(r, 0);
    if (!inb.size || !out.size)
        return AUDIO_INVALID_UPDATE_INFO;
    uint8_t *in = malloc(inb.size), *o = calloc(1, out.size);
    if (!in || !o) {
        free(in);
        free(o);
        return KR_OUT_OF_MEMORY;
    }
    rl_result rc;
    if (!rl_ipc_buf_read(&inb, in, inb.size)) {
        rc = KR_INVALID_MEM_STATE;
    } else {
        pthread_mutex_lock(&ar->lock);
        rc = update(ar, in, (uint32_t)inb.size, o, (uint32_t)out.size);
        pthread_mutex_unlock(&ar->lock);
        if (rc == KR_SUCCESS) {
            rl_hos_event_clear(ar->event);
            if (!rl_ipc_buf_write(&out, o, out.size))
                rc = KR_INVALID_MEM_STATE;
        }
    }
    free(in);
    free(o);
    return rc;
}

static const rl_ipc_cmd renderer_cmds[] = {
    { 0, "GetSampleRate", r_sample_rate },
    { 1, "GetSampleCount", r_sample_count },
    { 2, "GetMixBufferCount", r_mix_buffer_count },
    { 3, "GetState", r_get_state },
    { 4, "RequestUpdate", r_request_update },
    { 5, "Start", r_start },
    { 6, "Stop", r_stop },
    { 7, "QuerySystemEvent", r_query_event },
    { 8, "SetRenderingTimeLimit", r_set_time_limit },
    { 9, "GetRenderingTimeLimit", r_get_time_limit },
    { 10, "RequestUpdateAuto", r_request_update },
    { 11, "ExecuteAudioRendererRendering", r_execute },
    { 12, "SetVoiceDropParameter", r_set_drop },
    { 13, "GetVoiceDropParameter", r_get_drop },
};

static const rl_ipc_class renderer_class = {
    .name = "nn::audio::detail::IAudioRenderer",
    .cmds = renderer_cmds,
    .ncmds = sizeof renderer_cmds / sizeof renderer_cmds[0],
    .pointer_buffer_size = 0,
    .destroy = audren_destroy,
};

static audren *renderer_alloc(const ar_config *c)
{
    audren *ar = calloc(1, sizeof *ar);
    if (!ar)
        return NULL;
    pthread_mutex_init(&ar->lock, NULL);
    ar->revision = c->revision;
    ar->rate = c->rate;
    ar->sample_count = c->sample_count;
    ar->mix_buf_count = c->mix_bufs;
    ar->submix_count = c->submix;
    ar->voice_count = c->voices;
    ar->sink_count = c->sinks;
    ar->effect_count = c->effects;
    ar->perf_frames = c->perf_frames;
    ar->time_limit = 100;
    ar->voice_drop = 1.0f;
    ar->pool_count = c->effects + AR_WAVEBUFS * c->voices;
    ar->mix_count = c->submix + 1;
    ar->voices = calloc(c->voices, sizeof *ar->voices);
    ar->res = calloc(c->voices, sizeof *ar->res);
    ar->pools = calloc(ar->pool_count, sizeof *ar->pools);
    ar->mixes = calloc(ar->mix_count, sizeof *ar->mixes);
    ar->sinks = calloc(c->sinks, sizeof *ar->sinks);
    ar->tmp = calloc((size_t)c->sample_count * (AR_VOICE_CH + 2), sizeof *ar->tmp);
    ar->event = rl_hos_event_create();
    bool ok = ar->voices && ar->res && ar->pools && ar->mixes && ar->sinks && ar->tmp && ar->event;
    for (uint32_t i = 0; ok && i < ar->mix_count; i++) {
        ar->mixes[i].buf = calloc((size_t)AR_MIXBUFS * c->sample_count, sizeof(float));
        ok = ar->mixes[i].buf != NULL;
    }
    if (ok) {
        for (uint32_t i = 0; i < c->voices; i++)
            voice_reset(&ar->voices[i]);
        ar->host = rl_audio_open(c->rate, 2);
        ok = ar->host != NULL;
    }
    if (!ok) {
        audren_destroy(ar);
        return NULL;
    }
    rl_audio_pause(ar->host, true);
    return ar;
}

static rl_result m_open(rl_ipc_req *r)
{
    uint8_t in[0x48];
    ar_config c;
    if (!rl_ipc_in(r, in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    read_config(in, &c);
    uint64_t wb_size = le64(in + 0x38);
    if (rev_number(c.revision) < 0)
        return AUDIO_OPERATION_FAILED;
    if ((c.rate != 32000 && c.rate != 48000) || c.sample_count == 0 || c.sample_count > 960 || c.mix_bufs == 0 ||
        c.mix_bufs > 256 || c.voices > 1024 || c.sinks > 256 || c.effects > 1024 || c.submix > 256)
        return AUDIO_OPERATION_FAILED;
    if (work_buffer_size(&c) > wb_size)
        return AUDIO_WORK_BUFFER_TOO_SMALL;
    rl_log(RL_LOG_INFO, "audren", "open: %u Hz, %u samples per frame, %u voices, %u mix buffers, %u submixes, %u sinks, %u effects, revision %d",
           c.rate, c.sample_count, c.voices, c.mix_bufs, c.submix, c.sinks, c.effects, rev_number(c.revision));
    audren *ar = renderer_alloc(&c);
    if (!ar)
        return AUDIO_OPERATION_FAILED;
    rl_ipc_object *o = rl_ipc_object_new(&renderer_class, ar);
    if (!o) {
        audren_destroy(ar);
        return KR_OUT_OF_MEMORY;
    }
    return rl_ipc_push_object(r, o);
}

static rl_result m_work_size(rl_ipc_req *r)
{
    uint8_t in[0x34];
    ar_config c;
    if (!rl_ipc_in(r, in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    read_config(in, &c);
    if (rev_number(c.revision) < 0)
        return RL_MAKE_RESULT(153, 2);
    put64(rl_ipc_out(r, 8), work_buffer_size(&c));
    return KR_SUCCESS;
}

#define DEVICE_NAME_SIZE 0x100
#define DEVICE_NAME "AudioStereoJackOutput"

typedef struct audio_device {
    float volume;
    hos_revent *events[3];
} audio_device;

static void device_destroy(void *p)
{
    audio_device *d = p;
    for (int i = 0; i < 3; i++)
        if (d->events[i])
            hos_kobj_unref(&d->events[i]->obj);
    free(d);
}

static rl_result dev_list(rl_ipc_req *r)
{
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    uint8_t name[DEVICE_NAME_SIZE] = DEVICE_NAME;
    uint32_t n = 0;
    if (out.size >= DEVICE_NAME_SIZE && rl_ipc_buf_write(&out, name, sizeof name))
        n = 1;
    put32(rl_ipc_out(r, 4), n);
    return KR_SUCCESS;
}

static rl_result dev_set_volume(rl_ipc_req *r)
{
    audio_device *d = r->self->state;
    float v;
    if (!rl_ipc_in(r, &v, 4))
        return SF_RESULT_INVALID_HEADER;
    d->volume = v;
    return KR_SUCCESS;
}

static rl_result dev_get_volume(rl_ipc_req *r)
{
    audio_device *d = r->self->state;
    __builtin_memcpy(rl_ipc_out(r, 4), &d->volume, 4);
    return KR_SUCCESS;
}

static rl_result dev_active_name(rl_ipc_req *r)
{
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    uint8_t name[DEVICE_NAME_SIZE] = DEVICE_NAME;
    if (out.size >= DEVICE_NAME_SIZE)
        rl_ipc_buf_write(&out, name, sizeof name);
    return KR_SUCCESS;
}

static rl_result dev_event(rl_ipc_req *r, int i)
{
    audio_device *d = r->self->state;
    if (!d->events[i])
        d->events[i] = rl_hos_event_create();
    return d->events[i] ? rl_ipc_push_copy(r, &d->events[i]->obj) : KR_OUT_OF_RESOURCE;
}

static rl_result dev_system_event(rl_ipc_req *r) { return dev_event(r, 0); }
static rl_result dev_input_event(rl_ipc_req *r) { return dev_event(r, 1); }
static rl_result dev_output_event(rl_ipc_req *r) { return dev_event(r, 2); }

static rl_result dev_channel_count(rl_ipc_req *r)
{
    put32(rl_ipc_out(r, 4), 2);
    return KR_SUCCESS;
}

static const rl_ipc_cmd device_cmds[] = {
    { 0, "ListAudioDeviceName", dev_list },
    { 1, "SetAudioDeviceOutputVolume", dev_set_volume },
    { 2, "GetAudioDeviceOutputVolume", dev_get_volume },
    { 3, "GetActiveAudioDeviceName", dev_active_name },
    { 4, "QueryAudioDeviceSystemEvent", dev_system_event },
    { 5, "GetActiveChannelCount", dev_channel_count },
    { 6, "ListAudioDeviceNameAuto", dev_list },
    { 7, "SetAudioDeviceOutputVolumeAuto", dev_set_volume },
    { 8, "GetAudioDeviceOutputVolumeAuto", dev_get_volume },
    { 10, "GetActiveAudioDeviceNameAuto", dev_active_name },
    { 11, "QueryAudioDeviceInputEvent", dev_input_event },
    { 12, "QueryAudioDeviceOutputEvent", dev_output_event },
    { 13, "GetActiveAudioOutputDeviceName", dev_active_name },
    { 14, "ListAudioOutputDeviceName", dev_list },
};

static const rl_ipc_class device_class = {
    .name = "nn::audio::detail::IAudioDevice",
    .cmds = device_cmds,
    .ncmds = sizeof device_cmds / sizeof device_cmds[0],
    .pointer_buffer_size = 0,
    .destroy = device_destroy,
};

static rl_result m_device(rl_ipc_req *r)
{
    audio_device *d = calloc(1, sizeof *d);
    rl_ipc_object *o = d ? rl_ipc_object_new(&device_class, d) : NULL;
    if (!o) {
        free(d);
        return KR_OUT_OF_MEMORY;
    }
    d->volume = 1.0f;
    return rl_ipc_push_object(r, o);
}

static const rl_ipc_cmd manager_cmds[] = {
    { 0, "OpenAudioRenderer", m_open },
    { 1, "GetWorkBufferSize", m_work_size },
    { 2, "GetAudioDeviceService", m_device },
    { 4, "GetAudioDeviceServiceWithRevisionInfo", m_device },
};

static const rl_ipc_class manager_class = {
    .name = "nn::audio::detail::IAudioRendererManager",
    .cmds = manager_cmds,
    .ncmds = sizeof manager_cmds / sizeof manager_cmds[0],
    .pointer_buffer_size = RL_AUDIO_POINTER_BUFFER_SIZE,
    .destroy = NULL,
};

rl_ipc_object *audren_manager_new(void) { return rl_ipc_object_new(&manager_class, NULL); }
