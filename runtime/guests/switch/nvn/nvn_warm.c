/**
 * @file nvn_warm.c
 * @brief The launch warm-up: the host driver's compile of every program the
 * build translated, done before the game draws.
 *
 * A draw's pipeline depends on state the game sets as it draws, but the
 * expensive part of it, the driver lowering each shader stage to GPU code,
 * depends only on the stage's module and specialization constants. The
 * warm-up makes that part for every vertex and fragment program of the
 * build's shader table (nvn_pipe.h: pre-raster and fragment libraries), and
 * for every compute program the game has registered (whose workgroup size
 * the game's control section holds), on one worker thread per processor. A
 * draw then only links libraries.
 *
 * RELIFT_PIPELINE_WARM is "block" (the default: the game's first submit
 * returns when the warm-up is done), "background" (it returns at once and a
 * draw that needs a pipeline nobody has made waits for the warm-up to end)
 * or "off".
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

/** @brief A compute program the game registered: its key and workgroup size. */
typedef struct compute_job {
    char key[65];
    uint32_t wg[3];
} compute_job;

static struct {
    pthread_mutex_t lock;
    compute_job *compute;
    uint32_t ncompute, capcompute;
    bool launched;
    uint32_t total, next, done, nvertex, nfragment, ncompute_done, failed;
    uint64_t t0, last_log;
} W = { .lock = PTHREAD_MUTEX_INITIALIZER };

static uint32_t cpu_count(void)
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

/** @brief The key of a draw that uses module m alone in its stage with the specialization constants it starts with. */
static void default_key(nvn_pipe_key *k, const nvn_module *m, uint32_t stage)
{
    memset(k, 0, sizeof *k);
    k->mods[stage] = m->m;
    nvn_module_default_spec(m, &k->nspec[stage], k->spec_map[stage], k->spec[stage]);
    nvn_spec_sort(&k->nspec[stage], k->spec_map[stage], k->spec[stage]);
    k->polygon = VK_POLYGON_MODE_FILL;
    k->negative_one_to_one = nvn_device_depth_mode() == 0;
}

/** @brief Makes the pipeline part of the program with key; wg is the workgroup size of a compute program, else NULL. */
static bool warm_key(const char *key, const uint32_t *wg, uint32_t *kind)
{
    char why[200];
    nvn_module *m = nvn_module_get(key, wg ? NVN_STAGE_COMPUTE : -1, why, sizeof why);
    if (!m) {
        rl_log(RL_LOG_ERROR, "nvn", "warm-up: %s", why);
        return false;
    }
    nvn_pipe_key k;
    *kind = 0;
    if (wg && m->stage == (int)NVN_STAGE_COMPUTE) {
        VkSpecializationMapEntry map[NVN_MAX_SPECS];
        uint32_t vals[NVN_MAX_SPECS], n = 3;
        for (uint32_t i = 0; i < 3; i++) {
            map[i] = (VkSpecializationMapEntry){ i, 4 * i, 4 };
            vals[i] = wg[i];
        }
        nvn_module_default_spec(m, &n, map, vals);
        nvn_spec_sort(&n, map, vals);
        *kind = 3;
        return nvn_pipe_compute(m->m, n, map, vals) != NULL;
    }
    if (wg || !nvn_pipe_uses_libraries())
        return true;
    if (m->stage == 0) {
        default_key(&k, m, 0);
        *kind = 1;
        return nvn_pipe_warm_vertex(&k);
    }
    if (m->stage == 1) {
        default_key(&k, m, 1);
        *kind = 2;
        return nvn_pipe_warm_fragment(&k);
    }
    return true;
}

static void progress(bool force)
{
    uint64_t now = rl_clock_ns();
    if (!force && now - W.last_log < 2000000000ull)
        return;
    W.last_log = now;
    rl_log(RL_LOG_INFO, "nvn", "shader warm-up: %u of %u programs, %.1f s", W.done, W.total, (now - W.t0) / 1e9);
}

static void *worker(void *arg)
{
    (void)arg;
    nvn_pipe_worker();
    for (;;) {
        pthread_mutex_lock(&W.lock);
        uint32_t i = W.next++;
        uint32_t n = W.total - W.ncompute;
        compute_job cj;
        bool is_compute = i >= n && i < W.total;
        if (is_compute)
            cj = W.compute[i - n];
        pthread_mutex_unlock(&W.lock);
        if (i >= W.total)
            return NULL;
        char key[65];
        uint32_t kind = 0;
        bool ok = true;
        if (is_compute)
            ok = warm_key(cj.key, cj.wg, &kind);
        else if (nvn_shader_key_at(i, key))
            ok = warm_key(key, NULL, &kind);
        pthread_mutex_lock(&W.lock);
        W.done++;
        W.nvertex += kind == 1;
        W.nfragment += kind == 2;
        W.ncompute_done += kind == 3;
        W.failed += !ok;
        progress(false);
        pthread_mutex_unlock(&W.lock);
    }
}

static void *background_main(void *arg)
{
    (void)arg;
    worker(NULL);
    return NULL;
}

static void launch(void)
{
    W.launched = true;
    const char *mode = getenv("RELIFT_PIPELINE_WARM");
    bool off = mode && !strcmp(mode, "off"), background = mode && !strcmp(mode, "background");
    if (!nvn_draw_layouts() || off) {
        nvn_pipe_warmed();
        return;
    }
    pthread_mutex_lock(&W.lock);
    W.total = nvn_shader_count() + W.ncompute;
    W.t0 = W.last_log = rl_clock_ns();
    uint32_t total = W.total;
    pthread_mutex_unlock(&W.lock);
    uint32_t nthreads = cpu_count();
    const char *want = getenv("RELIFT_WARM_THREADS");
    if (want && atoi(want) > 0)
        nthreads = (uint32_t)atoi(want);
    if (nthreads > total)
        nthreads = total;
    if (!nthreads) {
        nvn_pipe_warmed();
        return;
    }
    rl_log(RL_LOG_INFO, "nvn", "shader warm-up: %u programs on %u threads%s", total, nthreads,
           background ? ", the game starts now" : "");
    nvn_pipe_warming(true);
    pthread_t th[256];
    uint32_t started = 0;
    for (uint32_t i = 0; i < nthreads && i < 256; i++)
        if (pthread_create(&th[started], NULL, background ? background_main : worker, NULL) == 0)
            started++;
    if (!started)
        worker(NULL);
    if (background) {
        for (uint32_t i = 0; i < started; i++)
            pthread_detach(th[i]);
        return;
    }
    for (uint32_t i = 0; i < started; i++)
        pthread_join(th[i], NULL);
    uint64_t made, made_ns, late, late_ns;
    nvn_pipe_stats(&made, &made_ns, &late, &late_ns);
    progress(true);
    rl_log(RL_LOG_INFO, "nvn",
           "shader warm-up done in %.1f s: %u vertex and %u fragment libraries, %u compute pipelines, %u refused; the driver "
           "compiled %llu in %.1f s of thread time",
           (rl_clock_ns() - W.t0) / 1e9, W.nvertex, W.nfragment, W.ncompute_done, W.failed, (unsigned long long)made,
           made_ns / 1e9);
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
    if (nvn_shader_stage(mem, code) != (int)NVN_STAGE_COMPUTE || !control)
        return;
    compute_job j;
    memset(&j, 0, sizeof j);
    if (!nvn_shader_key(mem, code, j.key, &(uint64_t){ 0 }))
        return;
    for (uint32_t i = 0; i < 3; i++)
        j.wg[i] = rl_ld32(mem, control + 0x58 + 4 * i);
    pthread_mutex_lock(&W.lock);
    for (uint32_t i = 0; i < W.ncompute; i++)
        if (!memcmp(&W.compute[i], &j, sizeof j)) {
            pthread_mutex_unlock(&W.lock);
            return;
        }
    if (W.ncompute == W.capcompute) {
        uint32_t cap = W.capcompute ? W.capcompute * 2 : 64;
        compute_job *c = realloc(W.compute, cap * sizeof *c);
        if (!c) {
            pthread_mutex_unlock(&W.lock);
            return;
        }
        W.compute = c;
        W.capcompute = cap;
    }
    W.compute[W.ncompute++] = j;
    bool after = W.launched;
    pthread_mutex_unlock(&W.lock);
    if (after) {
        uint32_t kind;
        warm_key(j.key, j.wg, &kind);
    }
}
