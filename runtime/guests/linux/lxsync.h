/**
 * @file lxsync.h
 * @brief A mutex and a condition variable over the host's primitives for the
 * Linux layer's threads: pthreads where the host has them, Win32 slim locks
 * and condition variables on Windows (the core links no pthread there).
 */
#ifndef RL_LINUX_SYNC_H
#define RL_LINUX_SYNC_H

#include <stdbool.h>
#include <stdint.h>

#if defined(_WIN32)
#include <windows.h>
typedef struct lx_mutex {
    SRWLOCK m;
} lx_mutex;
typedef struct lx_cond {
    CONDITION_VARIABLE c;
} lx_cond;
#define LX_MUTEX_INIT { SRWLOCK_INIT }
#define LX_COND_INIT { CONDITION_VARIABLE_INIT }
static inline void lx_mutex_lock(lx_mutex *m) { AcquireSRWLockExclusive(&m->m); }
static inline void lx_mutex_unlock(lx_mutex *m) { ReleaseSRWLockExclusive(&m->m); }
static inline void lx_cond_signal(lx_cond *c) { WakeConditionVariable(&c->c); }
static inline void lx_cond_broadcast(lx_cond *c) { WakeAllConditionVariable(&c->c); }
/** @brief Waits on c with m held, at most timeout_ns (negative: for ever). False when the wait timed out. */
static inline bool lx_cond_wait(lx_cond *c, lx_mutex *m, int64_t timeout_ns)
{
    DWORD ms = timeout_ns < 0 ? INFINITE : (DWORD)((timeout_ns + 999999) / 1000000);
    return SleepConditionVariableSRW(&c->c, &m->m, ms, 0) != 0;
}
#else
#include <errno.h>
#include <pthread.h>
#include <time.h>
typedef struct lx_mutex {
    pthread_mutex_t m;
} lx_mutex;
typedef struct lx_cond {
    pthread_cond_t c;
} lx_cond;
#define LX_MUTEX_INIT { PTHREAD_MUTEX_INITIALIZER }
#define LX_COND_INIT { PTHREAD_COND_INITIALIZER }
static inline void lx_mutex_lock(lx_mutex *m) { pthread_mutex_lock(&m->m); }
static inline void lx_mutex_unlock(lx_mutex *m) { pthread_mutex_unlock(&m->m); }
static inline void lx_cond_signal(lx_cond *c) { pthread_cond_signal(&c->c); }
static inline void lx_cond_broadcast(lx_cond *c) { pthread_cond_broadcast(&c->c); }
static inline bool lx_cond_wait(lx_cond *c, lx_mutex *m, int64_t timeout_ns)
{
    if (timeout_ns < 0)
        return pthread_cond_wait(&c->c, &m->m) == 0;
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    int64_t ns = (int64_t)t.tv_nsec + timeout_ns % 1000000000;
    t.tv_sec += (time_t)(timeout_ns / 1000000000 + ns / 1000000000);
    t.tv_nsec = (long)(ns % 1000000000);
    return pthread_cond_timedwait(&c->c, &m->m, &t) != ETIMEDOUT;
}
#endif

#endif
