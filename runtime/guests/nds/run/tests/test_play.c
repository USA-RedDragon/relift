/**
 * @file test_play.c
 * @brief The screens' layouts and the bottom screen's place in them.
 */
#include "play.h"
#include "runner.h"

#include <relift/settings.h>

#include <string.h>

/** @brief A frame whose pixel i holds the 6-bit channels (i & 63, (i >> 6) & 63, the screen's index). */
static void test_frame(uint32_t *px)
{
    for (uint32_t i = 0; i < 2 * RL_NDS_SCREEN_W * RL_NDS_SCREEN_H; i++)
        px[i] = (i & 63u) | ((i >> 6) & 63u) << 6 | (i >= RL_NDS_SCREEN_W * RL_NDS_SCREEN_H ? 1u : 0u) << 12;
}

static uint32_t g_px[2 * RL_NDS_SCREEN_W * RL_NDS_SCREEN_H];
static uint8_t g_rgb[2 * RL_NDS_SCREEN_W * RL_NDS_SCREEN_H * 3];
static uint8_t g_want[2 * RL_NDS_SCREEN_W * RL_NDS_SCREEN_H * 3];

TEST(layouts_stack_or_place_the_screens_side_by_side)
{
    uint32_t w, h;
    rl_nds_layout_size(RL_NDS_LAYOUT_STACKED, &w, &h);
    CHECK_EQ(w, 256);
    CHECK_EQ(h, 384);
    rl_nds_layout_size(RL_NDS_LAYOUT_SIDE_BY_SIDE, &w, &h);
    CHECK_EQ(w, 512);
    CHECK_EQ(h, 192);
    test_frame(g_px);
    rl_nds_frame_rgb(g_px, g_want);
    rl_nds_layout_rgb(RL_NDS_LAYOUT_STACKED, g_px, g_rgb);
    CHECK(memcmp(g_rgb, g_want, sizeof g_rgb) == 0);
    rl_nds_layout_rgb(RL_NDS_LAYOUT_SIDE_BY_SIDE, g_px, g_rgb);
    for (uint32_t y = 0; y < 192; y += 37)
        for (uint32_t x = 0; x < 512; x += 41) {
            uint32_t src = x < 256 ? y * 256 + x : (192 + y) * 256 + (x - 256);
            CHECK_EQ(memcmp(g_rgb + (y * 512 + x) * 3, g_want + src * 3, 3), 0);
        }
}

TEST(only_the_bottom_screen_takes_touches)
{
    uint8_t x = 0, y = 0;
    CHECK(!rl_nds_layout_touch(RL_NDS_LAYOUT_STACKED, 10, 191, &x, &y));
    CHECK(rl_nds_layout_touch(RL_NDS_LAYOUT_STACKED, 10, 192, &x, &y));
    CHECK_EQ(x, 10);
    CHECK_EQ(y, 0);
    CHECK(rl_nds_layout_touch(RL_NDS_LAYOUT_STACKED, 255, 383, &x, &y));
    CHECK_EQ(x, 255);
    CHECK_EQ(y, 191);
    CHECK(!rl_nds_layout_touch(RL_NDS_LAYOUT_STACKED, 256, 300, &x, &y));
    CHECK(!rl_nds_layout_touch(RL_NDS_LAYOUT_STACKED, 0, 384, &x, &y));
    CHECK(!rl_nds_layout_touch(RL_NDS_LAYOUT_SIDE_BY_SIDE, 255, 100, &x, &y));
    CHECK(rl_nds_layout_touch(RL_NDS_LAYOUT_SIDE_BY_SIDE, 300, 100, &x, &y));
    CHECK_EQ(x, 44);
    CHECK_EQ(y, 100);
    CHECK(!rl_nds_layout_touch(RL_NDS_LAYOUT_SIDE_BY_SIDE, 300, 192, &x, &y));
}

TEST(the_layout_setting_picks_the_layout)
{
    rl_nds_declare_settings();
    CHECK_EQ(rl_nds_layout_setting(), RL_NDS_LAYOUT_STACKED);
    CHECK(rl_setting_bool("video.nds_integer_scale"));
    CHECK_EQ(rl_settings_set("video.nds_layout", "side_by_side"), RL_SETTING_OK);
    CHECK_EQ(rl_nds_layout_setting(), RL_NDS_LAYOUT_SIDE_BY_SIDE);
    CHECK_EQ(rl_settings_set("video.nds_layout", "stacked"), RL_SETTING_OK);
    CHECK_EQ(rl_nds_layout_setting(), RL_NDS_LAYOUT_STACKED);
}
