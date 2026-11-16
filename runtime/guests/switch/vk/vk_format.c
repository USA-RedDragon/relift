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
 * sampled depth match. A constant depth bias is in units of the format's
 * resolution, 2^-24 for D24 and the primitive's float ulp for D32; on a
 * substituted attachment the layer gives it as an absolute offset of
 * constant x 2^-24 through VK_EXT_depth_bias_control (float representation,
 * exact), in pipelines and in vkCmdSetDepthBias. For dynamic bias each
 * command buffer keeps the guest's values and gives the host the ones that
 * fit the depth attachment in use, again at each render pass, subpass and
 * dynamic rendering begin. Buffer copies of the depth aspect, whose texels
 * differ in layout between the two formats, are not converted.
 */
#include "vkf.h"

#include <pthread.h>
#include <string.h>

/** @brief The resolution of a 24-bit depth format: its constant depth bias unit. */
#define D24_UNIT (1.0f / 16777216.0f)

/** @brief Render passes with a substituted depth attachment (mask of their subpasses that use one, bit 63 set) and substituted image views, by host handle. */
static vkf_map *g_passes, *g_views;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void note(vkf_map **m, uint64_t h, uint64_t v)
{
    pthread_mutex_lock(&g_lock);
    if (!*m)
        *m = vkf_map_new();
    vkf_map_put(*m, h, v ? (void *)(uintptr_t)v : NULL);
    pthread_mutex_unlock(&g_lock);
}

static uint64_t lookup(vkf_map *const *m, uint64_t h)
{
    pthread_mutex_lock(&g_lock);
    uint64_t v = *m && h ? (uint64_t)(uintptr_t)vkf_map_get(*m, h) : 0;
    pthread_mutex_unlock(&g_lock);
    return v;
}

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
    d->bias_float = false;
    if (!d->depth24)
        return;
    uint32_t n = 0;
    VKF_FN(k->fns, vkEnumerateDeviceExtensionProperties)(pd, NULL, &n, NULL);
    VkExtensionProperties *p = vkf_alloc(k, (n + 1) * sizeof *p);
    VKF_FN(k->fns, vkEnumerateDeviceExtensionProperties)(pd, NULL, &n, p);
    bool has = false;
    for (uint32_t i = 0; i < n; i++)
        has = has || !strcmp(p[i].extensionName, VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME);
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = VKF_FN(k->fns, vkGetPhysicalDeviceFeatures2);
    if (has && gf2) {
        VkPhysicalDeviceDepthBiasControlFeaturesEXT f = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f };
        gf2(pd, &f2);
        d->bias_float = f.depthBiasControl && f.floatRepresentation && f.depthBiasExact;
    }
    if (!d->bias_float)
        rl_log(RL_LOG_WARN, "vk", "the host has no 24-bit depth and no exact float depth bias; a constant depth bias on D24 is in D32 units");
}

void vkf_depth_device_extend(vkf_call *k, const vkf_dev *d, VkDeviceCreateInfo *ci)
{
    if (!d->bias_float)
        return;
    const char **exts = vkf_alloc(k, (ci->enabledExtensionCount + 1) * sizeof *exts);
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        exts[i] = ci->ppEnabledExtensionNames[i];
    exts[ci->enabledExtensionCount] = VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME;
    ci->ppEnabledExtensionNames = exts;
    ci->enabledExtensionCount++;
    VkPhysicalDeviceDepthBiasControlFeaturesEXT *f = vkf_alloc(k, sizeof *f);
    *f = (VkPhysicalDeviceDepthBiasControlFeaturesEXT){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT,
        .pNext = (void *)ci->pNext,
        .depthBiasControl = VK_TRUE,
        .floatRepresentation = VK_TRUE,
        .depthBiasExact = VK_TRUE,
    };
    ci->pNext = f;
}

uint64_t vkf_depth_pass_mask(const vkf_dev *d, const VkAttachmentDescription *a, uint32_t na, const VkSubpassDescription *s, uint32_t ns)
{
    uint64_t mask = 0;
    for (uint32_t i = 0; d && d->depth24 && s && i < ns && i < 63; i++) {
        const VkAttachmentReference *r = s[i].pDepthStencilAttachment;
        if (r && r->attachment < na && vkf_depth_substituted(d, a[r->attachment].format))
            mask |= 1ull << i;
    }
    return mask;
}

uint64_t vkf_depth_pass_mask2(const vkf_dev *d, const VkRenderPassCreateInfo2 *ci)
{
    uint64_t mask = 0;
    for (uint32_t i = 0; d && d->depth24 && ci && i < ci->subpassCount && i < 63; i++) {
        const VkAttachmentReference2 *r = ci->pSubpasses[i].pDepthStencilAttachment;
        if (r && r->attachment < ci->attachmentCount && vkf_depth_substituted(d, ci->pAttachments[r->attachment].format))
            mask |= 1ull << i;
    }
    return mask;
}

void vkf_depth_note_pass(uint64_t pass, uint64_t mask)
{
    if (mask && pass)
        note(&g_passes, pass, mask | (1ull << 63));
}

void vkf_depth_note_view(const vkf_dev *d, uint64_t view, VkFormat guest)
{
    if (vkf_depth_substituted(d, guest))
        note(&g_views, view, 1);
}

void vkf_depth_forget_pass(vkf_call *k)
{
    if (g_passes && k->c->x[1])
        note(&g_passes, k->c->x[1], 0);
}

void vkf_depth_forget_view(vkf_call *k)
{
    if (g_views && k->c->x[1])
        note(&g_views, k->c->x[1], 0);
}

bool vkf_depth_pass_uses(uint64_t pass, uint32_t subpass) { return subpass < 63 && (lookup(&g_passes, pass) >> subpass) & 1; }

bool vkf_depth_view_uses(uint64_t view) { return lookup(&g_views, view) != 0; }

/** @brief Gives the host the command buffer's dynamic depth bias for its current depth attachment, when that changes what the host needs. */
static void emit_bias(vkf_call *k, vkf_obj *o, VkCommandBuffer cb, bool force)
{
    bool scaled = o->depth_now && o->dev->bias_float;
    if (!o->bias_set || (!force && scaled == o->bias_scaled))
        return;
    if (scaled) {
        VkDepthBiasRepresentationInfoEXT rep = {
            .sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT,
            .depthBiasRepresentation = VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT,
            .depthBiasExact = VK_TRUE,
        };
        VkDepthBiasInfoEXT bi = {
            .sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_INFO_EXT,
            .pNext = &rep,
            .depthBiasConstantFactor = o->bias[0] * D24_UNIT,
            .depthBiasClamp = o->bias[1],
            .depthBiasSlopeFactor = o->bias[2],
        };
        VKF_FN(k->fns, vkCmdSetDepthBias2EXT)(cb, &bi);
    } else
        VKF_FN(k->fns, vkCmdSetDepthBias)(cb, o->bias[0], o->bias[1], o->bias[2]);
    o->bias_scaled = scaled;
}

void vkf_depth_attachment(vkf_call *k, vkf_obj *o, bool substituted)
{
    if (!o || !o->dev || !o->dev->depth24)
        return;
    o->depth_now = substituted;
    emit_bias(k, o, (VkCommandBuffer)o->host, false);
}

void vkf_depth_cb_begin(vkf_obj *o, const VkCommandBufferBeginInfo *bi)
{
    o->bias_set = o->bias_scaled = o->depth_now = false;
    if (!o->dev || !o->dev->depth24 || !bi || !bi->pInheritanceInfo)
        return;
    const VkCommandBufferInheritanceInfo *in = bi->pInheritanceInfo;
    o->depth_now = vkf_depth_pass_uses((uint64_t)(uintptr_t)in->renderPass, in->subpass);
    for (const VkBaseInStructure *b = in->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_RENDERING_INFO)
            o->depth_now = o->depth_now || vkf_depth_substituted(o->dev, ((const VkCommandBufferInheritanceRenderingInfo *)b)->depthAttachmentFormat);
}

void vkf_depth_begin_pass(vkf_call *k)
{
    vkf_obj *o = k->obj;
    if (!o || !o->dev || !o->dev->depth24 || !k->c->x[1])
        return;
    o->pass = vkf_g64(k, k->c->x[1] + offsetof(VkRenderPassBeginInfo, renderPass));
    o->subpass = 0;
    vkf_depth_attachment(k, o, vkf_depth_pass_uses(o->pass, 0));
}

void vkf_depth_next_subpass(vkf_call *k)
{
    vkf_obj *o = k->obj;
    if (!o || !o->dev || !o->dev->depth24)
        return;
    o->subpass++;
    vkf_depth_attachment(k, o, vkf_depth_pass_uses(o->pass, o->subpass));
}

void vkf_depth_end_pass(vkf_call *k)
{
    vkf_obj *o = k->obj;
    if (o && o->dev && o->dev->depth24)
        o->depth_now = false;
}

int vkh_vkCmdSetDepthBias(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkCommandBuffer cb = vkf_first(&k, VKG_KIND_VkCommandBuffer, c->x[0]);
    if (!cb)
        return vkf_bad_handle(&k, VKG_vkCmdSetDepthBias, c->x[0]);
    vkf_obj *o = k.obj;
    float f[3] = { vkf_s(&k, 0), vkf_s(&k, 1), vkf_s(&k, 2) };
    if (!o->dev || !o->dev->depth24) {
        VKF_FN(k.fns, vkCmdSetDepthBias)(cb, f[0], f[1], f[2]);
        return vkf_end(&k);
    }
    memcpy(o->bias, f, sizeof f);
    o->bias_set = true;
    emit_bias(&k, o, cb, true);
    return vkf_end(&k);
}

void vkf_depth_pipeline(vkf_call *k, const vkf_dev *d, VkGraphicsPipelineCreateInfo *ci)
{
    if (!d || !d->depth24 || !d->bias_float || !ci->pRasterizationState || !ci->pRasterizationState->depthBiasEnable)
        return;
    for (uint32_t i = 0; ci->pDynamicState && i < ci->pDynamicState->dynamicStateCount; i++)
        if (ci->pDynamicState->pDynamicStates[i] == VK_DYNAMIC_STATE_DEPTH_BIAS)
            return;
    bool sub = ci->renderPass ? vkf_depth_pass_uses((uint64_t)(uintptr_t)ci->renderPass, ci->subpass) : false;
    for (const VkBaseInStructure *b = ci->pNext; !ci->renderPass && b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO)
            sub = vkf_depth_substituted(d, ((const VkPipelineRenderingCreateInfo *)b)->depthAttachmentFormat);
    if (!sub)
        return;
    VkPipelineRasterizationStateCreateInfo *r = vkf_alloc(k, sizeof *r);
    *r = *ci->pRasterizationState;
    VkDepthBiasRepresentationInfoEXT *rep = vkf_alloc(k, sizeof *rep);
    *rep = (VkDepthBiasRepresentationInfoEXT){
        .sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT,
        .pNext = r->pNext,
        .depthBiasRepresentation = VK_DEPTH_BIAS_REPRESENTATION_FLOAT_EXT,
        .depthBiasExact = VK_TRUE,
    };
    r->pNext = rep;
    r->depthBiasConstantFactor *= D24_UNIT;
    ci->pRasterizationState = r;
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
