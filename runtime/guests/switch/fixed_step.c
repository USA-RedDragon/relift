/**
 * @file fixed_step.c
 * @brief The fixed-step clock of clock.fixed_step: a guest clock that only
 * the guest's own progress moves, and the timers that run on it.
 *
 * In fixed-step mode one guest thread runs at a time (the scheduler's token,
 * ksched.c) and the guest never sees a host clock. The guest clock starts at
 * FS_EPOCH_NS and moves at three kinds of points, all decided by the guest's
 * own code:
 * - each preemption point and svc of the running thread adds HOS_FS_POINT_NS;
 * - a yield with no other thread to run, and a moment when no thread can run
 *   at all, move it to the next deadline: a timer (the vi vsync, the audio
 *   frame, the hid sampling) or the timeout of a wait;
 * - at each of those points the timers and waits that are due run, in
 *   deadline order, timers before waits at the same time, timers in the
 *   order they were added and waits by thread id.
 * Two runs of the same program with the same input therefore see the same
 * clock values, the same timer ticks and the same thread order.
 *
 * Timer callbacks run without the kernel lock while no guest code runs: on
 * the thread that holds the token, or on the clock thread when every guest
 * thread waits.
 */
#include "hos.h"

#include <stdlib.h>
#include <string.h>

/** @brief The guest clock at start (8 s, a console's uptime when a game starts). */
#define FS_EPOCH_NS 8000000000ull
/** @brief The guest wall clock at start: 2026-01-01 00:00:00 UTC. */
#define FS_WALL_EPOCH_S 1767225600ll
/** @brief Most timers at once. */
#define FS_TIMERS 16
/** @brief Seed of the pseudo-random sequence that replaces host entropy. */
#define FS_SEED 0x52454C4946545321ull

typedef struct fs_timer {
    hos_fs_fn *fn;
    void *ctx;
    uint64_t next;
    uint64_t period;
    uint64_t order;
    bool used;
} fs_timer;

static struct {
    uint64_t now;
    uint64_t due_at;
    fs_timer timers[FS_TIMERS];
    uint64_t added;
    pthread_cond_t cv;
    pthread_t thread;
    bool thread_up;
    bool idle;
    bool quit;
    pthread_mutex_t rng_lock;
    uint64_t rng;
} g_fs = { .rng_lock = PTHREAD_MUTEX_INITIALIZER };

static uint64_t fs_now(void) { return __atomic_load_n(&g_fs.now, __ATOMIC_ACQUIRE); }

uint64_t hos_clock_ns(void) { return hos.sched.fixed ? fs_now() : rl_clock_ns(); }

int64_t hos_fs_wall_seconds(void) { return FS_WALL_EPOCH_S + (int64_t)((fs_now() - FS_EPOCH_NS) / 1000000000ull); }

void hos_fs_init(void)
{
    memset(g_fs.timers, 0, sizeof g_fs.timers);
    g_fs.added = 0;
    g_fs.idle = g_fs.quit = false;
    __atomic_store_n(&g_fs.now, FS_EPOCH_NS, __ATOMIC_RELEASE);
    __atomic_store_n(&g_fs.due_at, UINT64_MAX, __ATOMIC_RELEASE);
    pthread_cond_init(&g_fs.cv, NULL);
    pthread_mutex_lock(&g_fs.rng_lock);
    g_fs.rng = FS_SEED;
    pthread_mutex_unlock(&g_fs.rng_lock);
    rl_set_guest_clock(fs_now);
}

void hos_fs_shutdown(void)
{
    hos_lock();
    g_fs.quit = true;
    bool join = g_fs.thread_up;
    g_fs.thread_up = false;
    pthread_cond_signal(&g_fs.cv);
    hos_unlock();
    if (join)
        pthread_join(g_fs.thread, NULL);
    hos_lock();
    memset(g_fs.timers, 0, sizeof g_fs.timers);
    hos_unlock();
    rl_set_guest_clock(NULL);
}

void hos_fs_charge(uint64_t ns) { __atomic_add_fetch(&g_fs.now, ns, __ATOMIC_ACQ_REL); }

bool hos_fs_due(void) { return fs_now() >= __atomic_load_n(&g_fs.due_at, __ATOMIC_ACQUIRE); }

static void note_due(uint64_t at)
{
    if (at < __atomic_load_n(&g_fs.due_at, __ATOMIC_RELAXED))
        __atomic_store_n(&g_fs.due_at, at, __ATOMIC_RELEASE);
}

uint64_t hos_fs_next_deadline(void)
{
    uint64_t at = UINT64_MAX;
    for (int i = 0; i < FS_TIMERS; i++)
        if (g_fs.timers[i].used && g_fs.timers[i].next < at)
            at = g_fs.timers[i].next;
    for (hos_thread *t = hos.threads; t; t = t->next_all)
        if (t->deadline && t->deadline < at)
            at = t->deadline;
    return at;
}

void hos_fs_set_deadline(hos_thread *t, int64_t timeout_ns)
{
    t->expired = false;
    t->deadline = 0;
    if (timeout_ns < 0)
        return;
    uint64_t now = fs_now();
    t->deadline = (uint64_t)timeout_ns > UINT64_MAX - now ? UINT64_MAX - 1 : now + (uint64_t)timeout_ns;
    note_due(t->deadline);
}

void *hos_fs_every(uint64_t first, uint64_t period, hos_fs_fn *fn, void *ctx)
{
    void *h = NULL;
    hos_lock();
    for (int i = 0; i < FS_TIMERS && !h; i++) {
        fs_timer *t = &g_fs.timers[i];
        if (t->used)
            continue;
        *t = (fs_timer){ .fn = fn, .ctx = ctx, .next = first, .period = period, .order = g_fs.added++, .used = true };
        note_due(first);
        h = t;
    }
    hos_unlock();
    return h;
}

void hos_fs_cancel(void *timer)
{
    if (!timer)
        return;
    hos_lock();
    ((fs_timer *)timer)->used = false;
    hos_unlock();
}

static fs_timer *first_timer(uint64_t now)
{
    fs_timer *best = NULL;
    for (int i = 0; i < FS_TIMERS; i++) {
        fs_timer *t = &g_fs.timers[i];
        if (t->used && t->next <= now &&
            (!best || t->next < best->next || (t->next == best->next && t->order < best->order)))
            best = t;
    }
    return best;
}

static hos_thread *first_wait(uint64_t now)
{
    hos_thread *best = NULL;
    for (hos_thread *t = hos.threads; t; t = t->next_all)
        if (t->deadline && t->deadline <= now &&
            (!best || t->deadline < best->deadline || (t->deadline == best->deadline && t->id < best->id)))
            best = t;
    return best;
}

static void expire(hos_thread *t)
{
    t->deadline = 0;
    t->expired = true;
    pthread_cond_signal(&t->cv);
    hos_sched_ready(t);
}

void hos_fs_fire_due(void)
{
    for (;;) {
        hos_lock();
        uint64_t now = fs_now();
        fs_timer *tm = first_timer(now);
        hos_thread *w = first_wait(now);
        if (w && (!tm || w->deadline < tm->next)) {
            expire(w);
            hos_unlock();
            continue;
        }
        if (!tm) {
            __atomic_store_n(&g_fs.due_at, hos_fs_next_deadline(), __ATOMIC_RELEASE);
            hos_unlock();
            return;
        }
        hos_fs_fn *fn = tm->fn;
        void *ctx = tm->ctx;
        if (tm->period)
            tm->next += tm->period;
        else
            tm->used = false;
        hos_unlock();
        fn(ctx);
    }
}

static bool anyone_ready(void)
{
    if (hos.sched.token)
        return true;
    for (int32_t c = 0; c < HOS_CORES; c++) {
        hos_thread *t = hos.sched.cores[c].selected;
        if (t && t->runnable)
            return true;
    }
    return false;
}

static void *clock_main(void *arg)
{
    (void)arg;
    hos_lock();
    for (;;) {
        while (!g_fs.idle && !g_fs.quit)
            pthread_cond_wait(&g_fs.cv, &hos.lock);
        if (g_fs.quit)
            break;
        g_fs.idle = false;
        if (anyone_ready() || hos.sched.clock_busy)
            continue;
        uint64_t next = hos_fs_next_deadline();
        if (next == UINT64_MAX)
            continue;
        hos.sched.clock_busy = true;
        if (next > fs_now())
            __atomic_store_n(&g_fs.now, next, __ATOMIC_RELEASE);
        hos_unlock();
        hos_fs_fire_due();
        hos_lock();
        hos.sched.clock_busy = false;
        hos_sched_kick();
    }
    hos_unlock();
    return NULL;
}

void hos_fs_idle(void)
{
    if (g_fs.quit || !hos.live_threads)
        return;
    if (!g_fs.thread_up) {
        if (pthread_create(&g_fs.thread, NULL, clock_main, NULL) != 0) {
            rl_log(RL_LOG_ERROR, "sched", "fixed step: no clock thread; the guest clock stops");
            return;
        }
        g_fs.thread_up = true;
    }
    g_fs.idle = true;
    pthread_cond_signal(&g_fs.cv);
}

void hos_fs_random(void *dst, size_t n)
{
    uint8_t *p = dst;
    pthread_mutex_lock(&g_fs.rng_lock);
    while (n) {
        uint64_t z = (g_fs.rng += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= z >> 31;
        size_t k = n < 8 ? n : 8;
        memcpy(p, &z, k);
        p += k;
        n -= k;
    }
    pthread_mutex_unlock(&g_fs.rng_lock);
}
