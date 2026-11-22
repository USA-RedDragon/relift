/**
 * @file taa.c
 * @brief relift's plain temporal upscaler (shaders/taa.comp) for the upscaler stage: one pass per frame over the
 * display, a history image in and one out, swapped each frame, then AMD's RCAS sharpening (shaders/taa_rcas.comp)
 * over the new history at the strength picture.taa_sharpness gives (none at 0).
 */
#include "taa.h"

#include "core.h"
#include "ffx.h"
#include "post_data.h"

#include <relift/settings.h>

#include <stdlib.h>
#include <string.h>

#define RECORDS 32u
#define CB_STRIDE 256u
#define MIN_ALPHA 0.08f
#define VARIANCE_GAMMA 1.25f

/** @brief taa.comp's Params, std140. */
typedef struct cb_taa {
    int32_t render[2], display[2];
    float jitter[2], motion_scale[2], motion_jitter[2];
    float min_alpha, variance_gamma;
    int32_t flags;
    float stops;
} cb_taa;

enum { F_RESET = 1, F_HDR = 2, F_INVERTED = 4, F_MOTION_DISPLAY = 8 };

/** @brief taa_rcas.comp's descriptors: the history TAA wrote, the output, the parameters. */
static const rl_ffx_bind g_rcas_binds[] = {
    { 0, 3, RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
    { 0, 5, RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE },
    { 0, 7, RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
};

static const rl_ffx_bind g_binds[] = {
    { 0, 0, RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE }, { 0, 1, RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
    { 0, 2, RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE }, { 0, 3, RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
    { 0, 4, RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE }, { 0, 5, RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE },
    { 0, 6, RLVK_DESCRIPTOR_TYPE_SAMPLER },       { 0, 7, RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
};

struct rl_taa {
    rl_ffx_vk vk;
    rl_ffx_pipeline pipe, rcas;
    rlvk_handle sampler, pool, cb, cb_memory;
    rl_ffx_image history[2];
    uint32_t display[2], used, parity;
    bool fresh, first, pending;
    float previous_jitter[2];
};

static bool failed(rl_taa *t, const char *what, rlvk_result r)
{
    rl_log(RL_LOG_WARN, "upscale", "TAA did not start: %s failed (VkResult %d)", what, (int)r);
    rl_taa_free(t);
    return false;
}

rl_taa *rl_taa_new(const rl_post_device *d)
{
    if (!d || !d->device || !d->get_device_proc_addr)
        return NULL;
    rl_taa *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    if (!rl_ffx_vk_load(&t->vk, d, "TAA")) {
        rl_taa_free(t);
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
    };
    rlvk_result r = t->vk.vkCreateSampler(t->vk.device, &si, NULL, &t->sampler);
    if (r != RLVK_SUCCESS)
        return failed(t, "vkCreateSampler", r), NULL;
    if (!rl_ffx_pipeline_new(&t->vk, rl_post_spv_taa, sizeof rl_post_spv_taa, g_binds,
                             sizeof g_binds / sizeof g_binds[0], 0, NULL, 0, t->sampler, &t->pipe))
        return failed(t, "the pipeline", 0), NULL;
    if (!rl_ffx_pipeline_new(&t->vk, rl_post_spv_taa_rcas, sizeof rl_post_spv_taa_rcas, g_rcas_binds,
                             sizeof g_rcas_binds / sizeof g_rcas_binds[0], 0, NULL, 0, 0, &t->rcas))
        return failed(t, "the sharpening pipeline", 0), NULL;
    rlvk_descriptor_pool_size ps[4] = {
        { RLVK_DESCRIPTOR_TYPE_SAMPLER, RECORDS },
        { RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RECORDS * 5 },
        { RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE, RECORDS * 3 },
        { RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, RECORDS * 2 },
    };
    rlvk_descriptor_pool_create_info dp = {
        .sType = RLVK_ST_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = RECORDS * 2, .poolSizeCount = 4, .pPoolSizes = ps
    };
    if ((r = t->vk.vkCreateDescriptorPool(t->vk.device, &dp, NULL, &t->pool)) != RLVK_SUCCESS)
        return failed(t, "vkCreateDescriptorPool", r), NULL;
    rlvk_buffer_create_info bi = { .sType = RLVK_ST_BUFFER_CREATE_INFO,
                                   .size = CB_STRIDE * RECORDS,
                                   .usage = RLVK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | RLVK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if ((r = t->vk.vkCreateBuffer(t->vk.device, &bi, NULL, &t->cb)) != RLVK_SUCCESS)
        return failed(t, "vkCreateBuffer", r), NULL;
    rlvk_memory_requirements req;
    t->vk.vkGetBufferMemoryRequirements(t->vk.device, t->cb, &req);
    if (!rl_ffx_bind_memory(&t->vk, &req, &t->cb_memory) ||
        t->vk.vkBindBufferMemory(t->vk.device, t->cb, t->cb_memory, 0) != RLVK_SUCCESS)
        return failed(t, "the constant buffer's memory", 0), NULL;
    t->first = true;
    return t;
}

bool rl_taa_takes(const rl_upscale_inputs *in)
{
    return in->depth.handle && in->motion.handle && !in->in.x && !in->in.y && !in->out_rect.x && !in->out_rect.y &&
           in->in.width <= in->out_rect.width && in->in.height <= in->out_rect.height;
}

static bool make_history(rl_taa *t, uint32_t w, uint32_t h)
{
    for (int i = 0; i < 2; i++)
        rl_ffx_image_free(&t->vk, &t->history[i]);
    t->display[0] = w;
    t->display[1] = h;
    for (int i = 0; i < 2; i++)
        if (!rl_ffx_image_new(&t->vk, RLVK_FORMAT_R16G16B16A16_SFLOAT, w, h, 1, &t->history[i]))
            return false;
    t->fresh = true;
    t->first = true;
    return true;
}

bool rl_taa_record(rl_taa *t, uint64_t cmd, const rl_upscale_inputs *in)
{
    if (!t || !cmd || !rl_taa_takes(in) || !in->color.handle || !in->out.handle || t->used == RECORDS)
        return false;
    uint32_t dw = in->out_rect.width, dh = in->out_rect.height;
    if (!t->history[0].image || t->display[0] != dw || t->display[1] != dh) {
        if (t->pending)
            return false;
        if (!make_history(t, dw, dh)) {
            for (int i = 0; i < 2; i++)
                rl_ffx_image_free(&t->vk, &t->history[i]);
            return false;
        }
    }
    double sharp = rl_setting_float("picture.taa_sharpness");
    bool sharpen = sharp > 0.0;
    rlvk_handle set = 0, rset = 0;
    rlvk_descriptor_set_allocate_info da = { .sType = RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = t->pool,
                                             .descriptorSetCount = 1,
                                             .pSetLayouts = &t->pipe.set_layout };
    if (t->vk.vkAllocateDescriptorSets(t->vk.device, &da, &set) != RLVK_SUCCESS)
        return false;
    da.pSetLayouts = &t->rcas.set_layout;
    if (sharpen && t->vk.vkAllocateDescriptorSets(t->vk.device, &da, &rset) != RLVK_SUCCESS)
        return false;
    bool odd = t->parity & 1;
    rlvk_handle views[6] = { in->color.handle, in->depth.handle, in->motion.handle, t->history[odd].view,
                             t->history[!odd].view, in->out.handle };
    rlvk_descriptor_image_info ii[6];
    rlvk_write_descriptor_set wr[7];
    for (uint32_t i = 0; i < 6; i++) {
        ii[i] = (rlvk_descriptor_image_info){ .imageView = views[i], .imageLayout = RLVK_IMAGE_LAYOUT_GENERAL };
        wr[i] = (rlvk_write_descriptor_set){ .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = i,
                                             .descriptorCount = 1,
                                             .descriptorType = (int32_t)g_binds[i].type,
                                             .pImageInfo = &ii[i] };
    }
    uint32_t off = t->used * CB_STRIDE;
    rlvk_descriptor_buffer_info bi = { .buffer = t->cb, .offset = off, .range = sizeof(cb_taa) };
    wr[6] = (rlvk_write_descriptor_set){ .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
                                         .dstSet = set,
                                         .dstBinding = 7,
                                         .descriptorCount = 1,
                                         .descriptorType = RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         .pBufferInfo = &bi };
    t->vk.vkUpdateDescriptorSets(t->vk.device, 7, wr, 0, NULL);
    if (sharpen) {
        rlvk_write_descriptor_set rw3[3] = { wr[4], wr[5], wr[6] };
        rlvk_descriptor_image_info hist = { .imageView = t->history[!odd].view,
                                            .imageLayout = RLVK_IMAGE_LAYOUT_GENERAL };
        rw3[0].dstBinding = 3;
        rw3[0].descriptorType = RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        rw3[0].pImageInfo = &hist;
        for (int i = 0; i < 3; i++)
            rw3[i].dstSet = rset;
        t->vk.vkUpdateDescriptorSets(t->vk.device, 3, rw3, 0, NULL);
    }
    t->used++;
    t->pending = true;
    bool reset = in->reset || t->first;
    float rw = (float)in->in.width, rh = (float)in->in.height;
    float tw = in->motion_display_size ? (float)dw : rw, th = in->motion_display_size ? (float)dh : rh;
    cb_taa c = {
        .render = { (int32_t)in->in.width, (int32_t)in->in.height },
        .display = { (int32_t)dw, (int32_t)dh },
        .jitter = { in->jitter[0], in->jitter[1] },
        .motion_scale = { in->motion_scale[0] / tw, in->motion_scale[1] / th },
        .min_alpha = MIN_ALPHA,
        .variance_gamma = VARIANCE_GAMMA,
        .flags = (reset ? F_RESET : 0) | (in->hdr ? F_HDR : 0) | (in->depth_inverted ? F_INVERTED : 0) |
                 (in->motion_display_size ? F_MOTION_DISPLAY : 0),
        .stops = 2.0f * (1.0f - (float)(sharp < 1.0 ? sharp : 1.0)),
    };
    if (in->motion_jittered && !reset) {
        c.motion_jitter[0] = (t->previous_jitter[0] - in->jitter[0]) / tw;
        c.motion_jitter[1] = (t->previous_jitter[1] - in->jitter[1]) / th;
    }
    t->previous_jitter[0] = in->jitter[0];
    t->previous_jitter[1] = in->jitter[1];
    const uint32_t cs = RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, xfer = RLVK_PIPELINE_STAGE_TRANSFER_BIT;
    const uint32_t rw_access = RLVK_ACCESS_SHADER_READ_BIT | RLVK_ACCESS_SHADER_WRITE_BIT;
    if (t->fresh) {
        rlvk_image_memory_barrier ib[2];
        for (int i = 0; i < 2; i++)
            ib[i] = (rlvk_image_memory_barrier){ .sType = RLVK_ST_IMAGE_MEMORY_BARRIER,
                                                 .dstAccessMask = RLVK_ACCESS_TRANSFER_WRITE_BIT | rw_access,
                                                 .oldLayout = RLVK_IMAGE_LAYOUT_UNDEFINED,
                                                 .newLayout = RLVK_IMAGE_LAYOUT_GENERAL,
                                                 .srcQueueFamilyIndex = (uint32_t)RLVK_QUEUE_FAMILY_IGNORED,
                                                 .dstQueueFamilyIndex = (uint32_t)RLVK_QUEUE_FAMILY_IGNORED,
                                                 .image = t->history[i].image,
                                                 .subresourceRange = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        t->vk.vkCmdPipelineBarrier(cmd, cs | xfer, cs | xfer, 0, 0, NULL, 0, NULL, 2, ib);
        for (int i = 0; i < 2; i++)
            rl_ffx_clear(&t->vk, cmd, t->history[i].image, 0.0f, 0.0f);
        t->fresh = false;
    }
    t->first = false;
    t->vk.vkCmdUpdateBuffer(cmd, t->cb, off, sizeof c, &c);
    rl_ffx_barrier(&t->vk, cmd, cs | xfer, rw_access | RLVK_ACCESS_TRANSFER_WRITE_BIT, cs,
                   rw_access | RLVK_ACCESS_UNIFORM_READ_BIT);
    t->vk.vkCmdBindPipeline(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, t->pipe.pipe);
    t->vk.vkCmdBindDescriptorSets(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, t->pipe.layout, 0, 1, &set, 0, NULL);
    t->vk.vkCmdDispatch(cmd, (dw + 7) / 8, (dh + 7) / 8, 1);
    if (sharpen) {
        rl_ffx_barrier(&t->vk, cmd, cs, rw_access, cs, rw_access);
        t->vk.vkCmdBindPipeline(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, t->rcas.pipe);
        t->vk.vkCmdBindDescriptorSets(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, t->rcas.layout, 0, 1, &rset, 0, NULL);
        t->vk.vkCmdDispatch(cmd, (dw + 7) / 8, (dh + 7) / 8, 1);
    }
    t->parity++;
    return true;
}

void rl_taa_retire(rl_taa *t)
{
    if (!t || !t->pool)
        return;
    t->vk.vkResetDescriptorPool(t->vk.device, t->pool, 0);
    t->used = 0;
    t->pending = false;
}

void rl_taa_free(rl_taa *t)
{
    if (!t)
        return;
    if (t->vk.vkDestroyImageView) {
        for (int i = 0; i < 2; i++)
            rl_ffx_image_free(&t->vk, &t->history[i]);
        rl_ffx_pipeline_free(&t->vk, &t->pipe);
        rl_ffx_pipeline_free(&t->vk, &t->rcas);
        if (t->cb)
            t->vk.vkDestroyBuffer(t->vk.device, t->cb, NULL);
        if (t->cb_memory)
            t->vk.vkFreeMemory(t->vk.device, t->cb_memory, NULL);
        if (t->pool)
            t->vk.vkDestroyDescriptorPool(t->vk.device, t->pool, NULL);
        if (t->sampler)
            t->vk.vkDestroySampler(t->vk.device, t->sampler, NULL);
    }
    free(t);
}
