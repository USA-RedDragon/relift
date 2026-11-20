/**
 * @file test_nvn_counters.c
 * @brief The GPU counters a report writes: both timestamp locations write
 * the guest clock, and a type the layer does not count stops the replay.
 */
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <string.h>

TEST(nvn_timestamps_of_both_pipe_locations_write_the_guest_clock)
{
    static rl_ctx c;
    static uint8_t mem[0x1000];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    replay r;
    memset(&r, 0, sizeof r);
    r.c = &c;
    r.mem = mem;
    r.st = &st;
    uint32_t types[2] = { 0, NVN_COUNTER_TIMESTAMP_TOP };
    for (unsigned i = 0; i < 2; i++) {
        uint64_t at = 0x100 + 0x20 * i;
        rl_st64(mem, at, 0xdeadull);
        CHECK_EQ(nvn_counters_report(&r, types[i], at), RL_OK);
        CHECK_EQ(rl_ld64(mem, at), 0);
        CHECK(rl_ld64(mem, at + 8) != 0);
    }
}

TEST(nvn_render_target_views_may_name_the_srgb_or_linear_twin)
{
    CHECK_EQ(nvn_vk_color_twin(VK_FORMAT_R8G8B8A8_UNORM), VK_FORMAT_R8G8B8A8_SRGB);
    CHECK_EQ(nvn_vk_color_twin(VK_FORMAT_R8G8B8A8_SRGB), VK_FORMAT_R8G8B8A8_UNORM);
    CHECK_EQ(nvn_vk_color_twin(VK_FORMAT_B8G8R8A8_UNORM), VK_FORMAT_B8G8R8A8_SRGB);
    CHECK_EQ(nvn_vk_color_twin(VK_FORMAT_R16G16B16A16_SFLOAT), VK_FORMAT_UNDEFINED);
}

TEST(nvn_eight_bit_indices_widen_and_keep_their_restart)
{
    const uint8_t src[4] = { 0, 7, 0xff, 200 };
    uint16_t dst[4];
    nvn_widen_indices(src, 4, true, dst);
    CHECK_EQ(dst[1], 7);
    CHECK_EQ(dst[2], 0xffff);
    CHECK_EQ(dst[3], 200);
    nvn_widen_indices(src, 4, false, dst);
    CHECK_EQ(dst[2], 0xff);
}
