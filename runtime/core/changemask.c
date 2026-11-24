/**
 * @file changemask.c
 * @brief relift's moved-surface mask (shaders/changemask.comp) for the FSR 2 and FSR 3 ports: one pass per frame over
 * the render rectangle, last frame's view depth, hold count and reference depth in and this frame's out (two images
 * each, swapped each frame), the mask out.
 * The disocclusion tolerance is FSR 2.2's depth clip's (ffx_fsr2_depth_clip.h): a separation of 1.37e-5 times the
 * field of view's corner factor times the render diagonal per unit of view depth, with a falloff power from 1 to 3 by
 * the render size against 1920x1080.
 */
#include "changemask.h"

#include "core.h"
#include "post_data.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#define RECORDS 32u
#define CB_STRIDE 256u
#define SEPARATION 1.37e-5f

/** @brief changemask.comp's Params, std140. */
typedef struct cb_mask {
    int32_t render[2], motion_size[2];
    float motion_scale[2], motion_jitter[2], device_to_view[2], jitter_shift[2];
    float separation, power;
    int32_t flags, pad;
    float reference_shift[2];
} cb_mask;

enum { F_PREVIOUS = 1, F_REFERENCE = 2, F_REFRESH = 4 };

/** @brief Frames between reference depths: a surface moving an eighth of a pixel a frame shows within two of them. */
#define REFERENCE_FRAMES 8u

static const rl_ffx_bind g_binds[] = {
    { 0, 0, RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE }, { 0, 1, RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
    { 0, 2, RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE }, { 0, 3, RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
    { 0, 4, RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE }, { 0, 5, RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE },
    { 0, 6, RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE }, { 0, 8, RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE },
    { 0, 9, RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE }, { 0, 7, RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER },
};
#define NBINDS (sizeof g_binds / sizeof g_binds[0])

struct rl_changemask {
    rl_ffx_vk vk;
    rl_ffx_pipeline pipe;
    rlvk_handle pool, cb, cb_memory;
    rl_ffx_image depth[2], hold[2], reference[2], mask;
    uint32_t size[2], render[2], used, parity, since_reference;
    bool fresh, valid, pending, reference_valid;
    float previous_jitter[2], reference_jitter[2];
};

static rl_changemask *failed(rl_changemask *m, const char *what, rlvk_result r)
{
    rl_log(RL_LOG_WARN, "upscale", "the moved-surface mask did not start: %s failed (VkResult %d)", what, (int)r);
    rl_changemask_free(m);
    return NULL;
}

rl_changemask *rl_changemask_new(const rl_ffx_vk *vk)
{
    if (!vk || !vk->device)
        return NULL;
    rl_changemask *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->vk = *vk;
    if (!rl_ffx_pipeline_new(&m->vk, rl_post_spv_changemask, sizeof rl_post_spv_changemask, g_binds, NBINDS, 0, NULL,
                             0, 0, &m->pipe))
        return failed(m, "the pipeline", 0);
    rlvk_descriptor_pool_size ps[3] = {
        { RLVK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, RECORDS * 5 },
        { RLVK_DESCRIPTOR_TYPE_STORAGE_IMAGE, RECORDS * 4 },
        { RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, RECORDS },
    };
    rlvk_descriptor_pool_create_info dp = {
        .sType = RLVK_ST_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = RECORDS, .poolSizeCount = 3, .pPoolSizes = ps
    };
    rlvk_result r;
    if ((r = m->vk.vkCreateDescriptorPool(m->vk.device, &dp, NULL, &m->pool)) != RLVK_SUCCESS)
        return failed(m, "vkCreateDescriptorPool", r);
    rlvk_buffer_create_info bi = { .sType = RLVK_ST_BUFFER_CREATE_INFO,
                                   .size = CB_STRIDE * RECORDS,
                                   .usage = RLVK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | RLVK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if ((r = m->vk.vkCreateBuffer(m->vk.device, &bi, NULL, &m->cb)) != RLVK_SUCCESS)
        return failed(m, "vkCreateBuffer", r);
    rlvk_memory_requirements req;
    m->vk.vkGetBufferMemoryRequirements(m->vk.device, m->cb, &req);
    if (!rl_ffx_bind_memory(&m->vk, &req, &m->cb_memory) ||
        m->vk.vkBindBufferMemory(m->vk.device, m->cb, m->cb_memory, 0) != RLVK_SUCCESS)
        return failed(m, "the constant buffer's memory", 0);
    return m;
}

static void drop_images(rl_changemask *m)
{
    for (int i = 0; i < 2; i++) {
        rl_ffx_image_free(&m->vk, &m->depth[i]);
        rl_ffx_image_free(&m->vk, &m->hold[i]);
        rl_ffx_image_free(&m->vk, &m->reference[i]);
    }
    rl_ffx_image_free(&m->vk, &m->mask);
    m->size[0] = m->size[1] = 0;
}

static bool make_images(rl_changemask *m, uint32_t w, uint32_t h)
{
    drop_images(m);
    for (int i = 0; i < 2; i++)
        if (!rl_ffx_image_new(&m->vk, RLVK_FORMAT_R32_SFLOAT, w, h, 1, &m->depth[i]) ||
            !rl_ffx_image_new(&m->vk, RLVK_FORMAT_R16_SFLOAT, w, h, 1, &m->hold[i]) ||
            !rl_ffx_image_new(&m->vk, RLVK_FORMAT_R32_SFLOAT, w, h, 1, &m->reference[i]))
            return false;
    if (!rl_ffx_image_new(&m->vk, RLVK_FORMAT_R16_SFLOAT, w, h, 1, &m->mask))
        return false;
    m->size[0] = w;
    m->size[1] = h;
    m->fresh = true;
    m->valid = false;
    return true;
}

bool rl_changemask_record(rl_changemask *m, uint64_t cmd, const rl_upscale_inputs *in,
                          const rl_changemask_camera *cam, bool reset, rlvk_handle *mask, rlvk_handle *image)
{
    if (!m || !cmd || !in->depth.handle || !in->motion.handle || m->used == RECORDS)
        return false;
    uint32_t rw = in->in.width, rh = in->in.height;
    if (!rw || !rh)
        return false;
    if (m->size[0] < rw || m->size[1] < rh) {
        if (m->pending)
            return false;
        if (!make_images(m, rw, rh)) {
            drop_images(m);
            return false;
        }
    }
    rlvk_handle set = 0;
    rlvk_descriptor_set_allocate_info da = { .sType = RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO,
                                             .descriptorPool = m->pool,
                                             .descriptorSetCount = 1,
                                             .pSetLayouts = &m->pipe.set_layout };
    if (m->vk.vkAllocateDescriptorSets(m->vk.device, &da, &set) != RLVK_SUCCESS)
        return false;
    bool odd = m->parity & 1;
    rlvk_handle views[9] = { in->depth.handle,    in->motion.handle,       m->depth[odd].view,
                             m->hold[odd].view,   m->depth[!odd].view,     m->mask.view,
                             m->hold[!odd].view,  m->reference[odd].view,  m->reference[!odd].view };
    rlvk_descriptor_image_info ii[9];
    rlvk_write_descriptor_set wr[NBINDS];
    for (uint32_t i = 0; i < 9; i++) {
        ii[i] = (rlvk_descriptor_image_info){ .imageView = views[i], .imageLayout = RLVK_IMAGE_LAYOUT_GENERAL };
        wr[i] = (rlvk_write_descriptor_set){ .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
                                             .dstSet = set,
                                             .dstBinding = g_binds[i].binding,
                                             .descriptorCount = 1,
                                             .descriptorType = (int32_t)g_binds[i].type,
                                             .pImageInfo = &ii[i] };
    }
    uint32_t off = m->used * CB_STRIDE;
    rlvk_descriptor_buffer_info bi = { .buffer = m->cb, .offset = off, .range = sizeof(cb_mask) };
    wr[9] = (rlvk_write_descriptor_set){ .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
                                         .dstSet = set,
                                         .dstBinding = 7,
                                         .descriptorCount = 1,
                                         .descriptorType = RLVK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         .pBufferInfo = &bi };
    m->vk.vkUpdateDescriptorSets(m->vk.device, NBINDS, wr, 0, NULL);
    m->used++;
    m->pending = true;
    bool previous = m->valid && !reset && m->render[0] == rw && m->render[1] == rh;
    float tw = in->motion_display_size ? (float)in->out_rect.width : (float)rw;
    float th = in->motion_display_size ? (float)in->out_rect.height : (float)rh;
    uint32_t mw = in->motion.width ? in->motion.width : rw, mh = in->motion.height ? in->motion.height : rh;
    uint32_t sw = in->motion_display_size ? in->out_rect.width : rw;
    uint32_t sh = in->motion_display_size ? in->out_rect.height : rh;
    float diagonal = sqrtf((float)rw * (float)rw + (float)rh * (float)rh);
    float corner = sqrtf(1.0f + cam->tan_half[0] * cam->tan_half[0] + cam->tan_half[1] * cam->tan_half[1]);
    float resolution = diagonal / sqrtf(1920.0f * 1920.0f + 1080.0f * 1080.0f);
    cb_mask c = {
        .render = { (int32_t)rw, (int32_t)rh },
        .motion_size = { (int32_t)(sw < mw ? sw : mw), (int32_t)(sh < mh ? sh : mh) },
        .motion_scale = { in->motion_scale[0] / tw, in->motion_scale[1] / th },
        .device_to_view = { cam->device_to_view[0], cam->device_to_view[1] },
        .jitter_shift = { m->previous_jitter[0] - in->jitter[0], m->previous_jitter[1] - in->jitter[1] },
        .separation = SEPARATION * corner * diagonal,
        .power = 1.0f + 2.0f * (resolution < 1.0f ? resolution : 1.0f),
        .flags = previous ? F_PREVIOUS : 0,
    };
    if (!previous)
        m->reference_valid = false;
    if (m->reference_valid) {
        c.flags |= F_REFERENCE;
        c.reference_shift[0] = m->reference_jitter[0] - in->jitter[0];
        c.reference_shift[1] = m->reference_jitter[1] - in->jitter[1];
    }
    if (!m->reference_valid || ++m->since_reference >= REFERENCE_FRAMES) {
        c.flags |= F_REFRESH;
        m->since_reference = 0;
        m->reference_valid = true;
        m->reference_jitter[0] = in->jitter[0];
        m->reference_jitter[1] = in->jitter[1];
    }
    if (in->motion_jittered && previous) {
        c.motion_jitter[0] = (m->previous_jitter[0] - in->jitter[0]) / tw;
        c.motion_jitter[1] = (m->previous_jitter[1] - in->jitter[1]) / th;
    }
    m->previous_jitter[0] = in->jitter[0];
    m->previous_jitter[1] = in->jitter[1];
    const uint32_t cs = RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, xfer = RLVK_PIPELINE_STAGE_TRANSFER_BIT;
    const uint32_t rw_access = RLVK_ACCESS_SHADER_READ_BIT | RLVK_ACCESS_SHADER_WRITE_BIT;
    if (m->fresh) {
        rlvk_image_memory_barrier ib[7];
        rlvk_handle imgs[7] = { m->depth[0].image,     m->depth[1].image,     m->hold[0].image, m->hold[1].image,
                                m->reference[0].image, m->reference[1].image, m->mask.image };
        for (int i = 0; i < 7; i++)
            ib[i] = (rlvk_image_memory_barrier){ .sType = RLVK_ST_IMAGE_MEMORY_BARRIER,
                                                 .dstAccessMask = RLVK_ACCESS_TRANSFER_WRITE_BIT | rw_access,
                                                 .oldLayout = RLVK_IMAGE_LAYOUT_UNDEFINED,
                                                 .newLayout = RLVK_IMAGE_LAYOUT_GENERAL,
                                                 .srcQueueFamilyIndex = (uint32_t)RLVK_QUEUE_FAMILY_IGNORED,
                                                 .dstQueueFamilyIndex = (uint32_t)RLVK_QUEUE_FAMILY_IGNORED,
                                                 .image = imgs[i],
                                                 .subresourceRange = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        m->vk.vkCmdPipelineBarrier(cmd, cs | xfer, cs | xfer, 0, 0, NULL, 0, NULL, 7, ib);
        for (int i = 0; i < 7; i++)
            rl_ffx_clear(&m->vk, cmd, imgs[i], 0.0f, 0.0f);
        m->fresh = false;
    }
    m->vk.vkCmdUpdateBuffer(cmd, m->cb, off, sizeof c, &c);
    rl_ffx_barrier(&m->vk, cmd, cs | xfer, rw_access | RLVK_ACCESS_TRANSFER_WRITE_BIT, cs,
                   rw_access | RLVK_ACCESS_UNIFORM_READ_BIT);
    m->vk.vkCmdBindPipeline(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, m->pipe.pipe);
    m->vk.vkCmdBindDescriptorSets(cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, m->pipe.layout, 0, 1, &set, 0, NULL);
    m->vk.vkCmdDispatch(cmd, (rw + 7) / 8, (rh + 7) / 8, 1);
    rl_ffx_barrier(&m->vk, cmd, cs, rw_access, cs, rw_access);
    m->render[0] = rw;
    m->render[1] = rh;
    m->valid = true;
    m->parity++;
    *mask = m->mask.view;
    *image = m->mask.image;
    return true;
}

void rl_changemask_retire(rl_changemask *m)
{
    if (!m || !m->pool)
        return;
    m->vk.vkResetDescriptorPool(m->vk.device, m->pool, 0);
    m->used = 0;
    m->pending = false;
}

void rl_changemask_free(rl_changemask *m)
{
    if (!m)
        return;
    if (m->vk.vkDestroyImageView) {
        drop_images(m);
        rl_ffx_pipeline_free(&m->vk, &m->pipe);
        if (m->cb)
            m->vk.vkDestroyBuffer(m->vk.device, m->cb, NULL);
        if (m->cb_memory)
            m->vk.vkFreeMemory(m->vk.device, m->cb_memory, NULL);
        if (m->pool)
            m->vk.vkDestroyDescriptorPool(m->vk.device, m->pool, NULL);
    }
    free(m);
}
