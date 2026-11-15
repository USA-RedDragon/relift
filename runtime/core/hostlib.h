/**
 * @file hostlib.h
 * @brief Host shared libraries the runtime loads at run time: a thin shim
 * over dlopen and LoadLibrary, plus the host Vulkan loader.
 *
 * Guest layers never call dlopen themselves, so the host-specific library
 * names and APIs live here, in one file.
 */
#ifndef RELIFT_HOSTLIB_H
#define RELIFT_HOSTLIB_H

#include <relift/abi_core.h>

/** @brief Opens the first library of a NULL-terminated name list that loads; NULL when none does. */
RL_RUNTIME_API void *rl_hostlib_open(const char *const *names);
/** @brief Looks up a symbol in a library from rl_hostlib_open; NULL when absent. */
RL_RUNTIME_API void *rl_hostlib_sym(void *lib, const char *name);

/** @brief Environment variable naming the Vulkan loader library to use instead of the host default. */
#define RL_VULKAN_LIBRARY_ENV "RELIFT_VULKAN_LIBRARY"

/**
 * @brief The host Vulkan loader's vkGetInstanceProcAddr, or NULL when no
 * loader is installed. Tries RELIFT_VULKAN_LIBRARY, then, in a build linked
 * against a Vulkan implementation (RL_VULKAN_LINKED, MoltenVK on Apple
 * hosts), the linked one, then the host's default names (libvulkan.so.1 on
 * Linux, libvulkan.1.dylib and libMoltenVK.dylib on macOS, vulkan-1.dll on
 * Windows). The library stays loaded.
 */
RL_RUNTIME_API void *rl_hostlib_vulkan_gipa(void);

#endif
