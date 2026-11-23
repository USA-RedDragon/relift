/**
 * @file nvn_xlate.c
 * @brief Shader tables made on the player's device: the NVIDIA program translator (gpus/xlate, rlx.h) linked into
 * the program translates the game's programs for this device when no table fits it.
 *
 * A table fits when its table.stamp is the translator stamp the build expects and its table.device holds the same
 * bytes as the device.json this run wrote (another driver, another GPU or a changed device probe gives other
 * bytes). nvn_xlate_start runs right after device.json is written: it keeps the table folder when its tables fit,
 * else uses <shader folder>/tables when those fit, else translates the game's programs into <shader folder>/tables
 * on a worker thread. The programs are programs.rlsp (the program pack that goes with the game data) in the folder
 * of the build's tables (nvn_shader_table_dir) or in the shader folder, or, in a development build, the shader
 * folder's programs/ and meshes.json.
 *
 * Everything that reads a table waits for the translation first (nvn_xlate_wait). The wait shows the compile screen
 * (relift/warmup.h) after SCREEN_NS, counting the programs translated: the same "Compiling shaders" card the warm-up
 * shows next, so the player sees one compile step in two passes.
 *
 * Without the translator (no RL_NVN_XLATE) every function here does nothing, and the tables are the ones the build
 * made, as before. RELIFT_NVN_XLATE=off keeps those even with the translator linked, for a deliberate A/B run.
 * The translator is used only when its stamp is the one the build expects: a table it made for another stamp would
 * be refused anyway.
 */
#include "nvn_priv.h"
#include "nvn_vk.h"

#include <relift/warmup.h>

#include "core.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#ifdef RL_NVN_XLATE
#include "rlx.h"
#endif

/** @brief How long a translation runs before the compile screen shows (ns). */
#define SCREEN_NS (1000ull * 1000000ull)

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    /** @brief A translation is running. */
    bool busy;
    bool screen;
    /** @brief The folder the tables are read from, when it is not nvn_shader_table_dir's default. */
    char dir[1100];
    bool have_dir;
    uint32_t done, total;
} X = { .lock = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

const char *nvn_xlate_table_dir(void)
{
    pthread_mutex_lock(&X.lock);
    const char *d = X.have_dir ? X.dir : NULL;
    pthread_mutex_unlock(&X.lock);
    return d;
}

void nvn_xlate_wait(void)
{
    pthread_mutex_lock(&X.lock);
    uint64_t t0 = rl_clock_ns();
    while (X.busy) {
        if (!X.screen && rl_clock_ns() - t0 >= SCREEN_NS) {
            X.screen = true;
            pthread_mutex_unlock(&X.lock);
            (void)nvn_vk_window_early();
            pthread_mutex_lock(&X.lock);
            continue;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t ns = (uint64_t)ts.tv_nsec + 250000000ull;
        ts.tv_sec += (time_t)(ns / 1000000000ull);
        ts.tv_nsec = (long)(ns % 1000000000ull);
        pthread_cond_timedwait(&X.cv, &X.lock, &ts);
    }
    pthread_mutex_unlock(&X.lock);
}

#ifdef RL_NVN_XLATE

/** @brief The bytes of file path (malloc'd, *n long), or NULL when it cannot be read. */
static uint8_t *slurp(const char *path, size_t *n)
{
    *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    uint8_t *b = NULL;
    size_t len = 0, cap = 0;
    for (;;) {
        if (len == cap) {
            cap = cap ? cap * 2 : 4096;
            uint8_t *nb = realloc(b, cap);
            if (!nb) {
                free(b);
                fclose(f);
                return NULL;
            }
            b = nb;
        }
        size_t got = fread(b + len, 1, cap - len, f);
        len += got;
        if (!got)
            break;
    }
    fclose(f);
    *n = len;
    return b;
}

/** @brief Whether dir/name holds exactly the n bytes at want. */
static bool same_file(const char *dir, const char *name, const uint8_t *want, size_t n)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t have_n;
    uint8_t *have = slurp(path, &have_n);
    bool same = have && have_n == n && !memcmp(have, want, n);
    free(have);
    return same;
}

/** @brief Whether the tables in dir fit: the stamp is stamp and table.device is device (n bytes), with a table. */
static bool fits(const char *dir, const char *stamp, const uint8_t *device, size_t n)
{
    char line[32];
    snprintf(line, sizeof line, "%s\n", stamp);
    char path[1200];
    snprintf(path, sizeof path, "%s/table.rlspv", dir);
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return same_file(dir, "table.stamp", (const uint8_t *)line, strlen(line)) &&
           same_file(dir, "table.device", device, n);
}

static int on_progress(void *user, uint32_t done, uint32_t total)
{
    (void)user;
    pthread_mutex_lock(&X.lock);
    X.done = done;
    X.total = total;
    pthread_mutex_unlock(&X.lock);
    rl_warmup_progress(done, 0);
    return 1;
}

/** @brief What the worker translates. */
typedef struct job {
    rlx *x;
    char out[1100];
} job;

static void *worker(void *arg)
{
    job *j = arg;
    char why[512] = "";
    rlx_built b = { 0 };
    uint64_t t0 = rl_clock_ns();
    int rc = rlx_write_tables(j->x, j->out, 0, on_progress, NULL, &b, why, sizeof why);
    double s = (double)(rl_clock_ns() - t0) / 1e9;
    rlx_free(j->x);
    if (rc)
        rl_log(RL_LOG_ERROR, "nvn", "cannot translate the shader tables into %s: %s", j->out, why);
    else
        rl_log(RL_LOG_INFO, "nvn", "shader tables made on this device in %s: %u of %u programs translated (%u refused) in %.1f s",
               j->out, b.translated, b.programs, b.refused, s);
    rl_warmup_end();
    pthread_mutex_lock(&X.lock);
    if (!rc) {
        snprintf(X.dir, sizeof X.dir, "%s", j->out);
        X.have_dir = true;
    }
    pthread_mutex_unlock(&X.lock);
    if (!rc)
        nvn_shader_tables_reopen();
    pthread_mutex_lock(&X.lock);
    X.busy = false;
    pthread_cond_broadcast(&X.cv);
    pthread_mutex_unlock(&X.lock);
    free(j);
    return NULL;
}

/** @brief Whether dir holds the program pack or a programs folder; the path to give rlx_new goes to out. */
static bool programs_in(const char *dir, char *out, size_t n)
{
    snprintf(out, n, "%s/programs.rlsp", dir);
    FILE *f = fopen(out, "rb");
    if (f) {
        fclose(f);
        return true;
    }
    char sub[1200];
    snprintf(sub, sizeof sub, "%s/programs", dir);
    struct stat st;
    if (stat(sub, &st) == 0 && S_ISDIR(st.st_mode)) {
        snprintf(out, n, "%s", dir);
        return true;
    }
    return false;
}

/**
 * @brief The programs to translate: in the folder the build's tables are read from (where a package or relift push
 * puts the pack beside them), else in the shader folder; false for none.
 */
static bool programs_path(const char *shaders, char *out, size_t n)
{
    return programs_in(nvn_shader_table_dir(), out, n) || programs_in(shaders, out, n);
}

void nvn_xlate_start(void)
{
    const char *mode = getenv("RELIFT_NVN_XLATE");
    if ((mode && !strcmp(mode, "off")) || nvn_generation() != 2)
        return;
    if (rlx_abi() != RLX_ABI) {
        rl_log(RL_LOG_ERROR, "nvn", "the shader translator's ABI is %u, the runtime's %u", rlx_abi(), RLX_ABI);
        return;
    }
    const char *want = nvn_shader_expected_stamp();
    const char *stamp = rlx_stamp();
    if (want && strcmp(want, stamp)) {
        rl_log(RL_LOG_WARN, "nvn", "the linked shader translator is %s, the build expects %s: not translating", stamp,
               want);
        return;
    }
    const char *shaders = nvn_shader_dir();
    char path[1200];
    snprintf(path, sizeof path, "%s/device.json", shaders);
    size_t dn;
    uint8_t *device = slurp(path, &dn);
    if (!device) {
        rl_log(RL_LOG_WARN, "nvn", "no %s: not translating", path);
        return;
    }
    char local[1100];
    snprintf(local, sizeof local, "%s/tables", shaders);
    if (fits(nvn_shader_table_dir(), stamp, device, dn)) {
        free(device);
        return;
    }
    if (fits(local, stamp, device, dn)) {
        pthread_mutex_lock(&X.lock);
        snprintf(X.dir, sizeof X.dir, "%s", local);
        X.have_dir = true;
        pthread_mutex_unlock(&X.lock);
        nvn_shader_tables_reopen();
        rl_log(RL_LOG_INFO, "nvn", "shader tables made on this device earlier: %s", local);
        free(device);
        return;
    }
    char programs[1200];
    if (!programs_path(shaders, programs, sizeof programs)) {
        rl_log(RL_LOG_WARN, "nvn",
               "the shader tables in %s were not made for this device and translator, and neither it nor %s has a "
               "programs.rlsp to make them from",
               nvn_shader_table_dir(), shaders);
        free(device);
        return;
    }
    char why[512] = "";
    rlx *x = rlx_new(programs, device, dn, why, sizeof why);
    free(device);
    if (!x) {
        rl_log(RL_LOG_ERROR, "nvn", "cannot open the programs at %s: %s", programs, why);
        return;
    }
    uint32_t total = rlx_programs(x);
    job *j = calloc(1, sizeof *j);
    if (!j) {
        rlx_free(x);
        return;
    }
    j->x = x;
    snprintf(j->out, sizeof j->out, "%s", local);
    rl_log(RL_LOG_INFO, "nvn", "making the shader tables for this device: %u programs from %s into %s", total, programs,
           local);
    pthread_mutex_lock(&X.lock);
    X.busy = true;
    X.screen = false;
    X.done = 0;
    X.total = total;
    pthread_mutex_unlock(&X.lock);
    rl_warmup_begin(total, RL_WARMUP_BACKGROUND);
    pthread_t t;
    if (pthread_create(&t, NULL, worker, j) == 0) {
        pthread_detach(t);
        return;
    }
    worker(j);
}

#else

void nvn_xlate_start(void) {}

#endif
