/**
 * @file hostlib.c
 * @brief Run-time loading of host libraries (hostlib.h).
 */
#include "hostlib.h"
#include "core.h"
#include "gpu_driver.h"

#include <relift/settings.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>

static void *open_one(const char *name) { return (void *)LoadLibraryA(name); }

static void *sym_one(void *lib, const char *name) { return (void *)(uintptr_t)GetProcAddress((HMODULE)lib, name); }

static const char *const vulkan_names[] = { "vulkan-1.dll", NULL };
#else
#include <dlfcn.h>

static void *open_one(const char *name) { return dlopen(name, RTLD_NOW | RTLD_LOCAL); }

static void *sym_one(void *lib, const char *name) { return dlsym(lib, name); }

#if defined(__APPLE__)
static const char *const vulkan_names[] = { "libvulkan.1.dylib", "libMoltenVK.dylib", NULL };
#else
static const char *const vulkan_names[] = { "libvulkan.so.1", "libvulkan.so", NULL };
#endif
#endif

void *rl_hostlib_open(const char *const *names)
{
    for (; names && *names; names++) {
        void *lib = open_one(*names);
        if (lib)
            return lib;
    }
    return NULL;
}

void *rl_hostlib_sym(void *lib, const char *name) { return lib ? sym_one(lib, name) : NULL; }

#if defined(RL_VULKAN_LINKED)
void *vkGetInstanceProcAddr(void *instance, const char *name);
#endif

static char g_loaded[RL_GPU_DRIVER_NAME_MAX];
static char g_failed[RL_GPU_DRIVER_NAME_MAX];
static void (*g_failed_hook)(const char *name);

void rl_hostlib_on_gpu_driver_failed(void (*hook)(const char *name)) { g_failed_hook = hook; }

const char *rl_hostlib_gpu_driver_loaded(void) { return g_loaded[0] ? g_loaded : NULL; }

static const char *selected_name(void)
{
    rl_settings_declare_runtime();
    const char *name = rl_setting_str(RL_GPU_DRIVER_KEY);
    return name && *name && strcmp(name, RL_GPU_DRIVER_SYSTEM) ? name : NULL;
}

#if defined(__ANDROID__)
#include <vulkan/vulkan.h>

static bool describe(PFN_vkGetInstanceProcAddr gipa, char *out, size_t size);

#endif

#if defined(__ANDROID__) && defined(RL_ADRENOTOOLS)
#include <adrenotools/driver.h>
#include <pthread.h>

/** @brief The driverName line of a describe() answer. */
static const char *driver_name(const char *d)
{
    for (int i = 0; i < 4 && d; i++) {
        d = strchr(d, '\n');
        d = d ? d + 1 : NULL;
    }
    return d ? d : "";
}

static size_t line_len(const char *s)
{
    const char *e = strchr(s, '\n');
    return e ? (size_t)(e - s) : strlen(s);
}

static void *g_custom_gipa;

static void *open_package(const rl_gpu_driver *d)
{
    Dl_info self;
    if (!dladdr((void *)(uintptr_t)rl_hostlib_vulkan_gipa, &self) || !self.dli_fname)
        return NULL;
    char hooks[1024];
    const char *slash = strrchr(self.dli_fname, '/');
    size_t n = slash ? (size_t)(slash - self.dli_fname) + 1 : 0;
    if (n >= sizeof hooks)
        return NULL;
    memcpy(hooks, self.dli_fname, n);
    hooks[n] = 0;
    return adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hooks, d->dir, d->library, NULL,
                                      NULL);
}

static void *open_custom(const rl_gpu_driver *d)
{
    void *lib = open_package(d);
    if (!lib) {
        rl_log(RL_LOG_ERROR, "hostlib", "driver package %s (%s) did not load; using the system driver", d->name,
               d->library);
        return NULL;
    }
    void *gipa = rl_hostlib_sym(lib, "vkGetInstanceProcAddr");
    if (!gipa) {
        rl_log(RL_LOG_ERROR, "hostlib", "Vulkan driver: package %s (%s) has no vkGetInstanceProcAddr; using the system driver",
               d->name, d->library);
        return NULL;
    }
    char got[1024], sys[1024];
    if (!describe((PFN_vkGetInstanceProcAddr)gipa, got, sizeof got)) {
        rl_log(RL_LOG_ERROR, "hostlib", "Vulkan driver: package %s (%s) did not start (%s); using the system driver",
               d->name, d->library, got);
        return NULL;
    }
    void *syslib = rl_hostlib_open(vulkan_names);
    void *sysgipa = rl_hostlib_sym(syslib, "vkGetInstanceProcAddr");
    const char *a = driver_name(got), *b = "";
    if (sysgipa && describe((PFN_vkGetInstanceProcAddr)sysgipa, sys, sizeof sys) &&
        (b = driver_name(sys), line_len(a) == line_len(b)) && !strncmp(a, b, line_len(a))) {
        rl_log(RL_LOG_ERROR, "hostlib",
               "Vulkan driver: package %s (%s) answered as the system driver; using the system driver", d->name,
               d->library);
        return NULL;
    }
    rl_log(RL_LOG_INFO, "hostlib", "Vulkan driver: package %s (%s), %.*s", d->name, d->library, (int)line_len(a), a);
    return gipa;
}

static void *custom_gipa(void)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static bool tried;
    bool failed = false;
    pthread_mutex_lock(&lock);
    if (!tried) {
        rl_gpu_driver d;
        const char *name = selected_name();
        if (name) {
            tried = true;
            if (rl_gpu_driver_find(name, &d))
                g_custom_gipa = open_custom(&d);
            if (g_custom_gipa) {
                snprintf(g_loaded, sizeof g_loaded, "%s", name);
            } else {
                snprintf(g_failed, sizeof g_failed, "%s", name);
                failed = true;
            }
        }
    }
    void *gipa = g_custom_gipa;
    void (*hook)(const char *) = g_failed_hook;
    pthread_mutex_unlock(&lock);
    if (failed && hook)
        hook(g_failed);
    return gipa;
}
#else
static void *custom_gipa(void)
{
    static int warned;
    const char *name = selected_name();
    if (name && !__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED)) {
        rl_log(RL_LOG_WARN, "hostlib", "%s=%s: this build loads only the system Vulkan driver", RL_GPU_DRIVER_KEY,
               name);
        snprintf(g_failed, sizeof g_failed, "%s", name);
        if (g_failed_hook)
            g_failed_hook(g_failed);
    }
    return NULL;
}
#endif

void *rl_hostlib_vulkan_gipa(void)
{
    void *custom = custom_gipa();
    if (custom)
        return custom;
    if (!g_loaded[0])
        snprintf(g_loaded, sizeof g_loaded, "%s", RL_GPU_DRIVER_SYSTEM);
    const char *env = getenv(RL_VULKAN_LIBRARY_ENV);
    const char *const one[] = { env, NULL };
#if defined(RL_VULKAN_LINKED)
    if (!env || !*env)
        return (void *)(uintptr_t)vkGetInstanceProcAddr;
#endif
    void *lib = rl_hostlib_open(env && *env ? one : vulkan_names);
    if (!lib) {
        rl_log(RL_LOG_WARN, "hostlib", "no Vulkan loader found%s%s", env ? " at " : "", env ? env : "");
        return NULL;
    }
    return rl_hostlib_sym(lib, "vkGetInstanceProcAddr");
}

#if defined(__ANDROID__)
#include <vulkan/vulkan.h>

static bool describe(PFN_vkGetInstanceProcAddr gipa, char *out, size_t size)
{
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    if (!create) {
        snprintf(out, size, "no vkCreateInstance");
        return false;
    }
    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "relift driver check",
        .apiVersion = VK_API_VERSION_1_1,
    };
    VkInstanceCreateInfo info = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
    VkInstance inst = VK_NULL_HANDLE;
    VkResult r = create(&info, NULL, &inst);
    if (r != VK_SUCCESS) {
        snprintf(out, size, "vkCreateInstance returned %d", (int)r);
        return false;
    }
    PFN_vkDestroyInstance destroy = (PFN_vkDestroyInstance)gipa(inst, "vkDestroyInstance");
    PFN_vkEnumeratePhysicalDevices list = (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties2 props2 =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
    VkPhysicalDevice dev = VK_NULL_HANDLE;
    uint32_t n = 1;
    bool ok = false;
    if (!list || !props2) {
        snprintf(out, size, "the instance has no device functions");
    } else if ((r = list(inst, &n, &dev)) < 0 || n == 0) {
        snprintf(out, size, "no GPU (vkEnumeratePhysicalDevices returned %d, %u devices)", (int)r, n);
    } else {
        VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
        VkPhysicalDeviceProperties2 p = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
        props2(dev, &p);
        snprintf(out, size, "%s\n%u\n%u\n%u\n%s\n%s", p.properties.deviceName, p.properties.vendorID,
                 p.properties.driverVersion, p.properties.apiVersion, drv.driverName, drv.driverInfo);
        ok = true;
    }
    if (destroy)
        destroy(inst, NULL);
    return ok;
}

bool rl_hostlib_vulkan_probe(const char *dir, const char *library, char *out, size_t size)
{
    void *lib = NULL;
    if (!dir) {
        lib = rl_hostlib_open(vulkan_names);
    } else {
#if defined(RL_ADRENOTOOLS)
        rl_gpu_driver d;
        memset(&d, 0, sizeof d);
        snprintf(d.name, sizeof d.name, "check");
        snprintf(d.dir, sizeof d.dir, "%s%s", dir, dir[0] && dir[strlen(dir) - 1] == '/' ? "" : "/");
        snprintf(d.library, sizeof d.library, "%s", library ? library : "");
        lib = open_package(&d);
#else
        (void)library;
        snprintf(out, size, "this build loads only the system driver");
        return false;
#endif
    }
    if (!lib) {
        const char *e = dlerror();
        snprintf(out, size, "the driver library did not load%s%s", e ? ": " : "", e ? e : "");
        return false;
    }
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)rl_hostlib_sym(lib, "vkGetInstanceProcAddr");
    if (!gipa) {
        snprintf(out, size, "the driver has no vkGetInstanceProcAddr");
        return false;
    }
    return describe(gipa, out, size);
}
#endif
