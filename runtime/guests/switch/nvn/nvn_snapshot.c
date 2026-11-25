/**
 * @file nvn_snapshot.c
 * @brief The NVN layer's part of the diagnostic snapshot (core/snapshot.h): the GPU profile of each present of the
 * window (per pass and, with the per-draw timestamps, per draw), why each batch ended and the host Vulkan calls per
 * draw, a frame trace of one present in the format RELIFT_NVN_TRACE writes (so nvn_trace.py and the replay tools read
 * it), and what the layer holds in memory. Nothing runs outside a window: untriggered, the step after each present is
 * one atomic load. Only with the debug stub built in.
 *
 * The core's window counts presents where a guest hands one to the presenter, inside this layer's own present; the
 * layer's work needs to run outside its lock, so the hooks only note the request and the step (nvn_snapshot_step,
 * called at the end of nvn_present) does the work: it arms at the first present after the trigger, reports each
 * present of the window, and finishes after the last one. The provider's ready hook keeps the window open until then.
 */
#include "gdbstub.h"
#include "nvn_vk.h"

#if defined(RL_DEBUG_STUB)

#include "nvn_module.h"
#include "nvn_pipe.h"
#include "nvn_priv.h"
#include "snapshot.h"

#include <relift/settings.h>

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__GLIBC__)
#include <malloc.h>
#endif

enum { SN_IDLE, SN_REQUESTED, SN_RUNNING, SN_DONE };

static struct {
    _Atomic int state;
    uint32_t window, got;
    char dir[1100];
    FILE *gpu, *batches;
    /** @brief What the run had set before the window turned the profile on. */
    bool was_profile;
    char *old_draws;
    uint64_t trace_from;
    bool traced;
    bool memory_written;
} sn;

/** @brief Which of the window's log lines are about batches and host Vulkan calls (the rest is GPU time). */
static bool is_batch_line(const char *m)
{
    return strstr(m, "ended at") || strstr(m, "host Vulkan calls") || strstr(m, "NVN commands replayed") ||
           strstr(m, "batches that may write") || strstr(m, "counter reports");
}

static void tee(void *user, const char *tag, const char *msg)
{
    (void)user;
    if (strcmp(tag, "nvn"))
        return;
    FILE *f = is_batch_line(msg) ? sn.batches : sn.gpu;
    if (f)
        fprintf(f, "%s\n", msg);
}

static void snap_begin(void *user, const char *dir, uint32_t presents)
{
    (void)user;
    snprintf(sn.dir, sizeof sn.dir, "%s", dir);
    sn.window = presents;
    sn.got = 0;
    sn.traced = sn.memory_written = false;
    atomic_store(&sn.state, SN_REQUESTED);
}

static bool snap_ready(void *user)
{
    (void)user;
    int s = atomic_load(&sn.state);
    return s == SN_IDLE || s == SN_DONE;
}

static void size_text(char *out, size_t n, uint64_t b)
{
    if (b >= 1ull << 30)
        snprintf(out, n, "%.2f GiB", (double)b / (double)(1ull << 30));
    else if (b >= 1ull << 20)
        snprintf(out, n, "%.1f MiB", (double)b / (double)(1ull << 20));
    else
        snprintf(out, n, "%.1f KiB", (double)b / 1024.0);
}

typedef struct tex_row {
    uint32_t w, h, d, levels, format;
    uint64_t size;
} tex_row;

static int by_size(const void *a, const void *b)
{
    const tex_row *x = a, *y = b;
    return x->size < y->size ? 1 : x->size > y->size ? -1 : 0;
}

/** @brief Writes memory_nvn.txt: everything the layer holds that costs host or device memory. Takes the layer's lock. */
static void write_holdings(void)
{
    char path[1300], t[48], u[48];
    snprintf(path, sizeof path, "%s/memory_nvn.txt", sn.dir);
    FILE *f = fopen(path, "w");
    if (!f)
        return;
    uint32_t libs, gfx, comp, sets, mirrors, pieces, spans, dropped;
    uint64_t made, made_ns, late, late_ns, set_cmds, set_bytes, mir_bytes, mir_staging;
    nvn_pipe_held(&libs, &gfx, &comp);
    nvn_pipe_stats(&made, &made_ns, &late, &late_ns);
    nvn_cmdsets_held(&sets, &set_cmds, &set_bytes);
    nvn_mirrors_held(&mirrors, &mir_bytes, &mir_staging);
    nvn_pools_counts(&pieces, &spans, &dropped);
    fprintf(f, "What the NVN layer holds.\n\n");
    fprintf(f, "pipelines: %u graphics, %u compute, %u pipeline libraries; %llu made since start (%.1f s), %llu of them late\n",
            gfx, comp, libs, (unsigned long long)made, (double)made_ns / 1e9, (unsigned long long)late);
    fprintf(f, "shader modules: %u\n", nvn_modules_held());
    size_text(t, sizeof t, set_bytes);
    fprintf(f, "command sets: %u holding %llu commands, %s\n", sets, (unsigned long long)set_cmds, t);
    size_text(t, sizeof t, mir_bytes);
    size_text(u, sizeof u, mir_staging);
    fprintf(f, "mirrors: %u covering %s of guest memory; staging held by their uploads and readbacks: %s\n", mirrors, t, u);
    fprintf(f, "pool table: %u pieces of %u pools, %u left out\n", pieces, spans, dropped);
    uint64_t arenas = g.arena.size;
    for (uint32_t i = 0; i < NVN_SLOTS; i++)
        arenas += g.arenas[i].size;
    size_text(t, sizeof t, arenas);
    fprintf(f, "staging arenas: %s over %u batch slots (%u made, peak %u)\n", t, (unsigned)NVN_SLOTS, g.slots_made, g.slots_peak);

    nvn_lock();
    uint32_t n = g.nimages;
    tex_row *rows = n ? malloc((size_t)n * sizeof *rows) : NULL;
    uint32_t k = 0;
    uint64_t total = 0, buckets[6] = { 0 }, bucket_bytes[6] = { 0 };
    static const uint64_t edges[5] = { 64u << 10, 1u << 20, 16u << 20, 64u << 20, 256u << 20 };
    for (uint32_t i = 0; rows && i < n; i++) {
        const vimg *v = g.images[i];
        if (!v)
            continue;
        uint64_t sz = v->d.size;
        rows[k++] = (tex_row){ v->d.width, v->d.height, v->d.depth, v->d.levels, v->d.format, sz };
        total += sz;
        uint32_t b = 0;
        while (b < 5 && sz >= edges[b])
            b++;
        buckets[b]++;
        bucket_bytes[b] += sz;
    }
    nvn_unlock();
    size_text(t, sizeof t, total);
    fprintf(f, "\ntextures: %u images, %s of guest storage\n", k, t);
    static const char *const names[6] = { "under 64 KiB", "64 KiB to 1 MiB", "1 to 16 MiB", "16 to 64 MiB", "64 to 256 MiB",
                                          "256 MiB and over" };
    for (uint32_t b = 0; b < 6; b++) {
        size_text(t, sizeof t, bucket_bytes[b]);
        fprintf(f, "  %-18s %6llu images, %s\n", names[b], (unsigned long long)buckets[b], t);
    }
    if (rows) {
        qsort(rows, k, sizeof *rows, by_size);
        fprintf(f, "largest textures (width x height x depth, mip levels, NVN format, storage):\n");
        for (uint32_t i = 0; i < k && i < 25; i++) {
            size_text(t, sizeof t, rows[i].size);
            fprintf(f, "  %ux%ux%u, %u levels, format %u, %s\n", rows[i].w, rows[i].h, rows[i].d, rows[i].levels, rows[i].format, t);
        }
    }
    free(rows);
#if defined(__GLIBC__)
    struct mallinfo2 mi = mallinfo2();
    fprintf(f, "\nhost heap: %.1f MiB in arenas, %.1f MiB in use, %.1f MiB free but kept, %.1f MiB mapped\n",
            (double)mi.arena / 1048576.0, (double)mi.uordblks / 1048576.0, (double)mi.fordblks / 1048576.0,
            (double)mi.hblkhd / 1048576.0);
#endif
    fclose(f);
    sn.memory_written = true;
}

/** @brief Turns the GPU profile, the per-draw timestamps and the call counts on for the window; arms the frame trace. */
static void arm(uint64_t total)
{
    char path[1300];
    snprintf(path, sizeof path, "%s/gpu.txt", sn.dir);
    sn.gpu = fopen(path, "w");
    snprintf(path, sizeof path, "%s/batches.txt", sn.dir);
    sn.batches = fopen(path, "w");
    sn.was_profile = rl_setting_bool("gpu.profile");
    const char *d = getenv("RELIFT_GPU_PROFILE_DRAWS");
    sn.old_draws = d ? strdup(d) : NULL;
    nvn_lock();
    nvn_vk_settle();
    setenv("RELIFT_GPU_PROFILE_DRAWS", "1", 1);
    rl_settings_set("gpu.profile", "true");
    nvn_gpuprof_reset();
    nvn_vk_count_on = 1;
    nvn_unlock();
    /* The frame trace: the next present whole, where the layer keeps its own folder layout (frame-<n>/trace.jsonl). */
    snprintf(path, sizeof path, "%s/trace", sn.dir);
    sn.trace_from = g.presents + 1;
    nvn_trace_arm(path, sn.trace_from);
    sn.traced = true;
    /* Rebase the report's running counts on this present, so the first reported present is the first of the window. */
    nvn_vk_report_now(total);
    if (sn.gpu)
        fprintf(sn.gpu, "GPU profile of the %u presents after the trigger, one block per present; every draw is timed.\n"
                        "Timestamps serialize the GPU, so compare parts with each other, not with the unprofiled frame time.\n",
                sn.window);
    if (sn.batches)
        fprintf(sn.batches, "Why each batch ended, and the host Vulkan calls per command and per draw, one block per present.\n");
    rl_log_tee(tee, NULL);
    atomic_store(&sn.state, SN_RUNNING);
}

/** @brief Ends the window's work: turns what arm turned on back off and writes the holdings. */
static void finish(void)
{
    rl_log_tee(NULL, NULL);
    nvn_trace_disarm();
    nvn_lock();
    nvn_vk_settle();
    rl_settings_set("gpu.profile", sn.was_profile ? "true" : "false");
    if (sn.old_draws)
        setenv("RELIFT_GPU_PROFILE_DRAWS", sn.old_draws, 1);
    else
        unsetenv("RELIFT_GPU_PROFILE_DRAWS");
    nvn_gpuprof_reset();
    if (!sn.was_profile)
        nvn_vk_count_on = 0;
    nvn_unlock();
    free(sn.old_draws);
    sn.old_draws = NULL;
    if (sn.gpu)
        fclose(sn.gpu);
    if (sn.batches)
        fclose(sn.batches);
    sn.gpu = sn.batches = NULL;
    write_holdings();
    atomic_store(&sn.state, SN_DONE);
}

void nvn_snapshot_step(uint64_t total)
{
    static bool registered;
    if (!registered) {
        registered = true;
        nvn_snapshot_register();
    }
    int s = atomic_load_explicit(&sn.state, memory_order_relaxed);
    if (s == SN_IDLE || s == SN_DONE)
        return;
    if (s == SN_REQUESTED) {
        arm(total);
        return;
    }
    sn.got++;
    FILE *fs[2] = { sn.gpu, sn.batches };
    for (int i = 0; i < 2; i++)
        if (fs[i])
            fprintf(fs[i], "\n== present %llu (%u of %u after the trigger) ==\n", (unsigned long long)total, sn.got, sn.window);
    nvn_vk_report_now(total);
    if (sn.got >= sn.window)
        finish();
}

static void snap_end(void *user, const char *dir)
{
    (void)user, (void)dir;
    int s = atomic_load(&sn.state);
    if (s == SN_REQUESTED) {
        /* The layer never saw a present of the window (no NVN presents): nothing was armed. */
        atomic_store(&sn.state, SN_IDLE);
        return;
    }
    if (s == SN_RUNNING)
        finish();
    if (atomic_load(&sn.state) == SN_DONE) {
        rl_snapshot_file("gpu.txt", "The GPU profile of each present of the window: time per render pass and per draw (named by its programs and target size), dispatches, copies and the layer's own work, plus the gaps between batches and the presenter's own GPU time.");
        rl_snapshot_file("batches.txt", "Per present: how many batches there were and where each ended (the call that forced the batch out), the NVN commands replayed and the host Vulkan calls by command, in total and per draw.");
        if (sn.memory_written)
            rl_snapshot_file("memory_nvn.txt", "What the NVN layer holds: pipelines, pipeline libraries, shader modules, command sets, mirrors, staging buffers, and the textures by size with the largest listed.");
        char name[96], path[1400];
        snprintf(name, sizeof name, "trace/frame-%llu/trace.jsonl", (unsigned long long)sn.trace_from);
        snprintf(path, sizeof path, "%s/%s", sn.dir, name);
        FILE *f = fopen(path, "r");
        if (f) {
            fclose(f);
            rl_snapshot_file(name, "The frame trace of one present in RELIFT_NVN_TRACE's format: every NVN command with what it bound. Read it with runtime/tools/nvn_trace.py; bufs/ and imgs/ beside it hold the bytes it refers to.");
            snprintf(name, sizeof name, "trace/frame-%llu/order.jsonl", (unsigned long long)sn.trace_from);
            rl_snapshot_file(name, "The same present's submits, event waits and signals in queue order (nvn_trace.py order).");
        }
    }
    atomic_store(&sn.state, SN_IDLE);
}

void nvn_snapshot_register(void)
{
    rl_snapshot_provider p = { .name = "nvn", .begin = snap_begin, .ready = snap_ready, .end = snap_end };
    rl_snapshot_register(&p);
}

#else

void nvn_snapshot_step(uint64_t total) { (void)total; }
void nvn_snapshot_register(void) {}

#endif
