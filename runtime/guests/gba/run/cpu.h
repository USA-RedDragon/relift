/**
 * @file cpu.h
 * @brief The ARMv4T CPU on the Game Boy Advance machine: one scheduler CPU
 * bound to a lifted-code thread, the bus calls, the code fetch table, the
 * high-level BIOS at SWI and IRQ entry, and the interrupt lines the I bit
 * gates.
 */
#ifndef RELIFT_GBA_RUN_CPU_H
#define RELIFT_GBA_RUN_CPU_H

#include <relift/gba/run.h>

#include "arm32.h"
#include "gba.h"
#include "gbamain.h"

/** @brief Called at every interrupt check (a sync point whose budget ran out) with the registers stored. */
typedef void rl_gba_check_fn(void *user, rl_ctx *c);

/** @brief Counters for --stats. */
typedef struct rl_gba_run_stats {
    /** @brief SWIs run. */
    uint64_t swis;
    /** @brief SWIs the BIOS does not implement (run as no-ops). */
    uint64_t unknown_swis;
    /** @brief Interrupts entered at sync points. */
    uint64_t irqs;
    /** @brief Interrupts entered inside a BIOS call (IntrWait, Halt). */
    uint64_t bios_irqs;
    /** @brief Instructions in RAM that found other code than they were lifted from. */
    uint64_t code_changes;
} rl_gba_run_stats;


/** @brief The CPU of one machine. */
typedef struct rl_gba_run {
    rl_gba *g;
    rl_sched *s;
    rl_sched_cpu *k;
    struct rl_thread *t;
    rl_ctx *c;
    /** @brief The entry point of the cartridge. */
    uint32_t entry;
    rl_gba_check_fn *check;
    void *check_user;
    rl_gba_run_stats stats;
    /** @brief Every lifted function in work RAM, sorted by at and unit (rl_gba_cpu_variant). */
    const rl_gba_variant *variants;
    uint32_t variant_count;
} rl_gba_run;

/**
 * @brief Binds a new thread and scheduler CPU to g (attached to a new
 * scheduler) and sets the state the BIOS hands to the cartridge: registers,
 * stacks, I/O. rl_core_init with rl_arm32_cpu must have run. Returns 0 or -1.
 */
int rl_gba_run_init(rl_gba_run *r, rl_gba *g);
/** @brief Frees what rl_gba_run_init made (not g). */
void rl_gba_run_free(rl_gba_run *r);
/** @brief Runs the cartridge from its entry point until the guest stops; returns RL_OK or RL_PENDING. */
int rl_gba_run_start(rl_gba_run *r);
/** @brief The run of the calling thread, or NULL. */
rl_gba_run *rl_gba_run_current(void);

#endif
