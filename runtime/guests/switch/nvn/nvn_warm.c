/**
 * @file nvn_warm.c
 * @brief The shader warm-up: the host driver's compile of the programs a game
 * uses, done by worker threads ahead of the draws that need them.
 *
 * A draw's pipeline depends on state the game sets as it draws, but the
 * expensive part of it, the driver lowering each shader stage to GPU code,
 * depends only on the stage's module and specialization constants. The
 * warm-up makes that part (nvn_pipe.h: a pre-raster library of a vertex
 * program, a fragment library of a fragment program, a compute pipeline of a
 * compute program, whose workgroup size the game's control section holds), so
 * a draw only links libraries.
 *
 * Three things feed it:
 *
 * - The programs the game registers (nvnProgramSetShaders), as it registers
 *   them: a game builds its programs when it loads, well before the first
 *   draw that uses them, so the compile runs beside the loading.
 * - The programs and pipelines earlier launches recorded (nvn_pipe.h), made
 *   at the first submit before the game goes on, so a later launch has every
 *   program of a play ready before the game asks for any (and finds them in
 *   the driver's pipeline cache).
 * - With RELIFT_PIPELINE_WARM=all, every program of the build's shader
 *   table, at the first submit.
 *
 * RELIFT_PIPELINE_WARM is "registered" (the default: the first two),
 * "all" (also the table, and the first submit returns when it is done),
 * "all-background" (the first submit returns at once and a draw that needs
 * a pipeline nobody has made waits for the warm-up to end) or "off".
 *
 * A draw that needs a library or pipeline a worker is making waits for it;
 * one nobody has started it makes itself.
 */
#include "nvn_module.h"
#include "nvn_pipe.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

/** @brief A program to warm: its key, stage and, for a compute program, workgroup size. */
typedef struct job {
    char key[65];
    uint32_t stage;
    uint32_t wg[3];
} job;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    job *queue;
    uint32_t nqueue, capqueue, active;
    bool pool_started;
    bool launched, all, background;
    uint32_t total, next, done, nrecorded, nshader, failed;
    /** @brief The RELIFT_WARM_THREADS count, 0 when unset. */
    uint32_t forced;
    uint64_t t0, last_log;
} W = { .lock = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

/** @brief Memory one compile may take: a driver compile of a large translated program was measured at 2.4 GiB. */
#define COMPILE_MEMORY (4ull << 30)

/**
 * @brief The workers to start: one per processor, and no more than the
 * memory available (Linux) or installed (elsewhere) holds at COMPILE_MEMORY
 * each, so that a worker per processor of a big machine does not run it out
 * of memory. RELIFT_WARM_THREADS overrides both: more compiles at once also
 * use up the process's memory mappings faster (a driver compile maps many
 * blocks), which maps_check watches.
 */
static uint32_t worker_count(void)
{
    uint64_t ram = 0;
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    uint32_t cpus = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1u;
    MEMORYSTATUSEX ms = { .dwLength = sizeof ms };
    if (GlobalMemoryStatusEx(&ms))
        ram = ms.ullTotalPhys;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    uint32_t cpus = n > 0 ? (uint32_t)n : 1u;
    long pages = sysconf(_SC_PHYS_PAGES), size = sysconf(_SC_PAGESIZE);
    if (pages > 0 && size > 0)
        ram = (uint64_t)pages * (uint64_t)size;
    FILE *f = fopen("/proc/meminfo", "r");
    char line[128];
    while (f && fgets(line, sizeof line, f)) {
        unsigned long long kib;
        if (sscanf(line, "MemAvailable: %llu kB", &kib) == 1) {
            ram = kib << 10;
            break;
        }
    }
    if (f)
        fclose(f);
#endif
    uint64_t by_memory = ram / COMPILE_MEMORY;
    if (by_memory && by_memory < cpus)
        cpus = (uint32_t)by_memory;
    const char *want = getenv("RELIFT_WARM_THREADS");
    if (want && atoi(want) > 0) {
        cpus = (uint32_t)atoi(want);
        W.forced = cpus;
    }
    return cpus ? cpus : 1u;
}

bool nvn_warm_maps_note(uint64_t count, uint64_t limit, uint32_t forced, char *buf, size_t n)
{
    if (!limit || count * 4 < limit * 3)
        return false;
    char why[128] = "";
    if (forced)
        snprintf(why, sizeof why, "; RELIFT_WARM_THREADS=%u runs that many compiles at once and skips the memory rule",
                 forced);
    snprintf(buf, n,
             "the process holds %llu memory mappings of the %llu vm.max_map_count allows: the driver's shader compiles "
             "use them up, and at the limit allocations and thread creation fail%s",
             (unsigned long long)count, (unsigned long long)limit, why);
    return true;
}

/** @brief Logs nvn_warm_maps_note's warning once when this process's mappings run high (Linux and Android). */
static void maps_check(void)
{
#if defined(__linux__)
    static bool warned;
    if (warned)
        return;
    unsigned long long limit = 0, count = 0;
    FILE *f = fopen("/proc/sys/vm/max_map_count", "r");
    if (f) {
        if (fscanf(f, "%llu", &limit) != 1)
            limit = 0;
        fclose(f);
    }
    f = limit ? fopen("/proc/self/maps", "r") : NULL;
    if (!f)
        return;
    char chunk[65536];
    size_t got;
    while ((got = fread(chunk, 1, sizeof chunk, f)) > 0)
        for (size_t i = 0; i < got; i++)
            count += chunk[i] == '\n';
    fclose(f);
    char msg[512];
    if (nvn_warm_maps_note(count, limit, W.forced, msg, sizeof msg)) {
        warned = true;
        rl_log(RL_LOG_WARN, "nvn", "%s", msg);
    }
#endif
}

/** @brief The key of a draw that uses module m alone in its stage with the specialization constants it starts with. */
void nvn_warm_key(nvn_pipe_key *k, const nvn_module *m, uint32_t stage)
{
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    nvn_gfx_init(&st);
    memset(k, 0, sizeof *k);
    k->mods[stage] = m->m;
    nvn_module_default_spec(m, &k->nspec[stage], k->spec_map[stage], k->spec[stage]);
    if (stage == 1 && m->alpha_test && k->nspec[stage] < NVN_MAX_SPECS) {
        k->spec_map[stage][k->nspec[stage]] = (VkSpecializationMapEntry){ ALPHA_FUNC_SPEC, 4 * k->nspec[stage], 4 };
        k->spec[stage][k->nspec[stage]++] = NVN_ALPHA_ALWAYS;
    }
    nvn_spec_sort(&k->nspec[stage], k->spec_map[stage], k->spec[stage]);
    k->polygon = VK_POLYGON_MODE_FILL;
    k->negative_one_to_one = nvn_device_depth_mode() == 0;
    k->sample_mask = st.sample_mask & 1u;
    k->viewport_swizzle = st.viewport_swizzle[0];
    k->depth_clamp = st.depth_clamp;
}

/**
 * @brief Makes what the driver compiles of the program with key and stage
 * (negative: the stage its translation names; wg: a compute program's
 * workgroup size, all zero when the game has not said).
 */
static bool warm_program(const char *key, int stage, const uint32_t wg[3])
{
    static _Atomic bool ready;
    if (!ready) {
        if (!nvn_vk_ready())
            return false;
        ready = true;
    }
    char why[200];
    nvn_module *m = nvn_module_get(key, stage, why, sizeof why);
    if (!m) {
        rl_log(RL_LOG_ERROR, "nvn", "warm-up: %s", why);
        return false;
    }
    stage = m->stage;
    nvn_pipe_key k;
    if (stage == (int)NVN_STAGE_COMPUTE) {
        if (!wg[0] && !wg[1] && !wg[2])
            return true;
        VkSpecializationMapEntry map[NVN_MAX_SPECS];
        uint32_t vals[NVN_MAX_SPECS], n = 3;
        for (uint32_t i = 0; i < 3; i++) {
            map[i] = (VkSpecializationMapEntry){ i, 4 * i, 4 };
            vals[i] = wg[i];
        }
        nvn_module_default_spec(m, &n, map, vals);
        nvn_spec_sort(&n, map, vals);
        return nvn_pipe_compute(m->m, n, map, vals) != NULL;
    }
    if (!nvn_pipe_uses_libraries() || (stage != 0 && stage != 1))
        return true;
    nvn_warm_key(&k, m, stage);
    return stage == 0 ? nvn_pipe_warm_vertex(&k) : nvn_pipe_warm_fragment(&k);
}

/** @brief Makes the pipeline of a record the earlier launches kept. */
static bool make_recorded(const nvn_pipe_rec *rec)
{
    char why[200];
    if (rec->kind == NVN_REC_PROGRAM)
        return warm_program(rec->keys[5], (int)rec->k.topology, rec->k.spec[0]);
    if (rec->kind == NVN_REC_COMPUTE) {
        nvn_module *m = nvn_module_get(rec->keys[5], (int)NVN_STAGE_COMPUTE, why, sizeof why);
        if (!m)
            return false;
        VkSpecializationMapEntry map[NVN_MAX_SPECS];
        uint32_t vals[NVN_MAX_SPECS], n = rec->k.nspec[0];
        memcpy(map, rec->k.spec_map[0], n * sizeof map[0]);
        memcpy(vals, rec->k.spec[0], n * sizeof vals[0]);
        return nvn_pipe_compute(m->m, n, map, vals) != NULL;
    }
    nvn_pipe_key k = rec->k;
    nvn_module *mods[5] = { NULL };
    for (uint32_t s = 0; s < 5; s++) {
        if (!rec->keys[s][0])
            continue;
        if (!(mods[s] = nvn_module_get(rec->keys[s], (int)s, why, sizeof why)))
            return false;
        k.mods[s] = mods[s]->m;
    }
    if (mods[1] && mods[0] && !(k.mods[1] = nvn_module_fragment_for(mods[0], mods[1])))
        return false;
    return nvn_pipe_graphics(&k) != NULL;
}

/** @brief Logs progress at most every two seconds, or at once when force is set; W.lock held. */
static void progress(bool force, const char *what)
{
    uint64_t now = rl_clock_ns();
    if (!force && now - W.last_log < 2000000000ull)
        return;
    W.last_log = now;
    rl_log(RL_LOG_INFO, "nvn", "shader warm-up (%s): %u of %u programs, %.1f s", what, W.done, W.total,
           (now - W.t0) / 1e9);
    maps_check();
}

/** @brief A launch worker: takes the next table program or recorded pipeline until none is left. */
static void *launch_worker(void *arg)
{
    (void)arg;
    nvn_pipe_worker();
    for (;;) {
        pthread_mutex_lock(&W.lock);
        uint32_t i = W.next++;
        pthread_mutex_unlock(&W.lock);
        if (i >= W.total)
            return NULL;
        bool ok = true;
        if (i < W.nrecorded) {
            ok = make_recorded(nvn_pipe_recorded_at(i));
        } else {
            char key[65];
            uint32_t wg[3] = { 0, 0, 0 };
            if (nvn_shader_key_at(i - W.nrecorded, key))
                ok = warm_program(key, -1, wg);
        }
        pthread_mutex_lock(&W.lock);
        W.done++;
        W.failed += !ok;
        progress(false, i < W.nrecorded ? "recorded" : "table");
        pthread_mutex_unlock(&W.lock);
    }
}

/** @brief A pool worker: makes the programs the game registers, in the order it registers them. */
static void *pool_worker(void *arg)
{
    (void)arg;
    nvn_pipe_worker();
    for (;;) {
        pthread_mutex_lock(&W.lock);
        while (!W.nqueue)
            pthread_cond_wait(&W.cv, &W.lock);
        job j = W.queue[0];
        memmove(W.queue, W.queue + 1, --W.nqueue * sizeof *W.queue);
        W.active++;
        pthread_mutex_unlock(&W.lock);
        bool ok = warm_program(j.key, (int)j.stage, j.wg);
        pthread_mutex_lock(&W.lock);
        W.active--;
        if (!ok)
            W.failed++;
        pthread_mutex_unlock(&W.lock);
    }
}

static void launch(void)
{
    const char *mode = getenv("RELIFT_PIPELINE_WARM");
    bool off = mode && !strcmp(mode, "off");
    W.all = mode && (!strcmp(mode, "all") || !strcmp(mode, "all-background"));
    W.background = mode && !strcmp(mode, "all-background");
    if (off || !nvn_vk_ready()) {
        nvn_pipe_warmed();
        return;
    }
    pthread_mutex_lock(&W.lock);
    W.launched = true;
    W.nrecorded = nvn_pipe_recorded();
    W.nshader = W.all ? nvn_shader_count() : 0;
    W.total = W.nrecorded + W.nshader;
    W.t0 = W.last_log = rl_clock_ns();
    uint32_t total = W.total, nthreads = worker_count();
    pthread_mutex_unlock(&W.lock);
    if (nthreads > total)
        nthreads = total;
    if (!nthreads) {
        nvn_pipe_warmed();
        return;
    }
    rl_log(RL_LOG_INFO, "nvn", "shader warm-up: %u recorded pipelines and %u table programs on %u threads%s",
           W.nrecorded, W.nshader, nthreads, W.background ? ", the game starts now" : "");
    nvn_pipe_warming(true);
    pthread_t th[256];
    uint32_t started = 0;
    for (uint32_t i = 0; i < nthreads && i < 256; i++)
        if (pthread_create(&th[started], NULL, launch_worker, NULL) == 0)
            started++;
    if (!started)
        launch_worker(NULL);
    if (W.background) {
        for (uint32_t i = 0; i < started; i++)
            pthread_detach(th[i]);
        return;
    }
    for (uint32_t i = 0; i < started; i++)
        pthread_join(th[i], NULL);
    uint64_t made, made_ns, late, late_ns;
    nvn_pipe_stats(&made, &made_ns, &late, &late_ns);
    pthread_mutex_lock(&W.lock);
    progress(true, "done");
    pthread_mutex_unlock(&W.lock);
    rl_log(RL_LOG_INFO, "nvn",
           "shader warm-up done in %.1f s: %u of %u programs, %u refused; the driver compiled %llu in %.1f s of thread "
           "time",
           (rl_clock_ns() - W.t0) / 1e9, W.done, W.total, W.failed, (unsigned long long)made, made_ns / 1e9);
    nvn_pipe_warming(false);
    nvn_pipe_warmed();
}

void nvn_warm_launch(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, launch);
}

void nvn_warm_program(const uint8_t *mem, uint64_t code, uint64_t control)
{
    job j;
    memset(&j, 0, sizeof j);
    int stage = nvn_shader_stage(mem, code);
    if (stage < 0 || stage > (int)NVN_STAGE_COMPUTE || (stage == (int)NVN_STAGE_COMPUTE && !control))
        return;
    if (!nvn_shader_key(mem, code, j.key, &(uint64_t){ 0 }))
        return;
    j.stage = (uint32_t)stage;
    if (stage == (int)NVN_STAGE_COMPUTE)
        for (uint32_t i = 0; i < 3; i++)
            j.wg[i] = rl_ld32(mem, control + 0x58 + 4 * i);
    const char *mode = getenv("RELIFT_PIPELINE_WARM");
    if (mode && !strcmp(mode, "off"))
        return;
    if (!nvn_pipe_record_program(j.key, j.stage, j.wg))
        return;
    pthread_mutex_lock(&W.lock);
    if (W.nqueue == W.capqueue) {
        uint32_t cap = W.capqueue ? W.capqueue * 2 : 64;
        job *q = realloc(W.queue, cap * sizeof *q);
        if (!q) {
            pthread_mutex_unlock(&W.lock);
            return;
        }
        W.queue = q;
        W.capqueue = cap;
    }
    W.queue[W.nqueue++] = j;
    if (!W.pool_started) {
        W.pool_started = true;
        uint32_t n = worker_count();
        for (uint32_t i = 0; i < n && i < 64; i++) {
            pthread_t t;
            if (pthread_create(&t, NULL, pool_worker, NULL) == 0)
                pthread_detach(t);
        }
    }
    pthread_cond_signal(&W.cv);
    pthread_mutex_unlock(&W.lock);
}
