/**
 * @file test_audio.c
 * @brief The host audio interface: the null output drains in real time, the
 * stub decoder returns silence of the packet's length, and packet parsing.
 */
#include "runner.h"

#include <relift/audio.h>

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
    rl_audio_pause(o, false);
    sleep_ms(50);
    uint32_t q = rl_audio_queued(o);
    CHECK(q < 4800 - 1500);
    CHECK(q > 4800 - 3600);
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
