/**
 * @file nvn_dev.c
 * @brief NVN2 devices, queues, memory pools, buffers and syncs.
 *
 * A memory pool's storage is guest memory, imported into the host Vulkan
 * device as a buffer (VK_EXT_external_memory_host); the pool's GPU address
 * is that buffer's device address, so the translated programs dereference
 * the addresses the game computes from it, and the layer turns a GPU
 * address back into the guest address of the same byte (nvn_gpu_guest).
 * A host without the import (or nvn.memory set to mirror) gets a device
 * buffer kept equal to the guest bytes instead (nvn_mirror.c), with the same
 * addresses. Without either a pool's GPU address is its guest address.
 */
#include "nvn_priv.h"
#include "nvn_watch.h"
#include "vkdev.h"
#include "services.h"
#include <relift/settings.h>
#include "vi.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>

/**
 * @brief nvnDeviceGetInteger answers by NVNdeviceInfo without a driver
 * layout: one SDK driver's constants, and the Switch's values where the
 * driver reads what its device initialization measured (scratch memory
 * factors, L2 size, texture limits). With a layout only those measured
 * values are taken from here.
 */
static const int32_t device_info[] = {
    231, 30, 14, 65536, 256, 8, 32, 4, 16, 32, 16, 4, 4, 4, 32, 32, 32, 256, 256, 4, 8, 512, 256, 2, 256, 4096,
    16384, 16, 1024, 1024, 64, 1024, 2147483647, 65535, 65535, 8, 1048576, 4096, 16, 4096, 1, 1, 1, 262144, 16,
    2048, 201, 201, 17, 0, 1, 8, 8, 4, 32, 128, 32, 128, 65536, 1, 4, 16384, 134217728, 2048, 16384, 16384, 1, 1, 1,
    1, 5, 1, 1, 1, 1, 1, 1, 16, 2048, 32, 4096, 65536, 65536, 256, 16384, 262144, 4096, 1, 4, 2, 0, 4096, 16384,
    4096, 128, 32, 65535, 1, 0, 256, 16, 65535, 65535, 256, 1, 16, 16, 16, 1, 1, 1, 256, 0, 16, 8, 16, 32, 31,
    536870912, 16777216, 16777216, 1073741824, 32, 1, 64, 0, 1, 2147483647, 32, 32, 4194303, 512, 256, 32, 16383,
    0, 8, 0, 0, 4, 256, 16, 0, 1, 31, 0, 64, 1, 58368, 24576, 0, 256, 262144, 0, 0, 0, 201, 14, 201, 17, 32768, 8,
    4, 256, 256, 4, 65536, 201, 5, 1,
};

typedef struct nvn_device {
    nvn_obj o;
    uint32_t origin_mode, depth_mode;
    /** @brief The builder's flags (nvnDeviceBuilderGetFlags). */
    uint32_t flags;
    /** @brief Whether the driver's fields are written into the guest's device object (see nvn_device_fields). */
    bool fields;
} nvn_device;

typedef struct nvn_queue {
    nvn_obj o;
    uint64_t device;
    uint64_t submits;
} nvn_queue;

/** @brief NVN_MEMORY_POOL_FLAGS_VIRTUAL: a pool of GPU addresses that nvnMemoryPoolMapVirtual backs. */
#define NVN_POOL_VIRTUAL 0x200u

typedef struct nvn_pool {
    nvn_obj o;
    uint64_t memory, size;
    uint32_t flags;
    nvn_import im;
    /**
     * @brief For a virtual pool, whether a physical pool (phys) is mapped
     * into it; memory then holds the guest address its first byte reaches,
     * and im its own buffer over that pool's memory.
     */
    bool mapped;
    uint64_t phys;
    /** @brief The pool's storage is a mirror (nvn_mirror.c), not an import. */
    bool mirrored;
    /** @brief A virtual pool whose addresses lie inside its physical pool's mirror: im holds the address and the shared buffer, and owns nothing. */
    bool alias;
} nvn_pool;

/**
 * @brief Pools by GPU address, for nvn_gpu_guest and nvn_gpu_mapped: imported and mirrored pools at their device
 * address, and a pool the host device holds no buffer for (buf NULL) at its guest address, its GPU address. Spans may
 * overlap: a host whose device addresses are the host pointers of imported
 * memory (llvmpipe) gives a virtual pool's import the addresses of the
 * physical pool's bytes it reaches, so the same address names the same byte
 * in both. max is the largest span size, which bounds the search back.
 */
static struct {
    pthread_rwlock_t lock;
    struct pool_span {
        uint64_t gpu, size, memory;
        void *buf;
        /** @brief The guest address of the buffer's first byte, which memory is at or past. */
        uint64_t base;
    } *v;
    uint32_t n, cap;
    uint64_t max;
} g_pools = { PTHREAD_RWLOCK_INITIALIZER, NULL, 0, 0, 0 };

static void pools_add(uint64_t gpu, uint64_t size, uint64_t memory, void *buf, uint64_t base)
{
    pthread_rwlock_wrlock(&g_pools.lock);
    if (g_pools.n == g_pools.cap) {
        uint32_t cap = g_pools.cap ? 2 * g_pools.cap : 64;
        void *v = realloc(g_pools.v, cap * sizeof *g_pools.v);
        if (!v) {
            pthread_rwlock_unlock(&g_pools.lock);
            return;
        }
        g_pools.v = v;
        g_pools.cap = cap;
    }
    uint32_t i = g_pools.n++;
    while (i && g_pools.v[i - 1].gpu > gpu) {
        g_pools.v[i] = g_pools.v[i - 1];
        i--;
    }
    g_pools.v[i].gpu = gpu;
    g_pools.v[i].size = size;
    g_pools.v[i].memory = memory;
    g_pools.v[i].buf = buf;
    g_pools.v[i].base = base;
    if (size > g_pools.max)
        g_pools.max = size;
    pthread_rwlock_unlock(&g_pools.lock);
}

/**
 * @brief The imports of pools the guest finalized, kept alive. On Switch 2 a pool's GPU address is its CPU address, so a
 * pool made again over the same bytes has the address the old one had, and the guest still holds addresses of the old
 * pool (in buffers, constant banks and recorded commands). The guest's pages stay mapped after a finalize, so the host
 * import stays valid: a pool made again over exactly the same range takes it back, with its device address. A kept
 * import is freed when a new pool overlaps it with another range, or when the guest discards any of its pages.
 */
static struct kept_import {
    uint64_t memory, size;
    nvn_import im;
} *g_kept;
static uint32_t g_kept_n, g_kept_cap;
static pthread_mutex_t g_kept_lock = PTHREAD_MUTEX_INITIALIZER;

static void pools_remove(uint64_t gpu, uint64_t size, void *buf);

/** @brief Frees the kept imports overlapping guest [memory, memory + size), but for one of exactly that range when keep_exact is set. */
static void kept_release(uint64_t memory, uint64_t size, bool keep_exact)
{
    nvn_import gone[16];
    uint64_t gone_size[16];
    uint32_t n = 0;
    do {
        n = 0;
        pthread_mutex_lock(&g_kept_lock);
        for (uint32_t i = 0; i < g_kept_n && n < 16;) {
            struct kept_import *k = &g_kept[i];
            bool overlap = k->memory < memory + size && memory < k->memory + k->size;
            if (overlap && !(keep_exact && k->memory == memory && k->size == size)) {
                gone_size[n] = k->size;
                gone[n++] = k->im;
                *k = g_kept[--g_kept_n];
            } else {
                i++;
            }
        }
        pthread_mutex_unlock(&g_kept_lock);
        for (uint32_t i = 0; i < n; i++) {
            pools_remove(gone[i].gpu, gone_size[i], gone[i].buf);
            nvn_vk_unimport(&gone[i]);
        }
    } while (n == 16);
}

/** @brief The guest discards pages: kept imports over them would hold the old pages, so they go. */
static void kept_discard(uint64_t addr, uint64_t size) { kept_release(addr, size, false); }

static void kept_put(uint64_t memory, uint64_t size, const nvn_import *im)
{
    /* A finalize returns after the device is done with the pool, as it did when the import was freed: the game may
     * write the pages at once. */
    rl_vkdev_wait_idle(rl_vkdev());
    pthread_mutex_lock(&g_kept_lock);
    if (g_kept_n == g_kept_cap) {
        uint32_t cap = g_kept_cap ? 2 * g_kept_cap : 64;
        struct kept_import *v = realloc(g_kept, cap * sizeof *v);
        if (!v) {
            pthread_mutex_unlock(&g_kept_lock);
            pools_remove(im->gpu, size, im->buf);
            nvn_import gone = *im;
            nvn_vk_unimport(&gone);
            return;
        }
        g_kept = v;
        g_kept_cap = cap;
    }
    g_kept[g_kept_n++] = (struct kept_import){ memory, size, *im };
    __atomic_store_n(&hos.vm.discard_hook, kept_discard, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_kept_lock);
}

/** @brief Takes the kept import of exactly guest [memory, memory + size) into out; frees kept ones that overlap otherwise. */
static bool kept_take(uint64_t memory, uint64_t size, nvn_import *out)
{
    kept_release(memory, size, true);
    bool found = false;
    pthread_mutex_lock(&g_kept_lock);
    for (uint32_t i = 0; i < g_kept_n && !found; i++)
        if (g_kept[i].memory == memory && g_kept[i].size == size) {
            *out = g_kept[i].im;
            g_kept[i] = g_kept[--g_kept_n];
            found = true;
        }
    pthread_mutex_unlock(&g_kept_lock);
    if (found) {
        pools_remove(out->gpu, size, out->buf);
        nvn_watch_release(memory, size);
    }
    return found;
}

uint32_t nvn_pools_kept(void)
{
    pthread_mutex_lock(&g_kept_lock);
    uint32_t n = g_kept_n;
    pthread_mutex_unlock(&g_kept_lock);
    return n;
}

/** @brief The spans of the imports the layer freed, newest last: what a device fault at an address no pool holds names. */
#define FREED_MAX 256
static struct pool_span g_freed[FREED_MAX];
static uint32_t g_freed_n;

/** @brief Drops the span of size bytes that starts at gpu over the import whose buffer is buf (spans of two imports may start at one address). */
static void pools_remove(uint64_t gpu, uint64_t size, void *buf)
{
    pthread_rwlock_wrlock(&g_pools.lock);
    for (uint32_t i = 0; i < g_pools.n; i++)
        if (g_pools.v[i].buf == buf && g_pools.v[i].gpu == gpu && g_pools.v[i].size == size) {
            g_freed[g_freed_n++ % FREED_MAX] = g_pools.v[i];
            memmove(&g_pools.v[i], &g_pools.v[i + 1], (g_pools.n - i - 1) * sizeof *g_pools.v);
            g_pools.n--;
            break;
        }
    pthread_rwlock_unlock(&g_pools.lock);
}

uint64_t nvn_gpu_guest(uint64_t gpu)
{
    uint64_t out = gpu;
    pthread_rwlock_rdlock(&g_pools.lock);
    uint32_t lo = 0, hi = g_pools.n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_pools.v[mid].gpu <= gpu)
            lo = mid + 1;
        else
            hi = mid;
    }
    for (uint32_t i = lo; i-- > 0 && gpu - g_pools.v[i].gpu < g_pools.max;)
        if (gpu - g_pools.v[i].gpu < g_pools.v[i].size) {
            out = g_pools.v[i].memory + (gpu - g_pools.v[i].gpu);
            break;
        }
    pthread_rwlock_unlock(&g_pools.lock);
    return out;
}

size_t nvn_gpu_describe(uint64_t gpu, char *out, size_t n)
{
    size_t w = 0;
    pthread_rwlock_rdlock(&g_pools.lock);
    const struct pool_span *below = NULL, *above = NULL;
    for (uint32_t i = 0; i < g_pools.n; i++) {
        const struct pool_span *p = &g_pools.v[i];
        if (gpu >= p->gpu && gpu - p->gpu < p->size)
            w += (size_t)snprintf(out + w, w < n ? n - w : 0,
                                  "in pool span 0x%llx+0x%llx (guest 0x%llx, offset 0x%llx); ",
                                  (unsigned long long)p->gpu, (unsigned long long)p->size,
                                  (unsigned long long)p->memory, (unsigned long long)(gpu - p->gpu));
        else if (p->gpu + p->size <= gpu && (!below || p->gpu + p->size > below->gpu + below->size))
            below = p;
        else if (p->gpu > gpu && (!above || p->gpu < above->gpu))
            above = p;
    }
    uint32_t have = g_freed_n < FREED_MAX ? g_freed_n : FREED_MAX;
    for (uint32_t k = 0; k < have; k++) {
        const struct pool_span *f = &g_freed[(g_freed_n - 1 - k) % FREED_MAX];
        if (gpu >= f->gpu && gpu - f->gpu < f->size)
            w += (size_t)snprintf(out + w, w < n ? n - w : 0,
                                  "in FREED pool span 0x%llx+0x%llx (guest 0x%llx, offset 0x%llx, %u frees ago); ",
                                  (unsigned long long)f->gpu, (unsigned long long)f->size,
                                  (unsigned long long)f->memory, (unsigned long long)(gpu - f->gpu), k);
    }
    if (!w) {
        if (below)
            w += (size_t)snprintf(out + w, w < n ? n - w : 0, "0x%llx past the span 0x%llx+0x%llx below; ",
                                  (unsigned long long)(gpu - below->gpu - below->size),
                                  (unsigned long long)below->gpu, (unsigned long long)below->size);
        if (above)
            w += (size_t)snprintf(out + w, w < n ? n - w : 0, "0x%llx before the span 0x%llx+0x%llx above; ",
                                  (unsigned long long)(above->gpu - gpu), (unsigned long long)above->gpu,
                                  (unsigned long long)above->size);
        if (!w)
            w += (size_t)snprintf(out, n, "no pool span; ");
    }
    pthread_rwlock_unlock(&g_pools.lock);
    return w;
}

bool nvn_gpu_mapped(uint64_t gpu, uint64_t size)
{
    bool ok = false;
    pthread_rwlock_rdlock(&g_pools.lock);
    for (uint32_t i = 0; i < g_pools.n && !ok; i++)
        ok = gpu >= g_pools.v[i].gpu && gpu - g_pools.v[i].gpu < g_pools.v[i].size &&
             size <= g_pools.v[i].size - (gpu - g_pools.v[i].gpu);
    pthread_rwlock_unlock(&g_pools.lock);
    return ok;
}

bool nvn_guest_buffer(uint64_t guest, uint64_t size, void **buf, uint64_t *off)
{
    bool found = false;
    pthread_rwlock_rdlock(&g_pools.lock);
    for (uint32_t i = 0; i < g_pools.n && !found; i++) {
        const struct pool_span *p = &g_pools.v[i];
        if (p->buf && guest >= p->memory && guest - p->memory + size <= p->size) {
            *buf = p->buf;
            *off = guest - p->base;
            found = true;
        }
    }
    pthread_rwlock_unlock(&g_pools.lock);
    return found;
}

bool nvn_guest_buffer_rest(uint64_t guest, void **buf, uint64_t *off, uint64_t *before, uint64_t *rest)
{
    bool found = false;
    pthread_rwlock_rdlock(&g_pools.lock);
    for (uint32_t i = 0; i < g_pools.n && !found; i++) {
        const struct pool_span *p = &g_pools.v[i];
        if (p->buf && guest >= p->memory && guest - p->memory < p->size) {
            *buf = p->buf;
            *off = guest - p->base;
            *before = guest - p->memory;
            *rest = p->size - (guest - p->memory);
            found = true;
        }
    }
    pthread_rwlock_unlock(&g_pools.lock);
    return found;
}

/** @brief The GPU address of a pool's first byte. */
static uint64_t pool_gpu(const nvn_pool *p)
{
    if (p->flags & NVN_POOL_VIRTUAL)
        return p->im.gpu;
    return p->im.gpu ? p->im.gpu : p->memory;
}

typedef struct nvn_buffer {
    nvn_obj o;
    uint64_t pool, offset, size;
    uint64_t memory, gpu;
} nvn_buffer;

typedef struct nvn_sync {
    nvn_obj o;
    /** @brief The sync was fenced; it is signaled once the host device finished batch seq (nvn_vk_submitted). */
    bool signaled;
    uint64_t seq;
} nvn_sync;

static void trace(const char *fmt, ...) RL_PRINTF(1, 2);
static void trace(const char *fmt, ...)
{
    if (!rl_cfg()->trace_os)
        return;
    va_list ap;
    va_start(ap, fmt);
    rl_vlog(RL_LOG_INFO, "nvn", fmt, ap);
    va_end(ap);
}

/** @brief nvnDeviceInitialize(NVNdevice *, const NVNdeviceBuilder *): NVNboolean. */
uint64_t nvnh_nvnDeviceInitialize(rl_ctx *c, uint8_t *mem)
{
    uint64_t dev = c->x[0], builder = c->x[1], flags = 0;
    int rc = nvn_driver_call(c, mem, "nvnDeviceBuilderGetFlags", &builder, 1, &flags);
    if (rc != RL_OK)
        return (uint64_t)rc;
    if ((rc = nvn_gpu_open(c, mem)) != RL_OK)
        return (uint64_t)rc;
    nvn_device *d = (nvn_device *)nvn_obj_new(dev, NVN_DEVICE, sizeof *d);
    if (d)
        d->flags = (uint32_t)flags;
    d = (nvn_device *)nvn_obj_publish((nvn_obj *)d);
    trace("nvnDeviceInitialize(0x%llx, flags 0x%x)", (unsigned long long)dev, (unsigned)flags);
    c->x[0] = d != NULL;
    return nvn_ret(c);
}

static const struct rl_nvn_layout *g_layout;

void rl_nvn_set_layout(const struct rl_nvn_layout *l) { g_layout = l; }

/**
 * @brief Stops when any setting the driver's device start reads is set: the
 * driver reads them through the hook its settings word holds, so this asks
 * the same hook for each one (once per process).
 */
static int check_settings(rl_ctx *c, uint8_t *mem)
{
    static bool done;
    if (done || !g_layout->settings_hook)
        return RL_OK;
    uint64_t hook = rl_ld64(mem, g_layout->settings_hook);
    if (!hook)
        return rl_guest_fatal(c, "nvn", "the NVN2 driver's settings hook (word 0x%llx) is not installed",
                              (unsigned long long)g_layout->settings_hook);
    for (uint32_t i = 0; i < g_layout->nsettings; i++) {
        const char *key = g_layout->settings[i];
        size_t len = strlen(key) + 1;
        uint64_t sp = c->sp, found = 0;
        c->sp = (sp - len) & ~15ull;
        memcpy(mem + c->sp, key, len);
        uint64_t arg = c->sp;
        int rc = nvn_guest_call(c, mem, hook, &arg, 1, &found);
        c->sp = sp;
        if (rc != RL_OK)
            return rc;
        if (found)
            return rl_guest_fatal(c, "nvn", "the NVN2 driver setting \"%s\" is set; the device fields read from the "
                                  "driver assume it is not", key);
    }
    done = true;
    return RL_OK;
}

int nvn_device_fields(rl_ctx *c, uint8_t *mem, uint64_t dev)
{
    if (!g_layout)
        return rl_guest_fatal(c, "nvn", "the build read no NVN2 driver layout");
    nvn_device *d = (nvn_device *)nvn_obj_get(dev, NVN_DEVICE);
    if (!d)
        return rl_guest_fatal(c, "nvn", "device 0x%llx is not initialized", (unsigned long long)dev);
    if (d->fields)
        return RL_OK;
    int rc = check_settings(c, mem);
    if (rc != RL_OK)
        return rc;
    uint64_t gpu = nvn_gpu_device();
    if (!gpu)
        return rl_guest_fatal(c, "nvn", "the GPU is not open");
    if (g_layout->flags != RL_NVN_NONE)
        rl_st32(mem, dev + g_layout->flags, d->flags);
    if (g_layout->gpu_device != RL_NVN_NONE)
        rl_st64(mem, dev + g_layout->gpu_device, gpu);
    for (uint32_t i = 0; i < g_layout->nfields; i++) {
        const struct rl_nvn_field *f = &g_layout->fields[i];
        if (f->info < 0) {
            memcpy(mem + dev + f->off, f->bytes, f->len);
            continue;
        }
        uint32_t v = 0;
        if ((rc = nvn_gpu_info_u32(c, mem, f->info, &v)) != RL_OK)
            return rc;
        rl_st32(mem, dev + f->off, v);
    }
    d->fields = true;
    return RL_OK;
}

int nvn_builder_device_fields(rl_ctx *c, uint8_t *mem, uint64_t builder)
{
    if (!g_layout)
        return rl_guest_fatal(c, "nvn", "the build read no NVN2 driver layout");
    return nvn_device_fields(c, mem, rl_ld64(mem, builder + g_layout->builder_device));
}

int nvn_builder_storage_class(rl_ctx *c, uint8_t *mem, uint64_t builder, uint64_t *cls)
{
    *cls = 0;
    if (!g_layout)
        return RL_OK;
    int rc = nvn_builder_device_fields(c, mem, builder);
    if (rc != RL_OK)
        return rc;
    return nvn_guest_call(c, mem, g_layout->storage_class, &builder, 1, cls);
}

/** @brief nvnTextureBuilderGetStorageClass(const NVNtextureBuilder *): the driver's own function, on the fields it reads. */
uint64_t nvnh_nvnTextureBuilderGetStorageClass(rl_ctx *c, uint8_t *mem)
{
    if (!g_layout)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnTextureBuilderGetStorageClass: the build read no %s driver layout%s%s",
                                        nvn_version_name(), nvn_layout_error() ? ": " : "",
                                        nvn_layout_error() ? nvn_layout_error() : "");
    uint64_t builder = c->x[0], cls = 0;
    int rc = nvn_builder_device_fields(c, mem, builder);
    if (rc != RL_OK || (rc = nvn_guest_call(c, mem, g_layout->storage_class, &builder, 1, &cls)) != RL_OK)
        return (uint64_t)rc;
    trace("nvnTextureBuilderGetStorageClass(0x%llx) = 0x%llx", (unsigned long long)builder, (unsigned long long)cls);
    c->x[0] = cls;
    return nvn_ret(c);
}

/** @brief nvnDeviceFinalize(NVNdevice *). */
uint64_t nvnh_nvnDeviceFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_DEVICE));
    return nvn_ret(c);
}

/** @brief nvnDeviceGetInteger(const NVNdevice *, NVNdeviceInfo, int *). */
/**
 * @brief The driver's answer for NVNdeviceInfo info into *out, from the
 * build's layout (or the runtime's own table without one). Returns RL_OK or
 * a pending stop.
 */
static int device_integer(rl_ctx *c, uint8_t *mem, uint32_t info, int32_t *out)
{
    uint32_t own = sizeof device_info / sizeof device_info[0];
    const struct rl_nvn_layout *l = g_layout;
    if (l && l->nints) {
        if (info >= l->nints)
            return rl_guest_fatal(c, "nvn", "nvnDeviceGetInteger: the driver knows no NVNdeviceInfo %u", info);
        if (l->int_device[info] == 0) {
            *out = l->ints[info];
            return RL_OK;
        }
        if (l->int_device[info] == 2) {
            uint32_t v = 0;
            int rc = nvn_gpu_info_u32(c, mem, l->ints[info], &v);
            *out = (int32_t)v;
            return rc;
        }
        if (nvn_version_measured(info, out))
            return RL_OK;
        if (info >= own || nvn_generation() == 1)
            return rl_guest_fatal(c, "nvn",
                                  "nvnDeviceGetInteger: the driver reads NVNdeviceInfo %u from device state the "
                                  "runtime has no value for",
                                  info);
    } else if (info >= own) {
        return rl_guest_fatal(c, "nvn", "nvnDeviceGetInteger: unknown NVNdeviceInfo %u", info);
    }
    *out = device_info[info];
    return RL_OK;
}

/** @brief nvnDeviceGetInteger(const NVNdevice *, NVNdeviceInfo, int *). */
uint64_t nvnh_nvnDeviceGetInteger(rl_ctx *c, uint8_t *mem)
{
    int32_t v = 0;
    uint64_t dst = c->x[2];
    int rc = device_integer(c, mem, (uint32_t)c->x[1], &v);
    if (rc != RL_OK)
        return (uint64_t)rc;
    rl_st32(mem, dst, (uint32_t)v);
    return nvn_ret(c);
}

static uint32_t g_origin_mode, g_depth_mode;

uint32_t nvn_device_depth_mode(void) { return __atomic_load_n(&g_depth_mode, __ATOMIC_RELAXED); }
uint32_t nvn_device_origin_mode(void) { return __atomic_load_n(&g_origin_mode, __ATOMIC_RELAXED); }

void nvn_window_viewport(uint32_t origin, uint32_t height, const float v[4], float out[4])
{
    out[0] = v[0];
    out[1] = origin == NVN_ORIGIN_UPPER_LEFT ? v[1] + v[3] : (float)height - v[1];
    out[2] = v[2];
    out[3] = -v[3];
}

int64_t nvn_window_top(uint32_t origin, uint32_t height, int64_t y, int64_t h)
{
    return origin == NVN_ORIGIN_UPPER_LEFT ? y : (int64_t)height - y - h;
}

void nvn_frame_presented(void) { vi_guest_frame(); }

int64_t nvn_vsync_after(int64_t t) { return vi_vsync_after(t); }

void nvn_trace(char kind, uint64_t a, uint64_t b, uint64_t c) { vi_trace(kind, a, b, c); }

bool nvn_trace_on(void) { return vi_trace_on(); }

/** @brief nvnDeviceSetWindowOriginMode(NVNdevice *, NVNwindowOriginMode). */
uint64_t nvnh_nvnDeviceSetWindowOriginMode(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_device *d = (nvn_device *)nvn_obj_get(c->x[0], NVN_DEVICE);
    if (d)
        d->origin_mode = (uint32_t)c->x[1];
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "window origin mode %u", (uint32_t)c->x[1]);
    __atomic_store_n(&g_origin_mode, (uint32_t)c->x[1], __ATOMIC_RELAXED);
    return nvn_ret(c);
}

/** @brief nvnDeviceSetDepthMode(NVNdevice *, NVNdepthMode). */
uint64_t nvnh_nvnDeviceSetDepthMode(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_device *d = (nvn_device *)nvn_obj_get(c->x[0], NVN_DEVICE);
    if (d)
        d->depth_mode = (uint32_t)c->x[1];
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "depth mode %u", (uint32_t)c->x[1]);
    __atomic_store_n(&g_depth_mode, (uint32_t)c->x[1], __ATOMIC_RELAXED);
    return nvn_ret(c);
}

/**
 * @brief nvnDeviceGetWindowOriginMode(const NVNdevice *): NVNwindowOriginMode. The driver returns the word its
 * nvnDeviceSetWindowOriginMode stored in the device object (a one-load getter in every NVN1 and NVN2 driver read);
 * the layer keeps that mode in its own record of the device, and the driver's initial 0 is the default the layer
 * starts from. A device the layer does not know gives the last mode set on any device.
 */
uint64_t nvnh_nvnDeviceGetWindowOriginMode(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_device *d = (nvn_device *)nvn_obj_get(c->x[0], NVN_DEVICE);
    c->x[0] = d ? d->origin_mode : nvn_device_origin_mode();
    return nvn_ret(c);
}

/** @brief nvnDeviceGetDepthMode(const NVNdevice *): NVNdepthMode, as nvnDeviceGetWindowOriginMode answers for the origin. */
uint64_t nvnh_nvnDeviceGetDepthMode(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_device *d = (nvn_device *)nvn_obj_get(c->x[0], NVN_DEVICE);
    c->x[0] = d ? d->depth_mode : nvn_device_depth_mode();
    return nvn_ret(c);
}

/**
 * @brief nvnDeviceGenerateDebugDomainId(const NVNdevice *): the next id of a counter the driver keeps in its own
 * data (a fetch-and-add of 1 that returns the old value, from the 0 of its zeroed data), shared by every device.
 */
uint64_t nvnh_nvnDeviceGenerateDebugDomainId(rl_ctx *c, uint8_t *mem)
{
    static uint32_t next;
    (void)mem;
    c->x[0] = __atomic_fetch_add(&next, 1, __ATOMIC_RELAXED);
    return nvn_ret(c);
}

/** @brief nvnDeviceGetImageHandle(const NVNdevice *, int id): the id, as the driver encodes it (sdk 0x5d6830). */
uint64_t nvnh_nvnDeviceGetImageHandle(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = (uint64_t)(int64_t)(int32_t)c->x[1];
    return nvn_ret(c);
}

/**
 * @brief nvnDeviceGetTextureHandle(const NVNdevice *, int texture, int
 * sampler): texture | sampler << 20, as the driver encodes it; NVN1 ORs the
 * two as 32-bit words, sign-extends and adds its mark (16.2.0 0x44d970,
 * nvn_sampled_handle_mark).
 */
uint64_t nvnh_nvnDeviceGetTextureHandle(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    if (nvn_generation() == 1)
        c->x[0] = (uint64_t)(int64_t)(int32_t)((uint32_t)c->x[1] | (uint32_t)c->x[2] << 20) | nvn_sampled_handle_mark();
    else
        c->x[0] = (uint64_t)(int64_t)(int32_t)c->x[1] | (uint64_t)(int64_t)(int32_t)c->x[2] << 20;
    return nvn_ret(c);
}

/** @brief nvnQueueInitialize(NVNqueue *, const NVNqueueBuilder *): NVNboolean. */
uint64_t nvnh_nvnQueueInitialize(rl_ctx *c, uint8_t *mem)
{
    uint64_t queue = c->x[0], builder = c->x[1], device = 0;
    int rc = nvn_driver_call(c, mem, "nvnQueueBuilderGetDevice", &builder, 1, &device);
    if (rc != RL_OK)
        return (uint64_t)rc;
    nvn_queue *q = (nvn_queue *)nvn_obj_new(queue, NVN_QUEUE, sizeof *q);
    if (q)
        q->device = device;
    q = (nvn_queue *)nvn_obj_publish((nvn_obj *)q);
    trace("nvnQueueInitialize(0x%llx, device 0x%llx)", (unsigned long long)queue, (unsigned long long)device);
    c->x[0] = q != NULL;
    return nvn_ret(c);
}

/** @brief nvnQueueFinalize(NVNqueue *). */
uint64_t nvnh_nvnQueueFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_QUEUE));
    return nvn_ret(c);
}

/** @brief nvnQueueFlush(NVNqueue *): relift runs each submit when it is made. */
uint64_t nvnh_nvnQueueFlush(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return nvn_ret(c);
}

/** @brief nvnQueueFinish(NVNqueue *): returns once the host device finished every submit. */
uint64_t nvnh_nvnQueueFinish(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_lock();
    nvn_vk_settle();
    nvn_unlock();
    return nvn_ret(c);
}

/** @brief nvnQueueSubmitCommands(NVNqueue *, int count, const NVNcommandHandle *handles). */
uint64_t nvnh_nvnQueueSubmitCommands(rl_ctx *c, uint8_t *mem)
{
    nvn_queue *q = (nvn_queue *)nvn_obj_get(c->x[0], NVN_QUEUE);
    int32_t n = (int32_t)c->x[1];
    uint64_t handles = c->x[2];
    if (!q)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnQueueSubmitCommands on 0x%llx, not a queue",
                                        (unsigned long long)c->x[0]);
    for (int32_t i = 0; i < n; i++) {
        int rc = nvn_cmd_submit(c, mem, q->o.addr, rl_ld64(mem, handles + 8u * (uint64_t)i));
        if (rc != RL_OK)
            return (uint64_t)rc;
    }
    if (q->submits++ == 0)
        rl_log(RL_LOG_INFO, "nvn", "first submit: queue 0x%llx, %d command sets", (unsigned long long)q->o.addr, n);
    return nvn_ret(c);
}

/** @brief nvnQueueFenceSync(NVNqueue *, NVNsync *, NVNsyncCondition, int flags): signaled once the work submitted so far finished. */
uint64_t nvnh_nvnQueueFenceSync(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_sync *s = (nvn_sync *)nvn_obj_get(c->x[1], NVN_SYNC);
    if (!s)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnQueueFenceSync with 0x%llx, not a sync",
                                        (unsigned long long)c->x[1]);
    s->seq = nvn_vk_submitted();
    s->signaled = true;
    return nvn_ret(c);
}

/** @brief nvnQueueWaitSync(NVNqueue *, const NVNsync *): NVNboolean. */
uint64_t nvnh_nvnQueueWaitSync(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 1;
    return nvn_ret(c);
}

/** @brief nvn.memory: how pools reach the host device (see nvn_pool_back). */
enum { MEMORY_AUTO = 0, MEMORY_IMPORT = 1, MEMORY_MIRROR = 2 };

/**
 * @brief Gives pool p a device buffer over its guest bytes: an import where the host has one, a mirror otherwise or
 * when nvn.memory says mirror. False when neither works, which leaves the pool's GPU address its guest address.
 */
static bool pool_back(nvn_pool *p, uint64_t memory, uint64_t size)
{
    int mode = rl_setting_enum("nvn.memory");
    if (mode != MEMORY_MIRROR && kept_take(memory, size, &p->im))
        return true;
    if (mode != MEMORY_MIRROR && nvn_vk_import(memory, size, NULL, &p->im))
        return true;
    if (mode != MEMORY_IMPORT && nvn_mirror_make(memory, size, &p->im)) {
        static bool said;
        if (!said) {
            said = true;
            rl_log(RL_LOG_INFO, "nvn", "memory pools are mirrored into device memory (%s)",
                   mode == MEMORY_MIRROR ? "nvn.memory is mirror" : "the host device cannot import guest memory");
        }
        p->mirrored = true;
        return true;
    }
    return false;
}

bool nvn_pool_make(uint64_t pool, uint64_t memory, uint64_t size, uint32_t flags)
{
    nvn_pool *p = (nvn_pool *)nvn_obj_new(pool, NVN_POOL, sizeof *p);
    if (p) {
        p->memory = memory;
        p->size = size;
        p->flags = flags;
        bool virt = p->flags & NVN_POOL_VIRTUAL;
        if (!virt && pool_back(p, memory, size))
            pools_add(p->im.gpu, size, memory, p->im.buf, memory);
        else if (!virt) {
            rl_log(RL_LOG_WARN, "nvn", "memory pool 0x%llx (0x%llx bytes at 0x%llx) is not imported into the host device; "
                   "its GPU address is its guest address",
                   (unsigned long long)pool, (unsigned long long)size, (unsigned long long)memory);
            pools_add(memory, size, memory, NULL, memory);
        }
    }
    return nvn_obj_publish((nvn_obj *)p) != NULL;
}

/**
 * @brief Writes gpu into every field where the driver keeps a GPU address of
 * the guest's pool object at pool: the layer maps a pool once, so each kind of
 * mapping the console makes of it is that one range.
 */
static void pool_ranges(uint8_t *mem, uint64_t pool, uint64_t gpu)
{
    rl_st64(mem, pool + g_layout->pool_gpu, gpu);
    for (uint32_t i = 0; i < g_layout->npool_ranges; i++)
        rl_st64(mem, pool + g_layout->pool_ranges[i], gpu);
}

/**
 * @brief Writes into the guest's pool object at pool, made from builder, the
 * fields the driver's own texture code reads from a pool: what the driver's
 * own nvnMemoryPoolInitialize leaves constant, then the device, flags, GPU
 * addresses, mapping handles (the pool's own address, see
 * nvnh_NvRmGpuMappingModify), storage and size (nothing when the build read
 * no pool layout).
 */
static int pool_fields(rl_ctx *c, uint8_t *mem, uint64_t pool, uint64_t builder)
{
    const struct rl_nvn_layout *l = g_layout;
    nvn_pool *p = (nvn_pool *)nvn_obj_get(pool, NVN_POOL);
    if (!l || !l->pool_fields || !p)
        return RL_OK;
    uint64_t dev = 0;
    int rc = nvn_driver_call(c, mem, "nvnMemoryPoolBuilderGetDevice", &builder, 1, &dev);
    if (rc != RL_OK)
        return rc;
    bool virt = p->flags & NVN_POOL_VIRTUAL;
    const struct rl_nvn_field *f = virt ? l->pool_virtual : l->pool_fields;
    uint32_t n = virt ? l->npool_virtual : l->npool_fields;
    for (uint32_t i = 0; i < n; i++) {
        if (f[i].info >= 0)
            return rl_guest_fatal(c, "nvn", "the driver's pool field 0x%x comes from the GPU information", f[i].off);
        memcpy(mem + pool + f[i].off, f[i].bytes, f[i].len);
    }
    rl_st64(mem, pool + l->pool_device, dev);
    rl_st32(mem, pool + l->pool_flags, p->flags);
    pool_ranges(mem, pool, pool_gpu(p));
    for (uint32_t i = 0; i < l->npool_mappings; i++)
        rl_st64(mem, pool + l->pool_mappings[i], pool);
    rl_st64(mem, pool + l->pool_memory, p->memory);
    rl_st64(mem, pool + l->pool_size, p->size);
    return RL_OK;
}

#define NATIVE_IMAGES 64u

/** @brief The first handle NvRmMemHandleAllocNativeImage hands out; the SDK's own NvRm handles count from 1. */
#define NATIVE_HANDLE_BASE 0x40000000u

static struct {
    bool used;
    uint32_t image;
} g_native_image[NATIVE_IMAGES];

/**
 * @brief NvRmMemHandleAllocNativeImage(NvRmDevice *, uint32_t native image, uint64_t, uint32_t *handle): NvError.
 * The SDK's own makes an NvRm object that remembers the native image in guest memory the layer cannot read back by
 * its handle, so the layer hands out its own handles and remembers which native image each names.
 */
uint64_t nvnh_NvRmMemHandleAllocNativeImage(rl_ctx *c, uint8_t *mem)
{
    uint32_t image = (uint32_t)c->x[1];
    uint64_t out = c->x[3];
    uint32_t slot = NATIVE_IMAGES;
    for (uint32_t i = 0; i < NATIVE_IMAGES && slot == NATIVE_IMAGES; i++)
        if (!g_native_image[i].used)
            slot = i;
    if (slot == NATIVE_IMAGES || !out)
        return (uint64_t)rl_guest_fatal(c, "nvn", "NvRmMemHandleAllocNativeImage: %s",
                                        out ? "more than 64 native images are open" : "no place for the handle");
    g_native_image[slot].used = true;
    g_native_image[slot].image = image;
    rl_st32(mem, out, NATIVE_HANDLE_BASE + slot);
    trace("NvRmMemHandleAllocNativeImage(native image %u) = handle 0x%x", image, NATIVE_HANDLE_BASE + slot);
    c->x[0] = 0;
    return nvn_ret(c);
}

#define NATIVE_BUILDERS 32u

static struct {
    uint64_t builder;
    uint32_t handle;
} g_native_builder[NATIVE_BUILDERS];

/**
 * @brief nvnMemoryPoolBuilderSetNativeHandleNVX(NVNmemoryPoolBuilder *, uint32_t): the graphics device handle of a
 * native image (nn::gs), whose memory the pool is then made of by nvnMemoryPoolInitialize. Kept beside the builder
 * rather than in it, as no getter of the driver reads it back.
 */
uint64_t nvnh_nvnMemoryPoolBuilderSetNativeHandleNVX(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    uint64_t builder = c->x[0];
    uint32_t handle = (uint32_t)c->x[1], slot = NATIVE_BUILDERS;
    for (uint32_t i = 0; i < NATIVE_BUILDERS; i++) {
        if (g_native_builder[i].builder == builder) {
            slot = i;
            break;
        }
        if (slot == NATIVE_BUILDERS && !g_native_builder[i].builder)
            slot = i;
    }
    if (slot == NATIVE_BUILDERS)
        return (uint64_t)rl_guest_fatal(c, "nvn", "more than %u native image pool builders are open", NATIVE_BUILDERS);
    g_native_builder[slot].builder = builder;
    g_native_builder[slot].handle = handle;
    trace("nvnMemoryPoolBuilderSetNativeHandleNVX(0x%llx, %u)", (unsigned long long)builder, handle);
    return nvn_ret(c);
}

/** @brief The native handle set on builder, taken out of the table; false when none. */
static bool native_handle_take(uint64_t builder, uint32_t *handle)
{
    for (uint32_t i = 0; i < NATIVE_BUILDERS; i++)
        if (g_native_builder[i].builder == builder) {
            *handle = g_native_builder[i].handle;
            g_native_builder[i].builder = 0;
            return true;
        }
    return false;
}

/** @brief nvnMemoryPoolInitialize(NVNmemoryPool *, const NVNmemoryPoolBuilder *): NVNboolean. */
uint64_t nvnh_nvnMemoryPoolInitialize(rl_ctx *c, uint8_t *mem)
{
    uint64_t pool = c->x[0], builder = c->x[1], memory = 0, size = 0, flags = 0;
    int rc = nvn_driver_call(c, mem, "nvnMemoryPoolBuilderGetMemory", &builder, 1, &memory);
    if (rc == RL_OK)
        rc = nvn_driver_call(c, mem, "nvnMemoryPoolBuilderGetSize", &builder, 1, &size);
    if (rc == RL_OK)
        rc = nvn_driver_call(c, mem, "nvnMemoryPoolBuilderGetFlags", &builder, 1, &flags);
    if (rc != RL_OK)
        return (uint64_t)rc;
    uint32_t native;
    if (native_handle_take(builder, &native)) {
        uint32_t slot = native - NATIVE_HANDLE_BASE;
        if (native < NATIVE_HANDLE_BASE || slot >= NATIVE_IMAGES || !g_native_image[slot].used ||
            !rl_gsv_native_image_region(g_native_image[slot].image, &memory, &size))
            return (uint64_t)rl_guest_fatal(c, "nvn", "nvnMemoryPoolInitialize from native handle 0x%x, which names no native image bound to memory",
                                            native);
    }
    bool ok = nvn_pool_make(pool, memory, size, (uint32_t)flags);
    if (ok && (rc = pool_fields(c, mem, pool, builder)) != RL_OK)
        return (uint64_t)rc;
    trace("nvnMemoryPoolInitialize(0x%llx, memory 0x%llx, size 0x%llx, flags 0x%x)", (unsigned long long)pool,
          (unsigned long long)memory, (unsigned long long)size, (unsigned)flags);
    c->x[0] = ok;
    return nvn_ret(c);
}

/** @brief nvnMemoryPoolFinalize(NVNmemoryPool *). */
uint64_t nvnh_nvnMemoryPoolFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_pool *p = (nvn_pool *)nvn_obj_get(c->x[0], NVN_POOL);
    if (p && !p->im.gpu && !(p->flags & NVN_POOL_VIRTUAL))
        pools_remove(p->memory, p->size, NULL);
    if (p && p->im.gpu) {
        /* A kept import's span stays in the table: the device may still run commands the game recorded while the
         * pool lived, and the memory is still there. */
        if (p->alias) {
            pools_remove(p->im.gpu, p->size, p->im.buf);
            memset(&p->im, 0, sizeof p->im);
        } else if (p->mirrored) {
            pools_remove(p->im.gpu, p->size, p->im.buf);
            nvn_mirror_free(&p->im);
        } else {
            kept_put(p->memory, p->size, &p->im);
            memset(&p->im, 0, sizeof p->im);
        }
    }
    nvn_obj_free(p ? &p->o : NULL);
    return nvn_ret(c);
}

static nvn_pool *pool_of(rl_ctx *c, uint64_t a) { return (nvn_pool *)nvn_obj_get(a ? a : c->x[0], NVN_POOL); }

/** @brief nvnMemoryPoolMap(const NVNmemoryPool *): the storage, which the game's CPU writes in place. */
uint64_t nvnh_nvnMemoryPoolMap(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_pool *p = pool_of(c, 0);
    c->x[0] = p ? p->memory : 0;
    return nvn_ret(c);
}

/** @brief nvnMemoryPoolFlushMappedRange(const NVNmemoryPool *, ptrdiff_t, size_t). */
uint64_t nvnh_nvnMemoryPoolFlushMappedRange(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return nvn_ret(c);
}

/** @brief nvnMemoryPoolInvalidateMappedRange(const NVNmemoryPool *, ptrdiff_t, size_t). */
uint64_t nvnh_nvnMemoryPoolInvalidateMappedRange(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return nvn_ret(c);
}

/** @brief nvnMemoryPoolGetBufferAddress(const NVNmemoryPool *): its GPU address. */
uint64_t nvnh_nvnMemoryPoolGetBufferAddress(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_pool *p = pool_of(c, 0);
    c->x[0] = p ? pool_gpu(p) : 0;
    return nvn_ret(c);
}

/** @brief nvnMemoryPoolGetFlags(const NVNmemoryPool *). */
uint64_t nvnh_nvnMemoryPoolGetFlags(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_pool *p = pool_of(c, 0);
    c->x[0] = p ? p->flags : 0;
    return nvn_ret(c);
}

/** @brief nvnMemoryPoolGetSize(const NVNmemoryPool *). */
uint64_t nvnh_nvnMemoryPoolGetSize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_pool *p = pool_of(c, 0);
    c->x[0] = p ? p->size : 0;
    return nvn_ret(c);
}

/**
 * @brief nvnMemoryPoolMapVirtual(NVNmemoryPool *, int, const NVNmappingRequest *):
 * NVNboolean. As the driver: the pool must be virtual, and each request's
 * offsets and size must be multiples of the page size, the device's answer
 * for the info the build found the driver's mapping reads (false otherwise); a
 * request without a physical pool unmaps. The virtual pool's GPU addresses
 * reach the physical pages mapped into it: a second host import of the
 * physical pool's guest bytes, from the mapped distance, with the device
 * address the host gives it. As on the console, that range is the virtual
 * pool's own on hosts that address each allocation apart; a host whose
 * device addresses are the host pointers (llvmpipe) gives it the physical
 * pool's addresses of the same bytes instead, which reach the same bytes. A physical pool that is a mirror needs no
 * second buffer: the virtual pool's addresses are the mirror's from the mapped distance. One import is one linear
 * map, so every request must map at the same distance into the same pool,
 * and anything else stops. The storage class (the page kind) only chooses
 * how the console's GPU compresses the pages, which the host does not model.
 */
uint64_t nvnh_nvnMemoryPoolMapVirtual(rl_ctx *c, uint8_t *mem)
{
    uint64_t pool = c->x[0], req = c->x[2];
    int32_t n = (int32_t)c->x[1];
    nvn_pool *p = (nvn_pool *)nvn_obj_get(pool, NVN_POOL);
    if (!p || !(p->flags & NVN_POOL_VIRTUAL)) {
        c->x[0] = 0;
        return nvn_ret(c);
    }
    if (!g_layout || !g_layout->nints)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnMemoryPoolMapVirtual: the build read no %s driver layout",
                                        nvn_version_name());
    if (g_layout->pool_page_info == RL_NVN_NONE)
        return (uint64_t)rl_guest_fatal(c, "nvn",
                                        "nvnMemoryPoolMapVirtual: the build found no page size the %s driver checks "
                                        "mappings against",
                                        nvn_version_name());
    int32_t page_size = 0;
    int rc = device_integer(c, mem, g_layout->pool_page_info, &page_size);
    if (rc != RL_OK)
        return (uint64_t)rc;
    if (page_size <= 0 || (page_size & (page_size - 1)))
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnMemoryPoolMapVirtual: the driver's page size %d is no power of two",
                                        page_size);
    const uint64_t page = (uint64_t)page_size;
    for (int32_t i = 0; i < n; i++) {
        uint64_t r = req + 0x28 * (uint64_t)i;
        uint64_t phys = rl_ld64(mem, r), poff = rl_ld64(mem, r + 8), voff = rl_ld64(mem, r + 16);
        uint64_t size = rl_ld64(mem, r + 24);
        if ((poff | voff | size) & (page - 1)) {
            c->x[0] = 0;
            return nvn_ret(c);
        }
        if (!phys)
            continue;
        nvn_pool *q = (nvn_pool *)nvn_obj_get(phys, NVN_POOL);
        if (!q || (q->flags & NVN_POOL_VIRTUAL) || poff > q->size || size > q->size - poff || voff > p->size ||
            size > p->size - voff)
            return (uint64_t)rl_guest_fatal(c, "nvn",
                                            "nvnMemoryPoolMapVirtual(0x%llx): request %d maps 0x%llx bytes of pool 0x%llx "
                                            "at 0x%llx to 0x%llx, outside the pools",
                                            (unsigned long long)pool, i, (unsigned long long)size,
                                            (unsigned long long)phys, (unsigned long long)poff,
                                            (unsigned long long)voff);
        if (poff < voff || p->size > q->size - (poff - voff))
            return (uint64_t)rl_guest_fatal(c, "nvn",
                                            "nvnMemoryPoolMapVirtual(0x%llx): request %d maps the virtual pool past the "
                                            "ends of pool 0x%llx (not implemented)",
                                            (unsigned long long)pool, i, (unsigned long long)phys);
        uint64_t memory = q->memory + poff - voff;
        if (p->mapped && (p->memory != memory || p->phys != phys))
            return (uint64_t)rl_guest_fatal(c, "nvn",
                                            "nvnMemoryPoolMapVirtual(0x%llx): request %d maps pages from another place "
                                            "than earlier ones (only one linear range per virtual pool is implemented)",
                                            (unsigned long long)pool, i);
        if (!p->mapped) {
            if (q->mirrored) {
                p->im = (nvn_import){ .buf = q->im.buf, .memory = NULL, .gpu = q->im.gpu + (poff - voff) };
                p->alias = true;
            } else if (!q->im.gpu ||
                       (!kept_take(memory, p->size, &p->im) && !nvn_vk_import(memory, p->size, NULL, &p->im))) {
                return (uint64_t)rl_guest_fatal(c, "nvn",
                                                "nvnMemoryPoolMapVirtual(0x%llx): pool 0x%llx is not in the host "
                                                "device, or its bytes from 0x%llx cannot be imported again",
                                                (unsigned long long)pool, (unsigned long long)phys,
                                                (unsigned long long)(poff - voff));
            }
            pools_add(p->im.gpu, p->size, memory, p->im.buf, p->alias ? q->memory : memory);
        }
        p->memory = memory;
        p->phys = phys;
        p->mapped = true;
    }
    if (g_layout->pool_fields)
        pool_ranges(mem, pool, p->im.gpu);
    trace("nvnMemoryPoolMapVirtual(0x%llx, %d requests): GPU 0x%llx over guest 0x%llx", (unsigned long long)pool, n,
          (unsigned long long)p->im.gpu, (unsigned long long)p->memory);
    c->x[0] = 1;
    return nvn_ret(c);
}

/**
 * @brief NvRmGpuMappingModify(NvRmGpuMapping *, uint64_t offset, uint64_t
 * size, const params *): NvError. The layer owns every pool's GPU mapping and
 * leaves the pool's own address where the driver keeps its mapping handles,
 * so the only handles the driver's kept code passes are pools. The driver's
 * own nvnTextureInitialize calls it to give a compressible pool's pages
 * under a texture the texture's page kind (the u32 at params); kinds only
 * choose how the console's GPU compresses the pages, which the host does
 * not model, so it checks the range (within the pool rounded up to the 64
 * KiB big page the driver aligns to) and succeeds. A NULL params is the SDK's
 * own NvError 4 (bad parameter); a handle that is no pool stops.
 */
uint64_t nvnh_NvRmGpuMappingModify(rl_ctx *c, uint8_t *mem)
{
    uint64_t h = c->x[0], off = c->x[1], size = c->x[2], params = c->x[3];
    nvn_pool *p = (nvn_pool *)nvn_obj_get(h, NVN_POOL);
    if (!p)
        return (uint64_t)rl_guest_fatal(c, "nvn", "NvRmGpuMappingModify on 0x%llx, which is no memory pool mapping of the NVN2 layer",
                                        (unsigned long long)h);
    if (!params) {
        c->x[0] = 4;
        return nvn_ret(c);
    }
    uint64_t end = (p->size + 0xffffu) & ~(uint64_t)0xffff;
    if (off >= p->size || size > end - off)
        return (uint64_t)rl_guest_fatal(c, "nvn",
                                        "NvRmGpuMappingModify(pool 0x%llx): 0x%llx bytes at 0x%llx, outside its 0x%llx bytes",
                                        (unsigned long long)h, (unsigned long long)size, (unsigned long long)off,
                                        (unsigned long long)p->size);
    trace("NvRmGpuMappingModify(pool 0x%llx, 0x%llx bytes at 0x%llx, kind 0x%x)", (unsigned long long)h,
          (unsigned long long)size, (unsigned long long)off, (unsigned)rl_ld32(mem, params));
    c->x[0] = 0;
    return nvn_ret(c);
}

uint64_t nvn_pool_gpu(uint64_t pool)
{
    nvn_pool *p = (nvn_pool *)nvn_obj_get(pool, NVN_POOL);
    return p ? pool_gpu(p) : 0;
}

uint64_t nvn_pool_memory(uint64_t pool, uint64_t *size)
{
    nvn_pool *p = (nvn_pool *)nvn_obj_get(pool, NVN_POOL);
    if (size)
        *size = p ? p->size : 0;
    return p ? p->memory : 0;
}

/** @brief nvnBufferInitialize(NVNbuffer *, const NVNbufferBuilder *): NVNboolean. */
uint64_t nvnh_nvnBufferInitialize(rl_ctx *c, uint8_t *mem)
{
    uint64_t buf = c->x[0], builder = c->x[1], pool = 0, offset = 0, size = 0;
    int rc = nvn_driver_call(c, mem, "nvnBufferBuilderGetMemoryPool", &builder, 1, &pool);
    if (rc == RL_OK)
        rc = nvn_driver_call(c, mem, "nvnBufferBuilderGetMemoryOffset", &builder, 1, &offset);
    if (rc == RL_OK)
        rc = nvn_driver_call(c, mem, "nvnBufferBuilderGetSize", &builder, 1, &size);
    if (rc != RL_OK)
        return (uint64_t)rc;
    nvn_buffer *b = (nvn_buffer *)nvn_obj_new(buf, NVN_BUFFER, sizeof *b);
    if (b) {
        b->pool = pool;
        b->offset = offset;
        b->size = size;
        b->memory = nvn_pool_memory(pool, NULL) + offset;
        b->gpu = nvn_pool_gpu(pool) + offset;
    }
    c->x[0] = nvn_obj_publish((nvn_obj *)b) != NULL;
    return nvn_ret(c);
}

/** @brief nvnBufferFinalize(NVNbuffer *). */
uint64_t nvnh_nvnBufferFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_BUFFER));
    return nvn_ret(c);
}

static nvn_buffer *buffer_of(rl_ctx *c) { return (nvn_buffer *)nvn_obj_get(c->x[0], NVN_BUFFER); }

/** @brief nvnBufferMap(const NVNbuffer *). */
uint64_t nvnh_nvnBufferMap(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_buffer *b = buffer_of(c);
    c->x[0] = b ? b->memory : 0;
    return nvn_ret(c);
}

/** @brief nvnBufferGetAddress(const NVNbuffer *). */
uint64_t nvnh_nvnBufferGetAddress(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_buffer *b = buffer_of(c);
    c->x[0] = b ? b->gpu : 0;
    return nvn_ret(c);
}

/** @brief nvnBufferGetSize(const NVNbuffer *). */
uint64_t nvnh_nvnBufferGetSize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_buffer *b = buffer_of(c);
    c->x[0] = b ? b->size : 0;
    return nvn_ret(c);
}

/** @brief nvnBufferGetMemoryPool(const NVNbuffer *). */
uint64_t nvnh_nvnBufferGetMemoryPool(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_buffer *b = buffer_of(c);
    c->x[0] = b ? b->pool : 0;
    return nvn_ret(c);
}

/** @brief nvnBufferGetMemoryOffset(const NVNbuffer *). */
uint64_t nvnh_nvnBufferGetMemoryOffset(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_buffer *b = buffer_of(c);
    c->x[0] = b ? b->offset : 0;
    return nvn_ret(c);
}

/** @brief nvnBufferFlushMappedRange(const NVNbuffer *, ptrdiff_t, size_t). */
uint64_t nvnh_nvnBufferFlushMappedRange(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return nvn_ret(c);
}

/** @brief nvnBufferInvalidateMappedRange(const NVNbuffer *, ptrdiff_t, size_t). */
uint64_t nvnh_nvnBufferInvalidateMappedRange(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return nvn_ret(c);
}

/** @brief nvnSyncInitialize(NVNsync *, NVNdevice *): NVNboolean. */
uint64_t nvnh_nvnSyncInitialize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = nvn_obj_publish(nvn_obj_new(c->x[0], NVN_SYNC, sizeof(nvn_sync))) != NULL;
    return nvn_ret(c);
}

/** @brief nvnSyncFinalize(NVNsync *). */
uint64_t nvnh_nvnSyncFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_SYNC));
    return nvn_ret(c);
}

void nvn_sync_signal(uint64_t sync)
{
    nvn_sync *s = sync ? (nvn_sync *)nvn_obj_get(sync, NVN_SYNC) : NULL;
    if (s) {
        s->seq = nvn_vk_submitted();
        s->signaled = true;
    }
}

/** @brief NVNsyncWaitResult values. */
enum { SYNC_ALREADY_SIGNALED = 0, SYNC_CONDITION_SATISFIED = 1, SYNC_TIMEOUT_EXPIRED = 2, SYNC_FAILED = 3 };

/**
 * @brief nvnSyncWait(const NVNsync *, uint64_t timeout_ns): NVNsyncWaitResult. A fenced sync whose work the host
 * device is still running is waited for (the batch in flight is the only one, so the wait is short and the timeout
 * does not cut it).
 */
uint64_t nvnh_nvnSyncWait(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_sync *s = (nvn_sync *)nvn_obj_get(c->x[0], NVN_SYNC);
    uint64_t rc = SYNC_FAILED;
    if (s && !s->signaled) {
        rc = SYNC_TIMEOUT_EXPIRED;
    } else if (s && s->seq <= nvn_vk_finished()) {
        rc = SYNC_ALREADY_SIGNALED;
    } else if (s) {
        nvn_lock();
        nvn_vk_settle();
        nvn_unlock();
        rc = SYNC_CONDITION_SATISFIED;
    }
    c->x[0] = rc;
    return nvn_ret(c);
}
