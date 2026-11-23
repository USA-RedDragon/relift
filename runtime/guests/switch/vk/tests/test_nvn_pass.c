/**
 * @file test_nvn_pass.c
 * @brief Render passes follow what the console orders: draws into the same
 * attachments share one host pass, and the pass ends, with its barrier, only
 * where the game changes targets, calls a barrier or reads a target the pass
 * drew into; dispatches the game puts no barrier between run without one unless
 * one touches what another writes. The device calls go to recording stand-ins.
 */
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <stdlib.h>
#include <string.h>

static struct {
    unsigned begins, ends, barriers;
    char order[64];
    unsigned n;
    /** @brief The layer count of the last pass begun, and the type, first layer and layers of the last view made. */
    uint32_t pass_layers, view_type, view_layer, view_layers, views;
} calls;

static void note(char c)
{
    if (calls.n + 1 < sizeof calls.order)
        calls.order[calls.n++] = c;
}

static void fake_begin(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    (void)cb;
    calls.begins++;
    calls.pass_layers = ri->layerCount;
    note('B');
}

static VkResult fake_view(VkDevice dev, const VkImageViewCreateInfo *ci, const VkAllocationCallbacks *a, VkImageView *out)
{
    (void)dev, (void)a;
    calls.views++;
    calls.view_type = (uint32_t)ci->viewType;
    calls.view_layer = ci->subresourceRange.baseArrayLayer;
    calls.view_layers = ci->subresourceRange.layerCount;
    *out = (VkImageView)(uintptr_t)(0x1000 + calls.views);
    return VK_SUCCESS;
}

static void fake_end(VkCommandBuffer cb)
{
    (void)cb;
    calls.ends++;
    note('E');
}

static void fake_barrier(VkCommandBuffer cb, VkPipelineStageFlags src, VkPipelineStageFlags dst,
                                    VkDependencyFlags dep, uint32_t nm, const VkMemoryBarrier *m, uint32_t nb,
                                    const VkBufferMemoryBarrier *b, uint32_t ni, const VkImageMemoryBarrier *i)
{
    (void)cb, (void)dep, (void)nm, (void)m, (void)nb, (void)b, (void)ni, (void)i;
    if (src == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT && dst == VK_PIPELINE_STAGE_ALL_COMMANDS_BIT)
        calls.barriers++;
    note('|');
}

/** @brief A recording replay over two color images of 64x32, with stand-ins for the pass and barrier calls. */
typedef struct fixture {
    vkf_fns saved;
    nvn_gfx st;
    replay r;
    vimg a, b;
} fixture;

static void setup(fixture *f)
{
    memset(f, 0, sizeof *f);
    memset(&calls, 0, sizeof calls);
    f->saved = g.fns;
    g.fns.f[VKG_vkCmdBeginRendering] = (void (*)(void))fake_begin;
    g.fns.f[VKG_vkCmdEndRendering] = (void (*)(void))fake_end;
    g.fns.f[VKG_vkCmdPipelineBarrier] = (void (*)(void))fake_barrier;
    g.fns.f[VKG_vkCreateImageView] = (void (*)(void))fake_view;
    f->a.d.width = f->b.d.width = 64;
    f->a.d.height = f->b.d.height = 32;
    f->st.ncolor = 1;
    f->st.color[0] = (rt){ .v = &f->a, .view = (VkImageView)(uintptr_t)0x10 };
    f->st.compute_overlap = 1;
    f->r.st = &f->st;
    nvn_vk_assume_recording(true);
}

static void teardown(fixture *f)
{
    nvn_vk_end_rendering();
    nvn_vk_assume_recording(false);
    g.fns = f->saved;
}

static void target(fixture *f, vimg *v, uintptr_t view)
{
    f->st.color[0] = (rt){ .v = v, .view = (VkImageView)view };
}

TEST(nvn_draws_into_the_same_targets_share_one_pass)
{
    static fixture f;
    setup(&f);
    VkRect2D area;
    for (unsigned i = 0; i < 5; i++)
        CHECK(nvn_vk_begin_rendering(&f.r, &area));
    CHECK_EQ(calls.begins, 1u);
    CHECK_EQ(calls.ends, 0u);
    CHECK_EQ(area.extent.width, 64u);
    CHECK_EQ(area.extent.height, 32u);
    teardown(&f);
    CHECK_EQ(calls.ends, 1u);
    CHECK_EQ(calls.barriers, 1u);
}

TEST(nvn_a_layered_draw_renders_every_layer_the_targets_share)
{
    static fixture f;
    setup(&f);
    f.a.d.target = 8;
    f.a.d.levels = 1;
    f.b.d = f.a.d;
    f.st.color[0] = (rt){ .v = &f.a, .layer = 0, .layers = 6, .view = (VkImageView)(uintptr_t)0x10 };
    f.st.depth = (rt){ .v = &f.b, .layer = 2, .layers = 4, .view = (VkImageView)(uintptr_t)0x20 };
    VkRect2D area;
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    CHECK_EQ(calls.pass_layers, 1u);
    CHECK_EQ(calls.views, 0u);
    CHECK(nvn_vk_begin_rendering_layered(&f.r, &area, true));
    CHECK_EQ(calls.begins, 2u);
    CHECK_EQ(calls.pass_layers, 4u);
    CHECK_EQ(calls.views, 2u);
    CHECK_EQ(calls.view_type, (uint32_t)VK_IMAGE_VIEW_TYPE_2D_ARRAY);
    CHECK_EQ(calls.view_layer, 2u);
    CHECK_EQ(calls.view_layers, 4u);
    CHECK(nvn_vk_begin_rendering_layered(&f.r, &area, true));
    CHECK_EQ(calls.begins, 2u);
    CHECK_EQ(calls.views, 2u);
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    CHECK_EQ(calls.begins, 3u);
    CHECK_EQ(calls.pass_layers, 1u);
    teardown(&f);
    free(f.a.sub);
    free(f.b.sub);
}

TEST(nvn_a_layered_draw_into_one_layer_is_a_plain_pass)
{
    static fixture f;
    setup(&f);
    f.st.color[0].layers = 1;
    VkRect2D area;
    CHECK(nvn_vk_begin_rendering_layered(&f.r, &area, true));
    CHECK_EQ(calls.pass_layers, 1u);
    CHECK_EQ(calls.views, 0u);
    teardown(&f);
}

TEST(nvn_new_render_targets_end_the_pass_with_its_barrier)
{
    static fixture f;
    setup(&f);
    VkRect2D area;
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    target(&f, &f.b, 0x20);
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    CHECK_EQ(calls.begins, 2u);
    CHECK_EQ(calls.ends, 1u);
    CHECK(!strncmp(calls.order, "BE|B", 4));
    teardown(&f);
}

TEST(nvn_a_game_barrier_ends_the_pass_and_the_next_draw_opens_another)
{
    static fixture f;
    setup(&f);
    VkRect2D area;
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    nvn_vk_game_barrier(&f.r);
    CHECK_EQ(calls.ends, 1u);
    CHECK_EQ(calls.barriers, 1u);
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    CHECK_EQ(calls.begins, 2u);
    nvn_vk_end_rendering();
    nvn_vk_game_barrier(&f.r);
    CHECK_EQ(calls.barriers, 3u);
    CHECK(!strcmp(calls.order, "BE|BE||"));
    teardown(&f);
}

TEST(nvn_reading_a_target_of_the_open_pass_ends_it_first)
{
    static fixture f;
    setup(&f);
    VkRect2D area;
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    nvn_vk_reading(&f.b);
    CHECK_EQ(calls.ends, 0u);
    nvn_vk_reading(&f.a);
    CHECK_EQ(calls.ends, 1u);
    CHECK_EQ(calls.barriers, 1u);
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    CHECK_EQ(calls.begins, 2u);
    teardown(&f);
}

TEST(nvn_a_command_outside_a_pass_ends_the_open_one)
{
    static fixture f;
    setup(&f);
    VkRect2D area;
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    CHECK(nvn_vk_begin(&f.r));
    CHECK_EQ(calls.ends, 1u);
    CHECK_EQ(calls.barriers, 1u);
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    CHECK_EQ(calls.begins, 2u);
    teardown(&f);
}

/** @brief A 2D color array of two layers with two levels each: level 0 at 0, level 1 at 0x1000, layers 0x2000 apart. */
static void array_image(vimg *v, uint64_t storage)
{
    memset(v, 0, sizeof *v);
    v->storage = storage;
    v->d.target = 4;
    v->d.levels = 2;
    v->d.depth = 2;
    v->d.layer_stride = 0x2000;
    v->d.size = 0x4000;
    v->d.level_offset[1] = 0x1000;
}

TEST(nvn_dispatches_without_a_hazard_or_barrier_between_them_overlap)
{
    static fixture f;
    setup(&f);
    vimg img;
    array_image(&img, 0x100000);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&img, 0, true, 0, 1, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    unsigned before = calls.barriers;
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&img, 0, true, 1, 1, true);
    nvn_vk_dispatch_buffer(0x200000, 0x100, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&img, 1, true, 0, 1, true);
    nvn_vk_dispatch_buffer(0x300000, 0x100, false);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before);
    nvn_vk_game_barrier(&f.r);
    CHECK_EQ(calls.barriers, before + 1);
    teardown(&f);
}

TEST(nvn_a_dispatch_touching_what_an_open_one_writes_waits_for_it)
{
    static fixture f;
    setup(&f);
    vimg img;
    array_image(&img, 0x100000);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&img, 1, true, 0, 1, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    unsigned before = calls.barriers;
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&img, 0, false, 0, 1, false);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 1);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_buffer(0x200000, 0x100, false);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 1);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_buffer(0x2000f0, 0x10, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 2);
    teardown(&f);
}

TEST(nvn_any_other_command_after_open_dispatches_waits_for_them)
{
    static fixture f;
    setup(&f);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_buffer(0x200000, 0x100, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    unsigned before = calls.barriers;
    VkRect2D area;
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    CHECK_EQ(calls.barriers, before + 1);
    CHECK_EQ(calls.begins, 1u);
    teardown(&f);
}

TEST(nvn_a_view_of_every_layer_covers_each_layers_levels)
{
    static fixture f;
    setup(&f);
    vimg img;
    array_image(&img, 0x100000);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&img, 0, true, 1, 1, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    unsigned before = calls.barriers;
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&img, 1, true, 0, 0, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&img, 0, false, 0, 0, false);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 1);
    teardown(&f);
}

TEST(nvn_dispatches_writing_other_levels_of_every_face_overlap)
{
    static fixture f;
    setup(&f);
    vimg cube;
    array_image(&cube, 0x100000);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&cube, 0, true, 0, 0, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    unsigned before = calls.barriers;
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&cube, 1, true, 0, 0, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&cube, 0, false, 1, 1, false);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 1);
    teardown(&f);
}

TEST(nvn_dispatches_writing_other_faces_of_a_cube_overlap)
{
    static fixture f;
    setup(&f);
    vimg cube;
    memset(&cube, 0, sizeof cube);
    cube.storage = 0x100000;
    cube.d.target = NVN_TARGET_CUBEMAP;
    cube.d.levels = 2;
    cube.d.depth = 1;
    cube.d.layer_stride = 0x2000;
    cube.d.size = 0xc000;
    cube.d.level_offset[1] = 0x1000;
    unsigned before = 0;
    for (uint32_t face = 0; face < 6; face++) {
        nvn_vk_dispatch_reset();
        nvn_vk_dispatch_image(&cube, 0, true, face, 1, true);
        CHECK(nvn_vk_begin_dispatch(&f.r));
        nvn_vk_dispatched();
        if (face == 0)
            before = calls.barriers;
    }
    CHECK_EQ(calls.barriers, before);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&cube, 0, true, 5, 1, false);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 1);
    teardown(&f);
}

TEST(nvn_a_dispatch_storing_through_global_addresses_waits_and_is_waited_for)
{
    static fixture f;
    setup(&f);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_buffer(0x200000, 0x100, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    unsigned before = calls.barriers;
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_anywhere();
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 1);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_buffer(0x900000, 0x100, false);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 2);
    teardown(&f);
}

TEST(nvn_a_dispatch_writing_an_image_through_a_register_handle_is_ordered_before_a_later_reader)
{
    CHECK(nvn_dispatch_unbounded(NVN_IFACE_BINDLESS));
    CHECK(nvn_dispatch_unbounded(NVN_IFACE_GLOBAL_MEMORY));
    CHECK(nvn_dispatch_unbounded(NVN_IFACE_GLOBAL_STORE));
    CHECK(!nvn_dispatch_unbounded(NVN_IFACE_TEXTURE));
    static fixture f;
    setup(&f);
    vimg img;
    array_image(&img, 0x100000);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_anywhere();
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    unsigned before = calls.barriers;
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_image(&img, 0, true, 0, 1, false);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 1);
    teardown(&f);
}

TEST(nvn_dispatches_run_one_after_another_unless_the_game_set_the_overlap_mode)
{
    static fixture f;
    setup(&f);
    f.st.compute_overlap = 0;
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_buffer(0x200000, 0x100, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    unsigned before = calls.barriers;
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_buffer(0x300000, 0x100, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 1);
    nvn_cmd mode = { .op = NVN_CMD_SET_COMPUTE_OVERLAP_MODE, .x = { 0, 1 } };
    CHECK_EQ(nvn_draw_cmd(&f.r, &mode), RL_OK);
    CHECK_EQ(f.st.compute_overlap, 1u);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_buffer(0x400000, 0x100, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 1);
    mode.x[1] = 0;
    CHECK_EQ(nvn_draw_cmd(&f.r, &mode), RL_OK);
    nvn_vk_dispatch_reset();
    nvn_vk_dispatch_buffer(0x500000, 0x100, true);
    CHECK(nvn_vk_begin_dispatch(&f.r));
    nvn_vk_dispatched();
    CHECK_EQ(calls.barriers, before + 2);
    teardown(&f);
}

TEST(nvn_a_clear_of_a_whole_one_level_target_writes_all_of_it)
{
    static fixture f;
    setup(&f);
    f.a.d.target = 1;
    f.a.d.levels = 1;
    CHECK(nvn_vk_clears_all(&f.r, &f.st.color[0]));
    f.st.scissor_set = true;
    f.st.scissor = (VkRect2D){ { 0, 0 }, { 64, 32 } };
    CHECK(nvn_vk_clears_all(&f.r, &f.st.color[0]));
    f.st.scissor.extent.width = 63;
    CHECK(!nvn_vk_clears_all(&f.r, &f.st.color[0]));
    f.st.scissor_set = false;
    f.st.ncolor = 2;
    f.b.d.width = 32;
    f.st.color[1] = (rt){ .v = &f.b, .view = (VkImageView)(uintptr_t)0x20 };
    CHECK(!nvn_vk_clears_all(&f.r, &f.st.color[0]));
    f.st.ncolor = 1;
    f.a.d.levels = 2;
    CHECK(!nvn_vk_clears_all(&f.r, &f.st.color[0]));
    teardown(&f);
}

TEST(nvn_a_target_with_its_sync_pending_is_brought_up_to_date_when_its_pass_begins)
{
    static fixture f;
    setup(&f);
    f.a.sync_pending = true;
    VkRect2D area;
    CHECK(nvn_vk_begin_rendering(&f.r, &area));
    CHECK(!f.a.sync_pending);
    CHECK_EQ(calls.begins, 1u);
    teardown(&f);
}

static unsigned g_clears;

static void fake_clear(VkCommandBuffer cb, uint32_t na, const VkClearAttachment *a, uint32_t nr, const VkClearRect *rr)
{
    (void)cb, (void)na, (void)a, (void)nr, (void)rr;
    g_clears++;
}

/**
 * @brief Clears obey the render enable as draws do: with rendering off (nvnCommandBufferSetRenderEnable false, or a
 * conditional render whose condition fails), a depth or color clear leaves its target as it was, so a game's pass
 * setup that clears a depth target it then tests EQUAL keeps the depth it copied there. With rendering on, both clear.
 */
TEST(nvn_clears_obey_the_render_enable)
{
    static fixture f;
    setup(&f);
    g.fns.f[VKG_vkCmdClearAttachments] = (void (*)(void))fake_clear;
    g_clears = 0;
    f.a.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    f.b.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    f.st.depth = (rt){ .v = &f.b, .view = (VkImageView)(uintptr_t)0x20 };
    static float rgba[4] = { 0, 0, 0, 0 };
    nvn_cmdset *s = calloc(1, sizeof *s + 2 * sizeof(nvn_cmd));
    s->n = 2;
    s->bytes = (uint8_t *)rgba;
    s->nbytes = sizeof rgba;
    nvn_cmd *depth = &s->cmds[0], *color = &s->cmds[1];
    float one = 1.0f;
    depth->op = NVN_CMD_CLEAR_DEPTH_STENCIL;
    depth->x[1] = 1;
    memcpy(&depth->d[0], &one, 4);
    color->op = NVN_CMD_CLEAR_COLOR;
    color->x[1] = 0;
    color->x[3] = 0xf;
    f.r.s = s;
    color->x[2] = NVN_SNAP;
    f.st.render_mode = NVN_RENDER_NEVER;
    CHECK_EQ(nvn_vk_cmd(&f.r, depth), RL_OK);
    CHECK_EQ(nvn_vk_cmd(&f.r, color), RL_OK);
    CHECK_EQ(g_clears, 0);
    f.st.render_mode = NVN_RENDER_ALWAYS;
    CHECK_EQ(nvn_vk_cmd(&f.r, depth), RL_OK);
    CHECK_EQ(g_clears, 1);
    CHECK_EQ(nvn_vk_cmd(&f.r, color), RL_OK);
    CHECK_EQ(g_clears, 2);
    teardown(&f);
    free(s);
}
