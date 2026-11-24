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
    rlvk_handle instance, phys, device;
    /** @brief The queue the core's own users (the presenter, the picture pipeline) submit to, and the one a guest layer submits to; one queue when the family has only one, else two. */
    rlvk_handle queue, guest_queue;
    uint32_t family, queue_count;
    char name[256];
    rlvk_memory_properties mem;
    /** @brief The instance has VK_KHR_surface and the backend's surface extensions, the device VK_KHR_swapchain. */
    /** @brief The API version the instance was made for. */
    uint32_t instance_api;
    bool surfaces;
    /** @brief VK_KHR_present_id and VK_KHR_present_wait are on, and vkWaitForPresentKHR is loaded: a present chained with a present id can be waited for until the display shows it. */
    bool present_wait;
    /**
     * @brief VK_EXT_debug_utils is on because a capture tool (RenderDoc) is in the process: a layer may label its
     * commands and name its objects for the tool. Off otherwise, so plain runs make no label calls.
     */
    bool debug_utils;
    rlvk_result(RLVK_CALL *vkWaitForPresentKHR)(rlvk_handle device, rlvk_handle swapchain, uint64_t id, uint64_t timeout);
} rlvk_dev;

/** @brief A buffer and its memory; map is set for a host-visible one. */
typedef struct rlvk_buf {
    rlvk_handle buffer, memory;
    void *map;
    rlvk_size size;
    bool coherent;
} rlvk_buf;

/** @brief What a requirement hook sees and fills while the core makes the device. */
typedef struct rl_vkdev_build {
    rlvk_gipa_fn gipa;
    rlvk_handle instance, phys;
    uint32_t family;
    /** @brief The device extensions enabled so far (add with rl_vkdev_add_ext). */
    const char **exts;
    uint32_t nexts, max_exts;
    /** @brief The pNext chain of the VkDeviceCreateInfo (a hook links its feature structs in front). */
    const void *pnext;
} rl_vkdev_build;

/**
 * @brief Links the pNext chain that starts at head (structs that each begin with sType and pNext) in front of b's chain:
 * the last struct of head's chain then points at what b->pnext held, and b->pnext at head.
 */
void rl_vkdev_chain(rl_vkdev_build *b, void *head);

/** @brief Called while the device is made, with the physical device chosen; false stops the device (the log names who). */
typedef bool rl_vkdev_hook(void *user, rl_vkdev_build *b);

/**
 * @brief Registers what a user of the device needs: the Vulkan API version of the instance, queue flags the family
 * must have, and a hook that adds device extensions and features. Guest layers call it at startup; the core makes
 * one device with the union on the first rl_vkdev call by anyone. A call after the device exists logs who came too
 * late and aborts, so the order is fixed instead of papered over.
 */
void rl_vkdev_require(const char *who, uint32_t api_version, uint32_t queue_flags, rl_vkdev_hook *hook, void *user);
/** @brief Enables a device extension from a hook; false when the list is full. */
bool rl_vkdev_add_ext(rl_vkdev_build *b, const char *name);
/** @brief Takes the guest queue for a submit (the core's queue lock when the guest shares that queue). */
void rl_vkdev_guest_queue_lock(void);
/** @brief Gives it back. */
void rl_vkdev_guest_queue_unlock(void);

/**
 * @brief The device, made the first time it is asked for on any thread; NULL when Vulkan cannot start (the log says
 * why, once).
 */
const rlvk_dev *rl_vkdev(void);
/** @brief Takes the queue for vkQueueSubmit or vkQueuePresentKHR. */
void rl_vkdev_queue_lock(void);
/** @brief Gives the queue back. */
void rl_vkdev_queue_unlock(void);
/**
 * @brief vkDeviceWaitIdle with every queue of the device held, as Vulkan requires: the guest queue, then the core's.
 * Callers hold neither lock.
 */
void rl_vkdev_wait_idle(const rlvk_dev *d);
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
