/**
 * @file audio_test.c
 * @brief The console's sound at the host rate: one second of CPU clocks
 * makes exactly the host rate's frames in pieces of any size, a PSG tone is
 * heard and its pitch is kept, the FM unit is heard on a Japanese Master
 * System when the audio control port unmutes it, and silence stays silent.
 */
#include "audio.h"
#include "runner.h"

#include <stdlib.h>

static sms m;
static sms_audio a;
static uint8_t rom[0x8000];
static int16_t pcm[2 * 44100 + 64];

static void boot(sms_region region)
{
    sms_config cfg = {.system = SMS_SYSTEM_MS, .region = region, .mapper = SMS_MAPPER_SEGA};
    sms_free(&m);
    CHECK_EQ(sms_init(&m, &cfg, rom, sizeof rom), 0);
    sms_audio_init(&a, &m, SMS_AUDIO_RATE);
}

static size_t take_second(void)
{
    size_t n = 0;
    for (uint64_t t = 12345;; t += 12345) {
        uint64_t now = t < 3579545 ? t : 3579545;
        n += sms_audio_take(&a, &m, now, pcm + 2 * n, 44100 + 32 - n);
        if (now == 3579545)
            return n;
    }
}

TEST(audio_a_second_of_clocks_makes_a_second_of_frames)
{
    boot(SMS_REGION_NTSC_U);
    CHECK_EQ(take_second(), 44100);
    int peak = 0;
    for (size_t i = 0; i < 2 * 44100; i++)
        peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
    CHECK_EQ(peak, 0);
}

TEST(audio_a_psg_tone_keeps_its_pitch)
{
    boot(SMS_REGION_NTSC_U);
    sms_out(&m, 0x7F, 0x80 | 0x0E, 0);
    sms_out(&m, 0x7F, 0x07, 0);
    sms_out(&m, 0x7F, 0x90, 0);
    CHECK_EQ(take_second(), 44100);
    int crossings = 0;
    for (size_t i = 1; i < 44100; i++)
        crossings += (pcm[2 * (i - 1)] < 0) != (pcm[2 * i] < 0);
    int hz = crossings / 2;
    int want = (int)(3579545 / (32 * 0x7E));
    CHECK(hz >= want - 4 && hz <= want + 4);
    CHECK(pcm[2000] == pcm[2001]);
}

TEST(audio_fm_is_heard_on_a_japanese_console_when_unmuted)
{
    boot(SMS_REGION_NTSC_J);
    sms_out(&m, 0xF2, 0x01, 0);
    sms_out(&m, 0xF0, 0x30, 0);
    sms_out(&m, 0xF1, 0x10, 0);
    sms_out(&m, 0xF0, 0x10, 0);
    sms_out(&m, 0xF1, 0xAC, 0);
    sms_out(&m, 0xF0, 0x20, 0);
    sms_out(&m, 0xF1, 0x17, 0);
    CHECK_EQ(take_second(), 44100);
    int peak = 0;
    for (size_t i = 0; i < 2 * 44100; i++)
        peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
    CHECK(peak > 100);
    boot(SMS_REGION_NTSC_J);
    sms_out(&m, 0xF0, 0x30, 0);
    sms_out(&m, 0xF1, 0x10, 0);
    sms_out(&m, 0xF0, 0x20, 0);
    sms_out(&m, 0xF1, 0x17, 0);
    CHECK_EQ(take_second(), 44100);
    peak = 0;
    for (size_t i = 0; i < 2 * 44100; i++)
        peak = abs(pcm[i]) > peak ? abs(pcm[i]) : peak;
    CHECK_EQ(peak, 0);
}
