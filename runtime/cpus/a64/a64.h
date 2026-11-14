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
