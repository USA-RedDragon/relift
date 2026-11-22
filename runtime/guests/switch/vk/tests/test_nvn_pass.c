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

#include <string.h>

static struct {
    unsigned begins, ends, barriers;
    char order[64];
    unsigned n;
} calls;

static void note(char c)
{
    if (calls.n + 1 < sizeof calls.order)
        calls.order[calls.n++] = c;
}

static void fake_begin(VkCommandBuffer cb, const VkRenderingInfo *ri)
{
    (void)cb;
    (void)ri;
    calls.begins++;
    note('B');
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
    f->a.d.width = f->b.d.width = 64;
    f->a.d.height = f->b.d.height = 32;
    f->st.ncolor = 1;
    f->st.color[0] = (rt){ .v = &f->a, .view = (VkImageView)(uintptr_t)0x10 };
    f->r.st = &f->st;
    f->r.recording = true;
}

static void teardown(fixture *f)
{
    nvn_vk_end_rendering();
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
