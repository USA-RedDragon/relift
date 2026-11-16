/**
 * @file vk_format.c
 * @brief Depth formats the console always has: VK_FORMAT_D24_UNORM_S8_UINT
 * and VK_FORMAT_X8_D24_UNORM_PACK32.
 *
 * Games do not always check the answers they get (one picks another depth
 * format for an image and still views it as D24S8, The Forge stops when a
 * format it samples reports no sampling), so on a host without 24-bit depth
 * (AMD GPUs on RADV and AMDVLK, Apple GPUs on MoltenVK) the layer uses
 * D32_SFLOAT_S8_UINT and D32_SFLOAT in their place: the format queries
 * answer for the format used (vkf_host_format, used by vk_props.c), and
 * images, image views, render pass attachments, dynamic rendering formats
 * and image format lists reach the host with it (vkf_depth_format).
 *
 * Every 24-bit depth value is exact in 32-bit float, so depth tests and
 * sampled depth match; a depth bias in units of the format's resolution is
 * as large as on the console for depths of 0.5 and above and smaller below.
 * Buffer copies of the depth aspect, whose texels differ in layout between
 * the two formats, are not converted.
 */
#include "vkf.h"

#include <string.h>

static VkFormat substitute(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D24_UNORM_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_FORMAT_D32_SFLOAT;
    default: return f;
    }
}

static bool host_lacks(const vkf_fns *fns, VkPhysicalDevice pd, VkFormat f)
{
    VkFormatProperties fp;
    memset(&fp, 0, sizeof fp);
    VKF_FN(fns, vkGetPhysicalDeviceFormatProperties)(pd, f, &fp);
    return !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT);
}

VkFormat vkf_host_format(const vkf_fns *fns, VkPhysicalDevice pd, VkFormat f)
{
    VkFormat s = substitute(f);
    if (s == f || !host_lacks(fns, pd, f))
        return f;
    if (rl_once(0x464D545355420000ull | (uint64_t)f))
        rl_log(RL_LOG_INFO, "vk", "format %d: the host has no 24-bit depth; images use format %d and queries answer for it", (int)f,
               (int)s);
    return s;
}

void vkf_depth_device_setup(vkf_call *k, vkf_dev *d, VkPhysicalDevice pd)
{
    d->depth24 = host_lacks(k->fns, pd, VK_FORMAT_D24_UNORM_S8_UINT) || host_lacks(k->fns, pd, VK_FORMAT_X8_D24_UNORM_PACK32);
}

VkFormat vkf_depth_format(const vkf_dev *d, VkFormat f) { return d && d->depth24 ? substitute(f) : f; }

bool vkf_depth_substituted(const vkf_dev *d, VkFormat f) { return vkf_depth_format(d, f) != f; }

void vkf_depth_format_list(vkf_call *k, const vkf_dev *d, const void *chain)
{
    if (!d || !d->depth24)
        return;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)chain; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO) {
            VkImageFormatListCreateInfo *l = (VkImageFormatListCreateInfo *)b;
            VkFormat *v = vkf_alloc(k, (l->viewFormatCount + 1) * sizeof *v);
            for (uint32_t i = 0; i < l->viewFormatCount; i++)
                v[i] = vkf_depth_format(d, l->pViewFormats[i]);
            l->pViewFormats = v;
        } else if (b->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO) {
            VkPipelineRenderingCreateInfo *r = (VkPipelineRenderingCreateInfo *)b;
            r->depthAttachmentFormat = vkf_depth_format(d, r->depthAttachmentFormat);
            r->stencilAttachmentFormat = vkf_depth_format(d, r->stencilAttachmentFormat);
        } else if (b->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO) {
            VkCommandBufferInheritanceRenderingInfo *r = (VkCommandBufferInheritanceRenderingInfo *)b;
            r->depthAttachmentFormat = vkf_depth_format(d, r->depthAttachmentFormat);
            r->stencilAttachmentFormat = vkf_depth_format(d, r->stencilAttachmentFormat);
        }
    }
}
