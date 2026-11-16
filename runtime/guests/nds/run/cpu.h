/**
 * @file cpu.h
 * @brief The two ARM CPUs on the Nintendo DS machine: one scheduler CPU,
 * lifted-code thread and dispatch table each, the bus calls, the code fetch
 * tables, the entry checks with their variants, the ARM9's CP15, the
 * high-level BIOS at SWI and IRQ entry, and the boot without a BIOS image.
 */
#ifndef RELIFT_NDS_RUN_CPU_H
#define RELIFT_NDS_RUN_CPU_H

#include <relift/nds/run.h>

#include "arm32.h"
#include "nds.h"
#include "ndsmain.h"

#include <stdbool.h>

/** @brief Counters of one CPU for --stats. */
typedef struct rl_nds_arm_stats {
    /** @brief SWIs run. */
    uint64_t swis;
    /** @brief SWIs the high-level BIOS does not implement (run as no-ops). */
    uint64_t unknown_swis;
    /** @brief Interrupts entered. */
    uint64_t irqs;
    /** @brief Entries that found other bytes than their function was lifted from and went on in another unit's. */
    uint64_t variants;
    /** @brief Entries and dispatch misses that found no lifted code (the program stopped). */
    uint64_t unfound;
    /** @brief Byte compares of entry checks (the others were known good). */
    uint64_t compares;
} rl_nds_arm_stats;

typedef struct rl_nds_run rl_nds_run;

/** @brief A function of a CPU's units, for the variant search. */
typedef struct nds_entry {
    /** @brief The entry, with bit 0 set in Thumb state. */
    uint32_t at;
    uint32_t unit, fn;
} nds_entry;

/** @brief One CPU. */
typedef struct rl_nds_arm {
    rl_nds_run *run;
    /** @brief RL_NDS_ARM9 or RL_NDS_ARM7. */
    unsigned id;
    rl_sched_cpu *k;
    struct rl_thread *t;
    rl_ctx *c;
    /** @brief Where the boot left the CPU. */
    uint32_t entry;
    /** @brief The exception vector base (the ARM9's CP15; 0 on the ARM7). */
    uint32_t vectors;
    /** @brief The I bit as the lifted code last wrote it (the context's copy may sit in a local of the lifted code). */
    uint8_t i;
    /** @brief Cycles of a code fetch (rl_ctx.waits). */
    uint8_t waits[16 * 6];
    /** @brief The dispatch table of the CPU's units. */
    rl_fn *table;
    uint64_t lo, count;
    /** @brief Every function of the CPU's units, sorted by entry: where a failed entry check looks for a variant. */
    nds_entry *entries;
    uint32_t entry_count;
    /** @brief How the CPU stopped: 0 running, 1 halted for good, 2 the program stopped. */
    int stopped;
    rl_nds_arm_stats stats;
} rl_nds_arm;

/** @brief Both CPUs on one machine. */
struct rl_nds_run {
    const rl_nds_program *p;
    rl_nds *m;
    rl_sched *s;
    rl_nds_arm cpu[2];
    /** @brief Per unit, the index of its first function in seen. */
    uint32_t *first;
    /** @brief Per function, the code epoch plus one at its last good entry check, 0 for none. */
    uint64_t *seen;
    uint32_t fn_total;
    /** @brief Stop with a report once a CPU passes this tick (0: never). */
    uint64_t max_ticks;
};

/**
 * @brief Creates the machine and its scheduler, binds both CPUs to threads,
 * builds their dispatch tables and boots the cartridge of p as the BIOS
 * leaves it. rl_core_init with rl_arm32_cpu must have run. Returns 0 or -1.
 */
int rl_nds_run_init(rl_nds_run *r, const rl_nds_program *p);
/** @brief Runs both CPUs on their fibers until each has stopped; returns 0 when both halted for good. */
int rl_nds_run_start(rl_nds_run *r);
/** @brief Frees what rl_nds_run_init made. */
void rl_nds_run_free(rl_nds_run *r);

#endif
