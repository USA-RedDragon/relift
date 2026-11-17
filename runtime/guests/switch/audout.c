/**
 * @file audout.c
 * @brief audout:u: IAudioOutManager and IAudioOut, 16-bit PCM output played
 * through the host audio interface (or the null output).
 *
 * Commands and layouts follow Ryujinx (MIT) Ryujinx.Horizon/Sdk/Audio/Detail
 * AudioOutManager and AudioOut, and libnx (ISC) audout.c. A buffer appended
 * while the output is started is copied to the host stream at once. The
 * output plays on the system clock, as on the console: the audio timer
 * advances the play position 5 ms at a time, up to the frames appended,
 * releases the buffers it has passed and signals the buffer event. The host
 * device keeps its own pace (rl_audio_drift steers it onto the same clock),
 * so a device that runs fast or slow does not change what the guest sees.
 */
#include "hos_audio.h"

#include <stdlib.h>
#include <string.h>

/** @brief Most buffers queued at once (Ryujinx's limit). */
#define OUT_MAX_BUFFERS 32
#define OUT_RATE 48000
#define OUT_DEVICE_NAME "DeviceOut"
#define OUT_NAME_SIZE 0x100
#define OUT_STATE_STARTED 0
#define OUT_STATE_STOPPED 1

typedef struct out_buf {
    uint64_t tag;
    uint64_t data;
    uint64_t data_size;
    uint64_t end_frame;
    bool written;
} out_buf;

typedef struct audout {
    pthread_mutex_t lock;
    uint32_t channels;
    uint32_t state;
    float volume;
    out_buf q[OUT_MAX_BUFFERS];
    uint32_t qn;
    uint64_t released[OUT_MAX_BUFFERS];
    uint32_t rn;
    uint64_t written_frames;
    /** @brief Frames the output played on the system clock, 240 per 5 ms tick up to written_frames. */
    uint64_t played;
    rl_audio_out *host;
    hos_revent *event;
    void *tick;
} audout;

static void write_buffer(audout *a, out_buf *b)
{
    uint64_t frame_bytes = (uint64_t)a->channels * 2;
    uint64_t frames = b->data_size / frame_bytes;
    int16_t *pcm = calloc(frames * a->channels + 1, sizeof *pcm);
    if (pcm) {
        if (!audio_guest_read(b->data, pcm, frames * frame_bytes))
            memset(pcm, 0, frames * frame_bytes);
        if (a->volume != 1.0f)
            for (uint64_t i = 0; i < frames * a->channels; i++)
                pcm[i] = (int16_t)(pcm[i] * a->volume);
        rl_audio_write(a->host, pcm, (uint32_t)frames);
    }
    free(pcm);
    a->written_frames += frames;
    b->end_frame = a->written_frames;
    b->written = true;
}

static void release_done(audout *a, bool *signal)
{
    while (a->qn && a->q[0].written && a->q[0].end_frame <= a->played) {
        if (a->rn < OUT_MAX_BUFFERS)
            a->released[a->rn++] = a->q[0].tag;
        memmove(&a->q[0], &a->q[1], (a->qn - 1) * sizeof a->q[0]);
        a->qn--;
        *signal = true;
    }
}

static void out_tick(void *ctx)
{
    audout *a = ctx;
    bool sig = false;
    pthread_mutex_lock(&a->lock);
    if (a->state == OUT_STATE_STARTED) {
        a->played += OUT_RATE / (1000000000ull / AUDIO_FRAME_NS);
        if (a->played > a->written_frames)
            a->played = a->written_frames;
    }
    if (a->state == OUT_STATE_STARTED)
        release_done(a, &sig);
    pthread_mutex_unlock(&a->lock);
    if (sig)
        rl_hos_event_signal(a->event);
}

static void out_destroy(void *p)
{
    audout *a = p;
    audio_tick_remove(a->tick);
    rl_audio_close(a->host);
    if (a->event)
        hos_kobj_unref(&a->event->obj);
    pthread_mutex_destroy(&a->lock);
    free(a);
}

static rl_result out_get_state(rl_ipc_req *r)
{
    audout *a = r->self->state;
    pthread_mutex_lock(&a->lock);
    put32(rl_ipc_out(r, 4), a->state);
    pthread_mutex_unlock(&a->lock);
    return KR_SUCCESS;
}

static rl_result out_start(rl_ipc_req *r)
{
    audout *a = r->self->state;
    pthread_mutex_lock(&a->lock);
    if (a->state != OUT_STATE_STARTED) {
        a->state = OUT_STATE_STARTED;
        for (uint32_t i = 0; i < a->qn; i++)
            if (!a->q[i].written)
                write_buffer(a, &a->q[i]);
        rl_audio_pause(a->host, false);
    }
    pthread_mutex_unlock(&a->lock);
    return KR_SUCCESS;
}

static rl_result out_stop(rl_ipc_req *r)
{
    audout *a = r->self->state;
    pthread_mutex_lock(&a->lock);
    if (a->state != OUT_STATE_STOPPED) {
        a->state = OUT_STATE_STOPPED;
        rl_audio_pause(a->host, true);
    }
    pthread_mutex_unlock(&a->lock);
    return KR_SUCCESS;
}

static rl_result out_append(rl_ipc_req *r)
{
    audout *a = r->self->state;
    uint64_t tag;
    uint8_t ub[0x28];
    rl_ipc_buf in = rl_ipc_auto_in(r, 0);
    if (!rl_ipc_in(r, &tag, 8))
        return SF_RESULT_INVALID_HEADER;
    if (in.size < sizeof ub || !rl_ipc_buf_read(&in, ub, sizeof ub))
        return KR_INVALID_MEM_STATE;
    pthread_mutex_lock(&a->lock);
    if (a->qn >= OUT_MAX_BUFFERS) {
        pthread_mutex_unlock(&a->lock);
        return AUDIO_BUFFER_RING_FULL;
    }
    out_buf *b = &a->q[a->qn++];
    memset(b, 0, sizeof *b);
    b->tag = tag;
    b->data = le64(ub + 8) + le64(ub + 32);
    b->data_size = le64(ub + 24);
    if (a->state == OUT_STATE_STARTED)
        write_buffer(a, b);
    pthread_mutex_unlock(&a->lock);
    return KR_SUCCESS;
}

static rl_result out_register_event(rl_ipc_req *r)
{
    audout *a = r->self->state;
    return rl_ipc_push_copy(r, &a->event->obj);
}

static rl_result out_get_released(rl_ipc_req *r)
{
    audout *a = r->self->state;
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    bool sig = false;
    pthread_mutex_lock(&a->lock);
    if (a->state == OUT_STATE_STARTED)
        release_done(a, &sig);
    uint32_t n = a->rn;
    if ((uint64_t)n * 8 > out.size)
        n = (uint32_t)(out.size / 8);
    rl_result rc = KR_SUCCESS;
    uint64_t none = 0;
    if (n ? !rl_ipc_buf_write(&out, a->released, (uint64_t)n * 8)
          : out.size >= sizeof none && !rl_ipc_buf_write(&out, &none, sizeof none))
        rc = KR_INVALID_MEM_STATE;
    if (rc == KR_SUCCESS) {
        memmove(a->released, a->released + n, (a->rn - n) * 8);
        a->rn -= n;
        put32(rl_ipc_out(r, 4), n);
    }
    pthread_mutex_unlock(&a->lock);
    return rc;
}

static rl_result out_contains(rl_ipc_req *r)
{
    audout *a = r->self->state;
    uint64_t tag;
    if (!rl_ipc_in(r, &tag, 8))
        return SF_RESULT_INVALID_HEADER;
    uint8_t found = 0;
    pthread_mutex_lock(&a->lock);
    for (uint32_t i = 0; i < a->qn; i++)
        found |= a->q[i].tag == tag;
    pthread_mutex_unlock(&a->lock);
    *(uint8_t *)rl_ipc_out(r, 1) = found;
    return KR_SUCCESS;
}

static rl_result out_buffer_count(rl_ipc_req *r)
{
    audout *a = r->self->state;
    pthread_mutex_lock(&a->lock);
    put32(rl_ipc_out(r, 4), a->qn + a->rn);
    pthread_mutex_unlock(&a->lock);
    return KR_SUCCESS;
}

static rl_result out_played_samples(rl_ipc_req *r)
{
    audout *a = r->self->state;
    pthread_mutex_lock(&a->lock);
    put64(rl_ipc_out(r, 8), a->played);
    pthread_mutex_unlock(&a->lock);
    return KR_SUCCESS;
}

static rl_result out_flush(rl_ipc_req *r)
{
    audout *a = r->self->state;
    pthread_mutex_lock(&a->lock);
    bool pending = false;
    while (a->qn) {
        if (a->rn < OUT_MAX_BUFFERS)
            a->released[a->rn++] = a->q[0].tag;
        memmove(&a->q[0], &a->q[1], (a->qn - 1) * sizeof a->q[0]);
        a->qn--;
    }
    pthread_mutex_unlock(&a->lock);
    *(uint8_t *)rl_ipc_out(r, 1) = pending;
    rl_hos_event_signal(a->event);
    return KR_SUCCESS;
}

static rl_result out_set_volume(rl_ipc_req *r)
{
    audout *a = r->self->state;
    float v;
    if (!rl_ipc_in(r, &v, 4))
        return SF_RESULT_INVALID_HEADER;
    pthread_mutex_lock(&a->lock);
    a->volume = v < 0 ? 0 : (v > 1 ? 1 : v);
    pthread_mutex_unlock(&a->lock);
    return KR_SUCCESS;
}

static rl_result out_get_volume(rl_ipc_req *r)
{
    audout *a = r->self->state;
    pthread_mutex_lock(&a->lock);
    float v = a->volume;
    pthread_mutex_unlock(&a->lock);
    __builtin_memcpy(rl_ipc_out(r, 4), &v, 4);
    return KR_SUCCESS;
}

static const rl_ipc_cmd out_cmds[] = {
    { 0, "GetAudioOutState", out_get_state },
    { 1, "Start", out_start },
    { 2, "Stop", out_stop },
    { 3, "AppendAudioOutBuffer", out_append },
    { 4, "RegisterBufferEvent", out_register_event },
    { 5, "GetReleasedAudioOutBuffers", out_get_released },
    { 6, "ContainsAudioOutBuffer", out_contains },
    { 7, "AppendAudioOutBufferAuto", out_append },
    { 8, "GetReleasedAudioOutBuffersAuto", out_get_released },
    { 9, "GetAudioOutBufferCount", out_buffer_count },
    { 10, "GetAudioOutPlayedSampleCount", out_played_samples },
    { 11, "FlushAudioOutBuffers", out_flush },
    { 12, "SetAudioOutVolume", out_set_volume },
    { 13, "GetAudioOutVolume", out_get_volume },
};

static const rl_ipc_class out_class = {
    .name = "nn::audio::detail::IAudioOut",
    .cmds = out_cmds,
    .ncmds = sizeof out_cmds / sizeof out_cmds[0],
    .pointer_buffer_size = 0,
    .destroy = out_destroy,
};

static void write_name(const rl_ipc_buf *b, const char *name)
{
    uint8_t buf[OUT_NAME_SIZE] = { 0 };
    memcpy(buf, name, strlen(name));
    rl_ipc_buf_write(b, buf, OUT_NAME_SIZE);
}

static rl_result mgr_list(rl_ipc_req *r)
{
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    uint32_t n = 0;
    if (out.size >= OUT_NAME_SIZE) {
        write_name(&out, OUT_DEVICE_NAME);
        n = 1;
    }
    put32(rl_ipc_out(r, 4), n);
    return KR_SUCCESS;
}

static rl_result mgr_open(rl_ipc_req *r)
{
    uint32_t in[4];
    if (!rl_ipc_in(r, in, 16))
        return SF_RESULT_INVALID_HEADER;
    uint32_t rate = in[0];
    uint32_t channels = in[1] & 0xFFFF;
    rl_ipc_buf name_in = rl_ipc_auto_in(r, 0), name_out = rl_ipc_auto_out(r, 0);
    char name[OUT_NAME_SIZE + 1] = { 0 };
    if (name_in.size && !rl_ipc_buf_read(&name_in, name, name_in.size < OUT_NAME_SIZE ? name_in.size : OUT_NAME_SIZE))
        return KR_INVALID_MEM_STATE;
    if (name[0] && strcmp(name, OUT_DEVICE_NAME) != 0)
        return AUDIO_DEVICE_NOT_FOUND;
    if (rate != 0 && rate != OUT_RATE)
        return AUDIO_UNSUPPORTED_SAMPLE_RATE;
    if (channels != 0 && channels != 1 && channels != 2 && channels != 6)
        return AUDIO_UNSUPPORTED_CHANNELS;
    audout *a = calloc(1, sizeof *a);
    if (!a)
        return KR_OUT_OF_MEMORY;
    pthread_mutex_init(&a->lock, NULL);
    a->channels = channels == 6 ? 6 : 2;
    a->state = OUT_STATE_STOPPED;
    a->volume = 1.0f;
    a->event = rl_hos_event_create();
    a->host = rl_audio_open(OUT_RATE, a->channels);
    rl_ipc_object *o = rl_ipc_object_new(&out_class, a);
    if (!a->event || !a->host || !o) {
        if (o)
            rl_ipc_object_unref(o);
        else
            out_destroy(a);
        return AUDIO_OPERATION_FAILED;
    }
    a->tick = audio_tick_add(out_tick, a);
    rl_audio_pause(a->host, true);
    if (name_out.size >= OUT_NAME_SIZE)
        write_name(&name_out, OUT_DEVICE_NAME);
    uint8_t *res = rl_ipc_out(r, 16);
    put32(res, OUT_RATE);
    put32(res + 4, a->channels);
    put32(res + 8, 2);
    put32(res + 12, OUT_STATE_STOPPED);
    return rl_ipc_push_object(r, o);
}

static const rl_ipc_cmd mgr_cmds[] = {
    { 0, "ListAudioOuts", mgr_list },
    { 1, "OpenAudioOut", mgr_open },
    { 2, "ListAudioOutsAuto", mgr_list },
    { 3, "OpenAudioOutAuto", mgr_open },
};

static const rl_ipc_class mgr_class = {
    .name = "nn::audio::detail::IAudioOutManager",
    .cmds = mgr_cmds,
    .ncmds = sizeof mgr_cmds / sizeof mgr_cmds[0],
    .pointer_buffer_size = RL_AUDIO_POINTER_BUFFER_SIZE,
    .destroy = NULL,
};

rl_ipc_object *audout_manager_new(void) { return rl_ipc_object_new(&mgr_class, NULL); }
