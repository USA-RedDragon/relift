/**
 * @file srv_audsmx.c
 * @brief The Switch 2 stream mixer audsmx:u (nn::audio::sysmix), which
 * carries a program's audio input streams.
 *
 * switchbrew gives the command ids and argument shapes without names. The
 * names, raw layouts and the stream ring are verified from a Switch 2 SDK's
 * client (nn::audio::sysmix::detail):
 * - IUserSession 1 creates a stream over a work buffer in the client's
 *   memory (address and size in the request); the service reaches it
 *   through the process handle given at session creation.
 * - OpenStream lays the ring out in that buffer before it sends
 *   IExternalStreamSession 0: three 0x40-byte control blocks at +0, +0x40
 *   and +0x1000, the samples from +0x1040 (+0x80 more with open flag bit 0),
 *   frames of channels times the format's bytes per sample, and a ring of
 *   frame count + 1 frames. The u32 at +0x1004 is the producer's position
 *   (set to the open's second count), the u32 at +0x44 the consumer's.
 * - An input stream's reads and filled-frame count run in the client over
 *   those positions without a request, so the service fills the ring on its
 *   own clock.
 * The command names, the input stream's device value 2 and creation flags
 * 2, engagement gating the stream, data notification mode 2 (signal when at
 * least threshold frames are filled) and purge advancing the consumer
 * agree with Lichii (src/core/hle/service/audio/nx2_audio_device_manager.h
 * and nx2_sysmix.cpp, docs/nx2/microphone.md), which match the SDK reading
 * above and tie-break what the client code does not show (inferred).
 * The input device has no sound to give: relift fills each engaged input
 * stream with silence at its sample rate, one 5 ms block per audio timer
 * frame as a live capture fills (inferred from Lichii, which keeps one block
 * per tick for input; a full ring is left full). Commands relift does not
 * answer stop by name.
 */
#include "hos.h"
#include "hos_audio.h"
#include "services.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define RING_CONSUMER 0x44u
#define RING_PRODUCER 0x1004u
#define RING_DATA 0x1040u
#define RING_DATA_EXTRA 0x80u

/** @brief Bytes per sample of SampleFormat 1 to 5, the SDK client's table (PCM8, PCM16, PCM24 in 4 bytes, PCM32, float). */
static const uint32_t sample_bytes[5] = { 1, 2, 4, 4, 4 };

typedef struct smx_stream {
    pthread_mutex_t lock;
    uint64_t buf, size;
    uint32_t device, flags, id;
    bool open, engaged;
    uint32_t data_mode;
    int32_t threshold;
    uint64_t data;
    uint32_t frame_bytes, cap, rate;
    uint64_t owed;
    hos_revent *data_event, *state_event;
    void *tick;
} smx_stream;

/** @brief CreateStream's device value of an input stream. */
#define SMX_DEVICE_INPUT 2u
/** @brief Most streams engaged by id at once. */
#define SMX_MAX_STREAMS 32

static pthread_mutex_t g_streams_lock = PTHREAD_MUTEX_INITIALIZER;
static smx_stream *g_streams[SMX_MAX_STREAMS];
static uint32_t g_next_id = 1;

/** @brief Ring positions of an open stream: false when the guest has moved them out of the ring. */
static bool occupancy(smx_stream *s, uint32_t *r, uint32_t *w, uint32_t *filled)
{
    if (!audio_guest_read(s->buf + RING_CONSUMER, r, 4) || !audio_guest_read(s->buf + RING_PRODUCER, w, 4) ||
        *r >= s->cap || *w >= s->cap)
        return false;
    *filled = *w >= *r ? *w - *r : *w + s->cap - *r;
    return true;
}

static void notify(smx_stream *s, uint32_t filled)
{
    if (s->data_mode == 2 && s->data_event && filled >= (uint32_t)s->threshold)
        rl_hos_event_signal(s->data_event);
}

/** @brief One 5 ms frame of the audio timer: silence up to the free room of the ring. */
static void produce(void *ctx)
{
    smx_stream *s = ctx;
    static const uint8_t zero[4096];
    pthread_mutex_lock(&s->lock);
    uint32_t w, r, filled;
    if (!s->open || !s->engaged || !occupancy(s, &r, &w, &filled)) {
        pthread_mutex_unlock(&s->lock);
        return;
    }
    s->owed += (uint64_t)s->rate * 5;
    uint32_t want = (uint32_t)(s->owed / 1000);
    s->owed %= 1000;
    uint32_t room = s->cap - 1 - filled;
    uint32_t n = want < room ? want : room;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t at = s->data + (uint64_t)w * s->frame_bytes;
        (void)audio_guest_write(at, zero, s->frame_bytes < sizeof zero ? s->frame_bytes : sizeof zero);
        w = w + 1 == s->cap ? 0 : w + 1;
    }
    if (n) {
        __atomic_thread_fence(__ATOMIC_RELEASE);
        (void)audio_guest_write(s->buf + RING_PRODUCER, &w, 4);
        notify(s, filled + n);
    }
    pthread_mutex_unlock(&s->lock);
}

static void stream_destroy(void *state)
{
    smx_stream *s = state;
    pthread_mutex_lock(&g_streams_lock);
    for (uint32_t i = 0; i < SMX_MAX_STREAMS; i++)
        if (g_streams[i] == s)
            g_streams[i] = NULL;
    pthread_mutex_unlock(&g_streams_lock);
    if (s->tick)
        audio_tick_remove(s->tick);
    if (s->data_event)
        hos_kobj_unref(&s->data_event->obj);
    if (s->state_event)
        hos_kobj_unref(&s->state_event->obj);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

#define STREAM(r) ((smx_stream *)(r)->self->state)

/**
 * @brief 0 OpenStream: a StreamFormat (u32 format, channels, sample rate), a
 * u32 frame count, a u32 starting fill and u32 open flags; the client has
 * laid the ring out already.
 */
static rl_result open_stream(rl_ipc_req *r)
{
    struct {
        uint32_t format, channels, rate, frames, start, flags;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    smx_stream *s = STREAM(r);
    if (in.format < 1 || in.format > 5 || in.channels < 1 || in.rate == 0 || in.frames < 1) {
        rl_ipc_stop(r, "audsmx", "OpenStream of format %u, %u channels, %u Hz, %u frames is not answered yet",
                    in.format, in.channels, in.rate, in.frames);
        return AUDIO_OPERATION_FAILED;
    }
    pthread_mutex_lock(&s->lock);
    s->frame_bytes = sample_bytes[in.format - 1] * in.channels;
    s->cap = in.frames + 1;
    s->rate = in.rate;
    s->data = s->buf + RING_DATA + ((in.flags & 1) ? RING_DATA_EXTRA : 0);
    bool fits = s->data + (uint64_t)s->cap * s->frame_bytes <= s->buf + s->size;
    s->open = fits;
    pthread_mutex_unlock(&s->lock);
    if (!fits) {
        rl_ipc_stop(r, "audsmx", "OpenStream: a ring of %u frames of %u bytes does not fit the 0x%llx-byte buffer",
                    s->cap, s->frame_bytes, (unsigned long long)s->size);
        return AUDIO_OPERATION_FAILED;
    }
    if (s->device != SMX_DEVICE_INPUT)
        return KR_SUCCESS;
    if (!s->tick)
        s->tick = audio_tick_add(produce, s);
    return s->tick ? KR_SUCCESS : KR_OUT_OF_RESOURCE;
}

/** @brief 1 CloseStream: the ring stops filling. */
static rl_result close_stream(rl_ipc_req *r)
{
    smx_stream *s = STREAM(r);
    pthread_mutex_lock(&s->lock);
    s->open = false;
    pthread_mutex_unlock(&s->lock);
    return KR_SUCCESS;
}

static rl_result push_event(rl_ipc_req *r, hos_revent **slot)
{
    if (!*slot)
        *slot = rl_hos_event_create();
    if (!*slot)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_copy(r, &(*slot)->obj);
}

/** @brief 5 AcquireStreamStateChangeEvent: an event, signaled when the stream's state changes. */
static rl_result acquire_state_event(rl_ipc_req *r) { return push_event(r, &STREAM(r)->state_event); }

/**
 * @brief 7 AcquireStreamDataEvent: a u32 notification mode and an s32
 * threshold; an event, signaled in mode 2 while at least threshold frames
 * are filled. Mode 0 never signals.
 */
static rl_result acquire_data_event(rl_ipc_req *r)
{
    struct {
        uint32_t mode;
        int32_t threshold;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    smx_stream *s = STREAM(r);
    if (in.mode != 0 && in.mode != 2) {
        rl_ipc_stop(r, "audsmx", "AcquireStreamDataEvent with notification mode %u is not answered yet", in.mode);
        return AUDIO_OPERATION_FAILED;
    }
    rl_result rc = push_event(r, &s->data_event);
    if (rc != KR_SUCCESS)
        return rc;
    pthread_mutex_lock(&s->lock);
    s->data_mode = in.mode;
    s->threshold = in.threshold < 0 ? 0 : in.threshold;
    uint32_t rd, wr, filled;
    if (s->open && occupancy(s, &rd, &wr, &filled))
        notify(s, filled);
    pthread_mutex_unlock(&s->lock);
    return KR_SUCCESS;
}

/** @brief 8 ReleaseStreamDataEvent: notifications stop. */
static rl_result release_data_event(rl_ipc_req *r)
{
    smx_stream *s = STREAM(r);
    pthread_mutex_lock(&s->lock);
    s->data_mode = 0;
    pthread_mutex_unlock(&s->lock);
    return KR_SUCCESS;
}

/** @brief 11 PurgeStream: a u32 frame count, 0 for all; drops up to that many filled frames, the count dropped out. */
static rl_result purge_stream(rl_ipc_req *r)
{
    uint32_t frames;
    if (!rl_srv_in(r, &frames, 4))
        return SF_RESULT_INVALID_HEADER;
    smx_stream *s = STREAM(r);
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = 0;
    pthread_mutex_lock(&s->lock);
    uint32_t rd, wr, filled;
    if (s->device != SMX_DEVICE_INPUT || !s->open || !occupancy(s, &rd, &wr, &filled)) {
        pthread_mutex_unlock(&s->lock);
        return AUDIO_OPERATION_FAILED;
    }
    uint32_t n = frames == 0 || frames > filled ? filled : frames;
    uint32_t to = (uint32_t)(((uint64_t)rd + n) % s->cap);
    (void)audio_guest_write(s->buf + RING_CONSUMER, &to, 4);
    *out = n;
    notify(s, filled - n);
    pthread_mutex_unlock(&s->lock);
    return KR_SUCCESS;
}

static rl_result release_ok(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

static rl_result stream_not_answered(rl_ipc_req *r)
{
    static const char *const names[13] = {
        "OpenStream",      "CloseStream", NULL, NULL, "SetStreamGain", "AcquireStreamStateChangeEvent",
        "ReleaseStreamStateChangeEvent", "AcquireStreamDataEvent", "ReleaseStreamDataEvent", "ClockStream",
        "PrimeStream",     "PurgeStream", "RenameStream",
    };
    const char *name = r->cmd < 13 && names[r->cmd] ? names[r->cmd] : "?";
    rl_ipc_stop(r, "audsmx", "IExternalStreamSession %u %s is not answered yet", r->cmd, name);
    return AUDIO_OPERATION_FAILED;
}

static const rl_ipc_cmd stream_cmds[] = {
    { 0, "OpenStream", open_stream },
    { 1, "CloseStream", close_stream },
    { 2, "?", stream_not_answered },
    { 3, "?", stream_not_answered },
    { 4, "SetStreamGain", stream_not_answered },
    { 5, "AcquireStreamStateChangeEvent", acquire_state_event },
    { 6, "ReleaseStreamStateChangeEvent", release_ok },
    { 7, "AcquireStreamDataEvent", acquire_data_event },
    { 8, "ReleaseStreamDataEvent", release_data_event },
    { 9, "ClockStream", stream_not_answered },
    { 10, "PrimeStream", stream_not_answered },
    { 11, "PurgeStream", purge_stream },
    { 12, "RenameStream", stream_not_answered },
};

static const rl_ipc_class stream_class = { "nn::audio::sysmix::IExternalStreamSession", stream_cmds,
                                           sizeof stream_cmds / sizeof stream_cmds[0], 0, stream_destroy };

/**
 * @brief 1 CreateStream: the stream's name in a type-5 buffer, a u32 device
 * (2 for input), u32 creation flags, a u64 the SDK takes from the applet
 * resource user id, the work buffer's address and size; a u32 stream id and
 * an IExternalStreamSession out. relift fills only input streams; an output
 * stream stops by name.
 */
static rl_result create_stream(rl_ipc_req *r)
{
    struct {
        uint32_t device, flags;
        uint64_t aruid, buf, size;
    } in;
    if (!rl_srv_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    if (in.device != SMX_DEVICE_INPUT) {
        rl_ipc_stop(r, "audsmx", "CreateStream of device %u (flags 0x%x): output streams are not answered yet",
                    in.device, in.flags);
        return AUDIO_OPERATION_FAILED;
    }
    if (in.size < RING_DATA || in.buf == 0) {
        rl_ipc_stop(r, "audsmx", "CreateStream over a 0x%llx-byte buffer is not answered yet",
                    (unsigned long long)in.size);
        return AUDIO_OPERATION_FAILED;
    }
    smx_stream *s = calloc(1, sizeof *s);
    if (!s)
        return KR_OUT_OF_RESOURCE;
    pthread_mutex_init(&s->lock, NULL);
    s->buf = in.buf;
    s->size = in.size;
    s->device = in.device;
    s->flags = in.flags;
    s->id = __atomic_fetch_add(&g_next_id, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&g_streams_lock);
    bool placed = false;
    for (uint32_t i = 0; i < SMX_MAX_STREAMS && !placed; i++)
        if (!g_streams[i]) {
            g_streams[i] = s;
            placed = true;
        }
    pthread_mutex_unlock(&g_streams_lock);
    if (!placed) {
        stream_destroy(s);
        return KR_OUT_OF_RESOURCE;
    }
    rl_ipc_object *o = rl_ipc_object_new(&stream_class, s);
    if (!o) {
        stream_destroy(s);
        return KR_OUT_OF_RESOURCE;
    }
    uint32_t *out = rl_ipc_out(r, 4);
    if (!out)
        return SF_RESULT_INVALID_HEADER;
    *out = s->id;
    return rl_ipc_push_object(r, o);
}

/** @brief Engages or disengages the streams whose u32 ids fill the type-5 buffer. */
static rl_result set_engaged(rl_ipc_req *r, bool engaged)
{
    rl_ipc_buf b = rl_ipc_auto_in(r, 0);
    if (b.size % 4)
        return SF_RESULT_INVALID_HEADER;
    for (uint64_t at = 0; at < b.size; at += 4) {
        uint32_t id;
        rl_ipc_buf one = { b.addr + at, 4, b.attr };
        if (!rl_ipc_buf_read(&one, &id, 4))
            return SF_RESULT_INVALID_HEADER;
        smx_stream *s = NULL;
        pthread_mutex_lock(&g_streams_lock);
        for (uint32_t i = 0; i < SMX_MAX_STREAMS; i++)
            if (g_streams[i] && g_streams[i]->id == id)
                s = g_streams[i];
        if (s) {
            pthread_mutex_lock(&s->lock);
            s->engaged = engaged;
            pthread_mutex_unlock(&s->lock);
        }
        pthread_mutex_unlock(&g_streams_lock);
        if (!s)
            return AUDIO_OPERATION_FAILED;
    }
    return KR_SUCCESS;
}

/** @brief 2 EngageStreams: the streams start (an input stream starts filling). */
static rl_result engage_streams(rl_ipc_req *r) { return set_engaged(r, true); }

/** @brief 3 DisengageStreams: the streams stop. */
static rl_result disengage_streams(rl_ipc_req *r) { return set_engaged(r, false); }

static const rl_ipc_cmd session_cmds[] = {
    { 1, "CreateStream", create_stream },
    { 2, "EngageStreams", engage_streams },
    { 3, "DisengageStreams", disengage_streams },
};

static const rl_ipc_class session_class = { "nn::audio::sysmix::IUserSession", session_cmds,
                                            sizeof session_cmds / sizeof session_cmds[0], 0, NULL };

/** @brief 0: the client's pid, its process handle, a u32 (1 from the SDK) and a u64; an IUserSession out. */
static rl_result create_user_session(rl_ipc_req *r)
{
    rl_ipc_object *o = rl_srv_object(&session_class, NULL);
    return o ? rl_ipc_push_object(r, o) : KR_OUT_OF_RESOURCE;
}

static const rl_ipc_cmd manager_cmds[] = {
    { 0, "CreateUserSession", create_user_session },
};

static const rl_ipc_class manager_class = { "nn::audio::sysmix::IUserSessionManager", manager_cmds,
                                            sizeof manager_cmds / sizeof manager_cmds[0], 0, NULL };

static rl_ipc_object *manager_factory(void) { return rl_srv_object(&manager_class, NULL); }

void rl_srv_install_audsmx(void)
{
    if (hos_switch2())
        rl_ipc_register_service("audsmx:u", manager_factory);
}
