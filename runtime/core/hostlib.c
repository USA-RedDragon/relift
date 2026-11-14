/**
 * @file hostlib.c
 * @brief Run-time loading of host libraries (hostlib.h).
 */
#include "hostlib.h"
#include "core.h"

#include <stdlib.h>

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

void *rl_hostlib_vulkan_gipa(void)
{
    const char *env = getenv(RL_VULKAN_LIBRARY_ENV);
    const char *const one[] = { env, NULL };
    void *lib = rl_hostlib_open(env && *env ? one : vulkan_names);
    if (!lib) {
        rl_log(RL_LOG_WARN, "hostlib", "no Vulkan loader found%s%s", env ? " at " : "", env ? env : "");
        return NULL;
    }
    return rl_hostlib_sym(lib, "vkGetInstanceProcAddr");
}
