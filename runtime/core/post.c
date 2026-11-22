/**
 * @file post.c
 * @brief Player picture options (post.h): the core picture pipeline.
 *
 * The pipeline runs compute passes on the core's Vulkan device (vkdev.h),
 * shared with the window presenter: the frame goes into a host
 * buffer as one RGBA8 word per pixel, is copied to a device buffer, runs
 * through the passes the options ask for and comes back the same way. The
 * passes' SPIR-V and SMAA's textures are post_data.h (from
 * runtime/core/shaders and runtime/third_party/smaa). The upscaler stage
 * (rl_post_set_upscaler) records into the same command buffer ahead of the
 * anti-aliasing passes, which then run at its output size.
 *
 * With every option off and no upscaler rl_post_apply returns the frame it
 * was given and nothing here starts. When Vulkan cannot start or a frame fails, the
 * options are off for the rest of the run and the log says why.
 */
#include <relift/post.h>
#include <relift/settings.h>
#include <relift/upscale.h>

#include "core.h"
#include "post_data.h"
#include "postgpu.h"
#include "vkdev.h"
#include "spirvval.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define GROUP 8u
#define NBIND 6u

/** @brief The push constants of every pass (post.glsl Params). */
typedef struct params {
    int32_t width, height;
    float threshold;
    int32_t max_search_steps, max_search_steps_diag, corner_rounding;
} params;

enum { B_SRC, B_IN, B_OUT, B_EDGES, B_BLEND, B_AREA, B_SEARCH, B_STAGE_IN, B_STAGE_OUT, B_COUNT };

enum { P_FXAA, P_SMAA_EDGES, P_SMAA_WEIGHTS, P_SMAA_WEIGHTS_FAST, P_SMAA_BLEND, P_COUNT };

typedef struct vk {
    const rlvk_dev *d;
    rlvk_handle pool, cmd, fence;
    rlvk_handle set_layout, layout, dpool, set;
    rlvk_handle pipe[P_COUNT];
    rlvk_buf b[B_COUNT];
    uint32_t sw, sh, w, h;
    bool staged;
    /** @brief relift's own window stage (upscale.h): the `picture.upscaler` choice at `picture.upscale_scale` times. */
    rl_post_device own_device;
    rl_upscaler *own_u;
    uint32_t own_choice;
    const rl_post_stage *cur;
    void *cur_user;
    bool up_started, up_ok;
    uint64_t up_gen;
    /** @brief What rl_post_gpu_plan decided for the frame being recorded. */
    bool plan_up;
    uint32_t plan_aa;
    bool tried, ok;
} vk;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static vk g_vk;
static const rl_post_stage *g_up;
static void *g_up_user;
static uint64_t g_up_gen;

static bool own_start(void *user, const rl_post_device *device)
{
    vk *v = user;
    v->own_device = *device;
    return true;
}

static bool own_plan(void *user, uint32_t width, uint32_t height, uint32_t *out_width, uint32_t *out_height)
{
    vk *v = user;
    uint32_t choice = rl_upscale_spatial(rl_setting_enum("picture.upscaler"));
    if (choice != v->own_choice || !v->own_u) {
        rl_upscaler_free(v->own_u);
        v->own_u = choice == RL_UPSCALE_OFF ? NULL : rl_upscaler_new(&v->own_device, choice, RL_UPSCALE_BUFFERS);
        v->own_choice = choice;
    }
    int64_t scale = rl_setting_int("picture.upscale_scale");
    *out_width = width * (uint32_t)scale;
    *out_height = height * (uint32_t)scale;
    return v->own_u != NULL;
}

static void own_record(void *user, uint64_t cmd, uint64_t in, uint64_t out, uint32_t width, uint32_t height,
                       uint32_t out_width, uint32_t out_height)
{
    vk *v = user;
    rl_upscaler_retire(v->own_u);
    rl_upscale_inputs ui = {
        .color = { .handle = in, .width = width, .height = height },
        .in = { 0, 0, width, height },
        .out = { .handle = out, .width = out_width, .height = out_height },
        .out_rect = { 0, 0, out_width, out_height },
        .pre_exposure = 1.0f,
    };
    rl_upscaler_record(v->own_u, cmd, &ui);
}

static const rl_post_stage g_own_stage = { "relift", own_start, own_plan, own_record };

/** @brief The stage the window runs this frame: the guest's, else relift's own for a spatial `picture.upscaler`. */
static const rl_post_stage *stage_now(vk *v, void **user)
{
    const rl_post_stage *s = __atomic_load_n(&g_up, __ATOMIC_ACQUIRE);
    *user = g_up_user;
    if (!s && !rl_upscale_window_off() && rl_upscale_spatial(rl_setting_enum("picture.upscaler")) != RL_UPSCALE_OFF) {
        s = &g_own_stage;
        *user = v;
    }
    return s;
}
static uint8_t *g_out;
static size_t g_out_cap;

static const struct {
    const uint32_t *code;
    size_t size;
} g_spv[P_COUNT] = {
    [P_FXAA] = { rl_post_spv_fxaa, sizeof rl_post_spv_fxaa },
    [P_SMAA_EDGES] = { rl_post_spv_smaa_edges, sizeof rl_post_spv_smaa_edges },
    [P_SMAA_WEIGHTS] = { rl_post_spv_smaa_weights, sizeof rl_post_spv_smaa_weights },
    [P_SMAA_WEIGHTS_FAST] = { rl_post_spv_smaa_weights_fast, sizeof rl_post_spv_smaa_weights_fast },
    [P_SMAA_BLEND] = { rl_post_spv_smaa_blend, sizeof rl_post_spv_smaa_blend },
};

/** @brief SMAA's quality presets (SMAA.hlsl SMAA_PRESET_*) by picture.aa_quality; low and medium skip the diagonal and
 * corner passes. */
static const struct {
    float threshold;
    int32_t steps, diag_steps, corner;
    bool fast;
} g_smaa[] = {
    { 0.15f, 4, 0, 0, true },
    { 0.1f, 8, 0, 0, true },
    { 0.1f, 16, 8, 25, false },
    { 0.05f, 32, 16, 25, false },
};

static bool fail(const char *what, rlvk_result r)
{
    rl_log(RL_LOG_WARN, "post", "picture options are off: %s failed (VkResult %d)", what, (int)r);
    return false;
}

static bool make_pipelines(vk *v)
{
    rlvk_descriptor_set_layout_binding bind[NBIND];
    for (uint32_t i = 0; i < NBIND; i++)
        bind[i] = (rlvk_descriptor_set_layout_binding){
            .binding = i,
            .descriptorType = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT,
        };
    rlvk_descriptor_set_layout_create_info sl = {
        .sType = RLVK_ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = NBIND,
        .pBindings = bind,
    };
    rlvk_result r = v->d->vkCreateDescriptorSetLayout(v->d->device, &sl, NULL, &v->set_layout);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout", r);
    rlvk_push_constant_range pcr = { .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(params) };
    rlvk_pipeline_layout_create_info pl = {
        .sType = RLVK_ST_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &v->set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    r = v->d->vkCreatePipelineLayout(v->d->device, &pl, NULL, &v->layout);
    if (r != RLVK_SUCCESS)
        return fail("vkCreatePipelineLayout", r);
    for (int p = 0; p < P_COUNT; p++) {
        rlvk_shader_module_create_info sm = {
            .sType = RLVK_ST_SHADER_MODULE_CREATE_INFO,
            .codeSize = g_spv[p].size,
            .pCode = g_spv[p].code,
        };
        rlvk_handle module = 0;
        rl_spirv_check(sm.pCode, sm.codeSize / 4, "a picture pipeline program");
        r = v->d->vkCreateShaderModule(v->d->device, &sm, NULL, &module);
        if (r != RLVK_SUCCESS)
            return fail("vkCreateShaderModule", r);
        rlvk_compute_pipeline_create_info cp = {
            .sType = RLVK_ST_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = RLVK_ST_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = RLVK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = "main",
            },
            .layout = v->layout,
            .basePipelineIndex = -1,
        };
        r = v->d->vkCreateComputePipelines(v->d->device, 0, 1, &cp, NULL, &v->pipe[p]);
        v->d->vkDestroyShaderModule(v->d->device, module, NULL);
        if (r != RLVK_SUCCESS)
            return fail("vkCreateComputePipelines", r);
    }
    rlvk_descriptor_pool_size ps = { .type = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = NBIND };
    rlvk_descriptor_pool_create_info dp = {
        .sType = RLVK_ST_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &ps,
    };
    r = v->d->vkCreateDescriptorPool(v->d->device, &dp, NULL, &v->dpool);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateDescriptorPool", r);
    rlvk_descriptor_set_allocate_info da = {
        .sType = RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = v->dpool,
        .descriptorSetCount = 1,
        .pSetLayouts = &v->set_layout,
    };
    r = v->d->vkAllocateDescriptorSets(v->d->device, &da, &v->set);
    if (r != RLVK_SUCCESS)
        return fail("vkAllocateDescriptorSets", r);
    rlvk_command_pool_create_info cpi = {
        .sType = RLVK_ST_COMMAND_POOL_CREATE_INFO,
        .flags = RLVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = v->d->family,
    };
    r = v->d->vkCreateCommandPool(v->d->device, &cpi, NULL, &v->pool);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateCommandPool", r);
    rlvk_command_buffer_allocate_info ca = {
        .sType = RLVK_ST_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = v->pool,
        .level = RLVK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    r = v->d->vkAllocateCommandBuffers(v->d->device, &ca, &v->cmd);
    if (r != RLVK_SUCCESS)
        return fail("vkAllocateCommandBuffers", r);
    rlvk_fence_create_info fi = { .sType = RLVK_ST_FENCE_CREATE_INFO };
    r = v->d->vkCreateFence(v->d->device, &fi, NULL, &v->fence);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateFence", r);
    return true;
}

static rlvk_result submit(vk *v, const rlvk_submit_info *sub)
{
    rl_vkdev_queue_lock();
    rlvk_result r = v->d->vkQueueSubmit(v->d->queue, 1, sub, v->fence);
    rl_vkdev_queue_unlock();
    return r;
}

/** @brief Copies data (n words) into a new device buffer through a host buffer, and waits. */
static bool upload(vk *v, rlvk_buf *dst, const uint32_t *data, size_t n)
{
    rlvk_buf stage = { 0 };
    rlvk_size bytes = (rlvk_size)n * 4;
    bool ok = rl_vkdev_buf_make(v->d, dst, bytes, RLVK_BUFFER_USAGE_STORAGE_BUFFER_BIT | RLVK_BUFFER_USAGE_TRANSFER_DST_BIT, false,
                       false) &&
              rl_vkdev_buf_make(v->d, &stage, bytes, RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, false);
    if (ok) {
        memcpy(stage.map, data, (size_t)bytes);
        rl_vkdev_buf_flush(v->d, &stage);
        rlvk_command_buffer_begin_info bi = {
            .sType = RLVK_ST_COMMAND_BUFFER_BEGIN_INFO,
            .flags = RLVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        rlvk_buffer_copy copy = { .size = bytes };
        rlvk_submit_info sub = { .sType = RLVK_ST_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &v->cmd };
        ok = v->d->vkBeginCommandBuffer(v->cmd, &bi) == RLVK_SUCCESS;
        if (ok) {
            v->d->vkCmdCopyBuffer(v->cmd, stage.buffer, dst->buffer, 1, &copy);
            ok = v->d->vkEndCommandBuffer(v->cmd) == RLVK_SUCCESS &&
                 submit(v, &sub) == RLVK_SUCCESS &&
                 v->d->vkWaitForFences(v->d->device, 1, &v->fence, 1, ~(uint64_t)0) == RLVK_SUCCESS;
            v->d->vkResetFences(v->d->device, 1, &v->fence);
        }
        if (!ok)
            fail("uploading SMAA's textures", 0);
    }
    rl_vkdev_buf_free(v->d, &stage);
    return ok;
}

/** @brief SMAA's area and search textures, one word per texel, in device buffers. */
static bool upload_luts(vk *v)
{
    enum { AREA = sizeof rl_post_smaa_area / 2, SEARCH = sizeof rl_post_smaa_search };
    uint32_t *words = malloc(AREA * sizeof *words);
    if (!words)
        return fail("allocation", 0);
    for (size_t i = 0; i < AREA; i++)
        words[i] = (uint32_t)rl_post_smaa_area[2 * i] | (uint32_t)rl_post_smaa_area[2 * i + 1] << 8;
    bool ok = upload(v, &v->b[B_AREA], words, AREA);
    for (size_t i = 0; ok && i < SEARCH; i++)
        words[i] = rl_post_smaa_search[i];
    ok = ok && upload(v, &v->b[B_SEARCH], words, SEARCH);
    free(words);
    return ok;
}

static bool start(vk *v)
{
    v->d = rl_vkdev();
    if (!v->d)
        return fail("starting the Vulkan device", 0);
    return make_pipelines(v) && upload_luts(v);
}

/**
 * @brief Buffers for a sw by sh picture in and a w by h picture after the upscaler (the anti-aliasing size), and the
 * descriptor set that names them.
 */
static bool size_buffers(vk *v, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h, bool staged)
{
    if (v->sw == sw && v->sh == sh && v->w == w && v->h == h && (!staged || v->staged))
        return true;
    v->staged = false;
    v->sw = v->sh = v->w = v->h = 0;
    rlvk_size src = (rlvk_size)sw * sh * 4, bytes = (rlvk_size)w * h * 4;
    const uint32_t storage = RLVK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const uint32_t both = RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT | RLVK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!rl_vkdev_buf_make(v->d, &v->b[B_SRC], src, storage | both, false, false) ||
        !rl_vkdev_buf_make(v->d, &v->b[B_IN], bytes, storage | both, false, false) ||
        !rl_vkdev_buf_make(v->d, &v->b[B_OUT], bytes, storage | RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, false) ||
        !rl_vkdev_buf_make(v->d, &v->b[B_EDGES], bytes, storage, false, false) ||
        !rl_vkdev_buf_make(v->d, &v->b[B_BLEND], bytes, storage, false, false))
        return false;
    if (staged && (!rl_vkdev_buf_make(v->d, &v->b[B_STAGE_IN], src, RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, false) ||
                   !rl_vkdev_buf_make(v->d, &v->b[B_STAGE_OUT], bytes, RLVK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true)))
        return false;
    v->staged = staged;
    static const int bound[NBIND] = { B_IN, B_OUT, B_EDGES, B_BLEND, B_AREA, B_SEARCH };
    rlvk_descriptor_buffer_info info[NBIND];
    rlvk_write_descriptor_set wr[NBIND];
    for (uint32_t i = 0; i < NBIND; i++) {
        info[i] = (rlvk_descriptor_buffer_info){ .buffer = v->b[bound[i]].buffer, .range = v->b[bound[i]].size };
        wr[i] = (rlvk_write_descriptor_set){
            .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
            .dstSet = v->set,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &info[i],
        };
    }
    v->d->vkUpdateDescriptorSets(v->d->device, NBIND, wr, 0, NULL);
    v->sw = sw;
    v->sh = sh;
    v->w = w;
    v->h = h;
    return true;
}

static void barrier(vk *v, uint32_t src_stage, uint32_t src, uint32_t dst_stage, uint32_t dst)
{
    rlvk_memory_barrier mb = { .sType = RLVK_ST_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst };
    v->d->vkCmdPipelineBarrier(v->cmd, src_stage, dst_stage, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void dispatch(vk *v, int pipe, const params *pc)
{
    v->d->vkCmdBindPipeline(v->cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, v->pipe[pipe]);
    v->d->vkCmdPushConstants(v->cmd, v->layout, RLVK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *pc, pc);
    v->d->vkCmdDispatch(v->cmd, (v->w + GROUP - 1) / GROUP, (v->h + GROUP - 1) / GROUP, 1);
}

static void pack(uint32_t *dst, const uint8_t *rgb, size_t n)
{
    for (size_t i = 0; i < n; i++, rgb += 3)
        dst[i] = (uint32_t)rgb[0] | (uint32_t)rgb[1] << 8 | (uint32_t)rgb[2] << 16 | 0xff000000u;
}

static void unpack(uint8_t *rgb, const uint32_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++, rgb += 3) {
        rgb[0] = (uint8_t)src[i];
        rgb[1] = (uint8_t)(src[i] >> 8);
        rgb[2] = (uint8_t)(src[i] >> 16);
    }
}

/** @brief Records SMAA 1x at a quality preset (picture.aa_quality): edges, blending weights, neighborhood blending. */
static void record_smaa(vk *v, params pc, uint32_t quality)
{
    quality = quality < 4 ? quality : 2;
    pc.threshold = g_smaa[quality].threshold;
    pc.max_search_steps = g_smaa[quality].steps;
    pc.max_search_steps_diag = g_smaa[quality].diag_steps;
    pc.corner_rounding = g_smaa[quality].corner;
    const uint32_t cs = RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const uint32_t w = RLVK_ACCESS_SHADER_WRITE_BIT, r = RLVK_ACCESS_SHADER_READ_BIT;
    dispatch(v, P_SMAA_EDGES, &pc);
    barrier(v, cs, w, cs, r);
    dispatch(v, g_smaa[quality].fast ? P_SMAA_WEIGHTS_FAST : P_SMAA_WEIGHTS, &pc);
    barrier(v, cs, w, cs, r);
    dispatch(v, P_SMAA_BLEND, &pc);
}

/** @brief Starts the upscaler on the pipeline's device the first time it is needed; false when it is off. */
static bool upscaler_ready(vk *v, const rl_post_stage *stage, void *user)
{
    uint64_t gen = __atomic_load_n(&g_up_gen, __ATOMIC_ACQUIRE);
    if (stage != v->cur || user != v->cur_user || gen != v->up_gen) {
        v->up_gen = gen;
        v->cur = stage;
        v->cur_user = user;
        v->up_started = v->up_ok = false;
    }
    if (!v->cur)
        return false;
    if (!v->up_started) {
        v->up_started = true;
        rl_post_device d = {
            .instance = v->d->instance,
            .physical_device = v->d->phys,
            .device = v->d->device,
            .queue = v->d->queue,
            .queue_family = v->d->family,
            .get_device_proc_addr = (void *)(uintptr_t)v->d->vkGetDeviceProcAddr,
        };
        v->up_ok = v->cur->start(v->cur_user, &d);
        if (!v->up_ok)
            rl_log(RL_LOG_WARN, "post", "the upscaler %s did not start; the picture is shown at its own size",
                   v->cur->name);
    }
    return v->up_ok;
}

/**
 * @brief Records the passes between the picture in B_SRC (or B_IN without an upscaler) and the result, which ends in
 * B_OUT with aa on and in B_IN otherwise.
 */
static void record_passes(vk *v, bool up, uint32_t aa, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h)
{
    const uint32_t xfer = RLVK_PIPELINE_STAGE_TRANSFER_BIT, cs = RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const uint32_t any_read = RLVK_ACCESS_SHADER_READ_BIT | RLVK_ACCESS_TRANSFER_READ_BIT;
    const uint32_t any_write = RLVK_ACCESS_SHADER_WRITE_BIT | RLVK_ACCESS_TRANSFER_WRITE_BIT;
    barrier(v, xfer, RLVK_ACCESS_TRANSFER_WRITE_BIT, xfer | cs, any_read);
    if (up) {
        v->cur->record(v->cur_user, v->cmd, v->b[B_SRC].buffer, v->b[B_IN].buffer, sw, sh, w, h);
        barrier(v, xfer | cs, any_write, xfer | cs, any_read);
    }
    if (aa != RL_POST_AA_OFF) {
        v->d->vkCmdBindDescriptorSets(v->cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, v->layout, 0, 1, &v->set, 0, NULL);
        params pc = { .width = (int32_t)w, .height = (int32_t)h };
        if (aa == RL_POST_AA_FXAA)
            dispatch(v, P_FXAA, &pc);
        else
            record_smaa(v, pc, rl_setting_enum("picture.aa_quality"));
        barrier(v, cs, RLVK_ACCESS_SHADER_WRITE_BIT, xfer, RLVK_ACCESS_TRANSFER_READ_BIT);
    }
}

/**
 * @brief Runs the upscaler (when up) and the passes of aa on sw by sh RGB888 pixels into out, which is w by h (the
 * upscaler's output size).
 */
static bool run(vk *v, bool up, uint32_t aa, const uint8_t *rgb, uint8_t *out, uint32_t sw, uint32_t sh, uint32_t w,
                uint32_t h)
{
    if (!size_buffers(v, sw, sh, w, h, true))
        return false;
    size_t n = (size_t)w * h;
    rlvk_buf *si = &v->b[B_STAGE_IN], *so = &v->b[B_STAGE_OUT];
    pack(si->map, rgb, (size_t)sw * sh);
    rl_vkdev_buf_flush(v->d, si);

    rlvk_command_buffer_begin_info bi = {
        .sType = RLVK_ST_COMMAND_BUFFER_BEGIN_INFO,
        .flags = RLVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    rlvk_result r = v->d->vkBeginCommandBuffer(v->cmd, &bi);
    if (r != RLVK_SUCCESS)
        return fail("vkBeginCommandBuffer", r);
    rlvk_buffer_copy in_copy = { .size = (rlvk_size)sw * sh * 4 }, copy = { .size = (rlvk_size)n * 4 };
    v->d->vkCmdCopyBuffer(v->cmd, si->buffer, v->b[up ? B_SRC : B_IN].buffer, 1, &in_copy);
    record_passes(v, up, aa, sw, sh, w, h);
    v->d->vkCmdCopyBuffer(v->cmd, v->b[aa != RL_POST_AA_OFF ? B_OUT : B_IN].buffer, so->buffer, 1, &copy);
    barrier(v, RLVK_PIPELINE_STAGE_TRANSFER_BIT, RLVK_ACCESS_TRANSFER_WRITE_BIT, RLVK_PIPELINE_STAGE_HOST_BIT,
            RLVK_ACCESS_HOST_READ_BIT);
    r = v->d->vkEndCommandBuffer(v->cmd);
    if (r != RLVK_SUCCESS)
        return fail("vkEndCommandBuffer", r);

    rlvk_submit_info sub = { .sType = RLVK_ST_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &v->cmd };
    r = submit(v, &sub);
    if (r != RLVK_SUCCESS)
        return fail("vkQueueSubmit", r);
    r = v->d->vkWaitForFences(v->d->device, 1, &v->fence, 1, ~(uint64_t)0);
    v->d->vkResetFences(v->d->device, 1, &v->fence);
    if (r != RLVK_SUCCESS)
        return fail("vkWaitForFences", r);
    rl_vkdev_buf_invalidate(v->d, so);
    unpack(out, so->map, n);
    return true;
}

bool rl_post_integer(bool fit)
{
    rl_settings_declare_runtime();
    uint32_t f = rl_setting_enum("picture.filter");
    return f == RL_POST_FILTER_INTEGER || (f == RL_POST_FILTER_DEFAULT && !fit);
}

int rl_post_sampling(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h)
{
    if (!src_w || !src_h || (dst_w % src_w == 0 && dst_h % src_h == 0))
        return RL_POST_SAMPLE_NEAREST;
    rl_settings_declare_runtime();
    uint32_t f = rl_setting_enum("picture.filter");
    if (f == RL_POST_FILTER_SHARP_BILINEAR)
        return RL_POST_SAMPLE_SHARP;
    if (f != RL_POST_FILTER_DEFAULT)
        return RL_POST_SAMPLE_NEAREST;
    return dst_w > 2 * src_w && dst_h > 2 * src_h ? RL_POST_SAMPLE_SHARP : RL_POST_SAMPLE_LINEAR;
}

void rl_post_set_upscaler(const rl_post_stage *stage, void *user)
{
    pthread_mutex_lock(&g_lock);
    __atomic_store_n(&g_up, stage, __ATOMIC_RELEASE);
    g_up_user = user;
    __atomic_add_fetch(&g_up_gen, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lock);
}

const uint8_t *rl_post_apply(const uint8_t *rgb, uint32_t width, uint32_t height, uint32_t *out_width,
                             uint32_t *out_height)
{
    rl_settings_declare_runtime();
    *out_width = width;
    *out_height = height;
    uint32_t aa = rl_setting_enum("picture.aa");
    pthread_mutex_lock(&g_lock);
    void *user = NULL;
    const rl_post_stage *stage = stage_now(&g_vk, &user);
    if ((aa == RL_POST_AA_OFF && !stage) || !width || !height) {
        pthread_mutex_unlock(&g_lock);
        return rgb;
    }
    const uint8_t *shown = rgb;
    if (!g_vk.tried) {
        g_vk.tried = true;
        g_vk.ok = start(&g_vk);
    }
    uint32_t w = width, h = height;
    bool up = g_vk.ok && upscaler_ready(&g_vk, stage, user) && g_vk.cur->plan(g_vk.cur_user, width, height, &w, &h) &&
              w && h;
    if (!up) {
        w = width;
        h = height;
    }
    if (!up && aa == RL_POST_AA_OFF) {
        pthread_mutex_unlock(&g_lock);
        return rgb;
    }
    size_t bytes = (size_t)w * h * 3;
    if (g_vk.ok && bytes > g_out_cap) {
        uint8_t *p = realloc(g_out, bytes);
        if (p) {
            g_out = p;
            g_out_cap = bytes;
        } else {
            g_vk.ok = fail("allocation", 0);
        }
    }
    if (g_vk.ok)
        g_vk.ok = run(&g_vk, up, aa, rgb, g_out, width, height, w, h);
    if (g_vk.ok) {
        shown = g_out;
        *out_width = w;
        *out_height = h;
    }
    pthread_mutex_unlock(&g_lock);
    return shown;
}

rl_post_gpu *rl_post_gpu_new(void)
{
    vk *v = calloc(1, sizeof *v);
    if (v && !start(v)) {
        rl_post_gpu_free(v);
        return NULL;
    }
    return v;
}

void rl_post_gpu_free(rl_post_gpu *v)
{
    if (!v)
        return;
    const rlvk_dev *d = v->d;
    if (d) {
        rl_vkdev_wait_idle(d);
        rl_upscaler_free(v->own_u);
        for (int i = 0; i < B_COUNT; i++)
            rl_vkdev_buf_free(d, &v->b[i]);
        for (int i = 0; i < P_COUNT; i++)
            if (v->pipe[i])
                d->vkDestroyPipeline(d->device, v->pipe[i], NULL);
        if (v->dpool)
            d->vkDestroyDescriptorPool(d->device, v->dpool, NULL);
        if (v->layout)
            d->vkDestroyPipelineLayout(d->device, v->layout, NULL);
        if (v->set_layout)
            d->vkDestroyDescriptorSetLayout(d->device, v->set_layout, NULL);
        if (v->fence)
            d->vkDestroyFence(d->device, v->fence, NULL);
        if (v->pool)
            d->vkDestroyCommandPool(d->device, v->pool, NULL);
    }
    free(v);
}

bool rl_post_gpu_wanted(void)
{
    rl_settings_declare_runtime();
    return rl_setting_enum("picture.aa") != RL_POST_AA_OFF || __atomic_load_n(&g_up, __ATOMIC_ACQUIRE) ||
           (!rl_upscale_window_off() && rl_upscale_spatial(rl_setting_enum("picture.upscaler")) != RL_UPSCALE_OFF);
}

bool rl_post_gpu_plan(rl_post_gpu *v, uint32_t width, uint32_t height, uint32_t *out_width, uint32_t *out_height)
{
    rl_settings_declare_runtime();
    *out_width = width;
    *out_height = height;
    uint32_t aa = rl_setting_enum("picture.aa");
    void *user = NULL;
    const rl_post_stage *stage = stage_now(v, &user);
    if ((aa == RL_POST_AA_OFF && !stage) || !width || !height)
        return false;
    uint32_t w = width, h = height;
    bool up = upscaler_ready(v, stage, user) && v->cur->plan(v->cur_user, width, height, &w, &h) && w && h;
    if (!up) {
        w = width;
        h = height;
    }
    if (!up && aa == RL_POST_AA_OFF)
        return false;
    v->plan_up = up;
    v->plan_aa = aa;
    *out_width = w;
    *out_height = h;
    return true;
}

bool rl_post_gpu_record(rl_post_gpu *v, uint64_t cmd, uint64_t src, uint32_t sw, uint32_t sh, uint64_t dst, uint32_t w,
                        uint32_t h)
{
    if (!size_buffers(v, sw, sh, w, h, false))
        return false;
    const rlvk_dev *d = v->d;
    rlvk_handle own = v->cmd;
    v->cmd = cmd;
    rlvk_buffer_image_copy in = {
        .imageSubresource = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { sw, sh, 1 },
    };
    d->vkCmdCopyImageToBuffer(cmd, src, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, v->b[v->plan_up ? B_SRC : B_IN].buffer,
                              1, &in);
    record_passes(v, v->plan_up, v->plan_aa, sw, sh, w, h);
    rlvk_buffer_image_copy out = {
        .imageSubresource = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { w, h, 1 },
    };
    d->vkCmdCopyBufferToImage(cmd, v->b[v->plan_aa != RL_POST_AA_OFF ? B_OUT : B_IN].buffer, dst,
                              RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &out);
    v->cmd = own;
    return true;
}
