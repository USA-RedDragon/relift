/**
 * @file test_ai.c
 * @brief The Nintendo 64 audio interface: the two-entry DMA FIFO and the
 * status bits, the interrupt at the start of each buffer, the DAC's rate
 * from AI_DACRATE, the samples read from RDRAM and the frames handed out.
 */
#include "runner.h"

#include "n64.h"
#include "n64_int.h"

#include <stdlib.h>
#include <string.h>

enum {
    AI_DRAM = 0x04500000u,
    AI_LEN = 0x04500004u,
    AI_CONTROL = 0x04500008u,
    AI_STATUS = 0x0450000Cu,
    AI_DACRATE = 0x04500010u,
    AI_BITRATE = 0x04500014u,
    MI_MASK = 0x0430000Cu,
    BUF = 0x10000,
};

typedef struct mach {
    rl_sched *s;
    rl_sched_cpu *k;
    rl_n64 *m;
    uint8_t rom[0x2000];
} mach;

static void mach_new(mach *t, char region)
{
    memset(t->rom, 0, sizeof t->rom);
    t->rom[0x3E] = (uint8_t)region;
    t->s = rl_sched_create(NULL);
    t->k = rl_sched_add_cpu(t->s, 1);
    t->m = rl_n64_create(t->rom, sizeof t->rom);
    rl_n64_attach(t->m, t->s, t->k);
    rl_n64_mi_lower(t->m, RL_N64_IRQ_ALL);
    rl_n64_write(t->m, MI_MASK, 4, 0x20u);
}

static void mach_free(mach *t)
{
    rl_n64_destroy(t->m);
    rl_sched_destroy(t->s);
}

static void advance(mach *t, uint64_t n)
{
    rl_sched_stall(t->k, n);
    rl_sched_run(t->k);
}

static bool irq(mach *t) { return rl_n64_irq_pending(t->m) == RL_N64_IRQ_AI; }

static void play(mach *t, uint32_t addr, uint32_t len)
{
    rl_n64_write(t->m, AI_DRAM, 4, addr);
    rl_n64_write(t->m, AI_LEN, 4, len);
}

/** @brief Ticks one sample of the DAC lasts at the rate now. */
static uint64_t sample_ticks(mach *t) { return (uint64_t)RL_N64_HZ / rl_n64_ai_rate(t->m); }

TEST(n64_ai_dac_rate_is_the_video_clock_over_dacrate_plus_one)
{
    mach t;
    mach_new(&t, 'E');
    CHECK_EQ(rl_n64_ai_rate(t.m), 44100);
    rl_n64_write(t.m, AI_DACRATE, 4, 1103);
    CHECK_EQ(rl_n64_ai_rate(t.m), 48681818u / 1104u);
    rl_n64_write(t.m, AI_DACRATE, 4, 0x3FFFu);
    CHECK_EQ(rl_n64_ai_rate(t.m), 48681818u / 0x4000u);
    mach_free(&t);
    mach_new(&t, 'P');
    rl_n64_write(t.m, AI_DACRATE, 4, 1103);
    CHECK_EQ(rl_n64_ai_rate(t.m), 49656530u / 1104u);
    mach_free(&t);
}

TEST(n64_ai_fifo_holds_two_buffers_and_reports_them)
{
    mach t;
    mach_new(&t, 'E');
    rl_n64_write(t.m, AI_DACRATE, 4, 1103);
    CHECK_EQ(rl_n64_read(t.m, AI_STATUS, 4) & 0xC0000001u, 0u);
    play(&t, BUF, 0x400);
    CHECK_EQ(rl_n64_read(t.m, AI_STATUS, 4) & 0xC0000001u, 0x40000000u);
    play(&t, BUF + 0x400, 0x200);
    CHECK_EQ(rl_n64_read(t.m, AI_STATUS, 4) & 0xC0000001u, 0xC0000001u);
    play(&t, BUF + 0x800, 0x100);
    CHECK_EQ(rl_n64_read(t.m, AI_LEN, 4), 0x400u);
    CHECK_EQ(rl_n64_read(t.m, AI_STATUS, 4) & (1u << 25), 0u);
    rl_n64_write(t.m, AI_CONTROL, 4, 1);
    CHECK_EQ(rl_n64_read(t.m, AI_STATUS, 4) & (1u << 25), 1u << 25);
    CHECK_EQ(rl_n64_read(t.m, AI_STATUS, 4) & (1u << 20 | 1u << 24), 1u << 20 | 1u << 24);
    mach_free(&t);
}

TEST(n64_ai_raises_its_interrupt_when_each_buffer_starts)
{
    mach t;
    mach_new(&t, 'E');
    rl_n64_write(t.m, AI_DACRATE, 4, 1103);
    rl_n64_write(t.m, AI_CONTROL, 4, 1);
    uint64_t st = sample_ticks(&t);
    advance(&t, 1000);
    play(&t, BUF, 400);
    CHECK(irq(&t));
    rl_n64_write(t.m, AI_STATUS, 4, 0);
    CHECK(!irq(&t));
    play(&t, BUF + 400, 200);
    CHECK(!irq(&t));
    advance(&t, 99u * st - 3u * st);
    CHECK(!irq(&t));
    uint32_t left = rl_n64_read(t.m, AI_LEN, 4);
    CHECK(left >= 8u && left <= 24u);
    advance(&t, 6u * st);
    CHECK(irq(&t));
    CHECK_EQ(rl_n64_read(t.m, AI_STATUS, 4) & 0xC0000001u, 0x40000000u);
    left = rl_n64_read(t.m, AI_LEN, 4);
    CHECK(left >= 180u && left <= 196u);
    rl_n64_write(t.m, AI_STATUS, 4, 0);
    advance(&t, 50u * st + 5u * st);
    CHECK(!irq(&t));
    CHECK_EQ(rl_n64_read(t.m, AI_STATUS, 4) & 0xC0000000u, 0u);
    mach_free(&t);
}

TEST(n64_ai_does_not_play_while_disabled)
{
    mach t;
    mach_new(&t, 'E');
    rl_n64_write(t.m, AI_DACRATE, 4, 1103);
    play(&t, BUF, 400);
    advance(&t, 1000u * sample_ticks(&t));
    CHECK_EQ(rl_n64_read(t.m, AI_LEN, 4), 400u);
    rl_n64_write(t.m, AI_CONTROL, 4, 1);
    advance(&t, 10u * sample_ticks(&t));
    CHECK(rl_n64_read(t.m, AI_LEN, 4) < 400u);
    mach_free(&t);
}

TEST(n64_ai_hands_out_one_frame_per_output_period_and_the_samples_it_read)
{
    mach t;
    mach_new(&t, 'E');
    rl_n64_write(t.m, AI_DACRATE, 4, 1103);
    uint8_t *ram = rl_n64_rdram(t.m);
    for (unsigned i = 0; i < 2000; i++) {
        ram[(BUF + i * 4u) ^ 3] = 0x03;
        ram[(BUF + i * 4u + 1u) ^ 3] = 0xE8;
        ram[(BUF + i * 4u + 2u) ^ 3] = 0xF8;
        ram[(BUF + i * 4u + 3u) ^ 3] = 0x30;
    }
    rl_n64_write(t.m, AI_CONTROL, 4, 1);
    play(&t, BUF, 8000);
    advance(&t, RL_N64_HZ / 4u);
    static int16_t pcm[RL_N64_AUDIO_RING * 2];
    size_t n = rl_n64_audio_take(t.m, pcm, RL_N64_AUDIO_RING);
    CHECK(n >= RL_N64_AUDIO_RATE / 4u - 2u && n <= RL_N64_AUDIO_RATE / 4u + 2u);
    size_t loud = 0;
    for (size_t i = 8; i < n; i++)
        if (pcm[i * 2] == 1000 && pcm[i * 2 + 1] == -2000)
            loud++;
    CHECK(loud > 1500);
    CHECK_EQ(pcm[(n - 1) * 2], 0);
    CHECK_EQ(rl_n64_audio_take(t.m, pcm, 4), 0);
    mach_free(&t);
}

TEST(n64_ai_idle_dac_makes_silence_for_the_time_that_passed)
{
    mach t;
    mach_new(&t, 'E');
    advance(&t, RL_N64_HZ);
    static int16_t pcm[RL_N64_AUDIO_RING * 2];
    size_t total = 0, n;
    while ((n = rl_n64_audio_take(t.m, pcm, RL_N64_AUDIO_RING)) > 0) {
        for (size_t i = 0; i < n * 2; i++)
            CHECK_EQ(pcm[i], 0);
        total += n;
    }
    CHECK(total <= RL_N64_AUDIO_RING);
    mach_free(&t);
}

TEST(n64_ai_the_second_buffer_keeps_the_8k_address_carry_of_the_first)
{
    mach t;
    mach_new(&t, 'E');
    rl_n64_write(t.m, AI_DACRATE, 4, 1103);
    uint8_t *ram = rl_n64_rdram(t.m);
    for (unsigned i = 0; i < 4; i++) {
        ram[(0x6000 + i * 4u) ^ 3] = 0x22;
        ram[(0x8000 + i * 4u) ^ 3] = 0x33;
    }
    rl_n64_write(t.m, AI_CONTROL, 4, 1);
    play(&t, 0x2000u - 8u, 8);
    play(&t, 0x6000u, 16);
    advance(&t, 40u * sample_ticks(&t));
    static int16_t pcm[256 * 2];
    size_t n = rl_n64_audio_take(t.m, pcm, 256);
    unsigned third = 0, second = 0;
    for (size_t i = 0; i < n; i++) {
        third += pcm[i * 2] == 0x3300;
        second += pcm[i * 2] == 0x2200;
    }
    CHECK(third >= 2);
    CHECK_EQ(second, 0);
    mach_free(&t);
}
