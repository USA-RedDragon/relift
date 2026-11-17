/**
 * @file ym2413_test.c
 * @brief The YM2413: silence after reset, catch-up timing, key on, and one
 * fixed script (a melodic voice, then rhythm mode) whose samples hash to the
 * value ares produces.
 */
#include "runner.h"
#include "ym2413.h"

static sms_ym2413 dev;

static void reg(uint64_t t, uint8_t a, uint8_t v)
{
    sms_ym2413_address(&dev, t, a);
    sms_ym2413_write(&dev, t, v);
}

TEST(ym2413_reset_is_silent_and_makes_a_sample_every_72_clocks)
{
    sms_ym2413_reset(&dev);
    sms_ym2413_catch_up(&dev, 71);
    CHECK_EQ(dev.count, 0);
    sms_ym2413_catch_up(&dev, 72 * 100);
    CHECK_EQ(dev.count, 100);
    int32_t s[100];
    CHECK_EQ(sms_ym2413_drain(&dev, s, 100), 100);
    for (int i = 0; i < 100; i++)
        CHECK_EQ(s[i], 0);
}

TEST(ym2413_a_keyed_voice_sounds)
{
    sms_ym2413_reset(&dev);
    reg(0, 0x30, 0x10);
    reg(0, 0x10, 0xAC);
    reg(0, 0x20, 0x17);
    sms_ym2413_catch_up(&dev, 72 * 2000);
    int32_t s[2000];
    size_t n = sms_ym2413_drain(&dev, s, 2000);
    CHECK_EQ(n, 2000);
    int nonzero = 0;
    for (size_t i = 0; i < n; i++)
        nonzero += s[i] != 0;
    CHECK(nonzero > 0);
}

TEST(ym2413_the_fixed_script_matches_ares)
{
    sms_ym2413_reset(&dev);
    reg(0, 48, 49);
    reg(100, 16, 172);
    reg(200, 32, 23);
    reg(300, 14, 32);
    reg(400, 54, 1);
    reg(500, 14, 63);
    uint32_t h = 2166136261u;
    size_t total = 0, nonzero = 0;
    for (uint64_t t = 0; t <= 200000 + 7199; t += 7200) {
        sms_ym2413_catch_up(&dev, t < 200000 ? t : 200000);
        int32_t s[256];
        size_t n;
        while ((n = sms_ym2413_drain(&dev, s, 256)) != 0) {
            for (size_t i = 0; i < n; i++) {
                uint32_t v = (uint32_t)s[i];
                for (int k = 0; k < 4; k++)
                    h = (h ^ ((v >> (8 * k)) & 0xFFu)) * 16777619u;
                nonzero += s[i] != 0;
            }
            total += n;
        }
    }
    CHECK_EQ(total, 2777);
    CHECK_EQ(nonzero, 2768);
    CHECK_EQ(h, 0x3b1ddfe2u);
}
