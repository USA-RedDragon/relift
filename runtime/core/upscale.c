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
    X(vkCmdDispatch, void, (rlvk_handle, uint32_t, uint32_t, uint32_t))

/** @brief The push constants of upscale.comp (Params). */
typedef struct params {
    int32_t src[4], dst[4];
    int32_t src_pitch[2], dst_pitch[2];
    int32_t mode, lobes;
    float step[2], ratio[2];
} params;

/** @brief upscale.comp's modes. */
enum { MODE_COPY, MODE_NEAREST, MODE_BILINEAR, MODE_SHARP_BILINEAR, MODE_LANCZOS };

struct rl_upscaler {
#define UPSCALE_FN_FIELD(name, ret, args) ret(RLVK_CALL *name) args;
    UPSCALE_FNS(UPSCALE_FN_FIELD)
#undef UPSCALE_FN_FIELD
    rlvk_handle device, set_layout, layout, pipe, pool, sampler;
    rlvk_handle sets[SETS];
    uint32_t used, choice, kind;
};

static const char *const g_names[RL_UPSCALE_CHOICES] = {
    "auto", "game", "off", "nearest", "bilinear", "sharp_bilinear", "lanczos",
};

const char *rl_upscale_name(uint32_t choice) { return choice < RL_UPSCALE_CHOICES ? g_names[choice] : "?"; }

uint32_t rl_upscale_spatial(uint32_t choice)
{
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
        return game_ok ? RL_UPSCALE_GAME : RL_UPSCALE_BILINEAR;
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
    bool buffers = u->kind == RL_UPSCALE_BUFFERS;
    rlvk_descriptor_set_layout_binding bind[2] = {
        { .binding = 0,
          .descriptorType = buffers ? RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER : RLVK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
          .descriptorCount = 1,
          .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT },
        { .binding = 1,
          .descriptorType = buffers ? RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER : RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
          .descriptorCount = 1,
          .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT },
    };
    rlvk_descriptor_set_layout_create_info sl = {
        .sType = RLVK_ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 2, .pBindings = bind
    };
    rlvk_result r = u->vkCreateDescriptorSetLayout(u->device, &sl, NULL, &u->set_layout);
    if (r != RLVK_SUCCESS)
        return failed(u, "vkCreateDescriptorSetLayout", r);
    rlvk_push_constant_range pcr = { .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(params) };
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
        .codeSize = buffers ? sizeof rl_post_spv_upscale_buffers : sizeof rl_post_spv_upscale,
        .pCode = buffers ? rl_post_spv_upscale_buffers : rl_post_spv_upscale,
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
    rlvk_descriptor_pool_size ps[2] = { { bind[0].descriptorType, SETS }, { bind[1].descriptorType, SETS } };
    rlvk_descriptor_pool_create_info dp = {
        .sType = RLVK_ST_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = SETS, .poolSizeCount = 2, .pPoolSizes = ps
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
    return make(u) ? u : NULL;
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
    rlvk_handle set = u->sets[u->used++];
    rlvk_write_descriptor_set wr[2];
    rlvk_descriptor_buffer_info bi[2];
    rlvk_descriptor_image_info ii[2];
    for (uint32_t i = 0; i < 2; i++) {
        uint64_t h = i ? in->out.handle : in->color.handle;
        wr[i] = (rlvk_write_descriptor_set){ .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = i,
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
    u->vkUpdateDescriptorSets(u->device, 2, wr, 0, NULL);
    params pc = {
        .src = { (int32_t)in->in.x, (int32_t)in->in.y, (int32_t)in->in.width, (int32_t)in->in.height },
        .dst = { (int32_t)in->out_rect.x, (int32_t)in->out_rect.y, (int32_t)in->out_rect.width,
                 (int32_t)in->out_rect.height },
        .src_pitch = { (int32_t)in->color.width, (int32_t)in->color.height },
        .dst_pitch = { (int32_t)in->out.width, (int32_t)in->out.height },
        .mode = mode_of(u->choice),
        .lobes = LANCZOS_LOBES,
    };
    rl_upscale_steps(&in->in, &in->out_rect, pc.step, pc.ratio);
    u->vkCmdBindPipeline(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, u->pipe);
    u->vkCmdBindDescriptorSets(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, u->layout, 0, 1, &set, 0, NULL);
    u->vkCmdPushConstants(cmd, u->layout, RLVK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    u->vkCmdDispatch(cmd, (in->out_rect.width + GROUP - 1) / GROUP, (in->out_rect.height + GROUP - 1) / GROUP, 1);
    return true;
}

void rl_upscaler_retire(rl_upscaler *u)
{
    if (u)
        u->used = 0;
}

void rl_upscaler_free(rl_upscaler *u)
{
    if (!u)
        return;
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
