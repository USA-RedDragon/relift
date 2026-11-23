/**
 * @file nvn_texset.c
 * @brief The texture set (descriptor set 2) the translated programs sample
 * through: the guest's texture and sampler pools as descriptor arrays
 * indexed by pool id.
 *
 * Binding 0 holds the sampler pool, binding 1 + 10 kind + dim the textures
 * of numeric kind kind (0 float, 1 signed, 2 unsigned) seen as dimension
 * dim (the translator's Dim), binding 31 a storage buffer with a word per
 * texture id (its Dim in bits 0..8, its numeric kind from bit 8, which the
 * programs select the array of a bound handle with), and binding 32 + 8 kind + dim the images registered with
 * nvnTexturePoolRegisterImage as storage images of the translator's
 * ImageKind kind and ImageDim dim: formatted kinds through a view of the
 * image's own numeric kind, raw kinds through an unsigned view of the
 * access size, atomics through R32ui. One set serves every draw; an element is written the
 * first time a draw reads its handle, and written again when the pool
 * entry changed. Elements no draw named are left unwritten (the arrays are
 * partially bound), and the programs only read the ones their handles name.
 */
#include "nvn_vk.h"
#include "nvn_slots.h"

#include <stdlib.h>

#define SAMPLERS 4096u
#define KINDS 3u
#define DIMS 9u
#define DIMS_BINDING 31u
#define IMAGE_KINDS 9u
#define IMAGE_DIMS 6u
#define IMAGE_BINDING(k, d) (32u + 8u * (k) + (d))

enum { IK_FLOAT, IK_SINT, IK_UINT, IK_RAW8, IK_RAW16, IK_RAW32, IK_RAW64, IK_RAW128, IK_ATOMIC32 };
enum { ID_1D, ID_BUFFER, ID_1D_ARRAY, ID_2D, ID_2D_ARRAY, ID_3D };

enum { DIM_1D, DIM_2D, DIM_3D, DIM_CUBE, DIM_1D_ARRAY, DIM_2D_ARRAY, DIM_CUBE_ARRAY, DIM_2D_MS, DIM_2D_MS_ARRAY };

static struct {
    bool tried, ok;
    /**
     * @brief Texture ids the arrays hold: as many as the device's
     * update-after-bind sampled image budget allows across the kind and
     * dimension arrays, at most the 2^20 a handle can name.
     */
    uint32_t ntex;
    /**
     * @brief The storage image arrays the layout declares (bit k * IMAGE_DIMS + d for kind k and dimension d): those
     * the shader table's programs use (all when the table does not say).
     */
    uint64_t img_used;
    /**
     * @brief Slots that also hold storage images: as many as the device's update-after-bind storage image budget
     * allows across the declared arrays, at most ntex.
     */
    uint32_t nimg;
    /**
     * @brief Texture ids and slots. A handle names one of 2^20 texture ids, the arrays hold ntex slots: the ids below
     * pinned are their own slots (a program that reads a handle out of memory names the id, which the host cannot
     * rewrite). pinned is all but a sixteenth of the slots, so every id of a texture pool up to that size (an id is an
     * index into the pool) has a slot of its own. The others share the slots from pinned on: id_slot maps an id to its
     * slot plus one and slot_id a slot to its id plus one (0: free). Slots are handed out from the free stack, then in turn; taking one that holds a
     * texture waits for the work that may still use it. Everything below is indexed by slot, and the handle a program
     * reads from a constant bank has its id replaced by the slot.
     */
    nvn_slots slots;
    /** @brief The ids nvn_texset_sync_registered keeps in their slots, the flag of each (by id below the pinned ones) and the frame it last refreshed them. */
    uint32_t *synced, nsynced, capsynced;
    bool *is_synced;
    uint64_t sync_frame;
    /**
     * @brief What the last refresh of every synced id saw: the watch stamp (nvn_watch_stamp), the synced count and the
     * GPU write stamp; and the synced ids whose image has no write watch, which every frame checks by their bytes.
     */
    uint64_t seen_stamp, seen_written;
    uint32_t seen_count;
    uint32_t *loose, nloose, caploose;
    VkDescriptorSetLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    staging dims;
    /** @brief The image each texture id's elements hold, the guest texture and view bytes registered then, and the views made for a texture view. */
    vimg **tex_img;
    uint64_t *tex_guest;
    /** @brief The kind of each texture id's bound view (its format's sampled type). */
    uint32_t *tex_kind;
    uint8_t (*tex_view)[17];
    VkImageView (*tex_views)[DIMS];
    /** @brief The storage views written for each image id, by binding (32 onwards). */
    VkImageView (*img_views)[IMAGE_KINDS * 8u];
    /** @brief The level each texture id's view starts at. */
    uint32_t *tex_level;
    /** @brief The target and layers (first, count: 0 for the rest) of each slot's view, which storage images follow. */
    uint32_t *tex_target, *tex_layer, *tex_layers;
    /**
     * @brief Storage images are indexed by the same slot as the sampled arrays: a program reads one handle word
     * both ways, so a texture registered as an image takes a slot below nimg.
     * img_done[slot]: the storage descriptors of the slot are written for what the sampled ones hold.
     */
    bool *img_done;
    /**
     * @brief How formatted surface accesses reach each slot's storage image: NVN_SURFACE_DIRECT through the view of
     * its own numeric kind, NVN_SURFACE_RGB9E5 (every E5B9G9R9 image) through its 32-bit raw view, which moves the
     * texels unchanged as the console's formatted accesses to the format do (it has no shared-exponent surface
     * encoder, so programs pack the format themselves), NVN_SURFACE_NONE when neither is possible.
     */
    uint8_t *img_surface;
    /** @brief The sampler each sampler id's element holds and the builder bytes it was made from. */
    VkSampler *samplers;
    /**
     * @brief The sampler an id with no registered sampler reads: nearest, repeating, base level only (inferred: the
     * all-zero descriptor a pool entry that was never written holds; a handle without a sampler, such as a texel
     * fetch handle, names sampler id 0).
     */
    VkSampler fallback;
    uint64_t (*sampler_bytes)[16];
} t;

/**
 * @brief Gives every sampler id the sampler of a pool entry that was never written: nearest, repeating, base level
 * only (inferred: the all-zero descriptor), and every element of every sampled image and storage image array a null
 * descriptor (VK_EXT_robustness2: reads give zeros, stores are dropped). A host driver dereferences a descriptor
 * nothing was written to, and a program may pick any id out of a handle, so no element is left empty; a handle
 * whose texture was never bound reads zeros, as a never written pool entry does (inferred).
 */
static bool fill_samplers(void)
{
    VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                               .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                               .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                               .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                               .maxLod = 0.25f };
    if (!t.fallback && VK(vkCreateSampler)(g.dev, &ci, NULL, &t.fallback) != VK_SUCCESS)
        return false;
    VkDescriptorImageInfo *ii = malloc(SAMPLERS * sizeof *ii);
    if (!ii)
        return false;
    for (uint32_t i = 0; i < SAMPLERS; i++)
        ii[i] = (VkDescriptorImageInfo){ t.fallback, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = t.set,
                               .dstBinding = 0,
                               .descriptorCount = SAMPLERS,
                               .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                               .pImageInfo = ii };
    VK(vkUpdateDescriptorSets)(g.dev, 1, &w, 0, NULL);
    free(ii);
    if (!g.null_descriptor)
        return true;
    uint32_t most = t.ntex > t.nimg ? t.ntex : t.nimg;
    VkDescriptorImageInfo *nul = calloc(most, sizeof *nul);
    if (!nul)
        return false;
    for (uint32_t i = 0; i < most; i++)
        nul[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    for (uint32_t k = 0; k < KINDS; k++)
        for (uint32_t dd = 0; dd < DIMS; dd++) {
            w = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstSet = t.set,
                                        .dstBinding = 1 + 10 * k + dd,
                                        .descriptorCount = t.ntex,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                        .pImageInfo = nul };
            VK(vkUpdateDescriptorSets)(g.dev, 1, &w, 0, NULL);
        }
    for (uint32_t k = 0; k < IMAGE_KINDS; k++)
        for (uint32_t dd = 0; dd < IMAGE_DIMS; dd++) {
            if (!(t.img_used >> (k * IMAGE_DIMS + dd) & 1))
                continue;
            w = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                        .dstSet = t.set,
                                        .dstBinding = IMAGE_BINDING(k, dd),
                                        .descriptorCount = t.nimg,
                                        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                        .pImageInfo = nul };
            VK(vkUpdateDescriptorSets)(g.dev, 1, &w, 0, NULL);
        }
    free(nul);
    return true;
}

static bool make(void)
{
    if (t.tried)
        return t.ok;
    t.tried = true;
    VkPhysicalDeviceVulkan12Properties p12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12 };
    VK(vkGetPhysicalDeviceProperties2)(g.phys, &p2);
    uint32_t budget = p12.maxDescriptorSetUpdateAfterBindSampledImages;
    if (p12.maxPerStageDescriptorUpdateAfterBindSampledImages < budget)
        budget = p12.maxPerStageDescriptorUpdateAfterBindSampledImages;
    t.ntex = budget / (KINDS * DIMS);
    if (t.ntex > 1u << 20)
        t.ntex = 1u << 20;
    uint32_t images = p12.maxDescriptorSetUpdateAfterBindStorageImages;
    if (p12.maxPerStageDescriptorUpdateAfterBindStorageImages < images)
        images = p12.maxPerStageDescriptorUpdateAfterBindStorageImages;
    if (!nvn_shader_image_arrays(&t.img_used))
        t.img_used = (1ull << (IMAGE_KINDS * IMAGE_DIMS)) - 1;
    uint32_t arrays = (uint32_t)__builtin_popcountll(t.img_used);
    t.nimg = arrays ? images / arrays : t.ntex;
    if (t.nimg > t.ntex)
        t.nimg = t.ntex;
    rl_log(RL_LOG_INFO, "nvn", "texture set: %u texture ids, %u of them images in %u arrays", t.ntex, t.nimg, arrays);
    VkDescriptorSetLayoutBinding b[2 + KINDS * DIMS + IMAGE_KINDS * IMAGE_DIMS];
    VkDescriptorBindingFlags f[2 + KINDS * DIMS + IMAGE_KINDS * IMAGE_DIMS];
    uint32_t n = 0;
    const VkDescriptorBindingFlags pending =
        g.unused_while_pending ? VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT : 0;
    const VkDescriptorBindingFlags partial =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | pending;
    b[n] = (VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_SAMPLER, SAMPLERS, VK_SHADER_STAGE_ALL, NULL };
    f[n++] = partial;
    for (uint32_t k = 0; k < KINDS; k++)
        for (uint32_t d = 0; d < DIMS; d++) {
            b[n] = (VkDescriptorSetLayoutBinding){ 1 + 10 * k + d, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, t.ntex,
                                                   VK_SHADER_STAGE_ALL, NULL };
            f[n++] = partial;
        }
    b[n] = (VkDescriptorSetLayoutBinding){ DIMS_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL,
                                           NULL };
    f[n++] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT | pending;
    for (uint32_t k = 0; k < IMAGE_KINDS; k++)
        for (uint32_t d = 0; d < IMAGE_DIMS; d++) {
            if (!(t.img_used >> (k * IMAGE_DIMS + d) & 1))
                continue;
            b[n] = (VkDescriptorSetLayoutBinding){ IMAGE_BINDING(k, d), VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, t.nimg,
                                                   VK_SHADER_STAGE_ALL, NULL };
            f[n++] = partial;
        }
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = n, .pBindingFlags = f
    };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .pNext = &fi,
                                           .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
                                           .bindingCount = n,
                                           .pBindings = b };
    if (VK(vkCreateDescriptorSetLayout)(g.dev, &li, NULL, &t.layout) != VK_SUCCESS)
        return false;
    VkDescriptorPoolSize ps[4] = { { VK_DESCRIPTOR_TYPE_SAMPLER, SAMPLERS },
                                   { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, t.ntex * KINDS * DIMS },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, t.nimg * IMAGE_KINDS * IMAGE_DIMS } };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                       .maxSets = 1,
                                       .poolSizeCount = 4,
                                       .pPoolSizes = ps };
    if (VK(vkCreateDescriptorPool)(g.dev, &pci, NULL, &t.pool) != VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = t.pool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &t.layout };
    if (VK(vkAllocateDescriptorSets)(g.dev, &ai, &t.set) != VK_SUCCESS)
        return false;
    if (!nvn_vk_staging_new(&t.dims, 4ull * t.ntex))
        return false;
    memset(t.dims.map, 0, 4ull * t.ntex);
    VkDescriptorBufferInfo bi = { t.dims.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = t.set,
                               .dstBinding = DIMS_BINDING,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               .pBufferInfo = &bi };
    VK(vkUpdateDescriptorSets)(g.dev, 1, &w, 0, NULL);
    t.tex_img = calloc(t.ntex, sizeof *t.tex_img);
    t.tex_guest = calloc(t.ntex, sizeof *t.tex_guest);
    t.tex_kind = calloc(t.ntex, sizeof *t.tex_kind);
    t.tex_view = calloc(t.ntex, sizeof *t.tex_view);
    t.tex_views = calloc(t.ntex, sizeof *t.tex_views);
    t.img_views = calloc(t.nimg, sizeof *t.img_views);
    t.tex_level = calloc(t.ntex, sizeof *t.tex_level);
    t.tex_target = calloc(t.ntex, sizeof *t.tex_target);
    t.tex_layer = calloc(t.ntex, sizeof *t.tex_layer);
    t.tex_layers = calloc(t.ntex, sizeof *t.tex_layers);
    bool slots_ok = nvn_slots_init(&t.slots, t.ntex, 1u << 20, t.ntex - t.ntex / 16);
    t.is_synced = calloc(t.ntex, sizeof *t.is_synced);
    t.img_done = calloc(t.ntex, sizeof *t.img_done);
    t.img_surface = calloc(t.ntex, sizeof *t.img_surface);
    t.samplers = calloc(SAMPLERS, sizeof *t.samplers);
    t.sampler_bytes = calloc(SAMPLERS, sizeof *t.sampler_bytes);
    bool filled = fill_samplers();
    t.ok = filled && t.tex_img && t.tex_guest && t.tex_kind && t.tex_view && t.tex_views && t.img_views && t.tex_level && t.tex_target && t.tex_layer && t.tex_layers && t.img_done && t.img_surface && t.is_synced && slots_ok && t.samplers && t.sampler_bytes;
    return t.ok;
}

VkDescriptorSetLayout nvn_texset_layout(void) { return make() ? t.layout : NULL; }

VkDescriptorSet nvn_texset(void) { return t.set; }

/** @brief The numeric kind of a host format: 1 signed integer, 2 unsigned integer, else 0. */
static uint32_t kind_of(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_SINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R8G8B8A8_SINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_SINT:
        return 1;
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R32_UINT: case VK_FORMAT_R8G8_UINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R8G8B8A8_UINT:
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        return 2;
    default:
        return 0;
    }
}

/** @brief The translator's Dim of an NVNtextureTarget, and the view type of each Dim. */
static uint32_t dim_of(uint32_t target)
{
    static const uint8_t map[] = { DIM_1D,          DIM_2D, DIM_3D,   DIM_1D_ARRAY,   DIM_2D_ARRAY, DIM_2D_MS,
                                   DIM_2D_MS_ARRAY, DIM_2D, DIM_CUBE, DIM_CUBE_ARRAY, DIM_1D };
    return target < sizeof map ? map[target] : DIM_2D;
}

static const VkImageViewType view_types[DIMS] = { VK_IMAGE_VIEW_TYPE_1D,       VK_IMAGE_VIEW_TYPE_2D,
                                                 VK_IMAGE_VIEW_TYPE_3D,       VK_IMAGE_VIEW_TYPE_CUBE,
                                                 VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D_ARRAY,
                                                 VK_IMAGE_VIEW_TYPE_CUBE_ARRAY, VK_IMAGE_VIEW_TYPE_2D,
                                                 VK_IMAGE_VIEW_TYPE_2D_ARRAY };

/** @brief The dimensions a texture of Dim d is also sampled as (a 2D texture as a one-layer 2D array). */
static uint32_t dims_fitting(uint32_t d)
{
    switch (d) {
    case DIM_2D: case DIM_2D_ARRAY: return 1u << DIM_2D | 1u << DIM_2D_ARRAY;
    case DIM_1D: case DIM_1D_ARRAY: return 1u << DIM_1D | 1u << DIM_1D_ARRAY;
    case DIM_CUBE: return 1u << DIM_CUBE | 1u << DIM_2D_ARRAY | 1u << DIM_CUBE_ARRAY;
    default: return 1u << d;
    }
}

uint32_t nvn_texset_view_layers(const vimg *v, const nvn_view *nv)
{
    uint32_t all = nvn_vk_image_range(v).layerCount;
    if (!nv || !nv->has_layers)
        return all;
    if (nv->num_layers)
        return (uint32_t)nv->num_layers;
    return (uint32_t)nv->min_layer < all ? all - (uint32_t)nv->min_layer : 0;
}

uint32_t nvn_texset_fit(uint32_t dim, uint32_t width, uint32_t height, uint32_t layers)
{
    uint32_t fit = dims_fitting(dim);
    if (dim == DIM_2D_ARRAY && width == height && layers >= 6) {
        fit |= 1u << DIM_CUBE;
        if (layers % 6 == 0)
            fit |= 1u << DIM_CUBE_ARRAY;
    }
    return fit;
}

/** @brief NVNwrapMode (CLAMP 0, REPEAT 1, MIRROR_CLAMP 2, MIRROR_CLAMP_TO_EDGE 3, MIRROR_CLAMP_TO_BORDER 4, CLAMP_TO_BORDER 5, MIRRORED_REPEAT 6, CLAMP_TO_EDGE 7). */
static VkSamplerAddressMode wrap(uint32_t m)
{
    static const VkSamplerAddressMode map[] = {
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,        VK_SAMPLER_ADDRESS_MODE_REPEAT,
        VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE,
        VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER,
        VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT,      VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    return m < 8 ? map[m] : VK_SAMPLER_ADDRESS_MODE_REPEAT;
}

/** @brief A host sampler for the NVNsamplerBuilder bytes b, read through the SDK driver's getters. */
static int make_sampler(replay *r, const uint64_t b[16], VkSampler *out)
{
    uint32_t w[4] = { 0 }, mm[4] = { 0 }, lod[4] = { 0 }, cmp[4] = { 0 }, border[4] = { 0 };
    uint64_t ret = 0, red = 0;
    int rc = nvn_vk_getter(r, "nvnSamplerBuilderGetWrapMode", b, 128, NULL, 0, 3, w, &ret);
    if (rc == RL_OK)
        rc = nvn_vk_getter(r, "nvnSamplerBuilderGetMinMagFilter", b, 128, NULL, 0, 2, mm, &ret);
    if (rc == RL_OK)
        rc = nvn_vk_getter(r, "nvnSamplerBuilderGetLodClamp", b, 128, NULL, 0, 2, lod, &ret);
    if (rc == RL_OK)
        rc = nvn_vk_getter(r, "nvnSamplerBuilderGetCompare", b, 128, NULL, 0, 2, cmp, &ret);
    if (rc == RL_OK)
        rc = nvn_vk_getter(r, "nvnSamplerBuilderGetBorderColor", b, 128, NULL, 0, 1, border, &ret);
    if (rc == RL_OK)
        rc = nvn_vk_getter(r, "nvnSamplerBuilderGetReductionFilter", b, 128, NULL, 0, 0, NULL, &red);
    float bias = 0, aniso = 1;
    if (rc == RL_OK)
        rc = nvn_vk_getter(r, "nvnSamplerBuilderGetLodBias", b, 128, NULL, 0, 0, NULL, &ret);
    bias = r->fret;
    if (rc == RL_OK)
        rc = nvn_vk_getter(r, "nvnSamplerBuilderGetMaxAnisotropy", b, 128, NULL, 0, 0, NULL, &ret);
    aniso = r->fret;
    if (rc != RL_OK)
        return rc;
    uint32_t min = mm[0], mag = mm[1];
    if (min > 5 || mag > 1 || (uint32_t)red > 2 || cmp[0] > 1 || (cmp[0] && (cmp[1] < 1 || cmp[1] > 8)))
        return nvn_vk_stop(r, "sampler state min %u mag %u reduction %u compare %u/%u", min, mag, (uint32_t)red,
                           cmp[0], cmp[1]);
    float lmin, lmax, bc[4];
    memcpy(&lmin, &lod[0], 4);
    memcpy(&lmax, &lod[1], 4);
    memcpy(bc, border, 16);
    VkSamplerReductionModeCreateInfo rm = { .sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO,
                                            .reductionMode = red == 1   ? VK_SAMPLER_REDUCTION_MODE_MIN
                                                             : red == 2 ? VK_SAMPLER_REDUCTION_MODE_MAX
                                                                        : VK_SAMPLER_REDUCTION_MODE_WEIGHTED_AVERAGE };
    bool transparent = !bc[0] && !bc[1] && !bc[2] && !bc[3], opaque_black = !bc[0] && !bc[1] && !bc[2] && bc[3] == 1;
    bool opaque_white = bc[0] == 1 && bc[1] == 1 && bc[2] == 1 && bc[3] == 1;
    bool bordered = wrap(w[0]) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                  wrap(w[1]) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER ||
                  wrap(w[2]) == VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    if (bordered && !transparent && !opaque_black && !opaque_white)
        return nvn_vk_stop(r, "a sampler border color (%g, %g, %g, %g), which the layer does not take yet", bc[0],
                           bc[1], bc[2], bc[3]);
    VkSamplerCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = red ? &rm : NULL,
        .magFilter = mag ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (min & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = min >= 4 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = wrap(w[0]),
        .addressModeV = wrap(w[1]),
        .addressModeW = wrap(w[2]),
        .mipLodBias = bias,
        .anisotropyEnable = aniso > 1,
        .maxAnisotropy = aniso > 16 ? 16 : aniso,
        .compareEnable = cmp[0] != 0,
        .compareOp = cmp[0] ? (VkCompareOp)(cmp[1] - 1) : VK_COMPARE_OP_NEVER,
        .minLod = min < 2 ? 0 : lmin,
        .maxLod = min < 2 ? 0.25f : lmax,
        .borderColor = opaque_white   ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                       : opaque_black ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK
                                      : VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
    };
    if (VK(vkCreateSampler)(g.dev, &ci, NULL, out) != VK_SUCCESS)
        return nvn_vk_stop(r, "vkCreateSampler failed");
    return RL_OK;
}

static int bind_sampler(replay *r, uint32_t id)
{
    if (id >= SAMPLERS)
        return nvn_vk_stop(r, "sampler id %u is past the layer's %u", id, SAMPLERS);
    uint8_t found[128];
    static const uint8_t zero[128];
    bool none = !nvn_sampler_registered(id, found);
    const uint8_t *bytes = none ? zero : found;
    if (t.samplers[id] && !memcmp(t.sampler_bytes[id], bytes, 128) && (!none || t.samplers[id] == t.fallback))
        return RL_OK;
    VkSampler s;
    int rc = RL_OK;
    if (none) {
        if (!t.fallback) {
            VkSamplerCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
                                       .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                       .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                       .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
                                       .maxLod = 0.25f };
            if (VK(vkCreateSampler)(g.dev, &ci, NULL, &t.fallback) != VK_SUCCESS)
                return nvn_vk_stop(r, "cannot make the sampler for unregistered sampler ids");
        }
        s = t.fallback;
    } else {
        rc = make_sampler(r, (const uint64_t *)bytes, &s);
    }
    if (rc != RL_OK)
        return rc;
    if (t.samplers[id]) {
        if (!nvn_vk_flush(r))
            return nvn_vk_stop(r, "the host device failed a submit");
        if (t.samplers[id] != t.fallback)
            VK(vkDestroySampler)(g.dev, t.samplers[id], NULL);
    }
    t.samplers[id] = s;
    memcpy(t.sampler_bytes[id], bytes, 128);
    VkDescriptorImageInfo ii = { s, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = t.set,
                               .dstBinding = 0,
                               .dstArrayElement = id,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER,
                               .pImageInfo = &ii };
    VK(vkUpdateDescriptorSets)(g.dev, 1, &w, 0, NULL);
    return RL_OK;
}

/** @brief The view format of storage image kind ik over an image of format f (texel bytes bpb), or UNDEFINED. */
static VkFormat image_format(uint32_t ik, VkFormat f, uint32_t bpb)
{
    switch (ik) {
    case IK_FLOAT: case IK_SINT: case IK_UINT:
        if (f == VK_FORMAT_R8G8B8A8_SRGB)
            f = VK_FORMAT_R8G8B8A8_UNORM;
        return kind_of(f) == ik - IK_FLOAT ? f : VK_FORMAT_UNDEFINED;
    case IK_RAW8: return bpb == 1 ? VK_FORMAT_R8_UINT : VK_FORMAT_UNDEFINED;
    case IK_RAW16: return bpb == 2 ? VK_FORMAT_R16_UINT : VK_FORMAT_UNDEFINED;
    case IK_RAW32: case IK_ATOMIC32: return bpb == 4 ? VK_FORMAT_R32_UINT : VK_FORMAT_UNDEFINED;
    case IK_RAW64: return bpb == 8 ? VK_FORMAT_R32G32_UINT : VK_FORMAT_UNDEFINED;
    case IK_RAW128: return bpb == 16 ? VK_FORMAT_R32G32B32A32_UINT : VK_FORMAT_UNDEFINED;
    default: return VK_FORMAT_UNDEFINED;
    }
}

/**
 * @brief The second half of one rule: an element of a sampled or storage image array holds its texture's view or
 * a null descriptor (fill_samplers writes the null to every element at creation). When slot is rebound, the
 * elements of the arrays its new texture has no view for (the sampled kind kind with the dimensions in fit, or the
 * storage image entries in fit, bit kind * IMAGE_DIMS + dim) go back to a null descriptor, so a program that reads
 * the slot through a dimension or kind the texture has no view of reads zeros (VK_EXT_robustness2 nullDescriptor)
 * and never a stale element of the texture the slot held before (inferred: the hardware reads such a handle
 * through the texture header's own type and faults on nothing).
 */
/** @brief Writes the programs' word for texture slot sl: its Dim in bits 0..8 and its numeric kind from bit 8. */
static void set_word(uint32_t sl, uint32_t dim, uint32_t kind)
{
    uint32_t w = dim | kind << 8;
    memcpy(t.dims.map + 4ull * sl, &w, 4);
}

static void null_rest(uint32_t slot, bool image, uint32_t kind, uint64_t fit)
{
    if (!g.null_descriptor)
        return;
    uint32_t count = image ? IMAGE_KINDS * IMAGE_DIMS : KINDS * DIMS;
    VkDescriptorImageInfo ii = { NULL, NULL, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w[IMAGE_KINDS * IMAGE_DIMS];
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t k = image ? i / IMAGE_DIMS : i / DIMS, d = image ? i % IMAGE_DIMS : i % DIMS;
        if ((image ? fit >> i & 1u : k == kind && fit >> d & 1u) != 0)
            continue;
        if (image && !(t.img_used >> i & 1))
            continue;
        w[n++] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = t.set,
                                         .dstBinding = image ? IMAGE_BINDING(k, d) : 1 + 10 * k + d,
                                         .dstArrayElement = slot,
                                         .descriptorCount = 1,
                                         .descriptorType = image ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE
                                                                 : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                         .pImageInfo = &ii };
    }
    if (n)
        VK(vkUpdateDescriptorSets)(g.dev, n, w, 0, NULL);
}

/** @brief Writes image v (level level) as storage image id into every kind and dimension array it can take. */
static int bind_image(replay *r, uint32_t id, uint32_t slot, vimg *v, uint32_t level, uint32_t target, uint32_t layer,
                      uint32_t layers)
{
    uint32_t bpb, bw, bh;
    if (!nvn_tex_format(v->d.format, &bpb, &bw, &bh) || bw != 1 || bh != 1 || v->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return nvn_vk_stop(r, "image id %u has format %u, which is not a storage format", id, v->d.format);
    uint32_t dims;
    switch (target) {
    case 0: case 3: dims = 1u << ID_1D | 1u << ID_1D_ARRAY; break;
    case 1: case 4: case 7: dims = 1u << ID_2D | 1u << ID_2D_ARRAY; break;
    case 2: dims = 1u << ID_3D; break;
    case NVN_TARGET_CUBEMAP: case NVN_TARGET_CUBEMAP_ARRAY: dims = 1u << ID_2D_ARRAY; break;
    default: return nvn_vk_stop(r, "image id %u has target %u", id, target);
    }
    static const VkImageViewType types[IMAGE_DIMS] = { VK_IMAGE_VIEW_TYPE_1D,       VK_IMAGE_VIEW_TYPE_1D,
                                                       VK_IMAGE_VIEW_TYPE_1D_ARRAY, VK_IMAGE_VIEW_TYPE_2D,
                                                       VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_3D };
    VkDescriptorImageInfo ii[IMAGE_KINDS * IMAGE_DIMS];
    VkWriteDescriptorSet w[IMAGE_KINDS * IMAGE_DIMS];
    uint32_t n = 0;
    uint64_t wrote = 0;
    bool fits = false, formatted = false;
    for (uint32_t k = 0; k < IMAGE_KINDS; k++) {
        VkFormat f = image_format(k, v->format, bpb);
        if (f == VK_FORMAT_UNDEFINED)
            continue;
        VkFormatProperties fp;
        VK(vkGetPhysicalDeviceFormatProperties)(g.phys, f, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT |
                                    (k == IK_ATOMIC32 ? VK_FORMAT_FEATURE_STORAGE_IMAGE_ATOMIC_BIT : 0);
        if ((fp.optimalTilingFeatures & need) != need)
            continue;
        for (uint32_t d = 0; d < IMAGE_DIMS; d++) {
            if (!(dims & (1u << d)))
                continue;
            fits = true;
            formatted |= k <= IK_UINT;
            if (!(t.img_used >> (k * IMAGE_DIMS + d) & 1))
                continue;
            uint32_t at = 8u * k + d;
            if (t.img_views[slot][at]) {
                if (!nvn_vk_flush(r))
                    return nvn_vk_stop(r, "the host device failed a submit");
                VK(vkDestroyImageView)(g.dev, t.img_views[slot][at], NULL);
                t.img_views[slot][at] = NULL;
            }
            VkImageView iv = nvn_vk_storage_view(v, types[d], f, level, layer, layers);
            if (!iv)
                return nvn_vk_stop(r, "cannot make a storage view of image id %u", id);
            t.img_views[slot][at] = iv;
            ii[n] = (VkDescriptorImageInfo){ NULL, iv, VK_IMAGE_LAYOUT_GENERAL };
            w[n] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                           .dstSet = t.set,
                                           .dstBinding = IMAGE_BINDING(k, d),
                                           .dstArrayElement = slot,
                                           .descriptorCount = 1,
                                           .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                                           .pImageInfo = &ii[n] };
            wrote |= 1ull << (k * IMAGE_DIMS + d);
            n++;
        }
    }
    if (!fits)
        return nvn_vk_stop(r, "image id %u (format %u) has no storage view the host device supports", id,
                           v->d.format);
    t.img_surface[slot] = v->format == VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 ? NVN_SURFACE_RGB9E5
                          : formatted                                    ? NVN_SURFACE_DIRECT
                                                                         : NVN_SURFACE_NONE;
    if (n)
        VK(vkUpdateDescriptorSets)(g.dev, n, w, 0, NULL);
    null_rest(slot, true, 0, wrote);
    if (nvn_trace_active)
        rl_log(RL_LOG_INFO, "nvn", "trace: storage image id %u slot %u (format %u, host format %d, usage 0x%x) has views of kinds/dims mask 0x%llx", id,
               slot, v->d.format, (int)v->format, (unsigned)v->usage, (unsigned long long)wrote);
    return RL_OK;
}

/**
 * @brief The slot of texture id: its own number below the pinned ids, else the one it holds, or a free one, or the
 * next in turn after the work that may use it has run. A texture registered as an image needs one below nimg.
 */
static int slot_for(replay *r, uint32_t id, bool image, uint32_t *slot)
{
    uint32_t evicted;
    *slot = nvn_slots_take(&t.slots, id, &evicted);
    if (*slot == UINT32_MAX)
        return nvn_vk_stop(r, "texture id %u is past the 2^20 a handle names", id);
    if (image && *slot >= t.nimg)
        return nvn_vk_stop(r, "texture id %u is registered as an image but the host holds %u images", id, t.nimg);
    if (!evicted)
        return RL_OK;
    if (!nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    t.tex_img[*slot] = NULL;
    t.tex_guest[*slot] = 0;
    t.img_done[*slot] = false;
    return RL_OK;
}

void nvn_texset_image_gone(const vimg *v)
{
    if (!t.ok || !v)
        return;
    for (uint32_t sl = 0; sl < t.ntex; sl++) {
        if (t.tex_img[sl] != v)
            continue;
        for (uint32_t d = 0; d < DIMS; d++)
            if (t.tex_views[sl][d]) {
                VK(vkDestroyImageView)(g.dev, t.tex_views[sl][d], NULL);
                t.tex_views[sl][d] = NULL;
            }
        null_rest(sl, false, 0, 0);
        if (sl < t.nimg) {
            for (uint32_t at = 0; at < IMAGE_KINDS * 8u; at++)
                if (t.img_views[sl][at]) {
                    VK(vkDestroyImageView)(g.dev, t.img_views[sl][at], NULL);
                    t.img_views[sl][at] = NULL;
                }
            null_rest(sl, true, 0, 0);
        }
        t.tex_img[sl] = NULL;
        t.tex_guest[sl] = 0;
        t.img_done[sl] = false;
    }
}

void nvn_texset_texture_gone(uint64_t tex)
{
    if (!t.ok)
        return;
    nvn_lock();
    for (uint32_t ts = 0; ts < t.ntex; ts++)
        if (t.tex_guest[ts] == tex) {
            t.tex_guest[ts] = 0;
            t.img_done[ts] = false;
            nvn_slots_free(&t.slots, ts);
        }
    nvn_unlock();
}

/** @brief Makes slot sl hold null descriptors only, so every read of it gives zeros; *kind and *dim get what it now says. */
static int null_slot(replay *r, uint32_t sl, uint32_t *kind, uint32_t *dim)
{
    if (t.tex_img[sl] && !nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    for (uint32_t d = 0; d < DIMS; d++)
        if (t.tex_views[sl][d]) {
            VK(vkDestroyImageView)(g.dev, t.tex_views[sl][d], NULL);
            t.tex_views[sl][d] = NULL;
        }
    *kind = 0;
    *dim = DIM_2D;
    null_rest(sl, false, 0, 0);
    t.tex_img[sl] = NULL;
    t.tex_guest[sl] = 0;
    t.img_done[sl] = false;
    t.tex_kind[sl] = 0;
    memset(t.tex_view[sl], 0, sizeof t.tex_view[sl]);
    set_word(sl, *dim, *kind);
    return RL_OK;
}

/**
 * @brief A sampled handle whose texture id was never registered names a pool entry nothing was written to: it takes
 * a slot of its own whose every array element is a null descriptor, so it reads zeros (inferred, as for a separate
 * texture handle), and a texture registered under the id later is bound afresh.
 */
static int bind_unwritten(replay *r, uint32_t id, uint32_t sid, uint32_t *kind, uint32_t *dim, uint32_t *slot,
                          bool with_sampler)
{
    if (rl_report_unknown(0x4e564e5445584944ull ^ id, "nvn",
                          "a sampled handle names texture id %u, which no texture was registered under: it reads zeros",
                          id))
        return nvn_vk_stop(r, "texture id %u is not registered", id);
    int src = slot_for(r, id, false, slot);
    if (src != RL_OK)
        return src;
    if ((src = null_slot(r, *slot, kind, dim)) != RL_OK)
        return src;
    return with_sampler ? bind_sampler(r, sid) : RL_OK;
}

/**
 * @brief nvn_texset_bind; with_sampler false leaves the handle's sampler id alone (a separate texture handle names none),
 * and as_image takes the texture for an image whether it was registered as one or not (a game may bind the descriptor
 * it registered as a texture to an image slot: both are the same texture pool entry).
 */
/**
 * @brief Whether the texture at tex still has storage, as the console sees it: a texture keeps the address it was
 * initialized at, not its pool object, so one whose pool object was finalized still reads its bytes while a live pool or
 * the kept import of the finalized one covers them (a game finalizes a pool and keeps sampling the textures it made
 * in it, or makes the pool again over the same bytes). Without one, the console's GPU would read unmapped memory.
 * pool_live answers for the registered textures the set keeps in their slots (one without storage is reported and reads
 * zeros); storage_live for one a program binds.
 */
static bool pool_live(uint32_t id, uint64_t tex)
{
    nvn_tex_desc d;
    if (!nvn_texture_desc(tex, &d))
        return false;
    if (nvn_tex_storage(&d))
        return true;
    (void)rl_report_unknown(0x4e564e504f4f4c47ull ^ id, "nvn",
                            "texture id %u (texture 0x%llx) lies in memory pool 0x%llx, which no longer exists, and no "
                            "pool covers its storage at 0x%llx: it is not made resident and reads zeros",
                            id, (unsigned long long)tex, (unsigned long long)d.pool, (unsigned long long)d.storage);
    return false;
}

/**
 * @brief pool_live for a texture a program binds: RL_OK with *live set when it has storage. One whose storage address
 * was recorded but that no pool covers any longer is a stop; one with none recorded (its pool was unknown when it was
 * recorded) reads zeros, as before.
 */
static int storage_live(replay *r, uint32_t id, uint64_t tex, bool *live)
{
    nvn_tex_desc d;
    *live = nvn_texture_desc(tex, &d) && nvn_tex_storage(&d);
    if (*live || !d.storage)
        return RL_OK;
    return nvn_vk_stop(r,
                       "texture id %u (texture 0x%llx) lies in memory pool 0x%llx, which no longer exists, and no pool "
                       "covers its storage at 0x%llx: the console's GPU would read unmapped memory",
                       id, (unsigned long long)tex, (unsigned long long)d.pool, (unsigned long long)d.storage);
}

static int bind_texture_slot(replay *r, uint32_t handle, uint32_t *kind, uint32_t *dim, uint32_t *slot, bool with_sampler,
                             bool as_image)
{
    if (!make())
        return nvn_vk_stop(r, "cannot make the texture set");
    uint32_t id = handle & 0xfffffu, sid = handle >> 20;
    uint8_t view[17] = { 0 };
    bool has_view = false, image = false;
    uint64_t tex = nvn_tex_registered(id, view, &has_view, &image);
    if (tex) {
        bool live = false;
        int rc = storage_live(r, id, tex, &live);
        if (rc != RL_OK)
            return rc;
        if (!live) {
            (void)pool_live(id, tex);
            tex = 0;
        }
    }
    if (!tex && !as_image)
        return bind_unwritten(r, id, sid, kind, dim, slot, with_sampler);
    if (!tex)
        return nvn_vk_stop(r, "texture id %u (handle 0x%x) is not registered", id, handle);
    image |= as_image;
    int src = slot_for(r, id, image, slot);
    if (src != RL_OK)
        return src;
    uint32_t sl = *slot;
    view[16] = has_view;
    vimg *v = nvn_vk_image(r, tex);
    if (!v)
        return RL_PENDING;
    nvn_vk_reading(v);
    if (!nvn_vk_refresh(r, v))
        return nvn_vk_stop(r, "cannot refill texture id %u from its storage", id);
    if (image)
        nvn_vk_written(v);
    if (t.tex_img[sl] == v && t.tex_guest[sl] == tex && !memcmp(t.tex_view[sl], view, sizeof view)) {
        nvn_vk_dispatch_image(v, t.tex_level[sl], image, t.tex_layer[sl], t.tex_layers[sl], image);
        *kind = t.tex_kind[sl];
        memcpy(dim, t.dims.map + 4ull * sl, 4);
        *dim &= 0xffu;
        return !with_sampler || (image && !nvn_sampler_registered(sid, (uint8_t[128]){ 0 })) ? RL_OK : bind_sampler(r, sid);
    }
    nvn_view nv;
    VkFormat vf = v->format;
    if (has_view) {
        int rc = nvn_view_read(r->c, r->mem, view, &nv);
        if (rc != RL_OK)
            return rc;
        VkImageAspectFlags a;
        if (nv.has_format && nv.format != v->d.format &&
            (vf = nvn_vk_view_format(v, nv.format, &a)) == VK_FORMAT_UNDEFINED)
            return nvn_vk_stop(r, "texture id %u is registered with a view of format %u over format %u", id, nv.format,
                               v->d.format);
        if ((nv.has_levels && (nv.base_level < 0 || nv.num_levels < 0)) ||
            (nv.has_layers && (nv.min_layer < 0 || nv.num_layers < 0)))
            return nvn_vk_stop(r, "texture id %u is registered with a view of levels %d+%d, layers %d+%d", id,
                               nv.base_level, nv.num_levels, nv.min_layer, nv.num_layers);
    }
    *kind = kind_of(vf);
    *dim = dim_of(has_view && nv.has_target ? nv.target : v->d.target);
    if (t.tex_img[sl] && !nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    for (uint32_t d = 0; d < DIMS; d++)
        if (t.tex_views[sl][d]) {
            VK(vkDestroyImageView)(g.dev, t.tex_views[sl][d], NULL);
            t.tex_views[sl][d] = NULL;
        }
    VkDescriptorImageInfo ii[DIMS];
    VkWriteDescriptorSet w[DIMS];
    uint32_t layers = nvn_texset_view_layers(v, has_view ? &nv : NULL);
    uint32_t n = 0, fit = nvn_texset_fit(*dim, v->d.width, v->d.height, layers);
    for (uint32_t d = 0; d < DIMS; d++) {
        if (!(fit & (1u << d)))
            continue;
        VkImageView iv = nvn_vk_view(v, view_types[d], has_view ? &nv : NULL);
        if (!iv)
            return nvn_vk_stop(r, "cannot make a view of texture id %u as dimension %u", id, d);
        if (has_view)
            t.tex_views[sl][d] = iv;
        ii[n] = (VkDescriptorImageInfo){ NULL, iv, VK_IMAGE_LAYOUT_GENERAL };
        w[n] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = t.set,
                                       .dstBinding = 1 + 10 * *kind + d,
                                       .dstArrayElement = sl,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       .pImageInfo = &ii[n] };
        n++;
    }
    VK(vkUpdateDescriptorSets)(g.dev, n, w, 0, NULL);
    null_rest(sl, false, *kind, fit);
    t.tex_level[sl] = has_view && nv.has_levels ? (uint32_t)nv.base_level : 0;
    t.img_done[sl] = false;
    t.tex_target[sl] = has_view && nv.has_target ? nv.target : v->d.target;
    t.tex_layer[sl] = has_view && nv.has_layers ? (uint32_t)nv.min_layer : 0;
    t.tex_layers[sl] = has_view && nv.has_layers ? (uint32_t)nv.num_layers : 0;
    t.tex_img[sl] = v;
    t.tex_kind[sl] = *kind;
    t.tex_guest[sl] = tex;
    memcpy(t.tex_view[sl], view, sizeof view);
    set_word(sl, *dim, *kind);
    nvn_vk_dispatch_image(v, t.tex_level[sl], image, t.tex_layer[sl], t.tex_layers[sl], image);
    return !with_sampler || (image && !nvn_sampler_registered(sid, (uint8_t[128]){ 0 })) ? RL_OK : bind_sampler(r, sid);
}

static int bind_texture(replay *r, uint32_t handle, uint32_t *kind, uint32_t *dim, uint32_t *slot, bool with_sampler,
                        bool as_image)
{
    int rc = bind_texture_slot(r, handle, kind, dim, slot, with_sampler, as_image);
    if (rc == RL_OK && nvn_trace_active) {
        uint8_t view[17] = { 0 };
        bool has_view = false, image = false;
        nvn_tex_registered(handle & 0xfffffu, view, &has_view, &image);
        view[16] = has_view;
        nvn_trace_texture(r, t.tex_img[*slot], handle & 0xfffffu, *slot, *kind, *dim, image, view);
    }
    return rc;
}

int nvn_texset_bind(replay *r, uint32_t handle, uint32_t *kind, uint32_t *dim, uint32_t *slot)
{
    return bind_texture(r, handle, kind, dim, slot, true, false);
}

int nvn_texset_separate_texture(replay *r, uint32_t handle, uint32_t *slot)
{
    uint8_t view[17];
    bool has_view = false, image = false;
    if (!nvn_tex_registered(handle & 0xfffffu, view, &has_view, &image)) {
        *slot = UINT32_MAX;
        return RL_OK;
    }
    uint32_t kind, dim;
    return bind_texture(r, handle, &kind, &dim, slot, false, false);
}

bool nvn_texset_view_fits(const vimg *v, const nvn_view *nv)
{
    VkImageAspectFlags a;
    return !nv->has_format || nv->format == v->d.format || nvn_vk_view_format(v, nv->format, &a) != VK_FORMAT_UNDEFINED;
}

/**
 * @brief Whether the texture registered under id can be made resident: the layer can make a host image of it, and see
 * it through the view it was registered with. A registration the layer cannot (a format or target it does not handle,
 * or a view format of blocks of another footprint, as a descriptor left from an earlier use of the pool entry may) is
 * not made resident: it is reported once, and a program that does sample it stops as the bind of a handle in a
 * constant bank does.
 */
static int resident_ok(replay *r, uint32_t id, uint64_t tex, const uint8_t *view, bool has_view, bool *ok)
{
    *ok = true;
    nvn_tex_desc d;
    if (nvn_texture_desc(tex, &d) && !nvn_vk_can_image(&d)) {
        *ok = false;
        (void)rl_report_unknown(0x4e564e4e4f494d47ull ^ id, "nvn",
                                "texture id %u is registered with format %u, target %u, %u samples, which the layer "
                                "cannot make a host image of: it is not made resident",
                                id, d.format, d.target, d.samples);
        return RL_OK;
    }
    if (!has_view)
        return RL_OK;
    vimg *v = nvn_vk_image(r, tex);
    if (!v)
        return RL_PENDING;
    nvn_view nv;
    int rc = nvn_view_read(r->c, r->mem, view, &nv);
    if (rc != RL_OK)
        return rc;
    if (!nvn_texset_view_fits(v, &nv)) {
        *ok = false;
        (void)rl_report_unknown(0x4e564e5645574944ull ^ id, "nvn",
                                "texture id %u is registered with a view of format %u over format %u, which the host cannot "
                                "give it: it is not made resident",
                                id, nv.format, v->d.format);
    }
    return RL_OK;
}

/**
 * @brief A registered id whose texture's memory pool is gone reads zeros, as its handle bound from a constant bank does:
 * the slot of its own (ids below the pinned ones) drops the image it may still hold from before the pool went.
 */
static int stale_slot(replay *r, uint32_t id)
{
    if (id >= t.slots.pinned || !t.tex_img[id])
        return RL_OK;
    uint32_t kind, dim;
    return null_slot(r, id, &kind, &dim);
}

/**
 * @brief Brings the texture registered under synced id up to date in its slot: a slot that still holds the image of the
 * same registration only has its bytes checked (nvn_vk_refresh, by its write watch where it has one); a changed
 * registration is bound again, and one whose pool is gone reads zeros.
 */
static int refresh_synced(replay *r, uint32_t id)
{
    uint8_t view[17];
    bool has_view, image;
    uint64_t tex = nvn_tex_registered(id, view, &has_view, &image);
    if (!tex)
        return RL_OK;
    if (!pool_live(id, tex))
        return stale_slot(r, id);
    if (t.tex_img[id] && t.tex_guest[id] == tex) {
        if (!nvn_vk_refresh(r, t.tex_img[id]))
            return nvn_vk_stop(r, "cannot refill texture id %u from its storage", id);
        return RL_OK;
    }
    bool ok;
    int rc = resident_ok(r, id, tex, view, has_view, &ok);
    if (rc != RL_OK || !ok)
        return rc;
    uint32_t kind, dim, slot;
    rc = bind_texture(r, id, &kind, &dim, &slot, false, false);
    if (rc == RL_OK && t.tex_img[slot])
        t.tex_img[slot]->synced = true;
    return rc;
}

int nvn_texset_sync_registered(replay *r)
{
    if (!make())
        return nvn_vk_stop(r, "cannot make the texture set");
    uint32_t ids[256], n;
    while ((n = nvn_tex_pending(true, ids, 256)) != 0)
        for (uint32_t i = 0; i < n; i++)
            if (ids[i] < SAMPLERS) {
                int rc = bind_sampler(r, ids[i]);
                if (rc != RL_OK)
                    return rc;
            }
    bool refresh = t.sync_frame != g.presents + 1;
    t.sync_frame = g.presents + 1;
    while ((n = nvn_tex_pending(false, ids, 256)) != 0)
        for (uint32_t i = 0; i < n; i++) {
            uint8_t view[17];
            bool has_view, image;
            uint64_t tex = nvn_tex_registered(ids[i], view, &has_view, &image);
            if (!tex)
                continue;
            if (!pool_live(ids[i], tex)) {
                int rc = stale_slot(r, ids[i]);
                if (rc != RL_OK)
                    return rc;
                continue;
            }
            if (ids[i] >= t.slots.pinned)
                return nvn_vk_stop(r,
                                   "texture id %u is registered, but a program reads texture handles out of memory and the "
                                   "host keeps only the ids below %u in a slot of their own",
                                   ids[i], t.slots.pinned);
            if (t.nsynced == t.capsynced) {
                uint32_t cap = t.capsynced ? 2 * t.capsynced : 1024;
                uint32_t *p = realloc(t.synced, cap * sizeof *p);
                if (!p)
                    return nvn_vk_stop(r, "out of memory");
                t.synced = p;
                t.capsynced = cap;
            }
            bool ok;
            int rc = resident_ok(r, ids[i], tex, view, has_view, &ok);
            if (rc != RL_OK)
                return rc;
            if (!ok)
                continue;
            if (!t.is_synced[ids[i]]) {
                t.is_synced[ids[i]] = true;
                t.synced[t.nsynced++] = ids[i];
                if (t.nsynced >= 1024 && !(t.nsynced & (t.nsynced - 1)))
                    rl_log(RL_LOG_INFO, "nvn", "texture set: %u registered textures kept in their slots", t.nsynced);
            }
            uint32_t kind, dim, slot;
            rc = bind_texture(r, ids[i], &kind, &dim, &slot, false, false);
            if (rc != RL_OK)
                return rc;
            if (t.tex_img[slot])
                t.tex_img[slot]->synced = true;
        }
    if (!refresh)
        return RL_OK;
    uint64_t stamp = nvn_watch_stamp();
    bool full = stamp != t.seen_stamp || t.nsynced != t.seen_count;
    t.seen_stamp = stamp;
    t.seen_count = t.nsynced;
    if (full)
        t.nloose = 0;
    uint32_t n_ids = full ? t.nsynced : t.nloose;
    for (uint32_t i = 0; i < n_ids; i++) {
        uint32_t id = full ? t.synced[i] : t.loose[i];
        int rc = refresh_synced(r, id);
        if (rc != RL_OK)
            return rc;
        vimg *v = t.tex_img[id];
        if (full && v && !v->watch.on && v->d.target != NVN_TARGET_BUFFER) {
            if (t.nloose == t.caploose) {
                uint32_t cap = t.caploose ? 2 * t.caploose : 256;
                uint32_t *p = realloc(t.loose, cap * sizeof *p);
                if (!p)
                    return nvn_vk_stop(r, "out of memory");
                t.loose = p;
                t.caploose = cap;
            }
            t.loose[t.nloose++] = id;
        }
    }
    if (!nvn_vk_sync_aliases(r, t.seen_written))
        return RL_PENDING;
    t.seen_written = g.write_stamp;
    return RL_OK;
}

int nvn_texset_bind_words(replay *r, const uint8_t *bytes, uint32_t size, uint32_t offset, bool indexed)
{
    if (!make())
        return nvn_vk_stop(r, "cannot make the texture set");
    for (uint32_t o = offset; o <= size && size - o >= 4; o += 4) {
        uint32_t handle;
        memcpy(&handle, bytes + o, 4);
        uint32_t id = handle & 0xfffffu;
        uint8_t view[17];
        bool has_view, image;
        uint64_t tex = id < t.slots.pinned ? nvn_tex_registered(id, view, &has_view, &image) : 0;
        if (tex) {
            int rc = RL_OK;
            bool ok = pool_live(id, tex);
            if (!ok)
                rc = stale_slot(r, id);
            else
                rc = resident_ok(r, id, tex, view, has_view, &ok);
            uint32_t kind, dim, slot;
            if (rc == RL_OK && ok)
                rc = bind_texture(r, handle, &kind, &dim, &slot, true, false);
            if (rc != RL_OK)
                return rc;
        }
        if (!indexed)
            break;
    }
    return RL_OK;
}

uint32_t nvn_texset_pinned(void) { return t.ok ? t.slots.pinned : 0; }

bool nvn_texset_declares_image(uint32_t binding)
{
    if (!make() || binding < IMAGE_BINDING(0, 0))
        return true;
    uint32_t k = (binding - IMAGE_BINDING(0, 0)) / 8, d = (binding - IMAGE_BINDING(0, 0)) % 8;
    return k < IMAGE_KINDS && d < IMAGE_DIMS && (t.img_used >> (k * IMAGE_DIMS + d) & 1);
}

uint32_t nvn_texset_images(void) { return make() ? t.nimg : 0; }

int nvn_texset_sampler(replay *r, uint32_t id)
{
    if (!make())
        return nvn_vk_stop(r, "cannot make the texture set");
    return bind_sampler(r, id);
}

int nvn_texset_image(replay *r, uint32_t handle, uint32_t *slot, uint32_t *kind, uint32_t *dim)
{
    uint32_t ts;
    int rc = bind_texture(r, handle, kind, dim, &ts, true, true);
    if (rc != RL_OK)
        return rc;
    uint32_t id = handle & 0xfffffu;
    if (t.img_done[ts]) {
        *slot = ts;
        return RL_OK;
    }
    if (!nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    if ((rc = bind_image(r, id, ts, t.tex_img[ts], t.tex_level[ts], t.tex_target[ts], t.tex_layer[ts],
                         t.tex_layers[ts])) != RL_OK)
        return rc;
    t.img_done[ts] = true;
    *slot = ts;
    return RL_OK;
}

uint32_t nvn_texset_format(uint32_t slot)
{
    return make() && slot < t.ntex && t.tex_img[slot] ? t.tex_img[slot]->d.format : 0;
}

uint32_t nvn_texset_surface_kind(uint32_t slot, uint32_t kind)
{
    if (!make() || slot >= t.ntex || t.img_surface[slot] == NVN_SURFACE_DIRECT)
        return kind;
    return t.img_surface[slot] == NVN_SURFACE_RGB9E5 ? NVN_SURFACE_KIND_RGB9E5 : UINT32_MAX;
}
