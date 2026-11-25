/**
 * @file compile_check.c
 * @brief Makes every program of a shader table on the local Vulkan device, the way the launch warm-up does, outside
 * any game, and names each program the driver crashes on, hangs on or refuses.
 *
 * The work goes through nvn_warm_check (nvn_warm.c), which is the warm-up's own path: the same module patching, layouts,
 * specialization constants and library state, so a program that kills the driver here kills it there. It runs in child
 * processes over chunks of the table (the tool starts itself again with --child), so a driver crash or hang takes down a
 * child, not the tool. A child keeps a state file the tool reads after it dies: the programs it had finished and the ones
 * in flight. The in-flight programs are the suspects; each is made alone in a fresh child to confirm it, and when none
 * confirms (a crash that needs the programs around it, or a driver thread that died later) the suspects are made one at
 * a time on one thread, which names the program that kills it by itself. The programs a dead child left unstarted go
 * back in the queue.
 *
 * Each child makes the NVN2 device the warm-up uses, with only the layout of the texture set
 * (nvn_texset_layout_only), and still takes a few hundred MiB to 2 GiB of memory depending on the driver. --jobs is
 * the most children; the tool starts one, measures its peak (VmHWM) once it has made a program, and starts more only
 * while MemAvailable, less what the running children will still grow by, keeps --reserve-mb (4096 on Android, 2048
 * elsewhere) free (nvn_check_children_fit). With less than that free and nothing running it refuses to start.
 *
 * Usage: relift_compile_check TABLE_DIR [--device DEVICE_JSON] [--chunk N] [--jobs N] [--threads N] [--timeout S]
 * [--reserve-mb M] [--work DIR], or --self-test [--work DIR]. TABLE_DIR holds table.rlspv, table.rlspi and table.stamp, or a build directory whose shaders/ folder
 * does. Exit status: 0 when every program was made, 1 when some were not, 2 on a usage or setup error.
 */
#include "nvn_priv.h"
#include "nvn_module.h"
#include "nvn_pipe.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

/** @brief The most threads a child runs, and so the most in-flight slots a state file has. */
#define MAX_THREADS 64u

/** @brief The state file of a child: progress the tool reads once the child is gone. */
typedef struct state {
    _Atomic uint32_t done;
    /** @brief Per thread, the table index being made plus one, 0 when idle. */
    _Atomic uint32_t slot[MAX_THREADS];
    /** @brief Per program of the range (first to first + count), 0 not started, 1 started, 2 finished. */
    _Atomic uint8_t prog[];
} state;

/** @brief The options of a run. */
typedef struct opts {
    const char *dir, *device, *work;
    uint32_t chunk, jobs, threads, timeout;
    /** @brief Memory kept free while children run, in MiB (--reserve-mb). */
    uint32_t reserve_mb;
} opts;

/** @brief One program the driver did not take. */
typedef struct bad {
    uint32_t index;
    char key[65];
    char what[40];
    char why[220];
    char spv[1100];
    char stage[16];
} bad;

static const char *g_self;

/** @brief Seconds on the monotonic clock. */
static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/** @brief Sleeps for ms milliseconds. */
static void nap(long ms)
{
    struct timespec ts = { .tv_sec = ms / 1000, .tv_nsec = (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

/** @brief Whether path is a regular file. */
static bool is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

/** @brief A kB value of a "Name:   N kB" line in file path, or fallback when it has none. */
static uint64_t proc_kb(const char *path, const char *name, uint64_t fallback)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return fallback;
    char line[256];
    size_t n = strlen(name);
    uint64_t v = fallback;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, name, n) && line[n] == ':') {
            v = strtoull(line + n + 1, NULL, 10);
            break;
        }
    fclose(f);
    return v;
}

/** @brief The memory the system has available, in bytes (MemAvailable), or UINT64_MAX when it does not say. */
static uint64_t mem_available(void)
{
    uint64_t kb = proc_kb("/proc/meminfo", "MemAvailable", UINT64_MAX);
    return kb == UINT64_MAX ? kb : kb * 1024;
}

/** @brief A child's resident memory now (VmRSS) or at its peak (VmHWM), in bytes; 0 when unknown. */
static uint64_t child_mem(pid_t pid, const char *which)
{
    char path[64];
    snprintf(path, sizeof path, "/proc/%d/status", (int)pid);
    return proc_kb(path, which, 0) * 1024;
}

/** @brief Sets the environment both the tool and its children read the table and the device through. */
static void set_env(const opts *o)
{
    char path[1200];
    setenv("RELIFT_NVN_TABLES", o->dir, 1);
    snprintf(path, sizeof path, "%s/shaders", o->work);
    setenv("RELIFT_NVN_SHADERS", path, 1);
    snprintf(path, sizeof path, "%s/cache", o->work);
    setenv("RELIFT_PIPELINE_CACHE", path, 1);
    setenv("RELIFT_NVN_TABLE_STAMP", "allow", 1);
    setenv("RELIFT_VIDEO", "headless", 1);
}

/** @brief The work a child thread does: the programs from first to first + count, taken in order. */
typedef struct work {
    uint32_t first, count, nthreads;
    _Atomic uint32_t next;
    state *st;
    FILE *fail;
    pthread_mutex_t lock;
} work;

typedef struct arg {
    work *w;
    uint32_t id;
} arg;

/**
 * @brief A self-test aid: RELIFT_COMPILE_CHECK_FAULT=<index>:crash or <index>:hang makes the child die or stall on that
 * table index, standing in for a driver that does, so the tool's naming of such a program can be tested.
 */
static void inject_fault(uint32_t idx)
{
    const char *f = getenv("RELIFT_COMPILE_CHECK_FAULT");
    if (!f || (uint32_t)strtoul(f, NULL, 10) != idx)
        return;
    const char *kind = strchr(f, ':');
    if (kind && !strcmp(kind + 1, "crash"))
        raise(SIGSEGV);
    else if (kind && !strcmp(kind + 1, "hang"))
        for (;;)
            pause();
}

/** @brief A child thread: makes programs until the range is done, recording the ones the driver refuses. */
static void *child_thread(void *p)
{
    arg *a = p;
    work *w = a->w;
    for (;;) {
        uint32_t i = atomic_fetch_add(&w->next, 1);
        if (i >= w->count)
            return NULL;
        uint32_t idx = w->first + i;
        char key[65], why[300];
        if (!nvn_shader_key_at(idx, key))
            return NULL;
        atomic_store(&w->st->prog[i], 1);
        atomic_store(&w->st->slot[a->id], idx + 1);
        inject_fault(idx);
        bool ok = nvn_warm_check(key, why, sizeof why);
        if (!ok) {
            pthread_mutex_lock(&w->lock);
            fprintf(w->fail, "%u\t%s\t%s\n", idx, key, why);
            fflush(w->fail);
            pthread_mutex_unlock(&w->lock);
        }
        atomic_store(&w->st->prog[i], 2);
        atomic_store(&w->st->slot[a->id], 0);
        atomic_fetch_add(&w->st->done, 1);
    }
}

/** @brief The child: makes the programs first to first + count on nthreads threads, state in statefile. */
static int run_child(const char *statefile, uint32_t first, uint32_t count, uint32_t nthreads)
{
    nvn_texset_layout_only();
    int fd = open(statefile, O_RDWR);
    if (fd < 0)
        return 2;
    size_t len = sizeof(state) + count;
    state *st = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (st == MAP_FAILED)
        return 2;
    char failpath[1300];
    snprintf(failpath, sizeof failpath, "%s.fail", statefile);
    work w = { .first = first, .count = count, .nthreads = nthreads, .st = st, .lock = PTHREAD_MUTEX_INITIALIZER };
    w.fail = fopen(failpath, "w");
    if (!w.fail)
        return 2;
    pthread_t t[MAX_THREADS];
    arg args[MAX_THREADS];
    for (uint32_t i = 0; i < nthreads; i++) {
        args[i] = (arg){ &w, i };
        if (pthread_create(&t[i], NULL, child_thread, &args[i]) != 0) {
            nthreads = i;
            break;
        }
    }
    for (uint32_t i = 0; i < nthreads; i++)
        pthread_join(t[i], NULL);
    fclose(w.fail);
    return 0;
}

/** @brief A child the tool started: its process, its state and what range it makes. */
typedef struct job {
    pid_t pid;
    uint32_t first, count, threads;
    char statefile[1300];
    state *st;
    double started, progressed;
    uint32_t last_done;
    bool hung, active;
    /** @brief Made one program at a time on purpose, to confirm or find a suspect. */
    bool solo;
    /** @brief The child's peak resident memory, read while it runs. */
    uint64_t peak;
} job;

/** @brief The outcome of a finished child, and what the tool does about it. */
typedef struct report {
    bad *list;
    uint32_t n, cap;
} report;

/** @brief Runs of table indices still to make. */
typedef struct range {
    uint32_t first, count;
    bool solo;
} range;

typedef struct queue {
    range *r;
    uint32_t n, cap;
} queue;

static void push_range(queue *q, uint32_t first, uint32_t count, bool solo)
{
    if (!count)
        return;
    if (q->n == q->cap) {
        q->cap = q->cap ? q->cap * 2 : 64;
        q->r = realloc(q->r, q->cap * sizeof *q->r);
        if (!q->r)
            exit(2);
    }
    q->r[q->n++] = (range){ first, count, solo };
}

/** @brief The stage name of a program's SPIR-V, and the file it is written to for the report. */
static void dump_spv(const opts *o, bad *b)
{
    size_t n = 0;
    uint32_t *w = nvn_shader_spirv(b->key, &n);
    snprintf(b->stage, sizeof b->stage, "?");
    b->spv[0] = 0;
    if (!w)
        return;
    int stage;
    static const char *const names[] = { "vertex", "fragment", "geometry", "tess-ctrl", "tess-eval", "compute" };
    if (nvn_spirv_stage(w, n, &stage) && stage >= 0 && stage < 6)
        snprintf(b->stage, sizeof b->stage, "%s", names[stage]);
    char dir[1200];
    snprintf(dir, sizeof dir, "%s/spv", o->work);
    mkdir(dir, 0777);
    snprintf(b->spv, sizeof b->spv, "%s/%s.spv", dir, b->key);
    FILE *f = fopen(b->spv, "wb");
    if (f) {
        fwrite(w, 4, n, f);
        fclose(f);
    } else {
        b->spv[0] = 0;
    }
    free(w);
}

/** @brief Notes a program the driver did not take, once per index. */
static void note_bad(const opts *o, report *r, uint32_t idx, const char *what, const char *why)
{
    for (uint32_t i = 0; i < r->n; i++)
        if (r->list[i].index == idx)
            return;
    if (r->n == r->cap) {
        r->cap = r->cap ? r->cap * 2 : 32;
        r->list = realloc(r->list, r->cap * sizeof *r->list);
        if (!r->list)
            exit(2);
    }
    bad *b = &r->list[r->n++];
    memset(b, 0, sizeof *b);
    b->index = idx;
    nvn_shader_key_at(idx, b->key);
    snprintf(b->what, sizeof b->what, "%s", what);
    snprintf(b->why, sizeof b->why, "%s", why);
    dump_spv(o, b);
    printf("%s: program %s (table index %u, %s) %s; spirv %s\n", what, b->key, idx, b->stage, why,
           b->spv[0] ? b->spv : "unavailable");
    fflush(stdout);
}

/** @brief A name for how a child ended. */
static void describe(int status, bool hung, char *out, size_t n)
{
    if (hung)
        snprintf(out, n, "hang (killed after no progress)");
    else if (WIFSIGNALED(status))
        snprintf(out, n, "the driver crashed: signal %d (%s)", WTERMSIG(status), strsignal(WTERMSIG(status)));
    else
        snprintf(out, n, "the child exited with status %d", WEXITSTATUS(status));
}

/** @brief Starts a child over a range; false when it cannot. */
static bool start_job(const opts *o, job *j, uint32_t first, uint32_t count, bool solo, uint32_t serial)
{
    memset(j, 0, sizeof *j);
    j->first = first;
    j->count = count;
    j->solo = solo;
    j->threads = solo ? 1 : o->threads;
    if (j->threads > count)
        j->threads = count;
    snprintf(j->statefile, sizeof j->statefile, "%s/state-%u", o->work, serial);
    int fd = open(j->statefile, O_RDWR | O_CREAT | O_TRUNC, 0666);
    size_t len = sizeof(state) + count;
    if (fd < 0 || ftruncate(fd, (off_t)len) != 0)
        return false;
    j->st = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (j->st == MAP_FAILED)
        return false;
    char a1[16], a2[16], a3[16];
    snprintf(a1, sizeof a1, "%u", first);
    snprintf(a2, sizeof a2, "%u", count);
    snprintf(a3, sizeof a3, "%u", j->threads);
    fflush(stdout);
    pid_t pid = fork();
    if (pid < 0)
        return false;
    if (pid == 0) {
        char logpath[1300];
        snprintf(logpath, sizeof logpath, "%s.log", j->statefile);
        int lf = open(logpath, O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (lf >= 0) {
            dup2(lf, 1);
            dup2(lf, 2);
            close(lf);
        }
        execl(g_self, g_self, "--child", o->dir, j->statefile, a1, a2, a3, (char *)NULL);
        _exit(127);
    }
    j->pid = pid;
    j->started = j->progressed = now_s();
    j->active = true;
    return true;
}

/** @brief Reads the failure lines a child wrote into the report. */
static void read_failures(const opts *o, const job *j, report *r)
{
    char path[1300];
    snprintf(path, sizeof path, "%s.fail", j->statefile);
    FILE *f = fopen(path, "r");
    char line[600];
    while (f && fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        char *why = tab ? strchr(tab + 1, '\t') : NULL;
        if (!why)
            continue;
        *why++ = 0;
        why[strcspn(why, "\n")] = 0;
        note_bad(o, r, (uint32_t)strtoul(line, NULL, 10), "refused", why);
    }
    if (f)
        fclose(f);
}

/** @brief Handles a finished or killed child: its failures, then its suspects and unstarted programs. */
static void finish_job(const opts *o, job *j, int status, report *r, queue *q, uint32_t *confirmed_flag)
{
    read_failures(o, j, r);
    bool clean = !j->hung && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (clean)
        return;
    char how[200];
    describe(status, j->hung, how, sizeof how);
    uint32_t suspects = 0, run_start = 0;
    bool in_run = false;
    for (uint32_t i = 0; i <= j->count; i++) {
        uint8_t s = i < j->count ? atomic_load(&j->st->prog[i]) : 9;
        bool pending = s == 0;
        if (pending && !in_run) {
            in_run = true;
            run_start = i;
        } else if (!pending && in_run) {
            in_run = false;
            push_range(q, j->first + run_start, i - run_start, j->solo);
        }
        if (s == 1)
            suspects++;
    }
    if (j->solo || suspects <= 1) {
        for (uint32_t i = 0; i < j->count; i++)
            if (atomic_load(&j->st->prog[i]) == 1) {
                note_bad(o, r, j->first + i, j->hung ? "hang" : "crash", how);
                *confirmed_flag = 1;
            }
        if (!suspects && !j->solo)
            printf("child over %u..%u ended (%s) with no program in flight; its unstarted programs are queued\n",
                   j->first, j->first + j->count - 1, how);
        return;
    }
    printf("child over %u..%u ended (%s) with %u programs in flight; making each alone to name it\n", j->first,
           j->first + j->count - 1, how, suspects);
    for (uint32_t i = 0; i < j->count; i++)
        if (atomic_load(&j->st->prog[i]) == 1)
            push_range(q, j->first + i, 1, true);
}

/** @brief The device.json this device wrote into the work folder, compared with the one the table was made from. */
static void compare_device(const opts *o)
{
    char mine[1300], theirs[1300];
    snprintf(mine, sizeof mine, "%s/shaders/device.json", o->work);
    if (o->device)
        snprintf(theirs, sizeof theirs, "%s", o->device);
    else
        snprintf(theirs, sizeof theirs, "%s/device.json", o->dir);
    if (!is_file(mine) || !is_file(theirs)) {
        printf("device: no device.json to compare (%s%s)\n", is_file(mine) ? "" : "this device's not written; ",
               is_file(theirs) ? "" : "the table's not found");
        return;
    }
    FILE *a = fopen(mine, "rb"), *b = fopen(theirs, "rb");
    bool same = a && b;
    while (same) {
        int ca = fgetc(a), cb = fgetc(b);
        if (ca != cb)
            same = false;
        if (ca == EOF || cb == EOF)
            break;
    }
    if (a)
        fclose(a);
    if (b)
        fclose(b);
    printf("device: %s is %s this device's capabilities (%s)\n", theirs, same ? "the same as" : "different from", mine);
}

/** @brief The make-everything driver: queues the table in chunks and runs them over o->jobs children. */
static int run_check(const opts *o, report *r)
{
    set_env(o);
    char path[1300];
    snprintf(path, sizeof path, "%s/shaders", o->work);
    mkdir(o->work, 0777);
    mkdir(path, 0777);
    snprintf(path, sizeof path, "%s/cache", o->work);
    mkdir(path, 0777);
    uint32_t n = nvn_shader_count();
    if (!n) {
        fprintf(stderr, "no shader table in %s (table.rlspv)\n", o->dir);
        return 2;
    }
    printf("compile-check: %u programs from %s, chunks of %u, %u children, %u threads each, work %s\n", n, o->dir,
           o->chunk, o->jobs, o->threads, o->work);
    queue q = { 0 };
    for (uint32_t at = 0; at < n; at += o->chunk)
        push_range(&q, at, n - at < o->chunk ? n - at : o->chunk, false);
    /* Take from the end so the front of the table goes first. */
    for (uint32_t i = 0; i < q.n / 2; i++) {
        range t = q.r[i];
        q.r[i] = q.r[q.n - 1 - i];
        q.r[q.n - 1 - i] = t;
    }
    job *jobs = calloc(o->jobs, sizeof *jobs);
    uint32_t serial = 0, total_done = 0, confirmed = 0;
    double t0 = now_s(), last_print = t0;
    /* A child's footprint: the largest peak of a child that has made a program (its device and tables are up). */
    uint64_t footprint = 0, reserve = (uint64_t)o->reserve_mb << 20;
    bool told = false;
    for (;;) {
        bool any = false;
        uint32_t running = 0;
        uint64_t growth = 0;
        for (uint32_t k = 0; k < o->jobs; k++) {
            job *j = &jobs[k];
            if (!j->active)
                continue;
            running++;
            uint64_t hwm = child_mem(j->pid, "VmHWM"), rss = child_mem(j->pid, "VmRSS");
            if (hwm > j->peak)
                j->peak = hwm;
            if (atomic_load(&j->st->done) && j->peak > footprint)
                footprint = j->peak;
            if (footprint > rss)
                growth += footprint - rss;
        }
        uint64_t avail = mem_available();
        avail = avail > growth ? avail - growth : 0;
        uint32_t may = nvn_check_children_fit(avail, reserve, footprint, running, o->jobs);
        if (!running && !may && q.n) {
            fprintf(stderr,
                    "compile-check: refused to start a child: %llu MiB available, %u MiB must stay free (--reserve-mb)%s\n",
                    (unsigned long long)(avail >> 20), o->reserve_mb, footprint ? "" : ", and a child's size is not known yet");
            return 2;
        }
        if (footprint && !told) {
            told = true;
            printf("compile-check: a child takes %llu MiB; %llu MiB available, %u MiB kept free\n",
                   (unsigned long long)(footprint >> 20), (unsigned long long)(mem_available() >> 20), o->reserve_mb);
        }
        for (uint32_t k = 0; k < o->jobs; k++) {
            job *j = &jobs[k];
            if (j->active) {
                any = true;
                int status;
                pid_t w = waitpid(j->pid, &status, WNOHANG);
                uint32_t d = atomic_load(&j->st->done);
                if (d != j->last_done) {
                    j->last_done = d;
                    j->progressed = now_s();
                }
                if (w == 0 && now_s() - j->progressed > o->timeout) {
                    j->hung = true;
                    kill(j->pid, SIGKILL);
                    w = waitpid(j->pid, &status, 0);
                }
                if (w == j->pid) {
                    j->active = false;
                    total_done += atomic_load(&j->st->done);
                    finish_job(o, j, status, r, &q, &confirmed);
                    munmap(j->st, sizeof(state) + j->count);
                }
            }
            if (!j->active && q.n && may) {
                may--;
                range rg = q.r[--q.n];
                if (!start_job(o, j, rg.first, rg.count, rg.solo, serial++)) {
                    fprintf(stderr, "cannot start a child: %s\n", strerror(errno));
                    return 2;
                }
                any = true;
            }
        }
        if (!any && !q.n)
            break;
        if (now_s() - last_print > 15) {
            uint32_t live = total_done;
            for (uint32_t k = 0; k < o->jobs; k++)
                if (jobs[k].active)
                    live += atomic_load(&jobs[k].st->done);
            printf("progress: %u of %u made, %u problems, %.0f s\n", live, n, r->n, now_s() - t0);
            fflush(stdout);
            last_print = now_s();
        }
        nap(100);
    }
    printf("compile-check done: %u programs, %u not taken, %.0f s\n", n, r->n, now_s() - t0);
    compare_device(o);
    snprintf(path, sizeof path, "%s/report.txt", o->work);
    FILE *f = fopen(path, "w");
    if (f) {
        for (uint32_t i = 0; i < r->n; i++)
            fprintf(f, "%s\t%s\t%u\t%s\t%s\t%s\n", r->list[i].what, r->list[i].key, r->list[i].index,
                    r->list[i].stage, r->list[i].why, r->list[i].spv);
        fclose(f);
        printf("report: %s\n", path);
    }
    return r->n ? 1 : 0;
}


/** @brief A valid vertex program (a vec4 position passed through), the one program of the self-test the driver takes. */
static const uint32_t good_spv[] = {
    0x07230203, 0x00010000, 0x0008000b, 0x0000001b, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000d, 0x00000011, 0x00000015,
    0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00060005,
    0x0000000b, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000000b, 0x00000000,
    0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000000b, 0x00000001, 0x505f6c67, 0x746e696f,
    0x657a6953, 0x00000000, 0x00070006, 0x0000000b, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369,
    0x0065636e, 0x00070006, 0x0000000b, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e,
    0x00030005, 0x0000000d, 0x00000000, 0x00030005, 0x00000011, 0x00736f70, 0x00030005, 0x00000015,
    0x006c6f63, 0x00030047, 0x0000000b, 0x00000002, 0x00050048, 0x0000000b, 0x00000000, 0x0000000b,
    0x00000000, 0x00050048, 0x0000000b, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000000b,
    0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000000b, 0x00000003, 0x0000000b, 0x00000004,
    0x00040047, 0x00000011, 0x0000001e, 0x00000000, 0x00040047, 0x00000015, 0x0000001e, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040015, 0x00000008, 0x00000020, 0x00000000,
    0x0004002b, 0x00000008, 0x00000009, 0x00000001, 0x0004001c, 0x0000000a, 0x00000006, 0x00000009,
    0x0006001e, 0x0000000b, 0x00000007, 0x00000006, 0x0000000a, 0x0000000a, 0x00040020, 0x0000000c,
    0x00000003, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000003, 0x00040015, 0x0000000e,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000e, 0x0000000f, 0x00000000, 0x00040020, 0x00000010,
    0x00000001, 0x00000007, 0x0004003b, 0x00000010, 0x00000011, 0x00000001, 0x00040020, 0x00000013,
    0x00000003, 0x00000007, 0x0004003b, 0x00000013, 0x00000015, 0x00000003, 0x0004002b, 0x00000006,
    0x00000017, 0x3f000000, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8,
    0x00000005, 0x0004003d, 0x00000007, 0x00000012, 0x00000011, 0x00050041, 0x00000013, 0x00000014,
    0x0000000d, 0x0000000f, 0x0003003e, 0x00000014, 0x00000012, 0x0004003d, 0x00000007, 0x00000016,
    0x00000011, 0x0005008e, 0x00000007, 0x00000018, 0x00000016, 0x00000017, 0x00070050, 0x00000007,
    0x00000019, 0x00000017, 0x00000017, 0x00000017, 0x00000017, 0x00050081, 0x00000007, 0x0000001a,
    0x00000018, 0x00000019, 0x0003003e, 0x00000015, 0x0000001a, 0x000100fd, 0x00010038,
};

/** @brief Writes a 40-byte table entry (key of 32 bytes made of byte, offset, words) at e. */
static void put_entry(uint8_t *e, uint8_t byte, uint32_t off, uint32_t words)
{
    memset(e, byte, 32);
    memcpy(e + 32, &off, 4);
    memcpy(e + 36, &words, 4);
}

/**
 * @brief The self-test: plants a table of four programs in the work folder (a valid one, a module that is not
 * SPIR-V, one the child crashes on and one it hangs on, by RELIFT_COMPILE_CHECK_FAULT) and checks that the run names
 * the last three by the right kind and takes the first. Exit 77 when this machine has no Vulkan device.
 */
static int self_test(opts *o)
{
    static const uint32_t bad_spv[] = { 0x07230203, 0x00010000, 0xdeadbeef, 0x12345678, 0xffffffff };
    const uint32_t *mods[4] = { good_spv, bad_spv, good_spv, good_spv };
    const uint32_t words[4] = { sizeof good_spv / 4, sizeof bad_spv / 4, sizeof good_spv / 4, sizeof good_spv / 4 };
    char dir[1300], path[1400];
    snprintf(dir, sizeof dir, "%s/selftest-table", o->work);
    mkdir(o->work, 0777);
    mkdir(dir, 0777);
    snprintf(path, sizeof path, "%s/table.rlspv", dir);
    FILE *f = fopen(path, "wb");
    if (!f)
        return 2;
    uint8_t head[12 + 4 * 40] = { 'R', 'L', 'S', 'P', 'V', '0', '0', '1', 4 };
    uint32_t off = sizeof head;
    for (uint32_t i = 0; i < 4; i++) {
        put_entry(head + 12 + 40 * i, (uint8_t)(0x10 * (i + 1)), off, words[i]);
        off += 4 * words[i];
    }
    fwrite(head, 1, sizeof head, f);
    for (uint32_t i = 0; i < 4; i++)
        fwrite(mods[i], 4, words[i], f);
    fclose(f);
    o->dir = dir;
    o->chunk = 4;
    o->jobs = 2;
    o->threads = 1;
    o->timeout = 5;
    setenv("RELIFT_COMPILE_CHECK_FAULT", "2:crash", 1);
    report r = { 0 };
    int rc = run_check(o, &r);
    unsetenv("RELIFT_COMPILE_CHECK_FAULT");
    /* The hang is checked in a second run, as the fault hook names one program at a time. */
    report h = { 0 };
    setenv("RELIFT_COMPILE_CHECK_FAULT", "3:hang", 1);
    o->timeout = 3;
    int rc2 = run_check(o, &h);
    unsetenv("RELIFT_COMPILE_CHECK_FAULT");
    for (uint32_t i = 0; i < r.n; i++)
        if (strstr(r.list[i].why, "no host Vulkan")) {
            printf("self-test skipped: no host Vulkan device\n");
            return 77;
        }
    bool bad_named = false, crash_named = false, hang_named = false, good_clean = true;
    for (uint32_t i = 0; i < r.n; i++) {
        bad_named |= r.list[i].index == 1 && !strcmp(r.list[i].what, "refused");
        crash_named |= r.list[i].index == 2 && !strcmp(r.list[i].what, "crash");
        good_clean &= r.list[i].index != 0;
    }
    for (uint32_t i = 0; i < h.n; i++)
        hang_named |= h.list[i].index == 3 && !strcmp(h.list[i].what, "hang");
    printf("self-test: bad module %s, crash %s, hang %s, valid module %s (exit %d, %d)\n",
           bad_named ? "named" : "MISSED", crash_named ? "named" : "MISSED", hang_named ? "named" : "MISSED",
           good_clean ? "taken" : "REFUSED", rc, rc2);
    return bad_named && crash_named && hang_named && good_clean && rc == 1 && rc2 == 1 ? 0 : 1;
}

/** @brief Reads a field of this process's /proc/self/status in KiB (VmHWM: peak resident memory), 0 when it has none. */
static uint64_t status_kb(const char *field)
{
    FILE *f = fopen("/proc/self/status", "r");
    char line[256];
    uint64_t v = 0;
    size_t n = strlen(field);
    while (f && fgets(line, sizeof line, f))
        if (!strncmp(line, field, n) && line[n] == ':')
            v = strtoull(line + n + 1, NULL, 10);
    if (f)
        fclose(f);
    return v;
}

/**
 * @brief --one KEY: makes the one program whose key starts with KEY in this process, and prints its compile time and
 * this process's peak resident memory (VmHWM); with RELIFT_PIPE_STATS set to the key, the driver's own statistics of
 * its pipeline library (instruction counts) are logged too. The measure of one shader's cost to a driver.
 */
static int run_one(opts *o, const char *prefix)
{
    char tp[1300], dir[1300], wk[PATH_MAX], dk[PATH_MAX];
    if (!o->dir)
        return 2;
    snprintf(tp, sizeof tp, "%s/table.rlspv", o->dir);
    if (!is_file(tp)) {
        snprintf(dir, sizeof dir, "%s/shaders", o->dir);
        o->dir = dir;
    }
    mkdir(o->work, 0777);
    if (realpath(o->work, wk))
        o->work = wk;
    if (realpath(o->dir, dk))
        o->dir = dk;
    set_env(o);
    nvn_texset_layout_only();
    char key[65], why[300];
    for (uint32_t i = 0; nvn_shader_key_at(i, key); i++) {
        if (strncmp(key, prefix, strlen(prefix)))
            continue;
        uint64_t before = status_kb("VmHWM");
        nvn_pipe_stats_for(key);
        double t0 = now_s();
        bool ok = nvn_warm_check(key, why, sizeof why);
        double dt = now_s() - t0;
        printf("one %s index %u: %s in %.3f s, peak resident %llu MiB (%llu MiB before it), now %llu MiB\n", key, i,
               ok ? "made" : why, dt, (unsigned long long)(status_kb("VmHWM") / 1024),
               (unsigned long long)(before / 1024), (unsigned long long)(status_kb("VmRSS") / 1024));
        return ok ? 0 : 1;
    }
    fprintf(stderr, "no program with key %s\n", prefix);
    return 2;
}

/** @brief Parses a number option; exits with a usage error when it is not one. */
static uint32_t number(const char *name, const char *v, uint32_t lo, uint32_t hi)
{
    char *end;
    unsigned long x = strtoul(v, &end, 10);
    if (*end || x < lo || x > hi) {
        fprintf(stderr, "%s wants a number from %u to %u, not '%s'\n", name, lo, hi, v);
        exit(2);
    }
    return (uint32_t)x;
}

int main(int argc, char **argv)
{
    static char self[PATH_MAX];
    ssize_t sn = readlink("/proc/self/exe", self, sizeof self - 1);
    if (sn > 0) {
        self[sn] = 0;
        g_self = self;
    } else if (realpath(argv[0], self)) {
        g_self = self;
    } else {
        g_self = argv[0];
    }
    if (argc == 7 && !strcmp(argv[1], "--child")) {
        opts o = { .dir = argv[2], .work = NULL };
        char work[1300];
        snprintf(work, sizeof work, "%s", argv[3]);
        char *slash = strrchr(work, '/');
        if (slash)
            *slash = 0;
        o.work = work;
        set_env(&o);
        return run_child(argv[3], (uint32_t)strtoul(argv[4], NULL, 10), (uint32_t)strtoul(argv[5], NULL, 10),
                         (uint32_t)strtoul(argv[6], NULL, 10));
    }
#if defined(__ANDROID__)
    const uint32_t reserve_mb = 4096;
#else
    const uint32_t reserve_mb = 2048;
#endif
    opts o = { .chunk = 64, .jobs = 4, .threads = 1, .timeout = 180, .work = "compile-check-work", .reserve_mb = reserve_mb };
    char dir[1300];
    bool selftest = false;
    const char *one = NULL;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        bool has = i + 1 < argc;
        if (!strcmp(a, "--device") && has)
            o.device = argv[++i];
        else if (!strcmp(a, "--work") && has)
            o.work = argv[++i];
        else if (!strcmp(a, "--chunk") && has)
            o.chunk = number(a, argv[++i], 1, 1u << 20);
        else if (!strcmp(a, "--jobs") && has)
            o.jobs = number(a, argv[++i], 1, 256);
        else if (!strcmp(a, "--threads") && has)
            o.threads = number(a, argv[++i], 1, MAX_THREADS);
        else if (!strcmp(a, "--timeout") && has)
            o.timeout = number(a, argv[++i], 1, 86400);
        else if (!strcmp(a, "--reserve-mb") && has)
            o.reserve_mb = number(a, argv[++i], 0, 1u << 20);
        else if (!strcmp(a, "--self-test"))
            selftest = true;
        else if (!strcmp(a, "--one") && has)
            one = argv[++i];
        else if (a[0] != '-' && !o.dir)
            o.dir = a;
        else {
            fprintf(stderr,
                    "usage: %s TABLE_DIR [--device DEVICE_JSON] [--chunk N] [--jobs N] [--threads N] [--timeout S] "
                    "[--reserve-mb M] [--work DIR]\n",
                    argv[0]);
            return 2;
        }
    }
    if (selftest)
        return self_test(&o);
    if (one)
        return run_one(&o, one);
    if (!o.dir) {
        fprintf(stderr, "usage: %s TABLE_DIR [--device DEVICE_JSON] [--chunk N] [--jobs N] [--threads N] [--timeout S] "
                        "[--reserve-mb M] [--work DIR]\n", argv[0]);
        return 2;
    }
    char tp[1300];
    snprintf(tp, sizeof tp, "%s/table.rlspv", o.dir);
    if (!is_file(tp)) {
        snprintf(tp, sizeof tp, "%s/shaders/table.rlspv", o.dir);
        if (is_file(tp)) {
            snprintf(dir, sizeof dir, "%s/shaders", o.dir);
            o.dir = dir;
        }
    }
    char wk[PATH_MAX], dk[PATH_MAX];
    mkdir(o.work, 0777);
    if (realpath(o.work, wk))
        o.work = wk;
    if (realpath(o.dir, dk))
        o.dir = dk;
    report r = { 0 };
    return run_check(&o, &r);
}
