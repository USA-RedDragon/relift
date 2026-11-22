/**
 * @file vk_swap.c
 * @brief Guest swapchains: the guest renders into images the layer owns, and
 * each present blits the image into a host swapchain image.
 *
 * A Switch title never sees its window change size, so a guest swapchain
 * keeps the extent and format it was created with. The host swapchain behind
 * it follows the host surface: it is recreated whenever the surface reports
 * out of date, suboptimal with a new extent or transform, or the provider's
 * extent generation changes, and the present letterboxes the guest image into
 * it. The host swapchain asks for no rotation: on a display turned from its
 * native orientation (Android) the presentation engine rotates, and the
 * suboptimal result that asks for pre-rotation does not recreate it. The
 * guest never gets VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR from a
 * resize.
 *
 * A host surface can be lost: Android destroys the window's surface when the
 * app goes to the background and makes a new one when it comes back. The
 * guest never sees that either. The swapchain drops its host swapchain, and
 * acquire waits until the provider can make a new host surface for the same
 * window, then builds a host swapchain on it; a present in between drops the
 * frame.
 *
 * Presents are paced like the console's FIFO queue: each frame is shown at
 * the first vsync of vi_vsync_after that follows its present and the frame
 * before it, and an image comes back to vkAcquireNextImageKHR only when the
 * frame after it is shown. A game that renders faster than 60 frames a
 * second waits in acquire, as it waits in DequeueBuffer on the console.
 *
 * Guest swapchain handles are ids from this file (0x57 in the top byte).
 */
#include "vkf.h"
#include "vi.h"
#include "vk_overlay.h"

#include <relift/touchpad.h>

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SWAP_MAX 8u
#define HOST_MAX 16u
#define OFFSCREEN_IMAGES 2u
#define RETIRED_MAX 64u
#define SWAP_TAG 0x5700000000000000ull

typedef struct vkf_swap {
    uint64_t id;
    vkf_obj *dobj;
    VkDevice dev;
    const vkf_fns *fns;
    VkPhysicalDevice phys;
    const vkf_fns *ifns;
    VkSurfaceKHR surface;
    /** @brief The host surface presents go to: the guest's surface, or one made again after the host lost it. */
    VkSurfaceKHR hsurf;
    /** @brief hsurf was made by this file and is destroyed with the swapchain. */
    bool hsurf_own;
    /** @brief The host surface was lost: acquire makes a new one before a host swapchain. */
    bool lost;
    /** @brief While lost: when to ask the provider for a new host surface next (monotonic ns). */
    int64_t renew_at;
    VkInstance inst;
    const vkf_fns *gfns;
    VkFormat format;
    VkColorSpaceKHR color_space;
    VkExtent2D extent;
    VkPresentModeKHR mode;
    VkFilter filter;
    uint32_t n;
    VkImage img[SWAP_MAX];
    VkDeviceMemory mem[SWAP_MAX];
    VkFence done[SWAP_MAX];
    VkCommandBuffer cmd[SWAP_MAX];
    bool busy[SWAP_MAX];
    bool held[SWAP_MAX];
    uint32_t host_idx[SWAP_MAX];
    int64_t free_at[SWAP_MAX];
    int64_t last_shown;
    uint32_t shown;
    bool any_shown;
    uint32_t next;
    VkSwapchainKHR host;
    uint32_t host_n;
    VkImage host_img[HOST_MAX];
    VkDeviceMemory host_mem[HOST_MAX];
    bool offscreen;
    VkSemaphore ready[HOST_MAX];
    VkExtent2D host_extent;
    VkSurfaceTransformFlagBitsKHR host_hint;
    VkSemaphore retired[RETIRED_MAX];
    uint32_t nretired;
    bool stale;
    bool old;
    uint64_t gen;
    uint32_t creations;
    VkCommandPool pool;
    uint32_t pool_family;
    VkBuffer cap_buf;
    VkDeviceMemory cap_mem;
    void *cap_ptr;
    VkCommandBuffer cap_cmd;
    VkFence cap_fence;
    /** @brief The writer thread still reads cap_ptr (vkf_capture_queue). */
    bool cap_busy;
    /** @brief 1 + the guest image the last capture copies from, until acquire waits for that copy; 0 for none. */
    uint32_t cap_img;
    vkf_overlay *ov;
    VkSemaphore again;
    /** @brief The id of the last host present (VK_KHR_present_id). */
    uint64_t present_id;
    /** @brief Repaints of a paused frame, for tests. */
    uint32_t repaints;
    pthread_mutex_t lock;
} vkf_swap;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static vkf_map *g_swaps;
static uint64_t g_next_id = 1;

static vkf_swap *swap_of(uint64_t id, const vkf_obj *dobj)
{
    pthread_mutex_lock(&g_lock);
    vkf_swap *s = (id & 0xFF00000000000000ull) == SWAP_TAG && g_swaps ? vkf_map_get(g_swaps, id) : NULL;
    pthread_mutex_unlock(&g_lock);
    return s && (!dobj || s->dobj == dobj) ? s : NULL;
}

#define DFN(s, name) VKF_FN((s)->fns, name)
#define IFN(s, name) VKF_FN((s)->ifns, name)

static int64_t now_ns(void)
{
    if (hos_fixed())
        return (int64_t)hos_clock_ns();
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

static void nap(void)
{
    struct timespec d = { 0, 5000000 };
    nanosleep(&d, NULL);
}

void vkf_surface_extent(uint64_t surface, uint32_t guest[2], uint32_t host[2], uint64_t *generation)
{
    const rl_vk_surface_provider *p = vkf.cfg.surfaces;
    uint64_t layer = 0;
    if (!rl_vi_render_layer(&layer) || !rl_vi_layer_size(layer, &guest[0], &guest[1]))
        vi_get_resolution(&guest[0], &guest[1]);
    if (p && p->extent)
        p->extent(p->user, surface, guest, host, generation);
    else {
        host[0] = guest[0];
        host[1] = guest[1];
        *generation = 0;
    }
}

/** @brief Largest image side of a vi surface. */
#define VI_MAX_EXTENT 4096u

/**
 * @brief The capabilities the console's swapchain layer reports for a vi
 * surface, read from NvSwapchain::GetPhysicalDeviceSurfaceCapabilitiesKHR in
 * the SDK's vulkan.nso (build 590d76cc419d9397b6859527d4262ce7deaa54e8,
 * verified): 2 to 3 images, extents 1x1 to 4096x4096 with the window's size
 * as the current extent (0xFFFFFFFF when a side is outside that range), one
 * array layer, INHERIT as the only and current transform, opaque alpha, and
 * transfer, sampled, storage, color and input attachment usage.
 */
void vkf_vi_caps(uint64_t surface, VkSurfaceCapabilitiesKHR *caps)
{
    uint32_t guest[2], host[2];
    uint64_t gen;
    vkf_surface_extent(surface, guest, host, &gen);
    bool fits = guest[0] - 1u < VI_MAX_EXTENT && guest[1] - 1u < VI_MAX_EXTENT;
    caps->currentExtent = fits ? (VkExtent2D){ guest[0], guest[1] } : (VkExtent2D){ UINT32_MAX, UINT32_MAX };
    caps->minImageExtent = (VkExtent2D){ 1, 1 };
    caps->maxImageExtent = (VkExtent2D){ VI_MAX_EXTENT, VI_MAX_EXTENT };
    caps->minImageCount = 2;
    caps->maxImageCount = 3;
    caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_INHERIT_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT |
                                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
}

uint64_t vkh_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, c->x[0]);
    PFN_vkGetPhysicalDeviceSurfaceCapabilitiesKHR fn = VKF_FN(k.fns, vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    VkSurfaceCapabilitiesKHR *caps = vkf_ptr(&k, c->x[2]);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (caps && vkf_offscreen()) {
        memset(caps, 0, sizeof *caps);
        r = VK_SUCCESS;
    } else if (caps) {
        r = fn(pd, (VkSurfaceKHR)(uintptr_t)c->x[1], caps);
    }
    if (r >= 0)
        vkf_vi_caps(c->x[1], caps);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetPhysicalDeviceSurfaceCapabilitiesKHR, r);
    return vkf_end(&k);
}

uint64_t vkh_vkGetPhysicalDeviceSurfaceCapabilities2KHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceSurfaceCapabilities2KHR, c->x[0]);
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR fn = VKF_FN(k.fns, vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkGetPhysicalDeviceSurfaceCapabilities2KHR);
    const VkPhysicalDeviceSurfaceInfo2KHR *info = vkg_ain_VkPhysicalDeviceSurfaceInfo2KHR(&k, c->x[1], 1);
    VkSurfaceCapabilities2KHR *caps = vkg_ain_VkSurfaceCapabilities2KHR(&k, c->x[2], 1);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (info && caps && vkf_offscreen()) {
        memset(&caps->surfaceCapabilities, 0, sizeof caps->surfaceCapabilities);
        r = VK_SUCCESS;
    } else if (info && caps) {
        r = fn(pd, info, caps);
    }
    if (r >= 0) {
        vkf_vi_caps((uint64_t)(uintptr_t)info->surface, &caps->surfaceCapabilities);
        vkg_aout_VkSurfaceCapabilities2KHR(&k, c->x[2], caps, 1);
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetPhysicalDeviceSurfaceCapabilities2KHR, r);
    return vkf_end(&k);
}

uint64_t vkh_vkGetPhysicalDeviceSurfacePresentModesKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceSurfacePresentModesKHR, c->x[0]);
    uint64_t gcount = c->x[2], gmodes = c->x[3];
    VkResult r = VK_SUCCESS;
    if (!gcount)
        r = VK_ERROR_INITIALIZATION_FAILED;
    else if (!gmodes)
        rl_st32(mem, gcount, 1);
    else if (!vkf_g32(&k, gcount))
        r = VK_INCOMPLETE;
    else {
        rl_st32(mem, gmodes, VK_PRESENT_MODE_FIFO_KHR);
        rl_st32(mem, gcount, 1);
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetPhysicalDeviceSurfacePresentModesKHR, r);
    return vkf_end(&k);
}

static const VkSurfaceFormatKHR vi_formats[] = {
    { VK_FORMAT_R8G8B8A8_SRGB, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
    { VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR },
};

uint64_t vkh_vkGetPhysicalDeviceSurfaceFormatsKHR(rl_ctx *restrict c, uint8_t *mem)
{
    const VkSurfaceFormatKHR *formats = vi_formats;
    const uint32_t n = sizeof vi_formats / sizeof vi_formats[0];
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceSurfaceFormatsKHR, c->x[0]);
    uint64_t gcount = c->x[2], glist = c->x[3];
    VkResult r = VK_SUCCESS;
    if (!gcount)
        r = VK_ERROR_INITIALIZATION_FAILED;
    else if (!glist)
        rl_st32(mem, gcount, n);
    else {
        uint32_t cap = vkf_g32(&k, gcount), w = cap < n ? cap : n;
        for (uint32_t i = 0; i < w; i++) {
            rl_st32(mem, glist + 8u * i, (uint32_t)formats[i].format);
            rl_st32(mem, glist + 8u * i + 4u, (uint32_t)formats[i].colorSpace);
        }
        rl_st32(mem, gcount, w);
        r = w < n ? VK_INCOMPLETE : VK_SUCCESS;
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetPhysicalDeviceSurfaceFormatsKHR, r);
    return vkf_end(&k);
}

uint64_t vkh_vkGetPhysicalDeviceSurfaceFormats2KHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceSurfaceFormats2KHR, c->x[0]);
    PFN_vkGetPhysicalDeviceSurfaceFormats2KHR fn = VKF_FN(k.fns, vkGetPhysicalDeviceSurfaceFormats2KHR);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkGetPhysicalDeviceSurfaceFormats2KHR);
    const VkPhysicalDeviceSurfaceInfo2KHR *info = vkg_ain_VkPhysicalDeviceSurfaceInfo2KHR(&k, c->x[1], 1);
    uint32_t *count = vkf_ptr(&k, c->x[2]);
    uint64_t cap = count ? *count : 0;
    VkSurfaceFormat2KHR *list = vkg_ain_VkSurfaceFormat2KHR(&k, c->x[3], cap);
    VkResult r;
    if (!vkf_offscreen())
        r = fn(pd, info, count, list);
    else if (!info || !count)
        r = VK_ERROR_INITIALIZATION_FAILED;
    else {
        const uint32_t n = sizeof vi_formats / sizeof vi_formats[0];
        uint32_t w = !list ? n : cap < n ? (uint32_t)cap : n;
        for (uint32_t i = 0; list && i < w; i++)
            list[i].surfaceFormat = vi_formats[i];
        *count = w;
        r = list && w < n ? VK_INCOMPLETE : VK_SUCCESS;
    }
    if (r >= 0 && list) {
        uint64_t w = *count;
        vkg_aout_VkSurfaceFormat2KHR(&k, c->x[3], list, w < cap ? w : cap);
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetPhysicalDeviceSurfaceFormats2KHR, r);
    return vkf_end(&k);
}

static uint32_t memory_type(const vkf_dev *d, uint32_t bits)
{
    for (uint32_t i = 0; i < d->type_count; i++)
        if ((bits & (1u << i)) && (d->type_flags[i] & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            return i;
    for (uint32_t i = 0; i < d->type_count; i++)
        if (bits & (1u << i))
            return i;
    return UINT32_MAX;
}

static void wait_all(vkf_swap *s)
{
    for (uint32_t i = 0; i < s->n; i++)
        if (s->busy[i]) {
            DFN(s, vkWaitForFences)(s->dev, 1, &s->done[i], VK_TRUE, UINT64_MAX);
            DFN(s, vkResetFences)(s->dev, 1, &s->done[i]);
            s->busy[i] = false;
        }
}

static void retire_semaphores(vkf_swap *s)
{
    for (uint32_t i = 0; i < s->host_n; i++) {
        if (s->nretired == RETIRED_MAX) {
            vkf_device_wait_idle(s->dobj->dev, s->fns, s->dev);
            for (uint32_t j = 0; j < s->nretired; j++)
                DFN(s, vkDestroySemaphore)(s->dev, s->retired[j], NULL);
            s->nretired = 0;
        }
        s->retired[s->nretired++] = s->ready[i];
        s->ready[i] = NULL;
    }
    s->host_n = 0;
}

static VkSurfaceFormatKHR pick_format(vkf_swap *s)
{
    uint32_t n = 0;
    IFN(s, vkGetPhysicalDeviceSurfaceFormatsKHR)(s->phys, s->hsurf, &n, NULL);
    VkSurfaceFormatKHR *f = calloc(n ? n : 1, sizeof *f);
    if (!f)
        abort();
    IFN(s, vkGetPhysicalDeviceSurfaceFormatsKHR)(s->phys, s->hsurf, &n, f);
    bool srgb = s->format == VK_FORMAT_B8G8R8A8_SRGB || s->format == VK_FORMAT_R8G8B8A8_SRGB ||
                s->format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    VkSurfaceFormatKHR pick = n ? f[0] : (VkSurfaceFormatKHR){ VK_FORMAT_B8G8R8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR };
    int best = -1;
    for (uint32_t i = 0; i < n; i++) {
        VkFormatProperties fp;
        IFN(s, vkGetPhysicalDeviceFormatProperties)(s->phys, f[i].format, &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_DST_BIT))
            continue;
        bool fs = f[i].format == VK_FORMAT_B8G8R8A8_SRGB || f[i].format == VK_FORMAT_R8G8B8A8_SRGB;
        bool fu = f[i].format == VK_FORMAT_B8G8R8A8_UNORM || f[i].format == VK_FORMAT_R8G8B8A8_UNORM;
        int score = f[i].format == s->format ? 3 : (srgb ? fs : fu) ? 2 : 1;
        if (f[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
            score = 0;
        if (score > best) {
            best = score;
            pick = f[i];
        }
    }
    free(f);
    return pick;
}

static VkPresentModeKHR pick_mode(vkf_swap *s)
{
    uint32_t n = 0;
    IFN(s, vkGetPhysicalDeviceSurfacePresentModesKHR)(s->phys, s->hsurf, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16)
        n = 16;
    IFN(s, vkGetPhysicalDeviceSurfacePresentModesKHR)(s->phys, s->hsurf, &n, m);
    for (uint32_t i = 0; i < n; i++)
        if (m[i] == s->mode)
            return s->mode;
    return VK_PRESENT_MODE_FIFO_KHR;
}

static bool have_host(const vkf_swap *s) { return s->offscreen ? s->host_n != 0 : s->host != NULL; }

static void offscreen_destroy(vkf_swap *s)
{
    for (uint32_t i = 0; i < s->host_n; i++) {
        DFN(s, vkDestroyImage)(s->dev, s->host_img[i], NULL);
        DFN(s, vkFreeMemory)(s->dev, s->host_mem[i], NULL);
    }
    s->host_n = 0;
}

/** @brief Makes the offscreen host images match the host extent. VK_NOT_READY while it has no area. Lock held. */
static VkResult offscreen_create(vkf_swap *s)
{
    uint32_t guest[2], host[2];
    uint64_t gen;
    vkf_surface_extent((uint64_t)(uintptr_t)s->surface, guest, host, &gen);
    if (!host[0] || !host[1])
        return VK_NOT_READY;
    wait_all(s);
    offscreen_destroy(s);
    const vkf_dev *d = s->dobj->dev;
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = s->format,
        .extent = { host[0], host[1], 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    for (uint32_t i = 0; i < OFFSCREEN_IMAGES; i++) {
        VkResult r = DFN(s, vkCreateImage)(s->dev, &ii, NULL, &s->host_img[i]);
        if (r < 0)
            goto fail;
        s->host_n = i + 1;
        VkMemoryRequirements req;
        DFN(s, vkGetImageMemoryRequirements)(s->dev, s->host_img[i], &req);
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = memory_type(d, req.memoryTypeBits),
        };
        s->host_mem[i] = NULL;
        r = DFN(s, vkAllocateMemory)(s->dev, &ai, NULL, &s->host_mem[i]);
        if (r >= 0)
            r = DFN(s, vkBindImageMemory)(s->dev, s->host_img[i], s->host_mem[i], 0);
        if (r < 0) {
            rl_log(RL_LOG_ERROR, "vk", "offscreen host image %ux%u: %d", host[0], host[1], r);
            goto fail;
        }
    }
    s->host_extent = (VkExtent2D){ host[0], host[1] };
    s->gen = gen;
    s->stale = false;
    s->creations++;
    return VK_SUCCESS;
fail:
    offscreen_destroy(s);
    return VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

/** @brief Drops the host swapchain of a lost host surface. Lock held. */
static void lose(vkf_swap *s)
{
    if (rl_once(0x4c4f535400000000ull ^ s->id))
        rl_log(RL_LOG_INFO, "vk", "the host surface of swapchain 0x%llx was lost; a new one is made when the window has one again",
               (unsigned long long)s->id);
    wait_all(s);
    vkf_device_wait_idle(s->dobj->dev, s->fns, s->dev);
    if (s->ov) {
        vkf_overlay_free(s->ov);
        s->ov = NULL;
    }
    if (s->host) {
        vkf_flip_forget(s->host);
        DFN(s, vkDestroySwapchainKHR)(s->dev, s->host, NULL);
        s->host = NULL;
    }
    retire_semaphores(s);
    s->lost = true;
}

/** @brief A new host surface for the guest's window after the host lost it; VK_NOT_READY while the window has none.
 * Lock held. */
static VkResult renew(vkf_swap *s)
{
    const rl_vk_surface_provider *p = vkf.cfg.surfaces;
    if (!p || !p->recreate)
        return VK_ERROR_SURFACE_LOST_KHR;
    int64_t now = now_ns();
    if (now < s->renew_at)
        return VK_NOT_READY;
    s->renew_at = now + 100000000ll;
    uint64_t fresh = 0;
    void *gipa;
    memcpy(&gipa, &vkf.gipa, sizeof gipa);
    int32_t r = p->recreate(p->user, s->inst, gipa, (uint64_t)(uintptr_t)s->surface, &fresh);
    if (r < 0 || !fresh)
        return VK_NOT_READY;
    if (s->hsurf_own)
        VKF_FN(s->gfns, vkDestroySurfaceKHR)(s->inst, s->hsurf, NULL);
    s->hsurf = (VkSurfaceKHR)(uintptr_t)fresh;
    s->hsurf_own = true;
    s->lost = false;
    s->renew_at = 0;
    s->stale = true;
    rl_log(RL_LOG_INFO, "vk", "swapchain 0x%llx has a new host surface", (unsigned long long)s->id);
    return VK_SUCCESS;
}

/** @brief Makes the host swapchain match the host surface. The blit does not rotate, so the presentation engine turns
 * the image when the display is not in its native orientation. VK_NOT_READY while the surface has no area. Lock
 * held. */
static VkResult host_create(vkf_swap *s)
{
    if (s->offscreen)
        return offscreen_create(s);
    if (s->lost) {
        VkResult n = renew(s);
        if (n < 0 || n == VK_NOT_READY)
            return n;
    }
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = IFN(s, vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(s->phys, s->hsurf, &caps);
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        lose(s);
        return VK_NOT_READY;
    }
    if (r < 0)
        return r;
    uint32_t guest[2], host[2];
    uint64_t gen;
    vkf_surface_extent((uint64_t)(uintptr_t)s->surface, guest, host, &gen);
    VkExtent2D e = caps.currentExtent;
    VkSurfaceTransformFlagBitsKHR transform = caps.currentTransform;
    if (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
        transform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    if (e.width == UINT32_MAX) {
        e.width = host[0] < caps.minImageExtent.width ? caps.minImageExtent.width : host[0];
        e.height = host[1] < caps.minImageExtent.height ? caps.minImageExtent.height : host[1];
        if (e.width > caps.maxImageExtent.width)
            e.width = caps.maxImageExtent.width;
        if (e.height > caps.maxImageExtent.height)
            e.height = caps.maxImageExtent.height;
    }
    if (!e.width || !e.height)
        return VK_NOT_READY;
    VkSurfaceFormatKHR fmt = pick_format(s);
    uint32_t count = caps.minImageCount + 1;
    if (caps.maxImageCount && count > caps.maxImageCount)
        count = caps.maxImageCount;
    if (count > HOST_MAX)
        count = HOST_MAX;
    VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    if (!(caps.supportedCompositeAlpha & alpha))
        alpha = (VkCompositeAlphaFlagBitsKHR)(caps.supportedCompositeAlpha & -caps.supportedCompositeAlpha);
    VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (caps.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    VkSwapchainCreateInfoKHR ci = {
        .sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR,
        .surface = s->hsurf,
        .minImageCount = count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = e,
        .imageArrayLayers = 1,
        .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = transform,
        .compositeAlpha = alpha,
        .presentMode = pick_mode(s),
        .clipped = VK_TRUE,
        .oldSwapchain = s->host,
    };
    VkSwapchainKHR sw = NULL;
    r = DFN(s, vkCreateSwapchainKHR)(s->dev, &ci, NULL, &sw);
    if (r == VK_ERROR_SURFACE_LOST_KHR || r == VK_ERROR_NATIVE_WINDOW_IN_USE_KHR) {
        lose(s);
        return VK_NOT_READY;
    }
    if (r < 0) {
        rl_log(RL_LOG_ERROR, "vk", "host swapchain %ux%u: %d", e.width, e.height, r);
        return r;
    }
    uint32_t n = 0;
    DFN(s, vkGetSwapchainImagesKHR)(s->dev, sw, &n, NULL);
    if (n > HOST_MAX)
        n = HOST_MAX;
    VkSemaphore ready[HOST_MAX] = { 0 };
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < n && r >= 0; i++)
        r = DFN(s, vkCreateSemaphore)(s->dev, &sci, NULL, &ready[i]);
    if (r < 0) {
        rl_log(RL_LOG_ERROR, "vk", "host swapchain semaphore: %d", r);
        for (uint32_t i = 0; i < n; i++)
            if (ready[i])
                DFN(s, vkDestroySemaphore)(s->dev, ready[i], NULL);
        DFN(s, vkDestroySwapchainKHR)(s->dev, sw, NULL);
        s->stale = true;
        return r;
    }
    wait_all(s);
    if (s->ov) {
        vkf_device_wait_idle(s->dobj->dev, s->fns, s->dev);
        vkf_overlay_free(s->ov);
        s->ov = NULL;
    }
    if (s->host) {
        vkf_flip_forget(s->host);
        DFN(s, vkDestroySwapchainKHR)(s->dev, s->host, NULL);
    }
    retire_semaphores(s);
    s->host = sw;
    DFN(s, vkGetSwapchainImagesKHR)(s->dev, sw, &n, s->host_img);
    for (uint32_t i = 0; i < n; i++)
        s->ready[i] = ready[i];
    s->host_n = n;
    s->host_extent = e;
    s->host_hint = caps.currentTransform;
    if (rl_touchpad_wanted() && (usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)) {
        uint32_t vw = 0, vh = 0;
        rl_touchpad_view_size(&vw, &vh);
        if (!vw || !vh)
            rl_touchpad_view(e.width, e.height, 0, NULL);
        s->ov = vkf_overlay_new(s->dev, s->fns, s->dobj->dev, fmt.format, e, s->host_img, n);
    }
    s->gen = gen;
    s->stale = false;
    s->creations++;
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "vk", "host swapchain %ux%u, format %d, %u images, for guest %ux%u", e.width, e.height,
               fmt.format, n, s->extent.width, s->extent.height);
    return VK_SUCCESS;
}

/** @brief A suboptimal host swapchain needs a new one: not when the surface only asks for the rotation the
 * presentation engine already does. Lock held. */
static bool suboptimal_stale(vkf_swap *s)
{
    VkSurfaceCapabilitiesKHR caps;
    if (IFN(s, vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(s->phys, s->hsurf, &caps) < 0)
        return true;
    if (caps.currentTransform != s->host_hint)
        return true;
    return caps.currentExtent.width != UINT32_MAX && (caps.currentExtent.width != s->host_extent.width ||
                                                      caps.currentExtent.height != s->host_extent.height);
}

static void swap_free(vkf_swap *s)
{
    wait_all(s);
    if (s->host || s->nretired || s->host_n)
        vkf_device_wait_idle(s->dobj->dev, s->fns, s->dev);
    for (uint32_t i = 0; i < s->n; i++) {
        DFN(s, vkDestroyImage)(s->dev, s->img[i], NULL);
        DFN(s, vkFreeMemory)(s->dev, s->mem[i], NULL);
        DFN(s, vkDestroyFence)(s->dev, s->done[i], NULL);
    }
    if (s->cap_buf)
        vkf_capture_flush();
    if (s->cap_fence)
        DFN(s, vkDestroyFence)(s->dev, s->cap_fence, NULL);
    if (s->cap_buf)
        DFN(s, vkDestroyBuffer)(s->dev, s->cap_buf, NULL);
    if (s->cap_mem)
        DFN(s, vkFreeMemory)(s->dev, s->cap_mem, NULL);
    if (s->pool)
        DFN(s, vkDestroyCommandPool)(s->dev, s->pool, NULL);
    vkf_overlay_free(s->ov);
    if (s->again)
        DFN(s, vkDestroySemaphore)(s->dev, s->again, NULL);
    if (s->host) {
        vkf_flip_forget(s->host);
        DFN(s, vkDestroySwapchainKHR)(s->dev, s->host, NULL);
    }
    if (s->hsurf_own)
        VKF_FN(s->gfns, vkDestroySurfaceKHR)(s->inst, s->hsurf, NULL);
    if (s->offscreen)
        offscreen_destroy(s);
    retire_semaphores(s);
    for (uint32_t j = 0; j < s->nretired; j++)
        DFN(s, vkDestroySemaphore)(s->dev, s->retired[j], NULL);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

/**
 * @brief Host usage for a swapchain image beyond what the guest asked: every
 * usage a vi surface supports that the host format allows. The console's
 * NvSwapchain::CreateSwapchainKHR (vulkan.nso) creates its images with the
 * guest's imageUsage alone (verified), and games still sample swapchain
 * images they created only as color attachments, which the console's
 * driver accepts (inferred from shipped games doing it).
 */
static VkImageUsageFlags console_usage(VkFormatFeatureFlags f)
{
    VkImageUsageFlags u = 0;
    if (f & VK_FORMAT_FEATURE_TRANSFER_DST_BIT)
        u |= VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (f & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)
        u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (f & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT)
        u |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (f & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)
        u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    return u;
}

static VkResult swap_images(vkf_swap *s, const VkSwapchainCreateInfoKHR *ci)
{
    const vkf_dev *d = s->dobj->dev;
    VkFormatProperties fp;
    IFN(s, vkGetPhysicalDeviceFormatProperties)(s->phys, ci->imageFormat, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_BLIT_SRC_BIT)) {
        rl_log(RL_LOG_ERROR, "vk", "vkCreateSwapchainKHR: format %d cannot be blitted on the host", ci->imageFormat);
        return VK_ERROR_FORMAT_NOT_SUPPORTED;
    }
    s->filter = (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR
                                                                                                 : VK_FILTER_NEAREST;
    VkImageCreateInfo ii = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = ci->imageFormat,
        .extent = { ci->imageExtent.width, ci->imageExtent.height, 1 },
        .mipLevels = 1,
        .arrayLayers = ci->imageArrayLayers ? ci->imageArrayLayers : 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | console_usage(fp.optimalTilingFeatures),
        .sharingMode = ci->imageSharingMode,
        .queueFamilyIndexCount = ci->imageSharingMode == VK_SHARING_MODE_CONCURRENT ? ci->queueFamilyIndexCount : 0,
        .pQueueFamilyIndices = ci->imageSharingMode == VK_SHARING_MODE_CONCURRENT ? ci->pQueueFamilyIndices : NULL,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    for (uint32_t i = 0; i < s->n; i++) {
        VkResult r = DFN(s, vkCreateImage)(s->dev, &ii, NULL, &s->img[i]);
        if (r < 0)
            return r;
        VkMemoryRequirements req;
        DFN(s, vkGetImageMemoryRequirements)(s->dev, s->img[i], &req);
        VkMemoryAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = memory_type(d, req.memoryTypeBits),
        };
        r = DFN(s, vkAllocateMemory)(s->dev, &ai, NULL, &s->mem[i]);
        if (r >= 0)
            r = DFN(s, vkBindImageMemory)(s->dev, s->img[i], s->mem[i], 0);
        if (r >= 0)
            r = DFN(s, vkCreateFence)(s->dev, &fci, NULL, &s->done[i]);
        if (r < 0)
            return r;
    }
    return VK_SUCCESS;
}

uint64_t vkh_vkCreateSwapchainKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkCreateSwapchainKHR, c->x[0]);
    VkSwapchainCreateInfoKHR *ci = vkg_ain_VkSwapchainCreateInfoKHR(&k, c->x[1], 1);
    uint64_t gout = c->x[3];
    if (!ci || !gout || !ci->imageExtent.width || !ci->imageExtent.height) {
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateSwapchainKHR, VK_ERROR_INITIALIZATION_FAILED);
        return vkf_end(&k);
    }
    vkf_swap *s = calloc(1, sizeof *s);
    if (!s)
        abort();
    pthread_mutex_init(&s->lock, NULL);
    s->dobj = k.obj;
    s->dev = dev;
    s->fns = k.fns;
    s->phys = k.obj->parent->host;
    s->ifns = k.obj->parent->fns;
    s->offscreen = vkf_offscreen();
    s->surface = ci->surface;
    s->hsurf = ci->surface;
    s->inst = k.obj->parent->parent->host;
    s->gfns = k.obj->parent->parent->fns;
    s->format = ci->imageFormat;
    s->color_space = ci->imageColorSpace;
    s->extent = ci->imageExtent;
    s->mode = ci->presentMode;
    s->n = ci->minImageCount < 2 ? 2 : ci->minImageCount > SWAP_MAX ? SWAP_MAX : ci->minImageCount;
    vkf_swap *old = swap_of((uint64_t)(uintptr_t)ci->oldSwapchain, k.obj);
    if (old) {
        pthread_mutex_lock(&old->lock);
        s->last_shown = old->last_shown;
        if (old->surface == s->surface && old->hsurf_own) {
            s->hsurf = old->hsurf;
            s->hsurf_own = true;
            old->hsurf_own = false;
            s->lost = old->lost;
        }
        if (old->surface == s->surface && old->host) {
            wait_all(old);
            s->host = old->host;
            old->host = NULL;
            retire_semaphores(old);
            s->stale = true;
        }
        old->old = true;
        pthread_mutex_unlock(&old->lock);
    }
    VkResult r = swap_images(s, ci);
    if (r >= 0) {
        r = host_create(s);
        if (r == VK_NOT_READY) {
            s->stale = true;
            r = VK_SUCCESS;
        }
    }
    if (r < 0) {
        swap_free(s);
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateSwapchainKHR, r);
        return vkf_end(&k);
    }
    pthread_mutex_lock(&g_lock);
    if (!g_swaps)
        g_swaps = vkf_map_new();
    s->id = SWAP_TAG | g_next_id++;
    vkf_map_put(g_swaps, s->id, s);
    pthread_mutex_unlock(&g_lock);
    vkf_s64(&k, gout, s->id);
    c->x[0] = VK_SUCCESS;
    return vkf_end(&k);
}

static void swap_forget(vkf_swap *s)
{
    pthread_mutex_lock(&g_lock);
    vkf_map_put(g_swaps, s->id, NULL);
    pthread_mutex_unlock(&g_lock);
}

uint64_t vkh_vkDestroySwapchainKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkDestroySwapchainKHR, c->x[0]);
    vkf_swap *s = swap_of(c->x[1], k.obj);
    if (s) {
        swap_forget(s);
        swap_free(s);
    } else if (c->x[1])
        rl_log(RL_LOG_ERROR, "vk", "vkDestroySwapchainKHR: 0x%llx is not a swapchain of this device",
               (unsigned long long)c->x[1]);
    return vkf_end(&k);
}

typedef struct teardown {
    const vkf_obj *dobj;
    vkf_swap *list[64];
    uint32_t n;
} teardown;

static void collect(uint64_t key, void *value, void *user)
{
    (void)key;
    teardown *t = user;
    vkf_swap *s = value;
    if (s->dobj == t->dobj && t->n < 64)
        t->list[t->n++] = s;
}

void vkf_swap_device_teardown(const vkf_obj *dobj)
{
    teardown t = { .dobj = dobj };
    pthread_mutex_lock(&g_lock);
    vkf_map_each(g_swaps, collect, &t);
    pthread_mutex_unlock(&g_lock);
    for (uint32_t i = 0; i < t.n; i++) {
        swap_forget(t.list[i]);
        swap_free(t.list[i]);
    }
}

uint64_t vkh_vkGetSwapchainImagesKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkGetSwapchainImagesKHR, c->x[0]);
    vkf_swap *s = swap_of(c->x[1], k.obj);
    uint64_t gcount = c->x[2], garray = c->x[3];
    if (!s || !gcount) {
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetSwapchainImagesKHR, VK_ERROR_UNKNOWN);
        return vkf_end(&k);
    }
    if (!garray) {
        rl_st32(mem, gcount, s->n);
        c->x[0] = VK_SUCCESS;
        return vkf_end(&k);
    }
    uint32_t cap = vkf_g32(&k, gcount), w = cap < s->n ? cap : s->n;
    for (uint32_t i = 0; i < w; i++)
        vkf_s64(&k, garray + 8u * i, (uint64_t)(uintptr_t)s->img[i]);
    rl_st32(mem, gcount, w);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetSwapchainImagesKHR, (w < s->n ? VK_INCOMPLETE : VK_SUCCESS));
    return vkf_end(&k);
}

static bool any_held(const vkf_swap *s)
{
    for (uint32_t i = 0; i < s->n; i++)
        if (s->held[i])
            return true;
    return false;
}

static VkResult swap_acquire(vkf_swap *s, uint64_t timeout, VkSemaphore sem, VkFence fence, uint32_t *index)
{
    int64_t start = now_ns(), slept = 0, host_ns = 0, fence_ns = 0;
    VkResult r = VK_SUCCESS;
    pthread_mutex_lock(&s->lock);
    for (;;) {
        if (s->old) {
            r = VK_ERROR_OUT_OF_DATE_KHR;
            break;
        }
        uint32_t guest[2], host[2];
        uint64_t gen;
        vkf_surface_extent((uint64_t)(uintptr_t)s->surface, guest, host, &gen);
        if ((!have_host(s) || s->stale || s->lost || gen != s->gen) && !any_held(s)) {
            r = host_create(s);
            if (r == VK_NOT_READY) {
                if (!timeout || (timeout != UINT64_MAX && now_ns() - start >= (int64_t)timeout)) {
                    r = timeout ? VK_TIMEOUT : VK_NOT_READY;
                    break;
                }
                pthread_mutex_unlock(&s->lock);
                nap();
                pthread_mutex_lock(&s->lock);
                continue;
            }
            if (r < 0)
                break;
        }
        if (!s->offscreen && !s->host) {
            if (!timeout || (timeout != UINT64_MAX && now_ns() - start >= (int64_t)timeout)) {
                r = timeout ? VK_TIMEOUT : VK_NOT_READY;
                break;
            }
            pthread_mutex_unlock(&s->lock);
            nap();
            pthread_mutex_lock(&s->lock);
            continue;
        }
        uint32_t i = s->n;
        for (uint32_t t = 0; t < s->n; t++) {
            uint32_t j = (s->next + t) % s->n;
            if (!s->held[j] && (i == s->n || s->free_at[j] < s->free_at[i]))
                i = j;
        }
        if (i == s->n) {
            r = VK_NOT_READY;
            break;
        }
        int64_t now = now_ns();
        if (s->free_at[i] > now) {
            if (!timeout) {
                r = VK_NOT_READY;
                break;
            }
            int64_t left = timeout == UINT64_MAX ? INT64_MAX : (int64_t)timeout - (now - start);
            if (left <= 0) {
                r = VK_TIMEOUT;
                break;
            }
            int64_t d = s->free_at[i] - now < left ? s->free_at[i] - now : left;
            pthread_mutex_unlock(&s->lock);
            if (hos_fixed()) {
                hos_sleep_thread(d);
            } else {
                struct timespec ts = { (time_t)(d / 1000000000ll), (long)(d % 1000000000ll) };
                nanosleep(&ts, NULL);
            }
            slept += now_ns() - now;
            pthread_mutex_lock(&s->lock);
            continue;
        }
        if (s->busy[i]) {
            int64_t f0 = now_ns();
            r = DFN(s, vkWaitForFences)(s->dev, 1, &s->done[i], VK_TRUE, timeout);
            fence_ns += now_ns() - f0;
            if (r != VK_SUCCESS)
                break;
            DFN(s, vkResetFences)(s->dev, 1, &s->done[i]);
            s->busy[i] = false;
        }
        if (s->cap_img == i + 1) {
            DFN(s, vkWaitForFences)(s->dev, 1, &s->cap_fence, VK_TRUE, UINT64_MAX);
            s->cap_img = 0;
        }
        uint32_t hi = 0;
        if (s->offscreen) {
            hi = i % s->host_n;
            r = VK_SUCCESS;
        } else {
            int64_t h0 = now_ns();
            r = DFN(s, vkAcquireNextImageKHR)(s->dev, s->host, timeout, sem, fence, &hi);
            host_ns += now_ns() - h0;
        }
        if (r == VK_ERROR_SURFACE_LOST_KHR) {
            lose(s);
            r = VK_ERROR_OUT_OF_DATE_KHR;
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR && !any_held(s)) {
            s->stale = true;
            continue;
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            r = VK_NOT_READY;
            break;
        }
        if (r == VK_SUBOPTIMAL_KHR) {
            if (suboptimal_stale(s))
                s->stale = true;
            r = VK_SUCCESS;
        }
        if (r != VK_SUCCESS)
            break;
        s->held[i] = true;
        s->host_idx[i] = hi;
        s->next = (i + 1) % s->n;
        *index = i;
        break;
    }
    pthread_mutex_unlock(&s->lock);
    if (vi_trace_on())
        vi_trace('A', (uint64_t)start, (uint64_t)slept, (uint64_t)host_ns | (uint64_t)fence_ns << 32);
    return r;
}

static uint32_t family_of(const vkf_dev *d, const vkf_fns *fns, VkDevice dev, VkQueue q);

/** @brief Offscreen swapchains have no host acquire to signal the guest's semaphore and fence: an empty submit does. */
static VkResult acquire_signal(vkf_call *k, const vkf_swap *s, VkSemaphore sem, VkFence fence)
{
    if (!s->offscreen || (!sem && !fence))
        return VK_SUCCESS;
    const vkf_dev *d = k->obj->dev;
    VkQueue q = NULL;
    DFN(s, vkGetDeviceQueue)(s->dev, d->nqueue_families ? d->queue_families[0] : 0, 0, &q);
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .signalSemaphoreCount = sem ? 1 : 0,
        .pSignalSemaphores = &sem,
    };
    vkf_queue_lock(k);
    VkResult r = DFN(s, vkQueueSubmit)(q, 1, &si, fence);
    if (r >= 0)
        vkf_settle(s->fns, q);
    vkf_queue_unlock(k);
    return r;
}

uint64_t vkh_vkAcquireNextImageKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkAcquireNextImageKHR, c->x[0]);
    vkf_swap *s = swap_of(c->x[1], k.obj);
    uint32_t idx = 0;
    vkf_wait_enter(&k, c->x[2]);
    VkResult r = s && c->x[5] ? swap_acquire(s, c->x[2], (VkSemaphore)(uintptr_t)c->x[3],
                                             (VkFence)(uintptr_t)c->x[4], &idx)
                              : VK_ERROR_UNKNOWN;
    vkf_wait_return(&k);
    if (r == VK_SUCCESS)
        r = acquire_signal(&k, s, (VkSemaphore)(uintptr_t)c->x[3], (VkFence)(uintptr_t)c->x[4]);
    if (r == VK_SUCCESS)
        rl_st32(mem, c->x[5], idx);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkAcquireNextImageKHR, r);
    return vkf_end(&k);
}

uint64_t vkh_vkAcquireNextImage2KHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkAcquireNextImage2KHR, c->x[0]);
    uint64_t gi = c->x[1];
    VkResult r = VK_ERROR_UNKNOWN;
    uint32_t idx = 0;
    if (gi && c->x[2]) {
        vkf_swap *s = swap_of(vkf_g64(&k, gi + offsetof(VkAcquireNextImageInfoKHR, swapchain)), k.obj);
        uint64_t timeout = vkf_g64(&k, gi + offsetof(VkAcquireNextImageInfoKHR, timeout));
        VkSemaphore sem = (VkSemaphore)(uintptr_t)vkf_g64(&k, gi + offsetof(VkAcquireNextImageInfoKHR, semaphore));
        VkFence fence = (VkFence)(uintptr_t)vkf_g64(&k, gi + offsetof(VkAcquireNextImageInfoKHR, fence));
        vkf_wait_enter(&k, timeout);
        if (s)
            r = swap_acquire(s, timeout, sem, fence, &idx);
        vkf_wait_return(&k);
        if (s && r == VK_SUCCESS)
            r = acquire_signal(&k, s, sem, fence);
    }
    if (r == VK_SUCCESS)
        rl_st32(mem, c->x[2], idx);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkAcquireNextImage2KHR, r);
    return vkf_end(&k);
}

static uint32_t family_of(const vkf_dev *d, const vkf_fns *fns, VkDevice dev, VkQueue q)
{
    for (uint32_t f = 0; f < d->nqueue_families; f++)
        for (uint32_t i = 0; i < d->queue_counts[f]; i++) {
            VkQueue h = NULL;
            VKF_FN(fns, vkGetDeviceQueue)(dev, d->queue_families[f], i, &h);
            if (h == q)
                return d->queue_families[f];
        }
    return d->nqueue_families ? d->queue_families[0] : 0;
}

static VkResult ensure_pool(vkf_swap *s, uint32_t family)
{
    if (s->pool && s->pool_family == family)
        return VK_SUCCESS;
    wait_all(s);
    if (s->pool)
        DFN(s, vkDestroyCommandPool)(s->dev, s->pool, NULL);
    s->pool = NULL;
    VkCommandPoolCreateInfo pci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = family,
    };
    VkResult r = DFN(s, vkCreateCommandPool)(s->dev, &pci, NULL, &s->pool);
    if (r < 0)
        return r;
    s->pool_family = family;
    VkCommandBufferAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = s->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = s->n,
    };
    return DFN(s, vkAllocateCommandBuffers)(s->dev, &ai, s->cmd);
}

static void barrier(vkf_swap *s, VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to,
                    VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags sstage, VkPipelineStageFlags dstage)
{
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = src,
        .dstAccessMask = dst,
        .oldLayout = from,
        .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    DFN(s, vkCmdPipelineBarrier)(cb, sstage, dstage, 0, 0, NULL, 0, NULL, 1, &b);
}

static void record(vkf_swap *s, uint32_t i, uint32_t hi)
{
    VkCommandBuffer cb = s->cmd[i];
    VkImage src = s->img[i], dst = s->host_img[hi];
    DFN(s, vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    DFN(s, vkBeginCommandBuffer)(cb, &bi);
    barrier(s, cb, src, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
            VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    barrier(s, cb, dst, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    uint32_t gw = s->extent.width, gh = s->extent.height, hw = s->host_extent.width, hh = s->host_extent.height;
    uint32_t dw = hw, dh = hh;
    if ((uint64_t)hw * gh > (uint64_t)hh * gw)
        dw = (uint32_t)((uint64_t)gw * hh / gh);
    else
        dh = (uint32_t)((uint64_t)gh * hw / gw);
    if (!dw)
        dw = 1;
    if (!dh)
        dh = 1;
    int32_t x = (int32_t)((hw - dw) / 2), y = (int32_t)((hh - dh) / 2);
    uint32_t vw = 0, vh = 0;
    int32_t pic[4];
    if (s->ov && (rl_touchpad_view_size(&vw, &vh), vw && vh)) {
        rl_touchpad_picture(pic);
        if (pic[2] > 0 && pic[3] > 0) {
            x = (int32_t)((int64_t)pic[0] * hw / vw);
            y = (int32_t)((int64_t)pic[1] * hh / vh);
            dw = (uint32_t)((uint64_t)pic[2] * hw / vw);
            dh = (uint32_t)((uint64_t)pic[3] * hh / vh);
        }
    }
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (dw != hw || dh != hh || x || y) {
        VkClearColorValue black = { .float32 = { 0, 0, 0, 1 } };
        DFN(s, vkCmdClearColorImage)(cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);
        barrier(s, cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT);
    }
    VkImageBlit blit = {
        .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .srcOffsets = { { 0, 0, 0 }, { (int32_t)gw, (int32_t)gh, 1 } },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffsets = { { x, y, 0 }, { x + (int32_t)dw, y + (int32_t)dh, 1 } },
    };
    DFN(s, vkCmdBlitImage)(cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                           &blit, s->filter);
    if (!vkf_overlay_record(s->ov, cb, hi, i, dst))
        barrier(s, cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    barrier(s, cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT,
            0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    DFN(s, vkEndCommandBuffer)(cb);
}

static void show(vkf_swap *s, uint32_t i)
{
    int64_t now = now_ns();
    int64_t at = vi_vsync_after(now > s->last_shown ? now : s->last_shown);
    if (vi_trace_on())
        vi_trace('P', (uint64_t)at, (uint64_t)s->last_shown, i | (uint64_t)s->n << 8);
    s->last_shown = at;
    if (s->any_shown && s->shown != i)
        s->free_at[s->shown] = at;
    s->free_at[i] = INT64_MAX;
    s->shown = i;
    s->any_shown = true;
}

static bool capture_ready(vkf_swap *s)
{
    if (s->cap_ptr)
        return true;
    const vkf_dev *d = s->dobj->dev;
    VkBufferCreateInfo bi = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = (VkDeviceSize)s->extent.width * s->extent.height * 4,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    if (DFN(s, vkCreateBuffer)(s->dev, &bi, NULL, &s->cap_buf) < 0)
        return false;
    VkMemoryRequirements req;
    DFN(s, vkGetBufferMemoryRequirements)(s->dev, s->cap_buf, &req);
    const uint32_t want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < d->type_count && type == UINT32_MAX; i++)
        if ((req.memoryTypeBits & (1u << i)) && (d->type_flags[i] & want) == want)
            type = i;
    VkMemoryAllocateInfo ai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = type,
    };
    VkCommandBufferAllocateInfo ci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = s->pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    if (type == UINT32_MAX || DFN(s, vkAllocateMemory)(s->dev, &ai, NULL, &s->cap_mem) < 0 ||
        DFN(s, vkBindBufferMemory)(s->dev, s->cap_buf, s->cap_mem, 0) < 0 ||
        DFN(s, vkAllocateCommandBuffers)(s->dev, &ci, &s->cap_cmd) < 0 ||
        DFN(s, vkCreateFence)(s->dev, &fci, NULL, &s->cap_fence) < 0 ||
        DFN(s, vkMapMemory)(s->dev, s->cap_mem, 0, VK_WHOLE_SIZE, 0, &s->cap_ptr) < 0) {
        rl_log(RL_LOG_ERROR, "vk", "capture: no host-visible buffer for a %ux%u image", s->extent.width,
               s->extent.height);
        s->cap_ptr = NULL;
        return false;
    }
    return true;
}

static void capture_copied(void *arg)
{
    vkf_swap *s = arg;
    DFN(s, vkWaitForFences)(s->dev, 1, &s->cap_fence, VK_TRUE, UINT64_MAX);
}

static void capture_written(void *arg)
{
    vkf_swap *s = arg;
    __atomic_store_n(&s->cap_busy, false, __ATOMIC_RELEASE);
}

/**
 * @brief Copies guest image i to the capture buffer after the present's blit
 * (queue order: the barrier waits for all earlier commands, the blit and its
 * semaphore waits included) and leaves the wait and the PNG to the writer.
 */
static void capture(vkf_swap *s, VkQueue q, uint32_t i, uint64_t frame)
{
    bool bgra = s->format == VK_FORMAT_B8G8R8A8_UNORM || s->format == VK_FORMAT_B8G8R8A8_SRGB;
    bool rgba = s->format == VK_FORMAT_R8G8B8A8_UNORM || s->format == VK_FORMAT_R8G8B8A8_SRGB ||
                s->format == VK_FORMAT_A8B8G8R8_UNORM_PACK32 || s->format == VK_FORMAT_A8B8G8R8_SRGB_PACK32;
    if ((!bgra && !rgba) || !capture_ready(s))
        return;
    if (__atomic_load_n(&s->cap_busy, __ATOMIC_ACQUIRE))
        vkf_capture_flush();
    uint64_t at = rl_clock_ns();
    VkCommandBuffer cb = s->cap_cmd;
    DFN(s, vkResetCommandBuffer)(cb, 0);
    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    DFN(s, vkBeginCommandBuffer)(cb, &bi);
    barrier(s, cb, s->img[i], VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region = {
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { s->extent.width, s->extent.height, 1 },
    };
    DFN(s, vkCmdCopyImageToBuffer)(cb, s->img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->cap_buf, 1, &region);
    barrier(s, cb, s->img[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            VK_ACCESS_TRANSFER_READ_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    DFN(s, vkEndCommandBuffer)(cb);
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    DFN(s, vkResetFences)(s->dev, 1, &s->cap_fence);
    if (DFN(s, vkQueueSubmit)(q, 1, &si, s->cap_fence) < 0)
        return;
    s->cap_busy = true;
    s->cap_img = i + 1;
    vkf_capture_job job = {
        .frame = frame,
        .guest_ns = at,
        .w = s->extent.width,
        .h = s->extent.height,
        .stride = (size_t)s->extent.width * 4,
        .bgra = bgra,
        .px = s->cap_ptr,
        .ready = capture_copied,
        .done = capture_written,
        .arg = s,
    };
    vkf_capture_queue(&job);
}

static VkResult swap_present(vkf_swap *s, const vkf_dev *d, VkQueue q, const VkSemaphore *waits, uint32_t nwaits,
                             uint32_t i)
{
    pthread_mutex_lock(&s->lock);
    VkResult r = VK_SUCCESS;
    if (i >= s->n || !s->held[i]) {
        rl_log(RL_LOG_ERROR, "vk", "vkQueuePresentKHR: image %u of swapchain 0x%llx was not acquired", i,
               (unsigned long long)s->id);
        r = VK_ERROR_UNKNOWN;
        goto out;
    }
    if (!s->offscreen && (!s->host || s->lost)) {
        VkPipelineStageFlags drop[16];
        for (uint32_t w = 0; w < nwaits && w < 16; w++)
            drop[w] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo none = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = nwaits < 16 ? nwaits : 16,
            .pWaitSemaphores = waits,
            .pWaitDstStageMask = drop,
        };
        r = DFN(s, vkQueueSubmit)(q, 1, &none, s->done[i]);
        s->held[i] = false;
        if (r >= 0)
            s->busy[i] = true;
        show(s, i);
        r = r < 0 ? r : VK_SUCCESS;
        goto out;
    }
    r = ensure_pool(s, family_of(d, s->fns, s->dev, q));
    if (r < 0)
        goto out;
    uint32_t hi = s->host_idx[i];
    record(s, i, hi);
    VkPipelineStageFlags stages[16];
    for (uint32_t w = 0; w < nwaits && w < 16; w++)
        stages[w] = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .waitSemaphoreCount = nwaits < 16 ? nwaits : 16,
        .pWaitSemaphores = waits,
        .pWaitDstStageMask = stages,
        .commandBufferCount = 1,
        .pCommandBuffers = &s->cmd[i],
        .signalSemaphoreCount = s->offscreen ? 0 : 1,
        .pSignalSemaphores = &s->ready[hi],
    };
    int64_t t_s = vi_trace_now();
    r = DFN(s, vkQueueSubmit)(q, 1, &si, s->done[i]);
    int64_t t_h = vi_trace_now();
    s->held[i] = false;
    if (r < 0)
        goto out;
    s->busy[i] = true;
    uint64_t frame;
    if (vkf_capture_due(&frame))
        capture(s, q, i, frame);
    vkf_decode_stats_frame(frame + 1);
    vkf_shadow_stats_frame(frame + 1);
    vkf_present_id pid;
    VkPresentInfoKHR pi = {
        .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
        .pNext = vkf_flip_chain(d, &pid, NULL, ++s->present_id),
        .waitSemaphoreCount = 1,
        .pWaitSemaphores = &s->ready[hi],
        .swapchainCount = 1,
        .pSwapchains = &s->host,
        .pImageIndices = &hi,
    };
    int64_t t_x = vi_trace_now();
    r = s->offscreen ? VK_SUCCESS : DFN(s, vkQueuePresentKHR)(q, &pi);
    if (!s->offscreen && r >= 0)
        vkf_flip_presented(d, s->dev, s->host, s->present_id);
    vkf_settle(s->fns, q);
    if (t_s)
        vi_trace('H', (uint64_t)(t_h - t_s), (uint64_t)(t_x - t_h), (uint64_t)(vi_trace_now() - t_x));
    show(s, i);
    if (r == VK_ERROR_SURFACE_LOST_KHR) {
        lose(s);
        r = VK_SUCCESS;
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && suboptimal_stale(s)))
        s->stale = true;
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
        r = VK_SUCCESS;
out:
    pthread_mutex_unlock(&s->lock);
    if (r == VK_SUCCESS)
        vi_guest_frame();
    return r;
}

/**
 * @brief Shows guest image i again with the overlay as it is now, while the on-screen controller pauses the game
 * (the guest thread waits inside its present). A suboptimal result that only asks for the rotation the presentation
 * engine already does (every present on a display turned from its native orientation) keeps the host swapchain, as a
 * present does; one that needs a new host swapchain, or a lost surface, gets it here, since the paused game makes no
 * acquire that would. Lock not held.
 */
static void repaint(vkf_swap *s, VkQueue q, uint32_t i)
{
    pthread_mutex_lock(&s->lock);
    if (!s->offscreen && (!s->host || s->stale || s->lost) && !any_held(s)) {
        int64_t at = now_ns();
        if (at >= s->renew_at || !s->lost) {
            DFN(s, vkQueueWaitIdle)(q);
            host_create(s);
        }
    }
    if (!s->host || s->stale || s->lost || !s->pool || i >= s->n) {
        pthread_mutex_unlock(&s->lock);
        return;
    }
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    if (!s->again && DFN(s, vkCreateSemaphore)(s->dev, &sci, NULL, &s->again) < 0)
        s->again = NULL;
    if (!s->again) {
        pthread_mutex_unlock(&s->lock);
        return;
    }
    DFN(s, vkQueueWaitIdle)(q);
    if (s->busy[i]) {
        DFN(s, vkWaitForFences)(s->dev, 1, &s->done[i], VK_TRUE, UINT64_MAX);
        DFN(s, vkResetFences)(s->dev, 1, &s->done[i]);
        s->busy[i] = false;
    }
    uint32_t hi = 0;
    VkResult r = DFN(s, vkAcquireNextImageKHR)(s->dev, s->host, UINT64_MAX, s->again, NULL, &hi);
    if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR) {
        record(s, i, hi);
        VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
        VkSubmitInfo si = {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &s->again,
            .pWaitDstStageMask = &stage,
            .commandBufferCount = 1,
            .pCommandBuffers = &s->cmd[i],
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &s->ready[hi],
        };
        if (DFN(s, vkQueueSubmit)(q, 1, &si, s->done[i]) >= 0) {
            s->busy[i] = true;
            VkPresentInfoKHR pi = {
                .sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR,
                .waitSemaphoreCount = 1,
                .pWaitSemaphores = &s->ready[hi],
                .swapchainCount = 1,
                .pSwapchains = &s->host,
                .pImageIndices = &hi,
            };
            r = DFN(s, vkQueuePresentKHR)(q, &pi);
        }
    }
    if (r == VK_ERROR_OUT_OF_DATE_KHR || (r == VK_SUBOPTIMAL_KHR && suboptimal_stale(s)))
        s->stale = true;
    if (r == VK_ERROR_SURFACE_LOST_KHR)
        lose(s);
    s->repaints++;
    pthread_mutex_unlock(&s->lock);
}

uint64_t vkh_vkQueuePresentKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkQueue q = vkf_first(&k, VKG_KIND_VkQueue, c->x[0]);
    if (!q)
        return vkf_bad_handle(&k, VKG_vkQueuePresentKHR, c->x[0]);
    uint64_t g = c->x[1];
    if (!g) {
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkQueuePresentKHR, VK_ERROR_INITIALIZATION_FAILED);
        return vkf_end(&k);
    }
    vkf_wait_enter(&k, 1);
    vkf_queue_enter(&k);
    int64_t t_q = vi_trace_now();
    uint32_t nwaits = vkf_g32(&k, g + offsetof(VkPresentInfoKHR, waitSemaphoreCount));
    uint64_t gwaits = vkf_g64(&k, g + offsetof(VkPresentInfoKHR, pWaitSemaphores));
    uint32_t nsw = vkf_g32(&k, g + offsetof(VkPresentInfoKHR, swapchainCount));
    uint64_t gsw = vkf_g64(&k, g + offsetof(VkPresentInfoKHR, pSwapchains));
    uint64_t gidx = vkf_g64(&k, g + offsetof(VkPresentInfoKHR, pImageIndices));
    uint64_t gres = vkf_g64(&k, g + offsetof(VkPresentInfoKHR, pResults));
    const VkSemaphore *waits = gwaits ? (const VkSemaphore *)vkf_ptr(&k, gwaits) : NULL;
    VkResult all = VK_SUCCESS;
    vkf_obj *dobj = k.obj->parent;
    for (uint32_t j = 0; j < nsw && gsw && gidx; j++) {
        vkf_swap *s = swap_of(vkf_g64(&k, gsw + 8u * j), dobj);
        VkResult r = s ? swap_present(s, k.obj->dev, q, j ? NULL : waits, j ? 0 : nwaits, vkf_g32(&k, gidx + 4u * j))
                       : VK_ERROR_UNKNOWN;
        if (gres)
            rl_st32(mem, gres + 4u * j, (uint32_t)r);
        if (r < 0 && all >= 0)
            all = r;
        if (s && r >= 0 && j == 0)
            while (rl_touchpad_hold(16))
                repaint(s, q, vkf_g32(&k, gidx));
    }
    vkf_queue_unlock(&k);
    int64_t t_p = vi_trace_now();
    vkf_wait_return(&k);
    if (t_q)
        vi_trace('Q', (uint64_t)(t_q - k.trace_t0), (uint64_t)(t_p - t_q), (uint64_t)(vi_trace_now() - t_p));
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkQueuePresentKHR, all);
    return vkf_end(&k);
}

uint32_t rl_vk_swapchain_repaints(uint64_t guest_swapchain)
{
    vkf_swap *s = swap_of(guest_swapchain, NULL);
    if (!s)
        return 0;
    pthread_mutex_lock(&s->lock);
    uint32_t n = s->repaints;
    pthread_mutex_unlock(&s->lock);
    return n;
}

bool rl_vk_swapchain_host(uint64_t guest_swapchain, uint32_t *width, uint32_t *height, uint32_t *creations)
{
    vkf_swap *s = swap_of(guest_swapchain, NULL);
    if (!s)
        return false;
    pthread_mutex_lock(&s->lock);
    *width = s->host_extent.width;
    *height = s->host_extent.height;
    *creations = s->creations;
    pthread_mutex_unlock(&s->lock);
    return true;
}

static const char *const g_unsupported[] = {
    "vkGetSwapchainStatusKHR",
    "vkCreateSharedSwapchainsKHR",
    "vkGetSwapchainCounterEXT",
    "vkGetPastPresentationTimingGOOGLE",
    "vkGetRefreshCycleDurationGOOGLE",
    "vkSetHdrMetadataEXT",
    "vkReleaseSwapchainImagesEXT",
    "vkReleaseSwapchainImagesKHR",
    "vkWaitForPresentKHR",
    "vkWaitForPresent2KHR",
    "vkSetLocalDimmingAMD",
    NULL,
};

bool vkf_swap_unsupported(const char *name)
{
    for (const char *const *n = g_unsupported; *n; n++)
        if (!strcmp(*n, name))
            return true;
    return false;
}
