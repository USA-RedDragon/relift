/**
 * @file nvn_dev.c
 * @brief NVN2 devices, queues, memory pools, buffers and syncs.
 *
 * GPU addresses are guest addresses: a memory pool's GPU address is the
 * guest address of its storage, so a buffer or texture address names the
 * guest bytes behind it.
 */
#include "nvn_priv.h"

#include <stdarg.h>
#include <stdlib.h>

/**
 * @brief nvnDeviceGetInteger answers by NVNdeviceInfo: the SDK driver's
 * constants, and the Switch's values where the driver reads what its device
 * initialization measured (scratch memory factors, L2 size, texture limits).
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
} nvn_device;

typedef struct nvn_queue {
    nvn_obj o;
    uint64_t device;
    uint64_t submits;
} nvn_queue;

typedef struct nvn_pool {
    nvn_obj o;
    uint64_t memory, size;
    uint32_t flags;
} nvn_pool;

typedef struct nvn_buffer {
    nvn_obj o;
    uint64_t pool, offset, size;
    uint64_t memory;
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
    nvn_device *d = (nvn_device *)nvn_obj_new(dev, NVN_DEVICE, sizeof *d);
    trace("nvnDeviceInitialize(0x%llx, flags 0x%x)", (unsigned long long)dev, (unsigned)flags);
    c->x[0] = d != NULL;
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
uint64_t nvnh_nvnDeviceGetInteger(rl_ctx *c, uint8_t *mem)
{
    uint32_t info = (uint32_t)c->x[1];
    if (info >= sizeof device_info / sizeof device_info[0])
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnDeviceGetInteger: unknown NVNdeviceInfo %u", info);
    rl_st32(mem, c->x[2], (uint32_t)device_info[info]);
    return nvn_ret(c);
}

static uint32_t g_origin_mode, g_depth_mode;

uint32_t nvn_device_depth_mode(void) { return __atomic_load_n(&g_depth_mode, __ATOMIC_RELAXED); }
uint32_t nvn_device_origin_mode(void) { return __atomic_load_n(&g_origin_mode, __ATOMIC_RELAXED); }

/** @brief nvnDeviceSetWindowOriginMode(NVNdevice *, NVNwindowOriginMode). */
uint64_t nvnh_nvnDeviceSetWindowOriginMode(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_device *d = (nvn_device *)nvn_obj_get(c->x[0], NVN_DEVICE);
    if (d)
        d->origin_mode = (uint32_t)c->x[1];
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
    nvn_pool *p = (nvn_pool *)nvn_obj_new(pool, NVN_POOL, sizeof *p);
    if (p) {
        p->memory = memory;
        p->size = size;
        p->flags = (uint32_t)flags;
    }
    trace("nvnMemoryPoolInitialize(0x%llx, memory 0x%llx, size 0x%llx, flags 0x%x)", (unsigned long long)pool,
          (unsigned long long)memory, (unsigned long long)size, (unsigned)flags);
    c->x[0] = p != NULL;
    return nvn_ret(c);
}

/** @brief nvnMemoryPoolFinalize(NVNmemoryPool *). */
uint64_t nvnh_nvnMemoryPoolFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_POOL));
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

/** @brief nvnMemoryPoolGetBufferAddress(const NVNmemoryPool *): its GPU address, the guest address of its storage. */
uint64_t nvnh_nvnMemoryPoolGetBufferAddress(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_pool *p = pool_of(c, 0);
    c->x[0] = p ? p->memory : 0;
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
    c->x[0] = b ? b->memory : 0;
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
