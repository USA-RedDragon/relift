/**
 * @file vk_device.c
 * @brief The one physical device the guest sees.
 *
 * The Switch has one GPU, so the guest gets one physical device and one
 * device group of it. The vk.device setting (RELIFT_VK_DEVICE) picks it by a part of its name;
 * otherwise it is the first host device whose family 0 has graphics and
 * compute and, when the guest enabled VK_NN_vi_surface, that can present
 * through the surface provider.
 */
#include "vkf.h"

#include "services.h"

#include <relift/settings.h>

#include <stdlib.h>
#include <string.h>

static bool usable(const vkf_fns *fns, VkPhysicalDevice pd)
{
    uint32_t n = 1;
    VkQueueFamilyProperties q = { 0 };
    VKF_FN(fns, vkGetPhysicalDeviceQueueFamilyProperties)(pd, &n, &q);
    const uint32_t need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    return n && (q.queueFlags & need) == need;
}

static bool presents(const vkf_obj *inst, VkPhysicalDevice pd)
{
    const rl_vk_surface_provider *sp = vkf.cfg.surfaces;
    if (!inst->inst->vi_surface || !sp || !sp->presents)
        return true;
    void *gipa;
    memcpy(&gipa, &vkf.gipa, sizeof gipa);
    return sp->presents(sp->user, inst->host, gipa, pd, 0);
}

static const char *name_of(const vkf_fns *fns, VkPhysicalDevice pd, VkPhysicalDeviceProperties *p)
{
    VKF_FN(fns, vkGetPhysicalDeviceProperties)(pd, p);
    return p->deviceName;
}

static VkPhysicalDevice choose(vkf_call *k, VkInstance inst)
{
    vkf_obj *io = k->obj;
    uint32_t n = 0;
    if (VKF_FN(k->fns, vkEnumeratePhysicalDevices)(inst, &n, NULL) < 0 || !n)
        return NULL;
    VkPhysicalDevice *all = vkf_alloc(k, n * sizeof *all);
    if (VKF_FN(k->fns, vkEnumeratePhysicalDevices)(inst, &n, all) < 0 || !n)
        return NULL;
    rl_switch_declare_settings();
    const char *want = rl_setting_str("vk.device");
    VkPhysicalDeviceProperties p;
    VkPhysicalDevice pick = NULL;
    for (uint32_t i = 0; i < n && want && *want && !pick; i++)
        if (strstr(name_of(k->fns, all[i], &p), want))
            pick = all[i];
    if (want && *want && !pick)
        rl_log(RL_LOG_WARN, "vk", "vk.device=%s matches no host device; choosing one", want);
    if (pick && !presents(io, pick))
        rl_log(RL_LOG_WARN, "vk",
               "vk.device=%s: %s cannot present to this video backend's surfaces, so the game's swapchain "
               "will fail; choose another device or video backend",
               want, name_of(k->fns, pick, &p));
    for (uint32_t i = 0; i < n && !pick; i++)
        if (usable(k->fns, all[i]) && presents(io, all[i]))
            pick = all[i];
    if (!pick)
        pick = all[0];
    rl_log(RL_LOG_INFO, "vk", "physical device: %s (%u on the host)", name_of(k->fns, pick, &p), n);
    return pick;
}

static uint64_t the_device(vkf_call *k, VkInstance inst)
{
    vkf_inst *vi = k->obj->inst;
    if (!vi->physical)
        vi->physical = choose(k, inst);
    return vkf_wrap(k, VKG_KIND_VkPhysicalDevice, vi->physical);
}

int vkh_vkEnumeratePhysicalDevices(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkInstance inst = vkf_first(&k, VKG_KIND_VkInstance, c->x[0]);
    if (!inst)
        return vkf_bad_handle(&k, VKG_vkEnumeratePhysicalDevices, c->x[0]);
    if (!c->x[1]) {
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkEnumeratePhysicalDevices, VK_ERROR_INITIALIZATION_FAILED);
        return vkf_end(&k);
    }
    uint64_t pd = the_device(&k, inst);
    if (!pd) {
        rl_st32(mem, c->x[1], 0);
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkEnumeratePhysicalDevices, VK_SUCCESS);
        return vkf_end(&k);
    }
    VkResult r = VK_SUCCESS;
    if (c->x[2]) {
        if (vkf_g32(&k, c->x[1])) {
            vkf_s64(&k, c->x[2], pd);
            rl_st32(mem, c->x[1], 1);
        } else
            r = VK_INCOMPLETE;
    } else
        rl_st32(mem, c->x[1], 1);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkEnumeratePhysicalDevices, r);
    return vkf_end(&k);
}

int vkh_vkEnumeratePhysicalDeviceGroups(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkInstance inst = vkf_first(&k, VKG_KIND_VkInstance, c->x[0]);
    if (!inst)
        return vkf_bad_handle(&k, VKG_vkEnumeratePhysicalDeviceGroups, c->x[0]);
    if (!c->x[1]) {
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkEnumeratePhysicalDeviceGroups, VK_ERROR_INITIALIZATION_FAILED);
        return vkf_end(&k);
    }
    uint64_t pd = the_device(&k, inst);
    uint32_t n = pd ? 1 : 0;
    VkResult r = VK_SUCCESS;
    if (c->x[2] && n) {
        if (vkf_g32(&k, c->x[1])) {
            uint64_t g = c->x[2];
            rl_st32(mem, g + offsetof(VkPhysicalDeviceGroupProperties, physicalDeviceCount), 1);
            vkf_s64(&k, g + offsetof(VkPhysicalDeviceGroupProperties, physicalDevices), pd);
            rl_st32(mem, g + offsetof(VkPhysicalDeviceGroupProperties, subsetAllocation), VK_FALSE);
        } else {
            n = 0;
            r = VK_INCOMPLETE;
        }
    }
    rl_st32(mem, c->x[1], n);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkEnumeratePhysicalDeviceGroups, r);
    return vkf_end(&k);
}
