/**
 * @file input.c
 * @brief Host input snapshots: the source function and the published snapshot.
 */
#include <relift/input.h>
#include <relift/touchpad.h>

#include "core.h"

#include <pthread.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static rl_input_source_fn *g_fn;
static void *g_user;
static rl_input_snapshot g_last;
static bool g_have_last;

void rl_input_set_source(rl_input_source_fn *fn, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_fn = fn;
    g_user = user;
    pthread_mutex_unlock(&g_lock);
}

void rl_input_publish(const rl_input_snapshot *s)
{
    pthread_mutex_lock(&g_lock);
    g_last = *s;
    g_have_last = true;
    pthread_mutex_unlock(&g_lock);
}

void rl_input_sample(rl_input_snapshot *out)
{
    pthread_mutex_lock(&g_lock);
    rl_input_source_fn *fn = g_fn;
    void *user = g_user;
    if (!fn) {
        if (g_have_last) {
            *out = g_last;
        } else {
            memset(out, 0, sizeof *out);
            out->pads[0].connected = true;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (fn) {
        memset(out, 0, sizeof *out);
        fn(user, out);
    }
    rl_touchpad_fill(out->touch_pad, RL_INPUT_TOUCH_PAD);
}

static uint64_t g_host[RL_INPUT_HOST_KINDS];

void rl_input_host_event(enum rl_input_host_kind kind)
{
    static const char *const names[RL_INPUT_HOST_KINDS] = {"key", "mouse", "gamepad", "touch"};
    if ((unsigned)kind >= RL_INPUT_HOST_KINDS)
        return;
    uint64_t before = 0;
    for (int i = 0; i < RL_INPUT_HOST_KINDS; i++)
        before += __atomic_load_n(&g_host[i], __ATOMIC_RELAXED);
    __atomic_add_fetch(&g_host[kind], 1, __ATOMIC_RELAXED);
    if (!before)
        rl_log(RL_LOG_INFO, "input", "host input: first event (%s); this run has manual input", names[kind]);
}

uint64_t rl_input_host_events(enum rl_input_host_kind kind)
{
    if ((unsigned)kind < RL_INPUT_HOST_KINDS)
        return __atomic_load_n(&g_host[kind], __ATOMIC_RELAXED);
    uint64_t n = 0;
    for (int i = 0; i < RL_INPUT_HOST_KINDS; i++)
        n += __atomic_load_n(&g_host[i], __ATOMIC_RELAXED);
    return n;
}
