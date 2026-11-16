/**
 * @file vk_props.c
 * @brief The physical device as the guest sees it: the Switch driver's
 * properties, limits, features, memory types, formats and extensions
 * (vk_switch.c), whatever the host has.
 *
 * Where the host can do more than the Switch, the guest sees the Switch's
 * value and takes the paths it takes on the console. Where the host can do
 * less and a use site checks the limit (descriptor and pipeline layouts,
 * images, framebuffers), the guest still sees the Switch's value and only a
 * call that goes past the host's limit fails, with a log line naming the
 * limit and the host. Any other limit the host falls short of is the host's
 * for the guest, with a warning; under RELIFT_STRICT vkCreateDevice fails
 * instead. Features, extensions and format features are the Switch's that
 * the host also has, so a game that checks them takes its fallback on a host
 * without one, and enabling one the host lacks fails vkCreateDevice.
 */
#include "vkf.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { T_U32, T_I32, T_F32, T_U64 };

enum {
    /** The guest may use up to the value: the host's must not be smaller. */
    K_MAX,
    /** The guest may use down to the value: the host's must not be larger. */
    K_MIN,
    /** An alignment the guest keeps: the host's must not be larger. */
    K_ALIGN,
    /** A range the guest stays inside: the host's must contain it. */
    K_RANGE,
    /** Sample counts the guest may use: the host must have each. */
    K_SAMPLES,
    /** A capability the guest may rely on when true. */
    K_CAP,
    /** Precision and behavior: a difference is logged. */
    K_WARN,
    /** Describes the host's own results (timestampPeriod): the guest sees the host's value. */
    K_HOST,
    /** A performance hint: the Switch's value, never checked. */
    K_HINT,
};

typedef struct vkf_limit {
    const char *name;
    uint16_t off;
    uint8_t type, n, kind;
    /** A use site checks it against the host: U_FAIL fails the call, U_LOG logs and forwards it. */
    uint8_t use;
} vkf_limit;

enum { U_NONE, U_FAIL, U_LOG };

#define L(f, t, n, k, u) { #f, offsetof(VkPhysicalDeviceLimits, f), T_##t, n, K_##k, u }

static const vkf_limit limits[] = {
    L(maxImageDimension1D, U32, 1, MAX, U_FAIL),
    L(maxImageDimension2D, U32, 1, MAX, U_FAIL),
    L(maxImageDimension3D, U32, 1, MAX, U_FAIL),
    L(maxImageDimensionCube, U32, 1, MAX, U_FAIL),
    L(maxImageArrayLayers, U32, 1, MAX, U_FAIL),
    L(maxTexelBufferElements, U32, 1, MAX, U_NONE),
    L(maxUniformBufferRange, U32, 1, MAX, U_NONE),
    L(maxStorageBufferRange, U32, 1, MAX, U_NONE),
    L(maxPushConstantsSize, U32, 1, MAX, U_FAIL),
    L(maxMemoryAllocationCount, U32, 1, MAX, U_NONE),
    L(maxSamplerAllocationCount, U32, 1, MAX, U_NONE),
    L(bufferImageGranularity, U64, 1, ALIGN, U_NONE),
    L(sparseAddressSpaceSize, U64, 1, MAX, U_NONE),
    L(maxBoundDescriptorSets, U32, 1, MAX, U_FAIL),
    L(maxPerStageDescriptorSamplers, U32, 1, MAX, U_LOG),
    L(maxPerStageDescriptorUniformBuffers, U32, 1, MAX, U_LOG),
    L(maxPerStageDescriptorStorageBuffers, U32, 1, MAX, U_LOG),
    L(maxPerStageDescriptorSampledImages, U32, 1, MAX, U_LOG),
    L(maxPerStageDescriptorStorageImages, U32, 1, MAX, U_LOG),
    L(maxPerStageDescriptorInputAttachments, U32, 1, MAX, U_LOG),
    L(maxPerStageResources, U32, 1, MAX, U_LOG),
    L(maxDescriptorSetSamplers, U32, 1, MAX, U_LOG),
    L(maxDescriptorSetUniformBuffers, U32, 1, MAX, U_LOG),
    L(maxDescriptorSetUniformBuffersDynamic, U32, 1, MAX, U_LOG),
    L(maxDescriptorSetStorageBuffers, U32, 1, MAX, U_LOG),
    L(maxDescriptorSetStorageBuffersDynamic, U32, 1, MAX, U_LOG),
    L(maxDescriptorSetSampledImages, U32, 1, MAX, U_LOG),
    L(maxDescriptorSetStorageImages, U32, 1, MAX, U_LOG),
    L(maxDescriptorSetInputAttachments, U32, 1, MAX, U_LOG),
    L(maxVertexInputAttributes, U32, 1, MAX, U_NONE),
    L(maxVertexInputBindings, U32, 1, MAX, U_NONE),
    L(maxVertexInputAttributeOffset, U32, 1, MAX, U_NONE),
    L(maxVertexInputBindingStride, U32, 1, MAX, U_NONE),
    L(maxVertexOutputComponents, U32, 1, MAX, U_NONE),
    L(maxTessellationGenerationLevel, U32, 1, MAX, U_NONE),
    L(maxTessellationPatchSize, U32, 1, MAX, U_NONE),
    L(maxTessellationControlPerVertexInputComponents, U32, 1, MAX, U_NONE),
    L(maxTessellationControlPerVertexOutputComponents, U32, 1, MAX, U_NONE),
    L(maxTessellationControlPerPatchOutputComponents, U32, 1, MAX, U_NONE),
    L(maxTessellationControlTotalOutputComponents, U32, 1, MAX, U_NONE),
    L(maxTessellationEvaluationInputComponents, U32, 1, MAX, U_NONE),
    L(maxTessellationEvaluationOutputComponents, U32, 1, MAX, U_NONE),
    L(maxGeometryShaderInvocations, U32, 1, MAX, U_NONE),
    L(maxGeometryInputComponents, U32, 1, MAX, U_NONE),
    L(maxGeometryOutputComponents, U32, 1, MAX, U_NONE),
    L(maxGeometryOutputVertices, U32, 1, MAX, U_NONE),
    L(maxGeometryTotalOutputComponents, U32, 1, MAX, U_NONE),
    L(maxFragmentInputComponents, U32, 1, MAX, U_NONE),
    L(maxFragmentOutputAttachments, U32, 1, MAX, U_NONE),
    L(maxFragmentDualSrcAttachments, U32, 1, MAX, U_NONE),
    L(maxFragmentCombinedOutputResources, U32, 1, MAX, U_NONE),
    L(maxComputeSharedMemorySize, U32, 1, MAX, U_NONE),
    L(maxComputeWorkGroupCount, U32, 3, MAX, U_NONE),
    L(maxComputeWorkGroupInvocations, U32, 1, MAX, U_NONE),
    L(maxComputeWorkGroupSize, U32, 3, MAX, U_NONE),
    L(subPixelPrecisionBits, U32, 1, WARN, U_NONE),
    L(subTexelPrecisionBits, U32, 1, WARN, U_NONE),
    L(mipmapPrecisionBits, U32, 1, WARN, U_NONE),
    L(maxDrawIndexedIndexValue, U32, 1, MAX, U_NONE),
    L(maxDrawIndirectCount, U32, 1, MAX, U_NONE),
    L(maxSamplerLodBias, F32, 1, MAX, U_NONE),
    L(maxSamplerAnisotropy, F32, 1, MAX, U_NONE),
    L(maxViewports, U32, 1, MAX, U_NONE),
    L(maxViewportDimensions, U32, 2, MAX, U_NONE),
    L(viewportBoundsRange, F32, 2, RANGE, U_NONE),
    L(viewportSubPixelBits, U32, 1, WARN, U_NONE),
    L(minMemoryMapAlignment, U64, 1, HINT, U_NONE),
    L(minTexelBufferOffsetAlignment, U64, 1, ALIGN, U_NONE),
    L(minUniformBufferOffsetAlignment, U64, 1, ALIGN, U_NONE),
    L(minStorageBufferOffsetAlignment, U64, 1, ALIGN, U_NONE),
    L(minTexelOffset, I32, 1, MIN, U_NONE),
    L(maxTexelOffset, U32, 1, MAX, U_NONE),
    L(minTexelGatherOffset, I32, 1, MIN, U_NONE),
    L(maxTexelGatherOffset, U32, 1, MAX, U_NONE),
    L(minInterpolationOffset, F32, 1, MIN, U_NONE),
    L(maxInterpolationOffset, F32, 1, MAX, U_NONE),
    L(subPixelInterpolationOffsetBits, U32, 1, WARN, U_NONE),
    L(maxFramebufferWidth, U32, 1, MAX, U_FAIL),
    L(maxFramebufferHeight, U32, 1, MAX, U_FAIL),
    L(maxFramebufferLayers, U32, 1, MAX, U_FAIL),
    L(framebufferColorSampleCounts, U32, 1, SAMPLES, U_NONE),
    L(framebufferDepthSampleCounts, U32, 1, SAMPLES, U_NONE),
    L(framebufferStencilSampleCounts, U32, 1, SAMPLES, U_NONE),
    L(framebufferNoAttachmentsSampleCounts, U32, 1, SAMPLES, U_NONE),
    L(maxColorAttachments, U32, 1, MAX, U_NONE),
    L(sampledImageColorSampleCounts, U32, 1, SAMPLES, U_NONE),
    L(sampledImageIntegerSampleCounts, U32, 1, SAMPLES, U_NONE),
    L(sampledImageDepthSampleCounts, U32, 1, SAMPLES, U_NONE),
    L(sampledImageStencilSampleCounts, U32, 1, SAMPLES, U_NONE),
    L(storageImageSampleCounts, U32, 1, SAMPLES, U_NONE),
    L(maxSampleMaskWords, U32, 1, MAX, U_NONE),
    L(timestampComputeAndGraphics, U32, 1, CAP, U_NONE),
    L(timestampPeriod, F32, 1, HOST, U_NONE),
    L(maxClipDistances, U32, 1, MAX, U_NONE),
    L(maxCullDistances, U32, 1, MAX, U_NONE),
    L(maxCombinedClipAndCullDistances, U32, 1, MAX, U_NONE),
    L(discreteQueuePriorities, U32, 1, WARN, U_NONE),
    L(pointSizeRange, F32, 2, RANGE, U_NONE),
    L(lineWidthRange, F32, 2, RANGE, U_NONE),
    L(pointSizeGranularity, F32, 1, WARN, U_NONE),
    L(lineWidthGranularity, F32, 1, WARN, U_NONE),
    L(strictLines, U32, 1, WARN, U_NONE),
    L(standardSampleLocations, U32, 1, WARN, U_NONE),
    L(optimalBufferCopyOffsetAlignment, U64, 1, HINT, U_NONE),
    L(optimalBufferCopyRowPitchAlignment, U64, 1, HINT, U_NONE),
    L(nonCoherentAtomSize, U64, 1, ALIGN, U_NONE),
};

_Static_assert(sizeof limits / sizeof limits[0] == 106, "every member of VkPhysicalDeviceLimits");

typedef struct lval {
    double d;
    uint64_t u;
} lval;

static lval value(const vkf_limit *l, const VkPhysicalDeviceLimits *lim, unsigned i)
{
    const uint8_t *p = (const uint8_t *)lim + l->off;
    lval v = { 0, 0 };
    switch (l->type) {
    case T_U32: {
        uint32_t x;
        memcpy(&x, p + 4 * i, 4);
        v.d = x;
        v.u = x;
        break;
    }
    case T_I32: {
        int32_t x;
        memcpy(&x, p + 4 * i, 4);
        v.d = x;
        break;
    }
    case T_F32: {
        float x;
        memcpy(&x, p + 4 * i, 4);
        v.d = x;
        break;
    }
    default:
        memcpy(&v.u, p + 8 * i, 8);
        v.d = (double)v.u;
        break;
    }
    return v;
}

/** @brief True when the host's value falls short of the Switch's (K_WARN: differs). */
static bool short_of(const vkf_limit *l, lval s, lval h, unsigned i)
{
    switch (l->kind) {
    case K_MAX:
        return l->type == T_U64 ? h.u < s.u : h.d < s.d;
    case K_MIN:
        return h.d > s.d;
    case K_ALIGN:
        return h.u > s.u;
    case K_RANGE:
        return i == 0 ? h.d > s.d : h.d < s.d;
    case K_SAMPLES:
        return (s.u & ~h.u) != 0;
    case K_CAP:
        return s.u && !h.u;
    case K_WARN:
        return h.d != s.d;
    default:
        return false;
    }
}

static void show(char *buf, size_t n, const vkf_limit *l, const VkPhysicalDeviceLimits *lim)
{
    size_t at = 0;
    for (unsigned i = 0; i < l->n && at < n; i++) {
        lval v = value(l, lim, i);
        const char *sep = i ? (l->kind == K_RANGE ? " to " : ", ") : "";
        if (l->kind == K_SAMPLES)
            at += (size_t)snprintf(buf + at, n - at, "%s0x%llx", sep, (unsigned long long)v.u);
        else if (l->type == T_U64 || l->type == T_U32)
            at += (size_t)snprintf(buf + at, n - at, "%s%llu", sep, (unsigned long long)v.u);
        else if (l->type == T_I32)
            at += (size_t)snprintf(buf + at, n - at, "%s%lld", sep, (long long)v.d);
        else
            at += (size_t)snprintf(buf + at, n - at, "%s%.9g", sep, v.d);
    }
}

static bool limit_short(const vkf_limit *l, const VkPhysicalDeviceLimits *host)
{
    for (unsigned i = 0; i < l->n; i++)
        if (short_of(l, value(l, &vkf_switch.props.limits, i), value(l, host, i), i))
            return true;
    return false;
}

static void set_value(const vkf_limit *l, VkPhysicalDeviceLimits *lim, unsigned i, const VkPhysicalDeviceLimits *from)
{
    size_t w = l->type == T_U64 ? 8 : 4;
    memcpy((uint8_t *)lim + l->off + w * i, (const uint8_t *)from + l->off + w * i, w);
}

/**
 * @brief The limits the guest sees on that host: the Switch's, with the
 * host's value for each limit no use site checks that the host falls short
 * of. Logs each shortfall once; returns how many limits were lowered.
 */
static unsigned guest_limits(const VkPhysicalDeviceProperties *host, VkPhysicalDeviceLimits *out)
{
    const VkPhysicalDeviceLimits *s = &vkf_switch.props.limits;
    *out = *s;
    out->timestampPeriod = host->limits.timestampPeriod;
    unsigned lowered = 0;
    for (size_t j = 0; j < sizeof limits / sizeof limits[0]; j++) {
        const vkf_limit *l = &limits[j];
        if (l->kind == K_HOST || l->kind == K_HINT || !limit_short(l, &host->limits))
            continue;
        bool once = rl_once(0x766b6c6d00000000ull | j);
        char sv[96], hv[96];
        show(sv, sizeof sv, l, s);
        show(hv, sizeof hv, l, &host->limits);
        if (l->kind == K_WARN) {
            if (once)
                rl_log(RL_LOG_INFO, "vk", "%s: %s %s, the Switch's %s", host->deviceName, l->name, hv, sv);
            continue;
        }
        if (l->use == U_FAIL) {
            if (once)
                rl_log(RL_LOG_WARN, "vk", "%s: %s %s, below the Switch's %s; a call that needs more fails", host->deviceName,
                       l->name, hv, sv);
            continue;
        }
        if (l->use == U_LOG) {
            if (once)
                rl_log(RL_LOG_WARN, "vk", "%s: %s %s, below the Switch's %s; a call that needs more is logged and forwarded",
                       host->deviceName, l->name, hv, sv);
            continue;
        }
        for (unsigned i = 0; i < l->n; i++) {
            lval sw = value(l, s, i), h = value(l, &host->limits, i);
            if (l->kind == K_SAMPLES) {
                uint32_t m = (uint32_t)(sw.u & h.u);
                memcpy((uint8_t *)out + l->off + 4 * i, &m, 4);
            } else if (short_of(l, sw, h, i))
                set_value(l, out, i, &host->limits);
        }
        lowered++;
        if (once)
            rl_log(RL_LOG_WARN, "vk", "%s: %s %s, short of the Switch's %s; the guest sees the host's", host->deviceName, l->name,
                   hv, sv);
    }
    return lowered;
}

static const char *const feature_names[] = {
    "robustBufferAccess",
    "fullDrawIndexUint32",
    "imageCubeArray",
    "independentBlend",
    "geometryShader",
    "tessellationShader",
    "sampleRateShading",
    "dualSrcBlend",
    "logicOp",
    "multiDrawIndirect",
    "drawIndirectFirstInstance",
    "depthClamp",
    "depthBiasClamp",
    "fillModeNonSolid",
    "depthBounds",
    "wideLines",
    "largePoints",
    "alphaToOne",
    "multiViewport",
    "samplerAnisotropy",
    "textureCompressionETC2",
    "textureCompressionASTC_LDR",
    "textureCompressionBC",
    "occlusionQueryPrecise",
    "pipelineStatisticsQuery",
    "vertexPipelineStoresAndAtomics",
    "fragmentStoresAndAtomics",
    "shaderTessellationAndGeometryPointSize",
    "shaderImageGatherExtended",
    "shaderStorageImageExtendedFormats",
    "shaderStorageImageMultisample",
    "shaderStorageImageReadWithoutFormat",
    "shaderStorageImageWriteWithoutFormat",
    "shaderUniformBufferArrayDynamicIndexing",
    "shaderSampledImageArrayDynamicIndexing",
    "shaderStorageBufferArrayDynamicIndexing",
    "shaderStorageImageArrayDynamicIndexing",
    "shaderClipDistance",
    "shaderCullDistance",
    "shaderFloat64",
    "shaderInt64",
    "shaderInt16",
    "shaderResourceResidency",
    "shaderResourceMinLod",
    "sparseBinding",
    "sparseResidencyBuffer",
    "sparseResidencyImage2D",
    "sparseResidencyImage3D",
    "sparseResidency2Samples",
    "sparseResidency4Samples",
    "sparseResidency8Samples",
    "sparseResidency16Samples",
    "sparseResidencyAliased",
    "variableMultisampleRate",
    "inheritedQueries",
};

_Static_assert(sizeof feature_names / sizeof feature_names[0] == sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32),
               "every member of VkPhysicalDeviceFeatures");

static const vkf_switch_struct *find(const vkf_switch_struct *t, uint32_t n, uint32_t stype)
{
    for (uint32_t i = 0; i < n; i++)
        if ((uint32_t)t[i].stype == stype)
            return &t[i];
    return NULL;
}

/** @brief Counts the VkBool32 members after the header that want sets and have lacks; logs each. */
static unsigned missing_bools(const char *host, const char *what, const VkBool32 *want, const VkBool32 *have, uint32_t n,
                              const char *const *names)
{
    unsigned m = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!want[i] || have[i])
            continue;
        if (names)
            rl_log(RL_LOG_ERROR, "vk", "vkCreateDevice: the guest enables %s, which %s lacks", names[i], host);
        else
            rl_log(RL_LOG_ERROR, "vk", "vkCreateDevice: the guest enables member %u of %s, which %s lacks", i, what, host);
        m++;
    }
    return m;
}

static unsigned check_features(vkf_call *k, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const char *host)
{
    VkPhysicalDeviceFeatures have;
    VKF_FN(k->fns, vkGetPhysicalDeviceFeatures)(pd, &have);
    if (vkf_decode_mode() != RL_VK_DECODE_OFF)
        have.textureCompressionBC = VK_TRUE;
    unsigned m = 0;
    const uint32_t nf = sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32);
    if (ci->pEnabledFeatures)
        m += missing_bools(host, "VkPhysicalDeviceFeatures", (const VkBool32 *)ci->pEnabledFeatures, (const VkBool32 *)&have, nf,
                           feature_names);
    PFN_vkGetPhysicalDeviceFeatures2 f2 = VKF_FN(k->fns, vkGetPhysicalDeviceFeatures2);
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
            const VkPhysicalDeviceFeatures2 *w = (const VkPhysicalDeviceFeatures2 *)b;
            m += missing_bools(host, "VkPhysicalDeviceFeatures", (const VkBool32 *)&w->features, (const VkBool32 *)&have, nf,
                               feature_names);
            continue;
        }
        const vkf_switch_struct *s = find(vkf_switch.features_ext, vkf_switch.features_ext_count, (uint32_t)b->sType);
        if (!s || !f2)
            continue;
        uint8_t *q = vkf_alloc(k, s->size);
        memset(q, 0, s->size);
        ((VkBaseOutStructure *)q)->sType = b->sType;
        VkPhysicalDeviceFeatures2 w = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = q };
        f2(pd, &w);
        char what[48];
        snprintf(what, sizeof what, "the struct of sType %u", (unsigned)b->sType);
        const size_t head = sizeof(VkBaseOutStructure);
        m += missing_bools(host, what, (const VkBool32 *)((const uint8_t *)b + head), (const VkBool32 *)(q + head),
                           (uint32_t)((s->size - head) / sizeof(VkBool32)), NULL);
    }
    return m;
}

static unsigned check_extensions(vkf_call *k, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const char *host)
{
    uint32_t n = 0;
    PFN_vkEnumerateDeviceExtensionProperties f = VKF_FN(k->fns, vkEnumerateDeviceExtensionProperties);
    if (f(pd, NULL, &n, NULL) < 0)
        n = 0;
    VkExtensionProperties *p = vkf_alloc(k, (n + 1) * sizeof *p);
    if (n && f(pd, NULL, &n, p) < 0)
        n = 0;
    unsigned m = 0;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (!vkf_switch_ext(p, n, ci->ppEnabledExtensionNames[i])) {
            rl_log(RL_LOG_ERROR, "vk", "vkCreateDevice: the guest enables %s, which %s lacks", ci->ppEnabledExtensionNames[i], host);
            m++;
        }
    return m;
}

VkResult vkf_props_device_check(vkf_call *k, VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, vkf_dev *d)
{
    VkPhysicalDeviceProperties host;
    VKF_FN(k->fns, vkGetPhysicalDeviceProperties)(pd, &host);
    d->physical = pd;
    d->host_limits = host.limits;
    memcpy(d->host_name, host.deviceName, sizeof d->host_name);
    d->host_name[sizeof d->host_name - 1] = 0;
    VkPhysicalDeviceLimits g;
    unsigned lowered = guest_limits(&host, &g);
    if (lowered && rl_cfg()->strict)
        rl_log(RL_LOG_ERROR, "vk", "vkCreateDevice: %s falls short of %u of the Switch's limits (RELIFT_STRICT)", host.deviceName,
               lowered);
    unsigned bad = rl_cfg()->strict ? lowered : 0;
    unsigned feats = check_features(k, pd, ci, d->host_name);
    unsigned exts = check_extensions(k, pd, ci, d->host_name);
    if (exts)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (feats)
        return VK_ERROR_FEATURE_NOT_PRESENT;
    return bad ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS;
}


VkFormatProperties vkf_switch_format_props(VkFormat format)
{
    uint32_t lo = 0, hi = vkf_switch.format_count;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        VkFormat f = vkf_switch.formats[mid].format;
        if (f == format)
            return vkf_switch.formats[mid].props;
        if ((uint32_t)f < (uint32_t)format)
            lo = mid + 1;
        else
            hi = mid;
    }
    return (VkFormatProperties){ 0 };
}

const VkExtensionProperties *vkf_switch_ext(const VkExtensionProperties *list, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i].extensionName, name))
            return &list[i];
    return NULL;
}

static void guest_props(vkf_call *k, VkPhysicalDevice pd, VkPhysicalDeviceProperties *out)
{
    VkPhysicalDeviceProperties host;
    VKF_FN(k->fns, vkGetPhysicalDeviceProperties)(pd, &host);
    *out = vkf_switch.props;
    memcpy(out->pipelineCacheUUID, host.pipelineCacheUUID, VK_UUID_SIZE);
    guest_limits(&host, &out->limits);
}

enum { MAX_CHAIN = 64 };

/** @brief Fills the guest pNext chain at g: base at its own sType, the rest from table; others stay as the guest left them. */
static void fill_chain(vkf_call *k, uint64_t g, VkStructureType base_type, const void *base, size_t base_size,
                       const vkf_switch_struct *table, uint32_t n)
{
    const size_t head = sizeof(VkBaseOutStructure);
    for (unsigned i = 0; g && i < MAX_CHAIN; i++, g = vkf_g64(k, g + 8)) {
        uint32_t st = vkf_g32(k, g);
        if (st == (uint32_t)base_type) {
            memcpy(k->mem + g + head, base, base_size);
            continue;
        }
        const vkf_switch_struct *s = find(table, n, st);
        if (s)
            memcpy(k->mem + g + head, (const uint8_t *)s->data + head, s->size - head);
        else if (rl_cfg()->trace_os)
            rl_log(RL_LOG_INFO, "vk", "sType %u: the Switch's driver leaves it as it is", st);
    }
}

static void trace_chain(vkf_call *k, const char *cmd, uint64_t g)
{
    if (!rl_cfg()->trace_os)
        return;
    char buf[512];
    size_t at = 0;
    for (unsigned i = 0; g && i < MAX_CHAIN && at < sizeof buf; i++, g = vkf_g64(k, g + 8))
        at += (size_t)snprintf(buf + at, sizeof buf - at, " %u", vkf_g32(k, g));
    rl_log(RL_LOG_INFO, "vk", "%s:%s", cmd, at ? buf : " (none)");
}

/** @brief Device and driver UUIDs are the host's: they name its pipeline caches and its external memory. */
static void host_ids(vkf_call *k, VkPhysicalDevice pd, uint64_t g)
{
    PFN_vkGetPhysicalDeviceProperties2 f = VKF_FN(k->fns, vkGetPhysicalDeviceProperties2);
    for (unsigned i = 0; f && g && i < MAX_CHAIN; i++, g = vkf_g64(k, g + 8)) {
        if (vkf_g32(k, g) != (uint32_t)VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES)
            continue;
        VkPhysicalDeviceIDProperties id = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &id };
        f(pd, &p2);
        const size_t head = sizeof(VkBaseOutStructure);
        memcpy(k->mem + g + head, (const uint8_t *)&id + head, sizeof id - head);
    }
}

int vkh_vkGetPhysicalDeviceProperties(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceProperties, c->x[0]);
    if (c->x[1]) {
        VkPhysicalDeviceProperties p;
        guest_props(&k, pd, &p);
        memcpy(mem + c->x[1], &p, sizeof p);
    }
    return vkf_end(&k);
}

int vkh_vkGetPhysicalDeviceProperties2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceProperties2, c->x[0]);
    trace_chain(&k, "vkGetPhysicalDeviceProperties2", c->x[1]);
    VkPhysicalDeviceProperties p;
    guest_props(&k, pd, &p);
    fill_chain(&k, c->x[1], VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p, sizeof p, vkf_switch.props_ext,
               vkf_switch.props_ext_count);
    host_ids(&k, pd, c->x[1]);
    return vkf_end(&k);
}

/** @brief The Switch's features that the host also has: the core struct, then one per entry of features_ext. */
typedef struct guest_features {
    VkPhysicalDeviceFeatures core;
    vkf_switch_struct ext[MAX_CHAIN];
} guest_features;

static void and_bools(VkBool32 *a, const VkBool32 *b, uint32_t n, const char *host, const char *const *names, uint32_t stype)
{
    for (uint32_t i = 0; i < n; i++) {
        if (a[i] && !b[i] && rl_once(0x766b666500000000ull ^ ((uint64_t)stype << 8) ^ i)) {
            if (names)
                rl_log(RL_LOG_WARN, "vk", "%s lacks %s, which the Switch has; the guest does not see it", host, names[i]);
            else
                rl_log(RL_LOG_WARN, "vk", "%s lacks member %u of the features of sType %u, which the Switch has; the guest does not see it",
                       host, i, stype);
        }
        a[i] = a[i] && b[i];
    }
}

static void features_of(vkf_call *k, VkPhysicalDevice pd, guest_features *g)
{
    const size_t head = sizeof(VkBaseOutStructure);
    uint32_t n = vkf_switch.features_ext_count < MAX_CHAIN ? vkf_switch.features_ext_count : MAX_CHAIN;
    VkPhysicalDeviceFeatures2 h = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    void **hs = vkf_alloc(k, (n + 1) * sizeof *hs);
    VkBaseOutStructure *prev = (VkBaseOutStructure *)&h;
    for (uint32_t i = 0; i < n; i++) {
        const vkf_switch_struct *s = &vkf_switch.features_ext[i];
        hs[i] = vkf_alloc(k, s->size);
        memset(hs[i], 0, s->size);
        ((VkBaseOutStructure *)hs[i])->sType = s->stype;
        prev->pNext = hs[i];
        prev = hs[i];
    }
    PFN_vkGetPhysicalDeviceFeatures2 f2 = VKF_FN(k->fns, vkGetPhysicalDeviceFeatures2);
    if (f2)
        f2(pd, &h);
    else
        VKF_FN(k->fns, vkGetPhysicalDeviceFeatures)(pd, &h.features);
    VkPhysicalDeviceProperties hp;
    VKF_FN(k->fns, vkGetPhysicalDeviceProperties)(pd, &hp);
    if (vkf_decode_mode() != RL_VK_DECODE_OFF)
        h.features.textureCompressionBC = VK_TRUE;
    g->core = vkf_switch.features;
    and_bools((VkBool32 *)&g->core, (const VkBool32 *)&h.features, sizeof g->core / sizeof(VkBool32), hp.deviceName,
              feature_names, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    for (uint32_t i = 0; i < n; i++) {
        const vkf_switch_struct *s = &vkf_switch.features_ext[i];
        uint8_t *q = vkf_alloc(k, s->size);
        memcpy(q, s->data, s->size);
        if (f2)
            and_bools((VkBool32 *)(q + head), (const VkBool32 *)((const uint8_t *)hs[i] + head),
                      (uint32_t)((s->size - head) / sizeof(VkBool32)), hp.deviceName, NULL, (uint32_t)s->stype);
        else
            memset(q + head, 0, s->size - head);
        g->ext[i] = (vkf_switch_struct){ s->stype, s->size, q };
    }
}

int vkh_vkGetPhysicalDeviceFeatures(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceFeatures, c->x[0]);
    if (c->x[1]) {
        guest_features *g = vkf_alloc(&k, sizeof *g);
        features_of(&k, pd, g);
        memcpy(mem + c->x[1], &g->core, sizeof g->core);
    }
    return vkf_end(&k);
}

int vkh_vkGetPhysicalDeviceFeatures2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceFeatures2, c->x[0]);
    trace_chain(&k, "vkGetPhysicalDeviceFeatures2", c->x[1]);
    guest_features *g = vkf_alloc(&k, sizeof *g);
    features_of(&k, pd, g);
    uint32_t n = vkf_switch.features_ext_count < MAX_CHAIN ? vkf_switch.features_ext_count : MAX_CHAIN;
    fill_chain(&k, c->x[1], VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &g->core, sizeof g->core, g->ext, n);
    return vkf_end(&k);
}

int vkh_vkGetPhysicalDeviceMemoryProperties(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    if (!vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]))
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceMemoryProperties, c->x[0]);
    if (c->x[1])
        memcpy(mem + c->x[1], &vkf_switch.memory, sizeof vkf_switch.memory);
    return vkf_end(&k);
}

int vkh_vkGetPhysicalDeviceMemoryProperties2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    if (!vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]))
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceMemoryProperties2, c->x[0]);
    trace_chain(&k, "vkGetPhysicalDeviceMemoryProperties2", c->x[1]);
    fill_chain(&k, c->x[1], VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &vkf_switch.memory, sizeof vkf_switch.memory,
               NULL, 0);
    return vkf_end(&k);
}

/**
 * @brief The Switch's features of a format that the host also has; for a
 * format the layer decodes, the optimal-tiling ones the host has for the
 * format its images use instead.
 */
static VkFormatProperties guest_format(vkf_call *k, VkPhysicalDevice pd, VkFormat format)
{
    VkFormatProperties s = vkf_switch_format_props(format);
    VkFormatProperties h = { 0 };
    if (s.linearTilingFeatures || s.optimalTilingFeatures || s.bufferFeatures) {
        PFN_vkGetPhysicalDeviceFormatProperties fp = VKF_FN(k->fns, vkGetPhysicalDeviceFormatProperties);
        VkFormat sub = vkf_decode_host_format(k->fns, pd, format);
        fp(pd, sub == format ? vkf_host_format(k->fns, pd, format) : format, &h);
        if (sub != format) {
            VkFormatProperties o = { 0 };
            fp(pd, sub, &o);
            h.optimalTilingFeatures = o.optimalTilingFeatures;
        }
    }
    VkFormatProperties g = {
        .linearTilingFeatures = s.linearTilingFeatures & h.linearTilingFeatures,
        .optimalTilingFeatures = s.optimalTilingFeatures & h.optimalTilingFeatures,
        .bufferFeatures = s.bufferFeatures & h.bufferFeatures,
    };
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "vk", "format %u: Switch 0x%x 0x%x 0x%x, guest 0x%x 0x%x 0x%x", (unsigned)format, s.linearTilingFeatures,
               s.optimalTilingFeatures, s.bufferFeatures, g.linearTilingFeatures, g.optimalTilingFeatures, g.bufferFeatures);
    return g;
}

int vkh_vkGetPhysicalDeviceFormatProperties(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceFormatProperties, c->x[0]);
    VkFormatProperties g = guest_format(&k, pd, (VkFormat)(uint32_t)c->x[1]);
    if (c->x[2])
        memcpy(mem + c->x[2], &g, sizeof g);
    return vkf_end(&k);
}

int vkh_vkGetPhysicalDeviceFormatProperties2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceFormatProperties2, c->x[0]);
    VkFormatProperties g = guest_format(&k, pd, (VkFormat)(uint32_t)c->x[1]);
    fill_chain(&k, c->x[2], VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2, &g, sizeof g, NULL, 0);
    return vkf_end(&k);
}

/** @brief Limits host image format properties to what the Switch allows for that image type (one sample for a decoded format). */
static void clamp_image_format(VkImageFormatProperties *p, VkImageType type, bool decoded)
{
    const VkPhysicalDeviceLimits *s = &vkf_switch.props.limits;
    uint32_t dim = type == VK_IMAGE_TYPE_1D ? s->maxImageDimension1D
                   : type == VK_IMAGE_TYPE_3D ? s->maxImageDimension3D
                                              : s->maxImageDimension2D;
    uint32_t *e[3] = { &p->maxExtent.width, &p->maxExtent.height, &p->maxExtent.depth };
    for (unsigned i = 0; i < 3; i++)
        if (*e[i] > dim)
            *e[i] = dim;
    if (p->maxArrayLayers > s->maxImageArrayLayers)
        p->maxArrayLayers = s->maxImageArrayLayers;
    p->sampleCounts &= s->framebufferColorSampleCounts | s->framebufferDepthSampleCounts | s->sampledImageColorSampleCounts |
                       s->sampledImageDepthSampleCounts | s->storageImageSampleCounts;
    if (!p->sampleCounts || decoded)
        p->sampleCounts = VK_SAMPLE_COUNT_1_BIT;
}

/** @brief The format and flags the host is asked about for an image of the guest's. */
static VkFormat image_host_format(vkf_call *k, VkPhysicalDevice pd, VkFormat format, VkImageTiling tiling, VkImageCreateFlags *flags)
{
    VkFormat sub = tiling == VK_IMAGE_TILING_OPTIMAL ? vkf_decode_host_format(k->fns, pd, format) : format;
    if (sub == format)
        return vkf_host_format(k->fns, pd, format);
    *flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT;
    return sub;
}

static bool switch_has(VkFormat format, VkImageTiling tiling)
{
    VkFormatProperties s = vkf_switch_format_props(format);
    return tiling == VK_IMAGE_TILING_LINEAR ? s.linearTilingFeatures != 0 : s.optimalTilingFeatures != 0;
}

int vkh_vkGetPhysicalDeviceImageFormatProperties(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceImageFormatProperties, c->x[0]);
    VkFormat format = (VkFormat)(uint32_t)c->x[1];
    VkImageType type = (VkImageType)(uint32_t)c->x[2];
    VkImageTiling tiling = (VkImageTiling)(uint32_t)c->x[3];
    VkImageFormatProperties p = { 0 };
    VkResult r = VK_ERROR_FORMAT_NOT_SUPPORTED;
    bool decoded = false;
    if (switch_has(format, tiling)) {
        VkImageCreateFlags flags = (VkImageCreateFlags)c->x[5];
        VkFormat hf = image_host_format(&k, pd, format, tiling, &flags);
        decoded = vkf_decode_format_of(format) && hf != format;
        r = VKF_FN(k.fns, vkGetPhysicalDeviceImageFormatProperties)(pd, hf, type, tiling, (VkImageUsageFlags)c->x[4], flags, &p);
    }
    if (r >= 0)
        clamp_image_format(&p, type, decoded);
    if (c->x[6])
        memcpy(mem + c->x[6], &p, sizeof p);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetPhysicalDeviceImageFormatProperties, r);
    return vkf_end(&k);
}

int vkh_vkGetPhysicalDeviceImageFormatProperties2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceImageFormatProperties2, c->x[0]);
    PFN_vkGetPhysicalDeviceImageFormatProperties2 f = VKF_FN(k.fns, vkGetPhysicalDeviceImageFormatProperties2);
    if (!f)
        return vkf_no_fn(&k, VKG_vkGetPhysicalDeviceImageFormatProperties2);
    VkPhysicalDeviceImageFormatInfo2 *info = vkg_ain_VkPhysicalDeviceImageFormatInfo2(&k, c->x[1], 1);
    VkImageFormatProperties2 *out = vkg_ain_VkImageFormatProperties2(&k, c->x[2], 1);
    VkResult r = VK_ERROR_FORMAT_NOT_SUPPORTED;
    bool decoded = false;
    if (info && out && switch_has(info->format, info->tiling)) {
        VkFormat guest = info->format;
        VkImageCreateFlags flags = info->flags;
        info->format = image_host_format(&k, pd, guest, info->tiling, &info->flags);
        decoded = vkf_decode_format_of(guest) && info->format != guest;
        r = f(pd, info, out);
        info->format = guest;
        info->flags = flags;
    }
    if (out) {
        if (r >= 0)
            clamp_image_format(&out->imageFormatProperties, info->type, decoded);
        else
            memset(&out->imageFormatProperties, 0, sizeof out->imageFormatProperties);
        vkg_aout_VkImageFormatProperties2(&k, c->x[2], out, 1);
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetPhysicalDeviceImageFormatProperties2, r);
    return vkf_end(&k);
}

int vkh_vkEnumerateInstanceVersion(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    if (c->x[0])
        rl_st32(mem, c->x[0], vkf_switch.instance_version);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkEnumerateInstanceVersion, VK_SUCCESS);
    return vkf_end(&k);
}


/** @brief Per set layout: descriptors of each class in each shader stage, and in the whole set. */
typedef struct set_counts {
    uint32_t stage[6][7];
    uint32_t set[8];
} set_counts;

enum { C_SAMPLER, C_UBO, C_SSBO, C_SAMPLED, C_STORAGE_IMAGE, C_INPUT, C_RESOURCES };
enum { S_SAMPLER, S_UBO, S_UBO_DYN, S_SSBO, S_SSBO_DYN, S_SAMPLED, S_STORAGE_IMAGE, S_INPUT };

static const char *const class_limits[7] = {
    "maxPerStageDescriptorSamplers",     "maxPerStageDescriptorUniformBuffers", "maxPerStageDescriptorStorageBuffers",
    "maxPerStageDescriptorSampledImages", "maxPerStageDescriptorStorageImages",  "maxPerStageDescriptorInputAttachments",
    "maxPerStageResources",
};

static const char *const set_limits[8] = {
    "maxDescriptorSetSamplers",      "maxDescriptorSetUniformBuffers",        "maxDescriptorSetUniformBuffersDynamic",
    "maxDescriptorSetStorageBuffers", "maxDescriptorSetStorageBuffersDynamic", "maxDescriptorSetSampledImages",
    "maxDescriptorSetStorageImages",  "maxDescriptorSetInputAttachments",
};

static const char *const stage_names[6] = { "vertex", "tessellation control", "tessellation evaluation",
                                            "geometry", "fragment", "compute" };

static uint32_t class_limit(const VkPhysicalDeviceLimits *l, unsigned i)
{
    const uint32_t v[7] = {
        l->maxPerStageDescriptorSamplers,     l->maxPerStageDescriptorUniformBuffers, l->maxPerStageDescriptorStorageBuffers,
        l->maxPerStageDescriptorSampledImages, l->maxPerStageDescriptorStorageImages,  l->maxPerStageDescriptorInputAttachments,
        l->maxPerStageResources,
    };
    return v[i];
}

static uint32_t set_limit(const VkPhysicalDeviceLimits *l, unsigned i)
{
    const uint32_t v[8] = {
        l->maxDescriptorSetSamplers,      l->maxDescriptorSetUniformBuffers,        l->maxDescriptorSetUniformBuffersDynamic,
        l->maxDescriptorSetStorageBuffers, l->maxDescriptorSetStorageBuffersDynamic, l->maxDescriptorSetSampledImages,
        l->maxDescriptorSetStorageImages,  l->maxDescriptorSetInputAttachments,
    };
    return v[i];
}

/** @brief Counts the bindings of a guest VkDescriptorSetLayoutCreateInfo at g. */
static void count_set(vkf_call *k, uint64_t g, set_counts *o)
{
    memset(o, 0, sizeof *o);
    uint32_t n = vkf_g32(k, g + offsetof(VkDescriptorSetLayoutCreateInfo, bindingCount));
    uint64_t b = vkf_g64(k, g + offsetof(VkDescriptorSetLayoutCreateInfo, pBindings));
    for (uint32_t i = 0; b && i < n; i++) {
        uint64_t e = b + i * (uint64_t)sizeof(VkDescriptorSetLayoutBinding);
        uint32_t type = vkf_g32(k, e + offsetof(VkDescriptorSetLayoutBinding, descriptorType));
        uint32_t count = vkf_g32(k, e + offsetof(VkDescriptorSetLayoutBinding, descriptorCount));
        uint32_t stages = vkf_g32(k, e + offsetof(VkDescriptorSetLayoutBinding, stageFlags));
        int cl[2] = { -1, -1 }, sl[2] = { -1, -1 };
        switch (type) {
        case VK_DESCRIPTOR_TYPE_SAMPLER:
            cl[0] = C_SAMPLER, sl[0] = S_SAMPLER;
            break;
        case VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER:
            cl[0] = C_SAMPLER, cl[1] = C_SAMPLED, sl[0] = S_SAMPLER, sl[1] = S_SAMPLED;
            break;
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
        case VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER:
            cl[0] = C_SAMPLED, sl[0] = S_SAMPLED;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_IMAGE:
        case VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER:
            cl[0] = C_STORAGE_IMAGE, sl[0] = S_STORAGE_IMAGE;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
            cl[0] = C_UBO, sl[0] = S_UBO;
            break;
        case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC:
            cl[0] = C_UBO, sl[0] = S_UBO, sl[1] = S_UBO_DYN;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
            cl[0] = C_SSBO, sl[0] = S_SSBO;
            break;
        case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC:
            cl[0] = C_SSBO, sl[0] = S_SSBO, sl[1] = S_SSBO_DYN;
            break;
        case VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT:
            cl[0] = C_INPUT, sl[0] = S_INPUT;
            break;
        default:
            break;
        }
        for (unsigned j = 0; j < 2; j++)
            if (sl[j] >= 0)
                o->set[sl[j]] += count;
        for (unsigned s = 0; s < 6; s++) {
            if (!(stages & (1u << s)))
                continue;
            for (unsigned j = 0; j < 2; j++)
                if (cl[j] >= 0)
                    o->stage[s][cl[j]] += count;
            if (cl[0] >= 0)
                o->stage[s][C_RESOURCES] += count;
        }
    }
}

/**
 * @brief Logs, once per limit, a call whose descriptor counts go past a host
 * limit that is below the Switch's, and forwards it; fails it under
 * RELIFT_STRICT.
 */
static VkResult over(vkf_call *k, const char *cmd, const set_counts *t)
{
    const vkf_dev *d = k->obj->dev;
    const VkPhysicalDeviceLimits *h = &d->host_limits, *s = &vkf_switch.props.limits;
    bool strict = rl_cfg()->strict, past = false;
    for (unsigned st = 0; st < 6; st++)
        for (unsigned i = 0; i < 7; i++)
            if (t->stage[st][i] > class_limit(h, i) && class_limit(h, i) < class_limit(s, i)) {
                past = true;
                if (strict || rl_once(0x766b636c00000000ull | i))
                    rl_log(strict ? RL_LOG_ERROR : RL_LOG_WARN, "vk", "%s: %u in the %s stage; %s allows %u (%s), the Switch %u%s",
                           cmd, t->stage[st][i], stage_names[st], d->host_name, class_limit(h, i), class_limits[i],
                           class_limit(s, i), strict ? "" : "; forwarded");
            }
    for (unsigned i = 0; i < 8; i++)
        if (t->set[i] > set_limit(h, i) && set_limit(h, i) < set_limit(s, i)) {
            past = true;
            if (strict || rl_once(0x766b737400000000ull | i))
                rl_log(strict ? RL_LOG_ERROR : RL_LOG_WARN, "vk", "%s: %u descriptors; %s allows %u (%s), the Switch %u%s", cmd,
                       t->set[i], d->host_name, set_limit(h, i), set_limits[i], set_limit(s, i), strict ? "" : "; forwarded");
        }
    return past && strict ? VK_ERROR_INITIALIZATION_FAILED : VK_SUCCESS;
}

static pthread_mutex_t g_sets_lock = PTHREAD_MUTEX_INITIALIZER;
static vkf_map *g_sets;

VkResult vkf_check_set_layout(vkf_call *k)
{
    if (!k->c->x[1])
        return VK_SUCCESS;
    set_counts t;
    count_set(k, k->c->x[1], &t);
    return over(k, "vkCreateDescriptorSetLayout", &t);
}

void vkf_note_set_layout(vkf_call *k)
{
    uint64_t out = k->c->x[3];
    uint64_t h = out ? vkf_g64(k, out) : 0;
    if (!h || !k->c->x[1])
        return;
    set_counts *t = malloc(sizeof *t);
    if (!t)
        abort();
    count_set(k, k->c->x[1], t);
    pthread_mutex_lock(&g_sets_lock);
    if (!g_sets)
        g_sets = vkf_map_new();
    free(vkf_map_get(g_sets, h));
    vkf_map_put(g_sets, h, t);
    pthread_mutex_unlock(&g_sets_lock);
}

void vkf_forget_set_layout(vkf_call *k)
{
    uint64_t h = k->c->x[1];
    pthread_mutex_lock(&g_sets_lock);
    if (g_sets && h) {
        free(vkf_map_get(g_sets, h));
        vkf_map_put(g_sets, h, NULL);
    }
    pthread_mutex_unlock(&g_sets_lock);
}

VkResult vkf_check_pipeline_layout(vkf_call *k)
{
    uint64_t g = k->c->x[1];
    if (!g)
        return VK_SUCCESS;
    const vkf_dev *d = k->obj->dev;
    const VkPhysicalDeviceLimits *h = &d->host_limits, *s = &vkf_switch.props.limits;
    uint32_t sets = vkf_g32(k, g + offsetof(VkPipelineLayoutCreateInfo, setLayoutCount));
    if (sets > h->maxBoundDescriptorSets) {
        rl_log(RL_LOG_ERROR, "vk", "vkCreatePipelineLayout: %u descriptor sets; %s allows %u (maxBoundDescriptorSets), the Switch %u",
               sets, d->host_name, h->maxBoundDescriptorSets, s->maxBoundDescriptorSets);
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    uint32_t nr = vkf_g32(k, g + offsetof(VkPipelineLayoutCreateInfo, pushConstantRangeCount));
    uint64_t pr = vkf_g64(k, g + offsetof(VkPipelineLayoutCreateInfo, pPushConstantRanges));
    for (uint32_t i = 0; pr && i < nr; i++) {
        uint64_t e = pr + i * (uint64_t)sizeof(VkPushConstantRange);
        uint32_t end = vkf_g32(k, e + offsetof(VkPushConstantRange, offset)) + vkf_g32(k, e + offsetof(VkPushConstantRange, size));
        if (end > h->maxPushConstantsSize) {
            rl_log(RL_LOG_ERROR, "vk", "vkCreatePipelineLayout: push constants to byte %u; %s allows %u (maxPushConstantsSize), the Switch %u",
                   end, d->host_name, h->maxPushConstantsSize, s->maxPushConstantsSize);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    set_counts sum;
    memset(&sum, 0, sizeof sum);
    uint64_t pl = vkf_g64(k, g + offsetof(VkPipelineLayoutCreateInfo, pSetLayouts));
    pthread_mutex_lock(&g_sets_lock);
    for (uint32_t i = 0; pl && i < sets; i++) {
        const set_counts *t = g_sets ? vkf_map_get(g_sets, vkf_g64(k, pl + 8ull * i)) : NULL;
        if (!t)
            continue;
        for (unsigned st = 0; st < 6; st++)
            for (unsigned j = 0; j < 7; j++)
                sum.stage[st][j] += t->stage[st][j];
        for (unsigned j = 0; j < 8; j++)
            sum.set[j] += t->set[j];
    }
    pthread_mutex_unlock(&g_sets_lock);
    return over(k, "vkCreatePipelineLayout", &sum);
}

VkResult vkf_check_image(vkf_call *k)
{
    uint64_t g = k->c->x[1];
    if (!g)
        return VK_SUCCESS;
    const vkf_dev *d = k->obj->dev;
    const VkPhysicalDeviceLimits *h = &d->host_limits;
    uint32_t type = vkf_g32(k, g + offsetof(VkImageCreateInfo, imageType));
    uint32_t flags = vkf_g32(k, g + offsetof(VkImageCreateInfo, flags));
    uint32_t w = vkf_g32(k, g + offsetof(VkImageCreateInfo, extent));
    uint32_t ht = vkf_g32(k, g + offsetof(VkImageCreateInfo, extent) + 4);
    uint32_t dp = vkf_g32(k, g + offsetof(VkImageCreateInfo, extent) + 8);
    uint32_t layers = vkf_g32(k, g + offsetof(VkImageCreateInfo, arrayLayers));
    const char *name = "maxImageDimension2D";
    uint32_t lim = h->maxImageDimension2D, big = w > ht ? w : ht;
    if (type == VK_IMAGE_TYPE_1D)
        name = "maxImageDimension1D", lim = h->maxImageDimension1D, big = w;
    else if (type == VK_IMAGE_TYPE_3D)
        name = "maxImageDimension3D", lim = h->maxImageDimension3D, big = big > dp ? big : dp;
    else if (flags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT)
        name = "maxImageDimensionCube", lim = h->maxImageDimensionCube;
    if (big > lim) {
        rl_log(RL_LOG_ERROR, "vk", "vkCreateImage: %ux%ux%u; %s allows %u (%s)", w, ht, dp, d->host_name, lim, name);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    if (layers > h->maxImageArrayLayers) {
        rl_log(RL_LOG_ERROR, "vk", "vkCreateImage: %u layers; %s allows %u (maxImageArrayLayers)", layers, d->host_name,
               h->maxImageArrayLayers);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return VK_SUCCESS;
}

VkResult vkf_check_framebuffer(vkf_call *k)
{
    uint64_t g = k->c->x[1];
    if (!g)
        return VK_SUCCESS;
    const vkf_dev *d = k->obj->dev;
    const VkPhysicalDeviceLimits *h = &d->host_limits;
    uint32_t w = vkf_g32(k, g + offsetof(VkFramebufferCreateInfo, width));
    uint32_t ht = vkf_g32(k, g + offsetof(VkFramebufferCreateInfo, height));
    uint32_t layers = vkf_g32(k, g + offsetof(VkFramebufferCreateInfo, layers));
    if (w > h->maxFramebufferWidth || ht > h->maxFramebufferHeight || layers > h->maxFramebufferLayers) {
        rl_log(RL_LOG_ERROR, "vk", "vkCreateFramebuffer: %ux%u, %u layers; %s allows %ux%u, %u layers (maxFramebuffer*)", w, ht,
               layers, d->host_name, h->maxFramebufferWidth, h->maxFramebufferHeight, h->maxFramebufferLayers);
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    return VK_SUCCESS;
}


void vkf_mem_pick(const VkPhysicalDeviceMemoryProperties *h, const VkPhysicalDeviceMemoryProperties *s, uint32_t buffer_bits,
                  uint32_t image_bits, uint8_t *out)
{
    const uint32_t access = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    const uint32_t odd = VK_MEMORY_PROPERTY_PROTECTED_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT | 0xc0u;
    for (uint32_t i = 0; i < s->memoryTypeCount; i++) {
        uint32_t want = s->memoryTypes[i].propertyFlags;
        uint64_t size = s->memoryHeaps[s->memoryTypes[i].heapIndex].size;
        uint32_t local = want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? 0 : want & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        int best = -1, best_score = -1;
        for (uint32_t pass = 0; pass < 2 && best < 0; pass++) {
            uint32_t need = want & access;
            if (pass)
                need &= ~VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            for (uint32_t t = 0; t < h->memoryTypeCount; t++) {
                uint32_t f = h->memoryTypes[t].propertyFlags;
                if ((f & need) != need || (f & odd & ~want))
                    continue;
                int score = 0;
                if (buffer_bits & (1u << t))
                    score += 16;
                if (image_bits & (1u << t))
                    score += 8;
                if (h->memoryHeaps[h->memoryTypes[t].heapIndex].size >= size)
                    score += 4;
                if ((f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) == local)
                    score += 2;
                if ((f & access) == (want & access))
                    score += 1;
                if (score > best_score)
                    best = (int)t, best_score = score;
            }
        }
        out[i] = (uint8_t)(best < 0 ? 0 : best);
    }
}

/**
 * @brief Picks the host memory type behind each Switch type: the needed host-access bits, then a type the device's
 * buffers and images may use, then a heap at least as large as the Switch's, then device-local for the Switch's
 * device-only types and system memory for its mapped ones (the layer maps those into guest memory).
 */
void vkf_mem_types_setup(vkf_call *k, vkf_dev *d, VkPhysicalDevice pd)
{
    VKF_FN(k->fns, vkGetPhysicalDeviceMemoryProperties)(pd, &d->host_memory);
    vkf_mem_pick(&d->host_memory, &vkf_switch.memory, ~0u, ~0u, d->host_type);
}

/** @brief The host memory types a probe buffer and image of the device may use, as bit masks. */
static void resource_type_bits(const vkf_fns *fns, VkDevice dev, uint32_t *buffer_bits, uint32_t *image_bits)
{
    *buffer_bits = *image_bits = ~0u;
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = 65536,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT |
                 VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer b = NULL;
    if (VKF_FN(fns, vkCreateBuffer)(dev, &bi, NULL, &b) >= 0) {
        VkMemoryRequirements r = { 0 };
        VKF_FN(fns, vkGetBufferMemoryRequirements)(dev, b, &r);
        VKF_FN(fns, vkDestroyBuffer)(dev, b, NULL);
        *buffer_bits = r.memoryTypeBits;
    }
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { 64, 64, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage im = NULL;
    if (VKF_FN(fns, vkCreateImage)(dev, &ii, NULL, &im) >= 0) {
        VkMemoryRequirements r = { 0 };
        VKF_FN(fns, vkGetImageMemoryRequirements)(dev, im, &r);
        VKF_FN(fns, vkDestroyImage)(dev, im, NULL);
        *image_bits = r.memoryTypeBits;
    }
}

void vkf_mem_types_device(vkf_dev *d, VkDevice dev)
{
    uint32_t buffer_bits, image_bits;
    resource_type_bits(&d->fns, dev, &buffer_bits, &image_bits);
    vkf_mem_pick(&d->host_memory, &vkf_switch.memory, buffer_bits, image_bits, d->host_type);
    const VkPhysicalDeviceMemoryProperties *h = &d->host_memory;
    for (uint32_t i = 0; i < vkf_switch.memory.memoryTypeCount; i++) {
        uint32_t t = d->host_type[i];
        rl_log(RL_LOG_INFO, "vk", "Switch memory type %u (0x%x) is host type %u (0x%x)%s%s", i, vkf_switch.memory.memoryTypes[i].propertyFlags,
               t, t < h->memoryTypeCount ? h->memoryTypes[t].propertyFlags : 0u, buffer_bits & (1u << t) ? ", buffers" : "",
               image_bits & (1u << t) ? ", images" : "");
    }
}

uint32_t vkf_guest_type_bits(const vkf_dev *d, uint32_t host_bits)
{
    uint32_t g = 0;
    for (uint32_t i = 0; i < vkf_switch.memory.memoryTypeCount; i++)
        if (host_bits & (1u << d->host_type[i]))
            g |= 1u << i;
    return g;
}

static void type_bits_out(vkf_call *k, uint64_t g)
{
    if (!g || !k->obj || !k->obj->dev)
        return;
    uint32_t h = vkf_g32(k, g);
    uint32_t guest = vkf_guest_type_bits(k->obj->dev, h);
    if (h && !guest && rl_once(0x766b6d7400000000ull | h))
        rl_log(RL_LOG_ERROR, "vk", "a resource may use host memory types 0x%x, none behind a Switch memory type", h);
    rl_st32(k->mem, g, guest);
}

void vkf_mem_reqs_out(vkf_call *k)
{
    if (k->c->x[2])
        type_bits_out(k, k->c->x[2] + offsetof(VkMemoryRequirements, memoryTypeBits));
}

void vkf_mem_reqs2_out(vkf_call *k)
{
    if (k->c->x[2])
        type_bits_out(k, k->c->x[2] + offsetof(VkMemoryRequirements2, memoryRequirements) + offsetof(VkMemoryRequirements, memoryTypeBits));
}
