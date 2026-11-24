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
#include "nvn/nvn_module.h"
#include "nvn/nvn_sched.h"
#include "nvn/nvn_watch.h"
#include "tc.h"

#include <relift/ports.h>

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

TEST(nvn_clear_buffer_in_a_pool_fills_the_value_in_command_order)
{
    RECORDING(c, heap, cb);
    IMPORTED_POOL(c, heap, gpu, mem, nvn_clear_buffer_in_a_pool_fills_the_value_in_command_order);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferClearBuffer, cb, gpu + 0x100, 32, 0xa1b2c3d4), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferCopyBufferToBuffer, cb, gpu + 0x100, gpu + 0x200, 16), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferClearBuffer, cb, gpu + 0x110, 16, 0x01020304), RET);
    FINISHED(s, c, cb);
    CHECK_EQ(s->n, 3);
    memset(t_mem() + mem, 0, 0x1000);
    CHECK_EQ(nvn_vk_run(c, t_mem(), heap + 0x800, s), RL_OK);
    CHECK(nvn_vk_settle());
    for (unsigned i = 0; i < 16; i++) {
        CHECK_EQ(t_mem()[mem + 0x100 + i], (uint8_t)(0xa1b2c3d4u >> (8 * (i % 4))));
        CHECK_EQ(t_mem()[mem + 0x200 + i], (uint8_t)(0xa1b2c3d4u >> (8 * (i % 4))));
        CHECK_EQ(t_mem()[mem + 0x110 + i], (uint8_t)(0x01020304u >> (8 * (i % 4))));
    }
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
    CHECK(nvn_vk_settle());
    for (unsigned i = 0; i < 16; i++) {
        CHECK_EQ(t_mem()[mem + 0x200 + i], 0x10 + i);
        CHECK_EQ(t_mem()[mem + 0x100 + i], 0x40 + i);
    }
}

/** @brief Records into cb a wait for an event's value to equal value, or a signal writing it (the event is set later). */
static void record_event(rl_ctx *c, uint64_t cb, bool wait, uint32_t value)
{
    c->x[0] = cb;
    c->x[1] = 0;
    c->x[2] = wait ? NVN_EVENT_WAIT_EQUAL : NVN_EVENT_SIGNAL_WRITE;
    c->x[3] = value;
    c->x[4] = 0;
    c->x[5] = value;
    c->x[30] = RET;
    CHECK_EQ((wait ? nvnh_nvnCommandBufferWaitEvent : nvnh_nvnCommandBufferSignalEvent)(c, t_mem()), RET);
}

TEST(nvn_a_command_set_resumed_inside_another_run_is_submitted)
{
    RECORDING(c, heap, cb);
    IMPORTED_POOL(c, heap, gpu, mem, nvn_a_command_set_resumed_inside_another_run_is_submitted);
    uint64_t data = heap + 0x2000, word = mem + 0x800;
    for (unsigned i = 0; i < 16; i++)
        t_mem()[data + i] = (uint8_t)(0x60 + i);
    record_event(c, cb, true, 1);
    update_uniform_buffer(c, cb, gpu, 0x100, data, 16);
    FINISHED(waiting, c, cb);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    record_event(c, cb, false, 1);
    FINISHED(signaling, c, cb);
    /* No event object exists here: the commands get the event word nvn_event_word would give one. */
    ((nvn_cmd *)&waiting->cmds[0])->x[1] = word;
    ((nvn_cmd *)&signaling->cmds[0])->x[1] = word;
    memset(t_mem() + mem, 0, 0x1000);
    CHECK_EQ(nvn_vk_run(c, t_mem(), heap + 0x800, waiting), RL_OK);
    CHECK(nvn_vk_settle());
    CHECK_EQ(t_mem()[mem + 0x100], 0);
    CHECK_EQ(nvn_vk_run(c, t_mem(), heap + 0x900, signaling), RL_OK);
    CHECK(nvn_vk_settle());
    CHECK_EQ(rl_ld32(t_mem(), word), 1);
    for (unsigned i = 0; i < 16; i++)
        CHECK_EQ(t_mem()[mem + 0x100 + i], 0x60 + i);
}

/**
 * A command set queued behind an event by one guest thread and resumed when another thread's submit signals it runs
 * on the signaling thread's context: replay calls driver code and writes scratch below its context's stack pointer,
 * and the first thread may be running guest code on its own stack by then.
 */
TEST(nvn_a_run_resumed_by_another_threads_submit_replays_on_that_thread)
{
    RECORDING(c, heap, cb);
    IMPORTED_POOL(c, heap, gpu, mem, nvn_a_run_resumed_by_another_threads_submit_replays_on_that_thread);
    uint64_t data = heap + 0x2000, word = mem + 0x800;
    record_event(c, cb, true, 1);
    update_uniform_buffer(c, cb, gpu, 0x100, data, 16);
    FINISHED(waiting, c, cb);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    record_event(c, cb, false, 1);
    FINISHED(signaling, c, cb);
    ((nvn_cmd *)&waiting->cmds[0])->x[1] = word;
    ((nvn_cmd *)&signaling->cmds[0])->x[1] = word;
    memset(t_mem() + mem, 0, 0x1000);
    rl_ctx *other = calloc(1, sizeof *other);
    CHECK(other != NULL);
    memcpy(other, c, sizeof *other);
    other->sp = c->sp - 0x10000;
    CHECK_EQ(nvn_vk_run(c, t_mem(), heap + 0x800, waiting), RL_OK);
    CHECK(nvn_vk_settle());
    CHECK_EQ(nvn_vk_run(other, t_mem(), heap + 0x900, signaling), RL_OK);
    CHECK(nvn_vk_settle());
    CHECK_EQ(rl_ld32(t_mem(), word), 1);
    CHECK(nvn_vk_exec_ctx() == other);
    free(other);
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

TEST(nvn_integer_attributes_widen_to_four_words)
{
    const uint8_t b8[8] = { 0x80, 0x7f, 0xff, 0x00, 0x01, 0xfe, 0x02, 0x00 };
    uint32_t out[8];
    nvn_expand_integer(b8, 4, VK_FORMAT_R8G8B8_SINT, 2, out);
    const uint32_t w8[8] = { 0xffffff80u, 0x7fu, 0xffffffffu, 1u, 1u, 0xfffffffeu, 2u, 1u };
    CHECK(memcmp(out, w8, sizeof out) == 0);
    nvn_expand_integer(b8, 4, VK_FORMAT_R8G8_UINT, 2, out);
    const uint32_t u8w[8] = { 0x80u, 0x7fu, 0u, 1u, 1u, 0xfeu, 0u, 1u };
    CHECK(memcmp(out, u8w, sizeof out) == 0);
    const uint16_t b16[4] = { 0x8000, 0x1234, 0xffff, 0x0001 };
    nvn_expand_integer((const uint8_t *)b16, 4, VK_FORMAT_R16G16_SINT, 2, out);
    const uint32_t w16[8] = { 0xffff8000u, 0x1234u, 0u, 1u, 0xffffffffu, 1u, 0u, 1u };
    CHECK(memcmp(out, w16, sizeof out) == 0);
    nvn_expand_integer((const uint8_t *)b16, 4, VK_FORMAT_R16G16_UINT, 2, out);
    const uint32_t u16[8] = { 0x8000u, 0x1234u, 0u, 1u, 0xffffu, 1u, 0u, 1u };
    CHECK(memcmp(out, u16, sizeof out) == 0);
    const uint32_t b32[4] = { 0x80000000u, 7u, 0xdeadbeefu, 9u };
    nvn_expand_integer((const uint8_t *)b32, 16, VK_FORMAT_R32G32B32A32_SINT, 1, out);
    CHECK(memcmp(out, b32, 16) == 0);
}

TEST(nvn_vertices_and_indices_in_an_imported_pool_are_read_from_the_pool)
{
    RECORDING(c, heap, cb);
    (void)cb;
    IMPORTED_POOL(c, heap, gpu, mem, nvn_vertices_and_indices_in_an_imported_pool_are_read_from_the_pool);
    void *buf;
    uint64_t base;
    CHECK(nvn_guest_buffer(mem, 0x10000, &buf, &base));
    VkDeviceSize off = 0;
    CHECK(nvn_draw_pool_span(gpu + 0x200, 0x100, 4, &off) == (VkBuffer)buf);
    CHECK_EQ(off, base + 0x200);
    CHECK(nvn_draw_pool_span(gpu + 0x202, 0x100, 2, &off) == (VkBuffer)buf);
    CHECK_EQ(off, base + 0x202);
    CHECK(nvn_draw_pool_span(gpu + 0x202, 0x100, 4, &off) == NULL);
    CHECK(nvn_draw_pool_span(gpu + 0xff00, 0x200, 4, &off) == NULL);
    CHECK(nvn_draw_pool_span(gpu + 0x200, 0, 4, &off) == NULL);
}

static const uint32_t spv_bda_load[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x00000006, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x000014e3, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x000014e4, 0x00000001, 0x0005000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060010,
    0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x000100fd, 0x00010038,
};

static const uint32_t spv_bda_store[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x0000001c, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x000014e3, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e,
    0x000014e4, 0x00000001, 0x0007000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c,
    0x00000014, 0x00060010, 0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00030047,
    0x00000007, 0x00000002, 0x00050048, 0x00000007, 0x00000000, 0x00000023, 0x00000000, 0x00040047,
    0x00000009, 0x00000006, 0x00000004, 0x00030047, 0x0000000a, 0x00000002, 0x00050048, 0x0000000a,
    0x00000000, 0x00000023, 0x00000000, 0x00040047, 0x00000014, 0x0000000b, 0x0000001c, 0x00020013,
    0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030027, 0x00000006, 0x000014e5, 0x0003001e,
    0x00000007, 0x00000006, 0x00040015, 0x00000008, 0x00000020, 0x00000000, 0x0003001d, 0x00000009,
    0x00000008, 0x0003001e, 0x0000000a, 0x00000009, 0x00040020, 0x00000006, 0x000014e5, 0x0000000a,
    0x00040020, 0x0000000b, 0x00000009, 0x00000007, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000009,
    0x00040015, 0x0000000d, 0x00000020, 0x00000001, 0x0004002b, 0x0000000d, 0x0000000e, 0x00000000,
    0x00040020, 0x0000000f, 0x00000009, 0x00000006, 0x00040017, 0x00000012, 0x00000008, 0x00000003,
    0x00040020, 0x00000013, 0x00000001, 0x00000012, 0x0004003b, 0x00000013, 0x00000014, 0x00000001,
    0x0004002b, 0x00000008, 0x00000015, 0x00000000, 0x00040020, 0x00000016, 0x00000001, 0x00000008,
    0x0004002b, 0x00000008, 0x00000019, 0x00000007, 0x00040020, 0x0000001a, 0x000014e5, 0x00000008,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x00050041,
    0x0000000f, 0x00000010, 0x0000000c, 0x0000000e, 0x0004003d, 0x00000006, 0x00000011, 0x00000010,
    0x00050041, 0x00000016, 0x00000017, 0x00000014, 0x00000015, 0x0004003d, 0x00000008, 0x00000018,
    0x00000017, 0x00060041, 0x0000001a, 0x0000001b, 0x00000011, 0x0000000e, 0x00000018, 0x0005003e,
    0x0000001b, 0x00000019, 0x00000002, 0x00000004, 0x000100fd, 0x00010038,
};

static const uint32_t spv_image_store[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x00000018, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00000012, 0x00060010,
    0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00030047, 0x00000009, 0x00000019,
    0x00040047, 0x00000009, 0x00000021, 0x00000001, 0x00040047, 0x00000009, 0x00000022, 0x00000000,
    0x00040047, 0x0000000f, 0x00000006, 0x00000004, 0x00030047, 0x00000010, 0x00000002, 0x00040048,
    0x00000010, 0x00000000, 0x00000018, 0x00050048, 0x00000010, 0x00000000, 0x00000023, 0x00000000,
    0x00030047, 0x00000012, 0x00000018, 0x00040047, 0x00000012, 0x00000021, 0x00000000, 0x00040047,
    0x00000012, 0x00000022, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002,
    0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x00090019, 0x00000007, 0x00000006, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x00000002, 0x00000021, 0x00040020, 0x00000008, 0x00000000,
    0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000000, 0x00040015, 0x0000000b, 0x00000020,
    0x00000001, 0x00040017, 0x0000000c, 0x0000000b, 0x00000002, 0x0004002b, 0x0000000b, 0x0000000d,
    0x00000000, 0x0005002c, 0x0000000c, 0x0000000e, 0x0000000d, 0x0000000d, 0x0003001d, 0x0000000f,
    0x00000006, 0x0003001e, 0x00000010, 0x0000000f, 0x00040020, 0x00000011, 0x0000000c, 0x00000010,
    0x0004003b, 0x00000011, 0x00000012, 0x0000000c, 0x00040020, 0x00000013, 0x0000000c, 0x00000006,
    0x00040017, 0x00000016, 0x00000006, 0x00000004, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x00000007, 0x0000000a, 0x00000009, 0x00060041,
    0x00000013, 0x00000014, 0x00000012, 0x0000000d, 0x0000000d, 0x0004003d, 0x00000006, 0x00000015,
    0x00000014, 0x00070050, 0x00000016, 0x00000017, 0x00000015, 0x00000015, 0x00000015, 0x00000015,
    0x00050063, 0x0000000a, 0x0000000e, 0x00000017, 0x00002000, 0x000100fd, 0x00010038,
};

static const uint32_t spv_mesh_buffer[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x00000017, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000a, 0x0000000f, 0x00060010,
    0x00000004, 0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00040047, 0x00000007, 0x00000006,
    0x00000004, 0x00030047, 0x00000008, 0x00000002, 0x00050048, 0x00000008, 0x00000000, 0x00000023,
    0x00000000, 0x00040047, 0x0000000a, 0x00000021, 0x00000002, 0x00040047, 0x0000000a, 0x00000022,
    0x00000003, 0x00040047, 0x0000000f, 0x0000000b, 0x0000001c, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000000, 0x0003001d, 0x00000007,
    0x00000006, 0x0003001e, 0x00000008, 0x00000007, 0x00040020, 0x00000009, 0x0000000c, 0x00000008,
    0x0004003b, 0x00000009, 0x0000000a, 0x0000000c, 0x00040015, 0x0000000b, 0x00000020, 0x00000001,
    0x0004002b, 0x0000000b, 0x0000000c, 0x00000000, 0x00040017, 0x0000000d, 0x00000006, 0x00000003,
    0x00040020, 0x0000000e, 0x00000001, 0x0000000d, 0x0004003b, 0x0000000e, 0x0000000f, 0x00000001,
    0x0004002b, 0x00000006, 0x00000010, 0x00000000, 0x00040020, 0x00000011, 0x00000001, 0x00000006,
    0x0004002b, 0x00000006, 0x00000014, 0x00000001, 0x00040020, 0x00000015, 0x0000000c, 0x00000006,
    0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x00050041,
    0x00000011, 0x00000012, 0x0000000f, 0x00000010, 0x0004003d, 0x00000006, 0x00000013, 0x00000012,
    0x00060041, 0x00000015, 0x00000016, 0x0000000a, 0x0000000c, 0x00000013, 0x0003003e, 0x00000016,
    0x00000014, 0x000100fd, 0x00010038,
};

static const uint32_t spv_ssbo_atomic[] = {
    0x07230203, 0x00010600, 0x000d000b, 0x00000012, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0006000f, 0x00000005, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000a, 0x00060010, 0x00000004,
    0x00000011, 0x00000001, 0x00000001, 0x00000001, 0x00040047, 0x00000007, 0x00000006, 0x00000004,
    0x00030047, 0x00000008, 0x00000002, 0x00050048, 0x00000008, 0x00000000, 0x00000023, 0x00000000,
    0x00040047, 0x0000000a, 0x00000021, 0x00000000, 0x00040047, 0x0000000a, 0x00000022, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020,
    0x00000000, 0x0003001d, 0x00000007, 0x00000006, 0x0003001e, 0x00000008, 0x00000007, 0x00040020,
    0x00000009, 0x0000000c, 0x00000008, 0x0004003b, 0x00000009, 0x0000000a, 0x0000000c, 0x00040015,
    0x0000000b, 0x00000020, 0x00000001, 0x0004002b, 0x0000000b, 0x0000000c, 0x00000000, 0x00040020,
    0x0000000d, 0x0000000c, 0x00000006, 0x0004002b, 0x00000006, 0x0000000f, 0x00000001, 0x0004002b,
    0x00000006, 0x00000010, 0x00000000, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x00060041, 0x0000000d, 0x0000000e, 0x0000000a, 0x0000000c, 0x0000000c,
    0x000700ea, 0x00000006, 0x00000011, 0x0000000e, 0x0000000f, 0x00000010, 0x0000000f, 0x000100fd,
    0x00010038,
};

/**
 * @brief Records nvnCommandBufferCopyTextureToBuffer(cb, tex, no view, region, buffer, flags 0) as lifted code calls
 * it.
 */
static void copy_texture_to_buffer(rl_ctx *c, uint64_t cb, uint64_t tex, uint64_t region, uint64_t buffer)
{
    c->x[4] = buffer;
    c->x[5] = 0;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferCopyTextureToBuffer, cb, tex, 0, region), RET);
}

/**
 * The console's copy writes each row of the region's texels at the row stride and leaves the bytes between rows as
 * they are: a region put in a block-linear texture by CopyBufferToTexture comes back the same through
 * CopyTextureToBuffer, by a GPU copy into the pool (aligned) and by a readback (an address no Vulkan copy may start
 * at), and a cleared region comes back as the clear color's bytes.
 */
TEST(nvn_copy_texture_to_buffer_returns_the_texels_in_the_strides_rows)
{
    RECORDING(c, heap, cb);
    IMPORTED_POOL(c, heap, gpu, mem, nvn_copy_texture_to_buffer_returns_the_texels_in_the_strides_rows);
    nvn_tex_desc d = { .target = 1, .format = 37, .width = 16, .height = 8, .depth = 1, .levels = 1,
                       .swizzle = { 2, 3, 4, 5 }, .pool = heap + 0x100, .offset = 0x8000 };
    CHECK(nvn_tex_layout(&d));
    CHECK(d.offset + d.size <= 0x10000);
    uint64_t tex = heap + 0x3000, region = heap + 0x600, clear = heap + 0x700, color = heap + 0x780;
    nvn_region rg = { 2, 1, 0, 8, 4, 1 }, cr = { 0, 6, 0, 4, 2, 1 };
    memcpy(t_mem() + region, &rg, sizeof rg);
    memcpy(t_mem() + clear, &cr, sizeof cr);
    const float rgba[4] = { 1.0f, 0.0f, 0.0f, 1.0f };
    memcpy(t_mem() + color, rgba, sizeof rgba);
    CHECK(nvn_texture_record(tex, &d, NULL));
    uint8_t *m = t_mem() + mem;
    memset(m, 0xee, 0x8000);
    for (unsigned y = 0; y < 4; y++)
        for (unsigned i = 0; i < 32; i++)
            m[0x100 + 64 * y + i] = (uint8_t)(y * 32 + i + 1);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetCopyRowStride, cb, 64, 0, 0), RET);
    c->x[4] = region;
    c->x[5] = 0;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferCopyBufferToTexture, cb, gpu + 0x100, tex, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetCopyRowStride, cb, 48, 0, 0), RET);
    copy_texture_to_buffer(c, cb, tex, region, gpu + 0x1000);
    copy_texture_to_buffer(c, cb, tex, region, gpu + 0x2002);
    c->x[4] = color;
    c->x[5] = 0xf;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferClearTexture, cb, tex, 0, clear), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetCopyRowStride, cb, 0, 0, 0), RET);
    copy_texture_to_buffer(c, cb, tex, clear, gpu + 0x3000);
    FINISHED(s, c, cb);
    CHECK_EQ(s->n, 5);
    CHECK_EQ(nvn_vk_run(c, t_mem(), heap + 0x800, s), RL_OK);
    CHECK(nvn_vk_settle());
    for (unsigned y = 0; y < 4; y++) {
        for (unsigned i = 0; i < 32; i++) {
            CHECK_EQ(m[0x1000 + 48 * y + i], (uint8_t)(y * 32 + i + 1));
            CHECK_EQ(m[0x2002 + 48 * y + i], (uint8_t)(y * 32 + i + 1));
        }
        for (unsigned i = 32; i < 48; i++) {
            CHECK_EQ(m[0x1000 + 48 * y + i], 0xee);
            CHECK_EQ(m[0x2002 + 48 * y + i], 0xee);
        }
    }
    CHECK_EQ(m[0x1000 + 48 * 3 + 48], 0xee);
    CHECK_EQ(m[0x2001], 0xee);
    static const uint8_t red[4] = { 0xff, 0x00, 0x00, 0xff };
    for (unsigned i = 0; i < 2 * 4 * 4; i++)
        CHECK_EQ(m[0x3000 + i], red[i % 4]);
    CHECK_EQ(m[0x3000 + 32], 0xee);
}

/**
 * The guest write watch names what writes into its range: a write that meets it counts, one beside it does not, and a
 * memory pool made over watched bytes (whose device buffer the GPU writes in place) is one.
 */
TEST(nvn_guest_write_watch_counts_writes_into_its_range_and_pools_over_it)
{
    RECORDING(c, heap, cb);
    (void)cb;
    nvn_wwatch_set(heap + 0x100800, 8);
    CHECK(!nvn_wwatch_note(heap + 0x100808, 8, "a write beside it"));
    CHECK(!nvn_wwatch_note(heap + 0x1007f8, 8, "a write before it"));
    CHECK(nvn_wwatch_note(heap + 0x1007fc, 8, "a write over its start"));
    CHECK_EQ(nvn_wwatch_hits(), 1);
    CHECK(nvn_pool_make(heap + 0x100, heap + 0x100000, 0x10000, 0x18c));
    CHECK_EQ(nvn_wwatch_hits(), 2);
    nvn_wwatch_add(heap + 0x20000, 0x10);
    CHECK(nvn_wwatch_note(heap + 0x2000c, 4, "a write into a second range"));
    CHECK_EQ(nvn_wwatch_hits(), 3);
    nvn_wwatch_set(0, 0);
    CHECK(!nvn_wwatch_note(heap + 0x100800, 8, "a write with no watch"));
}

TEST(nvn_a_program_writes_guest_memory_only_when_it_stores_to_a_guest_buffer)
{
    CHECK(nvn_spirv_stores(spv_bda_store, sizeof spv_bda_store / 4));
    CHECK(!nvn_spirv_stores(spv_bda_load, sizeof spv_bda_load / 4));
    CHECK(nvn_spirv_stores(spv_ssbo_atomic, sizeof spv_ssbo_atomic / 4));
    CHECK(!nvn_spirv_stores(spv_image_store, sizeof spv_image_store / 4));
    CHECK(!nvn_spirv_stores(spv_mesh_buffer, sizeof spv_mesh_buffer / 4));
}

TEST(nvn_after_a_lost_device_a_stop_names_the_loss_and_its_fault_first)
{
    CHECK(nvn_vk_lost_text() == NULL);
    CHECK(strcmp(nvn_vk_fault_type(5), "a shader program counter, invalid") == 0);
    CHECK(strcmp(nvn_vk_fault_type(2), "an invalid write") == 0);
    CHECK(strcmp(nvn_vk_fault_type(99), "an unknown kind") == 0);
    const int types[1] = { 5 };
    const uint64_t addrs[1] = { 0xdf00519190ull };
    const char *const where[1] = { "0xffae6e70 before the span 0xe000000000+0x1f000000" };
    nvn_vk_lost_note("CTX SWITCH TIMEOUT", types, addrs, where, 1);
    const char *s = nvn_vk_lost_text();
    CHECK(s != NULL);
    CHECK(strstr(s, "the host device was lost: CTX SWITCH TIMEOUT") == s);
    CHECK(strstr(s, "a shader program counter, invalid 0xdf00519190") != NULL);
    CHECK(strstr(s, "before the span 0xe000000000") != NULL);
    nvn_vk_lost_note("a second loss", NULL, NULL, NULL, 0);
    CHECK(strstr(nvn_vk_lost_text(), "a second loss") == NULL);
}

/** @brief The GPU trace report shows 64 hits by default; RELIFT_NVN_GPU_TRACE_LINES and _PCS move the window. */
TEST(nvn_gpu_trace_report_window_comes_from_the_environment)
{
    uint32_t lines, from, to;
    unsetenv("RELIFT_NVN_GPU_TRACE_LINES");
    unsetenv("RELIFT_NVN_GPU_TRACE_PCS");
    trace_window(&lines, &from, &to);
    CHECK_EQ(lines, 64u);
    CHECK_EQ(from, 0u);
    CHECK_EQ(to, UINT32_MAX);
    setenv("RELIFT_NVN_GPU_TRACE_LINES", "500", 1);
    setenv("RELIFT_NVN_GPU_TRACE_PCS", "c00-ce0", 1);
    trace_window(&lines, &from, &to);
    CHECK_EQ(lines, 500u);
    CHECK_EQ(from, 0xc00u);
    CHECK_EQ(to, 0xce0u);
    setenv("RELIFT_NVN_GPU_TRACE_PCS", "0x62f0", 1);
    trace_window(&lines, &from, &to);
    CHECK_EQ(from, 0x62f0u);
    CHECK_EQ(to, 0x62f0u);
    unsetenv("RELIFT_NVN_GPU_TRACE_LINES");
    unsetenv("RELIFT_NVN_GPU_TRACE_PCS");
}
