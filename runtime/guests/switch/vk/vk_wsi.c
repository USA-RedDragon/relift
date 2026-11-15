/**
 * @file vk_wsi.c
 * @brief VK_NN_vi_surface: the guest's surface for an nn::vi native window
 * becomes a host surface made by the configured surface provider.
 *
 * The host has no VK_NN_vi_surface. When the guest enables it, instance
 * creation enables the provider's host extensions instead, and
 * vkCreateViSurfaceNN asks the provider for a surface for the guest window.
 * Everything after that (surface queries, swapchains, present) is plain
 * VK_KHR_surface and VK_KHR_swapchain forwarding.
 */
#include "vkf.h"

int vkh_vkCreateViSurfaceNN(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkInstance inst = vkf_first(&k, VKG_KIND_VkInstance, c->x[0]);
    if (!inst)
        return vkf_bad_handle(&k, VKG_vkCreateViSurfaceNN, c->x[0]);
    uint64_t gi = c->x[1];
    uint64_t gout = c->x[3];
    const rl_vk_surface_provider *sp = vkf.cfg.surfaces;
    if (!gi || !gout || !sp || !k.obj->inst->vi_surface) {
        c->x[0] = (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
        return vkf_end(&k);
    }
    uint64_t window = vkf_g64(&k, gi + offsetof(VkViSurfaceCreateInfoNN, window));
    uint64_t surface = 0;
    void *gipa;
    memcpy(&gipa, &vkf.gipa, sizeof gipa);
    int32_t r = sp->create(sp->user, inst, gipa, window, &surface);
    if (r >= 0)
        vkf_s64(&k, gout, surface);
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "vk", "vkCreateViSurfaceNN(window 0x%llx) = %d", (unsigned long long)window, r);
    c->x[0] = (uint32_t)r;
    return vkf_end(&k);
}

static const char *const headless_exts[] = { VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_HEADLESS_SURFACE_EXTENSION_NAME, NULL };

static int32_t headless_create(void *user, void *host_instance, void *gipa, uint64_t guest_window, uint64_t *surface)
{
    (void)user;
    (void)guest_window;
    PFN_vkGetInstanceProcAddr get;
    memcpy(&get, &gipa, sizeof get);
    PFN_vkVoidFunction raw = get(host_instance, "vkCreateHeadlessSurfaceEXT");
    PFN_vkCreateHeadlessSurfaceEXT f;
    memcpy(&f, &raw, sizeof f);
    if (!f)
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    VkHeadlessSurfaceCreateInfoEXT ci = { .sType = VK_STRUCTURE_TYPE_HEADLESS_SURFACE_CREATE_INFO_EXT };
    VkSurfaceKHR s = NULL;
    VkResult r = f(host_instance, &ci, NULL, &s);
    *surface = (uint64_t)(uintptr_t)s;
    return r;
}

static const rl_vk_surface_provider headless = {
    .instance_extensions = headless_exts,
    .create = headless_create,
    .offscreen = true,
};

const rl_vk_surface_provider *rl_vk_headless_surfaces(void) { return &headless; }

int vkh_vkGetPhysicalDeviceSurfaceSupportKHR(rl_ctx *restrict c, uint8_t *mem)
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
        const rl_vk_surface_provider *sp = vkf.cfg.surfaces;
        if (sp && sp->offscreen) {
            *out = VK_TRUE;
            r = VK_SUCCESS;
        } else {
            r = fn(pd, (uint32_t)c->x[1], (VkSurfaceKHR)(uintptr_t)c->x[2], out);
        }
    }
    c->x[0] = (uint32_t)r;
    return vkf_end(&k);
}
