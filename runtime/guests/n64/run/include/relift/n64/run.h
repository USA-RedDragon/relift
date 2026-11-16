/**
 * @file relift/n64/run.h
 * @brief The entries lifted VR4300 code of a Nintendo 64 ROM calls
 * (guests/n64/guest, frontend.rs) besides relift/mips/abi.h: device
 * accesses go to the machine, a CP0 write the runtime must see ends the
 * budget and its sync point applies it, and the TLB instructions end their
 * block in a trap.
 */
#ifndef RELIFT_N64_RUN_H
#define RELIFT_N64_RUN_H

#include <relift/mips/abi.h>

/** @brief A read of size bytes at physical address addr outside RDRAM, at the CPU's cycle. */
RL_RUNTIME_API uint64_t rl_n64_cpu_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size);
/** @brief A write of size bytes of value at physical address addr outside RDRAM, at the CPU's cycle. */
RL_RUNTIME_API void rl_n64_cpu_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value);

/** @brief Ends the budget, so the block's sync point calls rl_n64_cp0_sync. */
RL_RUNTIME_API void rl_n64_cp0_break(rl_ctx *c);
/**
 * @brief The sync point after a write of Status, Cause, Compare, Count or
 * Wired, with the block's register writes done: applies the write (the
 * interrupt mask, the timer event), then runs rl_sync with next, which takes
 * an interrupt that is due. Returns RL_OK or RL_PENDING.
 */
RL_RUNTIME_API int rl_n64_cp0_sync(rl_ctx *c, uint8_t *mem, uint64_t next);
/** @brief TLBR (0), TLBWI (1), TLBWR (2) or TLBP (3), after the block's register writes. Returns RL_OK. */
RL_RUNTIME_API int rl_n64_tlb(rl_ctx *c, uint8_t *mem, uint32_t op);

#endif
