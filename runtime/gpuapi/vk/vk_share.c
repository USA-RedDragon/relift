/**
 * @file vk_share.c
 * @brief The guest's instance and device on the core's (core/vkdev.h), so
 * forwarded Vulkan, the presenter and the picture pipeline use one host
 * device.
 *
 * The layer asks at the guest's first vkCreateInstance, and only when the
 * core device is not made yet; a program that never creates an instance (an
 * NVN game whose vulkan module is loaded but unused) or whose core device
 * already exists (the NVN layer made it first) asks for nothing, so its core
 * device is exactly what it is without the layer, and a guest instance there
 * gets its own host objects as before. The ask adds the
 * instance extensions the layer forwards and, on the device, the profile's
 * device extensions and the layer's own (placed or imported memory, device
 * fault, exact depth bias, present id and wait, swapchain) that the host
 * has, and the host's features for each struct no other user of the core
 * device chained already. Three are left off: robustBufferAccess and
 * robustImageAccess, which change what out-of-bounds accesses do, and
 * multi-device buffer addresses. A guest that enables one of them gets its
 * own device, so no guest runs with a robustness it did not ask for.
 *
 * The guest's vkCreateInstance takes the core's instance when it carries no
 * pNext and every extension it needs is on it; its vkCreateDevice takes the
 * core's device when every extension, feature and queue flag of the final
 * request (the guest's plus the layer's) is on the core device and the
 * core's queue family is 0. Otherwise each gets its own host object, as
 * without sharing, and the log says why. What the guest sees through the
 * property and feature queries is the profile filter's either way. The
 * guest never destroys a shared object: vkDestroyDevice waits for the
 * guest queue, every guest queue is the core's guest queue under
 * rl_vkdev_guest_queue_lock, and vkDeviceWaitIdle waits for that queue
 * only. One guest instance and one guest device share at a time.
 */
#include "vkf.h"

#include "vkdev.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/** @brief VkPhysicalDevicePresentIdFeaturesKHR and VkPhysicalDevicePresentWaitFeaturesKHR, which the generated types leave out. */
typedef struct present_feature {
    VkStructureType sType;
    void *pNext;
    VkBool32 enabled;
} present_feature;

#define STYPE_PRESENT_ID_FEATURES ((VkStructureType)1000294001)
#define STYPE_PRESENT_WAIT_FEATURES ((VkStructureType)1000248000)
#define PRESENT_ID_EXT "VK_KHR_present_id"
#define PRESENT_WAIT_EXT "VK_KHR_present_wait"

static bool g_asked;
static bool g_hook_ran;
static uint32_t g_inst_users, g_dev_users;

/** @brief Whether the host lists name. */
static bool listed(const VkExtensionProperties *p, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(p[i].extensionName, name))
            return true;
    return false;
}

/** @brief The feature structs the hook adds when no other user of the core device added one of the same sType. */
static struct {
    VkPhysicalDeviceFeatures2 f2;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceMapMemoryPlacedFeaturesEXT placed;
    VkPhysicalDeviceFaultFeaturesEXT fault;
    VkPhysicalDeviceDepthBiasControlFeaturesEXT bias;
    present_feature id;
    present_feature wait;
} g_feat;

/** @brief Whether b's chain already holds a struct of sType t. */
static bool chained(const rl_vkdev_build *b, VkStructureType t)
{
    for (const VkBaseInStructure *l = b->pnext; l; l = l->pNext)
        if (l->sType == t)
            return true;
    return false;
}

/** @brief Links one struct (its pNext cleared) in front of b's chain when the chain has none of its sType. */
static void add_struct(rl_vkdev_build *b, void *s)
{
    VkBaseOutStructure *o = s;
    if (chained(b, o->sType))
        return;
    o->pNext = NULL;
    rl_vkdev_chain(b, o);
}

/**
 * @brief The core device's hook: the profile's device extensions and the layer's own that the host has, and the
 * host's features, except the ones that change what out-of-bounds accesses do (robustBufferAccess,
 * robustImageAccess) and multi-device addresses: a guest that enables one of those gets its own device.
 */
static bool share_hook(void *user, rl_vkdev_build *b)
{
    (void)user;
    PFN_vkEnumerateDeviceExtensionProperties list =
        (PFN_vkEnumerateDeviceExtensionProperties)b->gipa(b->instance, "vkEnumerateDeviceExtensionProperties");
    PFN_vkGetPhysicalDeviceFeatures2 gf2 =
        (PFN_vkGetPhysicalDeviceFeatures2)b->gipa(b->instance, "vkGetPhysicalDeviceFeatures2");
    if (!list || !gf2)
        return true;
    VkPhysicalDevice pd = (VkPhysicalDevice)b->phys;
    uint32_t n = 0;
    if (list(pd, NULL, &n, NULL) < 0)
        return true;
    VkExtensionProperties *p = calloc(n + 1, sizeof *p);
    if (!p)
        return false;
    if (list(pd, NULL, &n, p) < 0)
        n = 0;
    bool ok = true;
    for (uint32_t i = 0; i < vkf_switch.device_ext_count && ok; i++) {
        const char *e = vkf_switch.device_exts[i].extensionName;
        if (listed(p, n, e))
            ok = rl_vkdev_add_ext(b, e);
    }
    bool placed = listed(p, n, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) && listed(p, n, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME);
    bool fault = listed(p, n, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    bool bias = listed(p, n, VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME);
    bool present = listed(p, n, PRESENT_ID_EXT) && listed(p, n, PRESENT_WAIT_EXT);
    static const char *const own[] = {
        VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
        VK_KHR_SWAPCHAIN_EXTENSION_NAME,
    };
    for (size_t i = 0; i < sizeof own / sizeof own[0] && ok; i++)
        if (listed(p, n, own[i]))
            ok = rl_vkdev_add_ext(b, own[i]);
    free(p);
    VkPhysicalDeviceProperties props;
    PFN_vkGetPhysicalDeviceProperties gp =
        (PFN_vkGetPhysicalDeviceProperties)b->gipa(b->instance, "vkGetPhysicalDeviceProperties");
    gp(pd, &props);
    g_feat.f2 = (VkPhysicalDeviceFeatures2){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 };
    g_feat.f11 = (VkPhysicalDeviceVulkan11Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES };
    g_feat.f12 = (VkPhysicalDeviceVulkan12Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    g_feat.f13 = (VkPhysicalDeviceVulkan13Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    g_feat.placed = (VkPhysicalDeviceMapMemoryPlacedFeaturesEXT){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT };
    g_feat.fault = (VkPhysicalDeviceFaultFeaturesEXT){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
    g_feat.bias = (VkPhysicalDeviceDepthBiasControlFeaturesEXT){
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT };
    VkBaseOutStructure *q[7];
    uint32_t nq = 0;
    q[nq++] = (VkBaseOutStructure *)&g_feat.f2;
    q[nq++] = (VkBaseOutStructure *)&g_feat.f11;
    if (props.apiVersion >= VKF_API_VERSION(1, 2))
        q[nq++] = (VkBaseOutStructure *)&g_feat.f12;
    if (props.apiVersion >= VKF_API_VERSION(1, 3))
        q[nq++] = (VkBaseOutStructure *)&g_feat.f13;
    if (placed)
        q[nq++] = (VkBaseOutStructure *)&g_feat.placed;
    if (fault)
        q[nq++] = (VkBaseOutStructure *)&g_feat.fault;
    if (bias)
        q[nq++] = (VkBaseOutStructure *)&g_feat.bias;
    for (uint32_t i = 0; i + 1 < nq; i++)
        q[i]->pNext = q[i + 1];
    gf2(pd, &g_feat.f2);
    g_feat.f2.features.robustBufferAccess = VK_FALSE;
    g_feat.f13.robustImageAccess = VK_FALSE;
    g_feat.f12.bufferDeviceAddressMultiDevice = VK_FALSE;
    g_feat.fault.deviceFaultVendorBinary = VK_FALSE;
    if (placed && g_feat.placed.memoryMapPlaced && g_feat.placed.memoryUnmapReserve && ok) {
        ok = rl_vkdev_add_ext(b, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME) &&
             rl_vkdev_add_ext(b, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME);
        add_struct(b, &g_feat.placed);
    }
    if (fault && g_feat.fault.deviceFault && ok) {
        ok = rl_vkdev_add_ext(b, VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
        add_struct(b, &g_feat.fault);
    }
    if (bias && g_feat.bias.depthBiasControl && ok) {
        ok = rl_vkdev_add_ext(b, VK_EXT_DEPTH_BIAS_CONTROL_EXTENSION_NAME);
        add_struct(b, &g_feat.bias);
    }
    if (present && ok && !chained(b, STYPE_PRESENT_ID_FEATURES)) {
        g_feat.wait = (present_feature){ .sType = STYPE_PRESENT_WAIT_FEATURES };
        g_feat.id = (present_feature){ .sType = STYPE_PRESENT_ID_FEATURES, .pNext = &g_feat.wait };
        VkPhysicalDeviceFeatures2 pq = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &g_feat.id };
        gf2(pd, &pq);
        if (g_feat.id.enabled && g_feat.wait.enabled) {
            ok = rl_vkdev_add_ext(b, PRESENT_ID_EXT) && rl_vkdev_add_ext(b, PRESENT_WAIT_EXT);
            g_feat.wait.pNext = NULL;
            rl_vkdev_chain(b, &g_feat.id);
        }
    }
    if (!chained(b, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2)) {
        if (props.apiVersion >= VKF_API_VERSION(1, 3))
            add_struct(b, &g_feat.f13);
        if (props.apiVersion >= VKF_API_VERSION(1, 2))
            add_struct(b, &g_feat.f12);
        add_struct(b, &g_feat.f11);
        add_struct(b, &g_feat.f2);
    }
    if (!ok)
        rl_log(RL_LOG_WARN, "vk", "the core device's extension list is full; forwarded Vulkan keeps its own device");
    g_hook_ran = ok;
    return true;
}

void vkf_share_ask(void)
{
    if (g_asked)
        return;
    if (rl_vkdev_made()) {
        rl_log(RL_LOG_INFO, "vk", "the core device was made before Vulkan forwarding started; the guest gets its own");
        return;
    }
    g_asked = true;
    const rl_vk_surface_provider *sp = vkf.cfg.surfaces;
    for (const char *const *e = sp ? sp->instance_extensions : NULL; e && *e; e++)
        rl_vkdev_want_instance_ext("Vulkan forwarding", *e);
    for (const char *const *e = vkg_instance_exts; *e; e++)
        rl_vkdev_want_instance_ext("Vulkan forwarding", *e);
    rl_vkdev_require("Vulkan forwarding", VKF_API_VERSION(1, 3), VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, share_hook,
                     NULL);
}

/** @brief The core device when sharing is possible at all: asked for, made with the hook, on the layer's loader. */
static const rlvk_dev *core_for_sharing(void)
{
    if (!g_asked)
        return NULL;
    const rlvk_dev *d = rl_vkdev();
    void *gipa;
    memcpy(&gipa, &vkf.gipa, sizeof gipa);
    if (!d || !g_hook_ran || (void *)(uintptr_t)d->gipa != gipa)
        return NULL;
    return d;
}

static bool in_names(const char *const *list, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(list[i], name))
            return true;
    return false;
}

VkInstance vkf_share_instance(const VkInstanceCreateInfo *ci)
{
    const rlvk_dev *d = core_for_sharing();
    if (!d)
        return NULL;
    if (g_inst_users) {
        rl_log(RL_LOG_INFO, "vk", "vkCreateInstance: a second instance while one shares the core's; it gets its own");
        return NULL;
    }
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (!in_names(d->instance_exts, d->ninstance_exts, ci->ppEnabledExtensionNames[i])) {
            rl_log(RL_LOG_INFO, "vk", "vkCreateInstance: %s is not on the core's instance; the guest gets its own",
                   ci->ppEnabledExtensionNames[i]);
            return NULL;
        }
    g_inst_users++;
    rl_log(RL_LOG_INFO, "vk", "the guest's instance is the core's");
    return (VkInstance)(uintptr_t)d->instance;
}

void vkf_share_instance_done(void) { g_inst_users--; }

VkPhysicalDevice vkf_share_physical(const vkf_inst *inst)
{
    const rlvk_dev *d = inst->shared ? core_for_sharing() : NULL;
    return d ? (VkPhysicalDevice)(uintptr_t)d->phys : NULL;
}

/** @brief A struct of the core device's chain by its sType, or NULL. */
static const VkBaseInStructure *core_struct(const rlvk_dev *d, VkStructureType t)
{
    for (const VkBaseInStructure *b = d->device_pnext; b; b = b->pNext)
        if (b->sType == t)
            return b;
    return NULL;
}

/** @brief Whether every VkBool32 set in want (n of them) is set in have. */
static bool bools_within(const VkBool32 *want, const VkBool32 *have, size_t n)
{
    for (size_t i = 0; i < n; i++)
        if (want[i] && !(have && have[i]))
            return false;
    return true;
}

/** @brief A struct promoted to Vulkan 1.1, 1.2 or 1.3 and where each of its members sits in the version's struct. */
typedef struct promoted {
    VkStructureType stype, into;
    uint16_t n;
    uint16_t from[20], to[20];
} promoted;

#define P(src, dst, field) offsetof(src, field)
#define Q(src, dst, field) offsetof(dst, field)

#define F11 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES
#define F12 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
#define F13 VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES
#define V11 VkPhysicalDeviceVulkan11Features
#define V12 VkPhysicalDeviceVulkan12Features
#define V13 VkPhysicalDeviceVulkan13Features
#define DI VkPhysicalDeviceDescriptorIndexingFeatures

static const promoted g_promoted[] = {
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_16BIT_STORAGE_FEATURES, F11, 4,
      { P(VkPhysicalDevice16BitStorageFeatures, V11, storageBuffer16BitAccess),
        P(VkPhysicalDevice16BitStorageFeatures, V11, uniformAndStorageBuffer16BitAccess),
        P(VkPhysicalDevice16BitStorageFeatures, V11, storagePushConstant16),
        P(VkPhysicalDevice16BitStorageFeatures, V11, storageInputOutput16) },
      { Q(VkPhysicalDevice16BitStorageFeatures, V11, storageBuffer16BitAccess),
        Q(VkPhysicalDevice16BitStorageFeatures, V11, uniformAndStorageBuffer16BitAccess),
        Q(VkPhysicalDevice16BitStorageFeatures, V11, storagePushConstant16),
        Q(VkPhysicalDevice16BitStorageFeatures, V11, storageInputOutput16) } },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES, F11, 3,
      { P(VkPhysicalDeviceMultiviewFeatures, V11, multiview), P(VkPhysicalDeviceMultiviewFeatures, V11, multiviewGeometryShader),
        P(VkPhysicalDeviceMultiviewFeatures, V11, multiviewTessellationShader) },
      { Q(VkPhysicalDeviceMultiviewFeatures, V11, multiview), Q(VkPhysicalDeviceMultiviewFeatures, V11, multiviewGeometryShader),
        Q(VkPhysicalDeviceMultiviewFeatures, V11, multiviewTessellationShader) } },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VARIABLE_POINTERS_FEATURES, F11, 2,
      { P(VkPhysicalDeviceVariablePointersFeatures, V11, variablePointersStorageBuffer),
        P(VkPhysicalDeviceVariablePointersFeatures, V11, variablePointers) },
      { Q(VkPhysicalDeviceVariablePointersFeatures, V11, variablePointersStorageBuffer),
        Q(VkPhysicalDeviceVariablePointersFeatures, V11, variablePointers) } },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROTECTED_MEMORY_FEATURES, F11, 1,
      { P(VkPhysicalDeviceProtectedMemoryFeatures, V11, protectedMemory) },
      { Q(VkPhysicalDeviceProtectedMemoryFeatures, V11, protectedMemory) } },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES, F11, 1,
      { P(VkPhysicalDeviceSamplerYcbcrConversionFeatures, V11, samplerYcbcrConversion) },
      { Q(VkPhysicalDeviceSamplerYcbcrConversionFeatures, V11, samplerYcbcrConversion) } },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_DRAW_PARAMETERS_FEATURES, F11, 1,
      { P(VkPhysicalDeviceShaderDrawParametersFeatures, V11, shaderDrawParameters) },
      { Q(VkPhysicalDeviceShaderDrawParametersFeatures, V11, shaderDrawParameters) } },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_8BIT_STORAGE_FEATURES, F12, 3,
      { P(VkPhysicalDevice8BitStorageFeatures, V12, storageBuffer8BitAccess),
        P(VkPhysicalDevice8BitStorageFeatures, V12, uniformAndStorageBuffer8BitAccess),
        P(VkPhysicalDevice8BitStorageFeatures, V12, storagePushConstant8) },
      { Q(VkPhysicalDevice8BitStorageFeatures, V12, storageBuffer8BitAccess),
        Q(VkPhysicalDevice8BitStorageFeatures, V12, uniformAndStorageBuffer8BitAccess),
        Q(VkPhysicalDevice8BitStorageFeatures, V12, storagePushConstant8) } },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_ATOMIC_INT64_FEATURES, F12, 2,
      { P(VkPhysicalDeviceShaderAtomicInt64Features, V12, shaderBufferInt64Atomics),
        P(VkPhysicalDeviceShaderAtomicInt64Features, V12, shaderSharedInt64Atomics) },
      { Q(VkPhysicalDeviceShaderAtomicInt64Features, V12, shaderBufferInt64Atomics),
        Q(VkPhysicalDeviceShaderAtomicInt64Features, V12, shaderSharedInt64Atomics) } },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES, F12, 20,
      { P(DI, V12, shaderInputAttachmentArrayDynamicIndexing), P(DI, V12, shaderUniformTexelBufferArrayDynamicIndexing),
        P(DI, V12, shaderStorageTexelBufferArrayDynamicIndexing), P(DI, V12, shaderUniformBufferArrayNonUniformIndexing),
        P(DI, V12, shaderSampledImageArrayNonUniformIndexing), P(DI, V12, shaderStorageBufferArrayNonUniformIndexing),
        P(DI, V12, shaderStorageImageArrayNonUniformIndexing), P(DI, V12, shaderInputAttachmentArrayNonUniformIndexing),
        P(DI, V12, shaderUniformTexelBufferArrayNonUniformIndexing),
        P(DI, V12, shaderStorageTexelBufferArrayNonUniformIndexing),
        P(DI, V12, descriptorBindingUniformBufferUpdateAfterBind), P(DI, V12, descriptorBindingSampledImageUpdateAfterBind),
        P(DI, V12, descriptorBindingStorageImageUpdateAfterBind), P(DI, V12, descriptorBindingStorageBufferUpdateAfterBind),
        P(DI, V12, descriptorBindingUniformTexelBufferUpdateAfterBind),
        P(DI, V12, descriptorBindingStorageTexelBufferUpdateAfterBind),
        P(DI, V12, descriptorBindingUpdateUnusedWhilePending), P(DI, V12, descriptorBindingPartiallyBound),
        P(DI, V12, descriptorBindingVariableDescriptorCount), P(DI, V12, runtimeDescriptorArray) },
      { Q(DI, V12, shaderInputAttachmentArrayDynamicIndexing), Q(DI, V12, shaderUniformTexelBufferArrayDynamicIndexing),
        Q(DI, V12, shaderStorageTexelBufferArrayDynamicIndexing), Q(DI, V12, shaderUniformBufferArrayNonUniformIndexing),
        Q(DI, V12, shaderSampledImageArrayNonUniformIndexing), Q(DI, V12, shaderStorageBufferArrayNonUniformIndexing),
        Q(DI, V12, shaderStorageImageArrayNonUniformIndexing), Q(DI, V12, shaderInputAttachmentArrayNonUniformIndexing),
        Q(DI, V12, shaderUniformTexelBufferArrayNonUniformIndexing),
        Q(DI, V12, shaderStorageTexelBufferArrayNonUniformIndexing),
        Q(DI, V12, descriptorBindingUniformBufferUpdateAfterBind), Q(DI, V12, descriptorBindingSampledImageUpdateAfterBind),
        Q(DI, V12, descriptorBindingStorageImageUpdateAfterBind), Q(DI, V12, descriptorBindingStorageBufferUpdateAfterBind),
        Q(DI, V12, descriptorBindingUniformTexelBufferUpdateAfterBind),
        Q(DI, V12, descriptorBindingStorageTexelBufferUpdateAfterBind),
        Q(DI, V12, descriptorBindingUpdateUnusedWhilePending), Q(DI, V12, descriptorBindingPartiallyBound),
        Q(DI, V12, descriptorBindingVariableDescriptorCount), Q(DI, V12, runtimeDescriptorArray) } },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES, F12, 1,
      { P(VkPhysicalDeviceScalarBlockLayoutFeatures, V12, scalarBlockLayout) },
      { Q(VkPhysicalDeviceScalarBlockLayoutFeatures, V12, scalarBlockLayout) } },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES, F12, 3,
      { P(VkPhysicalDeviceVulkanMemoryModelFeatures, V12, vulkanMemoryModel),
        P(VkPhysicalDeviceVulkanMemoryModelFeatures, V12, vulkanMemoryModelDeviceScope),
        P(VkPhysicalDeviceVulkanMemoryModelFeatures, V12, vulkanMemoryModelAvailabilityVisibilityChains) },
      { Q(VkPhysicalDeviceVulkanMemoryModelFeatures, V12, vulkanMemoryModel),
        Q(VkPhysicalDeviceVulkanMemoryModelFeatures, V12, vulkanMemoryModelDeviceScope),
        Q(VkPhysicalDeviceVulkanMemoryModelFeatures, V12, vulkanMemoryModelAvailabilityVisibilityChains) } },
    { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INLINE_UNIFORM_BLOCK_FEATURES, F13, 2,
      { P(VkPhysicalDeviceInlineUniformBlockFeatures, V13, inlineUniformBlock),
        P(VkPhysicalDeviceInlineUniformBlockFeatures, V13, descriptorBindingInlineUniformBlockUpdateAfterBind) },
      { Q(VkPhysicalDeviceInlineUniformBlockFeatures, V13, inlineUniformBlock),
        Q(VkPhysicalDeviceInlineUniformBlockFeatures, V13, descriptorBindingInlineUniformBlockUpdateAfterBind) } },
};

/** @brief The size of a feature struct the core may have in its chain, or 0 when the layer does not know it. */
static size_t known_size(VkStructureType t)
{
    if (t == STYPE_PRESENT_ID_FEATURES || t == STYPE_PRESENT_WAIT_FEATURES)
        return sizeof(present_feature);
    switch (t) {
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES:
        return sizeof(V11);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES:
        return sizeof(V12);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES:
        return sizeof(V13);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceMapMemoryPlacedFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceFaultFeaturesEXT);
    case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_BIAS_CONTROL_FEATURES_EXT:
        return sizeof(VkPhysicalDeviceDepthBiasControlFeaturesEXT);
    default:
        return 0;
    }
}

/** @brief Whether the guest's struct b asks only for what the core device has; why says what does not fit. */
static bool struct_fits(const rlvk_dev *d, const VkBaseInStructure *b, const char **why)
{
    const size_t head = sizeof(VkBaseInStructure);
    if (b->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) {
        const VkPhysicalDeviceFeatures2 *c = (const void *)core_struct(d, b->sType);
        *why = "a VkPhysicalDeviceFeatures member";
        return bools_within((const VkBool32 *)&((const VkPhysicalDeviceFeatures2 *)b)->features,
                            c ? (const VkBool32 *)&c->features : NULL, sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32));
    }
    for (size_t i = 0; i < sizeof g_promoted / sizeof g_promoted[0]; i++) {
        const promoted *p = &g_promoted[i];
        if (p->stype != b->sType)
            continue;
        const uint8_t *c = (const uint8_t *)core_struct(d, p->into);
        *why = "a member of a struct promoted to Vulkan 1.1, 1.2 or 1.3";
        for (uint16_t m = 0; m < p->n; m++) {
            VkBool32 want, have = VK_FALSE;
            memcpy(&want, (const uint8_t *)b + p->from[m], sizeof want);
            if (c)
                memcpy(&have, c + p->to[m], sizeof have);
            if (want && !have)
                return false;
        }
        return true;
    }
    size_t size = known_size(b->sType);
    const VkBaseInStructure *c = core_struct(d, b->sType);
    if (!size) {
        *why = "a pNext struct the layer cannot compare";
        return false;
    }
    *why = "a member of an extension's feature struct";
    return bools_within((const VkBool32 *)((const uint8_t *)b + head),
                        c ? (const VkBool32 *)((const uint8_t *)c + head) : NULL, (size - head) / sizeof(VkBool32));
}

bool vkf_share_device_fits(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci)
{
    const rlvk_dev *d = core_for_sharing();
    if (!d || (VkPhysicalDevice)(uintptr_t)d->phys != pd)
        return false;
    if (d->family != 0) {
        rl_log(RL_LOG_INFO, "vk", "vkCreateDevice: the core's queue family is %u, not the guest's 0; the guest gets its own",
               d->family);
        return false;
    }
    if (g_dev_users) {
        rl_log(RL_LOG_INFO, "vk", "vkCreateDevice: a second device while one shares the core's; it gets its own");
        return false;
    }
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++)
        if (!in_names(d->device_exts, d->ndevice_exts, ci->ppEnabledExtensionNames[i])) {
            rl_log(RL_LOG_INFO, "vk", "vkCreateDevice: %s is not on the core's device; the guest gets its own",
                   ci->ppEnabledExtensionNames[i]);
            return false;
        }
    const VkPhysicalDeviceFeatures2 *core2 = (const void *)core_struct(d, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
    if (ci->pEnabledFeatures &&
        !bools_within((const VkBool32 *)ci->pEnabledFeatures, core2 ? (const VkBool32 *)&core2->features : NULL,
                      sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32))) {
        rl_log(RL_LOG_INFO, "vk", "vkCreateDevice: the guest enables a feature the core's device left off (such as "
                                  "robustBufferAccess); the guest gets its own device");
        return false;
    }
    for (const VkBaseInStructure *b = ci->pNext; b; b = b->pNext) {
        const char *why = "";
        if (!struct_fits(d, b, &why)) {
            rl_log(RL_LOG_INFO, "vk", "vkCreateDevice: %s (sType %d) is off on the core's device; the guest gets its own",
                   why, (int)b->sType);
            return false;
        }
    }
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++)
        if (ci->pQueueCreateInfos[i].flags) {
            rl_log(RL_LOG_INFO, "vk", "vkCreateDevice: queue create flags; the guest gets its own device");
            return false;
        }
    return true;
}

VkDevice vkf_share_device_take(vkf_dev *dev)
{
    const rlvk_dev *d = core_for_sharing();
    g_dev_users++;
    dev->shared = true;
    dev->host_queues = 1;
    rl_log(RL_LOG_INFO, "vk", "the guest's device is the core's (%s), on its guest queue", d->name);
    return (VkDevice)(uintptr_t)d->device;
}

void vkf_share_device_done(void) { g_dev_users--; }

VkQueue vkf_share_queue(void)
{
    const rlvk_dev *d = rl_vkdev();
    return d ? (VkQueue)(uintptr_t)d->guest_queue : NULL;
}

VkResult VKAPI_PTR vkf_share_device_wait_idle(VkDevice dev)
{
    (void)dev;
    const rlvk_dev *d = rl_vkdev();
    PFN_vkQueueWaitIdle wait = (PFN_vkQueueWaitIdle)(uintptr_t)d->gipa(d->instance, "vkQueueWaitIdle");
    return wait((VkQueue)(uintptr_t)d->guest_queue);
}
