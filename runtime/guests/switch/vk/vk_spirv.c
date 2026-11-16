/**
 * @file vk_spirv.c
 * @brief Storage images and texel buffers accessed in their view's format.
 *
 * On the console a shader's storage image and storage texel buffer accesses
 * take their format from the view bound (Maxwell's formatted surface loads
 * and stores carry no format), so a game may declare one format in the
 * shader and bind a view of another (one game's SMAA edge pass declares rg32f
 * on an R8G8B8A8_UNORM view). In Vulkan that is undefined: NVIDIA and
 * llvmpipe write all four components in the view's format, RADV only the
 * shader's two. On a device with the formatless storage features the layer
 * rewrites each such OpTypeImage format to Unknown, which makes every host
 * use the view's format, and declares the two formatless capabilities.
 * Image types that atomics may use keep their format (an atomic needs it),
 * and so do image types written with fewer than four components (DXC writes
 * a float2 as two): a formatless write must give every component of the
 * view, and the console's values for the ones the shader leaves out are not
 * known.
 */
#include "vkf.h"

#include <stdlib.h>
#include <string.h>

enum {
    SPV_MAGIC = 0x07230203,
    OP_CAPABILITY = 17,
    OP_TYPE_IMAGE = 25,
    OP_TYPE_VECTOR = 23,
    OP_IMAGE_TEXEL_POINTER = 60,
    OP_IMAGE_WRITE = 99,
    CAP_READ_WITHOUT_FORMAT = 55,
    CAP_WRITE_WITHOUT_FORMAT = 56,
    DIM_SUBPASS = 6,
    SAMPLED_STORAGE = 2,
    FMT_R32F = 3,
    FMT_R32I = 24,
    FMT_R32UI = 33,
    FMT_R64UI = 40,
    FMT_R64I = 41,
};

static bool atomic_format(uint32_t f)
{
    return f == FMT_R32F || f == FMT_R32I || f == FMT_R32UI || f == FMT_R64UI || f == FMT_R64I;
}

/** @brief Whether opcode op has a result type in word 1 and a result id in word 2. */
static bool typed_result(uint32_t op)
{
    return op == 4 || (op >= 41 && op <= 52 && op != 49 && op != 50) || op == 54 || op == 59 || op == 61 || op == 65 || op == 66 ||
           (op >= 77 && op <= 98) || (op >= 100 && op <= 105) || (op >= 109 && op <= 202) || (op >= 227 && op <= 242) ||
           (op >= 245 && op <= 255);
}

/** @brief What the rewrite needs to know of a module: its atomics, capabilities and the image types written with fewer than four components. */
typedef struct scan {
    bool atomics, has_read, has_write;
    size_t last_cap;
    uint32_t bound;
    uint32_t *type_of;
    uint8_t *width;
    uint8_t *short_write;
} scan;

static bool scan_module(const uint32_t *in, size_t n, scan *sc)
{
    for (size_t i = 5; i < n;) {
        uint32_t len = in[i] >> 16, op = in[i] & 0xFFFF;
        if (!len || i + len > n)
            return false;
        if (op == OP_IMAGE_TEXEL_POINTER)
            sc->atomics = true;
        if (op == OP_CAPABILITY && len >= 2) {
            sc->last_cap = i + len;
            sc->has_read |= in[i + 1] == CAP_READ_WITHOUT_FORMAT;
            sc->has_write |= in[i + 1] == CAP_WRITE_WITHOUT_FORMAT;
        }
        if (op == OP_TYPE_VECTOR && len >= 4 && in[i + 1] < sc->bound)
            sc->width[in[i + 1]] = (uint8_t)(in[i + 3] > 255 ? 255 : in[i + 3]);
        else if (typed_result(op) && len >= 3 && in[i + 2] < sc->bound)
            sc->type_of[in[i + 2]] = in[i + 1];
        if (op == OP_IMAGE_WRITE && len >= 4 && in[i + 1] < sc->bound && in[i + 3] < sc->bound) {
            uint32_t img = sc->type_of[in[i + 1]], texel = sc->type_of[in[i + 3]];
            uint32_t w = texel < sc->bound && sc->width[texel] ? sc->width[texel] : 1;
            if (img < sc->bound && w < 4)
                sc->short_write[img] = 1;
        }
        i += len;
    }
    return sc->last_cap != 0;
}

static size_t rewrite(const uint32_t *in, size_t n, const scan *sc, uint32_t *out)
{
    size_t o = 0, changed = 0;
    for (size_t i = 0; i < n;) {
        uint32_t len = i < 5 ? 1 : in[i] >> 16, op = i < 5 ? 0 : in[i] & 0xFFFF;
        memcpy(out + o, in + i, len * sizeof *in);
        if (op == OP_TYPE_IMAGE && len >= 9 && in[i + 3] != DIM_SUBPASS && in[i + 7] == SAMPLED_STORAGE && in[i + 8] &&
            !(sc->atomics && atomic_format(in[i + 8])) && !(in[i + 1] < sc->bound && sc->short_write[in[i + 1]])) {
            out[o + 8] = 0;
            changed++;
        }
        o += len;
        i += len;
        if (i == sc->last_cap) {
            if (!sc->has_read) {
                out[o++] = (2u << 16) | OP_CAPABILITY;
                out[o++] = CAP_READ_WITHOUT_FORMAT;
            }
            if (!sc->has_write) {
                out[o++] = (2u << 16) | OP_CAPABILITY;
                out[o++] = CAP_WRITE_WITHOUT_FORMAT;
            }
        }
    }
    return changed ? o : 0;
}

size_t vkf_spirv_formatless(const uint32_t *in, size_t n, uint32_t *out)
{
    if (n < 5 || in[0] != SPV_MAGIC || in[3] > (1u << 22))
        return 0;
    scan sc = { .bound = in[3] };
    sc.type_of = calloc(sc.bound + 1, sizeof *sc.type_of);
    sc.width = calloc(sc.bound + 1, 1);
    sc.short_write = calloc(sc.bound + 1, 1);
    size_t m = sc.type_of && sc.width && sc.short_write && scan_module(in, n, &sc) ? rewrite(in, n, &sc, out) : 0;
    free(sc.type_of);
    free(sc.width);
    free(sc.short_write);
    return m;
}

void vkf_spirv_device_features(vkf_call *k, vkf_dev *d, VkPhysicalDevice pd, VkDeviceCreateInfo *ci)
{
    VkPhysicalDeviceFeatures have;
    memset(&have, 0, sizeof have);
    VKF_FN(k->fns, vkGetPhysicalDeviceFeatures)(pd, &have);
    d->formatless = have.shaderStorageImageReadWithoutFormat && have.shaderStorageImageWriteWithoutFormat;
    if (!d->formatless)
        return;
    VkPhysicalDeviceFeatures2 *f2 = NULL;
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)
            f2 = (VkPhysicalDeviceFeatures2 *)b;
    if (f2) {
        f2->features.shaderStorageImageReadWithoutFormat = VK_TRUE;
        f2->features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
        return;
    }
    VkPhysicalDeviceFeatures *f = vkf_alloc(k, sizeof *f);
    if (ci->pEnabledFeatures)
        *f = *ci->pEnabledFeatures;
    else
        memset(f, 0, sizeof *f);
    f->shaderStorageImageReadWithoutFormat = VK_TRUE;
    f->shaderStorageImageWriteWithoutFormat = VK_TRUE;
    ci->pEnabledFeatures = f;
}

int vkh_vkCreateShaderModule(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkCreateShaderModule, c->x[0]);
    VkShaderModuleCreateInfo *ci = vkg_ain_VkShaderModuleCreateInfo(&k, c->x[1], 1);
    if (ci && ci->pCode && k.obj->dev->formatless) {
        size_t n = ci->codeSize / 4;
        uint32_t *out = vkf_alloc(&k, (n + 4) * sizeof *out);
        size_t m = vkf_spirv_formatless(ci->pCode, n, out);
        if (m) {
            if (rl_once(0x464D544C45535300ull))
                rl_log(RL_LOG_INFO, "vk", "storage images with a format in the shader use their view's format");
            ci->pCode = out;
            ci->codeSize = m * 4;
        }
    }
    VkShaderModule *outm = vkf_ptr(&k, c->x[3]);
    VkResult r = VKF_FN(k.fns, vkCreateShaderModule)(dev, ci, NULL, outm);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateShaderModule, r);
    return vkf_end(&k);
}
