/**
 * @file log.c
 * @brief Logging, report-once keys, the unknown-call reporter, environment
 * configuration, the clock and the program's own files (executable path,
 * shipped assets).
 */
#include "core.h"
#include "gdbstub.h"

#include <stdarg.h>
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

void rl_log_fork(int step)
{
    switch (step) {
    case RL_FORK_PREPARE:
        log_lock();
        break;
    case RL_FORK_PARENT:
        log_unlock();
        break;
    default:
#if defined(_WIN32)
        InitializeSRWLock(&g_log_lock);
#else
        pthread_mutex_init(&g_log_lock, NULL);
#endif
        break;
    }
}

/** @brief Size of the report-once key set (open addressing). */
#define ONCE_SLOTS 4096u
static uint64_t g_once[ONCE_SLOTS];
static bool g_once_used[ONCE_SLOTS];

static bool g_level_read;
#if defined(RL_DEBUG_STUB)
/** @brief The last log lines, for the diagnostic snapshot (rl_log_tail); guarded by the log lock. */
#define RING_LINES 512u
#define RING_COLS 320
static char g_ring[RING_LINES][RING_COLS];
static size_t g_ring_next;
#endif

rl_config *rl_cfg(void) { return &g_cfg; }

static bool env_flag(const char *name)
{
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") != 0 && strcmp(v, "false") != 0;
}

/** @brief Sets the log level from `RELIFT_LOG`. */
static void read_log_level(void)
{
    const char *v = getenv("RELIFT_LOG");
    g_cfg.log_level = RL_LOG_INFO;
    if (v) {
        if (!strcmp(v, "debug"))
            g_cfg.log_level = RL_LOG_DEBUG;
        else if (!strcmp(v, "warn"))
            g_cfg.log_level = RL_LOG_WARN;
        else if (!strcmp(v, "error"))
            g_cfg.log_level = RL_LOG_ERROR;
    }
    __atomic_store_n(&g_level_read, true, __ATOMIC_RELEASE);
}

void rl_config_load(void)
{
    const char *v;
    g_cfg.strict = env_flag("RELIFT_STRICT");
    g_cfg.trace_os = env_flag("RELIFT_TRACE_OS");
    read_log_level();
    v = getenv("RELIFT_HOST_PAGE");
    g_cfg.host_page = v ? (uint32_t)strtoul(v, NULL, 0) : 0;
    v = getenv("RELIFT_ADDRESS_BITS");
    g_cfg.address_bits = v ? (uint32_t)strtoul(v, NULL, 0) : 0;
    g_cfg.loaded = true;
}

void rl_vlog(int level, const char *tag, const char *fmt, va_list ap)
{
    static const char *const names[] = { "debug", "info", "warn", "error" };
    if (!__atomic_load_n(&g_level_read, __ATOMIC_ACQUIRE)) {
        log_lock();
        if (!g_level_read)
            read_log_level();
        log_unlock();
    }
    if (level < g_cfg.log_level)
        return;
    log_lock();
#if defined(RL_DEBUG_STUB)
    /* The diagnostic snapshot (snapshot.h) keeps the last lines: formatted once, written to both places. */
    va_list again;
    va_copy(again, ap);
    char *slot = g_ring[g_ring_next++ % RING_LINES];
    int head = snprintf(slot, RING_COLS, "[relift %s %s] ", names[level & 3], tag);
    if (head > 0 && head < RING_COLS)
        vsnprintf(slot + head, (size_t)(RING_COLS - head), fmt, again);
    va_end(again);
#endif
    fprintf(stderr, "[relift %s %s] ", names[level & 3], tag);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    fflush(stderr);
    log_unlock();
}

#if defined(RL_DEBUG_STUB)
size_t rl_log_tail(char *out, size_t size, size_t lines)
{
    log_lock();
    size_t have = g_ring_next < RING_LINES ? g_ring_next : RING_LINES;
    if (lines > have)
        lines = have;
    size_t o = 0;
    for (size_t i = g_ring_next - lines; i < g_ring_next; i++) {
        const char *l = g_ring[i % RING_LINES];
        size_t n = strlen(l);
        if (o + n + 2 > size)
            break;
        memcpy(out + o, l, n);
        out[o + n] = '\n';
        o += n + 1;
    }
    log_unlock();
    if (o < size)
        out[o] = 0;
    return o;
}
#endif

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

uint32_t rl_os_cpu_count(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1u;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1u;
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

static rl_asset_loader g_asset_loader;

void rl_set_asset_loader(rl_asset_loader fn) { g_asset_loader = fn; }

static void *read_whole_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *data = NULL;
    long n = fseek(f, 0, SEEK_END) == 0 ? ftell(f) : -1;
    if (n >= 0 && fseek(f, 0, SEEK_SET) == 0) {
        data = malloc(n ? (size_t)n : 1);
        if (data && fread(data, 1, (size_t)n, f) != (size_t)n) {
            free(data);
            data = NULL;
        }
    }
    fclose(f);
    if (data)
        *len = (size_t)n;
    return data;
}

void *rl_asset_load(const char *name, size_t *len)
{
    static const char *const roots[] = { "%s/%s", "%s/../share/relift/%s", "%s/../Resources/%s" };
    char exe[1024], path[4096];
    if (rl_exe_dir(exe, sizeof exe))
        for (size_t i = 0; i < sizeof roots / sizeof *roots; i++) {
            snprintf(path, sizeof path, roots[i], exe, name);
            void *data = read_whole_file(path, len);
            if (data)
                return data;
        }
    return g_asset_loader ? g_asset_loader(name, len) : NULL;
}
