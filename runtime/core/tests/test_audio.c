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

static uint64_t g_fake_frames, g_fake_pauses, g_fake_closed;

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
    sleep_ms(600);
    CHECK_EQ(g_fake_pauses, 1);
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
    sleep_ms(600);
    CHECK_EQ(g_fake_closed, 1);
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
