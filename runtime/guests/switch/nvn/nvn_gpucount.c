/**
 * @file nvn_gpucount.c
 * @brief Hardware counters per render pass (RELIFT_GPU_COUNTERS, with gpu.profile on): VK_KHR_performance_query.
 *
 * RELIFT_GPU_COUNTERS=list logs every counter the host driver offers. RELIFT_GPU_COUNTERS=<name>,<name>,... counts
 * those (as many as the driver counts in one pass of the hardware; more are refused) between the start and the end of
 * each render pass, and the profile report lists, per pass label, what each counter counted per present. The driver
 * must expose the extension (Turnip: TU_DEBUG=perfc), and the counters are the driver's own, so the figures are
 * those of the host GPU: bytes and cycles of the memory and shader units, to tell a pass bound by memory traffic from
 * one bound by arithmetic.
 */
#include "nvn_vk.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** @brief The structs of VK_KHR_performance_query, which the generated headers do not carry. */
typedef struct pq_counter {
    VkStructureType sType;
    void *pNext;
    uint32_t unit, scope, storage;
    uint8_t uuid[16];
} pq_counter;

typedef struct pq_desc {
    VkStructureType sType;
    void *pNext;
    uint32_t flags;
    char name[256], category[256], description[256];
} pq_desc;

typedef struct pq_create {
    VkStructureType sType;
    const void *pNext;
    uint32_t family, count;
    const uint32_t *indices;
} pq_create;

typedef struct pq_lock {
    VkStructureType sType;
    const void *pNext;
    uint32_t flags;
    uint64_t timeout;
} pq_lock;

typedef struct pq_submit {
    VkStructureType sType;
    const void *pNext;
    uint32_t pass;
} pq_submit;

#define STYPE_PQ_COUNTER ((VkStructureType)1000116004)
#define STYPE_PQ_DESC ((VkStructureType)1000116005)
#define STYPE_PQ_CREATE ((VkStructureType)1000116001)
#define STYPE_PQ_LOCK ((VkStructureType)1000116003)
#define STYPE_PQ_SUBMIT ((VkStructureType)1000116006)
#define QUERY_PERFORMANCE ((VkQueryType)1000116000)
/** @brief The counters counted at once, and the passes (queries) a batch may hold. */
#define MAX_COUNTERS 8u
#define QUERIES 512u
#define LABELS 256u

static struct {
    bool tried, on, listed;
    uint32_t n;
    char name[MAX_COUNTERS][64];
    uint32_t storage[MAX_COUNTERS];
    VkQueryPool pool[NVN_SLOTS];
    uint32_t used[NVN_SLOTS];
    char lab[NVN_SLOTS][QUERIES][48];
    struct {
        char lab[48];
        double sum[MAX_COUNTERS];
        uint64_t count;
    } tot[LABELS];
    uint32_t ntot;
} pq;

typedef VkResult (*enum_fn)(VkPhysicalDevice, uint32_t, uint32_t *, pq_counter *, pq_desc *);
typedef void (*passes_fn)(VkPhysicalDevice, const pq_create *, uint32_t *);
typedef VkResult (*lock_fn)(VkDevice, const pq_lock *);

static bool setup(void)
{
    const char *want = getenv("RELIFT_GPU_COUNTERS");
    if (!want || !*want || !g.perf_query)
        return false;
    enum_fn en;
    passes_fn pf;
    lock_fn lk;
    PFN_vkVoidFunction a = VK(vkGetInstanceProcAddr)(g.instance, "vkEnumeratePhysicalDeviceQueueFamilyPerformanceQueryCountersKHR");
    PFN_vkVoidFunction b = VK(vkGetInstanceProcAddr)(g.instance, "vkGetPhysicalDeviceQueueFamilyPerformanceQueryPassesKHR");
    PFN_vkVoidFunction c = VK(vkGetDeviceProcAddr)(g.dev, "vkAcquireProfilingLockKHR");
    memcpy(&en, &a, sizeof en);
    memcpy(&pf, &b, sizeof pf);
    memcpy(&lk, &c, sizeof lk);
    if (!en || !pf || !lk)
        return false;
    uint32_t n = 0;
    if (en(g.phys, g.family, &n, NULL, NULL) != VK_SUCCESS || !n)
        return false;
    pq_counter *cs = calloc(n, sizeof *cs);
    pq_desc *ds = calloc(n, sizeof *ds);
    if (!cs || !ds) {
        free(cs);
        free(ds);
        return false;
    }
    for (uint32_t i = 0; i < n; i++) {
        cs[i].sType = STYPE_PQ_COUNTER;
        ds[i].sType = STYPE_PQ_DESC;
    }
    if (en(g.phys, g.family, &n, cs, ds) < VK_SUCCESS) {
        free(cs);
        free(ds);
        return false;
    }
    uint32_t pick[MAX_COUNTERS];
    bool list = !strcmp(want, "list");
    if (list)
        for (uint32_t i = 0; i < n; i++)
            rl_log(RL_LOG_INFO, "nvn", "perf counter %u: %s (%s) unit %u storage %u", i, ds[i].name, ds[i].category, cs[i].unit,
                   cs[i].storage);
    for (const char *p = want; !list && *p && pq.n < MAX_COUNTERS;) {
        size_t len = strcspn(p, ",");
        uint32_t i = 0;
        while (i < n && !(strlen(ds[i].name) == len && !strncmp(ds[i].name, p, len)))
            i++;
        if (i == n) {
            rl_log(RL_LOG_WARN, "nvn", "perf counter '%.*s' is not one the driver offers", (int)len, p);
        } else {
            pick[pq.n] = i;
            snprintf(pq.name[pq.n], sizeof pq.name[0], "%s", ds[i].name);
            pq.storage[pq.n++] = cs[i].storage;
        }
        p += len + (p[len] == ',');
    }
    free(cs);
    free(ds);
    if (!pq.n)
        return false;
    pq_create ci = { STYPE_PQ_CREATE, NULL, g.family, pq.n, pick };
    uint32_t passes = 0;
    pf(g.phys, &ci, &passes);
    if (passes != 1) {
        rl_log(RL_LOG_WARN, "nvn", "perf counters: these %u counters need %u passes of the hardware, one is possible: fewer or others",
               pq.n, passes);
        return false;
    }
    pq_lock ll = { STYPE_PQ_LOCK, NULL, 0, 1000000000ull };
    if (lk(g.dev, &ll) != VK_SUCCESS) {
        rl_log(RL_LOG_WARN, "nvn", "perf counters: the profiling lock was refused");
        return false;
    }
    VkQueryPoolCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                                 .pNext = &ci,
                                 .queryType = QUERY_PERFORMANCE,
                                 .queryCount = QUERIES };
    for (uint32_t i = 0; i < NVN_SLOTS; i++)
        if (VK(vkCreateQueryPool)(g.dev, &qi, NULL, &pq.pool[i]) != VK_SUCCESS) {
            rl_log(RL_LOG_WARN, "nvn", "perf counters: no query pool");
            return false;
        }
    rl_log(RL_LOG_INFO, "nvn", "perf counters on: %u in one pass, per render pass", pq.n);
    return true;
}

bool nvn_gpucount_on(void)
{
    if (!pq.tried) {
        pq.tried = true;
        pq.on = nvn_gpuprof_on() && setup();
    }
    return pq.on;
}

const void *nvn_gpucount_submit_next(const void *next)
{
    static _Thread_local pq_submit sub;
    if (!pq.on)
        return next;
    sub = (pq_submit){ STYPE_PQ_SUBMIT, next, 0 };
    return &sub;
}

void nvn_gpucount_batch_begin(void)
{
    if (!pq.on)
        return;
    VK(vkResetQueryPool)(g.dev, pq.pool[g.slot], 0, QUERIES);
    pq.used[g.slot] = 0;
}

void nvn_gpucount_pass_begin(void)
{
    if (!pq.on || pq.used[g.slot] >= QUERIES)
        return;
    VK(vkCmdBeginQuery)(g.cb, pq.pool[g.slot], pq.used[g.slot], 0);
}

void nvn_gpucount_pass_end(const char *fmt, ...)
{
    if (!pq.on || pq.used[g.slot] >= QUERIES)
        return;
    VK(vkCmdEndQuery)(g.cb, pq.pool[g.slot], pq.used[g.slot]);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(pq.lab[g.slot][pq.used[g.slot]], sizeof pq.lab[0][0], fmt, ap);
    va_end(ap);
    pq.used[g.slot]++;
}

void nvn_gpucount_done(uint32_t s)
{
    uint32_t n = pq.used[s];
    pq.used[s] = 0;
    if (!pq.on || !n)
        return;
    static uint64_t res[QUERIES][MAX_COUNTERS];
    VkResult vr = VK(vkGetQueryPoolResults)(g.dev, pq.pool[s], 0, n, sizeof res[0] * n, res, sizeof res[0],
                                            VK_QUERY_RESULT_WAIT_BIT);
    if (vr != VK_SUCCESS) {
        rl_log(RL_LOG_WARN, "nvn", "perf counters: results not read (VkResult %d)", (int)vr);
        return;
    }
    for (uint32_t q = 0; q < n; q++) {
        uint32_t i = 0;
        while (i < pq.ntot && strcmp(pq.tot[i].lab, pq.lab[s][q]))
            i++;
        if (i == pq.ntot) {
            if (pq.ntot == LABELS)
                continue;
            memset(&pq.tot[pq.ntot], 0, sizeof pq.tot[0]);
            snprintf(pq.tot[pq.ntot++].lab, sizeof pq.tot[0].lab, "%s", pq.lab[s][q]);
        }
        pq.tot[i].count++;
        for (uint32_t c = 0; c < pq.n; c++) {
            uint64_t raw = res[q][c];
            double v;
            switch (pq.storage[c]) {
            case 0: /* int32 */
                v = (double)(int32_t)raw;
                break;
            case 1: /* int64 */
                v = (double)(int64_t)raw;
                break;
            case 2: /* uint32 */
                v = (double)(uint32_t)raw;
                break;
            case 3: /* uint64 */
                v = (double)raw;
                break;
            case 4: { /* float32 */
                float f;
                memcpy(&f, &raw, sizeof f);
                v = f;
                break;
            }
            default: { /* float64 */
                memcpy(&v, &raw, sizeof v);
                break;
            }
            }
            pq.tot[i].sum[c] += v;
        }
    }
}

void nvn_gpucount_report(double frames)
{
    if (!pq.on || frames <= 0)
        return;
    for (uint32_t k = 0; k < 40 && pq.ntot; k++) {
        uint32_t b = LABELS;
        double best = 0;
        for (uint32_t i = 0; i < pq.ntot; i++)
            if (pq.tot[i].count && pq.tot[i].sum[0] >= best) {
                best = pq.tot[i].sum[0];
                b = i;
            }
        if (b == LABELS)
            break;
        char line[512];
        int o = snprintf(line, sizeof line, "%.1f x %s:", (double)pq.tot[b].count / frames, pq.tot[b].lab);
        for (uint32_t c = 0; c < pq.n && o < (int)sizeof line - 80; c++)
            o += snprintf(line + o, sizeof line - (size_t)o, " %s %.0f", pq.name[c], pq.tot[b].sum[c] / frames);
        rl_log(RL_LOG_INFO, "nvn", "GPU counters per present %s", line);
        pq.tot[b].count = 0;
    }
    pq.ntot = 0;
}
