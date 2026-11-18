/**
 * @file srv_nv_ch.c
 * @brief Channel devices (/dev/nvhost-gpu, -vic, -nvdec, -nvjpg, -nvenc) and
 * the debug devices. A submit to the VIC or NVDEC channel runs its command
 * buffers on the host1x command processor (host1x.c) before it returns;
 * every submit then marks its syncpoint increments complete, so a guest
 * waiting on the returned fence sees the work done.
 *
 * Ioctl numbers, argument layouts and results follow Ryujinx (MIT,
 * Copyright (c) Ryujinx Team and Contributors) NvHostChannelDeviceFile,
 * NvHostGpuDeviceFile, NvHostDbgGpuDeviceFile and NvHostProfGpuDeviceFile;
 * GetErrorInfo and GetErrorNotification follow libnx (ISC, Copyright (c)
 * libnx Authors) nvioctl numbers.
 */
#include "host1x.h"

#include <stdlib.h>
#include <string.h>

#define MODULE_SYNCPOINTS 16u
#define HOST1X_END (1ull << 40)

enum {
    GPFIFO_FENCE_WAIT = 1u << 0,
    GPFIFO_FENCE_INCREMENT = 1u << 1,
    GPFIFO_INCREMENT_WITH_VALUE = 1u << 8
};

enum { PRIORITY_LOW = 50, PRIORITY_MEDIUM = 100, PRIORITY_HIGH = 150 };
enum { GPU_EVENT_BPT_INT = 1, GPU_EVENT_BPT_PAUSE = 2, GPU_EVENT_ERROR_NOTIFIER = 3 };

typedef struct ch_priv {
    uint32_t timeout, submit_timeout, timeslice;
    uint32_t syncpt;
    int32_t as_fd;
    int32_t nvmap_fd;
    uint64_t user_data;
    hos_revent *events[3];
    h1x_stream stream;
} ch_priv;

typedef struct get_parameter {
    uint32_t parameter;
    uint32_t value;
} get_parameter;

typedef struct alloc_gpfifo_ex {
    uint32_t num_entries, num_jobs, flags;
    nv_fence fence;
    uint32_t reserved[3];
} alloc_gpfifo_ex;

typedef struct submit_gpfifo {
    int64_t address;
    int32_t num_entries;
    uint32_t flags;
    nv_fence fence;
} submit_gpfifo;

typedef struct map_cmd_header {
    int32_t num_entries;
    int32_t data_address;
    uint8_t attach_host_ch_das;
    uint8_t padding1;
    int16_t padding2;
} map_cmd_header;

typedef struct cmd_handle {
    int32_t map_handle;
    int32_t map_address;
} cmd_handle;

typedef struct submit_header {
    int32_t cmdbufs, relocs, syncpt_incrs, fences;
} submit_header;

typedef struct cmd_buffer {
    int32_t mem;
    uint32_t offset;
    int32_t words;
} cmd_buffer;

typedef struct syncpt_incr {
    uint32_t id, incrs, reserved[3];
} syncpt_incr;

_Static_assert(sizeof(alloc_gpfifo_ex) == 32, "alloc gpfifo ex");
_Static_assert(sizeof(submit_gpfifo) == 24, "submit gpfifo");
_Static_assert(sizeof(map_cmd_header) == 12, "map command buffer");
_Static_assert(sizeof(cmd_buffer) == 12, "command buffer");
_Static_assert(sizeof(syncpt_incr) == 20, "syncpoint increment");

static uint32_t dev_syncpoints[MODULE_SYNCPOINTS];
static nv_va host1x;
static bool host1x_inited;

void nv_ch_reset(void)
{
    memset(dev_syncpoints, 0, sizeof dev_syncpoints);
    if (host1x_inited)
        nv_va_destroy(&host1x);
    h1x_reset();
    host1x_inited = false;
}

bool nv_host1x_map(nv_map *m, uint64_t *out)
{
    if (m->dma_addr) {
        *out = m->dma_addr;
        return true;
    }
    if (!host1x_inited) {
        nv_va_init(&host1x, NV_PAGE, HOST1X_END);
        host1x_inited = true;
    }
    uint64_t va;
    if (!nv_va_find(&host1x, m->size, 1, NV_PAGE, &va) || va > UINT32_MAX || va + m->size > UINT32_MAX ||
        !nv_va_take(&host1x, va, m->size))
        return false;
    m->dma_addr = va;
    *out = va;
    return true;
}

static ch_priv *priv_of(nv_file *f) { return f->priv; }

static int32_t ch_open(nv_file *f)
{
    ch_priv *c = calloc(1, sizeof *c);
    if (!c)
        return NVE_OUT_OF_MEMORY;
    c->timeout = 3000;
    c->as_fd = -1;
    c->nvmap_fd = -1;
    c->syncpt = nv_sync_alloc(false);
    if (f->dev == &nv_dev_gpu) {
        for (int i = 0; i < 3; i++) {
            c->events[i] = rl_hos_event_create();
            if (!c->events[i]) {
                for (int k = 0; k < i; k++)
                    hos_kobj_unref(&c->events[k]->obj);
                nv_sync_release(c->syncpt);
                free(c);
                return NVE_OUT_OF_MEMORY;
            }
        }
    }
    f->priv = c;
    return NVE_OK;
}

static void ch_close(nv_file *f)
{
    ch_priv *c = priv_of(f);
    for (int i = 0; i < 3; i++)
        if (c->events[i])
            hos_kobj_unref(&c->events[i]->obj);
    nv_sync_release(c->syncpt);
    free(c);
    f->priv = NULL;
}

bool nv_ch_bind(nv_file *ch, int32_t as_fd)
{
    if (ch->dev->open != ch_open)
        return false;
    priv_of(ch)->as_fd = as_fd;
    return true;
}

static int32_t ch_query_event(nv_file *f, uint32_t id, hos_revent **out)
{
    ch_priv *c = priv_of(f);
    if (id < GPU_EVENT_BPT_INT || id > GPU_EVENT_ERROR_NOTIFIER || !c->events[id - 1])
        return NVE_INVALID_INPUT;
    *out = c->events[id - 1];
    return NVE_OK;
}

static int32_t ch_get_syncpoint(nv_file *f, nv_args *a)
{
    (void)f;
    get_parameter *p = (get_parameter *)(void *)a->data;
    if (p->parameter >= MODULE_SYNCPOINTS)
        return NVE_INVALID_INPUT;
    if (dev_syncpoints[p->parameter] == 0)
        dev_syncpoints[p->parameter] = nv_sync_alloc(false);
    p->value = dev_syncpoints[p->parameter];
    return p->value == 0 ? NVE_TRY_AGAIN : NVE_OK;
}

static int32_t ch_get_wait_base(nv_file *f, nv_args *a)
{
    (void)f;
    ((get_parameter *)(void *)a->data)->value = 0;
    return NVE_OK;
}

static int32_t ch_set_submit_timeout(nv_file *f, nv_args *a)
{
    memcpy(&priv_of(f)->submit_timeout, a->data, 4);
    return NVE_OK;
}

static int32_t ch_map_command_buffer(nv_file *f, nv_args *a)
{
    (void)f;
    map_cmd_header h;
    memcpy(&h, a->data, sizeof h);
    if (h.num_entries < 0 || (uint64_t)h.num_entries * sizeof(cmd_handle) > a->size - sizeof h)
        return NVE_INVALID_INPUT;
    for (int32_t i = 0; i < h.num_entries; i++) {
        uint8_t *at = a->data + sizeof h + (size_t)i * sizeof(cmd_handle);
        cmd_handle e;
        memcpy(&e, at, sizeof e);
        nv_map *m = nv_map_get(e.map_handle);
        if (!m)
            return NVE_INVALID_INPUT;
        uint64_t dma;
        e.map_address = nv_host1x_map(m, &dma) ? (int32_t)dma : -1;
        memcpy(at, &e, sizeof e);
    }
    return NVE_OK;
}

static int32_t ch_unmap_command_buffer(nv_file *f, nv_args *a)
{
    (void)f;
    map_cmd_header h;
    memcpy(&h, a->data, sizeof h);
    if (h.num_entries < 0 || (uint64_t)h.num_entries * sizeof(cmd_handle) > a->size - sizeof h)
        return NVE_INVALID_INPUT;
    for (int32_t i = 0; i < h.num_entries; i++) {
        cmd_handle e;
        memcpy(&e, a->data + sizeof h + (size_t)i * sizeof e, sizeof e);
        if (!nv_map_get(e.map_handle))
            return NVE_INVALID_INPUT;
    }
    return NVE_OK;
}

static int32_t ch_submit(nv_file *f, nv_args *a)
{
    submit_header h;
    memcpy(&h, a->data, sizeof h);
    if (h.cmdbufs < 0 || h.relocs < 0 || h.syncpt_incrs < 0 || h.fences < 0)
        return NVE_INVALID_INPUT;
    uint64_t cmds_at = sizeof h;
    uint64_t incrs_at = cmds_at + (uint64_t)h.cmdbufs * sizeof(cmd_buffer) + (uint64_t)h.relocs * (16 + 4);
    uint64_t fences_at = incrs_at + (uint64_t)h.syncpt_incrs * sizeof(syncpt_incr);
    uint64_t end = fences_at + (uint64_t)h.fences * 4;
    if (end > a->size)
        return NVE_INVALID_INPUT;
    for (int32_t i = 0; i < h.cmdbufs; i++) {
        cmd_buffer cb;
        memcpy(&cb, a->data + cmds_at + (size_t)i * sizeof cb, sizeof cb);
        nv_map *m = nv_map_get(cb.mem);
        if (!m || cb.words < 0 || (cb.offset & 3) || (uint64_t)cb.offset + (uint64_t)cb.words * 4 > m->size)
            return NVE_INVALID_INPUT;
    }
    for (int32_t i = 0; i < h.syncpt_incrs; i++) {
        syncpt_incr si;
        memcpy(&si, a->data + incrs_at + (size_t)i * sizeof si, sizeof si);
        if (!nv_sync_valid(si.id))
            return NVE_INVALID_INPUT;
    }
    for (int32_t i = 0; i < h.syncpt_incrs; i++) {
        syncpt_incr si;
        memcpy(&si, a->data + incrs_at + (size_t)i * sizeof si, sizeof si);
        uint32_t threshold = nv_sync_max_add(si.id, si.incrs);
        if (i < h.fences)
            memcpy(a->data + fences_at + (size_t)i * 4, &threshold, 4);
    }
    for (int32_t i = 0; i < h.cmdbufs; i++) {
        cmd_buffer cb;
        memcpy(&cb, a->data + cmds_at + (size_t)i * sizeof cb, sizeof cb);
        nv_map *m = nv_map_get(cb.mem);
        if (m->addr && (f->dev == &nv_dev_vic || f->dev == &nv_dev_nvdec))
            h1x_run(&priv_of(f)->stream, (const uint32_t *)(void *)(hos_mem() + m->addr + cb.offset), (uint32_t)cb.words);
    }
    for (int32_t i = 0; i < h.syncpt_incrs; i++) {
        syncpt_incr si;
        memcpy(&si, a->data + incrs_at + (size_t)i * sizeof si, sizeof si);
        nv_sync_complete(si.id);
    }
    return NVE_OK;
}

static int32_t ch_set_nvmap_fd(nv_file *f, nv_args *a)
{
    memcpy(&priv_of(f)->nvmap_fd, a->data, 4);
    return NVE_OK;
}

static int32_t ch_set_timeout(nv_file *f, nv_args *a)
{
    memcpy(&priv_of(f)->timeout, a->data, 4);
    return NVE_OK;
}

/**
 * @brief The words of GPFIFO entry e: bits 42 to 62 (the GP_ENTRY1 LENGTH
 * field as envytools documents it). An entry of no words fetches nothing.
 */
static uint32_t gp_entry_words(uint64_t e) { return (uint32_t)(e >> 42) & 0x1FFFFF; }

static int32_t gpfifo_submit(ch_priv *c, nv_args *a, submit_gpfifo *h, const uint8_t *entries, uint64_t entry_bytes)
{
    if (h->num_entries < 0 || (uint64_t)h->num_entries * 8 > entry_bytes)
        return NVE_INVALID_INPUT;
    for (int32_t i = 0; i < h->num_entries; i++) {
        uint64_t e;
        memcpy(&e, entries + (size_t)i * 8, 8);
        if (gp_entry_words(e)) {
            a->stop = "Maxwell pushbuffer execution is not implemented: the program uses NVN1";
            return NVE_NOT_SUPPORTED;
        }
    }
    if ((h->flags & GPFIFO_FENCE_WAIT) && (h->flags & GPFIFO_INCREMENT_WITH_VALUE))
        return NVE_INVALID_INPUT;
    h->fence.id = c->syncpt;
    if (h->flags & (GPFIFO_FENCE_INCREMENT | GPFIFO_INCREMENT_WITH_VALUE)) {
        uint32_t count = (h->flags & GPFIFO_FENCE_INCREMENT) ? 2u : 0u;
        if (h->flags & GPFIFO_INCREMENT_WITH_VALUE)
            count += h->fence.value;
        h->fence.value = nv_sync_max_add(h->fence.id, count);
    } else {
        h->fence.value = nv_sync_max(h->fence.id);
    }
    h->flags = 0;
    nv_sync_complete(c->syncpt);
    return NVE_OK;
}

static int32_t ch_submit_gpfifo(nv_file *f, nv_args *a)
{
    return gpfifo_submit(priv_of(f), a, (submit_gpfifo *)(void *)a->data, a->data + sizeof(submit_gpfifo),
                         a->size - sizeof(submit_gpfifo));
}

static int32_t ch_submit_gpfifo_ex(nv_file *f, nv_args *a)
{
    return gpfifo_submit(priv_of(f), a, (submit_gpfifo *)(void *)a->data, a->inl, a->inl_size);
}

static int32_t ch_alloc_obj_ctx(nv_file *f, nv_args *a)
{
    (void)f;
    (void)a;
    return NVE_OK;
}

static int32_t ch_zcull_bind(nv_file *f, nv_args *a)
{
    (void)f;
    (void)a;
    return NVE_OK;
}

static int32_t ch_set_error_notifier(nv_file *f, nv_args *a)
{
    (void)f;
    (void)a;
    return NVE_OK;
}

static int32_t ch_set_priority(nv_file *f, nv_args *a)
{
    uint32_t prio;
    memcpy(&prio, a->data, 4);
    switch (prio) {
    case PRIORITY_LOW: priv_of(f)->timeslice = 1300; break;
    case PRIORITY_MEDIUM: priv_of(f)->timeslice = 2600; break;
    case PRIORITY_HIGH: priv_of(f)->timeslice = 5200; break;
    default: return NVE_INVALID_INPUT;
    }
    return NVE_OK;
}

static int32_t ch_alloc_gpfifo_ex(nv_file *f, nv_args *a)
{
    ch_priv *c = priv_of(f);
    alloc_gpfifo_ex *p = (alloc_gpfifo_ex *)(void *)a->data;
    p->fence.id = c->syncpt;
    p->fence.value = nv_sync_min(c->syncpt);
    return NVE_OK;
}

static int32_t ch_set_timeslice(nv_file *f, nv_args *a)
{
    uint32_t slice;
    memcpy(&slice, a->data, 4);
    if (slice < 1000 || slice > 50000)
        return NVE_INVALID_INPUT;
    priv_of(f)->timeslice = slice;
    return NVE_OK;
}

static int32_t ch_set_user_data(nv_file *f, nv_args *a)
{
    memcpy(&priv_of(f)->user_data, a->data, 8);
    return NVE_OK;
}

static int32_t ch_get_error(nv_file *f, nv_args *a)
{
    (void)f;
    memset(a->data, 0, a->size);
    return NVE_OK;
}

#define CH(type, nr, var, stub, min, name, fn) { (type), (nr), (var), (stub), (min), (name), (fn) }
#define H NV_MAGIC_HOST_CUSTOM
#define G NV_MAGIC_HOST
#define V1 NV_V_IOCTL
#define COMMON_IOCTLS                                                                                                \
    CH(H, 0x01, V1, true, 16, "Submit", ch_submit), CH(H, 0x02, V1, false, 8, "GetSyncpoint", ch_get_syncpoint),      \
        CH(H, 0x03, V1, true, 8, "GetWaitBase", ch_get_wait_base),                                                   \
        CH(H, 0x07, V1, true, 4, "SetSubmitTimeout", ch_set_submit_timeout),                                         \
        CH(H, 0x09, V1, false, 12, "MapCommandBuffer", ch_map_command_buffer),                                       \
        CH(H, 0x0A, V1, true, 12, "UnmapCommandBuffer", ch_unmap_command_buffer),                                    \
        CH(G, 0x01, V1, true, 4, "SetNvMapFd", ch_set_nvmap_fd), CH(G, 0x03, V1, true, 4, "SetTimeout", ch_set_timeout), \
        CH(G, 0x08, V1, true, 24, "SubmitGpfifo", ch_submit_gpfifo),                                                 \
        CH(G, 0x09, V1, true, 16, "AllocObjCtx", ch_alloc_obj_ctx),                                                  \
        CH(G, 0x0B, V1, true, 16, "ZcullBind", ch_zcull_bind),                                                       \
        CH(G, 0x0C, V1, true, 24, "SetErrorNotifier", ch_set_error_notifier),                                        \
        CH(G, 0x0D, V1, true, 4, "SetPriority", ch_set_priority),                                                    \
        CH(G, 0x16, V1, true, 0, "GetErrorInfo", ch_get_error),                                                      \
        CH(G, 0x17, V1, true, 0, "GetErrorNotification", ch_get_error),                                              \
        CH(G, 0x18, V1, true, 32, "AllocGpfifoEx", ch_alloc_gpfifo_ex),                                              \
        CH(G, 0x1A, V1, true, 32, "AllocGpfifoEx2", ch_alloc_gpfifo_ex),                                             \
        CH(G, 0x1D, V1, true, 4, "SetTimeslice", ch_set_timeslice),                                                  \
        CH(NV_MAGIC_GPU, 0x14, V1, true, 8, "SetUserData", ch_set_user_data)

static const nv_ioctl_entry chan_ioctls[] = { COMMON_IOCTLS };
static const nv_ioctl_entry gpu_ioctls[] = {
    COMMON_IOCTLS,
    CH(G, 0x1B, NV_V_IOCTL2, true, 24, "SubmitGpfifoEx", ch_submit_gpfifo_ex),
};
#undef CH
#undef H
#undef G
#undef V1
#undef COMMON_IOCTLS

#define CHANNEL_DEV(sym, devpath, table, events)                                                                     \
    const nv_dev_class sym = {                                                                                       \
        .path = (devpath),                                                                                           \
        .ioctls = (table),                                                                                           \
        .nioctls = sizeof(table) / sizeof((table)[0]),                                                               \
        .open = ch_open,                                                                                             \
        .close = ch_close,                                                                                           \
        .query_event = (events),                                                                                     \
    }

CHANNEL_DEV(nv_dev_gpu, "/dev/nvhost-gpu", gpu_ioctls, ch_query_event);
CHANNEL_DEV(nv_dev_nvdec, "/dev/nvhost-nvdec", chan_ioctls, NULL);
CHANNEL_DEV(nv_dev_vic, "/dev/nvhost-vic", chan_ioctls, NULL);
CHANNEL_DEV(nv_dev_nvjpg, "/dev/nvhost-nvjpg", chan_ioctls, NULL);
CHANNEL_DEV(nv_dev_nvenc, "/dev/nvhost-nvenc", chan_ioctls, NULL);

const nv_dev_class nv_dev_dbg_gpu = { .path = "/dev/nvhost-dbg-gpu", .debug_only = true };
const nv_dev_class nv_dev_prof_gpu = { .path = "/dev/nvhost-prof-gpu", .debug_only = true };
