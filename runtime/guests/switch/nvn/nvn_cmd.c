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

typedef struct nvn_cmdbuf {
    nvn_obj o;
    uint64_t device;
    uint64_t cmd_pool, cmd_offset, cmd_size;
    uint64_t ctl_memory, ctl_size;
    uint64_t callback, callback_data;
    uint32_t row_stride, image_stride, recording_mode;
    bool recording;
    nvn_cmd *cmds;
    uint32_t n, cap;
} nvn_cmdbuf;

static struct {
    pthread_mutex_t lock;
    nvn_cmdset **sets;
    uint32_t n, cap;
} sets = { .lock = PTHREAD_MUTEX_INITIALIZER };

static nvn_cmdbuf *cb_of(rl_ctx *c) { return (nvn_cmdbuf *)nvn_obj_get(c->x[0], NVN_CMDBUF); }

static void cb_drop(nvn_obj *o) { free(((nvn_cmdbuf *)o)->cmds); }

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
    c->x[0] = b != NULL;
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

/** @brief nvnCommandBufferGetCommandMemoryUsed(const NVNcommandBuffer *): relift writes none of it. */
uint64_t nvnh_nvnCommandBufferGetCommandMemoryUsed(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 0;
    return nvn_ret(c);
}

/** @brief nvnCommandBufferGetCommandMemoryFree(const NVNcommandBuffer *). */
uint64_t nvnh_nvnCommandBufferGetCommandMemoryFree(rl_ctx *c, uint8_t *mem)
{
    return nvnh_nvnCommandBufferGetCommandMemorySize(c, mem);
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
    b->n = 0;
    return nvn_ret(c);
}

/** @brief nvnCommandBufferEndRecording(NVNcommandBuffer *): NVNcommandHandle. */
uint64_t nvnh_nvnCommandBufferEndRecording(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_cmdbuf *b = cb_of(c);
    if (!b)
        return not_cb(c, "nvnCommandBufferEndRecording");
    nvn_cmdset *s = malloc(sizeof *s + (size_t)b->n * sizeof(nvn_cmd));
    if (!s)
        return (uint64_t)rl_guest_fatal(c, "nvn", "out of memory for a command set of %u commands", b->n);
    s->n = b->n;
    if (b->n)
        memcpy(s->cmds, b->cmds, (size_t)b->n * sizeof(nvn_cmd));
    b->recording = false;
    b->n = 0;
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
    (void)mem;
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
    for (unsigned i = 0; i < 8; i++) {
        e->x[i] = c->x[i];
        e->d[i] = c->v[i].d[0];
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
    (void)mem;
    const nvn_cmdset *s = nvn_cmdset_of(handle);
    if (!s)
        return rl_guest_fatal(c, "nvn", "queue 0x%llx: 0x%llx is not a command handle relift made",
                              (unsigned long long)queue, (unsigned long long)handle);
    if (rl_cfg()->trace_os)
        for (uint32_t i = 0; i < s->n; i++)
            rl_log(RL_LOG_INFO, "nvn", "  submit %s", nvn_cmd_name(s->cmds[i].op));
    return RL_OK;
}

int nvn_present(rl_ctx *c, uint8_t *mem, uint64_t queue, uint64_t texture)
{
    (void)c;
    (void)mem;
    (void)queue;
    (void)texture;
    return RL_OK;
}
