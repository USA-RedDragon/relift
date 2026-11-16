/**
 * @file ndst.h
 * @brief The test fixture of the NDS machine: a machine attached to a
 * scheduler in ordered mode with the two CPUs (ratios 1 and 2), driven from
 * the test's own host thread by moving the CPUs' clocks. A timed ordered
 * access from the host thread must not need to wait (the other CPU has no
 * fiber to run): keep the other CPU's clock ahead.
 */
#ifndef RELIFT_NDS_NDST_H
#define RELIFT_NDS_NDST_H

#include "nds.h"
#include "runner.h"

/** @brief The fixture. */
typedef struct ndst {
    /** @brief The machine. */
    rl_nds *m;
    /** @brief Its scheduler. */
    rl_sched *s;
    /** @brief The scheduler CPUs, indexed by RL_NDS_ARM9 and RL_NDS_ARM7. */
    rl_sched_cpu *k[2];
} ndst;

/** @brief A fresh attached machine (one per test process). */
ndst *ndst_new(void);
/** @brief Moves cpu's clock forward to tick (rounded up to the CPU's cycle); nothing when it is there already. */
void ndst_to(ndst *t, unsigned cpu, uint64_t tick);
/** @brief Moves both clocks to tick. */
void ndst_both(ndst *t, uint64_t tick);

/**
 * @brief Runs the op stream ops (scene.h) as untimed ARM9 accesses; the
 * values its reads return go to out (at most max). A wait for a line moves
 * both clocks to the start of the next such line. Returns how many reads
 * there were.
 */
uint32_t ndst_scene(ndst *t, const uint32_t *ops, uint32_t *out, uint32_t max);

/** @brief An ARM9 untimed read of size bytes. */
static inline uint32_t r9(ndst *t, uint32_t a, unsigned size) { return rl_nds_read(t->m, RL_NDS_ARM9, a, size); }
/** @brief An ARM7 untimed read of size bytes. */
static inline uint32_t r7(ndst *t, uint32_t a, unsigned size) { return rl_nds_read(t->m, RL_NDS_ARM7, a, size); }
/** @brief An ARM9 untimed write of size bytes. */
static inline void w9(ndst *t, uint32_t a, unsigned size, uint32_t v) { rl_nds_write(t->m, RL_NDS_ARM9, a, size, v); }
/** @brief An ARM7 untimed write of size bytes. */
static inline void w7(ndst *t, uint32_t a, unsigned size, uint32_t v) { rl_nds_write(t->m, RL_NDS_ARM7, a, size, v); }

#endif
