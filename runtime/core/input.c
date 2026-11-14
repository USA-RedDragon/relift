/**
 * @file input.c
 * @brief Host input snapshots: the source function and the published snapshot.
 */
#include <relift/input.h>

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
}
