/**
 * @file vk_capture.c
 * @brief Debug capture of presented guest images: with RELIFT_VK_CAPTURE set
 * to <dir>[:N], every Nth vkQueuePresentKHR writes the guest swapchain image
 * as an 8-bit RGB PNG named frame-<frame number>-<guest ms>ms.png (png.h). Off by
 * default. A writer
 * thread waits for the GPU copy and writes the file, so the present that
 * takes the capture does not wait for either.
 */
#include "vkf.h"
#include "hostio_win.h"
#include "png.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static pthread_mutex_t g_cap_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_cap_read;
static char g_cap_dir[PATH_MAX];
static uint64_t g_cap_every = 1;
static uint64_t g_cap_frame;
/** @brief The presents [from, to) that RELIFT_VK_CAPTURE_WINDOW=<from>-<to> limits capturing to; to is 0 without a window. */
static uint64_t g_cap_from, g_cap_to;

static void set_spec(const char *spec)
{
    g_cap_dir[0] = 0;
    g_cap_every = 1;
    g_cap_frame = 0;
    g_cap_from = g_cap_to = 0;
    const char *win = getenv("RELIFT_VK_CAPTURE_WINDOW");
    if (win && sscanf(win, "%llu-%llu", (unsigned long long *)&g_cap_from, (unsigned long long *)&g_cap_to) != 2)
        g_cap_from = g_cap_to = 0;
    if (!spec || !*spec)
        return;
    const char *colon = strrchr(spec, ':');
    size_t n = colon ? (size_t)(colon - spec) : strlen(spec);
    if (colon) {
        char *end;
        unsigned long long every = strtoull(colon + 1, &end, 10);
        if (*end || !every) {
            rl_log(RL_LOG_ERROR, "vk", "%s=%s: the part after ':' must be a frame count above 0", RL_VK_CAPTURE_ENV,
                   spec);
            return;
        }
        g_cap_every = every;
    }
    if (!n || n >= sizeof g_cap_dir)
        return;
    memcpy(g_cap_dir, spec, n);
    g_cap_dir[n] = 0;
    if (mkdir(g_cap_dir, 0777) != 0 && errno != EEXIST) {
        rl_log(RL_LOG_ERROR, "vk", "%s: cannot create %s: %s", RL_VK_CAPTURE_ENV, g_cap_dir, strerror(errno));
        g_cap_dir[0] = 0;
        return;
    }
    rl_log(RL_LOG_INFO, "vk", "capturing every %llu presented frame%s to %s", (unsigned long long)g_cap_every,
           g_cap_every == 1 ? "" : "s", g_cap_dir);
}

#define QUEUE_MAX 4u

static pthread_mutex_t g_q_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_q_work = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_q_room = PTHREAD_COND_INITIALIZER;
static vkf_capture_job g_q[QUEUE_MAX];
static uint32_t g_q_head, g_q_count, g_q_busy;
static bool g_q_started;

static void *writer_main(void *arg)
{
    (void)arg;
    pthread_mutex_lock(&g_q_lock);
    for (;;) {
        while (!g_q_count)
            pthread_cond_wait(&g_q_work, &g_q_lock);
        vkf_capture_job j = g_q[g_q_head];
        g_q_head = (g_q_head + 1) % QUEUE_MAX;
        g_q_count--;
        g_q_busy++;
        pthread_mutex_unlock(&g_q_lock);
        if (j.ready)
            j.ready(j.arg);
        vkf_capture_write(j.frame, j.guest_ns, j.w, j.h, j.px, j.stride, j.bgra);
        if (j.done)
            j.done(j.arg);
        pthread_mutex_lock(&g_q_lock);
        g_q_busy--;
        pthread_cond_broadcast(&g_q_room);
    }
    return NULL;
}

void vkf_capture_queue(const vkf_capture_job *job)
{
    pthread_mutex_lock(&g_q_lock);
    if (!g_q_started) {
        pthread_t t;
        g_q_started = pthread_create(&t, NULL, writer_main, NULL) == 0;
        if (g_q_started)
            pthread_detach(t);
    }
    if (!g_q_started) {
        pthread_mutex_unlock(&g_q_lock);
        if (job->ready)
            job->ready(job->arg);
        vkf_capture_write(job->frame, job->guest_ns, job->w, job->h, job->px, job->stride, job->bgra);
        if (job->done)
            job->done(job->arg);
        return;
    }
    while (g_q_count == QUEUE_MAX)
        pthread_cond_wait(&g_q_room, &g_q_lock);
    g_q[(g_q_head + g_q_count) % QUEUE_MAX] = *job;
    g_q_count++;
    pthread_cond_signal(&g_q_work);
    pthread_mutex_unlock(&g_q_lock);
}

void vkf_capture_flush(void)
{
    pthread_mutex_lock(&g_q_lock);
    while (g_q_count || g_q_busy)
        pthread_cond_wait(&g_q_room, &g_q_lock);
    pthread_mutex_unlock(&g_q_lock);
}

void rl_vk_capture(const char *spec)
{
    vkf_capture_flush();
    pthread_mutex_lock(&g_cap_lock);
    set_spec(spec);
    g_cap_read = true;
    pthread_mutex_unlock(&g_cap_lock);
}

bool vkf_capture_due(uint64_t *frame)
{
    pthread_mutex_lock(&g_cap_lock);
    if (!g_cap_read) {
        set_spec(getenv(RL_VK_CAPTURE_ENV));
        g_cap_read = true;
    }
    uint64_t f = g_cap_frame++;
    bool due = g_cap_dir[0] && (g_cap_to ? f >= g_cap_from && f < g_cap_to : f % g_cap_every == 0);
    pthread_mutex_unlock(&g_cap_lock);
    *frame = f;
    return due;
}

bool vkf_capture_write(uint64_t frame, uint64_t guest_ns, uint32_t w, uint32_t h, const uint8_t *px, size_t stride,
                       bool bgra)
{
    char path[PATH_MAX + 64];
    pthread_mutex_lock(&g_cap_lock);
    snprintf(path, sizeof path, "%s/frame-%06llu-%08llums.png", g_cap_dir, (unsigned long long)frame,
             (unsigned long long)(guest_ns / 1000000u));
    pthread_mutex_unlock(&g_cap_lock);
    bool ok = rl_png_write_rgb(path, w, h, px, stride, bgra);
    if (!ok)
        rl_log(RL_LOG_ERROR, "vk", "capture: cannot write %s", path);
    return ok;
}
