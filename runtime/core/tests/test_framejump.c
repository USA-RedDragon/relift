/**
 * @file test_framejump.c
 * @brief The frame jump detector: a one-frame flash between agreeing frames is found and named, a fade, a cut and a
 * flash that stays are not, and the signature samples the pixel at each grid cell's center.
 */
#include "runner.h"

#include "framejump.h"

#include <stdlib.h>

#define W 256u
#define H 144u

/** @brief A gray picture of level v with a lighter band whose left edge is at x0, as a scene. */
static void scene(uint8_t *px, uint8_t v, uint32_t x0)
{
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *p = px + ((size_t)y * W + x) * 4;
            uint8_t l = x >= x0 && x < x0 + 40 ? (uint8_t)(v + 30) : v;
            p[0] = p[1] = p[2] = l;
            p[3] = 255;
        }
}

/** @brief Dark brown blocks in a staircase: 4-row groups, bright blobs every 20 columns, moving 4 right per 8 rows. */
static void staircase(uint8_t *px)
{
    for (uint32_t y = 0; y < H; y++)
        for (uint32_t x = 0; x < W; x++) {
            uint8_t *p = px + ((size_t)y * W + x) * 4;
            uint32_t phase = (x + 4 * (y / 8)) % 20;
            bool blob = phase < 4;
            p[0] = blob ? 220 : (uint8_t)(40 + 8 * ((y / 4) % 4));
            p[1] = blob ? 160 : 28;
            p[2] = blob ? 60 : 16;
            p[3] = 255;
        }
}

/** @brief A picture's pixels; the process stops when there is no memory, as a test should not go on without it. */
static uint8_t *pic(void)
{
    uint8_t *p = malloc((size_t)W * H * 4);
    if (!p)
        abort();
    return p;
}

static bool push(rl_jump *j, uint64_t n, const uint8_t *px, uint64_t *out)
{
    uint8_t sig[RL_JUMP_SIG];
    rl_jump_sig_rgba(px, W, H, (size_t)W * 4, false, sig);
    return rl_jump_push(j, n, sig, out);
}

TEST(frame_jump_signature_samples_the_center_of_each_cell)
{
    uint8_t *px = pic();
    for (uint32_t i = 0; i < W * H; i++) {
        px[4 * i] = (uint8_t)(i % W);
        px[4 * i + 1] = (uint8_t)(i % W);
        px[4 * i + 2] = (uint8_t)(i % W);
    }
    uint8_t sig[RL_JUMP_SIG];
    rl_jump_sig_rgba(px, W, H, (size_t)W * 4, false, sig);
    /* 256 columns over 64 cells: cell x has its center at 4x + 2. */
    for (uint32_t x = 0; x < RL_JUMP_SIG_W; x++)
        CHECK_EQ(sig[5 * RL_JUMP_SIG_W + x], 4 * x + 2);
    CHECK_EQ(rl_jump_luma(255, 0, 0), 76);
    CHECK_EQ(rl_jump_luma(0, 255, 0), 150);
    CHECK_EQ(rl_jump_luma(0, 0, 255), 29);
    uint8_t a[RL_JUMP_SIG], b[RL_JUMP_SIG];
    memset(a, 10, sizeof a);
    memset(b, 13, sizeof b);
    CHECK_EQ(rl_jump_dist(a, b), 48);
    CHECK_EQ(rl_jump_dist(b, a), 48);
    free(px);
}

TEST(frame_jump_finds_one_flashed_frame_in_a_fade)
{
    uint8_t *px = pic();
    rl_jump j = { 0 };
    uint64_t out = 0;
    uint64_t hits = 0, at = 0;
    for (uint64_t n = 0; n < 40; n++) {
        if (n == 17)
            staircase(px);
        else
            scene(px, (uint8_t)(10 + 3 * n), 100);
        if (push(&j, n, px, &out)) {
            hits++;
            at = out;
        }
    }
    CHECK_EQ(hits, 1);
    CHECK_EQ(at, 17);
    free(px);
}

TEST(frame_jump_ignores_a_cut_a_fade_and_a_change_that_stays)
{
    uint8_t *px = pic();
    rl_jump j = { 0 };
    uint64_t out = 0;
    for (uint64_t n = 0; n < 60; n++) {
        if (n < 20)
            scene(px, (uint8_t)(5 * n), 30);
        else if (n < 30)
            scene(px, 200, 180);
        else
            staircase(px);
        CHECK(!push(&j, n, px, &out));
    }
    free(px);
}

TEST(frame_jump_takes_two_neighbours_before_it_can_answer)
{
    uint8_t *px = pic();
    rl_jump j = { 0 };
    uint64_t out = 99;
    staircase(px);
    CHECK(!push(&j, 0, px, &out));
    scene(px, 20, 10);
    CHECK(!push(&j, 1, px, &out));
    CHECK(push(&j, 2, px, &out) == false);
    staircase(px);
    CHECK(!push(&j, 3, px, &out));
    scene(px, 20, 10);
    CHECK(push(&j, 4, px, &out));
    CHECK_EQ(out, 3);
    CHECK(j.d_next >= RL_JUMP_DEFAULT_THRESHOLD && j.d_prev >= RL_JUMP_DEFAULT_THRESHOLD);
    CHECK_EQ(j.d_skip, 0);
    free(px);
}
