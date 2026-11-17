/**
 * @file test_nvn.c
 * @brief relift's NVN2 procs called as lifted code calls them: texture
 * layout, device queries, syncs and command recording.
 */
#include "nvn/nvn_priv.h"
#include "tc.h"

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

TEST(nvn_block_linear_layout)
{
    nvn_tex_desc d = { .target = 1, .format = 0x38, .width = 1920, .height = 1080, .depth = 1, .levels = 1, .samples = 0 };
    CHECK(nvn_tex_layout(&d));
    CHECK_EQ(d.block_h, 16);
    CHECK_EQ(d.size, 7680ull * 1152);
    CHECK_EQ(d.align, 512);
    nvn_tex_desc m = { .target = 1, .format = 0x25, .width = 256, .height = 256, .levels = 3 };
    CHECK(nvn_tex_layout(&m));
    CHECK_EQ(m.level_offset[1], 1024ull * 256);
    CHECK_EQ(m.size, 1024ull * 256 + 512ull * 128 + 256ull * 64);
    nvn_tex_desc a = { .target = 4, .format = 0x25, .width = 16, .height = 16, .depth = 3, .levels = 1 };
    CHECK(nvn_tex_layout(&a));
    CHECK_EQ(a.block_h, 2);
    CHECK_EQ(a.layer_stride, 1024);
    CHECK_EQ(a.size, 3072);
    nvn_tex_desc bc = { .target = 1, .format = 0x41, .width = 64, .height = 64, .levels = 1 };
    CHECK(nvn_tex_layout(&bc));
    CHECK_EQ(bc.size, 128ull * 16);
    nvn_tex_desc lin = { .target = 1, .format = 0x25, .width = 100, .height = 10, .levels = 1, .flags = 1u << 4 };
    CHECK(nvn_tex_layout(&lin));
    CHECK_EQ(lin.align, 32);
    CHECK_EQ(lin.size, 416ull * 10);
    nvn_tex_desc bad = { .target = 1, .format = 0x7fff, .width = 1, .height = 1 };
    CHECK(!nvn_tex_layout(&bad));
}

TEST(nvn_device_queries_and_syncs)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 0, heap + 0x100, 0), RET);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x100), 231);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 0x3a, heap + 0x100, 0), RET);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x100), 65536);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetTextureHandle, heap, 5, 3, 0), RET);
    CHECK_EQ(c->x[0], 5 | 3ull << 20);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetImageHandle, heap, 7, 0, 0), RET);
    CHECK_EQ(c->x[0], 7);
    uint64_t sync = heap + 0x200;
    CHECK_EQ(call(c, nvnh_nvnSyncInitialize, sync, heap, 0, 0), RET);
    CHECK_EQ(c->x[0], 1);
    CHECK_EQ(call(c, nvnh_nvnSyncWait, sync, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 2);
    nvn_sync_signal(sync);
    CHECK_EQ(call(c, nvnh_nvnSyncWait, sync, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(call(c, nvnh_nvnSyncFinalize, sync, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnSyncWait, sync, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 3);
}

TEST(nvn_command_buffer_records_and_submits)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400, queue = heap + 0x800, handles = heap + 0x900;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, cb, heap, 0, 0), RET);
    CHECK_EQ(c->x[0], 1);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, cb, heap, 0, 0x4000), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetCommandMemoryFree, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0x4000);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    c->v[0].d[0] = 0x3ff0000000000000ull;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferClearColor, cb, 0, heap, 0xf), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetStencilRef, cb, 3, 0x80, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0), RET);
    uint64_t h = c->x[0];
    const nvn_cmdset *s = nvn_cmdset_of(h);
    CHECK(s != NULL);
    CHECK_EQ(s->n, 2);
    CHECK_EQ(s->cmds[0].op, NVN_CMD_CLEAR_COLOR);
    CHECK_EQ(s->cmds[0].x[3], 0xf);
    CHECK_EQ(s->cmds[0].d[0], 0x3ff0000000000000ull);
    CHECK_EQ(s->cmds[1].op, NVN_CMD_SET_STENCIL_REF);
    CHECK(nvn_cmdset_of(h + 0x100000) == NULL);
    CHECK(nvn_cmdset_of(5) == NULL);
    rl_st64(t_mem(), handles, h);
    CHECK_EQ(call(c, nvnh_nvnQueueSubmitCommands, queue, 1, handles, 0), RL_PENDING);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferFinalize, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RL_PENDING);
}

TEST(nvn_texels_land_in_gobs)
{
    nvn_tex_desc d = { .target = 1, .format = 0x25, .width = 64, .height = 64, .levels = 1 };
    CHECK(nvn_tex_layout(&d));
    static uint8_t storage[64 * 64 * 4], in[64 * 64 * 4], out[64 * 64 * 4];
    for (unsigned i = 0; i < sizeof in; i++)
        in[i] = (uint8_t)(i * 7 + i / 251);
    nvn_region all = { 0, 0, 0, 64, 64, 1 };
    CHECK(nvn_tex_copy(&d, storage, 0, 0, &all, in, true));
    CHECK_EQ(storage[16], in[256 * 1]);
    CHECK_EQ(storage[48], in[256 * 3]);
    CHECK_EQ(storage[64], in[16]);
    CHECK_EQ(storage[64 + 16], in[256 * 1 + 16]);
    CHECK_EQ(storage[128], in[256 * 4]);
    CHECK_EQ(storage[192 + 16 * 3 + 5], in[256 * 7 + 16 + 5]);
    CHECK_EQ(storage[256], in[32]);
    CHECK_EQ(storage[256 + 64 + 128], in[256 * 4 + 48]);
    CHECK_EQ(storage[512 * 8], in[256 * 64 / 4 * 0 + 64 * 4 * 0 + 64]);
    CHECK(nvn_tex_copy(&d, storage, 0, 0, &all, out, false));
    CHECK(memcmp(in, out, sizeof in) == 0);
    nvn_region part = { 8, 4, 0, 16, 8, 1 };
    uint8_t small[16 * 8 * 4];
    CHECK(nvn_tex_copy(&d, storage, 0, 0, &part, small, false));
    CHECK_EQ(small[0], in[(4 * 64 + 8) * 4]);
    CHECK_EQ(small[16 * 4 * 7 + 4 * 15 + 3], in[(11 * 64 + 23) * 4 + 3]);
    nvn_region outside = { 60, 0, 0, 8, 1, 1 };
    CHECK(!nvn_tex_copy(&d, storage, 0, 0, &outside, small, false));
}

/** @brief Where the driver's viewport puts clip-space y, as a memory row: rows counted from the bottom in lower-left mode. */
static float driver_row(uint32_t origin, float rows, const float v[4], float ndc_y)
{
    float scale = origin == NVN_ORIGIN_UPPER_LEFT ? -v[3] / 2 : v[3] / 2;
    float window = ndc_y * scale + v[1] + v[3] / 2;
    return origin == NVN_ORIGIN_UPPER_LEFT ? window : rows - window;
}

/** @brief Where a host viewport (x, y, width, height) puts clip-space y, as a framebuffer row (Vulkan's rule). */
static float host_row(const float vp[4], float ndc_y) { return ndc_y * vp[3] / 2 + vp[1] + vp[3] / 2; }

TEST(nvn_viewport_matches_driver_rows)
{
    static const float views[][4] = { { 0, 0, 1920, 1080 }, { 100, 40, 300, 200 }, { 0, 600, 800, 480 } };
    for (uint32_t origin = 0; origin < 2; origin++)
        for (unsigned i = 0; i < 3; i++)
            for (float y = -1; y <= 1; y += 0.5f) {
                float vp[4];
                nvn_window_viewport(origin, 1080, views[i], vp);
                CHECK_EQ((int64_t)(host_row(vp, y) * 4), (int64_t)(driver_row(origin, 1080, views[i], y) * 4));
            }
    float full[4];
    nvn_window_viewport(NVN_ORIGIN_UPPER_LEFT, 1080, views[0], full);
    CHECK_EQ((int64_t)host_row(full, 1), 0);
    CHECK_EQ((int64_t)host_row(full, -1), 1080);
    CHECK_EQ(nvn_window_top(NVN_ORIGIN_UPPER_LEFT, 1080, 40, 200), 40);
    CHECK_EQ(nvn_window_top(NVN_ORIGIN_LOWER_LEFT, 1080, 40, 200), 840);
}
