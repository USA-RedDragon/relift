/**
 * @file bios.h
 * @brief The high-level BIOS of the Game Boy Advance: the software
 * interrupts (SWI 0x00 to 0x2A), the IRQ stub behind vector 0x18, the
 * post-boot state and the value reads of the protected BIOS region return.
 *
 * No BIOS image is used or needed. Every call reproduces what the console's
 * BIOS leaves behind, bit for bit where it is known: r0, r1 and r3 (r2 and
 * r12 are restored by its dispatcher), the memory it writes, the frames its
 * dispatcher and routines push on the supervisor and system stacks, and the
 * I/O writes it makes. Cycle costs: a fixed cost per call plus a cost per event
 * (loop iterations, runs, multiplier cycles) fitted exactly to the console's
 * measured counts under WAITCNT 0x4317, plus the cycles the bus reports for
 * every data access, so the costs follow the machine's wait states.
 *
 * The caller is the ARM32 CPU layer at a SWI exit (or, for the IRQ stub, at
 * IRQ entry and at the stub's return address). It performs the exception
 * entry itself (LR_svc, SPSR_svc, the mode change) and passes the state the
 * BIOS sees through rl_gba_swi_cpu. All memory goes through rl_gba_bus,
 * which is the machine's memory map; the bus may run interrupts inside a
 * write of IME (0x04000208) or HALTCNT (0x04000301), exactly where the
 * console would take them (see rl_gba_bus).
 */
#ifndef RELIFT_GBA_BIOS_H
#define RELIFT_GBA_BIOS_H

#include <stdbool.h>
#include <stdint.h>

/** @brief What a read of the BIOS region returns after boot. */
#define RL_GBA_BIOS_OPEN_BOOT 0xE129F000u
/** @brief ... while an IRQ handler runs (called from the stub). */
#define RL_GBA_BIOS_OPEN_IRQ 0xE25EF004u
/** @brief ... after the IRQ stub returned. */
#define RL_GBA_BIOS_OPEN_IRQ_DONE 0xE55EC002u
/** @brief ... after a SWI returned. */
#define RL_GBA_BIOS_OPEN_SWI 0xE3A02004u

/** @brief The IRQ stub's return address: the handler returns here (BX LR). */
#define RL_GBA_BIOS_IRQ_RETURN 0x00000138u
/** @brief The return address inside the SWI dispatcher (LR of a routine): a routine leaves it in r3 or on its stack, and a handler inside IntrWait finds it in LR_sys. */
#define RL_GBA_BIOS_SWI_RETURN 0x00000170u
/**
 * @brief relift's slide: the address an interrupt taken inside a BIOS call
 * returns to (LR_irq - 4, ARM). relift runs no BIOS code; each cycle of a
 * call that the console spends with interrupts open is a check here, so the
 * interrupt lands on the console's cycle while the return address a handler
 * finds in its stack frame is relift's, not the console's.
 */
#define RL_GBA_BIOS_SLIDE 0x00003F00u

/**
 * @brief The machine's memory as the BIOS sees it.
 *
 * read and write take addresses aligned to size (1, 2 or 4); the BIOS does
 * the ARM7TDMI's rotation of misaligned loads itself. A read of an address
 * nothing answers returns open bus, which the machine supplies (while a call
 * runs, rl_gba_bios.open_bus still holds the value of the last BIOS exit).
 *
 * Interrupts: a write to IME or HALTCNT may return only after the CPU took
 * the interrupts the console would take at that point (HALTCNT: after the
 * halt ended). The interrupt runs in system mode with the I bit of the SWI's
 * caller, through rl_gba_bios_irq_entry, and the guest handler runs to
 * completion inside the write.
 */
typedef struct rl_gba_bus {
    /** @brief Passed to every callback. */
    void *user;
    /** @brief Reads size bytes at addr. */
    uint32_t (*read)(void *user, uint32_t addr, unsigned size);
    /** @brief Writes the low size bytes of value at addr. */
    void (*write)(void *user, uint32_t addr, unsigned size, uint32_t value);
    /**
     * @brief The cycles of one data access (1 for zero wait states); NULL
     * charges 1 per access.
     */
    unsigned (*cycles)(void *user, uint32_t addr, unsigned size, bool sequential);
    /**
     * @brief A timed data read: moves the machine's clock over the access
     * (and over a DMA transfer that is due first) and returns the value.
     * When load, store, idle and point are all set, the BIOS runs on the
     * machine's clock: a call's cycles, its data accesses and the cycles in
     * which it takes interrupts pass there, so timers, DMA and interrupts
     * land inside a call when the console's would. Otherwise read, write and cycles are used
     * and the call's cycles are reported in rl_gba_bios.cycles.
     */
    uint32_t (*load)(void *user, uint32_t addr, unsigned size, bool sequential);
    /** @brief A timed code fetch of size bytes at addr (the return's reload of the caller's code). */
    uint32_t (*fetch)(void *user, uint32_t addr, unsigned size, bool sequential);
    /** @brief The write twin of load (a HALTCNT write halts there). */
    void (*store)(void *user, uint32_t addr, unsigned size, uint32_t value, bool sequential);
    /** @brief cycles cycles without a data access: the BIOS's code fetches, which a DMA transfer stalls. */
    void (*idle)(void *user, unsigned cycles);
    /** @brief cycles internal cycles, which a DMA transfer may overlap; NULL uses idle. */
    void (*internal)(void *user, unsigned cycles);
    /**
     * @brief A cycle of a call in which a pending interrupt may be taken
     * (relift's slide at RL_GBA_BIOS_SLIDE, or the return to the caller):
     * when rl_gba_irq_sample says the interrupt is taken and spsr's I bit
     * is clear, the CPU layer takes it with LR_irq = lr and SPSR_irq =
     * spsr, runs the handler and the stub's return, and gives an
     * rl_gba_point. regs holds r0 to r14 as a handler finds them there:
     * the stub stacks r0 to r3 and r12, and the handler sees the others
     * (r13 and r14 are those of spsr's mode when it is user, system or
     * supervisor; in other modes the CPU keeps its own), then regs[15], the
     * supervisor stack pointer, which a SWI from the handler stacks below.
     */
    int (*point)(void *user, uint32_t lr, uint32_t spsr, const uint32_t *regs);
} rl_gba_bus;

/** @brief What rl_gba_bus.point did. */
typedef enum rl_gba_point {
    /** @brief No interrupt was taken. */
    RL_GBA_POINT_NONE,
    /** @brief An interrupt ran and returned to lr - 4 with CPSR = spsr. */
    RL_GBA_POINT_TAKEN,
    /**
     * @brief An interrupt ran and its handler changed the return (LR_irq or
     * SPSR_irq on its stack): the CPU holds the state the stub's return
     * restored and continues there, outside the BIOS call.
     */
    RL_GBA_POINT_LEFT,
} rl_gba_point;

/** @brief BIOS state kept between calls. */
typedef struct rl_gba_bios {
    /** @brief What a read of 0x00000000-0x00003FFF returns now (rotated per address). */
    uint32_t open_bus;
    /**
     * @brief Cycles of the last call or stub entry or return that the bus
     * did not charge: all of them on a bus without the timed callbacks, 0 on
     * a timed bus.
     */
    uint64_t cycles;
    /** @brief Every cycle of the last call or stub entry or return. */
    uint64_t total;
    /** @brief The part of cycles the bus charged for data accesses. */
    uint64_t bus_cycles;
    /** @brief What the last call counted for its cycle model. */
    uint32_t events[20];
} rl_gba_bios;

/** @brief The CPU state a SWI sees and changes. */
typedef struct rl_gba_swi_cpu {
    /**
     * @brief r0 to r12: r0 to r3 are arguments in and results out; the
     * others are only read (the BIOS's routines stack some of them).
     */
    uint32_t r[13];
    /** @brief The address after the SWI instruction (LR_svc). */
    uint32_t ret;
    /** @brief The caller's CPSR (SPSR_svc). */
    uint32_t cpsr;
    /** @brief The supervisor stack pointer. */
    uint32_t sp_svc;
    /** @brief The system (and user) mode stack pointer and link register. */
    uint32_t sp_sys, lr_sys;
} rl_gba_swi_cpu;

/** @brief How a SWI ended. */
typedef enum rl_gba_swi_status {
    /** @brief It returned to rl_gba_swi_cpu.ret with r0 to r3 updated. */
    RL_GBA_SWI_OK,
    /**
     * @brief The console's BIOS never returns from this call (Div by zero
     * with a numerator other than -1, 0 or 1): the CPU loops in the BIOS and
     * only interrupts still run.
     */
    RL_GBA_SWI_HANG,
    /**
     * @brief SoftReset or HardReset: the CPU must load rl_gba_bios_boot_state
     * and continue at rl_gba_swi_cpu.ret.
     */
    RL_GBA_SWI_RESET,
    /** @brief A number this BIOS does not implement (the sound driver calls). */
    RL_GBA_SWI_UNKNOWN,
    /**
     * @brief An interrupt handler that ran inside the call changed where its
     * return goes (RL_GBA_POINT_LEFT): the call ended there, r0 to r3 are
     * not results and the CPU continues in the state the IRQ return gave.
     */
    RL_GBA_SWI_LEFT,
} rl_gba_swi_status;

/** @brief The CPU state after boot or a reset. */
typedef struct rl_gba_boot {
    /** @brief Entry point. */
    uint32_t pc;
    /** @brief CPSR (system mode). */
    uint32_t cpsr;
    /** @brief Stack pointers of system, IRQ and supervisor mode. */
    uint32_t sp_sys, sp_irq, sp_svc;
} rl_gba_boot;

/** @brief Sets b to its state after power on. */
void rl_gba_bios_init(rl_gba_bios *b);

/** @brief The CPU state the cartridge starts in (entry 0x08000000). */
rl_gba_boot rl_gba_bios_boot_state(void);

/**
 * @brief Writes the I/O registers the BIOS leaves set when it hands over to
 * the cartridge (DISPCNT forced blank cleared, the affine identity, SOUNDBIAS,
 * RCNT, POSTFLG).
 */
void rl_gba_bios_boot_io(rl_gba_bios *b, const rl_gba_bus *bus);

/**
 * @brief Runs SWI number (the comment field: bits 16-23 in ARM state, bits
 * 0-7 in Thumb state) as the console's BIOS does; adds nothing to the CPU's
 * banked registers (LR_svc and SPSR_svc are the CPU's exception entry).
 */
rl_gba_swi_status rl_gba_swi(rl_gba_bios *b, const rl_gba_bus *bus, unsigned number, rl_gba_swi_cpu *cpu);

/** @brief The registers the IRQ stub stacks and changes. */
typedef struct rl_gba_irq_cpu {
    /** @brief r0 to r3, r12, LR_irq and SP_irq. */
    uint32_t r[4], r12, lr, sp;
    /**
     * @brief The interrupt came from running code, and the CPU layer charges
     * the prefetch of the interrupted instruction and the return's refill of
     * the caller's code as accesses of their own; the stub then counts the
     * cycles of the BIOS code alone.
     */
    bool from_code;
} rl_gba_irq_cpu;

/**
 * @brief The stub at vector 0x18, after the CPU's exception entry: pushes
 * r0-r3, r12 and LR on the IRQ stack, sets r0 = 0x04000000 and
 * LR = RL_GBA_BIOS_IRQ_RETURN, and returns the handler address read from
 * 0x03FFFFFC (ARM state; LDR PC does not change state).
 */
uint32_t rl_gba_bios_irq_entry(rl_gba_bios *b, const rl_gba_bus *bus, rl_gba_irq_cpu *cpu);

/**
 * @brief The stub's tail at RL_GBA_BIOS_IRQ_RETURN: pops r0-r3, r12 and LR
 * and returns LR - 4, where the CPU continues after restoring CPSR from
 * SPSR_irq (SUBS PC, LR, #4).
 */
uint32_t rl_gba_bios_irq_return(rl_gba_bios *b, const rl_gba_bus *bus, rl_gba_irq_cpu *cpu);

/** @brief What a read of size bytes at addr in the BIOS region returns now. */
uint32_t rl_gba_bios_read(const rl_gba_bios *b, uint32_t addr, unsigned size);

#endif
