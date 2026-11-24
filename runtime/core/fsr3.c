/**
 * @file fsr3.c
 * @brief The AMD FidelityFX FSR 3 upscaler (FSR 3.1.4, MIT; provenance in runtime/third_party/fsr3) for the upscaler
 * stage: the dispatch logic of ffx_fsr3upscaler.cpp, ported to the stage's Vulkan calls (ffx.h). The passes are
 * FidelityFX's GLSL, unchanged, compiled per option set into fsr3_data.h.
 *
 * What differs from ffx_fsr3upscaler.cpp: no reactive mask generation and no debug view, the default transparency
 * and composition mask, the jittered motion vector path always compiled in with its cancellation zero when the motion
 * vectors carry no jitter, the maximum render size equal to the display size, and a camera the inputs do not describe
 * (near 0.1, far 10000, a vertical field of view of 60 degrees), which FSR 3 only reads for its disocclusion
 * tolerance, as in fsr2.c.
 */
#include "fsr3.h"

#include "changemask.h"
#include "core.h"
#include "ffx.h"
#include "fsr3_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RECORDS 32u
#define SETS (RECORDS * RL_FSR3_PASSES)
#define CB_FSR3 0u
#define CB_SPD 256u
#define CB_RCAS 512u
#define CB_BYTES 768u
#define SPD_MIP_VIEWS 6u
#define CAMERA_NEAR 0.1f
#define CAMERA_FAR 10000.0f
#define CAMERA_FOV_Y 1.0471976f
#define FRAME_SECONDS (1.0f / 60.0f)

/** @brief The FSR 3 upscaler's own images (ffx_fsr3upscaler.cpp's internal surfaces that the compiled passes read). */
enum {
    R_DILATED_MOTION_VECTORS,
    R_DILATED_DEPTH,
    R_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH,
    R_ACCUMULATION_1,
    R_ACCUMULATION_2,
    R_LUMA_1,
    R_LUMA_2,
    R_INTERMEDIATE,
    R_SHADING_CHANGE,
    R_NEW_LOCKS,
    R_INTERNAL_UPSCALED_COLOR_1,
    R_INTERNAL_UPSCALED_COLOR_2,
    R_SPD_MIPS,
    R_FARTHEST_DEPTH_MIP1,
    R_LUMA_HISTORY_1,
    R_LUMA_HISTORY_2,
    R_SPD_ATOMIC_COUNT,
    R_DILATED_REACTIVE_MASKS,
    R_DEFAULT_REACTIVITY,
    R_DEFAULT_EXPOSURE,
    R_FRAME_INFO,
    R_COUNT
};

/** @brief The size an image has: the maximum render size, the display size, one pixel, or half the render size. */
enum { SIZE_RENDER, SIZE_DISPLAY, SIZE_ONE, SIZE_HALF_RENDER };

static const struct {
    int32_t format;
    uint8_t size, chain;
} g_images[R_COUNT] = {
    [R_DILATED_MOTION_VECTORS] = { RLVK_FORMAT_R16G16_SFLOAT, SIZE_RENDER, 0 },
    [R_DILATED_DEPTH] = { RLVK_FORMAT_R32_SFLOAT, SIZE_RENDER, 0 },
    [R_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH] = { RLVK_FORMAT_R32_UINT, SIZE_RENDER, 0 },
    [R_ACCUMULATION_1] = { RLVK_FORMAT_R8_UNORM, SIZE_RENDER, 0 },
    [R_ACCUMULATION_2] = { RLVK_FORMAT_R8_UNORM, SIZE_RENDER, 0 },
    [R_LUMA_1] = { RLVK_FORMAT_R16_SFLOAT, SIZE_RENDER, 0 },
    [R_LUMA_2] = { RLVK_FORMAT_R16_SFLOAT, SIZE_RENDER, 0 },
    [R_INTERMEDIATE] = { RLVK_FORMAT_R16_SFLOAT, SIZE_RENDER, 0 },
    [R_SHADING_CHANGE] = { RLVK_FORMAT_R8_UNORM, SIZE_HALF_RENDER, 0 },
    [R_NEW_LOCKS] = { RLVK_FORMAT_R8_UNORM, SIZE_DISPLAY, 0 },
    [R_INTERNAL_UPSCALED_COLOR_1] = { RLVK_FORMAT_R16G16B16A16_SFLOAT, SIZE_DISPLAY, 0 },
    [R_INTERNAL_UPSCALED_COLOR_2] = { RLVK_FORMAT_R16G16B16A16_SFLOAT, SIZE_DISPLAY, 0 },
    [R_SPD_MIPS] = { RLVK_FORMAT_R16G16_SFLOAT, SIZE_HALF_RENDER, 1 },
    [R_FARTHEST_DEPTH_MIP1] = { RLVK_FORMAT_R16_SFLOAT, SIZE_HALF_RENDER, 0 },
    [R_LUMA_HISTORY_1] = { RLVK_FORMAT_R16G16B16A16_SFLOAT, SIZE_RENDER, 0 },
    [R_LUMA_HISTORY_2] = { RLVK_FORMAT_R16G16B16A16_SFLOAT, SIZE_RENDER, 0 },
    [R_SPD_ATOMIC_COUNT] = { RLVK_FORMAT_R32_UINT, SIZE_ONE, 0 },
    [R_DILATED_REACTIVE_MASKS] = { RLVK_FORMAT_R8G8B8A8_UNORM, SIZE_RENDER, 0 },
    [R_DEFAULT_REACTIVITY] = { RLVK_FORMAT_R8_UNORM, SIZE_ONE, 0 },
    [R_DEFAULT_EXPOSURE] = { RLVK_FORMAT_R32G32_SFLOAT, SIZE_ONE, 0 },
    [R_FRAME_INFO] = { RLVK_FORMAT_R32G32B32A32_SFLOAT, SIZE_ONE, 0 },
};

/** @brief The FidelityFX resource name of each image, as ffx_fsr3upscaler.cpp names it (for the debug dump). */
static const char *const g_image_names[R_COUNT] = {
    [R_DILATED_MOTION_VECTORS] = "dilated_motion_vectors",
    [R_DILATED_DEPTH] = "dilated_depth",
    [R_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH] = "reconstructed_previous_nearest_depth",
    [R_ACCUMULATION_1] = "accumulation_1",
    [R_ACCUMULATION_2] = "accumulation_2",
    [R_LUMA_1] = "luma_1",
    [R_LUMA_2] = "luma_2",
    [R_INTERMEDIATE] = "intermediate",
    [R_SHADING_CHANGE] = "shading_change",
    [R_NEW_LOCKS] = "new_locks",
    [R_INTERNAL_UPSCALED_COLOR_1] = "internal_upscaled_color_1",
    [R_INTERNAL_UPSCALED_COLOR_2] = "internal_upscaled_color_2",
    [R_SPD_MIPS] = "spd_mips",
    [R_FARTHEST_DEPTH_MIP1] = "farthest_depth_mip1",
    [R_LUMA_HISTORY_1] = "luma_history_1",
    [R_LUMA_HISTORY_2] = "luma_history_2",
    [R_SPD_ATOMIC_COUNT] = "spd_atomic_count",
    [R_DILATED_REACTIVE_MASKS] = "dilated_reactive_masks",
    [R_DEFAULT_REACTIVITY] = "default_reactivity",
    [R_DEFAULT_EXPOSURE] = "default_exposure",
    [R_FRAME_INFO] = "frame_info",
};

/** @brief What a shader resource name binds (the frame's choice of ping-pong images resolved in view_of). */
enum {
    N_UNKNOWN,
    N_SAMPLER,
    N_INPUT_COLOR,
    N_INPUT_MOTION,
    N_INPUT_DEPTH,
    N_INPUT_EXPOSURE,
    N_REACTIVE,
    N_COMPOSITION,
    N_OUTPUT,
    N_CB_FSR3,
    N_CB_SPD,
    N_CB_RCAS,
    N_SPD_MIP,
    N_ACCUMULATION_SRV,
    N_ACCUMULATION_UAV,
    N_UPSCALED_SRV,
    N_UPSCALED_UAV,
    N_LUMA_HISTORY_SRV,
    N_LUMA_HISTORY_UAV,
    N_CURRENT_LUMA,
    N_PREVIOUS_LUMA,
    N_FIXED
};

static const struct {
    const char *name;
    uint8_t what, image;
} g_names[] = {
    { "s_LinearClamp", N_SAMPLER, 0 },
    { "s_PointClamp", N_SAMPLER, 0 },
    { "r_input_color_jittered", N_INPUT_COLOR, 0 },
    { "r_input_motion_vectors", N_INPUT_MOTION, 0 },
    { "r_input_depth", N_INPUT_DEPTH, 0 },
    { "r_input_exposure", N_INPUT_EXPOSURE, 0 },
    { "r_reactive_mask", N_REACTIVE, 0 },
    { "r_transparency_and_composition_mask", N_COMPOSITION, 0 },
    { "rw_upscaled_output", N_OUTPUT, 0 },
    { "cbFSR3Upscaler", N_CB_FSR3, 0 },
    { "cbSPD", N_CB_SPD, 0 },
    { "cbRCAS", N_CB_RCAS, 0 },
    { "rw_spd_mip0", N_SPD_MIP, 0 },
    { "rw_spd_mip1", N_SPD_MIP, 1 },
    { "rw_spd_mip2", N_SPD_MIP, 2 },
    { "rw_spd_mip3", N_SPD_MIP, 3 },
    { "rw_spd_mip4", N_SPD_MIP, 4 },
    { "rw_spd_mip5", N_SPD_MIP, 5 },
    { "r_spd_mips", N_FIXED, R_SPD_MIPS },
    { "rw_spd_global_atomic", N_FIXED, R_SPD_ATOMIC_COUNT },
    { "rw_frame_info", N_FIXED, R_FRAME_INFO },
    { "rw_dilated_motion_vectors", N_FIXED, R_DILATED_MOTION_VECTORS },
    { "r_dilated_motion_vectors", N_FIXED, R_DILATED_MOTION_VECTORS },
    { "rw_dilated_depth", N_FIXED, R_DILATED_DEPTH },
    { "r_dilated_depth", N_FIXED, R_DILATED_DEPTH },
    { "rw_reconstructed_previous_nearest_depth", N_FIXED, R_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH },
    { "r_reconstructed_previous_nearest_depth", N_FIXED, R_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH },
    { "rw_farthest_depth", N_FIXED, R_INTERMEDIATE },
    { "r_farthest_depth", N_FIXED, R_INTERMEDIATE },
    { "rw_luma_instability", N_FIXED, R_INTERMEDIATE },
    { "r_luma_instability", N_FIXED, R_INTERMEDIATE },
    { "rw_farthest_depth_mip1", N_FIXED, R_FARTHEST_DEPTH_MIP1 },
    { "r_farthest_depth_mip1", N_FIXED, R_FARTHEST_DEPTH_MIP1 },
    { "rw_shading_change", N_FIXED, R_SHADING_CHANGE },
    { "r_shading_change", N_FIXED, R_SHADING_CHANGE },
    { "rw_dilated_reactive_masks", N_FIXED, R_DILATED_REACTIVE_MASKS },
    { "r_dilated_reactive_masks", N_FIXED, R_DILATED_REACTIVE_MASKS },
    { "rw_new_locks", N_FIXED, R_NEW_LOCKS },
    { "rw_current_luma", N_CURRENT_LUMA, 0 },
    { "r_current_luma", N_CURRENT_LUMA, 0 },
    { "r_previous_luma", N_PREVIOUS_LUMA, 0 },
    { "r_accumulation", N_ACCUMULATION_SRV, 0 },
    { "rw_accumulation", N_ACCUMULATION_UAV, 0 },
    { "r_internal_upscaled_color", N_UPSCALED_SRV, 0 },
    { "rw_internal_upscaled_color", N_UPSCALED_UAV, 0 },
    { "r_rcas_input", N_UPSCALED_UAV, 0 },
    { "r_luma_history", N_LUMA_HISTORY_SRV, 0 },
    { "rw_luma_history", N_LUMA_HISTORY_UAV, 0 },
};

/** @brief cbFSR3Upscaler (Fsr3UpscalerConstants), std140. */
typedef struct cb_fsr3 {
    int32_t render_size[2], previous_render_size[2], upscale_size[2], previous_upscale_size[2];
    int32_t max_render_size[2], max_upscale_size[2];
    float device_to_view_depth[4], jitter[2], previous_jitter[2], motion_vector_scale[2], downscale[2];
    float motion_vector_jitter_cancel[2], tan_half_fov, jitter_phase_count;
    float delta_time, delta_pre_exposure, view_space_to_meters, frame_index;
    float velocity_factor, reactiveness_scale, shading_change_scale, accumulation_added_per_frame;
    float min_disocclusion_accumulation;
} cb_fsr3;

/** @brief cbSPD (Fsr3UpscalerSpdConstants). */
typedef struct cb_spd {
    uint32_t mips, num_work_groups, work_group_offset[2], render_size[2];
} cb_spd;

struct rl_fsr3 {
    rl_ffx_vk vk;
    rlvk_handle sampler, pool, cb, cb_memory;
    uint8_t name_what[RL_FSR3_NAMES], name_image[RL_FSR3_NAMES];
    rl_ffx_pipeline modules[RL_FSR3_MODULES];
    rl_ffx_image images[R_COUNT];
    rlvk_handle mip_views[SPD_MIP_VIEWS];
    uint32_t display[2], render_max[2];
    uint32_t used, parity;
    bool sized, fresh, first, pending;
    cb_fsr3 c;
    float pre_exposure, previous_jitter[2];
    /** @brief The debug dump (RELIFT_UPSCALER_DUMP), or NULL. */
    rl_ffx_dump *dump;
    /** @brief relift's moved-surface mask, bound as the transparency and composition mask, or NULL. */
    rl_changemask *moved;
    /** @brief This frame's mask view (0 without one). */
    rlvk_handle composition, composition_image;
};

static bool failed(rl_fsr3 *f, const char *what, rlvk_result r)
{
    rl_log(RL_LOG_WARN, "upscale", "the FSR 3 upscaler did not start: %s failed (VkResult %d)", what, (int)r);
    rl_fsr3_free(f);
    return false;
}

rl_fsr3 *rl_fsr3_new(const rl_post_device *d)
{
    if (!d || !d->device || !d->get_device_proc_addr)
        return NULL;
    if (!rl_ffx_has_quads(d)) {
        rl_log(RL_LOG_WARN, "upscale",
               "the FSR 3 upscaler needs quad subgroup operations in compute shaders; this device has none");
        return NULL;
    }
    rl_fsr3 *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    if (!rl_ffx_vk_load(&f->vk, d, "the FSR 3 upscaler")) {
        rl_fsr3_free(f);
        return NULL;
    }
    for (uint32_t i = 0; i < RL_FSR3_NAMES; i++)
        for (size_t k = 0; k < sizeof g_names / sizeof g_names[0]; k++)
            if (!strcmp(rl_fsr3_names[i], g_names[k].name)) {
                f->name_what[i] = g_names[k].what;
                f->name_image[i] = g_names[k].image;
            }
    for (uint32_t i = 0; i < RL_FSR3_NAMES; i++)
        if (!f->name_what[i]) {
            rl_log(RL_LOG_WARN, "upscale",
                   "the FSR 3 upscaler did not start: its passes read %s, which relift does not bind",
                   rl_fsr3_names[i]);
            rl_fsr3_free(f);
            return NULL;
        }
    rlvk_sampler_create_info si = {
        .sType = RLVK_ST_SAMPLER_CREATE_INFO,
        .magFilter = RLVK_FILTER_LINEAR,
        .minFilter = RLVK_FILTER_LINEAR,
        .mipmapMode = RLVK_SAMPLER_MIPMAP_MODE_LINEAR,
        .addressModeU = RLVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = RLVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = RLVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxAnisotropy = 1.0f,
        .maxLod = 1000.0f,
    };
    rlvk_result r = f->vk.vkCreateSampler(f->vk.device, &si, NULL, &f->sampler);
    if (r != RLVK_SUCCESS)
        return failed(f, "vkCreateSampler", r), NULL;
    rlvk_descriptor_pool_size ps[4] = {
        { RLVK_DESCRIPTOR_TYPE_SAMPLER, SETS * 2 },
        { RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SETS * 16 },
        { RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS * 16 },
        { RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SETS * 2 },
    };
    rlvk_descriptor_pool_create_info dp = {
        .sType = RLVK_ST_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = SETS, .poolSizeCount = 4, .pPoolSizes = ps
    };
    if ((r = f->vk.vkCreateDescriptorPool(f->vk.device, &dp, NULL, &f->pool)) != RLVK_SUCCESS)
        return failed(f, "vkCreateDescriptorPool", r), NULL;
    rlvk_buffer_create_info bi = { .sType = RLVK_ST_BUFFER_CREATE_INFO,
                                   .size = CB_BYTES,
                                   .usage = RLVK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | RLVK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if ((r = f->vk.vkCreateBuffer(f->vk.device, &bi, NULL, &f->cb)) != RLVK_SUCCESS)
        return failed(f, "vkCreateBuffer", r), NULL;
    rlvk_memory_requirements req;
    f->vk.vkGetBufferMemoryRequirements(f->vk.device, f->cb, &req);
    if (!rl_ffx_bind_memory(&f->vk, &req, &f->cb_memory) ||
        f->vk.vkBindBufferMemory(f->vk.device, f->cb, f->cb_memory, 0) != RLVK_SUCCESS)
        return failed(f, "the constant buffer's memory", 0), NULL;
    f->c.velocity_factor = 1.0f;
    f->c.reactiveness_scale = 1.0f;
    f->c.shading_change_scale = 1.0f;
    f->c.accumulation_added_per_frame = 1.0f / 3.0f;
    f->c.min_disocclusion_accumulation = -1.0f / 3.0f;
    f->moved = rl_changemask_new(&f->vk);
    f->dump = rl_ffx_dump_new("fsr3");
    f->first = true;
    return f;
}

static bool make_module(rl_fsr3 *f, uint32_t m)
{
    if (f->modules[m].pipe)
        return true;
    const rl_fsr3_module *src = &rl_fsr3_modules[m];
    rl_ffx_bind binds[48];
    uint32_t n = 0;
    for (uint32_t i = 0; i < src->nbinds && n < 48; i++)
        binds[n++] = (rl_ffx_bind){ src->binds[i].set, src->binds[i].binding, src->binds[i].type };
    return rl_ffx_pipeline_new(&f->vk, src->code, src->size, binds, n, 0, NULL, 0, f->sampler, &f->modules[m]);
}

static void drop_images(rl_fsr3 *f)
{
    for (uint32_t i = 0; i < SPD_MIP_VIEWS; i++)
        if (f->mip_views[i])
            f->vk.vkDestroyImageView(f->vk.device, f->mip_views[i], NULL), f->mip_views[i] = 0;
    for (int i = 0; i < R_COUNT; i++)
        rl_ffx_image_free(&f->vk, &f->images[i]);
    f->sized = false;
}

/** @brief Makes the images for a display of w by h (the maximum render size the same). */
static bool make_images(rl_fsr3 *f, uint32_t w, uint32_t h)
{
    drop_images(f);
    f->display[0] = f->render_max[0] = w;
    f->display[1] = f->render_max[1] = h;
    uint32_t hw = w / 2 ? w / 2 : 1, hh = h / 2 ? h / 2 : 1, big = hw > hh ? hw : hh, chain = 1;
    while (big >> chain)
        chain++;
    if (chain < SPD_MIP_VIEWS)
        return false;
    for (int i = 0; i < R_COUNT; i++) {
        uint32_t iw = 1, ih = 1;
        if (g_images[i].size == SIZE_RENDER || g_images[i].size == SIZE_DISPLAY)
            iw = w, ih = h;
        else if (g_images[i].size == SIZE_HALF_RENDER)
            iw = hw, ih = hh;
        if (!rl_ffx_image_new(&f->vk, g_images[i].format, iw, ih, g_images[i].chain ? chain : 1, &f->images[i]))
            return false;
    }
    for (uint32_t i = 0; i < SPD_MIP_VIEWS; i++)
        if (!(f->mip_views[i] = rl_ffx_view(&f->vk, f->images[R_SPD_MIPS].image, RLVK_FORMAT_R16G16_SFLOAT, i, 1)))
            return false;
    f->sized = true;
    f->fresh = true;
    f->first = true;
    return true;
}

/** @brief Records the debug dump of f's images (rl_ffx_dump_record) for a frame of in. */
static void dump(rl_fsr3 *f, uint64_t cmd, const rl_upscale_inputs *in)
{
    if (!f->dump)
        return;
    uint32_t w = f->display[0], h = f->display[1];
    uint32_t hw = w / 2 ? w / 2 : 1, hh = h / 2 ? h / 2 : 1;
    rl_ffx_dump_image im[R_COUNT + 1];
    for (uint32_t i = 0; i < R_COUNT; i++) {
        uint32_t iw = 1, ih = 1;
        if (g_images[i].size == SIZE_RENDER || g_images[i].size == SIZE_DISPLAY)
            iw = w, ih = h;
        else if (g_images[i].size == SIZE_HALF_RENDER)
            iw = hw, ih = hh;
        im[i] = (rl_ffx_dump_image){ g_image_names[i], f->images[i].image, g_images[i].format, iw, ih,
                                     g_images[i].size == SIZE_RENDER };
    }
    uint32_t render[2] = { in->in.width, in->in.height };
    im[R_COUNT] = (rl_ffx_dump_image){ "transparency_and_composition_mask", f->composition ? f->composition_image : 0,
                                       RLVK_FORMAT_R16_SFLOAT, w, h, true };
    rl_ffx_dump_record(f->dump, &f->vk, cmd, im, R_COUNT + 1, render, &f->c, sizeof f->c);
}

bool rl_fsr3_takes(const rl_upscale_inputs *in)
{
    return in->depth.handle && in->motion.handle && !in->in.x && !in->in.y && !in->out_rect.x && !in->out_rect.y &&
           in->in.width <= in->out_rect.width && in->in.height <= in->out_rect.height;
}

/** @brief The view the frame binds for name index n, or 0. */
static rlvk_handle view_of(const rl_fsr3 *f, uint32_t n, const rl_upscale_inputs *in)
{
    bool odd = f->parity & 1;
    switch (f->name_what[n]) {
    case N_INPUT_COLOR:
        return in->color.handle;
    case N_INPUT_MOTION:
        return in->motion.handle;
    case N_INPUT_DEPTH:
        return in->depth.handle;
    case N_INPUT_EXPOSURE:
        if (in->auto_exposure)
            return f->images[R_FRAME_INFO].view;
        return in->exposure.handle ? in->exposure.handle : f->images[R_DEFAULT_EXPOSURE].view;
    case N_REACTIVE:
        return in->reactive.handle ? in->reactive.handle : f->images[R_DEFAULT_REACTIVITY].view;
    case N_COMPOSITION:
        return f->composition ? f->composition : f->images[R_DEFAULT_REACTIVITY].view;
    case N_OUTPUT:
        return in->out.handle;
    case N_SPD_MIP:
        return f->mip_views[f->name_image[n]];
    case N_ACCUMULATION_SRV:
        return f->images[odd ? R_ACCUMULATION_2 : R_ACCUMULATION_1].view;
    case N_ACCUMULATION_UAV:
        return f->images[odd ? R_ACCUMULATION_1 : R_ACCUMULATION_2].view;
    case N_UPSCALED_SRV:
        return f->images[odd ? R_INTERNAL_UPSCALED_COLOR_2 : R_INTERNAL_UPSCALED_COLOR_1].view;
    case N_UPSCALED_UAV:
        return f->images[odd ? R_INTERNAL_UPSCALED_COLOR_1 : R_INTERNAL_UPSCALED_COLOR_2].view;
    case N_LUMA_HISTORY_SRV:
        return f->images[odd ? R_LUMA_HISTORY_2 : R_LUMA_HISTORY_1].view;
    case N_LUMA_HISTORY_UAV:
        return f->images[odd ? R_LUMA_HISTORY_1 : R_LUMA_HISTORY_2].view;
    case N_CURRENT_LUMA:
        return f->images[odd ? R_LUMA_2 : R_LUMA_1].view;
    case N_PREVIOUS_LUMA:
        return f->images[odd ? R_LUMA_1 : R_LUMA_2].view;
    case N_FIXED:
        return f->images[f->name_image[n]].view;
    default:
        return 0;
    }
}

/** @brief Allocates and fills pass module m's set for this frame; 0 when none is left. */
static rlvk_handle pass_set(rl_fsr3 *f, uint32_t m, const rl_upscale_inputs *in)
{
    rlvk_handle set = 0;
    rlvk_descriptor_set_allocate_info da = { .sType = RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = f->pool,
                                             .descriptorSetCount = 1,
                                             .pSetLayouts = &f->modules[m].set_layout };
    if (f->vk.vkAllocateDescriptorSets(f->vk.device, &da, &set) != RLVK_SUCCESS)
        return 0;
    const rl_fsr3_module *src = &rl_fsr3_modules[m];
    rlvk_write_descriptor_set wr[48];
    rlvk_descriptor_image_info ii[48];
    rlvk_descriptor_buffer_info bi[48];
    uint32_t n = 0;
    for (uint32_t i = 0; i < src->nbinds && n < 48; i++) {
        const rl_fsr3_bind *b = &src->binds[i];
        if (b->type == RLVK_DESCRIPTOR_TYPE_SAMPLER)
            continue;
        wr[n] = (rlvk_write_descriptor_set){ .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = b->binding,
                                             .descriptorCount = 1,
                                             .descriptorType = b->type };
        uint8_t what = f->name_what[b->name];
        if (b->type == RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            uint32_t off = what == N_CB_SPD ? CB_SPD : what == N_CB_RCAS ? CB_RCAS : CB_FSR3;
            uint32_t size = what == N_CB_SPD ? sizeof(cb_spd) : what == N_CB_RCAS ? 16u : sizeof(cb_fsr3);
            bi[n] = (rlvk_descriptor_buffer_info){ .buffer = f->cb, .offset = off, .range = size };
            wr[n].pBufferInfo = &bi[n];
        } else {
            ii[n] = (rlvk_descriptor_image_info){ .imageView = view_of(f, b->name, in),
                                                   .imageLayout = RLVK_IMAGE_LAYOUT_GENERAL };
            if (!ii[n].imageView)
                return 0;
            wr[n].pImageInfo = &ii[n];
        }
        n++;
    }
    f->vk.vkUpdateDescriptorSets(f->vk.device, n, wr, 0, NULL);
    return set;
}

/**
 * @brief The camera FSR 3 is told about for in (ffx_fsr3upscaler.cpp's setupDeviceDepthToViewSpaceDepthParams and tanHalfFOV):
 * device_to_view gets deviceToViewDepth, *tan_half_fov the tangent of half the view angle across the screen.
 */
static void camera(const rl_upscale_inputs *in, float device_to_view[4], float *tan_half_fov)
{
    float aspect = (float)in->in.width / (float)in->in.height;
    float fov_x = atanf(tanf(CAMERA_FOV_Y / 2.0f) * aspect) * 2.0f;
    *tan_half_fov = tanf(fov_x * 0.5f);
    float lo = CAMERA_NEAR, hi = CAMERA_FAR;
    if (in->depth_inverted) {
        float t = lo;
        lo = hi;
        hi = t;
    }
    float q = hi / (lo - hi);
    device_to_view[0] = -q;
    device_to_view[1] = q * lo;
    float cot = cosf(0.5f * CAMERA_FOV_Y) / sinf(0.5f * CAMERA_FOV_Y);
    device_to_view[2] = 1.0f / (cot / aspect);
    device_to_view[3] = 1.0f / cot;
}

/** @brief Fills the constants of this frame (fsr3upscalerDispatch's) into f->c and spd; reset starts the history. */
static void constants(rl_fsr3 *f, const rl_upscale_inputs *in, bool reset, cb_spd *spd, uint32_t rcas[4])
{
    cb_fsr3 *c = &f->c;
    float rw = (float)in->in.width, rh = (float)in->in.height;
    c->previous_jitter[0] = c->jitter[0];
    c->previous_jitter[1] = c->jitter[1];
    c->jitter[0] = in->jitter[0];
    c->jitter[1] = in->jitter[1];
    c->previous_render_size[0] = c->render_size[0];
    c->previous_render_size[1] = c->render_size[1];
    c->render_size[0] = (int32_t)in->in.width;
    c->render_size[1] = (int32_t)in->in.height;
    c->max_render_size[0] = (int32_t)f->render_max[0];
    c->max_render_size[1] = (int32_t)f->render_max[1];
    camera(in, c->device_to_view_depth, &c->tan_half_fov);
    c->view_space_to_meters = 1.0f;
    c->previous_upscale_size[0] = c->upscale_size[0];
    c->previous_upscale_size[1] = c->upscale_size[1];
    c->upscale_size[0] = c->max_upscale_size[0] = (int32_t)f->display[0];
    c->upscale_size[1] = c->max_upscale_size[1] = (int32_t)f->display[1];
    c->downscale[0] = rw / (float)f->display[0];
    c->downscale[1] = rh / (float)f->display[1];
    float previous = f->pre_exposure;
    f->pre_exposure = in->pre_exposure != 0.0f ? in->pre_exposure : 1.0f;
    c->delta_pre_exposure = previous > 0.0f ? f->pre_exposure / previous : 1.0f;
    float tw = in->motion_display_size ? (float)f->display[0] : rw;
    float th = in->motion_display_size ? (float)f->display[1] : rh;
    c->motion_vector_scale[0] = in->motion_scale[0] / tw;
    c->motion_vector_scale[1] = in->motion_scale[1] / th;
    c->motion_vector_jitter_cancel[0] = c->motion_vector_jitter_cancel[1] = 0.0f;
    if (in->motion_jittered) {
        c->motion_vector_jitter_cancel[0] = (f->previous_jitter[0] - c->jitter[0]) / tw;
        c->motion_vector_jitter_cancel[1] = (f->previous_jitter[1] - c->jitter[1]) / th;
    }
    f->previous_jitter[0] = c->jitter[0];
    f->previous_jitter[1] = c->jitter[1];
    float ratio = (float)f->display[0] / rw;
    int32_t phases = (int32_t)(8.0f * ratio * ratio);
    if (reset || c->jitter_phase_count == 0.0f)
        c->jitter_phase_count = (float)phases;
    else if (phases > (int32_t)c->jitter_phase_count)
        c->jitter_phase_count += 1.0f;
    else if (phases < (int32_t)c->jitter_phase_count)
        c->jitter_phase_count -= 1.0f;
    c->delta_time = FRAME_SECONDS;
    c->frame_index = reset ? 0.0f : c->frame_index + 1.0f;
    uint32_t ex = (in->in.width - 1) / 64, ey = (in->in.height - 1) / 64;
    uint32_t big = in->in.width > in->in.height ? in->in.width : in->in.height, mips = 0;
    while ((big >> (mips + 1)) && mips < 12)
        mips++;
    *spd = (cb_spd){
        .mips = mips, .num_work_groups = (ex + 1) * (ey + 1), .render_size = { in->in.width, in->in.height }
    };
    rl_ffx_rcas(in->sharpness, rcas);
}

bool rl_fsr3_record(rl_fsr3 *f, uint64_t cmd, const rl_upscale_inputs *in)
{
    if (!f || !cmd || !rl_fsr3_takes(in) || !in->color.handle || !in->out.handle)
        return false;
    uint32_t dw = in->out_rect.width, dh = in->out_rect.height;
    if (!f->sized || f->display[0] != dw || f->display[1] != dh) {
        if (f->pending)
            return false;
        if (!make_images(f, dw, dh)) {
            drop_images(f);
            return false;
        }
    }
    if (f->used + RL_FSR3_PASSES > SETS)
        return false;
    uint32_t opts = (in->hdr ? 1u : 0u) | (in->motion_display_size ? 0u : 2u) | (in->depth_inverted ? 4u : 0u) |
                    (in->sharpness > 0.0f ? 8u : 0u);
    uint32_t mods[RL_FSR3_PASSES];
    for (uint32_t p = 0; p < RL_FSR3_PASSES; p++) {
        mods[p] = rl_fsr3_pass_module[p][opts];
        if (!make_module(f, mods[p])) {
            rl_log(RL_LOG_WARN, "upscale", "the FSR 3 upscaler: pass %u did not compile on this device", p);
            return false;
        }
    }
    bool sharpen = opts & 8u;
    uint32_t npasses = sharpen ? RL_FSR3_PASSES : RL_FSR3_PASSES - 1;
    bool reset = in->reset || f->first;
    f->composition = 0;
    if (f->moved) {
        float d2v[4], tan_half_fov;
        camera(in, d2v, &tan_half_fov);
        rl_changemask_camera cam = { { d2v[0], d2v[1] }, { d2v[2], d2v[3] } };
        rlvk_handle image = 0;
        if (!rl_changemask_record(f->moved, cmd, in, &cam, reset, &f->composition, &image))
            return false;
        f->composition_image = image;
    }
    rlvk_handle sets[RL_FSR3_PASSES];
    for (uint32_t p = 0; p < npasses; p++)
        if (!(sets[p] = pass_set(f, mods[p], in)))
            return false;
    f->used += RL_FSR3_PASSES;
    f->pending = true;
    const uint32_t cs = RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, xfer = RLVK_PIPELINE_STAGE_TRANSFER_BIT;
    const uint32_t rw_access = RLVK_ACCESS_SHADER_READ_BIT | RLVK_ACCESS_SHADER_WRITE_BIT;
    if (f->fresh) {
        rlvk_image_memory_barrier ib[R_COUNT];
        for (int i = 0; i < R_COUNT; i++)
            ib[i] = (rlvk_image_memory_barrier){ .sType = RLVK_ST_IMAGE_MEMORY_BARRIER,
                                                 .dstAccessMask = RLVK_ACCESS_TRANSFER_WRITE_BIT | rw_access,
                                                 .oldLayout = RLVK_IMAGE_LAYOUT_UNDEFINED,
                                                 .newLayout = RLVK_IMAGE_LAYOUT_GENERAL,
                                                 .srcQueueFamilyIndex = (uint32_t)RLVK_QUEUE_FAMILY_IGNORED,
                                                 .dstQueueFamilyIndex = (uint32_t)RLVK_QUEUE_FAMILY_IGNORED,
                                                 .image = f->images[i].image,
                                                 .subresourceRange = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, ~0u, 0, 1 } };
        f->vk.vkCmdPipelineBarrier(cmd, cs | xfer, cs | xfer, 0, 0, NULL, 0, NULL, R_COUNT, ib);
        for (int i = 0; i < R_COUNT; i++)
            rl_ffx_clear(&f->vk, cmd, f->images[i].image, 0.0f, 0.0f);
    } else {
        rl_ffx_barrier(&f->vk, cmd, cs, rw_access, cs | xfer, rw_access | RLVK_ACCESS_TRANSFER_WRITE_BIT);
    }
    bool odd = f->parity & 1;
    if (reset) {
        rl_ffx_clear(&f->vk, cmd, f->images[odd ? R_ACCUMULATION_2 : R_ACCUMULATION_1].image, 0.0f, 0.0f);
        rl_ffx_clear(&f->vk, cmd, f->images[R_FRAME_INFO].image, -1.0f, 1.0f);
    }
    float far = in->depth_inverted ? 0.0f : 1.0f;
    rl_ffx_clear(&f->vk, cmd, f->images[R_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH].image, far, far);
    rl_ffx_clear(&f->vk, cmd, f->images[R_SPD_ATOMIC_COUNT].image, 0.0f, 0.0f);
    rl_ffx_clear(&f->vk, cmd, f->images[R_SPD_MIPS].image, 0.0f, 0.0f);
    f->fresh = false;
    f->first = false;
    cb_spd spd;
    uint32_t rcas[4];
    constants(f, in, reset, &spd, rcas);
    f->vk.vkCmdUpdateBuffer(cmd, f->cb, CB_FSR3, sizeof f->c, &f->c);
    f->vk.vkCmdUpdateBuffer(cmd, f->cb, CB_SPD, sizeof spd, &spd);
    f->vk.vkCmdUpdateBuffer(cmd, f->cb, CB_RCAS, sizeof rcas, rcas);
    rl_ffx_barrier(&f->vk, cmd, xfer, RLVK_ACCESS_TRANSFER_WRITE_BIT, cs, rw_access | RLVK_ACCESS_UNIFORM_READ_BIT);
    uint32_t src_x = (in->in.width + 7) / 8, src_y = (in->in.height + 7) / 8;
    uint32_t half_x = (in->in.width / 2 + 7) / 8, half_y = (in->in.height / 2 + 7) / 8;
    uint32_t spd_x = (in->in.width - 1) / 64 + 1, spd_y = (in->in.height - 1) / 64 + 1;
    uint32_t dst_x = (dw + 7) / 8, dst_y = (dh + 7) / 8;
    uint32_t groups[RL_FSR3_PASSES][2] = {
        { src_x, src_y }, { spd_x, spd_y }, { spd_x, spd_y }, { half_x, half_y },
        { src_x, src_y }, { src_x, src_y }, { dst_x, dst_y }, { (dw + 15) / 16, (dh + 15) / 16 },
    };
    for (uint32_t p = 0; p < npasses; p++) {
        const rl_ffx_pipeline *md = &f->modules[mods[p]];
        f->vk.vkCmdBindPipeline(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, md->pipe);
        f->vk.vkCmdBindDescriptorSets(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, md->layout, 0, 1, &sets[p], 0, NULL);
        f->vk.vkCmdDispatch(cmd, groups[p][0], groups[p][1], 1);
        if (p + 1 < npasses)
            rl_ffx_barrier(&f->vk, cmd, cs, rw_access, cs, rw_access);
    }
    dump(f, cmd, in);
    f->parity++;
    return true;
}

void rl_fsr3_retire(rl_fsr3 *f)
{
    if (!f || !f->pool)
        return;
    f->vk.vkResetDescriptorPool(f->vk.device, f->pool, 0);
    rl_changemask_retire(f->moved);
    rl_ffx_dump_retire(f->dump, &f->vk);
    f->used = 0;
    f->pending = false;
}

void rl_fsr3_free(rl_fsr3 *f)
{
    if (!f)
        return;
    if (f->vk.vkDestroyImageView) {
        rl_ffx_dump_free(f->dump, &f->vk);
    rl_changemask_free(f->moved);
        drop_images(f);
        for (uint32_t m = 0; m < RL_FSR3_MODULES; m++)
            rl_ffx_pipeline_free(&f->vk, &f->modules[m]);
        if (f->cb)
            f->vk.vkDestroyBuffer(f->vk.device, f->cb, NULL);
        if (f->cb_memory)
            f->vk.vkFreeMemory(f->vk.device, f->cb_memory, NULL);
        if (f->pool)
            f->vk.vkDestroyDescriptorPool(f->vk.device, f->pool, NULL);
        if (f->sampler)
            f->vk.vkDestroySampler(f->vk.device, f->sampler, NULL);
    }
    free(f);
}
