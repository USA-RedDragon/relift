/**
 * @file audio.c
 * @brief Host audio output and Opus decoder dispatch, the null output and the
 * stub decoder. The real backends live in runtime/host.
 */
#include <relift/audio.h>

#include "core.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

static const rl_audio_backend *g_audio;
static const rl_opus_backend *g_opus;

typedef struct null_out {
    rl_audio_out head;
    pthread_mutex_t lock;
    uint32_t rate;
    bool paused;
    double queued;
    uint64_t last_ns;
} null_out;

static void null_drain(null_out *n)
{
    uint64_t now = rl_clock_ns();
    if (!n->paused && now > n->last_ns) {
        n->queued -= (double)(now - n->last_ns) * n->rate / 1e9;
        if (n->queued < 0)
            n->queued = 0;
    }
    n->last_ns = now;
}

static rl_audio_out *null_open(uint32_t rate, uint32_t channels)
{
    (void)channels;
    null_out *n = calloc(1, sizeof *n);
    if (!n)
        return NULL;
    n->head.b = rl_audio_null_backend();
    n->rate = rate ? rate : 48000;
    n->paused = true;
    n->last_ns = rl_clock_ns();
    pthread_mutex_init(&n->lock, NULL);
    return &n->head;
}

static void null_close(rl_audio_out *o)
{
    null_out *n = (null_out *)o;
    pthread_mutex_destroy(&n->lock);
    free(n);
}

static void null_write(rl_audio_out *o, const int16_t *pcm, uint32_t frames)
{
    null_out *n = (null_out *)o;
    (void)pcm;
    pthread_mutex_lock(&n->lock);
    null_drain(n);
    n->queued += frames;
    pthread_mutex_unlock(&n->lock);
}

static uint32_t null_queued(rl_audio_out *o)
{
    null_out *n = (null_out *)o;
    pthread_mutex_lock(&n->lock);
    null_drain(n);
    uint32_t q = (uint32_t)(n->queued + 0.5);
    pthread_mutex_unlock(&n->lock);
    return q;
}

static void null_pause(rl_audio_out *o, bool paused)
{
    null_out *n = (null_out *)o;
    pthread_mutex_lock(&n->lock);
    null_drain(n);
    n->paused = paused;
    pthread_mutex_unlock(&n->lock);
}

static const rl_audio_backend g_null_audio = {
    .name = "null",
    .open = null_open,
    .close = null_close,
    .write = null_write,
    .queued = null_queued,
    .pause = null_pause,
};

const rl_audio_backend *rl_audio_null_backend(void) { return &g_null_audio; }

void rl_audio_set_backend(const rl_audio_backend *b) { g_audio = b; }

const rl_audio_backend *rl_audio_backend_current(void)
{
    if (g_audio)
        return g_audio;
#ifdef RL_HOST_AUDIO
    const char *e = getenv("RELIFT_AUDIO");
    if (!e || strcmp(e, "null") != 0)
        return rl_host_audio_backend();
#endif
    return &g_null_audio;
}

rl_audio_out *rl_audio_open(uint32_t rate, uint32_t channels)
{
    const rl_audio_backend *b = rl_audio_backend_current();
    rl_audio_out *o = b->open(rate, channels);
    if (!o && b != &g_null_audio) {
        rl_log(RL_LOG_WARN, "audio", "%s output failed to open, using the null output", b->name);
        b = &g_null_audio;
        o = b->open(rate, channels);
    }
    return o;
}

void rl_audio_close(rl_audio_out *o)
{
    if (o)
        o->b->close(o);
}

void rl_audio_write(rl_audio_out *o, const int16_t *pcm, uint32_t frames)
{
    if (o && frames)
        o->b->write(o, pcm, frames);
}

uint32_t rl_audio_queued(rl_audio_out *o) { return o ? o->b->queued(o) : 0; }

void rl_audio_pause(rl_audio_out *o, bool paused)
{
    if (o)
        o->b->pause(o, paused);
}

typedef struct stub_opus {
    rl_opus head;
    uint32_t rate;
    uint32_t channels;
} stub_opus;

static int32_t toc_frames(const uint8_t *p, uint32_t len, uint32_t rate)
{
    if (len < 1)
        return RL_OPUS_BAD_ARG;
    uint32_t count;
    switch (p[0] & 3) {
    case 0:
        count = 1;
        break;
    case 1:
    case 2:
        count = 2;
        break;
    default:
        if (len < 2)
            return RL_OPUS_INVALID_PACKET;
        count = p[1] & 0x3F;
    }
    uint32_t per;
    if (p[0] & 0x80)
        per = (rate << ((p[0] >> 3) & 3)) / 400;
    else if ((p[0] & 0x60) == 0x60)
        per = (p[0] & 8) ? rate / 50 : rate / 100;
    else {
        uint32_t s = (p[0] >> 3) & 3;
        per = s == 3 ? rate * 60 / 1000 : (rate << s) / 100;
    }
    uint32_t total = per * count;
    if (count == 0 || total * 25 > rate * 3)
        return RL_OPUS_INVALID_PACKET;
    return (int32_t)total;
}

int32_t rl_opus_packet_frames(const uint8_t *pkt, uint32_t len, uint32_t rate)
{
    return toc_frames(pkt, len, rate);
}

static rl_opus *stub_open(uint32_t rate, uint32_t channels, uint32_t streams, uint32_t coupled, const uint8_t *mapping)
{
    (void)streams;
    (void)coupled;
    (void)mapping;
    stub_opus *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->rate = rate;
    d->channels = channels;
    return &d->head;
}

static void stub_close(rl_opus *d) { free(d); }

static int32_t stub_decode(rl_opus *o, const uint8_t *pkt, uint32_t len, int16_t *pcm, uint32_t max_frames)
{
    stub_opus *d = (stub_opus *)o;
    int32_t n = toc_frames(pkt, len, d->rate);
    if (n < 0)
        return n;
    if ((uint32_t)n > max_frames)
        return RL_OPUS_BUFFER_TOO_SMALL;
    memset(pcm, 0, (size_t)n * d->channels * sizeof *pcm);
    return n;
}

static void stub_reset(rl_opus *d) { (void)d; }

static const rl_opus_backend g_stub_opus = {
    .name = "stub",
    .open = stub_open,
    .close = stub_close,
    .decode = stub_decode,
    .reset = stub_reset,
};

const rl_opus_backend *rl_opus_stub_backend(void) { return &g_stub_opus; }

void rl_opus_set_backend(const rl_opus_backend *b) { g_opus = b; }

const rl_opus_backend *rl_opus_backend_current(void)
{
    if (g_opus)
        return g_opus;
#ifdef RL_HOST_AUDIO
    return rl_host_opus_backend();
#else
    return &g_stub_opus;
#endif
}

rl_opus *rl_opus_open(uint32_t rate, uint32_t channels, uint32_t streams, uint32_t coupled, const uint8_t *mapping)
{
    const rl_opus_backend *b = rl_opus_backend_current();
    rl_opus *d = b->open(rate, channels, streams, coupled, mapping);
    if (d)
        d->b = b;
    return d;
}

void rl_opus_close(rl_opus *d)
{
    if (d)
        d->b->close(d);
}

int32_t rl_opus_decode(rl_opus *d, const uint8_t *pkt, uint32_t len, int16_t *pcm, uint32_t max_frames)
{
    return d->b->decode(d, pkt, len, pcm, max_frames);
}

void rl_opus_reset(rl_opus *d) { d->b->reset(d); }
