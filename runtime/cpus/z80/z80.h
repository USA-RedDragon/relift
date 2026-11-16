/**
 * @file z80.h
 * @brief The Z80 CPU layer's host-side interface: the descriptor the core
 * allocates contexts from, the thread entry and the binding of a thread to a
 * scheduler CPU with the CPU's interrupt hooks. Lifted code uses only
 * relift/z80/abi.h.
 */
#ifndef RELIFT_Z80_H
#define RELIFT_Z80_H

#include <relift/sched.h>
#include <relift/z80/abi.h>

#include "core.h"

/** @brief The Z80 descriptor for rl_core_init. */
RL_RUNTIME_API const rl_cpu_desc *rl_z80_cpu(void);

/**
 * @brief Binds thread t to scheduler CPU k (rl_sched_attach) and installs
 * the Z80 interrupt entry and return hooks on k, reading the machine's
 * inputs and ports through ints. The machine raises a line
 * (RL_Z80_LINE_INT, RL_Z80_LINE_NMI) with rl_sched_raise when its input is
 * asserted and lowers it when it is released or taken. The scheduler mask
 * of k is the NMI line, and the INT line while IFF1 is set (rl_z80_iff keeps
 * it current; a guest layer that sets iff1 in the context itself calls
 * rl_z80_iff too), and the entry hook decides. Returns 0, or -1 when t has
 * no context or k is already bound.
 *
 * The entry, at a sync point whose exit goes to next: an NMI is taken first
 * and never masked; INT is taken while IFF1 is set and the previous
 * instruction was not EI. The hook stores next in the context's ret and
 * returns the address of the interrupt entry block (RL_Z80_ENTRY plus the
 * kind: NMI, or the one of the interrupt mode); that block, an ordinary
 * lifted unit, pushes ret, clears the flip-flops, charges the entry's
 * T-states and goes to the vector, so a lifted program must contain the
 * entry blocks it can take. After the handler returns the return hook reads
 * the address its return instruction popped (the two bytes below sp); when
 * it is not next, rl_sync redirects there (RL_PEND_REDIRECT).
 */
RL_RUNTIME_API int rl_z80_attach(rl_sched_cpu *k, struct rl_thread *t, const rl_z80_ints *ints);

/**
 * @brief The guest thread entry: sets c->pc and runs the function at pc,
 * following redirects (rl_run_from). Returns RL_OK or RL_PENDING.
 */
RL_RUNTIME_API int rl_z80_run(rl_ctx *c, uint8_t *mem, uint64_t pc);

/** @brief Flags of f as "SZYHXPNC" with '-' for a clear bit, into buf (at least 9 bytes). */
RL_RUNTIME_API const char *rl_z80_flags_str(uint8_t f, char *buf);

/**
 * @brief Sets the function rl_dispatch_miss runs instead of the core's miss
 * path (a guest layer's variant dispatcher), with the lifted signature and
 * the target in c->pc; NULL restores the core's path.
 */
RL_RUNTIME_API void rl_z80_set_miss(rl_fn fn);

#endif
