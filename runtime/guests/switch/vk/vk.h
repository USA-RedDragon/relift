/**
 * @file vk.h
 * @brief Vulkan forwarding for the Switch guest: guest Vulkan calls run on the
 * host's Vulkan driver.
 *
 * The guest gets guest addresses of thunks from vkGetInstanceProcAddr. Each
 * thunk (generated from vk.xml by relift-vkgen into gen/, or written by hand
 * in this folder) has the lifted-code signature and translates guest structs,
 * pointers, handles and pNext chains to host ones. Device memory the guest
 * maps lands inside the guest reservation: placed mapping
 * (VK_EXT_map_memory_placed), else host-pointer import
 * (VK_EXT_external_memory_host), else a shadow copy. The guest's
 * VK_NN_vi_surface is created by a surface provider (SDL3 in the player, a
 * headless surface in tests). See README.md in this folder.
 *
 * This header names no Vulkan type, so it can be included next to any other
 * Vulkan header (SDL_vulkan.h).
 */
#ifndef RELIFT_SWITCH_VK_H
#define RELIFT_SWITCH_VK_H

#include "core.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief How guest-mapped device memory reaches guest memory (rl_vk_config.map_mode). */
enum rl_vk_map_mode {
    /** @brief The best the device offers: placed, then import, then shadow. */
    RL_VK_MAP_AUTO = 0,
    /** @brief vkMapMemory2 with VK_MEMORY_MAP_PLACED_BIT_EXT at the guest address; unmap keeps the range reserved. */
    RL_VK_MAP_PLACED = 1,
    /** @brief Host-visible allocations are guest pages imported with VK_EXT_external_memory_host. */
    RL_VK_MAP_IMPORT = 2,
    /** @brief A guest copy of the host mapping, synchronized at flush, invalidate, submit and wait. */
    RL_VK_MAP_SHADOW = 3
};

/** @brief Environment variable that forces a map mode: placed, import or shadow. */
#define RL_VK_MAP_ENV "RELIFT_VK_MAP"

/**
 * @brief Creates host surfaces for the guest's VK_NN_vi_surface.
 *
 * Handles are passed as void * (dispatchable) and uint64_t (VkSurfaceKHR);
 * results are VkResult values.
 */
typedef struct rl_vk_surface_provider {
    /** @brief Host instance extensions the provider needs, NULL-terminated; added when the guest enables VK_NN_vi_surface. */
    const char *const *instance_extensions;
    /**
     * @brief Creates a host surface for the guest window (the guest address
     * of the nn::vi::NativeWindowHandle the guest passed). gipa is the host
     * vkGetInstanceProcAddr. Returns a VkResult.
     */
    int32_t (*create)(void *user, void *host_instance, void *gipa, uint64_t guest_window, uint64_t *surface);
    /** @brief Passed to create. */
    void *user;
} rl_vk_surface_provider;

/** @brief A provider that creates VK_EXT_headless_surface surfaces (tests, offscreen runs). */
RL_RUNTIME_API const rl_vk_surface_provider *rl_vk_headless_surfaces(void);

/** @brief Changes guest memory map state for the forwarding layer (the guest layer takes its own lock). */
typedef int rl_vk_vm_set_fn(void *user, uint64_t addr, uint64_t size, uint32_t perm, int content);

/** @brief Settings of the forwarding layer. */
typedef struct rl_vk_config {
    /** @brief Guest address of the thunk table: one 4-byte slot per command, rl_vk_thunk_bytes() in all; a range the guest never executes (for example inside the unlifted vulkan module). */
    uint64_t thunk_base;
    /** @brief The guest address space. */
    rl_vm *vm;
    /** @brief First guest address of the range that mapped device memory may use; free in the guest map. */
    uint64_t map_base;
    /** @brief Size of that range. */
    uint64_t map_size;
    /** @brief rl_vm tag given to mapped pages (the guest layer's memory state for them). */
    uint64_t map_tag;
    /** @brief enum rl_vk_map_mode; RELIFT_VK_MAP overrides it. */
    int map_mode;
    /** @brief Changes the guest map; NULL calls rl_vm_set on vm under the layer's own lock. */
    rl_vk_vm_set_fn *vm_set;
    /** @brief Passed to vm_set. */
    void *vm_user;
    /** @brief Creates surfaces for VK_NN_vi_surface; NULL when the host shows nothing. */
    const rl_vk_surface_provider *surfaces;
    /** @brief The host vkGetInstanceProcAddr; NULL loads the host Vulkan loader (rl_hostlib_vulkan_gipa). */
    void *gipa;
} rl_vk_config;

/** @brief Starts the layer: loads the host loader and builds the thunk unit. Returns 0, or -1 when no host Vulkan exists. */
RL_RUNTIME_API int rl_vk_init(const rl_vk_config *cfg);
/** @brief Destroys what the guest left alive and releases the layer. */
RL_RUNTIME_API void rl_vk_shutdown(void);
/** @brief The unit of thunks at cfg.thunk_base, for rl_units_register. */
RL_RUNTIME_API const rl_unit *rl_vk_unit(void);
/** @brief Bytes of guest address space the thunk table covers. */
RL_RUNTIME_API uint64_t rl_vk_thunk_bytes(void);
/** @brief The thunk for a Vulkan symbol a guest module imports (vkGetInstanceProcAddr), or NULL. */
RL_RUNTIME_API rl_fn rl_vk_import(const char *symbol);
/** @brief Guest address of the thunk for a command name, 0 when the layer has none. */
RL_RUNTIME_API uint64_t rl_vk_guest_addr(const char *name);
/** @brief Number of live guest-visible dispatchable handles (tests). */
RL_RUNTIME_API uint32_t rl_vk_live_handles(void);
/** @brief The map mode the device behind a guest VkDevice handle uses, or -1. */
RL_RUNTIME_API int rl_vk_device_map_mode(uint64_t guest_device);

#endif
