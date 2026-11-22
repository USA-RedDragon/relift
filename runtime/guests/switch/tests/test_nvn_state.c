/**
 * @file test_nvn_state.c
 * @brief The command buffer state setters that record into relift's command
 * stream and what replay keeps of them: the sample mask, alpha reference,
 * point size, render enable, tessellation levels, viewport swizzles,
 * conservative dilate and provoking vertex. Each test records the procs as
 * lifted code calls them, then replays the command set onto a queue's draw
 * state (no host device needed: the setters only change state).
 */
#include "nvn/nvn_priv.h"
#include "nvn/nvn_vk.h"
#include "tc.h"

#include <relift/ports.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define RET 0x7000000ull

static uint64_t call(rl_ctx *c, rl_fn_t *fn, uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3)
{
    c->x[0] = x0;
    c->x[1] = x1;
    c->x[2] = x2;
    c->x[3] = x3;
    c->x[30] = RET;
    return fn(c, t_mem());
}

static uint64_t bits(float x)
{
    uint32_t v;
    memcpy(&v, &x, sizeof v);
    return v;
}

/** @brief Declares c, heap and cb: a context with a command buffer recording. */
#define RECORDING(c, heap, cb)                                                                                         \
    rl_ctx *c = t_boot(NULL);                                                                                          \
    uint64_t heap = t_heap(c, 0x200000), cb = heap + 0x400;                                                           \
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, cb, heap, 0, 0), RET);                                           \
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, cb, heap, 0, 0x4000), RET);                                \
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET)

/** @brief Declares s: the command set recording ended with. */
#define FINISHED(s, c, cb)                                                                                             \
    CHECK_EQ(call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0), RET);                                            \
    const nvn_cmdset *s = nvn_cmdset_of(c->x[0]);                                                                      \
    CHECK(s != NULL)

/** @brief Replays every command of s onto st. */
static void replay_state(const nvn_cmdset *s, nvn_gfx *st)
{
    replay r = { .s = s, .st = st };
    for (uint32_t i = 0; i < s->n; i++)
        CHECK_EQ(nvn_draw_cmd(&r, &s->cmds[i]), RL_OK);
}

TEST(nvn_sample_mask_alpha_ref_and_point_size_record_their_values)
{
    RECORDING(c, heap, cb);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetSampleMask, cb, 0x1ffff5, 0, 0), RET);
    c->v[0].d[0] = bits(0.5f);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetAlphaRef, cb, 0, 0, 0), RET);
    c->v[0].d[0] = bits(4.0f);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetPointSize, cb, 0, 0, 0), RET);
    FINISHED(s, c, cb);
    CHECK_EQ(s->n, 3);
    CHECK_EQ(s->cmds[0].op, NVN_CMD_SET_SAMPLE_MASK);
    CHECK_EQ(s->cmds[1].op, NVN_CMD_SET_ALPHA_REF);
    CHECK_EQ(s->cmds[2].op, NVN_CMD_SET_POINT_SIZE);
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_gfx_init(&st);
    CHECK_EQ(st.sample_mask, 0xffff);
    CHECK_EQ(bits(st.point_size), bits(1.0f));
    CHECK_EQ(bits(st.alpha_ref), bits(0.0f));
    replay_state(s, &st);
    CHECK_EQ(st.sample_mask, 0xfff5);
    CHECK_EQ(bits(st.alpha_ref), bits(0.5f));
    CHECK_EQ(bits(st.point_size), bits(4.0f));
}

TEST(nvn_render_enable_follows_the_last_of_enable_and_conditional)
{
    RECORDING(c, heap, cb);
    uint64_t counters = heap + 0x1000;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetRenderEnable, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetRenderEnableConditional, cb, 1, counters, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetRenderEnableConditional, cb, 7, counters + 64, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetRenderEnable, cb, 1, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetRenderEnableConditional, cb, 0, counters, 0), RET);
    FINISHED(s, c, cb);
    CHECK_EQ(s->n, 5);
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_gfx_init(&st);
    CHECK_EQ(st.render_mode, NVN_RENDER_ALWAYS);
    replay r = { .s = s, .st = &st };
    CHECK_EQ(nvn_draw_cmd(&r, &s->cmds[0]), RL_OK);
    CHECK_EQ(st.render_mode, NVN_RENDER_NEVER);
    bool run = true;
    CHECK_EQ(nvn_draw_render_enabled(&r, &run), RL_OK);
    CHECK(!run);
    CHECK_EQ(nvn_draw_cmd(&r, &s->cmds[1]), RL_OK);
    CHECK_EQ(st.render_mode, NVN_RENDER_IF_NOT_EQUAL);
    CHECK_EQ(st.render_addr, counters);
    CHECK_EQ(nvn_draw_cmd(&r, &s->cmds[2]), RL_OK);
    CHECK_EQ(st.render_mode, NVN_RENDER_IF_NOT_EQUAL);
    CHECK_EQ(st.render_addr, counters);
    CHECK_EQ(nvn_draw_cmd(&r, &s->cmds[3]), RL_OK);
    CHECK_EQ(st.render_mode, NVN_RENDER_ALWAYS);
    CHECK_EQ(nvn_draw_cmd(&r, &s->cmds[4]), RL_OK);
    CHECK_EQ(st.render_mode, NVN_RENDER_IF_EQUAL);
}

TEST(nvn_conditional_render_compares_the_two_counters)
{
    RECORDING(c, heap, cb);
    uint64_t counters = heap + 0x1000;
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_gfx_init(&st);
    replay r = { .c = c, .mem = t_mem(), .st = &st };
    st.render_addr = counters;
    bool run;
    rl_st64(t_mem(), counters, 0x1234);
    rl_st64(t_mem(), counters + 8, 111);
    rl_st64(t_mem(), counters + 16, 0x1234);
    rl_st64(t_mem(), counters + 24, 222);
    st.render_mode = NVN_RENDER_IF_EQUAL;
    CHECK_EQ(nvn_draw_render_enabled(&r, &run), RL_OK);
    CHECK(run);
    st.render_mode = NVN_RENDER_IF_NOT_EQUAL;
    CHECK_EQ(nvn_draw_render_enabled(&r, &run), RL_OK);
    CHECK(!run);
    rl_st64(t_mem(), counters + 16, 0);
    CHECK_EQ(nvn_draw_render_enabled(&r, &run), RL_OK);
    CHECK(run);
    st.render_mode = NVN_RENDER_IF_EQUAL;
    CHECK_EQ(nvn_draw_render_enabled(&r, &run), RL_OK);
    CHECK(!run);
    st.render_mode = NVN_RENDER_ALWAYS;
    CHECK_EQ(nvn_draw_render_enabled(&r, &run), RL_OK);
    CHECK(run);
}

TEST(nvn_a_draw_with_render_disabled_does_nothing)
{
    RECORDING(c, heap, cb);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetRenderEnable, cb, 0, 0, 0), RET);
    c->x[4] = 3;
    c->x[5] = 0;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferDrawArrays, cb, 4, 0, 3), RET);
    FINISHED(s, c, cb);
    CHECK_EQ(s->n, 2);
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_gfx_init(&st);
    replay_state(s, &st);
    CHECK_EQ(st.render_mode, NVN_RENDER_NEVER);
}

TEST(nvn_tessellation_levels_and_viewport_swizzles_keep_the_arrays_they_point_at)
{
    RECORDING(c, heap, cb);
    float inner[2] = { 2.0f, 3.0f }, outer[4] = { 4.0f, 5.0f, 6.0f, 7.0f };
    memcpy(t_mem() + heap + 0x2000, inner, sizeof inner);
    memcpy(t_mem() + heap + 0x2100, outer, sizeof outer);
    uint32_t sw[8] = { 1, 2, 4, 6, 0, 3, 5, 7 };
    memcpy(t_mem() + heap + 0x2200, sw, sizeof sw);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetInnerTessellationLevels, cb, heap + 0x2000, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetOuterTessellationLevels, cb, heap + 0x2100, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetViewportSwizzles, cb, 1, 2, heap + 0x2200), RET);
    memset(t_mem() + heap + 0x2000, 0, 0x300);
    FINISHED(s, c, cb);
    CHECK_EQ(s->n, 3);
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_gfx_init(&st);
    CHECK_EQ(bits(st.tess_inner[0]), bits(1.0f));
    CHECK_EQ(bits(st.tess_outer[3]), bits(1.0f));
    replay_state(s, &st);
    CHECK_EQ(bits(st.tess_inner[0]), bits(2.0f));
    CHECK_EQ(bits(st.tess_inner[1]), bits(3.0f));
    CHECK_EQ(bits(st.tess_outer[0]), bits(4.0f));
    CHECK_EQ(bits(st.tess_outer[3]), bits(7.0f));
    CHECK_EQ(st.viewport_swizzle[0], NVN_VIEWPORT_SWIZZLE_IDENTITY);
    CHECK_EQ(st.viewport_swizzle[1], 1u | 2u << 4 | 4u << 8 | 6u << 12);
    CHECK_EQ(st.viewport_swizzle[2], 0u | 3u << 4 | 5u << 8 | 7u << 12);
    CHECK_EQ(st.viewport_swizzle[3], NVN_VIEWPORT_SWIZZLE_IDENTITY);
}

TEST(nvn_conservative_dilate_is_quarter_pixels_to_three)
{
    static const struct {
        float dilate;
        uint32_t quarters;
    } cases[] = { { 0.0f, 0 }, { -1.0f, 0 }, { 0.1f, 1 }, { 0.25f, 1 }, { 0.26f, 2 }, { 0.5f, 2 },
                  { 0.75f, 3 }, { 2.0f, 3 }, { NAN, 0 } };
    const unsigned n = sizeof cases / sizeof cases[0];
    RECORDING(c, heap, cb);
    for (unsigned i = 0; i < n; i++) {
        c->v[0].d[0] = bits(cases[i].dilate);
        CHECK_EQ(call(c, nvnh_nvnCommandBufferSetConservativeRasterDilate, cb, 0, 0, 0), RET);
    }
    FINISHED(s, c, cb);
    CHECK_EQ(s->n, n);
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_gfx_init(&st);
    replay r = { .s = s, .st = &st };
    for (unsigned i = 0; i < n; i++) {
        CHECK_EQ(nvn_draw_cmd(&r, &s->cmds[i]), RL_OK);
        CHECK_EQ(st.conservative_dilate, cases[i].quarters);
    }
}

TEST(nvn_provoking_vertex_and_the_no_effect_setters_replay)
{
    RECORDING(c, heap, cb);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetProvokingVertexMode, cb, 1, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetPatchSize, cb, 3, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetGeometryProcessingMode, cb, 1, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBindCoverageModulationTable, cb, heap + 0x2000, 0, 0), RET);
    FINISHED(s, c, cb);
    CHECK_EQ(s->n, 4);
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_gfx_init(&st);
    CHECK(!st.provoking_last);
    replay_state(s, &st);
    CHECK(st.provoking_last);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetProvokingVertexMode, cb, 0, 0, 0), RET);
    FINISHED(s2, c, cb);
    replay_state(s2, &st);
    CHECK(!st.provoking_last);
}

TEST(nvn_debug_group_procs_for_either_engine_do_nothing)
{
    static const char *const zero[] = {
#define NVN_NAME(n) #n,
        NVN_ZERO_PROCS(NVN_NAME)
#undef NVN_NAME
    };
    static const char *const want[] = { "nvnCommandBufferPushDebugGroupCompute", "nvnCommandBufferPushDebugGroupGraphics",
                                        "nvnCommandBufferPopDebugGroupCompute", "nvnCommandBufferPopDebugGroupGraphics" };
    for (unsigned i = 0; i < sizeof want / sizeof want[0]; i++) {
        bool found = false;
        for (unsigned j = 0; j < sizeof zero / sizeof zero[0]; j++)
            found |= !strcmp(zero[j], want[i]);
        CHECK(found);
    }
}

/** @brief Declares gpu and mem: a memory pool imported into the host device, or returns when there is none. */
#define IMPORTED_POOL(c, heap, gpu, mem, name)                                                                         \
    uint64_t mem = (heap) + 0x100000, gpu;                                                                             \
    do {                                                                                                               \
        if (!nvn_pool_make((heap) + 0x100, mem, 0x10000, 0x18c) || (gpu = nvn_pool_gpu((heap) + 0x100)) == mem) {      \
            fprintf(stderr, #name ": no host device that imports guest memory, skipped\n");                            \
            return;                                                                                                    \
        }                                                                                                              \
    } while (0)

static void update_uniform_buffer(rl_ctx *c, uint64_t cb, uint64_t gpu, uint64_t offset, uint64_t data, uint64_t size)
{
    c->x[0] = cb;
    c->x[1] = gpu;
    c->x[2] = 0x1000;
    c->x[3] = offset;
    c->x[4] = size;
    c->x[5] = data;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferUpdateUniformBuffer(c, t_mem()), RET);
}

TEST(nvn_update_uniform_buffer_in_a_pool_lands_in_the_order_the_commands_run)
{
    RECORDING(c, heap, cb);
    IMPORTED_POOL(c, heap, gpu, mem, nvn_update_uniform_buffer_in_a_pool_lands_in_the_order_the_commands_run);
    uint64_t data = heap + 0x2000;
    for (unsigned i = 0; i < 16; i++) {
        t_mem()[data + i] = (uint8_t)(0x10 + i);
        t_mem()[data + 16 + i] = (uint8_t)(0x40 + i);
    }
    update_uniform_buffer(c, cb, gpu, 0x100, data, 16);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferCopyBufferToBuffer, cb, gpu + 0x100, gpu + 0x200, 16), RET);
    update_uniform_buffer(c, cb, gpu, 0x100, data + 16, 16);
    FINISHED(s, c, cb);
    CHECK_EQ(s->n, 3);
    memset(t_mem() + mem, 0, 0x1000);
    CHECK_EQ(nvn_vk_run(c, t_mem(), heap + 0x800, s), RL_OK);
    for (unsigned i = 0; i < 16; i++) {
        CHECK_EQ(t_mem()[mem + 0x200 + i], 0x10 + i);
        CHECK_EQ(t_mem()[mem + 0x100 + i], 0x40 + i);
    }
}

TEST(nvn_a_square_2d_array_of_six_layers_is_also_sampled_as_a_cube)
{
    enum { DIM_CUBE = 3, DIM_2D_ARRAY = 5, DIM_CUBE_ARRAY = 6 };
    uint32_t cube = 1u << DIM_CUBE, array = 1u << DIM_2D_ARRAY, cubes = 1u << DIM_CUBE_ARRAY;
    CHECK_EQ(nvn_texset_fit(DIM_2D_ARRAY, 64, 64, 6) & (cube | cubes), cube | cubes);
    CHECK_EQ(nvn_texset_fit(DIM_2D_ARRAY, 64, 64, 12) & (cube | cubes), cube | cubes);
    CHECK_EQ(nvn_texset_fit(DIM_2D_ARRAY, 64, 64, 8) & (cube | cubes), cube);
    CHECK_EQ(nvn_texset_fit(DIM_2D_ARRAY, 64, 64, 5) & (cube | cubes), 0u);
    CHECK_EQ(nvn_texset_fit(DIM_2D_ARRAY, 64, 32, 6) & (cube | cubes), 0u);
    CHECK(nvn_texset_fit(DIM_2D_ARRAY, 64, 32, 1) & array);
}

TEST(nvn_dispatch_compute_indirect_takes_its_counts_from_the_pool_buffer_on_the_gpu)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t pool = heap, storage = heap + 0x100000;
    CHECK(nvn_pool_make(pool, storage, 0x40000, 0x18c));
    uint64_t gpu = nvn_pool_gpu(pool);
    if (gpu == storage)
        return;
    void *buf = NULL;
    uint64_t off = 0;
    CHECK(nvn_guest_buffer(nvn_gpu_guest(gpu + 0x40), 12, &buf, &off));
    CHECK(buf != NULL);
    CHECK_EQ(off, 0x40);
    CHECK(!nvn_guest_buffer(nvn_gpu_guest(gpu + 0x3fffc), 12, &buf, &off));
    static nvn_gfx st;
    memset(&st, 0, sizeof st);
    replay r;
    memset(&r, 0, sizeof r);
    r.c = c;
    r.mem = t_mem();
    r.st = &st;
    nvn_cmd e = { .op = NVN_CMD_DISPATCH_COMPUTE_INDIRECT, .x = { 0, gpu + 0x40 } };
    CHECK_EQ(nvn_draw_cmd(&r, &e), RL_PENDING);
    e.x[1] = gpu + 0x3fffc;
    CHECK_EQ(nvn_draw_cmd(&r, &e), RL_PENDING);
}

TEST(nvn_indirect_mesh_draws_stop_when_their_records_or_count_leave_the_pool)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t pool = heap, storage = heap + 0x100000;
    CHECK(nvn_pool_make(pool, storage, 0x40000, 0x18c));
    uint64_t gpu = nvn_pool_gpu(pool);
    if (gpu == storage)
        return;
    static nvn_gfx st;
    memset(&st, 0, sizeof st);
    st.render_mode = NVN_RENDER_ALWAYS;
    replay r;
    memset(&r, 0, sizeof r);
    r.c = c;
    r.mem = t_mem();
    r.st = &st;
    rl_st32(t_mem(), storage + 0x40, 0);
    nvn_cmd e = { .op = NVN_CMD_MULTI_DRAW_MESH_TASKS_INDIRECT_COUNT, .x = { 0, gpu + 0x100, gpu + 0x40, 4, 16 } };
    CHECK_EQ(nvn_draw_cmd(&r, &e), RL_OK);
    e.x[2] = gpu + 0x40000;
    CHECK_EQ(nvn_draw_cmd(&r, &e), RL_PENDING);
    nvn_cmd m = { .op = NVN_CMD_MULTI_DRAW_MESH_TASKS_INDIRECT, .x = { 0, gpu + 0x3fff0, 4, 16 } };
    CHECK_EQ(nvn_draw_cmd(&r, &m), RL_PENDING);
}
