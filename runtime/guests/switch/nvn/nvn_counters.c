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
 *
 * A report does not wait for the device: it writes a GPU timestamp query where it sits in the command stream, and
 * once the batch finished, its completion adds the queries of the draws before each report (and zeroes a counter at
 * each reset) in stream order and writes each report's value and moment, on the guest clock, to its address, as the
 * console's GPU writes it when it reaches the report: the value is what the waiting path read back at the same place.
 * A reports thread runs the completion as soon as the batch's timeline value is reached, so a guest spinning on the
 * memory sees it then, and a wait on the batch runs it itself if the thread has not, so no wait depends on that
 * thread waking. A GPU timestamp becomes a guest time through a pair of the GPU clock and the guest clock's host clock
 * read at one moment when the batch is submitted (VK_KHR_calibrated_timestamps), so a report's time is when the GPU
 * reached it. Where the host has no such pair (nvn_vk_calibrate false) the fallback takes the batch's last timestamp
 * as the moment the reports thread saw the fence signal, which is late by that thread's wake-up. A wait on the batch (nvn_vk_settle and everything that waits for the device) returns only after its
 * reports are written. In fixed-step mode a report still waits for the device and takes the guest clock then, so
 * runs repeat exactly.
 */
#include "nvn_vk.h"

#include <pthread.h>

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

/** @brief Reports and resets a batch may hold before one waits for the device first. */
#define EVENTS 255u

/** @brief A reset or a report at its place in the stream: after the draws of query slots below slot. */
typedef struct event {
    nvn_gfx *st;
    uint64_t addr;
    uint32_t type, slot;
    /** @brief A report, not a reset; and whether its counter had been reset when it was recorded. */
    bool report, valid;
} event;

/**
 * @brief The resets and reports of the batch being recorded or run, in stream order, each report with a timestamp
 * query of its own index (index n is the batch's last timestamp, for the fallback clock), and the completion state.
 * One batch at a time is recorded or in flight (nvn_vk_begin waits for the previous), so one list serves both.
 */
static struct {
    VkQueryPool pool;
    bool failed;
    event ev[EVENTS];
    uint32_t n;
    pthread_mutex_t mu;
    pthread_cond_t cv;
    pthread_t thread;
    bool started;
    /** @brief The batch's timeline value and whether its completion is queued, being done (claimed) or done. */
    uint64_t value;
    bool queued, claimed, done;
    /** @brief The batch's calibrated pair (nvn_vk_calibrate), read when it was submitted; cal false without one. */
    bool cal;
    uint64_t cal_gpu, cal_ns;
    uint8_t *mem;
    /** @brief The GPU timestamps of the last batch's reports, for nvn_counters_async_check. */
    uint64_t last_ts[EVENTS];
} rp = { .mu = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

/** @brief Adds the query results of slots [from, to) to their queues' counters. */
static void accumulate(const uint64_t *occ, const uint64_t (*stats)[STATS], uint32_t from, uint32_t to)
{
    for (uint32_t i = from; i < to; i++) {
        uint64_t *a = q.owner[i]->counter_acc;
        const uint64_t *s = stats[i];
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
    }
}

/**
 * @brief The completion's work on read-back results: the events in stream order, each draw's queries added to its
 * queue's counters before the events after it, a counter zeroed at its reset, and each report's value and time
 * (from ts on the calibrated pair or the fallback, or seen without ts) written to guest memory.
 */
static void walk(const uint64_t *occ, const uint64_t (*stats)[STATS], const uint64_t *ts, uint32_t np, uint32_t n,
                 uint64_t seen)
{
    uint32_t at = 0;
    for (uint32_t i = 0; i < n; i++) {
        const event *e = &rp.ev[i];
        accumulate(occ, stats, at, e->slot);
        at = e->slot;
        if (!e->report) {
            e->st->counter_acc[e->type] = 0;
            continue;
        }
        uint64_t value = e->type == 0 || e->type == NVN_COUNTER_TIMESTAMP_TOP || e->type >= NVN_COUNTERS || !e->valid
                             ? 0
                             : e->st->counter_acc[e->type];
        uint64_t when = seen;
        if (ts && rp.cal) {
            when = (uint64_t)((double)rp.cal_ns + (double)(int64_t)(ts[i] - rp.cal_gpu) * g.timestamp_period_ns);
        } else if (ts) {
            uint64_t back = (uint64_t)((double)(ts[n] - ts[i]) * g.timestamp_period_ns);
            when = back < seen ? seen - back : 0;
        }
        __atomic_store_n((uint64_t *)(rp.mem + e->addr), value, __ATOMIC_RELAXED);
        __atomic_store_n((uint64_t *)(rp.mem + e->addr + 8), when, __ATOMIC_RELEASE);
    }
    accumulate(occ, stats, at, np);
    if (ts)
        memcpy(rp.last_ts, ts, sizeof ts[0] * n);
}

/**
 * @brief The batch's completion, once its work finished: walks its resets and reports in stream order, adding each
 * draw's queries to its queue's counters before the events after it, zeroing a counter at its reset, and writing each
 * report's value and time to guest memory. Runs once per batch, on whichever thread claimed it.
 */
static void complete(void)
{
    static uint64_t occ[BATCH], stats[BATCH][STATS], ts[EVENTS + 1];
    uint32_t np = q.pending, n = rp.n;
    uint64_t seen = rl_guest_clock_ns();
    bool ok = true;
    if (np)
        ok = VK(vkGetQueryPoolResults)(g.dev, q.occ, 0, np, sizeof occ[0] * np, occ, sizeof occ[0],
                                       VK_QUERY_RESULT_64_BIT) == VK_SUCCESS &&
             VK(vkGetQueryPoolResults)(g.dev, q.stats, 0, np, sizeof stats[0] * np, stats, sizeof stats[0],
                                       VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    bool stamped = n && !hos_fixed();
    if (ok && stamped)
        ok = VK(vkGetQueryPoolResults)(g.dev, rp.pool, 0, n + 1, sizeof ts[0] * (n + 1), ts, sizeof ts[0],
                                       VK_QUERY_RESULT_64_BIT) == VK_SUCCESS;
    if (!ok)
        rl_log(RL_LOG_ERROR, "nvn", "the host device could not read its counter queries; %u reports are not written", n);
    else
        walk(occ, stats, stamped ? ts : NULL, np, n, seen);
    if (np) {
        VK(vkResetQueryPool)(g.dev, q.occ, 0, np);
        VK(vkResetQueryPool)(g.dev, q.stats, 0, np);
    }
    if (stamped)
        VK(vkResetQueryPool)(g.dev, rp.pool, 0, n + 1);
    q.pending = 0;
    rp.n = 0;
}

/** @brief Claims the queued completion and runs it, or waits for the thread that claimed it. rp.mu held. */
static void claim_or_wait(void)
{
    if (!rp.claimed) {
        rp.claimed = true;
        pthread_mutex_unlock(&rp.mu);
        complete();
        pthread_mutex_lock(&rp.mu);
        rp.done = true;
        pthread_cond_broadcast(&rp.cv);
    }
    while (!rp.done)
        pthread_cond_wait(&rp.cv, &rp.mu);
}

/**
 * @brief The reports thread: waits on the timeline for each queued batch with reports and completes it as soon as
 * the GPU finished it, so a guest spinning on a report's memory sees it then, unless settle completed it first.
 */
static void *reports_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&rp.mu);
    for (;;) {
        while (!rp.queued || rp.claimed)
            pthread_cond_wait(&rp.cv, &rp.mu);
        uint64_t value = rp.value;
        pthread_mutex_unlock(&rp.mu);
        VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO,
                                   .semaphoreCount = 1,
                                   .pSemaphores = &g.timeline,
                                   .pValues = &value };
        VkResult vr = VK(vkWaitSemaphores)(g.dev, &wi, UINT64_MAX);
        pthread_mutex_lock(&rp.mu);
        if (vr == VK_SUCCESS && rp.queued && rp.value == value)
            claim_or_wait();
        else if (vr != VK_SUCCESS)
            while (rp.queued && rp.value == value)
                pthread_cond_wait(&rp.cv, &rp.mu);
    }
    return NULL;
}

/** @brief Whether reports and resets can wait for the batch's completion; false keeps the waiting path. */
static bool deferrable(replay *r)
{
    if (rp.failed || g.timestamp_period_ns <= 0)
        return false;
    if (rp.n == EVENTS && !nvn_vk_flush(r))
        return false;
    if (!rp.pool) {
        VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                     .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                     .queryCount = EVENTS + 1 };
        if (VK(vkCreateQueryPool)(g.dev, &qi, NULL, &rp.pool) != VK_SUCCESS) {
            rp.failed = true;
            return false;
        }
        VK(vkResetQueryPool)(g.dev, rp.pool, 0, EVENTS + 1);
    }
    rp.mem = r->mem;
    if (!rp.started && g.timeline_ok && !hos_fixed()) {
        if (pthread_create(&rp.thread, NULL, reports_main, NULL) != 0) {
            rp.failed = true;
            return false;
        }
        rp.started = true;
    }
    return nvn_vk_begin(r);
}

/** @brief Records event e at this place in the stream, with a timestamp query of its index. */
static void add_event(event e)
{
    nvn_vk_barrier(g.cb);
    VK(vkCmdWriteTimestamp)(g.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, rp.pool, rp.n);
    e.slot = q.pending;
    rp.ev[rp.n++] = e;
}

/** @brief Submits what the queue recorded and waits for it: the completion adds the finished draws' queries. */
static int harvest(replay *r)
{
    if (!q.pending && !rp.n)
        return RL_OK;
    if (!nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
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
    if (deferrable(r)) {
        add_event((event){ .st = st, .type = type });
    } else {
        int rc = harvest(r);
        if (rc != RL_OK)
            return rc;
        st->counter_acc[type] = 0;
    }
    st->counting = true;
    st->counter_valid |= 1u << type;
    st->counter_inexact &= ~(1u << type);
    return RL_OK;
}

void nvn_counters_close(VkCommandBuffer cb)
{
    if (rp.n)
        VK(vkCmdWriteTimestamp)(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, rp.pool, rp.n);
}

void nvn_counters_submitted(uint64_t value)
{
    if (!rp.n && !q.pending)
        return;
    uint64_t cg = 0, cn = 0;
    bool cal = rp.n && nvn_vk_calibrate(&cg, &cn);
    pthread_mutex_lock(&rp.mu);
    rp.cal = cal;
    rp.cal_gpu = cg;
    rp.cal_ns = cn;
    rp.value = value;
    rp.queued = true;
    rp.claimed = false;
    rp.done = false;
    if (rp.started && value && rp.n)
        pthread_cond_broadcast(&rp.cv);
    pthread_mutex_unlock(&rp.mu);
}

void nvn_counters_settle(void)
{
    pthread_mutex_lock(&rp.mu);
    if (rp.queued) {
        claim_or_wait();
        rp.queued = false;
        pthread_cond_broadcast(&rp.cv);
    }
    pthread_mutex_unlock(&rp.mu);
}

uint32_t nvn_counters_reports_pending(void) { return rp.n; }

int nvn_counters_report(replay *r, uint32_t type, uint64_t addr)
{
    nvn_gfx *st = r->st;
    uint64_t value = 0;
    bool counted = type != 0 && type != NVN_COUNTER_TIMESTAMP_TOP;
    if (counted) {
        if (type >= NVN_COUNTERS)
            return nvn_vk_stop(r, "nvnCommandBufferReportCounter of counter type %u, which the layer does not count",
                               type);
        if (!(st->counter_valid & 1u << type) && st->drew_uncounted)
            return nvn_vk_stop(r, "nvnCommandBufferReportCounter of counter type %u, which no reset began and a "
                                  "draw has run since the queue started",
                               type);
        if (st->counter_valid & 1u << type && st->counter_inexact & 1u << type)
            return nvn_vk_stop(r, "nvnCommandBufferReportCounter of counter type %u after a tessellated draw, whose "
                                  "tessellation primitives the host device does not count",
                               type);
    }
    uint64_t at = nvn_gpu_guest(addr);
    if (at % 8 == 0 && (!counted || pools()) && deferrable(r)) {
        add_event((event){ .st = st, .addr = at, .type = type, .report = true, .valid = (st->counter_valid >> type & 1u) != 0 });
        if (hos_fixed() && !nvn_vk_flush(r))
            return nvn_vk_stop(r, "the host device failed a submit");
        return RL_OK;
    }
    if (counted && st->counter_valid & 1u << type) {
        int rc = harvest(r);
        if (rc != RL_OK)
            return rc;
        value = st->counter_acc[type];
    }
    if (!nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    rl_st64(r->mem, at, value);
    rl_st64(r->mem, at + 8, rl_guest_clock_ns());
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
    if (has_tessellation)
        st->counter_inexact |= 1u << 9 | 1u << 13;
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

int nvn_counters_async_check(uint8_t *mem, uint64_t addr)
{
    nvn_lock();
    staging big;
    const uint64_t bytes = 64ull << 20;
    int rc = 0;
    if (!nvn_vk_staging_new(&big, bytes)) {
        nvn_unlock();
        return 1;
    }
    memset(mem + addr, 0xaa, 32);
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    replay r = { .mem = mem, .st = &st };
    uint64_t t0 = rl_guest_clock_ns();
    nvn_vk_begin(&r);
    if (nvn_counters_report(&r, 0, addr) != RL_OK)
        rc = 2;
    for (uint32_t i = 0; i < 64; i++) {
        VK(vkCmdFillBuffer)(g.cb, big.buf, 0, bytes, i);
        nvn_vk_barrier(g.cb);
    }
    if (!rc && nvn_counters_report(&r, NVN_COUNTER_TIMESTAMP_TOP, addr + 16) != RL_OK)
        rc = 3;
    if (!rc && rl_ld64(mem, addr + 8) != 0xaaaaaaaaaaaaaaaaull)
        rc = 4;
    if (!rc && !nvn_vk_submit(&r))
        rc = 5;
    uint64_t limit = rl_guest_clock_ns() + 5000000000ull;
    while (!rc && __atomic_load_n((uint64_t *)(mem + addr + 24), __ATOMIC_ACQUIRE) == 0xaaaaaaaaaaaaaaaaull)
        if (rl_guest_clock_ns() > limit)
            rc = 6;
    uint64_t t1 = rl_guest_clock_ns();
    if (!rc && !nvn_vk_settle())
        rc = 7;
    uint64_t a = rl_ld64(mem, addr + 8), b = rl_ld64(mem, addr + 24);
    if (!rc && (rl_ld64(mem, addr) != 0 || rl_ld64(mem, addr + 16) != 0))
        rc = 8;
    else if (!rc && (a < t0 || b < a || b > t1))
        rc = 9;
    else if (!rc && b - a < 100000)
        rc = 10;
    uint64_t gpu1, ns1;
    if (!rc && g.calibrated) {
        if (!nvn_vk_calibrate(&gpu1, &ns1))
            rc = 11;
        for (int i = 0; !rc && i < 2; i++) {
            double want = (double)ns1 - (double)(gpu1 - rp.last_ts[i]) * g.timestamp_period_ns;
            double got = (double)(i ? b : a);
            if (got - want > 15000.0 || want - got > 15000.0)
                rc = 12 + i;
        }
    }
    nvn_vk_staging_free(&big);
    nvn_unlock();
    return rc;
}

int nvn_counters_order_check(uint8_t *mem, uint64_t addr)
{
    static const int stat_of[NVN_COUNTERS] = { -1, -2, 0, 1, 2, 8, 9, 3, 7, -1, 4, 5, 6, -3, -1 };
    static uint64_t occ[3], stats[3][STATS];
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    rp.mem = mem;
    rp.n = 0;
    for (uint32_t d = 0; d < 3; d++) {
        occ[d] = 1000 + d;
        for (uint32_t k = 0; k < STATS; k++)
            stats[d][k] = 100 * (d + 1) + k;
        q.owner[d] = &st;
        q.geom[d] = d == 1;
        q.tess[d] = 0;
    }
    for (uint32_t t = 1; t < NVN_COUNTERS; t++)
        rp.ev[rp.n++] = (event){ .st = &st, .type = t, .slot = 0 };
    for (uint32_t t = 0; t < NVN_COUNTERS; t++)
        rp.ev[rp.n++] = (event){ .st = &st, .addr = addr + 16 * t, .type = t, .slot = 1, .report = true, .valid = t != 0 };
    rp.ev[rp.n++] = (event){ .st = &st, .type = 3, .slot = 2 };
    for (uint32_t t = 0; t < NVN_COUNTERS; t++)
        rp.ev[rp.n++] = (event){ .st = &st, .addr = addr + 256 + 16 * t, .type = t, .slot = 3, .report = true, .valid = t != 0 };
    walk(occ, stats, NULL, 3, rp.n, 77);
    rp.n = 0;
    for (uint32_t r = 0; r < 2; r++)
        for (uint32_t t = 0; t < NVN_COUNTERS; t++) {
            uint64_t want = 0;
            uint32_t first = r == 1 && t == 3 ? 2 : 0, last = r == 0 ? 1 : 3;
            for (uint32_t dr = first; dr < last; dr++) {
                int k = stat_of[t];
                if (k >= 0)
                    want += stats[dr][k];
                else if (k == -2)
                    want += occ[dr];
                else if (k == -3)
                    want += dr == 1 ? stats[dr][4] : stats[dr][1];
            }
            uint64_t got = rl_ld64(mem, addr + 256 * r + 16 * t), when = rl_ld64(mem, addr + 256 * r + 16 * t + 8);
            if (got != want || when != 77)
                return 1 + (int)(r * NVN_COUNTERS + t);
        }
    return 0;
}
