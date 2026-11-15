/**
 * @file p2cpu.h
 * @brief A small ARM7TDMI interpreter for the probe replays: the ARM and
 * Thumb instructions the SWI probe's IWRAM stub, IRQ handler and scan routine
 * use, on the machine's bus (rl_gba_fetch, rl_gba_load, rl_gba_store,
 * rl_gba_idle) with the instruction timing of test_bus_console.c. A SWI and
 * the IRQ vector go to the high-level BIOS through the hooks.
 */
#ifndef RELIFT_GBA_P2CPU_H
#define RELIFT_GBA_P2CPU_H

#include "gba.h"

#include <stdbool.h>
#include <stdint.h>

enum { P2_USR = 0x10, P2_IRQ = 0x12, P2_SVC = 0x13, P2_SYS = 0x1F };

typedef struct p2cpu p2cpu;

/** @brief How the interpreter reaches the BIOS. */
typedef struct p2cpu_hooks {
    void *user;
    /** @brief A SWI after the exception entry (SVC mode, LR_svc, SPSR_svc set); returns where to continue, with CPSR restored by the hook. */
    void (*swi)(p2cpu *c, unsigned number);
} p2cpu_hooks;

struct p2cpu {
    rl_gba *g;
    rl_sched_cpu *sc;
    uint32_t r[16];
    uint32_t cpsr;
    uint32_t bank_sp[3], bank_lr[3], spsr[3];
    bool ns;
    p2cpu_hooks hooks;
    uint64_t steps;
    unsigned depth;
    bool msr_masked;
    uint64_t last;
};

/** @brief Executes one instruction (no interrupt check). */
void p2cpu_step(p2cpu *c);
/** @brief After p2cpu_irq: runs the handler until the BIOS stub returned from it (CPSR = SPSR_irq, pc = LR_irq - 4). */
bool p2cpu_run_irq(p2cpu *c, uint64_t limit);

void p2cpu_init(p2cpu *c, rl_gba *g, rl_sched_cpu *sc);
/** @brief Switches mode (banks r13, r14) and sets CPSR to v. */
void p2cpu_set_cpsr(p2cpu *c, uint32_t v);
uint32_t *p2cpu_spsr(p2cpu *c);
/** @brief Branches to target (bit 0 selects Thumb) with the pipeline reload. */
void p2cpu_jump(p2cpu *c, uint32_t target);
/** @brief Sets the program counter after code that filled the pipeline itself (the BIOS's return). */
void p2cpu_resume(p2cpu *c, uint32_t pc);
/** @brief Takes the IRQ exception with LR_irq = lr and SPSR_irq = spsr, and runs the BIOS stub's entry; the CPU is at the handler. */
void p2cpu_irq(p2cpu *c, uint32_t lr, uint32_t spsr);
/** @brief Runs until the program counter is stop (an IRQ is taken at every instruction boundary where it is allowed). Returns false after limit instructions. */
bool p2cpu_run(p2cpu *c, uint32_t stop, uint64_t limit);
/** @brief The interrupt check of an instruction boundary: runs due events, takes the IRQ when allowed. */
bool p2cpu_poll(p2cpu *c);

#endif
