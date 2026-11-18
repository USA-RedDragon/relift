/**
 * @file sched_int.h
 * @brief The device scheduler's records, shared by sched.c (the scheduler,
 * which needs only the C library, the host OS and the fibers) and sync.c
 * (the lifted-code entries, which need the rest of the core).
 */
#ifndef RELIFT_SCHED_INT_H
#define RELIFT_SCHED_INT_H

#include "fiber.h"

#include <relift/sched.h>

/** @brief rl_sched.holder when no CPU holds the run. */
#define RL_SCHED_NO_HOLDER UINT32_MAX

/** @brief One mapped guest range. */
typedef struct sched_range {
    /** @brief First guest address. */
    uint64_t lo;
    /** @brief Last guest address (inclusive, so a range may end at the top of the space). */
    uint64_t hi;
    /** @brief The device whose handlers answer. */
    rl_device *owner;
    /** @brief Devices caught up before an access: the owner and its observers. */
    uint64_t sync;
} sched_range;

/** @brief Binds a guest thread to the host thread when its CPU resumes (rl_thread_bind). */
typedef void rl_sched_bind_fn(struct rl_thread *t);

struct rl_sched_cpu {
    rl_sched_clock_state clock;
    rl_time own;
    uint32_t index;
    uint32_t rank;
    uint64_t lines;
    uint64_t mask;
    rl_irq_hook *irq;
    rl_irq_return_hook *irq_ret;
    uint32_t irq_depth;
    rl_sched *sched;
    struct rl_thread *thread;
    rl_fiber *fiber;
    rl_sched_body *body;
    void *arg;
    bool active;
    bool halted;
};

struct rl_sched {
    rl_sched_config cfg;
    rl_sched_stats stats;
    uint64_t time;
    uint64_t seq;
    rl_event **heap;
    uint32_t qn, qcap;
    rl_device *devs[RL_SCHED_MAX_DEVICES];
    uint32_t ndev;
    sched_range *ranges;
    uint32_t nr, rcap;
    uint32_t last;
    rl_sched_cpu cpus[RL_SCHED_MAX_CPUS];
    uint32_t ncpu;
    uint32_t route[RL_SCHED_LINES];
    uint32_t holder;
    bool started;
    bool in_main;
    uint32_t home_cpu;
    rl_fiber home;
    rl_fiber *running;
    rl_sched_bind_fn *bind;
};

#endif
