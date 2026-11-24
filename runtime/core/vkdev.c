/**
 * @file vkdev.c
 * @brief The core's shared Vulkan device (vkdev.h).
 */
#include "vkdev.h"

#include <relift/settings.h>
#include <relift/window.h>

#include "capture.h"
#include "core.h"
#include "gpu_floor.h"
#include "hostlib.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define MAX_INSTANCE_EXTS 48
#define MAX_WANTED_INSTANCE_EXTS 32

/** @brief VkPhysicalDeviceProperties up to the device name; rest covers the remainder of the real structure. */
typedef struct rlvk_device_properties {
    uint32_t apiVersion, driverVersion, vendorID, deviceID;
    int32_t deviceType;
    char deviceName[256];
    uint8_t pipelineCacheUUID[16];
    uint64_t rest[128];
} rlvk_device_properties;

typedef void(RLVK_CALL *rlvk_props_fn)(rlvk_handle, rlvk_device_properties *);

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_queue = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_guest_queue = PTHREAD_MUTEX_INITIALIZER;
static rlvk_dev g_dev;
static bool g_tried, g_ok;

#define MAX_REQUIRES 8
#define MAX_DEVICE_EXTS 160

typedef struct require {
    const char *who;
    uint32_t api, queue_flags;
    rl_vkdev_hook *hook;
    void *user;
} require;

static require g_req[MAX_REQUIRES];
static uint32_t g_nreq;
static bool g_shared_queue;
/** @brief Why a window backend that draws through Vulkan got no swapchains, for the warning; NULL when it did. */
static const char *g_no_surface;
/** @brief The instance was made with VK_EXT_debug_utils (rlvk_dev.debug_utils). */
static bool g_debug_utils;
static const char *g_want_inst[MAX_WANTED_INSTANCE_EXTS];
static uint32_t g_nwant_inst;
/** @brief The extension lists the instance and the device were made with (rlvk_dev points at them). */
static const char *g_inst_exts[MAX_INSTANCE_EXTS];
static const char *g_dev_exts[MAX_DEVICE_EXTS];

static bool fail(const char *what, rlvk_result r)
{
    rl_log(RL_LOG_WARN, "vulkan", "the core's Vulkan device is off: %s failed (VkResult %d)", what, (int)r);
    return false;
}

static int device_rank(int32_t type)
{
    static const int rank[] = { 4, 1, 0, 2, 3 };
    return type >= 0 && type <= 4 ? rank[type] : 5;
}

static bool has_device_ext(rlvk_dev *d, rlvk_handle phys, const char *name)
{
    uint32_t n = 0;
    if (d->vkEnumerateDeviceExtensionProperties(phys, NULL, &n, NULL) != RLVK_SUCCESS || !n)
        return false;
    rlvk_extension_properties *e = calloc(n, sizeof *e);
    if (!e)
        return false;
    bool found = false;
    if (d->vkEnumerateDeviceExtensionProperties(phys, NULL, &n, e) >= RLVK_SUCCESS)
        for (uint32_t i = 0; i < n && !found; i++)
            found = !strcmp(e[i].extensionName, name);
    free(e);
    return found;
}

static bool listed(const rlvk_extension_properties *e, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name))
            return true;
    return false;
}

/**
 * @brief The instance extensions to ask for: portability enumeration when the loader has it, and the window
 * backend's surface extensions when every one of them is there.
 */
static uint32_t instance_exts(rlvk_gipa_fn gipa, const char *out[MAX_INSTANCE_EXTS], bool *surfaces)
{
    uint32_t count = 0, n = 0;
    *surfaces = false;
    rlvk_instance_extensions_fn list = (rlvk_instance_extensions_fn)gipa(0, "vkEnumerateInstanceExtensionProperties");
    rlvk_extension_properties *e = NULL;
    if (list && list(NULL, &n, NULL) == RLVK_SUCCESS && n) {
        e = calloc(n, sizeof *e);
        if (!e || list(NULL, &n, e) < RLVK_SUCCESS)
            n = 0;
    }
    if (listed(e, n, "VK_KHR_portability_enumeration"))
        out[count++] = "VK_KHR_portability_enumeration";
    if (listed(e, n, "VK_EXT_debug_utils") && rl_capture_tool()) {
        out[count++] = "VK_EXT_debug_utils";
        g_debug_utils = true;
    }
    /* the 1.1 query functions on a 1.0 loader: Android's loader calls the driver's
       vkGetPhysicalDeviceImageFormatProperties2 inside vkGetPhysicalDeviceSurfaceFormatsKHR, and fills that entry only
       for a 1.1 instance or one with this extension */
    if (listed(e, n, "VK_KHR_get_physical_device_properties2"))
        out[count++] = "VK_KHR_get_physical_device_properties2";
    const rl_window_backend *b = rl_window_backend_current();
    const char *const *want = b && b->vulkan_extensions && b->vulkan_surface ? b->vulkan_extensions() : NULL;
    if (b && b->vulkan_surface && !(want && want[0]))
        g_no_surface = "the window backend could not load Vulkan or name its surface extensions";
    if (want && want[0]) {
        uint32_t first = count;
        bool all = true;
        for (size_t i = 0; want[i] && all; i++) {
            all = listed(e, n, want[i]) && count < MAX_INSTANCE_EXTS;
            if (all)
                out[count++] = want[i];
            else
                g_no_surface = want[i];
        }
        if (!all)
            count = first;
        *surfaces = all;
    }
    for (uint32_t w = 0; w < g_nwant_inst && count < MAX_INSTANCE_EXTS; w++) {
        bool have = false;
        for (uint32_t i = 0; i < count && !have; i++)
            have = !strcmp(out[i], g_want_inst[w]);
        if (!have && listed(e, n, g_want_inst[w]))
            out[count++] = g_want_inst[w];
    }
    free(e);
    return count;
}

static bool make_instance(rlvk_dev *d)
{
    rlvk_create_instance_fn create = (rlvk_create_instance_fn)d->gipa(0, "vkCreateInstance");
    if (!create)
        return fail("vkCreateInstance lookup", 0);
    uint32_t api = RLVK_API_VERSION_1_0;
    for (uint32_t i = 0; i < g_nreq; i++)
        api = g_req[i].api > api ? g_req[i].api : api;
    /* At least 1.1 where the loader has it: the device is queried with 1.1 calls (vkGetPhysicalDeviceFeatures2), and
       Android's loader fills its driver table's 1.1 entry points only for a 1.1 instance, while its own swapchain code
       calls them (vkGetPhysicalDeviceSurfaceFormatsKHR on a driver loaded beside the system one called a NULL one). */
    typedef rlvk_result(RLVK_CALL * version_fn)(uint32_t *);
    version_fn version = (version_fn)d->gipa(0, "vkEnumerateInstanceVersion");
    uint32_t have = 0;
    if (version && version(&have) == RLVK_SUCCESS && have >= RLVK_API_VERSION_1_1 && api < RLVK_API_VERSION_1_1)
        api = RLVK_API_VERSION_1_1;
    rlvk_application_info app = {
        .sType = RLVK_ST_APPLICATION_INFO,
        .pApplicationName = "relift",
        .pEngineName = "relift",
        .apiVersion = api,
    };
    d->instance_api = api;
    const char *exts[MAX_INSTANCE_EXTS];
    uint32_t n = instance_exts(d->gipa, exts, &d->surfaces);
    bool portability = n && !strcmp(exts[0], "VK_KHR_portability_enumeration");
    rlvk_instance_create_info ci = {
        .sType = RLVK_ST_INSTANCE_CREATE_INFO,
        .flags = portability ? RLVK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR : 0,
        .pApplicationInfo = &app,
        .enabledExtensionCount = n,
        .ppEnabledExtensionNames = exts,
    };
    rlvk_result r = create(&ci, NULL, &d->instance);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateInstance", r);
    for (uint32_t i = 0; i < n; i++)
        g_inst_exts[i] = exts[i];
    d->instance_exts = g_inst_exts;
    d->ninstance_exts = n;
#define RLVK_LOAD_I(name, ret, args)                                                                                 \
    d->name = (ret(RLVK_CALL *) args)d->gipa(d->instance, #name);                                                    \
    if (!d->name)                                                                                                    \
        return fail(#name " lookup", 0);
    RLVK_INSTANCE_FNS(RLVK_LOAD_I)
#undef RLVK_LOAD_I
#define RLVK_LOAD_S(name, ret, args)                                                                                 \
    d->name = (ret(RLVK_CALL *) args)d->gipa(d->instance, #name);                                                    \
    d->surfaces = d->surfaces && d->name;
    if (d->surfaces) {
        RLVK_SURFACE_FNS(RLVK_LOAD_S)
    }
#undef RLVK_LOAD_S
    return true;
}

static bool pick_device(rlvk_dev *d)
{
    rlvk_props_fn props = (rlvk_props_fn)d->gipa(d->instance, "vkGetPhysicalDeviceProperties");
    uint32_t n = 0;
    rlvk_result r = d->vkEnumeratePhysicalDevices(d->instance, &n, NULL);
    if (r != RLVK_SUCCESS || !n)
        return fail("vkEnumeratePhysicalDevices", r);
    rlvk_handle *devs = calloc(n, sizeof *devs);
    if (!devs)
        return fail("allocation", 0);
    r = d->vkEnumeratePhysicalDevices(d->instance, &n, devs);
    int best = 12;
    uint32_t need = RLVK_QUEUE_COMPUTE_BIT;
    for (uint32_t i = 0; i < g_nreq; i++)
        need |= g_req[i].queue_flags;
    rl_settings_declare_runtime();
    const char *want = rl_setting_str("vk.device");
    for (uint32_t i = 0; r >= RLVK_SUCCESS && i < n; i++) {
        uint32_t nf = 0;
        d->vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nf, NULL);
        rlvk_queue_family_properties fam[16];
        nf = nf > 16 ? 16 : nf;
        d->vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nf, fam);
        rlvk_device_properties p = { 0 };
        if (props)
            props(devs[i], &p);
        if (want && *want && !strstr(p.deviceName, want))
            continue;
        for (uint32_t f = 0; f < nf; f++) {
            if ((fam[f].queueFlags & need) != need || !fam[f].queueCount)
                continue;
            int rank = device_rank(p.deviceType) * 2 + !(fam[f].queueFlags & RLVK_QUEUE_GRAPHICS_BIT);
            if (rank < best) {
                best = rank;
                d->phys = devs[i];
                d->family = f;
                d->queue_count = fam[f].queueCount;
                memcpy(d->name, p.deviceName, sizeof d->name);
            }
        }
    }
    free(devs);
    if (!d->phys)
        return fail("finding a device with a compute queue", r);
    d->vkGetPhysicalDeviceMemoryProperties(d->phys, &d->mem);
    return true;
}

/** @brief A VkPhysicalDevicePresentIdFeaturesKHR or VkPhysicalDevicePresentWaitFeaturesKHR. */
typedef struct present_feature {
    int32_t sType;
    struct present_feature *next;
    uint32_t enabled;
} present_feature;

/** @brief A VkPhysicalDeviceFeatures2 with the 55 core feature flags left out of the way. */
typedef struct features2 {
    int32_t sType;
    void *next;
    uint32_t core[55];
} features2;

static present_feature g_present_wait_feat = { 1000248000, NULL, 1 };
static present_feature g_present_id_feat = { 1000294001, &g_present_wait_feat, 1 };

/** @brief Whether the device has both present extensions with their features, and adds them to b when so. */
static bool add_present_wait(rlvk_dev *d, rl_vkdev_build *b)
{
    if (!d->surfaces || !has_device_ext(d, d->phys, "VK_KHR_present_id") ||
        !has_device_ext(d, d->phys, "VK_KHR_present_wait"))
        return false;
    typedef void(RLVK_CALL * features2_fn)(rlvk_handle, features2 *);
    features2_fn get = (features2_fn)d->gipa(d->instance, "vkGetPhysicalDeviceFeatures2");
    if (!get)
        get = (features2_fn)d->gipa(d->instance, "vkGetPhysicalDeviceFeatures2KHR");
    if (!get)
        return false;
    present_feature wait = { 1000248000, NULL, 0 }, id = { 1000294001, &wait, 0 };
    features2 f = { .sType = 1000059000, .next = &id };
    get(d->phys, &f);
    if (!id.enabled || !wait.enabled)
        return false;
    if (!rl_vkdev_add_ext(b, "VK_KHR_present_id") || !rl_vkdev_add_ext(b, "VK_KHR_present_wait"))
        return false;
    bool have_id = false, have_wait = false;
    for (const present_feature *l = b->pnext; l; l = l->next) {
        have_id = have_id || l->sType == g_present_id_feat.sType;
        have_wait = have_wait || l->sType == g_present_wait_feat.sType;
    }
    if (have_id != have_wait)
        return false;
    if (!have_id)
        rl_vkdev_chain(b, &g_present_id_feat);
    return true;
}

static bool make_device(rlvk_dev *d)
{
    float prio[2] = { 1.0f, 1.0f };
    uint32_t queues = d->queue_count > 1 ? 2 : 1;
    rlvk_device_queue_create_info q = {
        .sType = RLVK_ST_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = d->family,
        .queueCount = queues,
        .pQueuePriorities = prio,
    };
    const char *exts[MAX_DEVICE_EXTS];
    uint32_t n = 0;
    if (has_device_ext(d, d->phys, "VK_KHR_portability_subset"))
        exts[n++] = "VK_KHR_portability_subset";
    d->surfaces = d->surfaces && has_device_ext(d, d->phys, "VK_KHR_swapchain");
    if (d->surfaces)
        exts[n++] = "VK_KHR_swapchain";
    rl_vkdev_build b = { .gipa = d->gipa, .instance = d->instance, .phys = d->phys, .family = d->family };
    b.exts = exts;
    b.nexts = n;
    b.max_exts = MAX_DEVICE_EXTS;
    for (uint32_t i = 0; i < g_nreq; i++)
        if (g_req[i].hook && !g_req[i].hook(g_req[i].user, &b))
            return fail(g_req[i].who, 0);
    bool present_wait = add_present_wait(d, &b);
    rl_gpu_floor_build(d, &b);
    rlvk_device_create_info ci = {
        .sType = RLVK_ST_DEVICE_CREATE_INFO,
        .pNext = b.pnext,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &q,
        .enabledExtensionCount = b.nexts,
        .ppEnabledExtensionNames = exts,
    };
    rlvk_result r = d->vkCreateDevice(d->phys, &ci, NULL, &d->device);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateDevice", r);
    for (uint32_t i = 0; i < b.nexts; i++)
        g_dev_exts[i] = exts[i];
    d->device_exts = g_dev_exts;
    d->ndevice_exts = b.nexts;
    d->device_pnext = b.pnext;
#define RLVK_LOAD_D(name, ret, args)                                                                                 \
    d->name = (ret(RLVK_CALL *) args)d->vkGetDeviceProcAddr(d->device, #name);                                       \
    if (!d->name)                                                                                                    \
        return fail(#name " lookup", 0);
    RLVK_DEVICE_FNS(RLVK_LOAD_D)
#undef RLVK_LOAD_D
#define RLVK_LOAD_W(name, ret, args)                                                                                 \
    d->name = (ret(RLVK_CALL *) args)d->vkGetDeviceProcAddr(d->device, #name);                                       \
    d->surfaces = d->surfaces && d->name;
    if (d->surfaces) {
        RLVK_SWAPCHAIN_FNS(RLVK_LOAD_W)
    }
#undef RLVK_LOAD_W
    d->vkWaitSemaphores = (rlvk_result(RLVK_CALL *)(rlvk_handle, const void *, uint64_t))d->vkGetDeviceProcAddr(
        d->device, "vkWaitSemaphores");
    if (present_wait) {
        d->vkWaitForPresentKHR = (rlvk_result(RLVK_CALL *)(rlvk_handle, rlvk_handle, uint64_t, uint64_t))
            d->vkGetDeviceProcAddr(d->device, "vkWaitForPresentKHR");
        d->present_wait = d->vkWaitForPresentKHR != NULL;
    }
    d->vkGetDeviceQueue(d->device, d->family, 0, &d->guest_queue);
    d->queue = d->guest_queue;
    if (queues > 1)
        d->vkGetDeviceQueue(d->device, d->family, 1, &d->queue);
    g_shared_queue = d->queue == d->guest_queue;
    rl_gpu_floor_start(d);
    return true;
}

void rl_vkdev_require(const char *who, uint32_t api_version, uint32_t queue_flags, rl_vkdev_hook *hook, void *user)
{
    pthread_mutex_lock(&g_lock);
    if (g_tried || g_nreq == MAX_REQUIRES) {
        rl_log(RL_LOG_ERROR, "vulkan", "%s asked for Vulkan device requirements %s", who,
               g_tried ? "after the device was made: it came too late, so it must register before the first use of "
                         "the device (guest layers at startup)"
                       : "but too many layers registered");
        pthread_mutex_unlock(&g_lock);
        abort();
    }
    g_req[g_nreq++] = (require){ who, api_version, queue_flags, hook, user };
    pthread_mutex_unlock(&g_lock);
}

void rl_vkdev_want_instance_ext(const char *who, const char *name)
{
    pthread_mutex_lock(&g_lock);
    if (g_tried || g_nwant_inst == MAX_WANTED_INSTANCE_EXTS) {
        rl_log(RL_LOG_ERROR, "vulkan", "%s asked for the instance extension %s %s", who, name,
               g_tried ? "after the device was made: it came too late" : "but too many were asked for");
        pthread_mutex_unlock(&g_lock);
        abort();
    }
    g_want_inst[g_nwant_inst++] = name;
    pthread_mutex_unlock(&g_lock);
}

bool rl_vkdev_made(void)
{
    pthread_mutex_lock(&g_lock);
    bool t = g_tried;
    pthread_mutex_unlock(&g_lock);
    return t;
}

void rl_vkdev_chain(rl_vkdev_build *b, void *head)
{
    if (!head)
        return;
    struct link {
        int32_t stype;
        struct link *next;
    } *l = head;
    while (l->next)
        l = l->next;
    l->next = (struct link *)(uintptr_t)b->pnext;
    b->pnext = head;
}

bool rl_vkdev_add_ext(rl_vkdev_build *b, const char *name)
{
    for (uint32_t i = 0; i < b->nexts; i++)
        if (!strcmp(b->exts[i], name))
            return true;
    if (b->nexts == b->max_exts)
        return false;
    b->exts[b->nexts++] = name;
    return true;
}

void rl_vkdev_guest_queue_lock(void)
{
    rl_gpu_floor_work();
    pthread_mutex_lock(g_shared_queue ? &g_queue : &g_guest_queue);
}

void rl_vkdev_guest_queue_unlock(void) { pthread_mutex_unlock(g_shared_queue ? &g_queue : &g_guest_queue); }

void rl_vkdev_wait_idle(const rlvk_dev *d)
{
    if (!g_shared_queue)
        pthread_mutex_lock(&g_guest_queue);
    pthread_mutex_lock(&g_queue);
    d->vkDeviceWaitIdle(d->device);
    pthread_mutex_unlock(&g_queue);
    if (!g_shared_queue)
        pthread_mutex_unlock(&g_guest_queue);
}

const rlvk_dev *rl_vkdev(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_tried) {
        g_tried = true;
        g_dev.gipa = (rlvk_gipa_fn)rl_hostlib_vulkan_gipa();
        g_ok = g_dev.gipa ? make_instance(&g_dev) && pick_device(&g_dev) && make_device(&g_dev) : fail("loading Vulkan", 0);
        if (g_ok)
            rl_log(RL_LOG_INFO, "vulkan", "core device up on %s, queue family %u, %u queue%s, %s", g_dev.name, g_dev.family,
                   g_shared_queue ? 1u : 2u, g_shared_queue ? "" : "s", g_dev.surfaces ? "with swapchains" : "without swapchains");
        g_dev.debug_utils = g_ok && g_debug_utils;
        if (g_dev.debug_utils)
            rl_log(RL_LOG_INFO, "vulkan", "a capture tool is loaded: VK_EXT_debug_utils on, for command labels and object names");
        if (g_ok && !g_dev.surfaces && g_no_surface)
            rl_log(RL_LOG_WARN, "vulkan",
                   "NO SWAPCHAINS: the window shows frames copied through host memory, many times slower than a "
                   "swapchain, because the Vulkan instance lacks %s%s",
                   g_no_surface,
                   strstr(g_no_surface, "wayland") ? " (RenderDoc's layer hides it: run under X11 with DISPLAY set)" : "");
        else if (g_ok && !g_dev.surfaces && rl_window_backend_current() && rl_window_backend_current()->vulkan_surface)
            rl_log(RL_LOG_WARN, "vulkan",
                   "NO SWAPCHAINS: the window shows frames copied through host memory, many times slower than a "
                   "swapchain, because the device has no VK_KHR_swapchain");
    }
    const rlvk_dev *d = g_ok ? &g_dev : NULL;
    pthread_mutex_unlock(&g_lock);
    return d;
}

void rl_vkdev_queue_lock(void) { pthread_mutex_lock(&g_queue); }

void rl_vkdev_queue_unlock(void) { pthread_mutex_unlock(&g_queue); }

int rl_vkdev_memory_type(const rlvk_dev *d, uint32_t bits, uint32_t want, uint32_t nice)
{
    int found = -1;
    for (uint32_t i = 0; i < d->mem.memoryTypeCount; i++) {
        uint32_t f = d->mem.memoryTypes[i].propertyFlags;
        if (!(bits & (1u << i)) || (f & want) != want)
            continue;
        if ((f & nice) == nice)
            return (int)i;
        if (found < 0)
            found = (int)i;
    }
    return found;
}

void rl_vkdev_buf_free(const rlvk_dev *d, rlvk_buf *b)
{
    if (b->map)
        d->vkUnmapMemory(d->device, b->memory);
    if (b->buffer)
        d->vkDestroyBuffer(d->device, b->buffer, NULL);
    if (b->memory)
        d->vkFreeMemory(d->device, b->memory, NULL);
    memset(b, 0, sizeof *b);
}

bool rl_vkdev_buf_make(const rlvk_dev *d, rlvk_buf *b, rlvk_size size, uint32_t usage, bool host, bool cached)
{
    rl_vkdev_buf_free(d, b);
    rlvk_buffer_create_info ci = { .sType = RLVK_ST_BUFFER_CREATE_INFO, .size = size, .usage = usage };
    rlvk_result r = d->vkCreateBuffer(d->device, &ci, NULL, &b->buffer);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateBuffer", r);
    rlvk_memory_requirements req;
    d->vkGetBufferMemoryRequirements(d->device, b->buffer, &req);
    uint32_t want = host ? RLVK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0;
    uint32_t nice = host ? RLVK_MEMORY_PROPERTY_HOST_COHERENT_BIT | (cached ? RLVK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0)
                         : RLVK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    int type = rl_vkdev_memory_type(d, req.memoryTypeBits, want, nice);
    if (type < 0)
        return fail("finding a memory type", 0);
    rlvk_memory_allocate_info ai = {
        .sType = RLVK_ST_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    r = d->vkAllocateMemory(d->device, &ai, NULL, &b->memory);
    if (r != RLVK_SUCCESS)
        return fail("vkAllocateMemory", r);
    r = d->vkBindBufferMemory(d->device, b->buffer, b->memory, 0);
    if (r != RLVK_SUCCESS)
        return fail("vkBindBufferMemory", r);
    b->size = size;
    b->coherent = (d->mem.memoryTypes[type].propertyFlags & RLVK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
    if (host) {
        r = d->vkMapMemory(d->device, b->memory, 0, ~(rlvk_size)0, 0, &b->map);
        if (r != RLVK_SUCCESS)
            return fail("vkMapMemory", r);
    }
    return true;
}

void rl_vkdev_buf_flush(const rlvk_dev *d, const rlvk_buf *b)
{
    rlvk_mapped_memory_range range = { .sType = RLVK_ST_MAPPED_MEMORY_RANGE, .memory = b->memory, .size = ~(rlvk_size)0 };
    if (!b->coherent)
        d->vkFlushMappedMemoryRanges(d->device, 1, &range);
}

void rl_vkdev_buf_invalidate(const rlvk_dev *d, const rlvk_buf *b)
{
    rlvk_mapped_memory_range range = { .sType = RLVK_ST_MAPPED_MEMORY_RANGE, .memory = b->memory, .size = ~(rlvk_size)0 };
    if (!b->coherent)
        d->vkInvalidateMappedMemoryRanges(d->device, 1, &range);
}
