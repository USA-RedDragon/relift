/**
 * @file nvn_xfb.c
 * @brief NVN transform feedback on the host device, as the NVN2 driver programs the console's GPU (SDK 22 driver
 * sdk.nso, read with runtime/tools/nvn2).
 *
 * The driver's commands:
 *
 * - BindTransformFeedbackBuffer(index, address, size) and BindTransformFeedbackBuffers(first, count, ranges) write
 *   SET_STREAM_OUT_BUFFER_ENABLE/ADDRESS/SIZE of each buffer (a zero address disables it).
 * - BeginTransformFeedback(control) runs macro 0xe06: SET_STREAM_OUTPUT on, SET_STREAM_OUT_BUFFER_LOAD_WRITE_POINTER
 *   of the four buffers 0 (each capture starts at its buffer's first byte), and an inline write of the shadow
 *   SET_STREAM_OUT_CONTROL_STRIDE(0) to the control buffer's word 0.
 * - EndTransformFeedback(control) and PauseTransformFeedback(control) turn the output off and report each buffer's
 *   STREAMING_BYTE_COUNT (one word, sub-reports 0 to 3) to the control buffer's words 1 to 4.
 * - ResumeTransformFeedback(control) loads each buffer's write pointer from words 1 to 4 and turns the output on.
 * - DrawTransformFeedback(mode, control) runs macro 0xe54 with the mode and control words 0 and 1 fetched from
 *   memory: BEGIN(mode), SET_DRAW_AUTO_STRIDE(word 0), DRAW_AUTO(word 1), END: word 1 / word 0 vertices from vertex 0.
 *   SET_DRAW_AUTO_START is never written by the driver; it is taken as 0 (inferred: its reset value is not in the
 *   driver's data).
 *
 * What a program captures is in its GLSLC header; the translation decorates the captured words (interface records
 * NVN_IFACE_XFB give each buffer's stride). Each draw while a capture is on binds the buffers and runs between
 * vkCmdBeginTransformFeedbackEXT and vkCmdEndTransformFeedbackEXT with the queue's counter buffer, which holds each
 * buffer's byte count as the console's write pointer does; the commands that read or write the control buffer wait
 * for the work before them (nvn_vk_flush) and move the counts between it and the counters on the CPU, in command
 * order. The shadow stride is the stride the last capturing program bound gave buffer 0; a program without a
 * transform feedback section leaves it (the driver writes the registers only for a program with one).
 *
 * A host without VK_EXT_transform_feedback takes the memory path the translation builds for a geometry program with a
 * point list output (relift_gpu_nv's io::xfb): the program writes one record per vertex it emits into a scratch buffer,
 * slot (primitive * invocations + invocation) * max vertices + k, and after the draw the replay waits for it and
 * writes the records that were written, in slot order, into each buffer at its byte count: the words whose layout
 * select byte is not 0xff, as the hardware writes them. As the hardware, a vertex that does not fit in a buffer is not
 * written and its count stops (inferred: the driver's data does not say what the GPU does at the end of a buffer;
 * Vulkan's transform feedback, which the other path runs on, writes no vertex past the buffer either). The memory path
 * draws one instance (a geometry program's primitive id restarts with each instance).
 */
#include "nvn_module.h"

#include <string.h>

/** @brief Whether the queue's counter buffer exists (made the first time a capture begins). */
static bool counters(nvn_gfx *st)
{
    return st->xfb.counters.buf || nvn_vk_xfb_counters_new(&st->xfb.counters);
}

/** @brief Notes the stride the program bound to each stage gives buffer 0, as binding it writes the register. */
static int shadow_stride(replay *r)
{
    nvn_gfx *st = r->st;
    for (uint32_t s = 0; s < 5; s++) {
        if (!st->code[s])
            continue;
        const nvn_module *m = nvn_draw_module(r, s);
        if (!m)
            return RL_PENDING;
        if (m->xfb_mask & 1u)
            st->xfb.stride0 = m->xfb_stride[0];
    }
    return RL_OK;
}

/** @brief The guest address of control word k, checked to be mapped. */
static int control_word(replay *r, uint64_t control, uint32_t k, uint64_t *at)
{
    if (!nvn_gpu_mapped(control + 4u * k, 4)) {
        char where[512];
        nvn_gpu_describe(control + 4u * k, where, sizeof where);
        return nvn_vk_stop(r, "a transform feedback control buffer at 0x%llx is in no pool: %s",
                           (unsigned long long)control, where);
    }
    *at = nvn_gpu_guest(control + 4u * k);
    return RL_OK;
}

/** @brief Waits for the work before the command, so the counters and the control buffer hold what it left. */
static int settle(replay *r)
{
    return nvn_vk_flush(r) ? RL_OK : nvn_vk_stop(r, "the host device failed a submit");
}

static int begin(replay *r, uint64_t control)
{
    nvn_gfx *st = r->st;
    int rc = shadow_stride(r);
    if (rc != RL_OK)
        return rc;
    if (!counters(st))
        return nvn_vk_stop(r, "cannot make the transform feedback counters");
    if ((rc = settle(r)) != RL_OK)
        return rc;
    memset(st->xfb.counters.map, 0, 4u * NVN_XFB_BUFFERS);
    uint64_t at;
    if (control) {
        if ((rc = control_word(r, control, 0, &at)) != RL_OK)
            return rc;
        rl_st32(r->mem, at, st->xfb.stride0);
    }
    st->xfb.on = true;
    return RL_OK;
}

/** @brief End and Pause: the output stops and each buffer's byte count goes to control words 1 to 4. */
static int stop(replay *r, uint64_t control)
{
    nvn_gfx *st = r->st;
    st->xfb.on = false;
    if (!control || !st->xfb.counters.buf)
        return RL_OK;
    int rc = settle(r);
    for (uint32_t i = 0; rc == RL_OK && i < NVN_XFB_BUFFERS; i++) {
        uint64_t at;
        if ((rc = control_word(r, control, 1 + i, &at)) == RL_OK)
            rl_st32(r->mem, at, rl_ld32(st->xfb.counters.map, 4u * i));
    }
    return rc;
}

static int resume(replay *r, uint64_t control)
{
    nvn_gfx *st = r->st;
    if (!counters(st))
        return nvn_vk_stop(r, "cannot make the transform feedback counters");
    int rc = settle(r);
    for (uint32_t i = 0; rc == RL_OK && i < NVN_XFB_BUFFERS; i++) {
        uint64_t at;
        if ((rc = control_word(r, control, 1 + i, &at)) == RL_OK)
            rl_st32(st->xfb.counters.map, 4u * i, rl_ld32(r->mem, at));
    }
    if (rc == RL_OK)
        st->xfb.on = true;
    return rc;
}

static int bind(replay *r, uint32_t i, uint64_t addr, uint64_t size)
{
    if (i >= NVN_XFB_BUFFERS)
        return nvn_vk_stop(r, "transform feedback buffer %u, past the %u the GPU has", i, NVN_XFB_BUFFERS);
    r->st->xfb.addr[i] = addr;
    r->st->xfb.size[i] = addr ? size : 0;
    return RL_OK;
}

/** @brief DrawTransformFeedback: control word 1 (buffer 0's byte count) over word 0 (the stride) vertices. */
static int draw_auto(replay *r, uint32_t mode, uint64_t control)
{
    uint64_t w0, w1;
    int rc = control_word(r, control, 0, &w0);
    if (rc == RL_OK)
        rc = control_word(r, control, 1, &w1);
    if (rc == RL_OK)
        rc = settle(r);
    if (rc != RL_OK)
        return rc;
    uint32_t stride = rl_ld32(r->mem, w0), bytes = rl_ld32(r->mem, w1);
    if (!stride)
        return nvn_vk_stop(r, "nvnCommandBufferDrawTransformFeedback with a stride of 0 in its control buffer");
    return nvn_draw_arrays(r, mode, 0, bytes / stride);
}

int nvn_xfb_cmd(replay *r, const nvn_cmd *e)
{
    switch (e->op) {
    case NVN_CMD_BIND_TRANSFORM_FEEDBACK_BUFFER:
        return bind(r, (uint32_t)e->x[1], e->x[2], e->x[3]);
    case NVN_CMD_BIND_TRANSFORM_FEEDBACK_BUFFERS: {
        const uint8_t *ranges = nvn_cmd_bytes(r->s, e->x[3]);
        int rc = RL_OK;
        for (uint32_t k = 0; rc == RL_OK && k < (uint32_t)e->x[2]; k++)
            rc = bind(r, (uint32_t)e->x[1] + k, rl_ld64(ranges, 16u * k), rl_ld64(ranges, 16u * k + 8));
        return rc;
    }
    case NVN_CMD_BEGIN_TRANSFORM_FEEDBACK:
        return begin(r, e->x[1]);
    case NVN_CMD_END_TRANSFORM_FEEDBACK:
    case NVN_CMD_PAUSE_TRANSFORM_FEEDBACK:
        return stop(r, e->x[1]);
    case NVN_CMD_RESUME_TRANSFORM_FEEDBACK:
        return resume(r, e->x[1]);
    case NVN_CMD_DRAW_TRANSFORM_FEEDBACK:
        return draw_auto(r, (uint32_t)e->x[1], e->x[2]);
    default:
        return NVN_UNHANDLED;
    }
}

/** @brief The primitives count vertices of NVN draw mode make; false for a mode the memory path does not take. */
static bool primitives(uint32_t mode, uint32_t count, uint32_t *n)
{
    switch (mode) {
    case 0: *n = count; return true;
    case 1: *n = count / 2; return true;
    case 3: *n = count ? count - 1 : 0; return true;
    case 4: *n = count / 3; return true;
    case 5:
    case 6: *n = count > 2 ? count - 2 : 0; return true;
    default: return false;
    }
}

/** @brief The memory path's preparation: the scratch of slots records, zeroed, and its address. */
static int memory_prepare(replay *r, const nvn_module *last, uint32_t mode, uint32_t count, nvn_xfb_draw *x)
{
    nvn_gfx *st = r->st;
    uint32_t prims;
    if (!primitives(mode, count, &prims))
        return nvn_vk_stop(r, "program %s captures transform feedback from draw primitive %u, which the memory path "
                              "does not take", last->key, mode);
    if (st->inst_count != 1)
        return nvn_vk_stop(r, "program %s captures transform feedback in a draw of %u instances, which the memory path "
                              "does not take", last->key, st->inst_count);
    uint64_t bytes = (uint64_t)prims * last->xfb_slots * last->xfb_record;
    if (!bytes)
        return RL_OK;
    int rc = settle(r);
    if (rc != RL_OK)
        return rc;
    if (st->xfb.scratch.size < bytes) {
        if (st->xfb.scratch.buf)
            nvn_vk_staging_free(&st->xfb.scratch);
        if (!nvn_vk_xfb_scratch_new(&st->xfb.scratch, bytes, &st->xfb.scratch_gpu))
            return nvn_vk_stop(r, "cannot make %llu bytes of transform feedback records", (unsigned long long)bytes);
    }
    memset(st->xfb.scratch.map, 0, bytes);
    x->mem = last;
    x->slots = prims * last->xfb_slots;
    x->scratch = st->xfb.scratch_gpu;
    return RL_OK;
}

void nvn_xfb_pack(const nvn_module *m, const uint8_t *scratch, uint32_t slots, uint8_t *dst[NVN_XFB_BUFFERS],
                  const uint64_t size[NVN_XFB_BUFFERS], uint32_t count[NVN_XFB_BUFFERS])
{
    uint32_t at[NVN_XFB_BUFFERS], n = 0;
    for (uint32_t i = 0; i < NVN_XFB_BUFFERS; i++) {
        at[i] = n;
        if (m->xfb_mask & 1u << i)
            n += m->xfb_stride[i] + 4;
    }
    for (uint32_t s = 0; s < slots; s++) {
        const uint8_t *rec = scratch + (uint64_t)s * m->xfb_record;
        for (uint32_t i = 0; i < NVN_XFB_BUFFERS; i++) {
            uint32_t stride = m->xfb_stride[i];
            if (!(m->xfb_mask & 1u << i) || rl_ld32(rec, at[i] + stride) != 1 || (uint64_t)count[i] + stride > size[i])
                continue;
            for (uint32_t w = 0; w < stride / 4; w++)
                if (w >= 512 || m->xfb_select[i][w] != 0xff)
                    memcpy(dst[i] + count[i] + 4u * w, rec + at[i] + 4u * w, 4);
            count[i] += stride;
        }
    }
}

int nvn_xfb_draw_finish(replay *r, const nvn_xfb_draw *x)
{
    nvn_gfx *st = r->st;
    int rc = settle(r);
    if (rc != RL_OK)
        return rc;
    uint8_t *dst[NVN_XFB_BUFFERS] = { NULL };
    uint32_t count[NVN_XFB_BUFFERS];
    for (uint32_t i = 0; i < NVN_XFB_BUFFERS; i++) {
        count[i] = rl_ld32(st->xfb.counters.map, 4u * i);
        if (x->mem->xfb_mask & 1u << i)
            dst[i] = r->mem + nvn_gpu_guest(st->xfb.addr[i]);
    }
    nvn_xfb_pack(x->mem, st->xfb.scratch.map, x->slots, dst, st->xfb.size, count);
    for (uint32_t i = 0; i < NVN_XFB_BUFFERS; i++)
        rl_st32(st->xfb.counters.map, 4u * i, count[i]);
    return RL_OK;
}

int nvn_xfb_draw_prepare(replay *r, const nvn_module *last, uint32_t mode, uint32_t count, nvn_xfb_draw *x)
{
    nvn_gfx *st = r->st;
    memset(x, 0, sizeof *x);
    if (last && last->xfb_mask & 1u)
        st->xfb.stride0 = last->xfb_stride[0];
    if (!st->xfb.on || !last || !last->xfb_mask)
        return RL_OK;
    if (last->xfb_record) {
        for (uint32_t i = 0; i < NVN_XFB_BUFFERS; i++)
            if (last->xfb_mask & 1u << i && (!st->xfb.addr[i] || !nvn_gpu_mapped(st->xfb.addr[i], st->xfb.size[i])))
                return nvn_vk_stop(r, "program %s captures to transform feedback buffer %u, which is in no pool",
                                   last->key, i);
        return memory_prepare(r, last, mode, count, x);
    }
    if (!g.transform_feedback)
        return nvn_vk_stop(r, "program %s captures transform feedback, which the host device does not have",
                           last->key);
    for (uint32_t i = 0; i < NVN_XFB_BUFFERS; i++) {
        if (!(last->xfb_mask & 1u << i))
            continue;
        if (!st->xfb.addr[i] || !st->xfb.size[i])
            return nvn_vk_stop(r, "program %s captures to transform feedback buffer %u, which is not bound", last->key,
                               i);
        uint64_t guest = nvn_gpu_guest(st->xfb.addr[i]);
        void *buf;
        uint64_t off;
        if (!nvn_gpu_mapped(st->xfb.addr[i], st->xfb.size[i]) ||
            !nvn_guest_buffer(guest, st->xfb.size[i], &buf, &off))
            return nvn_vk_stop(r, "transform feedback buffer %u (0x%llx, %llu bytes) is in no one pool", i,
                               (unsigned long long)st->xfb.addr[i], (unsigned long long)st->xfb.size[i]);
        x->buf[i] = (VkBuffer)buf;
        x->off[i] = off;
        x->size[i] = st->xfb.size[i];
        nvn_vk_mark_write_range(guest, st->xfb.size[i]);
        x->n = i + 1;
    }
    if (x->n) {
        nvn_vk_mark_write();
        for (uint32_t i = 0; i < x->n; i++)
            if (!x->buf[i]) {
                x->buf[i] = x->buf[0];
                x->off[i] = x->off[0];
                x->size[i] = 0;
            }
        x->counters = st->xfb.counters.buf;
    }
    return RL_OK;
}

void nvn_xfb_draw_begin(VkCommandBuffer cb, const nvn_xfb_draw *x)
{
    if (!x->n)
        return;
    VkBuffer cbufs[NVN_XFB_BUFFERS];
    VkDeviceSize coffs[NVN_XFB_BUFFERS];
    for (uint32_t i = 0; i < x->n; i++) {
        cbufs[i] = x->counters;
        coffs[i] = 4u * i;
    }
    g.bind_xfb(cb, 0, x->n, x->buf, x->off, x->size);
    g.begin_xfb(cb, 0, x->n, cbufs, coffs);
}

void nvn_xfb_draw_end(VkCommandBuffer cb, const nvn_xfb_draw *x)
{
    if (!x->n)
        return;
    VkBuffer cbufs[NVN_XFB_BUFFERS];
    VkDeviceSize coffs[NVN_XFB_BUFFERS];
    for (uint32_t i = 0; i < x->n; i++) {
        cbufs[i] = x->counters;
        coffs[i] = 4u * i;
    }
    g.end_xfb(cb, 0, x->n, cbufs, coffs);
}
