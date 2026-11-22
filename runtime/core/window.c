/**
 * @file window.c
 * @brief The window backend in use, the event listener and rl_window_main.
 */
#include <relift/post.h>
#include <relift/settings.h>
#include <relift/touchpad.h>
#include <relift/window.h>

#include "capture.h"
#include "core.h"
#include "present.h"

#include <pthread.h>
#include <stdlib.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#endif
#include <string.h>
#include <time.h>

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

struct rl_present *rl_window_presenter(rl_window *w)
{
    return w && w->b && w->b->presenter ? w->b->presenter(w) : NULL;
}

bool rl_window_present_rgb_shown(rl_window *w, const uint8_t *rgb, uint32_t width, uint32_t height,
                                 uint32_t shown_width, uint32_t shown_height)
{
    rl_capture_present_begin();
    bool shown = w && w->b && w->b->present_rgb;
    if (shown) {
        if (!rl_window_presenter(w))
            rgb = rl_post_apply(rgb, width, height, &width, &height);
        w->b->present_rgb(w, rgb, width, height, shown_width, shown_height);
        while (!w->extra && rl_touchpad_hold(16))
            w->b->present_rgb(w, rgb, width, height, shown_width, shown_height);
    }
    rl_capture_present_end();
    return shown;
}

bool rl_window_present_rgb(rl_window *w, const uint8_t *rgb, uint32_t width, uint32_t height)
{
    return rl_window_present_rgb_shown(w, rgb, width, height, width, height);
}

bool rl_window_frame_point(rl_window *w, int32_t x, int32_t y, uint32_t *frame_x, uint32_t *frame_y)
{
    if (!w || !w->b || !w->b->frame_point)
        return false;
    return w->b->frame_point(w, x, y, frame_x, frame_y);
}

void rl_window_set_fit(rl_window *w, bool fit)
{
    if (w && w->b && w->b->set_fit)
        w->b->set_fit(w, fit);
}

uint32_t rl_window_display_count(void)
{
    const rl_window_backend *b = rl_window_backend_current();
    return b && b->display_count ? b->display_count() : 0;
}

rl_window *rl_window_open_display(uint32_t index)
{
    const rl_window_backend *b = rl_window_backend_current();
    rl_window *w = b && b->open_display ? b->open_display(index) : NULL;
    if (w)
        w->extra = true;
    return w;
}

void rl_window_close_display(rl_window *w)
{
    if (w && w->b && w->b->close)
        w->b->close(w);
}

void rl_window_size(rl_window *w, uint32_t *width, uint32_t *height)
{
    *width = *height = 0;
    if (w && w->b && w->b->size)
        w->b->size(w, width, height);
}

uint32_t rl_window_touches(rl_window *w, rl_window_touch *out, uint32_t max)
{
    return w && w->b && w->b->touches ? w->b->touches(w, out, max) : 0;
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

/** @brief How long the guest gets to leave after a quit signal asked it to (rl_window_set_quit_grace), in milliseconds. */
static uint32_t g_quit_grace_ms = 3000;
static int g_quit_forced;

void rl_window_set_quit_grace(uint32_t ms) { __atomic_store_n(&g_quit_grace_ms, ms, __ATOMIC_RELAXED); }

static void quit_sleep(uint32_t ms)
{
    struct timespec t = { ms / 1000, (long)(ms % 1000) * 1000000L };
#if defined(_WIN32)
    Sleep(ms);
    (void)t;
#else
    while (nanosleep(&t, &t) != 0)
        ;
#endif
}

typedef struct quit_force {
    uint32_t code;
} quit_force;

static void *quit_force_thread(void *p)
{
    uint32_t code = ((quit_force *)p)->code;
    free(p);
    quit_sleep(__atomic_load_n(&g_quit_grace_ms, __ATOMIC_RELAXED));
    rl_log(RL_LOG_INFO, "window", "the guest did not leave within the grace after the quit signal: ending the process");
    rl_process_exit(code);
    return NULL;
}

/**
 * @brief A quit signal (SIGTERM, SIGINT, or a console close on Windows) arrived: without a listener the process ends at
 * once with code; with one the listener is asked as for a window close, and a guest that has not ended the process
 * within the grace is ended anyway, so a signal always stops a run (and one that cannot start the timer ends it now).
 */
static void quit_requested(uint32_t code)
{
    pthread_mutex_lock(&g_lock);
    bool listened = g_fn != NULL;
    pthread_mutex_unlock(&g_lock);
    if (!listened)
        rl_process_exit(code);
    rl_window_emit(NULL, RL_WINDOW_CLOSE);
    if (__atomic_exchange_n(&g_quit_forced, 1, __ATOMIC_ACQ_REL))
        return;
    quit_force *f = malloc(sizeof *f);
    if (!f)
        rl_process_exit(code);
    f->code = code;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(&t, &attr, quit_force_thread, f);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(f);
        rl_process_exit(code);
    }
}

#if !defined(_WIN32)

static void *quit_watch(void *p)
{
    const sigset_t *set = p;
    for (;;) {
        int sig;
        if (sigwait(set, &sig) != 0)
            continue;
        quit_requested(128u + (uint32_t)sig);
    }
    return NULL;
}

static sigset_t g_quit_set;
static pthread_once_t g_quit_once = PTHREAD_ONCE_INIT;

static void quit_start(void)
{
    sigemptyset(&g_quit_set);
    sigaddset(&g_quit_set, SIGTERM);
    sigaddset(&g_quit_set, SIGINT);
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    sigset_t old;
    pthread_sigmask(SIG_BLOCK, &g_quit_set, &old);
    if (pthread_create(&t, &attr, quit_watch, &g_quit_set) != 0)
        pthread_sigmask(SIG_SETMASK, &old, NULL);
    pthread_attr_destroy(&attr);
}

void rl_window_quit_on_signals(void) { pthread_once(&g_quit_once, quit_start); }

#else

static BOOL WINAPI quit_console(DWORD type)
{
    if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT)
        return FALSE;
    quit_requested(type == CTRL_C_EVENT ? 130u : 143u);
    return TRUE;
}

void rl_window_quit_on_signals(void) { SetConsoleCtrlHandler(quit_console, TRUE); }

#endif

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
    rl_window_quit_on_signals();
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
