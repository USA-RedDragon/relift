/**
 * @file log.c
 * @brief Logging, report-once keys, environment configuration and the clock.
 */
#include "core.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static rl_config g_cfg = { false, false, RL_LOG_INFO, 0, 0, false };
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

/** @brief Size of the report-once key set (open addressing). */
#define ONCE_SLOTS 4096u
static uint64_t g_once[ONCE_SLOTS];
static bool g_once_used[ONCE_SLOTS];

rl_config *rl_cfg(void) { return &g_cfg; }

static bool env_flag(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") != 0 && strcmp(v, "false") != 0;
}

void rl_config_load(void)
{
    const char *v;
    g_cfg.strict = env_flag("RELIFT_STRICT");
    g_cfg.trace_svc = env_flag("RELIFT_TRACE_SVC");
    g_cfg.log_level = RL_LOG_INFO;
    v = getenv("RELIFT_LOG");
    if (v) {
        if (!strcmp(v, "debug"))
            g_cfg.log_level = RL_LOG_DEBUG;
        else if (!strcmp(v, "warn"))
            g_cfg.log_level = RL_LOG_WARN;
        else if (!strcmp(v, "error"))
            g_cfg.log_level = RL_LOG_ERROR;
    }
    v = getenv("RELIFT_HOST_PAGE");
    g_cfg.host_page = v ? (uint32_t)strtoul(v, NULL, 0) : 0;
    v = getenv("RELIFT_ADDRESS_BITS");
    g_cfg.address_bits = v ? (uint32_t)strtoul(v, NULL, 0) : 0;
    g_cfg.loaded = true;
}

void rl_vlog(int level, const char *tag, const char *fmt, va_list ap)
{
    static const char *const names[] = { "debug", "info", "warn", "error" };
    if (level < g_cfg.log_level)
        return;
    pthread_mutex_lock(&g_log_lock);
    fprintf(stderr, "[relift %s %s] ", names[level & 3], tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    pthread_mutex_unlock(&g_log_lock);
}

void rl_log(int level, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    rl_vlog(level, tag, fmt, ap);
    va_end(ap);
}

bool rl_once(uint64_t key)
{
    uint64_t h = key * 0x9E3779B97F4A7C15ull;
    bool fresh = false;
    pthread_mutex_lock(&g_log_lock);
    for (uint32_t i = 0; i < ONCE_SLOTS; i++) {
        uint32_t s = (uint32_t)((h >> 40) + i) & (ONCE_SLOTS - 1);
        if (!g_once_used[s]) {
            g_once_used[s] = true;
            g_once[s] = key;
            fresh = true;
            break;
        }
        if (g_once[s] == key)
            break;
    }
    pthread_mutex_unlock(&g_log_lock);
    return fresh;
}

void rl_once_reset(void)
{
    pthread_mutex_lock(&g_log_lock);
    memset(g_once_used, 0, sizeof g_once_used);
    pthread_mutex_unlock(&g_log_lock);
}

uint64_t rl_clock_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

uint64_t rl_ns_to_ticks(uint64_t ns)
{
    return (uint64_t)(((rl_u128)ns * 12u) / 625u);
}

uint64_t rl_ticks_to_ns(uint64_t ticks)
{
    return (uint64_t)(((rl_u128)ticks * 625u) / 12u);
}

uint64_t rl_mrs_cntpct(rl_ctx *c)
{
    (void)c;
    return rl_ns_to_ticks(rl_clock_ns());
}

uint32_t rl_os_page_size(void)
{
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (uint32_t)p : 4096u;
}
