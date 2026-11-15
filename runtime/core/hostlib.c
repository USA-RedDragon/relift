/**
 * @file hostlib.c
 * @brief Run-time loading of host libraries (hostlib.h).
 */
#include "hostlib.h"
#include "core.h"
#include "gpu_driver.h"

#include <relift/settings.h>

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

static const char *selected_name(void)
{
    rl_settings_declare_runtime();
    const char *name = rl_setting_str(RL_GPU_DRIVER_KEY);
    return name && *name && strcmp(name, RL_GPU_DRIVER_SYSTEM) ? name : NULL;
}

#if defined(__ANDROID__) && defined(RL_ADRENOTOOLS)
#include <adrenotools/driver.h>
#include <pthread.h>

static void *g_custom_gipa;

static void *open_custom(const rl_gpu_driver *d)
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
    void *lib = adrenotools_open_libvulkan(RTLD_NOW, ADRENOTOOLS_DRIVER_CUSTOM, NULL, hooks, d->dir, d->library, NULL,
                                           NULL);
    if (!lib) {
        rl_log(RL_LOG_ERROR, "hostlib", "driver package %s (%s) did not load; using the system driver", d->name,
               d->library);
        return NULL;
    }
    void *gipa = rl_hostlib_sym(lib, "vkGetInstanceProcAddr");
    rl_log(gipa ? RL_LOG_INFO : RL_LOG_ERROR, "hostlib", "Vulkan driver: package %s (%s)%s", d->name, d->library,
           gipa ? "" : " has no vkGetInstanceProcAddr; using the system driver");
    return gipa;
}

static void *custom_gipa(void)
{
    static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    static bool tried;
    pthread_mutex_lock(&lock);
    if (!tried) {
        rl_gpu_driver d;
        const char *name = selected_name();
        if (name) {
            tried = true;
            if (rl_gpu_driver_find(name, &d))
                g_custom_gipa = open_custom(&d);
        }
    }
    void *gipa = g_custom_gipa;
    pthread_mutex_unlock(&lock);
    return gipa;
}
#else
static void *custom_gipa(void)
{
    static int warned;
    const char *name = selected_name();
    if (name && !__atomic_exchange_n(&warned, 1, __ATOMIC_RELAXED))
        rl_log(RL_LOG_WARN, "hostlib", "%s=%s: this build loads only the system Vulkan driver", RL_GPU_DRIVER_KEY,
               name);
    return NULL;
}
#endif

void *rl_hostlib_vulkan_gipa(void)
{
    void *custom = custom_gipa();
    if (custom)
        return custom;
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
