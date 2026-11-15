/**
 * @file vk_submit.c
 * @brief Queue submits, with secondary command buffers submitted through a
 * primary.
 *
 * The console driver runs a secondary command buffer handed straight to
 * vkQueueSubmit, and a game does that; MoltenVK refuses it. A secondary that
 * reaches a submit is replaced by a primary of the layer's own that executes
 * it. The primary is recorded again when the guest has begun the secondary
 * again since: the guest may only do that once its earlier submits are
 * done, and so then the primary is not pending either.
 *
 * The console driver also begins a secondary without inheritance info, which
 * a game does and Vulkan does not allow; such a begin reaches the host with
 * empty inheritance info (no render pass, no queries).
 */
#include "vkf.h"

#include <stdlib.h>

/** @brief The primary that runs one secondary. */
typedef struct vkf_wrapper {
    /** @brief Host primary command buffer, from the device's wrap pool. */
    VkCommandBuffer primary;
    /** @brief The secondary's begin count when the primary was recorded. */
    uint64_t gen;
} vkf_wrapper;

static uint64_t g_begins;

int vkh_vkBeginCommandBuffer(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkCommandBuffer cb = vkf_first(&k, VKG_KIND_VkCommandBuffer, c->x[0]);
    if (!cb)
        return vkf_bad_handle(&k, VKG_vkBeginCommandBuffer, c->x[0]);
    VkCommandBufferBeginInfo *bi = vkg_ain_VkCommandBufferBeginInfo(&k, c->x[1], 1);
    vkf_obj *o = k.obj;
    if (o->secondary && bi) {
        o->begin_flags = bi->flags;
        o->gen = __atomic_add_fetch(&g_begins, 1, __ATOMIC_RELAXED);
        if (!bi->pInheritanceInfo) {
            VkCommandBufferInheritanceInfo *inh = vkf_alloc(&k, sizeof *inh);
            *inh = (VkCommandBufferInheritanceInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
            bi->pInheritanceInfo = inh;
        }
    }
    c->x[0] = (uint32_t)VKF_FN(k.fns, vkBeginCommandBuffer)(cb, bi);
    return vkf_end(&k);
}

static VkCommandBuffer wrapped(vkf_call *k, VkDevice dev, vkf_dev *d, VkCommandBuffer cb)
{
    vkf_obj *o = vkf_obj_find_host(VKG_KIND_VkCommandBuffer, cb);
    if (!o || !o->secondary)
        return cb;
    if (o->begin_flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT) {
        if (rl_once(0x5345434F4E440000ull))
            rl_log(RL_LOG_WARN, "vk", "a secondary command buffer that continues a render pass was submitted");
        return cb;
    }
    VkCommandBuffer out = cb;
    pthread_mutex_lock(&d->wrap_lock);
    if (!d->wrap_pool) {
        VkCommandPoolCreateInfo pci = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        };
        if (VKF_FN(k->fns, vkCreateCommandPool)(dev, &pci, NULL, &d->wrap_pool) < 0)
            d->wrap_pool = NULL;
        else
            rl_log(RL_LOG_INFO, "vk", "the guest submits secondary command buffers; they run through primaries");
    }
    vkf_wrapper *w = d->wrap_pool ? vkf_map_get(d->wraps, (uint64_t)(uintptr_t)cb) : NULL;
    if (d->wrap_pool && !w) {
        w = calloc(1, sizeof *w);
        if (!w)
            abort();
        VkCommandBufferAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = d->wrap_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        if (VKF_FN(k->fns, vkAllocateCommandBuffers)(dev, &ai, &w->primary) < 0) {
            free(w);
            w = NULL;
        } else
            vkf_map_put(d->wraps, (uint64_t)(uintptr_t)cb, w);
    }
    if (w && w->gen != o->gen) {
        VkCommandBufferBeginInfo bi = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = o->begin_flags & VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
        };
        VKF_FN(k->fns, vkResetCommandBuffer)(w->primary, 0);
        VKF_FN(k->fns, vkBeginCommandBuffer)(w->primary, &bi);
        VKF_FN(k->fns, vkCmdExecuteCommands)(w->primary, 1, &cb);
        if (VKF_FN(k->fns, vkEndCommandBuffer)(w->primary) >= 0)
            w->gen = o->gen;
    }
    if (w && w->gen == o->gen)
        out = w->primary;
    pthread_mutex_unlock(&d->wrap_lock);
    return out;
}

static const VkCommandBuffer *wrap_list(vkf_call *k, VkDevice dev, vkf_dev *d, const VkCommandBuffer *cbs, uint32_t n)
{
    VkCommandBuffer *out = NULL;
    for (uint32_t i = 0; i < n; i++) {
        VkCommandBuffer w = wrapped(k, dev, d, cbs[i]);
        if (w != cbs[i] && !out) {
            out = vkf_alloc(k, n * sizeof *out);
            for (uint32_t j = 0; j < n; j++)
                out[j] = cbs[j];
        }
        if (out)
            out[i] = w;
    }
    return out ? out : cbs;
}

static VkDevice queue_device(vkf_call *k) { return (VkDevice)k->obj->parent->host; }

int vkh_vkQueueSubmit(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkQueue q = vkf_first(&k, VKG_KIND_VkQueue, c->x[0]);
    if (!q)
        return vkf_bad_handle(&k, VKG_vkQueueSubmit, c->x[0]);
    uint32_t n = (uint32_t)c->x[1];
    VkSubmitInfo *si = vkg_ain_VkSubmitInfo(&k, c->x[2], n);
    vkf_dev *d = k.obj->dev;
    if (__atomic_load_n(&d->secondaries, __ATOMIC_RELAXED))
        for (uint32_t i = 0; si && i < n; i++)
            si[i].pCommandBuffers =
                wrap_list(&k, queue_device(&k), d, si[i].pCommandBuffers, si[i].commandBufferCount);
    vkf_queue_enter(&k);
    VkResult r = VKF_FN(k.fns, vkQueueSubmit)(q, n, si, (VkFence)(uintptr_t)c->x[3]);
    vkf_queue_unlock(&k);
    c->x[0] = (uint32_t)r;
    return vkf_end(&k);
}

int vkh_vkQueueSubmit2(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkQueue q = vkf_first(&k, VKG_KIND_VkQueue, c->x[0]);
    if (!q)
        return vkf_bad_handle(&k, VKG_vkQueueSubmit2, c->x[0]);
    PFN_vkQueueSubmit2 fn = VKF_FN(k.fns, vkQueueSubmit2);
    if (!fn)
        return vkf_no_fn(&k, VKG_vkQueueSubmit2);
    uint32_t n = (uint32_t)c->x[1];
    VkSubmitInfo2 *si = vkg_ain_VkSubmitInfo2(&k, c->x[2], n);
    vkf_dev *d = k.obj->dev;
    if (__atomic_load_n(&d->secondaries, __ATOMIC_RELAXED))
        for (uint32_t i = 0; si && i < n; i++) {
            VkCommandBufferSubmitInfo *ci = (VkCommandBufferSubmitInfo *)si[i].pCommandBufferInfos;
            for (uint32_t j = 0; ci && j < si[i].commandBufferInfoCount; j++)
                ci[j].commandBuffer = wrapped(&k, queue_device(&k), d, ci[j].commandBuffer);
        }
    vkf_queue_enter(&k);
    VkResult r = fn(q, n, si, (VkFence)(uintptr_t)c->x[3]);
    vkf_queue_unlock(&k);
    c->x[0] = (uint32_t)r;
    return vkf_end(&k);
}

void vkf_wrap_drop(const vkf_fns *fns, void *dev, vkf_dev *d, void *secondary)
{
    __atomic_sub_fetch(&d->secondaries, 1, __ATOMIC_RELAXED);
    pthread_mutex_lock(&d->wrap_lock);
    vkf_wrapper *w = vkf_map_get(d->wraps, (uint64_t)(uintptr_t)secondary);
    if (w) {
        vkf_map_put(d->wraps, (uint64_t)(uintptr_t)secondary, NULL);
        VKF_FN(fns, vkFreeCommandBuffers)((VkDevice)dev, d->wrap_pool, 1, &w->primary);
        free(w);
    }
    pthread_mutex_unlock(&d->wrap_lock);
}

static void free_one(uint64_t key, void *value, void *user)
{
    (void)key;
    (void)user;
    free(value);
}

void vkf_wrap_device_teardown(const vkf_fns *fns, void *dev, vkf_dev *d)
{
    if (d->wrap_pool)
        VKF_FN(fns, vkDestroyCommandPool)((VkDevice)dev, d->wrap_pool, NULL);
    d->wrap_pool = NULL;
    vkf_map_each(d->wraps, free_one, NULL);
    vkf_map_free(d->wraps);
    d->wraps = NULL;
}
