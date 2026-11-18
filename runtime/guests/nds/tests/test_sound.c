/**
 * @file test_sound.c
 * @brief The sound controller against values worked out by hand from
 * GBATEK: the mixer's bit widths, volume, dividers and panning, the start
 * delays, one-shot and loop ends, IMA-ADPCM decoding, square waves and
 * noise, SOUNDCNT's output selection, the capture units with their rounding
 * and their two bugs, and the ARM7 timer that a streaming driver runs at
 * the channel's rate (it overflows four ticks after each sample: an ARM7
 * timer counts from the second bus cycle after its start).
 *
 * With a channel at full volume, no divider and panned hard left, and the
 * master volume at 127, a sample s reaches the left speaker as
 * floor(s / 64) above the bias, which the host gets times 64.
 */
#include "ndst.h"

#define SND(n) (0x04000400u + 16u * (n))
#define SOUNDCNT 0x04000500u
#define SOUNDBIAS 0x04000504u
#define SNDCAPCNT 0x04000508u
#define SNDCAP0DAD 0x04000510u
#define SNDCAP0LEN 0x04000514u
#define POWCNT2 0x04000304u
#define TM0 0x04000100u
#define IF 0x04000214u
#define DATA 0x02000000u
#define CAPBUF 0x02001000u

/** @brief Ticks between output samples; a channel timer of TMR_ONE overflows once per output sample. */
#define T RL_NDS_AUDIO_TICKS
#define TMR_ONE (0x10000u - T / 4u)

enum {
    VOL_MAX = 127,
    PCM8 = 0u << 29,
    PCM16 = 1u << 29,
    ADPCM = 2u << 29,
    PSG = 3u << 29,
    LOOP = 1u << 27,
    ONESHOT = 2u << 27,
    HOLD = 1u << 15,
};
#define START 0x80000000u

/** @brief SOUNDxCNT with volume vol, divider div, panning pan and the rest of the bits in rest. */
static uint32_t cnt(uint32_t vol, uint32_t div, uint32_t pan, uint32_t rest) { return vol | div << 8 | pan << 16 | rest; }

/** @brief Sets up channel n (stopped) and the master: enabled at volume 127. */
static void channel(ndst *t, unsigned n, uint32_t sad, uint16_t tmr, uint16_t pnt, uint32_t len)
{
    w7(t, SOUNDCNT, 4, 0x8000u | VOL_MAX);
    w7(t, SND(n) + 4, 4, sad);
    w7(t, SND(n) + 8, 4, tmr | (uint32_t)pnt << 16);
    w7(t, SND(n) + 12, 4, len);
}

/** @brief Runs to tick and takes every output frame made so far into out; returns how many. */
static uint32_t frames(ndst *t, uint64_t tick, int16_t *out, uint32_t max)
{
    ndst_both(t, tick);
    (void)r7(t, SOUNDCNT, 4);
    return rl_nds_audio_take(t->m, out, max);
}

/** @brief The host value of a sample played at full volume, hard left, master 127. */
static int16_t left(int32_t s) { return (int16_t)((s >> 6) * 64); }

TEST(sound_puts_out_one_frame_every_1024_arm7_cycles)
{
    ndst *t = ndst_new();
    int16_t pcm[64];
    CHECK_EQ(frames(t, T - 2, pcm, 32), 0);
    CHECK_EQ(frames(t, T, pcm, 32), 1);
    CHECK_EQ(frames(t, 10 * T + 5, pcm, 32), 9);
    for (unsigned i = 0; i < 18; i++)
        CHECK_EQ(pcm[i], 0);
}

TEST(sound_pcm16_starts_after_three_samples_and_ends_one_shot)
{
    ndst *t = ndst_new();
    static const int16_t s[8] = {0x4000, -0x4000, 0x7FFF, -0x8000, 0x1234, -0x1234, 0x0040, -0x0001};
    for (unsigned i = 0; i < 8; i++)
        w9(t, DATA + 2 * i, 2, (uint16_t)s[i]);
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | ONESHOT | START));
    int16_t pcm[2 * 16];
    CHECK_EQ(frames(t, 14 * T, pcm, 16), 14);
    for (unsigned k = 0; k < 3; k++)
        CHECK_EQ(pcm[2 * k], 0);
    for (unsigned i = 0; i < 8; i++) {
        CHECK_EQ(pcm[2 * (3 + i)], left(s[i]));
        CHECK_EQ(pcm[2 * (3 + i) + 1], 0);
    }
    CHECK_EQ(pcm[2 * 11], 0);
    CHECK_EQ(left(0x7FFF), 0x7FC0);
    CHECK_EQ(left(-0x8000), -0x8000);
    CHECK_EQ(left(0x1234), 0x1200);
}

TEST(sound_one_shot_clears_busy_at_the_start_of_the_last_sample)
{
    ndst *t = ndst_new();
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | ONESHOT | START));
    ndst_both(t, 11 * T - 2);
    CHECK(r7(t, SND(0), 4) & START);
    ndst_both(t, 11 * T);
    CHECK_EQ(r7(t, SND(0), 4), cnt(VOL_MAX, 0, 0, PCM16 | ONESHOT));
}

TEST(sound_volume_divider_panning_and_master_scale_as_the_bit_widths_say)
{
    ndst *t = ndst_new();
    for (unsigned i = 0; i < 8; i++)
        w9(t, DATA + 2 * i, 2, 0x4000);
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    w7(t, SND(0), 4, cnt(64, 2, 64, PCM16 | LOOP | START));
    int16_t pcm[2 * 8];
    CHECK_EQ(frames(t, 5 * T, pcm, 8), 5);
    CHECK_EQ(pcm[2 * 3], 0x400);
    CHECK_EQ(pcm[2 * 3 + 1], 0x400);
    w7(t, SOUNDCNT, 4, 0x8000u | 64);
    CHECK_EQ(frames(t, 6 * T, pcm, 8), 1);
    CHECK_EQ(pcm[0], 0x200);
    CHECK_EQ(pcm[1], 0x200);
    w7(t, SND(0), 4, cnt(VOL_MAX, 3, 127, PCM16 | LOOP | START));
    CHECK_EQ(frames(t, 7 * T, pcm, 8), 1);
    CHECK_EQ(pcm[0], 0);
    CHECK_EQ(pcm[1], 0x200);
    w7(t, SOUNDCNT, 4, VOL_MAX);
    CHECK_EQ(frames(t, 8 * T, pcm, 8), 1);
    CHECK_EQ(pcm[1], 0);
}

TEST(sound_master_volume_and_bias_clip_to_ten_bits)
{
    ndst *t = ndst_new();
    for (unsigned i = 0; i < 8; i++)
        w9(t, DATA + 2 * i, 2, 0x7FFF);
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    channel(t, 1, DATA, TMR_ONE, 0, 4);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | LOOP | START));
    w7(t, SND(1), 4, cnt(VOL_MAX, 0, 0, PCM16 | LOOP | START));
    w7(t, SOUNDBIAS, 4, 0x300);
    int16_t pcm[2 * 8];
    CHECK_EQ(frames(t, 5 * T, pcm, 8), 5);
    CHECK_EQ(pcm[2 * 4], 0xFF * 64);
    w7(t, SOUNDBIAS, 4, 0x100);
    CHECK_EQ(frames(t, 6 * T, pcm, 8), 1);
    CHECK_EQ(pcm[0], 0x7FFF);
    CHECK_EQ(pcm[1], 0);
    CHECK_EQ(r7(t, SOUNDBIAS, 4), 0x100);
}

TEST(sound_pcm8_loops_back_to_the_loop_start_word)
{
    ndst *t = ndst_new();
    for (unsigned i = 0; i < 16; i++)
        w9(t, DATA + i, 1, i * 8u);
    channel(t, 0, DATA, TMR_ONE, 1, 3);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM8 | LOOP | START));
    int16_t pcm[2 * 40];
    CHECK_EQ(frames(t, 3 * T + 36 * T, pcm, 40), 39);
    for (unsigned i = 0; i < 16; i++)
        CHECK_EQ(pcm[2 * (3 + i)], left((int32_t)(i * 8u) << 8));
    for (unsigned i = 0; i < 12; i++)
        CHECK_EQ(pcm[2 * (19 + i)], left((int32_t)((4 + i) * 8u) << 8));
    CHECK(r7(t, SND(0), 4) & START);
}

TEST(sound_too_short_a_sound_stays_busy_and_silent)
{
    ndst *t = ndst_new();
    for (unsigned i = 0; i < 16; i++)
        w9(t, DATA + i, 1, 0x40);
    channel(t, 0, DATA, TMR_ONE, 1, 2);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM8 | ONESHOT | START));
    int16_t pcm[2 * 40];
    CHECK_EQ(frames(t, 30 * T, pcm, 40), 30);
    for (unsigned i = 0; i < 60; i++)
        CHECK_EQ(pcm[i], 0);
    CHECK(r7(t, SND(0), 4) & START);
}

/** @brief Captures channel 0 (capture 0 from ch(a), PCM16) at channel 1's timer, which is set to channel 0's. */
static void capture_channel0(ndst *t, uint16_t tmr, uint32_t words)
{
    w7(t, SND(1) + 8, 4, tmr);
    w7(t, SNDCAP0DAD, 4, CAPBUF);
    w7(t, SNDCAP0LEN, 4, words);
}

TEST(sound_adpcm_decodes_as_gbatek_rounds_and_restarts_after_eleven_samples)
{
    ndst *t = ndst_new();
    w9(t, DATA, 4, 0x2000u | 60u << 16);
    w9(t, DATA + 4, 1, 0xC7);
    w9(t, DATA + 5, 1, 0x83);
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    capture_channel0(t, TMR_ONE, 8);
    w7(t, SNDCAPCNT, 1, 0x80 | 0x02);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, ADPCM | ONESHOT | START));
    ndst_both(t, 16 * T);
    (void)r7(t, SOUNDCNT, 4);
    for (unsigned i = 0; i < 11; i++)
        CHECK_EQ(r9(t, CAPBUF + 2 * i, 2), 0);
    CHECK_EQ(r9(t, CAPBUF + 22, 2), 12452);
    CHECK_EQ(r9(t, CAPBUF + 24, 2), 6973);
    CHECK_EQ(r9(t, CAPBUF + 26, 2), 12129);
    CHECK_EQ(r9(t, CAPBUF + 28, 2), 11460);
}

TEST(sound_square_wave_starts_low_after_one_sample)
{
    ndst *t = ndst_new();
    channel(t, 8, 0, TMR_ONE, 0, 0);
    w7(t, SND(8), 4, cnt(VOL_MAX, 0, 0, PSG | 3u << 24 | START));
    int16_t pcm[2 * 20];
    CHECK_EQ(frames(t, 17 * T, pcm, 20), 17);
    CHECK_EQ(pcm[0], 0);
    for (unsigned k = 0; k < 16; k++)
        CHECK_EQ(pcm[2 * (1 + k)], (k & 7) < 4 ? left(-0x7FFF) : left(0x7FFF));
}

TEST(sound_duty_7_is_low_all_the_time_and_duty_0_one_eighth_high)
{
    ndst *t = ndst_new();
    channel(t, 9, 0, TMR_ONE, 0, 0);
    channel(t, 10, 0, TMR_ONE, 0, 0);
    w7(t, SND(9), 4, cnt(VOL_MAX, 0, 0, PSG | 7u << 24 | START));
    w7(t, SND(10), 4, cnt(VOL_MAX, 0, 127, PSG | 0u << 24 | START));
    int16_t pcm[2 * 20];
    CHECK_EQ(frames(t, 9 * T, pcm, 20), 9);
    for (unsigned k = 0; k < 8; k++) {
        CHECK_EQ(pcm[2 * (1 + k)], left(-0x7FFF));
        CHECK_EQ(pcm[2 * (1 + k) + 1], k == 7 ? left(0x7FFF) : left(-0x7FFF));
    }
}

TEST(sound_noise_follows_the_15_bit_generator_from_7fff)
{
    ndst *t = ndst_new();
    static const char seq[] = "LLLLLLLLLLLLLLHH";
    channel(t, 14, 0, TMR_ONE, 0, 0);
    w7(t, SND(14), 4, cnt(VOL_MAX, 0, 0, PSG | START));
    int16_t pcm[2 * 20];
    CHECK_EQ(frames(t, 17 * T, pcm, 20), 17);
    for (unsigned k = 0; k < 16; k++)
        CHECK_EQ(pcm[2 * (1 + k)], seq[k] == 'H' ? left(0x7FFF) : left(-0x7FFF));
}

TEST(sound_psg_format_on_a_low_channel_is_silent)
{
    ndst *t = ndst_new();
    channel(t, 2, 0, TMR_ONE, 0, 0);
    w7(t, SND(2), 4, cnt(VOL_MAX, 0, 0, PSG | 3u << 24 | START));
    int16_t pcm[2 * 20];
    CHECK_EQ(frames(t, 12 * T, pcm, 20), 12);
    for (unsigned i = 0; i < 24; i++)
        CHECK_EQ(pcm[i], 0);
}

TEST(sound_hold_keeps_the_last_sample_and_the_first_period_of_a_restart)
{
    ndst *t = ndst_new();
    for (unsigned i = 0; i < 8; i++)
        w9(t, DATA + 2 * i, 2, 0x4000);
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | ONESHOT | HOLD | START));
    int16_t pcm[2 * 20];
    CHECK_EQ(frames(t, 16 * T, pcm, 20), 16);
    for (unsigned k = 11; k < 16; k++)
        CHECK_EQ(pcm[2 * k], left(0x4000));
    ndst_both(t, 16 * T + T / 2);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | ONESHOT | HOLD | START));
    CHECK_EQ(frames(t, 21 * T, pcm, 20), 5);
    CHECK_EQ(pcm[0], left(0x4000));
    CHECK_EQ(pcm[2], 0);
    CHECK_EQ(pcm[4], 0);
    CHECK_EQ(pcm[6], 0);
    CHECK_EQ(pcm[8], left(0x4000));
}

TEST(sound_stop_silences_at_once_and_speaker_power_mutes)
{
    ndst *t = ndst_new();
    for (unsigned i = 0; i < 8; i++)
        w9(t, DATA + 2 * i, 2, 0x4000);
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | LOOP | START));
    int16_t pcm[2 * 20];
    CHECK_EQ(frames(t, 5 * T, pcm, 20), 5);
    CHECK_EQ(pcm[8], left(0x4000));
    w7(t, POWCNT2, 4, 0);
    CHECK_EQ(frames(t, 6 * T, pcm, 20), 1);
    CHECK_EQ(pcm[0], 0);
    w7(t, POWCNT2, 4, 1);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | LOOP));
    CHECK_EQ(frames(t, 7 * T, pcm, 20), 1);
    CHECK_EQ(pcm[0], 0);
}

TEST(sound_soundcnt_routes_channels_1_and_3_around_the_mixer)
{
    ndst *t = ndst_new();
    for (unsigned i = 0; i < 8; i++)
        w9(t, DATA + 2 * i, 2, 0x4000);
    for (unsigned i = 0; i < 8; i++)
        w9(t, DATA + 0x100 + 2 * i, 2, 0x1000);
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    channel(t, 1, DATA + 0x100, TMR_ONE, 0, 4);
    channel(t, 3, DATA + 0x100, TMR_ONE, 0, 4);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | LOOP | START));
    w7(t, SND(1), 4, cnt(VOL_MAX, 0, 0, PCM16 | LOOP | START));
    w7(t, SND(3), 4, cnt(VOL_MAX, 0, 127, PCM16 | LOOP | START));
    int16_t pcm[2 * 20];
    CHECK_EQ(frames(t, 5 * T, pcm, 20), 5);
    CHECK_EQ(pcm[8], left(0x5000));
    CHECK_EQ(pcm[9], left(0x1000));
    w7(t, SOUNDCNT, 4, 0x8000u | VOL_MAX | 1u << 8 | 3u << 10 | 0x1000);
    CHECK_EQ(frames(t, 6 * T, pcm, 20), 1);
    CHECK_EQ(pcm[0], left(0x1000));
    CHECK_EQ(pcm[1], left(0x1000));
    w7(t, SOUNDCNT, 4, 0x8000u | VOL_MAX | 0x1000 | 0x2000);
    CHECK_EQ(frames(t, 7 * T, pcm, 20), 1);
    CHECK_EQ(pcm[0], left(0x4000));
    CHECK_EQ(pcm[1], 0);
}

TEST(sound_capture_of_the_mixer_rounds_a_negative_half_toward_zero)
{
    ndst *t = ndst_new();
    static const int16_t s[8] = {3, -3, 2, -2, 0x100, -0x100, 5, -5};
    for (unsigned i = 0; i < 8; i++)
        w9(t, DATA + 2 * i, 2, (uint16_t)s[i]);
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    capture_channel0(t, TMR_ONE, 16);
    w7(t, SNDCAPCNT, 1, 0x80);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 64, PCM16 | LOOP | START));
    ndst_both(t, 12 * T);
    (void)r7(t, SOUNDCNT, 4);
    static const int16_t want[8] = {1, -1, 1, -1, 0x80, -0x80, 2, -2};
    for (unsigned i = 0; i < 8; i++)
        CHECK_EQ((int16_t)r9(t, CAPBUF + 2 * (3 + i), 2), want[i]);
}

TEST(sound_capture_pcm8_takes_the_high_byte)
{
    ndst *t = ndst_new();
    static const int16_t s[8] = {0x4000, -0x4000, 0x7FFF, -0x8000, 0x0080, -0x0080, 0x00FF, -0x0181};
    for (unsigned i = 0; i < 8; i++)
        w9(t, DATA + 2 * i, 2, (uint16_t)s[i]);
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    capture_channel0(t, TMR_ONE, 4);
    w7(t, SNDCAPCNT, 1, 0x80 | 0x08 | 0x02);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | LOOP | START));
    ndst_both(t, 12 * T);
    (void)r7(t, SOUNDCNT, 4);
    static const uint8_t want[8] = {0x40, 0xC0, 0x7F, 0x80, 0x00, 0x00, 0x00, 0xFE};
    for (unsigned i = 0; i < 8; i++)
        CHECK_EQ(r9(t, CAPBUF + 3 + i, 1), want[i]);
}

TEST(sound_capture_of_channel_0_shows_both_bugs)
{
    ndst *t = ndst_new();
    for (unsigned i = 0; i < 8; i++) {
        w9(t, DATA + 2 * i, 2, i < 4 ? 0x7FFF : 0xC000);
        w9(t, DATA + 0x100 + 2 * i, 2, i < 4 ? 0x7FFF : 0xF000);
    }
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    channel(t, 1, DATA + 0x100, TMR_ONE, 0, 4);
    capture_channel0(t, TMR_ONE, 16);
    w7(t, SND(1), 4, cnt(VOL_MAX, 0, 0, PCM16 | ONESHOT | START));
    w7(t, SNDCAPCNT, 1, 0x80 | 0x02 | 0x01);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | ONESHOT | START));
    ndst_both(t, 12 * T);
    (void)r7(t, SOUNDCNT, 4);
    CHECK_EQ(r9(t, CAPBUF + 6, 2), 0xFFFE);
    CHECK_EQ(r9(t, CAPBUF + 14, 2), 0xB000);
    w7(t, SNDCAPCNT, 1, 0);
    w7(t, SND(0), 4, 0);
    w7(t, SND(1), 4, 0);
    w7(t, SND(1), 4, cnt(VOL_MAX, 0, 0, PCM16 | ONESHOT | START));
    w7(t, SNDCAPCNT, 1, 0x80 | 0x02);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | ONESHOT | START));
    ndst_both(t, 24 * T + 64);
    (void)r7(t, SOUNDCNT, 4);
    CHECK_EQ(r9(t, CAPBUF + 6, 2), 0x7FFF);
    CHECK_EQ(r9(t, CAPBUF + 14, 2), 0x8000);
}

TEST(sound_one_shot_capture_stops_and_a_loop_wraps)
{
    ndst *t = ndst_new();
    for (unsigned i = 0; i < 8; i++)
        w9(t, DATA + 2 * i, 2, 0x1000);
    for (unsigned i = 0; i < 8; i++)
        w9(t, CAPBUF + 4 * i, 4, 0xDEADBEEFu);
    channel(t, 0, DATA, TMR_ONE, 0, 4);
    capture_channel0(t, TMR_ONE, 2);
    w7(t, SNDCAPCNT, 1, 0x80 | 0x04);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | LOOP | START));
    ndst_both(t, 4 * T - 2);
    CHECK_EQ(r7(t, SNDCAPCNT, 1), 0x84);
    ndst_both(t, 4 * T);
    CHECK_EQ(r7(t, SNDCAPCNT, 1), 0x04);
    CHECK_EQ(r9(t, CAPBUF + 8, 4), 0xDEADBEEFu);
    w7(t, SNDCAPCNT, 1, 0x80 | 0x00);
    ndst_both(t, 10 * T);
    CHECK_EQ(r7(t, SNDCAPCNT, 1), 0x80);
    CHECK_EQ(r9(t, CAPBUF, 4), 0x10001000u);
    CHECK_EQ(r9(t, CAPBUF + 4, 4), 0x10001000u);
    CHECK_EQ(r9(t, CAPBUF + 8, 4), 0xDEADBEEFu);
}

TEST(sound_arm7_timer_at_twice_the_reload_keeps_pace_with_a_channel)
{
    ndst *t = ndst_new();
    for (unsigned i = 0; i < 16; i++)
        w9(t, DATA + 2 * i, 2, (uint16_t)(0x100 * (i + 1)));
    channel(t, 0, DATA, TMR_ONE, 0, 8);
    w7(t, TM0, 4, (0x10000u - T / 2u) | 0x00C00000u);
    w7(t, SND(0), 4, cnt(VOL_MAX, 0, 0, PCM16 | LOOP | START));
    for (unsigned k = 1; k <= 12; k++) {
        ndst_both(t, (uint64_t)k * T + 2);
        CHECK_EQ(r7(t, IF, 4) & RL_NDS_IRQ_TIMER0, 0);
        ndst_both(t, (uint64_t)k * T + 6);
        CHECK_EQ(r7(t, IF, 4) & RL_NDS_IRQ_TIMER0, RL_NDS_IRQ_TIMER0);
        w7(t, IF, 4, RL_NDS_IRQ_TIMER0);
    }
    int16_t pcm[2 * 16];
    (void)r7(t, SOUNDCNT, 4);
    CHECK_EQ(rl_nds_audio_take(t->m, pcm, 16), 12);
    for (unsigned k = 3; k < 12; k++)
        CHECK_EQ(pcm[2 * k], left((int32_t)(0x100 * (k - 2))));
}
