/**
 * @file audren_dsp.c
 * @brief One rendered frame of the audio renderer: voices (16-bit PCM, float
 * PCM and ADPCM) decoded and resampled, mixed through the mixes, written to
 * the device and circular buffer sinks.
 *
 * Voice playback follows Ryujinx (MIT) DataSourceHelper.ProcessWaveBuffers,
 * VoiceState and AdpcmHelper; mixing follows MixCommand, VolumeCommand and
 * DeviceSinkCommand. Samples are floats in 16-bit units. Resampling is
 * linear (Ryujinx has several qualities), filters and effects are not
 * applied.
 */
#include "audren_priv.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/** @brief Longest source run of one frame: pitch is limited to 4x. */
/** @brief Queue depth (250 ms at 48 kHz) above which a rendered frame is dropped; a late timer tick can leave a backlog that the output drains within a few ticks, and dropping sooner loses sound. */
#define AR_QUEUE_MAX_FRAMES 12000u

#define MAX_PITCH 4.0f

static int16_t clamp16(float v)
{
    if (v > 32767.0f)
        return 32767;
    if (v < -32768.0f)
        return -32768;
    return (int16_t)v;
}

static int16_t sat16(int v) { return v > 32767 ? 32767 : (v < -32768 ? -32768 : (int16_t)v); }

static uint32_t nibbles_for(uint32_t samples)
{
    return samples % 14 + 16 * (samples / 14) + (samples % 14 ? 2 : 0);
}

/**
 * @brief Decodes count ADPCM samples. in starts at the byte holding the
 * nibble of sample (start + offset); the decoder state is in v.
 */
static int adpcm_decode(int16_t *out, const uint8_t *in, uint32_t in_len, uint32_t start, uint32_t end, uint32_t offset,
                        int count, const int16_t *coef, ar_voice *v)
{
    if (end < start)
        return 0;
    uint8_t pred_scale = (uint8_t)v->adpcm_pred;
    uint8_t scale = pred_scale & 0xF;
    uint8_t ci = (pred_scale >> 4) & 0xF;
    int h0 = v->adpcm_h0, h1 = v->adpcm_h1;
    int c0 = coef[ci * 2 & 15], c1 = coef[(ci * 2 + 1) & 15];
    int decoded = (int)(end - start - offset) < count ? (int)(end - start - offset) : count;
    uint32_t nib = nibbles_for(offset + start);
    int remaining = decoded, oi = 0;
    uint32_t ii = 0;
    while (remaining > 0) {
        int n;
        if (nib % 16 == 0) {
            if (ii >= in_len)
                break;
            pred_scale = in[ii++];
            scale = pred_scale & 0xF;
            ci = (pred_scale >> 4) & 0xF;
            c0 = coef[ci * 2 & 15];
            c1 = coef[(ci * 2 + 1) & 15];
            nib += 2;
            n = remaining < 14 ? remaining : 14;
        } else {
            n = 1;
        }
        int scale_fixed = 2048 << scale;
        if (n < 14) {
            for (int i = 0; i < n; i++) {
                if (ii >= in_len)
                    goto done;
                int value = in[ii], sample;
                if (nib & 1) {
                    sample = (int)((uint32_t)value << 28) >> 28;
                    ii++;
                } else {
                    sample = (int)((uint32_t)value << 24) >> 28;
                }
                nib++;
                int pred = c0 * h0 + c1 * h1;
                int s = sat16((int)(((int64_t)sample * scale_fixed + pred + 1024) >> 11));
                h1 = h0;
                h0 = s;
                out[oi++] = (int16_t)s;
                remaining--;
            }
        } else {
            for (int i = 0; i < 7; i++) {
                if (ii >= in_len)
                    goto done;
                int value = in[ii++];
                int s0 = (int)((uint32_t)value << 24) >> 28, s1 = (int)((uint32_t)value << 28) >> 28;
                int p0 = c0 * h0 + c1 * h1;
                int a = sat16((int)(((int64_t)s0 * scale_fixed + p0 + 1024) >> 11));
                int p1 = c0 * a + c1 * h0;
                int b = sat16((int)(((int64_t)s1 * scale_fixed + p1 + 1024) >> 11));
                h1 = a;
                h0 = b;
                out[oi++] = (int16_t)a;
                out[oi++] = (int16_t)b;
            }
            nib += 14;
            remaining -= 14;
        }
    }
done:
    v->adpcm_pred = pred_scale;
    v->adpcm_h0 = (int16_t)h0;
    v->adpcm_h1 = (int16_t)h1;
    return oi;
}

static void end_of_buffer(ar_voice *v, ar_wave *w)
{
    w->valid = false;
    v->wb_index = (v->wb_index + 1) % AR_WAVEBUFS;
    v->loop_cnt = 0;
    v->wb_consumed++;
    if (w->eos)
        v->played = 0;
}

/** @brief Decodes up to count source frames of the current wave buffer into src; -1 on a bad buffer. */
static int decode_run(ar_voice *v, const ar_wave *w, uint32_t start, uint32_t end, int count, float *src, uint32_t stride,
                      const int16_t *coef)
{
    uint32_t nch = v->nch ? v->nch : 1;
    if (end < start)
        return 0;
    int n = (int)(end - start - v->offset) < count ? (int)(end - start - v->offset) : count;
    if (n <= 0 || !w->addr || !w->size)
        return 0;
    if (v->format == AR_FMT_ADPCM) {
        uint32_t nib = nibbles_for(v->offset + start);
        uint64_t byte0 = nib / 2;
        if (byte0 >= w->size)
            return -1;
        uint64_t len = (uint64_t)(n / 14 + 2) * 8;
        if (byte0 + len > w->size)
            len = w->size - byte0;
        uint8_t *in = malloc(len + 1);
        int16_t *tmp = calloc((size_t)n + 16, sizeof *tmp);
        int got = -1;
        if (in && tmp && audio_guest_read(w->addr + byte0, in, len)) {
            got = adpcm_decode(tmp, in, (uint32_t)len, start, end, v->offset, n, coef, v);
            for (int i = 0; i < got; i++)
                src[i] = tmp[i];
        }
        free(in);
        free(tmp);
        return got;
    }
    uint32_t bytes = v->format == AR_FMT_PCMF32 ? 4 : 2;
    uint64_t first = (uint64_t)(start + v->offset) * nch * bytes;
    if (first >= w->size)
        return -1;
    uint64_t avail = (w->size - first) / (nch * bytes);
    if ((uint64_t)n > avail)
        n = (int)avail;
    if (n <= 0)
        return -1;
    uint8_t *raw = malloc((size_t)n * nch * bytes);
    if (!raw || !audio_guest_read(w->addr + first, raw, (uint64_t)n * nch * bytes)) {
        free(raw);
        return -1;
    }
    for (int i = 0; i < n; i++)
        for (uint32_t c = 0; c < nch; c++) {
            const uint8_t *p = raw + ((size_t)i * nch + c) * bytes;
            float s;
            if (bytes == 2) {
                int16_t t;
                memcpy(&t, p, 2);
                s = t;
            } else {
                float t;
                memcpy(&t, p, 4);
                s = t * 32768.0f;
            }
            src[(size_t)c * stride + i] = s;
        }
    free(raw);
    return n;
}

/** @brief Fills src (nch rows of stride floats) with need source frames, advancing the voice. */
static void fetch_source(ar_voice *v, int need, float *src, uint32_t stride)
{
    uint32_t nch = v->format == AR_FMT_ADPCM ? 1 : (v->nch ? v->nch : 1);
    int16_t coef[16] = { 0 };
    if (v->format == AR_FMT_ADPCM && v->ds_addr && v->ds_size >= 32)
        audio_guest_read(v->ds_addr, coef, 32);
    int y = 0;
    while (y < need) {
        if (v->wb_index >= AR_WAVEBUFS) {
            v->wb_index = 0;
            v->played = 0;
        }
        ar_wave *w = &v->wb[v->wb_index];
        if (!w->valid)
            break;
        if (v->offset == 0 && v->format == AR_FMT_ADPCM && w->ctx_addr) {
            uint8_t ctx[6];
            if (audio_guest_read(w->ctx_addr, ctx, 6)) {
                v->adpcm_pred = (uint16_t)(ctx[0] | ctx[1] << 8);
                v->adpcm_h0 = (int16_t)(ctx[2] | ctx[3] << 8);
                v->adpcm_h1 = (int16_t)(ctx[4] | ctx[5] << 8);
            }
        }
        uint32_t start = w->start, end = w->end;
        if (v->loop_cnt > 0 && w->loop_first != 0 && w->loop_last != 0 && w->loop_first <= w->loop_last) {
            start = w->loop_first;
            end = w->loop_last;
        }
        uint32_t total = end >= start ? end - start : 0;
        int n = decode_run(v, w, start, end, need - y, src + y, stride, coef);
        (void)nch;
        if (n < 0) {
            end_of_buffer(v, w);
            n = 0;
        }
        y += n;
        v->offset += (uint32_t)n;
        v->played += (uint32_t)n;
        if (v->offset >= total || n == 0) {
            v->offset = 0;
            if (w->loop) {
                v->loop_cnt++;
                if (w->loop_count >= 0 && (n == 0 || v->loop_cnt > (uint32_t)w->loop_count))
                    end_of_buffer(v, w);
                if (n == 0)
                    break;
            } else {
                end_of_buffer(v, w);
            }
        }
    }
}

/**
 * @brief Drops count buffers from the program's first queued one: each is marked sent and invalid, and counts as
 * consumed only when it is the one playing (the playing buffer then moves on), so no buffer is counted twice.
 */
static void flush_wave_buffers(ar_voice *v)
{
    uint32_t index = v->wb_start;
    for (uint32_t i = 0; i < v->flush_pending; i++) {
        v->wb[index].sent_to_dsp = true;
        if (v->wb_index == index) {
            v->wb_index = (v->wb_index + 1) % AR_WAVEBUFS;
            v->wb_consumed++;
        }
        v->wb[index].valid = false;
        index = (index + 1) % AR_WAVEBUFS;
    }
    v->flush_pending = 0;
}

static void voice_prepare(ar_voice *v, bool *active)
{
    *active = false;
    if (!v->in_use)
        return;
    if (v->flush_pending)
        flush_wave_buffers(v);
    switch (v->play) {
    case VP_STARTED:
        for (int i = 0; i < AR_WAVEBUFS; i++)
            if (!v->wb[i].sent_to_dsp) {
                v->wb[i].valid = true;
                v->wb[i].sent_to_dsp = true;
            }
        for (int i = 0; i < AR_WAVEBUFS; i++)
            if (v->wb[i].valid)
                *active = true;
        break;
    case VP_STOPPING:
        for (int i = 0; i < AR_WAVEBUFS; i++) {
            v->wb[i].sent_to_dsp = true;
            if (v->wb[i].valid) {
                v->wb_index = (v->wb_index + 1) % AR_WAVEBUFS;
                v->wb_consumed++;
            }
            v->wb[i].valid = false;
        }
        v->offset = 0;
        v->played = 0;
        v->frac = 0;
        v->loop_cnt = 0;
        v->adpcm_pred = 0;
        v->adpcm_h0 = v->adpcm_h1 = 0;
        memset(v->prev, 0, sizeof v->prev);
        v->play = VP_STOPPED;
        break;
    default:
        break;
    }
}

static void voice_render(audren *ar, ar_voice *v)
{
    uint32_t n = ar->sample_count;
    uint32_t nch = v->format == AR_FMT_ADPCM ? 1 : (v->nch ? v->nch : 1);
    float ratio = (float)v->rate / (float)ar->rate * v->pitch;
    if (!(ratio > 0.0f) || ratio > MAX_PITCH || v->rate == 0)
        return;
    int need = (int)(v->frac + ratio * (float)n);
    uint32_t stride = (uint32_t)need + 1;
    float *src = calloc((size_t)nch * stride, sizeof *src);
    if (!src)
        return;
    fetch_source(v, need, src, stride);
    ar_mix *mix = NULL;
    if (v->mix_id >= 0 && (uint32_t)v->mix_id < ar->mix_count && ar->mixes[v->mix_id].used)
        mix = &ar->mixes[v->mix_id];
    bool exact = ratio == 1.0f && v->frac == 0.0f;
    for (uint32_t c = 0; c < nch; c++) {
        float *out = ar->tmp + (size_t)c * n;
        const float *x = src + (size_t)c * stride;
        if (exact) {
            memcpy(out, x, n * sizeof *out);
        } else {
            for (uint32_t j = 0; j < n; j++) {
                float pos = v->frac + (float)j * ratio;
                int i = (int)pos;
                float f = pos - (float)i;
                float a = i == 0 ? v->prev[c] : x[i - 1];
                float b = need > 0 ? x[i < need ? i : need - 1] : a;
                out[j] = a + (b - a) * f;
            }
        }
        if (need > 0)
            v->prev[c] = x[need - 1];
    }
    v->frac = v->frac + ratio * (float)n - (float)need;
    if (v->frac < 0.0f || v->frac >= 1.0f)
        v->frac = 0.0f;
    if (mix) {
        for (uint32_t c = 0; c < nch; c++) {
            if (v->res_id[c] >= ar->voice_count)
                continue;
            const ar_chres *rc = &ar->res[v->res_id[c]];
            const float *out = ar->tmp + (size_t)c * n;
            for (uint32_t b = 0; b < mix->buf_count; b++) {
                float vol = rc->mix[b] * v->volume;
                if (vol == 0.0f)
                    continue;
                float *dst = mix->buf + (size_t)b * n;
                for (uint32_t j = 0; j < n; j++)
                    dst[j] += out[j] * vol;
            }
        }
    }
    free(src);
}

static int mix_depth(const audren *ar, uint32_t id)
{
    int d = 0;
    int32_t cur = (int32_t)id;
    while (cur != 0 && d < 32) {
        int32_t dest = ar->mixes[cur].dest_id;
        if (dest < 0 || (uint32_t)dest >= ar->mix_count || !ar->mixes[dest].used)
            break;
        cur = dest;
        d++;
    }
    return d;
}

static void route_mixes(audren *ar)
{
    uint32_t n = ar->sample_count;
    for (int depth = 32; depth >= 1; depth--) {
        for (uint32_t id = 1; id < ar->mix_count; id++) {
            ar_mix *m = &ar->mixes[id];
            if (!m->used || mix_depth(ar, id) != depth)
                continue;
            int32_t dest_id = m->dest_id;
            if (dest_id < 0 || (uint32_t)dest_id >= ar->mix_count || !ar->mixes[dest_id].used)
                continue;
            ar_mix *d = &ar->mixes[dest_id];
            for (uint32_t s = 0; s < m->buf_count; s++)
                for (uint32_t t = 0; t < d->buf_count; t++) {
                    float vol = m->matrix[s * AR_MIXBUFS + t] * m->volume;
                    if (vol == 0.0f)
                        continue;
                    const float *src = m->buf + (size_t)s * n;
                    float *dst = d->buf + (size_t)t * n;
                    for (uint32_t j = 0; j < n; j++)
                        dst[j] += src[j] * vol;
                }
        }
    }
}

static const float *sink_input(const audren *ar, const ar_sink *s, uint32_t k)
{
    const ar_mix *final = &ar->mixes[0];
    if (k >= s->input_count || s->inputs[k] >= final->buf_count)
        return NULL;
    return final->buf + (size_t)s->inputs[k] * ar->sample_count;
}

static void sink_device(audren *ar, const ar_sink *s)
{
    uint32_t n = ar->sample_count;
    int16_t *pcm = malloc((size_t)n * 2 * sizeof *pcm);
    if (!pcm)
        return;
    const float *in[AR_VOICE_CH];
    for (uint32_t k = 0; k < AR_VOICE_CH; k++)
        in[k] = sink_input(ar, s, k);
    for (uint32_t j = 0; j < n; j++) {
        float l = 0, r = 0;
        if (s->input_count == 1) {
            l = r = in[0] ? in[0][j] : 0;
        } else if (s->input_count == 6) {
            float fl = in[0] ? in[0][j] : 0, fr = in[1] ? in[1][j] : 0, fc = in[2] ? in[2][j] : 0;
            float bl = in[4] ? in[4][j] : 0, br = in[5] ? in[5][j] : 0;
            l = fl + 0.7071f * (fc + bl);
            r = fr + 0.7071f * (fc + br);
        } else {
            l = in[0] ? in[0][j] : 0;
            r = in[1] ? in[1][j] : 0;
        }
        pcm[2 * j] = clamp16(l);
        pcm[2 * j + 1] = clamp16(r);
    }
    if (rl_audio_queued(ar->host) < AR_QUEUE_MAX_FRAMES)
        rl_audio_write(ar->host, pcm, n);
    else
        rl_audio_count_drop(ar->host, n);
    free(pcm);
}

static void sink_circular(audren *ar, ar_sink *s)
{
    uint32_t n = ar->sample_count, ic = s->input_count;
    uint32_t bytes = n * ic * 2;
    if (!ic || !s->size || bytes > s->size || !s->addr)
        return;
    int16_t *pcm = malloc(bytes);
    if (!pcm)
        return;
    for (uint32_t j = 0; j < n; j++)
        for (uint32_t k = 0; k < ic; k++) {
            const float *in = sink_input(ar, s, k);
            pcm[j * ic + k] = clamp16(in ? in[j] : 0);
        }
    uint32_t first = s->size - s->last_write;
    if (first > bytes)
        first = bytes;
    audio_guest_write(s->addr + s->last_write, pcm, first);
    if (first < bytes)
        audio_guest_write(s->addr, (const uint8_t *)pcm + first, bytes - first);
    s->last_write = (s->last_write + bytes) % s->size;
    free(pcm);
}

void audren_render_frame(audren *ar)
{
    uint32_t n = ar->sample_count;
    for (uint32_t i = 0; i < ar->mix_count; i++)
        if (ar->mixes[i].buf)
            memset(ar->mixes[i].buf, 0, (size_t)AR_MIXBUFS * n * sizeof(float));
    for (uint32_t i = 0; i < ar->voice_count; i++) {
        ar_voice *v = &ar->voices[i];
        bool active;
        voice_prepare(v, &active);
        if (active)
            voice_render(ar, v);
        if (v->in_use)
            v->is_new = false;
    }
    route_mixes(ar);
    ar_mix *final = &ar->mixes[0];
    if (final->used && final->volume != 1.0f)
        for (uint32_t b = 0; b < final->buf_count; b++)
            for (uint32_t j = 0; j < n; j++)
                final->buf[(size_t)b * n + j] *= final->volume;
    bool device_done = false;
    for (uint32_t i = 0; final->used && i < ar->sink_count; i++) {
        ar_sink *s = &ar->sinks[i];
        if (!s->used)
            continue;
        if (s->type == SINK_DEVICE && !device_done) {
            sink_device(ar, s);
            device_done = true;
        } else if (s->type == SINK_CIRCULAR) {
            sink_circular(ar, s);
        }
    }
    ar->elapsed_frames++;
}
