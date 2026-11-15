/**
 * @file relift/gb/exact.h
 * @brief The entries lifted SM83 code of the exact Game Boy mode calls
 * (guests/gb/guest, frontend.rs): the device library owns the bus and the
 * instruction boundary, so every access, every boundary and the
 * instructions the library's CPU runs go through the Game Boy layer.
 */
#ifndef RELIFT_GB_EXACT_H
#define RELIFT_GB_EXACT_H

#include <relift/sm83/abi.h>

/** @brief Sync kind (info bits 0 to 3): the block falls through or jumps. */
#define RL_GB_SYNC_JUMP 0u
/** @brief Sync kind: the block calls; info bits 16 to 31 are the return address. */
#define RL_GB_SYNC_CALL 1u
/** @brief Sync kind: the block returns; next is the popped address. */
#define RL_GB_SYNC_RETURN 2u
/** @brief Set when the exit may reach lifted window code of the block's bank directly: the bank must still be mapped. */
#define RL_GB_SYNC_CHECK 8u
/** @brief The sync kind of an info word. */
#define RL_GB_SYNC_KIND(info) ((info) & 3u)
/** @brief The bank plus one of the block's code (0: fixed code); also the bank its call returns into. */
#define RL_GB_SYNC_BANK(info) (((info) >> 4) & 0xFFFu)
/** @brief The return address of a call. */
#define RL_GB_SYNC_RET(info) ((uint16_t)((info) >> 16))

/**
 * @brief The sync point after every instruction: ends the instruction that
 * ran, checks the host frames against the guest (a return to where its call
 * returns, the bank of a direct transfer into the window) and opens the
 * instruction at next, entering pending interrupts first. A next with bits
 * above 15 names a continuation of the same instruction and does nothing.
 * Returns RL_OK, or RL_PENDING with a redirect when the guest went where the
 * host frames cannot follow.
 */
RL_RUNTIME_API int rl_gb_sync(rl_ctx *c, uint8_t *mem, uint64_t next, uint32_t info);
/** @brief A bus read of the open instruction at the cycle its clock says. */
RL_RUNTIME_API uint64_t rl_gb_io_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size);
/** @brief A bus write of the open instruction at the cycle its clock says. */
RL_RUNTIME_API void rl_gb_io_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value);
/**
 * @brief Runs the open instruction (opcode in bits 16 to 23 of code) on the
 * library's CPU and opens the next one; bits 0 to 15 are where the lifted
 * code continues. Returns as rl_gb_sync.
 */
RL_RUNTIME_API int rl_gb_trap(rl_ctx *c, uint8_t *mem, uint32_t code);
/** @brief RETI's interrupt enable, set in the library's CPU. */
RL_RUNTIME_API void rl_gb_reti(rl_ctx *c);

#endif
