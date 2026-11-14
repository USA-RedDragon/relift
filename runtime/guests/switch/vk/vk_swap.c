/**
 * @file vk_swap.c
 * @brief Guest swapchains: the guest renders into images the layer owns, and
 * each present blits the image into a host swapchain image.
 *
 * A Switch title never sees its window change size, so a guest swapchain
 * keeps the extent and format it was created with. The host swapchain behind
 * it follows the host surface: it is recreated whenever the surface reports
 * out of date or suboptimal, or the provider's extent generation changes, and
 * the present letterboxes the guest image into it. The guest never gets
 * VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR from a resize.
 *
 * Guest swapchain handles are ids from this file (0x57 in the top byte).
 */
#include "vkf.h"
#include "vi.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SWAP_MAX 8u
#define HOST_MAX 16u
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
    uint32_t next;
    VkSwapchainKHR host;
    uint32_t host_n;
    VkImage host_img[HOST_MAX];
    VkSemaphore ready[HOST_MAX];
    VkExtent2D host_extent;
    VkSemaphore retired[RETIRED_MAX];
    uint32_t nretired;
    bool stale;
    bool old;
    uint64_t gen;
    uint32_t creations;
    VkCommandPool pool;
    uint32_t pool_family;
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

void vkf_vi_caps(uint64_t surface, VkSurfaceCapabilitiesKHR *caps)
{
    uint32_t guest[2], host[2];
    uint64_t gen;
    vkf_surface_extent(surface, guest, host, &gen);
    caps->currentExtent = (VkExtent2D){ guest[0], guest[1] };
    caps->minImageExtent = (VkExtent2D){ 1, 1 };
    if (caps->maxImageExtent.width < guest[0])
        caps->maxImageExtent.width = guest[0];
    if (caps->maxImageExtent.height < guest[1])
        caps->maxImageExtent.height = guest[1];
    caps->minImageCount = 2;
    caps->maxImageCount = SWAP_MAX;
    if (!caps->maxImageArrayLayers)
        caps->maxImageArrayLayers = 1;
    caps->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    caps->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    caps->supportedUsageFlags = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
}

int vkh_vkGetPhysicalDeviceSurfaceCapabilitiesKHR(rl_ctx *restrict c, uint8_t *mem)
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
    VkResult r = caps ? fn(pd, (VkSurfaceKHR)(uintptr_t)c->x[1], caps) : VK_ERROR_INITIALIZATION_FAILED;
    if (r >= 0)
        vkf_vi_caps(c->x[1], caps);
    c->x[0] = (uint32_t)r;
    return vkf_end(&k);
}

int vkh_vkGetPhysicalDeviceSurfaceCapabilities2KHR(rl_ctx *restrict c, uint8_t *mem)
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
    VkResult r = info && caps ? fn(pd, info, caps) : VK_ERROR_INITIALIZATION_FAILED;
    if (r >= 0) {
        vkf_vi_caps((uint64_t)(uintptr_t)info->surface, &caps->surfaceCapabilities);
        vkg_aout_VkSurfaceCapabilities2KHR(&k, c->x[2], caps, 1);
    }
    c->x[0] = (uint32_t)r;
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
            DFN(s, vkDeviceWaitIdle)(s->dev);
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
    IFN(s, vkGetPhysicalDeviceSurfaceFormatsKHR)(s->phys, s->surface, &n, NULL);
    VkSurfaceFormatKHR *f = calloc(n ? n : 1, sizeof *f);
    if (!f)
        abort();
    IFN(s, vkGetPhysicalDeviceSurfaceFormatsKHR)(s->phys, s->surface, &n, f);
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
    IFN(s, vkGetPhysicalDeviceSurfacePresentModesKHR)(s->phys, s->surface, &n, NULL);
    VkPresentModeKHR m[16];
    if (n > 16)
        n = 16;
    IFN(s, vkGetPhysicalDeviceSurfacePresentModesKHR)(s->phys, s->surface, &n, m);
    for (uint32_t i = 0; i < n; i++)
        if (m[i] == s->mode)
            return s->mode;
    return VK_PRESENT_MODE_FIFO_KHR;
}

/** @brief Makes the host swapchain match the host surface. VK_NOT_READY while the surface has no area. Lock held. */
static VkResult host_create(vkf_swap *s)
{
    VkSurfaceCapabilitiesKHR caps;
    VkResult r = IFN(s, vkGetPhysicalDeviceSurfaceCapabilitiesKHR)(s->phys, s->surface, &caps);
    if (r < 0)
        return r;
    uint32_t guest[2], host[2];
    uint64_t gen;
    vkf_surface_extent((uint64_t)(uintptr_t)s->surface, guest, host, &gen);
    VkExtent2D e = caps.currentExtent;
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
        .surface = s->surface,
        .minImageCount = count,
        .imageFormat = fmt.format,
        .imageColorSpace = fmt.colorSpace,
        .imageExtent = e,
        .imageArrayLayers = 1,
        .imageUsage = usage,
        .imageSharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .preTransform = caps.currentTransform,
        .compositeAlpha = alpha,
        .presentMode = pick_mode(s),
        .clipped = VK_TRUE,
        .oldSwapchain = s->host,
    };
    VkSwapchainKHR sw = NULL;
    r = DFN(s, vkCreateSwapchainKHR)(s->dev, &ci, NULL, &sw);
    if (r < 0) {
        rl_log(RL_LOG_ERROR, "vk", "host swapchain %ux%u: %d", e.width, e.height, r);
        return r;
    }
    wait_all(s);
    if (s->host)
        DFN(s, vkDestroySwapchainKHR)(s->dev, s->host, NULL);
    retire_semaphores(s);
    s->host = sw;
    uint32_t n = 0;
    DFN(s, vkGetSwapchainImagesKHR)(s->dev, sw, &n, NULL);
    if (n > HOST_MAX)
        n = HOST_MAX;
    DFN(s, vkGetSwapchainImagesKHR)(s->dev, sw, &n, s->host_img);
    VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    for (uint32_t i = 0; i < n; i++)
        DFN(s, vkCreateSemaphore)(s->dev, &sci, NULL, &s->ready[i]);
    s->host_n = n;
    s->host_extent = e;
    s->gen = gen;
    s->stale = false;
    s->creations++;
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "vk", "host swapchain %ux%u, format %d, %u images, for guest %ux%u", e.width, e.height,
               fmt.format, n, s->extent.width, s->extent.height);
    return VK_SUCCESS;
}

static void swap_free(vkf_swap *s)
{
    wait_all(s);
    if (s->host || s->nretired)
        DFN(s, vkDeviceWaitIdle)(s->dev);
    for (uint32_t i = 0; i < s->n; i++) {
        DFN(s, vkDestroyImage)(s->dev, s->img[i], NULL);
        DFN(s, vkFreeMemory)(s->dev, s->mem[i], NULL);
        DFN(s, vkDestroyFence)(s->dev, s->done[i], NULL);
    }
    if (s->pool)
        DFN(s, vkDestroyCommandPool)(s->dev, s->pool, NULL);
    if (s->host)
        DFN(s, vkDestroySwapchainKHR)(s->dev, s->host, NULL);
    retire_semaphores(s);
    for (uint32_t j = 0; j < s->nretired; j++)
        DFN(s, vkDestroySemaphore)(s->dev, s->retired[j], NULL);
    pthread_mutex_destroy(&s->lock);
    free(s);
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
        .usage = ci->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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

int vkh_vkCreateSwapchainKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkCreateSwapchainKHR, c->x[0]);
    VkSwapchainCreateInfoKHR *ci = vkg_ain_VkSwapchainCreateInfoKHR(&k, c->x[1], 1);
    uint64_t gout = c->x[3];
    if (!ci || !gout || !ci->imageExtent.width || !ci->imageExtent.height) {
        c->x[0] = (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
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
    s->surface = ci->surface;
    s->format = ci->imageFormat;
    s->color_space = ci->imageColorSpace;
    s->extent = ci->imageExtent;
    s->mode = ci->presentMode;
    s->n = ci->minImageCount < 2 ? 2 : ci->minImageCount > SWAP_MAX ? SWAP_MAX : ci->minImageCount;
    vkf_swap *old = swap_of((uint64_t)(uintptr_t)ci->oldSwapchain, k.obj);
    if (old) {
        pthread_mutex_lock(&old->lock);
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
        c->x[0] = (uint32_t)r;
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

int vkh_vkDestroySwapchainKHR(rl_ctx *restrict c, uint8_t *mem)
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

int vkh_vkGetSwapchainImagesKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkGetSwapchainImagesKHR, c->x[0]);
    vkf_swap *s = swap_of(c->x[1], k.obj);
    uint64_t gcount = c->x[2], garray = c->x[3];
    if (!s || !gcount) {
        c->x[0] = (uint32_t)VK_ERROR_UNKNOWN;
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
    c->x[0] = (uint32_t)(w < s->n ? VK_INCOMPLETE : VK_SUCCESS);
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
    int64_t start = now_ns();
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
        if ((!s->host || s->stale || gen != s->gen) && !any_held(s)) {
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
        uint32_t i = s->next, tries = 0;
        while (s->held[i] && tries++ < s->n)
            i = (i + 1) % s->n;
        if (s->held[i]) {
            r = VK_NOT_READY;
            break;
        }
        if (s->busy[i]) {
            r = DFN(s, vkWaitForFences)(s->dev, 1, &s->done[i], VK_TRUE, timeout);
            if (r != VK_SUCCESS)
                break;
            DFN(s, vkResetFences)(s->dev, 1, &s->done[i]);
            s->busy[i] = false;
        }
        uint32_t hi = 0;
        r = DFN(s, vkAcquireNextImageKHR)(s->dev, s->host, timeout, sem, fence, &hi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR && !any_held(s)) {
            s->stale = true;
            continue;
        }
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            r = VK_NOT_READY;
            break;
        }
        if (r == VK_SUBOPTIMAL_KHR) {
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
    return r;
}

int vkh_vkAcquireNextImageKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkAcquireNextImageKHR, c->x[0]);
    vkf_swap *s = swap_of(c->x[1], k.obj);
    uint32_t idx = 0;
    VkResult r = s && c->x[5] ? swap_acquire(s, c->x[2], (VkSemaphore)(uintptr_t)c->x[3],
                                             (VkFence)(uintptr_t)c->x[4], &idx)
                              : VK_ERROR_UNKNOWN;
    if (r == VK_SUCCESS)
        rl_st32(mem, c->x[5], idx);
    c->x[0] = (uint32_t)r;
    return vkf_end(&k);
}

int vkh_vkAcquireNextImage2KHR(rl_ctx *restrict c, uint8_t *mem)
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
        if (s)
            r = swap_acquire(s, vkf_g64(&k, gi + offsetof(VkAcquireNextImageInfoKHR, timeout)),
                             (VkSemaphore)(uintptr_t)vkf_g64(&k, gi + offsetof(VkAcquireNextImageInfoKHR, semaphore)),
                             (VkFence)(uintptr_t)vkf_g64(&k, gi + offsetof(VkAcquireNextImageInfoKHR, fence)), &idx);
    }
    if (r == VK_SUCCESS)
        rl_st32(mem, c->x[2], idx);
    c->x[0] = (uint32_t)r;
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
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (dw != hw || dh != hh) {
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
    barrier(s, cb, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_WRITE_BIT,
            0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    barrier(s, cb, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_ACCESS_TRANSFER_READ_BIT,
            0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    DFN(s, vkEndCommandBuffer)(cb);
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
        .signalSemaphoreCount = 1,
        .pSignalSemaphores = &s->ready[hi],
    };
    r = DFN(s, vkQueueSubmit)(q, 1, &si, s->done[i]);
    s->held[i] = false;
    if (r < 0)
        goto out;
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
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        s->stale = true;
        r = VK_SUCCESS;
    }
out:
    pthread_mutex_unlock(&s->lock);
    return r;
}

int vkh_vkQueuePresentKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkQueue q = vkf_first(&k, VKG_KIND_VkQueue, c->x[0]);
    if (!q)
        return vkf_bad_handle(&k, VKG_vkQueuePresentKHR, c->x[0]);
    uint64_t g = c->x[1];
    if (!g) {
        c->x[0] = (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
        return vkf_end(&k);
    }
    vkf_queue_enter(&k);
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
    }
    vkf_queue_unlock(&k);
    c->x[0] = (uint32_t)all;
    return vkf_end(&k);
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
