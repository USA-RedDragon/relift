/**
 * @file srv_nv_gpu.c
 * @brief /dev/nvhost-ctrl-gpu: the GPU description a driver reads at start.
 *
 * Ioctl numbers, argument layouts and the reported values follow Ryujinx
 * (MIT, Copyright (c) Ryujinx Team and Contributors)
 * NvHostCtrlGpuDeviceFile, which reports a Tegra X1.
 */
#include "nv.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

typedef struct gpu_characteristics {
    int32_t arch, impl, rev, num_gpc;
    int64_t l2_cache_size, on_board_video_memory_size;
    int32_t num_tpc_per_gpc, bus_type, big_page_size, compression_page_size, pde_coverage_bit_count,
        available_big_page_sizes, gpc_mask, sm_arch_sm_version, sm_arch_spa_version, sm_arch_warp_count,
        gpu_va_bit_count, reserved;
    int64_t flags;
    int32_t twod_class, threed_class, compute_class, gpfifo_class, inline_to_memory_class, dma_copy_class,
        max_fbps_count, fbp_en_mask, max_ltc_per_fbp, max_lts_per_ltc, max_tex_per_tpc, max_gpc_count,
        rop_l2_en_mask0, rop_l2_en_mask1;
    int64_t chip_name, gr_compbit_store_base_hw;
} gpu_characteristics;

typedef struct get_characteristics {
    int64_t buffer_size;
    int64_t buffer_address;
    gpu_characteristics c;
} get_characteristics;

typedef struct get_tpc_masks {
    int32_t mask_buffer_size;
    int32_t reserved;
    int64_t mask_buffer_address;
    int32_t tpc_mask;
    int32_t padding;
} get_tpc_masks;

typedef struct zcull_info {
    int32_t width_align_pixels, height_align_pixels, pixel_squares_by_aliquots, aliquot_total,
        region_byte_multiplier, region_header_size, subregion_header_size, subregion_width_align_pixels,
        subregion_height_align_pixels, subregion_count;
} zcull_info;

typedef struct num_vsms {
    uint32_t num_vsms;
    uint32_t reserved;
} num_vsms;

typedef struct vsms_mapping {
    uint8_t sm0_gpc, sm0_tpc, sm1_gpc, sm1_tpc;
    uint32_t reserved;
} vsms_mapping;

typedef struct active_slot_mask {
    int32_t slot;
    int32_t mask;
} active_slot_mask;

typedef struct gpu_time {
    uint64_t timestamp;
    uint64_t reserved;
} gpu_time;

_Static_assert(sizeof(gpu_characteristics) == 0xA0, "characteristics");
_Static_assert(sizeof(get_characteristics) == 0xB0, "get characteristics");
_Static_assert(sizeof(get_tpc_masks) == 0x18, "tpc masks");
_Static_assert(sizeof(zcull_info) == 0x28, "zcull info");

enum { GPU_EVENT_ERROR = 1, GPU_EVENT_UNKNOWN = 2 };

typedef struct gpu_priv {
    hos_revent *error_event;
    hos_revent *unknown_event;
} gpu_priv;

static uint64_t start_ns;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void fill_characteristics(gpu_characteristics *c)
{
    memset(c, 0, sizeof *c);
    c->arch = 0x120;
    c->impl = 0xB;
    c->rev = 0xA1;
    c->num_gpc = 0x1;
    c->l2_cache_size = 0x40000;
    c->num_tpc_per_gpc = 0x2;
    c->bus_type = 0x20;
    c->big_page_size = 0x20000;
    c->compression_page_size = 0x20000;
    c->pde_coverage_bit_count = 0x1B;
    c->available_big_page_sizes = 0x30000;
    c->gpc_mask = 0x1;
    c->sm_arch_sm_version = 0x503;
    c->sm_arch_spa_version = 0x503;
    c->sm_arch_warp_count = 0x80;
    c->gpu_va_bit_count = 0x28;
    c->flags = 0x55;
    c->twod_class = 0x902D;
    c->threed_class = 0xB197;
    c->compute_class = 0xB1C0;
    c->gpfifo_class = 0xB06F;
    c->inline_to_memory_class = 0xA140;
    c->dma_copy_class = 0xB0B5;
    c->max_fbps_count = 0x1;
    c->max_ltc_per_fbp = 0x2;
    c->max_lts_per_ltc = 0x1;
    c->max_gpc_count = 0x1;
    c->rop_l2_en_mask0 = 0x21D70;
    c->chip_name = 0x6230326D67;
}

static int32_t gpu_zcull_ctx_size(nv_file *f, nv_args *a)
{
    (void)f;
    int32_t size = 1;
    memcpy(a->data, &size, 4);
    return NVE_OK;
}

static int32_t gpu_zcull_info(nv_file *f, nv_args *a)
{
    (void)f;
    zcull_info *p = (zcull_info *)(void *)a->data;
    p->width_align_pixels = 0x20;
    p->height_align_pixels = 0x20;
    p->pixel_squares_by_aliquots = 0x400;
    p->aliquot_total = 0x800;
    p->region_byte_multiplier = 0x20;
    p->region_header_size = 0x20;
    p->subregion_header_size = 0xC0;
    p->subregion_width_align_pixels = 0x20;
    p->subregion_height_align_pixels = 0x40;
    p->subregion_count = 0x10;
    return NVE_OK;
}

static int32_t gpu_zbc_set_table(nv_file *f, nv_args *a)
{
    (void)f;
    (void)a;
    return NVE_OK;
}

static int32_t gpu_get_characteristics(nv_file *f, nv_args *a)
{
    (void)f;
    get_characteristics *p = (get_characteristics *)(void *)a->data;
    p->buffer_size = 0xA0;
    gpu_characteristics c;
    fill_characteristics(&c);
    if (a->size >= sizeof *p)
        p->c = c;
    if (a->inl && a->inl_size >= sizeof c)
        memcpy(a->inl, &c, sizeof c);
    return NVE_OK;
}

static int32_t gpu_get_tpc_masks(nv_file *f, nv_args *a)
{
    (void)f;
    get_tpc_masks *p = (get_tpc_masks *)(void *)a->data;
    if (p->mask_buffer_size != 0) {
        p->tpc_mask = 3;
        if (a->inl && a->inl_size >= 4) {
            int32_t mask = 3;
            memcpy(a->inl, &mask, 4);
        }
    }
    return NVE_OK;
}

static int32_t gpu_num_vsms(nv_file *f, nv_args *a)
{
    (void)f;
    ((num_vsms *)(void *)a->data)->num_vsms = 2;
    return NVE_OK;
}

static int32_t gpu_vsms_mapping(nv_file *f, nv_args *a)
{
    (void)f;
    vsms_mapping *p = (vsms_mapping *)(void *)a->data;
    p->sm0_gpc = 0;
    p->sm0_tpc = 0;
    p->sm1_gpc = 0;
    p->sm1_tpc = 1;
    return NVE_OK;
}

static int32_t gpu_active_slot_mask(nv_file *f, nv_args *a)
{
    (void)f;
    active_slot_mask *p = (active_slot_mask *)(void *)a->data;
    p->slot = 0x07;
    p->mask = 0x01;
    return NVE_OK;
}

static int32_t gpu_get_gpu_time(nv_file *f, nv_args *a)
{
    (void)f;
    gpu_time *p = (gpu_time *)(void *)a->data;
    if (start_ns == 0)
        start_ns = now_ns();
    p->timestamp = (now_ns() - start_ns) & 0xFFFFFFFFFFFFFFull;
    return NVE_OK;
}

static int32_t gpu_open(nv_file *f)
{
    gpu_priv *g = calloc(1, sizeof *g);
    if (!g)
        return NVE_OUT_OF_MEMORY;
    g->error_event = rl_hos_event_create();
    g->unknown_event = rl_hos_event_create();
    if (!g->error_event || !g->unknown_event) {
        hos_kobj_unref(g->error_event ? &g->error_event->obj : NULL);
        hos_kobj_unref(g->unknown_event ? &g->unknown_event->obj : NULL);
        free(g);
        return NVE_OUT_OF_MEMORY;
    }
    if (start_ns == 0)
        start_ns = now_ns();
    f->priv = g;
    return NVE_OK;
}

static void gpu_close(nv_file *f)
{
    gpu_priv *g = f->priv;
    hos_kobj_unref(&g->error_event->obj);
    hos_kobj_unref(&g->unknown_event->obj);
    free(g);
    f->priv = NULL;
}

static int32_t gpu_query_event(nv_file *f, uint32_t id, hos_revent **out)
{
    gpu_priv *g = f->priv;
    switch (id) {
    case GPU_EVENT_ERROR: *out = g->error_event; return NVE_OK;
    case GPU_EVENT_UNKNOWN: *out = g->unknown_event; return NVE_OK;
    default: return NVE_INVALID_INPUT;
    }
}

#define NG(nr, var, stub, min, name, fn) { NV_MAGIC_GPU, (nr), (var), (stub), (min), (name), (fn) }
#define V1 NV_V_IOCTL
#define V13 (NV_V_IOCTL | NV_V_IOCTL3)
static const nv_ioctl_entry gpu_ioctls[] = {
    NG(0x01, V1, false, 4, "ZcullGetCtxSize", gpu_zcull_ctx_size),
    NG(0x02, V1, false, 0x28, "ZcullGetInfo", gpu_zcull_info),
    NG(0x03, V1, true, 0x2C, "ZbcSetTable", gpu_zbc_set_table),
    NG(0x05, V13, false, 0x10, "GetCharacteristics", gpu_get_characteristics),
    NG(0x06, V13, false, 0x18, "GetTpcMasks", gpu_get_tpc_masks),
    NG(0x12, V13, true, 8, "NumVsms", gpu_num_vsms),
    NG(0x13, V13, true, 8, "VsmsMapping", gpu_vsms_mapping),
    NG(0x14, V1, true, 8, "GetActiveSlotMask", gpu_active_slot_mask),
    NG(0x1C, V1, false, 16, "GetGpuTime", gpu_get_gpu_time),
};
#undef NG
#undef V1
#undef V13

const nv_dev_class nv_dev_ctrl_gpu = {
    .path = "/dev/nvhost-ctrl-gpu",
    .ioctls = gpu_ioctls,
    .nioctls = sizeof gpu_ioctls / sizeof gpu_ioctls[0],
    .open = gpu_open,
    .close = gpu_close,
    .query_event = gpu_query_event,
};
