/**
 * @file nvn_vk.c
 * @brief relift's Vulkan device for the NVN2 layer: command sets replayed
 * into host command buffers, textures as host images, presents read back to
 * the host window and the capture folder.
 *
 * Every command set runs to completion inside nvnQueueSubmitCommands: the
 * layer records one host command buffer, submits it and waits for it. Every
 * image stays in the general layout and a full barrier separates any two
 * operations, so the order the game recorded is the order the host runs.
 *
 * A texture's host image is made the first time a command uses it, from the
 * guest bytes of its storage (deswizzled from block-linear), and from then on
 * the host image holds its contents: the guest bytes are not written back.
 * A command or format the layer does not handle stops the game with its name.
 */
#include "nvn_astc.h"
#include "nvn_vk.h"
#include "nvn_sched.h"
#include "hostlib.h"
#include "nvn_watch.h"
#include "vkdev.h"
#include "nvn_pipe.h"
#include "nvn_probe_spv.h"

#include <relift/input.h>
#include <relift/window.h>

#include "present.h"

#include <pthread.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

/** @brief A guest thread that keeps its core this long inside the NVN layer is logged. */
#define HOLD_WARN_NS 20000000ll

nvn_vkdev g = { .once = PTHREAD_ONCE_INIT, .lock = PTHREAD_MUTEX_INITIALIZER };

typedef void (*void_fn)(void);

static void_fn load(void *(*get)(void *, const char *), void *h, int i)
{
    void *p = get(h, vkg_cmds[i].name);
    for (const char *const *a = vkg_cmds[i].aliases; !p && a && *a; a++)
        p = get(h, *a);
    void_fn f;
    memcpy(&f, &p, sizeof f);
    return f;
}

static void write_caps(void);

static int64_t now_ns(void);

/** @brief Where the guest thread's time went, summed between two rate lines (nanoseconds and counts). */
static struct {
    uint64_t run_ns, wait_ns, submit_ns, flushes, runs, upload_ns, uploads, gpu_ns;
} tm;

/** @brief Per-present costs for the frame trace's T, X and Y lines (stats.trace): uploads from guest bytes and write-backs to them. */
static struct {
    uint64_t upload_ns, upload_bytes, wb_ns;
    uint32_t uploads, wbs, gpu_alias;
} ppc;

/** @brief Two timestamps around each submit: the device's busy time per frame. */
static struct {
    VkQueryPool pool;
    double period_ns;
    bool failed;
} gpu_ts;

/** @brief Submits si to the guest queue with g.fence, under the core's queue lock when the presenter shares the queue. */
static VkResult guest_submit(const VkSubmitInfo *si)
{
    rl_vkdev_guest_queue_lock();
    VkResult r = VK(vkQueueSubmit)(g.queue, 1, si, g.fence);
    rl_vkdev_guest_queue_unlock();
    return r;
}
static bool rob2_on;

/** @brief Feature structs of extensions the generated headers do not hold. */
typedef struct ext_features {
    VkStructureType sType;
    void *pNext;
    VkBool32 b[16];
} ext_features;

#define STYPE_DEPTH_CLIP_CONTROL ((VkStructureType)1000355000)
#define STYPE_SHADER_CLOCK ((VkStructureType)1000181000)
#define STYPE_ATOMIC_FLOAT ((VkStructureType)1000260000)
#define STYPE_IMAGE_ATOMIC_INT64 ((VkStructureType)1000234000)
#define STYPE_MAXIMAL_RECONVERGENCE ((VkStructureType)1000434000)
#define STYPE_SHADER_FMA ((VkStructureType)1000579000)
#define STYPE_COOPERATIVE_MATRIX ((VkStructureType)1000506000)
#define STYPE_ROBUSTNESS2 ((VkStructureType)1000286000)
#define STYPE_PROVOKING_VERTEX ((VkStructureType)1000254000)
#define STYPE_CONSERVATIVE_PROPS ((VkStructureType)1000101000)
#define STYPE_PIPELINE_LIBRARY ((VkStructureType)1000320000)
#define STYPE_MESH_SHADER ((VkStructureType)1000328000)
#define STYPE_MESH_SHADER_PROPS ((VkStructureType)1000328001)
#define STYPE_COOPERATIVE_MATRIX_PROPS ((VkStructureType)1000506001)

/**
 * @brief VkPhysicalDeviceMeshShaderPropertiesEXT, which the generated headers do not hold: every member after the
 * header is 32 bits, w[n] the nth (maxTaskWorkGroupInvocations w[4], maxTaskPayloadSize w[8], maxMeshWorkGroupInvocations
 * w[15], maxMeshSharedMemorySize w[19], maxMeshOutputVertices w[24], maxMeshOutputPrimitives w[25]).
 */
typedef struct mesh_props {
    VkStructureType sType;
    void *pNext;
    uint32_t w[32];
} mesh_props;

/** @brief VkCooperativeMatrixPropertiesKHR, which the generated headers do not hold. */
typedef struct coop_props {
    VkStructureType sType;
    void *pNext;
    uint32_t m, n, k, a_type, b_type, c_type, result_type;
    VkBool32 saturating;
    uint32_t scope;
} coop_props;

/** @brief vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR, when the instance has it. */
static VkResult (*coop_props_fn)(VkPhysicalDevice, uint32_t *, coop_props *);

/**
 * @brief The device extensions relift enables when the device has them, with
 * all their features: the ones device.json reports (and the translator may
 * use), plus depth clip control for the NVN depth mode.
 */
static const char *const device_exts[] = { "VK_EXT_depth_clip_control",          "VK_KHR_shader_clock",
                                           "VK_EXT_shader_atomic_float",         "VK_EXT_shader_image_atomic_int64",
                                           "VK_KHR_shader_maximal_reconvergence", "VK_KHR_shader_fma",
                                           "VK_KHR_cooperative_matrix",
                                           "VK_EXT_robustness2",                 "VK_EXT_provoking_vertex",
                                           "VK_KHR_pipeline_library",            "VK_EXT_graphics_pipeline_library",
                                           "VK_EXT_mesh_shader",                 "VK_EXT_device_fault" };
#define DEVICE_EXTS (sizeof device_exts / sizeof device_exts[0])

/** @brief The feature chain the device is made with; it lives until the core's vkCreateDevice returns. */
static struct {
    ext_features clip_control, clock, afloat, i64, recon, fma, coop, rob2, provoking, gpl, mesh, fault;
    VkPhysicalDeviceVulkan13Features f13;
    VkPhysicalDeviceVulkan12Features f12;
    VkPhysicalDeviceVulkan11Features f11;
    VkPhysicalDeviceFeatures2 f2;
} feat;

/** @brief The core's device hook: the extensions and features the NVN layer needs, enabled when the device has them. */
static bool device_hook(void *user, rl_vkdev_build *b)
{
    (void)user;
    void *(*get)(void *, const char *);
    memcpy(&get, &b->gipa, sizeof get);
    for (int i = 0; i < VKG_COUNT; i++)
        if (vkg_cmds[i].level == 0)
            g.fns.f[i] = load(get, NULL, i);
    g.instance = (VkInstance)(uintptr_t)b->instance;
    g.phys = (VkPhysicalDevice)(uintptr_t)b->phys;
    for (int i = 0; i < VKG_COUNT; i++)
        if (vkg_cmds[i].level != 0)
            g.fns.f[i] = load(get, g.instance, i);
    void *coop_fn = get(g.instance, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
    memcpy(&coop_props_fn, &coop_fn, sizeof coop_props_fn);
    VkPhysicalDeviceProperties props;
    VK(vkGetPhysicalDeviceProperties)(g.phys, &props);
    memcpy(g.name, props.deviceName, sizeof g.name);
    gpu_ts.period_ns = (double)props.limits.timestampPeriod;
    g.family = b->family;
    feat.clip_control = (ext_features){ .sType = STYPE_DEPTH_CLIP_CONTROL };
    feat.clock = (ext_features){ .sType = STYPE_SHADER_CLOCK };
    feat.afloat = (ext_features){ .sType = STYPE_ATOMIC_FLOAT };
    feat.i64 = (ext_features){ .sType = STYPE_IMAGE_ATOMIC_INT64 };
    feat.recon = (ext_features){ .sType = STYPE_MAXIMAL_RECONVERGENCE };
    feat.fma = (ext_features){ .sType = STYPE_SHADER_FMA };
    feat.coop = (ext_features){ .sType = STYPE_COOPERATIVE_MATRIX };
    feat.rob2 = (ext_features){ .sType = STYPE_ROBUSTNESS2 };
    feat.provoking = (ext_features){ .sType = STYPE_PROVOKING_VERTEX };
    feat.gpl = (ext_features){ .sType = STYPE_PIPELINE_LIBRARY };
    feat.mesh = (ext_features){ .sType = STYPE_MESH_SHADER };
    feat.fault = (ext_features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FAULT_FEATURES_EXT };
    ext_features *const feats[DEVICE_EXTS] = { &feat.clip_control, &feat.clock, &feat.afloat, &feat.i64,
                                               &feat.recon,        &feat.fma,   &feat.coop,   &feat.rob2,
                                               &feat.provoking,    NULL,        &feat.gpl,          &feat.mesh,
                                               &feat.fault };
    uint32_t next = 0;
    VK(vkEnumerateDeviceExtensionProperties)(g.phys, NULL, &next, NULL);
    VkExtensionProperties *exts = calloc(next + 1, sizeof *exts);
    if (exts)
        VK(vkEnumerateDeviceExtensionProperties)(g.phys, NULL, &next, exts);
    feat.f13 = (VkPhysicalDeviceVulkan13Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan13Features *f13 = &feat.f13;
    rob2_on = false;
    bool provoking_on = false, gpl_on = false, mesh_on = false, fault_on = false;
    for (uint32_t k = 0; k < DEVICE_EXTS; k++)
        for (uint32_t i = 0; exts && i < next; i++)
            if (!strcmp(exts[i].extensionName, device_exts[k])) {
                rl_vkdev_add_ext(b, device_exts[k]);
                if (feats[k]) {
                    feats[k]->pNext = f13->pNext;
                    f13->pNext = feats[k];
                }
                g.depth_clip_control |= k == 0;
                rob2_on |= feats[k] == &feat.rob2;
                provoking_on |= feats[k] == &feat.provoking;
                gpl_on |= feats[k] == &feat.gpl;
                mesh_on |= feats[k] == &feat.mesh;
                fault_on |= feats[k] == &feat.fault;
                break;
            }
    for (uint32_t i = 0; exts && i < next; i++)
        if (!strcmp(exts[i].extensionName, "VK_EXT_external_memory_host")) {
            rl_vkdev_add_ext(b, "VK_EXT_external_memory_host");
            g.host_import = true;
        } else if (!strcmp(exts[i].extensionName, "VK_EXT_conservative_rasterization")) {
            rl_vkdev_add_ext(b, "VK_EXT_conservative_rasterization");
            g.conservative_raster = true;
        } else if (!strcmp(exts[i].extensionName, "VK_NV_viewport_swizzle")) {
            rl_vkdev_add_ext(b, "VK_NV_viewport_swizzle");
            g.viewport_swizzle = true;
        }
    free(exts);
    feat.f12 = (VkPhysicalDeviceVulkan12Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                                                   .pNext = f13 };
    feat.f11 = (VkPhysicalDeviceVulkan11Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES,
                                                   .pNext = &feat.f12 };
    feat.f2 = (VkPhysicalDeviceFeatures2){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &feat.f11 };
    VK(vkGetPhysicalDeviceFeatures2)(g.phys, &feat.f2);
    if (!f13->dynamicRendering) {
        rl_log(RL_LOG_ERROR, "nvn", "%s has no dynamic rendering", g.name);
        return false;
    }
    g.depth_clamp = feat.f2.features.depthClamp;
    g.wide_lines = feat.f2.features.wideLines;
    g.query_ok = feat.f2.features.pipelineStatisticsQuery && feat.f2.features.geometryShader &&
                 feat.f2.features.tessellationShader && feat.f12.hostQueryReset;
    g.occlusion_precise = feat.f2.features.occlusionQueryPrecise;
    feat.f2.features.robustBufferAccess = VK_FALSE;
    g.null_descriptor = rob2_on && feat.rob2.b[2];
    g.provoking_last = provoking_on && feat.provoking.b[0];
    g.gpl = gpl_on && feat.gpl.b[0];
    if (g.gpl) {
        VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT
        };
        VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &gp };
        VK(vkGetPhysicalDeviceProperties2)(g.phys, &pp);
        g.gpl = gp.graphicsPipelineLibraryFastLinking && gp.graphicsPipelineLibraryIndependentInterpolationDecoration;
    }
    g.mesh_shader = mesh_on && feat.mesh.b[0] && feat.mesh.b[1];
    feat.mesh.b[2] = feat.mesh.b[3] = feat.mesh.b[4] = VK_FALSE;
    g.device_fault = fault_on && feat.fault.b[0];
    feat.fault.b[1] = VK_FALSE;
    feat.rob2.b[0] = feat.rob2.b[1] = VK_FALSE;
    g.address_replay = feat.f12.bufferDeviceAddress && feat.f12.bufferDeviceAddressCaptureReplay;
    feat.f12.bufferDeviceAddressMultiDevice = VK_FALSE;
    f13->robustImageAccess = VK_FALSE;
    rl_vkdev_chain(b, &feat.f2);
    g.dev_made = true;
    return true;
}

__attribute__((constructor)) static void require_device(void)
{
    rl_vkdev_require("the NVN layer", VKF_API_VERSION(1, 3), VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT, device_hook,
                     NULL);
}

static void bring_up(void)
{
    const rlvk_dev *core = rl_vkdev();
    if (!core) {
        rl_log(RL_LOG_ERROR, "nvn", "no host Vulkan device");
        return;
    }
    if (!g.dev_made) {
        rl_log(RL_LOG_ERROR, "nvn", "the core's Vulkan device was made without the NVN layer's requirements");
        return;
    }
    g.dev = (VkDevice)(uintptr_t)core->device;
    void (*raw)(void) = g.fns.f[VKG_vkGetDeviceProcAddr];
    void *(*gdpa)(void *, const char *);
    memcpy(&gdpa, &raw, sizeof gdpa);
    for (int i = 0; i < VKG_COUNT; i++)
        if (vkg_cmds[i].level == 2) {
            void_fn f = load(gdpa, g.dev, i);
            if (f)
                g.fns.f[i] = f;
        }
    if (g.mesh_shader) {
        void *draw = gdpa(g.dev, "vkCmdDrawMeshTasksEXT");
        memcpy(&g.draw_mesh_tasks, &draw, sizeof draw);
        g.mesh_shader = g.draw_mesh_tasks != NULL;
    }
    g.queue = (VkQueue)(uintptr_t)core->guest_queue;
    VK(vkGetPhysicalDeviceMemoryProperties)(g.phys, &g.memprops);
    {
        VkPhysicalDeviceProperties dp;
        VK(vkGetPhysicalDeviceProperties)(g.phys, &dp);
        g.max_storage_range = dp.limits.maxStorageBufferRange;
        g.max_index_value = dp.limits.maxDrawIndexedIndexValue;
    }
    VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                    .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                    .queueFamilyIndex = g.family };
    VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                                        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                                        .commandBufferCount = 1 };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (VK(vkCreateCommandPool)(g.dev, &pci, NULL, &g.pool) != VK_SUCCESS)
        return;
    cai.commandPool = g.pool;
    if (VK(vkAllocateCommandBuffers)(g.dev, &cai, &g.cb) != VK_SUCCESS ||
        VK(vkCreateFence)(g.dev, &fci, NULL, &g.fence) != VK_SUCCESS)
        return;
    if (g.conservative_raster) {
        struct {
            VkStructureType sType;
            void *pNext;
            float primitive, max_extra, granularity;
        } cp = { STYPE_CONSERVATIVE_PROPS, NULL, 0, 0, 0 };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &cp };
        VK(vkGetPhysicalDeviceProperties2)(g.phys, &p2);
        g.conservative_extra_max = cp.max_extra;
    }
    if (g.host_import) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT
        };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp };
        VK(vkGetPhysicalDeviceProperties2)(g.phys, &p2);
        g.import_align = hp.minImportedHostPointerAlignment ? hp.minImportedHostPointerAlignment : 4096;
    }
    if (!nvn_vk_staging_new(&g.arena, 64ull << 20)) {
        rl_log(RL_LOG_ERROR, "nvn", "cannot allocate the upload arena");
        return;
    }
    nvn_pipe_init();
    rl_log(RL_LOG_INFO, "nvn", "NVN2 device on %s (queue family %u)", g.name, g.family);
    write_caps();
    g.ok = true;
}


static void json_flags(FILE *f, const char *name, uint32_t v, const char *const *names, uint32_t n)
{
    fprintf(f, "\"%s\": [", name);
    bool first = true;
    for (uint32_t i = 0; i < n; i++)
        if (names[i] && v & (1u << i)) {
            fprintf(f, "%s\"%s\"", first ? "" : ", ", names[i]);
            first = false;
        }
    fprintf(f, "]");
}

static const char *const subgroup_ops[] = {
    "VK_SUBGROUP_FEATURE_BASIC_BIT",    "VK_SUBGROUP_FEATURE_VOTE_BIT",          "VK_SUBGROUP_FEATURE_ARITHMETIC_BIT",
    "VK_SUBGROUP_FEATURE_BALLOT_BIT",   "VK_SUBGROUP_FEATURE_SHUFFLE_BIT",       "VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT",
    "VK_SUBGROUP_FEATURE_CLUSTERED_BIT", "VK_SUBGROUP_FEATURE_QUAD_BIT",
};
static const char *const stages[] = {
    "VK_SHADER_STAGE_VERTEX_BIT",   "VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT", "VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT",
    "VK_SHADER_STAGE_GEOMETRY_BIT", "VK_SHADER_STAGE_FRAGMENT_BIT",             "VK_SHADER_STAGE_COMPUTE_BIT",
};

static const char *driver_name(uint32_t id)
{
    static const char *const names[] = {
        [1] = "VK_DRIVER_ID_AMD_PROPRIETARY",     [2] = "VK_DRIVER_ID_AMD_OPEN_SOURCE",
        [3] = "VK_DRIVER_ID_MESA_RADV",           [4] = "VK_DRIVER_ID_NVIDIA_PROPRIETARY",
        [5] = "VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS", [6] = "VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA",
        [7] = "VK_DRIVER_ID_IMAGINATION_PROPRIETARY", [8] = "VK_DRIVER_ID_QUALCOMM_PROPRIETARY",
        [9] = "VK_DRIVER_ID_ARM_PROPRIETARY",     [10] = "VK_DRIVER_ID_GOOGLE_SWIFTSHADER",
        [11] = "VK_DRIVER_ID_GGP_PROPRIETARY",    [12] = "VK_DRIVER_ID_BROADCOM_PROPRIETARY",
        [13] = "VK_DRIVER_ID_MESA_LLVMPIPE",      [14] = "VK_DRIVER_ID_MOLTENVK",
        [15] = "VK_DRIVER_ID_COREAVI_PROPRIETARY", [16] = "VK_DRIVER_ID_JUICE_PROPRIETARY",
        [17] = "VK_DRIVER_ID_VERISILICON_PROPRIETARY", [18] = "VK_DRIVER_ID_MESA_TURNIP",
        [19] = "VK_DRIVER_ID_MESA_V3DV",          [20] = "VK_DRIVER_ID_MESA_PANVK",
        [21] = "VK_DRIVER_ID_SAMSUNG_PROPRIETARY", [22] = "VK_DRIVER_ID_MESA_VENUS",
        [23] = "VK_DRIVER_ID_MESA_DOZEN",         [24] = "VK_DRIVER_ID_MESA_NVK",
        [25] = "VK_DRIVER_ID_IMAGINATION_OPEN_SOURCE_MESA", [26] = "VK_DRIVER_ID_MESA_HONEYKRISP",
    };
    return id < sizeof names / sizeof names[0] && names[id] ? names[id] : "VK_DRIVER_ID_UNKNOWN";
}

#define B(v) ((v) ? "true" : "false")

/**
 * @brief Runs one variant of probe.comp over the n32 words at w (a staging
 * buffer the probe reads and writes) with push constants n and m, as
 * `groups` workgroups, with full subgroups of `subgroup` invocations
 * required when it is not 0. False when the device cannot run it.
 */
static bool run_probe(const uint32_t *code, size_t size, staging *buf, uint32_t n, uint32_t m, uint32_t groups,
                      uint32_t subgroup)
{
    bool ok = false;
    VkShaderModule sm = 0;
    VkDescriptorSetLayout dsl = 0;
    VkPipelineLayout pl = 0;
    VkPipeline pipe = 0;
    VkDescriptorPool dp = 0;
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = size,
                                     .pCode = code };
    VkDescriptorSetLayoutBinding b = { .binding = 0,
                                       .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                       .descriptorCount = 1,
                                       .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo dli = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                            .bindingCount = 1,
                                            .pBindings = &b };
    VkPushConstantRange pcr = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .size = 2 * sizeof(uint32_t) };
    VkDescriptorPoolSize ps = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 1 };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1,
                                       .poolSizeCount = 1,
                                       .pPoolSizes = &ps };
    if (VK(vkCreateShaderModule)(g.dev, &smi, NULL, &sm) != VK_SUCCESS ||
        VK(vkCreateDescriptorSetLayout)(g.dev, &dli, NULL, &dsl) != VK_SUCCESS)
        goto out;
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1,
                                       .pSetLayouts = &dsl,
                                       .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &pcr };
    if (VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &pl) != VK_SUCCESS)
        goto out;
    VkComputePipelineCreateInfo cpi = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                   .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                   .module = sm,
                                                   .pName = "main" },
                                        .layout = pl };
    VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO,
        .requiredSubgroupSize = subgroup
    };
    if (subgroup) {
        cpi.stage.pNext = &rs;
        cpi.stage.flags = VK_PIPELINE_SHADER_STAGE_CREATE_REQUIRE_FULL_SUBGROUPS_BIT;
    }
    if (VK(vkCreateComputePipelines)(g.dev, 0, 1, &cpi, NULL, &pipe) != VK_SUCCESS ||
        VK(vkCreateDescriptorPool)(g.dev, &dpi, NULL, &dp) != VK_SUCCESS)
        goto out;
    VkDescriptorSet set;
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = dp,
                                        .descriptorSetCount = 1,
                                        .pSetLayouts = &dsl };
    if (VK(vkAllocateDescriptorSets)(g.dev, &dai, &set) != VK_SUCCESS)
        goto out;
    VkDescriptorBufferInfo bi = { .buffer = buf->buf, .range = VK_WHOLE_SIZE };
    VkWriteDescriptorSet wd = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = set,
                                .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                .pBufferInfo = &bi };
    VK(vkUpdateDescriptorSets)(g.dev, 1, &wd, 0, NULL);
    VkCommandBufferBeginInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                     .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (VK(vkResetCommandBuffer)(g.cb, 0) != VK_SUCCESS || VK(vkBeginCommandBuffer)(g.cb, &cbi) != VK_SUCCESS)
        goto out;
    uint32_t pc[2] = { n, m };
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &set, 0, NULL);
    VK(vkCmdPushConstants)(g.cb, pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, pc);
    VK(vkCmdDispatch)(g.cb, groups, 1, 1);
    nvn_vk_barrier(g.cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g.cb };
    ok = VK(vkEndCommandBuffer)(g.cb) == VK_SUCCESS && VK(vkResetFences)(g.dev, 1, &g.fence) == VK_SUCCESS &&
         guest_submit(&si) == VK_SUCCESS &&
         VK(vkWaitForFences)(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX) == VK_SUCCESS;
out:
    if (dp)
        VK(vkDestroyDescriptorPool)(g.dev, dp, NULL);
    if (pipe)
        VK(vkDestroyPipeline)(g.dev, pipe, NULL);
    if (pl)
        VK(vkDestroyPipelineLayout)(g.dev, pl, NULL);
    if (dsl)
        VK(vkDestroyDescriptorSetLayout)(g.dev, dsl, NULL);
    if (sm)
        VK(vkDestroyShaderModule)(g.dev, sm, NULL);
    return ok;
}

/**
 * @brief Whether every NaN the device's single-precision add, multiply,
 * fused multiply-add, NMin and NMax produce is 0x7fffffff, the SM86 result,
 * under each set of execution modes the translator may emit on this device
 * (SignedZeroInfNanPreserve, with and without DenormPreserve, OpFmaKHR when
 * it has it): runs probe.comp over NaN payloads, signaling NaNs, infinity
 * times zero and opposite infinities (nvn_probe_nan_check). False when the
 * device lacks SignedZeroInfNanPreserve or a probe cannot run.
 */
static bool probe_canonical_nan(bool szinp, bool dp32, bool fma32)
{
    if (!szinp)
        return false;
    uint32_t rows = nvn_probe_nan_rows();
    staging buf;
    if (!nvn_vk_staging_new(&buf, 8ull * rows * sizeof(uint32_t)))
        return false;
    uint32_t *w = (uint32_t *)(void *)buf.map;
    struct { const uint32_t *code; size_t size; bool on; } v[] = {
        { nvn_probe_nan, sizeof nvn_probe_nan, true },
        { nvn_probe_nan_dp, sizeof nvn_probe_nan_dp, dp32 },
        { nvn_probe_nan_fma, sizeof nvn_probe_nan_fma, fma32 },
        { nvn_probe_nan_dp_fma, sizeof nvn_probe_nan_dp_fma, dp32 && fma32 },
    };
    bool ok = true;
    for (size_t k = 0; ok && k < sizeof v / sizeof v[0]; k++) {
        if (!v[k].on)
            continue;
        memset(w, 0, 8ull * rows * sizeof(uint32_t));
        nvn_probe_nan_input(w);
        ok = run_probe(v[k].code, v[k].size, &buf, rows, 0, (rows + 63) / 64, 0) && nvn_probe_nan_check(w);
    }
    nvn_vk_staging_free(&buf);
    return ok;
}

/**
 * @brief What the device's single-precision add and multiply do with denormals: runs the DEN32 variant of probe.comp
 * (SignedZeroInfNanPreserve and no denormal mode, the modes the translator emits for a device with no denormal
 * promise) over denormal inputs and results and compares every result with the SM86 flushing model and with IEEE 754
 * (nvn_probe_denorm_check): 1 flush, 2 keep, 0 neither. With dp32 (the device lists DenormPreserve) it also runs the
 * DEN32 DP32 variant and sets *preserve_mode when that keeps denormals.
 */
static uint32_t probe_denorm32(bool szinp, bool dp32, bool *preserve_mode, nvn_denorm_class *cls)
{
    *preserve_mode = false;
    *cls = (nvn_denorm_class){ 0, 0, 255, 0 };
    if (!szinp)
        return 0;
    uint32_t rows = nvn_probe_denorm_rows(), kind = 0;
    staging buf;
    if (!nvn_vk_staging_new(&buf, 4ull * rows * sizeof(uint32_t)))
        return 0;
    uint32_t *w = (uint32_t *)(void *)buf.map;
    for (int mode = 0; mode < 2; mode++) {
        if (mode && !dp32)
            break;
        memset(w, 0, 4ull * rows * sizeof(uint32_t));
        nvn_probe_denorm_input(w);
        const uint32_t *code = mode ? nvn_probe_den_dp : nvn_probe_den;
        size_t size = mode ? sizeof nvn_probe_den_dp : sizeof nvn_probe_den;
        if (!run_probe(code, size, &buf, rows, 0, (rows + 63) / 64, 0))
            continue;
        nvn_denorm_class c;
        uint32_t got = nvn_probe_denorm_check(w, rows, &c);
        if (mode) {
            *preserve_mode = got == 2;
        } else {
            kind = got;
            *cls = c;
        }
    }
    nvn_vk_staging_free(&buf);
    return kind;
}

/**
 * @brief Whether the device's half-precision add, multiply, OpFmaKHR and
 * conversions, under the execution modes of the translator's native halves,
 * give exact binary16 results with denormals and round to nearest even
 * (NaN payloads aside): runs the F16 variant of probe.comp and checks every
 * result (nvn_probe_f16_check). Only asked when the device reports every
 * feature and property those halves need, since a driver may report them
 * and still flush or round otherwise.
 */
static bool probe_exact_f16(void)
{
    uint32_t words = nvn_probe_f16_words();
    staging buf;
    if (!nvn_vk_staging_new(&buf, (uint64_t)words * sizeof(uint32_t)))
        return false;
    uint32_t *w = (uint32_t *)(void *)buf.map;
    memset(w, 0, (uint64_t)words * sizeof(uint32_t));
    nvn_probe_f16_input(w);
    uint32_t bad = UINT32_MAX;
    if (run_probe(nvn_probe_f16, sizeof nvn_probe_f16, &buf, nvn_probe_f16_rows(), nvn_probe_f16_convs(), 32768 / 64, 0))
        bad = nvn_probe_f16_check(w);
    if (bad)
        rl_log(RL_LOG_INFO, "nvn", "%s: half-precision probe off in %u results; halves stay integer code", g.name, bad);
    nvn_vk_staging_free(&buf);
    return bad == 0;
}

/** @brief Whether the device lists a half-precision subgroup cooperative matrix multiply-add of shape m x n x k. */
static bool has_coop_shape(const coop_props *p, uint32_t count, uint32_t m, uint32_t n, uint32_t k)
{
    for (uint32_t i = 0; i < count; i++)
        if (p[i].m == m && p[i].n == n && p[i].k == k && !p[i].a_type && !p[i].b_type && !p[i].c_type &&
            !p[i].result_type && !p[i].saturating && p[i].scope == 3)
            return true;
    return false;
}

/**
 * @brief Whether VK_KHR_cooperative_matrix gives the SM86 HMMA.F16 results
 * when the translator builds its matrices from the guest's registers: runs
 * the HMMA variant of probe.comp on full 32-wide subgroups and checks every
 * output word against the HMMA model (nvn_probe_hmma_check), so that both
 * the arithmetic and the lane layout are measured. Only asked when the
 * device has the extension, the Vulkan memory model at device scope, native
 * halves and 32-wide compute subgroups, and lists both shapes.
 */
static bool probe_hmma_coop(bool usable)
{
    uint32_t count = 0;
    if (!usable || !coop_props_fn || coop_props_fn(g.phys, &count, NULL) != VK_SUCCESS || !count)
        return false;
    coop_props *p = calloc(count, sizeof *p);
    if (!p)
        return false;
    for (uint32_t i = 0; i < count; i++)
        p[i].sType = STYPE_COOPERATIVE_MATRIX_PROPS;
    bool shapes = coop_props_fn(g.phys, &count, p) == VK_SUCCESS && has_coop_shape(p, count, 16, 8, 16) &&
                  has_coop_shape(p, count, 16, 8, 8);
    free(p);
    if (!shapes)
        return false;
    uint32_t words = nvn_probe_hmma_words(), tiles = nvn_probe_hmma_tiles();
    staging buf;
    if (!nvn_vk_staging_new(&buf, (uint64_t)words * sizeof(uint32_t)))
        return false;
    uint32_t *w = (uint32_t *)(void *)buf.map;
    memset(w, 0, (uint64_t)words * sizeof(uint32_t));
    nvn_probe_hmma_input(w);
    uint32_t bad = UINT32_MAX;
    if (run_probe(nvn_probe_hmma, sizeof nvn_probe_hmma, &buf, tiles, tiles, 2 * tiles, 32))
        bad = nvn_probe_hmma_check(w);
    if (bad)
        rl_log(RL_LOG_INFO, "nvn", "%s: cooperative matrix HMMA probe off in %u words; HMMA stays exact code", g.name,
               bad);
    nvn_vk_staging_free(&buf);
    return bad == 0;
}

/**
 * @brief Writes what the host device reports that the SM86 translator reads, as the trimmed Vulkan profiles JSON
 * its host capabilities use, to <shader folder>/device.json.
 */
static void write_caps(void)
{
    ext_features clock = { .sType = STYPE_SHADER_CLOCK }, afloat = { .sType = STYPE_ATOMIC_FLOAT };
    ext_features i64 = { .sType = STYPE_IMAGE_ATOMIC_INT64 }, recon = { .sType = STYPE_MAXIMAL_RECONVERGENCE };
    ext_features fma = { .sType = STYPE_SHADER_FMA }, coop = { .sType = STYPE_COOPERATIVE_MATRIX };
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceVulkan12Features f12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13 };
    VkPhysicalDeviceVulkan11Features f11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12 };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11 };
    ext_features mesh = { .sType = STYPE_MESH_SHADER };
    mesh_props mp = { .sType = STYPE_MESH_SHADER_PROPS };
    VkPhysicalDeviceVulkan13Properties p13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_PROPERTIES };
    VkPhysicalDeviceVulkan12Properties p12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES, .pNext = &p13 };
    VkPhysicalDeviceVulkan11Properties p11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES, .pNext = &p12 };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p11 };
    uint32_t n = 0;
    VK(vkEnumerateDeviceExtensionProperties)(g.phys, NULL, &n, NULL);
    VkExtensionProperties *ext = calloc(n + 1, sizeof *ext);
    if (!ext)
        return;
    VK(vkEnumerateDeviceExtensionProperties)(g.phys, NULL, &n, ext);
    static const char *const wanted[] = { "VK_EXT_shader_atomic_float", "VK_EXT_shader_image_atomic_int64",
                                          "VK_KHR_shader_clock", "VK_KHR_shader_fma",
                                          "VK_KHR_shader_maximal_reconvergence", "VK_KHR_cooperative_matrix",
                                          "VK_EXT_mesh_shader" };
    bool has[7] = { false };
    for (uint32_t i = 0; i < n; i++)
        for (uint32_t k = 0; k < 7; k++)
            has[k] |= !strcmp(ext[i].extensionName, wanted[k]);
    ext_features *chain[] = { has[2] ? &clock : NULL, has[0] ? &afloat : NULL, has[1] ? &i64 : NULL,
                              has[4] ? &recon : NULL, has[3] ? &fma : NULL, has[5] ? &coop : NULL,
                              has[6] ? &mesh : NULL };
    for (uint32_t k = 0; k < 7; k++)
        if (chain[k]) {
            chain[k]->pNext = f13.pNext;
            f13.pNext = chain[k];
        }
    if (has[6])
        p13.pNext = &mp;
    VK(vkGetPhysicalDeviceFeatures2)(g.phys, &f2);
    VK(vkGetPhysicalDeviceProperties2)(g.phys, &p2);
    char path[1024];
    snprintf(path, sizeof path, "%s/device.json", nvn_shader_dir());
    nvn_mkdirs(nvn_shader_dir());
    FILE *f = fopen(path, "w");
    if (!f) {
        free(ext);
        rl_log(RL_LOG_ERROR, "nvn", "cannot write %s", path);
        return;
    }
    const VkPhysicalDeviceLimits *l = &p2.properties.limits;
    fprintf(f, "{\n \"capabilities\": {\n  \"device\": {\n   \"extensions\": {");
    bool first = true;
    for (uint32_t k = 0; k < 7; k++)
        for (uint32_t i = 0; i < n; i++)
            if (has[k] && !strcmp(ext[i].extensionName, wanted[k])) {
                fprintf(f, "%s\n    \"%s\": %u", first ? "" : ",", wanted[k], ext[i].specVersion);
                first = false;
                break;
            }
    fprintf(f, "\n   },\n   \"features\": {\n");
    fprintf(f, "    \"VkPhysicalDeviceFeatures\": { \"shaderInt64\": %s },\n", B(f2.features.shaderInt64));
    fprintf(f, "    \"VkPhysicalDeviceShaderAtomicFloatFeaturesEXT\": { \"shaderBufferFloat32AtomicAdd\": %s },\n",
            B(afloat.b[1]));
    fprintf(f, "    \"VkPhysicalDeviceShaderClockFeaturesKHR\": { \"shaderDeviceClock\": %s, \"shaderSubgroupClock\": %s },\n",
            B(clock.b[1]), B(clock.b[0]));
    fprintf(f, "    \"VkPhysicalDeviceShaderImageAtomicInt64FeaturesEXT\": { \"shaderImageInt64Atomics\": %s },\n",
            B(i64.b[0]));
    fprintf(f,
            "    \"VkPhysicalDeviceShaderFmaFeaturesKHR\": { \"shaderFmaFloat16\": %s, \"shaderFmaFloat32\": %s, "
            "\"shaderFmaFloat64\": %s },\n",
            B(fma.b[0]), B(fma.b[1]), B(fma.b[2]));
    fprintf(f,
            "    \"VkPhysicalDeviceShaderMaximalReconvergenceFeaturesKHR\": { \"shaderMaximalReconvergence\": %s },\n",
            B(recon.b[0]));
    fprintf(f,
            "    \"VkPhysicalDeviceVulkan12Features\": { \"bufferDeviceAddress\": %s, \"shaderBufferInt64Atomics\": %s, "
            "\"shaderSharedInt64Atomics\": %s, \"shaderFloat16\": %s, \"storageBuffer8BitAccess\": %s },\n",
            B(f12.bufferDeviceAddress), B(f12.shaderBufferInt64Atomics), B(f12.shaderSharedInt64Atomics),
            B(f12.shaderFloat16), B(f12.storageBuffer8BitAccess));
    fprintf(f, "    \"VkPhysicalDeviceVulkan11Features\": { \"storageBuffer16BitAccess\": %s },\n",
            B(f11.storageBuffer16BitAccess));
    fprintf(f,
            "    \"VkPhysicalDeviceVulkan13Features\": { \"computeFullSubgroups\": %s, \"shaderDemoteToHelperInvocation\": "
            "%s, \"subgroupSizeControl\": %s }",
            B(f13.computeFullSubgroups), B(f13.shaderDemoteToHelperInvocation), B(f13.subgroupSizeControl));
    if (has[6])
        fprintf(f, ",\n    \"VkPhysicalDeviceMeshShaderFeaturesEXT\": { \"taskShader\": %s, \"meshShader\": %s }",
                B(mesh.b[0]), B(mesh.b[1]));
    fprintf(f, "\n   },\n   \"properties\": {\n");
    fprintf(f,
            "    \"VkPhysicalDeviceProperties\": { \"deviceID\": %u, \"deviceName\": \"%s\", \"vendorID\": %u, \"limits\": "
            "{ \"maxComputeSharedMemorySize\": %u, \"maxComputeWorkGroupInvocations\": %u, \"maxComputeWorkGroupSize\": "
            "[%u, %u, %u], \"maxStorageBufferRange\": %u } },\n",
            p2.properties.deviceID, p2.properties.deviceName, p2.properties.vendorID, l->maxComputeSharedMemorySize,
            l->maxComputeWorkGroupInvocations, l->maxComputeWorkGroupSize[0], l->maxComputeWorkGroupSize[1],
            l->maxComputeWorkGroupSize[2], l->maxStorageBufferRange);
    fprintf(f, "    \"VkPhysicalDeviceVulkan11Properties\": { \"subgroupSize\": %u, ", p11.subgroupSize);
    json_flags(f, "subgroupSupportedOperations", p11.subgroupSupportedOperations, subgroup_ops, 8);
    fprintf(f, ", ");
    json_flags(f, "subgroupSupportedStages", p11.subgroupSupportedStages, stages, 6);
    fprintf(f,
            " },\n    \"VkPhysicalDeviceVulkan12Properties\": { \"driverID\": \"%s\", "
            "\"maxDescriptorSetUpdateAfterBindSampledImages\": %u, \"maxDescriptorSetUpdateAfterBindSamplers\": %u, "
            "\"maxPerStageDescriptorUpdateAfterBindStorageBuffers\": %u, \"shaderDenormFlushToZeroFloat32\": %s, "
            "\"shaderDenormPreserveFloat32\": %s, \"shaderSignedZeroInfNanPreserveFloat32\": %s, "
            "\"shaderDenormPreserveFloat16\": %s, \"shaderRoundingModeRTEFloat16\": %s, "
            "\"shaderSignedZeroInfNanPreserveFloat16\": %s },\n",
            driver_name(p12.driverID), p12.maxDescriptorSetUpdateAfterBindSampledImages,
            p12.maxDescriptorSetUpdateAfterBindSamplers, p12.maxPerStageDescriptorUpdateAfterBindStorageBuffers,
            B(p12.shaderDenormFlushToZeroFloat32), B(p12.shaderDenormPreserveFloat32),
            B(p12.shaderSignedZeroInfNanPreserveFloat32), B(p12.shaderDenormPreserveFloat16),
            B(p12.shaderRoundingModeRTEFloat16), B(p12.shaderSignedZeroInfNanPreserveFloat16));
    bool half = f12.shaderFloat16 && p12.shaderDenormPreserveFloat16 && p12.shaderRoundingModeRTEFloat16 &&
                p12.shaderSignedZeroInfNanPreserveFloat16 && p12.shaderSignedZeroInfNanPreserveFloat32 && has[3] &&
                fma.b[0];
    g.probes.canonical_nan32 = probe_canonical_nan(p12.shaderSignedZeroInfNanPreserveFloat32,
                                                   p12.shaderDenormPreserveFloat32, has[3] && fma.b[1]);
    nvn_denorm_class dc;
    g.probes.denorm32 = probe_denorm32(p12.shaderSignedZeroInfNanPreserveFloat32, p12.shaderDenormPreserveFloat32,
                                       &g.probes.denorm_preserve_mode32, &dc);
    g.probes.denorm_add_flush = dc.add_models & 1;
    g.probes.denorm_mul_fix = dc.mul_fix;
    g.probes.exact_f16 = half && probe_exact_f16();
    bool sub32 = f13.subgroupSizeControl && f13.computeFullSubgroups && p13.minSubgroupSize <= 32 &&
                 p13.maxSubgroupSize >= 32 && (p13.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT);
    g.probes.hmma_coop = probe_hmma_coop(has[5] && coop.b[0] && f12.vulkanMemoryModel &&
                                         f12.vulkanMemoryModelDeviceScope && f12.shaderFloat16 && sub32);
    fprintf(f,
            "    \"ReliftDeviceProbes\": { \"canonicalNan32\": %s, \"exactFloat16\": %s, \"hmmaCooperativeMatrix\": "
            "%s, \"denormDefault32\": %u, \"denormPreserveMode32\": %s, \"denormAddFlush32\": %s, "
            "\"denormMulFix32\": %u },\n",
            B(g.probes.canonical_nan32), B(g.probes.exact_f16), B(g.probes.hmma_coop), g.probes.denorm32,
            B(g.probes.denorm_preserve_mode32), B(g.probes.denorm_add_flush), g.probes.denorm_mul_fix);
    fprintf(f, "    \"VkPhysicalDeviceVulkan13Properties\": { \"maxSubgroupSize\": %u, \"minSubgroupSize\": %u, ",
            p13.maxSubgroupSize, p13.minSubgroupSize);
    json_flags(f, "requiredSubgroupSizeStages", p13.requiredSubgroupSizeStages, stages, 6);
    fprintf(f, " }");
    if (has[6])
        fprintf(f,
                ",\n    \"VkPhysicalDeviceMeshShaderPropertiesEXT\": { \"maxTaskWorkGroupInvocations\": %u, "
                "\"maxTaskPayloadSize\": %u, \"maxMeshWorkGroupInvocations\": %u, \"maxMeshSharedMemorySize\": %u, "
                "\"maxMeshOutputVertices\": %u, \"maxMeshOutputPrimitives\": %u }",
                mp.w[4], mp.w[8], mp.w[15], mp.w[19], mp.w[24], mp.w[25]);
    fprintf(f, "\n   }\n  }\n },\n \"source\": \"relift runtime, %s\"\n}\n", g.name);
    fclose(f);
    free(ext);
}

bool nvn_vk_denorm_stress(uint32_t rows, uint32_t seed, nvn_denorm_class *out)
{
    pthread_once(&g.once, bring_up);
    if (!g.ok || rows > 65535u * 64u)
        return false;
    staging buf;
    if (!nvn_vk_staging_new(&buf, 16ull * rows))
        return false;
    uint32_t *w = (uint32_t *)(void *)buf.map;
    memset(w, 0, 16ull * rows);
    nvn_probe_denorm_random(w, rows, seed);
    bool ok = run_probe(nvn_probe_den, sizeof nvn_probe_den, &buf, rows, 0, (rows + 63) / 64, 0);
    if (ok)
        nvn_probe_denorm_check(w, rows, out);
    nvn_vk_staging_free(&buf);
    return ok;
}

bool nvn_vk_probes(nvn_probes *out)
{
    pthread_once(&g.once, bring_up);
    *out = g.probes;
    return g.ok;
}

bool nvn_vk_ready(void)
{
    pthread_once(&g.once, bring_up);
    if (!g.ok)
        return false;
    pthread_mutex_lock(&g.lock);
    bool ok = nvn_draw_layouts();
    pthread_mutex_unlock(&g.lock);
    return ok;
}

static bool up(rl_ctx *c)
{
    pthread_once(&g.once, bring_up);
    if (!g.ok)
        rl_guest_fatal(c, "nvn", "the NVN2 layer needs a host Vulkan 1.3 device with dynamic rendering");
    else
        nvn_warm_launch();
    return g.ok;
}

/** @brief The host format of an NVNformat, VK_FORMAT_UNDEFINED for one the layer does not map. */
VkFormat nvn_vk_format(uint32_t f, VkImageAspectFlags *aspect)
{
    *aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    switch (f) {
    case 1: return VK_FORMAT_R8_UNORM;
    case 2: return VK_FORMAT_R8_SNORM;
    case 3: return VK_FORMAT_R8_UINT;
    case 4: return VK_FORMAT_R8_SINT;
    case 5: return VK_FORMAT_R16_SFLOAT;
    case 6: return VK_FORMAT_R16_UNORM;
    case 7: return VK_FORMAT_R16_SNORM;
    case 8: return VK_FORMAT_R16_UINT;
    case 9: return VK_FORMAT_R16_SINT;
    case 10: return VK_FORMAT_R32_SFLOAT;
    case 11: return VK_FORMAT_R32_UINT;
    case 12: return VK_FORMAT_R32_SINT;
    case 13: return VK_FORMAT_R8G8_UNORM;
    case 14: return VK_FORMAT_R8G8_SNORM;
    case 15: return VK_FORMAT_R8G8_UINT;
    case 16: return VK_FORMAT_R8G8_SINT;
    case 17: return VK_FORMAT_R16G16_SFLOAT;
    case 18: return VK_FORMAT_R16G16_UNORM;
    case 19: return VK_FORMAT_R16G16_SNORM;
    case 20: return VK_FORMAT_R16G16_UINT;
    case 21: return VK_FORMAT_R16G16_SINT;
    case 22: return VK_FORMAT_R32G32_SFLOAT;
    case 23: return VK_FORMAT_R32G32_UINT;
    case 24: return VK_FORMAT_R32G32_SINT;
    case 37: return VK_FORMAT_R8G8B8A8_UNORM;
    case 38: return VK_FORMAT_R8G8B8A8_SNORM;
    case 39: return VK_FORMAT_R8G8B8A8_UINT;
    case 40: return VK_FORMAT_R8G8B8A8_SINT;
    case 41: return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 42: return VK_FORMAT_R16G16B16A16_UNORM;
    case 43: return VK_FORMAT_R16G16B16A16_SNORM;
    case 44: return VK_FORMAT_R16G16B16A16_UINT;
    case 45: return VK_FORMAT_R16G16B16A16_SINT;
    case 46: return VK_FORMAT_R32G32B32A32_SFLOAT;
    case 47: return VK_FORMAT_R32G32B32A32_UINT;
    case 48: return VK_FORMAT_R32G32B32A32_SINT;
    case 50: *aspect = VK_IMAGE_ASPECT_DEPTH_BIT; return VK_FORMAT_D16_UNORM;
    case 52: *aspect = VK_IMAGE_ASPECT_DEPTH_BIT; return VK_FORMAT_D32_SFLOAT;
    case 53: *aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; return VK_FORMAT_D24_UNORM_S8_UINT;
    case 54: *aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT; return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case 56: return VK_FORMAT_R8G8B8A8_SRGB;
    case 61: return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 62: return VK_FORMAT_A2B10G10R10_UINT_PACK32;
    case 63: return VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    case 64: return VK_FORMAT_E5B9G9R9_UFLOAT_PACK32;
    case 65: return VK_FORMAT_BC1_RGB_UNORM_BLOCK;
    case 66: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    case 67: return VK_FORMAT_BC2_UNORM_BLOCK;
    case 68: return VK_FORMAT_BC3_UNORM_BLOCK;
    case 69: return VK_FORMAT_BC1_RGB_SRGB_BLOCK;
    case 70: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
    case 71: return VK_FORMAT_BC2_SRGB_BLOCK;
    case 72: return VK_FORMAT_BC3_SRGB_BLOCK;
    case 73: return VK_FORMAT_BC4_UNORM_BLOCK;
    case 74: return VK_FORMAT_BC4_SNORM_BLOCK;
    case 75: return VK_FORMAT_BC5_UNORM_BLOCK;
    case 76: return VK_FORMAT_BC5_SNORM_BLOCK;
    case 77: return VK_FORMAT_BC7_UNORM_BLOCK;
    case 78: return VK_FORMAT_BC7_SRGB_BLOCK;
    case 79: return VK_FORMAT_BC6H_SFLOAT_BLOCK;
    case 80: return VK_FORMAT_BC6H_UFLOAT_BLOCK;
    default:
        if (f >= 121 && f < 149) {
            static const VkFormat astc[] = {
                VK_FORMAT_ASTC_4x4_UNORM_BLOCK,   VK_FORMAT_ASTC_5x4_UNORM_BLOCK,   VK_FORMAT_ASTC_5x5_UNORM_BLOCK,
                VK_FORMAT_ASTC_6x5_UNORM_BLOCK,   VK_FORMAT_ASTC_6x6_UNORM_BLOCK,   VK_FORMAT_ASTC_8x5_UNORM_BLOCK,
                VK_FORMAT_ASTC_8x6_UNORM_BLOCK,   VK_FORMAT_ASTC_8x8_UNORM_BLOCK,   VK_FORMAT_ASTC_10x5_UNORM_BLOCK,
                VK_FORMAT_ASTC_10x6_UNORM_BLOCK,  VK_FORMAT_ASTC_10x8_UNORM_BLOCK,  VK_FORMAT_ASTC_10x10_UNORM_BLOCK,
                VK_FORMAT_ASTC_12x10_UNORM_BLOCK, VK_FORMAT_ASTC_12x12_UNORM_BLOCK,
            };
            uint32_t i = f - 121;
            return i < 14 ? astc[i] : (VkFormat)(astc[i - 14] + 1);
        }
        return VK_FORMAT_UNDEFINED;
    }
}

int32_t nvn_vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < g.memprops.memoryTypeCount; i++)
        if (bits & (1u << i) && (g.memprops.memoryTypes[i].propertyFlags & want) == want)
            return (int32_t)i;
    return -1;
}

bool nvn_vk_import(uint64_t guest, uint64_t size, const nvn_import *again, nvn_import *out)
{
    memset(out, 0, sizeof *out);
    pthread_once(&g.once, bring_up);
    if (!g.ok || !g.host_import || !size || guest % g.import_align || size % g.import_align) {
        rl_log(RL_LOG_WARN, "nvn", "import of 0x%llx (0x%llx bytes) refused: ok %d host_import %d align 0x%llx",
               (unsigned long long)guest, (unsigned long long)size, g.ok, g.host_import, (unsigned long long)g.import_align);
        return false;
    }
    nvn_watch_release(guest, size);
    void *host = rl_mem_base() + guest;
    VkMemoryHostPointerPropertiesEXT props = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
    if (VK(vkGetMemoryHostPointerPropertiesEXT)(g.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, host,
                                                &props) != VK_SUCCESS) {
        rl_log(RL_LOG_WARN, "nvn", "import of 0x%llx: vkGetMemoryHostPointerPropertiesEXT failed", (unsigned long long)guest);
        return false;
    }
    bool replay = g.address_replay, take_back = replay && again && again->buf_opaque && again->mem_opaque;
    VkBufferOpaqueCaptureAddressCreateInfo boc = { .sType = VK_STRUCTURE_TYPE_BUFFER_OPAQUE_CAPTURE_ADDRESS_CREATE_INFO,
                                                   .opaqueCaptureAddress = take_back ? again->buf_opaque : 0 };
    VkExternalMemoryBufferCreateInfo eb = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                                            .pNext = take_back ? &boc : NULL,
                                            .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .pNext = &eb,
                               .flags = replay ? VK_BUFFER_CREATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT : 0,
                               .size = size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                                        VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT };
    VkBuffer buf;
    VkDeviceMemory dm = NULL;
    if (VK(vkCreateBuffer)(g.dev, &bci, NULL, &buf) != VK_SUCCESS)
        return false;
    out->buf = buf;
    VkMemoryRequirements req;
    VK(vkGetBufferMemoryRequirements)(g.dev, buf, &req);
    int32_t t = nvn_vk_mem_type(req.memoryTypeBits & props.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VkMemoryOpaqueCaptureAddressAllocateInfo moc = { .sType = VK_STRUCTURE_TYPE_MEMORY_OPAQUE_CAPTURE_ADDRESS_ALLOCATE_INFO,
                                                     .opaqueCaptureAddress = take_back ? again->mem_opaque : 0 };
    VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                                             .pNext = take_back ? &moc : NULL,
                                             .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                                             .pHostPointer = host };
    VkMemoryAllocateFlagsInfo fl = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO,
                                     .pNext = &imp,
                                     .flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT |
                                              (replay ? VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_CAPTURE_REPLAY_BIT : 0) };
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .pNext = &fl,
                                 .allocationSize = size,
                                 .memoryTypeIndex = (uint32_t)t };
    VkResult ar = t >= 0 ? VK(vkAllocateMemory)(g.dev, &mai, NULL, &dm) : VK_ERROR_FEATURE_NOT_PRESENT;
    for (uint32_t alt = 0; ar != VK_SUCCESS && alt < g.memprops.memoryTypeCount; alt++) {
        if (!((req.memoryTypeBits & props.memoryTypeBits) >> alt & 1u) || (int32_t)alt == t)
            continue;
        mai.memoryTypeIndex = alt;
        ar = VK(vkAllocateMemory)(g.dev, &mai, NULL, &dm);
        if (ar == VK_SUCCESS)
            rl_log(RL_LOG_INFO, "nvn", "import of 0x%llx taken by memory type %u after type %d failed",
                   (unsigned long long)guest, alt, t);
    }
    bool ok = ar == VK_SUCCESS;
    out->memory = dm;
    VkResult br = ok ? VK(vkBindBufferMemory)(g.dev, buf, dm, 0) : VK_SUCCESS;
    if (!ok || br != VK_SUCCESS) {
        rl_log(RL_LOG_WARN, "nvn",
               "import of 0x%llx (0x%llx bytes) failed: allocate %d, bind %d, memory type %d (req bits 0x%x, host bits 0x%x)",
               (unsigned long long)guest, (unsigned long long)size, (int)ar, (int)br, t, req.memoryTypeBits,
               props.memoryTypeBits);
        nvn_vk_unimport(out);
        return take_back ? nvn_vk_import(guest, size, NULL, out) : false;
    }
    VkBufferDeviceAddressInfo ai = { .sType = VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, .buffer = buf };
    out->gpu = VK(vkGetBufferDeviceAddress)(g.dev, &ai);
    if (replay) {
        VkDeviceMemoryOpaqueCaptureAddressInfo mi = { .sType = VK_STRUCTURE_TYPE_DEVICE_MEMORY_OPAQUE_CAPTURE_ADDRESS_INFO,
                                                      .memory = dm };
        out->buf_opaque = VK(vkGetBufferOpaqueCaptureAddress)(g.dev, &ai);
        out->mem_opaque = VK(vkGetDeviceMemoryOpaqueCaptureAddress)(g.dev, &mi);
    }
    if (take_back && again->gpu != out->gpu)
        rl_log(RL_LOG_INFO, "nvn", "import of 0x%llx did not get its old address 0x%llx back, got 0x%llx",
               (unsigned long long)guest, (unsigned long long)again->gpu, (unsigned long long)out->gpu);
    return out->gpu != 0;
}

bool nvn_vk_address_replay(void)
{
    pthread_once(&g.once, bring_up);
    return g.ok && g.address_replay;
}

void nvn_vk_unimport(nvn_import *im)
{
    if (im->buf || im->memory)
        VK(vkDeviceWaitIdle)(g.dev);
    if (im->buf)
        VK(vkDestroyBuffer)(g.dev, (VkBuffer)im->buf, NULL);
    if (im->memory)
        VK(vkFreeMemory)(g.dev, (VkDeviceMemory)im->memory, NULL);
    memset(im, 0, sizeof *im);
}

static bool staging_new(staging *s, uint64_t size, bool read);
bool nvn_vk_gpu_buffer(VkBuffer *buf, VkDeviceMemory *memory, uint64_t size)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT };
    if (VK(vkCreateBuffer)(g.dev, &bci, NULL, buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    VK(vkGetBufferMemoryRequirements)(g.dev, *buf, &req);
    int32_t t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (t < 0)
        t = nvn_vk_mem_type(req.memoryTypeBits, 0);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)t };
    if (t < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, memory) != VK_SUCCESS ||
        VK(vkBindBufferMemory)(g.dev, *buf, *memory, 0) != VK_SUCCESS) {
        VK(vkDestroyBuffer)(g.dev, *buf, NULL);
        *buf = NULL;
        return false;
    }
    return true;
}

bool nvn_vk_staging_new(staging *s, uint64_t size)
{
    return staging_new(s, size, false);
}

bool nvn_vk_readback_new(staging *s, uint64_t size)
{
    return staging_new(s, size, true);
}

/**
 * @brief A host-visible, coherent buffer of size bytes, mapped; for a readback (the CPU reads what the device wrote)
 * from host-cached memory where the device has it, since reading uncached memory runs at a small fraction of the
 * speed.
 */
static bool staging_new(staging *s, uint64_t size, bool read)
{
    memset(s, 0, sizeof *s);
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size ? size : 4,
                               .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                                        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT };
    if (VK(vkCreateBuffer)(g.dev, &bci, NULL, &s->buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    VK(vkGetBufferMemoryRequirements)(g.dev, s->buf, &req);
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    int32_t t = read ? nvn_vk_mem_type(req.memoryTypeBits, host | VK_MEMORY_PROPERTY_HOST_CACHED_BIT) : -1;
    if (t < 0)
        t = nvn_vk_mem_type(req.memoryTypeBits, host);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)t };
    void *p = NULL;
    if (t < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, &s->memory) != VK_SUCCESS ||
        VK(vkBindBufferMemory)(g.dev, s->buf, s->memory, 0) != VK_SUCCESS ||
        VK(vkMapMemory)(g.dev, s->memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
        return false;
    s->map = p;
    s->size = size;
    return true;
}

void nvn_vk_staging_free(staging *s)
{
    if (s->memory)
        VK(vkFreeMemory)(g.dev, s->memory, NULL);
    if (s->buf)
        VK(vkDestroyBuffer)(g.dev, s->buf, NULL);
}

void nvn_vk_barrier(VkCommandBuffer cb)
{
    VkMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                          .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                          .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    VK(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &b, 0, NULL,
                             0, NULL);
}

void nvn_vk_dispatch_indirect(VkCommandBuffer cb, VkBuffer ibuf, uint64_t ioff, VkBuffer c0, uint64_t c0_off)
{
    if (c0) {
        VkBufferCopy cp = { .srcOffset = ioff, .dstOffset = c0_off, .size = 12 };
        VK(vkCmdCopyBuffer)(cb, ibuf, c0, 1, &cp);
        nvn_vk_barrier(cb);
    }
    VK(vkCmdDispatchIndirect)(cb, ibuf, ioff);
}

bool nvn_vk_begin(replay *r)
{
    if (r->recording)
        return true;
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    if (VK(vkResetCommandBuffer)(g.cb, 0) != VK_SUCCESS || VK(vkBeginCommandBuffer)(g.cb, &bi) != VK_SUCCESS)
        return false;
    r->recording = true;
    if (!gpu_ts.pool && !gpu_ts.failed && gpu_ts.period_ns > 0) {
        VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = 2 };
        gpu_ts.failed = VK(vkCreateQueryPool)(g.dev, &qi, NULL, &gpu_ts.pool) != VK_SUCCESS;
    }
    if (gpu_ts.pool) {
        VK(vkResetQueryPool)(g.dev, gpu_ts.pool, 0, 2);
        VK(vkCmdWriteTimestamp)(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, gpu_ts.pool, 0);
    }
    return true;
}

/**
 * @brief Logs what VK_EXT_device_fault tells about the lost device, once per process: its description, each faulting
 * address with what of the layer's pools held it (live, freed, or the nearest), and the vendor records.
 */
static void fault_report(void)
{
    static int done;
    if (!g.device_fault || __atomic_exchange_n(&done, 1, __ATOMIC_RELAXED))
        return;
    VkDeviceFaultCountsEXT n = { .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_COUNTS_EXT };
    if (VK(vkGetDeviceFaultInfoEXT)(g.dev, &n, NULL) < 0) {
        rl_log(RL_LOG_ERROR, "nvn", "device fault: the host gives no fault information");
        return;
    }
    VkDeviceFaultAddressInfoEXT *a = calloc(n.addressInfoCount + 1, sizeof *a);
    VkDeviceFaultVendorInfoEXT *v = calloc(n.vendorInfoCount + 1, sizeof *v);
    if (!a || !v) {
        free(a);
        free(v);
        return;
    }
    VkDeviceFaultInfoEXT info = { .sType = VK_STRUCTURE_TYPE_DEVICE_FAULT_INFO_EXT, .pAddressInfos = a, .pVendorInfos = v };
    n.vendorBinarySize = 0;
    if (VK(vkGetDeviceFaultInfoEXT)(g.dev, &n, &info) >= 0) {
        rl_log(RL_LOG_ERROR, "nvn", "device fault: %s (%u addresses, %u vendor records)", info.description,
               n.addressInfoCount, n.vendorInfoCount);
        for (uint32_t i = 0; i < n.addressInfoCount; i++) {
            char where[512];
            nvn_gpu_describe(a[i].reportedAddress, where, sizeof where);
            rl_log(RL_LOG_ERROR, "nvn", "device fault address: type %d, 0x%llx (precision 0x%llx): %s",
                   (int)a[i].addressType, (unsigned long long)a[i].reportedAddress,
                   (unsigned long long)a[i].addressPrecision, where);
        }
        for (uint32_t i = 0; i < n.vendorInfoCount; i++)
            rl_log(RL_LOG_ERROR, "nvn", "device fault vendor record: %s (code 0x%llx, data 0x%llx)", v[i].description,
                   (unsigned long long)v[i].vendorFaultCode, (unsigned long long)v[i].vendorFaultData);
    }
    free(a);
    free(v);
}

/** @brief Submits what r recorded and waits for it. */
bool nvn_vk_flush(replay *r)
{
    if (!r->recording) {
        if (g.arena_used) {
            g.arena_used = 0;
            g.flushes++;
        }
        return true;
    }
    r->recording = false;
    if (gpu_ts.pool)
        VK(vkCmdWriteTimestamp)(g.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gpu_ts.pool, 1);
    VkResult vr = VK(vkEndCommandBuffer)(g.cb);
    if (vr != VK_SUCCESS) {
        rl_log(RL_LOG_ERROR, "nvn", "vkEndCommandBuffer failed (%d)", (int)vr);
        return false;
    }
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &g.cb };
    int64_t t0 = now_ns();
    if ((vr = VK(vkResetFences)(g.dev, 1, &g.fence)) != VK_SUCCESS ||
        (vr = guest_submit(&si)) != VK_SUCCESS) {
        rl_log(RL_LOG_ERROR, "nvn", "the host device refused a submit (%d)", (int)vr);
        if (vr == VK_ERROR_DEVICE_LOST)
            fault_report();
        return false;
    }
    int64_t t1 = now_ns();
    nvn_core_leave();
    vr = VK(vkWaitForFences)(g.dev, 1, &g.fence, VK_TRUE, UINT64_MAX);
    tm.submit_ns += (uint64_t)(t1 - t0);
    tm.wait_ns += (uint64_t)(now_ns() - t1);
    tm.flushes++;
    bool ok = vr == VK_SUCCESS;
    uint64_t ts[2];
    if (ok && gpu_ts.pool &&
        VK(vkGetQueryPoolResults)(g.dev, gpu_ts.pool, 0, 2, sizeof ts, ts, sizeof ts[0], VK_QUERY_RESULT_64_BIT) ==
            VK_SUCCESS)
        tm.gpu_ns += (uint64_t)((double)(ts[1] - ts[0]) * gpu_ts.period_ns);
    if (!ok)
        rl_log(RL_LOG_ERROR, "nvn", "the host device failed while running a submit (%d: -4 is a lost device)", (int)vr);
    if (vr == VK_ERROR_DEVICE_LOST)
        fault_report();
    g.arena_used = 0;
    g.uniform_pending = false;
    g.flushes++;
    nvn_draw_reset();
    nvn_dlss_reset();
    return ok;
}

bool nvn_vk_sync_uniforms(replay *r)
{
    return !g.uniform_pending || nvn_vk_flush(r);
}

int nvn_vk_stage(replay *r, int (*stage)(replay *r, void *arg), void *arg, const char *what)
{
    for (int pass = 0; pass < 2; pass++) {
        uint64_t before = g.flushes;
        int rc = stage(r, arg);
        if (rc != RL_OK || g.flushes == before)
            return rc;
    }
    return nvn_vk_stop(r, "the data of %s does not fit one batch of the upload arena and descriptor pool", what);
}

static void image_free(vimg *v)
{
    for (uint32_t i = 0; i < 7; i++)
        if (v->views[i])
            VK(vkDestroyImageView)(g.dev, v->views[i], NULL);
    for (uint32_t i = 0; i < v->nsub; i++)
        VK(vkDestroyImageView)(g.dev, v->sub[i].view, NULL);
    free(v->sub);
    if (v->view)
        VK(vkDestroyImageView)(g.dev, v->view, NULL);
    if (v->image)
        VK(vkDestroyImage)(g.dev, v->image, NULL);
    if (v->memory)
        VK(vkFreeMemory)(g.dev, v->memory, NULL);
    free(v);
}

static VkImageSubresourceRange range_of(const vimg *v)
{
    bool one = v->d.target == 2 || v->d.target == NVN_TARGET_BUFFER;
    uint32_t layers = one ? 1 : v->d.depth ? v->d.depth : 1;
    if (v->d.target == NVN_TARGET_CUBEMAP)
        layers = 6;
    return (VkImageSubresourceRange){ .aspectMask = v->aspect,
                                      .levelCount = v->d.levels && v->d.target != NVN_TARGET_BUFFER ? v->d.levels : 1,
                                      .layerCount = layers };
}

VkImageSubresourceRange nvn_vk_image_range(const vimg *v) { return range_of(v); }

/** @brief A fast hash of n bytes (whole 8-byte words, then the tail). */
static uint64_t bytes_hash(const uint8_t *p, uint64_t n)
{
    uint64_t h = 0x9e3779b97f4a7c15ull ^ n, w;
    uint64_t i = 0;
    for (; i + 8 <= n; i += 8) {
        memcpy(&w, p + i, 8);
        h = (h ^ w) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    for (; i < n; i++)
        h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

/**
 * @brief A hash of the bytes of v's storage that its write watch does not cover (the partial pages at either end);
 * the whole storage's hash when v is not watched.
 */
static uint64_t edge_hash(replay *r, const vimg *v)
{
    const uint8_t *p = r->mem + v->storage;
    if (!v->watch.on)
        return bytes_hash(p, v->d.size);
    uint64_t head = v->watch.addr - v->storage, tail_at = v->watch.addr + v->watch.size - v->storage;
    return bytes_hash(p, head) * 31 + bytes_hash(p + tail_at, v->d.size - tail_at);
}

/** @brief The depth of level l of a 3D texture, 1 for other targets. */
static uint32_t depth_of(const nvn_tex_desc *d, uint32_t l)
{
    if (d->target != NVN_TARGET_3D)
        return 1;
    return d->depth >> l ? d->depth >> l : 1;
}

/** @brief Fills v from the guest bytes of its storage, every level of every layer. */
/** @brief For an ASTC format: 1 when it is the sRGB variant, 0 when UNORM; -1 for any other format. */
static int astc_srgb(uint32_t format)
{
    if (format < 121 || format >= 149)
        return -1;
    return format >= 135;
}

/**
 * @brief Fills out with the RGBA8 texels of layer layer, level level of the ASTC texture v (w x h x depth of rg
 * texels): the blocks come out of the guest's storage and nvn_astc_image decodes them. This is the one place a
 * different way of serving ASTC (a decode on the GPU, a transcode to a block format) would replace.
 */
static bool upload_decoded(replay *r, const vimg *v, uint32_t level, uint32_t layer, const nvn_region *rg, uint8_t *out)
{
    const nvn_tex_desc *d = &v->d;
    uint32_t bpb, bw, bh;
    if (!nvn_tex_format(d->format, &bpb, &bw, &bh))
        return false;
    uint32_t w = (uint32_t)rg->width, h = (uint32_t)rg->height, depth = (uint32_t)rg->depth;
    uint64_t block_bytes = (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb * depth;
    uint8_t *blocks = malloc(block_bytes ? block_bytes : 1);
    if (!blocks)
        return false;
    bool ok = nvn_tex_copy(d, r->mem + v->storage, level, layer, rg, blocks, false);
    for (uint32_t z = 0; ok && z < depth; z++)
        nvn_astc_image(bw, bh, astc_srgb(d->format) == 1, w, h, blocks + z * (block_bytes / depth), out + (uint64_t)z * w * h * 4);
    free(blocks);
    return ok;
}

static bool upload_body(replay *r, vimg *v);

static bool upload(replay *r, vimg *v)
{
    int64_t t0 = now_ns();
    bool ok = upload_body(r, v);
    ppc.upload_ns += (uint64_t)(now_ns() - t0);
    ppc.uploads++;
    ppc.upload_bytes += v->d.size;
    return ok;
}

static bool upload_body(replay *r, vimg *v)
{
    const nvn_tex_desc *d = &v->d;
    uint32_t bpb, bw, bh;
    if (!nvn_tex_format(d->format, &bpb, &bw, &bh))
        return false;
    if (d->target == NVN_TARGET_BUFFER) {
        uint64_t off, bytes = (uint64_t)d->width * bpb;
        void *pool;
        VkBuffer src = g.arena.buf;
        bool in_pool = nvn_guest_buffer(v->storage, bytes, &pool, &off) && off % bpb == 0;
        if (in_pool)
            src = pool;
        else if (!nvn_vk_upload_bytes(r, r->mem + v->storage, bytes, 0, 16, &off))
            return false;
        nvn_vk_begin(r);
        nvn_vk_barrier(g.cb);
        VkBufferImageCopy bic = { .bufferOffset = off,
                                  .imageSubresource = { .aspectMask = v->aspect, .layerCount = 1 },
                                  .imageExtent = { d->width, 1, 1 } };
        if (in_pool && v->written != v->flushed)
            VK(vkCmdCopyImageToBuffer)(g.cb, v->image, VK_IMAGE_LAYOUT_GENERAL, src, 1, &bic);
        else
            VK(vkCmdCopyBufferToImage)(g.cb, src, v->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
        v->flushed = v->written;
        nvn_vk_barrier(g.cb);
        return true;
    }
    VkImageSubresourceRange rr = range_of(v);
    uint64_t total = 0;
    for (uint32_t l = 0; l < rr.levelCount; l++) {
        uint32_t w = d->width >> l ? d->width >> l : 1, h = d->height >> l ? d->height >> l : 1;
        total += v->decoded ? (uint64_t)w * h * 4 * rr.layerCount * depth_of(d, l)
                            : (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb * rr.layerCount * depth_of(d, l);
    }
    staging s;
    if (!nvn_vk_staging_new(&s, total))
        return false;
    VkBufferImageCopy bic[NVN_TEX_MAX_LEVELS];
    uint64_t at = 0;
    for (uint32_t l = 0; l < rr.levelCount && l < NVN_TEX_MAX_LEVELS; l++) {
        uint32_t w = d->width >> l ? d->width >> l : 1, h = d->height >> l ? d->height >> l : 1;
        uint32_t dl = depth_of(d, l);
        uint64_t layer_bytes = v->decoded ? (uint64_t)w * h * 4 * dl
                                          : (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb * dl;
        nvn_region rg = { 0, 0, 0, (int32_t)w, (int32_t)h, (int32_t)dl };
        for (uint32_t k = 0; k < rr.layerCount; k++) {
            if (v->decoded) {
                if (!upload_decoded(r, v, l, k, &rg, s.map + at + layer_bytes * k)) {
                    nvn_vk_staging_free(&s);
                    return false;
                }
            } else if (!nvn_tex_copy(d, r->mem + v->storage, l, k, &rg, s.map + at + layer_bytes * k, false)) {
                nvn_vk_staging_free(&s);
                return false;
            }
        }
        bic[l] = (VkBufferImageCopy){ .bufferOffset = at,
                                      .imageSubresource = { .aspectMask = v->aspect,
                                                            .mipLevel = l,
                                                            .layerCount = rr.layerCount },
                                      .imageExtent = { w, h, dl } };
        at += layer_bytes * rr.layerCount;
    }
    nvn_vk_begin(r);
    VK(vkCmdCopyBufferToImage)(g.cb, s.buf, v->image, VK_IMAGE_LAYOUT_GENERAL, rr.levelCount, bic);
    nvn_vk_barrier(g.cb);
    bool ok = nvn_vk_flush(r);
    nvn_vk_staging_free(&s);
    return ok;
}

/** @brief For views other than storage views: the image's usage without storage (its format may not allow storage). */
static const void *plain_usage(const vimg *v, VkImageViewUsageCreateInfo *u)
{
    *u = (VkImageViewUsageCreateInfo){ .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                       .usage = v->usage & ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT };
    return v->storage_ok ? u : NULL;
}

/**
 * @brief Whether textures a and b on the same storage lay their common rows
 * out alike: single-level 2D textures of one width, row pitch, block height
 * and sample count, with one texel size and no blocks, differing only in
 * height and format (a render target and a smaller texture over its first
 * rows, as a shadow atlas and one of its pages, or a target read through a
 * format of the same texel size). An image copy of the common rows then
 * moves every byte where the guest's layout has it (sync_alias).
 */
static bool same_rows(const nvn_tex_desc *a, const nvn_tex_desc *b)
{
    uint32_t abpb, abw, abh, bbpb, bbw, bbh;
    if (a->format != b->format &&
        (!nvn_tex_format(a->format, &abpb, &abw, &abh) || !nvn_tex_format(b->format, &bbpb, &bbw, &bbh) ||
         abpb != bbpb || abw != 1 || abh != 1 || bbw != 1 || bbh != 1))
        return false;
    return a->width == b->width && a->target == 1 && b->target == 1 &&
           (a->levels ? a->levels : 1) == 1 && (b->levels ? b->levels : 1) == 1 && a->samples == b->samples &&
           a->block_h == b->block_h && a->stride == b->stride;
}

/** @brief Whether u and v keep any byte of their storage in common. */
static bool overlaps(const vimg *u, const vimg *v)
{
    return u->storage < v->storage + v->d.size && v->storage < u->storage + u->d.size;
}

/**
 * @brief Writes every level and layer of u back to its guest storage, in
 * the guest's layout, so a texture of another layout over the same bytes
 * reads what the GPU last wrote. Color images and depth images of one
 * aspect (whose host texels are the guest's bytes) only.
 */
static bool write_back_body(replay *r, vimg *u);

static bool write_back(replay *r, vimg *u)
{
    int64_t t0 = now_ns();
    bool ok = write_back_body(r, u);
    ppc.wb_ns += (uint64_t)(now_ns() - t0);
    ppc.wbs++;
    return ok;
}

static bool write_back_body(replay *r, vimg *u)
{
    const nvn_tex_desc *d = &u->d;
    uint32_t bpb, bw, bh;
    if (!nvn_tex_format(d->format, &bpb, &bw, &bh) || d->target == NVN_TARGET_BUFFER ||
        (u->aspect != VK_IMAGE_ASPECT_COLOR_BIT && u->aspect != VK_IMAGE_ASPECT_DEPTH_BIT) ||
        u->format == VK_FORMAT_D32_SFLOAT_S8_UINT || u->format == VK_FORMAT_D24_UNORM_S8_UINT)
        return false;
    VkImageSubresourceRange rr = range_of(u);
    uint64_t total = 0;
    for (uint32_t l = 0; l < rr.levelCount; l++) {
        uint32_t w = d->width >> l ? d->width >> l : 1, h = d->height >> l ? d->height >> l : 1;
        total += (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb * rr.layerCount * depth_of(d, l);
    }
    staging s;
    if (!nvn_vk_readback_new(&s, total))
        return false;
    VkBufferImageCopy bic[NVN_TEX_MAX_LEVELS];
    uint64_t at = 0;
    uint32_t n = rr.levelCount < NVN_TEX_MAX_LEVELS ? rr.levelCount : NVN_TEX_MAX_LEVELS;
    for (uint32_t l = 0; l < n; l++) {
        uint32_t w = d->width >> l ? d->width >> l : 1, h = d->height >> l ? d->height >> l : 1, dl = depth_of(d, l);
        bic[l] = (VkBufferImageCopy){ .bufferOffset = at,
                                      .imageSubresource = { .aspectMask = u->aspect,
                                                            .mipLevel = l,
                                                            .layerCount = rr.layerCount },
                                      .imageExtent = { w, h, dl } };
        at += (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb * dl * rr.layerCount;
    }
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    VK(vkCmdCopyImageToBuffer)(g.cb, u->image, VK_IMAGE_LAYOUT_GENERAL, s.buf, n, bic);
    nvn_vk_barrier(g.cb);
    bool ok = nvn_vk_flush(r);
    at = 0;
    for (uint32_t l = 0; ok && l < n; l++) {
        uint32_t w = d->width >> l ? d->width >> l : 1, h = d->height >> l ? d->height >> l : 1, dl = depth_of(d, l);
        uint64_t layer_bytes = (uint64_t)((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpb * dl;
        nvn_region rg = { 0, 0, 0, (int32_t)w, (int32_t)h, (int32_t)dl };
        for (uint32_t k = 0; ok && k < rr.layerCount; k++)
            ok = nvn_tex_copy(d, r->mem + u->storage, l, k, &rg, s.map + at + layer_bytes * k, true);
        at += layer_bytes * rr.layerCount;
    }
    nvn_vk_staging_free(&s);
    return ok;
}

/**
 * @brief Whether an image copy between u and v moves their bytes unchanged: one guest format, or color images both
 * kept in the host format that has their guest texels' bytes (not a substitute), whose sizes then agree.
 */
static bool copy_keeps_bytes(const vimg *u, const vimg *v)
{
    if (u->d.format == v->d.format)
        return true;
    VkImageAspectFlags a;
    return u->aspect == VK_IMAGE_ASPECT_COLOR_BIT && v->aspect == VK_IMAGE_ASPECT_COLOR_BIT &&
           u->format == nvn_vk_format(u->d.format, &a) && v->format == nvn_vk_format(v->d.format, &a);
}

/**
 * @brief Brings v up to date with the images over its storage that the
 * GPU wrote after v last took their bytes, oldest first: one of the same rows (see
 * same_rows) by an image copy, any other through its guest storage (it is
 * written back and v filled again from the bytes). Returns v, or NULL with
 * the game stopped when the newer image cannot be written back.
 */
static vimg *sync_alias(replay *r, vimg *v)
{
    for (;;) {
        vimg *src = NULL;
        for (uint32_t i = 0; i < g.nimages; i++) {
            vimg *u = g.images[i];
            if (u != v && overlaps(u, v) && u->written > v->written && (!src || u->written < src->written))
                src = u;
        }
        if (!src)
            return v;
        if (src->storage == v->storage && same_rows(&src->d, &v->d) && copy_keeps_bytes(src, v)) {
            uint32_t h = src->d.height < v->d.height ? src->d.height : v->d.height;
            nvn_vk_begin(r);
            nvn_vk_barrier(g.cb);
            VkImageCopy ic = { { src->aspect, 0, 0, 1 }, { 0, 0, 0 }, { v->aspect, 0, 0, 1 }, { 0, 0, 0 },
                               { v->d.width, h, 1 } };
            VK(vkCmdCopyImage)(g.cb, src->image, VK_IMAGE_LAYOUT_GENERAL, v->image, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
            nvn_vk_barrier(g.cb);
            ppc.gpu_alias++;
        } else if (!write_back(r, src) || !upload(r, v)) {
            nvn_vk_stop(r,
                        "texture storage 0x%llx (format %u, %ux%u) was written as format %u, %ux%u, which cannot be "
                        "read back into the guest's layout",
                        (unsigned long long)v->storage, v->d.format, v->d.width, v->d.height, src->d.format,
                        src->d.width, src->d.height);
            return NULL;
        }
        v->written = src->written;
    }
}

void nvn_vk_written(vimg *v) { v->written = ++g.write_stamp; }

static vimg *image_of_desc(replay *r, const nvn_tex_desc *d, uint64_t tex);

/** @brief The host image of the texture at guest address tex, made and filled from its storage the first time. */
static vimg *image_of(replay *r, uint64_t tex)
{
    nvn_tex_desc dd;
    const nvn_tex_desc *d = nvn_texture_desc(tex, &dd) ? &dd : NULL;
    if (!d) {
        char gone[160] = "no removal remembered";
        nvn_obj_gone(tex, gone, sizeof gone);
        rl_guest_fatal(r->c, "nvn", "0x%llx is not a texture (%s)", (unsigned long long)tex, gone);
        return NULL;
    }
    return image_of_desc(r, d, tex);
}

/**
 * @brief The host image of the texture d describes over its storage, made and filled from the storage the first
 * time; tex names the texture it came from in reports.
 */
static vimg *image_of_desc(replay *r, const nvn_tex_desc *d, uint64_t tex)
{
    uint64_t storage = nvn_pool_memory(d->pool, NULL) + d->offset;
    for (uint32_t i = 0; i < g.nimages; i++) {
        vimg *v = g.images[i];
        if (v->storage != storage)
            continue;
        if (v->d.format == d->format && v->d.width == d->width && v->d.height == d->height &&
            v->d.depth == d->depth && v->d.levels == d->levels && v->d.target == d->target &&
            v->d.samples == d->samples && v->d.flags == d->flags)
            return sync_alias(r, v);
    }
    VkImageAspectFlags aspect;
    VkFormat f = nvn_vk_format(d->format, &aspect);
    bool buffer = d->target == NVN_TARGET_BUFFER, line = d->target == 0 || d->target == 3;
    if (f == VK_FORMAT_UNDEFINED || d->samples > 1 ||
        (d->target != 0 && d->target != 1 && d->target != 3 && d->target != 4 && d->target != 7 &&
         d->target != NVN_TARGET_3D && d->target != NVN_TARGET_CUBEMAP &&
         d->target != NVN_TARGET_CUBEMAP_ARRAY && !buffer)) {
        rl_guest_fatal(r->c, "nvn", "texture 0x%llx: format %u, target %u, %u samples not handled",
                       (unsigned long long)tex, d->format, d->target, d->samples);
        return NULL;
    }
    if (g.nimages == MAX_IMAGES) {
        rl_guest_fatal(r->c, "nvn", "more than %u textures in use", MAX_IMAGES);
        return NULL;
    }
    vimg *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;
    v->storage = storage;
    v->d = *d;
    v->format = f;
    v->aspect = aspect;
    VkFormatProperties fp;
    VK(vkGetPhysicalDeviceFormatProperties)(g.phys, f, &fp);
    if (astc_srgb(d->format) >= 0 && !(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        v->decoded = true;
        v->format = f = astc_srgb(d->format) ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
        VK(vkGetPhysicalDeviceFormatProperties)(g.phys, f, &fp);
    }
    VkImageSubresourceRange rr = range_of(v);
    bool depth = aspect != VK_IMAGE_ASPECT_COLOR_BIT;
    VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    if (!(ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) {
        free(v);
        rl_guest_fatal(r->c, "nvn", "texture 0x%llx: the host device cannot sample format %u (Vulkan %d)",
                       (unsigned long long)tex, d->format, (int)f);
        return NULL;
    }
    VkImageUsageFlags attach = depth ? (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT
                                            ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                                            : 0)
                                     : (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0);
    uint32_t sbpb, sbw, sbh;
    bool can_store = !depth && !buffer && nvn_tex_format(d->format, &sbpb, &sbw, &sbh) && sbw == 1 && sbh == 1;
    v->storage_ok = can_store;
    VkImageCreateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = (depth ? 0 : VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT) |
                 (can_store ? VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0) |
                 (d->target == NVN_TARGET_CUBEMAP || d->target == NVN_TARGET_CUBEMAP_ARRAY ||
                          (d->target == 4 && d->width == d->height && rr.layerCount >= 6)
                      ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT
                      : 0) |
                 (d->target == NVN_TARGET_3D && !buffer ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0),
        .imageType = buffer || line ? VK_IMAGE_TYPE_1D : d->target == NVN_TARGET_3D ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D,
        .format = f,
        .extent = { d->width, buffer || line ? 1 : d->height, depth_of(d, 0) },
        .mipLevels = rr.levelCount,
        .arrayLayers = rr.layerCount,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | attach |
                 (can_store ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkMemoryRequirements req;
    if (VK(vkCreateImage)(g.dev, &ci, NULL, &v->image) != VK_SUCCESS)
        goto fail;
    VK(vkGetImageMemoryRequirements)(g.dev, v->image, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size };
    int32_t t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    mai.memoryTypeIndex = (uint32_t)t;
    if (t < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, &v->memory) != VK_SUCCESS ||
        VK(vkBindImageMemory)(g.dev, v->image, v->memory, 0) != VK_SUCCESS)
        goto fail;
    v->usage = ci.usage;
    VkImageViewUsageCreateInfo vu;
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .pNext = plain_usage(v, &vu),
                                  .image = v->image,
                                  .viewType = buffer || d->target == 0       ? VK_IMAGE_VIEW_TYPE_1D
                                              : d->target == 3             ? VK_IMAGE_VIEW_TYPE_1D_ARRAY
                                              : d->target == NVN_TARGET_3D ? VK_IMAGE_VIEW_TYPE_3D
                                              : rr.layerCount > 1          ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                                                                           : VK_IMAGE_VIEW_TYPE_2D,
                                  .format = f,
                                  .subresourceRange = rr };
    if (VK(vkCreateImageView)(g.dev, &vci, NULL, &v->view) != VK_SUCCESS)
        goto fail;
    nvn_vk_begin(r);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = v->image,
                                .subresourceRange = rr };
    VK(vkCmdPipelineBarrier)(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, 1, &ib);
    nvn_watch_start(&v->watch, storage, d->size);
    v->hash = bytes_hash(r->mem + storage, d->size);
    v->edge_hash = edge_hash(r, v);
    if (!depth && !upload(r, v))
        goto fail;
    v->hash_frame = g.presents + 1;
    if (depth && !nvn_vk_flush(r))
        goto fail;
    g.images[g.nimages++] = v;
    sync_alias(r, v);
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "host image for texture 0x%llx: storage 0x%llx, format %u (%d), %ux%u",
               (unsigned long long)tex, (unsigned long long)storage, d->format, (int)f, d->width, d->height);
    return v;
fail:
    image_free(v);
    rl_guest_fatal(r->c, "nvn", "cannot make a host image for texture 0x%llx", (unsigned long long)tex);
    return NULL;
}

vimg *nvn_vk_image(replay *r, uint64_t tex) { return image_of(r, tex); }

/**
 * @brief Refills v from its storage when the CPU changed the bytes: a
 * buffer texture at every use, any texture the GPU never wrote (the game
 * fills it through the CPU, as a glyph cache) when the hash of its storage
 * differs, checked at most once per presented frame.
 */
bool nvn_vk_refresh(replay *r, vimg *v)
{
    if (v->d.target == NVN_TARGET_BUFFER)
        return upload(r, v);
    if (v->written || v->hash_frame == g.presents + 1)
        return true;
    v->hash_frame = g.presents + 1;
    if (v->watch.on && !nvn_watch_changed(&v->watch) && edge_hash(r, v) == v->edge_hash)
        return true;
    nvn_watch_start(&v->watch, v->storage, v->d.size);
    uint64_t h = bytes_hash(r->mem + v->storage, v->d.size);
    v->edge_hash = edge_hash(r, v);
    if (h == v->hash)
        return true;
    v->hash = h;
    return v->aspect != VK_IMAGE_ASPECT_COLOR_BIT || upload(r, v);
}

VkImageView nvn_vk_storage_view(const vimg *v, VkImageViewType t, VkFormat f, uint32_t level, uint32_t layer,
                                uint32_t layers)
{
    if (!v->storage_ok)
        return NULL;
    VkImageSubresourceRange rr = range_of(v);
    rr.baseMipLevel = level;
    rr.levelCount = 1;
    if (layer >= rr.layerCount)
        return NULL;
    rr.baseArrayLayer = layer;
    rr.layerCount = layers && layers < rr.layerCount - layer ? layers : rr.layerCount - layer;
    if (t == VK_IMAGE_VIEW_TYPE_1D || t == VK_IMAGE_VIEW_TYPE_2D)
        rr.layerCount = 1;
    VkImageViewUsageCreateInfo u = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
                                     .usage = VK_IMAGE_USAGE_STORAGE_BIT };
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .pNext = &u,
                                  .image = v->image,
                                  .viewType = t,
                                  .format = f,
                                  .subresourceRange = rr };
    VkImageView out;
    return VK(vkCreateImageView)(g.dev, &vci, NULL, &out) == VK_SUCCESS ? out : NULL;
}

VkFormat nvn_vk_view_format(const vimg *v, uint32_t format, VkImageAspectFlags *aspect)
{
    uint32_t b0, w0, h0, b1, w1, h1;
    *aspect = 0;
    VkFormat f = nvn_vk_format(format, aspect);
    if (f == VK_FORMAT_UNDEFINED || v->decoded || *aspect != VK_IMAGE_ASPECT_COLOR_BIT || v->aspect != VK_IMAGE_ASPECT_COLOR_BIT ||
        !nvn_tex_format(format, &b1, &w1, &h1) || !nvn_tex_format(v->d.format, &b0, &w0, &h0) || b0 != b1 ||
        w0 != w1 || h0 != h1)
        return VK_FORMAT_UNDEFINED;
    return f;
}

VkImageView nvn_vk_view(vimg *v, VkImageViewType t, const nvn_view *nv)
{
    if ((uint32_t)t >= 7)
        return NULL;
    if (!nv && v->views[t])
        return v->views[t];
    VkImageSubresourceRange rr = range_of(v);
    bool stencil = nv && nv->has_ds_mode && nv->ds_mode == 1;
    rr.aspectMask = stencil ? rr.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT : rr.aspectMask & ~VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!rr.aspectMask)
        rr.aspectMask = range_of(v).aspectMask;
    if (nv && nv->has_levels) {
        rr.baseMipLevel = (uint32_t)nv->base_level;
        rr.levelCount = nv->num_levels ? (uint32_t)nv->num_levels : VK_REMAINING_MIP_LEVELS;
    }
    if (nv && nv->has_layers) {
        rr.baseArrayLayer = (uint32_t)nv->min_layer;
        rr.layerCount = nv->num_layers ? (uint32_t)nv->num_layers : VK_REMAINING_ARRAY_LAYERS;
    }
    if (t == VK_IMAGE_VIEW_TYPE_2D || t == VK_IMAGE_VIEW_TYPE_1D)
        rr.layerCount = 1;
    else if (t == VK_IMAGE_VIEW_TYPE_CUBE)
        rr.layerCount = 6;
    const uint32_t *swz = nv && nv->has_swizzle ? nv->swizzle : v->d.swizzle;
    VkComponentSwizzle sw[4];
    for (int i = 0; i < 4; i++)
        sw[i] = swz[i] <= 5 ? (VkComponentSwizzle)(swz[i] + 1) : VK_COMPONENT_SWIZZLE_IDENTITY;
    VkFormat f = v->format;
    if (nv && nv->has_format && nv->format != v->d.format) {
        VkImageAspectFlags a;
        f = nvn_vk_view_format(v, nv->format, &a);
        if (f == VK_FORMAT_UNDEFINED)
            return NULL;
    }
    VkImageViewUsageCreateInfo vu;
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .pNext = plain_usage(v, &vu),
                                  .image = v->image,
                                  .viewType = t,
                                  .format = f,
                                  .components = { sw[0], sw[1], sw[2], sw[3] },
                                  .subresourceRange = rr };
    VkImageView out;
    if (VK(vkCreateImageView)(g.dev, &vci, NULL, &out) != VK_SUCCESS)
        return NULL;
    if (!nv)
        v->views[t] = out;
    return out;
}

/**
 * @brief Begins dynamic rendering on r's render targets, limited to their common extent; with none (a draw that
 * only writes storage images) over the first viewport's size.
 */
bool nvn_vk_begin_rendering(replay *r, VkRect2D *area)
{
    VkRenderingAttachmentInfo ca[MAX_RT], da = { 0 };
    uint32_t w = UINT32_MAX, h = UINT32_MAX;
    for (uint32_t i = 0; i < r->st->ncolor; i++) {
        const rt *t = &r->st->color[i];
        ca[i] = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                             .imageView = t->view,
                                             .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                             .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                             .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
        if (t->v) {
            uint32_t lw = t->v->d.width >> t->level, lh = t->v->d.height >> t->level;
            w = (lw ? lw : 1) < w ? (lw ? lw : 1) : w;
            h = (lh ? lh : 1) < h ? (lh ? lh : 1) : h;
        }
    }
    if (r->st->depth.v) {
        da = (VkRenderingAttachmentInfo){ .sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO,
                                          .imageView = r->st->depth.view,
                                          .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                                          .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                                          .storeOp = VK_ATTACHMENT_STORE_OP_STORE };
        uint32_t lw = r->st->depth.v->d.width >> r->st->depth.level, lh = r->st->depth.v->d.height >> r->st->depth.level;
        w = (lw ? lw : 1) < w ? (lw ? lw : 1) : w;
        h = (lh ? lh : 1) < h ? (lh ? lh : 1) : h;
    }
    if (w == UINT32_MAX) {
        const float *v = r->st->viewports[0];
        if (r->st->ncolor || r->st->depth.v || v[2] < 1 || v[3] < 1)
            return false;
        w = (uint32_t)v[2];
        h = (uint32_t)v[3];
    }
    bool stencil = r->st->depth.v && (r->st->depth.v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT);
    *area = (VkRect2D){ { 0, 0 }, { w, h } };
    VkRenderingInfo ri = { .sType = VK_STRUCTURE_TYPE_RENDERING_INFO,
                           .renderArea = *area,
                           .layerCount = 1,
                           .colorAttachmentCount = r->st->ncolor,
                           .pColorAttachments = ca,
                           .pDepthAttachment = r->st->depth.v && (r->st->depth.v->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) ? &da : NULL,
                           .pStencilAttachment = stencil ? &da : NULL };
    nvn_vk_begin(r);
    VK(vkCmdBeginRendering)(g.cb, &ri);
    return true;
}

/** @brief The scissor rectangle clipped to area. */
static VkRect2D clip(const replay *r, VkRect2D area)
{
    if (!r->st->scissor_set)
        return area;
    int64_t x0 = r->st->scissor.offset.x;
    int64_t y0 = nvn_window_top(nvn_device_origin_mode(), area.extent.height, r->st->scissor.offset.y,
                                   r->st->scissor.extent.height);
    int64_t x1 = x0 + r->st->scissor.extent.width, y1 = y0 + r->st->scissor.extent.height;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > area.extent.width ? area.extent.width : x1;
    y1 = y1 > area.extent.height ? area.extent.height : y1;
    if (x1 <= x0 || y1 <= y0)
        return (VkRect2D){ { 0, 0 }, { 0, 0 } };
    return (VkRect2D){ { (int32_t)x0, (int32_t)y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } };
}

static uint8_t *read_back(replay *r, vimg *v);

/** @brief Stops the game with a report; with capture on, the 8-bit RGBA host images go to the capture folder first. */
int nvn_vk_stop(replay *r, const char *fmt, ...)
{
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    nvn_vk_flush(r);
    for (uint32_t i = 0; i < g.nimages && i < 16; i++) {
        uint64_t frame;
        vimg *v = g.images[i];
        uint8_t *px = read_back(r, v);
        if (px && vkf_capture_due(&frame)) {
            vkf_capture_write(frame, 0, v->d.width, v->d.height, px, (size_t)v->d.width * 4, false);
            rl_log(RL_LOG_INFO, "nvn", "texture storage 0x%llx at the stop captured as frame %llu",
                   (unsigned long long)v->storage, (unsigned long long)frame);
        }
        free(px);
    }
    return rl_guest_fatal(r->c, "nvn", "%s", msg);
}

static int clear_color(replay *r, const nvn_cmd *e)
{
    uint32_t idx = (uint32_t)e->x[1], mask = (uint32_t)e->x[3];
    const uint8_t *val = nvn_cmd_bytes(r->s, e->x[2]);
    if (idx >= r->st->ncolor || !r->st->color[idx].v)
        return nvn_vk_stop(r, "%s of render target %u, which is not set", nvn_cmd_name(e->op), idx);
    if (!val)
        return nvn_vk_stop(r, "%s without a color", nvn_cmd_name(e->op));
    if (mask == 0)
        return RL_OK;
    if (mask != 0xf)
        return nvn_vk_stop(r, "%s with channel mask 0x%x", nvn_cmd_name(e->op), mask);
    VkRect2D area;
    if (!nvn_vk_begin_rendering(r, &area))
        return nvn_vk_stop(r, "%s without render targets", nvn_cmd_name(e->op));
    VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = idx };
    memcpy(&ca.clearValue.color, val, 16);
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "clear target %u (storage 0x%llx) to %g %g %g %g", idx,
               (unsigned long long)r->st->color[idx].v->storage, (double)ca.clearValue.color.float32[0],
               (double)ca.clearValue.color.float32[1], (double)ca.clearValue.color.float32[2],
               (double)ca.clearValue.color.float32[3]);
    VkClearRect cr = { .rect = clip(r, area), .layerCount = 1 };
    if (cr.rect.extent.width && cr.rect.extent.height)
        VK(vkCmdClearAttachments)(g.cb, 1, &ca, 1, &cr);
    VK(vkCmdEndRendering)(g.cb);
    nvn_vk_barrier(g.cb);
    return RL_OK;
}

static int clear_depth_stencil(replay *r, const nvn_cmd *e)
{
    float depth;
    memcpy(&depth, &e->d[0], sizeof depth);
    bool depth_mask = (uint32_t)e->x[1] != 0;
    uint32_t stencil = (uint32_t)e->x[2], stencil_mask = (uint32_t)e->x[3] & 0xff;
    if (!r->st->depth.v)
        return nvn_vk_stop(r, "nvnCommandBufferClearDepthStencil without a depth target");
    if (stencil_mask != 0 && stencil_mask != 0xff)
        return nvn_vk_stop(r, "nvnCommandBufferClearDepthStencil with stencil mask 0x%x", stencil_mask);
    VkImageAspectFlags aspect = 0;
    if (depth_mask && (r->st->depth.v->aspect & VK_IMAGE_ASPECT_DEPTH_BIT))
        aspect |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (stencil_mask && (r->st->depth.v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT))
        aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!aspect)
        return RL_OK;
    VkRect2D area;
    nvn_vk_begin_rendering(r, &area);
    VkClearAttachment ca = { .aspectMask = aspect, .clearValue.depthStencil = { depth, stencil & 0xff } };
    VkClearRect cr = { .rect = clip(r, area), .layerCount = 1 };
    if (cr.rect.extent.width && cr.rect.extent.height)
        VK(vkCmdClearAttachments)(g.cb, 1, &ca, 1, &cr);
    VK(vkCmdEndRendering)(g.cb);
    nvn_vk_barrier(g.cb);
    return RL_OK;
}

bool nvn_vk_rt_inside(const nvn_tex_desc *d, uint32_t level, uint32_t layer)
{
    vimg v = { .d = *d };
    VkImageSubresourceRange rr = range_of(&v);
    if (level >= rr.levelCount)
        return false;
    return layer < (d->target == NVN_TARGET_3D ? depth_of(d, level) : rr.layerCount);
}

/**
 * @brief The view of level and layer of v (a depth slice of a 3D image, which
 * is made 2D array compatible), made the first time.
 */
static VkImageView sub_view(vimg *v, uint32_t level, uint32_t layer, VkFormat format)
{
    VkFormat own = format == v->format ? VK_FORMAT_UNDEFINED : format;
    for (uint32_t i = 0; i < v->nsub; i++)
        if (v->sub[i].level == level && v->sub[i].layer == layer && v->sub[i].format == own)
            return v->sub[i].view;
    if (v->nsub == v->capsub) {
        uint32_t cap = v->capsub ? 2 * v->capsub : 8;
        struct vsub *n = realloc(v->sub, cap * sizeof *n);
        if (!n)
            return NULL;
        v->sub = n;
        v->capsub = cap;
    }
    VkImageViewUsageCreateInfo vu;
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
                                  .pNext = plain_usage(v, &vu),
                                  .image = v->image,
                                  .viewType = VK_IMAGE_VIEW_TYPE_2D,
                                  .format = format,
                                  .subresourceRange = { v->aspect, level, 1, layer, 1 } };
    VkImageView view;
    if (VK(vkCreateImageView)(g.dev, &vci, NULL, &view) != VK_SUCCESS)
        return NULL;
    v->sub[v->nsub++] = (struct vsub){ level, layer, view, own };
    return view;
}

/** @brief The linear or sRGB twin of an 8-bit color format a texture view may name for a render target, else VK_FORMAT_UNDEFINED. */
VkFormat nvn_vk_color_twin(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;
    case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
    case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;
    case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
    default: return VK_FORMAT_UNDEFINED;
    }
}

bool nvn_vk_block_alias(const nvn_tex_desc *d, uint32_t view_format, nvn_tex_desc *ad)
{
    uint32_t bpb, bw, bh, vbpb, vbw, vbh;
    if (!nvn_tex_format(d->format, &bpb, &bw, &bh) || !nvn_tex_format(view_format, &vbpb, &vbw, &vbh) ||
        (bw == 1 && bh == 1) || vbw != 1 || vbh != 1 || vbpb != bpb || (d->levels ? d->levels : 1) != 1 ||
        d->samples > 1 || d->target == NVN_TARGET_BUFFER)
        return false;
    *ad = *d;
    ad->format = view_format;
    ad->width = (d->width + bw - 1) / bw;
    ad->height = (d->height + bh - 1) / bh;
    return nvn_tex_layout(ad) && ad->size == d->size;
}

/** @brief Makes *t the render target of texture tex seen through the view bytes view (NULL for none). */
static int target_of(replay *r, uint64_t tex, const uint8_t *view, rt *t)
{
    memset(t, 0, sizeof *t);
    if (!tex)
        return RL_OK;
    if (!(t->v = image_of(r, tex)))
        return RL_PENDING;
    t->format = VK_FORMAT_UNDEFINED;
    nvn_vk_written(t->v);
    if (view) {
        nvn_view nv;
        int rc = nvn_view_read(r->c, r->mem, view, &nv);
        if (rc != RL_OK)
            return rc;
        nvn_tex_desc alias_desc;
        if (nv.has_format && nv.format != t->v->d.format && (!nv.has_levels || nv.base_level == 0) &&
            nvn_vk_block_alias(&t->v->d, nv.format, &alias_desc)) {
            if (!(t->v = image_of_desc(r, &alias_desc, tex)))
                return RL_PENDING;
            nvn_vk_written(t->v);
        }
        if (nv.has_format && nv.format != t->v->d.format) {
            VkImageAspectFlags va = 0;
            VkFormat want = nvn_vk_format(nv.format, &va);
            if (va == VK_IMAGE_ASPECT_COLOR_BIT || va == 0) {
                if (want != VK_FORMAT_UNDEFINED && want == nvn_vk_color_twin(t->v->format))
                    t->format = want;
            }
        }
        if ((nv.has_format && nv.format != t->v->d.format && !t->format) || nv.has_swizzle ||
            (nv.has_target && nv.target != t->v->d.target) || (nv.has_levels && nv.base_level < 0) ||
            (nv.has_layers && nv.min_layer < 0))
            return nvn_vk_stop(r,
                        "render target 0x%llx (format %u, target %u) through a texture view: format %d:%u, "
                        "swizzle %d:%u,%u,%u,%u, target %d:%u, levels %d:%d+%d, layers %d:%d+%d",
                        (unsigned long long)tex, t->v->d.format, t->v->d.target, nv.has_format, nv.format,
                        nv.has_swizzle, nv.swizzle[0], nv.swizzle[1], nv.swizzle[2], nv.swizzle[3], nv.has_target,
                        nv.target, nv.has_levels, nv.base_level, nv.num_levels, nv.has_layers, nv.min_layer,
                        nv.num_layers);
        t->level = nv.has_levels ? (uint32_t)nv.base_level : 0;
        t->layer = nv.has_layers ? (uint32_t)nv.min_layer : 0;
    }
    VkImageSubresourceRange rr = range_of(t->v);
    if (!nvn_vk_rt_inside(&t->v->d, t->level, t->layer))
        return nvn_vk_stop(r, "render target 0x%llx at level %u layer %u, outside it", (unsigned long long)tex, t->level,
                    t->layer);
    VkFormat drawn = t->format ? t->format : t->v->format;
    bool whole = rr.levelCount == 1 && rr.layerCount == 1 && t->v->d.target != NVN_TARGET_3D && drawn == t->v->format;
    t->format = drawn;
    t->view = whole ? t->v->view : sub_view(t->v, t->level, t->layer, drawn);
    if (!t->view)
        return nvn_vk_stop(r, "no host view of render target 0x%llx", (unsigned long long)tex);
    return RL_OK;
}

static int set_render_targets(replay *r, const nvn_cmd *e)
{
    uint32_t n = (uint32_t)e->x[1];
    const uint64_t *colors = nvn_cmd_bytes(r->s, e->x[2]), *views = nvn_cmd_bytes(r->s, e->x[3]);
    if (n > MAX_RT)
        return nvn_vk_stop(r, "nvnCommandBufferSetRenderTargets with %u color targets", n);
    r->st->ncolor = n;
    for (uint32_t i = 0; i < n; i++) {
        int rc = target_of(r, colors ? colors[i] : 0, views && views[i] ? nvn_cmd_bytes(r->s, views[i]) : NULL,
                           &r->st->color[i]);
        if (rc != RL_OK)
            return rc;
    }
    return target_of(r, e->x[4], nvn_cmd_bytes(r->s, e->x[5]), &r->st->depth);
}

static int copy_buffer_to_buffer(replay *r, const nvn_cmd *e)
{
    uint64_t src = nvn_gpu_guest(e->x[1]), dst = nvn_gpu_guest(e->x[2]), size = e->x[3];
    if (!nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    memmove(r->mem + dst, r->mem + src, size);
    return RL_OK;
}

/**
 * @brief nvnCommandBufferUpdateUniformBuffer(buffer address, buffer size, offset, size, data): the data, copied when
 * the command was recorded, lands at the buffer's offset in the order the commands execute, so draws before it read
 * the old bytes and draws after it the new ones. In a pool imported into the host device it is a GPU write into
 * the pool's buffer, where the draws read their uniform buffers from; elsewhere it is a copy into guest memory.
 */
static int update_uniform_buffer(replay *r, const nvn_cmd *e)
{
    uint64_t size = e->x[4];
    const void *data = nvn_cmd_bytes(r->s, e->x[5]);
    if (!size)
        return RL_OK;
    if (!data)
        return nvn_vk_stop(r, "%s without its data", nvn_cmd_name(e->op));
    uint64_t at = nvn_gpu_guest(e->x[1]) + e->x[3], off;
    void *pool;
    if (size <= 65536 && size % 4 == 0 && nvn_guest_buffer(at, size, &pool, &off) && off % 4 == 0) {
        nvn_vk_begin(r);
        nvn_vk_barrier(g.cb);
        VK(vkCmdUpdateBuffer)(g.cb, (VkBuffer)pool, off, size, data);
        nvn_vk_barrier(g.cb);
        g.uniform_pending = true;
        return RL_OK;
    }
    memcpy(r->mem + at, data, size);
    return RL_OK;
}

/** @brief The level and first layer a copy names: its view's, or 0, plus the region's z (the layer of an array). */
static int copy_side(replay *r, uint64_t view, const nvn_region *rg, uint32_t *level, uint32_t *layer)
{
    *level = 0;
    *layer = (uint32_t)rg->z;
    const uint8_t *vb = nvn_cmd_bytes(r->s, view);
    if (!vb)
        return RL_OK;
    nvn_view nv;
    int rc = nvn_view_read(r->c, r->mem, vb, &nv);
    if (rc != RL_OK)
        return rc;
    if (nv.has_levels)
        *level = (uint32_t)nv.base_level;
    if (nv.has_layers)
        *layer += (uint32_t)nv.min_layer;
    return RL_OK;
}

/**
 * @brief nvnCommandBufferCopyTextureToTexture(src, srcView, srcRegion, dst,
 * dstView, dstRegion, flags): a texel copy between same-sized regions of
 * size-compatible formats, otherwise a blit (nearest, or linear with flag 1).
 */
static int copy_texture_to_texture(replay *r, const nvn_cmd *e)
{
    const nvn_region *sr = nvn_cmd_bytes(r->s, e->x[3]), *dr = nvn_cmd_bytes(r->s, e->x[6]);
    uint32_t flags = (uint32_t)e->x[7];
    if (!sr || !dr)
        return nvn_vk_stop(r, "nvnCommandBufferCopyTextureToTexture without a region");
    if (flags & ~1u)
        return nvn_vk_stop(r, "nvnCommandBufferCopyTextureToTexture with flags 0x%x", flags);
    if (sr->depth != 1 || dr->depth != 1 || sr->width <= 0 || sr->height <= 0 || dr->width <= 0 || dr->height <= 0)
        return nvn_vk_stop(r, "nvnCommandBufferCopyTextureToTexture of %dx%dx%d to %dx%dx%d texels", sr->width,
                           sr->height, sr->depth, dr->width, dr->height, dr->depth);
    vimg *src = image_of(r, e->x[1]);
    if (!src)
        return RL_PENDING;
    vimg *dst = image_of(r, e->x[4]);
    if (!dst)
        return RL_PENDING;
    if (!nvn_vk_refresh(r, src))
        return nvn_vk_stop(r, "nvnCommandBufferCopyTextureToTexture: the source texture's bytes could not be uploaded");
    nvn_vk_written(dst);
    uint32_t sl, slayer, dl, dlayer;
    int rc = copy_side(r, e->x[2], sr, &sl, &slayer);
    if (rc == RL_OK)
        rc = copy_side(r, e->x[5], dr, &dl, &dlayer);
    if (rc != RL_OK)
        return rc;
    uint32_t sb, sw, sh, db, dw, dh;
    if (!nvn_tex_format(src->d.format, &sb, &sw, &sh) || !nvn_tex_format(dst->d.format, &db, &dw, &dh))
        return nvn_vk_stop(r, "nvnCommandBufferCopyTextureToTexture of formats %u and %u", src->d.format,
                           dst->d.format);
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    VkImageSubresourceLayers ss = { src->aspect, sl, slayer, 1 }, ds = { dst->aspect, dl, dlayer, 1 };
    bool same = sr->width == dr->width && sr->height == dr->height && sb == db && sw == dw && sh == dh &&
                src->aspect == dst->aspect;
    if (same) {
        VkImageCopy ic = { ss, { sr->x, sr->y, 0 }, ds, { dr->x, dr->y, 0 },
                           { (uint32_t)sr->width, (uint32_t)sr->height, 1 } };
        VK(vkCmdCopyImage)(g.cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    } else {
        if (sw != 1 || sh != 1 || dw != 1 || dh != 1 || src->aspect != dst->aspect)
            return nvn_vk_stop(r, "nvnCommandBufferCopyTextureToTexture scaling format %u to format %u",
                               src->d.format, dst->d.format);
        VkImageBlit ib = { ss,
                           { { sr->x, sr->y, 0 }, { sr->x + sr->width, sr->y + sr->height, 1 } },
                           ds,
                           { { dr->x, dr->y, 0 }, { dr->x + dr->width, dr->y + dr->height, 1 } } };
        VkFilter f = flags & 1 && src->aspect == VK_IMAGE_ASPECT_COLOR_BIT ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        VK(vkCmdBlitImage)(g.cb, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &ib, f);
    }
    nvn_vk_barrier(g.cb);
    return RL_OK;
}

/**
 * @brief nvnCommandBufferCopyBufferToTexture(buffer, dst, dstView,
 * dstRegion, flags): the region's texels from the buffer, laid out with the
 * copy strides the command buffer had when it was recorded.
 */
static int copy_buffer_to_texture(replay *r, const nvn_cmd *e)
{
    const nvn_region *rg = nvn_cmd_bytes(r->s, e->x[4]);
    uint32_t flags = (uint32_t)e->x[5];
    if (!rg)
        return nvn_vk_stop(r, "nvnCommandBufferCopyBufferToTexture without a region");
    if (flags)
        return nvn_vk_stop(r, "nvnCommandBufferCopyBufferToTexture with flags 0x%x", flags);
    vimg *dst = image_of(r, e->x[2]);
    if (!dst)
        return RL_PENDING;
    nvn_copy_pitch p;
    if (!nvn_copy_pitch_of(dst->d.format, rg, (uint32_t)e->x[NVN_COPY_ROW_STRIDE_ARG],
                           (uint32_t)e->x[NVN_COPY_IMAGE_STRIDE_ARG], &p))
        return nvn_vk_stop(r,
                           "nvnCommandBufferCopyBufferToTexture of %dx%dx%d texels of format %u with row stride %u "
                           "and image stride %u",
                           rg->width, rg->height, rg->depth, dst->d.format, (uint32_t)e->x[NVN_COPY_ROW_STRIDE_ARG],
                           (uint32_t)e->x[NVN_COPY_IMAGE_STRIDE_ARG]);
    uint32_t level, layer;
    int rc = copy_side(r, e->x[3], rg, &level, &layer);
    if (rc != RL_OK)
        return rc;
    bool is3d = dst->d.target == NVN_TARGET_3D;
    if (is3d)
        layer -= (uint32_t)rg->z;
    if (!nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    uint64_t off;
    if (!nvn_vk_upload_bytes(r, r->mem + nvn_gpu_guest(e->x[1]), p.span, 0, 16, &off))
        return nvn_vk_stop(r, "nvnCommandBufferCopyBufferToTexture of %llu bytes, more than the upload arena holds",
                           (unsigned long long)p.span);
    nvn_vk_written(dst);
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    VkBufferImageCopy bic = {
        .bufferOffset = off,
        .bufferRowLength = p.row_texels,
        .bufferImageHeight = p.image_rows,
        .imageSubresource = { dst->aspect, level, layer, is3d ? 1u : (uint32_t)rg->depth },
        .imageOffset = { rg->x, rg->y, is3d ? rg->z : 0 },
        .imageExtent = { (uint32_t)rg->width, (uint32_t)rg->height, is3d ? (uint32_t)rg->depth : 1u },
    };
    VK(vkCmdCopyBufferToImage)(g.cb, g.arena.buf, dst->image, VK_IMAGE_LAYOUT_GENERAL, 1, &bic);
    nvn_vk_barrier(g.cb);
    return RL_OK;
}

/**
 * @brief nvnCommandBufferClearTexture, Texturei and Textureui(texture, view, region, color, mask): the region's
 * texels of each layer it covers set to color (four floats, signed or unsigned integers, the same 16 bytes), through
 * a one-layer render target of the texture in turn.
 */
static int clear_texture(replay *r, const nvn_cmd *e)
{
    const nvn_region *rg = nvn_cmd_bytes(r->s, e->x[3]);
    const uint8_t *val = nvn_cmd_bytes(r->s, e->x[4]);
    uint32_t mask = (uint32_t)e->x[5];
    if (!rg || !val)
        return nvn_vk_stop(r, "%s without a region or a color", nvn_cmd_name(e->op));
    if (mask == 0)
        return RL_OK;
    if (mask != 0xf)
        return nvn_vk_stop(r, "%s with channel mask 0x%x", nvn_cmd_name(e->op), mask);
    vimg *v = image_of(r, e->x[1]);
    if (!v)
        return RL_PENDING;
    if (v->d.target == NVN_TARGET_3D || v->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return nvn_vk_stop(r, "%s of a texture of target %u", nvn_cmd_name(e->op), v->d.target);
    uint32_t level, layer;
    int rc = copy_side(r, e->x[2], rg, &level, &layer);
    if (rc != RL_OK)
        return rc;
    nvn_vk_written(v);
    rt saved_color = r->st->color[0], saved_depth = r->st->depth;
    uint32_t saved_n = r->st->ncolor;
    VkRect2D clear = { { rg->x, rg->y }, { (uint32_t)rg->width, (uint32_t)rg->height } };
    uint32_t layers = rg->depth > 1 ? (uint32_t)rg->depth : 1;
    for (uint32_t i = 0; i < layers; i++) {
        rt t = { .v = v, .level = level, .layer = layer + i, .format = v->format };
        VkImageSubresourceRange rr = range_of(v);
        t.view = rr.levelCount == 1 && rr.layerCount == 1 ? v->view : sub_view(v, level, layer + i, v->format);
        if (!t.view)
            return nvn_vk_stop(r, "no host view of the texture %s clears", nvn_cmd_name(e->op));
        r->st->ncolor = 1;
        r->st->color[0] = t;
        r->st->depth = (rt){ 0 };
        VkRect2D area;
        if (!nvn_vk_begin_rendering(r, &area))
            return nvn_vk_stop(r, "%s without a target", nvn_cmd_name(e->op));
        VkClearAttachment ca = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0 };
        memcpy(&ca.clearValue.color, val, 16);
        VkClearRect cr = { .rect = clear, .layerCount = 1 };
        if (cr.rect.offset.x < 0 || cr.rect.offset.y < 0 || cr.rect.extent.width > area.extent.width ||
            cr.rect.extent.height > area.extent.height)
            cr.rect = area;
        VK(vkCmdClearAttachments)(g.cb, 1, &ca, 1, &cr);
        VK(vkCmdEndRendering)(g.cb);
        nvn_vk_barrier(g.cb);
    }
    r->st->ncolor = saved_n;
    r->st->color[0] = saved_color;
    r->st->depth = saved_depth;
    return RL_OK;
}

/**
 * @brief Runs one recorded command on the host device. ZCull is the GPU's
 * coarse depth-culling memory and ZF32 compression its lossless storage of
 * 32-bit float depth: both only speed up depth tests and leave every depth
 * value and test result as it is. The driver's SetZCullZF32CompressionEnable
 * writes one command word (method 0x23, one flag bit) and Save/RestoreZCullData
 * copy the culling memory to and from the game's buffer, an opaque blob the
 * game only hands back to the restore. The host device has neither, so these
 * three do nothing. The overlap mode, the two workload priorities and the resource occupancy are scheduling hints
 * for the GPU (how compute overlaps graphics, which queue work runs first, how much of the GPU a launch may take):
 * they change no result, only timing, and the host device schedules its own work, so they do nothing as well.
 */
static int run_cmd(replay *r, const nvn_cmd *e);

static bool sync_each_command(void)
{
    static int sync_each = -1;
    if (sync_each < 0)
        sync_each = getenv("RELIFT_NVN_SYNC") != NULL;
    return sync_each;
}

static int run_cmd(replay *r, const nvn_cmd *e)
{
    switch (e->op) {
    case NVN_CMD_WAIT_EVENT:
    case NVN_CMD_SIGNAL_EVENT:
        return nvn_vk_stop(r, "%s on 0x%llx, which is not an event the layer made", nvn_cmd_name(e->op),
                           (unsigned long long)e->x[1]);
    case NVN_CMD_CLEAR_COLOR:
        return clear_color(r, e);
    case NVN_CMD_CLEAR_TEXTURE:
    case NVN_CMD_CLEAR_TEXTUREI:
    case NVN_CMD_CLEAR_TEXTUREUI:
        return clear_texture(r, e);
    case NVN_CMD_CLEAR_DEPTH_STENCIL:
        return clear_depth_stencil(r, e);
    case NVN_CMD_SET_RENDER_TARGETS:
        return set_render_targets(r, e);
    case NVN_CMD_SET_SCISSOR:
        r->st->scissor = (VkRect2D){ { (int32_t)e->x[1], (int32_t)e->x[2] }, { (uint32_t)e->x[3], (uint32_t)e->x[4] } };
        r->st->scissor_set = true;
        return RL_OK;
    case NVN_CMD_UPDATE_UNIFORM_BUFFER:
        return update_uniform_buffer(r, e);
    case NVN_CMD_COPY_BUFFER_TO_BUFFER:
        return copy_buffer_to_buffer(r, e);
    case NVN_CMD_COPY_TEXTURE_TO_TEXTURE:
        return copy_texture_to_texture(r, e);
    case NVN_CMD_COPY_BUFFER_TO_TEXTURE:
        return copy_buffer_to_texture(r, e);
    case NVN_CMD_EVALUATE_DLSS:
        return nvn_dlss_run(r, e);
    case NVN_CMD_BARRIER:
        if (r->recording)
            nvn_vk_barrier(g.cb);
        return RL_OK;
    case NVN_CMD_FENCE_SYNC:
        if (r->nsyncs == sizeof r->syncs / sizeof r->syncs[0])
            return nvn_vk_stop(r, "more than %u fence syncs in one command set", r->nsyncs);
        r->syncs[r->nsyncs++] = e->x[1];
        return RL_OK;
    case NVN_CMD_SET_COMPUTE_OVERLAP_MODE:
    case NVN_CMD_SET_COMPUTE_WORKLOAD_PRIORITY:
    case NVN_CMD_SET_GRAPHICS_WORKLOAD_PRIORITY:
    case NVN_CMD_SET_RESOURCE_OCCUPANCY:
    case NVN_CMD_SET_ZCULL_ZF32_COMPRESSION_ENABLE:
    case NVN_CMD_SAVE_ZCULL_DATA:
    case NVN_CMD_RESTORE_ZCULL_DATA:
    case NVN_CMD_WAIT_SYNC:
    case NVN_CMD_SET_SHADER_SCRATCH_MEMORY:
    case NVN_CMD_SET_TEXTURE_POOL:
    case NVN_CMD_SET_SAMPLER_POOL:
    case NVN_CMD_SET_TILED_CACHE_ACTION:
    case NVN_CMD_SET_TILED_CACHE_TILE_SIZE:
        return RL_OK;
    default: {
        int rc = nvn_draw_cmd(r, e);
        if (rc == NVN_UNHANDLED)
            return nvn_vk_stop(r, "%s is not handled on the host device yet", nvn_cmd_name(e->op));
        return rc;
    }
    }
}

#define MAX_QUEUES 16u

static struct {
    uint64_t queue;
    nvn_gfx *st;
} queues[MAX_QUEUES];

/** @brief The draw state of queue, made the first time. */
static nvn_gfx *state_of(uint64_t queue)
{
    uint32_t i = 0;
    for (; i < MAX_QUEUES && queues[i].st; i++)
        if (queues[i].queue == queue)
            return queues[i].st;
    if (i == MAX_QUEUES)
        return NULL;
    nvn_gfx *st = calloc(1, sizeof *st);
    if (!st)
        return NULL;
    nvn_gfx_init(st);
    queues[i].queue = queue;
    queues[i].st = st;
    return st;
}

void nvn_gfx_init(nvn_gfx *st)
{
    st->line_width = 1.0f;
    st->sample_mask = 0xffffu;
    st->point_size = 1.0f;
    for (uint32_t k = 0; k < 16; k++)
        st->viewport_swizzle[k] = NVN_VIEWPORT_SWIZZLE_IDENTITY;
    for (uint32_t k = 0; k < 4; k++) {
        st->tess_outer[k] = 1.0f;
        if (k < 2)
            st->tess_inner[k] = 1.0f;
    }
    for (uint32_t k = 0; k < 16; k++) {
        st->depth_ranges[k][1] = 1.0f;
    }
    st->stencil_value_mask[0] = st->stencil_value_mask[1] = 0xff;
    st->stencil_mask[0] = st->stencil_mask[1] = 0xff;
}

bool nvn_vk_upload(replay *r, uint64_t addr, uint64_t n, uint64_t pad, uint64_t align, uint64_t *off)
{
    return nvn_vk_upload_bytes(r, r->mem + nvn_gpu_guest(addr), n, pad, align, off);
}

bool nvn_vk_upload_bytes(replay *r, const void *src, uint64_t n, uint64_t pad, uint64_t align, uint64_t *off)
{
    uint64_t total = n > pad ? n : pad;
    uint64_t at = (g.arena_used + align - 1) / align * align;
    if (at + total > g.arena.size) {
        if (!nvn_vk_flush(r))
            return false;
        at = 0;
    }
    if (total > g.arena.size)
        return false;
    memcpy(g.arena.map + at, src, n);
    if (total > n)
        memset(g.arena.map + at + n, 0, total - n);
    g.arena_used = at + total;
    *off = at;
    return true;
}

int nvn_vk_getter(replay *r, const char *name, const void *blob, size_t n, const uint64_t *args, unsigned nargs,
                  unsigned out_n, uint32_t *out, uint64_t *ret)
{
    rl_ctx *c = r->c;
    uint64_t sp = c->sp, base = (sp - 512) & ~15ull;
    uint64_t b = base, o = base + 256;
    if (n > 256 || out_n > 4)
        return nvn_vk_stop(r, "%s: getter arguments too large", name);
    memcpy(r->mem + b, blob, n);
    memset(r->mem + o, 0, 64);
    uint64_t a[8] = { b };
    unsigned k = 1;
    for (unsigned i = 0; i < nargs && k < 8; i++)
        a[k++] = args[i];
    for (unsigned i = 0; i < out_n && k < 8; i++)
        a[k++] = o + 4u * i;
    c->sp = base;
    int rc = nvn_driver_call(c, r->mem, name, a, k, ret);
    c->sp = sp;
    if (rc == RL_OK && out_n)
        memcpy(out, r->mem + o, out_n == 1 ? 16u : 4u * out_n);
    if (rc == RL_OK)
        memcpy(&r->fret, &c->v[0], sizeof r->fret);
    return rc;
}

static int64_t now_ns(void);
static __thread bool core_left;
static __thread int64_t lock_since;

void nvn_core_leave(void)
{
    if (core_left)
        return;
    core_left = true;
    hos_sched_leave();
}

void nvn_core_return(void)
{
    if (!core_left)
        return;
    core_left = false;
    hos_sched_return();
}

void nvn_lock(void)
{
    if (pthread_mutex_trylock(&g.lock) != 0) {
        nvn_core_leave();
        pthread_mutex_lock(&g.lock);
    }
    lock_since = now_ns();
}

void nvn_unlock(void)
{
    int64_t held = now_ns() - lock_since;
    bool kept = !core_left;
    pthread_mutex_unlock(&g.lock);
    nvn_core_return();
    if (kept && held > HOLD_WARN_NS)
        rl_log(RL_LOG_WARN, "nvn", "a guest thread held its core for %lld ms inside the NVN layer without leaving it",
               (long long)(held / 1000000));
}

/** @brief A submitted command set being replayed: its replay state and the syncs its fences signal when it finishes. */
typedef struct vk_run {
    replay r;
} vk_run;

static nvn_sched sched;

static int sched_exec(void *user, const nvn_cmd *e, const nvn_cmdset *s)
{
    replay *r = &((vk_run *)user)->r;
    r->s = s;
    int rc = run_cmd(r, e);
    if (rc == RL_OK && sync_each_command() && !nvn_vk_flush(r))
        rc = nvn_vk_stop(r, "the host device failed after %s", nvn_cmd_name(e->op));
    return rc;
}

static bool sched_flush(void *user) { return nvn_vk_flush(&((vk_run *)user)->r); }

static uint32_t *sched_word(void *user, uint64_t addr) { return (uint32_t *)(void *)(((vk_run *)user)->r.mem + addr); }

static const nvn_sched_ops sched_ops = { sched_exec, sched_flush, sched_word };

/** @brief Signals the fence syncs of the finished runs, frees them, and returns the first stop a pump found. */
static void finish_runs(nvn_sched_run *done)
{
    while (done) {
        nvn_sched_run *next = done->next;
        vk_run *u = done->user;
        for (uint32_t i = 0; i < u->r.nsyncs; i++)
            nvn_sync_signal(u->r.syncs[i]);
        free(u);
        nvn_sched_run_free(done);
        done = next;
    }
}

int nvn_vk_run(rl_ctx *c, uint8_t *mem, uint64_t queue, const nvn_cmdset *s)
{
    if (!up(c))
        return RL_PENDING;
    nvn_sched_item *items;
    uint32_t n;
    uint64_t bad;
    nvn_lock();
    replay probe = { .c = c, .mem = mem, .s = s };
    if (nvn_sched_flatten(s, &items, &n, &bad)) {
        int rc = nvn_vk_stop(&probe, "a call of command handle 0x%llx, which is not a command set relift made",
                             (unsigned long long)bad);
        nvn_unlock();
        return rc;
    }
    vk_run *u = calloc(1, sizeof *u);
    nvn_gfx *st = state_of(queue);
    if (!u || !st || !nvn_sched_add(&sched, queue, items, n, u)) {
        free(u);
        free(items);
        nvn_unlock();
        return rl_guest_fatal(c, "nvn", st ? "out of memory queueing a submit" : "more than %u queues", MAX_QUEUES);
    }
    u->r = (replay){ .c = c, .mem = mem, .s = s, .st = st };
    nvn_sched_run *done = NULL;
    int64_t t0 = now_ns();
    int rc = nvn_sched_pump(&sched, &sched_ops, &done);
    if (rc == RL_OK && !nvn_vk_flush(&u->r))
        rc = rl_guest_fatal(c, "nvn", "the host device failed a submit");
    tm.run_ns += (uint64_t)(now_ns() - t0);
    tm.runs++;
    nvn_trace('U', (uint64_t)(now_ns() - t0), n, 0);
    nvn_unlock();
    if (rc == RL_OK)
        finish_runs(done);
    return rc;
}

void nvn_vk_pump(rl_ctx *c, uint8_t *mem)
{
    if (!__atomic_load_n(&sched.pending, __ATOMIC_ACQUIRE))
        return;
    nvn_lock();
    nvn_sched_run *done = NULL;
    for (nvn_sched_run *r = sched.head; r; r = r->next) {
        vk_run *u = r->user;
        u->r.c = c;
        u->r.mem = mem;
    }
    int rc = sched.head ? nvn_sched_pump(&sched, &sched_ops, &done) : RL_OK;
    nvn_unlock();
    if (rc == RL_OK)
        finish_runs(done);
}

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/** @brief Reads v back into RGBA8 pixels (rows of 4 * width bytes); NULL when v is not a 4-byte color format. */
/** @brief A color image of v's size in RGBA8, with v's level 0 blitted into it (for formats other than RGBA8). */
static bool blit_rgba8(replay *r, vimg *v, VkImage *img, VkDeviceMemory *memory)
{
    VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                             .imageType = VK_IMAGE_TYPE_2D,
                             .format = VK_FORMAT_R8G8B8A8_UNORM,
                             .extent = { v->d.width, v->d.height, 1 },
                             .mipLevels = 1,
                             .arrayLayers = 1,
                             .samples = VK_SAMPLE_COUNT_1_BIT,
                             .tiling = VK_IMAGE_TILING_OPTIMAL,
                             .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
    VkMemoryRequirements req;
    *img = NULL;
    *memory = NULL;
    if (VK(vkCreateImage)(g.dev, &ci, NULL, img) != VK_SUCCESS)
        return false;
    VK(vkGetImageMemoryRequirements)(g.dev, *img, &req);
    int32_t t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)t };
    if (t < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, memory) != VK_SUCCESS ||
        VK(vkBindImageMemory)(g.dev, *img, *memory, 0) != VK_SUCCESS)
        return false;
    nvn_vk_begin(r);
    VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                .image = *img,
                                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(vkCmdPipelineBarrier)(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                             NULL, 1, &ib);
    VkImageBlit b = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                      .srcOffsets = { { 0, 0, 0 }, { (int32_t)v->d.width, (int32_t)v->d.height, 1 } },
                      .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                      .dstOffsets = { { 0, 0, 0 }, { (int32_t)v->d.width, (int32_t)v->d.height, 1 } } };
    VK(vkCmdBlitImage)(g.cb, v->image, VK_IMAGE_LAYOUT_GENERAL, *img, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       VK_FILTER_NEAREST);
    nvn_vk_barrier(g.cb);
    return true;
}

/**
 * @brief Reads v's level 0 back as RGBA8 pixels (rows of 4 * width bytes): RGBA8 images as they are, other color
 * formats converted by a blit (for looking at them, not exact). NULL for depth formats.
 */
static uint8_t *read_back(replay *r, vimg *v)
{
    if (v->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return NULL;
    bool direct = v->format == VK_FORMAT_R8G8B8A8_UNORM || v->format == VK_FORMAT_R8G8B8A8_SRGB;
    uint64_t bytes = (uint64_t)v->d.width * v->d.height * 4;
    staging s;
    VkImage tmp = NULL;
    VkDeviceMemory tmp_mem = NULL;
    uint8_t *px = malloc(bytes);
    if (!px || !nvn_vk_readback_new(&s, bytes)) {
        free(px);
        return NULL;
    }
    bool ok = direct || blit_rgba8(r, v, &tmp, &tmp_mem);
    if (ok) {
        nvn_vk_begin(r);
        VkBufferImageCopy bic = { .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                                  .imageExtent = { v->d.width, v->d.height, 1 } };
        VK(vkCmdCopyImageToBuffer)(g.cb, direct ? v->image : tmp, VK_IMAGE_LAYOUT_GENERAL, s.buf, 1, &bic);
        nvn_vk_barrier(g.cb);
        ok = nvn_vk_flush(r);
    }
    if (ok)
        memcpy(px, s.map, bytes);
    nvn_vk_staging_free(&s);
    if (tmp)
        VK(vkDestroyImage)(g.dev, tmp, NULL);
    if (tmp_mem)
        VK(vkFreeMemory)(g.dev, tmp_mem, NULL);
    if (!ok) {
        free(px);
        return NULL;
    }
    return px;
}

/**
 * @brief The readback of the presented image, kept between presents: a host buffer in cached memory when the host
 * has it (CPU reads of uncached write-combined memory run at a fraction of the speed of cached ones), the image
 * copied into it, and the RGB frame converted from it. One of each exists; nvn_present uses them under g.lock.
 */
static struct {
    staging s;
    bool coherent;
    uint8_t *rgb;
    size_t rgb_cap;
} pres_buf;

/** @brief Makes the present readback buffer at least bytes big, in cached host memory when there is some. */
static bool present_buffer(uint64_t bytes)
{
    if (pres_buf.s.buf && pres_buf.s.size >= bytes)
        return true;
    if (pres_buf.s.buf)
        nvn_vk_staging_free(&pres_buf.s);
    memset(&pres_buf.s, 0, sizeof pres_buf.s);
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = bytes,
                               .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
    if (VK(vkCreateBuffer)(g.dev, &bci, NULL, &pres_buf.s.buf) != VK_SUCCESS)
        return false;
    VkMemoryRequirements req;
    VK(vkGetBufferMemoryRequirements)(g.dev, pres_buf.s.buf, &req);
    int32_t t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
    pres_buf.coherent = false;
    if (t >= 0)
        pres_buf.coherent = (g.memprops.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    else
        pres_buf.coherent = (t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) >= 0;
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                 .allocationSize = req.size,
                                 .memoryTypeIndex = (uint32_t)t };
    void *p = NULL;
    if (t < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, &pres_buf.s.memory) != VK_SUCCESS ||
        VK(vkBindBufferMemory)(g.dev, pres_buf.s.buf, pres_buf.s.memory, 0) != VK_SUCCESS ||
        VK(vkMapMemory)(g.dev, pres_buf.s.memory, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS) {
        nvn_vk_staging_free(&pres_buf.s);
        memset(&pres_buf.s, 0, sizeof pres_buf.s);
        return false;
    }
    pres_buf.s.map = p;
    pres_buf.s.size = bytes;
    return true;
}

/**
 * @brief Copies v's level 0 into the present buffer as RGBA8 (RGBA8 images straight, other color formats through a
 * blit) and makes it visible to the CPU. NULL when it cannot (a depth image or a failed submit).
 */
static const uint8_t *present_readback(replay *r, vimg *v)
{
    if (v->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return NULL;
    bool direct = v->format == VK_FORMAT_R8G8B8A8_UNORM || v->format == VK_FORMAT_R8G8B8A8_SRGB;
    uint64_t bytes = (uint64_t)v->d.width * v->d.height * 4;
    if (!present_buffer(bytes))
        return NULL;
    VkImage tmp = NULL;
    VkDeviceMemory tmp_mem = NULL;
    bool ok = direct || blit_rgba8(r, v, &tmp, &tmp_mem);
    if (ok) {
        nvn_vk_begin(r);
        VkBufferImageCopy bic = { .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .layerCount = 1 },
                                  .imageExtent = { v->d.width, v->d.height, 1 } };
        VK(vkCmdCopyImageToBuffer)(g.cb, direct ? v->image : tmp, VK_IMAGE_LAYOUT_GENERAL, pres_buf.s.buf, 1, &bic);
        nvn_vk_barrier(g.cb);
        ok = nvn_vk_flush(r);
    }
    if (tmp)
        VK(vkDestroyImage)(g.dev, tmp, NULL);
    if (tmp_mem)
        VK(vkFreeMemory)(g.dev, tmp_mem, NULL);
    if (!ok)
        return NULL;
    if (!pres_buf.coherent) {
        VkMappedMemoryRange mr = { .sType = VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE,
                                   .memory = pres_buf.s.memory,
                                   .size = VK_WHOLE_SIZE };
        VK(vkInvalidateMappedMemoryRanges)(g.dev, 1, &mr);
    }
    return pres_buf.s.map;
}

/**
 * @brief Copies the crop rectangle of v's level 0 into a presenter slot's image on the GPU (RGBA8 images straight,
 * other color formats through a blit), and waits for the copy. The slot ends in TRANSFER_SRC_OPTIMAL.
 */
static bool present_to_slot(replay *r, vimg *v, const uint32_t rect[4], const rl_present_slot *s)
{
    if (v->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return false;
    bool direct = v->format == VK_FORMAT_R8G8B8A8_UNORM || v->format == VK_FORMAT_R8G8B8A8_SRGB;
    VkImage tmp = NULL;
    VkDeviceMemory tmp_mem = NULL;
    bool ok = direct || blit_rgba8(r, v, &tmp, &tmp_mem);
    VkImage slot = (VkImage)(uintptr_t)s->image;
    if (ok) {
        nvn_vk_begin(r);
        VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = slot,
                                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VK(vkCmdPipelineBarrier)(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &ib);
        VkImageCopy ic = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           .srcOffset = { (int32_t)rect[0], (int32_t)rect[1], 0 },
                           .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                           .extent = { rect[2], rect[3], 1 } };
        VK(vkCmdCopyImage)(g.cb, direct ? v->image : tmp, VK_IMAGE_LAYOUT_GENERAL, slot,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &ic);
        ib.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        ib.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        ib.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        ib.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        VK(vkCmdPipelineBarrier)(g.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &ib);
        ok = nvn_vk_flush(r);
    }
    if (tmp)
        VK(vkDestroyImage)(g.dev, tmp, NULL);
    if (tmp_mem)
        VK(vkFreeMemory)(g.dev, tmp_mem, NULL);
    return ok;
}

#define PRESENT_RATE_NS 5000000000ull

/**
 * @brief Holds a present until the vi vsync grid reaches the frame's turn: interval vsyncs (the window's present
 * interval; 0 does not wait) after the previous present's, as the console's display shows a frame at a vblank. The
 * grid is the one the vsync event runs on (and follows the host display), so the game's own vsync waits and its
 * presents line up. RELIFT_UNPACED=1, a measuring aid, skips the wait to find the host's cost of a frame.
 */
static void pace_present(uint32_t interval)
{
    static int off = -1;
    static int64_t last;
    if (off < 0) {
        const char *e = getenv("RELIFT_UNPACED");
        off = e && *e == '1';
    }
    if (off || interval == 0)
        return;
    int64_t now = now_ns();
    int64_t earliest = last + (int64_t)(interval > 1 ? interval - 1 : 0) * 16666667ll;
    int64_t at = nvn_vsync_after(now > earliest ? now : earliest);
    if (at > now) {
        struct timespec d = { (time_t)((at - now) / 1000000000ll), (long)((at - now) % 1000000000ll) };
        nanosleep(&d, NULL);
    }
    last = at;
}

static int64_t g_last_present_ns;

/**
 * @brief Writes the frame trace's per-present cost lines (stats.trace): T with the presenting thread's wall time since
 * its last present returned, its CPU time and the NVN replay time; X with the upload, write-back and pipeline creation times;
 * Y with the counts (uploads, write-backs, alias copies on the GPU, pipelines made in 16 bits each) and uploaded bytes.
 */
static void trace_present_costs(void)
{
    static int64_t last_wall, last_cpu;
    static uint64_t last_run, last_pm, last_pn, last_pl;
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    int64_t cpu = (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec, wall = now_ns();
    uint64_t pm, pn, pl, pln;
    nvn_pipe_stats(&pm, &pn, &pl, &pln);
    uint64_t run = tm.run_ns >= last_run ? tm.run_ns - last_run : tm.run_ns;
    if (last_wall) {
        nvn_trace('T', (uint64_t)(wall - g_last_present_ns), (uint64_t)(cpu - last_cpu), run);
        nvn_trace('X', ppc.upload_ns, ppc.wb_ns, pn - last_pn);
        nvn_trace('Y',
                  (uint64_t)(ppc.uploads & 0xffff) | (uint64_t)(ppc.wbs & 0xffff) << 16 |
                      (uint64_t)(ppc.gpu_alias & 0xffff) << 32 | (uint64_t)((pm - last_pm) & 0xffff) << 48,
                  ppc.upload_bytes, pl - last_pl);
    }
    memset(&ppc, 0, sizeof ppc);
    last_wall = wall;
    last_cpu = cpu;
    last_run = tm.run_ns;
    last_pm = pm;
    last_pn = pn;
    last_pl = pl;
}

/** @brief The window frames go to, opened with the first one; NULL in a headless run. */
static rl_window *present_window(uint32_t w, uint32_t h)
{
    static rl_window *win;
    const rl_window_backend *b = rl_window_backend_current();
    if (!win && b && b->open)
        win = b->open("relift", w, h);
    return win;
}

/** @brief Logs the guest's present rate every few seconds (info), so a run's speed shows without a profiler. */
static void present_rate(uint64_t total)
{
    static uint64_t last_ns, last_total;
    uint64_t now = (uint64_t)now_ns();
    if (!last_ns) {
        last_ns = now;
        return;
    }
    if (now - last_ns < PRESENT_RATE_NS)
        return;
    static struct timespec cpu_prev;
    struct timespec cpu;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu);
    double cpu_ms = (double)(cpu.tv_sec - cpu_prev.tv_sec) * 1e3 + (double)(cpu.tv_nsec - cpu_prev.tv_nsec) / 1e6;
    cpu_prev = cpu;
    double frames = (double)(total - last_total);
    static uint64_t pm0, pn0, pl0, pln0;
    uint64_t pm, pn, pl, pln;
    nvn_pipe_stats(&pm, &pn, &pl, &pln);
    rl_log(RL_LOG_INFO, "nvn",
           "per present: wall %.2f ms, thread cpu %.2f ms, replay %.2f ms in %.1f submits (%.1f batches), submit call %.2f ms, "
           "wait for the device %.2f ms, device busy %.2f ms, %.2f pipelines made taking %.2f ms (%.2f of them late, %.2f ms)",
           (double)(now - last_ns) / 1e6 / frames, cpu_ms / frames, (double)tm.run_ns / 1e6 / frames,
           (double)tm.runs / frames, (double)tm.flushes / frames, (double)tm.submit_ns / 1e6 / frames,
           (double)tm.wait_ns / 1e6 / frames, (double)tm.gpu_ns / 1e6 / frames, (double)(pm - pm0) / frames, (double)(pn - pn0) / 1e6 / frames,
           (double)(pl - pl0) / frames, (double)(pln - pln0) / 1e6 / frames);
    pm0 = pm;
    pn0 = pn;
    pl0 = pl;
    pln0 = pln;
    memset(&tm, 0, sizeof tm);
    uint64_t faults, starts;
    nvn_watch_stats(&faults, &starts);
    rl_log(RL_LOG_INFO, "nvn",
           "%llu presents, %.1f per second over the last %.1f s; texture watches %llu started, %llu written; "
           "host input %llu events",
           (unsigned long long)total, (double)(total - last_total) * 1e9 / (double)(now - last_ns),
           (double)(now - last_ns) / 1e9, (unsigned long long)starts, (unsigned long long)faults,
           (unsigned long long)rl_input_host_events(RL_INPUT_HOST_KINDS));
    last_ns = now;
    last_total = total;
}

int nvn_present(rl_ctx *c, uint8_t *mem, uint64_t queue, uint64_t texture, const int32_t crop[4], uint32_t interval)
{
    (void)queue;
    if (!up(c))
        return RL_PENDING;
    nvn_lock();
    nvn_gfx none = { 0 };
    replay r = { .c = c, .mem = mem, .st = &none };
    vimg *v = image_of(&r, texture);
    uint32_t w = v ? v->d.width : 0, h = v ? v->d.height : 0;
    uint32_t format = v ? v->d.format : 0;
    uint64_t n = g.presents++;
    nvn_trace_present(NULL);
    uint32_t rect[4] = { 0, 0, w, h };
    bool cropped = v && nvn_crop_rect(crop, w, h, rect);
    const rl_window_backend *b = rl_window_backend_current();
    rl_window *win = NULL;
    rl_present *pres = NULL;
    bool shown = true;
    const uint8_t *px = NULL;
    uint64_t frame;
    bool capture = cropped && vkf_capture_due(&frame);
    if (cropped && b && b->open) {
        pthread_mutex_unlock(&g.lock);
        win = present_window(rect[2], rect[3]);
        pres = win ? rl_window_presenter(win) : NULL;
        pthread_mutex_lock(&g.lock);
        v = image_of(&r, texture);
        shown = v != NULL;
    }
    if (shown && cropped && pres) {
        rl_present_slot slot;
        shown = rl_present_slot_begin(pres, rect[2], rect[3], &slot);
        if (shown && !present_to_slot(&r, v, rect, &slot)) {
            rl_present_slot_abort(pres, &slot);
            shown = false;
        } else if (shown) {
            rl_present_slot_end(pres, &slot, rect[2], rect[3]);
        }
    }
    bool pixels = capture || (cropped && win && !pres && b->present_rgb);
    if (shown && pixels)
        px = present_readback(&r, v);
    if (shown && px && capture)
        vkf_capture_write(frame, (uint64_t)now_ns(), rect[2], rect[3], px + ((size_t)rect[1] * w + rect[0]) * 4,
                          (size_t)w * 4, false);
    uint8_t *rgb = NULL;
    if (px && win && !pres && b->present_rgb) {
        size_t need = (size_t)rect[2] * rect[3] * 3;
        if (need > pres_buf.rgb_cap) {
            free(pres_buf.rgb);
            pres_buf.rgb = malloc(need);
            pres_buf.rgb_cap = pres_buf.rgb ? need : 0;
        }
        uint8_t *dst = pres_buf.rgb;
        for (uint32_t y = 0; dst && y < rect[3]; y++) {
            const uint8_t *src = px + ((size_t)(rect[1] + y) * w + rect[0]) * 4;
            for (uint32_t x = 0; x < rect[2]; x++, src += 4, dst += 3) {
                dst[0] = src[0];
                dst[1] = src[1];
                dst[2] = src[2];
            }
        }
        rgb = pres_buf.rgb;
    }
    nvn_unlock();
    if (!v)
        return RL_PENDING;
    if (!shown)
        return rl_guest_fatal(c, "nvn", "cannot present texture 0x%llx (format %u): its copy to the window failed",
                              (unsigned long long)texture, format);
    if (!cropped)
        return rl_guest_fatal(c, "nvn", "the window crop %d,%d %dx%d leaves nothing of the %ux%u texture 0x%llx",
                              crop[0], crop[1], crop[2], crop[3], w, h, (unsigned long long)texture);
    nvn_core_leave();
    if (nvn_trace_on())
        trace_present_costs();
    int64_t paced_at = now_ns();
    pace_present(interval);
    g_last_present_ns = now_ns();
    nvn_trace('N', n, (uint64_t)(g_last_present_ns - paced_at), 0);
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "present %llu: texture 0x%llx, %ux%u", (unsigned long long)n, (unsigned long long)texture,
               rect[2], rect[3]);
    nvn_frame_presented();
    present_rate(n + 1);
    if (rgb)
        rl_window_present_rgb(win, rgb, rect[2], rect[3]);
    nvn_core_return();
    if (n == 0)
        rl_log(RL_LOG_INFO, "nvn", "first frame on the host device: %ux%u, %s", rect[2], rect[3],
               pres ? "presented from the GPU" : px ? "presented through host memory" : "not shown");
    return RL_OK;
}
