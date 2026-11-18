/**
 * @file present.h
 * @brief Shows frames in a host window through a Vulkan swapchain of its own on the core's device (vkdev.h), for
 * windows a backend makes Vulkan surfaces for but draws nothing into itself (the windows on other displays of a
 * phone or a dual-screen handheld).
 *
 * Each presenter runs a host thread: rl_present_frame copies the frame into a one-frame mailbox and returns, and the
 * thread uploads the newest frame, scales it into the swapchain image by picture.filter (rl_post_integer and
 * rl_post_sampling: nearest, bilinear, or sharp bilinear, which is nearest to a whole multiple, then bilinear) and
 * presents. The thread
 * alone waits on the swapchain, so a slow or blocked display never holds the caller; frames it cannot show in time
 * are replaced by newer ones. The present mode is MAILBOX where the surface has it, else FIFO;
 * RELIFT_PRESENT_MODE=fifo or mailbox picks one (a measuring aid).
 */
#ifndef RELIFT_CORE_PRESENT_H
#define RELIFT_CORE_PRESENT_H

#include <relift/window.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct rl_present rl_present;

/** @brief Where a picture sits in a window, in the window's pixels. */
typedef struct rl_present_rect {
    uint32_t x, y, w, h;
} rl_present_rect;

/** @brief What a presenter has done, for logs and tests. */
typedef struct rl_present_stats {
    /** @brief Frames presented. */
    uint64_t presented;
    /** @brief Frames replaced in the mailbox by a newer one before the thread took them. */
    uint64_t replaced;
    /** @brief Swapchains made (the first one, then one per resize, rotation or new surface). */
    uint64_t swapchains;
    /** @brief The longest time between two presents, and their sum, in nanoseconds. */
    uint64_t max_gap_ns, sum_gap_ns;
    /** @brief The longest time from taking a frame to presenting it, and their sum, in nanoseconds. */
    uint64_t max_work_ns, sum_work_ns;
    /** @brief The swapchain's present mode (VkPresentModeKHR) and size; -1 and zeros without one. */
    int32_t mode;
    uint32_t width, height;
} rl_present_stats;

/**
 * @brief Where a shown_w by shown_h picture sits in a win_w by win_h window: scaled by the largest whole multiple
 * that fits when integer (when even 1 does not fit, as when not integer) or else to fill the window with the shape
 * kept, centered.
 */
void rl_present_place(uint32_t shown_w, uint32_t shown_h, uint32_t win_w, uint32_t win_h, bool integer,
                      rl_present_rect *out);

/**
 * @brief Where rect in an lw by lh picture lands in the swapchain image when the image's content is turned by
 * transform (a VkSurfaceTransformFlagBitsKHR rotation, clockwise, as preTransform says the presenter turned it so
 * the display needs no turn of its own).
 */
void rl_present_turn(const rl_present_rect *rect, uint32_t lw, uint32_t lh, uint32_t transform, rl_present_rect *out);

/**
 * @brief Starts a presenter for w, whose backend makes its Vulkan surfaces (vulkan_surface). NULL when the core
 * device has no swapchains; the log says why.
 */
rl_present *rl_present_open(rl_window *w);
/** @brief Stops the presenter and frees its swapchain and surface; waits for its thread. */
void rl_present_close(rl_present *p);
/**
 * @brief Hands a frame of width by height RGB888 pixels (rows packed) to the presenter, shown as shown_w by
 * shown_h square pixels and scaled the way rl_post_integer and rl_post_sampling say for a window whose own way is fit. Copies the
 * pixels and returns at once. When place is not NULL it gets where the picture will sit in a win_w by win_h window.
 */
void rl_present_frame(rl_present *p, const uint8_t *rgb, uint32_t width, uint32_t height, uint32_t shown_w,
                      uint32_t shown_h, bool fit, uint32_t win_w, uint32_t win_h, rl_present_rect *place);
/**
 * @brief Tells the presenter whether the window has a surface to draw in. false (the host is taking the surface
 * away) returns only once the thread has destroyed its swapchain and Vulkan surface; true (a new surface, or the
 * same one at a new size) makes the thread make them again and show the last frame.
 */
void rl_present_surface(rl_present *p, bool have);
/** @brief Copies the presenter's counters. */
void rl_present_stats_get(rl_present *p, rl_present_stats *out);
/**
 * @brief Reads back the next image the presenter shows, as RGBA8 rows of *width words (a test aid); waits at most
 * timeout_ms. Returns a malloc'd buffer the caller frees, or NULL.
 */
uint8_t *rl_present_capture(rl_present *p, uint32_t timeout_ms, uint32_t *width, uint32_t *height);

#endif
