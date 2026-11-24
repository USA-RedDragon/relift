/**
 * @file nvn_msaa.c
 * @brief Multisample textures: their samples moved between the host's multisample image and the guest's storage,
 * and resolved (nvnCommandBufferDownsample).
 *
 * The guest stores a multisample texture of n samples as a single-sample one gw by gh times its size (2x1, 2x2, 4x2
 * and 4x4 for 2, 4, 8 and 16 samples: the SDK 22.2.8 driver's tables at sdk 0xbb25d0 and 0xbb23d0, relift's
 * nvn_tex_layout, Mesa's NVK src/nouveau/nil/image.rs SampleLayout::px_extent_sa). Sample s of pixel (x, y) is that
 * texture's texel (x * gw + cx, y * gh + cy). The driver's texture header names the sample modes 2X1_D3D, 2X2,
 * 4X2_D3D and 4X4 for 2, 4, 8 and 16 samples (MULTI_SAMPLE_COUNT, NVC797 texture header bits 235:232, clc797tex.h;
 * written at sdk 0x69a854 to 0x69a8b0), the modes Mesa's NVK picks (image.rs choose_sample_layout, line 60), and the
 * cell (cx, cy) of each sample of those modes is NVK's SampleLayout::sa_offset (image.rs lines 107 to 140, Mesa
 * d31140da86c6), which NVK's storage image lowering uses on the hardware (su_info.rs compute_ms_table). NVK has no
 * table for 4X4 (sa_offset: todo); its cells are measured on the RTX 3090 (grid_of).
 *
 * A move goes through a single-sample host image of that larger size over the same storage (the twin), which the
 * layer's own single-sample upload and write-back fill from and store to the storage: an upload then writes each
 * sample of the multisample image from its texel (a draw shaded per sample into the image viewed in the unsigned
 * integer format of its texel size), a write-back stores each sample to its texel (a compute pass), so every bit
 * moves unchanged. Color formats only: a depth or stencil image is not moved.
 */
#include "nvn_msaa_spv.h"
#include "nvn_vk.h"

#include <stdlib.h>
#include <string.h>

/**
 * @brief Pixels of the sample grid per sample count, and the cell of each sample (cx | cy << 2 in bits 4s to 4s + 3).
 * 2, 4 and 8 samples are NVK's cells (image.rs sa_offset), and the RTX 3090 shows the same for color and stencil
 * targets: msaa_cells (switch/devtests/msaa_probe) draws each sample's index into a multisample target and reads the
 * memory back through a single-sample image over it. 16 samples are the 3090's cells of a 16-sample stencil target,
 * the only target it makes at 16; NVIDIA's texture header has one 16-sample storage mode (MULTI_SAMPLE_COUNT 4X4,
 * clc797tex.h), the one the console driver names for 16 samples.
 */
static bool grid_of(uint32_t samples, uint32_t *gw, uint32_t *gh, uint64_t *cells)
{
    switch (samples) {
    case 2:
        /* 2X1_D3D: (1 - s, 0) */
        *gw = 2, *gh = 1, *cells = 0x01u;
        return true;
    case 4:
        /* 2X2: (s & 1, s >> 1) */
        *gw = 2, *gh = 2, *cells = 0x5410u;
        return true;
    case 8:
        /* 4X2_D3D: (2,0) (1,1) (3,1) (1,0) (0,1) (0,0) (2,1) (3,0) */
        *gw = 4, *gh = 2, *cells = 0x36041752u;
        return true;
    case 16:
        /* 4X4: (2,2) (1,1) (1,2) (2,1) (0,1) (2,3) (3,2) (3,0) (1,3) (2,0) (1,0) (0,3) (0,2) (3,1) (3,3) (0,0) */
        *gw = 4, *gh = 4, *cells = 0x0f78c12d3be4695aull;
        return true;
    default:
        return false;
    }
}

bool nvn_msaa_cells(uint32_t samples, uint32_t *gw, uint32_t *gh, uint64_t *cells)
{
    return grid_of(samples, gw, gh, cells);
}

typedef struct params {
    uint32_t cells[2], gw, gh;
    int32_t layer;
    uint32_t width, height, n;
} params;

/** @brief What a multisample image keeps for its moves (vimg.msaa). */
typedef struct nvn_msaa_img {
    vimg *twin;
    VkFormat uint_format;
    /** @brief The twin's texels and the image's samples in uint_format; the image's samples in its own format. */
    VkImageView twin_uint, ms_uint, ms_own;
    /** @brief The image's layers as render targets in uint_format. */
    VkImageView *layers;
    uint32_t nlayers;
    VkDescriptorSet expand_set, gather_set, resolve_set;
} nvn_msaa_img;

#define PIPES 16u

static struct {
    bool tried, failed;
    VkDescriptorSetLayout one, two;
    VkPipelineLayout one_pl, two_pl;
    VkDescriptorPool pool;
    VkShaderModule vert, expand, gather, resolve;
    VkPipeline gather_pipe;
    struct {
        VkShaderModule fs;
        VkFormat f;
        VkSampleCountFlagBits s;
        VkPipeline p;
    } pipes[PIPES];
    uint32_t npipes;
} M;

static VkShaderModule module_of(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = size, .pCode = code };
    VkShaderModule m = NULL;
    return VK(vkCreateShaderModule)(g.dev, &ci, NULL, &m) == VK_SUCCESS ? m : NULL;
}

static bool layout_of(uint32_t n, VkShaderStageFlags stage, VkDescriptorSetLayout *dsl, VkPipelineLayout *pl)
{
    VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1, stage, NULL },
                                          { 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, stage, NULL } };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = n, .pBindings = b };
    VkPushConstantRange pr = { VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(params) };
    if (VK(vkCreateDescriptorSetLayout)(g.dev, &li, NULL, dsl) != VK_SUCCESS)
        return false;
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1,
                                       .pSetLayouts = dsl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pr };
    return VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, pl) == VK_SUCCESS;
}

static bool ready(void)
{
    if (M.tried)
        return !M.failed;
    M.tried = true;
    M.failed = true;
    if (!layout_of(1, VK_SHADER_STAGE_FRAGMENT_BIT, &M.one, &M.one_pl) ||
        !layout_of(2, VK_SHADER_STAGE_COMPUTE_BIT, &M.two, &M.two_pl))
        return false;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * 1024 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1024 } };
    VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                      .maxSets = 3 * 1024, .poolSizeCount = 2, .pPoolSizes = ps };
    if (VK(vkCreateDescriptorPool)(g.dev, &pi, NULL, &M.pool) != VK_SUCCESS)
        return false;
    M.vert = module_of(nvn_msaa_vert_spv, sizeof nvn_msaa_vert_spv);
    M.expand = module_of(nvn_msaa_expand_spv, sizeof nvn_msaa_expand_spv);
    M.gather = module_of(nvn_msaa_gather_spv, sizeof nvn_msaa_gather_spv);
    M.resolve = module_of(nvn_msaa_resolve_spv, sizeof nvn_msaa_resolve_spv);
    if (!M.vert || !M.expand || !M.gather || !M.resolve)
        return false;
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = M.gather,
                                                  .pName = "main" },
                                       .layout = M.two_pl };
    if (VK(vkCreateComputePipelines)(g.dev, NULL, 1, &ci, NULL, &M.gather_pipe) != VK_SUCCESS)
        return false;
    M.failed = false;
    return true;
}

/** @brief The full-screen draw of fs into one color target of format f at s samples, made the first time. */
static VkPipeline draw_pipe(VkShaderModule fs, VkPipelineLayout pl, VkFormat f, VkSampleCountFlagBits s)
{
    for (uint32_t i = 0; i < M.npipes; i++)
        if (M.pipes[i].fs == fs && M.pipes[i].f == f && M.pipes[i].s == s)
            return M.pipes[i].p;
    if (M.npipes == PIPES)
        return NULL;
    VkPipelineShaderStageCreateInfo st[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_VERTEX_BIT,
          .module = M.vert, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
          .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = VK_POLYGON_MODE_FILL, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = s };
    VkPipelineColorBlendAttachmentState ba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = 1, .pAttachments = &ba };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                         .colorAttachmentCount = 1, .pColorAttachmentFormats = &f };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, .pNext = &ri,
                                        .stageCount = 2, .pStages = st, .pVertexInputState = &vi,
                                        .pInputAssemblyState = &ia, .pViewportState = &vp, .pRasterizationState = &rs,
                                        .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &ds,
                                        .layout = pl };
    VkPipeline p = NULL;
    if (VK(vkCreateGraphicsPipelines)(g.dev, NULL, 1, &ci, NULL, &p) != VK_SUCCESS)
        return NULL;
    M.pipes[M.npipes].fs = fs;
    M.pipes[M.npipes].f = f;
    M.pipes[M.npipes].s = s;
    M.pipes[M.npipes++].p = p;
    return p;
}

/** @brief The unsigned integer format of n-byte texels. */
static VkFormat uint_of(uint32_t n)
{
    switch (n) {
    case 1: return VK_FORMAT_R8_UINT;
    case 2: return VK_FORMAT_R16_UINT;
    case 4: return VK_FORMAT_R32_UINT;
    case 8: return VK_FORMAT_R32G32_UINT;
    case 16: return VK_FORMAT_R32G32B32A32_UINT;
    default: return VK_FORMAT_UNDEFINED;
    }
}

static VkImageView view_of(VkImage img, VkImageViewType t, VkFormat f, VkImageAspectFlags a, uint32_t layer,
                           uint32_t layers, VkImageUsageFlags usage)
{
    VkImageViewUsageCreateInfo vu = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, .usage = usage };
    VkImageViewCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &vu, .image = img,
                                 .viewType = t, .format = f,
                                 .subresourceRange = { a, 0, 1, layer, layers } };
    VkImageView v = NULL;
    return VK(vkCreateImageView)(g.dev, &ci, NULL, &v) == VK_SUCCESS ? v : NULL;
}

static void set_images(VkDescriptorSet s, uint32_t n, const VkImageView *views)
{
    VkDescriptorImageInfo ii[2];
    VkWriteDescriptorSet w[2];
    for (uint32_t i = 0; i < n; i++) {
        ii[i] = (VkDescriptorImageInfo){ NULL, views[i], VK_IMAGE_LAYOUT_GENERAL };
        w[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = s, .dstBinding = i,
                                       .descriptorCount = 1,
                                       .descriptorType = i ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                           : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       .pImageInfo = &ii[i] };
    }
    VK(vkUpdateDescriptorSets)(g.dev, n, w, 0, NULL);
}

static VkDescriptorSet alloc_set(VkDescriptorSetLayout l)
{
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = M.pool, .descriptorSetCount = 1, .pSetLayouts = &l };
    VkDescriptorSet s = NULL;
    return VK(vkAllocateDescriptorSets)(g.dev, &ai, &s) == VK_SUCCESS ? s : NULL;
}

void nvn_msaa_free(vimg *v)
{
    nvn_msaa_img *m = v->msaa;
    if (!m)
        return;
    VkDescriptorSet sets[3] = { m->expand_set, m->gather_set, m->resolve_set };
    for (uint32_t i = 0; i < 3; i++)
        if (sets[i])
            VK(vkFreeDescriptorSets)(g.dev, M.pool, 1, &sets[i]);
    VkImageView views[3] = { m->twin_uint, m->ms_uint, m->ms_own };
    for (uint32_t i = 0; i < 3; i++)
        if (views[i])
            VK(vkDestroyImageView)(g.dev, views[i], NULL);
    for (uint32_t i = 0; i < m->nlayers; i++)
        if (m->layers[i])
            VK(vkDestroyImageView)(g.dev, m->layers[i], NULL);
    free(m->layers);
    if (m->twin)
        nvn_vk_image_drop(m->twin);
    free(m);
    v->msaa = NULL;
}

/** @brief The texture whose texels hold the samples of multisample texture d (nvn_tex_layout gives it d's bytes). */
static bool twin_desc(const nvn_tex_desc *d, nvn_tex_desc *t)
{
    uint32_t gw, gh;
    uint64_t cells;
    if (!grid_of(d->samples, &gw, &gh, &cells))
        return false;
    *t = *d;
    t->samples = 0;
    t->target = d->target == NVN_TARGET_2D_MULTISAMPLE_ARRAY ? 4u : 1u;
    t->width = d->width * gw;
    t->height = d->height * gh;
    t->levels = 1;
    return nvn_tex_layout_block(t, d->block_h) && t->size == d->size && t->layer_stride == d->layer_stride;
}

/** @brief v's moves: its twin over v's storage, views and descriptor sets, made the first time; why on failure. */
static nvn_msaa_img *moves_of(replay *r, vimg *v, const char **why)
{
    if (v->msaa)
        return v->msaa;
    *why = "the host device cannot make its passes (it needs sample rate shading and storage image writes without a "
           "format)";
    if (!g.sample_shading || !g.formatless_write || !ready())
        return NULL;
    *why = "it is not a color texture of 2, 4 or 8 samples in a format kept bit for bit";
    uint32_t bpb, bw, bh;
    VkImageAspectFlags a;
    nvn_tex_desc t;
    if (v->aspect != VK_IMAGE_ASPECT_COLOR_BIT || v->decoded || v->format != nvn_vk_format(v->d.format, &a) ||
        !nvn_tex_format(v->d.format, &bpb, &bw, &bh) || bw != 1 || bh != 1 || uint_of(bpb) == VK_FORMAT_UNDEFINED ||
        !twin_desc(&v->d, &t))
        return NULL;
    VkFormat uf = uint_of(bpb);
    VkSampleCountFlagBits s = nvn_vk_sample_bits(&v->d);
    VkImageFormatProperties ifp;
    *why = "the host device cannot draw its texel size's unsigned integer format at its sample count";
    if (VK(vkGetPhysicalDeviceImageFormatProperties)(g.phys, uf, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                     0, &ifp) != VK_SUCCESS ||
        !(ifp.sampleCounts & s))
        return NULL;
    *why = "out of host memory";
    nvn_msaa_img *m = calloc(1, sizeof *m);
    vimg *tw = m ? calloc(1, sizeof *tw) : NULL;
    if (!tw) {
        free(m);
        return NULL;
    }
    v->msaa = m;
    m->twin = tw;
    uint32_t layers = nvn_vk_image_range(v).layerCount;
    m->uint_format = uf;
    tw->d = t;
    tw->storage = v->storage;
    tw->pool_serial = v->pool_serial;
    tw->format = v->format;
    tw->aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    tw->storage_ok = true;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
                             .imageType = VK_IMAGE_TYPE_2D, .format = v->format,
                             .extent = { t.width, t.height, 1 }, .mipLevels = 1, .arrayLayers = layers,
                             .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    tw->usage = ci.usage;
    *why = "its twin image could not be made";
    VkMemoryRequirements req;
    if (VK(vkCreateImage)(g.dev, &ci, NULL, &tw->image) != VK_SUCCESS)
        goto fail;
    VK(vkGetImageMemoryRequirements)(g.dev, tw->image, &req);
    int32_t mt = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)mt };
    if (mt < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, &tw->memory) != VK_SUCCESS ||
        VK(vkBindImageMemory)(g.dev, tw->image, tw->memory, 0) != VK_SUCCESS)
        goto fail;
    m->twin_uint = view_of(tw->image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, uf, VK_IMAGE_ASPECT_COLOR_BIT, 0, layers,
                           VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT);
    m->ms_uint = view_of(v->image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, uf, VK_IMAGE_ASPECT_COLOR_BIT, 0, layers,
                         VK_IMAGE_USAGE_SAMPLED_BIT);
    m->ms_own = view_of(v->image, VK_IMAGE_VIEW_TYPE_2D_ARRAY, v->format, VK_IMAGE_ASPECT_COLOR_BIT, 0, layers,
                        VK_IMAGE_USAGE_SAMPLED_BIT);
    m->layers = calloc(layers, sizeof *m->layers);
    if (!m->twin_uint || !m->ms_uint || !m->ms_own || !m->layers)
        goto fail;
    m->nlayers = layers;
    for (uint32_t i = 0; i < layers; i++)
        if (!(m->layers[i] = view_of(v->image, VK_IMAGE_VIEW_TYPE_2D, uf, VK_IMAGE_ASPECT_COLOR_BIT, i, 1,
                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)))
            goto fail;
    *why = "the layer has no descriptor sets left for its passes";
    m->expand_set = alloc_set(M.one);
    m->gather_set = alloc_set(M.two);
    m->resolve_set = alloc_set(M.one);
    if (!m->expand_set || !m->gather_set || !m->resolve_set)
        goto fail;
    set_images(m->expand_set, 1, &m->twin_uint);
    VkImageView gv[2] = { m->ms_uint, m->twin_uint };
    set_images(m->gather_set, 2, gv);
    set_images(m->resolve_set, 1, &m->ms_own);
    nvn_vk_begin(r);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = tw->image,
                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } };
    VK(vkCmdPipelineBarrier)(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, 1, &ib);
    return m;
fail:
    nvn_msaa_free(v);
    return NULL;
}

/** @brief Records the full-screen draw of pipe with set and p into target (w by h), its old texels kept. */
static void draw_into(VkImageView target, uint32_t w, uint32_t h, VkPipeline pipe, VkPipelineLayout pl,
                      VkDescriptorSet set, const params *p)
{
    VkRenderingAttachmentInfo ca = { .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = target,
                                     .imageLayout = VK_IMAGE_LAYOUT_GENERAL, .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                     .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
    VkRect2D area = { { 0, 0 }, { w, h } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = area, .layerCount = 1,
                           .colorAttachmentCount = 1, .pColorAttachments = &ca };
    VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
    VK(vkCmdBeginRendering)(g.cb, &ri);
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    nvn_dyn_reset();
    VK(vkCmdSetViewport)(g.cb, 0, 1, &vp);
    VK(vkCmdSetScissor)(g.cb, 0, 1, &area);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &set, 0, NULL);
    VK(vkCmdPushConstants)(g.cb, pl, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *p, p);
    VK(vkCmdDraw)(g.cb, 3, 1, 0, 0);
    VK(vkCmdEndRendering)(g.cb);
}

bool nvn_msaa_upload(replay *r, vimg *v, const char **why)
{
    nvn_msaa_img *m = moves_of(r, v, why);
    if (!m)
        return false;
    *why = "its storage could not be read into its twin";
    if (!nvn_vk_fill_plain(r, m->twin))
        return false;
    VkPipeline pipe = draw_pipe(M.expand, M.one_pl, m->uint_format, nvn_vk_sample_bits(&v->d));
    *why = "the host device cannot make its upload pass";
    if (!pipe)
        return false;
    params p = { .width = v->d.width, .height = v->d.height };
    uint64_t cells = 0;
    grid_of(v->d.samples, &p.gw, &p.gh, &cells);
    p.cells[0] = (uint32_t)cells;
    p.cells[1] = (uint32_t)(cells >> 32);
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    for (uint32_t l = 0; l < m->nlayers; l++) {
        p.layer = (int32_t)l;
        draw_into(m->layers[l], v->d.width, v->d.height, pipe, M.one_pl, m->expand_set, &p);
    }
    nvn_vk_barrier(g.cb);
    return true;
}

bool nvn_msaa_write_back(replay *r, vimg *v)
{
    const char *why;
    nvn_msaa_img *m = moves_of(r, v, &why);
    if (!m)
        return false;
    params p = { .width = v->d.width, .height = v->d.height };
    uint64_t cells = 0;
    grid_of(v->d.samples, &p.gw, &p.gh, &cells);
    p.cells[0] = (uint32_t)cells;
    p.cells[1] = (uint32_t)(cells >> 32);
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.gather_pipe);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, M.two_pl, 0, 1, &m->gather_set, 0, NULL);
    VK(vkCmdPushConstants)(g.cb, M.two_pl, VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    VK(vkCmdDispatch)(g.cb, (v->d.width + 7) / 8, (v->d.height + 7) / 8, m->nlayers);
    nvn_vk_barrier(g.cb);
    return nvn_vk_store_plain(r, m->twin);
}

/**
 * @brief The samples Downsample averages for a source of samples samples (indices in bits 4i to 4i + 3) and how many.
 *
 * The driver's Downsample (SDK 22.2.8, sdk 0x650a00, its 2D engine blit at 0x6505f0) is one 2D engine blit (NV902D,
 * cl902d.h) over the smaller of the two textures' sizes, in texels of the twin layout: SET_PIXELS_FROM_MEMORY_
 * SAMPLE_MODE FILTER_BILINEAR with ORIGIN_CENTER, DU_DX = gw, DV_DY = gh, SRC_X0 = (gw - 1) / 2 and SRC_Y0 =
 * (gh - 1) / 2. What ORIGIN_CENTER means is in Mesa's nvc0 2D blit (src/gallium/drivers/nouveau/nvc0/nvc0_surface.c
 * nvc0_blit_eng2d, lines 1523 to 1600, Mesa d31140da86c6): a same-size copy there takes ORIGIN_CENTER with SRC_X0 at
 * the source texel's integer coordinate, so under it an integer coordinate is that texel's center, and its own
 * resolve takes ORIGIN_CORNER with SRC_X0 = x * gw + gw / 2 for the same point (lines 1586 to 1594). Pixel x then
 * filters at x * gw + (gw - 1) / 2 texel centers, half-way between grid columns gw / 2 - 1 and gw / 2 (rows the
 * same; with gh 1, row 0 alone), each tap at weight one half: every sample for 2X1_D3D and 2X2, and for 4X2_D3D the
 * four of the middle two columns, cells (1, 0), (2, 0), (1, 1) and (2, 1), samples 3, 0, 1 and 6 by NVK's cells,
 * and for 4X4 the middle 2x2 cells (1, 1), (2, 1), (1, 2) and (2, 2), samples 1, 3, 2 and 0 (grid_of).
 * Mesa keeps 8-sample resolves off this blit for that reason (nvc0_surface.c line 1744).
 */
static bool downsample_taps(uint32_t samples, uint32_t *taps, uint32_t *n)
{
    switch (samples) {
    case 2: *taps = 0x10u, *n = 2; return true;
    case 4: *taps = 0x3210u, *n = 4; return true;
    case 8: *taps = 0x6103u, *n = 4; return true;
    case 16: *taps = 0x3210u, *n = 4; return true;
    default: return false;
    }
}

bool nvn_msaa_downsample_taps(uint32_t samples, uint32_t *taps, uint32_t *n) { return downsample_taps(samples, taps, n); }

bool nvn_msaa_resolve(replay *r, vimg *src, VkImageView target, VkFormat f, uint32_t w, uint32_t h, const char **why)
{
    params p = { 0 };
    *why = "it has no known downsample taps";
    if (!downsample_taps(src->d.samples, &p.cells[0], &p.n))
        return false;
    nvn_msaa_img *m = moves_of(r, src, why);
    if (!m)
        return false;
    VkPipeline pipe = draw_pipe(M.resolve, M.one_pl, f, VK_SAMPLE_COUNT_1_BIT);
    *why = "the host device cannot make its pass";
    if (!pipe)
        return false;
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    draw_into(target, w, h, pipe, M.one_pl, m->resolve_set, &p);
    nvn_vk_barrier(g.cb);
    return true;
}
