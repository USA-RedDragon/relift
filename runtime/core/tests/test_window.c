/**
 * @file test_window.c
 * @brief rl_window_main: headless runs call the body inline; with a backend
 * the body runs on another thread while the caller runs the event loop, and
 * events reach the listener.
 */
#include "runner.h"

#include <relift/window.h>

#include <pthread.h>
#include <sched.h>
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
