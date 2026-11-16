/**
 * @file psg_test.c
 * @brief The SN76489: registers and latch, tone and noise behavior, catch-up
 * timing, the Game Gear stereo mask, the sample buffer, save states, and one
 * fixed script whose attenuations hash to the value ares produces.
 */
#include "psg.h"
#include "runner.h"

static sms_psg dev;

static sms_psg_sample take(void)
{
    sms_psg_sample s = {{15, 15, 15, 15}, -1.0, -1.0};
    sms_psg_drain(&dev, &s, 1);
    return s;
}

TEST(reset_is_silent_with_the_lfsr_seeded)
{
    sms_psg_reset(&dev, false);
    CHECK_EQ(dev.s.noise.lfsr, 0x8000);
    CHECK_EQ(dev.s.stereo, 0xFF);
    CHECK_EQ(sms_psg_next(&dev), 16);
    sms_psg_catch_up(&dev, 15);
    CHECK_EQ(dev.count, 0);
    sms_psg_catch_up(&dev, 16);
    CHECK_EQ(dev.count, 1);
    sms_psg_sample s = take();
    CHECK_EQ(s.att[0] & s.att[1] & s.att[2] & s.att[3], 15);
    CHECK(s.left == 0.0 && s.right == 0.0);
}

TEST(tone_writes_latch_and_combine_pitch_halves)
{
    sms_psg_reset(&dev, false);
    sms_psg_write(&dev, 0, 0x85);
    CHECK_EQ(dev.s.tone[0].pitch, 5);
    sms_psg_write(&dev, 0, 0x12);
    CHECK_EQ(dev.s.tone[0].pitch, (0x12u << 4) | 5);
    sms_psg_write(&dev, 0, 0xC3);
    sms_psg_write(&dev, 0, 0x3F);
    CHECK_EQ(dev.s.tone[2].pitch, 0x3F3);
    CHECK_EQ(dev.s.noise.pitch, 0x3F3);
    sms_psg_write(&dev, 0, 0xB7);
    CHECK_EQ(dev.s.tone[1].volume, 7);
    sms_psg_write(&dev, 0, 0x33);
    CHECK_EQ(dev.s.tone[1].volume, 7);
    CHECK_EQ(dev.s.tone[1].pitch, 0);
}

TEST(a_noise_write_reseeds_the_register)
{
    sms_psg_reset(&dev, false);
    dev.s.noise.lfsr = 0x1234;
    sms_psg_write(&dev, 0, 0xE6);
    CHECK_EQ(dev.s.noise.rate, 2);
    CHECK_EQ(dev.s.noise.enable, 1);
    CHECK_EQ(dev.s.noise.lfsr, 0x8000);
    dev.s.noise.lfsr = 0x1234;
    sms_psg_write(&dev, 0, 0x01);
    CHECK_EQ(dev.s.noise.rate, 1);
    CHECK_EQ(dev.s.noise.enable, 0);
    CHECK_EQ(dev.s.noise.lfsr, 0x8000);
}

TEST(a_tone_with_period_one_flips_every_sample)
{
    sms_psg_reset(&dev, false);
    sms_psg_write(&dev, 0, 0x81);
    sms_psg_write(&dev, 0, 0x90);
    sms_psg_catch_up(&dev, 16 * 8);
    unsigned level = 0;
    for (unsigned i = 0; i < 8; i++) {
        sms_psg_sample s = take();
        unsigned on = s.att[0] == 0;
        CHECK_EQ(on, (i + 1) & 1);
        level += on;
        CHECK(s.left == (on ? 0.25 : 0.0));
        CHECK(s.right == s.left);
    }
    CHECK_EQ(level, 4);
}

TEST(a_write_is_heard_only_by_samples_completed_after_it)
{
    sms_psg_reset(&dev, false);
    sms_psg_write(&dev, 0, 0x81);
    sms_psg_write(&dev, 32, 0x90);
    sms_psg_catch_up(&dev, 64);
    CHECK_EQ(dev.count, 4);
    CHECK_EQ(take().att[0], 15);
    CHECK_EQ(take().att[0], 15);
    CHECK_EQ(take().att[0], 0);
    CHECK_EQ(take().att[0], 15);
}

TEST(attenuation_steps_are_two_decibels)
{
    sms_psg_reset(&dev, false);
    CHECK(dev.level[0] == 1.0);
    CHECK(dev.level[15] == 0.0);
    CHECK(dev.level[3] > 0.49 && dev.level[3] < 0.51);
    CHECK(dev.level[6] > 0.24 && dev.level[6] < 0.26);
}

TEST(game_gear_stereo_masks_channels_per_side)
{
    sms_psg_reset(&dev, true);
    sms_psg_write(&dev, 0, 0x81);
    sms_psg_write(&dev, 0, 0x90);
    sms_psg_stereo(&dev, 0, 0x10);
    sms_psg_catch_up(&dev, 32);
    sms_psg_sample s = take();
    CHECK(s.left == 0.25);
    CHECK(s.right == 0.0);
    take();
    sms_psg_stereo(&dev, 48, 0x01);
    sms_psg_catch_up(&dev, 64);
    s = take();
    CHECK(s.left == 0.25 || s.left == 0.0);
    CHECK(s.right == 0.0);
    s = take();
    CHECK(s.left == 0.0);
    CHECK(s.right == 0.0);
    sms_psg_stereo(&dev, 80, 0x01);
    sms_psg_catch_up(&dev, 96);
    s = take();
    CHECK(s.left == 0.0);
    CHECK(s.right == 0.25);
}

TEST(a_master_system_ignores_the_stereo_port)
{
    sms_psg_reset(&dev, false);
    sms_psg_stereo(&dev, 0, 0x00);
    CHECK_EQ(dev.s.stereo, 0xFF);
}

TEST(a_full_buffer_drops_the_oldest_samples)
{
    sms_psg_reset(&dev, false);
    sms_psg_catch_up(&dev, 16ull * (SMS_PSG_BUF + 10));
    CHECK_EQ(dev.count, SMS_PSG_BUF);
    CHECK_EQ(dev.dropped, 10);
    CHECK_EQ(dev.s.samples, SMS_PSG_BUF + 10);
}

TEST(save_and_restore_replay_the_same_samples)
{
    sms_psg_reset(&dev, false);
    sms_psg_write(&dev, 0, 0x8A);
    sms_psg_write(&dev, 0, 0x03);
    sms_psg_write(&dev, 0, 0x90);
    sms_psg_write(&dev, 0, 0xE5);
    sms_psg_write(&dev, 0, 0xF2);
    sms_psg_catch_up(&dev, 1000);
    sms_psg_state saved;
    sms_psg_save(&dev, &saved);
    sms_psg_sample a[200];
    sms_psg_sample b[200];
    sms_psg_drain(&dev, a, 200);
    sms_psg_catch_up(&dev, 1000 + 16 * 200);
    CHECK_EQ(sms_psg_drain(&dev, a, 200), 200);
    sms_psg_restore(&dev, &saved);
    CHECK_EQ(dev.count, 0);
    sms_psg_catch_up(&dev, 1000 + 16 * 200);
    CHECK_EQ(sms_psg_drain(&dev, b, 200), 200);
    for (unsigned i = 0; i < 200; i++) {
        for (unsigned c = 0; c < 4; c++)
            CHECK_EQ(a[i].att[c], b[i].att[c]);
        CHECK(a[i].left == b[i].left);
    }
}

static const struct {
    uint64_t t;
    uint8_t v;
} vec[] = {
    {0, 0x9F},    {0, 0xBF},    {0, 0xDF},    {0, 0xFF},    {10, 0x8A},   {10, 0x05},   {10, 0x90},
    {20, 0xA3},   {20, 0x02},   {20, 0xB2},   {40, 0xC1},   {40, 0x01},   {40, 0xD4},   {100, 0xE7},
    {100, 0xF1},  {900, 0xE4},  {900, 0xF3},  {2500, 0xE5}, {2500, 0xF0}, {2500, 0xC5}, {3000, 0xE3},
    {4500, 0x9F}, {4500, 0xFF}, {6000, 0xB8}, {7000, 0x84}, {7000, 0x30}, {7000, 0x90},
};

TEST(the_fixed_script_matches_the_ares_attenuations)
{
    sms_psg_reset(&dev, false);
    uint64_t hash = 0xcbf29ce484222325ull;
    uint64_t n = 0;
    sms_psg_sample s[64];
    for (unsigned i = 0; i <= sizeof vec / sizeof vec[0]; i++) {
        if (i < sizeof vec / sizeof vec[0])
            sms_psg_write(&dev, vec[i].t, vec[i].v);
        else
            sms_psg_catch_up(&dev, 12000);
        size_t k;
        while ((k = sms_psg_drain(&dev, s, 64)) != 0) {
            for (size_t j = 0; j < k; j++, n++)
                for (unsigned c = 0; c < 4; c++)
                    hash = (hash ^ s[j].att[c]) * 0x100000001b3ull;
        }
    }
    CHECK_EQ(n, 750);
    CHECK_EQ(hash, 0xdbfb995ee6250befull);
}
