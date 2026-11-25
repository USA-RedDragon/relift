/**
 * @file heapsample.c
 * @brief A sampling heap profiler for runs too large for a full tracer: an LD_PRELOAD library that takes the call
 * stack of one allocation in every RELIFT_HEAPSAMPLE_BYTES bytes allocated (default 1 MiB, so an allocation of N
 * bytes is sampled with odds N / that, a large one always), keeps each sampled block until it is freed, and on SIGUSR2
 * (or at exit) writes the call sites that hold the most live sampled bytes, each weighted back to an estimate of all
 * its live bytes, to RELIFT_HEAPSAMPLE_OUT.<n> (default heapsample.<pid>.<n>), with /proc/self/maps for symbolizing
 * (tools/heapsample/report.py). Two dumps taken a while apart in a steady scene name what grows.
 *
 * Build: cc -O2 -shared -fPIC -o heapsample.so tools/heapsample/heapsample.c -ldl
 * Run:   LD_PRELOAD=$PWD/heapsample.so <program>; kill -USR2 <pid> to dump. The diagnostic snapshot (core/snapshot.h)
 * finds rl_heapsample_dump(path) here and writes its heap.txt with it.
 *
 * Linux with glibc only. Allocations made while the sampler itself runs (backtrace's first call) are not sampled.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define DEPTH 48
#define SITES 65536u
#define LIVE (1u << 22)

typedef struct site {
    uint64_t hash;
    void *pc[DEPTH];
    int depth;
    uint64_t live, total, count;
} site;

typedef struct block {
    void *p;
    uint32_t site;
    uint64_t size;
} block;

static void *(*real_malloc)(size_t);
static void (*real_free)(void *);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static void *(*real_memalign)(size_t, size_t);
static int (*real_posix_memalign)(void **, size_t, size_t);
static void *(*real_aligned_alloc)(size_t, size_t);

static site *g_sites;
static block *g_live;
static uint64_t g_period = 1u << 20;
static _Atomic int64_t g_countdown = 1 << 20;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int t_inside;
static _Atomic int g_dump_req, g_dumps;
static int g_ready;

/** @brief A static arena for the calls dlsym makes before the real allocator is known. */
static char g_boot[1 << 16];
static size_t g_boot_used;

static void *boot_alloc(size_t n)
{
    n = (n + 15) & ~(size_t)15;
    if (g_boot_used + n > sizeof g_boot)
        return NULL;
    void *p = g_boot + g_boot_used;
    g_boot_used += n;
    return p;
}

static int is_boot(void *p) { return (char *)p >= g_boot && (char *)p < g_boot + sizeof g_boot; }

static void *map_zero(size_t n)
{
    void *p = mmap(NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

static void dump(void);

static void on_usr2(int sig)
{
    (void)sig;
    atomic_store(&g_dump_req, 1);
}

static void init(void)
{
    static int busy;
    if (g_ready || busy)
        return;
    busy = 1;
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_free = dlsym(RTLD_NEXT, "free");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_memalign = dlsym(RTLD_NEXT, "memalign");
    real_posix_memalign = dlsym(RTLD_NEXT, "posix_memalign");
    real_aligned_alloc = dlsym(RTLD_NEXT, "aligned_alloc");
    const char *e = getenv("RELIFT_HEAPSAMPLE_BYTES");
    if (e && atoll(e) > 0)
        g_period = (uint64_t)atoll(e);
    atomic_store(&g_countdown, (int64_t)g_period);
    g_sites = map_zero(sizeof(site) * SITES);
    g_live = map_zero(sizeof(block) * LIVE);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_usr2;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR2, &sa, NULL);
    g_ready = 1;
    busy = 0;
}

static uint32_t live_slot(void *p) { return (uint32_t)(((uintptr_t)p >> 4) * 0x9E3779B97F4A7C15ull >> 42) & (LIVE - 1); }

/**
 * @brief Records the sampled block p of size bytes at the caller's stack, standing for max(size, period) bytes.
 * Under g_lock.
 */
static void sample(void *p, size_t size)
{
    void *pc[DEPTH + 2];
    int n = backtrace(pc, DEPTH + 2);
    int skip = n > 2 ? 2 : 0;
    uint64_t h = 1469598103934665603ull;
    for (int i = skip; i < n; i++)
        h = (h ^ (uintptr_t)pc[i]) * 1099511628211ull;
    uint32_t s = (uint32_t)(h % SITES);
    for (uint32_t k = 0; k < SITES; k++, s = (s + 1) % SITES) {
        if (g_sites[s].hash == h || !g_sites[s].hash)
            break;
    }
    site *st = &g_sites[s];
    if (!st->hash) {
        st->hash = h;
        st->depth = n - skip;
        memcpy(st->pc, pc + skip, sizeof(void *) * (size_t)st->depth);
    }
    uint64_t w = size > g_period ? size : g_period;
    st->live += w;
    st->total += w;
    st->count++;
    uint32_t b = live_slot(p);
    for (uint32_t k = 0; k < LIVE; k++, b = (b + 1) & (LIVE - 1))
        if (!g_live[b].p) {
            g_live[b] = (block){ p, s, w };
            return;
        }
}

/** @brief Drops the sampled block p, keeping each later entry of its probe chain reachable. Under g_lock. */
static void unsample(void *p)
{
    uint32_t b = live_slot(p);
    for (uint32_t k = 0; k < LIVE; k++, b = (b + 1) & (LIVE - 1)) {
        if (!g_live[b].p)
            return;
        if (g_live[b].p == p) {
            g_sites[g_live[b].site].live -= g_live[b].size;
            uint32_t hole = b;
            g_live[hole].p = NULL;
            for (uint32_t j = (hole + 1) & (LIVE - 1); g_live[j].p; j = (j + 1) & (LIVE - 1)) {
                uint32_t home = live_slot(g_live[j].p);
                if (((j - home) & (LIVE - 1)) >= ((j - hole) & (LIVE - 1))) {
                    g_live[hole] = g_live[j];
                    g_live[j].p = NULL;
                    hole = j;
                }
            }
            return;
        }
    }
}

static void after_alloc(void *p, size_t size)
{
    if (!p || !g_ready || t_inside)
        return;
    if (atomic_fetch_sub(&g_countdown, (int64_t)size) - (int64_t)size > 0 && size < g_period) {
        if (atomic_load(&g_dump_req))
            goto maybe_dump;
        return;
    }
    atomic_store(&g_countdown, (int64_t)g_period);
    t_inside = 1;
    pthread_mutex_lock(&g_lock);
    sample(p, size);
    pthread_mutex_unlock(&g_lock);
    t_inside = 0;
maybe_dump:
    if (atomic_exchange(&g_dump_req, 0)) {
        t_inside = 1;
        dump();
        t_inside = 0;
    }
}

static void before_free(void *p)
{
    if (!p || !g_ready || t_inside)
        return;
    t_inside = 1;
    pthread_mutex_lock(&g_lock);
    unsample(p);
    pthread_mutex_unlock(&g_lock);
    t_inside = 0;
}

static int by_live(const void *a, const void *b)
{
    const site *x = *(site *const *)a, *y = *(site *const *)b;
    return x->live < y->live ? 1 : x->live > y->live ? -1 : 0;
}

/** @brief Writes the dump to path; false when the file cannot be made. */
static bool dump_to(const char *path)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0)
        return false;
    static site *order[SITES];
    pthread_mutex_lock(&g_lock);
    uint32_t m = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < SITES; i++)
        if (g_sites[i].hash && g_sites[i].live) {
            order[m++] = &g_sites[i];
            total += g_sites[i].live;
        }
    qsort(order, m, sizeof order[0], by_live);
    dprintf(fd, "heapsample period %llu bytes; live estimate %llu bytes over %u sites\n",
            (unsigned long long)g_period, (unsigned long long)total, m);
    for (uint32_t i = 0; i < m && i < 60; i++) {
        site *s = order[i];
        dprintf(fd, "site %u live %llu total %llu samples %llu\n", i, (unsigned long long)s->live,
                (unsigned long long)s->total, (unsigned long long)s->count);
        for (int k = 0; k < s->depth; k++)
            dprintf(fd, "  %p\n", s->pc[k]);
    }
    pthread_mutex_unlock(&g_lock);
    dprintf(fd, "maps\n");
    int mf = open("/proc/self/maps", O_RDONLY);
    if (mf >= 0) {
        char buf[65536];
        ssize_t r;
        while ((r = read(mf, buf, sizeof buf)) > 0)
            if (write(fd, buf, (size_t)r) != r)
                break;
        close(mf);
    }
    close(fd);
    return true;
}

static void dump(void)
{
    char path[512];
    const char *out = getenv("RELIFT_HEAPSAMPLE_OUT");
    int n = atomic_fetch_add(&g_dumps, 1);
    if (out)
        snprintf(path, sizeof path, "%s.%d", out, n);
    else
        snprintf(path, sizeof path, "heapsample.%d.%d", (int)getpid(), n);
    dump_to(path);
}

/**
 * @brief Writes a dump to path now, on the calling thread (the diagnostic snapshot looks this symbol up with dlsym);
 * returns 0 when written, -1 when not.
 */
__attribute__((visibility("default"))) int rl_heapsample_dump(const char *path)
{
    if (!g_ready)
        return -1;
    int was = t_inside;
    t_inside = 1;
    bool ok = dump_to(path);
    t_inside = was;
    return ok ? 0 : -1;
}

void *malloc(size_t n)
{
    if (!real_malloc) {
        init();
        if (!real_malloc)
            return boot_alloc(n);
    }
    void *p = real_malloc(n);
    after_alloc(p, n);
    return p;
}

void *calloc(size_t a, size_t b)
{
    if (!real_calloc) {
        init();
        if (!real_calloc)
            return boot_alloc(a * b);
    }
    void *p = real_calloc(a, b);
    after_alloc(p, a * b);
    return p;
}

void *realloc(void *p, size_t n)
{
    if (is_boot(p)) {
        void *q = malloc(n);
        if (q)
            memcpy(q, p, n);
        return q;
    }
    if (!real_realloc)
        init();
    before_free(p);
    void *q = real_realloc(p, n);
    after_alloc(q, n);
    return q;
}

void free(void *p)
{
    if (!p || is_boot(p))
        return;
    if (!real_free)
        init();
    before_free(p);
    real_free(p);
}

void *memalign(size_t a, size_t n)
{
    if (!real_memalign)
        init();
    void *p = real_memalign(a, n);
    after_alloc(p, n);
    return p;
}

int posix_memalign(void **out, size_t a, size_t n)
{
    if (!real_posix_memalign)
        init();
    int rc = real_posix_memalign(out, a, n);
    if (!rc)
        after_alloc(*out, n);
    return rc;
}

void *aligned_alloc(size_t a, size_t n)
{
    if (!real_aligned_alloc)
        init();
    void *p = real_aligned_alloc(a, n);
    after_alloc(p, n);
    return p;
}

__attribute__((destructor)) static void at_exit_dump(void)
{
    if (g_ready && getenv("RELIFT_HEAPSAMPLE_AT_EXIT")) {
        t_inside = 1;
        dump();
    }
}
