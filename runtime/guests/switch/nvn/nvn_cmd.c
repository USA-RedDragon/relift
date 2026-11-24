/**
 * @file nvn_cmd.c
 * @brief NVN2 command buffers: recording into relift's own command lists and
 * handing them to the queue at submit.
 *
 * A command buffer records each command proc as one entry: which proc and
 * its argument registers (x0 to x7, d0 to d7). The game's command and
 * control memory is accounted for (the game sizes and refills it through
 * the Get*Memory* procs and the memory callback) but never written.
 * EndRecording turns the entries recorded since BeginRecording into a
 * command set and returns its handle.
 */
#include "nvn_priv.h"

#include <pthread.h>
#include <stdlib.h>

/** @brief Top bits of a command handle relift returns; the low bits index the set table. */
#define HANDLE_TAG 0x524c000000000000ull
#define HANDLE_INDEX(h) ((uint32_t)((h) & 0xffffffffu))
/** @brief Bytes of command memory one recorded command counts as in the used-memory getters. */
#define NVN_CMD_BYTES 16u

typedef struct nvn_cmdbuf {
    nvn_obj o;
    uint64_t device;
    uint64_t cmd_pool, cmd_offset, cmd_size;
    uint64_t ctl_memory, ctl_size;
    uint64_t callback, callback_data;
    uint32_t row_stride, image_stride, recording_mode;
    bool recording;
    /** @brief The commands the last recording ended with; the used-memory getters answer from it after EndRecording. */
    uint32_t ended_n;
    nvn_cmd *cmds;
    uint32_t n, cap;
    uint8_t *bytes;
    uint32_t nbytes, capbytes;
} nvn_cmdbuf;

static struct {
    pthread_mutex_t lock;
    nvn_cmdset **sets;
    uint32_t n, cap;
} sets = { .lock = PTHREAD_MUTEX_INITIALIZER };

static nvn_cmdbuf *cb_of(rl_ctx *c) { return (nvn_cmdbuf *)nvn_obj_get(c->x[0], NVN_CMDBUF); }

static void cb_drop(nvn_obj *o)
{
    free(((nvn_cmdbuf *)o)->cmds);
    free(((nvn_cmdbuf *)o)->bytes);
}

/** @brief An argument of a recorded command that points at guest bytes the command buffer copies when it records. */
typedef struct nvn_snap_rule {
    uint32_t op;
    /** @brief The argument register holding the pointer. */
    uint8_t reg;
    /** @brief The register holding the element count, or 0xff for one element. */
    uint8_t count_reg;
    /** @brief Bytes per element. */
    uint16_t elem;
} nvn_snap_rule;

#define ONE 0xffu
static const nvn_snap_rule snap_rules[] = {
    { NVN_CMD_CLEAR_COLOR, 2, ONE, 16 },
    { NVN_CMD_CLEAR_COLORI, 2, ONE, 16 },
    { NVN_CMD_CLEAR_COLORUI, 2, ONE, 16 },
    { NVN_CMD_CLEAR_TEXTURE, 2, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_CLEAR_TEXTURE, 3, ONE, sizeof(nvn_region) },
    { NVN_CMD_CLEAR_TEXTURE, 4, ONE, 16 },
    { NVN_CMD_CLEAR_TEXTUREI, 2, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_CLEAR_TEXTUREI, 3, ONE, sizeof(nvn_region) },
    { NVN_CMD_CLEAR_TEXTUREI, 4, ONE, 16 },
    { NVN_CMD_CLEAR_TEXTUREUI, 2, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_CLEAR_TEXTUREUI, 3, ONE, sizeof(nvn_region) },
    { NVN_CMD_CLEAR_TEXTUREUI, 4, ONE, 16 },
    { NVN_CMD_CALL_COMMANDS, 2, 1, 8 },
    { NVN_CMD_BIND_UNIFORM_BUFFERS, 4, 3, 16 },
    { NVN_CMD_BIND_STORAGE_BUFFERS, 4, 3, 16 },
    { NVN_CMD_BIND_VERTEX_BUFFERS, 3, 2, 16 },
    { NVN_CMD_BIND_TRANSFORM_FEEDBACK_BUFFERS, 3, 2, 16 },
    { NVN_CMD_BIND_TEXTURES, 4, 3, 8 },
    { NVN_CMD_BIND_SEPARATE_TEXTURES, 4, 3, 8 },
    { NVN_CMD_BIND_SEPARATE_SAMPLERS, 4, 3, 8 },
    { NVN_CMD_BIND_IMAGES, 4, 3, 8 },
    { NVN_CMD_SET_VIEWPORTS, 3, 2, 16 },
    { NVN_CMD_UPDATE_UNIFORM_BUFFER, 5, 4, 1 },
    { NVN_CMD_PUSH_COMPUTE_CONSTANTS, 4, 3, 1 },
    { NVN_CMD_PUSH_GRAPHICS_CONSTANTS, 4, 3, 1 },
    { NVN_CMD_SET_SCISSORS, 3, 2, 16 },
    { NVN_CMD_SET_DEPTH_RANGES, 3, 2, 8 },
    { NVN_CMD_SET_VIEWPORT_SWIZZLES, 3, 2, 16 },
    { NVN_CMD_SET_INNER_TESSELLATION_LEVELS, 1, ONE, 8 },
    { NVN_CMD_SET_OUTER_TESSELLATION_LEVELS, 1, ONE, 16 },
    { NVN_CMD_BIND_COVERAGE_MODULATION_TABLE, 1, ONE, 64 },
    { NVN_CMD_SHADING_RATE_IMAGE_PALETTE, 3, 2, 4 },
    { NVN_CMD_SET_RENDER_TARGETS, 2, 1, 8 },
    { NVN_CMD_SET_RENDER_TARGETS, 3, 1, 8 },
    { NVN_CMD_SET_RENDER_TARGETS, 5, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_BIND_BLEND_STATE, 1, ONE, 8 },
    { NVN_CMD_BIND_COLOR_STATE, 1, ONE, 4 },
    { NVN_CMD_BIND_CHANNEL_MASK_STATE, 1, ONE, 4 },
    { NVN_CMD_BIND_DEPTH_STENCIL_STATE, 1, ONE, 8 },
    { NVN_CMD_BIND_MULTISAMPLE_STATE, 1, ONE, 24 },
    { NVN_CMD_BIND_POLYGON_STATE, 1, ONE, 4 },
    { NVN_CMD_BIND_VERTEX_ATTRIB_STATE, 2, 1, 4 },
    { NVN_CMD_BIND_VERTEX_STREAM_STATE, 2, 1, 8 },
    { NVN_CMD_DOWNSAMPLE_TEXTURE_VIEW, 2, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_DOWNSAMPLE_TEXTURE_VIEW, 4, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_TILED_DOWNSAMPLE_TEXTURE_VIEW, 2, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_TILED_DOWNSAMPLE_TEXTURE_VIEW, 4, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_COPY_TEXTURE_TO_TEXTURE, 2, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_COPY_TEXTURE_TO_TEXTURE, 3, ONE, sizeof(nvn_region) },
    { NVN_CMD_COPY_TEXTURE_TO_TEXTURE, 5, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_COPY_TEXTURE_TO_TEXTURE, 6, ONE, sizeof(nvn_region) },
    { NVN_CMD_COPY_BUFFER_TO_TEXTURE, 3, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_COPY_BUFFER_TO_TEXTURE, 4, ONE, sizeof(nvn_region) },
    { NVN_CMD_COPY_TEXTURE_TO_BUFFER, 2, ONE, NVN_VIEW_BYTES },
    { NVN_CMD_COPY_TEXTURE_TO_BUFFER, 3, ONE, sizeof(nvn_region) },
    { NVN_CMD_EVALUATE_DLSS, 2, ONE, NVN_DLSS_PARAMS_BYTES },
    { NVN_CMD_CUDA_LAUNCH, 3, 4, 1 },
};
#undef ONE

/** @brief Copies n guest bytes at a into b's byte store; the value a recorded command keeps instead of a. */
static bool snap(nvn_cmdbuf *b, const uint8_t *mem, uint64_t a, uint64_t n, uint64_t *out)
{
    if (!a) {
        *out = 0;
        return true;
    }
    n = (n + 7) & ~7ull;
    if (n > 0x10000 || b->nbytes + n > 0x7fffffffu)
        return false;
    if (b->nbytes + n > b->capbytes) {
        uint32_t cap = b->capbytes ? b->capbytes : 1024;
        while (cap < b->nbytes + n)
            cap *= 2;
        uint8_t *nb = realloc(b->bytes, cap);
        if (!nb)
            return false;
        b->bytes = nb;
        b->capbytes = cap;
    }
    memcpy(b->bytes + b->nbytes, mem + a, n);
    *out = NVN_SNAP | b->nbytes;
    b->nbytes += (uint32_t)n;
    return true;
}

const void *nvn_cmd_bytes(const nvn_cmdset *s, uint64_t v)
{
    if (!(v & NVN_SNAP))
        return NULL;
    uint64_t off = v & ~NVN_SNAP;
    return off < s->nbytes ? s->bytes + off : NULL;
}

static uint64_t not_cb(rl_ctx *c, const char *proc)
{
    return (uint64_t)rl_guest_fatal(c, "nvn", "%s on 0x%llx, not a command buffer", proc, (unsigned long long)c->x[0]);
}

/** @brief nvnCommandBufferInitialize(NVNcommandBuffer *, NVNdevice *): NVNboolean. */
uint64_t nvnh_nvnCommandBufferInitialize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = (nvn_cmdbuf *)nvn_obj_new(c->x[0], NVN_CMDBUF, sizeof *b);
    if (b) {
        b->device = c->x[1];
        b->o.drop = cb_drop;
    }
    c->x[0] = nvn_obj_publish((nvn_obj *)b) != NULL;
    return nvn_ret(c);
}

/** @brief nvnCommandBufferFinalize(NVNcommandBuffer *). */
uint64_t nvnh_nvnCommandBufferFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_CMDBUF));
    return nvn_ret(c);
}

/** @brief nvnCommandBufferAddCommandMemory(NVNcommandBuffer *, const NVNmemoryPool *, ptrdiff_t offset, size_t size). */
uint64_t nvnh_nvnCommandBufferAddCommandMemory(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferAddCommandMemory");
    b->cmd_pool = c->x[1];
    b->cmd_offset = c->x[2];
    b->cmd_size = c->x[3];
    return nvn_ret(c);
}

/** @brief nvnCommandBufferAddControlMemory(NVNcommandBuffer *, void *memory, size_t size). */
uint64_t nvnh_nvnCommandBufferAddControlMemory(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferAddControlMemory");
    b->ctl_memory = c->x[1];
    b->ctl_size = c->x[2];
    return nvn_ret(c);
}

/** @brief nvnCommandBufferGetCommandMemorySize(const NVNcommandBuffer *). */
uint64_t nvnh_nvnCommandBufferGetCommandMemorySize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    c->x[0] = b ? b->cmd_size : 0;
    return nvn_ret(c);
}

/**
 * @brief The command memory the recorded commands stand for, in bytes. The driver (SDK 22.2.8) answers Used as its
 * write pointer less the start of the command memory, and Free as the end less the write pointer, so Used plus Free
 * is the memory the game added. relift writes none of that memory, but the game's end-of-pass code asks how much a
 * buffer used after EndRecording and submits only a buffer that used some, so a buffer with commands must answer
 * nonzero. Inferred: each command is counted as 16 bytes, where the driver's own encodings run from 4 bytes (a
 * single method word) to 96 (a draw record in control memory); the total stops at the memory the game added, and
 * only grows as commands are recorded.
 */
static uint64_t cmd_used(const nvn_cmdbuf *b)
{
    uint64_t used = (uint64_t)(b->recording ? b->n : b->ended_n) * NVN_CMD_BYTES;
    return b->cmd_size && used > b->cmd_size ? b->cmd_size : used;
}

/** @brief nvnCommandBufferGetCommandMemoryUsed(const NVNcommandBuffer *). */
uint64_t nvnh_nvnCommandBufferGetCommandMemoryUsed(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    c->x[0] = b ? cmd_used(b) : 0;
    return nvn_ret(c);
}

/** @brief nvnCommandBufferGetCommandMemoryFree(const NVNcommandBuffer *): the memory added, less what the commands use. */
uint64_t nvnh_nvnCommandBufferGetCommandMemoryFree(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    c->x[0] = b ? b->cmd_size - (b->cmd_size ? cmd_used(b) : 0) : 0;
    return nvn_ret(c);
}

/** @brief nvnCommandBufferGetControlMemorySize(const NVNcommandBuffer *). */
uint64_t nvnh_nvnCommandBufferGetControlMemorySize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    c->x[0] = b ? b->ctl_size : 0;
    return nvn_ret(c);
}

/** @brief nvnCommandBufferGetControlMemoryUsed(const NVNcommandBuffer *): relift writes none of it. */
uint64_t nvnh_nvnCommandBufferGetControlMemoryUsed(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 0;
    return nvn_ret(c);
}

/** @brief nvnCommandBufferGetControlMemoryFree(const NVNcommandBuffer *). */
uint64_t nvnh_nvnCommandBufferGetControlMemoryFree(rl_ctx *c, uint8_t *mem)
{
    return nvnh_nvnCommandBufferGetControlMemorySize(c, mem);
}

/** @brief nvnCommandBufferSetMemoryCallback(NVNcommandBuffer *, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC). */
uint64_t nvnh_nvnCommandBufferSetMemoryCallback(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferSetMemoryCallback");
    b->callback = c->x[1];
    return nvn_ret(c);
}

/** @brief nvnCommandBufferSetMemoryCallbackData(NVNcommandBuffer *, void *). */
uint64_t nvnh_nvnCommandBufferSetMemoryCallbackData(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferSetMemoryCallbackData");
    b->callback_data = c->x[1];
    return nvn_ret(c);
}

/**
 * @brief nvnCommandBufferGetMemoryCallback(const NVNcommandBuffer *): the callback nvnCommandBufferSetMemoryCallback
 * gave. The driver returns a word of the command buffer object (a one-load getter in every NVN1 and NVN2 driver
 * read) that its own setter fills; the layer keeps the callback in its record of the command buffer instead, so the
 * driver's word is never written. 0 before any is set, as in a driver-initialized command buffer.
 */
uint64_t nvnh_nvnCommandBufferGetMemoryCallback(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferGetMemoryCallback");
    c->x[0] = b->callback;
    return nvn_ret(c);
}

/** @brief nvnCommandBufferGetMemoryCallbackData(const NVNcommandBuffer *): the pointer nvnCommandBufferSetMemoryCallbackData gave. */
uint64_t nvnh_nvnCommandBufferGetMemoryCallbackData(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferGetMemoryCallbackData");
    c->x[0] = b->callback_data;
    return nvn_ret(c);
}

/**
 * @brief nvnCommandBufferIsRecording(const NVNcommandBuffer *): NVNboolean. The driver returns a byte its own
 * nvnCommandBufferBeginRecording sets and nvnCommandBufferEndRecording clears; both are the layer's, which keeps
 * the state in its record of the command buffer.
 */
uint64_t nvnh_nvnCommandBufferIsRecording(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferIsRecording");
    c->x[0] = b->recording;
    return nvn_ret(c);
}

/**
 * @brief nvnCommandBufferSetCopyRowStride(NVNcommandBuffer *, ptrdiff_t):
 * the bytes between rows of the buffer side of later buffer and texture
 * copies, 0 for rows packed tight. As in the driver, where it is one store
 * into the command buffer object that BeginRecording leaves alone, it is
 * state of the command buffer, not a command: each copy takes the value
 * current when it is recorded.
 */
uint64_t nvnh_nvnCommandBufferSetCopyRowStride(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferSetCopyRowStride");
    b->row_stride = (uint32_t)c->x[1];
    return nvn_ret(c);
}

/** @brief nvnCommandBufferSetCopyImageStride(NVNcommandBuffer *, ptrdiff_t): as SetCopyRowStride, between layers. */
uint64_t nvnh_nvnCommandBufferSetCopyImageStride(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferSetCopyImageStride");
    b->image_stride = (uint32_t)c->x[1];
    return nvn_ret(c);
}

/** @brief nvnCommandBufferGetCopyRowStride(const NVNcommandBuffer *). */
uint64_t nvnh_nvnCommandBufferGetCopyRowStride(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferGetCopyRowStride");
    c->x[0] = (uint64_t)(int64_t)(int32_t)b->row_stride;
    return nvn_ret(c);
}

/** @brief nvnCommandBufferGetCopyImageStride(const NVNcommandBuffer *). */
uint64_t nvnh_nvnCommandBufferGetCopyImageStride(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferGetCopyImageStride");
    c->x[0] = (uint64_t)(int64_t)(int32_t)b->image_stride;
    return nvn_ret(c);
}

/** @brief nvnCommandBufferSetRecordingMode(NVNcommandBuffer *, mode). */
uint64_t nvnh_nvnCommandBufferSetRecordingMode(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferSetRecordingMode");
    b->recording_mode = (uint32_t)c->x[1];
    return nvn_ret(c);
}

/** @brief nvnCommandBufferBeginRecording(NVNcommandBuffer *). */
uint64_t nvnh_nvnCommandBufferBeginRecording(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferBeginRecording");
    b->recording = true;
    b->ended_n = 0;
    b->n = 0;
    b->nbytes = 0;
    return nvn_ret(c);
}

/** @brief nvnCommandBufferEndRecording(NVNcommandBuffer *): NVNcommandHandle. */
uint64_t nvnh_nvnCommandBufferEndRecording(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferEndRecording");
    nvn_cmdset *s = malloc(sizeof *s + (size_t)b->n * sizeof(nvn_cmd) + b->nbytes);
    if (!s)
        return (uint64_t)rl_guest_fatal(c, "nvn", "out of memory for a command set of %u commands", b->n);
    s->n = b->n;
    s->nbytes = b->nbytes;
    s->bytes = (uint8_t *)(s->cmds + b->n);
    if (b->n)
        memcpy(s->cmds, b->cmds, (size_t)b->n * sizeof(nvn_cmd));
    if (b->nbytes)
        memcpy(s->bytes, b->bytes, b->nbytes);
    b->recording = false;
    b->ended_n = b->n;
    b->n = 0;
    b->nbytes = 0;
    pthread_mutex_lock(&sets.lock);
    if (sets.n == sets.cap) {
        uint32_t cap = sets.cap ? sets.cap * 2 : 256;
        nvn_cmdset **ns = realloc(sets.sets, cap * sizeof *ns);
        if (!ns) {
            pthread_mutex_unlock(&sets.lock);
            free(s);
            return (uint64_t)rl_guest_fatal(c, "nvn", "out of memory for command sets");
        }
        sets.sets = ns;
        sets.cap = cap;
    }
    uint32_t idx = sets.n++;
    sets.sets[idx] = s;
    pthread_mutex_unlock(&sets.lock);
    c->x[0] = HANDLE_TAG | idx;
    return nvn_ret(c);
}

uint64_t nvn_cmd_record(rl_ctx *c, uint8_t *mem, uint32_t op)
{
    if (op == NVN_CMD_EVALUATE_DLSS && nvn_dlss_game())
        return nvn_dlss_evaluate(c, mem);
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return (uint64_t)rl_guest_fatal(c, "nvn", "%s on 0x%llx, not a command buffer", nvn_cmd_name(op),
                                        (unsigned long long)c->x[0]);
    if (b->n == b->cap) {
        uint32_t cap = b->cap ? b->cap * 2 : 64;
        nvn_cmd *nc = realloc(b->cmds, cap * sizeof *nc);
        if (!nc)
            return (uint64_t)rl_guest_fatal(c, "nvn", "out of memory recording %s", nvn_cmd_name(op));
        b->cmds = nc;
        b->cap = cap;
    }
    nvn_cmd *e = &b->cmds[b->n++];
    e->op = op;
    nvn_stats_cmd(op, NVN_STATS_REC);
    for (unsigned i = 0; i < 8; i++) {
        e->x[i] = c->x[i];
        e->d[i] = c->v[i].d[0];
    }
    for (size_t i = 0; i < sizeof snap_rules / sizeof snap_rules[0]; i++) {
        const nvn_snap_rule *r = &snap_rules[i];
        if (r->op != op)
            continue;
        uint64_t count = r->count_reg == 0xffu ? 1 : (uint64_t)(uint32_t)c->x[r->count_reg];
        if (!snap(b, mem, c->x[r->reg], count * r->elem, &e->x[r->reg]))
            return (uint64_t)rl_guest_fatal(c, "nvn", "%s: cannot keep %llu bytes of argument %u", nvn_cmd_name(op),
                                            (unsigned long long)(count * r->elem), r->reg);
    }
    if (op == NVN_CMD_WAIT_EVENT || op == NVN_CMD_SIGNAL_EVENT)
        e->x[1] = nvn_event_word(c->x[1]);
    if (op == NVN_CMD_COPY_BUFFER_TO_TEXTURE || op == NVN_CMD_COPY_TEXTURE_TO_BUFFER) {
        e->x[NVN_COPY_ROW_STRIDE_ARG] = b->row_stride;
        e->x[NVN_COPY_IMAGE_STRIDE_ARG] = b->image_stride;
    }
    if (op == NVN_CMD_SET_RENDER_TARGETS && (e->x[3] & NVN_SNAP))
        for (uint32_t i = 0; i < (uint32_t)c->x[1]; i++) {
            uint64_t at = (e->x[3] & ~NVN_SNAP) + 8u * i, view, kept;
            memcpy(&view, b->bytes + at, 8);
            if (!snap(b, mem, view, NVN_VIEW_BYTES, &kept))
                return (uint64_t)rl_guest_fatal(c, "nvn", "%s: cannot keep texture view %u", nvn_cmd_name(op), i);
            memcpy(b->bytes + at, &kept, 8);
        }
    if (op == NVN_CMD_EVALUATE_DLSS && (e->x[2] & NVN_SNAP))
        for (uint32_t i = 0; i < NVN_DLSS_PARAM_TEXTURES; i++) {
            uint64_t at = (e->x[2] & ~NVN_SNAP) + nvn_dlss_texture_offsets[i] + 8u, view, kept;
            memcpy(&view, b->bytes + at, 8);
            if (!snap(b, mem, view, NVN_VIEW_BYTES, &kept))
                return (uint64_t)rl_guest_fatal(c, "nvn", "%s: cannot keep texture view %u", nvn_cmd_name(op), i);
            memcpy(b->bytes + at, &kept, 8);
        }
    return nvn_ret(c);
}

const nvn_cmdset *nvn_cmdset_of(uint64_t handle)
{
    if ((handle & ~0xffffffffull) != HANDLE_TAG)
        return NULL;
    pthread_mutex_lock(&sets.lock);
    const nvn_cmdset *s = HANDLE_INDEX(handle) < sets.n ? sets.sets[HANDLE_INDEX(handle)] : NULL;
    pthread_mutex_unlock(&sets.lock);
    return s;
}

void nvn_cmdset_free(uint64_t handle)
{
    if ((handle & ~0xffffffffull) != HANDLE_TAG)
        return;
    nvn_cmdset *s = NULL;
    pthread_mutex_lock(&sets.lock);
    if (HANDLE_INDEX(handle) < sets.n) {
        s = sets.sets[HANDLE_INDEX(handle)];
        sets.sets[HANDLE_INDEX(handle)] = NULL;
    }
    pthread_mutex_unlock(&sets.lock);
    free(s);
}

static const char *const cmd_names[] = {
#define NVN_CMD_NAME(op, proc) [op] = #proc,
    NVN_RECORDED(NVN_CMD_NAME)
#undef NVN_CMD_NAME
    [NVN_CMD_CUDA_LAUNCH] = "cuLaunchKernel (console DLSS)",
};

const char *nvn_cmd_name(uint32_t op)
{
    if (op < sizeof cmd_names / sizeof cmd_names[0] && cmd_names[op])
        return cmd_names[op];
    const char *n = op >= NVN_CMD_COUNT ? nvn_slot_name(op - NVN_CMD_COUNT) : NULL;
    return n ? n : "?";
}

#define NVN_RECORDER(op, proc)                                                                                         \
    uint64_t nvnh_##proc(rl_ctx *c, uint8_t *mem) { return nvn_cmd_record(c, mem, op); }
NVN_RECORDED(NVN_RECORDER)
#undef NVN_RECORDER

int nvn_cmd_submit(rl_ctx *c, uint8_t *mem, uint64_t queue, uint64_t handle)
{
    const nvn_cmdset *s = nvn_cmdset_of(handle);
    if (!s)
        return rl_guest_fatal(c, "nvn", "queue 0x%llx: 0x%llx is not a command handle relift made",
                              (unsigned long long)queue, (unsigned long long)handle);
    if (rl_cfg()->trace_os)
        for (uint32_t i = 0; i < s->n; i++)
            rl_log(RL_LOG_INFO, "nvn", "  submit %s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)", nvn_cmd_name(s->cmds[i].op),
                   (unsigned long long)s->cmds[i].x[1], (unsigned long long)s->cmds[i].x[2],
                   (unsigned long long)s->cmds[i].x[3], (unsigned long long)s->cmds[i].x[4],
                   (unsigned long long)s->cmds[i].x[5]);
    return nvn_vk_run(c, mem, queue, s);
}
