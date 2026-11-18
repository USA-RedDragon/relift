/**
 * @file test_window.c
 * @brief rl_window_main: headless runs call the body inline; with a backend
 * the body runs on another thread while the caller runs the event loop, and
 * events reach the listener; pointer mapping and scaling pass through to
 * the backend when it has them.
 */
#include "runner.h"

#include <relift/window.h>

#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <unistd.h>
#include <stdlib.h>

static pthread_t g_loop_thread;
static int g_inits, g_runs, g_wakes, g_fail_init;
static int g_events[8];
static int g_nevents;
static rl_window g_win;

static int fake_init(void)
{
    g_inits++;
    return g_fail_init ? -1 : 0;
}

static void fake_run(bool (*done)(void *user), void *user)
{
    g_runs++;
    g_loop_thread = pthread_self();
    rl_window_emit(&g_win, RL_WINDOW_FOCUS_GAINED);
    while (!done(user))
        sched_yield();
}

static void fake_wake(void) { __atomic_add_fetch(&g_wakes, 1, __ATOMIC_RELAXED); }

static const rl_window_backend g_fake = {
    .name = "fake",
    .init = fake_init,
    .run = fake_run,
    .wake = fake_wake,
};

static void on_event(void *user, rl_window *w, int event)
{
    (void)user;
    if (w == &g_win && g_nevents < 8)
        g_events[g_nevents++] = event;
}

typedef struct seen {
    pthread_t thread;
    const rl_window_backend *backend;
} seen;

static int body(void *arg)
{
    seen *s = arg;
    s->thread = pthread_self();
    s->backend = rl_window_backend_current();
    return 42;
}

static void reset(void)
{
    g_inits = g_runs = g_wakes = g_nevents = g_fail_init = 0;
    unsetenv(RL_WINDOW_ENV);
}

TEST(window_main_without_backend_runs_inline)
{
    reset();
    rl_window_set_backend(&g_fake);
    g_fail_init = 1;
    seen s = { 0 };
    CHECK_EQ(rl_window_main(body, &s), 42);
    CHECK(pthread_equal(s.thread, pthread_self()));
    CHECK(s.backend == NULL);
    CHECK_EQ(g_inits, 1);
    CHECK_EQ(g_runs, 0);
    setenv(RL_WINDOW_ENV, "none", 1);
    g_fail_init = 0;
    CHECK_EQ(rl_window_main(body, &s), 42);
    CHECK_EQ(g_inits, 1);
    CHECK(s.backend == NULL);
    reset();
    rl_window_set_backend(NULL);
}

TEST(window_main_runs_the_loop_on_the_caller)
{
    reset();
    rl_window_set_backend(&g_fake);
    rl_window_set_events(on_event, NULL);
    seen s = { 0 };
    CHECK_EQ(rl_window_main(body, &s), 42);
    CHECK_EQ(g_runs, 1);
    CHECK(pthread_equal(g_loop_thread, pthread_self()));
    CHECK(!pthread_equal(s.thread, pthread_self()));
    CHECK(s.backend == &g_fake);
    CHECK(g_wakes >= 1);
    CHECK_EQ(g_nevents, 1);
    CHECK_EQ(g_events[0], RL_WINDOW_FOCUS_GAINED);
    rl_window_set_events(NULL, NULL);
    rl_window_emit(&g_win, RL_WINDOW_CLOSE);
    CHECK_EQ(g_nevents, 1);
    rl_window_set_backend(NULL);
}

static int g_closes;

static void on_close(void *user, rl_window *w, int event)
{
    (void)user;
    (void)w;
    if (event == RL_WINDOW_CLOSE)
        __atomic_add_fetch(&g_closes, 1, __ATOMIC_RELEASE);
}

static int signal_body(void *arg)
{
    int sig = *(int *)arg;
    kill(getpid(), sig);
    for (int i = 0; i < 2000 && !__atomic_load_n(&g_closes, __ATOMIC_ACQUIRE); i++)
        usleep(1000);
    return __atomic_load_n(&g_closes, __ATOMIC_ACQUIRE);
}

#if !defined(_WIN32)
TEST(window_headless_sigterm_and_sigint_close)
{
    reset();
    setenv(RL_WINDOW_ENV, "headless", 1);
    rl_window_set_events(on_close, NULL);
    int sig = SIGTERM;
    CHECK_EQ(rl_window_main(signal_body, &sig), 1);
    __atomic_store_n(&g_closes, 0, __ATOMIC_RELEASE);
    sig = SIGINT;
    CHECK_EQ(rl_window_main(signal_body, &sig), 1);
    rl_window_set_events(NULL, NULL);
    reset();
}
#endif

TEST(window_env_headless_and_sdl3_override)
{
    reset();
    rl_window_set_backend(&g_fake);
    seen s = { 0 };
    setenv(RL_WINDOW_ENV, "headless", 1);
    CHECK_EQ(rl_window_main(body, &s), 42);
    CHECK(s.backend == NULL);
    CHECK_EQ(g_inits, 0);
    setenv(RL_WINDOW_ENV, "sdl3", 1);
    CHECK_EQ(rl_window_main(body, &s), 42);
    CHECK(s.backend == &g_fake);
    setenv(RL_WINDOW_ENV, "bogus", 1);
    CHECK_EQ(rl_window_main(body, &s), 42);
    CHECK(s.backend == &g_fake);
    reset();
    rl_window_set_backend(NULL);
}

#ifndef RL_HOST_VIDEO
TEST(window_env_sdl3_without_the_backend_is_headless)
{
    reset();
    setenv(RL_WINDOW_ENV, "sdl3", 1);
    seen s = { 0 };
    CHECK_EQ(rl_window_main(body, &s), 42);
    CHECK(s.backend == NULL);
    reset();
}
#endif

static bool g_fit;

static bool fake_point(rl_window *w, int32_t x, int32_t y, uint32_t *fx, uint32_t *fy)
{
    (void)w;
    if (x < 0 || y < 0)
        return false;
    *fx = (uint32_t)x / 2;
    *fy = (uint32_t)y / 2;
    return true;
}

static void fake_fit(rl_window *w, bool fit)
{
    (void)w;
    g_fit = fit;
}

static const rl_window_backend g_pointing = {
    .name = "pointing",
    .frame_point = fake_point,
    .set_fit = fake_fit,
};

TEST(window_frame_point_and_fit_reach_a_backend_that_has_them)
{
    rl_window plain = { .b = &g_fake }, pointing = { .b = &g_pointing };
    uint32_t x = 7, y = 7;
    CHECK(!rl_window_frame_point(&plain, 10, 10, &x, &y));
    CHECK(!rl_window_frame_point(NULL, 10, 10, &x, &y));
    CHECK_EQ(x, 7);
    rl_window_set_fit(&plain, true);
    rl_window_set_fit(NULL, true);
    CHECK(!g_fit);
    CHECK(rl_window_frame_point(&pointing, 10, 21, &x, &y));
    CHECK_EQ(x, 5);
    CHECK_EQ(y, 10);
    CHECK(!rl_window_frame_point(&pointing, -1, 0, &x, &y));
    rl_window_set_fit(&pointing, true);
    CHECK(g_fit);
    rl_window_set_fit(&pointing, false);
    CHECK(!g_fit);
}
