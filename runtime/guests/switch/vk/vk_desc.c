/**
 * @file vk_desc.c
 * @brief Descriptor pools that do not run out: when a host pool cannot hold
 * an allocation, the set comes from another host pool created with the same
 * parameters behind the same guest pool.
 *
 * The console driver does not hold a pool to its per-type counts, and games
 * size their pools for it; hosts that do (MoltenVK) would fail allocations
 * the console makes. Freeing, resetting and destroying the guest pool apply
 * to every host pool behind it.
 *
 * Set layouts reach the host with their bindings in binding number order.
 * Any order is valid, but a host driver (Mesa's Turnip) pairs its sorted
 * copy of the bindings with the caller's array when it reads immutable
 * samplers, and reads past the samplers of a binding given out of order.
 */
#include "vkf.h"

#include <stdlib.h>
#include <string.h>

/** @brief A guest descriptor pool: its first host pool's parameters and the host pools added for it. */
typedef struct vkf_dpool {
    /** @brief Create info for more host pools (pNext dropped). */
    VkDescriptorPoolCreateInfo ci;
    /** @brief Pool sizes ci points to. */
    VkDescriptorPoolSize *sizes;
    /** @brief Host pools added, oldest first. */
    VkDescriptorPool *extra;
    /** @brief Number of extra, and room for. */
    uint32_t n_extra, cap_extra;
    /** @brief Pools with VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT: the added host pool of each set from one. */
    vkf_map *owner;
} vkf_dpool;

static vkf_dpool *dpool_of(vkf_dev *d, uint64_t pool)
{
    pthread_mutex_lock(&d->dpool_lock);
    vkf_dpool *p = vkf_map_get(d->dpools, pool);
    pthread_mutex_unlock(&d->dpool_lock);
    return p;
}

static void dpool_free(vkf_dpool *p)
{
    vkf_map_free(p->owner);
    free(p->extra);
    free(p->sizes);
    free(p);
}

int vkh_vkCreateDescriptorPool(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkCreateDescriptorPool, c->x[0]);
    vkf_dev *d = k.obj->dev;
    const VkDescriptorPoolCreateInfo *ci = vkg_ain_VkDescriptorPoolCreateInfo(&k, c->x[1], 1);
    uint64_t gout = c->x[3];
    VkDescriptorPool pool = NULL;
    VkResult r = VKF_FN(k.fns, vkCreateDescriptorPool)(dev, ci, NULL, &pool);
    if (r >= 0 && ci && !ci->pNext) {
        vkf_dpool *p = calloc(1, sizeof *p);
        VkDescriptorPoolSize *sizes = malloc((ci->poolSizeCount + 1) * sizeof *sizes);
        if (!p || !sizes)
            abort();
        memcpy(sizes, ci->pPoolSizes, ci->poolSizeCount * sizeof *sizes);
        p->ci = *ci;
        p->ci.pPoolSizes = sizes;
        p->sizes = sizes;
        if (ci->flags & VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT)
            p->owner = vkf_map_new();
        pthread_mutex_lock(&d->dpool_lock);
        vkf_map_put(d->dpools, (uint64_t)(uintptr_t)pool, p);
        pthread_mutex_unlock(&d->dpool_lock);
    }
    if (r >= 0 && gout)
        vkf_s64(&k, gout, (uint64_t)(uintptr_t)pool);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateDescriptorPool, r);
    return vkf_end(&k);
}

static bool pool_full(VkResult r) { return r == VK_ERROR_OUT_OF_POOL_MEMORY || r == VK_ERROR_FRAGMENTED_POOL; }

static uint32_t twice(uint32_t v) { return v > UINT32_MAX / 2 ? UINT32_MAX : v * 2; }

int vkh_vkAllocateDescriptorSets(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkAllocateDescriptorSets, c->x[0]);
    vkf_dev *d = k.obj->dev;
    VkDescriptorSetAllocateInfo *ai = vkg_ain_VkDescriptorSetAllocateInfo(&k, c->x[1], 1);
    VkDescriptorSet *out = vkf_ptr(&k, c->x[2]);
    PFN_vkAllocateDescriptorSets alloc = VKF_FN(k.fns, vkAllocateDescriptorSets);
    VkResult r = alloc(dev, ai, out);
    vkf_dpool *p = ai && pool_full(r) ? dpool_of(d, (uint64_t)(uintptr_t)ai->descriptorPool) : NULL;
    if (p) {
        VkDescriptorPoolCreateInfo ci = p->ci;
        for (uint32_t i = p->n_extra; i-- > 0 && pool_full(r);) {
            ai->descriptorPool = p->extra[i];
            r = alloc(dev, ai, out);
        }
        for (uint32_t grow = 0; pool_full(r) && grow < 8; grow++) {
            if (p->n_extra == p->cap_extra) {
                p->cap_extra = p->cap_extra ? p->cap_extra * 2 : 4;
                p->extra = realloc(p->extra, p->cap_extra * sizeof *p->extra);
                if (!p->extra)
                    abort();
            }
            VkDescriptorPool np = NULL;
            VkResult cr = VKF_FN(k.fns, vkCreateDescriptorPool)(dev, &ci, NULL, &np);
            if (cr < 0) {
                r = cr;
                break;
            }
            ai->descriptorPool = np;
            r = alloc(dev, ai, out);
            if (!pool_full(r)) {
                if (rl_once(0x44504F4F4C000000ull))
                    rl_log(RL_LOG_INFO, "vk", "a descriptor pool ran out; adding host pools behind it");
                p->extra[p->n_extra++] = np;
                break;
            }
            VKF_FN(k.fns, vkDestroyDescriptorPool)(dev, np, NULL);
            for (uint32_t i = 0; i < ci.poolSizeCount; i++)
                p->sizes[i].descriptorCount = twice(p->sizes[i].descriptorCount);
            p->ci.maxSets = twice(p->ci.maxSets);
            ci = p->ci;
        }
        if (r >= 0 && p->owner)
            for (uint32_t i = 0; i < ai->descriptorSetCount; i++)
                vkf_map_put(p->owner, (uint64_t)(uintptr_t)out[i], (void *)ai->descriptorPool);
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkAllocateDescriptorSets, r);
    return vkf_end(&k);
}

int vkh_vkFreeDescriptorSets(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkFreeDescriptorSets, c->x[0]);
    VkDescriptorPool pool = (VkDescriptorPool)(uintptr_t)c->x[1];
    uint32_t n = (uint32_t)c->x[2];
    VkDescriptorSet *sets = vkf_ptr(&k, c->x[3]);
    PFN_vkFreeDescriptorSets fr = VKF_FN(k.fns, vkFreeDescriptorSets);
    vkf_dpool *p = dpool_of(k.obj->dev, (uint64_t)(uintptr_t)pool);
    if (!p || !p->n_extra || !p->owner || !sets) {
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkFreeDescriptorSets, fr(dev, pool, n, sets));
        return vkf_end(&k);
    }
    VkDescriptorSet *mine = vkf_alloc(&k, (n + 1) * sizeof *mine);
    uint32_t m = 0;
    VkResult r = VK_SUCCESS;
    for (uint32_t i = 0; i < n; i++) {
        VkDescriptorPool owner = sets[i] ? vkf_map_get(p->owner, (uint64_t)(uintptr_t)sets[i]) : NULL;
        if (!owner) {
            mine[m++] = sets[i];
            continue;
        }
        vkf_map_put(p->owner, (uint64_t)(uintptr_t)sets[i], NULL);
        VkResult e = fr(dev, owner, 1, &sets[i]);
        if (e < 0)
            r = e;
    }
    if (m) {
        VkResult e = fr(dev, pool, m, mine);
        if (e < 0)
            r = e;
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkFreeDescriptorSets, r);
    return vkf_end(&k);
}

int vkh_vkResetDescriptorPool(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkResetDescriptorPool, c->x[0]);
    VkDescriptorPool pool = (VkDescriptorPool)(uintptr_t)c->x[1];
    VkDescriptorPoolResetFlags flags = (VkDescriptorPoolResetFlags)c->x[2];
    PFN_vkResetDescriptorPool rs = VKF_FN(k.fns, vkResetDescriptorPool);
    VkResult r = rs(dev, pool, flags);
    vkf_dpool *p = dpool_of(k.obj->dev, (uint64_t)(uintptr_t)pool);
    for (uint32_t i = 0; p && i < p->n_extra; i++) {
        VkResult e = rs(dev, p->extra[i], flags);
        if (e < 0)
            r = e;
    }
    if (p && p->owner) {
        vkf_map_free(p->owner);
        p->owner = vkf_map_new();
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkResetDescriptorPool, r);
    return vkf_end(&k);
}

static void destroy_extra(vkf_call *k, VkDevice dev, vkf_dpool *p)
{
    for (uint32_t i = 0; i < p->n_extra; i++)
        VKF_FN(k->fns, vkDestroyDescriptorPool)(dev, p->extra[i], NULL);
}

int vkh_vkDestroyDescriptorPool(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkDestroyDescriptorPool, c->x[0]);
    vkf_dev *d = k.obj->dev;
    uint64_t pool = c->x[1];
    VKF_FN(k.fns, vkDestroyDescriptorPool)(dev, (VkDescriptorPool)(uintptr_t)pool, NULL);
    pthread_mutex_lock(&d->dpool_lock);
    vkf_dpool *p = vkf_map_get(d->dpools, pool);
    vkf_map_put(d->dpools, pool, NULL);
    pthread_mutex_unlock(&d->dpool_lock);
    if (p) {
        destroy_extra(&k, dev, p);
        dpool_free(p);
    }
    return vkf_end(&k);
}

typedef struct {
    vkf_call *k;
    VkDevice dev;
} teardown_ctx;

static void teardown_one(uint64_t key, void *value, void *user)
{
    (void)key;
    teardown_ctx *t = user;
    destroy_extra(t->k, t->dev, value);
    dpool_free(value);
}

void vkf_desc_device_teardown(vkf_call *k, vkf_dev *d, void *dev)
{
    teardown_ctx t = { k, dev };
    vkf_map_each(d->dpools, teardown_one, &t);
    vkf_map_free(d->dpools);
    d->dpools = NULL;
}

/** @brief The binding flags in a set layout's chain, or NULL. */
static VkDescriptorSetLayoutBindingFlagsCreateInfo *binding_flags(VkDescriptorSetLayoutCreateInfo *ci)
{
    for (VkBaseOutStructure *b = (VkBaseOutStructure *)(uintptr_t)ci->pNext; b; b = b->pNext)
        if (b->sType == VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO)
            return (VkDescriptorSetLayoutBindingFlagsCreateInfo *)b;
    return NULL;
}

/** @brief Puts the bindings of a converted set layout in binding number order, with the binding flags that go with
 * them. The copies are in the call's arena. */
static void sort_bindings(vkf_call *k, VkDescriptorSetLayoutCreateInfo *ci)
{
    if (!ci || ci->bindingCount < 2 || !ci->pBindings)
        return;
    uint32_t n = ci->bindingCount;
    bool sorted = true;
    for (uint32_t i = 1; i < n && sorted; i++)
        sorted = ci->pBindings[i - 1].binding <= ci->pBindings[i].binding;
    if (sorted)
        return;
    uint32_t *order = vkf_alloc(k, n * sizeof *order);
    for (uint32_t i = 0; i < n; i++) {
        uint32_t j = i;
        for (; j > 0 && ci->pBindings[order[j - 1]].binding > ci->pBindings[i].binding; j--)
            order[j] = order[j - 1];
        order[j] = i;
    }
    VkDescriptorSetLayoutBinding *b = vkf_alloc(k, n * sizeof *b);
    for (uint32_t i = 0; i < n; i++)
        b[i] = ci->pBindings[order[i]];
    ci->pBindings = b;
    VkDescriptorSetLayoutBindingFlagsCreateInfo *f = binding_flags(ci);
    if (f && f->bindingCount == n && f->pBindingFlags) {
        VkDescriptorBindingFlags *flags = vkf_alloc(k, n * sizeof *flags);
        for (uint32_t i = 0; i < n; i++)
            flags[i] = f->pBindingFlags[order[i]];
        f->pBindingFlags = flags;
    }
}

int vkh_vkCreateDescriptorSetLayout(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkCreateDescriptorSetLayout, c->x[0]);
    VkResult r = vkf_check_set_layout(&k);
    if (r != VK_SUCCESS) {
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateDescriptorSetLayout, r);
        return vkf_end(&k);
    }
    VkDescriptorSetLayoutCreateInfo *ci = vkg_ain_VkDescriptorSetLayoutCreateInfo(&k, c->x[1], 1);
    sort_bindings(&k, ci);
    VkDescriptorSetLayout *out = (VkDescriptorSetLayout *)vkf_ptr(&k, c->x[3]);
    r = VKF_FN(k.fns, vkCreateDescriptorSetLayout)(dev, ci, NULL, out);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateDescriptorSetLayout, r);
    if (r == VK_SUCCESS)
        vkf_note_set_layout(&k);
    return vkf_end(&k);
}

int vkh_vkGetDescriptorSetLayoutSupport(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkGetDescriptorSetLayoutSupport, c->x[0]);
    VkDescriptorSetLayoutCreateInfo *ci = vkg_ain_VkDescriptorSetLayoutCreateInfo(&k, c->x[1], 1);
    sort_bindings(&k, ci);
    VkDescriptorSetLayoutSupport *support = vkg_ain_VkDescriptorSetLayoutSupport(&k, c->x[2], 1);
    PFN_vkGetDescriptorSetLayoutSupport fn =
        (PFN_vkGetDescriptorSetLayoutSupport)k.fns->f[VKG_vkGetDescriptorSetLayoutSupport];
    if (!fn)
        return vkf_no_fn(&k, VKG_vkGetDescriptorSetLayoutSupport);
    fn(dev, ci, support);
    vkg_aout_VkDescriptorSetLayoutSupport(&k, c->x[2], support, 1);
    return vkf_end(&k);
}
