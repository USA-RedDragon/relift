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
 * are replaced by newer ones. The present mode follows the video.present_mode setting (rl_present_mode_choose).
 */
#ifndef RELIFT_CORE_PRESENT_H
#define RELIFT_CORE_PRESENT_H

#include <relift/window.h>

#include <stdbool.h>
#include <stdint.h>

typedef struct rl_present rl_present;

/** @brief Choices of the runtime's `video.present_mode` setting, as rl_setting_enum returns them. */
enum {
    RL_PRESENT_MODE_AUTO,
    RL_PRESENT_MODE_FIFO,
    RL_PRESENT_MODE_RELAXED,
    RL_PRESENT_MODE_LATEST_READY,
    RL_PRESENT_MODE_MAILBOX,
    RL_PRESENT_MODE_IMMEDIATE
};

/**
 * @brief The VkPresentModeKHR for a swapchain: the one the video.present_mode choice names when the surface offers
 * it, else what auto picks. Auto takes base (a mode the caller wants when nothing else is asked, FIFO or MAILBOX)
 * when offered, else FIFO, which every surface has. avail holds the n modes the surface offers.
 */
int32_t rl_present_mode_choose(uint32_t choice, int32_t base, const int32_t *avail, uint32_t n);

/** @brief A VkPresentModeKHR's name for logs. */
const char *rl_present_mode_name(int32_t mode);

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
 * @brief What a window lays over its pictures (the on-screen controller), asked on the presenter's thread for each
 * frame and while nothing new comes, so the overlay never takes the picture off the GPU. place gives where the picture
 * sits in view pixels (x, y, width, height), or false to let the presenter place it; pixels moves the overlay's time
 * on to now_ns, draws it when it changed and returns it (premultiplied RGBA8, the view's size, rows packed) with the
 * changed rectangle in dirty (empty when nothing changed), or NULL when it shows nothing.
 */
typedef struct rl_present_overlay {
    bool (*place)(void *user, int32_t rect[4]);
    const uint8_t *(*pixels)(void *user, uint64_t now_ns, uint32_t *width, uint32_t *height, int32_t dirty[4]);
    void *user;
} rl_present_overlay;
/** @brief Lays ov over every frame from now on (NULL: none). The presenter keeps a copy of ov. */
void rl_present_set_overlay(rl_present *p, const rl_present_overlay *ov);
/** @brief The overlay's blend of one pixel, as the GPU pass does it: premultiplied over, frame and result as RGBA8 words. */
uint32_t rl_present_overlay_blend(uint32_t frame, uint32_t over);

/** @brief A presenter-owned image a GPU guest layer fills with its picture (rl_present_slot_begin). */
typedef struct rl_present_slot {
    /** @brief VkImage: RGBA8, TRANSFER_SRC and TRANSFER_DST usage, same device and queue family as the core's. */
    uint64_t image;
    /** @brief Its VkFormat (R8G8B8A8_UNORM or _SRGB, as the swapchain is encoded): copy the guest picture's bytes into it. */
    int32_t format;
    uint32_t index;
} rl_present_slot;

/**
 * @brief Sets the hook called with the time (rl_clock_ns) each present of any presenter was shown on the display,
 * where the device can wait for presents (VK_KHR_present_wait); a guest layer locks its vsync timeline to it. NULL
 * clears it.
 */
void rl_present_set_shown_hook(void (*fn)(int64_t shown_ns));

/**
 * @brief Takes a free slot image of width by height for a picture that stays on the GPU. The caller records and runs
 * its copy into slot->image (from UNDEFINED, through TRANSFER_DST, ending in TRANSFER_SRC_OPTIMAL), waits until the
 * GPU has finished it, then calls rl_present_slot_end; rl_present_slot_abort gives the slot back unused. False when
 * the presenter has no free slot within a second or cannot make the image. The guest's thread never waits on the
 * swapchain: the newest ready slot replaces an older one the presenter has not taken yet.
 */
bool rl_present_slot_begin(rl_present *p, uint32_t width, uint32_t height, rl_present_slot *out);
/** @brief Hands the filled slot to the presenter, as a picture shown shown_w by shown_h square pixels. */
void rl_present_slot_end(rl_present *p, const rl_present_slot *s, uint32_t shown_w, uint32_t shown_h);
/**
 * @brief The presenter's GPU time with gpu.profile on, from its timestamps: the sum since the start in ticks of the
 * device's timestamp clock (the caller scales them by its timestampPeriod), and the presents timed.
 */
void rl_present_gpu_stats(uint64_t *ticks, uint64_t *presents);
/**
 * @brief rl_present_slot_end for a copy that is submitted but not finished: the presenter waits for timeline
 * semaphore (a VkSemaphore of the core's device) to reach value before it reads the slot, so the guest's thread does
 * not wait for its frame. Its GPU work waits, and the submission that signals value must already be on the queue; or,
 * with host_signaled (the value comes from vkSignalSemaphore, maybe later), the presenter's thread waits on the CPU
 * before it submits, so no submission on the queue waits for a signal not yet made.
 */
void rl_present_slot_end_after(rl_present *p, const rl_present_slot *s, uint32_t shown_w, uint32_t shown_h,
                               uint64_t semaphore, uint64_t value, bool host_signaled);
/** @brief How slot pictures (the backend's own way, as rl_window_set_fit says) fill the window: to fit, or by whole multiples. */
void rl_present_set_fit(rl_present *p, bool fit);
/**
 * @brief Where the presenter last drew its picture in the window's pixels, and that picture's size in the guest's
 * pixels (for mapping a pointer to a frame pixel); false before the first frame.
 */
bool rl_present_shown(rl_present *p, rl_present_rect *at, uint32_t *frame_w, uint32_t *frame_h);
/**
 * @brief The window's presenter, which its backend makes with the first ask; NULL when the window has none (a
 * backend that draws frames itself, or a host with no Vulkan swapchain). Guest GPU layers hand their pictures to it
 * with rl_present_slot_begin.
 */
rl_present *rl_window_presenter(rl_window *w);
/** @brief Gives a slot back that was not filled. */
void rl_present_slot_abort(rl_present *p, const rl_present_slot *s);

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
/**
 * @brief Whether present.capture saves draw number draw (counting from 1), which shows handed-over frame number frame:
 * every `every`th draw, and with a window [from, to) (to not 0) only draws of frames from `from` to before `to`, so
 * the first and every redraw of those frames are kept.
 */
bool rl_present_capture_due(uint64_t draw, uint64_t frame, uint64_t every, uint64_t from, uint64_t to);
/**
 * @brief The picture the window shows now, upright (the window's own orientation, whatever turn the swapchain
 * takes), as RGBA8 rows of *width words; waits at most timeout_ms. A malloc'd buffer the caller frees, or NULL.
 */
uint8_t *rl_present_snapshot(rl_present *p, uint32_t timeout_ms, uint32_t *width, uint32_t *height);
/**
 * @brief Shows rgb (RGB888, width by height, upright, drawn to fill the window) in place of the guest's frames,
 * which keep coming and are dropped, until a call with NULL goes back to them. Each call with a picture replaces
 * the one shown; the presenter copies it.
 */
void rl_present_still(rl_present *p, const uint8_t *rgb, uint32_t width, uint32_t height);

#endif
