/**
 * @file nvn_misc.c
 * @brief NVN procs with little state: descriptor handles, timestamps,
 * events, and the queries of queues, pools and windows.
 */
#include "nvn_priv.h"

/** @brief nvnDeviceGetSeparateTextureHandle(const NVNdevice *, int id): the id with the NVN1 mark, as the driver encodes it. */
uint64_t nvnh_nvnDeviceGetSeparateTextureHandle(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = (uint64_t)(int64_t)(int32_t)c->x[1] | nvn_sampled_handle_mark();
    return nvn_ret(c);
}

/** @brief nvnDeviceGetSeparateSamplerHandle(const NVNdevice *, int id): id << 20, as the driver encodes it. */
uint64_t nvnh_nvnDeviceGetSeparateSamplerHandle(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = (uint64_t)(int64_t)(int32_t)c->x[1] << 20;
    return nvn_ret(c);
}

/** @brief nvnDeviceGetTexelFetchHandle(const NVNdevice *, int id): the id with the NVN1 mark. */
uint64_t nvnh_nvnDeviceGetTexelFetchHandle(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = (uint64_t)(int64_t)(int32_t)c->x[1] | nvn_sampled_handle_mark();
    return nvn_ret(c);
}

/** @brief nvnDeviceGetUniformBufferHandle(const NVNdevice *, NVNbufferAddress, size_t): the driver's packing (sdk 0x5f85b0). */
uint64_t nvnh_nvnDeviceGetUniformBufferHandle(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    uint64_t hi = (0x1e0000000000ull + (c->x[2] << 41)) & 0xffffe00000000000ull;
    c->x[0] = hi | c->x[1] >> 4;
    return nvn_ret(c);
}

/** @brief nvnDeviceGetCommandHandleMemorySize(const NVNdevice *, NVNcommandHandle): relift's handles hold no guest memory. */
uint64_t nvnh_nvnDeviceGetCommandHandleMemorySize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 0;
    return nvn_ret(c);
}

/** @brief nvnDeviceFinalizeCommandHandle(NVNdevice *, NVNcommandHandle). */
uint64_t nvnh_nvnDeviceFinalizeCommandHandle(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdset_free(c->x[1]);
    return nvn_ret(c);
}

/** @brief nvnDeviceGetCurrentTimestampInNanoseconds(const NVNdevice *). */
uint64_t nvnh_nvnDeviceGetCurrentTimestampInNanoseconds(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = rl_guest_clock_ns();
    return nvn_ret(c);
}

/** @brief nvnDeviceGetTimestampInNanoseconds(const NVNdevice *, const NVNcounterData *): the counter's timestamp, which relift writes in nanoseconds. */
uint64_t nvnh_nvnDeviceGetTimestampInNanoseconds(rl_ctx *c, uint8_t *mem)
{
    c->x[0] = c->x[1] ? rl_ld64(mem, c->x[1] + 8) : 0;
    return nvn_ret(c);
}

typedef struct nvn_event {
    nvn_obj o;
    uint64_t pool, offset, addr;
} nvn_event;

/** @brief nvnEventInitialize(NVNevent *, const NVNeventBuilder *): NVNboolean; the value is the u32 at the builder's pool and offset. */
uint64_t nvnh_nvnEventInitialize(rl_ctx *c, uint8_t *mem)
{
    uint64_t ev = c->x[0], builder = c->x[1], pool = 0, offset = 0;
    int rc = nvn_driver_call(c, mem, "nvnEventBuilderGetMemoryPool", &builder, 1, &pool);
    if (rc == RL_OK)
        rc = nvn_driver_call(c, mem, "nvnEventBuilderGetMemoryOffset", &builder, 1, &offset);
    if (rc != RL_OK)
        return (uint64_t)rc;
    uint64_t base = nvn_pool_memory(pool, NULL);
    if (!base)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnEventInitialize: pool 0x%llx is not a memory pool",
                                        (unsigned long long)pool);
    nvn_event *e = (nvn_event *)nvn_obj_new(ev, NVN_EVENT, sizeof *e);
    if (e) {
        e->pool = pool;
        e->offset = offset;
        e->addr = base + offset;
    }
    c->x[0] = e != NULL;
    return nvn_ret(c);
}

uint64_t nvn_event_word(uint64_t event)
{
    nvn_event *e = (nvn_event *)nvn_obj_get(event, NVN_EVENT);
    return e ? e->addr : 0;
}

/** @brief nvnEventFinalize(NVNevent *). */
uint64_t nvnh_nvnEventFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_EVENT));
    return nvn_ret(c);
}

static nvn_event *event_of(rl_ctx *c)
{
    nvn_event *e = (nvn_event *)nvn_obj_get(c->x[0], NVN_EVENT);
    if (!e)
        rl_guest_fatal(c, "nvn", "event proc on 0x%llx, not an event", (unsigned long long)c->x[0]);
    return e;
}

/** @brief nvnEventGetValue(const NVNevent *): uint32_t. */
uint64_t nvnh_nvnEventGetValue(rl_ctx *c, uint8_t *mem)
{
    nvn_event *e = event_of(c);
    if (!e)
        return RL_PENDING;
    c->x[0] = __atomic_load_n((uint32_t *)(void *)(mem + e->addr), __ATOMIC_ACQUIRE);
    return nvn_ret(c);
}

/** @brief NVNeventSignalMode: write the value, or add it. */
enum { EVENT_WRITE = 0, EVENT_ADD = 1 };

/** @brief nvnEventSignal(NVNevent *, NVNeventSignalMode, uint32_t value). */
uint64_t nvnh_nvnEventSignal(rl_ctx *c, uint8_t *mem)
{
    nvn_event *e = event_of(c);
    if (!e)
        return RL_PENDING;
    uint32_t *p = (uint32_t *)(void *)(mem + e->addr);
    if ((uint32_t)c->x[1] == EVENT_ADD)
        __atomic_add_fetch(p, (uint32_t)c->x[2], __ATOMIC_RELEASE);
    else
        __atomic_store_n(p, (uint32_t)c->x[2], __ATOMIC_RELEASE);
    nvn_vk_pump(c, mem);
    return nvn_ret(c);
}

/** @brief nvnEventGetMemoryPool(const NVNevent *). */
uint64_t nvnh_nvnEventGetMemoryPool(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_event *e = event_of(c);
    if (!e)
        return RL_PENDING;
    c->x[0] = e->pool;
    return nvn_ret(c);
}

/** @brief nvnEventGetMemoryOffset(const NVNevent *). */
uint64_t nvnh_nvnEventGetMemoryOffset(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_event *e = event_of(c);
    if (!e)
        return RL_PENDING;
    c->x[0] = e->offset;
    return nvn_ret(c);
}

/** @brief NVNformats with integer channels, which the SDK driver's format table marks as having no float fast clear. */
static const uint64_t int_formats[3] = { 0x4001b19b19b19b18ull, 0x01b1840000000000ull, 0x600000000ull };

/**
 * @brief nvnDeviceRegisterFastClearColor(NVNdevice *, const float *color, NVNformat): NVNboolean. A fast clear
 * color only lets the GPU compress clears; relift's clears do not depend on it. The driver refuses formats with
 * integer channels and accepts the rest while its table has room; relift's table never fills.
 */
uint64_t nvnh_nvnDeviceRegisterFastClearColor(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    uint32_t f = (uint32_t)c->x[2];
    c->x[0] = f < 192 && !(int_formats[f / 64] >> (f % 64) & 1);
    return nvn_ret(c);
}

/**
 * @brief nvnDeviceRegisterFastClearDepth(NVNdevice *, float depth): NVNboolean. As for a fast clear color, the value
 * only lets the GPU compress depth clears, which relift's clears do not depend on. The driver adds the value to the
 * device's hardware table of clear values and answers whether it had room (NVN1 and NVN2 drivers read: true while the
 * table has an entry free, whatever the value); relift's table never fills.
 */
uint64_t nvnh_nvnDeviceRegisterFastClearDepth(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 1;
    return nvn_ret(c);
}

/** @brief Returns from a proc that has no effect in relift's layer (void: x0 is not read). */
static uint64_t noop(const rl_ctx *c) { return nvn_ret(c); }

/**
 * @brief nvnCommandBufferSetColorReductionEnable(NVNcommandBuffer *, NVNboolean). Does nothing:
 * the driver only appends one GPU method word to the command memory, which relift never writes, and color reduction is a GPU bandwidth optimization of render target writes that a host render target does not have; a full-precision target is within what the optimization may produce (inferred, see .work/s2-console-questions.md).
 */
uint64_t nvnh_nvnCommandBufferSetColorReductionEnable(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return noop(c);
}

/**
 * @brief nvnCommandBufferSetColorReductionThresholds(NVNcommandBuffer *, ...). Does nothing:
 * the driver only appends two GPU method words to the command memory, which relift never writes: the thresholds of the color reduction that nvnCommandBufferSetColorReductionEnable switches, which a host render target does not have.
 */
uint64_t nvnh_nvnCommandBufferSetColorReductionThresholds(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return noop(c);
}

/**
 * @brief nvnCommandBufferSetStencilCullCriteria(NVNcommandBuffer *, ...). Does nothing:
 * the driver only appends one GPU method pair to the command memory, which relift never writes: it sets when the GPU may cull a tile early on the stencil value, which is an optimization of the order work is done in that the host's own early tests also take.
 */
uint64_t nvnh_nvnCommandBufferSetStencilCullCriteria(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return noop(c);
}

/**
 * @brief nvnCommandBufferOverrideSubtileSize(NVNcommandBuffer *, ...). Does nothing:
 * the driver only appends one GPU method word to the command memory, which relift never writes: it sets the size of the GPU's binning subtiles, a layout the host GPU chooses for itself.
 */
uint64_t nvnh_nvnCommandBufferOverrideSubtileSize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return noop(c);
}

/**
 * @brief nvnCommandBufferSetSubpixelPrecisionBias(NVNcommandBuffer *, int, int). Does nothing:
 * the driver only appends sixteen GPU method words to the command memory, which relift never writes: they bias the sub-pixel grid the GPU snaps vertices to, which a host rasterizer fixes for itself (a vertex may land one sub-pixel step away from the console's).
 */
uint64_t nvnh_nvnCommandBufferSetSubpixelPrecisionBias(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return noop(c);
}

/**
 * @brief nvnCommandBufferDiscardColor(NVNcommandBuffer *, int). Does nothing:
 * the driver appends one GPU method word to the command memory, which relift never writes, unless its device flags it off (a byte of the device record); the call says the render target's contents are not needed afterward, so leaving them as they are is one of the values left undefined.
 */
uint64_t nvnh_nvnCommandBufferDiscardColor(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return noop(c);
}

/**
 * @brief nvnCommandBufferDiscardDepthStencil(NVNcommandBuffer *). Does nothing:
 * the driver appends one GPU method word to the command memory, which relift never writes, unless its device flags it off (a byte of the device record); the call says the depth and stencil contents are not needed afterward, so leaving them as they are is one of the values left undefined.
 */
uint64_t nvnh_nvnCommandBufferDiscardDepthStencil(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return noop(c);
}

/** @brief Procs that answer 0: no queue error, no command memory relift tracks, no attached debugger. */
uint64_t nvn_zero(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 0;
    return nvn_ret(c);
}

/** @brief nvnTextureGetTextureAddress(const NVNtexture *): the GPU address of its storage. */
uint64_t nvnh_nvnTextureGetTextureAddress(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_tex_desc dd;
    const nvn_tex_desc *d = nvn_texture_desc(c->x[0], &dd) ? &dd : NULL;
    c->x[0] = d ? nvn_pool_gpu(d->pool) + d->offset : 0;
    return nvn_ret(c);
}
