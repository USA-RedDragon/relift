/**
 * @file nvn_texset.c
 * @brief The texture set (descriptor set 2) the translated programs sample
 * through: the guest's texture and sampler pools as descriptor arrays
 * indexed by pool id.
 *
 * Binding 0 holds the sampler pool, binding 1 + 10 kind + dim the textures
 * of numeric kind kind (0 float, 1 signed, 2 unsigned) seen as dimension
 * dim (the translator's Dim), binding 31 a storage buffer with the Dim of
 * each texture id. One set serves every draw; an element is written the
 * first time a draw reads its handle, and written again when the pool
 * entry changed. Elements no draw named are left unwritten (the arrays are
 * partially bound), and the programs only read the ones their handles name.
 */
#include "nvn_vk.h"

#include <stdlib.h>

#define TEXTURES 32768u
#define SAMPLERS 4096u
#define KINDS 3u
#define DIMS 9u
#define DIMS_BINDING 31u

enum { DIM_1D, DIM_2D, DIM_3D, DIM_CUBE, DIM_1D_ARRAY, DIM_2D_ARRAY, DIM_CUBE_ARRAY, DIM_2D_MS, DIM_2D_MS_ARRAY };

static struct {
    bool tried, ok;
    VkDescriptorSetLayout layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    staging dims;
    /** @brief The image each texture id's elements hold, the guest texture and view bytes registered then, and the views made for a texture view. */
    vimg **tex_img;
    uint64_t *tex_guest;
    uint8_t (*tex_view)[17];
    VkImageView (*tex_views)[DIMS];
    /** @brief The sampler each sampler id's element holds and the builder bytes it was made from. */
    VkSampler *samplers;
    uint64_t (*sampler_bytes)[16];
} t;

static bool make(void)
{
    if (t.tried)
        return t.ok;
    t.tried = true;
    VkDescriptorSetLayoutBinding b[2 + KINDS * DIMS];
    VkDescriptorBindingFlags f[2 + KINDS * DIMS];
    uint32_t n = 0;
    const VkDescriptorBindingFlags partial =
        VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    b[n] = (VkDescriptorSetLayoutBinding){ 0, VK_DESCRIPTOR_TYPE_SAMPLER, SAMPLERS, VK_SHADER_STAGE_ALL, NULL };
    f[n++] = partial;
    for (uint32_t k = 0; k < KINDS; k++)
        for (uint32_t d = 0; d < DIMS; d++) {
            b[n] = (VkDescriptorSetLayoutBinding){ 1 + 10 * k + d, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TEXTURES,
                                                   VK_SHADER_STAGE_ALL, NULL };
            f[n++] = partial;
        }
    b[n] = (VkDescriptorSetLayoutBinding){ DIMS_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_ALL,
                                           NULL };
    f[n++] = VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
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
    VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_SAMPLER, SAMPLERS },
                                   { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, TEXTURES * KINDS * DIMS },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT,
                                       .maxSets = 1,
                                       .poolSizeCount = 3,
                                       .pPoolSizes = ps };
    if (VK(vkCreateDescriptorPool)(g.dev, &pci, NULL, &t.pool) != VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = t.pool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &t.layout };
    if (VK(vkAllocateDescriptorSets)(g.dev, &ai, &t.set) != VK_SUCCESS)
        return false;
    if (!nvn_vk_staging_new(&t.dims, 4ull * TEXTURES))
        return false;
    memset(t.dims.map, 0, 4ull * TEXTURES);
    VkDescriptorBufferInfo bi = { t.dims.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet w = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                               .dstSet = t.set,
                               .dstBinding = DIMS_BINDING,
                               .descriptorCount = 1,
                               .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                               .pBufferInfo = &bi };
    VK(vkUpdateDescriptorSets)(g.dev, 1, &w, 0, NULL);
    t.tex_img = calloc(TEXTURES, sizeof *t.tex_img);
    t.tex_guest = calloc(TEXTURES, sizeof *t.tex_guest);
    t.tex_view = calloc(TEXTURES, sizeof *t.tex_view);
    t.tex_views = calloc(TEXTURES, sizeof *t.tex_views);
    t.samplers = calloc(SAMPLERS, sizeof *t.samplers);
    t.sampler_bytes = calloc(SAMPLERS, sizeof *t.sampler_bytes);
    t.ok = t.tex_img && t.tex_guest && t.tex_view && t.tex_views && t.samplers && t.sampler_bytes;
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
    static const uint8_t map[] = { DIM_1D,         DIM_2D,          DIM_3D,   DIM_1D_ARRAY, DIM_2D_ARRAY, DIM_2D_MS,
                                   DIM_2D_MS_ARRAY, DIM_2D,         DIM_CUBE, DIM_CUBE_ARRAY };
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
    const uint8_t *bytes = nvn_sampler_registered(id);
    if (!bytes)
        return nvn_vk_stop(r, "sampler id %u is not registered", id);
    if (t.samplers[id] && !memcmp(t.sampler_bytes[id], bytes, 128))
        return RL_OK;
    VkSampler s;
    int rc = make_sampler(r, (const uint64_t *)bytes, &s);
    if (rc != RL_OK)
        return rc;
    if (t.samplers[id]) {
        if (!nvn_vk_flush(r))
            return nvn_vk_stop(r, "the host device failed a submit");
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

int nvn_texset_bind(replay *r, uint32_t handle, uint32_t *kind, uint32_t *dim)
{
    if (!make())
        return nvn_vk_stop(r, "cannot make the texture set");
    uint32_t id = handle & 0xfffffu, sid = handle >> 20;
    if (id >= TEXTURES)
        return nvn_vk_stop(r, "texture id %u is past the layer's %u", id, TEXTURES);
    uint8_t view[17] = { 0 };
    bool has_view = false, image = false;
    uint64_t tex = nvn_tex_registered(id, view, &has_view, &image);
    if (!tex)
        return nvn_vk_stop(r, "texture id %u (handle 0x%x) is not registered", id, handle);
    view[16] = has_view;
    vimg *v = nvn_vk_image(r, tex);
    if (!v)
        return RL_PENDING;
    if (t.tex_img[id] == v && t.tex_guest[id] == tex && !memcmp(t.tex_view[id], view, sizeof view)) {
        *kind = kind_of(v->format);
        memcpy(dim, t.dims.map + 4ull * id, 4);
        return bind_sampler(r, sid);
    }
    nvn_view nv;
    if (has_view) {
        int rc = nvn_view_read(r->c, r->mem, view, &nv);
        if (rc != RL_OK)
            return rc;
        if (nv.has_format && nv.format != v->d.format)
            return nvn_vk_stop(r, "texture id %u is registered with a view of format %u over format %u", id, nv.format,
                               v->d.format);
        if ((nv.has_levels && (nv.base_level < 0 || nv.num_levels <= 0)) ||
            (nv.has_layers && (nv.min_layer < 0 || nv.num_layers <= 0)))
            return nvn_vk_stop(r, "texture id %u is registered with a view of levels %d+%d, layers %d+%d", id,
                               nv.base_level, nv.num_levels, nv.min_layer, nv.num_layers);
    }
    *kind = kind_of(v->format);
    *dim = dim_of(has_view && nv.has_target ? nv.target : v->d.target);
    if (t.tex_img[id] && !nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    for (uint32_t d = 0; d < DIMS; d++)
        if (t.tex_views[id][d]) {
            VK(vkDestroyImageView)(g.dev, t.tex_views[id][d], NULL);
            t.tex_views[id][d] = NULL;
        }
    VkDescriptorImageInfo ii[DIMS];
    VkWriteDescriptorSet w[DIMS];
    uint32_t n = 0, fit = dims_fitting(*dim);
    for (uint32_t d = 0; d < DIMS; d++) {
        if (!(fit & (1u << d)))
            continue;
        VkImageView iv = nvn_vk_view(v, view_types[d], has_view ? &nv : NULL);
        if (!iv)
            return nvn_vk_stop(r, "cannot make a view of texture id %u as dimension %u", id, d);
        if (has_view)
            t.tex_views[id][d] = iv;
        ii[n] = (VkDescriptorImageInfo){ NULL, iv, VK_IMAGE_LAYOUT_GENERAL };
        w[n] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                       .dstSet = t.set,
                                       .dstBinding = 1 + 10 * *kind + d,
                                       .dstArrayElement = id,
                                       .descriptorCount = 1,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,
                                       .pImageInfo = &ii[n] };
        n++;
    }
    VK(vkUpdateDescriptorSets)(g.dev, n, w, 0, NULL);
    t.tex_img[id] = v;
    t.tex_guest[id] = tex;
    memcpy(t.tex_view[id], view, sizeof view);
    memcpy(t.dims.map + 4ull * id, dim, 4);
    return bind_sampler(r, sid);
}
