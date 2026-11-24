/**
 * @file vk_overlay.h
 * @brief The on-screen controller drawn over host swapchain images
 * (vk_overlay.c).
 */
#ifndef RELIFT_SWITCH_VK_OVERLAY_H
#define RELIFT_SWITCH_VK_OVERLAY_H

#include "vkf.h"

/** @brief Most host swapchain images an overlay draws on. */
#define VKF_OVERLAY_HOST_MAX 16u
/** @brief Staging buffers, one per guest swapchain image (the caller's slot). */
#define VKF_OVERLAY_SLOTS 8u

typedef struct vkf_overlay vkf_overlay;

/** @brief Makes the pipeline and framebuffers for the n host images of a swapchain; NULL when the host cannot. */
vkf_overlay *vkf_overlay_new(VkDevice dev, const vkf_fns *fns, const vkf_dev *d, VkFormat format, VkExtent2D extent,
                             const VkImage *imgs, uint32_t n);
/** @brief Frees an overlay; the device must be idle for its images. NULL is allowed. */
void vkf_overlay_free(vkf_overlay *o);
/**
 * @brief Records the overlay over target (host image host_index, in TRANSFER_DST_OPTIMAL) and leaves it in
 * PRESENT_SRC_KHR. slot names the staging buffer, free once the previous submission of this command buffer is done.
 * Returns false and records nothing when there is nothing to draw.
 */
bool vkf_overlay_record(vkf_overlay *o, VkCommandBuffer cb, uint32_t host_index, uint32_t slot, VkImage target);

#endif
