/**
 * @file test_nvn.c
 * @brief relift's NVN2 procs called as lifted code calls them: texture
 * layout, device queries, syncs and command recording.
 */
#include "nvn/nvn_priv.h"
#include "nvn/nvn_slots.h"
#include "nvn/nvn_watch.h"
#include "tc.h"
#include "vi.h"

#include <relift/ports.h>

#include <ftw.h>
#include <limits.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

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

TEST(nvn_warm_makes_the_rest_of_the_table_after_the_record_in_the_background)
{
    nvn_warm_plan p = nvn_warm_plan_for(NULL, 1100, 12000);
    CHECK_EQ(p.total, 1100u);
    CHECK_EQ(p.extra, 12000u);
    CHECK(p.hold);
    CHECK(!p.table_counted);
    p = nvn_warm_plan_for("registered", 1100, 12000);
    CHECK_EQ(p.total, 1100u);
    CHECK_EQ(p.extra, 12000u);
    p = nvn_warm_plan_for(NULL, 0, 12000);
    CHECK_EQ(p.total, 12000u);
    CHECK_EQ(p.extra, 0u);
    CHECK(p.table_counted);
    p = nvn_warm_plan_for("all", 1100, 12000);
    CHECK_EQ(p.total, 13100u);
    CHECK_EQ(p.extra, 0u);
    CHECK(p.hold);
    p = nvn_warm_plan_for("all-background", 1100, 12000);
    CHECK_EQ(p.total, 13100u);
    CHECK(!p.hold);
    p = nvn_warm_plan_for("off", 1100, 12000);
    CHECK_EQ(p.total, 0u);
    CHECK_EQ(p.extra, 0u);
}

TEST(nvn_warm_warns_when_mappings_near_the_limit)
{
    char msg[512] = "untouched";
    CHECK(!nvn_warm_maps_note(4000, 65530, 0, msg, sizeof msg));
    CHECK(!nvn_warm_maps_note(49146, 65530, 0, msg, sizeof msg));
    CHECK(!nvn_warm_maps_note(60000, 0, 0, msg, sizeof msg));
    CHECK(!strcmp(msg, "untouched"));
    CHECK(nvn_warm_maps_note(49148, 65530, 0, msg, sizeof msg));
    CHECK(strstr(msg, "49148 memory mappings of the 65530 vm.max_map_count allows") != NULL);
    CHECK(strstr(msg, "RELIFT_WARM_THREADS") == NULL);
    CHECK(nvn_warm_maps_note(60272, 65530, 6, msg, sizeof msg));
    CHECK(strstr(msg, "RELIFT_WARM_THREADS=6") != NULL);
    CHECK(strstr(msg, "skips the memory rule") != NULL);
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
    nvn_tex_desc astc = { .target = 4, .format = 123, .width = 64, .height = 56, .depth = 3, .levels = 7, .flags = 4 };
    CHECK(nvn_tex_layout(&astc));
    CHECK_EQ(astc.block_h, 2);
    CHECK_EQ(astc.size, 0x6000);
    CHECK(nvn_tex_layout_block(&astc, 1));
    CHECK_EQ(astc.block_h, 1);
    CHECK_EQ(astc.layer_stride, 0x1e00);
    CHECK_EQ(astc.size, 0x5a00);
    nvn_tex_desc lut = { .target = 2, .format = 0x25, .width = 32, .height = 32, .depth = 32, .levels = 1 };
    CHECK(nvn_tex_layout(&lut));
    CHECK_EQ(lut.block_h, 1);
    CHECK_EQ(lut.block_d, 16);
    CHECK_EQ(lut.size, 128ull * 32 * 32);
    nvn_tex_desc h20 = { .target = 1, .format = 0x25, .width = 16, .height = 20, .levels = 1 };
    CHECK(nvn_tex_layout(&h20));
    CHECK_EQ(h20.block_h, 2);
    CHECK_EQ(h20.size, 64ull * 32);
    nvn_tex_desc h10 = { .target = 1, .format = 0x25, .width = 16, .height = 10, .levels = 1 };
    CHECK(nvn_tex_layout(&h10));
    CHECK_EQ(h10.block_h, 1);
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

TEST(nvn_watch_sees_writes_to_watched_pages_only)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x400000);
    uint64_t hp = hos.vm.host_page;
    uint64_t at = (heap + 0x20000 + hp - 1) / hp * hp;
    uint64_t size = 40 * hp;
    uint8_t *m = t_mem();
    m[at + 5] = 1;
    nvn_watch w;
    CHECK(nvn_watch_start(&w, at + 7, size));
    CHECK(!nvn_watch_changed(&w));
    m[at + size + 3 * hp] = 9;
    CHECK(!nvn_watch_changed(&w));
    CHECK(!t_write(at + 21 * hp + 1, 7).faulted);
    CHECK_EQ(m[at + 21 * hp + 1], 7);
    CHECK(nvn_watch_changed(&w));
    nvn_watch w2;
    CHECK(nvn_watch_start(&w2, at + 7, size));
    CHECK(!nvn_watch_changed(&w2));
    CHECK(!t_write(at + 22 * hp, 3).faulted);
    CHECK(nvn_watch_changed(&w2));
    nvn_watch w3;
    CHECK(nvn_watch_start(&w3, at + 7, size));
    CHECK(!nvn_watch_changed(&w3));
    uint64_t faults, starts;
    nvn_watch_stats(&faults, &starts);
    CHECK(faults >= 2 && starts >= 3);
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

TEST(nvn_device_modes_read_back_what_was_set)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK_EQ(call(c, nvnh_nvnDeviceSetWindowOriginMode, heap, 1, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetWindowOriginMode, heap, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 1);
    CHECK_EQ(call(c, nvnh_nvnDeviceSetDepthMode, heap, 1, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetDepthMode, heap, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 1);
    CHECK_EQ(call(c, nvnh_nvnDeviceSetDepthMode, heap, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetDepthMode, heap, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetWindowOriginMode, heap, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 1);
}

TEST(nvn_debug_domain_ids_count_up)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x1000);
    CHECK_EQ(call(c, nvnh_nvnDeviceGenerateDebugDomainId, heap, 0, 0, 0), RET);
    uint64_t first = c->x[0];
    CHECK_EQ(call(c, nvnh_nvnDeviceGenerateDebugDomainId, heap, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], first + 1);
    CHECK_EQ(call(c, nvnh_nvnDeviceGenerateDebugDomainId, heap + 8, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], first + 2);
}

TEST(nvn_pools_report_their_descriptor_count)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t tex = heap + 0x300, smp = heap + 0x380;
    CHECK_EQ(call(c, nvnh_nvnTexturePoolInitialize, tex, heap, 0x1000, 0x8000), RET);
    CHECK_EQ(c->x[0], 1);
    CHECK_EQ(call(c, nvnh_nvnSamplerPoolInitialize, smp, heap, 0x2000, 2048), RET);
    CHECK_EQ(call(c, nvnh_nvnTexturePoolGetSize, tex, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0x8000);
    CHECK_EQ(call(c, nvnh_nvnSamplerPoolGetSize, smp, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 2048);
    CHECK_EQ(call(c, nvnh_nvnTexturePoolFinalize, tex, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnSamplerPoolFinalize, smp, 0, 0, 0), RET);
}

TEST(nvn_pools_report_the_memory_they_were_initialized_over)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t tex = heap + 0x300, smp = heap + 0x380;
    CHECK_EQ(call(c, nvnh_nvnTexturePoolInitialize, tex, heap, 0x1000, 0x8000), RET);
    CHECK_EQ(call(c, nvnh_nvnSamplerPoolInitialize, smp, heap + 0x40, 0x2000, 2048), RET);
    CHECK_EQ(call(c, nvnh_nvnTexturePoolGetMemoryPool, tex, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], heap);
    CHECK_EQ(call(c, nvnh_nvnTexturePoolGetMemoryOffset, tex, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0x1000);
    CHECK_EQ(call(c, nvnh_nvnSamplerPoolGetMemoryPool, smp, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], heap + 0x40);
    CHECK_EQ(call(c, nvnh_nvnSamplerPoolGetMemoryOffset, smp, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0x2000);
    CHECK_EQ(call(c, nvnh_nvnTexturePoolGetMemoryPool, smp, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
}

TEST(nvn_fast_clear_depth_is_accepted_like_the_drivers_while_its_table_has_room)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x1000);
    c->v[0].d[0] = 0;
    CHECK_EQ(call(c, nvnh_nvnDeviceRegisterFastClearDepth, heap, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 1);
}

TEST(nvn_command_buffer_settings_relift_has_no_use_for_return_and_record_nothing)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, cb, heap, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    uint64_t (*const procs[])(rl_ctx *, uint8_t *) = {
        nvnh_nvnCommandBufferSetColorReductionEnable,  nvnh_nvnCommandBufferSetColorReductionThresholds,
        nvnh_nvnCommandBufferSetStencilCullCriteria,   nvnh_nvnCommandBufferOverrideSubtileSize,
        nvnh_nvnCommandBufferSetSubpixelPrecisionBias, nvnh_nvnCommandBufferDiscardColor,
        nvnh_nvnCommandBufferDiscardDepthStencil,
    };
    for (size_t i = 0; i < sizeof procs / sizeof procs[0]; i++)
        CHECK_EQ(call(c, procs[i], cb, 1, 2, 3), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0), RET);
    const nvn_cmdset *set = nvn_cmdset_of(c->x[0]);
    CHECK(set != NULL);
    CHECK_EQ(set->n, 0);
}

TEST(nvn_dlss_optimal_settings_are_the_librarys)
{
    static const uint32_t rows[][9] = {
        { 3840, 2160, 0, 2227, 1253, 1920, 1080, 3840, 2160 }, { 3840, 2160, 1, 1280, 720, 1280, 720, 1280, 720 },
        { 3840, 2160, 2, 3840, 2160, 3802, 2138, 3840, 2160 }, { 3840, 2160, 5, 2227, 1253, 1920, 1080, 3840, 2160 },
        { 1920, 1080, 0, 1114, 626, 960, 540, 1920, 1080 },    { 1920, 1080, 2, 1920, 1080, 1901, 1069, 1920, 1080 },
        { 1366, 768, 1, 455, 256, 455, 256, 455, 256 },        { 1921, 1081, 0, 1114, 627, 961, 541, 1921, 1081 },
        { 25, 176, 0, 15, 102, 13, 88, 25, 176 },              { 232, 1625, 0, 135, 943, 116, 813, 232, 1625 },
        { 17, 9, 0, 10, 5, 9, 5, 17, 9 },                      { 17, 9, 2, 17, 9, 17, 9, 17, 9 },
        { 2048, 1152, 1, 683, 384, 683, 384, 683, 384 },
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        uint32_t out[6];
        nvn_dlss_optimal(rows[i][0], rows[i][1], rows[i][2], out);
        for (int k = 0; k < 6; k++)
            CHECK_EQ(out[k], rows[i][3 + k]);
    }
}

TEST(nvn_dlss_parameters_read_as_the_setters_store_them)
{
    uint8_t raw[NVN_DLSS_PARAMS_BYTES] = { 0 };
    uint64_t color = 0x1000, view = 0x2000, out = 0x3000, expo = 0x4000;
    float jitter[2] = { 0.25f, -0.5f }, one = 1.0f;
    uint32_t size[2] = { 1280, 720 }, base[2] = { 8, 4 };
    memcpy(raw, &color, 8);
    memcpy(raw + 8, &view, 8);
    memcpy(raw + 16, &out, 8);
    memcpy(raw + 104, &expo, 8);
    memcpy(raw + 80, jitter, 8);
    memcpy(raw + 88, &one, 4);
    memcpy(raw + 92, &one, 4);
    memcpy(raw + 96, &one, 4);
    raw[120] = 1;
    memcpy(raw + 128, size, 8);
    memcpy(raw + 168, base, 8);
    nvn_dlss_params p;
    nvn_dlss_params_of(raw, &p);
    CHECK_EQ(p.color, color);
    CHECK_EQ(p.color_view, view);
    CHECK_EQ(p.out, out);
    CHECK_EQ(p.out_view, 0);
    CHECK_EQ(p.exposure, expo);
    CHECK(p.jitter[0] == 0.25f && p.jitter[1] == -0.5f);
    CHECK(p.motion_scale[0] == 1.0f && p.pre_exposure == 1.0f);
    CHECK(p.reset);
    CHECK_EQ(p.in_size[0], 1280);
    CHECK_EQ(p.in_size[1], 720);
    CHECK_EQ(p.out_base[0], 8);
    CHECK_EQ(p.out_base[1], 4);
}

TEST(nvn_dlss_history_starts_again_on_another_network_or_size)
{
    nvn_dlss_seen seen = { 0 };
    const uint32_t a[4] = { 1920, 1080, 3840, 2160 }, b[4] = { 960, 540, 1920, 1080 };
    CHECK(!nvn_dlss_continues(&seen, 0x1000, a));
    CHECK(nvn_dlss_continues(&seen, 0x1000, a));
    CHECK(!nvn_dlss_continues(&seen, 0x2000, a));
    CHECK(!nvn_dlss_continues(&seen, 0x2000, b));
    CHECK(nvn_dlss_continues(&seen, 0x2000, b));
    CHECK(!nvn_dlss_continues(&seen, 0x1000, b));
}

TEST(nvn_dlss_builder_texture_ids_round_trip_through_the_builder)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t builder = heap + 0x100, ids = heap + 0x400, out = heap + 0x600, pool = heap + 0x800;
    for (uint32_t i = 0; i < 32; i++)
        rl_st32(t_mem(), ids + 4 * i, 1000 + i);
    CHECK_EQ(call(c, nvnh_nvnDlssNetworkBuilderSetTextureIDs, builder, pool, ids, 0), RET);
    CHECK_EQ(rl_ld64(t_mem(), builder + 8), pool);
    CHECK_EQ(rl_ld32(t_mem(), builder + 0x38), 1000);
    CHECK_EQ(rl_ld32(t_mem(), builder + 0x38 + 124), 1031);
    CHECK_EQ(rl_ld32(t_mem(), builder + 0x38 + 128), 0);
    CHECK_EQ(call(c, nvnh_nvnDlssNetworkBuilderGetTextureIDs, builder, out, 0, 0), RET);
    for (uint32_t i = 0; i < 32; i++)
        CHECK_EQ(rl_ld32(t_mem(), out + 4 * i), 1000 + i);
}

TEST(nvn_dlss_state_procs_stay_the_drivers_and_work_procs_do_not)
{
    CHECK(nvn_driver_index("nvnDlssNetworkBuilderSetDefaults") >= 0);
    CHECK(nvn_driver_index("nvnDlssParametersSetInputColor") >= 0);
    CHECK(nvn_driver_index("nvnDlssParametersGetPreExposure") >= 0);
    CHECK_EQ(nvn_driver_index("nvnCommandBufferEvaluateDlss"), -1);
    CHECK_EQ(nvn_driver_index("nvnDlssNetworkInitialize"), -1);
    CHECK_EQ(nvn_driver_index("nvnDlssNetworkBuilderGetStorageSize"), -1);
    CHECK_EQ(nvn_driver_index("nvnDlssNetworkBuilderGetOptimalSettings"), -1);
}

TEST(nvn_sampler_compare_stays_the_drivers)
{
    CHECK(nvn_driver_index("nvnSamplerCompare") >= 0);
    CHECK(nvn_driver_index("nvnTextureViewCompare") >= 0);
}

TEST(nvn_debug_and_checkpoint_procs_stay_the_drivers_and_the_layers_getters_do_not)
{
    CHECK(nvn_driver_index("nvnBufferGetDebugID") >= 0);
    CHECK(nvn_driver_index("nvnTextureGetDebugID") >= 0);
    CHECK(nvn_driver_index("nvnDeviceWalkDebugDatabase") >= 0);
    CHECK(nvn_driver_index("nvnCommandBufferInsertCheckpointDynamic") >= 0);
    CHECK(nvn_driver_index("nvnCommandBufferInsertCheckpointStatic") >= 0);
    CHECK(nvn_driver_index("nvnCommandBufferSetCommandMemoryCallbackEnabled") >= 0);
    CHECK(nvn_driver_index("nvnCommandBufferSetControlMemoryCallbackEnabled") >= 0);
    CHECK_EQ(nvn_driver_index("nvnCommandBufferIsRecording"), -1);
    CHECK_EQ(nvn_driver_index("nvnCommandBufferGetMemoryCallback"), -1);
    CHECK_EQ(nvn_driver_index("nvnTexturePoolGetMemoryPool"), -1);
    CHECK_EQ(nvn_driver_index("nvnWindowGetNativeWindow"), -1);
    CHECK(nvn_driver_index("nvnTextureGetSwizzle") >= 0);
    CHECK(nvn_driver_index("nvnTextureGetDepthStencilMode") >= 0);
    CHECK(nvn_driver_index("nvnTextureCompare") >= 0);
    CHECK(nvn_driver_reads_texture("nvnTextureGetSwizzle"));
    CHECK(nvn_driver_reads_texture("nvnTextureGetDepthStencilMode"));
    CHECK(nvn_driver_reads_texture("nvnTextureCompare"));
}

TEST(nvn_slots_hold_ids_past_the_device_limit)
{
    nvn_slots s;
    CHECK(nvn_slots_init(&s, 4, 1u << 20, 0));
    uint32_t ev;
    uint32_t a = nvn_slots_take(&s, 426141, &ev);
    CHECK(a < 4);
    CHECK_EQ(ev, 0);
    CHECK_EQ(nvn_slots_take(&s, 426141, &ev), a);
    CHECK_EQ(ev, 0);
    CHECK_EQ(nvn_slots_take(&s, 1u << 20, &ev), UINT32_MAX);
    uint32_t b = nvn_slots_take(&s, 7, &ev), c = nvn_slots_take(&s, 1048575, &ev);
    CHECK(b != a && c != a && b != c);
    nvn_slots_free(&s, b);
    CHECK_EQ(nvn_slots_take(&s, 99, &ev), b);
    CHECK_EQ(ev, 0);
    CHECK_EQ(nvn_slots_take(&s, 7, &ev) < 4, true);
    free(s.id_slot);
    free(s.slot_id);
    free(s.free_stack);
}

TEST(nvn_slots_pin_the_low_ids_to_their_own_numbers)
{
    nvn_slots s;
    CHECK(!nvn_slots_init(&s, 4, 1u << 20, 4));
    CHECK(nvn_slots_init(&s, 6, 1u << 20, 4));
    uint32_t ev = 9;
    CHECK_EQ(nvn_slots_take(&s, 0, &ev), 0);
    CHECK_EQ(nvn_slots_take(&s, 3, &ev), 3);
    CHECK_EQ(ev, 0);
    uint32_t a = nvn_slots_take(&s, 500000, &ev), b = nvn_slots_take(&s, 600000, &ev);
    CHECK(a >= 4 && a < 6 && b >= 4 && b < 6 && a != b);
    uint32_t c = nvn_slots_take(&s, 700000, &ev);
    CHECK_EQ(c, a);
    CHECK_EQ(ev, 500001);
    CHECK_EQ(nvn_slots_take(&s, 3, &ev), 3);
    nvn_slots_free(&s, 3);
    nvn_slots_free(&s, b);
    CHECK_EQ(nvn_slots_take(&s, 800000, &ev), b);
    CHECK_EQ(ev, 0);
    free(s.id_slot);
    free(s.slot_id);
    free(s.free_stack);
}

TEST(nvn_slots_evict_the_oldest_when_full)
{
    nvn_slots s;
    CHECK(nvn_slots_init(&s, 2, 1u << 20, 0));
    uint32_t ev;
    uint32_t a = nvn_slots_take(&s, 500000, &ev);
    uint32_t b = nvn_slots_take(&s, 600000, &ev);
    CHECK(a != b);
    uint32_t c = nvn_slots_take(&s, 700000, &ev);
    CHECK_EQ(c, a);
    CHECK_EQ(ev, 500001);
    CHECK_EQ(s.id_slot[500000], 0);
    CHECK_EQ(nvn_slots_take(&s, 600000, &ev), b);
    free(s.id_slot);
    free(s.slot_id);
    free(s.free_stack);
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
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetCommandMemoryUsed, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 32);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0), RET);
    uint64_t h = c->x[0];
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetCommandMemoryUsed, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 32);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetCommandMemoryFree, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0x4000 - 32);
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

/** Records one stencil-ref command into buffer cb and returns the set's handle. */
static uint64_t record_one(rl_ctx *c, uint64_t cb, uint64_t ref)
{
    call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0);
    call(c, nvnh_nvnCommandBufferSetStencilRef, cb, ref, 0x80, 0);
    call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0);
    return c->x[0];
}

TEST(nvn_a_command_set_ends_when_a_later_recording_writes_over_its_bytes)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400, other = heap + 0x500;
    uint64_t live0, bytes0, made0, over0, live, bytes, made, over;
    nvn_cmdset_stats(&live0, &bytes0, &made0, &over0);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, cb, heap, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, other, heap, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, cb, heap, 0x10000, 0x4000), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, other, heap, 0x20000, 0x4000), RET);
    /* two recordings in one added memory sit one after the other: the second does not write over the first */
    uint64_t h0 = record_one(c, cb, 1), h1 = record_one(c, cb, 2), h2 = record_one(c, other, 3);
    CHECK(nvn_cmdset_of(h0) != NULL && nvn_cmdset_of(h1) != NULL && nvn_cmdset_of(h2) != NULL);
    nvn_cmdset_stats(&live, &bytes, &made, &over);
    CHECK_EQ(live, live0 + 3);
    CHECK_EQ(over, over0);
    /* adding the memory again writes nothing: the sets stay, as a game may still submit them */
    const nvn_cmdset *inflight = nvn_cmdset_acquire(h0);
    CHECK(inflight != NULL);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, cb, heap, 0x10000, 0x4000), RET);
    CHECK(nvn_cmdset_of(h0) != NULL && nvn_cmdset_of(h1) != NULL);
    /* recording again starts at the memory's start and writes over the first set (one command, 16 bytes), not the second */
    uint64_t h3 = record_one(c, cb, 7);
    CHECK(nvn_cmdset_of(h0) == NULL);
    CHECK(nvn_cmdset_of(h1) != NULL);
    CHECK(nvn_cmdset_of(h2) != NULL);
    CHECK(nvn_cmdset_of(h3) != NULL);
    /* a run in flight keeps what it replays */
    CHECK_EQ(inflight->n, 1);
    CHECK_EQ(inflight->cmds[0].x[1], 1);
    nvn_cmdset_stats(&live, &bytes, &made, &over);
    CHECK_EQ(over, over0 + 1);
    CHECK_EQ(live, live0 + 3);
    nvn_cmdset_release(inflight);
    /* the next recording writes over the second set */
    uint64_t h4 = record_one(c, cb, 8);
    CHECK(nvn_cmdset_of(h1) == NULL);
    CHECK(nvn_cmdset_of(h4) != NULL);
}

TEST(nvn_empty_recordings_share_one_set)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, cb, heap, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, cb, heap, 0x10000, 0x4000), RET);
    uint64_t live0, bytes0, made0, over0, live, bytes, made, over;
    nvn_cmdset_stats(&live0, &bytes0, &made0, &over0);
    uint64_t h[3];
    for (int i = 0; i < 3; i++) {
        call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0);
        call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0);
        h[i] = c->x[0];
    }
    nvn_cmdset_stats(&live, &bytes, &made, &over);
    CHECK_EQ(made, made0 + 3);
    CHECK_EQ(live, live0);
    CHECK_EQ(bytes, bytes0);
    CHECK(h[0] != h[1] && nvn_cmdset_of(h[0]) == nvn_cmdset_of(h[2]));
    CHECK_EQ(nvn_cmdset_of(h[1])->n, 0);
    nvn_cmdset_free(h[0]);
    CHECK(nvn_cmdset_of(h[0]) == NULL && nvn_cmdset_of(h[1]) != NULL);
}

TEST(nvn_a_set_called_from_another_buffer_is_written_over_only_by_a_recording)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t outer = heap + 0x400, inner = heap + 0x500;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, outer, heap, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, inner, heap, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, inner, heap, 0x30000, 0x4000), RET);
    uint64_t callee = record_one(c, inner, 3);
    /* the outer buffer is given the same range: no bytes change until it records */
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, outer, heap, 0x30000, 0x4000), RET);
    CHECK(nvn_cmdset_of(callee) != NULL);
    /* its recording writes there, over the callee */
    uint64_t caller = record_one(c, outer, 5);
    CHECK(nvn_cmdset_of(callee) == NULL);
    CHECK(nvn_cmdset_of(caller) != NULL);
    /* a recording in memory elsewhere leaves it */
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, inner, heap, 0x50000, 0x4000), RET);
    uint64_t apart = record_one(c, inner, 6);
    CHECK(nvn_cmdset_of(caller) != NULL && nvn_cmdset_of(apart) != NULL);
}

TEST(nvn_indirect_draws_are_recorded_with_their_arguments)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, cb, heap, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, cb, heap, 0, 0x4000), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    c->x[4] = 0x5000;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferDrawElementsIndirect, cb, 4, 2, 0x4000), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferDrawArraysIndirect, cb, 4, 0x6000, 0), RET);
    c->x[4] = 8;
    c->x[5] = 32;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferMultiDrawArraysIndirectCount, cb, 4, 0x6000, 0x7000), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0), RET);
    const nvn_cmdset *s = nvn_cmdset_of(c->x[0]);
    CHECK(s != NULL);
    CHECK_EQ(s->n, 3);
    CHECK_EQ(s->cmds[0].op, NVN_CMD_DRAW_ELEMENTS_INDIRECT);
    CHECK_EQ(s->cmds[0].x[2], 2);
    CHECK_EQ(s->cmds[0].x[4], 0x5000);
    CHECK_EQ(s->cmds[1].op, NVN_CMD_DRAW_ARRAYS_INDIRECT);
    CHECK_EQ(s->cmds[1].x[2], 0x6000);
    CHECK_EQ(s->cmds[2].op, NVN_CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT);
    CHECK_EQ(s->cmds[2].x[4], 8);
    CHECK_EQ(s->cmds[2].x[5], 32);
}

TEST(nvn_command_buffer_memory_used_follows_the_recording)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, cb, heap, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, cb, heap, 0, 0x40), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetCommandMemoryUsed, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    for (int i = 0; i < 6; i++)
        CHECK_EQ(call(c, nvnh_nvnCommandBufferSetStencilRef, cb, 3, 0x80, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetCommandMemoryUsed, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0x40);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetCommandMemoryFree, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetCommandMemoryUsed, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
}

TEST(nvn_command_buffer_getters_read_the_layers_state)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, cb, heap, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetMemoryCallback, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetMemoryCallback, cb, 0x1234, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetMemoryCallbackData, cb, 0x5678, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetMemoryCallback, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0x1234);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetMemoryCallbackData, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0x5678);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferIsRecording, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferIsRecording, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 1);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferIsRecording, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferFinalize, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferIsRecording, cb, 0, 0, 0), RL_PENDING);
}

TEST(nvn_copy_strides_are_command_buffer_state_each_copy_keeps)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400, region = heap + 0x600;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferInitialize, cb, heap, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferAddCommandMemory, cb, heap, 0, 0x4000), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetCopyRowStride, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetCopyRowStride, cb, 256, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    c->x[4] = region;
    c->x[5] = 0;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferCopyBufferToTexture, cb, 0x1000, heap + 0x800, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferSetCopyImageStride, cb, 0x4000, 0, 0), RET);
    c->x[4] = region;
    c->x[5] = 0;
    CHECK_EQ(call(c, nvnh_nvnCommandBufferCopyBufferToTexture, cb, 0x1000, heap + 0x800, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferEndRecording, cb, 0, 0, 0), RET);
    const nvn_cmdset *s = nvn_cmdset_of(c->x[0]);
    CHECK(s != NULL);
    CHECK_EQ(s->n, 2);
    CHECK_EQ(s->cmds[0].x[NVN_COPY_ROW_STRIDE_ARG], 256);
    CHECK_EQ(s->cmds[0].x[NVN_COPY_IMAGE_STRIDE_ARG], 0);
    CHECK_EQ(s->cmds[1].x[NVN_COPY_IMAGE_STRIDE_ARG], 0x4000);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferBeginRecording, cb, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnCommandBufferGetCopyImageStride, cb, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 0x4000);
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

static void put_le(uint8_t *p, uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static int rm_entry(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

/** One module of three words, stored as a zstd frame with one raw block. */
static const uint32_t table_words[3] = { 0x07230203u, 0x00010600u, 0xdeadbeefu };

static void table_key(char key[65])
{
    for (int i = 0; i < 32; i++)
        snprintf(key + 2 * i, 3, "%02x", 0xa0 + i);
}

/** Writes dir/table.rlspv with one translation, keyed by table_key. */
static bool write_table(const char *dir)
{
    uint8_t frame[4 + 2 + 3 + sizeof table_words] = { 0x28, 0xb5, 0x2f, 0xfd, 0x20, sizeof table_words };
    put_le(frame + 6, (sizeof table_words << 3) | 1, 3);
    memcpy(frame + 9, table_words, sizeof table_words);
    uint8_t table[12 + 48 + sizeof frame + 4];
    memset(table, 0, sizeof table);
    memcpy(table, "RLSPV002", 8);
    put_le(table + 8, 1, 4);
    for (int i = 0; i < 32; i++)
        table[12 + i] = (uint8_t)(0xa0 + i);
    put_le(table + 44, 60, 8);
    put_le(table + 52, sizeof frame, 4);
    put_le(table + 56, 3, 4);
    memcpy(table + 60, frame, sizeof frame);
    char path[PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/table.rlspv", dir);
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(table, 1, sizeof table, f) == sizeof table;
    return fclose(f) == 0 && ok;
}

static bool make_scratch(char dir[PATH_MAX])
{
    const char *base = getenv("TMPDIR");
    snprintf(dir, PATH_MAX, "%s/relift-rlspv-XXXXXX", base && *base ? base : "/tmp");
    return mkdtemp(dir) != NULL;
}

TEST(nvn_reads_compressed_translation_table)
{
    char dir[PATH_MAX];
    CHECK(make_scratch(dir));
    CHECK(write_table(dir));
    setenv("RELIFT_NVN_SHADERS", dir, 1);
    char key[65];
    table_key(key);
    size_t n = 0;
    uint32_t *got = nvn_shader_spirv(key, &n);
    CHECK(got != NULL);
    CHECK_EQ(n, 3);
    CHECK(got && memcmp(got, table_words, sizeof table_words) == 0);
    free(got);
    key[0] = 'b';
    CHECK(nvn_shader_spirv(key, &n) == NULL);
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

/** @brief Interface records in the test table: more than the 48 a program could hold before. */
#define IFACE_RECS 60u

/** Writes dir/table.rlspi with one program of IFACE_RECS records, keyed by table_key; record i holds i to i + 3. */
static bool write_iface_table(const char *dir)
{
    size_t len = 12 + 40 + 16 * IFACE_RECS;
    uint8_t *t = calloc(1, len);
    if (!t)
        return false;
    memcpy(t, "RLSPI002", 8);
    put_le(t + 8, 1, 4);
    for (int i = 0; i < 32; i++)
        t[12 + i] = (uint8_t)(0xa0 + i);
    put_le(t + 44, 0, 4);
    put_le(t + 48, IFACE_RECS, 4);
    for (uint32_t i = 0; i < IFACE_RECS; i++)
        for (uint32_t w = 0; w < 4; w++)
            put_le(t + 52 + 16 * i + 4 * w, i + w, 4);
    char path[PATH_MAX + 16];
    snprintf(path, sizeof path, "%s/table.rlspi", dir);
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite(t, 1, len, f) == len;
    free(t);
    return f && fclose(f) == 0 && ok;
}

/** Child of nvn_iface_keeps_every_record: the shader tables are read once per process. */
static int iface_child(const char *dir)
{
    setenv("RELIFT_NVN_SHADERS", dir, 1);
    char key[65];
    table_key(key);
    if (nvn_shader_iface(key, NULL, 0) != IFACE_RECS)
        return 1;
    nvn_iface_rec recs[IFACE_RECS + 1];
    memset(recs, 0xff, sizeof recs);
    if (nvn_shader_iface(key, recs, IFACE_RECS) != IFACE_RECS)
        return 2;
    for (uint32_t i = 0; i < IFACE_RECS; i++)
        if (recs[i].kind != i || recs[i].bank != i + 1 || recs[i].offset != i + 2 || recs[i].spec != i + 3)
            return 3;
    if (recs[IFACE_RECS].kind != 0xffffffffu)
        return 4;
    if (nvn_shader_iface(key, recs, 10) != IFACE_RECS)
        return 5;
    return 0;
}

TEST(nvn_iface_keeps_every_record)
{
    char dir[PATH_MAX];
    CHECK(make_scratch(dir));
    CHECK(write_iface_table(dir));
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    CHECK(pid >= 0);
    if (pid == 0)
        _exit(iface_child(dir));
    int st = 0;
    CHECK(waitpid(pid, &st, 0) == pid);
    CHECK(WIFEXITED(st));
    CHECK_EQ(WEXITSTATUS(st), 0);
    nftw(dir, rm_entry, 4, FTW_DEPTH | FTW_PHYS);
}

/**
 * The child of nvn_shader_tables_follow_the_executable: it runs from a copy of the test program and says whether
 * the table beside that copy is found (RL_SHADER_EXPECT=found) or not (missing). Without the variable it is a test
 * that passes.
 */
TEST(nvn_shader_tables_child)
{
    const char *want = getenv("RL_SHADER_EXPECT");
    if (!want)
        return;
    CHECK(!getenv("RELIFT_NVN_SHADERS") && !getenv("RELIFT_NVN_TABLES"));
    char key[65];
    table_key(key);
    size_t n = 0;
    uint32_t *got = nvn_shader_spirv(key, &n);
    if (!strcmp(want, "found")) {
        CHECK(got != NULL);
        CHECK_EQ(n, 3);
        CHECK(got && memcmp(got, table_words, sizeof table_words) == 0);
    } else {
        CHECK(got == NULL);
    }
    free(got);
}

static bool copy_file(const char *from, const char *to)
{
    FILE *in = fopen(from, "rb"), *out = fopen(to, "wb");
    bool ok = in && out;
    char buf[1 << 16];
    size_t got;
    while (ok && (got = fread(buf, 1, sizeof buf, in)) > 0)
        ok = fwrite(buf, 1, got, out) == got;
    if (in)
        fclose(in);
    if (out)
        ok = fclose(out) == 0 && ok;
    return ok && chmod(to, 0755) == 0;
}

/** Runs the copy of the test program at exe from the folder cwd and tells whether its child test passed. */
static bool run_copy(const char *exe, const char *cwd, const char *expect)
{
    fflush(stdout);
    fflush(stderr);
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        unsetenv("RELIFT_NVN_SHADERS");
        unsetenv("RELIFT_NVN_TABLES");
        setenv("RELIFT_TEST_ONE", "nvn_shader_tables_child", 1);
        setenv("RL_SHADER_EXPECT", expect, 1);
        if (chdir(cwd) != 0)
            _exit(125);
        char *argv[] = { (char *)exe, NULL };
        execv(exe, argv);
        _exit(126);
    }
    int st = 0;
    return waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0;
}

TEST(nvn_shader_tables_follow_the_executable)
{
    char root[PATH_MAX], built[PATH_MAX + 32], moved[PATH_MAX + 32], elsewhere[PATH_MAX + 32], from[PATH_MAX], exe[PATH_MAX + 64];
    char shaders[PATH_MAX + 64], bare[PATH_MAX + 32];
    CHECK(make_scratch(root));
    CHECK(rl_exe_path(from, sizeof from));
    snprintf(built, sizeof built, "%s/built", root);
    snprintf(moved, sizeof moved, "%s/moved", root);
    snprintf(elsewhere, sizeof elsewhere, "%s/elsewhere", root);
    snprintf(bare, sizeof bare, "%s/bare", root);
    snprintf(shaders, sizeof shaders, "%s/shaders", built);
    CHECK(mkdir(built, 0755) == 0 && mkdir(shaders, 0755) == 0 && mkdir(elsewhere, 0755) == 0 && mkdir(bare, 0755) == 0);
    CHECK(write_table(shaders));
    snprintf(exe, sizeof exe, "%s/game", built);
    CHECK(copy_file(from, exe));
    CHECK(run_copy(exe, elsewhere, "found"));
    CHECK(rename(built, moved) == 0);
    snprintf(exe, sizeof exe, "%s/game", moved);
    {
        struct stat st;
        CHECK(stat(built, &st) != 0);
    }
    CHECK(run_copy(exe, elsewhere, "found"));
    snprintf(exe, sizeof exe, "%s/game", bare);
    CHECK(copy_file(from, exe));
    CHECK(run_copy(exe, moved, "missing"));
    nftw(root, rm_entry, 8, FTW_DEPTH | FTW_PHYS);
}

TEST(nvn_presents_count_guest_frames)
{
    uint64_t f0 = vi_guest_frames();
    nvn_frame_presented();
    nvn_frame_presented();
    CHECK_EQ(vi_guest_frames(), f0 + 2);
}

TEST(nvn_presents_show_the_window_crop)
{
    uint32_t r[4];
    static const int32_t none[4] = { 0, 0, 0, 0 };
    CHECK(nvn_crop_rect(none, 3840, 2160, r));
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == 3840 && r[3] == 2160);
    static const int32_t hd[4] = { 0, 0, 1920, 1080 };
    CHECK(nvn_crop_rect(hd, 3840, 2160, r));
    CHECK(r[0] == 0 && r[1] == 0 && r[2] == 1920 && r[3] == 1080);
    static const int32_t past[4] = { -8, 100, 4000, 3000 };
    CHECK(nvn_crop_rect(past, 3840, 2160, r));
    CHECK(r[0] == 0 && r[1] == 100 && r[2] == 3840 && r[3] == 2060);
    static const int32_t outside[4] = { 3840, 0, 16, 16 };
    CHECK(!nvn_crop_rect(outside, 3840, 2160, r));
}

static nvn_tex_desc sample_texture(void)
{
    nvn_tex_desc d = { .target = 1, .format = 37, .width = 64, .height = 32, .depth = 1, .levels = 3, .samples = 0,
                       .flags = 0, .swizzle = { 4, 3, 2, 1 }, .ds_mode = 2, .pool = 0x1000, .offset = 0x200 };
    return d;
}

TEST(nvn_texture_getters_answer_from_the_builders_values)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t tex = heap + 0x400, out = heap + 0x800;
    nvn_tex_desc d = sample_texture();
    CHECK(nvn_texture_record(tex, &d, NULL));
    c->x[4] = out + 12;
    c->x[0] = tex;
    c->x[1] = out;
    c->x[2] = out + 4;
    c->x[3] = out + 8;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnTextureGetSwizzle(c, t_mem()), RET);
    for (uint32_t i = 0; i < 4; i++)
        CHECK_EQ(rl_ld32(t_mem(), out + 4 * i), 4 - i);
    CHECK_EQ(call(c, nvnh_nvnTextureGetDepthStencilMode, tex, 0, 0, 0), RET);
    CHECK_EQ(c->x[0], 2);
    CHECK_EQ(call(c, nvnh_nvnTextureGetDepthStencilMode, tex + 8, 0, 0, 0), RL_PENDING);
}

TEST(nvn_texture_compare_follows_the_values_a_texture_was_built_from)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t a = heap + 0x400, b = heap + 0x500, e = heap + 0x600;
    nvn_tex_desc d = sample_texture();
    CHECK(nvn_texture_record(a, &d, NULL));
    CHECK(nvn_texture_record(b, &d, NULL));
    CHECK_EQ(call(c, nvnh_nvnTextureCompare, a, b, 0, 0), RET);
    CHECK_EQ(c->x[0], 1);
    d.offset += 0x100;
    CHECK(nvn_texture_record(e, &d, NULL));
    CHECK_EQ(call(c, nvnh_nvnTextureCompare, a, e, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
    d = sample_texture();
    d.swizzle[3] = 5;
    CHECK(nvn_texture_record(e, &d, NULL));
    CHECK_EQ(call(c, nvnh_nvnTextureCompare, a, e, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
    d = sample_texture();
    d.stride = 4096;
    CHECK(nvn_texture_record(e, &d, NULL));
    CHECK_EQ(call(c, nvnh_nvnTextureCompare, a, e, 0, 0), RET);
    CHECK_EQ(c->x[0], 1);
    d.flags = 16;
    CHECK(nvn_texture_record(e, &d, NULL));
    CHECK_EQ(call(c, nvnh_nvnTextureCompare, a, e, 0, 0), RET);
    CHECK_EQ(c->x[0], 0);
}

TEST(nvn_texture_storage_class_needs_the_builder_it_was_made_from)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    nvn_tex_desc d = sample_texture();
    rl_nvn_set_layout(NULL);
    CHECK(nvn_texture_record(heap + 0x400, &d, NULL));
    CHECK_EQ(call(c, nvnh_nvnTextureGetStorageClass, heap + 0x400, 0, 0, 0), RL_PENDING);
}

TEST(nvn_texture_storage_class_needs_the_drivers_layout_too)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    nvn_tex_desc d = sample_texture();
    uint8_t builder[NVN_TEXTURE_BUILDER_BYTES] = { 0 };
    rl_nvn_set_layout(NULL);
    CHECK(nvn_texture_record(heap + 0x400, &d, builder));
    c->sp = heap + 0x100000;
    CHECK_EQ(call(c, nvnh_nvnTextureGetStorageClass, heap + 0x400, 0, 0, 0), RL_PENDING);
}

TEST(nvn_zcull_storage_size_needs_the_drivers_layout)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    rl_nvn_set_layout(NULL);
    CHECK_EQ(call(c, nvnh_nvnTextureBuilderGetZCullStorageSize, heap, 0, 0, 0), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
}

TEST(nvn_storage_class_needs_the_drivers_layout)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    rl_nvn_set_layout(NULL);
    CHECK_EQ(call(c, nvnh_nvnTextureBuilderGetStorageClass, heap, 0, 0, 0), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
}

TEST(nvn_device_integers_come_from_the_drivers_table)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    static const int32_t ints[] = { 231, 40, 0, 7 };
    static const uint8_t device[] = { 0, 0, 1, 0 };
    static const struct rl_nvn_layout l = { .nints = 4, .ints = ints, .int_device = device };
    rl_nvn_set_layout(&l);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 1, heap + 0x100, 0), RET);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x100), 40);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 2, heap + 0x100, 0), RET);
    CHECK_EQ(rl_ld32(t_mem(), heap + 0x100), 14);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetInteger, heap, 4, heap + 0x100, 0), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    rl_nvn_set_layout(NULL);
}

static void map_req(uint64_t r, uint64_t phys, uint64_t poff, uint64_t voff, uint64_t size, uint32_t cls)
{
    rl_st64(t_mem(), r, phys);
    rl_st64(t_mem(), r + 8, poff);
    rl_st64(t_mem(), r + 16, voff);
    rl_st64(t_mem(), r + 24, size);
    rl_st32(t_mem(), r + 32, cls);
}

TEST(nvn_virtual_pools_reach_the_mapped_pages_through_an_import_of_their_own)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    static const int32_t ints[] = { 7, 0x10000 };
    static const uint8_t device[] = { 0, 0 };
    static const struct rl_nvn_layout l = { .nints = 2, .ints = ints, .int_device = device, .pool_page_info = 1 };
    rl_nvn_set_layout(&l);
    uint64_t phys = heap, virt = heap + 0x100, req = heap + 0x400, storage = heap + 0x100000;
    CHECK(nvn_pool_make(phys, storage, 0x80000, 0x18c));
    CHECK(nvn_pool_make(virt, 0, 0x40000, 0x2a1));
    map_req(req, phys, 0x10000, 0, 0x10000, 0x100);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolMapVirtual, phys, 1, req, 0), RET);
    CHECK_EQ(c->x[0], 0);
    map_req(req, phys, 0x10000, 0, 0x10000, 0x100);
    map_req(req + 0x28, phys, 0x20000, 0x10000, 0x20000, 0x108);
    map_req(req + 0x50, 0, 0, 0x30000, 0x10000, 0);
    uint64_t phys_gpu = nvn_pool_gpu(phys);
    if (phys_gpu == storage) {
        CHECK_EQ(call(c, nvnh_nvnMemoryPoolMapVirtual, virt, 3, req, 0), RL_PENDING);
        CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
        rl_nvn_set_layout(NULL);
        return;
    }
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolMapVirtual, virt, 3, req, 0), RET);
    CHECK_EQ(c->x[0], 1);
    uint64_t size = 0, gpu = nvn_pool_gpu(virt);
    CHECK_EQ(nvn_pool_memory(virt, &size), storage + 0x10000);
    CHECK_EQ(size, 0x40000);
    CHECK(gpu + 0x40000 <= phys_gpu || gpu >= phys_gpu + 0x80000 || gpu == phys_gpu + 0x10000);
    CHECK_EQ(nvn_gpu_guest(gpu + 0x20), storage + 0x10020);
    CHECK_EQ(nvn_gpu_guest(gpu + 0x3fff0), storage + 0x4fff0);
    CHECK_EQ(nvn_gpu_guest(phys_gpu + 0x8), storage + 0x8);
    CHECK_EQ(nvn_gpu_guest(phys_gpu + 0x60000), storage + 0x60000);
    map_req(req, phys, 0, 0, 0x10000, 0x100);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolMapVirtual, virt, 1, req, 0), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, virt, 0, 0, 0), RET);
    CHECK_EQ(nvn_gpu_guest(phys_gpu + 0x20020), storage + 0x20020);
    rl_nvn_set_layout(NULL);
}

TEST(nvn_a_pool_made_again_over_the_same_bytes_gets_its_old_address_back)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t pool = heap, again = heap + 0x100, other = heap + 0x200, storage = heap + 0x100000;
    CHECK(nvn_pool_make(pool, storage, 0x40000, 0x18c));
    uint64_t gpu = nvn_pool_gpu(pool);
    if (gpu == storage || !nvn_vk_address_replay()) {
        CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, pool, 0, 0, 0), RET);
        return;
    }
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, pool, 0, 0, 0), RET);
    CHECK(nvn_gpu_mapped(gpu + 0x20, 0x100));
    CHECK(nvn_pool_make(again, storage, 0x40000, 0x18c));
    CHECK_EQ(nvn_pool_gpu(again), gpu);
    CHECK(nvn_gpu_mapped(gpu + 0x20, 0x40000 - 0x20));
    CHECK_EQ(nvn_gpu_guest(gpu + 0x20), storage + 0x20);
    CHECK(nvn_pool_make(other, storage + 0x40000, 0x40000, 0x18c));
    CHECK(nvn_pool_gpu(other) != gpu);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, again, 0, 0, 0), RET);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, other, 0, 0, 0), RET);
}

TEST(nvn_a_pool_made_again_keeps_its_import_and_address)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x400000);
    uint64_t pool = heap, other = heap + 0x100, storage = heap + 0x100000;
    CHECK(nvn_pool_make(pool, storage, 0x40000, 0x24));
    uint64_t gpu = nvn_pool_gpu(pool);
    if (gpu == storage) {
        CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, pool, 0, 0, 0), RET);
        return;
    }
    for (int round = 0; round < 3; round++) {
        uint32_t kept = nvn_pools_kept();
        CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, pool, 0, 0, 0), RET);
        CHECK_EQ(nvn_pools_kept(), kept + 1);
        CHECK(nvn_gpu_mapped(gpu, 0x40000));
        CHECK(nvn_pool_make(other, storage + 0x100000, 0x40000, 0x24));
        kept = nvn_pools_kept();
        CHECK(nvn_pool_make(pool, storage, 0x40000, 0x24));
        CHECK_EQ(nvn_pools_kept(), kept - 1);
        CHECK_EQ(nvn_pool_gpu(pool), gpu);
        CHECK_EQ(nvn_gpu_guest(gpu + 0x1230), storage + 0x1230);
        CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, other, 0, 0, 0), RET);
    }
    /* A pool over part of a kept import with another range frees it and imports anew. */
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, pool, 0, 0, 0), RET);
    uint32_t before = nvn_pools_kept();
    CHECK(nvn_pool_make(pool, storage + 0x10000, 0x40000, 0x24));
    CHECK(nvn_pools_kept() < before);
    CHECK_EQ(nvn_gpu_guest(nvn_pool_gpu(pool) + 0x20), storage + 0x10020);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, pool, 0, 0, 0), RET);
    /* The guest discarding the pages frees the import kept over them. */
    before = nvn_pools_kept();
    CHECK_EQ(rl_vm_set(&hos.vm, storage, 0x80000, RL_VM_RW, 0, RL_VM_DISCARD), 0);
    CHECK(nvn_pools_kept() < before);
}

/**
 * @brief A texture keeps the address it was initialized at, not its pool object: after the game finalizes the pool
 * object, the texture still reads its bytes while the pool's kept import (or a pool made again over the same range)
 * covers them, and has no storage once the guest discards the pages, where the console's GPU would read unmapped
 * memory.
 */
TEST(nvn_a_texture_outlives_its_finalized_pool_object)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x400000);
    uint64_t pool = heap, again = heap + 0x100, tex = heap + 0x800, region = heap + 0x1000, data = heap + 0x2000;
    uint64_t storage = heap + 0x100000;
    uint8_t *m = t_mem();
    CHECK(nvn_pool_make(pool, storage, 0x40000, 0x24));
    bool imported = nvn_pool_gpu(pool) != storage;
    nvn_tex_desc d = { .target = 1, .format = 37, .width = 8, .height = 8, .depth = 1, .levels = 1, .flags = 1u << 4,
                       .stride = 32, .swizzle = { 2, 3, 4, 5 }, .pool = pool, .offset = 0x1000 };
    CHECK(nvn_tex_layout(&d));
    d.storage = storage + 0x1000;
    CHECK(nvn_texture_record(tex, &d, NULL));
    for (uint32_t i = 0; i < 256; i++)
        m[storage + 0x1000 + i] = (uint8_t)(i * 7 + 1);
    CHECK_EQ(nvn_tex_storage(&d), storage + 0x1000);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, pool, 0, 0, 0), RET);
    if (!imported) {
        /* Without an import nothing keeps the span: there is no storage, as on unmapped memory. */
        CHECK_EQ(nvn_tex_storage(&d), 0);
        return;
    }
    CHECK_EQ(nvn_tex_storage(&d), storage + 0x1000);
    int32_t box[6] = { 0, 0, 0, 8, 8, 1 };
    memcpy(m + region, box, sizeof box);
    memset(m + data, 0, 256);
    CHECK_EQ(call(c, nvnh_nvnTextureReadTexels, tex, 0, region, data), RET);
    CHECK(memcmp(m + data, m + storage + 0x1000, 256) == 0);
    /* A pool made again over the same range, under another object, covers it too. */
    CHECK(nvn_pool_make(again, storage, 0x40000, 0x24));
    CHECK_EQ(nvn_tex_storage(&d), storage + 0x1000);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, again, 0, 0, 0), RET);
    CHECK_EQ(nvn_tex_storage(&d), storage + 0x1000);
    /* The guest discarding the pages frees the kept import: the texture has no storage left. */
    CHECK_EQ(rl_vm_set(&hos.vm, storage, 0x40000, RL_VM_RW, 0, RL_VM_DISCARD), 0);
    CHECK_EQ(nvn_tex_storage(&d), 0);
}

TEST(nvn_pool_mappings_take_a_textures_page_kind)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t pool = heap, params = heap + 0x200, storage = heap + 0x100000;
    CHECK(nvn_pool_make(pool, storage, 0x38000, 0xa2));
    rl_st32(t_mem(), params, 0xdb);
    CHECK_EQ(call(c, nvnh_NvRmGpuMappingModify, pool, 0x10000, 0x30000, params), RET);
    CHECK_EQ(c->x[0], 0);
    CHECK_EQ(call(c, nvnh_NvRmGpuMappingModify, pool, 0, 0x10000, 0), RET);
    CHECK_EQ(c->x[0], 4);
    CHECK_EQ(call(c, nvnh_NvRmGpuMappingModify, pool, 0x10000, 0x40000, params), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(call(c, nvnh_NvRmGpuMappingModify, params, 0, 0x10000, params), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, pool, 0, 0, 0), RET);
}

TEST(nvn_render_targets_reach_every_level_layer_and_slice)
{
    nvn_tex_desc arr = { .target = 4, .format = 0x25, .width = 64, .height = 64, .depth = 24, .levels = 7 };
    CHECK(nvn_vk_rt_inside(&arr, 6, 23));
    CHECK(!nvn_vk_rt_inside(&arr, 7, 0));
    CHECK(!nvn_vk_rt_inside(&arr, 0, 24));
    nvn_tex_desc vol = { .target = 2, .format = 0x25, .width = 32, .height = 32, .depth = 16, .levels = 3 };
    CHECK(nvn_vk_rt_inside(&vol, 0, 15));
    CHECK(nvn_vk_rt_inside(&vol, 2, 3));
    CHECK(!nvn_vk_rt_inside(&vol, 2, 4));
    nvn_tex_desc cube = { .target = 8, .format = 0x25, .width = 16, .height = 16, .levels = 5 };
    CHECK(nvn_vk_rt_inside(&cube, 4, 5));
    CHECK(!nvn_vk_rt_inside(&cube, 0, 6));
}

static uint32_t f32_bits(float x)
{
    uint32_t v;
    memcpy(&v, &x, sizeof v);
    return v;
}

static float f32_of(uint32_t v)
{
    float x;
    memcpy(&x, &v, sizeof x);
    return x;
}

/** @brief Writes the NaN probe's outputs as a device would whose NaNs are canonical when canon, kept payloads otherwise. */
static void nan_device(uint32_t *w, bool canon)
{
    uint32_t rows = nvn_probe_nan_rows();
    for (uint32_t i = 0; i < rows; i++) {
        float a = f32_of(w[3 * i]), b = f32_of(w[3 * i + 1]), c = f32_of(w[3 * i + 2]);
        uint32_t r[5] = { f32_bits(a + b), f32_bits(a * b), f32_bits(fmaf(a, b, c)), f32_bits(fminf(a, b)),
                          f32_bits(fmaxf(a, b)) };
        for (int k = 0; k < 5; k++) {
            if (isnan(f32_of(r[k])) && canon)
                r[k] = 0x7fffffffu;
            w[3 * rows + 5 * i + k] = r[k];
        }
    }
}

TEST(nvn_probe_checks_a_fused_multiply_add_by_every_row)
{
    uint32_t rows = nvn_probe_gfma_rows();
    uint32_t *w = calloc(4, rows * 4ull);
    CHECK(w != NULL);
    nvn_probe_gfma_input(w);
    for (uint32_t i = 0; i < rows; i++) {
        float a, b, c, f;
        memcpy(&a, w + 3 * i, 4);
        memcpy(&b, w + 3 * i + 1, 4);
        memcpy(&c, w + 3 * i + 2, 4);
        f = fmaf(a, b, c);
        memcpy(w + 3 * rows + i, &f, 4);
    }
    CHECK(nvn_probe_gfma_check(w));
    /* A multiply rounded before the add is what an unfused device gives, and the inputs tell it from the fused result. */
    for (uint32_t i = 0; i < rows; i++) {
        float a, b, c;
        memcpy(&a, w + 3 * i, 4);
        memcpy(&b, w + 3 * i + 1, 4);
        memcpy(&c, w + 3 * i + 2, 4);
        volatile float p = a * b;
        float f = p + c;
        memcpy(w + 3 * rows + i, &f, 4);
    }
    CHECK(!nvn_probe_gfma_check(w));
    free(w);
}

TEST(nvn_probe_checks_nan_results)
{
    uint32_t rows = nvn_probe_nan_rows();
    uint32_t *w = calloc(8 * rows, sizeof *w);
    CHECK(w != NULL);
    nvn_probe_nan_input(w);
    nan_device(w, true);
    CHECK(nvn_probe_nan_check(w));
    nan_device(w, false);
    CHECK(!nvn_probe_nan_check(w));
    nan_device(w, true);
    for (uint32_t i = 0; i < rows; i++)
        if (w[3 * i] == 0x7fc00000u && w[3 * i + 1] == 0x00000000u)
            w[3 * rows + 5 * i] = 0x00000000u;
    CHECK(!nvn_probe_nan_check(w));
    memset(w + 3 * rows, 0, 5 * rows * sizeof *w);
    CHECK(!nvn_probe_nan_check(w));
    free(w);
}

TEST(nvn_half_rounding_reference)
{
    CHECK_EQ(nvn_half_of(1.0), 0x3c00);
    CHECK_EQ(nvn_half_of(-2.0), 0xc000);
    CHECK_EQ(nvn_half_of(65504.0), 0x7bff);
    CHECK_EQ(nvn_half_of(65519.99), 0x7bff);
    CHECK_EQ(nvn_half_of(65520.0), 0x7c00);
    CHECK_EQ(nvn_half_of(ldexp(1.0, -25)), 0x0000);
    CHECK_EQ(nvn_half_of(ldexp(3.0, -26)), 0x0001);
    CHECK_EQ(nvn_half_of(ldexp(3.0, -25)), 0x0002);
    CHECK_EQ(nvn_half_of(ldexp(1023.5, -24)), 0x0400);
    CHECK_EQ(nvn_half_of(1.0 + ldexp(1.0, -11)), 0x3c00);
    CHECK_EQ(nvn_half_of(1.0 + ldexp(3.0, -11)), 0x3c02);
    CHECK_EQ(nvn_half_of(-0.0), 0x8000);
    CHECK(nvn_half_value(0x0001) == ldexp(1.0, -24));
    CHECK(nvn_half_value(0x7bff) == 65504.0);
    CHECK(isinf(nvn_half_value(0xfc00)) && nvn_half_value(0xfc00) < 0);
    CHECK(isnan(nvn_half_value(0x7c01)));
    for (uint32_t h = 0; h < 0x10000; h++)
        if ((h & 0x7fff) <= 0x7c00)
            CHECK_EQ(nvn_half_of(nvn_half_value((uint16_t)h)), h);
}

/** @brief Writes the half probe's outputs as an exact binary16 device would, or one that flushes subnormal results. */
static void half_device(uint32_t *w, bool flush)
{
    uint32_t n = nvn_probe_f16_rows(), m = nvn_probe_f16_convs();
    for (uint32_t i = 0; i < 2 * n; i++) {
        unsigned sh = (i & 1) * 16;
        double x = nvn_half_value((uint16_t)(w[3 * (i / 2)] >> sh));
        double y = nvn_half_value((uint16_t)(w[3 * (i / 2) + 1] >> sh));
        double z = nvn_half_value((uint16_t)(w[3 * (i / 2) + 2] >> sh));
        double r[3] = { x + y, x * y, fma(x, y, z) };
        for (int k = 0; k < 3; k++) {
            uint16_t h = nvn_half_of(r[k]);
            if (flush && (h & 0x7c00) == 0)
                h &= 0x8000;
            uint32_t *o = &w[3 * n + 3 * (i / 2) + k];
            *o = (*o & ~(0xffffu << sh)) | (uint32_t)h << sh;
        }
    }
    for (uint32_t i = 0; i < m; i++)
        w[6 * n + m + i] = nvn_half_of((double)f32_of(w[6 * n + i]));
    for (uint32_t h = 0; h < 65536; h++) {
        double v = nvn_half_value((uint16_t)h);
        w[6 * n + 2 * m + 32768 + h] = isnan(v) ? 0x7fc00000u : f32_bits((float)v);
    }
}

TEST(nvn_probe_checks_half_results)
{
    uint32_t *w = calloc(nvn_probe_f16_words(), sizeof *w);
    CHECK(w != NULL);
    nvn_probe_f16_input(w);
    half_device(w, false);
    CHECK_EQ(nvn_probe_f16_check(w), 0);
    half_device(w, true);
    CHECK(nvn_probe_f16_check(w) > 0);
    half_device(w, false);
    uint32_t n = nvn_probe_f16_rows(), m = nvn_probe_f16_convs();
    w[6 * n + 2 * m + 32768 + 1] = 0;
    CHECK_EQ(nvn_probe_f16_check(w), 1);
    free(w);
}

/**
 * @brief One workgroup of the hmma case of gpus/sm86/tests/kat/compute.txt: a 16x16 A, 16x8 B and 16x8 C, row-major
 * packed halves, as the NVIDIA device read them.
 */
static const uint32_t hmma_kat_in[256] = {
    0x2a3a2107, 0x6b01a1c1, 0xb09490b8, 0x6b0404f2, 0xa28f5b37, 0x48007596, 0x7aa6540d, 0xd7e11b1b,
    0x374cb756, 0xfd5e5ee3, 0xcaea0518, 0x79827b7a, 0xcecf8a17, 0xf69542b8, 0x8330550f, 0x2eff2f12,
    0x814d31e8, 0x870d6796, 0x3c6e3096, 0xc9d4d020, 0x00d0722d, 0x039d74ed, 0x5efcea76, 0xeeca8c28,
    0x95da5109, 0x6d9deeee, 0x11bb55f8, 0x25199d60, 0xcb33444b, 0xc056855f, 0x3b74e9fb, 0xebe718df,
    0x3b91e572, 0xb1a4a4f9, 0x0ad67e72, 0x6fd5ca04, 0xff602bda, 0xfed0c435, 0xbd1aa3f1, 0xc54cb0e4,
    0xe0029715, 0x682204bb, 0x9daaf919, 0x711c718a, 0x08b8d0a0, 0xf4ee9a03, 0x5434b6b5, 0x8a473a6a,
    0x7f5f96b6, 0xb1a470b6, 0x1da5b627, 0xa618d143, 0xf3b3eb97, 0xf01aea92, 0x5e5284e4, 0x05f204ab,
    0x27a1d402, 0xbdb39a62, 0xd1484c93, 0x16a591f4, 0x1ff7f212, 0xcf014130, 0xed8dbab6, 0x05013278,
    0x74002b8e, 0x293dc206, 0x8d500f76, 0xba7c3a75, 0x57c9b2c0, 0x66be6e54, 0x7c1589b4, 0x26ae54ee,
    0x2fd940bb, 0xaa448655, 0x0981fa59, 0x784e1ea4, 0xddaaad70, 0xd1f4fb87, 0x3c202fb0, 0x131a83dc,
    0xc7bf13aa, 0x3287533d, 0x35645553, 0xac7dc96b, 0xf8013ebb, 0xcd8bbe9c, 0xe4daf1c3, 0xa87266a2,
    0xc13a13f3, 0xf305be92, 0xc34660fc, 0xe5bb876a, 0x94ad0fa3, 0xfd234065, 0xbad1612a, 0x122842b4,
    0x1f5ab5ad, 0x8f643855, 0x79f075e6, 0x8f16dc8b, 0xd3b7750f, 0x6354951f, 0x3379f0ee, 0x575aec6a,
    0xec54b3b3, 0x256a28b4, 0xe3371d01, 0x892bb303, 0x68dba816, 0x4abfadfd, 0x5d7f17ea, 0xf45aa8b6,
    0x7389d071, 0x805e5007, 0x62c25387, 0x51654acd, 0xfeae0341, 0x7c0513a4, 0x62f7c6f9, 0x44053836,
    0x06deb3b1, 0xd588ee38, 0x17e00db8, 0x28b08946, 0xb7d4ea02, 0xb74ba47d, 0xfe29acbe, 0xfb7e0776,
    0x7efa8fd2, 0x00b998ee, 0x7dd58a3f, 0x11f329f0, 0x8ce90a07, 0x9970d71d, 0xbf8876b4, 0x85ad1e9f,
    0x38d727ff, 0x924f0814, 0x9be0f62c, 0x51927773, 0x8e75ee98, 0xfae0f5e0, 0x9f6433ff, 0x1cda89e2,
    0x2e40df04, 0xab6fe701, 0x2b8b23a4, 0xa0621cba, 0x59241c90, 0xd56aa653, 0xed54b226, 0x5516bcc2,
    0x58bba837, 0xc308bd98, 0x98e47ee9, 0xee010ee0, 0x4e7e4092, 0x3eea7b74, 0x1a701d5a, 0x2b2f6d0a,
    0x10ce0a84, 0xec523b81, 0xd04e5ef4, 0x2c8c7375, 0xc6eab0aa, 0x6dc04993, 0x5ff7d829, 0x28ee3a6d,
    0xaef024b2, 0x0964ffa7, 0xad07e625, 0x85089ee2, 0xe6110c52, 0xa8716492, 0xf551d892, 0x3dd66ac8,
    0x68321cc1, 0xeaf3e110, 0xfd3649f4, 0x29c0047c, 0xf62042d7, 0xbdea967e, 0x972c3d70, 0x03a6b983,
    0xe2176811, 0x58bb65bb, 0x7dac664c, 0x8191fdec, 0x6024b5b0, 0xaabc5427, 0x3f49026e, 0xefb088c3,
    0xe04b034c, 0x3ef177ca, 0x298c0166, 0x8fd338ea, 0x11bff2d9, 0x707198b5, 0xce26f256, 0x684f8cd3,
    0xc61f665b, 0xbdcd3cdc, 0x7bb1388a, 0x0eb3cb7b, 0xd6458ddb, 0x49b91be1, 0x0d5b4027, 0xda7256f3,
    0x420ac140, 0xa88e9f61, 0x4a3539c7, 0x08e6209d, 0x6b67b77c, 0x3fa3e42d, 0x2bae62e1, 0x64ff9bd8,
    0x11c78edf, 0xded197d4, 0x568c912d, 0xa53b4250, 0xe62e615a, 0x2567c523, 0xfbc32fdb, 0x26b4f734,
    0xbeefe067, 0xba7d1aab, 0x7a9dfcc1, 0x0ca59ddf, 0x40912637, 0x23bc15b4, 0xf578ff92, 0xa676ce40,
    0x868f8b23, 0xe95478a4, 0xba861635, 0x7aa41c0e, 0x073ea193, 0xf3ec9c22, 0x364ba0c0, 0xe641a0d0,
    0xd69874da, 0x5da0239a, 0xcb936d5a, 0x276229bc, 0xe1a6e407, 0x5e66f3ce, 0x99f273db, 0xc5d3d13c,
    0x6d13be18, 0x7c05794b, 0x94c58f0a, 0xe7100e9c, 0xd96e7b10, 0x7a12c8e3, 0x4ed7994c, 0x23881349,
};
/** @brief The 16x8 D that device's HMMA gave for hmma_kat_in, then for it with every word ANDed with 0xbfffbfff. */
static const uint32_t hmma_kat_out[128] = {
    0x7ffffc00, 0xfc007fff, 0x7fff7fff, 0x7c007fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff,
    0x7fff6dd1, 0xfc007fff, 0x7fff7fff, 0x7c007fff, 0x7ffffc00, 0xfc007fff, 0x7fff7fff, 0xfc007fff,
    0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7c00, 0xfc007fff, 0x7fff7fff, 0xfc007fff,
    0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7ffff422, 0x7c007fff, 0x7fff7fff, 0xec427fff,
    0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7c00, 0x7c007fff, 0x7fff7fff, 0x687d7fff,
    0x7fff7c00, 0xfc007fff, 0x7fff7fff, 0x7c007fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff,
    0x7ffffc00, 0xfc007fff, 0x7fff7fff, 0x7c007fff, 0x7fff6dc8, 0x7c007fff, 0x7fff7fff, 0xfc007fff,
    0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff, 0x7fff7fff,
    0xb5ac1932, 0x386db0a3, 0xaa8c3cc0, 0x19f3323b, 0xba43b086, 0x40fab1c1, 0xbc1b3d1c, 0xb915b0ad,
    0x35172ca9, 0xbde63a35, 0x3bf43590, 0x2666bd06, 0xb6cb20be, 0xbda69ad8, 0xa9e43d48, 0x1c40b98c,
    0x391c3452, 0x400330db, 0xbd6eb8fc, 0xc11fb6e1, 0xa5e2b72f, 0x4075ad43, 0x3f48354d, 0xb735bcc4,
    0x2b0e2b9b, 0x2e512db2, 0xc2e53961, 0xb309b0b9, 0xa6223113, 0x3dbe35c4, 0x35cdb55d, 0xa816b9d3,
    0xc010aa07, 0xc37dc092, 0x377f3ecd, 0xae6bba88, 0xb5dc24b6, 0xb7e34080, 0xb999b6d8, 0x209eb984,
    0x31e2288e, 0xb753452c, 0xb8162300, 0x3b64b6d1, 0xb912a921, 0x3cd4285d, 0x3a8eb8b0, 0xbf9bb420,
    0xb5933516, 0xb60734ad, 0xb78f2a80, 0x2c0db8af, 0xb66f2d0a, 0x90c53283, 0xba23b118, 0x2abc2e26,
    0x2990bd0d, 0x3a693983, 0xa9f9301c, 0x3d7fc02e, 0x2f243d22, 0x424224ca, 0xbf3ac01d, 0xb87e38a0,
};

/** @brief Half `col` of row-major packed halves with `stride` words a row. */
static uint32_t kat_half(const uint32_t *s, int stride, int row, int col)
{
    return s[stride * row + col / 2] >> (16 * (col & 1)) & 0xffff;
}

TEST(nvn_hmma_model_matches_the_recorded_device)
{
    for (int pass = 0; pass < 2; pass++) {
        uint32_t s[256], a[128], b[64], c[64], d[64];
        for (int k = 0; k < 256; k++)
            s[k] = pass ? hmma_kat_in[k] & 0xbfffbfffu : hmma_kat_in[k];
        for (int l = 0; l < 32; l++) {
            int g = l >> 2, q = l & 3;
            a[4 * l] = s[8 * g + q];
            a[4 * l + 1] = s[8 * (g + 8) + q];
            a[4 * l + 2] = s[8 * g + q + 4];
            a[4 * l + 3] = s[8 * (g + 8) + q + 4];
            for (int r = 0; r < 2; r++)
                b[2 * l + r] = kat_half(s + 128, 4, 2 * q + 8 * r, g) | kat_half(s + 128, 4, 2 * q + 8 * r + 1, g) << 16;
            c[2 * l] = s[192 + 4 * g + q];
            c[2 * l + 1] = s[192 + 4 * (g + 8) + q];
        }
        nvn_hmma_warp(true, a, b, c, d);
        const uint32_t *o = hmma_kat_out + 64 * pass;
        for (int l = 0; l < 32; l++) {
            int g = l >> 2, q = l & 3;
            CHECK_EQ(d[2 * l], o[4 * g + q]);
            CHECK_EQ(d[2 * l + 1], o[4 * (g + 8) + q]);
        }
    }
}

TEST(nvn_hmma_step_specials)
{
    uint16_t one[8] = { 0x3c00, 0, 0, 0, 0, 0, 0, 0 }, inf[8] = { 0x7c00, 0, 0, 0, 0, 0, 0, 0 };
    uint16_t zero[8] = { 0 }, nan[8] = { 0x7e01, 0, 0, 0, 0, 0, 0, 0 }, two[8] = { 0x4000, 0, 0, 0, 0, 0, 0, 0 };
    CHECK_EQ(nvn_hmma_step(0x3c00, one, two), 0x4200);
    CHECK_EQ(nvn_hmma_step(0x8000, zero, zero), 0x0000);
    CHECK_EQ(nvn_hmma_step(0x0000, inf, two), 0x7c00);
    CHECK_EQ(nvn_hmma_step(0xfc00, inf, two), 0x7fff);
    CHECK_EQ(nvn_hmma_step(0x0000, inf, zero), 0x7fff);
    CHECK_EQ(nvn_hmma_step(0x3c00, nan, one), 0x7fff);
    uint16_t sixteen[8] = { 0x4c00, 0, 0, 0, 0, 0, 0, 0 };
    CHECK_EQ(nvn_hmma_step(0x7bff, one, one), 0x7bff);
    CHECK_EQ(nvn_hmma_step(0x7bff, one, sixteen), 0x7c00);
}

/** @brief Writes the HMMA probe's outputs as nvn_hmma_warp gives them. */
static void hmma_probe_answer(uint32_t *w)
{
    uint32_t tiles = nvn_probe_hmma_tiles();
    for (uint32_t t = 0; t < 2 * tiles; t++) {
        bool k16 = t < tiles;
        uint32_t per = k16 ? 8 : 5;
        const uint32_t *in = w + (k16 ? 256 * t : 256 * tiles + 160 * (t - tiles));
        uint32_t a[128] = { 0 }, b[64], c[64];
        for (int l = 0; l < 32; l++) {
            const uint32_t *x = in + per * l;
            for (int r = 0; r < (k16 ? 4 : 2); r++)
                a[4 * l + r] = x[r];
            b[2 * l] = x[k16 ? 4 : 2];
            b[2 * l + 1] = k16 ? x[5] : 0;
            c[2 * l] = x[k16 ? 6 : 3];
            c[2 * l + 1] = x[k16 ? 7 : 4];
        }
        nvn_hmma_warp(k16, a, b, c, w + 256 * tiles + 160 * tiles + 64 * t);
    }
}

TEST(nvn_probe_checks_hmma_results)
{
    uint32_t words = nvn_probe_hmma_words(), tiles = nvn_probe_hmma_tiles();
    uint32_t *w = calloc(words, sizeof *w);
    CHECK(w != NULL);
    nvn_probe_hmma_input(w);
    hmma_probe_answer(w);
    CHECK_EQ(nvn_probe_hmma_check(w), 0);
    w[256 * tiles + 160 * tiles + 5] ^= 1;
    CHECK_EQ(nvn_probe_hmma_check(w), 1);
    for (uint32_t l = 0; l < 32; l++) {
        uint32_t t = w[8 * l + 1];
        w[8 * l + 1] = w[8 * l + 2];
        w[8 * l + 2] = t;
    }
    hmma_probe_answer(w);
    for (uint32_t l = 0; l < 32; l++) {
        uint32_t t = w[8 * l + 1];
        w[8 * l + 1] = w[8 * l + 2];
        w[8 * l + 2] = t;
    }
    CHECK(nvn_probe_hmma_check(w) >= 60);
    free(w);
}

TEST(nvn_probes_reach_device_json)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_probes_reach_device_json: no host Vulkan device, skipped\n");
        return;
    }
    char path[PATH_MAX];
    snprintf(path, sizeof path, "%s/device.json", nvn_shader_dir());
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "nvn_probes_reach_device_json: %s was written elsewhere, skipped\n", path);
        return;
    }
    static char text[1 << 16];
    size_t len = fread(text, 1, sizeof text - 1, f);
    fclose(f);
    text[len] = 0;
    char want[256];
    snprintf(want, sizeof want,
             "\"canonicalNan32\": %s, \"exactFloat16\": %s, \"hmmaCooperativeMatrix\": %s, \"denormDefault32\": %u, "
             "\"denormPreserveMode32\": %s, \"denormAddFlush32\": %s, \"denormMulFix32\": %u",
             p.canonical_nan32 ? "true" : "false", p.exact_f16 ? "true" : "false", p.hmma_coop ? "true" : "false",
             p.denorm32, p.denorm_preserve_mode32 ? "true" : "false", p.denorm_add_flush ? "true" : "false",
             p.denorm_mul_fix);
    CHECK(strstr(text, want) != NULL);
    fprintf(stderr, "nvn_probes_reach_device_json: %s\n", want);
    CHECK(strstr(text, "\"standardSampleLocations\": true, \"framebufferColorSampleCounts\": ") ||
          strstr(text, "\"standardSampleLocations\": false, \"framebufferColorSampleCounts\": "));
}

TEST(nvn_denorm_probe_holds_on_millions_of_random_rows)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_denorm_probe_holds_on_millions_of_random_rows: no host Vulkan device, skipped\n");
        return;
    }
    nvn_denorm_class big;
    for (uint32_t seed = 1; seed <= 2; seed++) {
        CHECK(nvn_vk_denorm_stress(4000000, seed, &big));
        fprintf(stderr, "denorm stress seed %u: add models %u, mul models %u, mul fix %u\n", seed, big.add_models,
                big.mul_models, big.mul_fix);
        CHECK((big.add_models & 1) == (uint32_t)p.denorm_add_flush);
        CHECK(p.denorm_mul_fix == 255 ? big.mul_fix_ok == 0 : (big.mul_fix_ok >> p.denorm_mul_fix & 1) != 0);
    }
}

TEST(nvn_deferred_counter_reports_read_what_the_waiting_path_read)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK(heap != 0);
    CHECK_EQ(nvn_counters_order_check(t_mem(), heap + 0x1000), 0);
}

TEST(nvn_a_conditional_render_waits_only_for_a_report_or_write_still_due_at_its_counters)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_a_conditional_render_waits_only_for_a_report_or_write_still_due_at_its_counters: no host Vulkan device, skipped\n");
        return;
    }
    rl_ctx *c = t_boot(NULL);
    if (!p.counter_queries) {
        /* MoltenVK has no pipeline statistics queries: the host device counts nothing, with or without this change. */
        fprintf(stderr, "nvn_a_conditional_render_waits_only_for_a_report_or_write_still_due_at_its_counters: the host device has no counter queries, skipped\n");
        return;
    }
    uint64_t heap = t_heap(c, 0x200000);
    CHECK(heap != 0);
    CHECK_EQ(nvn_counters_cond_check(t_mem(), heap + 0x2000), 0);
}

TEST(nvn_timestamp_reports_land_once_the_gpu_ran_them_without_a_wait)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_timestamp_reports_land_once_the_gpu_ran_them_without_a_wait: no host Vulkan device, skipped\n");
        return;
    }
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK(heap != 0);
    CHECK_EQ(nvn_counters_async_check(t_mem(), heap + 0x100), 0);
    fprintf(stderr, "  timestamps %s\n", nvn_vk_calibrated() ? "calibrated" : "on the fallback (no calibrated pair)");
}

TEST(nvn_the_next_batch_is_recorded_while_one_runs_and_writers_are_waited_for)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_the_next_batch_is_recorded_while_one_runs_and_writers_are_waited_for: no host Vulkan device, skipped\n");
        return;
    }
    CHECK_EQ(nvn_vk_ring_check(), 0);
}

TEST(nvn_a_submit_never_waits_for_an_old_batch_while_a_slot_can_be_made)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_a_submit_never_waits_for_an_old_batch_while_a_slot_can_be_made: no host Vulkan device, skipped\n");
        return;
    }
    CHECK_EQ(nvn_vk_grow_check(), 0);
}

TEST(nvn_a_host_signaled_timeline_value_never_comes_before_its_fence)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_a_host_signaled_timeline_value_never_comes_before_its_fence: no host Vulkan device, skipped\n");
        return;
    }
    CHECK_EQ(nvn_vk_timeline_check(), 0);
}

TEST(nvn_a_batch_reads_done_only_once_its_writes_are_visible)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_a_batch_reads_done_only_once_its_writes_are_visible: no host Vulkan device, skipped\n");
        return;
    }
    CHECK_EQ(nvn_vk_done_word_check(), 0);
}

TEST(nvn_a_submit_returns_before_the_device_finished_and_a_settle_sees_its_results)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_a_submit_returns_before_the_device_finished_and_a_settle_sees_its_results: no host Vulkan device, skipped\n");
        return;
    }
    CHECK_EQ(nvn_vk_async_check(), 0);
}

TEST(nvn_a_read_of_bytes_the_open_batch_writes_waits_for_it)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_a_read_of_bytes_the_open_batch_writes_waits_for_it: no host Vulkan device, skipped\n");
        return;
    }
    CHECK_EQ(nvn_vk_open_write_check(), 0);
}

TEST(nvn_a_staging_buffer_kept_for_its_batch_serves_the_copy_without_a_wait)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_a_staging_buffer_kept_for_its_batch_serves_the_copy_without_a_wait: no host Vulkan device, skipped\n");
        return;
    }
    CHECK_EQ(nvn_vk_keep_check(), 0);
}

/**
 * The block-linear address of block (bx, by) of layer k of a BC7 array (16-byte blocks) of wb x hb blocks, computed from the
 * console's rules alone: GOBs of 64 bytes by 8 rows, laid out gob_h GOBs tall per block, the blocks in rows across the
 * width, the layer stride the layer's size rounded to a whole block. in_gob is the in-GOB offset of byte x of row y.
 */
static uint64_t independent_address(uint32_t bx, uint32_t by, uint32_t k, uint32_t wb, uint32_t hb, uint32_t gob_h,
                                    uint32_t (*in_gob)(uint32_t, uint32_t))
{
    uint64_t gobs_wide = ((uint64_t)wb * 16 + 63) / 64;
    uint64_t gob_rows = (hb + 7) / 8;
    uint64_t block_rows = (gob_rows + gob_h - 1) / gob_h;
    uint64_t layer_bytes = block_rows * gobs_wide * 512ull * gob_h;
    uint64_t stride = (layer_bytes + 512ull * gob_h - 1) / (512ull * gob_h) * 512ull * gob_h;
    uint64_t xbyte = (uint64_t)bx * 16;
    uint64_t block = (by / 8) / gob_h * gobs_wide + xbyte / 64;
    return stride * k + block * 512ull * gob_h + ((by / 8) % gob_h) * 512ull + in_gob((uint32_t)(xbyte % 64), by % 8);
}

TEST(nvn_a_three_layer_bc7_array_of_the_rock_size_lands_where_the_console_puts_it)
{
    static const uint32_t heights[] = { 4, 16 };
    for (uint32_t hi = 0; hi < 2; hi++) {
        nvn_tex_desc d = { .target = 4, .format = 77, .width = 3056, .height = 868, .depth = 3, .levels = 1 };
        CHECK(nvn_tex_layout_block(&d, heights[hi]));
        uint32_t wb = 764, hb = 217;
        CHECK_EQ(d.block_h, heights[hi]);
        /* the layer stride and size are the layer rounded up to the block, three of them */
        uint64_t want_stride = independent_address(0, 0, 1, wb, hb, heights[hi], nvn_gob_offset);
        CHECK_EQ(d.layer_stride, want_stride);
        CHECK_EQ(d.size, want_stride * 3);
        uint8_t *store = calloc(1, d.size);
        CHECK(store != NULL);
        if (!store)
            return;
        /* write every block (its own marker) through relift's copy, read it back through the independent address */
        uint32_t bad = 0, lost = 0;
        uint8_t *row = malloc((size_t)wb * 16);
        for (uint32_t k = 0; k < 3; k++)
            for (uint32_t by = 0; by < hb; by++) {
                for (uint32_t bx = 0; bx < wb; bx++) {
                    uint32_t id = (k * hb + by) * wb + bx + 1;
                    memset(row + bx * 16, 0, 16);
                    memcpy(row + bx * 16, &id, 4);
                    row[bx * 16 + 15] = (uint8_t)(id * 7);
                }
                nvn_region rg = { 0, (int32_t)(by * 4), 0, 3056, 4, 1 };
                lost += !nvn_tex_copy(&d, store, 0, k, &rg, row, true);
            }
        for (uint32_t k = 0; k < 3; k++)
            for (uint32_t by = 0; by < hb; by++)
                for (uint32_t bx = 0; bx < wb; bx++) {
                    uint32_t id = (k * hb + by) * wb + bx + 1, got;
                    /* the console keeps each 16-byte block whole, in one line of its GOB */
                    uint64_t at = independent_address(bx, by, k, wb, hb, heights[hi], nvn_gob_offset);
                    memcpy(&got, store + at, 4);
                    bad += got != id || store[at + 15] != (uint8_t)(id * 7);
                }
        CHECK_EQ(lost, 0u);
        CHECK_EQ(bad, 0u);
        free(row);
        free(store);
    }
}

TEST(nvn_gpu_texel_moves_match_the_cpu_layout_both_ways)
{
    nvn_probes p;
    if (!nvn_vk_probes(&p)) {
        fprintf(stderr, "nvn_gpu_texel_moves_match_the_cpu_layout_both_ways: no host Vulkan device, skipped\n");
        return;
    }
    for (uint32_t seed = 1; seed <= 2; seed++)
        CHECK_EQ(nvn_untile_check(seed), 0);
    /* the packed uploads were cut from slabs that later batches reused: far fewer slabs than uploads, none a device allocation each */
    uint64_t made, cut;
    nvn_untile_arena_stats(&made, &cut);
    CHECK(cut >= 8);
    CHECK(made * 2 <= cut);
}

#define OBJ_ADDRS 8u
#define OBJ_ROUNDS 100000u

typedef struct held_obj {
    nvn_obj o;
    uint32_t w, h;
    uint32_t *drops;
} held_obj;

typedef struct obj_hammer {
    uint64_t base;
    bool done;
    uint32_t bad, drops, made;
} obj_hammer;

static uint64_t obj_addr(const obj_hammer *h, uint32_t i) { return h->base + 0x100ull * i; }

static void held_drop(nvn_obj *o)
{
    held_obj *h = (held_obj *)o;
    h->w = 1;
    h->h = 7;
    __atomic_add_fetch(h->drops, 1, __ATOMIC_RELAXED);
}

static void held_make(obj_hammer *h, uint32_t i, uint32_t n)
{
    held_obj *o = (held_obj *)nvn_obj_new(obj_addr(h, i), NVN_EVENT, sizeof *o);
    if (!o)
        return;
    o->h = 1 + n % 97;
    o->w = 2 * o->h;
    o->drops = &h->drops;
    o->o.drop = held_drop;
    __atomic_add_fetch(&h->made, 1, __ATOMIC_RELAXED);
    CHECK(nvn_obj_publish(&o->o) == &o->o);
}

static void *obj_writer(void *arg)
{
    obj_hammer *h = arg;
    for (uint32_t n = 0; n < OBJ_ROUNDS; n++) {
        held_make(h, n % OBJ_ADDRS, n);
        if (n % 5 == 0)
            nvn_obj_free(nvn_obj_get(obj_addr(h, (n + 3) % OBJ_ADDRS), 0));
        if (n % 8 == 0)
            usleep(20);
    }
    __atomic_store_n(&h->done, true, __ATOMIC_RELEASE);
    return NULL;
}

static void *obj_reader(void *arg)
{
    obj_hammer *h = arg;
    for (uint32_t n = 0; !__atomic_load_n(&h->done, __ATOMIC_ACQUIRE); n++) {
        held_obj *o = (held_obj *)nvn_obj_acquire(obj_addr(h, n % OBJ_ADDRS), NVN_EVENT);
        if (!o)
            continue;
        if (o->w != 2 * o->h)
            __atomic_add_fetch(&h->bad, 1, __ATOMIC_RELAXED);
        if (n % 16 == 0) {
            usleep(200);
            if (o->w != 2 * o->h)
                __atomic_add_fetch(&h->bad, 1, __ATOMIC_RELAXED);
        }
        nvn_obj_release(&o->o);
    }
    return NULL;
}

TEST(nvn_objects_survive_create_finalize_and_lookup_from_several_threads)
{
    obj_hammer h = { .base = 0x60000000ull };
    pthread_t w, r[3];
    for (uint32_t i = 0; i < OBJ_ADDRS; i++)
        held_make(&h, i, i);
    for (uint32_t i = 0; i < 3; i++)
        CHECK_EQ(pthread_create(&r[i], NULL, obj_reader, &h), 0);
    CHECK_EQ(pthread_create(&w, NULL, obj_writer, &h), 0);
    pthread_join(w, NULL);
    for (uint32_t i = 0; i < 3; i++)
        pthread_join(r[i], NULL);
    CHECK_EQ(h.bad, 0u);
    for (uint32_t i = 0; i < OBJ_ADDRS; i++)
        nvn_obj_free(nvn_obj_get(obj_addr(&h, i), 0));
    CHECK_EQ(h.drops, h.made);
}

typedef struct look {
    obj_hammer *h;
    uint32_t *missing;
} look;

static void *obj_look(void *arg)
{
    look *l = arg;
    while (!__atomic_load_n(&l->h->done, __ATOMIC_ACQUIRE)) {
        nvn_obj *o = nvn_obj_acquire(l->h->base, NVN_EVENT);
        if (!o)
            __atomic_add_fetch(l->missing, 1, __ATOMIC_RELAXED);
        nvn_obj_release(o);
    }
    return NULL;
}

TEST(nvn_a_replaced_object_is_never_missing_from_a_lookup)
{
    obj_hammer h = { .base = 0x61000000ull };
    held_make(&h, 0, 0);
    pthread_t r;
    uint32_t missing = 0;
    look lk = { &h, &missing };
    CHECK_EQ(pthread_create(&r, NULL, obj_look, &lk), 0);
    for (uint32_t n = 0; n < 50000; n++)
        held_make(&h, 0, n);
    __atomic_store_n(&h.done, true, __ATOMIC_RELEASE);
    pthread_join(r, NULL);
    CHECK_EQ(missing, 0u);
    nvn_obj_free(nvn_obj_get(h.base, 0));
}

TEST(nvn_gpu_describe_names_the_live_and_the_freed_pool_that_held_an_address)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t pool = heap, storage = heap + 0x100000;
    CHECK(nvn_pool_make(pool, storage, 0x40000, 0x18c));
    uint64_t gpu = nvn_pool_gpu(pool);
    if (gpu == storage)
        return;
    char where[512];
    nvn_gpu_describe(gpu + 0x20, where, sizeof where);
    CHECK(strstr(where, "in pool span") != NULL);
    CHECK(strstr(where, "offset 0x20") != NULL);
    nvn_gpu_describe(gpu + 0x40010, where, sizeof where);
    CHECK(strstr(where, "0x10 past the span") != NULL);
    CHECK_EQ(call(c, nvnh_nvnMemoryPoolFinalize, pool, 0, 0, 0), RET);
    /* The finalized pool's import is kept (its span stays) until the guest discards the pages. */
    nvn_gpu_describe(gpu + 0x20, where, sizeof where);
    CHECK(strstr(where, "in pool span") != NULL);
    CHECK_EQ(rl_vm_set(&hos.vm, storage, 0x40000, RL_VM_RW, 0, RL_VM_DISCARD), 0);
    nvn_gpu_describe(gpu + 0x20, where, sizeof where);
    CHECK(strstr(where, "in pool span") == NULL);
    CHECK(strstr(where, "in FREED pool span") != NULL);
    CHECK(strstr(where, "0 frees ago") != NULL);
}

TEST(nvn_device_json_names_the_task_and_mesh_subgroup_stages)
{
    CHECK_EQ(strcmp(nvn_vk_stage_flag_name(0), "VK_SHADER_STAGE_VERTEX_BIT"), 0);
    CHECK_EQ(strcmp(nvn_vk_stage_flag_name(5), "VK_SHADER_STAGE_COMPUTE_BIT"), 0);
    CHECK_EQ(strcmp(nvn_vk_stage_flag_name(6), "VK_SHADER_STAGE_TASK_BIT_EXT"), 0);
    CHECK_EQ(strcmp(nvn_vk_stage_flag_name(7), "VK_SHADER_STAGE_MESH_BIT_EXT"), 0);
    CHECK(nvn_vk_stage_flag_name(8) == NULL);
}

/**
 * @brief A counter report's timestamp word holds ticks of the console's 19.2 MHz GPU timer, as its GPU writes it, and
 * nvnDeviceGetTimestampInNanoseconds converts them: 19,200,000 ticks are one second.
 */
TEST(nvn_counter_timestamps_are_gpu_ticks_and_convert_to_nanoseconds)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000), data = heap + 0x100;
    rl_st64(t_mem(), data, 0);
    rl_st64(t_mem(), data + 8, 19200000ull * 3 + 12);
    CHECK_EQ(call(c, nvnh_nvnDeviceGetTimestampInNanoseconds, heap, data, 0, 0), RET);
    CHECK_EQ(c->x[0], 3000000000ull + 625);
    CHECK_EQ(nvn_gpu_ticks(1000000000ull), 19200000ull);
    CHECK_EQ(nvn_gpu_ns(nvn_gpu_ticks(77000000ull)), 77000000ull);
}
