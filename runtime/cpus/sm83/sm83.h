/**
 * @file sm83.h
 * @brief The SM83 CPU layer's host-side interface: the descriptor the core
 * allocates contexts from, the thread entry and the binding of a thread to a
 * scheduler CPU with the CPU's interrupt hooks. Lifted code uses only
 * relift/sm83/abi.h.
 */
#ifndef RELIFT_SM83_H
#define RELIFT_SM83_H

#include <relift/sched.h>
#include <relift/sm83/abi.h>

#include "core.h"

/** @brief The SM83 descriptor for rl_core_init. */
RL_RUNTIME_API const rl_cpu_desc *rl_sm83_cpu(void);
/** @brief The registers for the GDB server (core/gdbstub.h), where it is built in (RL_DEBUG_STUB). */
RL_RUNTIME_API const struct rl_gdb_cpu *rl_sm83_gdb(void);

/**
 * @brief Binds thread t to scheduler CPU k (rl_sched_attach) and installs
 * the SM83 interrupt entry and return hooks on k, reading the machine's
 * request and enable state through ints. Returns 0, or -1 when t has no
 * context or k is already bound.
 *
 * The entry, when IME is 1 and a line is pending: acknowledges the lowest
 * line, clears IME, pushes the interrupted pc as sp -= 2 (high byte at the
 * higher address) into guest memory, charges RL_SM83_IRQ_CYCLES and returns
 * the vector. The stack write goes to mem directly, so a stack in ROM or
 * device space is not modeled. After the handler returns the return hook
 * reads the address the handler's RET popped (the two bytes below sp); when
 * it is not the interrupted pc, rl_sync redirects there (RL_PEND_REDIRECT).
 */
RL_RUNTIME_API int rl_sm83_attach(rl_sched_cpu *k, struct rl_thread *t, const rl_sm83_ints *ints);

/**
 * @brief The guest thread entry: sets c->pc and runs the function at pc,
 * following redirects (rl_run_from). Returns RL_OK or RL_PENDING.
 */
RL_RUNTIME_API int rl_sm83_run(rl_ctx *c, uint8_t *mem, uint64_t pc);

/** @brief Flags of f as "ZNHC" with '-' for a clear bit, into buf (at least 5 bytes). */
RL_RUNTIME_API const char *rl_sm83_flags_str(uint8_t f, char *buf);
/**
 * @brief Sets the function rl_dispatch_miss runs instead of the core's miss
 * path (a guest layer's interpreter), with the lifted signature and the
 * target in c->pc; NULL restores the core's path.
 */
RL_RUNTIME_API void rl_sm83_set_miss(rl_fn fn);
/**
 * @brief What the calling thread's last dispatch miss handed to the
 * rl_sm83_set_miss path knew: the target, the site of a jump (RL_JUMP) or the
 * call site and caller of a call. Consumes it. False when the last miss was
 * not at c->pc.
 */
RL_RUNTIME_API bool rl_sm83_last_miss(rl_ctx *c, rl_miss_info *m);

#endif
