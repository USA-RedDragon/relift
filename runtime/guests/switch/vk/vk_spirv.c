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
 * Image types that atomics may use keep their format (an atomic needs it).
 */
#include "vkf.h"

#include <string.h>

enum {
    SPV_MAGIC = 0x07230203,
    OP_CAPABILITY = 17,
    OP_TYPE_IMAGE = 25,
    OP_IMAGE_TEXEL_POINTER = 60,
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

size_t vkf_spirv_formatless(const uint32_t *in, size_t n, uint32_t *out)
{
    if (n < 5 || in[0] != SPV_MAGIC)
        return 0;
    bool atomics = false, has_read = false, has_write = false;
    size_t last_cap = 0, changed = 0;
    for (size_t i = 5; i < n;) {
        uint32_t len = in[i] >> 16, op = in[i] & 0xFFFF;
        if (!len || i + len > n)
            return 0;
        if (op == OP_IMAGE_TEXEL_POINTER)
            atomics = true;
        if (op == OP_CAPABILITY && len >= 2) {
            last_cap = i + len;
            has_read |= in[i + 1] == CAP_READ_WITHOUT_FORMAT;
            has_write |= in[i + 1] == CAP_WRITE_WITHOUT_FORMAT;
        }
        i += len;
    }
    if (!last_cap)
        return 0;
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        uint32_t len = i < 5 ? 1 : in[i] >> 16, op = i < 5 ? 0 : in[i] & 0xFFFF;
        memcpy(out + o, in + i, len * sizeof *in);
        if (op == OP_TYPE_IMAGE && len >= 9 && in[i + 3] != DIM_SUBPASS && in[i + 7] == SAMPLED_STORAGE && in[i + 8] &&
            !(atomics && atomic_format(in[i + 8]))) {
            out[o + 8] = 0;
            changed++;
        }
        o += len;
        i += len;
        if (i == last_cap) {
            if (!has_read) {
                out[o++] = (2u << 16) | OP_CAPABILITY;
                out[o++] = CAP_READ_WITHOUT_FORMAT;
            }
            if (!has_write) {
                out[o++] = (2u << 16) | OP_CAPABILITY;
                out[o++] = CAP_WRITE_WITHOUT_FORMAT;
            }
        }
    }
    return changed ? o : 0;
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
