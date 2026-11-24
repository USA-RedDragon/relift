/**
 * @file test_nvn_msaa.c
 * @brief Multisample textures get host images of their own sample count, which draw into them at that count and
 * bind only as multisample textures.
 */
#include "hos.h"
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <string.h>

/** @brief A replay over 64 KiB of guest memory, all zeros, with a pool over all of it. */
typedef struct rig {
    rl_ctx c;
    uint8_t mem[0x10000];
    nvn_gfx st;
    replay r;
} rig;

static bool rig_up(rig *g_, uint64_t pool)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return false;
    if (!hos.vm.host_page) {
        hos.vm.host_page = 4096;
        hos.vm.size = 0x10000;
    }
    memset(g_, 0, sizeof *g_);
    g_->r.c = &g_->c;
    g_->r.mem = g_->mem;
    g_->r.st = &g_->st;
    return nvn_pool_make(pool, 0, 0x10000, 0x2a1);
}

TEST(nvn_a_multisample_texture_gets_a_host_image_of_its_sample_count)
{
    static rig k;
    const uint64_t pool = 0x110, tex = 0x210, arr = 0x310;
    if (!rig_up(&k, pool))
        return;
    nvn_tex_desc d = { .target = NVN_TARGET_2D_MULTISAMPLE, .format = 37, .width = 16, .height = 8, .depth = 1,
                       .levels = 1, .samples = 8, .pool = pool, .offset = 0x1000 };
    CHECK(nvn_tex_layout(&d));
    CHECK(nvn_vk_can_image(&d));
    CHECK(nvn_texture_record(tex, &d, NULL));
    vimg *v = nvn_vk_image(&k.r, tex);
    CHECK(v != NULL);
    if (!v)
        return;
    CHECK_EQ(nvn_vk_sample_bits(&v->d), VK_SAMPLE_COUNT_8_BIT);
    CHECK(!v->storage_ok);
    CHECK_EQ(nvn_vk_image_range(v).layerCount, 1u);
    nvn_tex_desc a = d;
    a.target = NVN_TARGET_2D_MULTISAMPLE_ARRAY;
    a.depth = 3;
    a.samples = 4;
    a.offset = 0x8000;
    CHECK(nvn_tex_layout(&a));
    CHECK(nvn_texture_record(arr, &a, NULL));
    vimg *w = nvn_vk_image(&k.r, arr);
    CHECK(w != NULL);
    CHECK(w && nvn_vk_image_range(w).layerCount == 3u);
    CHECK(nvn_vk_flush(&k.r));
}

TEST(nvn_multisample_textures_need_a_multisample_target_and_one_level)
{
    nvn_tex_desc d = { .target = NVN_TARGET_2D_MULTISAMPLE, .format = 37, .width = 16, .height = 8, .depth = 1,
                       .levels = 1, .samples = 4 };
    CHECK(nvn_vk_can_image(&d));
    d.samples = 3;
    CHECK(!nvn_vk_can_image(&d));
    d.samples = 4;
    d.levels = 2;
    CHECK(!nvn_vk_can_image(&d));
    d.levels = 1;
    d.target = 1;
    CHECK(!nvn_vk_can_image(&d));
    d.samples = 1;
    CHECK(nvn_vk_can_image(&d));
}

TEST(nvn_render_targets_report_their_common_sample_count)
{
    vimg a = { .d = { .samples = 4 } }, b = { .d = { .samples = 4 } }, one = { .d = { .samples = 0 } };
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    uint32_t n = 99;
    CHECK(nvn_vk_target_samples(&st, &n));
    CHECK_EQ(n, 0u);
    st.ncolor = 2;
    st.color[1].v = &a;
    st.depth.v = &b;
    CHECK(nvn_vk_target_samples(&st, &n));
    CHECK_EQ(n, 4u);
    st.color[0].v = &one;
    CHECK(!nvn_vk_target_samples(&st, &n));
}
