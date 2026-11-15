/**
 * @file vk_overlay.c
 * @brief The on-screen controller over the host swapchain image: the overlay
 * pixels (touchpad.h) go to a sampled image through a staging buffer, and a
 * full-screen triangle blends them (premultiplied alpha) onto the host image
 * after the guest picture is blitted. Host-only: the guest's images are only
 * read.
 */
#include "vkf.h"
#include "vk_overlay.h"
#include "vk_overlay_spv.h"

#include <relift/touchpad.h>

#include <stdlib.h>
#include <string.h>

#define FN(o, name) VKF_FN((o)->fns, name)

typedef struct staging {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *map;
    VkDeviceSize size;
} staging;

struct vkf_overlay {
    VkDevice dev;
    const vkf_fns *fns;
    const vkf_dev *d;
    VkFormat format;
    VkExtent2D extent;
    uint32_t n;
    VkImageView views[VKF_OVERLAY_HOST_MAX];
    VkFramebuffer fbs[VKF_OVERLAY_HOST_MAX];
    VkRenderPass pass;
    VkDescriptorSetLayout dsl;
    VkDescriptorPool dpool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkSampler sampler;
    VkImage img;
    VkDeviceMemory img_mem;
    VkImageView img_view;
    uint32_t iw, ih;
    bool ready;
    staging slots[VKF_OVERLAY_SLOTS];
    bool failed;
};

static uint32_t mem_type(const vkf_dev *d, uint32_t bits, uint32_t want)
{
    for (uint32_t i = 0; i < d->type_count; i++)
        if ((bits & (1u << i)) && (d->type_flags[i] & want) == want)
            return i;
    return UINT32_MAX;
}

static bool srgb(VkFormat f)
{
    return f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
}

static VkShaderModule shader(vkf_overlay *o, const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule m = NULL;
    if (FN(o, vkCreateShaderModule)(o->dev, &ci, NULL, &m) < 0)
        return NULL;
    return m;
}

static bool make_pipeline(vkf_overlay *o)
{
    VkAttachmentDescription att = {
        .format = o->format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
    };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
    };
    VkRenderPassCreateInfo rp = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &sub,
    };
    if (FN(o, vkCreateRenderPass)(o->dev, &rp, NULL, &o->pass) < 0)
        return false;
    VkDescriptorSetLayoutBinding b = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dl = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &b,
    };
    if (FN(o, vkCreateDescriptorSetLayout)(o->dev, &dl, NULL, &o->dsl) < 0)
        return false;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 };
    VkDescriptorPoolCreateInfo dp = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &ps,
    };
    if (FN(o, vkCreateDescriptorPool)(o->dev, &dp, NULL, &o->dpool) < 0)
        return false;
    VkDescriptorSetAllocateInfo da = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = o->dpool,
        .descriptorSetCount = 1,
        .pSetLayouts = &o->dsl,
    };
    if (FN(o, vkAllocateDescriptorSets)(o->dev, &da, &o->set) < 0)
        return false;
    VkPipelineLayoutCreateInfo pl = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &o->dsl,
    };
    if (FN(o, vkCreatePipelineLayout)(o->dev, &pl, NULL, &o->layout) < 0)
        return false;
    VkSamplerCreateInfo sc = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .maxLod = 0,
    };
    if (FN(o, vkCreateSampler)(o->dev, &sc, NULL, &o->sampler) < 0)
        return false;
    VkShaderModule vs = shader(o, vkf_overlay_vert, sizeof vkf_overlay_vert);
    VkShaderModule fs = shader(o, vkf_overlay_frag, sizeof vkf_overlay_frag);
    bool ok = vs && fs;
    if (ok) {
        VkPipelineShaderStageCreateInfo st[2] = {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_VERTEX_BIT,
              .module = vs,
              .pName = "main" },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
              .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
              .module = fs,
              .pName = "main" },
        };
        VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
        VkPipelineInputAssemblyStateCreateInfo ia = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };
        VkPipelineViewportStateCreateInfo vp = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .scissorCount = 1,
        };
        VkPipelineRasterizationStateCreateInfo rs = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
            .lineWidth = 1.0f,
        };
        VkPipelineMultisampleStateCreateInfo ms = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        };
        VkPipelineColorBlendAttachmentState ba = {
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .alphaBlendOp = VK_BLEND_OP_ADD,
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
                              VK_COLOR_COMPONENT_A_BIT,
        };
        VkPipelineColorBlendStateCreateInfo cb = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1,
            .pAttachments = &ba,
        };
        VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo ds = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .dynamicStateCount = 2,
            .pDynamicStates = dyn,
        };
        VkGraphicsPipelineCreateInfo gp = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2,
            .pStages = st,
            .pVertexInputState = &vi,
            .pInputAssemblyState = &ia,
            .pViewportState = &vp,
            .pRasterizationState = &rs,
            .pMultisampleState = &ms,
            .pColorBlendState = &cb,
            .pDynamicState = &ds,
            .layout = o->layout,
            .renderPass = o->pass,
        };
        ok = FN(o, vkCreateGraphicsPipelines)(o->dev, NULL, 1, &gp, NULL, &o->pipe) >= 0;
    }
    if (vs)
        FN(o, vkDestroyShaderModule)(o->dev, vs, NULL);
    if (fs)
        FN(o, vkDestroyShaderModule)(o->dev, fs, NULL);
    return ok;
}

static bool make_targets(vkf_overlay *o, const VkImage *imgs)
{
    for (uint32_t i = 0; i < o->n; i++) {
        VkImageViewCreateInfo vc = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .image = imgs[i],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = o->format,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        if (FN(o, vkCreateImageView)(o->dev, &vc, NULL, &o->views[i]) < 0)
            return false;
        VkFramebufferCreateInfo fc = {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .renderPass = o->pass,
            .attachmentCount = 1,
            .pAttachments = &o->views[i],
            .width = o->extent.width,
            .height = o->extent.height,
            .layers = 1,
        };
        if (FN(o, vkCreateFramebuffer)(o->dev, &fc, NULL, &o->fbs[i]) < 0)
            return false;
    }
    return true;
}

static void drop_image(vkf_overlay *o)
{
    if (o->img_view)
        FN(o, vkDestroyImageView)(o->dev, o->img_view, NULL);
    if (o->img)
        FN(o, vkDestroyImage)(o->dev, o->img, NULL);
    if (o->img_mem)
        FN(o, vkFreeMemory)(o->dev, o->img_mem, NULL);
    o->img_view = NULL;
    o->img = NULL;
    o->img_mem = NULL;
    o->iw = o->ih = 0;
    o->ready = false;
}

static bool make_image(vkf_overlay *o, uint32_t w, uint32_t h)
{
    drop_image(o);
    VkFormat f = srgb(o->format) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = f,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    if (FN(o, vkCreateImage)(o->dev, &ii, NULL, &o->img) < 0)
        return false;
    VkMemoryRequirements req;
    FN(o, vkGetImageMemoryRequirements)(o->dev, o->img, &req);
    uint32_t t = mem_type(o->d, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t == UINT32_MAX)
        t = mem_type(o->d, req.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = t };
    if (t == UINT32_MAX || FN(o, vkAllocateMemory)(o->dev, &ai, NULL, &o->img_mem) < 0 ||
        FN(o, vkBindImageMemory)(o->dev, o->img, o->img_mem, 0) < 0)
        return false;
    VkImageViewCreateInfo vc = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = o->img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = f,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    if (FN(o, vkCreateImageView)(o->dev, &vc, NULL, &o->img_view) < 0)
        return false;
    VkDescriptorImageInfo di = { o->sampler, o->img_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet wr = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = o->set,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &di,
    };
    FN(o, vkUpdateDescriptorSets)(o->dev, 1, &wr, 0, NULL);
    o->iw = w;
    o->ih = h;
    return true;
}

static void drop_staging(vkf_overlay *o, staging *s)
{
    if (s->map)
        FN(o, vkUnmapMemory)(o->dev, s->mem);
    if (s->buf)
        FN(o, vkDestroyBuffer)(o->dev, s->buf, NULL);
    if (s->mem)
        FN(o, vkFreeMemory)(o->dev, s->mem, NULL);
    memset(s, 0, sizeof *s);
}

static bool grow(vkf_overlay *o, staging *s, VkDeviceSize size)
{
    if (s->size >= size)
        return true;
    drop_staging(o, s);
    VkBufferCreateInfo bc = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
    };
    if (FN(o, vkCreateBuffer)(o->dev, &bc, NULL, &s->buf) < 0)
        return false;
    VkMemoryRequirements req;
    FN(o, vkGetBufferMemoryRequirements)(o->dev, s->buf, &req);
    uint32_t t = mem_type(o->d, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = t };
    if (t == UINT32_MAX || FN(o, vkAllocateMemory)(o->dev, &ai, NULL, &s->mem) < 0 ||
        FN(o, vkBindBufferMemory)(o->dev, s->buf, s->mem, 0) < 0 ||
        FN(o, vkMapMemory)(o->dev, s->mem, 0, VK_WHOLE_SIZE, 0, &s->map) < 0) {
        drop_staging(o, s);
        return false;
    }
    s->size = size;
    return true;
}

vkf_overlay *vkf_overlay_new(VkDevice dev, const vkf_fns *fns, const vkf_dev *d, VkFormat format, VkExtent2D extent,
                             const VkImage *imgs, uint32_t n)
{
    vkf_overlay *o = calloc(1, sizeof *o);
    if (!o)
        abort();
    o->dev = dev;
    o->fns = fns;
    o->d = d;
    o->format = format;
    o->extent = extent;
    o->n = n < VKF_OVERLAY_HOST_MAX ? n : VKF_OVERLAY_HOST_MAX;
    if (!make_pipeline(o) || !make_targets(o, imgs)) {
        rl_log(RL_LOG_WARN, "vk", "the on-screen controller cannot draw on this host swapchain (format %d)", format);
        vkf_overlay_free(o);
        return NULL;
    }
    return o;
}

void vkf_overlay_free(vkf_overlay *o)
{
    if (!o)
        return;
    for (uint32_t i = 0; i < VKF_OVERLAY_SLOTS; i++)
        drop_staging(o, &o->slots[i]);
    drop_image(o);
    for (uint32_t i = 0; i < o->n; i++) {
        if (o->fbs[i])
            FN(o, vkDestroyFramebuffer)(o->dev, o->fbs[i], NULL);
        if (o->views[i])
            FN(o, vkDestroyImageView)(o->dev, o->views[i], NULL);
    }
    if (o->pipe)
        FN(o, vkDestroyPipeline)(o->dev, o->pipe, NULL);
    if (o->sampler)
        FN(o, vkDestroySampler)(o->dev, o->sampler, NULL);
    if (o->layout)
        FN(o, vkDestroyPipelineLayout)(o->dev, o->layout, NULL);
    if (o->dpool)
        FN(o, vkDestroyDescriptorPool)(o->dev, o->dpool, NULL);
    if (o->dsl)
        FN(o, vkDestroyDescriptorSetLayout)(o->dev, o->dsl, NULL);
    if (o->pass)
        FN(o, vkDestroyRenderPass)(o->dev, o->pass, NULL);
    free(o);
}

static void image_barrier(vkf_overlay *o, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                          VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src,
        .dstAccessMask = dst,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    FN(o, vkCmdPipelineBarrier)(cb, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

static void upload(vkf_overlay *o, VkCommandBuffer cb, uint32_t slot, const uint8_t *px, uint32_t w, const int32_t r[4])
{
    staging *s = &o->slots[slot % VKF_OVERLAY_SLOTS];
    VkDeviceSize size = (VkDeviceSize)r[2] * (VkDeviceSize)r[3] * 4;
    if (!grow(o, s, size))
        return;
    for (int32_t y = 0; y < r[3]; y++)
        memcpy((uint8_t *)s->map + (size_t)y * (size_t)r[2] * 4, px + (((size_t)(r[1] + y)) * w + (size_t)r[0]) * 4,
               (size_t)r[2] * 4);
    image_barrier(o, cb, o->img, o->ready ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (!o->ready) {
        VkClearColorValue zero = { .float32 = { 0, 0, 0, 0 } };
        VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        FN(o, vkCmdClearColorImage)(cb, o->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        image_barrier(o, cb, o->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                      VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                      VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    if (r[2] > 0 && r[3] > 0) {
        VkBufferImageCopy c = {
            .bufferRowLength = (uint32_t)r[2],
            .bufferImageHeight = (uint32_t)r[3],
            .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageOffset = { r[0], r[1], 0 },
            .imageExtent = { (uint32_t)r[2], (uint32_t)r[3], 1 },
        };
        FN(o, vkCmdCopyBufferToImage)(cb, s->buf, o->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
    }
    image_barrier(o, cb, o->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                  VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    o->ready = true;
}

bool vkf_overlay_record(vkf_overlay *o, VkCommandBuffer cb, uint32_t host_index, uint32_t slot, VkImage target)
{
    if (!o || host_index >= o->n)
        return false;
    rl_touchpad_tick(rl_clock_ns());
    uint32_t w = 0, h = 0;
    int32_t dirty[4];
    const uint8_t *px = rl_touchpad_pixels(&w, &h, dirty);
    if (!px || !w || !h)
        return false;
    if ((w != o->iw || h != o->ih) && !make_image(o, w, h)) {
        drop_image(o);
        return false;
    }
    if (!o->ready) {
        dirty[0] = dirty[1] = 0;
        dirty[2] = (int32_t)w;
        dirty[3] = (int32_t)h;
    }
    if (!o->ready || (dirty[2] > 0 && dirty[3] > 0))
        upload(o, cb, slot, px, w, dirty);
    image_barrier(o, cb, target, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    VkRenderPassBeginInfo rb = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = o->pass,
        .framebuffer = o->fbs[host_index],
        .renderArea = { { 0, 0 }, o->extent },
    };
    FN(o, vkCmdBeginRenderPass)(cb, &rb, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport vp = { 0, 0, (float)o->extent.width, (float)o->extent.height, 0, 1 };
    VkRect2D sc = { { 0, 0 }, o->extent };
    FN(o, vkCmdSetViewport)(cb, 0, 1, &vp);
    FN(o, vkCmdSetScissor)(cb, 0, 1, &sc);
    FN(o, vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, o->pipe);
    FN(o, vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, o->layout, 0, 1, &o->set, 0, NULL);
    FN(o, vkCmdDraw)(cb, 3, 1, 0, 0);
    FN(o, vkCmdEndRenderPass)(cb);
    return true;
}
