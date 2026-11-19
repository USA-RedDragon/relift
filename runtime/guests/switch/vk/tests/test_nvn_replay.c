/**
 * @file test_nvn_replay.c
 * @brief One recorded NVN2 command at a time through the command-buffer
 * replay (nvn_draw_cmd), for the commands that change only the replay's
 * state and make no Vulkan call.
 */
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <string.h>

/** @brief Replays e against a zeroed replay and state; returns its result. */
static int replay_one(nvn_gfx *st, const nvn_cmd *e)
{
    replay r;
    memset(&r, 0, sizeof r);
    r.st = st;
    return nvn_draw_cmd(&r, e);
}

TEST(nvn_replay_set_viewport_and_depth_range_are_kept_in_the_state)
{
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_cmd v = { .op = NVN_CMD_SET_VIEWPORT, .x = { 0, 4, 8, 1280, 720 } };
    CHECK_EQ(replay_one(&st, &v), RL_OK);
    CHECK_EQ(st.viewports[0][0], 4.0f);
    CHECK_EQ(st.viewports[0][1], 8.0f);
    CHECK_EQ(st.viewports[0][2], 1280.0f);
    CHECK_EQ(st.viewports[0][3], 720.0f);
    nvn_cmd d = { .op = NVN_CMD_SET_DEPTH_RANGE };
    float a = 0.25f, b = 0.75f;
    memcpy(&d.d[0], &a, 4);
    memcpy(&d.d[1], &b, 4);
    CHECK_EQ(replay_one(&st, &d), RL_OK);
    CHECK_EQ(st.depth_ranges[0][0], 0.25f);
    CHECK_EQ(st.depth_ranges[0][1], 0.75f);
}

TEST(nvn_replay_depth_clamp_is_kept_in_the_state)
{
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_cmd e = { .op = NVN_CMD_SET_DEPTH_CLAMP, .x = { 0, 0 } };
    CHECK_EQ(replay_one(&st, &e), RL_OK);
    CHECK(!st.depth_clamp);
}

TEST(nvn_compute_table_puts_pushes_then_buffers_then_handles)
{
    static nvn_gfx st;
    memset(&st, 0, sizeof st);
    for (uint32_t i = 0; i < 0x70; i++)
        st.push[NVN_STAGE_COMPUTE][0][i] = (uint8_t)(0x80 + i);
    st.push_used[NVN_STAGE_COMPUTE] = 1;
    for (uint32_t i = 0; i < 2; i++) {
        st.ubo[NVN_STAGE_COMPUTE][i].addr = 0x1000 + i;
        st.ubo[NVN_STAGE_COMPUTE][i].size = 0x100 + i;
    }
    for (uint32_t i = 0; i < 5; i++) {
        st.storage[i].addr = 0x2000 + i;
        st.storage[i].size = 0x200 + i;
    }
    st.image[NVN_STAGE_COMPUTE][0] = 0x443;
    uint8_t c0[NVN_BANK2_BYTES];
    uint32_t img_lo = 0, end = 0;
    CHECK(nvn_compute_table(&st, 2, 5, 0, 1, 0xf8, c0, &img_lo, &end));
    CHECK_EQ(img_lo, 0xf0);
    CHECK_EQ(end, 0xf8);
    CHECK_EQ(c0[0x10], 0x80);
    CHECK_EQ(c0[0x7f], 0x80 + 0x6f);
    uint64_t a;
    uint32_t sz;
    memcpy(&a, c0 + 0x80, 8);
    memcpy(&sz, c0 + 0x88, 4);
    CHECK_EQ(a, 0x1000);
    CHECK_EQ(sz, 0x100);
    memcpy(&a, c0 + 0xa0, 8);
    memcpy(&sz, c0 + 0xa8, 4);
    CHECK_EQ(a, 0x2000);
    CHECK_EQ(sz, 0x200);
    memcpy(&a, c0 + 0xe0, 8);
    CHECK_EQ(a, 0x2004);
    memcpy(&sz, c0 + 0xf0, 4);
    CHECK_EQ(sz, 0x443);
    CHECK(!nvn_compute_table(&st, 2, 5, 0, 1, 0x80, c0, &img_lo, &end));
    st.push_used[NVN_STAGE_COMPUTE] = 3;
    CHECK(!nvn_compute_table(&st, 2, 5, 0, 1, 0xf8, c0, &img_lo, &end));
}

TEST(nvn_copy_pitch_follows_the_drivers_strides)
{
    nvn_copy_pitch p;
    nvn_region rg = { 0, 0, 0, 10, 3, 2 };
    CHECK(nvn_copy_pitch_of(37, &rg, 0, 0, &p));
    CHECK_EQ(p.row_bytes, 40);
    CHECK_EQ(p.image_bytes, 120);
    CHECK_EQ(p.row_texels, 10);
    CHECK_EQ(p.image_rows, 3);
    CHECK_EQ(p.span, 120 + 80 + 40);
    CHECK(nvn_copy_pitch_of(37, &rg, 64, 256, &p));
    CHECK_EQ(p.row_texels, 16);
    CHECK_EQ(p.image_rows, 4);
    CHECK_EQ(p.span, 256 + 128 + 40);
    CHECK(!nvn_copy_pitch_of(37, &rg, 42, 0, &p));
    CHECK(!nvn_copy_pitch_of(37, &rg, 36, 0, &p));
    CHECK(!nvn_copy_pitch_of(37, &rg, 64, 100, &p));
    nvn_region bc = { 0, 0, 0, 10, 6, 1 };
    CHECK(nvn_copy_pitch_of(65, &bc, 0, 0, &p));
    CHECK_EQ(p.row_bytes, 24);
    CHECK_EQ(p.row_texels, 12);
    CHECK_EQ(p.image_rows, 8);
    CHECK_EQ(p.span, 48);
}
