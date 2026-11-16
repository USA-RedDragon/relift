/**
 * @file mips.h
 * @brief The VR4300 CPU layer's host-side interface: the descriptor the core
 * allocates contexts from, CP0 (exception entry and return, interrupts,
 * Count and Compare, Random), the TLB and address translation, the FPU slow
 * path, the bus of the slow paths and the thread entry. Lifted code uses
 * only relift/mips/abi.h.
 *
 * Not tied to a machine: a guest layer binds its scheduler CPU, routes its
 * interrupt lines to the IP bits (scheduler line n is IP n, for n from 2 to
 * 6), gives the slow paths its bus and installs the interrupt hooks.
 */
#ifndef RELIFT_MIPS_H
#define RELIFT_MIPS_H

#include <relift/mips/abi.h>

#include "core.h"

#include <stdbool.h>

/** @brief The VR4300 descriptor for rl_core_init (context layout, word code granule, big-endian data). */
RL_RUNTIME_API const rl_cpu_desc *rl_mips_cpu(void);

/** @brief The reset vector. */
#define RL_MIPS_RESET_VECTOR UINT64_C(0xFFFFFFFFBFC00000)

/** @brief Cold reset: Status BEV and ERL, Random 31, Wired 0, Config's reset value, the TLB cleared. Returns the reset vector. */
RL_RUNTIME_API uint64_t rl_mips_reset(rl_ctx *c);
/** @brief Soft reset or NMI taken at pc: ErrorEPC := pc, Status BEV, SR and ERL set, TS clear. Returns the reset vector. */
RL_RUNTIME_API uint64_t rl_mips_nmi(rl_ctx *c, uint64_t pc);

/**
 * @brief Exception entry with Cause.ExcCode code (and Cause.CE ce) for the
 * instruction at pc, in the delay slot of the branch at pc - 4 when
 * in_delay. With EXL clear: EPC := pc (or the branch), Cause.BD, EXL set.
 * With EXL set only ExcCode and CE change. Returns the general vector
 * (offset 0x180 from 0x80000000, or 0xBFC00200 with BEV).
 */
RL_RUNTIME_API uint64_t rl_mips_exception(rl_ctx *c, uint32_t code, uint64_t pc, bool in_delay, uint32_t ce);

/** @brief The outcome of an address translation. */
typedef enum rl_mips_xlat {
    /** @brief Translated. */
    RL_MIPS_XLAT_OK,
    /** @brief An address the current mode may not use: address error. */
    RL_MIPS_XLAT_ADDRESS,
    /** @brief No TLB entry matches: TLB refill. */
    RL_MIPS_XLAT_MISS,
    /** @brief The entry's page is not valid: TLB invalid. */
    RL_MIPS_XLAT_INVALID,
    /** @brief A store to a page that is not dirty: TLB modification. */
    RL_MIPS_XLAT_MODIFIED,
} rl_mips_xlat;

/**
 * @brief Translates vaddr for a load (write false) or store in the current
 * mode (KSU, EXL, ERL, and KX, SX or UX for 64-bit addressing): kseg0 and
 * kseg1 directly, kuseg, ksseg, kseg3 and the 64-bit mapped segments through
 * the TLB, kuseg directly while ERL is set, xkphys directly.
 */
RL_RUNTIME_API rl_mips_xlat rl_mips_translate(const rl_ctx *c, uint64_t vaddr, bool write, uint64_t *paddr);
/**
 * @brief The exception of a failed translation of vaddr by the instruction
 * at pc: BadVAddr, Context, XContext and EntryHi's VPN2 and R from vaddr,
 * then AdEL or AdES, TLBL or TLBS, or Mod. A miss with EXL clear goes to the
 * refill vector (offset 0, or 0x80 in 64-bit addressing). Returns the vector.
 */
RL_RUNTIME_API uint64_t rl_mips_xlat_exception(rl_ctx *c, rl_mips_xlat why, bool write, uint64_t vaddr,
                                               uint64_t pc, bool in_delay);

/** @brief Whether an interrupt is taken now: IE set, EXL and ERL clear, an IP bit set whose IM bit is set. */
RL_RUNTIME_API bool rl_mips_irq_pending(const rl_ctx *c);
/** @brief Sets Cause.IP2 to IP6 from bits 2 to 6 of ip (the machine's interrupt lines) and applies rl_mips_cp0_sync. */
RL_RUNTIME_API void rl_mips_set_ip(rl_ctx *c, uint32_t ip);
/**
 * @brief The interrupt entry at a sync point whose block goes on at next:
 * takes Cause.IP2 to IP6 from bits 2 to 6 of ip, checks the timer, and when
 * an interrupt is taken enters it (EPC := next) with a host frame that the
 * handler's ERET to next closes. Returns the vector, or RL_NO_IRQ. The guest
 * layer's rl_irq_hook calls it.
 */
RL_RUNTIME_API uint64_t rl_mips_irq_enter(rl_ctx *c, uint64_t next, uint32_t ip);
/** @brief An rl_irq_return_hook: where the handler's ERET went. */
RL_RUNTIME_API uint64_t rl_mips_irq_return(rl_ctx *c, uint8_t *mem, uint64_t expected);

/** @brief Sets Count to value at the current cycle. */
RL_RUNTIME_API void rl_mips_set_count(rl_ctx *c, uint32_t value);
/** @brief PClock cycles from now until Count next equals Compare (the guest layer posts its timer event there). */
RL_RUNTIME_API uint64_t rl_mips_timer_cycles(rl_ctx *c);

/** @brief A CACHE instruction that ran: its operation field and virtual address (code invalidation goes here). */
typedef void rl_mips_cache_hook(rl_ctx *c, uint32_t op, uint64_t vaddr);
/** @brief Installs the CACHE hook (NULL: CACHE does nothing). */
RL_RUNTIME_API void rl_mips_set_cache_hook(rl_mips_cache_hook *hook);

/** @brief The physical bus of the runtime's own accesses (TLB-mapped loads and stores in the guard slow paths). */
typedef struct rl_mips_bus {
    /** @brief Reads size bytes (1, 2, 4 or 8) at paddr, big-endian, as a number. */
    uint64_t (*read)(rl_ctx *c, uint8_t *mem, uint64_t paddr, uint32_t size);
    /** @brief Writes size bytes of value at paddr, big-endian. */
    void (*write)(rl_ctx *c, uint8_t *mem, uint64_t paddr, uint32_t size, uint64_t value);
} rl_mips_bus;
/** @brief Installs the bus (NULL: plain big-endian accesses of mem at the physical address). */
RL_RUNTIME_API void rl_mips_set_bus(const rl_mips_bus *bus);

/**
 * @brief Runs the COP1 arithmetic, conversion or compare instruction insn
 * on c with every VR4300 rule. Returns true when it raises the
 * floating-point exception (not yet entered: the destination is untouched
 * and FCR31 holds the cause).
 */
RL_RUNTIME_API bool rl_mips_fpu(rl_ctx *c, uint32_t insn);

/**
 * @brief The guest thread entry: runs the function at pc, following
 * redirects (exception vectors, ERET out of an interrupt); every restart
 * begins with no open host frame. Returns RL_OK or RL_PENDING.
 */
RL_RUNTIME_API int rl_mips_run(rl_ctx *c, uint8_t *mem, uint64_t pc);
/** @brief Ends the run with a redirect to pc: the run loop goes on there with no open host frame. Returns RL_PENDING. */
RL_RUNTIME_API int rl_mips_redirect(rl_ctx *c, uint64_t pc);

/** @brief Counters for tests and --stats. */
typedef struct rl_mips_stats {
    /** @brief Returns through the frame stack. */
    uint64_t returns;
    /** @brief Calls past RL_MIPS_SHADOW open frames. */
    uint64_t overflows;
    /** @brief Restarts of the run loop after a redirect. */
    uint64_t redirects;
    /** @brief Dispatch misses. */
    uint64_t misses;
    /** @brief Exception entries (interrupts included). */
    uint64_t exceptions;
    /** @brief Guard slow paths taken. */
    uint64_t slow;
} rl_mips_stats;

/** @brief The counters. */
RL_RUNTIME_API rl_mips_stats *rl_mips_get_stats(void);

/** @brief The mnemonic of an ExcCode ("Int", "TLBL", ...), or "???". */
RL_RUNTIME_API const char *rl_mips_exc_name(uint32_t code);

#endif
