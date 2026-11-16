/**
 * @file vk_pass.c
 * @brief Attachment contents kept through VK_ATTACHMENT_LOAD_OP_DONT_CARE and
 * VK_ATTACHMENT_STORE_OP_DONT_CARE.
 *
 * The console driver leaves an attachment's memory alone for DONT_CARE, and
 * games draw over contents they told the driver not to keep. On hosts where
 * DONT_CARE loses them (MoltenVK on Apple GPUs starts each tile with stale
 * tile memory), the layer passes LOAD and STORE instead. The vk.keep_attachments
 * setting chooses: auto (hosts measured to need it), on or off.
 */
#include "vkf.h"

#include <relift/settings.h>

#include <stdlib.h>
#include <string.h>

#define VENDOR_APPLE 0x106Bu

bool vkf_keep_attachments(uint32_t vendor)
{
    switch (rl_setting_enum("vk.keep_attachments")) {
    case RL_VK_KEEP_ON:
        return true;
    case RL_VK_KEEP_OFF:
        return false;
    default:
        return vendor == VENDOR_APPLE;
    }
}

static void keep_load(VkAttachmentLoadOp *op)
{
    if (*op == VK_ATTACHMENT_LOAD_OP_DONT_CARE)
        *op = VK_ATTACHMENT_LOAD_OP_LOAD;
}

static void keep_store(VkAttachmentStoreOp *op)
{
    if (*op == VK_ATTACHMENT_STORE_OP_DONT_CARE)
        *op = VK_ATTACHMENT_STORE_OP_STORE;
}

int vkh_vkCreateRenderPass(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkCreateRenderPass, c->x[0]);
    VkRenderPassCreateInfo *ci = vkg_ain_VkRenderPassCreateInfo(&k, c->x[1], 1);
    const vkf_dev *d = k.obj->dev;
    uint64_t mask = ci ? vkf_depth_pass_mask(d, ci->pAttachments, ci->attachmentCount, ci->pSubpasses, ci->subpassCount) : 0;
    if (ci && ci->attachmentCount && (d->keep_attachments || d->depth24)) {
        VkAttachmentDescription *a = vkf_alloc(&k, ci->attachmentCount * sizeof *a);
        memcpy(a, ci->pAttachments, ci->attachmentCount * sizeof *a);
        for (uint32_t i = 0; i < ci->attachmentCount; i++) {
            a[i].format = vkf_depth_format(d, a[i].format);
            if (!d->keep_attachments)
                continue;
            keep_load(&a[i].loadOp);
            keep_load(&a[i].stencilLoadOp);
            keep_store(&a[i].storeOp);
            keep_store(&a[i].stencilStoreOp);
        }
        ci->pAttachments = a;
    }
    VkRenderPass *out = vkf_ptr(&k, c->x[3]);
    VkResult r = VKF_FN(k.fns, vkCreateRenderPass)(dev, ci, NULL, out);
    if (r >= 0 && out)
        vkf_depth_note_pass((uint64_t)(uintptr_t)*out, mask);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateRenderPass, r);
    return vkf_end(&k);
}

int vkh_vkCreateRenderPass2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkCreateRenderPass2, c->x[0]);
    VkRenderPassCreateInfo2 *ci = vkg_ain_VkRenderPassCreateInfo2(&k, c->x[1], 1);
    const vkf_dev *d = k.obj->dev;
    uint64_t mask = vkf_depth_pass_mask2(d, ci);
    if (ci && (d->keep_attachments || d->depth24)) {
        VkAttachmentDescription2 *a = (VkAttachmentDescription2 *)ci->pAttachments;
        for (uint32_t i = 0; a && i < ci->attachmentCount; i++) {
            a[i].format = vkf_depth_format(d, a[i].format);
            if (!d->keep_attachments)
                continue;
            keep_load(&a[i].loadOp);
            keep_load(&a[i].stencilLoadOp);
            keep_store(&a[i].storeOp);
            keep_store(&a[i].stencilStoreOp);
        }
    }
    VkRenderPass *out = vkf_ptr(&k, c->x[3]);
    VkResult r = VKF_FN(k.fns, vkCreateRenderPass2)(dev, ci, NULL, out);
    if (r >= 0 && out)
        vkf_depth_note_pass((uint64_t)(uintptr_t)*out, mask);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateRenderPass2, r);
    return vkf_end(&k);
}

static void keep_rendering(VkRenderingAttachmentInfo *a)
{
    if (!a)
        return;
    keep_load(&a->loadOp);
    keep_store(&a->storeOp);
}

int vkh_vkCmdBeginRendering(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkCommandBuffer cb = vkf_first(&k, VKG_KIND_VkCommandBuffer, c->x[0]);
    if (!cb)
        return vkf_bad_handle(&k, VKG_vkCmdBeginRendering, c->x[0]);
    VkRenderingInfo *ri = vkg_ain_VkRenderingInfo(&k, c->x[1], 1);
    bool sub = ri && ri->pDepthAttachment && vkf_depth_view_uses((uint64_t)(uintptr_t)ri->pDepthAttachment->imageView);
    if (ri && k.obj->dev->keep_attachments) {
        VkRenderingAttachmentInfo *color = (VkRenderingAttachmentInfo *)ri->pColorAttachments;
        for (uint32_t i = 0; color && i < ri->colorAttachmentCount; i++)
            keep_rendering(&color[i]);
        keep_rendering((VkRenderingAttachmentInfo *)ri->pDepthAttachment);
        keep_rendering((VkRenderingAttachmentInfo *)ri->pStencilAttachment);
    }
    VKF_FN(k.fns, vkCmdBeginRendering)(cb, ri);
    vkf_depth_attachment(&k, k.obj, sub);
    return vkf_end(&k);
}

int vkh_vkCreateGraphicsPipelines(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkCreateGraphicsPipelines, c->x[0]);
    uint32_t n = (uint32_t)c->x[2];
    VkGraphicsPipelineCreateInfo *ci = vkg_ain_VkGraphicsPipelineCreateInfo(&k, c->x[3], n);
    for (uint32_t i = 0; ci && i < n; i++) {
        vkf_depth_pipeline(&k, k.obj->dev, &ci[i]);
        vkf_depth_format_list(&k, k.obj->dev, ci[i].pNext);
    }
    VkPipeline *out = vkf_ptr(&k, c->x[5]);
    VkResult r = VKF_FN(k.fns, vkCreateGraphicsPipelines)(dev, (VkPipelineCache)(uintptr_t)c->x[1], n, ci, NULL, out);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateGraphicsPipelines, r);
    return vkf_end(&k);
}
