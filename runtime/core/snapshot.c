/**
 * @file snapshot.c
 * @brief The diagnostic snapshot (snapshot.h): the trigger, the window of presents, the core's own files and the
 * providers' hooks. Built only where the debug stub is (gdbstub.h); elsewhere the entry points do nothing.
 */
#include "snapshot.h"

#include "gdbstub.h"

#if defined(RL_DEBUG_STUB)

#include "png.h"
#include "present.h"

#include <relift/settings.h>

#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif
#if defined(__linux__) || defined(__ANDROID__)
#include <dirent.h>
#endif

/** @brief Most providers, and most files an index lists. */
#define MAX_PROVIDERS 8u
#define MAX_FILES 64u
/** @brief Log lines the snapshot keeps (rl_log_tail keeps 512). */
#define LOG_LINES 500u
/** @brief How often the snapshot-now file is looked for. */
#define POLL_NS 500000000ull

typedef struct file_note {
    char name[96];
    char what[400];
} file_note;

static struct {
    pthread_mutex_t lock;
    rl_snapshot_provider prov[MAX_PROVIDERS];
    uint32_t nprov;
    /** @brief Set by rl_snapshot_request and by the file poll; read on every present. */
    _Atomic int requested;
    /** @brief The window is open (read without the lock by rl_snapshot_active). */
    _Atomic int active;
    uint32_t window, seen;
    uint64_t present_no, started_ns;
    _Atomic uint64_t presents, last_poll_ns;
    char dir[1100], last_dir[1100], root[1024];
    file_note files[MAX_FILES];
    uint32_t nfiles;
    /** @brief Settings value read at the start, so a change mid-window does not matter. */
    bool root_set;
} S = { .lock = PTHREAD_MUTEX_INITIALIZER };

void rl_snapshot_register(const rl_snapshot_provider *p)
{
    pthread_mutex_lock(&S.lock);
    uint32_t i = 0;
    while (i < S.nprov && strcmp(S.prov[i].name, p->name))
        i++;
    if (i < MAX_PROVIDERS) {
        S.prov[i] = *p;
        if (i == S.nprov)
            S.nprov++;
    }
    pthread_mutex_unlock(&S.lock);
}

void rl_snapshot_file(const char *name, const char *what)
{
    if (S.nfiles < MAX_FILES) {
        snprintf(S.files[S.nfiles].name, sizeof S.files[0].name, "%s", name);
        snprintf(S.files[S.nfiles].what, sizeof S.files[0].what, "%s", what);
        S.nfiles++;
    }
}

void rl_snapshot_request(void)
{
    if (rl_setting_int("debug.snapshot") > 0)
        atomic_store(&S.requested, 1);
}

bool rl_snapshot_active(void) { return atomic_load(&S.active) != 0; }

const char *rl_snapshot_last_dir(void) { return S.last_dir; }

void rl_snapshot_set_root(const char *dir)
{
    pthread_mutex_lock(&S.lock);
    S.root_set = dir && *dir;
    snprintf(S.root, sizeof S.root, "%s", dir ? dir : "");
    pthread_mutex_unlock(&S.lock);
}

/** @brief The folder that holds snapshots/ and snapshot-now: save.dir, else the app's user folder, else ".". */
static void root_dir(char *out, size_t n)
{
    if (S.root_set) {
        snprintf(out, n, "%s", S.root);
        return;
    }
    const char *save = rl_setting_str("save.dir");
    char user[900];
    const char *app = rl_settings_app();
    if (save && *save)
        snprintf(out, n, "%s", save);
    else if (rl_settings_user_dir(user, sizeof user, app && *app ? app : "relift"))
        snprintf(out, n, "%s", user);
    else
        snprintf(out, n, ".");
}

static void make_dirs(const char *path)
{
    char buf[1200];
    snprintf(buf, sizeof buf, "%s", path);
    for (char *s = buf + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            mkdir(buf, 0755);
            *s = '/';
        }
    mkdir(buf, 0755);
}

/** @brief Opens dir/name for writing and announces it; NULL when it cannot be made. */
static FILE *out_file(const char *name, const char *what)
{
    char path[1300];
    snprintf(path, sizeof path, "%s/%s", S.dir, name);
    FILE *f = fopen(path, "w");
    if (!f) {
        rl_log(RL_LOG_WARN, "snapshot", "cannot write %s (%s)", path, strerror(errno));
        return NULL;
    }
    rl_snapshot_file(name, what);
    return f;
}

static void put_info(void *user, const char *key, const char *value) { fprintf((FILE *)user, "%s: %s\n", key, value); }

static void write_build(void)
{
    FILE *f = out_file("build.txt", "The build stamp: the version, the game, the code mods and the build digest the program registered.");
    if (!f)
        return;
    fprintf(f, "snapshot present %llu, window %u presents\n", (unsigned long long)S.present_no, S.window);
    rl_report_info_each(put_info, f);
    fclose(f);
}

static void write_settings(void)
{
    FILE *f = out_file("settings.txt", "Every setting's effective value and where it came from (default, env or set).");
    if (!f)
        return;
    rl_settings_dump(f);
    fclose(f);
}

static void write_log(void)
{
    static char text[LOG_LINES * 330u];
    size_t n = rl_log_tail(text, sizeof text, LOG_LINES);
    FILE *f = out_file("log_tail.txt", "The last 500 log lines before the snapshot ended, oldest first.");
    if (!f)
        return;
    fwrite(text, 1, n, f);
    fclose(f);
}

static void write_picture(void)
{
    uint32_t w = 0, h = 0;
    uint8_t *px = rl_present_last_picture(2000, &w, &h);
    char path[1300];
    snprintf(path, sizeof path, "%s/picture.png", S.dir);
    if (px && rl_png_write_rgb(path, w, h, px, (size_t)w * 4, false))
        rl_snapshot_file("picture.png", "The picture the window showed when the snapshot ended.");
    else
        rl_log(RL_LOG_WARN, "snapshot", "no picture: the process has no presenter with a frame to read back");
    free(px);
}

#if defined(__linux__) || defined(__ANDROID__)
typedef struct map_rss {
    char name[200];
    uint64_t rss_kb, size_kb;
} map_rss;

static int by_rss(const void *a, const void *b)
{
    const map_rss *x = a, *y = b;
    return x->rss_kb < y->rss_kb ? 1 : x->rss_kb > y->rss_kb ? -1 : 0;
}

/** @brief Appends a file's text to out, up to max bytes. */
static void cat_file(FILE *out, const char *path, const char *title)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return;
    fprintf(out, "== %s\n", title);
    char line[512];
    while (fgets(line, sizeof line, f))
        fputs(line, out);
    fclose(f);
}

/** @brief The largest mappings by resident size, grouped by their name (the file or [heap], anonymous ones together). */
static void top_maps(FILE *out)
{
    FILE *f = fopen("/proc/self/smaps", "r");
    if (!f)
        return;
    static map_rss list[512];
    uint32_t n = 0;
    char line[600], name[200] = "";
    uint64_t size = 0;
    while (fgets(line, sizeof line, f)) {
        unsigned long long lo, hi, v;
        char perms[8], rest[400];
        int got = sscanf(line, "%llx-%llx %7s %*s %*s %*s %399[^\n]", &lo, &hi, perms, rest);
        if (got >= 3 && strchr(line, '-') && line[0] != 'R' && line[0] != 'S') {
            snprintf(name, sizeof name, "%s", got == 4 ? rest : "[anonymous]");
            size = (hi - lo) / 1024;
            continue;
        }
        if (sscanf(line, "Rss: %llu kB", &v) == 1) {
            uint32_t i = 0;
            while (i < n && strcmp(list[i].name, name))
                i++;
            if (i == n && n < 512) {
                snprintf(list[n].name, sizeof list[0].name, "%s", name);
                list[n].rss_kb = list[n].size_kb = 0;
                n++;
            }
            if (i < n) {
                list[i].rss_kb += v;
                list[i].size_kb += size;
            }
        }
    }
    fclose(f);
    qsort(list, n, sizeof list[0], by_rss);
    fprintf(out, "== largest mappings by resident size (name, resident kB, mapped kB)\n");
    for (uint32_t i = 0; i < n && i < 40; i++)
        fprintf(out, "%10llu %12llu  %s\n", (unsigned long long)list[i].rss_kb, (unsigned long long)list[i].size_kb,
                list[i].name);
}
#endif

static void write_memory(void)
{
    FILE *f = out_file("memory.txt", "The process's memory from the host: the kernel's totals, its mappings by resident size and the allocator's counters.");
    if (!f)
        return;
#if defined(__linux__) || defined(__ANDROID__)
    cat_file(f, "/proc/self/status", "/proc/self/status");
    cat_file(f, "/proc/self/smaps_rollup", "/proc/self/smaps_rollup");
    top_maps(f);
#else
    fprintf(f, "memory totals are read from /proc, which this host does not have\n");
#endif
#if defined(__GLIBC__) && defined(__GLIBC_PREREQ)
#if __GLIBC_PREREQ(2, 33)
    struct mallinfo2 mi = mallinfo2();
    fprintf(f, "== allocator (glibc mallinfo2)\narena %zu in-use %zu free %zu mmapped %zu\n", mi.arena, mi.uordblks,
            mi.fordblks, mi.hblkhd);
#endif
#endif
    fclose(f);
}

static void write_heap(void)
{
    char path[1300];
    snprintf(path, sizeof path, "%s/heap.txt", S.dir);
    /* The heapsample library (tools/heapsample), when preloaded, exports the call sites holding the most live bytes. */
    int (*dump)(const char *) = (int (*)(const char *))dlsym(RTLD_DEFAULT, "rl_heapsample_dump");
    if (dump && dump(path) == 0) {
        rl_snapshot_file("heap.txt", "The heapsample dump: the call sites that hold the most live heap bytes (read it with tools/heapsample/report.py).");
        return;
    }
    FILE *f = out_file("heap.txt", "Heap call sites: not available in this run (the heapsample library was not preloaded).");
    if (!f)
        return;
    fprintf(f, "heapsample is not loaded. Start the program with LD_PRELOAD=<heapsample.so> (build: tools/heapsample/heapsample.c)\n"
               "to get the call sites that hold the live heap; memory.txt has the totals.\n");
    fclose(f);
}

static void write_host_threads(void)
{
    FILE *f = out_file("threads_host.txt", "Every host thread: its id, name, kernel state, what it sleeps in and the CPU time it used.");
    if (!f)
        return;
#if defined(__linux__) || defined(__ANDROID__)
    DIR *d = opendir("/proc/self/task");
    struct dirent *e;
    fprintf(f, "%8s %-16s %-5s %-26s %s\n", "tid", "name", "state", "waiting in (wchan)", "cpu ticks (user+system)");
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char p[128], line[512] = "", comm[64] = "?", wchan[64] = "-";
        char state = '?';
        unsigned long long ut = 0, st = 0;
        snprintf(p, sizeof p, "/proc/self/task/%s/stat", e->d_name);
        FILE *s = fopen(p, "r");
        if (s) {
            if (fgets(line, sizeof line, s)) {
                char *rp = strrchr(line, ')');
                char *lp = strchr(line, '(');
                if (lp && rp && rp > lp) {
                    snprintf(comm, sizeof comm, "%.*s", (int)(rp - lp - 1) < 63 ? (int)(rp - lp - 1) : 63, lp + 1);
                    sscanf(rp + 2, "%c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &state, &ut, &st);
                }
            }
            fclose(s);
        }
        snprintf(p, sizeof p, "/proc/self/task/%s/wchan", e->d_name);
        s = fopen(p, "r");
        if (s) {
            if (!fgets(wchan, sizeof wchan, s))
                strcpy(wchan, "-");
            fclose(s);
        }
        wchan[strcspn(wchan, "\n")] = 0;
        fprintf(f, "%8s %-16s %-5c %-26s %llu\n", e->d_name, comm, state, wchan[0] ? wchan : "-", ut + st);
    }
    if (d)
        closedir(d);
#else
    fprintf(f, "this host has no /proc to list threads from\n");
#endif
    fclose(f);
}

static void write_index(void)
{
    char path[1300];
    snprintf(path, sizeof path, "%s/index.txt", S.dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "relift diagnostic snapshot, present %llu, covering the %u presents after the trigger.\n"
               "Folder: %s\n\nFiles:\n", (unsigned long long)S.present_no, S.window, S.dir);
    fprintf(f, "  index.txt\n      This list.\n");
    for (uint32_t i = 0; i < S.nfiles; i++)
        fprintf(f, "  %s\n      %s\n", S.files[i].name, S.files[i].what);
    fclose(f);
}

static void begin_window(void)
{
    char root[1024], stamp[64];
    root_dir(root, sizeof root);
    time_t t = time(NULL);
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(stamp, sizeof stamp, "%Y%m%d-%H%M%S", &tm);
    uint64_t pn = S.present_no;
    snprintf(S.dir, sizeof S.dir, "%s/snapshots/%s-p%llu", root, stamp, (unsigned long long)pn);
    make_dirs(S.dir);
    S.window = (uint32_t)rl_setting_int("debug.snapshot");
    S.seen = 0;
    S.nfiles = 0;
    S.started_ns = rl_clock_ns();
    atomic_store(&S.active, 1);
    rl_log(RL_LOG_INFO, "snapshot", "snapshot started at present %llu: %u presents into %s", (unsigned long long)pn, S.window, S.dir);
    for (uint32_t i = 0; i < S.nprov; i++)
        if (S.prov[i].begin)
            S.prov[i].begin(S.prov[i].user, S.dir, S.window);
}

static void end_window(void)
{
    for (uint32_t i = 0; i < S.nprov; i++)
        if (S.prov[i].end)
            S.prov[i].end(S.prov[i].user, S.dir);
    write_picture();
    write_settings();
    write_build();
    write_memory();
    write_heap();
    write_host_threads();
    write_log();
    write_index();
    atomic_store(&S.active, 0);
    snprintf(S.last_dir, sizeof S.last_dir, "%s", S.dir);
    rl_log(RL_LOG_INFO, "snapshot", "snapshot written to %s (%u files, %.1f s)", S.dir, S.nfiles + 1,
           (double)(rl_clock_ns() - S.started_ns) / 1e9);
}

/** @brief Looks for the snapshot-now file (at most every half second); removes it and returns true when found. */
static bool poll_file(uint64_t now)
{
    if (now - atomic_load(&S.last_poll_ns) < POLL_NS)
        return false;
    atomic_store(&S.last_poll_ns, now);
    if (rl_setting_int("debug.snapshot") <= 0)
        return false;
    char root[1024], path[1100];
    root_dir(root, sizeof root);
    snprintf(path, sizeof path, "%s/snapshot-now", root);
    struct stat st;
    if (stat(path, &st) != 0)
        return false;
    remove(path);
    return true;
}

void rl_snapshot_present(void)
{
    uint64_t n = atomic_fetch_add_explicit(&S.presents, 1, memory_order_relaxed) + 1;
    uint64_t now = rl_clock_ns();
    bool active = atomic_load_explicit(&S.active, memory_order_relaxed) != 0;
    if (!active && !atomic_load_explicit(&S.requested, memory_order_relaxed) && now - atomic_load_explicit(&S.last_poll_ns, memory_order_relaxed) < POLL_NS)
        return;
    pthread_mutex_lock(&S.lock);
    S.present_no = n;
    if (!S.active) {
        bool go = atomic_exchange(&S.requested, 0) != 0;
        if (!go)
            go = poll_file(now);
        if (go && rl_setting_int("debug.snapshot") > 0)
            begin_window();
    } else {
        S.seen++;
        for (uint32_t i = 0; i < S.nprov; i++)
            if (S.prov[i].present)
                S.prov[i].present(S.prov[i].user, S.dir, S.seen);
        if (S.seen >= S.window)
            end_window();
    }
    pthread_mutex_unlock(&S.lock);
}

#else

void rl_snapshot_register(const rl_snapshot_provider *p) { (void)p; }
void rl_snapshot_file(const char *name, const char *what) { (void)name, (void)what; }
void rl_snapshot_request(void) {}
void rl_snapshot_present(void) {}
bool rl_snapshot_active(void) { return false; }
void rl_snapshot_set_root(const char *dir) { (void)dir; }
const char *rl_snapshot_last_dir(void) { return ""; }

#endif
