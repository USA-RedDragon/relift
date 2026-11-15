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
