/**
 * @file vkdev.h
 * @brief The core's one Vulkan device, shared by the picture pipeline
 * (post.c) and the window presenter (present.c): an instance with the window
 * backend's surface extensions when a backend is up, the most capable
 * physical device with a graphics and compute queue, and a device with
 * VK_KHR_swapchain when the instance can make surfaces. Every user records
 * into its own command buffers and submits to the one queue under
 * rl_vkdev_queue_lock.
 */
#ifndef RELIFT_CORE_VKDEV_H
#define RELIFT_CORE_VKDEV_H

#include "vkmin.h"

#include <stdbool.h>

/** @brief The device and the functions loaded for it. */
typedef struct rlvk_dev {
#define RLVK_DEV_FIELD(name, ret, args) ret(RLVK_CALL *name) args;
    RLVK_INSTANCE_FNS(RLVK_DEV_FIELD)
    RLVK_DEVICE_FNS(RLVK_DEV_FIELD)
    RLVK_SURFACE_FNS(RLVK_DEV_FIELD)
    RLVK_SWAPCHAIN_FNS(RLVK_DEV_FIELD)
#undef RLVK_DEV_FIELD
    /** @brief The loader's vkGetInstanceProcAddr. */
    rlvk_gipa_fn gipa;
    rlvk_handle instance, phys, device, queue;
    uint32_t family;
    rlvk_memory_properties mem;
    /** @brief The instance has VK_KHR_surface and the backend's surface extensions, the device VK_KHR_swapchain. */
    bool surfaces;
} rlvk_dev;

/** @brief A buffer and its memory; map is set for a host-visible one. */
typedef struct rlvk_buf {
    rlvk_handle buffer, memory;
    void *map;
    rlvk_size size;
    bool coherent;
} rlvk_buf;

/**
 * @brief The device, made the first time it is asked for on any thread; NULL when Vulkan cannot start (the log says
 * why, once).
 */
const rlvk_dev *rl_vkdev(void);
/** @brief Takes the queue for vkQueueSubmit or vkQueuePresentKHR. */
void rl_vkdev_queue_lock(void);
/** @brief Gives the queue back. */
void rl_vkdev_queue_unlock(void);
/** @brief A memory type in bits with every flag of want, preferring one that also has every flag of nice; -1 if none. */
int rl_vkdev_memory_type(const rlvk_dev *d, uint32_t bits, uint32_t want, uint32_t nice);
/** @brief Makes b (freeing what it held): device-local, or host-visible and mapped (cached when asked). */
bool rl_vkdev_buf_make(const rlvk_dev *d, rlvk_buf *b, rlvk_size size, uint32_t usage, bool host, bool cached);
/** @brief Frees b and zeroes it. */
void rl_vkdev_buf_free(const rlvk_dev *d, rlvk_buf *b);
/** @brief Makes host writes to a mapped b visible to the device. */
void rl_vkdev_buf_flush(const rlvk_dev *d, const rlvk_buf *b);
/** @brief Makes device writes to a mapped b visible to the host. */
void rl_vkdev_buf_invalidate(const rlvk_dev *d, const rlvk_buf *b);

#endif
