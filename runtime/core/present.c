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
#include "present.h"

#include <relift/post.h>

#include "core.h"
#include "postgpu.h"
#include "vkdev.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_IMAGES 8
#define SLOTS 3
#define IDLE_WAIT_MS 250
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
};

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
    rl_vkdev_queue_lock();
    p->d->vkDeviceWaitIdle(p->d->device);
    rl_vkdev_queue_unlock();
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

static int32_t pick_mode(rl_present *p)
{
    int32_t modes[16];
    uint32_t n = 16;
    if (p->d->vkGetPhysicalDeviceSurfacePresentModesKHR(p->d->phys, p->surface, &n, modes) < RLVK_SUCCESS)
        n = 0;
    const char *env = getenv("RELIFT_PRESENT_MODE");
    int32_t want = env && !strcmp(env, "fifo") ? RLVK_PRESENT_MODE_FIFO_KHR : RLVK_PRESENT_MODE_MAILBOX_KHR;
    for (uint32_t i = 0; i < n; i++)
        if (modes[i] == want)
            return want;
    return RLVK_PRESENT_MODE_FIFO_KHR;
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
           mode == RLVK_PRESENT_MODE_MAILBOX_KHR ? "mailbox" : "fifo", transform, caps.currentTransform);
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
    rl_present_place(psw, psh, lw, lh, rl_post_integer(pfit), &logical);
    rl_present_turn(&logical, lw, lh, t, &at);
    if (!still) {
        pthread_mutex_lock(&p->lock);
        p->shown_at = logical;
        p->shown_fw = p->work_w;
        p->shown_fh = p->work_h;
        pthread_mutex_unlock(&p->lock);
    }
    int sampling = rl_post_sampling(cw, ch, at.w, at.h);
    bool sharp = sampling == RL_POST_SAMPLE_SHARP;
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
    if (r != RLVK_SUCCESS)
        return fail("vkBeginCommandBuffer", r);
    rlvk_handle sc = p->images[index];
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
    layout(p, sc, RLVK_IMAGE_LAYOUT_UNDEFINED, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, wr, xfer, xfer);
    rlvk_clear_color black = { .float32 = { 0, 0, 0, 1 } };
    rlvk_image_subresource_range all = { RLVK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    d->vkCmdClearColorImage(p->cmd, sc, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &all);
    layout(p, sc, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, wr, wr, xfer, xfer);
    if (at.w && at.h) {
        if (sharp) {
            layout(p, p->pre.image, RLVK_IMAGE_LAYOUT_UNDEFINED, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, wr, xfer,
                   xfer);
            blit(p, img, cw, ch, p->pre.image, 0, 0, p->pre.w, p->pre.h, RLVK_FILTER_NEAREST);
            layout(p, p->pre.image, RLVK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, RLVK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, wr,
                   rd, xfer, xfer);
            blit(p, p->pre.image, p->pre.w, p->pre.h, sc, at.x, at.y, at.w, at.h, RLVK_FILTER_LINEAR);
        } else {
            blit(p, img, cw, ch, sc, at.x, at.y, at.w, at.h,
                 sampling == RL_POST_SAMPLE_LINEAR ? RLVK_FILTER_LINEAR : RLVK_FILTER_NEAREST);
        }
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

static bool draw(rl_present *p, bool capture)
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
    if (capture && !p->sc_readable) {
        pthread_mutex_lock(&p->lock);
        p->capture_want = false;
        pthread_cond_broadcast(&p->cond);
        pthread_mutex_unlock(&p->lock);
        capture = false;
    }
    d->vkWaitForFences(d->device, 1, &p->fence, 1, ~(uint64_t)0);
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
    if (!record(p, index, capture)) {
        drop_surface(p);
        p->create_failed = true;
        return false;
    }
    d->vkResetFences(d->device, 1, &p->fence);
    uint32_t wait_stage = RLVK_PIPELINE_STAGE_TRANSFER_BIT;
    rlvk_submit_info sub = {
        .sType = RLVK_ST_SUBMIT_INFO,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &p->acquired,
        .pWaitDstStageMask = &wait_stage,
        .commandBufferCount = 1,
        .pCommandBuffers = &p->cmd,
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &p->done[index],
    };
    rlvk_present_info pi = {
        .sType = RLVK_ST_PRESENT_INFO_KHR,
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &p->done[index],
        .swapchainCount = 1,
        .pSwapchains = &p->swapchain,
        .pImageIndices = &index,
    };
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
    if (pr == RLVK_ERROR_OUT_OF_DATE_KHR || (pr == RLVK_SUBOPTIMAL_KHR && caps_moved(p)))
        p->stale = true;
    else if (pr == RLVK_ERROR_SURFACE_LOST_KHR)
        drop_surface(p);
    else if (pr != RLVK_SUCCESS)
        fail("vkQueuePresentKHR", pr);
    count_present(p, rl_clock_ns() - t0);
    if (capture) {
        d->vkWaitForFences(d->device, 1, &p->fence, 1, ~(uint64_t)0);
        keep_capture(p);
    }
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

static void *thread_main(void *arg)
{
    rl_present *p = arg;
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
        bool redo = (p->have_frame || p->still_shown) && (p->stale || moved);
        bool still_due = p->still_fresh;
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
                p->have_frame = true;
            }
            bool capture = p->capture_want;
            p->holds = true;
            pthread_mutex_unlock(&p->lock);
            bool shown = draw(p, capture);
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
        deadline(&t, IDLE_WAIT_MS);
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
    for (uint32_t i = 0; i < SLOTS; i++)
        image_free(p, &p->slots[i].im);
    rl_post_gpu_free(p->post);
    rl_vkdev_buf_free(d, &p->stage);
    rl_vkdev_buf_free(d, &p->readback);
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
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
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
    pthread_mutex_lock(&p->lock);
    slot *sl = &p->slots[s->index];
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
    pthread_cond_broadcast(&p->cond);
    pthread_mutex_unlock(&p->lock);
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
