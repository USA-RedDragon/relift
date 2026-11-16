/**
 * @file vk_wsi.c
 * @brief VK_NN_vi_surface: the guest's surface for an nn::vi native window
 * becomes a host surface made by the configured surface provider, or, for an
 * offscreen provider, a handle of the layer's own with no host surface.
 *
 * The host has no VK_NN_vi_surface. When the guest enables it, instance
 * creation enables the provider's host extensions instead, and
 * vkCreateViSurfaceNN asks the provider for a surface for the guest window.
 * Everything after that (surface queries, swapchains, present) is plain
 * VK_KHR_surface and VK_KHR_swapchain forwarding. Offscreen surfaces never
 * reach the host: the layer answers every surface call on them itself.
 */
#include "vkf.h"

#define OFFSCREEN_SURFACE_TAG 0x5300000000000000ull

static uint64_t g_offscreen_surfaces;

bool vkf_offscreen(void)
{
    const rl_vk_surface_provider *p = vkf.cfg.surfaces;
    return p && p->offscreen;
}

uint64_t vkh_vkCreateViSurfaceNN(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkInstance inst = vkf_first(&k, VKG_KIND_VkInstance, c->x[0]);
    if (!inst)
        return vkf_bad_handle(&k, VKG_vkCreateViSurfaceNN, c->x[0]);
    uint64_t gi = c->x[1];
    uint64_t gout = c->x[3];
    const rl_vk_surface_provider *sp = vkf.cfg.surfaces;
    if (!gi || !gout || !sp || !k.obj->inst->vi_surface || (!sp->offscreen && !sp->create)) {
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateViSurfaceNN, VK_ERROR_INITIALIZATION_FAILED);
        return vkf_end(&k);
    }
    uint64_t window = vkf_g64(&k, gi + offsetof(VkViSurfaceCreateInfoNN, window));
    uint64_t surface = 0;
    int32_t r = VK_SUCCESS;
    if (sp->offscreen) {
        surface = OFFSCREEN_SURFACE_TAG | __atomic_add_fetch(&g_offscreen_surfaces, 1, __ATOMIC_RELAXED);
    } else {
        void *gipa;
        memcpy(&gipa, &vkf.gipa, sizeof gipa);
        r = sp->create(sp->user, inst, gipa, window, &surface);
    }
    if (r >= 0)
        vkf_s64(&k, gout, surface);
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "vk", "vkCreateViSurfaceNN(window 0x%llx) = %d", (unsigned long long)window, r);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateViSurfaceNN, r);
    return vkf_end(&k);
}

uint64_t vkh_vkDestroySurfaceKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkInstance inst = vkf_first(&k, VKG_KIND_VkInstance, c->x[0]);
    if (!inst)
        return vkf_bad_handle(&k, VKG_vkDestroySurfaceKHR, c->x[0]);
    PFN_vkDestroySurfaceKHR fn = VKF_FN(k.fns, vkDestroySurfaceKHR);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkDestroySurfaceKHR);
    if (!vkf_offscreen())
        fn(inst, (VkSurfaceKHR)(uintptr_t)c->x[1], NULL);
    return vkf_end(&k);
}

static const char *const headless_exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, NULL };

static const rl_vk_surface_provider headless = {
    .instance_extensions = headless_exts,
    .offscreen = true,
};

const rl_vk_surface_provider *rl_vk_headless_surfaces(void) { return &headless; }

uint64_t vkh_vkGetPhysicalDeviceSurfaceSupportKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceSurfaceSupportKHR, c->x[0]);
    PFN_vkGetPhysicalDeviceSurfaceSupportKHR fn = VKF_FN(k.fns, vkGetPhysicalDeviceSurfaceSupportKHR);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkGetPhysicalDeviceSurfaceSupportKHR);
    VkBool32 *out = vkf_ptr(&k, c->x[3]);
    VkResult r = VK_ERROR_INITIALIZATION_FAILED;
    if (out) {
        if (vkf_offscreen()) {
            *out = VK_TRUE;
            r = VK_SUCCESS;
        } else {
            r = fn(pd, (uint32_t)c->x[1], (VkSurfaceKHR)(uintptr_t)c->x[2], out);
        }
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetPhysicalDeviceSurfaceSupportKHR, r);
    return vkf_end(&k);
}

uint64_t vkh_vkGetPhysicalDevicePresentRectanglesKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDevicePresentRectanglesKHR, c->x[0]);
    PFN_vkGetPhysicalDevicePresentRectanglesKHR fn = VKF_FN(k.fns, vkGetPhysicalDevicePresentRectanglesKHR);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkGetPhysicalDevicePresentRectanglesKHR);
    uint32_t *count = vkf_ptr(&k, c->x[2]);
    VkRect2D *rects = vkf_ptr(&k, c->x[3]);
    VkResult r;
    if (!vkf_offscreen())
        r = fn(pd, (VkSurfaceKHR)(uintptr_t)c->x[1], count, rects);
    else if (!count)
        r = VK_ERROR_INITIALIZATION_FAILED;
    else if (!rects) {
        *count = 1;
        r = VK_SUCCESS;
    } else if (!*count)
        r = VK_INCOMPLETE;
    else {
        uint32_t guest[2], host[2];
        uint64_t gen;
        vkf_surface_extent(c->x[1], guest, host, &gen);
        rects[0] = (VkRect2D){ { 0, 0 }, { guest[0], guest[1] } };
        *count = 1;
        r = VK_SUCCESS;
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetPhysicalDevicePresentRectanglesKHR, r);
    return vkf_end(&k);
}

uint64_t vkh_vkGetDeviceGroupSurfacePresentModesKHR(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkGetDeviceGroupSurfacePresentModesKHR, c->x[0]);
    PFN_vkGetDeviceGroupSurfacePresentModesKHR fn = VKF_FN(k.fns, vkGetDeviceGroupSurfacePresentModesKHR);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkGetDeviceGroupSurfacePresentModesKHR);
    VkDeviceGroupPresentModeFlagsKHR *modes = vkf_ptr(&k, c->x[2]);
    VkResult r;
    if (!vkf_offscreen())
        r = fn(dev, (VkSurfaceKHR)(uintptr_t)c->x[1], modes);
    else if (!modes)
        r = VK_ERROR_INITIALIZATION_FAILED;
    else {
        *modes = VK_DEVICE_GROUP_PRESENT_MODE_LOCAL_BIT_KHR;
        r = VK_SUCCESS;
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkGetDeviceGroupSurfacePresentModesKHR, r);
    return vkf_end(&k);
}
