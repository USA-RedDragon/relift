/**
 * @file nvn_untile.c
 * @brief Texture bytes moved between the guest's storage and a host image on the GPU (untile.comp): an upload reads
 * the storage through the buffer the storage's pool is imported as, a write-back writes it there, each texel's bytes
 * unchanged, in the guest's layout (block-linear or pitch-linear), with no CPU pass over the texels and no staging
 * copy.
 *
 * Only what the shader can move bit for bit goes this way: color images with storage usage, of 4, 8 or 16 bytes
 * per texel, one sample, in a pool the device imported, with a 4-byte-aligned storage. An upload of an image a shader
 * cannot store to (a depth format, a compressed one) of 4, 8 or 16 bytes per texel or block goes from the storage
 * into a packed device buffer on the GPU and from there into the image by a buffer copy (nvn_untile_upload_packed).
 * Anything else falls back to the CPU path (nvn_tex_copy through a staging buffer).
 */
#include "nvn_vk.h"
#include "nvn_untile_spv.h"
#include "spirvval.h"

#include <stdlib.h>
#include <string.h>

/** @brief Descriptor sets and views a recorded move uses, freed once the batch that uses them has run. */
#define DEAD_MAX 512u

typedef struct params {
    uint32_t base[2];
    uint32_t width, height, depth;
    uint32_t gbh, gbd, gobs_w, block_rows;
    uint32_t pitch, maxwell;
    uint32_t pad;
    uint32_t dst[2];
} params;

/** @brief A device buffer a recorded packed upload goes through, freed once the batch that uses it has run. */
typedef struct packed_buf {
    VkBuffer buf;
    VkDeviceMemory mem;
} packed_buf;

#define PACKED_MAX 256u

static struct {
    bool tried, failed;
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl;
    VkDescriptorPool pool;
    VkPipeline pipe[3][2][3];
    bool pipe_failed[3][2][3];
    /** @brief What each slot's batch holds until it finished (nvn_untile_release); cur is the recording slot. */
    VkDescriptorSet dead_sets[NVN_SLOTS][DEAD_MAX];
    VkImageView dead_views[NVN_SLOTS][DEAD_MAX];
    uint32_t ndead[NVN_SLOTS];
    packed_buf dead_bufs[NVN_SLOTS][PACKED_MAX];
    uint32_t ndead_bufs[NVN_SLOTS];
    uint32_t cur;
    uint64_t moves;
} U;

static bool ready(void)
{
    if (U.tried)
        return !U.failed;
    U.tried = true;
    VkDescriptorSetLayoutBinding b = { .binding = 0,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 1,
                                            .pBindings = &b };
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(params) };
    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, .descriptorCount = DEAD_MAX * NVN_SLOTS };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
                                       .maxSets = DEAD_MAX * NVN_SLOTS,
                                       .poolSizeCount = 1,
                                       .pPoolSizes = &ps };
    U.failed = VK(vkCreateDescriptorSetLayout)(g.dev, &dli, NULL, &U.dsl) != VK_SUCCESS;
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1,
                                       .pSetLayouts = &U.dsl,
                                       .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &pcr };
    U.failed = U.failed || VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &U.pl) != VK_SUCCESS ||
               VK(vkCreateDescriptorPool)(g.dev, &dpi, NULL, &U.pool) != VK_SUCCESS;
    return !U.failed;
}

static VkPipeline pipeline(uint32_t tile, uint32_t volume, uint32_t words_log2)
{
    VkPipeline *p = &U.pipe[tile][volume][words_log2];
    if (*p || U.pipe_failed[tile][volume][words_log2])
        return *p;
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = nvn_untile_spv[tile][volume][words_log2].size,
                                     .pCode = nvn_untile_spv[tile][volume][words_log2].code };
    VkShaderModule sm = 0;
    rl_spirv_check(smi.pCode, smi.codeSize / 4, "an untile program");
    if (VK(vkCreateShaderModule)(g.dev, &smi, NULL, &sm) == VK_SUCCESS) {
        VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                            .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                       .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                       .module = sm,
                                                       .pName = "main" },
                                            .layout = U.pl };
        if (VK(vkCreateComputePipelines)(g.dev, 0, 1, &cpi, NULL, p) != VK_SUCCESS)
            *p = 0;
        VK(vkDestroyShaderModule)(g.dev, sm, NULL);
    }
    U.pipe_failed[tile][volume][words_log2] = !*p;
    return *p;
}

void nvn_untile_release(uint32_t slot)
{
    uint32_t cur = U.cur;
    U.cur = slot;
    for (uint32_t i = 0; i < U.ndead[U.cur]; i++) {
        VK(vkFreeDescriptorSets)(g.dev, U.pool, 1, &U.dead_sets[U.cur][i]);
        VK(vkDestroyImageView)(g.dev, U.dead_views[U.cur][i], NULL);
    }
    U.ndead[U.cur] = 0;
    for (uint32_t i = 0; i < U.ndead_bufs[U.cur]; i++) {
        VK(vkDestroyBuffer)(g.dev, U.dead_bufs[U.cur][i].buf, NULL);
        VK(vkFreeMemory)(g.dev, U.dead_bufs[U.cur][i].mem, NULL);
    }
    U.ndead_bufs[U.cur] = 0;
    U.cur = cur;
}

void nvn_untile_slot(uint32_t slot) { U.cur = slot; }

uint64_t nvn_untile_moves(void) { return U.moves; }

/**
 * @brief The device address of guest bytes [guest, guest + size) through an imported pool; 0 when none holds them or
 * the pool is a mirror (nvn_mirror.c), whose bytes the moves would read stale and write without a readback.
 */
static uint64_t guest_address(uint64_t guest, uint64_t size)
{
    void *buf;
    uint64_t off;
    if (!nvn_guest_buffer(guest, size, &buf, &off) || nvn_mirror_buffer(buf))
        return 0;
    VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = (VkBuffer)buf };
    uint64_t a = VK(vkGetBufferDeviceAddress)(g.dev, &ai);
    return a ? a + off : 0;
}

static bool movable(const vimg *v);

bool nvn_untile_can(const vimg *v)
{
    uint64_t a;
    return movable(v) && (a = guest_address(v->storage, v->d.size)) != 0 && a % 4 == 0;
}

/** @brief The format checks of nvn_untile_can, without the storage's: v's texels can move as 4, 8 or 16-byte words. */
static bool movable(const vimg *v)
{
    const nvn_tex_desc *d = &v->d;
    uint32_t bpb, bw, bh;
    if (!v->storage_ok || v->decoded || v->aspect != VK_IMAGE_ASPECT_COLOR_BIT || d->samples > 1 ||
        d->target == NVN_TARGET_BUFFER || !nvn_tex_format(d->format, &bpb, &bw, &bh) || bw != 1 || bh != 1 ||
        (bpb != 4 && bpb != 8 && bpb != 16) || d->size > UINT32_MAX || d->layer_stride % 4)
        return false;
    for (uint32_t l = 0; l < (d->levels ? d->levels : 1) && l < NVN_TEX_MAX_LEVELS; l++)
        if (d->level_offset[l] % 4)
            return false;
    return ready();
}

/** @brief nvn_untile_move with the storage's first byte at device address addr. */
static bool move_at(replay *r, vimg *v, uint64_t addr, bool tile)
{
    const nvn_tex_desc *d = &v->d;
    uint32_t bpb, bw, bh;
    nvn_tex_format(d->format, &bpb, &bw, &bh);
    uint32_t wl = bpb == 4 ? 0 : bpb == 8 ? 1 : 2;
    static const VkFormat view_format[3] = { VK_FORMAT_R32_UINT, VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32B32A32_UINT };
    bool volume = d->target == NVN_TARGET_3D;
    VkPipeline pipe = pipeline(tile, volume, wl);
    if (!pipe || !addr || addr % 4)
        return false;
    uint32_t levels = d->levels ? d->levels : 1;
    uint32_t layers = volume ? 1 : d->target == NVN_TARGET_CUBEMAP ? 6 : d->depth ? d->depth : 1;
    if (levels > NVN_TEX_MAX_LEVELS)
        levels = NVN_TEX_MAX_LEVELS;
    if (U.ndead[U.cur] + levels * layers > DEAD_MAX && !nvn_vk_flush(r))
        return false;
    if (levels * layers > DEAD_MAX)
        return false;
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t w = d->width >> l ? d->width >> l : 1, h = d->height >> l ? d->height >> l : 1;
        uint32_t dl = volume ? (d->depth >> l ? d->depth >> l : 1) : 1;
        uint32_t gbh = 0, gbd = 1;
        if (d->block_h)
            nvn_tex_level_blocks(d, l, &gbh, &gbd);
        for (uint32_t k = 0; k < layers; k++) {
            VkImageView view = nvn_vk_storage_view(v, volume ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D,
                                                   view_format[wl], l, k, 1);
            if (!view)
                return false;
            VkDescriptorSet set;
            VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                               .descriptorPool = U.pool,
                                               .descriptorSetCount = 1,
                                               .pSetLayouts = &U.dsl };
            if (VK(vkAllocateDescriptorSets)(g.dev, &ai, &set) != VK_SUCCESS) {
                VK(vkDestroyImageView)(g.dev, view, NULL);
                return false;
            }
            U.dead_sets[U.cur][U.ndead[U.cur]] = set;
            U.dead_views[U.cur][U.ndead[U.cur]++] = view;
            VkDescriptorImageInfo ii = { .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL };
            VkWriteDescriptorSet wd = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstSet = set,
                                        .descriptorCount = 1,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                        .pImageInfo = &ii };
            VK(vkUpdateDescriptorSets)(g.dev, 1, &wd, 0, NULL);
            uint64_t base = addr + d->layer_stride * k + d->level_offset[l];
            uint32_t gobs_w = (w * bpb + 63) / 64;
            params pc = { { (uint32_t)base, (uint32_t)(base >> 32) },
                          w,
                          h,
                          dl,
                          gbh,
                          gbd,
                          gobs_w,
                          gbh ? (h + 8 * gbh - 1) / (8 * gbh) : 0,
                          (uint32_t)(d->stride ? d->stride : (uint64_t)w * bpb),
                          nvn_generation() == 1,
                          0,
                          { 0, 0 } };
            VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, U.pl, 0, 1, &set, 0, NULL);
            VK(vkCmdPushConstants)(g.cb, U.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            VK(vkCmdDispatch)(g.cb, (w + 7) / 8, (h + 7) / 8, dl);
        }
    }
    nvn_vk_barrier(g.cb);
    U.moves++;
    return true;
}

bool nvn_untile_move(replay *r, vimg *v, bool tile)
{
    if (tile)
        nvn_vk_mark_write_range(v->storage, v->d.size);
    return movable(v) && move_at(r, v, guest_address(v->storage, v->d.size), tile);
}

/** @brief A device-local buffer of size bytes with a device address, kept in U.dead_bufs until the batch has run; 0 when none. */
static uint64_t packed_new(uint64_t size, VkBuffer *out)
{
    packed_buf b = { 0 };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT };
    if (U.ndead_bufs[U.cur] == PACKED_MAX || VK(vkCreateBuffer)(g.dev, &bci, NULL, &b.buf) != VK_SUCCESS)
        return 0;
    VkMemoryRequirements req;
    VK(vkGetBufferMemoryRequirements)(g.dev, b.buf, &req);
    int32_t t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateFlagsInfo fl = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                     .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .pNext = &fl,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)t };
    if (t < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, &b.mem) != VK_SUCCESS) {
        VK(vkDestroyBuffer)(g.dev, b.buf, NULL);
        return 0;
    }
    VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b.buf };
    uint64_t a = VK(vkBindBufferMemory)(g.dev, b.buf, b.mem, 0) == VK_SUCCESS ? VK(vkGetBufferDeviceAddress)(g.dev, &ai) : 0;
    U.dead_bufs[U.cur][U.ndead_bufs[U.cur]++] = b;
    *out = b.buf;
    return a;
}

/** @brief Whether nvn_untile_upload_packed can take v: one aspect, 4, 8 or 16 bytes a texel or block, aligned layout. */
static bool packable(const vimg *v)
{
    const nvn_tex_desc *d = &v->d;
    uint32_t bpb, bw, bh;
    if (v->decoded || (v->aspect != VK_IMAGE_ASPECT_COLOR_BIT && v->aspect != VK_IMAGE_ASPECT_DEPTH_BIT) ||
        d->samples > 1 || d->target == NVN_TARGET_BUFFER || !nvn_tex_format(d->format, &bpb, &bw, &bh) ||
        (bpb != 4 && bpb != 8 && bpb != 16) || d->size > UINT32_MAX || d->layer_stride % 4 || d->stride % 4)
        return false;
    for (uint32_t l = 0; l < (d->levels ? d->levels : 1) && l < NVN_TEX_MAX_LEVELS; l++)
        if (d->level_offset[l] % 4)
            return false;
    return ready();
}

/** @brief nvn_untile_upload_packed with the storage's first byte at device address addr. */
static bool packed_at(replay *r, vimg *v, uint64_t addr)
{
    const nvn_tex_desc *d = &v->d;
    uint32_t bpb, bw, bh;
    nvn_tex_format(d->format, &bpb, &bw, &bh);
    uint32_t wl = bpb == 4 ? 0 : bpb == 8 ? 1 : 2;
    bool volume = d->target == NVN_TARGET_3D;
    VkPipeline pipe = pipeline(2, 0, wl);
    if (!pipe || !addr || addr % 4)
        return false;
    uint32_t levels = d->levels ? d->levels : 1;
    uint32_t layers = volume ? 1 : d->target == NVN_TARGET_CUBEMAP ? 6 : d->depth ? d->depth : 1;
    if (levels > NVN_TEX_MAX_LEVELS)
        levels = NVN_TEX_MAX_LEVELS;
    uint64_t total = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t w = d->width >> l ? d->width >> l : 1, h = d->height >> l ? d->height >> l : 1;
        uint32_t dl = volume ? (d->depth >> l ? d->depth >> l : 1) : 1;
        total += (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * dl * bpb * layers;
    }
    if (U.ndead_bufs[U.cur] == PACKED_MAX && !nvn_vk_flush(r))
        return false;
    VkBuffer buf;
    uint64_t dst = packed_new(total, &buf);
    if (!dst)
        return false;
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    VkBufferImageCopy bic[NVN_TEX_MAX_LEVELS];
    uint64_t at = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t w = d->width >> l ? d->width >> l : 1, h = d->height >> l ? d->height >> l : 1;
        uint32_t dl = volume ? (d->depth >> l ? d->depth >> l : 1) : 1;
        uint32_t wb = (w + bw - 1) / bw, hb = (h + bh - 1) / bh;
        uint32_t gbh = 0, gbd = 1;
        if (d->block_h)
            nvn_tex_level_blocks(d, l, &gbh, &gbd);
        uint64_t layer_bytes = (uint64_t)wb * hb * dl * bpb;
        for (uint32_t k = 0; k < layers; k++) {
            uint64_t base = addr + d->layer_stride * k + d->level_offset[l], out = dst + at + layer_bytes * k;
            params pc = { { (uint32_t)base, (uint32_t)(base >> 32) },
                          wb,
                          hb,
                          dl,
                          gbh,
                          gbd,
                          (wb * bpb + 63) / 64,
                          gbh ? (hb + 8 * gbh - 1) / (8 * gbh) : 0,
                          (uint32_t)(d->stride ? d->stride : (uint64_t)wb * bpb),
                          nvn_generation() == 1,
                          0,
                          { (uint32_t)out, (uint32_t)(out >> 32) } };
            VK(vkCmdPushConstants)(g.cb, U.pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            VK(vkCmdDispatch)(g.cb, (wb + 7) / 8, (hb + 7) / 8, dl);
        }
        bic[l] = (VkBufferImageCopy){ .bufferOffset = at,
                                      .imageSubresource = { .aspectMask = v->aspect, .mipLevel = l, .layerCount = layers },
                                      .imageExtent = { w, h, dl } };
        at += layer_bytes * layers;
    }
    nvn_vk_barrier(g.cb);
    VK(vkCmdCopyBufferToImage)(g.cb, buf, v->image, VK_IMAGE_LAYOUT_GENERAL, levels, bic);
    nvn_vk_barrier(g.cb);
    U.moves++;
    return true;
}

bool nvn_untile_upload_packed(replay *r, vimg *v)
{
    return packable(v) && packed_at(r, v, guest_address(v->storage, v->d.size));
}

/** @brief A host-visible buffer of size bytes with a device address, mapped, for nvn_untile_check. */
typedef struct dev_buf {
    VkBuffer buf;
    VkDeviceMemory mem;
    uint8_t *map;
    uint64_t addr;
} dev_buf;

static bool dev_buf_new(dev_buf *b, uint64_t size)
{
    memset(b, 0, sizeof *b);
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT };
    if (VK(vkCreateBuffer)(g.dev, &bci, NULL, &b->buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    VK(vkGetBufferMemoryRequirements)(g.dev, b->buf, &req);
    int32_t t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkMemoryAllocateFlagsInfo fl = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                     .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .pNext = &fl,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)t };
    void *p = NULL;
    if (t < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, &b->mem) != VK_SUCCESS ||
        VK(vkBindBufferMemory)(g.dev, b->buf, b->mem, 0) != VK_SUCCESS ||
        VK(vkMapMemory)(g.dev, b->mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
        return false;
    b->map = p;
    VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = b->buf };
    b->addr = VK(vkGetBufferDeviceAddress)(g.dev, &ai);
    return b->addr != 0;
}

static void dev_buf_free(dev_buf *b)
{
    if (b->buf)
        VK(vkDestroyBuffer)(g.dev, b->buf, NULL);
    if (b->mem)
        VK(vkFreeMemory)(g.dev, b->mem, NULL);
}

/** @brief An image for d made as the layer makes texture images (mutable, storage through an extended usage). */
static bool check_image(vimg *v, const nvn_tex_desc *d)
{
    memset(v, 0, sizeof *v);
    v->d = *d;
    v->format = nvn_vk_format(d->format, &v->aspect);
    v->storage_ok = true;
    bool volume = d->target == NVN_TARGET_3D;
    uint32_t layers = volume ? 1 : d->target == NVN_TARGET_CUBEMAP ? 6 : d->depth ? d->depth : 1;
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
                             .imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                             .format = v->format,
                             .extent = { d->width, d->height, volume ? d->depth : 1 },
                             .mipLevels = d->levels ? d->levels : 1,
                             .arrayLayers = layers,
                             .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    if (VK(vkCreateImage)(g.dev, &ci, NULL, &v->image) != VK_SUCCESS)
        return false;
    v->usage = ci.usage;
    VkMemoryRequirements req;
    VK(vkGetImageMemoryRequirements)(g.dev, v->image, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    return VK(vkAllocateMemory)(g.dev, &mai, NULL, &v->memory) == VK_SUCCESS &&
           VK(vkBindImageMemory)(g.dev, v->image, v->memory, 0) == VK_SUCCESS;
}

/**
 * @brief One case of nvn_untile_check: random guest bytes in a buffer are moved into an image and back out into a
 * zeroed buffer. The image's texels (copied out by the device) must equal the texels nvn_tex_copy reads from the
 * first buffer, and every byte of the second buffer must equal what the CPU write-back (nvn_tex_copy into a zeroed
 * storage) leaves there, padding included, so a CPU reader sees the same bytes either way.
 */
static bool check_case(nvn_tex_desc d, uint32_t seed)
{
    if (!nvn_tex_layout(&d))
        return false;
    uint32_t bpb, bw, bh;
    nvn_tex_format(d.format, &bpb, &bw, &bh);
    bool volume = d.target == NVN_TARGET_3D;
    uint32_t levels = d.levels ? d.levels : 1;
    uint32_t layers = volume ? 1 : d.target == NVN_TARGET_CUBEMAP ? 6 : d.depth ? d.depth : 1;
    uint64_t texels = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint64_t w = d.width >> l ? d.width >> l : 1, h = d.height >> l ? d.height >> l : 1;
        uint64_t dl = volume ? (d.depth >> l ? d.depth >> l : 1) : 1;
        texels += w * h * dl * layers;
    }
    dev_buf in, out, lin;
    vimg v;
    bool ok = dev_buf_new(&in, d.size) && dev_buf_new(&out, d.size) && dev_buf_new(&lin, texels * bpb) && check_image(&v, &d);
    uint8_t *want = ok ? malloc(texels * bpb) : NULL, *got = ok ? malloc(texels * bpb) : NULL;
    ok = ok && want && got;
    if (ok) {
        uint32_t x = seed * 2654435761u + 1;
        for (uint64_t i = 0; i < d.size; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            in.map[i] = (uint8_t)x;
        }
        memset(out.map, 0, d.size);
    }
    replay r = { 0 };
    VkBufferImageCopy bic[NVN_TEX_MAX_LEVELS];
    uint64_t at = 0;
    for (uint32_t l = 0; ok && l < levels; l++) {
        uint32_t w = d.width >> l ? d.width >> l : 1, h = d.height >> l ? d.height >> l : 1;
        uint32_t dl = volume ? (d.depth >> l ? d.depth >> l : 1) : 1;
        bic[l] = (VkBufferImageCopy){ .bufferOffset = at,
                                      .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, l, 0, layers },
                                      .imageExtent = { w, h, dl } };
        nvn_region rg = { 0, 0, 0, (int32_t)w, (int32_t)h, (int32_t)dl };
        for (uint32_t k = 0; ok && k < layers; k++)
            ok = nvn_tex_copy(&d, in.map, l, k, &rg, want + at + (uint64_t)w * h * dl * bpb * k, false);
        at += (uint64_t)w * h * dl * bpb * layers;
    }
    if (ok) {
        nvn_vk_begin(&r);
        VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = v.image,
                                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, layers } };
        VK(vkCmdPipelineBarrier)(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &ib);
        ok = move_at(&r, &v, in.addr, false) && move_at(&r, &v, out.addr, true);
        if (ok) {
            VK(vkCmdCopyImageToBuffer)(g.cb, v.image, VK_IMAGE_LAYOUT_GENERAL, lin.buf, levels, bic);
            nvn_vk_barrier(g.cb);
        }
        ok = nvn_vk_flush(&r) && ok;
    }
    ok = ok && memcmp(lin.map, want, texels * bpb) == 0;
    at = 0;
    for (uint32_t l = 0; ok && l < levels; l++) {
        uint32_t w = d.width >> l ? d.width >> l : 1, h = d.height >> l ? d.height >> l : 1;
        uint32_t dl = volume ? (d.depth >> l ? d.depth >> l : 1) : 1;
        nvn_region rg = { 0, 0, 0, (int32_t)w, (int32_t)h, (int32_t)dl };
        for (uint32_t k = 0; ok && k < layers; k++)
            ok = nvn_tex_copy(&d, out.map, l, k, &rg, got + at + (uint64_t)w * h * dl * bpb * k, false);
        at += (uint64_t)w * h * dl * bpb * layers;
    }
    ok = ok && memcmp(got, want, texels * bpb) == 0;
    uint8_t *cpu = ok ? calloc(1, d.size) : NULL;
    ok = ok && cpu;
    at = 0;
    for (uint32_t l = 0; ok && l < levels; l++) {
        uint32_t w = d.width >> l ? d.width >> l : 1, h = d.height >> l ? d.height >> l : 1;
        uint32_t dl = volume ? (d.depth >> l ? d.depth >> l : 1) : 1;
        nvn_region rg = { 0, 0, 0, (int32_t)w, (int32_t)h, (int32_t)dl };
        for (uint32_t k = 0; ok && k < layers; k++)
            ok = nvn_tex_copy(&d, cpu, l, k, &rg, want + at + (uint64_t)w * h * dl * bpb * k, true);
        at += (uint64_t)w * h * dl * bpb * layers;
    }
    ok = ok && memcmp(cpu, out.map, d.size) == 0;
    free(cpu);
    free(want);
    free(got);
    if (v.image)
        VK(vkDestroyImage)(g.dev, v.image, NULL);
    if (v.memory)
        VK(vkFreeMemory)(g.dev, v.memory, NULL);
    dev_buf_free(&in);
    dev_buf_free(&out);
    dev_buf_free(&lin);
    return ok;
}

/**
 * @brief One packed-upload case of nvn_untile_check: random guest bytes (depth values in [0, 1) for a depth format,
 * which a buffer copy into a depth image only defines there) go into an image through nvn_untile_upload_packed, and
 * the image's texels or blocks, copied out by the device, must equal what nvn_tex_copy reads from the storage.
 */
static bool check_packed_case(nvn_tex_desc d, uint32_t seed)
{
    if (!nvn_tex_layout(&d))
        return false;
    uint32_t bpb, bw, bh;
    nvn_tex_format(d.format, &bpb, &bw, &bh);
    bool volume = d.target == NVN_TARGET_3D;
    uint32_t levels = d.levels ? d.levels : 1;
    uint32_t layers = volume ? 1 : d.target == NVN_TARGET_CUBEMAP ? 6 : d.depth ? d.depth : 1;
    uint64_t bytes = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint64_t w = d.width >> l ? d.width >> l : 1, h = d.height >> l ? d.height >> l : 1;
        uint64_t dl = volume ? (d.depth >> l ? d.depth >> l : 1) : 1;
        bytes += (w + bw - 1) / bw * ((h + bh - 1) / bh) * dl * layers * bpb;
    }
    dev_buf in, lin;
    vimg v;
    memset(&v, 0, sizeof v);
    v.d = d;
    v.format = nvn_vk_format(d.format, &v.aspect);
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .imageType = volume ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
                             .format = v.format,
                             .extent = { d.width, d.height, volume ? d.depth : 1 },
                             .mipLevels = levels,
                             .arrayLayers = layers,
                             .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                      VK_IMAGE_USAGE_SAMPLED_BIT,
                             .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED };
    bool ok = dev_buf_new(&in, d.size) && dev_buf_new(&lin, bytes) && VK(vkCreateImage)(g.dev, &ci, NULL, &v.image) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements req;
        VK(vkGetImageMemoryRequirements)(g.dev, v.image, &req);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = req.size,
                                     .memoryTypeIndex = (uint32_t)nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
        ok = VK(vkAllocateMemory)(g.dev, &mai, NULL, &v.memory) == VK_SUCCESS &&
             VK(vkBindImageMemory)(g.dev, v.image, v.memory, 0) == VK_SUCCESS;
    }
    uint8_t *want = ok ? malloc(bytes) : NULL;
    ok = ok && want;
    if (ok) {
        uint32_t x = seed * 2654435761u + 1;
        for (uint64_t i = 0; i < d.size; i++) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            in.map[i] = (uint8_t)x;
        }
        if (v.aspect == VK_IMAGE_ASPECT_DEPTH_BIT)
            for (uint64_t i = 0; i + 4 <= d.size; i += 4) {
                float f = (float)(rl_ld32(in.map, i) >> 8) / 16777216.0f;
                memcpy(in.map + i, &f, 4);
            }
    }
    VkBufferImageCopy bic[NVN_TEX_MAX_LEVELS];
    uint64_t at = 0;
    for (uint32_t l = 0; ok && l < levels; l++) {
        uint32_t w = d.width >> l ? d.width >> l : 1, h = d.height >> l ? d.height >> l : 1;
        uint32_t dl = volume ? (d.depth >> l ? d.depth >> l : 1) : 1;
        uint64_t layer_bytes = (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * dl * bpb;
        bic[l] = (VkBufferImageCopy){ .bufferOffset = at,
                                      .imageSubresource = { v.aspect, l, 0, layers },
                                      .imageExtent = { w, h, dl } };
        nvn_region rg = { 0, 0, 0, (int32_t)w, (int32_t)h, (int32_t)dl };
        for (uint32_t k = 0; ok && k < layers; k++)
            ok = nvn_tex_copy(&d, in.map, l, k, &rg, want + at + layer_bytes * k, false);
        at += layer_bytes * layers;
    }
    replay r = { 0 };
    if (ok) {
        nvn_vk_begin(&r);
        VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = v.image,
                                    .subresourceRange = { v.aspect, 0, levels, 0, layers } };
        VK(vkCmdPipelineBarrier)(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &ib);
        ok = packable(&v) && packed_at(&r, &v, in.addr);
        if (ok) {
            VK(vkCmdCopyImageToBuffer)(g.cb, v.image, VK_IMAGE_LAYOUT_GENERAL, lin.buf, levels, bic);
            nvn_vk_barrier(g.cb);
        }
        ok = nvn_vk_flush(&r) && ok;
    }
    ok = ok && memcmp(lin.map, want, bytes) == 0;
    free(want);
    if (v.image)
        VK(vkDestroyImage)(g.dev, v.image, NULL);
    if (v.memory)
        VK(vkFreeMemory)(g.dev, v.memory, NULL);
    dev_buf_free(&in);
    dev_buf_free(&lin);
    return ok;
}

int nvn_untile_check(uint32_t seed)
{
    if (!nvn_vk_ready() || !ready())
        return -1;
    static const nvn_tex_desc cases[] = {
        { .target = 1, .format = 56, .width = 2560, .height = 1440, .levels = 1 },
        { .target = 1, .format = 37, .width = 301, .height = 77, .levels = 4 },
        { .target = 1, .format = 63, .width = 1920, .height = 1080, .levels = 1 },
        { .target = NVN_TARGET_3D, .format = 11, .width = 120, .height = 68, .depth = 16, .levels = 1 },
        { .target = NVN_TARGET_3D, .format = 41, .width = 33, .height = 17, .depth = 9, .levels = 3 },
        { .target = 4, .format = 46, .width = 37, .height = 21, .depth = 3, .levels = 2 },
        { .target = 1, .format = 37, .width = 300, .height = 50, .levels = 1, .flags = 1u << 4 },
    };
    for (uint32_t i = 0; i < sizeof cases / sizeof cases[0]; i++)
        if (!check_case(cases[i], seed + i))
            return (int)i + 1;
    static const nvn_tex_desc packed[] = {
        { .target = 1, .format = 52, .width = 1920, .height = 1080, .levels = 1 },
        { .target = 1, .format = 70, .width = 301, .height = 77, .levels = 4 },
        { .target = 1, .format = 75, .width = 256, .height = 256, .levels = 9 },
        { .target = 4, .format = 75, .width = 64, .height = 32, .depth = 3, .levels = 2 },
        { .target = 1, .format = 70, .width = 200, .height = 40, .levels = 1, .flags = 1u << 4 },
    };
    for (uint32_t i = 0; i < sizeof packed / sizeof packed[0]; i++)
        if (!check_packed_case(packed[i], seed + 50 + i))
            return 50 + (int)i;
    vimg outside = { .storage = 0x1000, .d = cases[0], .aspect = VK_IMAGE_ASPECT_COLOR_BIT, .storage_ok = true };
    nvn_tex_layout(&outside.d);
    return nvn_untile_can(&outside) ? 100 : 0;
}
