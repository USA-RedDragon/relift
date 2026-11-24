/**
 * @file timers.c
 * @brief The interval timers of setitimer and getitimer, run by the helper
 * thread of process.c: ITIMER_REAL counts the monotonic clock and sends
 * SIGALRM, ITIMER_VIRTUAL and ITIMER_PROF count the process's processor
 * time (the host does not split user from system time per process here,
 * so both count all of it) and send SIGVTALRM and SIGPROF. A timer is
 * process wide, as the kernel's; it is not inherited by a fork and is kept
 * across an execve (process.c passes it on through RELIFT_LINUX_EXEC).
 *
 * The signal goes out through lx_signal_post, so a blocking call of the
 * thread it lands on returns EINTR, or restarts after a handler with
 * SA_RESTART, as the call's own rules say.
 */
#include "hostio.h"
#include "linux_priv.h"

#include <stdio.h>
#include <string.h>

enum { L_ITIMER_REAL, L_ITIMER_VIRTUAL, L_ITIMER_PROF, TIMERS };

/** @brief How often a processor time timer looks at the clock, at most. */
#define CPU_POLL_NS 10000000ll

typedef struct itimer {
    /** @brief When it fires next on its clock, 0 when disarmed. */
    int64_t due;
    /** @brief The interval it rearms with, 0 for a one-shot. */
    int64_t interval;
} itimer;

static lx_mutex g_lock = LX_MUTEX_INIT;
static itimer g_timers[TIMERS];

static const int g_signal[TIMERS] = { L_SIGALRM, L_SIGVTALRM, L_SIGPROF };

static int64_t clock_of(int which) { return which == L_ITIMER_REAL ? (int64_t)rl_clock_ns() : hio_cputime_ns(); }

static int64_t tv_ns(const int64_t tv[2]) { return tv[0] * 1000000000ll + tv[1] * 1000; }

static void ns_tv(int64_t ns, int64_t tv[2])
{
    if (ns < 0)
        ns = 0;
    tv[0] = ns / 1000000000ll;
    tv[1] = (ns % 1000000000ll) / 1000;
}

/** @brief The timer's interval and remaining time, as struct itimerval (interval sec, usec, value sec, usec); under g_lock. */
static void get_locked(int which, int64_t out[4])
{
    const itimer *t = &g_timers[which];
    ns_tv(t->interval, out);
    if (!t->due) {
        out[2] = out[3] = 0;
        return;
    }
    int64_t left = t->due - clock_of(which);
    /* An armed timer never reads as disarmed: the kernel rounds up. */
    ns_tv(left > 0 ? left : 1000, out + 2);
}

int64_t lx_itimer_run(void)
{
    int64_t next = -1;
    int fire[TIMERS] = { 0 };
    lx_mutex_lock(&g_lock);
    for (int w = 0; w < TIMERS; w++) {
        itimer *t = &g_timers[w];
        if (!t->due)
            continue;
        int64_t now = clock_of(w);
        if (now >= t->due) {
            fire[w] = 1;
            if (t->interval > 0) {
                int64_t late = now - t->due;
                t->due += t->interval * (1 + late / t->interval);
            } else {
                t->due = 0;
            }
        }
        if (t->due) {
            int64_t left = t->due - now;
            if (w != L_ITIMER_REAL && left > CPU_POLL_NS)
                left = CPU_POLL_NS;
            if (next < 0 || left < next)
                next = left;
        }
    }
    lx_mutex_unlock(&g_lock);
    for (int w = 0; w < TIMERS; w++)
        if (fire[w])
            lx_signal_post(g_signal[w], 0x80 /* SI_KERNEL */, 0, 0);
    return next;
}

void lx_itimer_fork_child(void)
{
    static const lx_mutex fresh = LX_MUTEX_INIT;
    g_lock = fresh;
    memset(g_timers, 0, sizeof g_timers);
}

int lx_itimer_save(char *out, size_t cap)
{
    lx_mutex_lock(&g_lock);
    int n = 0;
    for (int w = 0; w < TIMERS; w++) {
        const itimer *t = &g_timers[w];
        long long left = t->due ? t->due - clock_of(w) : 0;
        if (t->due && left <= 0)
            left = 1;
        n += snprintf(out + n, cap > (size_t)n ? cap - (size_t)n : 0, w ? ",%lld:%lld" : "%lld:%lld", left, (long long)t->interval);
    }
    lx_mutex_unlock(&g_lock);
    return n;
}

void lx_itimer_restore(const char *s)
{
    lx_mutex_lock(&g_lock);
    for (int w = 0; w < TIMERS && s && *s; w++) {
        long long left = 0, interval = 0;
        if (sscanf(s, "%lld:%lld", &left, &interval) != 2)
            break;
        g_timers[w].interval = interval;
        g_timers[w].due = left > 0 ? clock_of(w) + left : 0;
        s = strchr(s, ',');
        if (s)
            s++;
    }
    lx_mutex_unlock(&g_lock);
}

static int64_t sys_getitimer(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if (a[0] >= TIMERS)
        return -L_EINVAL;
    int64_t v[4];
    lx_mutex_lock(&g_lock);
    get_locked((int)a[0], v);
    lx_mutex_unlock(&g_lock);
    return lx_put(a[1], v, sizeof v) ? 0 : -L_EFAULT;
}

static int64_t sys_setitimer(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if (a[0] >= TIMERS)
        return -L_EINVAL;
    int which = (int)a[0];
    int64_t nv[4] = { 0, 0, 0, 0 };
    if (a[1] && !lx_get(nv, a[1], sizeof nv))
        return -L_EFAULT;
    if (nv[0] < 0 || nv[1] < 0 || nv[1] >= 1000000 || nv[2] < 0 || nv[3] < 0 || nv[3] >= 1000000)
        return -L_EINVAL;
    int64_t old[4];
    lx_mutex_lock(&g_lock);
    get_locked(which, old);
    itimer *t = &g_timers[which];
    int64_t value = tv_ns(nv + 2);
    t->interval = tv_ns(nv);
    t->due = value ? clock_of(which) + value : 0;
    lx_mutex_unlock(&g_lock);
    lx_helper_kick();
    if (a[2] && !lx_put(a[2], old, sizeof old))
        return -L_EFAULT;
    return 0;
}

lx_sys_fn *lx_timer_call(uint32_t nr)
{
    switch (nr) {
    case NR_getitimer: return sys_getitimer;
    case NR_setitimer: return sys_setitimer;
    default: return NULL;
    }
}
