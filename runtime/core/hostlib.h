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

#include <stdbool.h>
#include <stddef.h>

/** @brief Opens the first library of a NULL-terminated name list that loads; NULL when none does. */
RL_RUNTIME_API void *rl_hostlib_open(const char *const *names);
/**
 * @brief Finds the first library of a NULL-terminated name list that the process already has loaded, without loading
 * one (a tool that injects itself, such as a capture layer); NULL when none is loaded.
 */
RL_RUNTIME_API void *rl_hostlib_loaded(const char *const *names);
/** @brief Looks up a symbol in a library from rl_hostlib_open; NULL when absent. */
RL_RUNTIME_API void *rl_hostlib_sym(void *lib, const char *name);

/** @brief Environment variable naming the Vulkan loader library to use instead of the host default. */
#define RL_VULKAN_LIBRARY_ENV "RELIFT_VULKAN_LIBRARY"

/**
 * @brief The host Vulkan loader's vkGetInstanceProcAddr, or NULL when no
 * loader is installed. On Android, in a build with libadrenotools
 * (RL_ADRENOTOOLS), video.gpu_driver may name an installed driver package
 * (gpu_driver.h): the first call that sees one loads it once, through a
 * second instance of the system loader, and every later call returns that
 * loader; a package that does not load falls back to the system driver.
 * Otherwise tries RELIFT_VULKAN_LIBRARY, then, in a build linked
 * against a Vulkan implementation (RL_VULKAN_LINKED, MoltenVK on Apple
 * hosts), the linked one, then the host's default names (libvulkan.so.1 on
 * Linux, libvulkan.1.dylib and libMoltenVK.dylib on macOS, vulkan-1.dll on
 * Windows). The library stays loaded.
 */
RL_RUNTIME_API void *rl_hostlib_vulkan_gipa(void);

/** @brief The driver rl_hostlib_vulkan_gipa loaded: "system", the package name, or NULL before the first call. */
RL_RUNTIME_API const char *rl_hostlib_gpu_driver_loaded(void);

/**
 * @brief Calls hook with the package name when the package video.gpu_driver names does not load and the system
 * driver is used instead. It runs once, on the thread of the first rl_hostlib_vulkan_gipa call, which waits for it.
 */
RL_RUNTIME_API void rl_hostlib_on_gpu_driver_failed(void (*hook)(const char *name));

#if defined(__ANDROID__)
#include <relift/window.h>

/** @brief The number of displays besides the app's own that can hold a window (host/android_display.c). */
RL_RUNTIME_API uint32_t rl_android_display_count(void);
/** @brief Opens a window on display index of those rl_android_display_count counts; NULL when it cannot. */
RL_RUNTIME_API rl_window *rl_android_display_open(uint32_t index);
/**
 * @brief Makes a VkSurfaceKHR on host Vulkan instance for an ANativeWindow (vkCreateAndroidSurfaceKHR); returns a
 * VkResult, VK_ERROR_INITIALIZATION_FAILED without a window or the function.
 */
RL_RUNTIME_API int32_t rl_android_vulkan_surface(void *native_window, void *instance, uint64_t *surface);

/**
 * @brief Loads a driver (dir and library: the package folder and its library file name; dir NULL: the system
 * driver), makes a Vulkan instance and reads its first GPU. True with six lines in out (deviceName, vendorID,
 * driverVersion, apiVersion, driverName, driverInfo); false with the reason. Meant for a process of its own: the
 * driver stays loaded.
 */
RL_RUNTIME_API bool rl_hostlib_vulkan_probe(const char *dir, const char *library, char *out, size_t size);
#endif

#endif
