/**
 * @file vulkan_heaps.h
 * @brief What an i386 guest is told about memory heaps: what the same
 * driver's i386 build would say. Only lavapipe (VK_DRIVER_ID_MESA_LLVMPIPE)
 * answers differently in a 32-bit build: it sizes its one heap by total RAM
 * and, when sizeof(void*) is 4, at 3 GiB (lvp_GetPhysicalDeviceMemoryProperties
 * in Mesa's lvp_device.c). radv, anv and turnip do not look at the pointer
 * width, and NVIDIA's drivers are not known to, so every other driver's
 * heaps, heapBudget and heapUsage pass through unchanged (a resizable-BAR
 * heap stays its full size).
 */
#ifndef RELIFT_I386_VULKAN_HEAPS_H
#define RELIFT_I386_VULKAN_HEAPS_H

#include <stdint.h>
#include <string.h>

/** @brief The heap size of lavapipe's i386 build. */
#define TG_VK_HEAP_CAP (3ULL << 30)

/** @brief VkDriverId of lavapipe. */
#define TG_VK_DRIVER_ID_LLVMPIPE 13

/** @brief VkPhysicalDeviceMemoryProperties in the i386 layout. */
enum {
    TG_VK_MEMPROPS_SIZE = 456,
    TG_VK_MEMPROPS_HEAP_COUNT = 260,
    TG_VK_MEMPROPS_HEAPS = 264,
};

/** @brief VkPhysicalDeviceMemoryBudgetPropertiesEXT in the i386 layout. */
enum {
    TG_VK_BUDGET_STYPE = 1000237000,
    TG_VK_BUDGET_SIZE = 264,
    TG_VK_BUDGET_BUDGET = 8,
    TG_VK_BUDGET_USAGE = 136,
};

/** @brief Caps the heaps of an i386-layout VkPhysicalDeviceMemoryProperties as the driver's i386 build does. */
static inline void tg_vk_cap_heaps(uint8_t *p, uint32_t driver_id)
{
    if (driver_id != TG_VK_DRIVER_ID_LLVMPIPE)
        return;
    uint32_t nh;
    memcpy(&nh, p + TG_VK_MEMPROPS_HEAP_COUNT, 4);
    for (uint32_t h = 0; h < nh && h < 16; h++) {
        uint64_t size;
        memcpy(&size, p + TG_VK_MEMPROPS_HEAPS + 12 * h, 8);
        if (size > TG_VK_HEAP_CAP) {
            size = TG_VK_HEAP_CAP;
            memcpy(p + TG_VK_MEMPROPS_HEAPS + 12 * h, &size, 8);
        }
    }
}

/** @brief Caps heapBudget and heapUsage of an i386-layout budget struct b the same way. */
static inline void tg_vk_cap_budget(uint8_t *b, uint32_t driver_id)
{
    if (driver_id != TG_VK_DRIVER_ID_LLVMPIPE)
        return;
    for (uint32_t i = 0; i < 32; i++) {
        uint8_t *at = b + TG_VK_BUDGET_BUDGET + 8 * i;
        uint64_t v;
        memcpy(&v, at, 8);
        if (v > TG_VK_HEAP_CAP) {
            v = TG_VK_HEAP_CAP;
            memcpy(at, &v, 8);
        }
    }
}

#endif
