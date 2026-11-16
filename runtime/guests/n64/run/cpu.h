/**
 * @file cpu.h
 * @brief The VR4300 CPU on the Nintendo 64 machine: the run's state.
 */
#ifndef RELIFT_N64_RUN_CPU_H
#define RELIFT_N64_RUN_CPU_H

#include "mips.h"
#include "n64.h"
#include "rsp.h"

#include <relift/n64/run.h>

/** @brief Called at every interrupt check (sync points that reach the runtime), for the host's stop and frame work. */
typedef void rl_n64_check_hook(void *user, rl_ctx *c);

/** @brief One run of the machine and its CPU. */
typedef struct rl_n64_run {
    rl_n64 *m;
    rl_sched *s;
    rl_sched_cpu *k;
    struct rl_thread *t;
    rl_ctx *c;
    uint64_t entry;
    rl_event timer;
    rl_n64_check_hook *check;
    void *check_user;
    /** @brief Where missed code writes RDRAM (PREFIX.rdram), or NULL for no dump. */
    const char *dump;
    /** @brief The RSP, or NULL before rl_n64_run_rsp. */
    rl_rsp *rsp;
} rl_n64_run;

/** @brief The run lifted code belongs to. */
rl_n64_run *rl_n64_run_current(void);
/**
 * @brief Sets up the scheduler, the CPU thread, the machine's hooks and the
 * high-level boot for boot variant cic with entry point entry: the machine
 * side (rl_n64_boot) and the CPU registers and CP0 state the boot code
 * leaves. Returns 0 or -1.
 */
int rl_n64_run_init(rl_n64_run *r, rl_n64 *m, uint32_t cic, uint32_t entry);
/**
 * @brief Gives the machine an RSP that runs the lifted images (kept, not
 * copied) out of the machine's IMEM. Returns 0, or -1 when the images do
 * not fit this runtime (why says why) or memory runs out.
 */
int rl_n64_run_rsp(rl_n64_run *r, const rl_rsp_image *const *images, uint32_t count, char *why, size_t n);
/** @brief Frees the scheduler and the RSP. */
void rl_n64_run_free(rl_n64_run *r);
/** @brief Runs the CPU from the entry point. Returns RL_OK or RL_PENDING. */
int rl_n64_run_start(rl_n64_run *r);

#endif
