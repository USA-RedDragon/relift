/**
 * @file bios.h
 * @brief The high-level BIOS of the Nintendo DS: the software interrupts of
 * both CPUs (GBATEK "BIOS Function Summary", NDS7 and NDS9 columns), the
 * invalid numbers, and what reads of the two BIOS regions return.
 *
 * No BIOS image is used or needed. Every call reproduces the results and the
 * memory writes GBATEK documents, the documented bugs of the console's
 * routines (CpuFastSet's slow tail, IntrWait on the ARM9, Halt's r0) and the
 * documented IntrWait flag protocol (ARM9: DTCM + 0x3FF8, ARM7: 0x0380FFF8).
 * Registers that GBATEK does not give a value for are left as the caller had
 * them. All memory goes through the machine (rl_nds_load, rl_nds_store), so a
 * call costs the bus accesses it makes plus a fixed cost per call; the
 * console's own SWI timings are not measured yet.
 *
 * The caller is the CPU layer at a SWI exit, after the exception entry. The
 * machine layer has no registers or guest code of its own: the CPU layer
 * supplies the two things a call may need from it (rl_nds_bios_hooks).
 */
#ifndef RELIFT_NDS_BIOS_H
#define RELIFT_NDS_BIOS_H

#include "nds.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief Cycles of a call besides its bus accesses and loops (a fixed cost until the console's is measured). */
#define RL_NDS_BIOS_CALL_CYCLES 16u
/** @brief What an IRQ taken inside the ARM9's IntrWait costs besides the entry and return (hwtest 3's wake cases). */
#define RL_NDS_BIOS_WAKE_IRQ9 40u

/** @brief What a call needs from the CPU layer. */
typedef struct rl_nds_bios_hooks {
    /** @brief Passed to both callbacks. */
    void *user;
    /**
     * @brief Runs the guest function at fn (bit 0 set: Thumb) with r0 to r3
     * = args, to its return, and returns the r0 it left: the callbacks of
     * the decompression calls.
     */
    uint32_t (*call)(void *user, uint32_t fn, const uint32_t args[4]);
    /**
     * @brief IntrWait's interrupt window: takes a pending interrupt now, runs
     * its handler to completion and returns true, when the caller's I bit
     * lets it in and the CPU's IRQ input is asserted; false otherwise.
     */
    bool (*irq)(void *user);
} rl_nds_bios_hooks;

/** @brief How a call ended. */
typedef enum rl_nds_swi_status {
    /** @brief It returned; r[] holds the registers the caller sees. */
    RL_NDS_SWI_OK,
    /** @brief The CPU halted for good (nothing can wake it): the call does not return. */
    RL_NDS_SWI_STOP,
    /** @brief The console's routine never returns (Div by zero, ARM9 Halt with IME 0). */
    RL_NDS_SWI_HANG,
    /** @brief SoftReset: the CPU loads rl_nds_bios_reset and continues at its pc. */
    RL_NDS_SWI_RESET,
    /** @brief A number no routine answers: the console jumps to address 0. */
    RL_NDS_SWI_INVALID,
    /** @brief It returned, but GBATEK gives no values for what it returns (GetBootProcs): the registers read 0. */
    RL_NDS_SWI_UNKNOWN_RESULT,
} rl_nds_swi_status;

/** @brief The CP15 control register value SoftReset sets on the ARM9. */
#define RL_NDS_BIOS_RESET_CONTROL 0x12078u

/** @brief The CPU state SoftReset leaves (system mode, r0 to r12, LR_svc, SPSR_svc, LR_irq and SPSR_irq zero, LR = pc). */
typedef struct rl_nds_bios_reset {
    /** @brief Where the CPU continues (bit 0 set: Thumb), also its LR. */
    uint32_t pc;
    /** @brief The ARM9's reset also flushes the caches and sets the CP15 control register to RL_NDS_BIOS_RESET_CONTROL. */
    bool cp15;
    /** @brief Stack pointers of system, IRQ and supervisor mode. */
    uint32_t sp_sys, sp_irq, sp_svc;
} rl_nds_bios_reset;

/**
 * @brief Runs SWI number (the comment field: bits 16 to 23 in ARM state,
 * bits 0 to 7 in Thumb state) of cpu. r[0] to r[3] are the arguments in and
 * the results out. SoftReset has done its memory work (zero fill, CP15
 * control) when it returns RL_NDS_SWI_RESET with reset filled in.
 */
rl_nds_swi_status rl_nds_bios_swi(rl_nds *m, unsigned cpu, unsigned number, uint32_t r[4], const rl_nds_bios_hooks *hooks,
                                  rl_nds_bios_reset *reset);

/**
 * @brief What a read of size bytes at addr of cpu's BIOS region returns after
 * boot. The ARM7's BIOS (0 to 0x3FFF) is read-protected against code outside
 * it once the boot code has set BIOSPROT (GBATEK "DS Memory Control - BIOS"):
 * every byte reads 0xFF, and relift's code is never the BIOS's. The ARM9's
 * (0xFFFF0000 to 0xFFFF7FFF) has no protection, but relift has no image to
 * show: every byte reads 0.
 */
uint32_t rl_nds_bios_read(unsigned cpu, uint32_t addr, unsigned size);

/** @brief The sine table (GetSineTable), index 0 to 63. */
uint32_t rl_nds_bios_sine(unsigned index);
/** @brief The pitch table (GetPitchTable), index 0 to 0x2FF. */
uint32_t rl_nds_bios_pitch(unsigned index);
/** @brief The volume table (GetVolumeTable), index 0 to 0x2D3. */
uint32_t rl_nds_bios_volume(unsigned index);

#endif
