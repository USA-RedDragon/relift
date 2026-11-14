/**
 * @file a64.h
 * @brief The ARM64 (A64) CPU layer of the runtime: its descriptor for the
 * core, FPCR handling, and the host-side entry into lifted A64 code.
 *
 * Not tied to a guest OS: a guest layer with A64 code picks this descriptor
 * and passes it to rl_core_init with its own guest descriptor.
 */
#ifndef RELIFT_A64_H
#define RELIFT_A64_H

#include "core.h"

#include <relift/a64/abi.h>

/** @brief Default FPCR of new contexts. A guest layer whose OS starts threads with another value sets c->fpcr. */
#define RL_FPCR_DEFAULT 0u

/** @brief The A64 CPU descriptor (context layout, 4-byte code granule, little-endian data). */
RL_RUNTIME_API const rl_cpu_desc *rl_a64_cpu(void);

/**
 * @brief Whether new contexts run in the reference exact-flags mode
 * (RL_FPCR_EXACT, every guarded block on the slow path): RELIFT_FP_EXACT=1,
 * for checking the host-flag path against it.
 */
RL_RUNTIME_API int rl_a64_exact_flags(void);
/** @brief Whether FPSR flags are produced on host hardware (the build's FPSR census found a usable read). */
RL_RUNTIME_API int rl_a64_fp_flags(void);
/** @brief Turns FPSR flag production on; program.c calls it before the program starts. */
RL_RUNTIME_API void rl_a64_set_fp_flags(int on);
/** @brief Saves and clears the host FP flags at a runtime entry that may do host FP work, in flag mode. */
static inline uint32_t rl_a64_flags_enter(void)
{
    if (!rl_a64_fp_flags())
        return 0;
    uint32_t f = rl_fp_host_flags();
    rl_fp_host_clear();
    return f;
}
/** @brief Restores the flags rl_a64_flags_enter saved when leaving the runtime entry. */
static inline void rl_a64_flags_leave(uint32_t f)
{
    if (rl_a64_fp_flags()) {
        rl_fp_host_clear();
        rl_fp_host_raise(f);
    }
}

/** @brief Sets the host FPCR to fpcr on arm64 hosts; no effect elsewhere. */
RL_RUNTIME_API void rl_host_set_fpcr(uint32_t fpcr);
/** @brief Reads the host FPCR on arm64 hosts; 0 elsewhere. */
RL_RUNTIME_API uint32_t rl_host_get_fpcr(void);

/**
 * @brief Calls guest code at entry from the host, as the runtime does for a
 * thread entry: sets c->pc and calls through the dispatch table. Returns the
 * lifted return code.
 */
RL_RUNTIME_API int rl_call_guest(rl_ctx *c, uint8_t *mem, uint64_t entry);

/**
 * @brief Names the kind of an instruction word that reached rl_undefined: "UDF
 * #imm", "BRK #imm", "HLT #imm" or "unliftable". Writes into buf and returns it.
 */
RL_RUNTIME_API const char *rl_a64_insn_kind(uint32_t insn, char *buf, size_t n);

#endif
