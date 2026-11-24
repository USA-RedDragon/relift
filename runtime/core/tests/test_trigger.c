/**
 * @file test_trigger.c
 * @brief The capture trigger keeps hits in order and fires at the presents RELIFT_TRIGGER_AT names and when
 * RELIFT_TRIGGER_FILE appears; the zero-band detector finds thin black bands over earlier content, and not black that
 * was already black, wide black or short runs.
 */
#include "runner.h"

#include "trigger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define W 240u
#define H 135u

/** @brief A textured picture: no pixel is near black. */
static void scene(uint8_t *px, uint32_t seed)
{
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *p = px + ((size_t)y * W + x) * 4;
            p[0] = (uint8_t)(40 + (x * 3 + seed) % 150);
            p[1] = (uint8_t)(40 + (y * 5 + seed) % 150);
            p[2] = (uint8_t)(60 + (x + y) % 100);
            p[3] = 255;
        }
}

/** @brief Paints columns x0 to x1, rows y0 to y1 exact black (alpha kept). */
static void black(uint8_t *px, uint32_t x0, uint32_t x1, uint32_t y0, uint32_t y1)
{
    for (uint32_t y = y0; y < y1; y++)
        for (uint32_t x = x0; x < x1; x++)
            memset(px + ((size_t)y * W + x) * 4, 0, 3);
}

TEST(trigger_keeps_hits_in_order_and_counts_drops)
{
    rl_trigger_reset();
    rl_trigger_hit h;
    CHECK(!rl_trigger_take(&h));
    rl_trigger_fire(7, "jump", "d %d", 3);
    rl_trigger_fire(9, "zband", "x");
    CHECK(rl_trigger_take(&h));
    CHECK_EQ(h.present, 7);
    CHECK(!strcmp(h.source, "jump"));
    CHECK(!strcmp(h.detail, "d 3"));
    CHECK(rl_trigger_take(&h));
    CHECK_EQ(h.present, 9);
    CHECK(!rl_trigger_take(&h));
    for (uint32_t i = 0; i < RL_TRIGGER_QUEUE + 3; i++)
        rl_trigger_fire(i, "t", "%u", i);
    CHECK_EQ(rl_trigger_dropped(), 3);
    rl_trigger_reset();
}

TEST(trigger_fires_at_listed_presents_and_on_a_file)
{
    rl_trigger_reset();
    setenv("RELIFT_TRIGGER_AT", "5,12", 1);
    char path[] = "test_trigger_fire.tmp";
    remove(path);
    setenv("RELIFT_TRIGGER_FILE", path, 1);
    rl_trigger_hit h;
    for (uint64_t p = 0; p < 20; p++) {
        if (p == 15) {
            FILE *f = fopen(path, "wb");
            CHECK(f != NULL);
            fclose(f);
        }
        rl_trigger_poll(p);
    }
    CHECK(rl_trigger_take(&h));
    CHECK_EQ(h.present, 5);
    CHECK(!strcmp(h.source, "at"));
    CHECK(rl_trigger_take(&h));
    CHECK_EQ(h.present, 12);
    CHECK(rl_trigger_take(&h));
    CHECK_EQ(h.present, 15);
    CHECK(!strcmp(h.source, "file"));
    CHECK(!rl_trigger_take(&h));
    FILE *f = fopen(path, "rb");
    CHECK(f == NULL);
    unsetenv("RELIFT_TRIGGER_AT");
    unsetenv("RELIFT_TRIGGER_FILE");
    rl_trigger_reset();
}

TEST(zero_band_finds_thin_black_over_content)
{
    uint8_t *a = malloc((size_t)W * H * 4), *b = malloc((size_t)W * H * 4);
    scene(a, 0);
    scene(b, 1);
    black(b, 18, 32, 0, 78);   /* a band at a tile's top edge */
    black(b, 150, 163, 74, 135); /* one to the bottom */
    rl_zband z[RL_ZBAND_MAX];
    uint32_t n = rl_zband_find(b, a, W, H, (size_t)W * 4, NULL, z, RL_ZBAND_MAX);
    CHECK_EQ(n, 2);
    CHECK_EQ(z[0].x0, 18);
    CHECK_EQ(z[0].x1, 32);
    CHECK_EQ(z[0].y0, 0);
    CHECK_EQ(z[0].y1, 78);
    CHECK_EQ(z[1].x0, 150);
    CHECK_EQ(z[1].y0, 74);
    CHECK_EQ(z[1].y1, 135);
    free(a);
    free(b);
}

TEST(zero_band_ignores_old_black_wide_black_and_short_runs)
{
    uint8_t *a = malloc((size_t)W * H * 4), *b = malloc((size_t)W * H * 4);
    scene(a, 0);
    black(a, 220, 230, 0, 135); /* black before too: a letterbox, not a band */
    memcpy(b, a, (size_t)W * H * 4);
    black(b, 0, 120, 40, 100);  /* half the picture: a scene change */
    black(b, 150, 160, 10, 14); /* four rows: too short */
    rl_zband z[RL_ZBAND_MAX];
    CHECK_EQ(rl_zband_find(b, a, W, H, (size_t)W * 4, NULL, z, RL_ZBAND_MAX), 0);
    /* A level of 2 takes near-black as zero. */
    scene(b, 1);
    for (uint32_t y = 20; y < 90; y++)
        for (uint32_t x = 60; x < 70; x++) {
            uint8_t *p = b + ((size_t)y * W + x) * 4;
            p[0] = 2;
            p[1] = 1;
            p[2] = 0;
        }
    CHECK_EQ(rl_zband_find(b, a, W, H, (size_t)W * 4, NULL, z, RL_ZBAND_MAX), 0);
    rl_zband_opts o = { .level = 2 };
    CHECK_EQ(rl_zband_find(b, a, W, H, (size_t)W * 4, &o, z, RL_ZBAND_MAX), 1);
    CHECK_EQ(z[0].x0, 60);
    CHECK_EQ(z[0].y1, 90);
    free(a);
    free(b);
}
