/**
 * @file vk_format.c
 * @brief Format queries for depth formats the console always has.
 *
 * The console supports VK_FORMAT_D24_UNORM_S8_UINT and
 * VK_FORMAT_X8_D24_UNORM_PACK32, and games do not always check the answer
 * they get (one picks another depth format for an image and still views it
 * as D24S8). MoltenVK on Apple GPUs without 24-bit depth reports both
 * unsupported but creates them as D32_SFLOAT_S8_UINT and D32_SFLOAT. On such
 * a driver the queries answer for the format it uses instead
 * (vkf_host_format, used by the format queries in vk_props.c).
 */
#include "vkf.h"

#include <string.h>

static VkFormat substitute(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_D24_UNORM_S8_UINT: return VK_FORMAT_D32_SFLOAT_S8_UINT;
    case VK_FORMAT_X8_D24_UNORM_PACK32: return VK_FORMAT_D32_SFLOAT;
    default: return f;
    }
}

static bool driver_substitutes(const vkf_fns *fns, VkPhysicalDevice pd)
{
    PFN_vkGetPhysicalDeviceProperties2 props2 = VKF_FN(fns, vkGetPhysicalDeviceProperties2);
    if (!props2)
        return false;
    VkPhysicalDeviceDriverProperties drv = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES };
    VkPhysicalDeviceProperties2 p = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &drv };
    props2(pd, &p);
    return p.properties.apiVersion >= VKF_API_VERSION(1, 2) && drv.driverID == VK_DRIVER_ID_MOLTENVK;
}

VkFormat vkf_host_format(const vkf_fns *fns, VkPhysicalDevice pd, VkFormat f)
{
    VkFormat s = substitute(f);
    if (s == f)
        return f;
    VkFormatProperties fp;
    memset(&fp, 0, sizeof fp);
    VKF_FN(fns, vkGetPhysicalDeviceFormatProperties)(pd, f, &fp);
    if (fp.optimalTilingFeatures || !driver_substitutes(fns, pd))
        return f;
    if (rl_once(0x464D545355420000ull | (uint64_t)f))
        rl_log(RL_LOG_INFO, "vk", "format %d: the driver uses format %d for it; queries answer for that", (int)f, (int)s);
    return s;
}
