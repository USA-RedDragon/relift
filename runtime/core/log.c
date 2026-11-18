/**
 * @file log.c
 * @brief Logging, report-once keys, the unknown-call reporter, environment
 * configuration and the clock.
 */
#include "core.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#else
#include <pthread.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#include <mach/mach.h>
#endif
#endif

static rl_config g_cfg = { false, false, RL_LOG_INFO, 0, 0, false };
#if defined(_WIN32)
static SRWLOCK g_log_lock = SRWLOCK_INIT;
static void log_lock(void) { AcquireSRWLockExclusive(&g_log_lock); }
static void log_unlock(void) { ReleaseSRWLockExclusive(&g_log_lock); }
#else
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static void log_lock(void) { pthread_mutex_lock(&g_log_lock); }
static void log_unlock(void) { pthread_mutex_unlock(&g_log_lock); }
#endif

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
    g_cfg.trace_os = env_flag("RELIFT_TRACE_OS");
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
    log_lock();
    fprintf(stderr, "[relift %s %s] ", names[level & 3], tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    log_unlock();
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
    log_lock();
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
    log_unlock();
    return fresh;
}

void rl_once_reset(void)
{
    log_lock();
    memset(g_once_used, 0, sizeof g_once_used);
    log_unlock();
}

bool rl_report_unknown(uint64_t key, const char *tag, const char *fmt, ...)
{
    if (rl_once(key)) {
        va_list ap;
        va_start(ap, fmt);
        rl_vlog(RL_LOG_WARN, tag, fmt, ap);
        va_end(ap);
    }
    return g_cfg.strict;
}

uint64_t rl_clock_ns(void)
{
#if defined(_WIN32)
    static LONGLONG freq;
    LARGE_INTEGER c;
    if (!freq) {
        LARGE_INTEGER q;
        QueryPerformanceFrequency(&q);
        freq = q.QuadPart;
    }
    QueryPerformanceCounter(&c);
    return (uint64_t)(((rl_u128)(uint64_t)c.QuadPart * 1000000000u) / (uint64_t)freq);
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static rl_guest_clock_fn *g_guest_clock;

void rl_set_guest_clock(rl_guest_clock_fn *fn) { __atomic_store_n(&g_guest_clock, fn, __ATOMIC_RELEASE); }

uint64_t rl_guest_clock_ns(void)
{
    rl_guest_clock_fn *fn = __atomic_load_n(&g_guest_clock, __ATOMIC_ACQUIRE);
    return fn ? fn() : rl_clock_ns();
}

uint64_t rl_ns_to_ticks(uint64_t ns)
{
    return (uint64_t)(((rl_u128)ns * rl_core_guest()->tick_hz) / 1000000000u);
}

uint64_t rl_ticks_to_ns(uint64_t ticks)
{
    return (uint64_t)(((rl_u128)ticks * 1000000000u) / rl_core_guest()->tick_hz);
}

uint64_t rl_os_memory_bytes(void)
{
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX pmc;
    return K32GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS *)&pmc, sizeof pmc) ? pmc.PrivateUsage : 0;
#elif defined(__APPLE__)
    task_vm_info_data_t info;
    mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
    return task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &n) == KERN_SUCCESS ? info.phys_footprint : 0;
#else
    unsigned long long pages = 0, resident = 0;
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        return 0;
    int got = fscanf(f, "%llu %llu", &pages, &resident);
    fclose(f);
    return got == 2 ? resident * rl_os_page_size() : 0;
#endif
}

uint32_t rl_os_page_size(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize ? si.dwPageSize : 4096u;
#else
    long p = sysconf(_SC_PAGESIZE);
    return p > 0 ? (uint32_t)p : 4096u;
#endif
}

bool rl_exe_path(char *buf, size_t n)
{
    char path[4096];
#if defined(_WIN32)
    DWORD len = GetModuleFileNameA(NULL, path, sizeof path);
    if (!len || len >= sizeof path)
        return false;
    for (DWORD i = 0; i < len; i++)
        if (path[i] == '\\')
            path[i] = '/';
#elif defined(__APPLE__)
    char raw[4096];
    uint32_t size = sizeof raw;
    if (_NSGetExecutablePath(raw, &size) != 0 || !realpath(raw, path))
        return false;
#else
    ssize_t len = readlink("/proc/self/exe", path, sizeof path - 1);
    if (len <= 0)
        return false;
    path[len] = 0;
#endif
    size_t m = strlen(path);
    if (m >= n)
        return false;
    memcpy(buf, path, m + 1);
    return true;
}

bool rl_exe_dir(char *buf, size_t n)
{
    if (!rl_exe_path(buf, n))
        return false;
    char *slash = strrchr(buf, '/');
    if (!slash)
        return false;
    if (slash == buf)
        slash++;
    *slash = 0;
    return true;
}
