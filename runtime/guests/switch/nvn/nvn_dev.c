/**
 * @file nvn_dev.c
 * @brief NVN2 devices, queues, memory pools, buffers and syncs.
 *
 * A memory pool's storage is guest memory, imported into the host Vulkan
 * device as a buffer (VK_EXT_external_memory_host); the pool's GPU address
 * is that buffer's device address, so the translated programs dereference
 * the addresses the game computes from it, and the layer turns a GPU
 * address back into the guest address of the same byte (nvn_gpu_guest).
 * Without an import a pool's GPU address is its guest address.
 */
#include "nvn_priv.h"
#include "vi.h"

#include <pthread.h>
#include <stdarg.h>
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
    /** @brief Whether the driver's fields are written into the guest's device object (see device_fields). */
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
} nvn_pool;

/** @brief Imported pools by GPU address, for nvn_gpu_guest. */
static struct {
    pthread_rwlock_t lock;
    struct pool_span {
        uint64_t gpu, size, memory;
        void *buf;
    } *v;
    uint32_t n, cap;
} g_pools = { PTHREAD_RWLOCK_INITIALIZER, NULL, 0, 0 };

static void pools_add(uint64_t gpu, uint64_t size, uint64_t memory, void *buf)
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
    pthread_rwlock_unlock(&g_pools.lock);
}

static void pools_remove(uint64_t gpu)
{
    pthread_rwlock_wrlock(&g_pools.lock);
    for (uint32_t i = 0; i < g_pools.n; i++)
        if (g_pools.v[i].gpu == gpu) {
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
    if (lo && gpu - g_pools.v[lo - 1].gpu < g_pools.v[lo - 1].size)
        out = g_pools.v[lo - 1].memory + (gpu - g_pools.v[lo - 1].gpu);
    pthread_rwlock_unlock(&g_pools.lock);
    return out;
}

bool nvn_guest_buffer(uint64_t guest, uint64_t size, void **buf, uint64_t *off)
{
    bool found = false;
    pthread_rwlock_rdlock(&g_pools.lock);
    for (uint32_t i = 0; i < g_pools.n && !found; i++) {
        const struct pool_span *p = &g_pools.v[i];
        if (p->buf && guest >= p->memory && guest - p->memory + size <= p->size) {
            *buf = p->buf;
            *off = guest - p->memory;
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
    bool signaled;
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

/**
 * @brief Writes into the guest's device object at dev the fields the
 * driver's nvnTextureBuilderGetStorageClass reads: the flags word, the
 * NvRmGpuDevice, and the bytes the driver's own nvnDeviceInitialize leaves
 * in the others (the layer keeps its own device state, so it never reads the
 * guest's object).
 */
static int device_fields(rl_ctx *c, uint8_t *mem, uint64_t dev)
{
    nvn_device *d = (nvn_device *)nvn_obj_get(dev, NVN_DEVICE);
    if (!d)
        return rl_guest_fatal(c, "nvn", "texture builder of device 0x%llx, which is not initialized",
                              (unsigned long long)dev);
    if (d->fields)
        return RL_OK;
    int rc = check_settings(c, mem);
    if (rc != RL_OK)
        return rc;
    uint64_t gpu = nvn_gpu_device();
    if (!gpu)
        return rl_guest_fatal(c, "nvn", "the GPU is not open");
    rl_st32(mem, dev + g_layout->flags, d->flags);
    rl_st64(mem, dev + g_layout->gpu_device, gpu);
    for (uint32_t i = 0; i < g_layout->nfields; i++)
        memcpy(mem + dev + g_layout->fields[i].off, g_layout->fields[i].bytes, g_layout->fields[i].len);
    d->fields = true;
    return RL_OK;
}

/** @brief nvnTextureBuilderGetStorageClass(const NVNtextureBuilder *): the driver's own function, on the fields it reads. */
uint64_t nvnh_nvnTextureBuilderGetStorageClass(rl_ctx *c, uint8_t *mem)
{
    if (!g_layout)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnTextureBuilderGetStorageClass: the build read no NVN2 driver layout");
    uint64_t builder = c->x[0], cls = 0;
    int rc = device_fields(c, mem, rl_ld64(mem, builder + g_layout->builder_device));
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
        if (info >= own)
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

/** @brief nvnDeviceGetImageHandle(const NVNdevice *, int id): the id, as the driver encodes it (sdk 0x5d6830). */
uint64_t nvnh_nvnDeviceGetImageHandle(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = (uint64_t)(int64_t)(int32_t)c->x[1];
    return nvn_ret(c);
}

/** @brief nvnDeviceGetTextureHandle(const NVNdevice *, int texture, int sampler): texture | sampler << 20, as the driver encodes it. */
uint64_t nvnh_nvnDeviceGetTextureHandle(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
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

/** @brief nvnQueueFinish(NVNqueue *). */
uint64_t nvnh_nvnQueueFinish(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
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

/** @brief nvnQueueFenceSync(NVNqueue *, NVNsync *, NVNsyncCondition, int flags): work runs at submit, so the sync is signaled now. */
uint64_t nvnh_nvnQueueFenceSync(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_sync *s = (nvn_sync *)nvn_obj_get(c->x[1], NVN_SYNC);
    if (!s)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnQueueFenceSync with 0x%llx, not a sync",
                                        (unsigned long long)c->x[1]);
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

bool nvn_pool_make(uint64_t pool, uint64_t memory, uint64_t size, uint32_t flags)
{
    nvn_pool *p = (nvn_pool *)nvn_obj_new(pool, NVN_POOL, sizeof *p);
    if (p) {
        p->memory = memory;
        p->size = size;
        p->flags = flags;
        bool virt = p->flags & NVN_POOL_VIRTUAL;
        if (!virt && nvn_vk_import(memory, size, &p->im))
            pools_add(p->im.gpu, size, memory, p->im.buf);
        else if (!virt)
            rl_log(RL_LOG_WARN, "nvn", "memory pool 0x%llx (0x%llx bytes at 0x%llx) is not imported into the host device; "
                   "its GPU address is its guest address",
                   (unsigned long long)pool, (unsigned long long)size, (unsigned long long)memory);
    }
    return p != NULL;
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
    bool ok = nvn_pool_make(pool, memory, size, (uint32_t)flags);
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
    if (p && p->im.gpu) {
        pools_remove(p->im.gpu);
        nvn_vk_unimport(&p->im);
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
 * request without a physical pool unmaps. As on the console, the virtual
 * pool has GPU addresses of its own, which reach the physical pages mapped
 * into it: a second host import of the physical pool's guest bytes, from the
 * mapped distance, with its own device address. One import is one linear
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
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnMemoryPoolMapVirtual: the build read no NVN2 driver layout");
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
            if (!q->im.gpu || !nvn_vk_import(memory, p->size, &p->im))
                return (uint64_t)rl_guest_fatal(c, "nvn",
                                                "nvnMemoryPoolMapVirtual(0x%llx): pool 0x%llx is not in the host "
                                                "device, or its bytes from 0x%llx cannot be imported again",
                                                (unsigned long long)pool, (unsigned long long)phys,
                                                (unsigned long long)(poff - voff));
            pools_add(p->im.gpu, p->size, memory, p->im.buf);
        }
        p->memory = memory;
        p->phys = phys;
        p->mapped = true;
    }
    trace("nvnMemoryPoolMapVirtual(0x%llx, %d requests): GPU 0x%llx over guest 0x%llx", (unsigned long long)pool, n,
          (unsigned long long)p->im.gpu, (unsigned long long)p->memory);
    c->x[0] = 1;
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
    c->x[0] = b != NULL;
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
    c->x[0] = nvn_obj_new(c->x[0], NVN_SYNC, sizeof(nvn_sync)) != NULL;
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
    if (s)
        s->signaled = true;
}

/** @brief NVNsyncWaitResult values. */
enum { SYNC_ALREADY_SIGNALED = 0, SYNC_CONDITION_SATISFIED = 1, SYNC_TIMEOUT_EXPIRED = 2, SYNC_FAILED = 3 };

/** @brief nvnSyncWait(const NVNsync *, uint64_t timeout_ns): NVNsyncWaitResult. */
uint64_t nvnh_nvnSyncWait(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_sync *s = (nvn_sync *)nvn_obj_get(c->x[0], NVN_SYNC);
    c->x[0] = !s ? SYNC_FAILED : s->signaled ? SYNC_ALREADY_SIGNALED : SYNC_TIMEOUT_EXPIRED;
    return nvn_ret(c);
}
