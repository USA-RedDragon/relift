/**
 * @file audio.c
 * @brief Host audio output and Opus decoder dispatch, the null output and the
 * stub decoder. The real backends live in runtime/host.
 */
#include <relift/audio.h>

#include "core.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

typedef struct tap_out {
    rl_audio_out head;
    rl_audio_out *in;
    uint32_t rate, channels, index;
    FILE *wav;
    uint64_t frames, dropped, underruns, writes, wav_frames, opened_ns, resumed_ns, active_ns, last_report_ns;
    uint32_t min_queued, max_queued;
    uint64_t period_frames, period_underruns, period_dropped;
    bool paused, started, report;
} tap_out;

static const rl_audio_backend g_tap;
static uint32_t g_streams;

static void put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void wav_header(tap_out *t)
{
    uint8_t h[44];
    uint32_t bytes = (uint32_t)(t->wav_frames * t->channels * 2);
    memcpy(h, "RIFF", 4);
    put_le32(h + 4, 36 + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    put_le32(h + 16, 16);
    h[20] = 1;
    h[21] = 0;
    h[22] = (uint8_t)t->channels;
    h[23] = 0;
    put_le32(h + 24, t->rate);
    put_le32(h + 28, t->rate * t->channels * 2);
    h[32] = (uint8_t)(t->channels * 2);
    h[33] = 0;
    h[34] = 16;
    h[35] = 0;
    memcpy(h + 36, "data", 4);
    put_le32(h + 40, bytes);
    long at = ftell(t->wav);
    fseek(t->wav, 0, SEEK_SET);
    fwrite(h, 1, sizeof h, t->wav);
    fseek(t->wav, at, SEEK_SET);
    fflush(t->wav);
}

static void tap_report(tap_out *t, uint64_t now)
{
    uint64_t active = t->active_ns + (t->paused ? 0 : now - t->resumed_ns);
    double secs = (double)active / 1e9;
    rl_log(RL_LOG_INFO, "audio",
           "stream %u: %u Hz %u ch, %.1f s playing, %llu frames written (%.4f of real time), %llu underruns, "
           "%llu frames dropped, queued %u..%u frames (this period: %llu frames, %llu underruns, %llu dropped)",
           t->index, t->rate, t->channels, secs, (unsigned long long)t->frames,
           secs > 0 ? (double)t->frames / (secs * t->rate) : 0.0, (unsigned long long)t->underruns,
           (unsigned long long)t->dropped, t->min_queued == UINT32_MAX ? 0 : t->min_queued, t->max_queued,
           (unsigned long long)t->period_frames, (unsigned long long)t->period_underruns,
           (unsigned long long)t->period_dropped);
    t->period_frames = t->period_underruns = t->period_dropped = 0;
    t->last_report_ns = now;
}

typedef struct open_job {
    pthread_mutex_t lock;
    pthread_cond_t done_cv;
    const rl_audio_backend *b;
    uint32_t rate, channels;
    rl_audio_out *out;
    rl_audio_out *timing;
    rl_audio_out *host;
    bool done, late, closed, paused;
    int refs;
} open_job;

typedef struct late_out {
    rl_audio_out head;
    open_job *j;
} late_out;

static const rl_audio_backend g_late;

static void job_unref(open_job *j)
{
    pthread_mutex_lock(&j->lock);
    int left = --j->refs;
    pthread_mutex_unlock(&j->lock);
    if (left == 0) {
        pthread_cond_destroy(&j->done_cv);
        pthread_mutex_destroy(&j->lock);
        free(j);
    }
}

static void *open_job_main(void *arg)
{
    open_job *j = arg;
    rl_audio_out *o = j->b->open(j->rate, j->channels);
    if (o && !o->b)
        o->b = j->b;
    pthread_mutex_lock(&j->lock);
    rl_audio_out *drop = NULL;
    if (j->late && o && !j->closed) {
        j->host = o;
        if (!j->paused)
            o->b->pause(o, false);
        rl_log(RL_LOG_INFO, "audio", "%s output opened after the deadline, switching from the null output", j->b->name);
    } else if (j->late) {
        drop = o;
    } else {
        j->out = o;
    }
    j->done = true;
    pthread_cond_signal(&j->done_cv);
    pthread_mutex_unlock(&j->lock);
    if (drop)
        j->b->close(drop);
    job_unref(j);
    return NULL;
}

static uint32_t open_timeout_ms(void)
{
    const char *e = getenv("RELIFT_AUDIO_OPEN_MS");
    if (e && *e)
        return (uint32_t)strtoul(e, NULL, 10);
    return 3000;
}

static void late_close(rl_audio_out *o)
{
    open_job *j = ((late_out *)o)->j;
    pthread_mutex_lock(&j->lock);
    j->closed = true;
    rl_audio_out *host = j->host;
    j->host = NULL;
    pthread_mutex_unlock(&j->lock);
    if (host)
        host->b->close(host);
    j->timing->b->close(j->timing);
    job_unref(j);
    free(o);
}

static void late_write(rl_audio_out *o, const int16_t *pcm, uint32_t frames)
{
    open_job *j = ((late_out *)o)->j;
    pthread_mutex_lock(&j->lock);
    j->timing->b->write(j->timing, pcm, frames);
    if (j->host)
        j->host->b->write(j->host, pcm, frames);
    pthread_mutex_unlock(&j->lock);
}

static uint32_t late_queued(rl_audio_out *o)
{
    open_job *j = ((late_out *)o)->j;
    pthread_mutex_lock(&j->lock);
    uint32_t q = j->timing->b->queued(j->timing);
    pthread_mutex_unlock(&j->lock);
    return q;
}

static void late_pause(rl_audio_out *o, bool paused)
{
    open_job *j = ((late_out *)o)->j;
    pthread_mutex_lock(&j->lock);
    j->paused = paused;
    j->timing->b->pause(j->timing, paused);
    if (j->host)
        j->host->b->pause(j->host, paused);
    pthread_mutex_unlock(&j->lock);
}

static const rl_audio_backend g_late = {
    .name = "late",
    .close = late_close,
    .write = late_write,
    .queued = late_queued,
    .pause = late_pause,
};

/**
 * Opens on a helper thread and waits at most the open deadline, so a host
 * backend stuck inside its library cannot stop the caller. When the deadline
 * passes the caller gets a stream timed by the null output, exactly as if the
 * open had failed; if the helper's open completes later, the host device is
 * attached and receives every later write and pause. Timing (queued frames)
 * always comes from the null output, so the switch changes nothing the guest
 * can observe. A helper stuck inside the library is left alone.
 */
static rl_audio_out *open_bounded(const rl_audio_backend *b, uint32_t rate, uint32_t channels)
{
    open_job *j = calloc(1, sizeof *j);
    if (!j)
        return NULL;
    pthread_mutex_init(&j->lock, NULL);
    pthread_cond_init(&j->done_cv, NULL);
    j->b = b;
    j->rate = rate;
    j->channels = channels;
    j->paused = true;
    j->refs = 2;
    pthread_t th;
    if (pthread_create(&th, NULL, open_job_main, j) != 0) {
        j->refs = 1;
        job_unref(j);
        return NULL;
    }
    pthread_detach(th);
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t ms = open_timeout_ms();
    ts.tv_sec += ms / 1000;
    ts.tv_nsec += (long)(ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&j->lock);
    while (!j->done && pthread_cond_timedwait(&j->done_cv, &j->lock, &ts) == 0)
        ;
    if (j->done) {
        rl_audio_out *o = j->out;
        pthread_mutex_unlock(&j->lock);
        job_unref(j);
        return o;
    }
    rl_audio_out *timing = null_open(rate, channels);
    late_out *l = timing ? calloc(1, sizeof *l) : NULL;
    if (!l) {
        if (timing)
            null_close(timing);
        j->closed = true;
        j->late = true;
        pthread_mutex_unlock(&j->lock);
        job_unref(j);
        return NULL;
    }
    j->late = true;
    j->timing = timing;
    pthread_mutex_unlock(&j->lock);
    rl_log(RL_LOG_WARN, "audio", "%s output did not open within %u ms, using the null output until it does", b->name, ms);
    l->head.b = &g_late;
    l->j = j;
    return &l->head;
}

static rl_audio_out *tap_open(const rl_audio_backend *b, uint32_t rate, uint32_t channels)
{
    rl_audio_out *in = b == &g_null_audio ? b->open(rate, channels) : open_bounded(b, rate, channels);
    if (!in)
        return NULL;
    if (!in->b)
        in->b = b;
    tap_out *t = calloc(1, sizeof *t);
    if (!t) {
        in->b->close(in);
        return NULL;
    }
    t->head.b = &g_tap;
    t->in = in;
    t->rate = rate;
    t->channels = channels;
    t->index = g_streams++;
    t->paused = true;
    t->min_queued = UINT32_MAX;
    t->opened_ns = t->last_report_ns = rl_clock_ns();
    const char *e = getenv("RELIFT_AUDIO_STATS");
    t->report = e && *e && strcmp(e, "0") != 0;
    const char *w = getenv("RELIFT_AUDIO_WAV");
    if (w && *w) {
        char path[4096];
        if (t->index)
            snprintf(path, sizeof path, "%s.%u", w, t->index);
        else
            snprintf(path, sizeof path, "%s", w);
        t->wav = fopen(path, "wb");
        if (t->wav) {
            uint8_t zero[44] = { 0 };
            fwrite(zero, 1, sizeof zero, t->wav);
            wav_header(t);
            t->report = true;
            rl_log(RL_LOG_INFO, "audio", "stream %u: recording to %s", t->index, path);
        } else {
            rl_log(RL_LOG_WARN, "audio", "cannot write %s", path);
        }
    }
    return &t->head;
}

static void tap_close(rl_audio_out *o)
{
    tap_out *t = (tap_out *)o;
    uint64_t now = rl_clock_ns();
    if (t->report)
        tap_report(t, now);
    if (t->wav) {
        wav_header(t);
        fclose(t->wav);
    }
    t->in->b->close(t->in);
    free(t);
}

static void tap_write(rl_audio_out *o, const int16_t *pcm, uint32_t frames)
{
    tap_out *t = (tap_out *)o;
    uint64_t now = rl_clock_ns();
    uint32_t q = t->in->b->held ? t->in->b->held(t->in) : t->in->b->queued(t->in);
    if (!t->paused) {
        if (t->started && q == 0) {
            t->underruns++;
            t->period_underruns++;
        }
        if (q < t->min_queued)
            t->min_queued = q;
        if (q > t->max_queued)
            t->max_queued = q;
    }
    t->in->b->write(t->in, pcm, frames);
    t->started = true;
    t->frames += frames;
    t->period_frames += frames;
    t->writes++;
    if (t->wav) {
        fwrite(pcm, sizeof *pcm * t->channels, frames, t->wav);
        t->wav_frames += frames;
        if (t->writes % 200 == 0)
            wav_header(t);
    }
    if (t->report && now - t->last_report_ns >= 10000000000ull)
        tap_report(t, now);
}

static uint32_t tap_queued(rl_audio_out *o)
{
    tap_out *t = (tap_out *)o;
    return t->in->b->queued(t->in);
}

static void tap_pause(rl_audio_out *o, bool paused)
{
    tap_out *t = (tap_out *)o;
    uint64_t now = rl_clock_ns();
    if (paused && !t->paused)
        t->active_ns += now - t->resumed_ns;
    else if (!paused && t->paused)
        t->resumed_ns = now;
    t->paused = paused;
    t->in->b->pause(t->in, paused);
}

static const rl_audio_backend g_tap = {
    .name = "tap",
    .open = NULL,
    .close = tap_close,
    .write = tap_write,
    .queued = tap_queued,
    .pause = tap_pause,
};

rl_audio_out *rl_audio_open(uint32_t rate, uint32_t channels)
{
    const rl_audio_backend *b = rl_audio_backend_current();
    rl_audio_out *o = tap_open(b, rate, channels);
    if (!o && b != &g_null_audio) {
        rl_log(RL_LOG_WARN, "audio", "%s output failed to open, using the null output", b->name);
        b = &g_null_audio;
        o = tap_open(b, rate, channels);
    }
    if (o)
        rl_log(RL_LOG_INFO, "audio", "output %s, %u Hz, %u channels", b->name, rate, channels);
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

void rl_audio_count_drop(rl_audio_out *o, uint32_t frames)
{
    if (o && o->b == &g_tap) {
        tap_out *t = (tap_out *)o;
        t->dropped += frames;
        t->period_dropped += frames;
    }
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

uint32_t rl_audio_lead_visible(uint32_t queued, uint32_t lead) { return queued > lead ? queued - lead : 0; }

uint64_t rl_audio_clock_played(rl_audio_clock *c, uint64_t consumed, uint64_t now_ns)
{
    if (c->running && now_ns > c->last_ns) {
        uint64_t t = (now_ns - c->last_ns) * c->rate + c->carry;
        c->played += t / 1000000000ull;
        c->carry = t % 1000000000ull;
    }
    c->last_ns = now_ns;
    if (c->played > consumed) {
        c->played = consumed;
        c->carry = 0;
    }
    if (consumed > c->device && c->played < consumed - c->device)
        c->played = consumed - c->device;
    return c->played;
}

void rl_audio_clock_run(rl_audio_clock *c, bool running, uint64_t consumed, uint64_t now_ns)
{
    rl_audio_clock_played(c, consumed, now_ns);
    c->running = running;
}

void rl_opus_set_backend(const rl_opus_backend *b) { g_opus = b; }

const rl_opus_backend *rl_opus_backend_current(void)
{
    if (g_opus)
        return g_opus;
#ifdef RL_HOST_OPUS
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
