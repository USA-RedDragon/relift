/**
 * @file test_audio.c
 * @brief The host audio interface: the null output drains in real time, the
 * stub decoder returns silence of the packet's length, and packet parsing.
 */
#include "runner.h"

#include <relift/audio.h>

#include "../core.h"

#include <stdlib.h>
#include <time.h>

static void sleep_ms(long ms)
{
    struct timespec ts = { 0, ms * 1000000L };
    nanosleep(&ts, NULL);
}

TEST(audio_null_output_drains_at_its_rate)
{
    rl_audio_set_backend(rl_audio_null_backend());
    rl_audio_out *o = rl_audio_open(48000, 2);
    CHECK(o != NULL);
    int16_t pcm[2 * 4800] = { 0 };
    rl_audio_write(o, pcm, 4800);
    CHECK_EQ(rl_audio_queued(o), 4800);
    sleep_ms(50);
    CHECK_EQ(rl_audio_queued(o), 4800);
    uint64_t t_before = rl_clock_ns();
    rl_audio_pause(o, false);
    uint64_t t_after = rl_clock_ns();
    uint64_t t_read;
    do {
        sleep_ms(5);
        t_read = rl_clock_ns();
    } while (t_read - t_after < 20000000ull);
    uint32_t q = rl_audio_queued(o);
    uint64_t t_done = rl_clock_ns();
    uint64_t lo_frames = (t_read - t_after) * 48000ull / 1000000000ull;
    uint64_t hi_frames = (t_done - t_before) * 48000ull / 1000000000ull + 1;
    if (hi_frames > 4800)
        hi_frames = 4800;
    CHECK(4800 - q + 1 >= lo_frames);
    CHECK(4800 - q <= hi_frames);
    rl_audio_close(o);
}

TEST(audio_packet_frames_follow_the_toc)
{
    const uint8_t celt20[] = { 0xF8, 0, 0 };
    const uint8_t silk60x2[] = { 0x19, 0, 0 };
    const uint8_t code3[] = { 0x7B, 0x03, 0 };
    CHECK_EQ(rl_opus_packet_frames(celt20, sizeof celt20, 48000), 960);
    CHECK_EQ(rl_opus_packet_frames(silk60x2, sizeof silk60x2, 48000), 2880 * 2);
    CHECK_EQ(rl_opus_packet_frames(celt20, sizeof celt20, 24000), 480);
    CHECK_EQ(rl_opus_packet_frames(code3, sizeof code3, 48000), 3 * 960);
    CHECK_EQ(rl_opus_packet_frames(celt20, 0, 48000), (unsigned long long)(int64_t)RL_OPUS_BAD_ARG);
}

TEST(audio_stub_decoder_returns_silence)
{
    rl_opus_set_backend(rl_opus_stub_backend());
    rl_opus *d = rl_opus_open(48000, 2, 0, 0, NULL);
    CHECK(d != NULL);
    const uint8_t pkt[] = { 0xF8, 1, 2, 3 };
    int16_t pcm[2 * 960];
    for (int i = 0; i < 2 * 960; i++)
        pcm[i] = 7;
    CHECK_EQ(rl_opus_decode(d, pkt, sizeof pkt, pcm, 960), 960);
    CHECK_EQ(pcm[0] | pcm[1919], 0);
    CHECK_EQ((int64_t)rl_opus_decode(d, pkt, sizeof pkt, pcm, 480), (unsigned long long)(int64_t)RL_OPUS_BUFFER_TOO_SMALL);
    rl_opus_close(d);
}

static rl_audio_out *hang_open(uint32_t rate, uint32_t channels)
{
    (void)rate;
    (void)channels;
    for (;;)
        sleep_ms(1000);
}

static const rl_audio_backend g_hang = { .name = "hang", .open = hang_open };

TEST(audio_open_survives_a_backend_that_never_returns)
{
    setenv("RELIFT_AUDIO_OPEN_MS", "200", 1);
    rl_audio_set_backend(&g_hang);
    uint64_t t0 = rl_clock_ns();
    rl_audio_out *o = rl_audio_open(48000, 2);
    uint64_t took = rl_clock_ns() - t0;
    CHECK(o != NULL);
    CHECK(o->b != &g_hang);
    CHECK(took < 5000000000ull);
    int16_t pcm[2 * 480] = { 0 };
    rl_audio_write(o, pcm, 480);
    CHECK_EQ(rl_audio_queued(o), 480);
    rl_audio_close(o);
}

static _Atomic uint64_t g_fake_frames, g_fake_pauses, g_fake_closed;

/** Waits up to 10 s for a counter the open helper thread sets; the helper's
 * wake-up after the backend's 500 ms open has no bound on a loaded host. */
static uint64_t wait_for(_Atomic uint64_t *counter, uint64_t want)
{
    for (int i = 0; i < 1000 && *counter < want; i++)
        sleep_ms(10);
    return *counter;
}

static rl_audio_out *slow_open(uint32_t rate, uint32_t channels)
{
    (void)rate;
    (void)channels;
    sleep_ms(500);
    rl_audio_out *o = calloc(1, sizeof *o);
    return o;
}

static void slow_close(rl_audio_out *o)
{
    g_fake_closed++;
    free(o);
}

static void slow_write(rl_audio_out *o, const int16_t *pcm, uint32_t frames)
{
    (void)o;
    (void)pcm;
    g_fake_frames += frames;
}

static uint32_t slow_queued(rl_audio_out *o)
{
    (void)o;
    return 0;
}

static void slow_pause(rl_audio_out *o, bool paused)
{
    (void)o;
    (void)paused;
    g_fake_pauses++;
}

static const rl_audio_backend g_slow = {
    .name = "slow",
    .open = slow_open,
    .close = slow_close,
    .write = slow_write,
    .queued = slow_queued,
    .pause = slow_pause,
};

TEST(audio_late_open_switches_the_stream_to_the_host_backend)
{
    setenv("RELIFT_AUDIO_OPEN_MS", "200", 1);
    rl_audio_set_backend(&g_slow);
    rl_audio_out *o = rl_audio_open(48000, 2);
    CHECK(o != NULL);
    int16_t pcm[2 * 480] = { 0 };
    rl_audio_pause(o, false);
    rl_audio_write(o, pcm, 480);
    CHECK_EQ(g_fake_frames, 0);
    CHECK_EQ(rl_audio_queued(o) > 0, 1);
    CHECK_EQ(wait_for(&g_fake_pauses, 1), 1);
    rl_audio_write(o, pcm, 480);
    CHECK_EQ(g_fake_frames, 480);
    CHECK_EQ(rl_audio_queued(o) > 0 && rl_audio_queued(o) <= 480, 1);
    rl_audio_close(o);
    CHECK_EQ(g_fake_closed, 1);
}

TEST(audio_late_open_after_close_is_closed_by_the_helper)
{
    setenv("RELIFT_AUDIO_OPEN_MS", "100", 1);
    rl_audio_set_backend(&g_slow);
    rl_audio_out *o = rl_audio_open(48000, 2);
    CHECK(o != NULL);
    rl_audio_close(o);
    CHECK_EQ(wait_for(&g_fake_closed, 1), 1);
    CHECK_EQ(g_fake_frames, 0);
}

TEST(audio_lead_is_not_seen_by_the_guest)
{
    CHECK_EQ(rl_audio_lead_visible(2704, 1264), 1440);
    CHECK_EQ(rl_audio_lead_visible(1264, 1264), 0);
    CHECK_EQ(rl_audio_lead_visible(700, 1264), 0);
    CHECK_EQ(rl_audio_lead_visible(0, 1264), 0);
    CHECK_EQ(rl_audio_lead_visible(1440, 0), 1440);
}

TEST(audio_clock_plays_between_device_pulls)
{
    rl_audio_clock c = { .rate = 48000, .device = 2048 };
    rl_audio_clock_run(&c, true, 0, 1000000000ull);
    CHECK_EQ(rl_audio_clock_played(&c, 2048, 1000000000ull), 0);
    CHECK_EQ(rl_audio_clock_played(&c, 2048, 1010000000ull), 480);
    CHECK_EQ(rl_audio_clock_played(&c, 2048, 1020000000ull), 960);
    CHECK_EQ(rl_audio_clock_played(&c, 2048, 1060000000ull), 2048);
    CHECK_EQ(rl_audio_clock_played(&c, 6144, 1061000000ull), 4096);
    rl_audio_clock_run(&c, false, 6144, 1062000000ull);
    CHECK_EQ(rl_audio_clock_played(&c, 6144, 1100000000ull), 4144);
    rl_audio_clock_run(&c, true, 6144, 1100000000ull);
    CHECK_EQ(rl_audio_clock_played(&c, 6144, 1100021000ull), 4145);
}

typedef struct drift_run {
    uint32_t underruns;
    double min_held, max_held, ratio;
} drift_run;

/**
 * A guest writes 5 ms of frames on the system clock; a device off by ppm takes
 * device frames a pull at the steered ratio and is seen up to 1 ms late.
 */
static drift_run run_drift(double ppm, uint32_t device, uint32_t seconds)
{
    rl_audio_drift d = { .rate = 48000 };
    drift_run r = { 0, 1e18, 0, 1.0 };
    double held = device + 240, asked = 0, ratio = 1.0, next_pull = 0;
    double pull_ns = device * 1e9 / (48000.0 * (1.0 + ppm / 1e6));
    uint64_t end = (uint64_t)seconds * 1000000000ull;
    uint32_t seed = 12345;
    for (uint64_t now = 0; now < end;) {
        if (next_pull <= (double)now) {
            seed = seed * 1664525u + 1013904223u;
            ratio = rl_audio_drift_ratio(&d, (uint64_t)asked, (uint64_t)next_pull + (seed >> 12) % 1000000u);
            double need = device * ratio;
            asked += need;
            if (held < need) {
                r.underruns++;
                held = 0;
            } else {
                held -= need;
            }
            if (now >= 60000000000ull && held < r.min_held)
                r.min_held = held;
            next_pull += pull_ns;
            continue;
        }
        held += 240;
        if (now >= 60000000000ull && held > r.max_held)
            r.max_held = held;
        now += 5000000;
    }
    r.ratio = ratio;
    return r;
}

TEST(audio_drift_holds_the_queue_for_an_hour)
{
    static const double ppm[] = { 300, -300, 0 };
    static const uint32_t device[] = { 960, 1024, 2048, 4096 };
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 4; k++) {
            drift_run r = run_drift(ppm[i], device[k], 3600);
            CHECK_EQ(r.underruns, 0);
            CHECK(r.min_held > 60);
            CHECK(r.max_held < device[k] + 240 + 480);
            CHECK(r.ratio > 1.0 - ppm[i] / 1e6 - 20e-6 && r.ratio < 1.0 - ppm[i] / 1e6 + 20e-6);
        }
}

TEST(audio_drift_leaves_out_a_pause)
{
    rl_audio_drift d = { .rate = 48000 };
    uint64_t asked = 0, now = 0;
    for (int i = 0; i < 2000; i++, now += 20000000, asked += 960)
        rl_audio_drift_ratio(&d, asked, now);
    CHECK_EQ(rl_audio_drift_ratio(&d, asked, now), 1.0);
    rl_audio_drift_pause(&d);
    now += 100000000;
    for (int i = 0; i < 2000; i++, now += 20000000, asked += 960)
        rl_audio_drift_ratio(&d, asked, now);
    CHECK_EQ(rl_audio_drift_ratio(&d, asked, now), 1.0);
}

TEST(audio_drift_stays_within_its_bound)
{
    drift_run r = run_drift(16000, 2048, 120);
    CHECK_EQ(r.ratio, 1.0 - RL_AUDIO_DRIFT_BOUND);
    CHECK(r.underruns > 0);
    r = run_drift(-16000, 2048, 120);
    CHECK_EQ(r.ratio, 1.0 + RL_AUDIO_DRIFT_BOUND);
}
