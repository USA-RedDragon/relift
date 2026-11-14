/**
 * @file vk_queue.c
 * @brief The Switch queue family layout, mapped onto the host's first family.
 *
 * The guest sees the one family the console's driver always reports, whatever
 * the host has. Every guest queue of it lands on a queue of host family 0;
 * when the host has fewer queues than the guest created, guest queues share
 * host queues and every queue operation on that device takes the device's
 * queue lock.
 */
#include "vkf.h"

#include "hos.h"

#include <stdlib.h>
#include <string.h>

static const VkQueueFamilyProperties switch_family = {
    .queueFlags = VKF_SWITCH_QUEUE_FLAGS,
    .queueCount = VKF_SWITCH_QUEUE_COUNT,
    .timestampValidBits = VKF_SWITCH_TIMESTAMP_BITS,
    .minImageTransferGranularity = { 1, 1, 1 },
};

int vkh_vkGetPhysicalDeviceQueueFamilyProperties(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    if (!vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]))
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceQueueFamilyProperties, c->x[0]);
    if (!c->x[1])
        return vkf_end(&k);
    uint32_t n = 1;
    if (c->x[2]) {
        n = vkf_g32(&k, c->x[1]) ? 1 : 0;
        if (n)
            memcpy(mem + c->x[2], &switch_family, sizeof switch_family);
    }
    rl_st32(mem, c->x[1], n);
    return vkf_end(&k);
}

int vkh_vkGetPhysicalDeviceQueueFamilyProperties2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkPhysicalDevice pd = vkf_first(&k, VKG_KIND_VkPhysicalDevice, c->x[0]);
    if (!pd)
        return vkf_bad_handle(&k, VKG_vkGetPhysicalDeviceQueueFamilyProperties2, c->x[0]);
    PFN_vkGetPhysicalDeviceQueueFamilyProperties2 f = VKF_FN(k.fns, vkGetPhysicalDeviceQueueFamilyProperties2);
    if (!f)
        return vkf_no_fn(&k, VKG_vkGetPhysicalDeviceQueueFamilyProperties2);
    if (!c->x[1])
        return vkf_end(&k);
    uint32_t n = 1;
    if (c->x[2]) {
        n = vkf_g32(&k, c->x[1]) ? 1 : 0;
        if (n) {
            VkQueueFamilyProperties2 *a = vkg_ain_VkQueueFamilyProperties2(&k, c->x[2], 1);
            f(pd, &n, a);
            n = 1;
            a->queueFamilyProperties = switch_family;
            vkg_aout_VkQueueFamilyProperties2(&k, c->x[2], a, 1);
        }
    }
    rl_st32(mem, c->x[1], n);
    return vkf_end(&k);
}

VkResult vkf_queue_create_infos(vkf_call *k, VkPhysicalDevice pd, VkDeviceCreateInfo *ci, vkf_dev *d)
{
    PFN_vkGetPhysicalDeviceQueueFamilyProperties f = VKF_FN(k->fns, vkGetPhysicalDeviceQueueFamilyProperties);
    uint32_t n = 1;
    VkQueueFamilyProperties host = { 0 };
    f(pd, &n, &host);
    const uint32_t need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT;
    if (!n || (host.queueFlags & need) != need || !host.queueCount) {
        rl_log(RL_LOG_ERROR, "vk", "host queue family 0 has no graphics and compute queue to stand in for the Switch's");
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    VkDeviceQueueCreateInfo *q = vkf_alloc(k, (ci->queueCreateInfoCount + 1) * sizeof *q);
    for (uint32_t i = 0; i < ci->queueCreateInfoCount; i++) {
        q[i] = ci->pQueueCreateInfos[i];
        if (q[i].queueFamilyIndex != 0 || !q[i].queueCount || q[i].queueCount > VKF_SWITCH_QUEUE_COUNT || d->guest_queues) {
            rl_log(RL_LOG_ERROR, "vk", "vkCreateDevice: %u queues of family %u; the Switch has one family of %u", q[i].queueCount,
                   q[i].queueFamilyIndex, VKF_SWITCH_QUEUE_COUNT);
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        d->guest_queues = q[i].queueCount;
        if (q[i].queueCount > host.queueCount)
            q[i].queueCount = host.queueCount;
        d->host_queues = q[i].queueCount;
    }
    ci->pQueueCreateInfos = q;
    if (d->host_queues < d->guest_queues)
        rl_log(RL_LOG_INFO, "vk", "the guest's %u queues share the host's %u", d->guest_queues, d->host_queues);
    return VK_SUCCESS;
}

static VkQueue host_queue(vkf_call *k, VkDevice dev, uint32_t family, uint32_t index)
{
    vkf_dev *d = k->obj->dev;
    if (family != 0 || index >= d->guest_queues) {
        rl_log(RL_LOG_ERROR, "vk", "vkGetDeviceQueue: no queue %u of family %u on this device", index, family);
        return NULL;
    }
    VkQueue q = NULL;
    VKF_FN(k->fns, vkGetDeviceQueue)(dev, 0, index % d->host_queues, &q);
    return q;
}

int vkh_vkGetDeviceQueue(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkGetDeviceQueue, c->x[0]);
    VkQueue q = host_queue(&k, dev, (uint32_t)c->x[1], (uint32_t)c->x[2]);
    if (c->x[3])
        vkf_s64(&k, c->x[3], vkf_wrap(&k, VKG_KIND_VkQueue, q));
    return vkf_end(&k);
}

int vkh_vkGetDeviceQueue2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkGetDeviceQueue2, c->x[0]);
    VkDeviceQueueInfo2 *info = vkg_ain_VkDeviceQueueInfo2(&k, c->x[1], 1);
    VkQueue q = info && !info->flags ? host_queue(&k, dev, info->queueFamilyIndex, info->queueIndex) : NULL;
    if (c->x[2])
        vkf_s64(&k, c->x[2], vkf_wrap(&k, VKG_KIND_VkQueue, q));
    return vkf_end(&k);
}

void vkf_queue_lock(vkf_call *k)
{
    vkf_dev *d = k->obj->dev;
    if (d->host_queues < d->guest_queues)
        pthread_mutex_lock(&d->queue_lock);
}

void vkf_queue_unlock(vkf_call *k)
{
    vkf_dev *d = k->obj->dev;
    if (d->host_queues < d->guest_queues)
        pthread_mutex_unlock(&d->queue_lock);
}

void vkf_queue_enter(vkf_call *k)
{
    vkf_queue_lock(k);
    vkf_shadow_flush(k);
}

void vkf_queue_pull_leave(vkf_call *k)
{
    vkf_shadow_pull(k);
    vkf_queue_unlock(k);
}

void vkf_wait_enter(vkf_call *k, uint64_t timeout)
{
    if (!timeout || k->left)
        return;
    k->left = true;
    hos_sched_leave();
}

void vkf_wait_return(vkf_call *k)
{
    if (!k->left)
        return;
    k->left = false;
    hos_sched_return();
}

void vkf_fences_wait_enter(vkf_call *k) { vkf_wait_enter(k, k->c->x[4]); }

void vkf_semaphores_wait_enter(vkf_call *k) { vkf_wait_enter(k, k->c->x[2]); }

void vkf_wait_pull_return(vkf_call *k)
{
    vkf_shadow_pull(k);
    vkf_wait_return(k);
}

void vkf_queue_wait_enter(vkf_call *k)
{
    vkf_wait_enter(k, 1);
    vkf_queue_lock(k);
}

void vkf_queue_wait_return(vkf_call *k)
{
    vkf_queue_pull_leave(k);
    vkf_wait_return(k);
}
