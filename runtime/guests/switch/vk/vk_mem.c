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
 *
 * Where the fault handler is up, a shadow copy is tracked: its guest pages
 * are a shared-memory object the layer also maps for itself, and they have
 * no host access while closed. The first guest access to a closed chunk
 * faults; the hook moves the device's changes in and opens the chunk. Each
 * synchronization closes every open chunk and moves the guest's changes out.
 * Only the chunks the guest touched since the last synchronization are
 * compared, and the guest sees the device's writes exactly as the untracked
 * copy does: as of its first access after the synchronization.
 */
#include "vkf.h"

#include <relift/settings.h>
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
    /** @brief Tracked shadow: the guest pages, also mapped at shm.host (NULL host when untracked). */
    rl_shm shm;
    /** @brief Tracked shadow: one byte per chunk, nonzero while the guest may access the chunk. */
    uint8_t *open;
    /** @brief Tracked shadow: number of nonzero bytes in open. */
    uint64_t open_n;
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

uint64_t vkf_writable_below(uint64_t addr)
{
    if (vkf.cfg.writable_below)
        return vkf.cfg.writable_below(vkf.cfg.vm_user, addr);
    const rl_vm *vm = vkf.cfg.vm;
    pthread_mutex_lock(&g_vm_lock);
    uint64_t lo = addr;
    while (lo && lo <= vm->size) {
        rl_vm_block b = rl_vm_query(vm, lo - 1);
        if ((b.perm & RL_VM_RW) != RL_VM_RW)
            break;
        lo = b.addr;
    }
    pthread_mutex_unlock(&g_vm_lock);
    return lo;
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

/** @brief The host memory types the buffer or image of a dedicated allocation may use; all of them when it has none. */
static uint32_t dedicated_type_bits(vkf_call *k, VkDevice dev, const VkMemoryAllocateInfo *ai)
{
    for (const VkBaseInStructure *b = ai->pNext; b; b = b->pNext) {
        if (b->sType != VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO)
            continue;
        const VkMemoryDedicatedAllocateInfo *ded = (const VkMemoryDedicatedAllocateInfo *)b;
        VkMemoryRequirements mr;
        memset(&mr, 0, sizeof mr);
        if (ded->buffer)
            VKF_FN(k->fns, vkGetBufferMemoryRequirements)(dev, ded->buffer, &mr);
        else if (ded->image)
            VKF_FN(k->fns, vkGetImageMemoryRequirements)(dev, ded->image, &mr);
        else
            break;
        return mr.memoryTypeBits;
    }
    return UINT32_MAX;
}

/**
 * @brief A host type that takes imported host pointers in place of type t: one of
 * accept with every property of t and no AMD device-coherence it lacks; t itself
 * when t takes them, -1 when none does.
 */
static int import_type(const vkf_dev *d, uint32_t t, uint32_t accept)
{
    if (accept & (1u << t))
        return (int)t;
    const uint32_t amd = 0xc0u;
    for (uint32_t i = 0; i < 32; i++)
        if ((accept & (1u << i)) && (d->type_flags[i] & d->type_flags[t]) == d->type_flags[t] &&
            !(d->type_flags[i] & amd & ~d->type_flags[t]))
            return (int)i;
    return -1;
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
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkAllocateMemory, VK_ERROR_INITIALIZATION_FAILED);
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
    if (visible && r->mode == RL_VK_MAP_PLACED) {
        uint64_t align = d->placed_align ? d->placed_align : 4096;
        ai->allocationSize = (ai->allocationSize + align - 1) & ~(align - 1);
    }
    if (visible && r->mode == RL_VK_MAP_IMPORT) {
        uint32_t allowed = dedicated_type_bits(&k, dev, ai);
        bool plain = !chain_has(ai->pNext, VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO) &&
                     drop_optional_dedication(&k, dev, ai);
        uint64_t align = d->import_align ? d->import_align : 4096;
        uint64_t size = (r->size + align - 1) & ~(align - 1);
        uint64_t va = plain ? vkf_va_alloc(size, align) : 0;
        PFN_vkGetMemoryHostPointerPropertiesEXT hpp = VKF_FN(k.fns, vkGetMemoryHostPointerPropertiesEXT);
        bool ok = false;
        if (va && hpp && !vkf_vm_set(va, size, RL_VM_RW, RL_VM_FRESH)) {
            VkMemoryHostPointerPropertiesEXT props = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
            int t = hpp(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, mem + va, &props) >= 0
                        ? import_type(d, ai->memoryTypeIndex, props.memoryTypeBits & (allowed == UINT32_MAX ? 1u << ai->memoryTypeIndex : allowed))
                        : -1;
            ok = t >= 0;
            if (ok && (uint32_t)t != ai->memoryTypeIndex) {
                ai->memoryTypeIndex = (uint32_t)t;
                r->flags = d->type_flags[t];
            }
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
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkAllocateMemory, res);
        return vkf_end(&k);
    }
    r->handle = (uint64_t)(uintptr_t)h;
    pthread_mutex_lock(&d->lock);
    vkf_map_put(d->mems, r->handle, r);
    pthread_mutex_unlock(&d->lock);
    vkf_s64(&k, gout, r->handle);
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkAllocateMemory, res);
    return vkf_end(&k);
}

enum { SYNC_LINE = 64, SYNC_BLOCK = 4096, TRACK_CHUNK = 64 * 1024 };

static void merge_lines(const uint8_t *from, uint8_t *to, uint8_t *base, uint64_t size)
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

static void merge(const uint8_t *from, uint8_t *to, uint8_t *base, uint64_t size)
{
    for (uint64_t at = 0; at < size; at += SYNC_BLOCK) {
        uint64_t n = size - at < SYNC_BLOCK ? size - at : SYNC_BLOCK;
        if (memcmp(from + at, base + at, n))
            merge_lines(from + at, to + at, base + at, n);
    }
}

/** @brief Tracked shadow mappings sorted by va, and their open state; the fault hook takes only this lock. */
static pthread_mutex_t g_track_lock = PTHREAD_MUTEX_INITIALIZER;
static vkf_mem **g_tracked;
static uint32_t g_tracked_n, g_tracked_cap;
static uint64_t g_chunk;
static uint64_t g_opened, g_closed;

static vkf_mem *tracked_at(uint64_t addr)
{
    uint32_t lo = 0, hi = g_tracked_n;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        vkf_mem *r = g_tracked[mid];
        if (addr < r->va)
            hi = mid;
        else if (addr >= r->va + r->va_size)
            lo = mid + 1;
        else
            return r;
    }
    return NULL;
}

/** @brief Moves the device's changes into chunk c and gives the guest access to it. Under g_track_lock. */
static void open_chunk(vkf_mem *r, uint64_t c)
{
    uint64_t at = c * g_chunk, end = at + g_chunk < r->va_size ? at + g_chunk : r->va_size;
    if (at < r->size)
        merge(r->host + at, r->shm.host + at, r->base + at, (end < r->size ? end : r->size) - at);
    if (rl_vm_host_access(vkf.cfg.vm, r->va + at, end - at, true) == 0) {
        r->open[c] = 1;
        r->open_n++;
        g_opened++;
    }
}

static bool track_fault(const rl_fault *f)
{
    pthread_mutex_lock(&g_track_lock);
    vkf_mem *r = tracked_at(f->guest_addr);
    bool ok = false;
    if (r) {
        uint64_t c = (f->guest_addr - r->va) / g_chunk;
        if (!r->open[c])
            open_chunk(r, c);
        ok = r->open[c];
    }
    pthread_mutex_unlock(&g_track_lock);
    return ok;
}

/** @brief Takes the guest's access to the open chunks overlapping [off, off+size) and moves its changes out. */
static void close_chunks(vkf_mem *r, uint64_t off, uint64_t size)
{
    pthread_mutex_lock(&g_track_lock);
    uint64_t last = (off + size - 1) / g_chunk;
    for (uint64_t c = off / g_chunk; r->open_n && c <= last; c++) {
        if (!r->open[c])
            continue;
        uint64_t e = c;
        while (e + 1 <= last && r->open[e + 1])
            e++;
        uint64_t at = c * g_chunk, end = (e + 1) * g_chunk < r->va_size ? (e + 1) * g_chunk : r->va_size;
        if (rl_vm_host_access(vkf.cfg.vm, r->va + at, end - at, false) == 0) {
            memset(r->open + c, 0, e - c + 1);
            r->open_n -= e - c + 1;
            g_closed += e - c + 1;
        } else
            rl_log(RL_LOG_ERROR, "vk", "could not close guest 0x%llx of a shadow mapping", (unsigned long long)(r->va + at));
        if (at < r->size)
            merge(r->shm.host + at, r->host + at, r->base + at, (end < r->size ? end : r->size) - at);
        c = e;
    }
    pthread_mutex_unlock(&g_track_lock);
}

static void track_add(vkf_mem *r)
{
    pthread_mutex_lock(&g_track_lock);
    if (g_tracked_n == g_tracked_cap) {
        g_tracked_cap = g_tracked_cap ? g_tracked_cap * 2 : 64;
        g_tracked = realloc(g_tracked, g_tracked_cap * sizeof *g_tracked);
        if (!g_tracked)
            abort();
    }
    uint32_t i = 0;
    while (i < g_tracked_n && g_tracked[i]->va < r->va)
        i++;
    memmove(&g_tracked[i + 1], &g_tracked[i], (g_tracked_n - i) * sizeof *g_tracked);
    g_tracked[i] = r;
    g_tracked_n++;
    pthread_mutex_unlock(&g_track_lock);
}

/** @brief Drops r from tracking and closes its pages; the guest range keeps them until it is discarded. */
static void track_drop(vkf_mem *r)
{
    if (!r->open)
        return;
    pthread_mutex_lock(&g_track_lock);
    for (uint32_t i = 0; i < g_tracked_n; i++)
        if (g_tracked[i] == r) {
            memmove(&g_tracked[i], &g_tracked[i + 1], (g_tracked_n - i - 1) * sizeof *g_tracked);
            g_tracked_n--;
            break;
        }
    pthread_mutex_unlock(&g_track_lock);
    rl_shm_destroy(&r->shm);
    free(r->open);
    r->open = NULL;
    r->open_n = 0;
}

/**
 * @brief Backs a shadow mapping's guest range with tracked pages holding
 * base. Returns false, with nothing changed, where tracking is unavailable.
 */
static bool track_map(vkf_mem *r)
{
    rl_vm *vm = vkf.cfg.vm;
    if (!rl_fault_installed())
        return false;
    if (!g_chunk) {
        g_chunk = TRACK_CHUNK > vm->host_page ? TRACK_CHUNK : vm->host_page;
        rl_fault_set_hook(track_fault);
    }
    uint64_t chunks = (r->va_size + g_chunk - 1) / g_chunk;
    uint8_t *open = calloc(chunks, 1);
    if (!open || rl_shm_create(&r->shm, r->va_size)) {
        free(open);
        return false;
    }
    memcpy(r->shm.host, r->base, r->size);
    if (rl_vm_map_shm(vm, r->va, r->va_size, &r->shm) || vkf_vm_set(r->va, r->va_size, RL_VM_RW, RL_VM_KEEP) ||
        rl_vm_host_access(vm, r->va, r->va_size, false)) {
        vkf_vm_set(r->va, r->va_size, RL_VM_NONE, RL_VM_DISCARD);
        rl_shm_destroy(&r->shm);
        free(open);
        return false;
    }
    r->open = open;
    track_add(r);
    return true;
}

static void sync_range(vkf_mem *r, uint64_t off, uint64_t size, bool to_host)
{
    if (r->mode != RL_VK_MAP_SHADOW || !r->mapped || !r->base || off >= r->size)
        return;
    if (size == VK_WHOLE_SIZE || off + size > r->size)
        size = r->size - off;
    if (r->open) {
        close_chunks(r, off, size);
        return;
    }
    uint8_t *g = vkf.cfg.vm->base + r->va + off;
    if (to_host)
        merge(g, r->host + off, r->base + off, size);
    else
        merge(r->host + off, g, r->base + off, size);
}

uint64_t rl_vk_shadow_chunks_opened(void)
{
    pthread_mutex_lock(&g_track_lock);
    uint64_t n = g_opened;
    pthread_mutex_unlock(&g_track_lock);
    return n;
}

void vkf_shadow_stats_frame(uint64_t frame)
{
    static uint64_t every, last_opened, last_closed;
    static bool read;
    if (!read) {
        int64_t n = rl_setting_int("stats.every");
        every = n > 0 ? (uint64_t)n : 0;
        read = true;
    }
    if (!every || frame % every || !g_chunk)
        return;
    pthread_mutex_lock(&g_track_lock);
    uint64_t opened = g_opened - last_opened, closed = g_closed - last_closed;
    last_opened = g_opened;
    last_closed = g_closed;
    pthread_mutex_unlock(&g_track_lock);
    rl_log(RL_LOG_INFO, "stats", "frame %llu: shadow mappings opened %.1f and closed %.1f chunks of %llu KiB per frame",
           (unsigned long long)frame, (double)opened / (double)every, (double)closed / (double)every,
           (unsigned long long)(g_chunk >> 10));
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
        track_drop(r);
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
    track_drop(r);
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
        c->x[0] = (uint32_t)vkf_result(&k, VKG_vkMapMemory, VK_ERROR_MEMORY_MAP_FAILED);
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
                uint64_t page = vkf.cfg.vm->host_page;
                r->va_size = (r->size + page - 1) & ~(page - 1);
                r->va = vkf_va_alloc(r->va_size, 0);
            }
            uint8_t *base = r->va ? malloc(r->size) : NULL;
            if (base)
                memcpy(base, p, r->size);
            r->base = base;
            r->host = p;
            bool tracked = base && track_map(r);
            if (!tracked && (!base || vkf_vm_set(r->va, r->va_size, RL_VM_RW, RL_VM_FRESH))) {
                free(base);
                r->base = NULL;
                r->host = NULL;
                VKF_FN(k.fns, vkUnmapMemory)(dev, (VkDeviceMemory)(uintptr_t)r->handle);
                res = VK_ERROR_MEMORY_MAP_FAILED;
            } else if (!tracked)
                memcpy(mem + r->va, base, r->size);
        }
    }
    if (res >= 0) {
        r->mapped = true;
        vkf_s64(&k, gout, r->va + offset);
    }
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkMapMemory, res);
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
    c->x[0] = (uint32_t)vkf_result(&k, cmd, res);
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

const uint8_t *vkf_mem_bytes(vkf_call *k, VkDevice dev, uint64_t memory, uint64_t off, uint64_t size, bool *unmap, bool *cached)
{
    *unmap = false;
    vkf_mem *r = mem_of(k->obj->dev, memory);
    if (!r || off > r->size || size > r->size - off)
        return NULL;
    *cached = (r->flags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) || r->mode == RL_VK_MAP_SHADOW;
    if (r->va && (r->mapped || r->mode == RL_VK_MAP_IMPORT))
        return vkf.cfg.vm->base + r->va + off;
    if (!(r->flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
        return NULL;
    void *p = NULL;
    if (VKF_FN(k->fns, vkMapMemory)(dev, (VkDeviceMemory)(uintptr_t)memory, off, size, 0, &p) < 0)
        return NULL;
    *unmap = true;
    return p;
}

void vkf_mem_bytes_done(vkf_call *k, VkDevice dev, uint64_t memory, bool unmap)
{
    if (unmap)
        VKF_FN(k->fns, vkUnmapMemory)(dev, (VkDeviceMemory)(uintptr_t)memory);
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
    c->x[0] = (uint32_t)vkf_result(&k, VKG_vkCreateBuffer, r);
    return vkf_end(&k);
}
