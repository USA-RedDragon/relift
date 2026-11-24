/**
 * @file nvn_stats.c
 * @brief The NVN use histogram, see nvn_stats.h.
 *
 * Counts are relaxed atomics; the lookups (a few hundred per run) take a lock. The file is written by a process exit
 * handler (rl_at_process_exit), which every way a run ends reaches: a stop pends a process exit, and SIGTERM and
 * SIGINT close the window, which exits the process.
 */
#include "nvn_stats.h"
#include "core.h"
#include "nvn_priv.h"

#include <pthread.h>
#include <stdlib.h>

#define OPS (NVN_CMD_COUNT + RL_NVN_SLOTS)
#define MAX_DRIVER 1024u

static const char *const kind_names[] = { "impl", "zero", "recorded", "missing", "driver" };

static struct {
    int on;
    char *path;
    bool hooked;
    pthread_mutex_t lock;
    uint64_t calls[RL_NVN_SLOTS];
    uint64_t lookups[RL_NVN_SLOTS];
    uint8_t kind[RL_NVN_SLOTS];
    const char *slot_name[RL_NVN_SLOTS];
    uint64_t ops[OPS][NVN_STATS_EVENTS];
    uint32_t ndriver;
    char *driver[MAX_DRIVER];
    uint64_t driver_lookups[MAX_DRIVER];
} st = { .on = -1, .lock = PTHREAD_MUTEX_INITIALIZER };

/** @brief The mark of the command this thread replays now (nvn_stats_mark). */
static _Thread_local uint8_t mark;

static void at_exit(void) { (void)nvn_stats_write(); }

static void hook(void)
{
    if (!st.hooked && st.path) {
        st.hooked = true;
        (void)rl_at_process_exit(at_exit);
    }
}

bool nvn_stats_on(void)
{
    int on = __atomic_load_n(&st.on, __ATOMIC_ACQUIRE);
    if (on >= 0)
        return on;
    pthread_mutex_lock(&st.lock);
    if (st.on < 0) {
        const char *p = getenv("RELIFT_NVN_STATS");
        st.path = p && *p ? strdup(p) : NULL;
        hook();
        __atomic_store_n(&st.on, st.path != NULL, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&st.lock);
    return st.on;
}

void nvn_stats_reset(const char *path, bool on)
{
    pthread_mutex_lock(&st.lock);
    free(st.path);
    st.path = path ? strdup(path) : NULL;
    for (uint32_t i = 0; i < st.ndriver; i++)
        free(st.driver[i]);
    st.ndriver = 0;
    memset(st.calls, 0, sizeof st.calls);
    memset(st.lookups, 0, sizeof st.lookups);
    memset(st.kind, 0, sizeof st.kind);
    memset(st.slot_name, 0, sizeof st.slot_name);
    memset(st.ops, 0, sizeof st.ops);
    memset(st.driver_lookups, 0, sizeof st.driver_lookups);
    hook();
    __atomic_store_n(&st.on, on, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&st.lock);
}

void nvn_stats_lookup(const char *name, nvn_stats_kind kind, uint32_t slot)
{
    if (!nvn_stats_on() || !name)
        return;
    pthread_mutex_lock(&st.lock);
    if (slot < RL_NVN_SLOTS) {
        st.lookups[slot]++;
        st.kind[slot] = (uint8_t)kind;
        st.slot_name[slot] = name;
    } else {
        uint32_t i = 0;
        while (i < st.ndriver && strcmp(st.driver[i], name))
            i++;
        if (i == st.ndriver && i < MAX_DRIVER && (st.driver[i] = strdup(name)) != NULL)
            st.ndriver++;
        if (i < st.ndriver)
            st.driver_lookups[i]++;
    }
    pthread_mutex_unlock(&st.lock);
}

void nvn_stats_call(uint32_t slot)
{
    if (slot < RL_NVN_SLOTS)
        __atomic_fetch_add(&st.calls[slot], 1, __ATOMIC_RELAXED);
}

void nvn_stats_cmd(uint32_t op, nvn_stats_event ev)
{
    if (op < OPS && ev < NVN_STATS_EVENTS && nvn_stats_on())
        __atomic_fetch_add(&st.ops[op][ev], 1, __ATOMIC_RELAXED);
}

int nvn_stats_mark(nvn_stats_event ev)
{
    if (ev == NVN_STATS_IGNORED || ev == NVN_STATS_PARTIAL)
        if (mark == NVN_STATS_OK || ev == NVN_STATS_PARTIAL)
            mark = (uint8_t)ev;
    return 0;
}

void nvn_stats_begin(void) { mark = NVN_STATS_OK; }

void nvn_stats_end(uint32_t op, int rc, bool unhandled)
{
    if (rc == 1)
        return;
    nvn_stats_cmd(op, unhandled ? NVN_STATS_UNHANDLED : rc ? NVN_STATS_STOPPED : (nvn_stats_event)mark);
}

uint64_t nvn_stats_count(uint32_t op, nvn_stats_event ev)
{
    return op < OPS && ev < NVN_STATS_EVENTS ? __atomic_load_n(&st.ops[op][ev], __ATOMIC_RELAXED) : 0;
}

uint64_t nvn_stats_calls(uint32_t slot)
{
    return slot < RL_NVN_SLOTS ? __atomic_load_n(&st.calls[slot], __ATOMIC_RELAXED) : 0;
}

void nvn_stats_print(FILE *f)
{
    fprintf(f, "# relift NVN stats for %s\n", nvn_version_name());
    fprintf(f, "# proc\tname\tkind\tlookups\tcalls\tflag\n");
    fprintf(f, "# cmd\tname\trecorded\tok\tignored\tpartial\tstopped\tunhandled\tflag\n");
    pthread_mutex_lock(&st.lock);
    for (uint32_t i = 0; i < RL_NVN_SLOTS; i++) {
        if (!st.slot_name[i])
            continue;
        uint8_t k = st.kind[i];
        bool gap = k == NVN_STATS_RECORDED || k == NVN_STATS_MISSING;
        fprintf(f, "proc\t%s\t%s\t%llu\t%llu\t%s\n", st.slot_name[i], kind_names[k], (unsigned long long)st.lookups[i],
                (unsigned long long)nvn_stats_calls(i), gap ? "gap" : "");
    }
    for (uint32_t i = 0; i < st.ndriver; i++)
        fprintf(f, "proc\t%s\tdriver\t%llu\t-\t\n", st.driver[i], (unsigned long long)st.driver_lookups[i]);
    pthread_mutex_unlock(&st.lock);
    for (uint32_t op = 0; op < OPS; op++) {
        uint64_t n[NVN_STATS_EVENTS], any = 0;
        for (uint32_t e = 0; e < NVN_STATS_EVENTS; e++)
            any |= n[e] = nvn_stats_count(op, (nvn_stats_event)e);
        if (!any)
            continue;
        bool gap = n[NVN_STATS_IGNORED] || n[NVN_STATS_PARTIAL] || n[NVN_STATS_STOPPED] || n[NVN_STATS_UNHANDLED];
        fprintf(f, "cmd\t%s", nvn_cmd_name(op));
        for (uint32_t e = 0; e < NVN_STATS_EVENTS; e++)
            fprintf(f, "\t%llu", (unsigned long long)n[e]);
        fprintf(f, "\t%s\n", gap ? "gap" : "");
    }
}

bool nvn_stats_write(void)
{
    if (!nvn_stats_on() || !st.path)
        return false;
    FILE *f = fopen(st.path, "w");
    if (!f)
        return false;
    nvn_stats_print(f);
    return fclose(f) == 0;
}
