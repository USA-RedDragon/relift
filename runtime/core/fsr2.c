/**
 * @file fsr2.c
 * @brief AMD FidelityFX Super Resolution 2 (MIT; provenance in runtime/third_party/fsr2) for the upscaler stage: the
 * dispatch logic of ffx_fsr2.cpp, ported to the stage's Vulkan calls. The passes are FidelityFX's GLSL, unchanged,
 * compiled per option set into fsr2_data.h.
 *
 * What differs from ffx_fsr2.cpp: no reactive mask generation, no transparency and composition mask (the default one
 * is bound), the jittered motion vector path always compiled in with its cancellation zero when the motion vectors
 * carry no jitter, the maximum render size equal to the display size, and a camera the inputs do not describe (near
 * 0.1, far 10000, a vertical field of view of 60 degrees), which FSR 2 only reads for its disocclusion tolerance.
 */
#include "fsr2.h"

#include "core.h"
#include "fsr2_data.h"
#include "hostlib.h"
#include "vkmin.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RECORDS 32u
#define SETS (RECORDS * RL_FSR2_PASSES)
#define CB_FSR2 0u
#define CB_SPD 256u
#define CB_RCAS 512u
#define CB_BYTES 768u
#define LUMA_MIP 4u
#define CAMERA_NEAR 0.1f
#define CAMERA_FAR 10000.0f
#define CAMERA_FOV_Y 1.0471976f
#define FRAME_SECONDS (1.0f / 60.0f)

/** @brief The device functions FSR 2 calls, as X(name, return type, parameters). */
#define FSR2_FNS(X)                                                                                                    \
    X(vkCreateShaderModule, rlvk_result,                                                                              \
      (rlvk_handle, const rlvk_shader_module_create_info *, const void *, rlvk_handle *))                             \
    X(vkDestroyShaderModule, void, (rlvk_handle, rlvk_handle, const void *))                                          \
    X(vkCreateDescriptorSetLayout, rlvk_result,                                                                       \
      (rlvk_handle, const rlvk_descriptor_set_layout_create_info *, const void *, rlvk_handle *))                     \
    X(vkDestroyDescriptorSetLayout, void, (rlvk_handle, rlvk_handle, const void *))                                   \
    X(vkCreatePipelineLayout, rlvk_result,                                                                            \
      (rlvk_handle, const rlvk_pipeline_layout_create_info *, const void *, rlvk_handle *))                           \
    X(vkDestroyPipelineLayout, void, (rlvk_handle, rlvk_handle, const void *))                                        \
    X(vkCreateComputePipelines, rlvk_result,                                                                          \
      (rlvk_handle, rlvk_handle, uint32_t, const rlvk_compute_pipeline_create_info *, const void *, rlvk_handle *))   \
    X(vkDestroyPipeline, void, (rlvk_handle, rlvk_handle, const void *))                                              \
    X(vkCreateDescriptorPool, rlvk_result,                                                                            \
      (rlvk_handle, const rlvk_descriptor_pool_create_info *, const void *, rlvk_handle *))                           \
    X(vkDestroyDescriptorPool, void, (rlvk_handle, rlvk_handle, const void *))                                        \
    X(vkResetDescriptorPool, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_flags))                                     \
    X(vkAllocateDescriptorSets, rlvk_result, (rlvk_handle, const rlvk_descriptor_set_allocate_info *, rlvk_handle *))  \
    X(vkUpdateDescriptorSets, void,                                                                                   \
      (rlvk_handle, uint32_t, const rlvk_write_descriptor_set *, uint32_t, const void *))                             \
    X(vkCreateSampler, rlvk_result, (rlvk_handle, const rlvk_sampler_create_info *, const void *, rlvk_handle *))     \
    X(vkDestroySampler, void, (rlvk_handle, rlvk_handle, const void *))                                               \
    X(vkCreateBuffer, rlvk_result, (rlvk_handle, const rlvk_buffer_create_info *, const void *, rlvk_handle *))       \
    X(vkDestroyBuffer, void, (rlvk_handle, rlvk_handle, const void *))                                                \
    X(vkGetBufferMemoryRequirements, void, (rlvk_handle, rlvk_handle, rlvk_memory_requirements *))                    \
    X(vkBindBufferMemory, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_handle, rlvk_size))                            \
    X(vkCreateImage, rlvk_result, (rlvk_handle, const rlvk_image_create_info *, const void *, rlvk_handle *))         \
    X(vkDestroyImage, void, (rlvk_handle, rlvk_handle, const void *))                                                 \
    X(vkGetImageMemoryRequirements, void, (rlvk_handle, rlvk_handle, rlvk_memory_requirements *))                     \
    X(vkBindImageMemory, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_handle, rlvk_size))                             \
    X(vkCreateImageView, rlvk_result, (rlvk_handle, const rlvk_image_view_create_info *, const void *, rlvk_handle *)) \
    X(vkDestroyImageView, void, (rlvk_handle, rlvk_handle, const void *))                                             \
    X(vkAllocateMemory, rlvk_result, (rlvk_handle, const rlvk_memory_allocate_info *, const void *, rlvk_handle *))   \
    X(vkFreeMemory, void, (rlvk_handle, rlvk_handle, const void *))                                                   \
    X(vkCmdBindPipeline, void, (rlvk_handle, int32_t, rlvk_handle))                                                   \
    X(vkCmdBindDescriptorSets, void,                                                                                  \
      (rlvk_handle, int32_t, rlvk_handle, uint32_t, uint32_t, const rlvk_handle *, uint32_t, const uint32_t *))       \
    X(vkCmdDispatch, void, (rlvk_handle, uint32_t, uint32_t, uint32_t))                                               \
    X(vkCmdPipelineBarrier, void,                                                                                     \
      (rlvk_handle, rlvk_flags, rlvk_flags, rlvk_flags, uint32_t, const rlvk_memory_barrier *, uint32_t, const void *, \
       uint32_t, const rlvk_image_memory_barrier *))                                                                  \
    X(vkCmdUpdateBuffer, void, (rlvk_handle, rlvk_handle, rlvk_size, rlvk_size, const void *))                        \
    X(vkCmdClearColorImage, void,                                                                                     \
      (rlvk_handle, rlvk_handle, int32_t, const void *, uint32_t, const rlvk_image_subresource_range *))

/** @brief FSR 2's own images (ffx_fsr2.cpp's internal surfaces that the compiled passes read). */
enum {
    R_PREPARED_INPUT_COLOR,
    R_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH,
    R_DILATED_MOTION_VECTORS_1,
    R_DILATED_MOTION_VECTORS_2,
    R_DILATED_DEPTH,
    R_LOCK_STATUS_1,
    R_LOCK_STATUS_2,
    R_LOCK_INPUT_LUMA,
    R_NEW_LOCKS,
    R_INTERNAL_UPSCALED_COLOR_1,
    R_INTERNAL_UPSCALED_COLOR_2,
    R_SCENE_LUMINANCE,
    R_LUMA_HISTORY_1,
    R_LUMA_HISTORY_2,
    R_SPD_ATOMIC_COUNT,
    R_DILATED_REACTIVE_MASKS,
    R_DEFAULT_REACTIVITY,
    R_DEFAULT_EXPOSURE,
    R_AUTO_EXPOSURE,
    R_COUNT
};

/** @brief The size an image has: the maximum render size, the display size, one pixel, or half the render size. */
enum { SIZE_RENDER, SIZE_DISPLAY, SIZE_ONE, SIZE_HALF_RENDER };

static const struct {
    int32_t format;
    uint8_t size;
} g_images[R_COUNT] = {
    [R_PREPARED_INPUT_COLOR] = { RLVK_FORMAT_R16G16B16A16_SFLOAT, SIZE_RENDER },
    [R_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH] = { RLVK_FORMAT_R32_UINT, SIZE_RENDER },
    [R_DILATED_MOTION_VECTORS_1] = { RLVK_FORMAT_R16G16_SFLOAT, SIZE_RENDER },
    [R_DILATED_MOTION_VECTORS_2] = { RLVK_FORMAT_R16G16_SFLOAT, SIZE_RENDER },
    [R_DILATED_DEPTH] = { RLVK_FORMAT_R32_SFLOAT, SIZE_RENDER },
    [R_LOCK_STATUS_1] = { RLVK_FORMAT_R16G16_SFLOAT, SIZE_DISPLAY },
    [R_LOCK_STATUS_2] = { RLVK_FORMAT_R16G16_SFLOAT, SIZE_DISPLAY },
    [R_LOCK_INPUT_LUMA] = { RLVK_FORMAT_R16_SFLOAT, SIZE_RENDER },
    [R_NEW_LOCKS] = { RLVK_FORMAT_R8_UNORM, SIZE_DISPLAY },
    [R_INTERNAL_UPSCALED_COLOR_1] = { RLVK_FORMAT_R16G16B16A16_SFLOAT, SIZE_DISPLAY },
    [R_INTERNAL_UPSCALED_COLOR_2] = { RLVK_FORMAT_R16G16B16A16_SFLOAT, SIZE_DISPLAY },
    [R_SCENE_LUMINANCE] = { RLVK_FORMAT_R16_SFLOAT, SIZE_HALF_RENDER },
    [R_LUMA_HISTORY_1] = { RLVK_FORMAT_R8G8B8A8_UNORM, SIZE_DISPLAY },
    [R_LUMA_HISTORY_2] = { RLVK_FORMAT_R8G8B8A8_UNORM, SIZE_DISPLAY },
    [R_SPD_ATOMIC_COUNT] = { RLVK_FORMAT_R32_UINT, SIZE_ONE },
    [R_DILATED_REACTIVE_MASKS] = { RLVK_FORMAT_R8G8_UNORM, SIZE_RENDER },
    [R_DEFAULT_REACTIVITY] = { RLVK_FORMAT_R8_UNORM, SIZE_ONE },
    [R_DEFAULT_EXPOSURE] = { RLVK_FORMAT_R32G32_SFLOAT, SIZE_ONE },
    [R_AUTO_EXPOSURE] = { RLVK_FORMAT_R32G32_SFLOAT, SIZE_ONE },
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
    N_CB_FSR2,
    N_CB_SPD,
    N_CB_RCAS,
    N_MIP_SHADING_CHANGE,
    N_MIP_5,
    N_DILATED_MOTION,
    N_PREVIOUS_DILATED_MOTION,
    N_LOCK_SRV,
    N_LOCK_UAV,
    N_UPSCALED_SRV,
    N_UPSCALED_UAV,
    N_LUMA_HISTORY_SRV,
    N_LUMA_HISTORY_UAV,
    N_FIXED
};

static const struct {
    const char *name;
    uint8_t what, image;
} g_names[] = {
    { "s_LinearClamp", N_SAMPLER, 0 },
    { "r_input_color_jittered", N_INPUT_COLOR, 0 },
    { "r_input_motion_vectors", N_INPUT_MOTION, 0 },
    { "r_input_depth", N_INPUT_DEPTH, 0 },
    { "r_input_exposure", N_INPUT_EXPOSURE, 0 },
    { "r_reactive_mask", N_REACTIVE, 0 },
    { "r_transparency_and_composition_mask", N_COMPOSITION, 0 },
    { "rw_upscaled_output", N_OUTPUT, 0 },
    { "cbFSR2", N_CB_FSR2, 0 },
    { "cbSPD", N_CB_SPD, 0 },
    { "cbRCAS", N_CB_RCAS, 0 },
    { "rw_img_mip_shading_change", N_MIP_SHADING_CHANGE, 0 },
    { "rw_img_mip_5", N_MIP_5, 0 },
    { "rw_spd_global_atomic", N_FIXED, R_SPD_ATOMIC_COUNT },
    { "rw_auto_exposure", N_FIXED, R_AUTO_EXPOSURE },
    { "rw_reconstructed_previous_nearest_depth", N_FIXED, R_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH },
    { "r_reconstructed_previous_nearest_depth", N_FIXED, R_RECONSTRUCTED_PREVIOUS_NEAREST_DEPTH },
    { "rw_dilated_motion_vectors", N_DILATED_MOTION, 0 },
    { "r_dilated_motion_vectors", N_DILATED_MOTION, 0 },
    { "r_previous_dilated_motion_vectors", N_PREVIOUS_DILATED_MOTION, 0 },
    { "rw_dilatedDepth", N_FIXED, R_DILATED_DEPTH },
    { "r_dilatedDepth", N_FIXED, R_DILATED_DEPTH },
    { "rw_lock_input_luma", N_FIXED, R_LOCK_INPUT_LUMA },
    { "r_lock_input_luma", N_FIXED, R_LOCK_INPUT_LUMA },
    { "rw_dilated_reactive_masks", N_FIXED, R_DILATED_REACTIVE_MASKS },
    { "r_dilated_reactive_masks", N_FIXED, R_DILATED_REACTIVE_MASKS },
    { "rw_prepared_input_color", N_FIXED, R_PREPARED_INPUT_COLOR },
    { "r_prepared_input_color", N_FIXED, R_PREPARED_INPUT_COLOR },
    { "rw_new_locks", N_FIXED, R_NEW_LOCKS },
    { "r_imgMips", N_FIXED, R_SCENE_LUMINANCE },
    { "r_internal_upscaled_color", N_UPSCALED_SRV, 0 },
    { "rw_internal_upscaled_color", N_UPSCALED_UAV, 0 },
    { "r_rcas_input", N_UPSCALED_UAV, 0 },
    { "r_lock_status", N_LOCK_SRV, 0 },
    { "rw_lock_status", N_LOCK_UAV, 0 },
    { "r_luma_history", N_LUMA_HISTORY_SRV, 0 },
    { "rw_luma_history", N_LUMA_HISTORY_UAV, 0 },
};

/** @brief cbFSR2 (Fsr2Constants), std140. */
typedef struct cb_fsr2 {
    int32_t render_size[2], max_render_size[2], display_size[2], input_color_dimensions[2], luma_mip_dimensions[2];
    int32_t luma_mip_level, frame_index;
    float device_to_view_depth[4], jitter[2], motion_vector_scale[2], downscale[2], motion_vector_jitter_cancel[2];
    float pre_exposure, previous_pre_exposure, tan_half_fov, jitter_phase_count, delta_time, dynamic_res_change,
        view_space_to_meters;
} cb_fsr2;

/** @brief cbSPD (Fsr2SpdConstants). */
typedef struct cb_spd {
    uint32_t mips, num_work_groups, work_group_offset[2], render_size[2];
} cb_spd;

typedef struct image {
    rlvk_handle image, memory, view;
} image;

/** @brief One pass module made into a pipeline. */
typedef struct module {
    rlvk_handle set_layout, layout, pipe;
} module;

struct rl_fsr2 {
#define FSR2_FN_FIELD(name, ret, args) ret(RLVK_CALL *name) args;
    FSR2_FNS(FSR2_FN_FIELD)
#undef FSR2_FN_FIELD
    rlvk_handle device;
    rlvk_memory_properties mem;
    rlvk_handle sampler, set0_layout, set0, pool, cb, cb_memory;
    uint8_t name_what[RL_FSR2_NAMES], name_image[RL_FSR2_NAMES];
    module modules[RL_FSR2_MODULES];
    image images[R_COUNT];
    rlvk_handle mip_views[2];
    uint32_t display[2], render_max[2], luma_mips;
    uint32_t used;
    bool sized, fresh, first, pending;
    uint32_t parity;
    cb_fsr2 c;
    float previous_jitter[2];
};

static bool failed(rl_fsr2 *f, const char *what, rlvk_result r)
{
    rl_log(RL_LOG_WARN, "upscale", "FSR 2 did not start: %s failed (VkResult %d)", what, (int)r);
    rl_fsr2_free(f);
    return false;
}

static int32_t memory_type(const rl_fsr2 *f, uint32_t bits)
{
    for (uint32_t pass = 0; pass < 2; pass++)
        for (uint32_t t = 0; t < f->mem.memoryTypeCount; t++)
            if ((bits & (1u << t)) &&
                (pass || (f->mem.memoryTypes[t].propertyFlags & RLVK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
                return (int32_t)t;
    return -1;
}

static bool bind_memory(rl_fsr2 *f, const rlvk_memory_requirements *req, rlvk_handle *memory)
{
    int32_t t = memory_type(f, req->memoryTypeBits);
    rlvk_memory_allocate_info ai = {
        .sType = RLVK_ST_MEMORY_ALLOCATE_INFO, .allocationSize = req->size, .memoryTypeIndex = (uint32_t)t
    };
    return t >= 0 && f->vkAllocateMemory(f->device, &ai, NULL, memory) == RLVK_SUCCESS;
}

/** @brief Whether the physical device runs quad subgroup operations in compute shaders, as the luminance pass needs. */
static bool has_quads(const rl_post_device *d)
{
    void *raw = rl_hostlib_vulkan_gipa();
    void *(RLVK_CALL *gipa)(rlvk_handle, const char *);
    memcpy(&gipa, &raw, sizeof gipa);
    void *fp = raw ? gipa(d->instance, "vkGetPhysicalDeviceProperties2") : NULL;
    if (!fp)
        return false;
    void(RLVK_CALL * props2)(rlvk_handle, void *);
    memcpy(&props2, &fp, sizeof props2);
    rlvk_subgroup_properties sg = { .sType = RLVK_ST_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    struct {
        int32_t sType;
        void *pNext;
        uint8_t properties[1024];
    } p2 = { .sType = RLVK_ST_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg };
    props2(d->physical_device, &p2);
    return (sg.supportedOperations & RLVK_SUBGROUP_FEATURE_QUAD_BIT) &&
           (sg.supportedStages & RLVK_SHADER_STAGE_COMPUTE_BIT);
}

rl_fsr2 *rl_fsr2_new(const rl_post_device *d)
{
    if (!d || !d->device || !d->get_device_proc_addr)
        return NULL;
    if (!has_quads(d)) {
        rl_log(RL_LOG_WARN, "upscale", "FSR 2 needs quad subgroup operations in compute shaders; this device has none");
        return NULL;
    }
    rl_fsr2 *f = calloc(1, sizeof *f);
    if (!f)
        return NULL;
    f->device = d->device;
    void *(RLVK_CALL *gdpa)(rlvk_handle, const char *);
    memcpy(&gdpa, &d->get_device_proc_addr, sizeof gdpa);
#define FSR2_LOAD(name, ret, args)                                                                                     \
    f->name = (ret(RLVK_CALL *) args)gdpa(f->device, #name);                                                         \
    if (!f->name) {                                                                                                    \
        failed(f, #name " lookup", 0);                                                                                 \
        return NULL;                                                                                                   \
    }
    FSR2_FNS(FSR2_LOAD)
#undef FSR2_LOAD
    void *raw = rl_hostlib_vulkan_gipa();
    void *(RLVK_CALL *gipa)(rlvk_handle, const char *);
    memcpy(&gipa, &raw, sizeof gipa);
    void *fp = gipa(d->instance, "vkGetPhysicalDeviceMemoryProperties");
    void(RLVK_CALL * props)(rlvk_handle, rlvk_memory_properties *);
    memcpy(&props, &fp, sizeof props);
    if (!fp) {
        failed(f, "vkGetPhysicalDeviceMemoryProperties lookup", 0);
        return NULL;
    }
    props(d->physical_device, &f->mem);
    for (uint32_t i = 0; i < RL_FSR2_NAMES; i++)
        for (size_t k = 0; k < sizeof g_names / sizeof g_names[0]; k++)
            if (!strcmp(rl_fsr2_names[i], g_names[k].name)) {
                f->name_what[i] = g_names[k].what;
                f->name_image[i] = g_names[k].image;
            }
    for (uint32_t i = 0; i < RL_FSR2_NAMES; i++)
        if (!f->name_what[i]) {
            rl_log(RL_LOG_WARN, "upscale", "FSR 2 did not start: its passes read %s, which relift does not bind",
                   rl_fsr2_names[i]);
            rl_fsr2_free(f);
            return NULL;
        }
    rlvk_sampler_create_info si = {
        .sType = RLVK_ST_SAMPLER_CREATE_INFO,
        .magFilter = RLVK_FILTER_LINEAR,
        .minFilter = RLVK_FILTER_LINEAR,
        .addressModeU = RLVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = RLVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = RLVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxAnisotropy = 1.0f,
        .maxLod = 1000.0f,
    };
    rlvk_result r = f->vkCreateSampler(f->device, &si, NULL, &f->sampler);
    if (r != RLVK_SUCCESS)
        return failed(f, "vkCreateSampler", r), NULL;
    rlvk_descriptor_set_layout_binding b0 = { .binding = 1,
                                              .descriptorType = RLVK_DESCRIPTOR_TYPE_SAMPLER,
                                              .descriptorCount = 1,
                                              .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT };
    rlvk_descriptor_set_layout_create_info sl = {
        .sType = RLVK_ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &b0
    };
    if ((r = f->vkCreateDescriptorSetLayout(f->device, &sl, NULL, &f->set0_layout)) != RLVK_SUCCESS)
        return failed(f, "vkCreateDescriptorSetLayout", r), NULL;
    rlvk_descriptor_pool_size ps[5] = {
        { RLVK_DESCRIPTOR_TYPE_SAMPLER, 1 },
        { RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, SETS * 16 },
        { RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE, SETS * 16 },
        { RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SETS * 2 },
        { RLVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 },
    };
    rlvk_descriptor_pool_create_info dp = {
        .sType = RLVK_ST_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = SETS + 1, .poolSizeCount = 5, .pPoolSizes = ps
    };
    if ((r = f->vkCreateDescriptorPool(f->device, &dp, NULL, &f->pool)) != RLVK_SUCCESS)
        return failed(f, "vkCreateDescriptorPool", r), NULL;
    rlvk_buffer_create_info bi = { .sType = RLVK_ST_BUFFER_CREATE_INFO,
                                   .size = CB_BYTES,
                                   .usage = RLVK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | RLVK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if ((r = f->vkCreateBuffer(f->device, &bi, NULL, &f->cb)) != RLVK_SUCCESS)
        return failed(f, "vkCreateBuffer", r), NULL;
    rlvk_memory_requirements req;
    f->vkGetBufferMemoryRequirements(f->device, f->cb, &req);
    if (!bind_memory(f, &req, &f->cb_memory) ||
        f->vkBindBufferMemory(f->device, f->cb, f->cb_memory, 0) != RLVK_SUCCESS)
        return failed(f, "the constant buffer's memory", 0), NULL;
    f->first = true;
    return f;
}

static bool make_module(rl_fsr2 *f, uint32_t m)
{
    module *md = &f->modules[m];
    if (md->pipe)
        return true;
    const rl_fsr2_module *src = &rl_fsr2_modules[m];
    rlvk_descriptor_set_layout_binding binds[32];
    uint32_t n = 0;
    for (uint32_t i = 0; i < src->nbinds && n < 32; i++)
        if (src->binds[i].set == 1)
            binds[n++] = (rlvk_descriptor_set_layout_binding){ .binding = src->binds[i].binding,
                                                                .descriptorType = src->binds[i].type,
                                                                .descriptorCount = 1,
                                                                .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT };
    rlvk_descriptor_set_layout_create_info sl = {
        .sType = RLVK_ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = n, .pBindings = binds
    };
    if (f->vkCreateDescriptorSetLayout(f->device, &sl, NULL, &md->set_layout) != RLVK_SUCCESS)
        return false;
    rlvk_handle layouts[2] = { f->set0_layout, md->set_layout };
    rlvk_pipeline_layout_create_info pl = {
        .sType = RLVK_ST_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = layouts
    };
    if (f->vkCreatePipelineLayout(f->device, &pl, NULL, &md->layout) != RLVK_SUCCESS)
        return false;
    rlvk_shader_module_create_info sm = {
        .sType = RLVK_ST_SHADER_MODULE_CREATE_INFO, .codeSize = src->size, .pCode = src->code
    };
    rlvk_handle shader = 0;
    if (f->vkCreateShaderModule(f->device, &sm, NULL, &shader) != RLVK_SUCCESS)
        return false;
    rlvk_compute_pipeline_create_info cp = {
        .sType = RLVK_ST_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = RLVK_ST_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = RLVK_SHADER_STAGE_COMPUTE_BIT,
                   .module = shader,
                   .pName = "main" },
        .layout = md->layout,
        .basePipelineIndex = -1,
    };
    rlvk_result r = f->vkCreateComputePipelines(f->device, 0, 1, &cp, NULL, &md->pipe);
    f->vkDestroyShaderModule(f->device, shader, NULL);
    return r == RLVK_SUCCESS;
}

static void drop_images(rl_fsr2 *f)
{
    for (int i = 0; i < 2; i++)
        if (f->mip_views[i])
            f->vkDestroyImageView(f->device, f->mip_views[i], NULL), f->mip_views[i] = 0;
    for (int i = 0; i < R_COUNT; i++) {
        image *im = &f->images[i];
        if (im->view)
            f->vkDestroyImageView(f->device, im->view, NULL);
        if (im->image)
            f->vkDestroyImage(f->device, im->image, NULL);
        if (im->memory)
            f->vkFreeMemory(f->device, im->memory, NULL);
        memset(im, 0, sizeof *im);
    }
    f->sized = false;
}

static rlvk_handle view(rl_fsr2 *f, rlvk_handle img, int32_t format, uint32_t level, uint32_t levels)
{
    rlvk_image_view_create_info vi = {
        .sType = RLVK_ST_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = RLVK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { RLVK_IMAGE_ASPECT_COLOR_BIT, level, levels, 0, 1 },
    };
    rlvk_handle v = 0;
    return f->vkCreateImageView(f->device, &vi, NULL, &v) == RLVK_SUCCESS ? v : 0;
}

/** @brief Makes FSR 2's images for a display of w by h (the maximum render size the same). */
static bool make_images(rl_fsr2 *f, uint32_t w, uint32_t h)
{
    drop_images(f);
    f->display[0] = f->render_max[0] = w;
    f->display[1] = f->render_max[1] = h;
    uint32_t hw = w / 2 ? w / 2 : 1, hh = h / 2 ? h / 2 : 1, big = hw > hh ? hw : hh;
    f->luma_mips = 1;
    while (big >> f->luma_mips)
        f->luma_mips++;
    if (f->luma_mips < 6)
        return false;
    for (int i = 0; i < R_COUNT; i++) {
        uint32_t iw = 1, ih = 1, mips = 1;
        if (g_images[i].size == SIZE_RENDER || g_images[i].size == SIZE_DISPLAY)
            iw = w, ih = h;
        else if (g_images[i].size == SIZE_HALF_RENDER)
            iw = hw, ih = hh, mips = f->luma_mips;
        rlvk_image_create_info ci = {
            .sType = RLVK_ST_IMAGE_CREATE_INFO,
            .imageType = RLVK_IMAGE_TYPE_2D,
            .format = g_images[i].format,
            .extent = { iw, ih, 1 },
            .mipLevels = mips,
            .arrayLayers = 1,
            .samples = RLVK_SAMPLE_COUNT_1_BIT,
            .tiling = RLVK_IMAGE_TILING_OPTIMAL,
            .usage = RLVK_IMAGE_USAGE_SAMPLED_BIT | RLVK_IMAGE_USAGE_STORAGE_BIT | RLVK_IMAGE_USAGE_TRANSFER_DST_BIT,
            .initialLayout = RLVK_IMAGE_LAYOUT_UNDEFINED,
        };
        image *im = &f->images[i];
        if (f->vkCreateImage(f->device, &ci, NULL, &im->image) != RLVK_SUCCESS)
            return false;
        rlvk_memory_requirements req;
        f->vkGetImageMemoryRequirements(f->device, im->image, &req);
        if (!bind_memory(f, &req, &im->memory) ||
            f->vkBindImageMemory(f->device, im->image, im->memory, 0) != RLVK_SUCCESS ||
            !(im->view = view(f, im->image, g_images[i].format, 0, mips)))
            return false;
    }
    f->mip_views[0] = view(f, f->images[R_SCENE_LUMINANCE].image, RLVK_FORMAT_R16_SFLOAT, LUMA_MIP, 1);
    f->mip_views[1] = view(f, f->images[R_SCENE_LUMINANCE].image, RLVK_FORMAT_R16_SFLOAT, 5, 1);
    f->sized = f->mip_views[0] && f->mip_views[1];
    f->fresh = true;
    f->first = true;
    return f->sized;
}

bool rl_fsr2_takes(const rl_upscale_inputs *in)
{
    return in->depth.handle && in->motion.handle && !in->in.x && !in->in.y && !in->out_rect.x && !in->out_rect.y &&
           in->in.width <= in->out_rect.width && in->in.height <= in->out_rect.height;
}

static void barrier(rl_fsr2 *f, uint64_t cmd, uint32_t src_stage, uint32_t src, uint32_t dst_stage, uint32_t dst)
{
    rlvk_memory_barrier mb = { .sType = RLVK_ST_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst };
    f->vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void clear(rl_fsr2 *f, uint64_t cmd, int i, float r, float g)
{
    float color[4] = { r, g, 0.0f, 0.0f };
    rlvk_image_subresource_range rr = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, ~0u, 0, 1 };
    f->vkCmdClearColorImage(cmd, f->images[i].image, RLVK_IMAGE_LAYOUT_GENERAL, color, 1, &rr);
}

/** @brief f's half-precision bits for v (round to nearest even, no NaN payloads), as FsrRcasCon packs them. */
static uint16_t half_bits(float v)
{
    uint32_t x;
    memcpy(&x, &v, 4);
    uint32_t sign = (x >> 16) & 0x8000u, e = (x >> 23) & 0xffu, m = x & 0x7fffffu;
    if (e >= 143)
        return (uint16_t)(sign | 0x7c00u);
    if (e < 113)
        return (uint16_t)sign;
    uint32_t h = ((e - 112) << 10) | (m >> 13), rest = m & 0x1fffu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1)))
        h++;
    return (uint16_t)(sign | h);
}

/** @brief The view the frame binds for name index n, or 0. */
static rlvk_handle view_of(const rl_fsr2 *f, uint32_t n, const rl_upscale_inputs *in)
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
            return f->images[R_AUTO_EXPOSURE].view;
        return in->exposure.handle ? in->exposure.handle : f->images[R_DEFAULT_EXPOSURE].view;
    case N_REACTIVE:
        return in->reactive.handle ? in->reactive.handle : f->images[R_DEFAULT_REACTIVITY].view;
    case N_COMPOSITION:
        return f->images[R_DEFAULT_REACTIVITY].view;
    case N_OUTPUT:
        return in->out.handle;
    case N_MIP_SHADING_CHANGE:
        return f->mip_views[0];
    case N_MIP_5:
        return f->mip_views[1];
    case N_DILATED_MOTION:
        return f->images[odd ? R_DILATED_MOTION_VECTORS_2 : R_DILATED_MOTION_VECTORS_1].view;
    case N_PREVIOUS_DILATED_MOTION:
        return f->images[odd ? R_DILATED_MOTION_VECTORS_1 : R_DILATED_MOTION_VECTORS_2].view;
    case N_LOCK_SRV:
        return f->images[odd ? R_LOCK_STATUS_2 : R_LOCK_STATUS_1].view;
    case N_LOCK_UAV:
        return f->images[odd ? R_LOCK_STATUS_1 : R_LOCK_STATUS_2].view;
    case N_UPSCALED_SRV:
        return f->images[odd ? R_INTERNAL_UPSCALED_COLOR_2 : R_INTERNAL_UPSCALED_COLOR_1].view;
    case N_UPSCALED_UAV:
        return f->images[odd ? R_INTERNAL_UPSCALED_COLOR_1 : R_INTERNAL_UPSCALED_COLOR_2].view;
    case N_LUMA_HISTORY_SRV:
        return f->images[odd ? R_LUMA_HISTORY_2 : R_LUMA_HISTORY_1].view;
    case N_LUMA_HISTORY_UAV:
        return f->images[odd ? R_LUMA_HISTORY_1 : R_LUMA_HISTORY_2].view;
    case N_FIXED:
        return f->images[f->name_image[n]].view;
    default:
        return 0;
    }
}

/** @brief Allocates and fills pass module m's set 1 for this frame; 0 when none is left. */
static rlvk_handle pass_set(rl_fsr2 *f, uint32_t m, const rl_upscale_inputs *in)
{
    rlvk_handle set = 0;
    rlvk_descriptor_set_allocate_info da = { .sType = RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = f->pool,
                                             .descriptorSetCount = 1,
                                             .pSetLayouts = &f->modules[m].set_layout };
    if (f->vkAllocateDescriptorSets(f->device, &da, &set) != RLVK_SUCCESS)
        return 0;
    const rl_fsr2_module *src = &rl_fsr2_modules[m];
    rlvk_write_descriptor_set wr[32];
    rlvk_descriptor_image_info ii[32];
    rlvk_descriptor_buffer_info bi[32];
    uint32_t n = 0;
    for (uint32_t i = 0; i < src->nbinds && n < 32; i++) {
        const rl_fsr2_bind *b = &src->binds[i];
        if (b->set != 1)
            continue;
        wr[n] = (rlvk_write_descriptor_set){ .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = b->binding,
                                             .descriptorCount = 1,
                                             .descriptorType = b->type };
        uint8_t what = f->name_what[b->name];
        if (b->type == RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER) {
            uint32_t off = what == N_CB_SPD ? CB_SPD : what == N_CB_RCAS ? CB_RCAS : CB_FSR2;
            uint32_t size = what == N_CB_SPD ? sizeof(cb_spd) : what == N_CB_RCAS ? 16u : sizeof(cb_fsr2);
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
    f->vkUpdateDescriptorSets(f->device, n, wr, 0, NULL);
    return set;
}

static bool start_sets(rl_fsr2 *f)
{
    if (f->set0)
        return true;
    rlvk_descriptor_set_allocate_info da = { .sType = RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = f->pool,
                                             .descriptorSetCount = 1,
                                             .pSetLayouts = &f->set0_layout };
    if (f->vkAllocateDescriptorSets(f->device, &da, &f->set0) != RLVK_SUCCESS)
        return false;
    rlvk_descriptor_image_info ii = { .sampler = f->sampler };
    rlvk_write_descriptor_set wr = { .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
                                     .dstSet = f->set0,
                                     .dstBinding = 1,
                                     .descriptorCount = 1,
                                     .descriptorType = RLVK_DESCRIPTOR_TYPE_SAMPLER,
                                     .pImageInfo = &ii };
    f->vkUpdateDescriptorSets(f->device, 1, &wr, 0, NULL);
    return true;
}

/** @brief Fills the constants of this frame (fsr2Dispatch's), into f->c and spd; reset starts the history again. */
static void constants(rl_fsr2 *f, const rl_upscale_inputs *in, bool reset, cb_spd *spd, uint32_t rcas[4])
{
    cb_fsr2 *c = &f->c;
    float rw = (float)in->in.width, rh = (float)in->in.height;
    c->jitter[0] = in->jitter[0];
    c->jitter[1] = in->jitter[1];
    c->render_size[0] = (int32_t)in->in.width;
    c->render_size[1] = (int32_t)in->in.height;
    c->max_render_size[0] = (int32_t)f->render_max[0];
    c->max_render_size[1] = (int32_t)f->render_max[1];
    c->display_size[0] = (int32_t)f->display[0];
    c->display_size[1] = (int32_t)f->display[1];
    c->input_color_dimensions[0] = (int32_t)in->color.width;
    c->input_color_dimensions[1] = (int32_t)in->color.height;
    float aspect = rw / rh;
    float fov_x = atanf(tanf(CAMERA_FOV_Y / 2.0f) * aspect) * 2.0f;
    c->tan_half_fov = tanf(fov_x * 0.5f);
    c->view_space_to_meters = 1.0f;
    float lo = CAMERA_NEAR, hi = CAMERA_FAR;
    if (in->depth_inverted) {
        float t = lo;
        lo = hi;
        hi = t;
    }
    float q = hi / (lo - hi);
    c->device_to_view_depth[0] = -q;
    c->device_to_view_depth[1] = q * lo;
    float cot = cosf(0.5f * CAMERA_FOV_Y) / sinf(0.5f * CAMERA_FOV_Y);
    c->device_to_view_depth[2] = 1.0f / (cot / aspect);
    c->device_to_view_depth[3] = 1.0f / cot;
    c->downscale[0] = rw / (float)f->display[0];
    c->downscale[1] = rh / (float)f->display[1];
    c->previous_pre_exposure = c->pre_exposure;
    c->pre_exposure = in->pre_exposure != 0.0f ? in->pre_exposure : 1.0f;
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
    c->frame_index = reset ? 0 : c->frame_index + 1;
    c->luma_mip_level = (int32_t)LUMA_MIP;
    float mip_div = (float)(2u << LUMA_MIP);
    c->luma_mip_dimensions[0] = (int32_t)((float)c->max_render_size[0] / mip_div);
    c->luma_mip_dimensions[1] = (int32_t)((float)c->max_render_size[1] / mip_div);
    uint32_t ex = (in->in.width - 1) / 64, ey = (in->in.height - 1) / 64;
    uint32_t big = in->in.width > in->in.height ? in->in.width : in->in.height, mips = 0;
    while ((big >> (mips + 1)) && mips < 12)
        mips++;
    *spd = (cb_spd){
        .mips = mips, .num_work_groups = (ex + 1) * (ey + 1), .render_size = { in->in.width, in->in.height }
    };
    float sharp = in->sharpness < 0.0f ? 0.0f : in->sharpness > 1.0f ? 1.0f : in->sharpness;
    float lin = exp2f(-(-2.0f * sharp + 2.0f));
    memcpy(&rcas[0], &lin, 4);
    rcas[1] = (uint32_t)half_bits(lin) | (uint32_t)half_bits(lin) << 16;
    rcas[2] = rcas[3] = 0;
}

bool rl_fsr2_record(rl_fsr2 *f, uint64_t cmd, const rl_upscale_inputs *in)
{
    if (!f || !cmd || !rl_fsr2_takes(in) || !in->color.handle || !in->out.handle)
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
    if (f->used + RL_FSR2_PASSES > SETS || !start_sets(f))
        return false;
    uint32_t opts = (in->hdr ? 1u : 0u) | (in->motion_display_size ? 0u : 2u) | (in->depth_inverted ? 4u : 0u) |
                    (in->sharpness > 0.0f ? 8u : 0u);
    uint32_t mods[RL_FSR2_PASSES];
    for (uint32_t p = 0; p < RL_FSR2_PASSES; p++) {
        mods[p] = rl_fsr2_pass_module[p][opts];
        if (!make_module(f, mods[p])) {
            rl_log(RL_LOG_WARN, "upscale", "FSR 2: pass %u did not compile on this device", p);
            return false;
        }
    }
    bool sharpen = opts & 8u;
    uint32_t npasses = sharpen ? RL_FSR2_PASSES : RL_FSR2_PASSES - 1;
    bool reset = in->reset || f->first;
    rlvk_handle sets[RL_FSR2_PASSES];
    for (uint32_t p = 0; p < npasses; p++)
        if (!(sets[p] = pass_set(f, mods[p], in)))
            return false;
    f->used += RL_FSR2_PASSES;
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
        f->vkCmdPipelineBarrier(cmd, cs | xfer, cs | xfer, 0, 0, NULL, 0, NULL, R_COUNT, ib);
    } else {
        barrier(f, cmd, cs, rw_access, cs | xfer, rw_access | RLVK_ACCESS_TRANSFER_WRITE_BIT);
    }
    bool odd = f->parity & 1;
    if (f->fresh)
        for (int i = 0; i < R_COUNT; i++)
            clear(f, cmd, i, 0.0f, 0.0f);
    if (reset) {
        clear(f, cmd, odd ? R_LOCK_STATUS_2 : R_LOCK_STATUS_1, 0.0f, 0.0f);
        clear(f, cmd, odd ? R_INTERNAL_UPSCALED_COLOR_2 : R_INTERNAL_UPSCALED_COLOR_1, 0.0f, 0.0f);
        clear(f, cmd, R_SCENE_LUMINANCE, 0.0f, 0.0f);
        clear(f, cmd, R_AUTO_EXPOSURE, -1.0f, 1e8f);
    }
    f->fresh = false;
    f->first = false;
    cb_spd spd;
    uint32_t rcas[4];
    constants(f, in, reset, &spd, rcas);
    f->vkCmdUpdateBuffer(cmd, f->cb, CB_FSR2, sizeof f->c, &f->c);
    f->vkCmdUpdateBuffer(cmd, f->cb, CB_SPD, sizeof spd, &spd);
    f->vkCmdUpdateBuffer(cmd, f->cb, CB_RCAS, sizeof rcas, rcas);
    barrier(f, cmd, xfer, RLVK_ACCESS_TRANSFER_WRITE_BIT, cs, rw_access | RLVK_ACCESS_UNIFORM_READ_BIT);
    uint32_t src_x = (in->in.width + 7) / 8, src_y = (in->in.height + 7) / 8;
    uint32_t dst_x = (dw + 7) / 8, dst_y = (dh + 7) / 8;
    uint32_t groups[RL_FSR2_PASSES][2] = {
        { (in->in.width - 1) / 64 + 1, (in->in.height - 1) / 64 + 1 },
        { src_x, src_y },
        { src_x, src_y },
        { src_x, src_y },
        { dst_x, dst_y },
        { (dw + 15) / 16, (dh + 15) / 16 },
    };
    for (uint32_t p = 0; p < npasses; p++) {
        const module *md = &f->modules[mods[p]];
        rlvk_handle both[2] = { f->set0, sets[p] };
        f->vkCmdBindPipeline(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, md->pipe);
        f->vkCmdBindDescriptorSets(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, md->layout, 0, 2, both, 0, NULL);
        f->vkCmdDispatch(cmd, groups[p][0], groups[p][1], 1);
        if (p + 1 < npasses)
            barrier(f, cmd, cs, rw_access, cs, rw_access);
    }
    f->parity++;
    return true;
}

void rl_fsr2_retire(rl_fsr2 *f)
{
    if (!f || !f->pool)
        return;
    f->vkResetDescriptorPool(f->device, f->pool, 0);
    f->set0 = 0;
    f->used = 0;
    f->pending = false;
}

void rl_fsr2_free(rl_fsr2 *f)
{
    if (!f)
        return;
    drop_images(f);
    for (uint32_t m = 0; m < RL_FSR2_MODULES; m++) {
        module *md = &f->modules[m];
        if (md->pipe)
            f->vkDestroyPipeline(f->device, md->pipe, NULL);
        if (md->layout)
            f->vkDestroyPipelineLayout(f->device, md->layout, NULL);
        if (md->set_layout)
            f->vkDestroyDescriptorSetLayout(f->device, md->set_layout, NULL);
    }
    if (f->cb)
        f->vkDestroyBuffer(f->device, f->cb, NULL);
    if (f->cb_memory)
        f->vkFreeMemory(f->device, f->cb_memory, NULL);
    if (f->pool)
        f->vkDestroyDescriptorPool(f->device, f->pool, NULL);
    if (f->set0_layout)
        f->vkDestroyDescriptorSetLayout(f->device, f->set0_layout, NULL);
    if (f->sampler)
        f->vkDestroySampler(f->device, f->sampler, NULL);
    free(f);
}
