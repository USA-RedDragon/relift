/**
 * @file arm32.h
 * @brief The ARMv4T CPU layer's host-side interface: the descriptor the core
 * allocates contexts from, the CPSR and mode banks, exception entry and
 * return, the interrupt lines the I bit gates, and the thread entry. Lifted
 * code uses only relift/arm32/abi.h.
 *
 * Not tied to a machine: a guest layer binds its scheduler CPU, decides
 * which of its lines the I bit masks (rl_arm32_set_lines) and implements its
 * exception vectors with these helpers.
 */
#ifndef RELIFT_ARM32_H
#define RELIFT_ARM32_H

#include <relift/arm32/abi.h>

#include "core.h"

#include <stdbool.h>

/** @brief The ARMv4T descriptor for rl_core_init (context layout, halfword code granule, little-endian data). */
RL_RUNTIME_API const rl_cpu_desc *rl_arm32_cpu(void);

/** @brief The registers for the GDB server (core/gdbstub.h), where it is built in (RL_DEBUG_STUB). */
RL_RUNTIME_API const struct rl_gdb_cpu *rl_arm32_gdb(void);

/** @brief The CPSR of c. */
RL_RUNTIME_API uint32_t rl_arm32_cpsr(const rl_ctx *c);

/** @brief The guest layer's answer to a code fetch whose waits entry is 0 (rl_arm32_fetch_slow). */
typedef int64_t rl_arm32_fetch_fn(rl_ctx *c, uint32_t addr, unsigned log2, unsigned seq);
/** @brief Installs the fetch hook; NULL makes such fetches cost one cycle. */
RL_RUNTIME_API void rl_arm32_set_fetch_hook(rl_arm32_fetch_fn *fn);
/**
 * @brief Writes the whole CPSR: flags, masks, Thumb bit, reserved bits and
 * mode, swapping r8 to r14 with the banks when the mode changes. Mode bits
 * that name no mode are kept as written and bank as user mode (as the
 * lifted code does). Updates the interrupt mask.
 */
RL_RUNTIME_API void rl_arm32_set_cpsr(rl_ctx *c, uint32_t cpsr);
/** @brief Changes the mode bits to m (low five bits), swapping the banks; nothing else changes. */
RL_RUNTIME_API void rl_arm32_set_mode(rl_ctx *c, uint32_t m);
/** @brief The slot of the SPSR of mode m, or NULL for user and system mode. */
RL_RUNTIME_API uint32_t *rl_arm32_spsr(rl_ctx *c, uint32_t m);
/** @brief r[n] (8 to 14) of mode m, wherever it is kept now. */
RL_RUNTIME_API uint32_t *rl_arm32_banked(rl_ctx *c, uint32_t m, unsigned n);

/**
 * @brief Exception entry into mode m (IRQ, supervisor, abort, undefined or
 * FIQ): SPSR_m := CPSR, mode m in ARM state with IRQs masked (and FIQs for
 * FIQ), LR_m := lr. The caller jumps to the vector.
 */
RL_RUNTIME_API void rl_arm32_exception(rl_ctx *c, uint32_t m, uint32_t lr);
/** @brief Exception return: CPSR := SPSR of the current mode (nothing in user and system mode). */
RL_RUNTIME_API void rl_arm32_restore(rl_ctx *c);

/**
 * @brief The scheduler lines of the guest layer: irq are masked while the I
 * bit is set, other (for example a DMA request) never are. Applies them to
 * the CPU's mask now.
 */
RL_RUNTIME_API void rl_arm32_set_lines(rl_ctx *c, uint64_t irq, uint64_t other);

/**
 * @brief The guest thread entry: runs the function at pc, following
 * redirects; every restart begins with no open host frame. Returns RL_OK or
 * RL_PENDING.
 */
RL_RUNTIME_API int rl_arm32_run(rl_ctx *c, uint8_t *mem, uint64_t pc);

/**
 * @brief Calls the lifted code at pc as a nested call that returns when the
 * code jumps to ret (an interrupt handler returning to its stub, say).
 * Returns RL_OK, or RL_PENDING with the pending record set.
 */
RL_RUNTIME_API int rl_arm32_call_guest(rl_ctx *c, uint8_t *mem, uint32_t pc, uint32_t ret);

/** @brief Sets what a dispatch miss runs instead of the core's report (the target is in c->pc), or NULL for the core's. */
RL_RUNTIME_API void rl_arm32_set_miss(rl_fn miss);

/** @brief Ends the run with a redirect to pc: the run loop goes on there with no open host frame. Returns RL_PENDING. */
RL_RUNTIME_API int rl_arm32_redirect(rl_ctx *c, uint32_t pc);

/** @brief Counters for tests and --stats. */
typedef struct rl_arm32_stats {
    /** @brief Returns through the frame stack. */
    uint64_t returns;
    /** @brief Calls past RL_ARM32_SHADOW open frames. */
    uint64_t overflows;
    /** @brief Restarts of the run loop after a redirect. */
    uint64_t redirects;
    /** @brief Dispatch misses. */
    uint64_t misses;
} rl_arm32_stats;

/** @brief The counters. */
RL_RUNTIME_API rl_arm32_stats *rl_arm32_get_stats(void);

/** @brief Mode name ("usr", "irq", ...) or "???". */
RL_RUNTIME_API const char *rl_arm32_mode_name(uint32_t m);

#endif
