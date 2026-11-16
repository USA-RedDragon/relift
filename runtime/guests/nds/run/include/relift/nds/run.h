/**
 * @file relift/nds/run.h
 * @brief The entries lifted code of a Nintendo DS cartridge calls
 * (guests/nds/guest, frontend.rs) besides relift/arm32/abi.h, on either CPU:
 * every data access goes to the machine's bus, which orders the shared ones
 * against the other CPU and charges their cost; a function's entry checks
 * the bytes it was lifted from; MCR and MRC reach the ARM9's CP15; SWI runs
 * the high-level BIOS.
 */
#ifndef RELIFT_NDS_RUN_H
#define RELIFT_NDS_RUN_H

#include <relift/arm32/abi.h>

/** @brief A data read of size bytes at addr through the machine's bus, at the CPU's tick. */
RL_RUNTIME_API uint64_t rl_nds_cpu_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size);
/** @brief A data write of size bytes of value at addr through the machine's bus, at the CPU's tick. */
RL_RUNTIME_API void rl_nds_cpu_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value);

/**
 * @brief The entry check of function fn of unit (the program's indices):
 * returns 0 when the CPU fetches there the bytes the function was lifted
 * from, 1 when it fetches others (and nothing was done).
 */
RL_RUNTIME_API int rl_nds_cpu_enter(rl_ctx *c, uint32_t unit, uint32_t fn);
/**
 * @brief After rl_nds_cpu_enter returned 1 at pc: the lifted function of
 * another unit whose entry is pc in that state and whose bytes the CPU
 * fetches there. With none it gives a function that stops the program with
 * a report of the CPU, the address and the bytes.
 */
RL_RUNTIME_API rl_fn rl_nds_cpu_variant(rl_ctx *c, uint32_t pc, unsigned thumb);

/**
 * @brief MCR to the ARM9's CP15 register at key (the lifter's key), when
 * cond: moves the TCMs, sets the vector base and waits for an interrupt as
 * the write asks.
 */
RL_RUNTIME_API void rl_nds_cpu_cp_write(rl_ctx *c, uint32_t key, uint32_t value, unsigned cond);
/** @brief MRC from the ARM9's CP15 register at key. */
RL_RUNTIME_API uint32_t rl_nds_cpu_cp_read(rl_ctx *c, uint32_t key);

/** @brief The block wrote i to the I bit: a clear with an interrupt pending ends the budget, so the sync point takes it. */
RL_RUNTIME_API void rl_nds_cpu_set_i(rl_ctx *c, uint32_t i);

/** @brief An instruction whose condition failed, after its fetch: a sync point with next the instruction after it. */
RL_RUNTIME_API int rl_nds_cpu_cond_fail(rl_ctx *c, uint8_t *mem, uint64_t next);

/** @brief Bit of a SWI trap code set for a SWI in Thumb state; bits 0 to 7 are the comment byte. */
#define RL_NDS_SWI_THUMB 0x100u

/**
 * @brief SWI, after the lifted exception entry (supervisor mode, LR_svc and
 * SPSR_svc set): runs the high-level BIOS call and returns from the
 * exception. Returns RL_OK to go on after the SWI, or RL_PENDING when the
 * program stops.
 */
RL_RUNTIME_API int rl_nds_swi_trap(rl_ctx *c, uint8_t *mem, uint32_t code);

#endif
