/**
 * @file nvn_gpuprof.c
 * @brief The GPU profile (setting gpu.profile): where each batch's GPU time goes, with parts that sum to its span.
 *
 * A batch writes a timestamp at its start and one at each mark: before a render pass begins, after it ends (with the
 * barrier that follows it), after each dispatch and after each transfer the layer records (an upload, a write-back,
 * the upscaler, the present copy), and at its end. Each timestamp waits for the commands before it (bottom of the
 * pipe), so the time between two marks is the GPU time of what was recorded between them, and the intervals of a
 * batch add up to its span exactly. An interval is named by its mark (a pass's size and attachment count, a
 * dispatch's program) or by the kinds of commands recorded in it. The time from one batch's end to the next one's
 * start on the GPU's clock is reported apart, as time between batches: the GPU ran other work then (the presenter,
 * another process) or had nothing queued, which is scheduling and not this batch's work. The presenter's own GPU
 * time is reported by the core (present.c) beside it.
 *
 * Timestamps inside a render pass are not used: on a tiling GPU they do not say when a draw's work ran.
 */
#include "nvn_vk.h"
#include "present.h"
#include "hos.h"

#include <relift/settings.h>

#include <stdarg.h>
#include <time.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/** @brief Marks per batch; a batch with more lumps the rest into its last interval. */
#define MARKS 1024u
#define LABEL 64u
/** @brief Distinct labels ranked per report. */
#define TOTALS 512u
/** @brief Batches and thread spans kept per report for the time between batches. */
#define RECS 2048u
#define SPANS 65536u

static struct {
    int on;
    /** @brief One timestamp pool per batch slot: MoltenVK reads every timestamp of a pool of more than 4096 as one. */
    VkQueryPool pool[NVN_SLOTS];
    double period_ns;
    /** @brief Per slot: marks written, and each interval's label (interval i ends at mark i + 1). */
    uint32_t n[NVN_SLOTS];
    char lab[NVN_SLOTS][MARKS][LABEL];
    /** @brief The kinds recorded since the last mark, for an interval with no name of its own. */
    char kinds[LABEL];
    /** @brief The GPU clock at the end of the last batch read, for the time between batches. */
    uint64_t last_end;
    struct total {
        char lab[LABEL];
        double ns;
        uint64_t count;
    } tot[TOTALS];
    uint32_t ntot;
    double span_ns, between_ns;
    uint64_t batches;
    /** @brief Per slot, on the host's clock: recording began, the batch was handed on, vkQueueSubmit returned. */
    int64_t rec_ns[NVN_SLOTS], queued_ns[NVN_SLOTS], entered_ns[NVN_SLOTS], handed_ns[NVN_SLOTS];
    int rec_tid[NVN_SLOTS];
    /** @brief The batches read since the last report, in queue order, for the time between batches (gap_report). */
    struct batch_rec {
        int64_t start, end; /* GPU clock in ns */
        int64_t rec, queued, entered, handed, seen; /* host clock */
        int tid;
    } recs[RECS];
    uint32_t nrecs;
    /** @brief What threads did on the host's clock since the last report (SPAN_*). */
    struct span {
        int64_t lo, hi;
        int tid, kind;
    } spans[SPANS];
    uint32_t nspans;
    struct wait_site {
        const char *site;
        double ns;
        uint64_t count;
    } waits[32];
    uint32_t nwaits;
} gp;

/** @brief Parts of the time between batches, in the order the painting prefers them (gap_report). */
enum { GAP_DEVICE = SPAN_KINDS, GAP_SUBMIT, GAP_GAME, GAP_N };
static const char *const gap_names[GAP_N] = {
    "fence wait", "NVN lock", "waiting for a guest core", "guest kernel wait", "present pacing", "replay", "present",
    "from vkQueueSubmit", "submit thread queue", "game code",
};

_Thread_local const char *nvn_wait_site;
static pthread_mutex_t span_mu = PTHREAD_MUTEX_INITIALIZER;

/** @brief hos_block's report: the guest kernel wait, then the wait for a core. */
static void block_span(int64_t lo, int64_t mid, int64_t hi)
{
    nvn_gpuprof_span(SPAN_BLOCK, lo, mid);
    nvn_gpuprof_span(SPAN_CORE, mid, hi);
}

/** @brief A small number per thread, to tell the threads' spans apart (no thread id call every host has). */
static int thread_tag(void)
{
    static int next;
    static _Thread_local int tag;
    if (!tag)
        tag = __atomic_add_fetch(&next, 1, __ATOMIC_RELAXED);
    return tag;
}

bool nvn_gpuprof_on(void)
{
    if (!gp.on) {
        gp.on = rl_setting_bool("gpu.profile") && g.timestamp_period_ns > 0 ? 1 : -1;
        if (gp.on > 0) {
            VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                         .queryType = VK_QUERY_TYPE_TIMESTAMP,
                                         .queryCount = MARKS };
            for (uint32_t i = 0; i < NVN_SLOTS && gp.on > 0; i++)
                if (VK(vkCreateQueryPool)(g.dev, &qi, NULL, &gp.pool[i]) != VK_SUCCESS)
                    gp.on = -1;
            gp.period_ns = g.timestamp_period_ns;
            if (gp.on > 0)
                hos_block_span = block_span;
            rl_log(gp.on > 0 ? RL_LOG_INFO : RL_LOG_WARN, "nvn", "GPU profile %s",
                   gp.on > 0 ? "on: every batch is timed by its parts" : "is off: no timestamp query pool");
        }
    }
    return gp.on > 0;
}

void nvn_gpuprof_batch_begin(void)
{
    if (!nvn_gpuprof_on())
        return;
    uint32_t s = g.slot;
    VK(vkResetQueryPool)(g.dev, gp.pool[s], 0, MARKS);
    VK(vkCmdWriteTimestamp)(g.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gp.pool[s], 0);
    gp.n[s] = 1;
    gp.kinds[0] = 0;
    gp.rec_ns[s] = nvn_gpuprof_now();
    gp.rec_tid[s] = thread_tag();
    __atomic_store_n(&gp.handed_ns[s], 0, __ATOMIC_RELEASE);
    __atomic_store_n(&gp.entered_ns[s], 0, __ATOMIC_RELEASE);
}

int64_t nvn_gpuprof_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

void nvn_gpuprof_queued(uint32_t s)
{
    if (gp.on > 0)
        gp.queued_ns[s] = nvn_gpuprof_now();
}

void nvn_gpuprof_entered(uint32_t s)
{
    if (gp.on > 0)
        __atomic_store_n(&gp.entered_ns[s], nvn_gpuprof_now(), __ATOMIC_RELEASE);
}

void nvn_gpuprof_handed(uint32_t s)
{
    if (gp.on > 0)
        __atomic_store_n(&gp.handed_ns[s], nvn_gpuprof_now(), __ATOMIC_RELEASE);
}

void nvn_gpuprof_waited(int64_t start_ns, int64_t end_ns)
{
    if (gp.on <= 0)
        return;
    nvn_gpuprof_span(SPAN_WAIT, start_ns, end_ns);
    const char *site = nvn_wait_site ? nvn_wait_site : "(no site)";
    uint32_t i = 0;
    while (i < gp.nwaits && strcmp(gp.waits[i].site, site))
        i++;
    if (i == gp.nwaits) {
        if (gp.nwaits == 32)
            i = 31;
        else
            gp.waits[gp.nwaits++].site = site;
    }
    gp.waits[i].ns += (double)(end_ns - start_ns);
    gp.waits[i].count++;
}

void nvn_gpuprof_span(int kind, int64_t lo, int64_t hi)
{
    if (gp.on <= 0 || hi <= lo)
        return;
    int tid = thread_tag();
    pthread_mutex_lock(&span_mu);
    if (gp.nspans < SPANS)
        gp.spans[gp.nspans++] = (struct span){ lo, hi, tid, kind };
    pthread_mutex_unlock(&span_mu);
}

/** @brief Paint step for the time a batch's thread spent before handing it on, in ns. */
#define STEP_NS 20000

/**
 * @brief Splits the time between batches by what held the next batch back. Without calibrated timestamps the GPU's
 * clock is put on the host's by the lower bound every batch gives (it cannot start before vkQueueSubmit returned),
 * taken at its largest over the report; the spread to the upper bound (it ended before its fence was seen) is logged.
 * Per gap from the end of one batch E to the start of the next S: after its vkQueueSubmit returned, in the submit
 * thread, and before it was handed on, which is split by the recording thread's spans (a fence wait first, then the
 * NVN lock, a guest core, pacing, replay, present) and the rest as game code.
 */
static void gap_report(double frames)
{
    if (gp.nrecs < 2)
        return;
    int64_t lo = INT64_MIN, hi = INT64_MAX;
    uint32_t lo_i = 0, hi_i = 0;
    for (uint32_t i = 0; i < gp.nrecs; i++) {
        struct batch_rec *b = &gp.recs[i];
        if (b->entered && b->entered - b->start > lo)
            lo = b->entered - b->start, lo_i = i;
        if (b->seen && b->seen - b->end < hi)
            hi = b->seen - b->end, hi_i = i;
    }
    if (lo == INT64_MIN)
        return;
    if (hi < lo) {
        struct batch_rec *a = &gp.recs[lo_i], *z = &gp.recs[hi_i];
        rl_log(RL_LOG_WARN, "nvn",
               "GPU profile clock bounds cross: batch %u entered %.3f ms after its GPU start; batch %u seen %.3f ms after its "
               "GPU end (spans %.3f, %.3f ms; host rec to seen %.3f, %.3f ms)",
               lo_i, (double)lo / 1e6, hi_i, (double)hi / 1e6, (double)(a->end - a->start) / 1e6,
               (double)(z->end - z->start) / 1e6, (double)(a->seen - a->rec) / 1e6, (double)(z->seen - z->rec) / 1e6);
        return;
    }
    int64_t off = lo;
    double part[GAP_N] = { 0 }, total = 0;
    pthread_mutex_lock(&span_mu);
    for (uint32_t i = 1; i < gp.nrecs; i++) {
        struct batch_rec *b = &gp.recs[i];
        int64_t e = gp.recs[i - 1].end + off, st = b->start + off;
        if (st <= e)
            continue;
        total += (double)(st - e);
        int64_t entered = b->entered ? b->entered : b->queued;
        int64_t dev_from = entered > e ? entered : e;
        if (st > dev_from)
            part[GAP_DEVICE] += (double)(st - dev_from);
        int64_t q_from = b->queued > e ? b->queued : e, q_to = entered < st ? entered : st;
        if (q_to > q_from)
            part[GAP_SUBMIT] += (double)(q_to - q_from);
        int64_t to = b->queued < st ? b->queued : st;
        static struct span near[256];
        uint32_t nn = 0;
        for (uint32_t k = 0; k < gp.nspans && nn < 256; k++)
            if (gp.spans[k].tid == b->tid && gp.spans[k].hi > e && gp.spans[k].lo < to)
                near[nn++] = gp.spans[k];
        for (int64_t t = e; t < to; t += STEP_NS) {
            int64_t w = to - t < STEP_NS ? to - t : STEP_NS;
            int best = GAP_GAME;
            for (uint32_t k = 0; k < nn; k++)
                if (near[k].lo <= t && t < near[k].hi && near[k].kind < best)
                    best = near[k].kind;
            part[best] += (double)w;
        }
    }
    pthread_mutex_unlock(&span_mu);
    char line[768];
    size_t l = 0;
    for (int k = 0; k < GAP_N && l < sizeof line; k++)
        l += (size_t)snprintf(line + l, sizeof line - l, "%s%s %.2f", k ? ", " : "", gap_names[k], part[k] / 1e6 / frames);
    rl_log(RL_LOG_INFO, "nvn", "GPU profile between batches %.2f ms: %s (GPU clock placed within %.2f ms)",
           total / 1e6 / frames, line, hi == INT64_MAX ? -1.0 : (double)(hi - lo) / 1e6);
}

void nvn_gpuprof_note(const char *kind)
{
    if (gp.on <= 0 || strstr(gp.kinds, kind))
        return;
    size_t l = strlen(gp.kinds);
    snprintf(gp.kinds + l, LABEL - l, "%s%s", l ? "+" : "", kind);
}

/** @brief Writes the mark that ends the interval labelled lab. */
static void mark(const char *lab)
{
    uint32_t s = g.slot;
    snprintf(gp.lab[s][gp.n[s] - 1], LABEL, "%s", lab);
    gp.kinds[0] = 0;
    VK(vkCmdWriteTimestamp)(g.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, gp.pool[s], gp.n[s]);
    gp.n[s]++;
}

void nvn_gpuprof_mark(const char *fmt, ...)
{
    if (gp.on <= 0 || !gp.n[g.slot] || gp.n[g.slot] >= MARKS)
        return;
    char lab[LABEL];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(lab, sizeof lab, fmt, ap);
    va_end(ap);
    mark(lab);
}

void nvn_gpuprof_mark_kinds(void)
{
    if (gp.on <= 0 || !gp.n[g.slot] || gp.n[g.slot] >= MARKS)
        return;
    char lab[LABEL];
    snprintf(lab, sizeof lab, "%s", gp.kinds[0] ? gp.kinds : "other");
    mark(lab);
}

static void add(const char *lab, double ns)
{
    uint32_t i = 0;
    while (i < gp.ntot && strcmp(gp.tot[i].lab, lab))
        i++;
    if (i == gp.ntot) {
        if (gp.ntot == TOTALS)
            i = TOTALS - 1, lab = "(more labels)";
        else {
            memset(&gp.tot[i], 0, sizeof gp.tot[i]);
            snprintf(gp.tot[gp.ntot++].lab, LABEL, "%s", lab);
        }
    }
    gp.tot[i].ns += ns;
    gp.tot[i].count++;
}

bool nvn_gpuprof_total(const char *lab, double *ns, uint64_t *count)
{
    for (uint32_t i = 0; i < gp.ntot; i++)
        if (!strcmp(gp.tot[i].lab, lab)) {
            *ns = gp.tot[i].ns;
            *count = gp.tot[i].count;
            return true;
        }
    return false;
}

void nvn_gpuprof_done(uint32_t s)
{
    uint32_t n = gp.n[s];
    gp.n[s] = 0;
    if (gp.on <= 0 || n < 2)
        return;
    static uint64_t ts[MARKS];
    /* The batch's fence has signaled; WAIT covers a driver (MoltenVK) that makes the query results available a moment
       after it, and a failure is said, not dropped. */
    VkResult vr = VK(vkGetQueryPoolResults)(g.dev, gp.pool[s], 0, n, sizeof ts[0] * n, ts, sizeof ts[0],
                                            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    if (vr != VK_SUCCESS) {
        rl_log(RL_LOG_WARN, "nvn", "gpu.profile: the batch's timestamps could not be read (VkResult %d)", (int)vr);
        return;
    }
    for (uint32_t i = 0; i + 1 < n; i++)
        add(gp.lab[s][i], (double)(ts[i + 1] - ts[i]) * gp.period_ns);
    gp.span_ns += (double)(ts[n - 1] - ts[0]) * gp.period_ns;
    if (gp.last_end && ts[0] > gp.last_end)
        gp.between_ns += (double)(ts[0] - gp.last_end) * gp.period_ns;
    gp.last_end = ts[n - 1];
    gp.batches++;
    if (gp.nrecs < RECS)
        gp.recs[gp.nrecs++] = (struct batch_rec){
            .start = (int64_t)((double)ts[0] * gp.period_ns),
            .end = (int64_t)((double)ts[n - 1] * gp.period_ns),
            .rec = gp.rec_ns[s],
            .queued = gp.queued_ns[s],
            .entered = __atomic_load_n(&gp.entered_ns[s], __ATOMIC_ACQUIRE),
            .handed = __atomic_load_n(&gp.handed_ns[s], __ATOMIC_ACQUIRE),
            .seen = nvn_gpuprof_now(),
            .tid = gp.rec_tid[s],
        };
}

void nvn_gpuprof_sums(double *span_ns, double *parts_ns, double *between_ns)
{
    double sum = 0;
    for (uint32_t i = 0; i < gp.ntot; i++)
        sum += gp.tot[i].ns;
    *span_ns = gp.span_ns;
    *parts_ns = sum;
    *between_ns = gp.between_ns;
}

void nvn_gpuprof_reset(void)
{
    for (uint32_t i = 0; i < NVN_SLOTS; i++)
        if (gp.pool[i])
            VK(vkDestroyQueryPool)(g.dev, gp.pool[i], NULL);
    memset(&gp, 0, sizeof gp);
}

void nvn_gpuprof_report(double frames)
{
    if (gp.on <= 0 || frames <= 0)
        return;
    static uint64_t ticks0, n0;
    uint64_t ticks = 0, pn = 0;
    rl_present_gpu_stats(&ticks, &pn);
    double pres_ns = (double)(ticks - ticks0) * gp.period_ns, pres_n = (double)(pn - n0);
    ticks0 = ticks;
    n0 = pn;
    rl_log(RL_LOG_INFO, "nvn",
           "GPU profile per present: batches %.2f ms in %.1f batches, between batches %.2f ms, presenter %.2f ms "
           "(%.2f presents)",
           gp.span_ns / 1e6 / frames, (double)gp.batches / frames, gp.between_ns / 1e6 / frames,
           pres_ns / 1e6 / frames, pres_n / frames);
    double shown = 0;
    for (uint32_t k = 0; k < 25 && gp.ntot; k++) {
        uint32_t b = 0;
        for (uint32_t i = 1; i < gp.ntot; i++)
            if (gp.tot[i].ns > gp.tot[b].ns)
                b = i;
        if (gp.tot[b].ns <= 0)
            break;
        shown += gp.tot[b].ns;
        rl_log(RL_LOG_INFO, "nvn", "GPU profile %7.2f ms %6.1f per present  %s", gp.tot[b].ns / 1e6 / frames,
               (double)gp.tot[b].count / frames, gp.tot[b].lab);
        gp.tot[b].ns = -gp.tot[b].ns;
    }
    rl_log(RL_LOG_INFO, "nvn", "GPU profile %7.2f ms in the other labels", (gp.span_ns - shown) / 1e6 / frames);
    gap_report(frames);
    gp.nrecs = 0;
    pthread_mutex_lock(&span_mu);
    gp.nspans = 0;
    pthread_mutex_unlock(&span_mu);
    for (uint32_t k = 0; k < 8 && gp.nwaits; k++) {
        uint32_t b = 0;
        for (uint32_t i = 1; i < gp.nwaits; i++)
            if (gp.waits[i].ns > gp.waits[b].ns)
                b = i;
        if (gp.waits[b].ns <= 0)
            break;
        rl_log(RL_LOG_INFO, "nvn", "GPU profile wait %7.2f ms %6.1f per present  %s", gp.waits[b].ns / 1e6 / frames,
               (double)gp.waits[b].count / frames, gp.waits[b].site);
        gp.waits[b].ns = -1;
    }
    gp.nwaits = 0;
    gp.ntot = 0;
    gp.span_ns = gp.between_ns = 0;
    gp.batches = 0;
}
