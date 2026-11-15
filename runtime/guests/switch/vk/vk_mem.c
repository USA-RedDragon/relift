/**
 * @file vk_mem.c
 * @brief Device memory the guest maps, inside the guest reservation.
 *
 * Per device, the layer picks the first mode that works (spike 0.5): placed
 * mapping (VK_EXT_map_memory_placed with memoryUnmapReserve, the driver maps
 * at mem + addr and leaves the range reserved on unmap), host-pointer import
 * (VK_EXT_external_memory_host: a host-visible allocation is backed by guest
 * pages), or a shadow copy (the guest gets its own pages, synchronized at
 * flush, invalidate, submit and wait against a third copy holding the bytes
 * as of the last synchronization: toward the host only the bytes the guest
 * changed move, toward the guest only the bytes the device changed, so
 * neither side's writes overwrite the other's). The guest range for each
 * mapping comes from the map window of rl_vk_config.
 */
#include "vkf.h"

#include <stdlib.h>
#include <string.h>

/** @brief One VkDeviceMemory. */
typedef struct vkf_mem {
    /** @brief The handle. */
    uint64_t handle;
    /** @brief allocationSize the guest asked for. */
    uint64_t size;
    /** @brief Property flags of its memory type. */
    uint32_t flags;
    /** @brief enum rl_vk_map_mode of this allocation (an import falls back to shadow per allocation). */
    int mode;
    /** @brief Guest address of the mapping range, 0 before the first map. */
    uint64_t va;
    /** @brief Bytes of guest address space at va. */
    uint64_t va_size;
    /** @brief Shadow: the host mapping. */
    uint8_t *host;
    /** @brief Shadow: the bytes both sides held at the last synchronization. */
    uint8_t *base;
    /** @brief True while the guest has it mapped. */
    bool mapped;
} vkf_mem;


typedef struct va_range {
    uint64_t addr;
    uint64_t size;
} va_range;

static va_range *g_used;
static uint32_t g_used_n, g_used_cap;

uint64_t vkf_va_alloc(uint64_t size, uint64_t align)
{
    uint64_t page = vkf.cfg.vm ? vkf.cfg.vm->host_page : 4096;
    if (align < page)
        align = page;
    size = (size + page - 1) & ~(page - 1);
    uint8_t *base = vkf.cfg.vm ? vkf.cfg.vm->base : NULL;
    uint64_t lo = vkf.cfg.map_base, hi = vkf.cfg.map_base + vkf.cfg.map_size;
    pthread_mutex_lock(&vkf.lock);
    uint64_t at = lo;
    uint64_t out = 0;
    for (uint32_t i = 0; i <= g_used_n; i++) {
        uint64_t host = (uint64_t)(uintptr_t)base + at;
        uint64_t aligned = ((host + align - 1) & ~(align - 1)) - (uint64_t)(uintptr_t)base;
        uint64_t end = i < g_used_n ? g_used[i].addr : hi;
        if (aligned + size <= end && aligned + size > aligned) {
            out = aligned;
            break;
        }
        if (i < g_used_n)
            at = g_used[i].addr + g_used[i].size;
    }
    if (out) {
        if (g_used_n == g_used_cap) {
            g_used_cap = g_used_cap ? g_used_cap * 2 : 64;
            g_used = realloc(g_used, g_used_cap * sizeof *g_used);
            if (!g_used)
                abort();
        }
        uint32_t i = 0;
        while (i < g_used_n && g_used[i].addr < out)
            i++;
        memmove(&g_used[i + 1], &g_used[i], (g_used_n - i) * sizeof *g_used);
        g_used[i] = (va_range){ out, size };
        g_used_n++;
    }
    pthread_mutex_unlock(&vkf.lock);
    if (!out)
        rl_log(RL_LOG_ERROR, "vk", "map window full: no 0x%llx bytes for device memory", (unsigned long long)size);
    return out;
}

void vkf_va_free(uint64_t addr)
{
    pthread_mutex_lock(&vkf.lock);
    for (uint32_t i = 0; i < g_used_n; i++)
        if (g_used[i].addr == addr) {
            memmove(&g_used[i], &g_used[i + 1], (g_used_n - i - 1) * sizeof *g_used);
            g_used_n--;
            break;
        }
    pthread_mutex_unlock(&vkf.lock);
}

static pthread_mutex_t g_vm_lock = PTHREAD_MUTEX_INITIALIZER;

int vkf_vm_set(uint64_t addr, uint64_t size, uint32_t perm, int content)
{
    uint64_t page = vkf.cfg.vm->guest_page;
    size = (size + page - 1) & ~(page - 1);
    if (vkf.cfg.vm_set)
        return vkf.cfg.vm_set(vkf.cfg.vm_user, addr, size, perm, content);
    pthread_mutex_lock(&g_vm_lock);
    int r = rl_vm_set(vkf.cfg.vm, addr, size, perm, perm ? vkf.cfg.map_tag : 0, content);
    pthread_mutex_unlock(&g_vm_lock);
    return r;
}


static bool has_ext(const VkExtensionProperties *p, uint32_t n, const char *name)
{
    for (uint32_t i = 0; i < n; i++)
        if (!strcmp(p[i].extensionName, name))
            return true;
    return false;
}

static const char *mode_name(int m)
{
    return m == RL_VK_MAP_PLACED ? "placed" : m == RL_VK_MAP_IMPORT ? "import" : "shadow";
}

void vkf_mem_device_setup(vkf_call *k, vkf_dev *d, void *phys, const char ***exts, uint32_t *n_exts, void **pnext)
{
    VkPhysicalDevice pd = phys;
    VkPhysicalDeviceMemoryProperties mp;
    VKF_FN(k->fns, vkGetPhysicalDeviceMemoryProperties)(pd, &mp);
    d->type_count = mp.memoryTypeCount;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        d->type_flags[i] = mp.memoryTypes[i].propertyFlags;
    uint32_t n = 0;
    VKF_FN(k->fns, vkEnumerateDeviceExtensionProperties)(pd, NULL, &n, NULL);
    VkExtensionProperties *p = vkf_alloc(k, (n + 1) * sizeof *p);
    VKF_FN(k->fns, vkEnumerateDeviceExtensionProperties)(pd, NULL, &n, p);
    const char **list = vkf_alloc(k, 4 * sizeof *list);
    uint32_t m = 0;
    bool placed_ok = false, import_ok = false;
    PFN_vkGetPhysicalDeviceFeatures2 gf2 = VKF_FN(k->fns, vkGetPhysicalDeviceFeatures2);
    PFN_vkGetPhysicalDeviceProperties2 gp2 = VKF_FN(k->fns, vkGetPhysicalDeviceProperties2);
    if (has_ext(p, n, VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME) && has_ext(p, n, VK_KHR_MAP_MEMORY_2_EXTENSION_NAME) && gf2 && gp2) {
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT f = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f };
        gf2(pd, &f2);
        VkPhysicalDeviceMapMemoryPlacedPropertiesEXT pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &pp };
        gp2(pd, &p2);
        placed_ok = f.memoryMapPlaced && f.memoryUnmapReserve;
        d->placed_align = pp.minPlacedMemoryMapAlignment;
    }
    if (has_ext(p, n, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME) && gp2) {
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp };
        gp2(pd, &p2);
        import_ok = true;
        d->import_align = hp.minImportedHostPointerAlignment;
    }
    int want = vkf.forced_mode;
    if (want == RL_VK_MAP_PLACED && !placed_ok)
        rl_log(RL_LOG_WARN, "vk", "placed mapping forced but the device lacks it");
    if (want == RL_VK_MAP_IMPORT && !import_ok)
        rl_log(RL_LOG_WARN, "vk", "host-pointer import forced but the device lacks it");
    if ((want == RL_VK_MAP_AUTO || want == RL_VK_MAP_PLACED) && placed_ok)
        d->map_mode = RL_VK_MAP_PLACED;
    else if ((want == RL_VK_MAP_AUTO || want == RL_VK_MAP_IMPORT || want == RL_VK_MAP_PLACED) && import_ok)
        d->map_mode = RL_VK_MAP_IMPORT;
    else
        d->map_mode = RL_VK_MAP_SHADOW;
    *pnext = NULL;
    if (d->map_mode == RL_VK_MAP_PLACED) {
        list[m++] = VK_KHR_MAP_MEMORY_2_EXTENSION_NAME;
        list[m++] = VK_EXT_MAP_MEMORY_PLACED_EXTENSION_NAME;
        VkPhysicalDeviceMapMemoryPlacedFeaturesEXT *f = vkf_alloc(k, sizeof *f);
        *f = (VkPhysicalDeviceMapMemoryPlacedFeaturesEXT){
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAP_MEMORY_PLACED_FEATURES_EXT,
            .memoryMapPlaced = VK_TRUE,
            .memoryUnmapReserve = VK_TRUE,
        };
        *pnext = f;
    } else if (d->map_mode == RL_VK_MAP_IMPORT) {
        list[m++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME;
    }
    *exts = list;
    *n_exts = m;
    rl_log(RL_LOG_INFO, "vk", "device memory maps by %s", mode_name(d->map_mode));
}

static void release_one(vkf_dev *d, vkf_mem *r, bool host_alive, void *dev);

typedef struct teardown {
    vkf_dev *d;
} teardown;

static void teardown_one(uint64_t key, void *value, void *user)
{
    (void)key;
    teardown *t = user;
    release_one(t->d, value, false, NULL);
}

void vkf_mem_device_teardown(vkf_dev *d)
{
    teardown t = { d };
    vkf_map_each(d->mems, teardown_one, &t);
}


static vkf_mem *mem_of(vkf_dev *d, uint64_t h)
{
    pthread_mutex_lock(&d->lock);
    vkf_mem *r = vkf_map_get(d->mems, h);
    pthread_mutex_unlock(&d->lock);
    return r;
}

static bool chain_has(const void *chain, VkStructureType s)
{
    for (const VkBaseInStructure *b = chain; b; b = b->pNext)
        if (b->sType == s)
            return true;
    return false;
}

static bool drop_optional_dedication(vkf_call *k, VkDevice dev, VkMemoryAllocateInfo *ai)
{
    for (VkBaseOutStructure *prev = (VkBaseOutStructure *)ai; prev->pNext; prev = prev->pNext) {
        VkBaseOutStructure *b = prev->pNext;
        if (b->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO)
            continue;
        const VkMemoryDedicatedAllocateInfo *ded = (const VkMemoryDedicatedAllocateInfo *)b;
        PFN_vkGetBufferMemoryRequirements2 req2 = VKF_FN(k->fns, vkGetBufferMemoryRequirements2);
        if (ded->image || !ded->buffer || !req2)
            return false;
        VkMemoryDedicatedRequirements dr = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS };
        VkMemoryRequirements2 mr = { .sType = VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2, .pNext = &dr };
        VkBufferMemoryRequirementsInfo2 ri = { .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_REQUIREMENTS_INFO_2, .buffer = ded->buffer };
        req2(dev, &ri, &mr);
        if (dr.requiresDedicatedAllocation)
            return false;
        prev->pNext = b->pNext;
        return true;
    }
    return true;
}

int vkh_vkAllocateMemory(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkAllocateMemory, c->x[0]);
    vkf_dev *d = k.obj->dev;
    VkMemoryAllocateInfo *ai = vkg_ain_VkMemoryAllocateInfo(&k, c->x[1], 1);
    uint64_t gout = c->x[3];
    if (!ai || !gout || ai->memoryTypeIndex >= vkf_switch.memory.memoryTypeCount) {
        c->x[0] = (uint32_t)VK_ERROR_INITIALIZATION_FAILED;
        return vkf_end(&k);
    }
    ai->memoryTypeIndex = d->host_type[ai->memoryTypeIndex];
    vkf_mem *r = calloc(1, sizeof *r);
    if (!r)
        abort();
    r->size = ai->allocationSize;
    r->flags = d->type_flags[ai->memoryTypeIndex];
    r->mode = d->map_mode;
    bool visible = r->flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
    if (visible && r->mode == RL_VK_MAP_IMPORT) {
        bool plain = !chain_has(ai->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) &&
                     drop_optional_dedication(&k, dev, ai);
        uint64_t align = d->import_align ? d->import_align : 4096;
        uint64_t size = (r->size + align - 1) & ~(align - 1);
        uint64_t va = plain ? vkf_va_alloc(size, align) : 0;
        PFN_vkGetMemoryHostPointerPropertiesEXT hpp = VKF_FN(k.fns, vkGetMemoryHostPointerPropertiesEXT);
        bool ok = false;
        if (va && hpp && !vkf_vm_set(va, size, RL_VM_RW, RL_VM_FRESH)) {
            VkMemoryHostPointerPropertiesEXT props = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
            ok = hpp(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, mem + va, &props) >= 0 &&
                 (props.memoryTypeBits & (1u << ai->memoryTypeIndex));
        }
        if (ok) {
            VkImportMemoryHostPointerInfoEXT *imp = vkf_alloc(&k, sizeof *imp);
            *imp = (VkImportMemoryHostPointerInfoEXT){
                .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
                .pNext = ai->pNext,
                .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
                .pHostPointer = mem + va,
            };
            ai->pNext = imp;
            ai->allocationSize = size;
            r->va = va;
            r->va_size = size;
        } else {
            if (va) {
                vkf_vm_set(va, size, RL_VM_NONE, RL_VM_DISCARD);
                vkf_va_free(va);
            }
            r->mode = RL_VK_MAP_SHADOW;
        }
    }
    VkDeviceMemory h = NULL;
    VkResult res = VKF_FN(k.fns, vkAllocateMemory)(dev, ai, NULL, &h);
    if (res < 0) {
        if (r->va) {
            vkf_vm_set(r->va, r->va_size, RL_VM_NONE, RL_VM_DISCARD);
            vkf_va_free(r->va);
        }
        free(r);
        c->x[0] = (uint32_t)res;
        return vkf_end(&k);
    }
    r->handle = (uint64_t)(uintptr_t)h;
    pthread_mutex_lock(&d->lock);
    vkf_map_put(d->mems, r->handle, r);
    pthread_mutex_unlock(&d->lock);
    vkf_s64(&k, gout, r->handle);
    c->x[0] = (uint32_t)res;
    return vkf_end(&k);
}

enum { SYNC_LINE = 64 };

static void merge(const uint8_t *from, uint8_t *to, uint8_t *base, uint64_t size)
{
    for (uint64_t at = 0; at < size; at += SYNC_LINE) {
        uint64_t n = size - at < SYNC_LINE ? size - at : SYNC_LINE;
        if (!memcmp(from + at, base + at, n))
            continue;
        for (uint64_t i = at; i < at + n; i++) {
            uint8_t v = from[i];
            if (v != base[i]) {
                to[i] = v;
                base[i] = v;
            }
        }
    }
}

static void sync_range(vkf_mem *r, uint64_t off, uint64_t size, bool to_host)
{
    if (r->mode != RL_VK_MAP_SHADOW || !r->mapped || !r->base || off >= r->size)
        return;
    if (size == VK_WHOLE_SIZE || off + size > r->size)
        size = r->size - off;
    uint8_t *g = vkf.cfg.vm->base + r->va + off;
    if (to_host)
        merge(g, r->host + off, r->base + off, size);
    else
        merge(r->host + off, g, r->base + off, size);
}

static void unmap_one(vkf_dev *d, vkf_mem *r, void *dev)
{
    if (!r->mapped)
        return;
    r->mapped = false;
    if (r->mode == RL_VK_MAP_PLACED && dev) {
        VkMemoryUnmapInfoKHR ui = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_UNMAP_INFO,
            .flags = VK_MEMORY_UNMAP_RESERVE_BIT_EXT,
            .memory = (VkDeviceMemory)(uintptr_t)r->handle,
        };
        VKF_FN(&d->fns, vkUnmapMemory2KHR)(dev, &ui);
        vkf_vm_set(r->va, r->va_size, RL_VM_NONE, RL_VM_KEEP);
    } else if (r->mode == RL_VK_MAP_SHADOW) {
        if (dev) {
            sync_range(r, 0, VK_WHOLE_SIZE, true);
            VKF_FN(&d->fns, vkUnmapMemory)(dev, (VkDeviceMemory)(uintptr_t)r->handle);
        }
        free(r->base);
        r->base = NULL;
        r->host = NULL;
        vkf_vm_set(r->va, r->va_size, RL_VM_NONE, RL_VM_DISCARD);
    }
}

static void release_one(vkf_dev *d, vkf_mem *r, bool host_alive, void *dev)
{
    if (host_alive)
        unmap_one(d, r, dev);
    else
        r->mapped = false;
    if (r->va) {
        vkf_vm_set(r->va, r->va_size, RL_VM_NONE, RL_VM_DISCARD);
        vkf_va_free(r->va);
    }
    free(r->base);
    free(r);
}

int vkh_vkFreeMemory(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkFreeMemory, c->x[0]);
    vkf_dev *d = k.obj->dev;
    uint64_t h = c->x[1];
    vkf_mem *r = mem_of(d, h);
    if (r)
        unmap_one(d, r, dev);
    VKF_FN(k.fns, vkFreeMemory)(dev, (VkDeviceMemory)(uintptr_t)h, NULL);
    if (r) {
        pthread_mutex_lock(&d->lock);
        vkf_map_put(d->mems, h, NULL);
        pthread_mutex_unlock(&d->lock);
        release_one(d, r, false, NULL);
    }
    return vkf_end(&k);
}


int vkh_vkMapMemory(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkMapMemory, c->x[0]);
    vkf_dev *d = k.obj->dev;
    vkf_mem *r = mem_of(d, c->x[1]);
    uint64_t offset = c->x[2];
    uint64_t gout = c->x[5];
    VkResult res = VK_SUCCESS;
    if (!r || !(r->flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) || r->mapped || offset >= r->size || !gout) {
        c->x[0] = (uint32_t)VK_ERROR_MEMORY_MAP_FAILED;
        return vkf_end(&k);
    }
    if (r->mode == RL_VK_MAP_PLACED) {
        uint64_t align = d->placed_align ? d->placed_align : 4096;
        if (!r->va) {
            r->va_size = (r->size + align - 1) & ~(align - 1);
            r->va = vkf_va_alloc(r->va_size, align);
        }
        if (!r->va)
            res = VK_ERROR_MEMORY_MAP_FAILED;
        else {
            VkMemoryMapPlacedInfoEXT pi = { .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_PLACED_INFO_EXT, .pPlacedAddress = mem + r->va };
            VkMemoryMapInfoKHR mi = {
                .sType = VK_STRUCTURE_TYPE_MEMORY_MAP_INFO,
                .pNext = &pi,
                .flags = VK_MEMORY_MAP_PLACED_BIT_EXT,
                .memory = (VkDeviceMemory)(uintptr_t)r->handle,
                .offset = 0,
                .size = VK_WHOLE_SIZE,
            };
            void *p = NULL;
            res = VKF_FN(k.fns, vkMapMemory2KHR)(dev, &mi, &p);
            if (res >= 0 && p != mem + r->va) {
                rl_log(RL_LOG_ERROR, "vk", "placed map landed at %p, not guest 0x%llx", p, (unsigned long long)r->va);
                res = VK_ERROR_MEMORY_MAP_FAILED;
            }
            if (res >= 0)
                vkf_vm_set(r->va, r->va_size, RL_VM_RW, RL_VM_KEEP);
        }
    } else if (r->mode == RL_VK_MAP_SHADOW) {
        void *p = NULL;
        res = VKF_FN(k.fns, vkMapMemory)(dev, (VkDeviceMemory)(uintptr_t)r->handle, 0, VK_WHOLE_SIZE, 0, &p);
        if (res >= 0) {
            if (!r->va) {
                r->va_size = r->size;
                r->va = vkf_va_alloc(r->size, 0);
            }
            uint8_t *base = r->va ? malloc(r->size) : NULL;
            if (!base || vkf_vm_set(r->va, r->va_size, RL_VM_RW, RL_VM_FRESH)) {
                free(base);
                VKF_FN(k.fns, vkUnmapMemory)(dev, (VkDeviceMemory)(uintptr_t)r->handle);
                res = VK_ERROR_MEMORY_MAP_FAILED;
            } else {
                r->host = p;
                r->base = base;
                memcpy(base, p, r->size);
                memcpy(mem + r->va, base, r->size);
            }
        }
    }
    if (res >= 0) {
        r->mapped = true;
        vkf_s64(&k, gout, r->va + offset);
    }
    c->x[0] = (uint32_t)res;
    return vkf_end(&k);
}

int vkh_vkUnmapMemory(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkUnmapMemory, c->x[0]);
    vkf_mem *r = mem_of(k.obj->dev, c->x[1]);
    if (r)
        unmap_one(k.obj->dev, r, dev);
    return vkf_end(&k);
}


typedef struct range_op {
    vkf_call *k;
    bool to_host;
} range_op;

static int ranges(rl_ctx *c, uint8_t *mem, int cmd, bool flush)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, cmd, c->x[0]);
    uint32_t n = (uint32_t)c->x[1];
    const VkMappedMemoryRange *rg = vkg_ain_VkMappedMemoryRange(&k, c->x[2], n);
    vkf_dev *d = k.obj->dev;
    if (flush)
        for (uint32_t i = 0; i < n; i++) {
            vkf_mem *r = mem_of(d, (uint64_t)(uintptr_t)rg[i].memory);
            if (r)
                sync_range(r, rg[i].offset, rg[i].size, true);
        }
    VkResult res = flush ? VKF_FN(k.fns, vkFlushMappedMemoryRanges)(dev, n, rg)
                         : VKF_FN(k.fns, vkInvalidateMappedMemoryRanges)(dev, n, rg);
    if (!flush && res >= 0)
        for (uint32_t i = 0; i < n; i++) {
            vkf_mem *r = mem_of(d, (uint64_t)(uintptr_t)rg[i].memory);
            if (r)
                sync_range(r, rg[i].offset, rg[i].size, false);
        }
    c->x[0] = (uint32_t)res;
    return vkf_end(&k);
}

int vkh_vkFlushMappedMemoryRanges(rl_ctx *restrict c, uint8_t *mem) { return ranges(c, mem, VKG_vkFlushMappedMemoryRanges, true); }

int vkh_vkInvalidateMappedMemoryRanges(rl_ctx *restrict c, uint8_t *mem)
{
    return ranges(c, mem, VKG_vkInvalidateMappedMemoryRanges, false);
}

static void sync_all(uint64_t key, void *value, void *user)
{
    (void)key;
    vkf_mem *r = value;
    const range_op *op = user;
    sync_range(r, 0, VK_WHOLE_SIZE, op->to_host);
}

static void shadow_sync(vkf_call *k, bool to_host)
{
    vkf_dev *d = k->obj ? k->obj->dev : NULL;
    if (!d || d->map_mode == RL_VK_MAP_PLACED)
        return;
    range_op op = { k, to_host };
    pthread_mutex_lock(&d->lock);
    vkf_map_each(d->mems, sync_all, &op);
    pthread_mutex_unlock(&d->lock);
}

void vkf_shadow_flush(vkf_call *k) { shadow_sync(k, true); }

void vkf_shadow_pull(vkf_call *k) { shadow_sync(k, false); }


int vkh_vkCreateBuffer(rl_ctx *restrict c, uint8_t *mem)
{
    vkf_call k;
    vkf_begin(&k, c, mem);
    VkDevice dev = vkf_first(&k, VKG_KIND_VkDevice, c->x[0]);
    if (!dev)
        return vkf_bad_handle(&k, VKG_vkCreateBuffer, c->x[0]);
    VkBufferCreateInfo *ci = vkg_ain_VkBufferCreateInfo(&k, c->x[1], 1);
    uint64_t gout = c->x[3];
    if (ci && k.obj->dev->map_mode == RL_VK_MAP_IMPORT) {
        VkExternalMemoryBufferCreateInfo *e = NULL;
        for (VkBaseOutStructure *b = (VkBaseOutStructure *)ci->pNext; b; b = b->pNext)
            if (b->sType == VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO)
                e = (VkExternalMemoryBufferCreateInfo *)b;
        if (e)
            e->handleTypes |= VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT;
        else {
            e = vkf_alloc(&k, sizeof *e);
            *e = (VkExternalMemoryBufferCreateInfo){
                .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
                .pNext = ci->pNext,
                .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT,
            };
            ci->pNext = e;
        }
    }
    VkBuffer b = NULL;
    VkResult r = VKF_FN(k.fns, vkCreateBuffer)(dev, ci, NULL, &b);
    if (r >= 0 && gout)
        vkf_s64(&k, gout, (uint64_t)(uintptr_t)b);
    c->x[0] = (uint32_t)r;
    return vkf_end(&k);
}
