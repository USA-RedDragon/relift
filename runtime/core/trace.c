/**
 * @file trace.c
 * @brief The frame trace (relift/trace.h): the file, the kinds filter, the
 * clock and the line format, shared by every guest.
 */
#include "core.h"

#include <relift/settings.h>
#include <relift/trace.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

static int g_state;
static FILE *g_file;
static char g_kinds[64];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t (*g_thread_id)(void);

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000ll + ts.tv_nsec;
}

bool rl_trace_on(void)
{
    int s = __atomic_load_n(&g_state, __ATOMIC_ACQUIRE);
    if (s)
        return s > 0;
    pthread_mutex_lock(&g_lock);
    if (!g_state) {
        const char *spec = rl_setting_str("stats.trace");
        char path[1024] = { 0 };
        const char *bar = spec ? strchr(spec, '|') : NULL;
        if (spec)
            snprintf(path, sizeof path, "%.*s", bar ? (int)(bar - spec) : (int)strlen(spec), spec);
        if (bar)
            snprintf(g_kinds, sizeof g_kinds, "%s", bar + 1);
        g_file = path[0] ? fopen(path, "w") : NULL;
        if (g_file)
            setvbuf(g_file, NULL, _IOFBF, 1u << 20);
        else if (path[0])
            rl_log(RL_LOG_WARN, "stats", "frame trace: cannot write %s", path);
        __atomic_store_n(&g_state, g_file ? 1 : -1, __ATOMIC_RELEASE);
    }
    s = g_state;
    pthread_mutex_unlock(&g_lock);
    return s > 0;
}

int64_t rl_trace_now(void) { return rl_trace_on() ? mono_ns() : 0; }

bool rl_trace_kind_listed(const char *kinds, char kind) { return !kinds[0] || (kind && strchr(kinds, kind)); }

/** @brief Whether the trace is on and writes lines of kind. */
static bool traced(char kind) { return rl_trace_on() && rl_trace_kind_listed(g_kinds, kind); }

void rl_trace_cpu(char kind, uint64_t a)
{
    if (!traced(kind))
        return;
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    unsigned long long wait = 0;
#if defined(__linux__)
    unsigned long long run = 0;
    FILE *f = fopen("/proc/thread-self/schedstat", "r");
    if (f) {
        if (fscanf(f, "%llu %llu", &run, &wait) != 2)
            wait = 0;
        fclose(f);
    }
#endif
    rl_trace(kind, a, (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec, wait);
}

void rl_trace(char kind, uint64_t a, uint64_t b, uint64_t c)
{
    if (!traced(kind))
        return;
    int64_t t = mono_ns();
    uint64_t (*idf)(void) = __atomic_load_n(&g_thread_id, __ATOMIC_ACQUIRE);
    uint64_t id = idf ? idf() : 0;
    pthread_mutex_lock(&g_lock);
    fprintf(g_file, "%c %lld %llu %llx %llx %llx\n", kind, (long long)t, (unsigned long long)id, (unsigned long long)a,
            (unsigned long long)b, (unsigned long long)c);
    if (kind == 'V')
        fflush(g_file);
    pthread_mutex_unlock(&g_lock);
}

void rl_trace_set_thread_id(uint64_t (*fn)(void)) { __atomic_store_n(&g_thread_id, fn, __ATOMIC_RELEASE); }
