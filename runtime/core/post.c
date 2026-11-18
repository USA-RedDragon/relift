/**
 * @file post.c
 * @brief Player picture options (post.h): the core picture pipeline.
 *
 * The pipeline runs compute passes on the device's first compute queue, on
 * a Vulkan instance and device of its own: the frame goes into a host
 * buffer as one RGBA8 word per pixel, is copied to a device buffer, runs
 * through the passes the options ask for and comes back the same way. The
 * passes' SPIR-V and SMAA's textures are post_data.h (from
 * runtime/core/shaders and runtime/third_party/smaa). The upscaler stage
 * (rl_post_set_upscaler) records into the same command buffer ahead of the
 * anti-aliasing passes, which then run at its output size.
 *
 * With every option off and no upscaler rl_post_apply returns the frame it
 * was given and nothing here starts. When Vulkan cannot start or a frame fails, the
 * options are off for the rest of the run and the log says why.
 */
#include <relift/post.h>
#include <relift/settings.h>

#include "core.h"
#include "hostlib.h"
#include "post_data.h"
#include "vkmin.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define GROUP 8u
#define NBIND 6u

/** @brief VkPhysicalDeviceProperties up to the device name; rest covers the remainder of the real structure. */
typedef struct rlvk_device_properties {
    uint32_t apiVersion, driverVersion, vendorID, deviceID;
    int32_t deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    uint64_t rest[128];
} rlvk_device_properties;

typedef void(RLVK_CALL *rlvk_props_fn)(rlvk_handle, rlvk_device_properties *);

/** @brief The push constants of every pass (post.glsl Params). */
typedef struct params {
    int32_t width, height;
    float threshold;
    int32_t max_search_steps, max_search_steps_diag, corner_rounding;
} params;

typedef struct buf {
    rlvk_handle buffer, memory;
    void *map;
    rlvk_size size;
    bool coherent;
} buf;

enum { B_SRC, B_IN, B_OUT, B_EDGES, B_BLEND, B_AREA, B_SEARCH, B_STAGE_IN, B_STAGE_OUT, B_COUNT };

enum { P_FXAA, P_SMAA_EDGES, P_SMAA_WEIGHTS, P_SMAA_WEIGHTS_FAST, P_SMAA_BLEND, P_COUNT };

typedef struct vk {
#define RLVK_FN_FIELD(name, ret, args) ret(RLVK_CALL *name) args;
    RLVK_INSTANCE_FNS(RLVK_FN_FIELD)
    RLVK_DEVICE_FNS(RLVK_FN_FIELD)
#undef RLVK_FN_FIELD
    rlvk_handle instance, phys, device, queue, pool, cmd, fence;
    rlvk_handle set_layout, layout, dpool, set;
    rlvk_handle pipe[P_COUNT];
    uint32_t family;
    rlvk_memory_properties mem;
    buf b[B_COUNT];
    uint32_t sw, sh, w, h;
} vk;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static vk g_vk;
static bool g_tried, g_ok;
static const rl_post_stage *g_up;
static void *g_up_user;
static bool g_up_started, g_up_ok;
static uint8_t *g_out;
static size_t g_out_cap;

static const struct {
    const uint32_t *code;
    size_t size;
} g_spv[P_COUNT] = {
    [P_FXAA] = { rl_post_spv_fxaa, sizeof rl_post_spv_fxaa },
    [P_SMAA_EDGES] = { rl_post_spv_smaa_edges, sizeof rl_post_spv_smaa_edges },
    [P_SMAA_WEIGHTS] = { rl_post_spv_smaa_weights, sizeof rl_post_spv_smaa_weights },
    [P_SMAA_WEIGHTS_FAST] = { rl_post_spv_smaa_weights_fast, sizeof rl_post_spv_smaa_weights_fast },
    [P_SMAA_BLEND] = { rl_post_spv_smaa_blend, sizeof rl_post_spv_smaa_blend },
};

/** @brief SMAA's quality presets (SMAA.hlsl SMAA_PRESET_*) by picture.aa_quality; low and medium skip the diagonal and
 * corner passes. */
static const struct {
    float threshold;
    int32_t steps, diag_steps, corner;
    bool fast;
} g_smaa[] = {
    { 0.15f, 4, 0, 0, true },
    { 0.1f, 8, 0, 0, true },
    { 0.1f, 16, 8, 25, false },
    { 0.05f, 32, 16, 25, false },
};

static bool fail(const char *what, rlvk_result r)
{
    rl_log(RL_LOG_WARN, "post", "picture options are off: %s failed (VkResult %d)", what, (int)r);
    return false;
}

static int device_rank(int32_t type)
{
    static const int rank[] = { 4, 1, 0, 2, 3 };
    return type >= 0 && type <= 4 ? rank[type] : 5;
}

static bool has_ext(vk *v, rlvk_handle phys, const char *name)
{
    uint32_t n = 0;
    if (v->vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL) != RLVK_SUCCESS || !n)
        return false;
    rlvk_extension_properties *e = calloc(n, sizeof *e);
    if (!e)
        return false;
    bool found = false;
    if (v->vkEnumerateDeviceExtensionProperties(phys, NULL, &n, e) >= RLVK_SUCCESS)
        for (uint32_t i = 0; i < n && !found; i++)
            found = !strcmp(e[i].extensionName, name);
    free(e);
    return found;
}

static bool make_instance(vk *v, rlvk_gipa_fn gipa)
{
    rlvk_create_instance_fn create = (rlvk_create_instance_fn)gipa(0, "vkCreateInstance");
    if (!create)
        return fail("vkCreateInstance lookup", 0);
    rlvk_application_info app = {
        .sType = RLVK_ST_APPLICATION_INFO,
        .pApplicationName = "relift",
        .pEngineName = "relift",
        .apiVersion = RLVK_API_VERSION_1_0,
    };
    static const char *const portability[] = { "VK_KHR_portability_enumeration" };
    rlvk_instance_create_info ci = {
        .sType = RLVK_ST_INSTANCE_CREATE_INFO,
        .flags = RLVK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR,
        .pApplicationInfo = &app,
        .enabledExtensionCount = 1,
        .ppEnabledExtensionNames = portability,
    };
    rlvk_result r = create(&ci, NULL, &v->instance);
    if (r == RLVK_ERROR_EXTENSION_NOT_PRESENT) {
        ci.flags = 0;
        ci.enabledExtensionCount = 0;
        r = create(&ci, NULL, &v->instance);
    }
    if (r != RLVK_SUCCESS)
        return fail("vkCreateInstance", r);
#define RLVK_LOAD_I(name, ret, args)                                                                                 \
    v->name = (ret(RLVK_CALL *) args)gipa(v->instance, #name);                                                       \
    if (!v->name)                                                                                                    \
        return fail(#name " lookup", 0);
    RLVK_INSTANCE_FNS(RLVK_LOAD_I)
#undef RLVK_LOAD_I
    return true;
}

static bool pick_device(vk *v, rlvk_gipa_fn gipa)
{
    rlvk_props_fn props = (rlvk_props_fn)gipa(v->instance, "vkGetPhysicalDeviceProperties");
    uint32_t n = 0;
    rlvk_result r = v->vkEnumeratePhysicalDevices(v->instance, &n, NULL);
    if (r != RLVK_SUCCESS || !n)
        return fail("vkEnumeratePhysicalDevices", r);
    rlvk_handle *devs = calloc(n, sizeof *devs);
    if (!devs)
        return fail("allocation", 0);
    r = v->vkEnumeratePhysicalDevices(v->instance, &n, devs);
    int best = 6;
    for (uint32_t i = 0; r >= RLVK_SUCCESS && i < n; i++) {
        uint32_t nf = 0;
        v->vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nf, NULL);
        rlvk_queue_family_properties fam[16];
        nf = nf > 16 ? 16 : nf;
        v->vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nf, fam);
        rlvk_device_properties p = { 0 };
        if (props)
            props(devs[i], &p);
        int rank = device_rank(p.deviceType);
        for (uint32_t f = 0; f < nf; f++)
            if ((fam[f].queueFlags & RLVK_QUEUE_COMPUTE_BIT) && fam[f].queueCount && rank < best) {
                best = rank;
                v->phys = devs[i];
                v->family = f;
                break;
            }
    }
    free(devs);
    if (!v->phys)
        return fail("finding a device with a compute queue", r);
    v->vkGetPhysicalDeviceMemoryProperties(v->phys, &v->mem);
    return true;
}

static bool make_device(vk *v)
{
    float prio = 1.0f;
    rlvk_device_queue_create_info q = {
        .sType = RLVK_ST_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = v->family,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    static const char *const subset[] = { "VK_KHR_portability_subset" };
    rlvk_device_create_info ci = {
        .sType = RLVK_ST_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &q,
    };
    if (has_ext(v, v->phys, subset[0])) {
        ci.enabledExtensionCount = 1;
        ci.ppEnabledExtensionNames = subset;
    }
    rlvk_result r = v->vkCreateDevice(v->phys, &ci, NULL, &v->device);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateDevice", r);
#define RLVK_LOAD_D(name, ret, args)                                                                                 \
    v->name = (ret(RLVK_CALL *) args)v->vkGetDeviceProcAddr(v->device, #name);                                       \
    if (!v->name)                                                                                                    \
        return fail(#name " lookup", 0);
    RLVK_DEVICE_FNS(RLVK_LOAD_D)
#undef RLVK_LOAD_D
    v->vkGetDeviceQueue(v->device, v->family, 0, &v->queue);
    return true;
}

/** @brief A memory type in bits with every flag of want, preferring one that also has every flag of nice. */
static int memory_type(const vk *v, uint32_t bits, uint32_t want, uint32_t nice)
{
    int found = -1;
    for (uint32_t i = 0; i < v->mem.memoryTypeCount; i++) {
        uint32_t f = v->mem.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (f & want) != want)
            continue;
        if ((f & nice) == nice)
            return (int)i;
        if (found < 0)
            found = (int)i;
    }
    return found;
}

static void buf_free(vk *v, buf *b)
{
    if (b->map)
        v->vkUnmapMemory(v->device, b->memory);
    if (b->buffer)
        v->vkDestroyBuffer(v->device, b->buffer, NULL);
    if (b->memory)
        v->vkFreeMemory(v->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

static bool buf_make(vk *v, buf *b, rlvk_size size, uint32_t usage, bool host, bool cached)
{
    buf_free(v, b);
    rlvk_buffer_create_info ci = { .sType = RLVK_ST_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    rlvk_result r = v->vkCreateBuffer(v->device, &ci, NULL, &b->buffer);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateBuffer", r);
    rlvk_memory_requirements req;
    v->vkGetBufferMemoryRequirements(v->device, b->buffer, &req);
    uint32_t want = host ? RLVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0;
    uint32_t nice = host ? RLVK_MEMORY_PROPERTY_HOST_COHERENT_BIT | (cached ? RLVK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0)
                         : RLVK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int type = memory_type(v, req.memoryTypeBits, want, nice);
    if (type < 0)
        return fail("finding a memory type", 0);
    rlvk_memory_allocate_info ai = {
        .sType = RLVK_ST_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    r = v->vkAllocateMemory(v->device, &ai, NULL, &b->memory);
    if (r != RLVK_SUCCESS)
        return fail("vkAllocateMemory", r);
    r = v->vkBindBufferMemory(v->device, b->buffer, b->memory, 0);
    if (r != RLVK_SUCCESS)
        return fail("vkBindBufferMemory", r);
    b->size = size;
    b->coherent = (v->mem.memoryTypes[type].propertyFlags & RLVK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (host) {
        r = v->vkMapMemory(v->device, b->memory, 0, ~(rlvk_size)0, 0, &b->map);
        if (r != RLVK_SUCCESS)
            return fail("vkMapMemory", r);
    }
    return true;
}

static bool make_pipelines(vk *v)
{
    rlvk_descriptor_set_layout_binding bind[NBIND];
    for (uint32_t i = 0; i < NBIND; i++)
        bind[i] = (rlvk_descriptor_set_layout_binding){
            .binding = i,
            .descriptorType = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT,
        };
    rlvk_descriptor_set_layout_create_info sl = {
        .sType = RLVK_ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = NBIND,
        .pBindings = bind,
    };
    rlvk_result r = v->vkCreateDescriptorSetLayout(v->device, &sl, NULL, &v->set_layout);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateDescriptorSetLayout", r);
    rlvk_push_constant_range pcr = { .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(params) };
    rlvk_pipeline_layout_create_info pl = {
        .sType = RLVK_ST_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &v->set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    r = v->vkCreatePipelineLayout(v->device, &pl, NULL, &v->layout);
    if (r != RLVK_SUCCESS)
        return fail("vkCreatePipelineLayout", r);
    for (int p = 0; p < P_COUNT; p++) {
        rlvk_shader_module_create_info sm = {
            .sType = RLVK_ST_SHADER_MODULE_CREATE_INFO,
            .codeSize = g_spv[p].size,
            .pCode = g_spv[p].code,
        };
        rlvk_handle module = 0;
        r = v->vkCreateShaderModule(v->device, &sm, NULL, &module);
        if (r != RLVK_SUCCESS)
            return fail("vkCreateShaderModule", r);
        rlvk_compute_pipeline_create_info cp = {
            .sType = RLVK_ST_COMPUTE_PIPELINE_CREATE_INFO,
            .stage = {
                .sType = RLVK_ST_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = RLVK_SHADER_STAGE_COMPUTE_BIT,
                .module = module,
                .pName = "main",
            },
            .layout = v->layout,
            .basePipelineIndex = -1,
        };
        r = v->vkCreateComputePipelines(v->device, 0, 1, &cp, NULL, &v->pipe[p]);
        v->vkDestroyShaderModule(v->device, module, NULL);
        if (r != RLVK_SUCCESS)
            return fail("vkCreateComputePipelines", r);
    }
    rlvk_descriptor_pool_size ps = { .type = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = NBIND };
    rlvk_descriptor_pool_create_info dp = {
        .sType = RLVK_ST_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &ps,
    };
    r = v->vkCreateDescriptorPool(v->device, &dp, NULL, &v->dpool);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateDescriptorPool", r);
    rlvk_descriptor_set_allocate_info da = {
        .sType = RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = v->dpool,
        .descriptorSetCount = 1,
        .pSetLayouts = &v->set_layout,
    };
    r = v->vkAllocateDescriptorSets(v->device, &da, &v->set);
    if (r != RLVK_SUCCESS)
        return fail("vkAllocateDescriptorSets", r);
    rlvk_command_pool_create_info cpi = {
        .sType = RLVK_ST_COMMAND_POOL_CREATE_INFO,
        .flags = RLVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = v->family,
    };
    r = v->vkCreateCommandPool(v->device, &cpi, NULL, &v->pool);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateCommandPool", r);
    rlvk_command_buffer_allocate_info ca = {
        .sType = RLVK_ST_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = v->pool,
        .level = RLVK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    r = v->vkAllocateCommandBuffers(v->device, &ca, &v->cmd);
    if (r != RLVK_SUCCESS)
        return fail("vkAllocateCommandBuffers", r);
    rlvk_fence_create_info fi = { .sType = RLVK_ST_FENCE_CREATE_INFO };
    r = v->vkCreateFence(v->device, &fi, NULL, &v->fence);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateFence", r);
    return true;
}

/** @brief Copies data (n words) into a new device buffer through a host buffer, and waits. */
static bool upload(vk *v, buf *dst, const uint32_t *data, size_t n)
{
    buf stage = { 0 };
    rlvk_size bytes = (rlvk_size)n * 4;
    bool ok = buf_make(v, dst, bytes, RLVK_BUFFER_USAGE_STORAGE_BUFFER_BIT | RLVK_BUFFER_USAGE_TRANSFER_DST_BIT, false,
                       false) &&
              buf_make(v, &stage, bytes, RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, false);
    if (ok) {
        memcpy(stage.map, data, (size_t)bytes);
        rlvk_mapped_memory_range range = { .sType = RLVK_ST_MAPPED_MEMORY_RANGE, .memory = stage.memory, .size = ~(rlvk_size)0 };
        if (!stage.coherent)
            v->vkFlushMappedMemoryRanges(v->device, 1, &range);
        rlvk_command_buffer_begin_info bi = {
            .sType = RLVK_ST_COMMAND_BUFFER_BEGIN_INFO,
            .flags = RLVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        rlvk_buffer_copy copy = { .size = bytes };
        rlvk_submit_info sub = { .sType = RLVK_ST_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &v->cmd };
        ok = v->vkBeginCommandBuffer(v->cmd, &bi) == RLVK_SUCCESS;
        if (ok) {
            v->vkCmdCopyBuffer(v->cmd, stage.buffer, dst->buffer, 1, &copy);
            ok = v->vkEndCommandBuffer(v->cmd) == RLVK_SUCCESS &&
                 v->vkQueueSubmit(v->queue, 1, &sub, v->fence) == RLVK_SUCCESS &&
                 v->vkWaitForFences(v->device, 1, &v->fence, 1, ~(uint64_t)0) == RLVK_SUCCESS;
            v->vkResetFences(v->device, 1, &v->fence);
        }
        if (!ok)
            fail("uploading SMAA's textures", 0);
    }
    buf_free(v, &stage);
    return ok;
}

/** @brief SMAA's area and search textures, one word per texel, in device buffers. */
static bool upload_luts(vk *v)
{
    enum { AREA = sizeof rl_post_smaa_area / 2, SEARCH = sizeof rl_post_smaa_search };
    uint32_t *words = malloc(AREA * sizeof *words);
    if (!words)
        return fail("allocation", 0);
    for (size_t i = 0; i < AREA; i++)
        words[i] = (uint32_t)rl_post_smaa_area[2 * i] | (uint32_t)rl_post_smaa_area[2 * i + 1] << 8;
    bool ok = upload(v, &v->b[B_AREA], words, AREA);
    for (size_t i = 0; ok && i < SEARCH; i++)
        words[i] = rl_post_smaa_search[i];
    ok = ok && upload(v, &v->b[B_SEARCH], words, SEARCH);
    free(words);
    return ok;
}

static bool start(vk *v)
{
    rlvk_gipa_fn gipa = (rlvk_gipa_fn)rl_hostlib_vulkan_gipa();
    if (!gipa)
        return fail("loading Vulkan", 0);
    return make_instance(v, gipa) && pick_device(v, gipa) && make_device(v) && make_pipelines(v) && upload_luts(v);
}

/**
 * @brief Buffers for a sw by sh picture in and a w by h picture after the upscaler (the anti-aliasing size), and the
 * descriptor set that names them.
 */
static bool size_buffers(vk *v, uint32_t sw, uint32_t sh, uint32_t w, uint32_t h)
{
    if (v->sw == sw && v->sh == sh && v->w == w && v->h == h)
        return true;
    v->sw = v->sh = v->w = v->h = 0;
    rlvk_size src = (rlvk_size)sw * sh * 4, bytes = (rlvk_size)w * h * 4;
    const uint32_t storage = RLVK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const uint32_t both = RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT | RLVK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!buf_make(v, &v->b[B_SRC], src, storage | both, false, false) ||
        !buf_make(v, &v->b[B_IN], bytes, storage | both, false, false) ||
        !buf_make(v, &v->b[B_OUT], bytes, storage | RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT, false, false) ||
        !buf_make(v, &v->b[B_EDGES], bytes, storage, false, false) ||
        !buf_make(v, &v->b[B_BLEND], bytes, storage, false, false) ||
        !buf_make(v, &v->b[B_STAGE_IN], src, RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, false) ||
        !buf_make(v, &v->b[B_STAGE_OUT], bytes, RLVK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true))
        return false;
    static const int bound[NBIND] = { B_IN, B_OUT, B_EDGES, B_BLEND, B_AREA, B_SEARCH };
    rlvk_descriptor_buffer_info info[NBIND];
    rlvk_write_descriptor_set wr[NBIND];
    for (uint32_t i = 0; i < NBIND; i++) {
        info[i] = (rlvk_descriptor_buffer_info){ .buffer = v->b[bound[i]].buffer, .range = v->b[bound[i]].size };
        wr[i] = (rlvk_write_descriptor_set){
            .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
            .dstSet = v->set,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &info[i],
        };
    }
    v->vkUpdateDescriptorSets(v->device, NBIND, wr, 0, NULL);
    v->sw = sw;
    v->sh = sh;
    v->w = w;
    v->h = h;
    return true;
}

static void barrier(vk *v, uint32_t src_stage, uint32_t src, uint32_t dst_stage, uint32_t dst)
{
    rlvk_memory_barrier mb = { .sType = RLVK_ST_MEMORY_BARRIER, .srcAccessMask = src, .dstAccessMask = dst };
    v->vkCmdPipelineBarrier(v->cmd, src_stage, dst_stage, 0, 1, &mb, 0, NULL, 0, NULL);
}

static void dispatch(vk *v, int pipe, const params *pc)
{
    v->vkCmdBindPipeline(v->cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, v->pipe[pipe]);
    v->vkCmdPushConstants(v->cmd, v->layout, RLVK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *pc, pc);
    v->vkCmdDispatch(v->cmd, (v->w + GROUP - 1) / GROUP, (v->h + GROUP - 1) / GROUP, 1);
}

static void pack(uint32_t *dst, const uint8_t *rgb, size_t n)
{
    for (size_t i = 0; i < n; i++, rgb += 3)
        dst[i] = (uint32_t)rgb[0] | (uint32_t)rgb[1] << 8 | (uint32_t)rgb[2] << 16 | 0xff000000u;
}

static void unpack(uint8_t *rgb, const uint32_t *src, size_t n)
{
    for (size_t i = 0; i < n; i++, rgb += 3) {
        rgb[0] = (uint8_t)src[i];
        rgb[1] = (uint8_t)(src[i] >> 8);
        rgb[2] = (uint8_t)(src[i] >> 16);
    }
}

/** @brief Records SMAA 1x at a quality preset (picture.aa_quality): edges, blending weights, neighborhood blending. */
static void record_smaa(vk *v, params pc, uint32_t quality)
{
    quality = quality < 4 ? quality : 2;
    pc.threshold = g_smaa[quality].threshold;
    pc.max_search_steps = g_smaa[quality].steps;
    pc.max_search_steps_diag = g_smaa[quality].diag_steps;
    pc.corner_rounding = g_smaa[quality].corner;
    const uint32_t cs = RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const uint32_t w = RLVK_ACCESS_SHADER_WRITE_BIT, r = RLVK_ACCESS_SHADER_READ_BIT;
    dispatch(v, P_SMAA_EDGES, &pc);
    barrier(v, cs, w, cs, r);
    dispatch(v, g_smaa[quality].fast ? P_SMAA_WEIGHTS_FAST : P_SMAA_WEIGHTS, &pc);
    barrier(v, cs, w, cs, r);
    dispatch(v, P_SMAA_BLEND, &pc);
}

/** @brief Starts the upscaler on the pipeline's device the first time it is needed; false when it is off. */
static bool upscaler_ready(vk *v)
{
    if (!g_up)
        return false;
    if (!g_up_started) {
        g_up_started = true;
        rl_post_device d = {
            .instance = v->instance,
            .physical_device = v->phys,
            .device = v->device,
            .queue = v->queue,
            .queue_family = v->family,
            .get_device_proc_addr = (void *)(uintptr_t)v->vkGetDeviceProcAddr,
        };
        g_up_ok = g_up->start(g_up_user, &d);
        if (!g_up_ok)
            rl_log(RL_LOG_WARN, "post", "the upscaler %s did not start; the picture is shown at its own size", g_up->name);
    }
    return g_up_ok;
}

/**
 * @brief Runs the upscaler (when up) and the passes of aa on sw by sh RGB888 pixels into out, which is w by h (the
 * upscaler's output size).
 */
static bool run(vk *v, bool up, uint32_t aa, const uint8_t *rgb, uint8_t *out, uint32_t sw, uint32_t sh, uint32_t w,
                uint32_t h)
{
    if (!size_buffers(v, sw, sh, w, h))
        return false;
    size_t n = (size_t)w * h;
    buf *si = &v->b[B_STAGE_IN], *so = &v->b[B_STAGE_OUT];
    pack(si->map, rgb, (size_t)sw * sh);
    rlvk_mapped_memory_range in_range = { .sType = RLVK_ST_MAPPED_MEMORY_RANGE, .memory = si->memory, .size = ~(rlvk_size)0 };
    if (!si->coherent)
        v->vkFlushMappedMemoryRanges(v->device, 1, &in_range);

    rlvk_command_buffer_begin_info bi = {
        .sType = RLVK_ST_COMMAND_BUFFER_BEGIN_INFO,
        .flags = RLVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    rlvk_result r = v->vkBeginCommandBuffer(v->cmd, &bi);
    if (r != RLVK_SUCCESS)
        return fail("vkBeginCommandBuffer", r);
    const uint32_t xfer = RLVK_PIPELINE_STAGE_TRANSFER_BIT, cs = RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const uint32_t any_read = RLVK_ACCESS_SHADER_READ_BIT | RLVK_ACCESS_TRANSFER_READ_BIT;
    const uint32_t any_write = RLVK_ACCESS_SHADER_WRITE_BIT | RLVK_ACCESS_TRANSFER_WRITE_BIT;
    rlvk_buffer_copy in_copy = { .size = (rlvk_size)sw * sh * 4 }, copy = { .size = (rlvk_size)n * 4 };
    v->vkCmdCopyBuffer(v->cmd, si->buffer, v->b[up ? B_SRC : B_IN].buffer, 1, &in_copy);
    barrier(v, xfer, RLVK_ACCESS_TRANSFER_WRITE_BIT, xfer | cs, any_read);
    if (up) {
        g_up->record(g_up_user, v->cmd, v->b[B_SRC].buffer, v->b[B_IN].buffer, sw, sh, w, h);
        barrier(v, xfer | cs, any_write, xfer | cs, any_read);
    }
    if (aa != RL_POST_AA_OFF) {
        v->vkCmdBindDescriptorSets(v->cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, v->layout, 0, 1, &v->set, 0, NULL);
        params pc = { .width = (int32_t)w, .height = (int32_t)h };
        if (aa == RL_POST_AA_FXAA)
            dispatch(v, P_FXAA, &pc);
        else
            record_smaa(v, pc, rl_setting_enum("picture.aa_quality"));
        barrier(v, cs, RLVK_ACCESS_SHADER_WRITE_BIT, xfer, RLVK_ACCESS_TRANSFER_READ_BIT);
    }
    v->vkCmdCopyBuffer(v->cmd, v->b[aa != RL_POST_AA_OFF ? B_OUT : B_IN].buffer, so->buffer, 1, &copy);
    barrier(v, RLVK_PIPELINE_STAGE_TRANSFER_BIT, RLVK_ACCESS_TRANSFER_WRITE_BIT, RLVK_PIPELINE_STAGE_HOST_BIT,
            RLVK_ACCESS_HOST_READ_BIT);
    r = v->vkEndCommandBuffer(v->cmd);
    if (r != RLVK_SUCCESS)
        return fail("vkEndCommandBuffer", r);

    rlvk_submit_info sub = { .sType = RLVK_ST_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &v->cmd };
    r = v->vkQueueSubmit(v->queue, 1, &sub, v->fence);
    if (r != RLVK_SUCCESS)
        return fail("vkQueueSubmit", r);
    r = v->vkWaitForFences(v->device, 1, &v->fence, 1, ~(uint64_t)0);
    v->vkResetFences(v->device, 1, &v->fence);
    if (r != RLVK_SUCCESS)
        return fail("vkWaitForFences", r);
    rlvk_mapped_memory_range out_range = { .sType = RLVK_ST_MAPPED_MEMORY_RANGE, .memory = so->memory, .size = ~(rlvk_size)0 };
    if (!so->coherent)
        v->vkInvalidateMappedMemoryRanges(v->device, 1, &out_range);
    unpack(out, so->map, n);
    return true;
}

void rl_post_set_upscaler(const rl_post_stage *stage, void *user)
{
    pthread_mutex_lock(&g_lock);
    __atomic_store_n(&g_up, stage, __ATOMIC_RELEASE);
    g_up_user = user;
    g_up_started = g_up_ok = false;
    pthread_mutex_unlock(&g_lock);
}

const uint8_t *rl_post_apply(const uint8_t *rgb, uint32_t width, uint32_t height, uint32_t *out_width,
                             uint32_t *out_height)
{
    rl_settings_declare_runtime();
    *out_width = width;
    *out_height = height;
    uint32_t aa = rl_setting_enum("picture.aa");
    if ((aa == RL_POST_AA_OFF && !__atomic_load_n(&g_up, __ATOMIC_ACQUIRE)) || !width || !height)
        return rgb;
    const uint8_t *shown = rgb;
    pthread_mutex_lock(&g_lock);
    if (!g_tried) {
        g_tried = true;
        g_ok = start(&g_vk);
    }
    uint32_t w = width, h = height;
    bool up = g_ok && upscaler_ready(&g_vk) && g_up->plan(g_up_user, width, height, &w, &h) && w && h;
    if (!up) {
        w = width;
        h = height;
    }
    if (!up && aa == RL_POST_AA_OFF) {
        pthread_mutex_unlock(&g_lock);
        return rgb;
    }
    size_t bytes = (size_t)w * h * 3;
    if (g_ok && bytes > g_out_cap) {
        uint8_t *p = realloc(g_out, bytes);
        if (p) {
            g_out = p;
            g_out_cap = bytes;
        } else {
            g_ok = fail("allocation", 0);
        }
    }
    if (g_ok)
        g_ok = run(&g_vk, up, aa, rgb, g_out, width, height, w, h);
    if (g_ok) {
        shown = g_out;
        *out_width = w;
        *out_height = h;
    }
    pthread_mutex_unlock(&g_lock);
    return shown;
}
