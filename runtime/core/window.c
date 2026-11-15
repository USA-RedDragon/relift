/**
 * @file window.c
 * @brief The window backend in use, the event listener and rl_window_main.
 */
#include <relift/settings.h>
#include <relift/touchpad.h>
#include <relift/window.h>

#include "core.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define BODY_STACK (16u << 20)

static const rl_window_backend *g_set;
static const rl_window_backend *g_up;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static rl_window_event_fn *g_fn;
static void *g_user;

void rl_window_set_backend(const rl_window_backend *b) { g_set = b; }

const rl_window_backend *rl_window_backend_current(void) { return __atomic_load_n(&g_up, __ATOMIC_ACQUIRE); }

void rl_window_set_events(rl_window_event_fn *fn, void *user)
{
    pthread_mutex_lock(&g_lock);
    g_fn = fn;
    g_user = user;
    pthread_mutex_unlock(&g_lock);
}

bool rl_window_present_rgb(rl_window *w, const uint8_t *rgb, uint32_t width, uint32_t height)
{
    if (!w || !w->b || !w->b->present_rgb)
        return false;
    w->b->present_rgb(w, rgb, width, height);
    while (rl_touchpad_hold(16))
        w->b->present_rgb(w, rgb, width, height);
    return true;
}

void rl_window_emit(rl_window *w, int event)
{
    pthread_mutex_lock(&g_lock);
    rl_window_event_fn *fn = g_fn;
    void *user = g_user;
    pthread_mutex_unlock(&g_lock);
    if (fn)
        fn(user, w, event);
}

static bool display_available(void)
{
#if (defined(__linux__) && !defined(__ANDROID__)) || defined(__FreeBSD__)
    const char *w = getenv("WAYLAND_DISPLAY"), *x = getenv("DISPLAY");
    return (w && *w) || (x && *x);
#else
    return true;
#endif
}

static const rl_window_backend *host_backend(void)
{
#ifdef RL_HOST_VIDEO
    return rl_host_window_backend();
#else
    return NULL;
#endif
}

static const rl_window_backend *wanted(void)
{
    rl_settings_declare_runtime();
    uint32_t v = rl_setting_enum("video");
    if (v == RL_VIDEO_HEADLESS || v == RL_VIDEO_NONE)
        return NULL;
    if (v == RL_VIDEO_SDL3) {
        const rl_window_backend *b = g_set ? g_set : host_backend();
        if (!b)
            rl_log(RL_LOG_WARN, "window", "video=sdl3, but this runtime was built without SDL3; running headless");
        return b;
    }
    if (g_set)
        return g_set;
    const rl_window_backend *b = host_backend();
    if (b && !display_available()) {
        rl_log(RL_LOG_INFO, "window", "no display (WAYLAND_DISPLAY and DISPLAY are unset); running headless");
        return NULL;
    }
    return b;
}

typedef struct body_run {
    int (*body)(void *);
    void *arg;
    int result;
    bool done;
} body_run;

static void *body_main(void *p)
{
    body_run *r = p;
    int result = r->body(r->arg);
    r->result = result;
    __atomic_store_n(&r->done, true, __ATOMIC_RELEASE);
    g_up->wake();
    return NULL;
}

static bool body_done(void *p) { return __atomic_load_n(&((body_run *)p)->done, __ATOMIC_ACQUIRE); }

int rl_window_main(int (*body)(void *arg), void *arg)
{
    const rl_window_backend *b = wanted();
    if (b && b->init() != 0) {
        rl_log(RL_LOG_INFO, "window", "%s has no display; running headless", b->name);
        b = NULL;
    }
    if (!b)
        return body(arg);
    __atomic_store_n(&g_up, b, __ATOMIC_RELEASE);
    body_run r = { .body = body, .arg = arg };
    pthread_attr_t attr;
    pthread_t t;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, BODY_STACK);
    int rc = pthread_create(&t, &attr, body_main, &r);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        rl_log(RL_LOG_ERROR, "window", "cannot start the program thread");
        return 1;
    }
    b->run(body_done, &r);
    pthread_join(t, NULL);
    __atomic_store_n(&g_up, NULL, __ATOMIC_RELEASE);
    return r.result;
}
