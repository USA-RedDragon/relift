/**
 * @file relift/gba/run.h
 * @brief The entries lifted ARMv4T code of a Game Boy Advance ROM calls
 * (guests/gba/guest, frontend.rs) besides relift/arm32/abi.h: every data
 * access goes to the machine's bus, which charges its cycles, and SWI runs
 * the high-level BIOS.
 */
#ifndef RELIFT_GBA_RUN_H
#define RELIFT_GBA_RUN_H

#include <relift/arm32/abi.h>

/** @brief A data read of size bytes at addr through the machine's bus, at the CPU's cycle. */
RL_RUNTIME_API uint64_t rl_gba_cpu_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size);
/** @brief A data write of size bytes of value at addr through the machine's bus, at the CPU's cycle. */
RL_RUNTIME_API void rl_gba_cpu_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value);

/**
 * @brief The first cycle of the instruction at pc: the fetch two
 * instructions ahead (4 bytes ahead in Thumb state, 8 in ARM state) through
 * the machine's bus, nonsequential after a data access, an internal cycle or
 * an exception return. Ends the previous instruction's data accesses.
 */
RL_RUNTIME_API void rl_gba_cpu_fetch(rl_ctx *c, uint32_t pc, unsigned thumb);
/**
 * @brief The first cycle of an instruction in work RAM lifted from unit:
 * when the CPU fetched another unit at pc (the last two code fetches read
 * what the pipeline holds; with neither at pc, memory now), returns 1 and
 * does nothing; otherwise runs rl_gba_cpu_fetch and returns 0.
 */
RL_RUNTIME_API int rl_gba_cpu_fetch_code(rl_ctx *c, uint32_t pc, unsigned thumb, uint32_t unit);
/**
 * @brief After rl_gba_cpu_fetch_code returned 1 at pc: the lifted function
 * whose entry is pc in that state, lifted from the unit the CPU fetched
 * there. With none it gives a function that stops the guest with a report.
 */
RL_RUNTIME_API rl_fn rl_gba_cpu_variant(rl_ctx *c, uint32_t pc, unsigned thumb);
/** @brief n internal cycles of the CPU, which leave the bus to the prefetch buffer. */
RL_RUNTIME_API void rl_gba_cpu_idle(rl_ctx *c, uint32_t n);
/**
 * @brief MSR writes i to the I bit: a set masks interrupts at once; a clear
 * lets the next instruction run before a pending interrupt is taken (the
 * mask opens at that instruction's fetch), as on NanoBoyAdvance. An
 * exception return's write goes to rl_arm32_set_i.
 */
RL_RUNTIME_API void rl_gba_cpu_msr_i(rl_ctx *c, uint32_t i);
/** @brief A taken branch to target refills the pipeline: a nonsequential and a sequential fetch there. */
RL_RUNTIME_API void rl_gba_cpu_refill(rl_ctx *c, uint32_t target, unsigned thumb, unsigned taken);
/**
 * @brief An instruction whose condition failed, after its fetch: takes
 * interrupts as the sync point after it would, with next the instruction
 * after it. Returns RL_OK or RL_PENDING as rl_sync does.
 */
RL_RUNTIME_API int rl_gba_cpu_cond_fail(rl_ctx *c, uint8_t *mem, uint64_t next);

/** @brief Bit of a SWI trap code set for a SWI in Thumb state; bits 0 to 7 are the comment byte. */
#define RL_GBA_SWI_THUMB 0x100u

/**
 * @brief SWI, after the lifted exception entry (supervisor mode, LR_svc and
 * SPSR_svc set): runs the BIOS call and returns from the exception. Returns
 * RL_OK to go on after the SWI, or RL_PENDING (a reset redirects, a call the
 * console never returns from stops the guest).
 */
RL_RUNTIME_API int rl_gba_swi_trap(rl_ctx *c, uint8_t *mem, uint32_t code);

#endif
