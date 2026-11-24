/**
 * @file present.c
 * @brief The window presenter (present.h).
 *
 * One frame in flight per presenter: the thread waits for the previous
 * frame's fence, acquires a swapchain image, packs the frame into a host
 * buffer, copies it into a source image, clears the swapchain image to black
 * and blits the source into the picture's place by rl_post_sampling
 * (through a prescale image for sharp bilinear), then presents. Each swapchain image has its own
 * semaphore for the present to wait on.
 */
#include "capture.h"
#include "flashwatch.h"
#include "png.h"
#include "present.h"

#include <relift/post.h>
#include <relift/settings.h>

#include "core.h"
#include "gpu_floor.h"
#include "post_data.h"
#include "postgpu.h"
#include "vkdev.h"

#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_IMAGES 8
#define SLOTS 3
#define IDLE_WAIT_MS 250
/** @brief How often an overlay is asked for changes while no frame comes (the pause sheet over a paused game). */
#define OVERLAY_POLL_MS 16
/**
 * @brief The shortest time between two draws for an overlay change alone: one refresh of a 60 Hz display. The
 * presenter does not know the panel's rate, and in mailbox mode nothing else holds it back, so an animating overlay
 * would redraw as fast as the GPU allows. New frames, resizes and captures are drawn at once.
 */
#define OVERLAY_MIN_NS 16666667ull
#define SLOT_WAIT_MS 1000
#define STATS_EVERY_NS 10000000000ull
#define QUARTER_TURNS (RLVK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR | RLVK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
#define ROTATIONS                                                                                                    \
    (RLVK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR | QUARTER_TURNS | RLVK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)

typedef struct image {
    rlvk_handle image, memory;
    uint32_t w, h;
    int32_t format;
} image;

enum { SLOT_FREE, SLOT_FILLING, SLOT_READY, SLOT_SHOWN };

typedef struct slot {
    image im;
    int state;
    /** @brief A timeline semaphore and value the slot's copy signals (rl_present_slot_end_after); 0 when finished. */
    rlvk_handle wait_sem;
    uint64_t wait_value;
    /** @brief The value comes from the host: the presenter waits for it on the CPU before its submit. */
    bool wait_cpu;
} slot;

struct rl_present {
    rl_window *win;
    const rlvk_dev *d;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;

    slot slots[SLOTS];
    int next_slot, cur_slot;
    bool fit;
    rl_present_rect shown_at;
    uint32_t shown_fw, shown_fh;
    uint32_t release_mask;
    bool gpu_frames;
    rl_post_gpu *post;
    bool post_failed;
    image pic;

    uint8_t *next;
    size_t next_cap;
    uint32_t next_w, next_h, next_sw, next_sh;
    bool next_fit, fresh, quit;
    bool surface_ok, holds, retry;
    rl_present_stats stats;
    bool capture_want;
    uint8_t *captured;
    uint32_t cap_w, cap_h;
    /** @brief Frames handed over (rl_present_frame, rl_present_slot_end) and swapchain images drawn, from 1. */
    uint64_t received, work_number, draws;
    /** @brief present.capture: its folder (empty: off), every how many draws, and the draw window [from, to). */
    char pc_dir[1024];
    uint64_t pc_every, pc_from, pc_to;
    /** @brief present.flash (flashwatch.h): every new frame read back and checked for a one-frame flash. */
    rl_flashwatch flash;
    /** @brief What the draw being captured did, for its log line (record fills it). */
    struct {
        const char *source;
        int slot;
        int32_t src_format;
        uint32_t fw, fh, cw, ch, sampling;
        rl_present_rect at;
        bool post, over, fresh;
    } drew;

    uint8_t *still_next, *still;
    size_t still_next_cap, still_cap;
    uint32_t still_next_w, still_next_h, still_w, still_h;
    bool still_on, still_fresh, still_shown;

    uint8_t *work;
    size_t work_cap;
    uint32_t work_w, work_h, work_sw, work_sh;
    bool work_fit, have_frame, create_failed, stale;
    uint64_t gen_seen;

    rlvk_handle surface, swapchain;
    rlvk_handle images[MAX_IMAGES], done[MAX_IMAGES];
    uint32_t nimages, sc_w, sc_h;
    uint32_t sc_transform, made_extent_w, made_extent_h, made_current;
    int32_t sc_format, mode;
    bool sc_readable;
    rlvk_handle pool, cmd, fence, acquired;
    rlvk_buf stage, readback;
    image src, pre;
    uint64_t last_present_ns, last_log_ns;
    /** @brief When the newest frame was handed over, and when the one being drawn was (rl_clock_ns). */
    uint64_t next_ready_ns, work_ready_ns;
    uint64_t present_ids;

    /** @brief The window's overlay (rl_present_set_overlay) and what its last pixels call gave. */
    rl_present_overlay ov;
    bool ov_on, ov_due, ov_upload_all;
    /** @brief When the presenter last drew (rl_clock_ns), for OVERLAY_MIN_NS. */
    uint64_t last_draw_ns;
    const uint8_t *ov_px;
    uint32_t ov_w, ov_h;
    /** @brief The rows changed since the last upload, first and one past the last. */
    uint32_t ov_y0, ov_y1;
    /** @brief The blend pass: the frame image and buffer, the overlay buffer, the pipeline and its set. */
    image frame;
    rlvk_buf ov_buf, fr_buf;
    rlvk_handle ov_set_layout, ov_layout, ov_pipe, ov_dpool, ov_set, ov_bound_fr, ov_bound_ov;
    bool ov_failed;
    /** @brief A present came back suboptimal with the surface unchanged (logged once). */
    bool suboptimal_seen;
};

/** @brief The blend pass's push constants (overlay.comp). */
typedef struct ov_params {
    uint32_t width, height, view_w, view_h, over_w, over_h, turn, swap_rb;
} ov_params;

static bool fail(const char *what, rlvk_result r)
{
    rl_log(RL_LOG_WARN, "present", "%s failed (VkResult %d)", what, (int)r);
    return false;
}

void rl_present_place(uint32_t shown_w, uint32_t shown_h, uint32_t win_w, uint32_t win_h, bool integer,
                      rl_present_rect *out)
{
    memset(out, 0, sizeof *out);
    if (!shown_w || !shown_h || !win_w || !win_h)
        return;
    uint64_t w, h;
    uint32_t k = win_w / shown_w < win_h / shown_h ? win_w / shown_w : win_h / shown_h;
    if (integer && k >= 1) {
        w = (uint64_t)shown_w * k;
        h = (uint64_t)shown_h * k;
    } else if ((uint64_t)win_w * shown_h <= (uint64_t)win_h * shown_w) {
        w = win_w;
        h = ((uint64_t)win_w * shown_h + shown_w / 2) / shown_w;
    } else {
        h = win_h;
        w = ((uint64_t)win_h * shown_w + shown_h / 2) / shown_h;
    }
    out->w = (uint32_t)(w < win_w ? w : win_w);
    out->h = (uint32_t)(h < win_h ? h : win_h);
    out->x = (win_w - out->w) / 2;
    out->y = (win_h - out->h) / 2;
}

void rl_present_turn(const rl_present_rect *in, uint32_t lw, uint32_t lh, uint32_t transform, rl_present_rect *out)
{
    if (transform & RLVK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR)
        *out = (rl_present_rect){ lh - in->y - in->h, in->x, in->h, in->w };
    else if (transform & RLVK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
        *out = (rl_present_rect){ in->y, lw - in->x - in->w, in->h, in->w };
    else if (transform & RLVK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)
        *out = (rl_present_rect){ lw - in->x - in->w, lh - in->y - in->h, in->w, in->h };
    else
        *out = *in;
}

static void image_free(rl_present *p, image *im)
{
    if (im->image)
        p->d->vkDestroyImage(p->d->device, im->image, NULL);
    if (im->memory)
        p->d->vkFreeMemory(p->d->device, im->memory, NULL);
    memset(im, 0, sizeof *im);
}

static bool image_make(rl_present *p, image *im, uint32_t w, uint32_t h, int32_t format)
{
    if (im->image && im->w == w && im->h == h && im->format == format)
        return true;
    image_free(p, im);
    const rlvk_dev *d = p->d;
    rlvk_image_create_info ci = {
        .sType = RLVK_ST_IMAGE_CREATE_INFO,
        .imageType = RLVK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = RLVK_SAMPLE_COUNT_1_BIT,
        .tiling = RLVK_IMAGE_TILING_OPTIMAL,
        .usage = RLVK_IMAGE_USAGE_TRANSFER_SRC_BIT | RLVK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .initialLayout = RLVK_IMAGE_LAYOUT_UNDEFINED,
    };
    rlvk_result r = d->vkCreateImage(d->device, &ci, NULL, &im->image);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateImage", r);
    rlvk_memory_requirements req;
    d->vkGetImageMemoryRequirements(d->device, im->image, &req);
    int type = rl_vkdev_memory_type(d, req.memoryTypeBits, 0, RLVK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type < 0)
        return fail("finding image memory", 0);
    rlvk_memory_allocate_info ai = {
        .sType = RLVK_ST_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = (uint32_t)type,
    };
    r = d->vkAllocateMemory(d->device, &ai, NULL, &im->memory);
    if (r == RLVK_SUCCESS)
        r = d->vkBindImageMemory(d->device, im->image, im->memory, 0);
    if (r != RLVK_SUCCESS)
        return fail("allocating image memory", r);
    im->w = w;
    im->h = h;
    im->format = format;
    return true;
}

static bool start_objects(rl_present *p)
{
    const rlvk_dev *d = p->d;
    rlvk_command_pool_create_info cpi = {
        .sType = RLVK_ST_COMMAND_POOL_CREATE_INFO,
        .flags = RLVK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = d->family,
    };
    rlvk_result r = d->vkCreateCommandPool(d->device, &cpi, NULL, &p->pool);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateCommandPool", r);
    rlvk_command_buffer_allocate_info ca = {
        .sType = RLVK_ST_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = p->pool,
        .level = RLVK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    r = d->vkAllocateCommandBuffers(d->device, &ca, &p->cmd);
    if (r != RLVK_SUCCESS)
        return fail("vkAllocateCommandBuffers", r);
    rlvk_fence_create_info fi = { .sType = RLVK_ST_FENCE_CREATE_INFO, .flags = RLVK_FENCE_CREATE_SIGNALED_BIT };
    r = d->vkCreateFence(d->device, &fi, NULL, &p->fence);
    if (r != RLVK_SUCCESS)
        return fail("vkCreateFence", r);
    rlvk_semaphore_create_info si = { .sType = RLVK_ST_SEMAPHORE_CREATE_INFO };
    r = d->vkCreateSemaphore(d->device, &si, NULL, &p->acquired);
    return r == RLVK_SUCCESS || fail("vkCreateSemaphore", r);
}

/** @brief Waits until the device is done with everything, so a swapchain or surface can go. */
static void idle(rl_present *p)
{
    rl_vkdev_wait_idle(p->d);
}

static void drop_swapchain(rl_present *p, bool keep_handle)
{
    const rlvk_dev *d = p->d;
    for (uint32_t i = 0; i < p->nimages; i++)
        if (p->done[i])
            d->vkDestroySemaphore(d->device, p->done[i], NULL);
    memset(p->done, 0, sizeof p->done);
    memset(p->images, 0, sizeof p->images);
    p->nimages = 0;
    if (!keep_handle && p->swapchain) {
        d->vkDestroySwapchainKHR(d->device, p->swapchain, NULL);
        p->swapchain = 0;
    }
}

static void drop_surface(rl_present *p)
{
    if (!p->surface && !p->swapchain)
        return;
    idle(p);
    drop_swapchain(p, false);
    if (p->surface)
        p->d->vkDestroySurfaceKHR(p->d->instance, p->surface, NULL);
    p->surface = 0;
    p->stats.mode = -1;
    p->stats.width = p->stats.height = 0;
}

static bool make_surface(rl_present *p)
{
    const rl_window_backend *b = p->win->b;
    uint64_t s = 0;
    if (!b || !b->vulkan_surface || b->vulkan_surface(p->win, (void *)(uintptr_t)p->d->instance, &s) != 0 || !s)
        return false;
    uint32_t ok = 0;
    p->d->vkGetPhysicalDeviceSurfaceSupportKHR(p->d->phys, p->d->family, s, &ok);
    if (!ok) {
        rl_log(RL_LOG_ERROR, "present", "the core device's queue cannot present to the %s window", b->name);
        p->d->vkDestroySurfaceKHR(p->d->instance, s, NULL);
        return false;
    }
    p->surface = s;
    p->stale = true;
    return true;
}

const char *rl_present_mode_name(int32_t mode)
{
    switch (mode) {
    case RLVK_PRESENT_MODE_IMMEDIATE_KHR:
        return "immediate";
    case RLVK_PRESENT_MODE_MAILBOX_KHR:
        return "mailbox";
    case RLVK_PRESENT_MODE_FIFO_RELAXED_KHR:
        return "fifo relaxed";
    case RLVK_PRESENT_MODE_FIFO_LATEST_READY_KHR:
        return "fifo latest ready";
    default:
        return "fifo";
    }
}

static bool offered(const int32_t *avail, uint32_t n, int32_t mode)
{
    for (uint32_t i = 0; i < n; i++)
        if (avail[i] == mode)
            return true;
    return false;
}

int32_t rl_present_mode_choose(uint32_t choice, int32_t base, const int32_t *avail, uint32_t n)
{
    static const int32_t named[] = {
        RLVK_PRESENT_MODE_FIFO_KHR,       RLVK_PRESENT_MODE_FIFO_KHR,      RLVK_PRESENT_MODE_FIFO_RELAXED_KHR,
        RLVK_PRESENT_MODE_FIFO_LATEST_READY_KHR, RLVK_PRESENT_MODE_MAILBOX_KHR, RLVK_PRESENT_MODE_IMMEDIATE_KHR,
    };
    if (choice != RL_PRESENT_MODE_AUTO && choice < sizeof named / sizeof *named && offered(avail, n, named[choice]))
        return named[choice];
    return offered(avail, n, base) ? base : RLVK_PRESENT_MODE_FIFO_KHR;
}

static int32_t pick_mode(rl_present *p)
{
    int32_t modes[16];
    uint32_t n = 16;
    if (p->d->vkGetPhysicalDeviceSurfacePresentModesKHR(p->d->phys, p->surface, &n, modes) < RLVK_SUCCESS)
        n = 0;
    return rl_present_mode_choose(rl_setting_enum("video.present_mode"), RLVK_PRESENT_MODE_MAILBOX_KHR, modes, n);
}

static bool pick_format(rl_present *p, int32_t *format, int32_t *space)
{
    rlvk_surface_format f[32];
    uint32_t n = 32;
    if (p->d->vkGetPhysicalDeviceSurfaceFormatsKHR(p->d->phys, p->surface, &n, f) < RLVK_SUCCESS || !n)
        return false;
    static const int32_t pref[] = { RLVK_FORMAT_R8G8B8A8_UNORM, RLVK_FORMAT_B8G8R8A8_UNORM, RLVK_FORMAT_R8G8B8A8_SRGB,
                                    RLVK_FORMAT_B8G8R8A8_SRGB };
    for (size_t k = 0; k < sizeof pref / sizeof pref[0]; k++)
        for (uint32_t i = 0; i < n; i++)
            if (f[i].format == pref[k] && f[i].colorSpace == RLVK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
                *format = f[i].format;
                *space = f[i].colorSpace;
                return true;
            }
    return false;
}

static bool make_swapchain(rl_present *p)
{
    const rlvk_dev *d = p->d;
    rlvk_surface_capabilities caps;
    rlvk_result r = d->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(d->phys, p->surface, &caps);
    if (r == RLVK_ERROR_SURFACE_LOST_KHR) {
        drop_surface(p);
        return false;
    }
    if (r != RLVK_SUCCESS)
        return fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR", r);
    uint32_t w = caps.currentExtent.width, h = caps.currentExtent.height;
    uint32_t transform = caps.currentTransform;
    if (p->gpu_frames && (caps.supportedTransforms & RLVK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
        transform = RLVK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    if (!(transform & ROTATIONS) || !(caps.supportedTransforms & transform))
        transform = (caps.supportedTransforms & RLVK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
                        ? RLVK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
                        : caps.supportedTransforms & (~caps.supportedTransforms + 1);
    p->made_extent_w = w;
    p->made_extent_h = h;
    p->made_current = caps.currentTransform;
    if (w == ~0u || h == ~0u) {
        rl_window_size(p->win, &w, &h);
        w = w < caps.minImageExtent.width ? caps.minImageExtent.width : w > caps.maxImageExtent.width ? caps.maxImageExtent.width : w;
        h = h < caps.minImageExtent.height ? caps.minImageExtent.height : h > caps.maxImageExtent.height ? caps.maxImageExtent.height : h;
    } else if ((caps.currentTransform & QUARTER_TURNS) && (transform & QUARTER_TURNS)) {
        uint32_t t = w;
        w = h;
        h = t;
    }
    if (!w || !h)
        return false;
    int32_t format, space;
    if (!pick_format(p, &format, &space))
        return fail("finding an RGBA8 surface format", 0);
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount)
        count = caps.maxImageCount;
    uint32_t alpha = (caps.supportedCompositeAlpha & RLVK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR) ? RLVK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                     : (caps.supportedCompositeAlpha & RLVK_COMPOSITE_ALPHA_INHERIT_BIT_KHR)
                         ? RLVK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                         : caps.supportedCompositeAlpha & (~caps.supportedCompositeAlpha + 1);
    if (!(caps.supportedUsageFlags & RLVK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return fail("a swapchain the presenter can copy into", 0);
    p->sc_readable = (caps.supportedUsageFlags & RLVK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    int32_t mode = pick_mode(p);
    idle(p);
    rlvk_handle old = p->swapchain;
    drop_swapchain(p, true);
    rlvk_swapchain_create_info ci = {
        .sType = RLVK_ST_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = p->surface,
        .minImageCount = count,
        .imageFormat = format,
        .imageColorSpace = space,
        .imageExtent = { w, h },
        .imageArrayLayers = 1,
        .imageUsage = RLVK_IMAGE_USAGE_TRANSFER_DST_BIT | (p->sc_readable ? RLVK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0),
        .preTransform = transform,
        .compositeAlpha = alpha,
        .presentMode = mode,
        .clipped = 1,
        .oldSwapchain = old,
    };
    r = d->vkCreateSwapchainKHR(d->device, &ci, NULL, &p->swapchain);
    if (old)
        d->vkDestroySwapchainKHR(d->device, old, NULL);
    if (r != RLVK_SUCCESS) {
        p->swapchain = 0;
        if (r == RLVK_ERROR_SURFACE_LOST_KHR)
            drop_surface(p);
        return fail("vkCreateSwapchainKHR", r);
    }
    uint32_t n = MAX_IMAGES;
    r = d->vkGetSwapchainImagesKHR(d->device, p->swapchain, &n, p->images);
    if (r < RLVK_SUCCESS)
        return fail("vkGetSwapchainImagesKHR", r);
    p->nimages = n;
    rlvk_semaphore_create_info si = { .sType = RLVK_ST_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < n; i++)
        if ((r = d->vkCreateSemaphore(d->device, &si, NULL, &p->done[i])) != RLVK_SUCCESS)
            return fail("vkCreateSemaphore", r);
    p->sc_w = w;
    p->sc_h = h;
    p->sc_transform = transform;
    p->sc_format = format;
    p->mode = mode;
    p->stale = false;
    pthread_mutex_lock(&p->lock);
    p->stats.swapchains++;
    p->stats.mode = mode;
    p->stats.width = w;
    p->stats.height = h;
    pthread_mutex_unlock(&p->lock);
    rl_log(RL_LOG_INFO, "present", "%s window: swapchain %ux%u, %u images, format %d, %s, transform %#x of %#x",
           p->win->b ? p->win->b->name : "a", w, h, n, (int)format,
           rl_present_mode_name(mode), transform, caps.currentTransform);
    return true;
}

static void layout(rl_present *p, rlvk_handle img, int32_t from, int32_t to, uint32_t src_access, uint32_t dst_access,
                   uint32_t src_stage, uint32_t dst_stage)
{
    rlvk_image_memory_barrier b = {
        .sType = RLVK_ST_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = RLVK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = RLVK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    p->d->vkCmdPipelineBarrier(p->cmd, src_stage, dst_stage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void blit(rl_present *p, rlvk_handle from, uint32_t fw, uint32_t fh, rlvk_handle to, uint32_t x, uint32_t y,
                 uint32_t w, uint32_t h, int32_t filter)
{
    rlvk_image_blit r = {
        .srcSubresource = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)fw, (int32_t)fh, 1 } },
        .dstSubresource = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { (int32_t)x, (int32_t)y, 0 }, { (int32_t)(x + w), (int32_t)(y + h), 1 } },
    };
    p->d->vkCmdBlitImage(p->cmd, from, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         1, &r, filter);
}

static bool is_srgb(int32_t f) { return f == RLVK_FORMAT_R8G8B8A8_SRGB || f == RLVK_FORMAT_B8G8R8A8_SRGB; }

/** @brief Packs a w by h RGB888 frame into RGBA8 words, turned the way transform turns the swapchain's content. */
static void pack_turned(uint32_t *dst, const uint8_t *rgb, uint32_t w, uint32_t h, uint32_t transform)
{
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x++, rgb += 3) {
            uint32_t word = (uint32_t)rgb[0] | (uint32_t)rgb[1] << 8 | (uint32_t)rgb[2] << 16 | 0xff000000u;
            size_t at;
            if (transform & RLVK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR)
                at = (size_t)x * h + (h - 1 - y);
            else if (transform & RLVK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
                at = (size_t)(w - 1 - x) * h + y;
            else if (transform & RLVK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)
                at = (size_t)(h - 1 - y) * w + (w - 1 - x);
            else
                at = (size_t)y * w + x;
            dst[at] = word;
        }
}

static void copy_whole(rl_present *p, rlvk_handle from, rlvk_handle to, uint32_t w, uint32_t h)
{
    rlvk_image_copy c = {
        .srcSubresource = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstSubresource = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .extent = { w, h, 1 },
    };
    p->d->vkCmdCopyImage(p->cmd, from, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to,
                         RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
}

/** @brief The format of the images the frame is scaled from: the swapchain's own encoding, so the blit passes bytes. */
static int32_t source_format(const rl_present *p)
{
    return is_srgb(__atomic_load_n(&p->sc_format, __ATOMIC_RELAXED)) ? RLVK_FORMAT_R8G8B8A8_SRGB
                                                                      : RLVK_FORMAT_R8G8B8A8_UNORM;
}

uint32_t rl_present_overlay_blend(uint32_t frame, uint32_t over)
{
    uint32_t a = over >> 24;
    if (a == 255)
        return over | 0xff000000u;
    if (!over)
        return frame | 0xff000000u;
    uint32_t k = 255 - a, r = 0;
    for (uint32_t s = 0; s < 24; s += 8) {
        uint32_t c = ((over >> s) & 255) + (((frame >> s) & 255) * k + 127) / 255;
        r |= (c > 255 ? 255 : c) << s;
    }
    return r | 0xff000000u;
}

/** @brief Makes the blend pass's pipeline (overlay.comp) and its descriptor set, once; false (logged) when it cannot. */
static bool ov_pipeline(rl_present *p)
{
    if (p->ov_pipe)
        return true;
    const rlvk_dev *d = p->d;
    rlvk_descriptor_set_layout_binding bind[2];
    for (uint32_t i = 0; i < 2; i++)
        bind[i] = (rlvk_descriptor_set_layout_binding){
            .binding = i,
            .descriptorType = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT,
        };
    rlvk_descriptor_set_layout_create_info sl = {
        .sType = RLVK_ST_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2,
        .pBindings = bind,
    };
    rlvk_result r = d->vkCreateDescriptorSetLayout(d->device, &sl, NULL, &p->ov_set_layout);
    if (r != RLVK_SUCCESS)
        return fail("overlay vkCreateDescriptorSetLayout", r);
    rlvk_push_constant_range pcr = { .stageFlags = RLVK_SHADER_STAGE_COMPUTE_BIT, .size = sizeof(ov_params) };
    rlvk_pipeline_layout_create_info pl = {
        .sType = RLVK_ST_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &p->ov_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    if ((r = d->vkCreatePipelineLayout(d->device, &pl, NULL, &p->ov_layout)) != RLVK_SUCCESS)
        return fail("overlay vkCreatePipelineLayout", r);
    rlvk_shader_module_create_info sm = {
        .sType = RLVK_ST_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof rl_post_spv_overlay,
        .pCode = rl_post_spv_overlay,
    };
    rlvk_handle module = 0;
    if ((r = d->vkCreateShaderModule(d->device, &sm, NULL, &module)) != RLVK_SUCCESS)
        return fail("overlay vkCreateShaderModule", r);
    rlvk_compute_pipeline_create_info cp = {
        .sType = RLVK_ST_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = RLVK_ST_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = RLVK_SHADER_STAGE_COMPUTE_BIT,
            .module = module,
            .pName = "main",
        },
        .layout = p->ov_layout,
        .basePipelineIndex = -1,
    };
    r = d->vkCreateComputePipelines(d->device, 0, 1, &cp, NULL, &p->ov_pipe);
    d->vkDestroyShaderModule(d->device, module, NULL);
    if (r != RLVK_SUCCESS) {
        p->ov_pipe = 0;
        return fail("overlay vkCreateComputePipelines", r);
    }
    rlvk_descriptor_pool_size ps = { .type = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = 2 };
    rlvk_descriptor_pool_create_info dp = {
        .sType = RLVK_ST_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &ps,
    };
    if ((r = d->vkCreateDescriptorPool(d->device, &dp, NULL, &p->ov_dpool)) != RLVK_SUCCESS)
        return fail("overlay vkCreateDescriptorPool", r);
    rlvk_descriptor_set_allocate_info da = {
        .sType = RLVK_ST_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = p->ov_dpool,
        .descriptorSetCount = 1,
        .pSetLayouts = &p->ov_set_layout,
    };
    if ((r = d->vkAllocateDescriptorSets(d->device, &da, &p->ov_set)) != RLVK_SUCCESS)
        return fail("overlay vkAllocateDescriptorSets", r);
    return true;
}

/**
 * @brief Readies the blend pass for a w by h swapchain image (after the frame fence): the pipeline, the frame image
 * and buffer, and the overlay buffer with the rows that changed copied in. False when the pass cannot run, which
 * leaves the frame without its overlay (logged once).
 */
static bool ov_ready(rl_present *p, uint32_t w, uint32_t h, int32_t format)
{
    const rlvk_dev *d = p->d;
    if (p->ov_failed)
        return false;
    rlvk_size fbytes = (rlvk_size)w * h * 4, obytes = (rlvk_size)p->ov_w * p->ov_h * 4;
    bool ok = ov_pipeline(p) && image_make(p, &p->frame, w, h, format);
    if (ok && p->fr_buf.size < fbytes) {
        rl_vkdev_buf_free(d, &p->fr_buf);
        p->ov_bound_fr = 0;
        ok = rl_vkdev_buf_make(d, &p->fr_buf, fbytes,
                               RLVK_BUFFER_USAGE_STORAGE_BUFFER_BIT | RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT |
                                   RLVK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               false, false);
    }
    if (ok && p->ov_buf.size < obytes) {
        rl_vkdev_buf_free(d, &p->ov_buf);
        p->ov_bound_fr = 0;
        ok = rl_vkdev_buf_make(d, &p->ov_buf, obytes, RLVK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true, false);
        p->ov_upload_all = true;
    }
    if (!ok) {
        p->ov_failed = true;
        rl_log(RL_LOG_WARN, "present", "the overlay pass is off: frames show without the on-screen controller");
        return false;
    }
    uint32_t y0 = p->ov_upload_all ? 0 : p->ov_y0, y1 = p->ov_upload_all ? p->ov_h : p->ov_y1;
    if (y1 > p->ov_h)
        y1 = p->ov_h;
    if (y0 < y1) {
        size_t row = (size_t)p->ov_w * 4;
        memcpy((uint8_t *)p->ov_buf.map + y0 * row, p->ov_px + y0 * row, (y1 - y0) * row);
        rl_vkdev_buf_flush(d, &p->ov_buf);
    }
    p->ov_upload_all = false;
    p->ov_y0 = p->ov_y1 = 0;
    /* A buffer made again can get the handle of the one it replaced, so a remake clears ov_bound_fr. */
    if (p->ov_bound_fr != p->fr_buf.buffer || p->ov_bound_ov != p->ov_buf.buffer) {
        rlvk_descriptor_buffer_info info[2] = {
            { .buffer = p->fr_buf.buffer, .range = p->fr_buf.size },
            { .buffer = p->ov_buf.buffer, .range = p->ov_buf.size },
        };
        rlvk_write_descriptor_set wr[2];
        for (uint32_t i = 0; i < 2; i++)
            wr[i] = (rlvk_write_descriptor_set){
                .sType = RLVK_ST_WRITE_DESCRIPTOR_SET,
                .dstSet = p->ov_set,
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = RLVK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                .pBufferInfo = &info[i],
            };
        d->vkUpdateDescriptorSets(d->device, 2, wr, 0, NULL);
        p->ov_bound_fr = p->fr_buf.buffer;
        p->ov_bound_ov = p->ov_buf.buffer;
    }
    return true;
}

/** @brief The quarter turns of transform, as overlay.comp counts them. */
static uint32_t ov_turn(uint32_t transform)
{
    if (transform & RLVK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR)
        return 1;
    if (transform & RLVK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR)
        return 2;
    if (transform & RLVK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
        return 3;
    return 0;
}

/**
 * @brief Records the blend pass: the frame image (the picture as it would go to the swapchain, TRANSFER_DST) through
 * the frame buffer and overlay.comp into swapchain image sc, left in TRANSFER_DST.
 */
static void ov_record(rl_present *p, rlvk_handle sc, uint32_t transform, uint32_t lw, uint32_t lh)
{
    const rlvk_dev *d = p->d;
    const uint32_t xfer = RLVK_PIPELINE_STAGE_TRANSFER_BIT, cs = RLVK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const uint32_t wr = RLVK_ACCESS_TRANSFER_WRITE_BIT, rd = RLVK_ACCESS_TRANSFER_READ_BIT;
    uint32_t w = p->frame.w, h = p->frame.h;
    layout(p, p->frame.image, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wr, rd,
           xfer, xfer);
    rlvk_buffer_image_copy whole = {
        .imageSubresource = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { w, h, 1 },
    };
    d->vkCmdCopyImageToBuffer(p->cmd, p->frame.image, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->fr_buf.buffer, 1,
                              &whole);
    rlvk_memory_barrier mb = { .sType = RLVK_ST_MEMORY_BARRIER,
                               .srcAccessMask = wr | RLVK_ACCESS_HOST_WRITE_BIT,
                               .dstAccessMask = RLVK_ACCESS_SHADER_READ_BIT | RLVK_ACCESS_SHADER_WRITE_BIT };
    d->vkCmdPipelineBarrier(p->cmd, xfer | RLVK_PIPELINE_STAGE_HOST_BIT, cs, 0, 1, &mb, 0, NULL, 0, NULL);
    int32_t f = p->sc_format;
    ov_params pc = {
        .width = w,
        .height = h,
        .view_w = lw,
        .view_h = lh,
        .over_w = p->ov_w,
        .over_h = p->ov_h,
        .turn = ov_turn(transform),
        .swap_rb = f == RLVK_FORMAT_B8G8R8A8_UNORM || f == RLVK_FORMAT_B8G8R8A8_SRGB,
    };
    d->vkCmdBindPipeline(p->cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, p->ov_pipe);
    d->vkCmdBindDescriptorSets(p->cmd, RLVK_PIPELINE_BIND_POINT_COMPUTE, p->ov_layout, 0, 1, &p->ov_set, 0, NULL);
    d->vkCmdPushConstants(p->cmd, p->ov_layout, RLVK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    d->vkCmdDispatch(p->cmd, (w + 15) / 16, (h + 15) / 16, 1);
    mb.srcAccessMask = RLVK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = rd;
    d->vkCmdPipelineBarrier(p->cmd, cs, xfer, 0, 1, &mb, 0, NULL, 0, NULL);
    layout(p, sc, RLVK_IMAGE_LAYOUT_UNDEFINED, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, wr, xfer, xfer);
    d->vkCmdCopyBufferToImage(p->cmd, p->fr_buf.buffer, sc, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &whole);
}

void rl_present_set_overlay(rl_present *p, const rl_present_overlay *ov)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->lock);
    p->ov_on = ov && ov->pixels;
    if (p->ov_on)
        p->ov = *ov;
    else
        memset(&p->ov, 0, sizeof p->ov);
    p->ov_px = NULL;
    p->ov_due = true;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
}

/** @brief The presenter's timestamps for gpu.profile: two per present, read when its fence is next waited for. */
static struct {
    bool tried, pending;
    rlvk_handle pool;
    void(RLVK_CALL *write)(rlvk_handle, uint32_t, rlvk_handle, uint32_t);
    void(RLVK_CALL *reset)(rlvk_handle, rlvk_handle, uint32_t, uint32_t);
    int32_t(RLVK_CALL *get)(rlvk_handle, rlvk_handle, uint32_t, uint32_t, size_t, void *, uint64_t, uint32_t);
    uint64_t ticks, presents;
} g_pt;

static bool gpu_timed(const rlvk_dev *d)
{
    if (!g_pt.tried) {
        g_pt.tried = true;
        if (!rl_setting_bool("gpu.profile"))
            return false;
        int32_t(RLVK_CALL * create)(rlvk_handle, const void *, const void *, rlvk_handle *);
        void *f = (void *)d->vkGetDeviceProcAddr(d->device, "vkCreateQueryPool");
        memcpy(&create, &f, sizeof f);
        f = (void *)d->vkGetDeviceProcAddr(d->device, "vkCmdWriteTimestamp");
        memcpy(&g_pt.write, &f, sizeof f);
        f = (void *)d->vkGetDeviceProcAddr(d->device, "vkCmdResetQueryPool");
        memcpy(&g_pt.reset, &f, sizeof f);
        f = (void *)d->vkGetDeviceProcAddr(d->device, "vkGetQueryPoolResults");
        memcpy(&g_pt.get, &f, sizeof f);
        /* VkQueryPoolCreateInfo: sType 11, query type 2 (timestamp), 2 queries. */
        struct {
            int32_t sType;
            const void *pNext;
            uint32_t flags;
            int32_t queryType;
            uint32_t queryCount;
            uint32_t stats;
        } qi = { 11, NULL, 0, 2, 2, 0 };
        if (!create || !g_pt.write || !g_pt.reset || !g_pt.get || create(d->device, &qi, NULL, &g_pt.pool) != 0)
            g_pt.pool = 0;
    }
    return g_pt.pool != 0;
}

void rl_present_gpu_stats(uint64_t *ticks, uint64_t *presents)
{
    *ticks = __atomic_load_n(&g_pt.ticks, __ATOMIC_RELAXED);
    *presents = __atomic_load_n(&g_pt.presents, __ATOMIC_RELAXED);
}

/** @brief Adds the last present's GPU time once its fence was waited for. */
static void gpu_time_read(const rlvk_dev *d)
{
    uint64_t ts[2];
    if (!g_pt.pending)
        return;
    g_pt.pending = false;
    /* VK_QUERY_RESULT_64_BIT */
    if (g_pt.get(d->device, g_pt.pool, 0, 2, sizeof ts, ts, sizeof ts[0], 1) == 0 && ts[1] >= ts[0]) {
        __atomic_add_fetch(&g_pt.ticks, ts[1] - ts[0], __ATOMIC_RELAXED);
        __atomic_add_fetch(&g_pt.presents, 1, __ATOMIC_RELAXED);
    }
}

/** @brief Records the frame into swapchain image index: upload, post passes, clear, scale. A still replaces it. */
static bool record(rl_present *p, uint32_t index, bool capture)
{
    const rlvk_dev *d = p->d;
    const uint32_t xfer = RLVK_PIPELINE_STAGE_TRANSFER_BIT;
    const uint32_t wr = RLVK_ACCESS_TRANSFER_WRITE_BIT, rd = RLVK_ACCESS_TRANSFER_READ_BIT;
    bool still = p->still_shown;
    const uint8_t *cpu_px = still ? p->still : p->work;
    uint32_t pw = still ? p->still_w : p->work_w, ph = still ? p->still_h : p->work_h;
    uint32_t psw = still ? p->still_w : p->work_sw, psh = still ? p->still_h : p->work_sh;
    bool pfit = still ? false : p->work_fit;
    bool gpu = !still && p->cur_slot >= 0;
    uint32_t t = gpu ? RLVK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : p->sc_transform;
    bool quarter = (t & QUARTER_TURNS) != 0;
    uint32_t lw = quarter ? p->sc_h : p->sc_w, lh = quarter ? p->sc_w : p->sc_h;
    uint32_t fw = quarter ? ph : pw, fh = quarter ? pw : ph;
    /* A still taken away before the first frame arrives leaves nothing to show: skip the pass. */
    if (!fw || !fh)
        return false;
    int32_t src_format = source_format(p);
    rlvk_handle img = 0;
    uint32_t cw = fw, ch = fh;
    bool post = false;
    if (!still && !p->post_failed && rl_post_gpu_wanted()) {
        if (!p->post && !(p->post = rl_post_gpu_new()))
            p->post_failed = true;
        post = p->post && rl_post_gpu_plan(p->post, fw, fh, &cw, &ch);
    }
    if (!post) {
        cw = fw;
        ch = fh;
    }
    rl_present_rect logical, at;
    int32_t ov_at[4];
    if (!still && p->ov_on && p->ov.place && p->ov.place(p->ov.user, ov_at) && ov_at[2] > 0 && ov_at[3] > 0 &&
        ov_at[0] >= 0 && ov_at[1] >= 0 && (uint32_t)(ov_at[0] + ov_at[2]) <= lw && (uint32_t)(ov_at[1] + ov_at[3]) <= lh)
        logical = (rl_present_rect){ (uint32_t)ov_at[0], (uint32_t)ov_at[1], (uint32_t)ov_at[2], (uint32_t)ov_at[3] };
    else
        rl_present_place(psw, psh, lw, lh, rl_post_integer(pfit), &logical);
    rl_present_turn(&logical, lw, lh, t, &at);
    bool over = !still && p->ov_on && p->ov_px && p->ov_w && p->ov_h && ov_ready(p, p->sc_w, p->sc_h, src_format);
    if (!still) {
        pthread_mutex_lock(&p->lock);
        p->shown_at = logical;
        p->shown_fw = p->work_w;
        p->shown_fh = p->work_h;
        pthread_mutex_unlock(&p->lock);
    }
    int sampling = rl_post_sampling(cw, ch, at.w, at.h);
    bool sharp = sampling == RL_POST_SAMPLE_SHARP;
    p->drew.source = still ? "still" : gpu ? "slot" : "host pixels";
    p->drew.slot = gpu ? p->cur_slot : -1;
    p->drew.src_format = gpu ? p->slots[p->cur_slot].im.format : src_format;
    p->drew.fw = fw;
    p->drew.fh = fh;
    p->drew.cw = cw;
    p->drew.ch = ch;
    p->drew.sampling = (uint32_t)sampling;
    p->drew.at = at;
    p->drew.post = post;
    p->drew.over = over;
    uint32_t kx = at.w / cw, ky = at.h / ch;
    kx = kx ? kx : 1;
    ky = ky ? ky : 1;
    image *slot_im = gpu ? &p->slots[p->cur_slot].im : NULL;
    bool copy_slot = gpu && slot_im->format != src_format;
    if ((!gpu || copy_slot) && !image_make(p, &p->src, fw, fh, src_format))
        return false;
    if ((sharp && !image_make(p, &p->pre, cw * kx, ch * ky, src_format)) ||
        (post && !image_make(p, &p->pic, cw, ch, src_format)))
        return false;
    if (!gpu) {
        rlvk_size bytes = (rlvk_size)fw * fh * 4;
        if (p->stage.size < bytes &&
            !rl_vkdev_buf_make(d, &p->stage, bytes, RLVK_BUFFER_USAGE_TRANSFER_SRC_BIT, true, false))
            return false;
        pack_turned(p->stage.map, cpu_px, pw, ph, t);
        rl_vkdev_buf_flush(d, &p->stage);
    }
    rlvk_size cap_bytes = (rlvk_size)p->sc_w * p->sc_h * 4;
    if (capture && p->readback.size < cap_bytes &&
        !rl_vkdev_buf_make(d, &p->readback, cap_bytes, RLVK_BUFFER_USAGE_TRANSFER_DST_BIT, true, true))
        return false;

    rlvk_command_buffer_begin_info bi = {
        .sType = RLVK_ST_COMMAND_BUFFER_BEGIN_INFO,
        .flags = RLVK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    rlvk_result r = d->vkBeginCommandBuffer(p->cmd, &bi);
    bool timed = r == RLVK_SUCCESS && gpu_timed(d);
    if (timed) {
        g_pt.reset(p->cmd, g_pt.pool, 0, 2);
        g_pt.write(p->cmd, RLVK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pt.pool, 0);
    }
    if (r != RLVK_SUCCESS)
        return fail("vkBeginCommandBuffer", r);
    rlvk_handle sc = p->images[index];
    rlvk_handle dst = over ? p->frame.image : sc;
    if (!gpu) {
        layout(p, p->src.image, RLVK_IMAGE_LAYOUT_UNDEFINED, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, wr,
               RLVK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, xfer);
        rlvk_buffer_image_copy copy = {
            .imageSubresource = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { fw, fh, 1 },
        };
        d->vkCmdCopyBufferToImage(p->cmd, p->stage.buffer, p->src.image, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                                  &copy);
        layout(p, p->src.image, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wr, rd,
               xfer, xfer);
        img = p->src.image;
    } else if (copy_slot) {
        layout(p, p->src.image, RLVK_IMAGE_LAYOUT_UNDEFINED, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, wr,
               RLVK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, xfer);
        copy_whole(p, slot_im->image, p->src.image, fw, fh);
        layout(p, p->src.image, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wr, rd,
               xfer, xfer);
        img = p->src.image;
    } else {
        img = slot_im->image;
    }
    if (post) {
        layout(p, p->pic.image, RLVK_IMAGE_LAYOUT_UNDEFINED, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, wr,
               RLVK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, xfer);
        if (!rl_post_gpu_record(p->post, (uint64_t)p->cmd, (uint64_t)img, fw, fh, (uint64_t)p->pic.image, cw, ch)) {
            d->vkEndCommandBuffer(p->cmd);
            return false;
        }
        layout(p, p->pic.image, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wr, rd,
               xfer, xfer);
        img = p->pic.image;
    }
    layout(p, dst, RLVK_IMAGE_LAYOUT_UNDEFINED, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, wr, xfer, xfer);
    rlvk_clear_color black = { .float32 = { 0, 0, 0, 1 } };
    rlvk_image_subresource_range all = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    d->vkCmdClearColorImage(p->cmd, dst, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    layout(p, dst, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, wr, wr, xfer, xfer);
    if (at.w && at.h) {
        if (sharp) {
            layout(p, p->pre.image, RLVK_IMAGE_LAYOUT_UNDEFINED, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, wr, xfer,
                   xfer);
            blit(p, img, cw, ch, p->pre.image, 0, 0, p->pre.w, p->pre.h, RLVK_FILTER_NEAREST);
            layout(p, p->pre.image, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wr,
                   rd, xfer, xfer);
            blit(p, p->pre.image, p->pre.w, p->pre.h, dst, at.x, at.y, at.w, at.h, RLVK_FILTER_LINEAR);
        } else {
            blit(p, img, cw, ch, dst, at.x, at.y, at.w, at.h,
                 sampling == RL_POST_SAMPLE_LINEAR ? RLVK_FILTER_LINEAR : RLVK_FILTER_NEAREST);
        }
    }
    if (over) {
        layout(p, dst, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, wr, wr, xfer,
               xfer);
        ov_record(p, sc, t, lw, lh);
    }
    if (capture) {
        layout(p, sc, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wr, rd, xfer, xfer);
        rlvk_buffer_image_copy back = {
            .imageSubresource = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
            .imageExtent = { p->sc_w, p->sc_h, 1 },
        };
        d->vkCmdCopyImageToBuffer(p->cmd, sc, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, p->readback.buffer, 1, &back);
        layout(p, sc, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, RLVK_IMAGE_LAYOUT_PRESENT_SRC_KHR, rd, 0, xfer,
               RLVK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
        rlvk_memory_barrier host = { .sType = RLVK_ST_MEMORY_BARRIER, .srcAccessMask = wr, .dstAccessMask = RLVK_ACCESS_HOST_READ_BIT };
        d->vkCmdPipelineBarrier(p->cmd, xfer, RLVK_PIPELINE_STAGE_HOST_BIT, 0, 1, &host, 0, NULL, 0, NULL);
    } else {
        layout(p, sc, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RLVK_IMAGE_LAYOUT_PRESENT_SRC_KHR, wr, 0, xfer,
               RLVK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    if (timed) {
        g_pt.write(p->cmd, RLVK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_pt.pool, 1);
        g_pt.pending = true;
    }
    r = d->vkEndCommandBuffer(p->cmd);
    return r == RLVK_SUCCESS || fail("vkEndCommandBuffer", r);
}

/** @brief Keeps the captured image of a frame, swapping B and R for a BGRA swapchain. */
static void keep_capture(rl_present *p)
{
    size_t n = (size_t)p->sc_w * p->sc_h;
    uint8_t *out = malloc(n * 4);
    if (!out)
        return;
    rl_vkdev_buf_invalidate(p->d, &p->readback);
    memcpy(out, p->readback.map, n * 4);
    if (p->sc_format == RLVK_FORMAT_B8G8R8A8_UNORM || p->sc_format == RLVK_FORMAT_B8G8R8A8_SRGB)
        for (size_t i = 0; i < n; i++) {
            uint8_t t = out[i * 4];
            out[i * 4] = out[i * 4 + 2];
            out[i * 4 + 2] = t;
        }
    pthread_mutex_lock(&p->lock);
    free(p->captured);
    p->captured = out;
    p->cap_w = p->sc_w;
    p->cap_h = p->sc_h;
    p->capture_want = false;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
}

static void count_present(rl_present *p, uint64_t took_ns)
{
    uint64_t now = rl_clock_ns();
    pthread_mutex_lock(&p->lock);
    rl_present_stats *s = &p->stats;
    s->presented++;
    if (p->last_present_ns) {
        uint64_t gap = now - p->last_present_ns;
        s->sum_gap_ns += gap;
        s->max_gap_ns = gap > s->max_gap_ns ? gap : s->max_gap_ns;
    }
    s->sum_work_ns += took_ns;
    s->max_work_ns = took_ns > s->max_work_ns ? took_ns : s->max_work_ns;
    rl_present_stats copy = *s;
    pthread_mutex_unlock(&p->lock);
    p->last_present_ns = now;
    if (!p->last_log_ns)
        p->last_log_ns = now;
    if (now - p->last_log_ns >= STATS_EVERY_NS && copy.presented > 1) {
        p->last_log_ns = now;
        rl_log(RL_LOG_DEBUG, "present", "%llu presented, %llu replaced, gap avg %.2f ms max %.2f ms, work avg %.2f ms max %.2f ms",
               (unsigned long long)copy.presented, (unsigned long long)copy.replaced,
               (double)copy.sum_gap_ns / (double)(copy.presented - 1) / 1e6, (double)copy.max_gap_ns / 1e6,
               (double)copy.sum_work_ns / (double)copy.presented / 1e6, (double)copy.max_work_ns / 1e6);
    }
}

/**
 * @brief Whether the surface's size or turn differs from the swapchain's. A swapchain the surface calls suboptimal for
 * another reason (a turn it cannot take) is kept rather than made again each frame.
 */
static bool caps_moved(rl_present *p)
{
    rlvk_surface_capabilities caps;
    if (p->d->vkGetPhysicalDeviceSurfaceCapabilitiesKHR(p->d->phys, p->surface, &caps) != RLVK_SUCCESS)
        return true;
    return caps.currentExtent.width != p->made_extent_w || caps.currentExtent.height != p->made_extent_h ||
           caps.currentTransform != p->made_current;
}

/** @brief Shows the work frame; false when it could not and should be tried again once something changes. */
/** @brief Gives back the slots the presenter no longer shows; its fence has passed, so the GPU is done with them. */
static void release_slots(rl_present *p)
{
    pthread_mutex_lock(&p->lock);
    for (uint32_t i = 0; i < SLOTS; i++)
        if (p->release_mask & (1u << i))
            p->slots[i].state = SLOT_FREE;
    p->release_mask = 0;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
}

/** @brief A present the trace thread waits to be shown. */
typedef struct trace_job {
    rlvk_handle swapchain;
    uint64_t id, ready_ns, queued_ns;
    /** @brief The present showed a new frame (not a repaint of the last one for the overlay). */
    bool frame;
} trace_job;

#define TRACE_QUEUE 32u
#define TRACE_WAIT_NS 200000000ull

static pthread_mutex_t g_trace_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_trace_cond = PTHREAD_COND_INITIALIZER;
static trace_job g_trace_q[TRACE_QUEUE];
static uint32_t g_trace_head, g_trace_count;
static bool g_trace_started;
static FILE *g_trace_file;
static const rlvk_dev *g_trace_dev;

/** @brief Called with the time each present was shown (rl_present_set_shown_hook). */
static void (*g_shown_fn)(int64_t shown_ns);
static bool g_watch_started;

/**
 * @brief Waits for each watched present to be shown, writes "ready queued shown" in nanoseconds to the trace file
 * when one is open and hands the time to the shown hook when one is set.
 */
static void *trace_main(void *arg)
{
    (void)arg;
    uint32_t lines = 0;
    pthread_mutex_lock(&g_trace_lock);
    for (;;) {
        while (!g_trace_count)
            pthread_cond_wait(&g_trace_cond, &g_trace_lock);
        trace_job j = g_trace_q[g_trace_head];
        g_trace_head = (g_trace_head + 1) % TRACE_QUEUE;
        g_trace_count--;
        pthread_mutex_unlock(&g_trace_lock);
        rlvk_result r = g_trace_dev->vkWaitForPresentKHR(g_trace_dev->device, j.swapchain, j.id, TRACE_WAIT_NS);
        uint64_t shown = rl_clock_ns();
        if (r == RLVK_SUCCESS && g_trace_file) {
            fprintf(g_trace_file, "%llu %llu %llu\n", (unsigned long long)j.ready_ns, (unsigned long long)j.queued_ns,
                    (unsigned long long)shown);
            if (++lines % 64 == 0)
                fflush(g_trace_file);
        }
        void (*fn)(int64_t) = __atomic_load_n(&g_shown_fn, __ATOMIC_ACQUIRE);
        if (r == RLVK_SUCCESS && fn && j.frame)
            fn((int64_t)shown);
        pthread_mutex_lock(&g_trace_lock);
    }
    return NULL;
}

void rl_present_set_shown_hook(void (*fn)(int64_t shown_ns)) { __atomic_store_n(&g_shown_fn, fn, __ATOMIC_RELEASE); }

/**
 * @brief Whether presents are watched until shown: the device can wait for presents, and the video.present_trace
 * setting names a file or a shown hook is set. Starts the watcher thread the first time.
 */
static bool trace_on(const rlvk_dev *d)
{
    if (!d->present_wait)
        return false;
    pthread_mutex_lock(&g_trace_lock);
    if (!g_trace_started) {
        g_trace_started = true;
        const char *path = rl_setting_str("video.present_trace");
        if (path && path[0])
            g_trace_file = fopen(path, "w");
    }
    bool want = g_trace_file || __atomic_load_n(&g_shown_fn, __ATOMIC_ACQUIRE);
    if (want && !g_watch_started) {
        g_trace_dev = d;
        pthread_t t;
        g_watch_started = pthread_create(&t, NULL, trace_main, NULL) == 0;
        if (g_watch_started)
            pthread_detach(t);
    }
    bool on = want && g_watch_started;
    pthread_mutex_unlock(&g_trace_lock);
    return on;
}

static void trace_queue(rlvk_handle swapchain, uint64_t id, uint64_t ready_ns, uint64_t queued_ns, bool frame)
{
    pthread_mutex_lock(&g_trace_lock);
    if (g_trace_count < TRACE_QUEUE) {
        g_trace_q[(g_trace_head + g_trace_count) % TRACE_QUEUE] = (trace_job){ swapchain, id, ready_ns, queued_ns, frame };
        g_trace_count++;
        pthread_cond_signal(&g_trace_cond);
    }
    pthread_mutex_unlock(&g_trace_lock);
}

/**
 * @brief Draws one frame of the picture, the still or the overlay. Nothing to draw (a still taken away before the first
 * frame arrived) is not a failure of the surface: marking it one would stop every later draw, and the frames that follow
 * would never reach the window.
 */
/**
 * @brief Reads present.capture, `<folder>[:<every>][:<from>-<to>]` (from and to: frame numbers), into p (presenter
 * thread). An unusable folder
 * turns it off with a warning.
 */
static void pc_read(rl_present *p)
{
    const char *spec = rl_setting_str("present.capture");
    p->pc_dir[0] = 0;
    p->pc_every = 1;
    p->pc_from = p->pc_to = 0;
    if (!spec || !*spec)
        return;
    char buf[sizeof p->pc_dir];
    snprintf(buf, sizeof buf, "%s", spec);
    char *range = strrchr(buf, ':');
    if (range && strchr(range, '-') && range != buf) {
        unsigned long long a = 0, b = 0;
        if (sscanf(range + 1, "%llu-%llu", &a, &b) == 2 && b > a) {
            p->pc_from = a;
            p->pc_to = b;
            *range = 0;
        }
    }
    char *every = strrchr(buf, ':');
    if (every && every != buf && every[1] >= '0' && every[1] <= '9') {
        char *end;
        unsigned long long e = strtoull(every + 1, &end, 10);
        if (!*end && e) {
            p->pc_every = e;
            *every = 0;
        }
    }
    for (char *s = buf + 1; *s; s++)
        if (*s == '/' || *s == '\\') {
            char sep = *s;
            *s = 0;
            MKDIR(buf);
            *s = sep;
        }
    if (MKDIR(buf) != 0 && errno != EEXIST) {
        rl_log(RL_LOG_WARN, "present", "present.capture: cannot make the folder %s (%s); no captures", buf,
               strerror(errno));
        return;
    }
    memcpy(p->pc_dir, buf, sizeof buf);
    rl_log(RL_LOG_INFO, "present", "present.capture: every %llu window pictures into %s",
           (unsigned long long)p->pc_every, p->pc_dir);
}

bool rl_present_capture_due(uint64_t draw, uint64_t frame, uint64_t every, uint64_t from, uint64_t to)
{
    if (to && (frame < from || frame >= to))
        return false;
    return (draw - 1) % (every ? every : 1) == 0;
}

/**
 * @brief Writes the swapchain image of draw number n, read back after relift's scaling and overlay pass, as
 * present-<frame>-d<draw>.png (frame: the number of the frame handed over that it shows), and logs what was drawn.
 */
static void pc_write(rl_present *p, uint64_t n)
{
    rl_vkdev_buf_invalidate(p->d, &p->readback);
    char path[sizeof p->pc_dir + 64];
    snprintf(path, sizeof path, "%s/present-%06llu-d%06llu.png", p->pc_dir, (unsigned long long)p->work_number,
             (unsigned long long)n);
    bool bgra = p->sc_format == RLVK_FORMAT_B8G8R8A8_UNORM || p->sc_format == RLVK_FORMAT_B8G8R8A8_SRGB;
    if (!rl_png_write_rgb(path, p->sc_w, p->sc_h, p->readback.map, (size_t)p->sc_w * 4, bgra)) {
        rl_log(RL_LOG_ERROR, "present", "present.capture: cannot write %s", path);
        return;
    }
    rl_log(RL_LOG_INFO, "present",
           "present.capture %s: draw %llu, frame %llu (%s), swapchain %ux%u format %d; source %s %d, %ux%u format %d, "
           "picture %ux%u%s, shown at %u,%u %ux%u (%s), overlay %s",
           path, (unsigned long long)n, (unsigned long long)p->work_number, p->drew.fresh ? "new" : "drawn again",
           p->sc_w, p->sc_h, p->sc_format, p->drew.source, p->drew.slot, p->drew.fw,
           p->drew.fh, p->drew.src_format, p->drew.cw, p->drew.ch, p->drew.post ? " after the picture passes" : "",
           p->drew.at.x, p->drew.at.y, p->drew.at.w, p->drew.at.h,
           p->drew.sampling == RL_POST_SAMPLE_SHARP    ? "sharp bilinear"
           : p->drew.sampling == RL_POST_SAMPLE_LINEAR ? "bilinear"
                                                       : "nearest",
           p->drew.over ? "on" : "off");
}

static bool draw(rl_present *p, bool capture, bool frame)
{
    const rlvk_dev *d = p->d;
    uint64_t t0 = rl_clock_ns();
    if (!p->surface && !make_surface(p)) {
        p->create_failed = true;
        return false;
    }
    uint64_t gen = __atomic_load_n(&p->win->generation, __ATOMIC_ACQUIRE);
    if (gen != p->gen_seen)
        p->stale = true;
    p->gen_seen = gen;
    if ((p->stale || !p->swapchain) && !make_swapchain(p)) {
        p->create_failed = p->surface != 0;
        return false;
    }
    if (p->still_shown ? !p->still_w || !p->still_h : !p->work_w || !p->work_h)
        return false;
    if (capture && !p->sc_readable) {
        pthread_mutex_lock(&p->lock);
        p->capture_want = false;
        pthread_cond_broadcast(&p->cond);
        pthread_mutex_unlock(&p->lock);
        capture = false;
    }
    d->vkWaitForFences(d->device, 1, &p->fence, 1, ~(uint64_t)0);
    gpu_time_read(d);
    release_slots(p);
    uint32_t index = 0;
    rlvk_result r = d->vkAcquireNextImageKHR(d->device, p->swapchain, ~(uint64_t)0, p->acquired, 0, &index);
    if (r == RLVK_ERROR_OUT_OF_DATE_KHR) {
        p->stale = true;
        return false;
    }
    if (r == RLVK_ERROR_SURFACE_LOST_KHR) {
        drop_surface(p);
        return false;
    }
    if (r != RLVK_SUCCESS && r != RLVK_SUBOPTIMAL_KHR)
        return fail("vkAcquireNextImageKHR", r);
    if (r == RLVK_SUBOPTIMAL_KHR && caps_moved(p))
        p->stale = true;
    d->vkResetCommandBuffer(p->cmd, 0);
    uint64_t n = ++p->draws;
    bool keep = p->pc_dir[0] && p->sc_readable && rl_present_capture_due(n, p->work_number, p->pc_every, p->pc_from, p->pc_to);
    bool watch = frame && p->sc_readable && rl_flashwatch_on(&p->flash);
    if (!record(p, index, capture || keep || watch)) {
        drop_surface(p);
        p->create_failed = true;
        return false;
    }
    d->vkResetFences(d->device, 1, &p->fence);
    uint32_t wait_stage[2] = { RLVK_PIPELINE_STAGE_TRANSFER_BIT, RLVK_PIPELINE_STAGE_TRANSFER_BIT };
    /* A slot whose copy was handed over unfinished: this submit waits on the GPU for it, not the guest's thread. */
    const slot *src = !p->still_shown && p->cur_slot >= 0 ? &p->slots[p->cur_slot] : NULL;
    rlvk_handle waits[2] = { p->acquired, src ? src->wait_sem : 0 };
    uint64_t values[2] = { 0, src ? src->wait_value : 0 };
    struct {
        int32_t sType;
        const void *pNext;
        uint32_t waitSemaphoreValueCount;
        const uint64_t *pWaitSemaphoreValues;
        uint32_t signalSemaphoreValueCount;
        const uint64_t *pSignalSemaphoreValues;
    } tsi = { 1000207003, NULL, 2, values, 0, NULL };
    bool after = src && src->wait_sem;
    if (after && src->wait_cpu) {
        /* The host signals the value once the copy's batch ended; nothing on the queue waits for that signal. */
        struct {
            int32_t sType;
            const void *pNext;
            rlvk_flags flags;
            uint32_t semaphoreCount;
            const rlvk_handle *pSemaphores;
            const uint64_t *pValues;
        } wi = { 1000207004, NULL, 0, 1, &src->wait_sem, &src->wait_value };
        rlvk_result wr = d->vkWaitSemaphores ? d->vkWaitSemaphores(d->device, &wi, UINT64_MAX) : -1;
        if (wr != RLVK_SUCCESS)
            rl_log(RL_LOG_ERROR, "present", "waiting for the frame's copy failed (%d)", (int)wr);
        after = false;
    }
    rlvk_submit_info sub = {
        .sType = RLVK_ST_SUBMIT_INFO,
        .pNext = after ? &tsi : NULL,
        .waitSemaphoreCount = after ? 2u : 1u,
        .pWaitSemaphores = waits,
        .pWaitDstStageMask = wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &p->cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &p->done[index],
    };
    struct {
        int32_t sType;
        const void *pNext;
        uint32_t swapchainCount;
        const uint64_t *pPresentIds;
    } pid = { 1000294000, NULL, 1, &(uint64_t){ 0 } };
    uint64_t id = trace_on(d) ? ++p->present_ids : 0;
    pid.pPresentIds = &id;
    rlvk_present_info pi = {
        .sType = RLVK_ST_PRESENT_INFO_KHR,
        .pNext = id ? &pid : NULL,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &p->done[index],
        .swapchainCount = 1,
        .pSwapchains = &p->swapchain,
        .pImageIndices = &index,
    };
    uint64_t queued_ns = rl_clock_ns();
    rl_vkdev_queue_lock();
    r = d->vkQueueSubmit(d->queue, 1, &sub, p->fence);
    rlvk_result pr = r == RLVK_SUCCESS ? d->vkQueuePresentKHR(d->queue, &pi) : r;
    rl_vkdev_queue_unlock();
    if (r != RLVK_SUCCESS) {
        rlvk_fence_create_info fi = { .sType = RLVK_ST_FENCE_CREATE_INFO, .flags = RLVK_FENCE_CREATE_SIGNALED_BIT };
        d->vkDestroyFence(d->device, p->fence, NULL);
        d->vkCreateFence(d->device, &fi, NULL, &p->fence);
        return fail("vkQueueSubmit", r);
    }
    if (pr == RLVK_ERROR_OUT_OF_DATE_KHR || (pr == RLVK_SUBOPTIMAL_KHR && caps_moved(p))) {
        p->stale = true;
    } else if (pr == RLVK_SUBOPTIMAL_KHR) {
        /* Shown. On Android it means a swapchain transform other than the display's: GPU frames keep identity, as a
         * blit cannot turn, and the compositor turns them. On the AYN Thor it composes every layer on the GPU even
         * unturned, so turning them here would only add work. */
        if (!p->suboptimal_seen)
            rl_log(RL_LOG_DEBUG, "present", "presents are suboptimal: swapchain transform %#x, display transform %#x",
                   p->sc_transform, p->made_current);
        p->suboptimal_seen = true;
    } else if (pr == RLVK_ERROR_SURFACE_LOST_KHR)
        drop_surface(p);
    else if (pr != RLVK_SUCCESS)
        fail("vkQueuePresentKHR", pr);
    if (id && pr == RLVK_SUCCESS)
        trace_queue(p->swapchain, id, p->work_ready_ns, queued_ns, frame);
    count_present(p, rl_clock_ns() - t0);
    if (capture || keep || watch)
        d->vkWaitForFences(d->device, 1, &p->fence, 1, ~(uint64_t)0);
    if (keep)
        pc_write(p, n);
    if (watch) {
        rl_vkdev_buf_invalidate(p->d, &p->readback);
        bool bgra = p->sc_format == RLVK_FORMAT_B8G8R8A8_UNORM || p->sc_format == RLVK_FORMAT_B8G8R8A8_SRGB;
        rl_flashwatch_push(&p->flash, p->work_number, p->readback.map, p->sc_w, p->sc_h, (size_t)p->sc_w * 4, bgra);
    }
    if (capture)
        keep_capture(p);
    return pr == RLVK_SUCCESS || pr == RLVK_SUBOPTIMAL_KHR;
}

static void deadline(struct timespec *t, uint32_t ms)
{
    clock_gettime(CLOCK_REALTIME, t);
    t->tv_sec += ms / 1000;
    t->tv_nsec += (long)(ms % 1000) * 1000000L;
    if (t->tv_nsec >= 1000000000L) {
        t->tv_sec++;
        t->tv_nsec -= 1000000000L;
    }
}

/**
 * @brief Asks the window's overlay for its pixels (lock held; released around the call): a change marks the overlay
 * due for a draw and widens the rows to upload.
 */
static void ov_poll(rl_present *p)
{
    rl_present_overlay ov = p->ov;
    pthread_mutex_unlock(&p->lock);
    uint32_t w = 0, h = 0;
    int32_t dirty[4] = { 0 };
    const uint8_t *px = ov.pixels(ov.user, rl_clock_ns(), &w, &h, dirty);
    pthread_mutex_lock(&p->lock);
    if (!p->ov_on)
        return;
    /* An empty overlay keeps size 0, as it is stored below: comparing the size it reports would see a change at every
     * poll and redraw the picture each time (an idle on-screen controller on Android cost a GPU frame every 2 ms). */
    if (!px)
        w = h = 0;
    if (px != p->ov_px || w != p->ov_w || h != p->ov_h) {
        p->ov_upload_all = true;
        p->ov_due = true;
    }
    p->ov_px = px;
    p->ov_w = px ? w : 0;
    p->ov_h = px ? h : 0;
    if (px && dirty[2] > 0 && dirty[3] > 0) {
        uint32_t y0 = (uint32_t)(dirty[1] > 0 ? dirty[1] : 0), y1 = (uint32_t)(dirty[1] + dirty[3]);
        if (p->ov_y0 == p->ov_y1) {
            p->ov_y0 = y0;
            p->ov_y1 = y1;
        } else {
            p->ov_y0 = y0 < p->ov_y0 ? y0 : p->ov_y0;
            p->ov_y1 = y1 > p->ov_y1 ? y1 : p->ov_y1;
        }
        p->ov_due = true;
    }
}

static void *thread_main(void *arg)
{
    rl_present *p = arg;
    pc_read(p);
    rl_flashwatch_open(&p->flash, rl_setting_str("present.flash"));
    pthread_mutex_lock(&p->lock);
    for (;;) {
        if (p->holds && (!p->surface_ok || p->quit)) {
            pthread_mutex_unlock(&p->lock);
            drop_surface(p);
            pthread_mutex_lock(&p->lock);
            p->holds = false;
            pthread_cond_broadcast(&p->cond);
            continue;
        }
        if (p->quit)
            break;
        if (p->retry) {
            p->retry = false;
            p->create_failed = false;
            p->stale = true;
        }
        bool moved = __atomic_load_n(&p->win->generation, __ATOMIC_ACQUIRE) != p->gen_seen;
        if (moved)
            p->create_failed = false;
        if (p->ov_on && p->surface_ok && (p->have_frame || p->fresh))
            ov_poll(p);
        bool redo = (p->have_frame || p->still_shown) && (p->stale || moved || (p->ov_on && p->ov_due));
        bool still_due = p->still_fresh;
        uint64_t since = rl_clock_ns() - p->last_draw_ns;
        if (redo && !p->stale && !moved && !p->fresh && !p->capture_want && !still_due && since < OVERLAY_MIN_NS) {
            struct timespec t;
            deadline(&t, (uint32_t)((OVERLAY_MIN_NS - since) / 1000000u) + 1u);
            pthread_cond_timedwait(&p->cond, &p->lock, &t);
            continue;
        }
        if (p->surface_ok && !p->create_failed && (p->fresh || p->capture_want || redo || still_due) &&
            (p->fresh || p->have_frame || p->still_on || p->still_shown)) {
            if (still_due) {
                p->still_fresh = false;
                if (p->still_on) {
                    uint8_t *t = p->still;
                    size_t c = p->still_cap;
                    p->still = p->still_next;
                    p->still_cap = p->still_next_cap;
                    p->still_next = t;
                    p->still_next_cap = c;
                    p->still_w = p->still_next_w;
                    p->still_h = p->still_next_h;
                }
                p->still_shown = p->still_on;
            }
            bool frame = p->fresh;
            if (p->fresh) {
                if (p->cur_slot >= 0)
                    p->release_mask |= 1u << p->cur_slot;
                if (p->next_slot >= 0) {
                    p->cur_slot = p->next_slot;
                    p->slots[p->cur_slot].state = SLOT_SHOWN;
                    p->work_w = p->next_w;
                    p->work_h = p->next_h;
                } else {
                    uint8_t *t = p->work;
                    size_t c = p->work_cap;
                    p->work = p->next;
                    p->work_cap = p->next_cap;
                    p->next = t;
                    p->next_cap = c;
                    p->work_w = p->next_w;
                    p->work_h = p->next_h;
                    p->cur_slot = -1;
                }
                p->next_slot = -1;
                p->work_sw = p->next_sw;
                p->work_sh = p->next_sh;
                p->work_fit = p->next_fit;
                p->fresh = false;
                p->work_ready_ns = p->next_ready_ns;
                p->work_number = p->received;
                p->have_frame = true;
                p->drew.fresh = true;
            } else {
                p->drew.fresh = false;
            }
            bool capture = p->capture_want;
            p->ov_due = false;
            p->holds = true;
            p->last_draw_ns = rl_clock_ns();
            pthread_mutex_unlock(&p->lock);
            bool shown = draw(p, capture, frame);
            pthread_mutex_lock(&p->lock);
            if (!p->surface)
                p->holds = false;
            if (!shown && !p->fresh && p->surface_ok) {
                struct timespec t;
                deadline(&t, IDLE_WAIT_MS);
                pthread_cond_timedwait(&p->cond, &p->lock, &t);
            }
            continue;
        }
        struct timespec t;
        deadline(&t, p->ov_on && p->have_frame ? OVERLAY_POLL_MS : IDLE_WAIT_MS);
        pthread_cond_timedwait(&p->cond, &p->lock, &t);
    }
    pthread_mutex_unlock(&p->lock);
    return NULL;
}

rl_present *rl_present_open(rl_window *w)
{
    const rlvk_dev *d = rl_vkdev();
    if (!d || !d->surfaces) {
        rl_log(RL_LOG_ERROR, "present", "no Vulkan device with swapchains: the %s window cannot show frames",
               w && w->b ? w->b->name : "new");
        return NULL;
    }
    rl_present *p = calloc(1, sizeof *p);
    if (!p)
        return NULL;
    p->win = w;
    p->d = d;
    p->surface_ok = true;
    p->next_slot = p->cur_slot = -1;
    p->stats.mode = -1;
    pthread_mutex_init(&p->lock, NULL);
    pthread_cond_init(&p->cond, NULL);
    p->gen_seen = ~(uint64_t)0;
    if (!start_objects(p) || pthread_create(&p->thread, NULL, thread_main, p) != 0) {
        rl_present_close(p);
        return NULL;
    }
    return p;
}

static void free_objects(rl_present *p)
{
    const rlvk_dev *d = p->d;
    idle(p);
    image_free(p, &p->src);
    image_free(p, &p->pre);
    image_free(p, &p->pic);
    image_free(p, &p->frame);
    rl_vkdev_buf_free(d, &p->fr_buf);
    rl_vkdev_buf_free(d, &p->ov_buf);
    if (p->ov_pipe)
        d->vkDestroyPipeline(d->device, p->ov_pipe, NULL);
    if (p->ov_layout)
        d->vkDestroyPipelineLayout(d->device, p->ov_layout, NULL);
    if (p->ov_dpool)
        d->vkDestroyDescriptorPool(d->device, p->ov_dpool, NULL);
    if (p->ov_set_layout)
        d->vkDestroyDescriptorSetLayout(d->device, p->ov_set_layout, NULL);
    for (uint32_t i = 0; i < SLOTS; i++)
        image_free(p, &p->slots[i].im);
    rl_post_gpu_free(p->post);
    rl_vkdev_buf_free(d, &p->stage);
    rl_vkdev_buf_free(d, &p->readback);
    rl_flashwatch_close(&p->flash);
    if (p->acquired)
        d->vkDestroySemaphore(d->device, p->acquired, NULL);
    if (p->fence)
        d->vkDestroyFence(d->device, p->fence, NULL);
    if (p->pool)
        d->vkDestroyCommandPool(d->device, p->pool, NULL);
}

void rl_present_close(rl_present *p)
{
    if (!p)
        return;
    if (p->pool && p->fence && p->acquired) {
        pthread_mutex_lock(&p->lock);
        p->quit = true;
        pthread_cond_broadcast(&p->cond);
        pthread_mutex_unlock(&p->lock);
        pthread_join(p->thread, NULL);
    }
    drop_surface(p);
    free_objects(p);
    pthread_mutex_destroy(&p->lock);
    pthread_cond_destroy(&p->cond);
    free(p->next);
    free(p->work);
    free(p->still);
    free(p->still_next);
    free(p->captured);
    free(p);
}

void rl_present_frame(rl_present *p, const uint8_t *rgb, uint32_t width, uint32_t height, uint32_t shown_w,
                      uint32_t shown_h, bool fit, uint32_t win_w, uint32_t win_h, rl_present_rect *place)
{
    if (place) {
        rl_present_place(shown_w, shown_h, win_w, win_h, rl_post_integer(fit), place);
    }
    if (!p || !rgb || !width || !height)
        return;
    size_t bytes = (size_t)width * height * 3;
    pthread_mutex_lock(&p->lock);
    if (bytes > p->next_cap) {
        uint8_t *g = realloc(p->next, bytes);
        if (!g) {
            pthread_mutex_unlock(&p->lock);
            return;
        }
        p->next = g;
        p->next_cap = bytes;
    }
    memcpy(p->next, rgb, bytes);
    if (p->fresh)
        p->stats.replaced++;
    if (p->next_slot >= 0)
        p->slots[p->next_slot].state = SLOT_FREE;
    p->next_slot = -1;
    p->next_w = width;
    p->next_h = height;
    p->next_sw = shown_w ? shown_w : width;
    p->next_sh = shown_h ? shown_h : height;
    p->next_fit = fit;
    p->fresh = true;
    p->received++;
    p->next_ready_ns = rl_clock_ns();
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
    rl_gpu_floor_frame();
}

bool rl_present_slot_begin(rl_present *p, uint32_t w, uint32_t h, rl_present_slot *out)
{
    rl_capture_present_begin();
    if (!p || !w || !h)
        return false;
    pthread_mutex_lock(&p->lock);
    if (!p->gpu_frames) {
        p->gpu_frames = true;
        p->stale = true;
    }
    struct timespec t;
    deadline(&t, SLOT_WAIT_MS);
    int found = -1;
    for (;;) {
        for (int i = 0; i < SLOTS && found < 0; i++)
            if (p->slots[i].state == SLOT_FREE)
                found = i;
        if (found >= 0 || p->quit || pthread_cond_timedwait(&p->cond, &p->lock, &t) != 0)
            break;
    }
    if (found < 0) {
        pthread_mutex_unlock(&p->lock);
        return false;
    }
    p->slots[found].state = SLOT_FILLING;
    pthread_mutex_unlock(&p->lock);
    int32_t format = source_format(p);
    if (!image_make(p, &p->slots[found].im, w, h, format)) {
        pthread_mutex_lock(&p->lock);
        p->slots[found].state = SLOT_FREE;
        pthread_mutex_unlock(&p->lock);
        return false;
    }
    out->image = p->slots[found].im.image;
    out->format = format;
    out->index = (uint32_t)found;
    return true;
}

void rl_present_slot_abort(rl_present *p, const rl_present_slot *s)
{
    pthread_mutex_lock(&p->lock);
    p->slots[s->index].state = SLOT_FREE;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
}

void rl_present_slot_end(rl_present *p, const rl_present_slot *s, uint32_t shown_w, uint32_t shown_h)
{
    rl_present_slot_end_after(p, s, shown_w, shown_h, 0, 0, false);
}

void rl_present_slot_end_after(rl_present *p, const rl_present_slot *s, uint32_t shown_w, uint32_t shown_h,
                               uint64_t semaphore, uint64_t value, bool host_signaled)
{
    pthread_mutex_lock(&p->lock);
    slot *sl = &p->slots[s->index];
    sl->wait_sem = (rlvk_handle)(uintptr_t)semaphore;
    sl->wait_value = value;
    sl->wait_cpu = host_signaled && semaphore;
    if (p->fresh)
        p->stats.replaced++;
    if (p->next_slot >= 0)
        p->slots[p->next_slot].state = SLOT_FREE;
    sl->state = SLOT_READY;
    p->next_slot = (int)s->index;
    p->next_w = sl->im.w;
    p->next_h = sl->im.h;
    p->next_sw = shown_w ? shown_w : sl->im.w;
    p->next_sh = shown_h ? shown_h : sl->im.h;
    p->next_fit = p->fit;
    p->fresh = true;
    p->received++;
    p->next_ready_ns = rl_clock_ns();
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
    rl_gpu_floor_frame();
    rl_capture_present_end();
}

void rl_present_set_fit(rl_present *p, bool fit)
{
    pthread_mutex_lock(&p->lock);
    p->fit = fit;
    pthread_mutex_unlock(&p->lock);
}

bool rl_present_shown(rl_present *p, rl_present_rect *at, uint32_t *frame_w, uint32_t *frame_h)
{
    pthread_mutex_lock(&p->lock);
    bool ok = p->shown_at.w && p->shown_at.h;
    *at = p->shown_at;
    *frame_w = p->shown_fw;
    *frame_h = p->shown_fh;
    pthread_mutex_unlock(&p->lock);
    return ok;
}

void rl_present_surface(rl_present *p, bool have)
{
    if (!p)
        return;
    pthread_mutex_lock(&p->lock);
    p->surface_ok = have;
    if (have)
        p->retry = true;
    pthread_cond_broadcast(&p->cond);
    uint64_t start = rl_clock_ns();
    bool warned = false;
    while (!have && p->holds) {
        struct timespec t;
        deadline(&t, 1000);
        pthread_cond_timedwait(&p->cond, &p->lock, &t);
        if (!warned && p->holds && rl_clock_ns() - start > 1000000000ull) {
            warned = true;
            rl_log(RL_LOG_WARN, "present", "still waiting for the presenter to let the surface go");
        }
    }
    pthread_mutex_unlock(&p->lock);
}

void rl_present_stats_get(rl_present *p, rl_present_stats *out)
{
    pthread_mutex_lock(&p->lock);
    *out = p->stats;
    pthread_mutex_unlock(&p->lock);
}

uint8_t *rl_present_capture(rl_present *p, uint32_t timeout_ms, uint32_t *width, uint32_t *height)
{
    pthread_mutex_lock(&p->lock);
    free(p->captured);
    p->captured = NULL;
    p->capture_want = true;
    pthread_cond_broadcast(&p->cond);
    struct timespec t;
    deadline(&t, timeout_ms);
    while (!p->captured && pthread_cond_timedwait(&p->cond, &p->lock, &t) == 0)
        ;
    uint8_t *out = p->captured;
    p->captured = NULL;
    p->capture_want = false;
    *width = out ? p->cap_w : 0;
    *height = out ? p->cap_h : 0;
    pthread_mutex_unlock(&p->lock);
    return out;
}

void rl_present_still(rl_present *p, const uint8_t *rgb, uint32_t width, uint32_t height)
{
    if (!p)
        return;
    size_t bytes = (size_t)width * height * 3;
    pthread_mutex_lock(&p->lock);
    if (rgb && width && height && bytes > p->still_next_cap) {
        uint8_t *g = realloc(p->still_next, bytes);
        if (!g) {
            pthread_mutex_unlock(&p->lock);
            return;
        }
        p->still_next = g;
        p->still_next_cap = bytes;
    }
    p->still_on = rgb && width && height;
    if (p->still_on) {
        memcpy(p->still_next, rgb, bytes);
        p->still_next_w = width;
        p->still_next_h = height;
    }
    p->still_fresh = true;
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
}

uint8_t *rl_present_snapshot(rl_present *p, uint32_t timeout_ms, uint32_t *width, uint32_t *height)
{
    uint32_t cw = 0, ch = 0;
    uint8_t *img = rl_present_capture(p, timeout_ms, &cw, &ch);
    *width = *height = 0;
    if (!img)
        return NULL;
    pthread_mutex_lock(&p->lock);
    uint32_t t = p->cur_slot >= 0 && !p->still_shown ? RLVK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR : p->sc_transform;
    pthread_mutex_unlock(&p->lock);
    bool quarter = (t & QUARTER_TURNS) != 0;
    uint32_t lw = quarter ? ch : cw, lh = quarter ? cw : ch;
    if (!(t & (QUARTER_TURNS | RLVK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR))) {
        *width = cw;
        *height = ch;
        return img;
    }
    uint32_t *out = malloc((size_t)lw * lh * 4);
    if (!out) {
        free(img);
        return NULL;
    }
    const uint32_t *in = (const uint32_t *)(void *)img;
    for (uint32_t y = 0; y < lh; y++)
        for (uint32_t x = 0; x < lw; x++) {
            size_t at;
            if (t & RLVK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR)
                at = (size_t)x * lh + (lh - 1 - y);
            else if (t & RLVK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR)
                at = (size_t)(lw - 1 - x) * lh + y;
            else
                at = (size_t)(lh - 1 - y) * lw + (lw - 1 - x);
            out[(size_t)y * lw + x] = in[at];
        }
    free(img);
    *width = lw;
    *height = lh;
    return (uint8_t *)out;
}
