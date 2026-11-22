/**
 * @file upscale.c
 * @brief The upscaler stage (upscale.h): relift's spatial choices as one compute pass (shaders/upscale.comp, its
 * SPIR-V in post_data.h), on the device and command buffer the caller gives.
 *
 * Each record takes one descriptor set from a fixed ring; rl_upscaler_retire, called once the GPU has finished the
 * records, makes the whole ring free again. The stage loads its own device functions through the caller's
 * vkGetDeviceProcAddr, so it works on the window pipeline's device and on a guest layer's alike.
 */
#include <relift/upscale.h>

#include "core.h"
#include "fsr2.h"
#include "fsr3.h"
#include "hostlib.h"
#include "post_data.h"
#include "vkmin.h"

#include <stdlib.h>
#include <string.h>

#define GROUP 8u
#define SETS 64u
#define LANCZOS_LOBES 3

/** @brief The device functions the stage calls, as X(name, return type, parameters). */
#define UPSCALE_FNS(X)                                                                                                 \
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
    X(vkAllocateDescriptorSets, rlvk_result, (rlvk_handle, const rlvk_descriptor_set_allocate_info *, rlvk_handle *))  \
    X(vkUpdateDescriptorSets, void,                                                                                   \
      (rlvk_handle, uint32_t, const rlvk_write_descriptor_set *, uint32_t, const void *))                             \
    X(vkCreateSampler, rlvk_result, (rlvk_handle, const rlvk_sampler_create_info *, const void *, rlvk_handle *))     \
    X(vkDestroySampler, void, (rlvk_handle, rlvk_handle, const void *))                                               \
    X(vkCmdBindPipeline, void, (rlvk_handle, int32_t, rlvk_handle))                                                   \
    X(vkCmdBindDescriptorSets, void,                                                                                  \
      (rlvk_handle, int32_t, rlvk_handle, uint32_t, uint32_t, const rlvk_handle *, uint32_t, const uint32_t *))       \
    X(vkCmdPushConstants, void, (rlvk_handle, rlvk_handle, rlvk_flags, uint32_t, uint32_t, const void *))             \
    X(vkCmdDispatch, void, (rlvk_handle, uint32_t, uint32_t, uint32_t))                                               \
    X(vkCmdPipelineBarrier, void,                                                                                     \
      (rlvk_handle, rlvk_flags, rlvk_flags, rlvk_flags, uint32_t, const rlvk_memory_barrier *, uint32_t, const void *, \
       uint32_t, const void *))                                                                                       \
    X(vkCreateBuffer, rlvk_result, (rlvk_handle, const rlvk_buffer_create_info *, const void *, rlvk_handle *))       \
    X(vkDestroyBuffer, void, (rlvk_handle, rlvk_handle, const void *))                                                \
    X(vkGetBufferMemoryRequirements, void, (rlvk_handle, rlvk_handle, rlvk_memory_requirements *))                    \
    X(vkAllocateMemory, rlvk_result, (rlvk_handle, const rlvk_memory_allocate_info *, const void *, rlvk_handle *))   \
    X(vkFreeMemory, void, (rlvk_handle, rlvk_handle, const void *))                                                   \
    X(vkBindBufferMemory, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_handle, rlvk_size))

/** @brief The push constants of upscale.comp (Params). */
typedef struct params {
    int32_t src[4], dst[4];
    int32_t src_pitch[2], dst_pitch[2];
    int32_t mode, lobes;
    float step[2], ratio[2];
} params;

/** @brief The push constants of fsr1.comp (Params). */
typedef struct fsr_params {
    int32_t src[4], dst[4];
    int32_t src_pitch[2], dst_pitch[2];
    int32_t pass;
    float stops;
} fsr_params;

/** @brief upscale.comp's modes. */
enum { MODE_COPY, MODE_NEAREST, MODE_BILINEAR, MODE_SHARP_BILINEAR, MODE_LANCZOS };

/** @brief RCAS's sharpness in stops below its strongest when the inputs ask for none: FidelityFX's sample default. */
#define FSR1_DEFAULT_STOPS 0.25f

/** @brief A device buffer and its memory. */
typedef struct work_buf {
    rlvk_handle buffer, memory;
    rlvk_size size;
} work_buf;

struct rl_upscaler {
#define UPSCALE_FN_FIELD(name, ret, args) ret(RLVK_CALL *name) args;
    UPSCALE_FNS(UPSCALE_FN_FIELD)
#undef UPSCALE_FN_FIELD
    rlvk_handle device, set_layout, layout, pipe, pool, sampler;
    rlvk_handle sets[SETS];
    rlvk_memory_properties mem;
    /** @brief FSR 1's buffer between its passes, and the one it replaced while records may still use it. */
    work_buf work, old;
    /** @brief FSR 2, for the FSR 2 choice on images; frames without depth and motion vectors run FSR 1. */
    rl_fsr2 *fsr2;
    /** @brief The FSR 3 upscaler, for its choice on images; frames without depth and motion vectors run FSR 1. */
    rl_fsr3 *fsr3;
    uint32_t used, choice, kind;
};

static bool is_fsr1(const rl_upscaler *u)
{
    return u->choice == RL_UPSCALE_FSR1 || u->choice == RL_UPSCALE_FSR2 || u->choice == RL_UPSCALE_FSR3;
}

static const char *const g_names[RL_UPSCALE_CHOICES] = {
    "auto", "game", "off", "nearest", "bilinear", "sharp_bilinear", "lanczos", "fsr1", "fsr2", "fsr3",
};

const char *rl_upscale_name(uint32_t choice) { return choice < RL_UPSCALE_CHOICES ? g_names[choice] : "?"; }

bool rl_upscale_temporal(uint32_t choice) { return choice == RL_UPSCALE_FSR2 || choice == RL_UPSCALE_FSR3; }

uint32_t rl_upscale_spatial(uint32_t choice)
{
    if (rl_upscale_temporal(choice))
        return RL_UPSCALE_FSR1;
    return choice <= RL_UPSCALE_GAME || choice >= RL_UPSCALE_CHOICES ? RL_UPSCALE_OFF : choice;
}

static bool g_in_frame;

void rl_upscale_in_frame(void) { __atomic_store_n(&g_in_frame, true, __ATOMIC_RELEASE); }

bool rl_upscale_window_off(void) { return __atomic_load_n(&g_in_frame, __ATOMIC_ACQUIRE); }

uint32_t rl_upscale_resolve(uint32_t choice, bool in_frame, bool game_ok)
{
    if (!in_frame)
        return rl_upscale_spatial(choice);
    if (choice == RL_UPSCALE_AUTO)
        return game_ok ? RL_UPSCALE_GAME : RL_UPSCALE_BEST_TEMPORAL;
    return choice < RL_UPSCALE_CHOICES ? choice : RL_UPSCALE_BILINEAR;
}

void rl_upscale_steps(const rl_upscale_rect *in, const rl_upscale_rect *out, float step[2], float ratio[2])
{
    step[0] = (float)in->width / (float)out->width;
    step[1] = (float)in->height / (float)out->height;
    ratio[0] = (float)out->width / (float)in->width;
    ratio[1] = (float)out->height / (float)in->height;
}

void rl_upscale_map(const rl_upscale_rect *in, const rl_upscale_rect *out, uint32_t x, uint32_t y, float src[2])
{
    float step[2], ratio[2];
    rl_upscale_steps(in, out, step, ratio);
    src[0] = ((float)x + 0.5f) * step[0] + (float)in->x;
    src[1] = ((float)y + 0.5f) * step[1] + (float)in->y;
}

static bool failed(rl_upscaler *u, const char *what, rlvk_result r)
{
    rl_log(RL_LOG_WARN, "upscale", "the %s upscaler did not start: %s failed (VkResult %d)", rl_upscale_name(u->choice),
           what, (int)r);
    rl_upscaler_free(u);
    return false;
}

static bool make(rl_upscaler *u)
{
    bool buffers = u->kind == RL_UPSCALE_BUFFERS, fsr = is_fsr1(u);
    int32_t in_type = buffers ? RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER : RLVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    int32_t out_type = buffers ? RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER : RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    rlvk_descriptor_set_layout_binding bind[3] = {
        { .binding = 0, .descriptorType = in_type, .descriptorCount = 1, .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = fsr ? 2 : 1,
          .descriptorType = out_type,
          .descriptorCount = 1,
          .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1,
          .descriptorType = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
          .descriptorCount = 1,
          .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT },
    };
    rlvk_descriptor_set_layout_create_info sl = {
        .sType = RLVK_ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = fsr ? 3 : 2, .pBindings = bind
    };
    rlvk_result r = u->vkCreateDescriptorSetLayout(u->device, &sl, NULL, &u->set_layout);
    if (r != RLVK_SUCCESS)
        return failed(u, "vkCreateDescriptorSetLayout", r);
    rlvk_push_constant_range pcr = { .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT,
                                     .size = fsr ? sizeof(fsr_params) : sizeof(params) };
    rlvk_pipeline_layout_create_info pl = { .sType = RLVK_ST_PIPELINE_LAYOUT_CREATE_INFO,
                                            .setLayoutCount = 1,
                                            .pSetLayouts = &u->set_layout,
                                            .pushConstantRangeCount = 1,
                                            .pPushConstantRanges = &pcr };
    r = u->vkCreatePipelineLayout(u->device, &pl, NULL, &u->layout);
    if (r != RLVK_SUCCESS)
        return failed(u, "vkCreatePipelineLayout", r);
    rlvk_shader_module_create_info sm = {
        .sType = RLVK_ST_SHADER_MODULE_CREATE_INFO,
        .codeSize = fsr ? (buffers ? sizeof rl_post_spv_fsr1_buffers : sizeof rl_post_spv_fsr1)
                        : (buffers ? sizeof rl_post_spv_upscale_buffers : sizeof rl_post_spv_upscale),
        .pCode = fsr ? (buffers ? rl_post_spv_fsr1_buffers : rl_post_spv_fsr1)
                     : (buffers ? rl_post_spv_upscale_buffers : rl_post_spv_upscale),
    };
    rlvk_handle module = 0;
    r = u->vkCreateShaderModule(u->device, &sm, NULL, &module);
    if (r != RLVK_SUCCESS)
        return failed(u, "vkCreateShaderModule", r);
    rlvk_compute_pipeline_create_info cp = {
        .sType = RLVK_ST_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = RLVK_ST_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = RLVK_SHADER_STAGE_COMPUTE_BIT,
                   .module = module,
                   .pName = "main" },
        .layout = u->layout,
        .basePipelineIndex = -1,
    };
    r = u->vkCreateComputePipelines(u->device, 0, 1, &cp, NULL, &u->pipe);
    u->vkDestroyShaderModule(u->device, module, NULL);
    if (r != RLVK_SUCCESS)
        return failed(u, "vkCreateComputePipelines", r);
    rlvk_descriptor_pool_size ps[3] = { { in_type, SETS }, { out_type, SETS }, { RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SETS } };
    rlvk_descriptor_pool_create_info dp = {
        .sType = RLVK_ST_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = SETS, .poolSizeCount = fsr ? 3 : 2, .pPoolSizes = ps
    };
    r = u->vkCreateDescriptorPool(u->device, &dp, NULL, &u->pool);
    if (r != RLVK_SUCCESS)
        return failed(u, "vkCreateDescriptorPool", r);
    rlvk_handle layouts[SETS];
    for (uint32_t i = 0; i < SETS; i++)
        layouts[i] = u->set_layout;
    rlvk_descriptor_set_allocate_info da = {
        .sType = RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = u->pool,
        .descriptorSetCount = SETS,
        .pSetLayouts = layouts,
    };
    r = u->vkAllocateDescriptorSets(u->device, &da, u->sets);
    if (r != RLVK_SUCCESS)
        return failed(u, "vkAllocateDescriptorSets", r);
    if (!buffers) {
        rlvk_sampler_create_info si = {
            .sType = RLVK_ST_SAMPLER_CREATE_INFO,
            .magFilter = RLVK_FILTER_NEAREST,
            .minFilter = RLVK_FILTER_NEAREST,
            .addressModeU = RLVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeV = RLVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .addressModeW = RLVK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
            .maxAnisotropy = 1.0f,
        };
        r = u->vkCreateSampler(u->device, &si, NULL, &u->sampler);
        if (r != RLVK_SUCCESS)
            return failed(u, "vkCreateSampler", r);
    }
    return true;
}

rl_upscaler *rl_upscaler_new(const rl_post_device *device, uint32_t choice, uint32_t kind)
{
    if (choice <= RL_UPSCALE_GAME || choice >= RL_UPSCALE_CHOICES || kind > RL_UPSCALE_BUFFERS || !device ||
        !device->device || !device->get_device_proc_addr) {
        rl_log(RL_LOG_WARN, "upscale", "the %s upscaler is not one relift runs", rl_upscale_name(choice));
        return NULL;
    }
    rl_upscaler *u = calloc(1, sizeof *u);
    if (!u)
        return NULL;
    u->device = device->device;
    u->choice = choice;
    u->kind = kind;
    void *(RLVK_CALL *gdpa)(rlvk_handle, const char *);
    memcpy(&gdpa, &device->get_device_proc_addr, sizeof gdpa);
#define UPSCALE_LOAD(name, ret, args)                                                                                  \
    u->name = (ret(RLVK_CALL *) args)gdpa(u->device, #name);                                                         \
    if (!u->name) {                                                                                                    \
        failed(u, #name " lookup", 0);                                                                                 \
        return NULL;                                                                                                   \
    }
    UPSCALE_FNS(UPSCALE_LOAD)
#undef UPSCALE_LOAD
    if (rl_upscale_temporal(choice) && kind == RL_UPSCALE_BUFFERS)
        u->choice = RL_UPSCALE_FSR1;
    if (u->choice == RL_UPSCALE_FSR2 && !(u->fsr2 = rl_fsr2_new(device)))
        rl_log(RL_LOG_WARN, "upscale", "the fsr2 upscaler runs FSR 1 instead");
    if (u->choice == RL_UPSCALE_FSR3 && !(u->fsr3 = rl_fsr3_new(device)))
        rl_log(RL_LOG_WARN, "upscale", "the fsr3 upscaler runs FSR 1 instead");
    if (is_fsr1(u)) {
        void *(RLVK_CALL *gipa)(rlvk_handle, const char *);
        void *raw = rl_hostlib_vulkan_gipa();
        memcpy(&gipa, &raw, sizeof gipa);
        void (RLVK_CALL *props)(rlvk_handle, rlvk_memory_properties *);
        void *fp = raw ? gipa(device->instance, "vkGetPhysicalDeviceMemoryProperties") : NULL;
        memcpy(&props, &fp, sizeof props);
        if (!fp) {
            failed(u, "vkGetPhysicalDeviceMemoryProperties lookup", 0);
            return NULL;
        }
        props(device->physical_device, &u->mem);
    }
    return make(u) ? u : NULL;
}

static void buf_drop(rl_upscaler *u, work_buf *b)
{
    if (b->buffer)
        u->vkDestroyBuffer(u->device, b->buffer, NULL);
    if (b->memory)
        u->vkFreeMemory(u->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

/** @brief Makes FSR 1's work buffer hold size bytes; false when it cannot (or the one it replaced is still in use). */
static bool work_fits(rl_upscaler *u, rlvk_size size)
{
    if (u->work.size >= size)
        return true;
    if (u->old.buffer)
        return false;
    work_buf b = { 0 };
    rlvk_buffer_create_info bi = {
        .sType = RLVK_ST_BUFFER_CREATE_INFO, .size = size, .usage = RLVK_BUFFER_USAGE_STORAGE_BUFFER_BIT
    };
    if (u->vkCreateBuffer(u->device, &bi, NULL, &b.buffer) != RLVK_SUCCESS)
        return false;
    rlvk_memory_requirements req;
    u->vkGetBufferMemoryRequirements(u->device, b.buffer, &req);
    int32_t type = -1;
    for (uint32_t pass = 0; pass < 2 && type < 0; pass++)
        for (uint32_t t = 0; t < u->mem.memoryTypeCount && type < 0; t++)
            if ((req.memoryTypeBits & (1u << t)) &&
                (pass || (u->mem.memoryTypes[t].propertyFlags & RLVK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
                type = (int32_t)t;
    rlvk_memory_allocate_info ai = {
        .sType = RLVK_ST_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = (uint32_t)type
    };
    if (type < 0 || u->vkAllocateMemory(u->device, &ai, NULL, &b.memory) != RLVK_SUCCESS ||
        u->vkBindBufferMemory(u->device, b.buffer, b.memory, 0) != RLVK_SUCCESS) {
        buf_drop(u, &b);
        return false;
    }
    b.size = size;
    if (u->used)
        u->old = u->work;
    else
        buf_drop(u, &u->work);
    u->work = b;
    return true;
}

static int32_t mode_of(uint32_t choice)
{
    switch (choice) {
    case RL_UPSCALE_OFF:
        return MODE_COPY;
    case RL_UPSCALE_NEAREST:
        return MODE_NEAREST;
    case RL_UPSCALE_BILINEAR:
        return MODE_BILINEAR;
    case RL_UPSCALE_SHARP_BILINEAR:
        return MODE_SHARP_BILINEAR;
    default:
        return MODE_LANCZOS;
    }
}

static bool rect_inside(const rl_upscale_rect *r, const rl_upscale_image *im)
{
    return r->width && r->height && r->x <= im->width && r->y <= im->height && r->width <= im->width - r->x &&
           r->height <= im->height - r->y;
}

bool rl_upscaler_record(rl_upscaler *u, uint64_t cmd, const rl_upscale_inputs *in)
{
    if (!u || !cmd || !in || !in->color.handle || !in->out.handle || !rect_inside(&in->in, &in->color) ||
        !rect_inside(&in->out_rect, &in->out) || u->used == SETS)
        return false;
    if (u->fsr2 && rl_fsr2_takes(in))
        return rl_fsr2_record(u->fsr2, cmd, in);
    if (u->fsr3 && rl_fsr3_takes(in))
        return rl_fsr3_record(u->fsr3, cmd, in);
    bool fsr = is_fsr1(u);
    if (fsr && !work_fits(u, (rlvk_size)in->out_rect.width * in->out_rect.height * 8))
        return false;
    rlvk_handle set = u->sets[u->used++];
    rlvk_write_descriptor_set wr[3];
    rlvk_descriptor_buffer_info bi[3];
    rlvk_descriptor_image_info ii[2];
    for (uint32_t i = 0; i < 2; i++) {
        uint64_t h = i ? in->out.handle : in->color.handle;
        wr[i] = (rlvk_write_descriptor_set){ .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = i ? (fsr ? 2 : 1) : 0,
                                             .descriptorCount = 1 };
        if (u->kind == RL_UPSCALE_BUFFERS) {
            bi[i] = (rlvk_descriptor_buffer_info){ .buffer = h, .range = ~(rlvk_size)0 };
            wr[i].descriptorType = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            wr[i].pBufferInfo = &bi[i];
        } else {
            ii[i] = (rlvk_descriptor_image_info){ .sampler = i ? 0 : u->sampler,
                                                   .imageView = h,
                                                   .imageLayout = RLVK_IMAGE_LAYOUT_GENERAL };
            wr[i].descriptorType = i ? RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE : RLVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            wr[i].pImageInfo = &ii[i];
        }
    }
    if (fsr) {
        bi[2] = (rlvk_descriptor_buffer_info){ .buffer = u->work.buffer, .range = ~(rlvk_size)0 };
        wr[2] = (rlvk_write_descriptor_set){ .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = 1,
                                             .descriptorCount = 1,
                                             .descriptorType = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                             .pBufferInfo = &bi[2] };
    }
    u->vkUpdateDescriptorSets(u->device, fsr ? 3 : 2, wr, 0, NULL);
    int32_t src[4] = { (int32_t)in->in.x, (int32_t)in->in.y, (int32_t)in->in.width, (int32_t)in->in.height };
    int32_t dst[4] = { (int32_t)in->out_rect.x, (int32_t)in->out_rect.y, (int32_t)in->out_rect.width,
                       (int32_t)in->out_rect.height };
    int32_t pitch[4] = { (int32_t)in->color.width, (int32_t)in->color.height, (int32_t)in->out.width,
                         (int32_t)in->out.height };
    uint32_t gx = (in->out_rect.width + GROUP - 1) / GROUP, gy = (in->out_rect.height + GROUP - 1) / GROUP;
    u->vkCmdBindPipeline(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, u->pipe);
    u->vkCmdBindDescriptorSets(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, u->layout, 0, 1, &set, 0, NULL);
    if (!fsr) {
        params pc = { .mode = mode_of(u->choice), .lobes = LANCZOS_LOBES };
        rl_upscale_steps(&in->in, &in->out_rect, pc.step, pc.ratio);
        memcpy(pc.src, src, sizeof src);
        memcpy(pc.dst, dst, sizeof dst);
        memcpy(pc.src_pitch, pitch, sizeof pc.src_pitch);
        memcpy(pc.dst_pitch, pitch + 2, sizeof pc.dst_pitch);
        u->vkCmdPushConstants(cmd, u->layout, RLVK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        u->vkCmdDispatch(cmd, gx, gy, 1);
        return true;
    }
    fsr_params pc = { .stops = in->sharpness > 0.0f ? 2.0f * (1.0f - (in->sharpness < 1.0f ? in->sharpness : 1.0f))
                                                    : FSR1_DEFAULT_STOPS };
    memcpy(pc.src, src, sizeof src);
    memcpy(pc.dst, dst, sizeof dst);
    memcpy(pc.src_pitch, pitch, sizeof pc.src_pitch);
    memcpy(pc.dst_pitch, pitch + 2, sizeof pc.dst_pitch);
    for (int32_t pass = 0; pass < 2; pass++) {
        pc.pass = pass;
        u->vkCmdPushConstants(cmd, u->layout, RLVK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
        u->vkCmdDispatch(cmd, gx, gy, 1);
        if (!pass) {
            rlvk_memory_barrier mb = { .sType = RLVK_ST_MEMORY_BARRIER,
                                       .srcAccessMask = RLVK_ACCESS_SHADER_WRITE_BIT,
                                       .dstAccessMask = RLVK_ACCESS_SHADER_READ_BIT };
            u->vkCmdPipelineBarrier(cmd, RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    0, 1, &mb, 0, NULL, 0, NULL);
        }
    }
    return true;
}

void rl_upscaler_retire(rl_upscaler *u)
{
    if (!u)
        return;
    u->used = 0;
    buf_drop(u, &u->old);
    rl_fsr2_retire(u->fsr2);
    rl_fsr3_retire(u->fsr3);
}

void rl_upscaler_free(rl_upscaler *u)
{
    if (!u)
        return;
    rl_fsr2_free(u->fsr2);
    rl_fsr3_free(u->fsr3);
    buf_drop(u, &u->old);
    buf_drop(u, &u->work);
    if (u->sampler)
        u->vkDestroySampler(u->device, u->sampler, NULL);
    if (u->pool)
        u->vkDestroyDescriptorPool(u->device, u->pool, NULL);
    if (u->pipe)
        u->vkDestroyPipeline(u->device, u->pipe, NULL);
    if (u->layout)
        u->vkDestroyPipelineLayout(u->device, u->layout, NULL);
    if (u->set_layout)
        u->vkDestroyDescriptorSetLayout(u->device, u->set_layout, NULL);
    free(u);
}
