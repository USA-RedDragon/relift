/**
 * @file audio_tick.c
 * @brief The audio timer: one host thread that calls every registered
 * callback once per 5 ms frame, on absolute deadlines so the rate does not
 * drift, and skips frames after a long stall instead of bursting.
 */
#include "hos_audio.h"

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <time.h>

/** @brief Most callbacks at once (renderers and outputs). */
#define TICK_MAX 32

typedef struct tick_entry {
    audio_tick_fn *fn;
    void *ctx;
    bool used;
} tick_entry;

static pthread_mutex_t g_mgmt = PTHREAD_MUTEX_INITIALIZER;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t idle;
    tick_entry e[TICK_MAX];
    uint32_t count;
    int running_index;
    bool thread_up;
    bool stop;
    pthread_t thread;
} g_tick = { .lock = PTHREAD_MUTEX_INITIALIZER, .idle = PTHREAD_COND_INITIALIZER, .running_index = -1 };

static void sleep_until(uint64_t deadline)
{
    uint64_t now = rl_clock_ns();
    if (deadline <= now)
        return;
    uint64_t d = deadline - now;
    struct timespec ts = { (time_t)(d / 1000000000ull), (long)(d % 1000000000ull) };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {
    }
}

static void *tick_main(void *arg)
{
    (void)arg;
    uint64_t next = rl_clock_ns() + AUDIO_FRAME_NS;
    for (;;) {
        sleep_until(next);
        uint64_t now = rl_clock_ns();
        next += AUDIO_FRAME_NS;
        if (now > next + 4 * AUDIO_FRAME_NS)
            next = now + AUDIO_FRAME_NS;
        pthread_mutex_lock(&g_tick.lock);
        if (g_tick.stop) {
            pthread_mutex_unlock(&g_tick.lock);
            return NULL;
        }
        for (int i = 0; i < TICK_MAX; i++) {
            if (!g_tick.e[i].used)
                continue;
            audio_tick_fn *fn = g_tick.e[i].fn;
            void *ctx = g_tick.e[i].ctx;
            g_tick.running_index = i;
            pthread_mutex_unlock(&g_tick.lock);
            fn(ctx);
            pthread_mutex_lock(&g_tick.lock);
            g_tick.running_index = -1;
            pthread_cond_broadcast(&g_tick.idle);
        }
        pthread_mutex_unlock(&g_tick.lock);
    }
}

void *audio_tick_add(audio_tick_fn *fn, void *ctx)
{
    void *h = NULL;
    pthread_mutex_lock(&g_mgmt);
    pthread_mutex_lock(&g_tick.lock);
    for (int i = 0; i < TICK_MAX && !h; i++) {
        if (g_tick.e[i].used)
            continue;
        g_tick.e[i] = (tick_entry){ fn, ctx, true };
        g_tick.count++;
        h = &g_tick.e[i];
    }
    if (h && !g_tick.thread_up) {
        g_tick.stop = false;
        if (pthread_create(&g_tick.thread, NULL, tick_main, NULL) == 0) {
            g_tick.thread_up = true;
        } else {
            ((tick_entry *)h)->used = false;
            g_tick.count--;
            h = NULL;
        }
    }
    pthread_mutex_unlock(&g_tick.lock);
    pthread_mutex_unlock(&g_mgmt);
    return h;
}

static void tick_join(void)
{
    pthread_mutex_lock(&g_tick.lock);
    bool join = g_tick.thread_up;
    g_tick.stop = true;
    g_tick.thread_up = false;
    pthread_t t = g_tick.thread;
    pthread_mutex_unlock(&g_tick.lock);
    if (join)
        pthread_join(t, NULL);
}

void audio_tick_remove(void *handle)
{
    if (!handle)
        return;
    tick_entry *e = handle;
    pthread_mutex_lock(&g_mgmt);
    pthread_mutex_lock(&g_tick.lock);
    e->used = false;
    g_tick.count--;
    while (g_tick.running_index == (int)(e - g_tick.e))
        pthread_cond_wait(&g_tick.idle, &g_tick.lock);
    bool last = g_tick.count == 0;
    pthread_mutex_unlock(&g_tick.lock);
    if (last)
        tick_join();
    pthread_mutex_unlock(&g_mgmt);
}

uint32_t audio_tick_count(void)
{
    pthread_mutex_lock(&g_tick.lock);
    uint32_t n = g_tick.count;
    pthread_mutex_unlock(&g_tick.lock);
    return n;
}

void hos_audio_shutdown(void)
{
    pthread_mutex_lock(&g_mgmt);
    pthread_mutex_lock(&g_tick.lock);
    for (int i = 0; i < TICK_MAX; i++)
        g_tick.e[i].used = false;
    g_tick.count = 0;
    pthread_mutex_unlock(&g_tick.lock);
    tick_join();
    pthread_mutex_unlock(&g_mgmt);
}
