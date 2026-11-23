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

#include <relift/settings.h>

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/** @brief Marks per batch; a batch with more lumps the rest into its last interval. */
#define MARKS 1024u
#define LABEL 64u
/** @brief Distinct labels ranked per report. */
#define TOTALS 512u

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
} gp;

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
        else
            snprintf(gp.tot[gp.ntot++].lab, LABEL, "%s", lab);
    }
    gp.tot[i].ns += ns;
    gp.tot[i].count++;
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
    gp.ntot = 0;
    gp.span_ns = gp.between_ns = 0;
    gp.batches = 0;
}
