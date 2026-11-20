/**
 * @file nvn_counters.c
 * @brief NVN GPU counters on the host device.
 *
 * nvnCommandBufferResetCounter zeroes a counter and nvnCommandBufferReportCounter writes its value and a timestamp
 * (16 bytes) to a GPU address. The driver's ResetCounter writes only a CLEAR_REPORT_VALUE method (SDK 22.2.0): the
 * counter restarts from zero and no memory changes. The timestamp (type 0) is the guest clock. Every other type
 * counts what the draws since its reset did: once a queue has reset one, each of its draws runs inside an occlusion
 * query and a pipeline statistics query, and a report reads the batch back and adds each statistic to the counters it
 * feeds. The timestamp at the top of the pipe (type 16) is the same clock as type 0: relift runs a command set's work in order, so the top and the bottom of the pipe are one moment. The statistic of each NVN counter is the one Vulkan names alike: samples passed, input assembly vertices and
 * primitives, vertex, tessellation control, tessellation evaluation, geometry and fragment invocations, geometry
 * primitives, and the clipper's input and output primitives. Tessellation evaluation primitives have no host
 * statistic, so a counter fed by a tessellated draw stops when it is reported; primitives generated are the geometry
 * primitives, or the input primitives with no geometry stage. Transform feedback primitives written are 0 here
 * because a draw with transform feedback stops earlier. Inferred: a draw with no geometry stage generates what it
 * assembles; the console's counters are hardware's and no capture says otherwise.
 */
#include "nvn_vk.h"

#define BATCH 1024u
#define STATS 10u

static struct {
    VkQueryPool occ, stats;
    uint32_t pending;
    nvn_gfx *owner[BATCH];
    uint8_t tess[BATCH], geom[BATCH];
} q;

static bool pools(void)
{
    if (q.occ)
        return true;
    VkQueryPoolCreateInfo oi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .queryType = VK_QUERY_TYPE_OCCLUSION,
                                 .queryCount = BATCH };
    VkQueryPoolCreateInfo si = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .queryType = VK_QUERY_TYPE_PIPELINE_STATISTICS,
                                 .queryCount = BATCH,
                                 .pipelineStatistics = 0x3ffu };
    if (VK(vkCreateQueryPool)(g.dev, &oi, NULL, &q.occ) != VK_SUCCESS ||
        VK(vkCreateQueryPool)(g.dev, &si, NULL, &q.stats) != VK_SUCCESS) {
        q.occ = NULL;
        return false;
    }
    VK(vkResetQueryPool)(g.dev, q.occ, 0, BATCH);
    VK(vkResetQueryPool)(g.dev, q.stats, 0, BATCH);
    return true;
}

/** @brief Submits what the queue recorded, reads back the finished draws' queries and adds them to their queues' counters. */
static int harvest(replay *r)
{
    if (!q.pending)
        return RL_OK;
    if (!nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    static uint64_t occ[BATCH], stats[BATCH][STATS];
    uint32_t n = q.pending;
    if (VK(vkGetQueryPoolResults)(g.dev, q.occ, 0, n, sizeof occ[0] * n, occ, sizeof occ[0], VK_QUERY_RESULT_64_BIT) !=
            VK_SUCCESS ||
        VK(vkGetQueryPoolResults)(g.dev, q.stats, 0, n, sizeof stats[0] * n, stats, sizeof stats[0],
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
        return nvn_vk_stop(r, "the host device could not read its counter queries");
    for (uint32_t i = 0; i < n; i++) {
        uint64_t *a = q.owner[i]->counter_acc, *s = stats[i];
        a[1] += occ[i];
        a[2] += s[0];
        a[3] += s[1];
        a[4] += s[2];
        a[5] += s[8];
        a[6] += s[9];
        a[7] += s[3];
        a[8] += s[7];
        a[10] += s[4];
        a[11] += s[5];
        a[12] += s[6];
        a[13] += q.geom[i] ? s[4] : s[1];
        if (q.tess[i])
            q.owner[i]->counter_inexact |= 1u << 9 | 1u << 13;
    }
    VK(vkResetQueryPool)(g.dev, q.occ, 0, n);
    VK(vkResetQueryPool)(g.dev, q.stats, 0, n);
    q.pending = 0;
    return RL_OK;
}

int nvn_counters_reset(replay *r, uint32_t type)
{
    if (type == 0 || type >= NVN_COUNTERS)
        return RL_OK;
    nvn_gfx *st = r->st;
    if (!g.query_ok)
        return nvn_vk_stop(r, "nvnCommandBufferResetCounter of counter type %u: the host device has no occlusion and "
                              "pipeline statistics queries",
                           type);
    if (!pools())
        return nvn_vk_stop(r, "the host device could not make its counter query pools");
    int rc = harvest(r);
    if (rc != RL_OK)
        return rc;
    st->counting = true;
    st->counter_acc[type] = 0;
    st->counter_valid |= 1u << type;
    st->counter_inexact &= ~(1u << type);
    return RL_OK;
}

int nvn_counters_report(replay *r, uint32_t type, uint64_t addr)
{
    nvn_gfx *st = r->st;
    uint64_t value = 0;
    if (type != 0 && type != NVN_COUNTER_TIMESTAMP_TOP) {
        if (type >= NVN_COUNTERS)
            return nvn_vk_stop(r, "nvnCommandBufferReportCounter of counter type %u, which the layer does not count",
                               type);
        if (!(st->counter_valid & 1u << type)) {
            if (st->drew_uncounted)
                return nvn_vk_stop(r, "nvnCommandBufferReportCounter of counter type %u, which no reset began and a "
                                      "draw has run since the queue started",
                                   type);
        } else {
            int rc = harvest(r);
            if (rc != RL_OK)
                return rc;
            if (st->counter_inexact & 1u << type)
                return nvn_vk_stop(r, "nvnCommandBufferReportCounter of counter type %u after a tessellated draw, whose "
                                      "tessellation primitives the host device does not count",
                                   type);
            value = st->counter_acc[type];
        }
    }
    if (!nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    rl_st64(r->mem, nvn_gpu_guest(addr), value);
    rl_st64(r->mem, nvn_gpu_guest(addr) + 8, rl_guest_clock_ns());
    return RL_OK;
}

int nvn_counters_prepare(replay *r)
{
    return r->st->counting && q.pending == BATCH ? harvest(r) : RL_OK;
}

uint32_t nvn_counters_begin(nvn_gfx *st, bool has_geometry, bool has_tessellation)
{
    if (!st->counting) {
        st->drew_uncounted = true;
        return UINT32_MAX;
    }
    uint32_t slot = q.pending++;
    q.owner[slot] = st;
    q.geom[slot] = has_geometry;
    q.tess[slot] = has_tessellation;
    VK(vkCmdBeginQuery)(g.cb, q.occ, slot, g.occlusion_precise ? VK_QUERY_CONTROL_PRECISE_BIT : 0);
    VK(vkCmdBeginQuery)(g.cb, q.stats, slot, 0);
    return slot;
}

void nvn_counters_end(uint32_t slot)
{
    if (slot == UINT32_MAX)
        return;
    VK(vkCmdEndQuery)(g.cb, q.stats, slot);
    VK(vkCmdEndQuery)(g.cb, q.occ, slot);
}
