/**
 * @file test_nvn_replay.c
 * @brief One recorded NVN2 command at a time through the command-buffer
 * replay (nvn_draw_cmd), for the commands that change only the replay's
 * state and make no Vulkan call.
 */
#include "nvn_priv.h"
#include "nvn_vk.h"
#include "runner.h"

#include <stdlib.h>
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

TEST(nvn_replay_an_unhandled_command_is_not_a_raised_stop)
{
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_cmd e = { .op = NVN_CMD_NONE };
    int rc = replay_one(&st, &e);
    CHECK_EQ(rc, NVN_UNHANDLED);
    CHECK(rc != RL_PENDING);
    CHECK(rc != RL_OK);
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
    const uint32_t groups[3] = { 20, 12, 16 };
    uint8_t c0[NVN_BANK2_BYTES];
    uint32_t img_lo = 0, end = 0;
    CHECK(nvn_compute_table(&st, groups, 2, 5, 0, 1, 0xf8, c0, &img_lo, &end));
    CHECK_EQ(img_lo, 0xf0);
    CHECK_EQ(end, 0xf8);
    CHECK_EQ(c0[0], 20);
    CHECK_EQ(c0[4], 12);
    CHECK_EQ(c0[8], 16);
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
    CHECK(!nvn_compute_table(&st, groups, 2, 5, 0, 1, 0x80, c0, &img_lo, &end));
    st.push_used[NVN_STAGE_COMPUTE] = 3;
    CHECK(!nvn_compute_table(&st, groups, 2, 5, 0, 1, 0xf8, c0, &img_lo, &end));
}

TEST(nvn_graphics_table_puts_storage_buffers_where_the_drivers_table_does)
{
    static nvn_gfx st;
    memset(&st, 0, sizeof st);
    st.gstorage[0][0].addr = 0x1111;
    st.gstorage[0][0].size = 0x10;
    st.gstorage[0][3].addr = 0x2222;
    st.gstorage[0][3].size = 0x30;
    st.gstorage[1][1].addr = 0x3333;
    st.gstorage[1][1].size = 0x40;
    uint8_t c0[NVN_GFX_C0_BYTES];
    uint64_t a;
    uint32_t sz;
    nvn_graphics_table(&st, 0, c0);
    memcpy(&a, c0 + 0x110, 8);
    memcpy(&sz, c0 + 0x118, 4);
    CHECK_EQ(a, 0x1111);
    CHECK_EQ(sz, 0x10);
    memcpy(&a, c0 + 0x140, 8);
    CHECK_EQ(a, 0x2222);
    nvn_graphics_table(&st, 1, c0);
    memcpy(&a, c0 + 0x520, 8);
    memcpy(&sz, c0 + 0x528, 4);
    CHECK_EQ(a, 0x3333);
    CHECK_EQ(sz, 0x40);
    memcpy(&a, c0 + 0x110, 8);
    CHECK_EQ(a, 0);
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

/** @brief The command set of one 8 byte handle array, for the bulk binds that snapshot their array. */
static nvn_cmdset *handle_set(const uint64_t *v, uint32_t n)
{
    nvn_cmdset *s = calloc(1, sizeof *s);
    s->bytes = malloc(8u * n);
    memcpy(s->bytes, v, 8u * n);
    s->nbytes = 8u * n;
    return s;
}

static uint32_t word_at(const nvn_gfx *st, uint32_t stage, uint32_t at)
{
    uint32_t w;
    memcpy(&w, st->bank2[stage] + at, 4);
    return w;
}

TEST(nvn_replay_separate_binds_land_where_the_driver_writes_them)
{
    static nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_cmd t = { .op = NVN_CMD_BIND_SEPARATE_TEXTURE, .x = { 0, 4, 3, 0x1234 } };
    CHECK_EQ(replay_one(&st, &t), RL_OK);
    nvn_cmd m = { .op = NVN_CMD_BIND_SEPARATE_SAMPLER, .x = { 0, 4, 5, 7u << 20 } };
    CHECK_EQ(replay_one(&st, &m), RL_OK);
    CHECK_EQ(word_at(&st, 4, 0x168 + 8 * 3), 0x1234u);
    CHECK_EQ(word_at(&st, 4, 0x568 + 8 * 5), 7u << 20);
    CHECK_EQ(word_at(&st, 4, 0x20 + 8 * 3), 0u);
    CHECK_EQ(word_at(&st, 4, 0x120), 0u);
    CHECK_EQ(st.separate_bound[4][0][0], 1u << 3);
    CHECK_EQ(st.separate_bound[4][1][0], 1u << 5);
    CHECK_EQ(st.separate_bound[0][0][0], 0u);
    nvn_cmd last = { .op = NVN_CMD_BIND_SEPARATE_TEXTURE, .x = { 0, 0, 127, 9 } };
    CHECK_EQ(replay_one(&st, &last), RL_OK);
    CHECK_EQ(word_at(&st, 0, 0x168 + 8 * 127), 9u);
    CHECK_EQ(st.separate_bound[0][0][3], 1u << 31);
    CHECK(0x168 + 8 * 127 + 4 <= NVN_BANK2_BYTES);
    CHECK(0x568 + 8 * 127 + 4 <= NVN_BANK2_BYTES);
}

TEST(nvn_replay_bulk_separate_binds_bind_each_slot)
{
    static nvn_gfx st;
    memset(&st, 0, sizeof st);
    const uint64_t textures[3] = { 11, 12, 13 }, samplers[2] = { 3u << 20, 4u << 20 };
    nvn_cmdset *ts = handle_set(textures, 3), *ss = handle_set(samplers, 2);
    replay r;
    memset(&r, 0, sizeof r);
    r.st = &st;
    r.s = ts;
    nvn_cmd t = { .op = NVN_CMD_BIND_SEPARATE_TEXTURES, .x = { 0, 1, 2, 3, NVN_SNAP } };
    CHECK_EQ(nvn_draw_cmd(&r, &t), RL_OK);
    r.s = ss;
    nvn_cmd m = { .op = NVN_CMD_BIND_SEPARATE_SAMPLERS, .x = { 0, 1, 9, 2, NVN_SNAP } };
    CHECK_EQ(nvn_draw_cmd(&r, &m), RL_OK);
    for (uint32_t i = 0; i < 3; i++)
        CHECK_EQ(word_at(&st, 1, 0x168 + 8 * (2 + i)), 11 + i);
    CHECK_EQ(word_at(&st, 1, 0x168 + 8 * 1), 0u);
    CHECK_EQ(word_at(&st, 1, 0x168 + 8 * 5), 0u);
    CHECK_EQ(word_at(&st, 1, 0x568 + 8 * 9), 3u << 20);
    CHECK_EQ(word_at(&st, 1, 0x568 + 8 * 10), 4u << 20);
    CHECK_EQ(st.separate_bound[1][0][0], 0x1cu);
    CHECK_EQ(st.separate_bound[1][1][0], 0x600u);
    free(ts->bytes);
    free(ts);
    free(ss->bytes);
    free(ss);
}

TEST(nvn_replay_deferred_binds_read_their_array_when_replayed)
{
    static nvn_gfx st;
    static uint8_t mem[0x1000];
    memset(&st, 0, sizeof st);
    replay r;
    memset(&r, 0, sizeof r);
    r.st = &st;
    r.mem = mem;
    nvn_cmd t = { .op = NVN_CMD_BIND_TEXTURES_DEFERRED, .x = { 0, 4, 1, 2, 0x100 } };
    CHECK_EQ(word_at(&st, 4, 0x20 + 8 * 1), 0u);
    const uint64_t handles[2] = { 31, 32 };
    memcpy(mem + 0x100, handles, sizeof handles);
    CHECK_EQ(nvn_draw_cmd(&r, &t), RL_OK);
    CHECK_EQ(word_at(&st, 4, 0x20 + 8 * 1), 31u);
    CHECK_EQ(word_at(&st, 4, 0x20 + 8 * 2), 32u);
    CHECK_EQ(word_at(&st, 4, 0x20), 0u);
    nvn_cmd i = { .op = NVN_CMD_BIND_IMAGES_DEFERRED, .x = { 0, 4, 0, 2, 0x100 } };
    CHECK_EQ(nvn_draw_cmd(&r, &i), RL_OK);
    CHECK_EQ(word_at(&st, 4, 0x120), 31u);
    CHECK_EQ(word_at(&st, 4, 0x128), 32u);
    nvn_cmd c = { .op = NVN_CMD_BIND_IMAGES_DEFERRED, .x = { 0, NVN_STAGE_COMPUTE, 2, 2, 0x100 } };
    CHECK_EQ(nvn_draw_cmd(&r, &c), RL_OK);
    CHECK_EQ(st.image[NVN_STAGE_COMPUTE][2], 31u);
    CHECK_EQ(st.image[NVN_STAGE_COMPUTE][3], 32u);
    nvn_cmd ts = { .op = NVN_CMD_BIND_SEPARATE_TEXTURES_DEFERRED, .x = { 0, 4, 6, 2, 0x100 } };
    CHECK_EQ(nvn_draw_cmd(&r, &ts), RL_OK);
    CHECK_EQ(word_at(&st, 4, 0x168 + 8 * 6), 31u);
    CHECK_EQ(word_at(&st, 4, 0x168 + 8 * 7), 32u);
    CHECK_EQ(st.separate_bound[4][0][0], 0xc0u);
    nvn_cmd ms = { .op = NVN_CMD_BIND_SEPARATE_SAMPLERS_DEFERRED, .x = { 0, 4, 0, 2, 0x100 } };
    CHECK_EQ(nvn_draw_cmd(&r, &ms), RL_OK);
    CHECK_EQ(word_at(&st, 4, 0x568), 31u);
    CHECK_EQ(st.separate_bound[4][1][0], 3u);
    uint64_t ranges[4] = { 0x5000, 0x40, 0x6000, 0x80 };
    memcpy(mem + 0x200, ranges, sizeof ranges);
    nvn_cmd ub = { .op = NVN_CMD_BIND_UNIFORM_BUFFERS_DEFERRED, .x = { 0, 1, 3, 2, 0x200 } };
    CHECK_EQ(nvn_draw_cmd(&r, &ub), RL_OK);
    CHECK_EQ(st.ubo[1][3].addr, 0x5000u);
    CHECK_EQ(st.ubo[1][3].size, 0x40u);
    CHECK_EQ(st.ubo[1][4].addr, 0x6000u);
    CHECK_EQ(st.ubo[1][4].size, 0x80u);
    nvn_cmd sb = { .op = NVN_CMD_BIND_STORAGE_BUFFERS_DEFERRED, .x = { 0, NVN_STAGE_COMPUTE, 1, 2, 0x200 } };
    CHECK_EQ(nvn_draw_cmd(&r, &sb), RL_OK);
    CHECK_EQ(st.storage[1].addr, 0x5000u);
    CHECK_EQ(st.storage[2].size, 0x80u);
    nvn_cmd none = { .op = NVN_CMD_BIND_TEXTURES_DEFERRED, .x = { 0, 4, 0, 0, 0 } };
    CHECK_EQ(nvn_draw_cmd(&r, &none), RL_OK);
}

TEST(nvn_separate_handles_of_unregistered_textures_read_a_null_texture)
{
    nvn_probes probes;
    if (!nvn_vk_probes(&probes))
        return;
    CHECK(nvn_texset_layout());
    static nvn_gfx st;
    memset(&st, 0, sizeof st);
    uint32_t tex = 9, smp = 3u << 20;
    memcpy(st.bank2_host[4] + NVN_BANK2_SEPARATE_TEXTURES + 8, &tex, 4);
    memcpy(st.bank2_host[4] + NVN_BANK2_SEPARATE_SAMPLERS, &smp, 4);
    st.separate_bound[4][0][0] = 1u << 1;
    st.separate_bound[4][1][0] = 1u;
    replay r;
    memset(&r, 0, sizeof r);
    r.st = &st;
    CHECK_EQ(nvn_separate_handles(&r, 4), RL_OK);
    uint32_t w;
    memcpy(&w, st.bank2_host[4] + NVN_BANK2_SEPARATE_TEXTURES + 8, 4);
    CHECK_EQ(w, 0u);
    memcpy(&w, st.bank2_host[4] + NVN_BANK2_SEPARATE_SAMPLERS, 4);
    CHECK_EQ(w, smp);
}

TEST(nvn_replay_binds_reach_the_task_and_mesh_stages)
{
    static nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_cmd t = { .op = NVN_CMD_BIND_TEXTURE, .x = { 0, NVN_STAGE_MESH, 6, 0x777 } };
    CHECK_EQ(replay_one(&st, &t), RL_OK);
    CHECK_EQ(word_at(&st, NVN_STAGE_MESH, 0x20 + 8 * 6), 0x777u);
    CHECK_EQ(word_at(&st, NVN_STAGE_TASK, 0x20 + 8 * 6), 0u);
    nvn_cmd s = { .op = NVN_CMD_BIND_STORAGE_BUFFER, .x = { 0, NVN_STAGE_TASK, 0, 0x4000, 0x80 } };
    CHECK_EQ(replay_one(&st, &s), RL_OK);
    nvn_cmd u = { .op = NVN_CMD_BIND_UNIFORM_BUFFER, .x = { 0, NVN_STAGE_MESH, 2, 0x5000, 0x40 } };
    CHECK_EQ(replay_one(&st, &u), RL_OK);
    CHECK_EQ(st.ubo[NVN_STAGE_MESH][2].addr, 0x5000u);
    uint8_t c0[NVN_GFX_C0_BYTES];
    uint64_t a;
    uint32_t sz;
    nvn_graphics_table(&st, NVN_STAGE_TASK, c0);
    memcpy(&a, c0 + 0x670, 8);
    memcpy(&sz, c0 + 0x678, 4);
    CHECK_EQ(a, 0x4000u);
    CHECK_EQ(sz, 0x80u);
}

TEST(nvn_a_mesh_pipeline_reads_back_from_its_interface_records)
{
    nvn_iface_rec recs[] = {
        { NVN_IFACE_TEXTURE, 2, 0x20, 0 },
        { NVN_IFACE_MESH, 7 | 1u << 8 | 1u << 9, 2, 3 },
        { NVN_IFACE_MESH_LIMITS, 64, 126, 36 },
        { NVN_IFACE_MESH_RECORD, 500, 378, 32 | 32u << 16 },
    };
    nvn_mesh_info m;
    CHECK(nvn_mesh_info_of(recs, 4, &m));
    CHECK(m.stage == 7 && m.native && m.indexed);
    CHECK(m.slots == 2 && m.topology == 3);
    CHECK(m.max_vertices == 64 && m.max_primitives == 126 && m.payload_words == 36);
    CHECK(m.record_words == 500 && m.index_words == 378 && m.task_lanes == 32 && m.mesh_lanes == 32);
    CHECK(!nvn_mesh_info_of(recs, 1, &m));
}
