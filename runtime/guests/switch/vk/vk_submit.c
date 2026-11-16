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
 * A game also frees or begins again a secondary it has just submitted,
 * without waiting for it (a game's movie player at the end of a movie),
 * which the console driver allows. The primary that runs it is the layer's
 * own and freeing or recording a pending command buffer is undefined on the
 * host, so each submit that runs primaries also signals a fence of the
 * layer's (an empty submit after it on the same queue), and a primary let
 * go of while its fence is unsignaled waits on a list until a later submit
 * or the device's end finds the fence signaled. Nothing waits on the host.
 *
 * The console driver also begins a secondary without inheritance info, which
 * a game does and Vulkan does not allow, and records whole render passes in
 * it, which Vulkan allows only in a primary (the Adreno 830 driver faults
 * executing such a secondary). Such a begin records into a primary twin of
 * the buffer instead, from the same pool, which the submit takes as it is;
 * a later begin with inheritance info goes back to the secondary.
 */
#include "vkf.h"

#include <stdlib.h>

/** @brief A submission that ran primaries: the fence an empty submit after it on the same queue signals. */
typedef struct vkf_wsub {
    /** @brief The layer's fence. */
    VkFence fence;
    /** @brief Wrappers and retired entries that hold it. */
    uint32_t refs;
    /** @brief The empty submit that signals the fence was made. */
    bool sent;
    /** @brief The fence was seen signaled. */
    bool done;
    /** @brief Next in the device's list. */
    struct vkf_wsub *next;
} vkf_wsub;

/** @brief What the guest let go of while a submission may still run it: freed once every one of subs is done. */
typedef struct vkf_wdead {
    /** @brief A primary of the wrap pool to free, or NULL. */
    VkCommandBuffer primary;
    /** @brief Guest command buffers to free from pool (a secondary and its primary twin), or NULL. */
    VkCommandBuffer guest[2];
    /** @brief The guest pool of guest, or the pool to destroy. */
    VkCommandPool pool;
    /** @brief Destroy pool rather than free guest from it. */
    bool destroy_pool;
    /** @brief The submissions to wait for, one reference each. */
    vkf_wsub **subs;
    /** @brief Number of subs. */
    uint32_t nsubs;
    /** @brief Next in the device's list. */
    struct vkf_wdead *next;
} vkf_wdead;

/** @brief The primary that runs one secondary, or for a primary twin only the submissions that ran it. */
typedef struct vkf_wrapper {
    /** @brief Host primary command buffer, from the device's wrap pool; NULL for a twin. */
    VkCommandBuffer primary;
    /** @brief The secondary's begin count when the primary was recorded. */
    uint64_t gen;
    /** @brief The guest pool of the secondary or twin. */
    VkCommandPool pool;
    /** @brief The submissions that last ran the primary, one per queue (at most two queues kept, the latest first). */
    vkf_wsub *sub[2];
    /** @brief The queue of each of sub. */
    VkQueue queue[2];
} vkf_wrapper;

static bool sub_done(const vkf_fns *fns, VkDevice dev, vkf_wsub *u)
{
    if (!u)
        return true;
    if (!u->sent)
        return false;
    if (!u->done)
        u->done = VKF_FN(fns, vkGetFenceStatus)(dev, u->fence) == VK_SUCCESS;
    return u->done;
}

static void sub_put(vkf_wsub *u)
{
    if (u)
        u->refs--;
}

static void free_guest(const vkf_fns *fns, VkDevice dev, vkf_wdead *e)
{
    for (int i = 0; i < 2; i++)
        if (e->guest[i])
            VKF_FN(fns, vkFreeCommandBuffers)(dev, e->pool, 1, &e->guest[i]);
}

static bool wrapper_idle(const vkf_fns *fns, VkDevice dev, vkf_wrapper *w)
{
    return sub_done(fns, dev, w->sub[0]) && sub_done(fns, dev, w->sub[1]);
}

/** @brief Moves w's submissions into e. Wrap lock held. */
static void take_subs(vkf_wdead *e, vkf_wrapper *w)
{
    if (!w)
        return;
    vkf_wsub **subs = realloc(e->subs, (e->nsubs + 2) * sizeof *subs);
    if (!subs)
        abort();
    e->subs = subs;
    for (int i = 0; i < 2; i++)
        if (w->sub[i]) {
            e->subs[e->nsubs++] = w->sub[i];
            w->sub[i] = NULL;
        }
}

/** @brief Moves w's submissions into a new retired entry for primary and pool. Wrap lock held. */
static vkf_wdead *retire(vkf_dev *d, vkf_wrapper *w, VkCommandBuffer primary, VkCommandPool pool)
{
    vkf_wdead *e = calloc(1, sizeof *e);
    if (!e)
        abort();
    e->primary = primary;
    e->pool = pool;
    take_subs(e, w);
    e->next = d->wdead;
    d->wdead = e;
    return e;
}

/** @brief Frees what is retired and done and recycles the fences no one holds. Wrap lock held. */
static void reap(const vkf_fns *fns, VkDevice dev, vkf_dev *d)
{
    for (vkf_wdead **pe = &d->wdead; *pe;) {
        vkf_wdead *e = *pe;
        bool done = true;
        for (uint32_t i = 0; i < e->nsubs && done; i++)
            done = sub_done(fns, dev, e->subs[i]);
        if (!done) {
            pe = &e->next;
            continue;
        }
        *pe = e->next;
        if (e->primary)
            VKF_FN(fns, vkFreeCommandBuffers)(dev, d->wrap_pool, 1, &e->primary);
        if (e->destroy_pool)
            VKF_FN(fns, vkDestroyCommandPool)(dev, e->pool, NULL);
        else
            free_guest(fns, dev, e);
        for (uint32_t i = 0; i < e->nsubs; i++)
            sub_put(e->subs[i]);
        free(e->subs);
        free(e);
    }
    for (vkf_wsub **pu = &d->wsubs; *pu;) {
        vkf_wsub *u = *pu;
        if (u->refs || (u->sent && !sub_done(fns, dev, u))) {
            pu = &u->next;
            continue;
        }
        *pu = u->next;
        if (u->sent)
            VKF_FN(fns, vkResetFences)(dev, 1, &u->fence);
        u->sent = u->done = false;
        u->next = d->wsub_free;
        d->wsub_free = u;
    }
}

/** @brief A fresh submission record with its fence unsignaled, or NULL. Wrap lock held. */
static vkf_wsub *sub_new(const vkf_fns *fns, VkDevice dev, vkf_dev *d)
{
    vkf_wsub *u = d->wsub_free;
    if (u)
        d->wsub_free = u->next;
    else {
        u = calloc(1, sizeof *u);
        if (!u)
            abort();
        VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (VKF_FN(fns, vkCreateFence)(dev, &fci, NULL, &u->fence) < 0) {
            free(u);
            return NULL;
        }
    }
    u->next = d->wsubs;
    d->wsubs = u;
    return u;
}

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
        bool primary = !bi->pInheritanceInfo && !(bi->flags & VK_COMMAND_BUFFER_USAGE_RENDER_PASS_CONTINUE_BIT);
        if (primary && !o->other) {
            VkCommandBufferAllocateInfo ai = {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .commandPool = (VkCommandPool)(uintptr_t)o->pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = 1,
            };
            VkCommandBuffer twin = NULL;
            if (VKF_FN(k.fns, vkAllocateCommandBuffers)(o->parent->host, &ai, &twin) >= 0) {
                o->other = twin;
                if (rl_once(0x5457494e00000000ull))
                    rl_log(RL_LOG_INFO, "vk", "the guest begins secondary command buffers as primaries; they record into primaries");
            }
        }
        if (o->other && primary != o->as_primary) {
            vkf_obj_swap_twin(o);
            o->as_primary = primary;
            cb = o->host;
        }
        o->begin_flags = bi->flags;
        o->gen = __atomic_add_fetch(&g_begins, 1, __ATOMIC_RELAXED);
        if (!o->as_primary && !bi->pInheritanceInfo) {
            VkCommandBufferInheritanceInfo *inh = vkf_alloc(&k, sizeof *inh);
            *inh = (VkCommandBufferInheritanceInfo){ .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_INHERITANCE_INFO };
            bi->pInheritanceInfo = inh;
        }
    }
    vkf_depth_cb_begin(o, bi);
    if (bi && bi->pInheritanceInfo)
        vkf_depth_format_list(&k, o->dev, bi->pInheritanceInfo->pNext);
    vkf_decode_cb_reset(k.fns, o->parent->host, o->dev, cb, false);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkBeginCommandBuffer, VKF_FN(k.fns, vkBeginCommandBuffer)(cb, bi));
    return vkf_end(&k);
}

/** @brief The secondaries one submit runs through primaries. */
typedef struct vkf_used {
    /** @brief Their host handles. */
    VkCommandBuffer *cb;
    /** @brief Number of cb, and room for. */
    uint32_t n, cap;
} vkf_used;

static void note_used(vkf_call *k, vkf_used *u, VkCommandBuffer cb)
{
    if (u->n == u->cap) {
        uint32_t cap = u->cap ? u->cap * 2 : 8;
        VkCommandBuffer *g = vkf_alloc(k, cap * sizeof *g);
        for (uint32_t i = 0; i < u->n; i++)
            g[i] = u->cb[i];
        u->cb = g;
        u->cap = cap;
    }
    u->cb[u->n++] = cb;
}

/** @brief Notes a primary twin a submit runs as it is, so that it is freed only once that submit is done. */
static void twin_used(vkf_call *k, vkf_dev *d, const vkf_obj *o, VkCommandBuffer cb, vkf_used *used)
{
    pthread_mutex_lock(&d->wrap_lock);
    vkf_wrapper *w = vkf_map_get(d->wraps, (uint64_t)(uintptr_t)cb);
    if (!w) {
        w = calloc(1, sizeof *w);
        if (!w)
            abort();
        w->pool = (VkCommandPool)(uintptr_t)o->pool;
        vkf_map_put(d->wraps, (uint64_t)(uintptr_t)cb, w);
    }
    note_used(k, used, cb);
    pthread_mutex_unlock(&d->wrap_lock);
}

static VkCommandBuffer wrapped(vkf_call *k, VkDevice dev, vkf_dev *d, VkCommandBuffer cb, vkf_used *used)
{
    vkf_obj *o = vkf_obj_find_host(VKG_KIND_VkCommandBuffer, cb);
    if (!o || !o->secondary)
        return cb;
    if (o->as_primary) {
        twin_used(k, d, o, cb, used);
        return cb;
    }
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
        } else {
            w->pool = (VkCommandPool)(uintptr_t)o->pool;
            vkf_map_put(d->wraps, (uint64_t)(uintptr_t)cb, w);
        }
    }
    if (w && w->gen != o->gen && !wrapper_idle(k->fns, dev, w)) {
        VkCommandBufferAllocateInfo ai = {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = d->wrap_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = 1,
        };
        VkCommandBuffer fresh = NULL;
        if (VKF_FN(k->fns, vkAllocateCommandBuffers)(dev, &ai, &fresh) >= 0) {
            retire(d, w, w->primary, NULL);
            w->primary = fresh;
        }
    }
    if (w && w->gen != o->gen && wrapper_idle(k->fns, dev, w)) {
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
    if (w && w->gen == o->gen) {
        out = w->primary;
        note_used(k, used, cb);
    }
    pthread_mutex_unlock(&d->wrap_lock);
    return out;
}

/** @brief After a submit to q that ran the primaries of used: an empty submit signals a fence of the layer's when they are done. Queue lock held. */
static void track(vkf_call *k, VkDevice dev, vkf_dev *d, VkQueue q, const vkf_used *used)
{
    pthread_mutex_lock(&d->wrap_lock);
    reap(k->fns, dev, d);
    vkf_wsub *u = used->n ? sub_new(k->fns, dev, d) : NULL;
    if (u && VKF_FN(k->fns, vkQueueSubmit)(q, 0, NULL, u->fence) >= 0)
        u->sent = true;
    for (uint32_t i = 0; u && u->sent && i < used->n; i++) {
        vkf_wrapper *w = vkf_map_get(d->wraps, (uint64_t)(uintptr_t)used->cb[i]);
        if (!w || (w->sub[0] == u))
            continue;
        int slot = w->queue[0] == q || !w->sub[0] ? 0 : 1;
        if (slot == 1) {
            sub_put(w->sub[1]);
            w->sub[1] = w->sub[0];
            w->queue[1] = w->queue[0];
        } else
            sub_put(w->sub[0]);
        w->sub[0] = u;
        w->queue[0] = q;
        u->refs++;
    }
    pthread_mutex_unlock(&d->wrap_lock);
}

static const VkCommandBuffer *wrap_list(vkf_call *k, VkDevice dev, vkf_dev *d, const VkCommandBuffer *cbs, uint32_t n, vkf_used *used)
{
    VkCommandBuffer *out = NULL;
    for (uint32_t i = 0; i < n; i++) {
        VkCommandBuffer w = wrapped(k, dev, d, cbs[i], used);
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
    for (uint32_t i = 0; d->decoding && si && i < n; i++)
        vkf_decode_uploads(&k, queue_device(&k), si[i].pCommandBuffers, si[i].commandBufferCount);
    vkf_used used = { 0 };
    if (__atomic_load_n(&d->secondaries, __ATOMIC_RELAXED))
        for (uint32_t i = 0; si && i < n; i++)
            si[i].pCommandBuffers =
                wrap_list(&k, queue_device(&k), d, si[i].pCommandBuffers, si[i].commandBufferCount, &used);
    vkf_queue_enter(&k);
    VkResult r = VKF_FN(k.fns, vkQueueSubmit)(q, n, si, (VkFence)(uintptr_t)c->x[3]);
    if (r >= 0 && (used.n || d->wdead))
        track(&k, queue_device(&k), d, q, &used);
    vkf_queue_unlock(&k);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkQueueSubmit, r);
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
    for (uint32_t i = 0; d->decoding && si && i < n; i++)
        for (uint32_t j = 0; j < si[i].commandBufferInfoCount; j++)
            vkf_decode_uploads(&k, queue_device(&k), &si[i].pCommandBufferInfos[j].commandBuffer, 1);
    vkf_used used = { 0 };
    if (__atomic_load_n(&d->secondaries, __ATOMIC_RELAXED))
        for (uint32_t i = 0; si && i < n; i++) {
            VkCommandBufferSubmitInfo *ci = (VkCommandBufferSubmitInfo *)si[i].pCommandBufferInfos;
            for (uint32_t j = 0; ci && j < si[i].commandBufferInfoCount; j++)
                ci[j].commandBuffer = wrapped(&k, queue_device(&k), d, ci[j].commandBuffer, &used);
        }
    vkf_queue_enter(&k);
    VkResult r = fn(q, n, si, (VkFence)(uintptr_t)c->x[3]);
    if (r >= 0 && (used.n || d->wdead))
        track(&k, queue_device(&k), d, q, &used);
    vkf_queue_unlock(&k);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkQueueSubmit2, r);
    return vkf_end(&k);
}

bool vkf_wrap_drop(const vkf_fns *fns, void *dev, vkf_dev *d, void *secondary, void *twin, bool keep)
{
    __atomic_sub_fetch(&d->secondaries, 1, __ATOMIC_RELAXED);
    bool deferred = false;
    pthread_mutex_lock(&d->wrap_lock);
    void *cb[2] = { secondary, twin };
    vkf_wrapper *w[2] = { NULL, NULL };
    bool idle = true;
    for (int i = 0; i < 2; i++)
        if (cb[i] && (w[i] = vkf_map_get(d->wraps, (uint64_t)(uintptr_t)cb[i]))) {
            vkf_map_put(d->wraps, (uint64_t)(uintptr_t)cb[i], NULL);
            idle = idle && wrapper_idle(fns, (VkDevice)dev, w[i]);
        }
    if (idle)
        for (int i = 0; i < 2; i++) {
            if (w[i] && w[i]->primary)
                VKF_FN(fns, vkFreeCommandBuffers)((VkDevice)dev, d->wrap_pool, 1, &w[i]->primary);
            if (w[i]) {
                sub_put(w[i]->sub[0]);
                sub_put(w[i]->sub[1]);
            }
        }
    else {
        vkf_wdead *e = retire(d, w[0], w[0] ? w[0]->primary : NULL, (w[0] ? w[0] : w[1])->pool);
        take_subs(e, w[1]);
        if (!keep) {
            e->guest[0] = (VkCommandBuffer)secondary;
            e->guest[1] = (VkCommandBuffer)twin;
        }
        deferred = !keep;
    }
    free(w[0]);
    free(w[1]);
    pthread_mutex_unlock(&d->wrap_lock);
    return deferred;
}

bool vkf_wrap_pool_destroy(const vkf_fns *fns, void *dev, vkf_dev *d, uint64_t pool)
{
    VkCommandPool p = (VkCommandPool)(uintptr_t)pool;
    pthread_mutex_lock(&d->wrap_lock);
    vkf_wdead *keep = NULL;
    uint32_t n = 0;
    for (vkf_wdead *e = d->wdead; e; e = e->next)
        if (!e->destroy_pool && e->pool == p)
            n += e->nsubs;
    if (n) {
        keep = retire(d, NULL, NULL, p);
        keep->destroy_pool = true;
        keep->subs = calloc(n, sizeof *keep->subs);
        if (!keep->subs)
            abort();
        for (vkf_wdead *e = d->wdead; e; e = e->next)
            if (e != keep && !e->destroy_pool && e->pool == p) {
                e->guest[0] = e->guest[1] = NULL;
                for (uint32_t i = 0; i < e->nsubs; i++) {
                    keep->subs[keep->nsubs++] = e->subs[i];
                    e->subs[i]->refs++;
                }
            }
    }
    reap(fns, (VkDevice)dev, d);
    pthread_mutex_unlock(&d->wrap_lock);
    return keep != NULL;
}

static void free_one(uint64_t key, void *value, void *user)
{
    (void)key;
    (void)user;
    free(value);
}

void vkf_wrap_device_teardown(const vkf_fns *fns, void *dev, vkf_dev *d)
{
    for (vkf_wsub *u = d->wsubs; u; u = u->next)
        if (u->sent && !u->done)
            VKF_FN(fns, vkWaitForFences)((VkDevice)dev, 1, &u->fence, VK_TRUE, UINT64_MAX);
    for (vkf_wdead *e = d->wdead; e;) {
        vkf_wdead *next = e->next;
        if (e->destroy_pool)
            VKF_FN(fns, vkDestroyCommandPool)((VkDevice)dev, e->pool, NULL);
        else
            free_guest(fns, (VkDevice)dev, e);
        free(e->subs);
        free(e);
        e = next;
    }
    d->wdead = NULL;
    for (int l = 0; l < 2; l++)
        for (vkf_wsub *u = l ? d->wsub_free : d->wsubs; u;) {
            vkf_wsub *next = u->next;
            VKF_FN(fns, vkDestroyFence)((VkDevice)dev, u->fence, NULL);
            free(u);
            u = next;
        }
    d->wsubs = d->wsub_free = NULL;
    if (d->wrap_pool)
        VKF_FN(fns, vkDestroyCommandPool)((VkDevice)dev, d->wrap_pool, NULL);
    d->wrap_pool = NULL;
    vkf_map_each(d->wraps, free_one, NULL);
    vkf_map_free(d->wraps);
    d->wraps = NULL;
}
