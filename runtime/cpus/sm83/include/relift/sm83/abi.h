/**
 * @file relift/sm83/abi.h
 * @brief Runtime ABI contract between relift-emitted C for SM83 guest code
 * (the Game Boy CPU) and the relift C runtime: the register context, the
 * guest cycle clock, dispatch, interrupt state and the entries lifted code
 * calls.
 *
 * Every lifted translation unit for an SM83 guest includes this header and
 * nothing else from the runtime; it includes relift/abi_core.h, the
 * CPU-neutral half. The clock and the interrupt model are docs/abi.md
 * section 13. Any change to a type, layout, signature or constant here bumps
 * RL_ABI_VERSION.
 */
#ifndef RELIFT_SM83_ABI_H
#define RELIFT_SM83_ABI_H

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>

/** @brief Interrupt lines of the SM83: line n is bit n of the request and enable bytes, vector 0x40 + 8 * n. */
#define RL_SM83_IRQ_LINES 5u
/** @brief Mask of the interrupt line bits. */
#define RL_SM83_IRQ_MASK 0x1Fu
/** @brief Guest cycles (T-cycles) an interrupt entry costs: five machine cycles. */
#define RL_SM83_IRQ_CYCLES 20
/** @brief Guest address of the vector of interrupt line n. */
#define RL_SM83_IRQ_VECTOR(n) (0x40u + 8u * (n))

/**
 * @brief The machine's interrupt request and enable registers, as the SM83
 * layer sees them. The guest layer fills one in and gives it to
 * rl_sm83_attach; it must outlive the thread.
 */
typedef struct rl_sm83_ints {
    /** @brief Passed to both callbacks. */
    void *opaque;
    /** @brief The lines that are both requested and enabled, as bits 0 to 4 (IF & IE); the CPU takes the lowest. */
    uint8_t (*pending)(void *opaque);
    /** @brief The CPU takes line: the machine clears the request. */
    void (*ack)(void *opaque, uint32_t line);
} rl_sm83_ints;

/**
 * @brief Guest thread context. One per guest thread, allocated by the runtime.
 *
 * Holds the architectural state at every call boundary (docs/abi.md,
 * "Locals and the context"). f is the flag byte as the CPU has it: bit 7 Z,
 * bit 6 N, bit 5 H, bit 4 C, low nibble zero. The lifted code's one-bit flag
 * slots are bits of f.
 */
struct rl_ctx {
    /** @brief Accumulator. */
    uint8_t a;
    /** @brief B. */
    uint8_t b;
    /** @brief C. */
    uint8_t c;
    /** @brief D. */
    uint8_t d;
    /** @brief E. */
    uint8_t e;
    /** @brief H. */
    uint8_t h;
    /** @brief L. */
    uint8_t l;
    /** @brief Flags (Z N H C in bits 7 to 4). */
    uint8_t f;
    /** @brief Stack pointer. */
    uint16_t sp;
    /** @brief Target guest address of the current indirect call or jump; read by rl_dispatch_miss. */
    uint16_t pc;
    /** @brief Interrupt master enable (IME): 1 or 0. */
    uint8_t ime;
    /** @brief Set by EI; the interrupt entry withholds one entry while it is set, then clears it (or rl_sm83_ei_commit clears it). */
    uint8_t ei_delay;
    /** @brief 1 while the CPU waits in HALT. */
    uint8_t halted;
    /** @brief Reserved, zero. */
    uint8_t reserved0;
    /** @brief Pending record, valid while lifted frames return RL_PENDING. First field of the core's block (rl_ctx_common). */
    rl_pending pend;
    /** @brief Runtime thread object. */
    struct rl_thread *thread;
    /** @brief Dispatch table: one entry per guest address from disp_lo; NULL means miss. */
    rl_fn *disp_table;
    /** @brief Lowest guest code address covered by disp_table. */
    uint64_t disp_lo;
    /** @brief Number of entries in disp_table. */
    uint64_t disp_count;
    /** @brief Guest cycle clock (docs/abi.md section 13). */
    rl_time time;
    /** @brief The machine's interrupt registers (rl_sm83_attach), or NULL. */
    const rl_sm83_ints *ints;
    /** @brief The lines the machine enables (IE), bits 0 to 4; the scheduler mask is these while ime is 1. */
    uint8_t irq_lines;
    /** @brief Reserved, zero. */
    uint8_t reserved1[7];
};

_Static_assert(offsetof(rl_ctx, a) == 0, "a");
_Static_assert(offsetof(rl_ctx, b) == 1, "b");
_Static_assert(offsetof(rl_ctx, c) == 2, "c");
_Static_assert(offsetof(rl_ctx, d) == 3, "d");
_Static_assert(offsetof(rl_ctx, e) == 4, "e");
_Static_assert(offsetof(rl_ctx, h) == 5, "h");
_Static_assert(offsetof(rl_ctx, l) == 6, "l");
_Static_assert(offsetof(rl_ctx, f) == 7, "f");
_Static_assert(offsetof(rl_ctx, sp) == 8, "sp");
_Static_assert(offsetof(rl_ctx, pc) == 10, "pc");
_Static_assert(offsetof(rl_ctx, ime) == 12, "ime");
_Static_assert(offsetof(rl_ctx, ei_delay) == 13, "ei_delay");
_Static_assert(offsetof(rl_ctx, halted) == 14, "halted");
_Static_assert(offsetof(rl_ctx, pend) == 16, "pend");
_Static_assert(offsetof(rl_ctx, thread) == 40, "thread");
_Static_assert(offsetof(rl_ctx, disp_table) == 48, "disp_table");
_Static_assert(offsetof(rl_ctx, disp_lo) == 56, "disp_lo");
_Static_assert(offsetof(rl_ctx, disp_count) == 64, "disp_count");
_Static_assert(offsetof(rl_ctx, time) == 72, "time");
_Static_assert(offsetof(rl_ctx, ints) == 88, "ints");
_Static_assert(offsetof(rl_ctx, irq_lines) == 96, "irq_lines");
_Static_assert(sizeof(rl_ctx) == 104, "rl_ctx size");
_Static_assert(_Alignof(rl_ctx) == 8, "rl_ctx alignment");

/** @brief Offset of the core's block (rl_ctx_common) in rl_ctx: pend to disp_count. */
#define RL_SM83_COMMON_OFFSET 16u
/** @brief Offset of the rl_time clock in rl_ctx. */
#define RL_SM83_TIME_OFFSET 72u
/** @brief log2 of the dispatch granule: SM83 instructions are byte aligned. */
#define RL_SM83_CODE_SHIFT 0u

_Static_assert(offsetof(rl_ctx, pend) == RL_SM83_COMMON_OFFSET + offsetof(rl_ctx_common, pend), "common pend");
_Static_assert(offsetof(rl_ctx, thread) == RL_SM83_COMMON_OFFSET + offsetof(rl_ctx_common, thread), "common thread");
_Static_assert(offsetof(rl_ctx, disp_table) == RL_SM83_COMMON_OFFSET + offsetof(rl_ctx_common, disp_table), "common disp_table");
_Static_assert(offsetof(rl_ctx, disp_lo) == RL_SM83_COMMON_OFFSET + offsetof(rl_ctx_common, disp_lo), "common disp_lo");
_Static_assert(offsetof(rl_ctx, disp_count) == RL_SM83_COMMON_OFFSET + offsetof(rl_ctx_common, disp_count), "common disp_count");
_Static_assert(sizeof(rl_ctx_common) == offsetof(rl_ctx, time) - RL_SM83_COMMON_OFFSET, "common block size");
_Static_assert(offsetof(rl_ctx, time) == RL_SM83_TIME_OFFSET, "time offset");

/** @brief Dispatch miss handler. Reads the target from c->pc. Has the lifted signature so a miss is a call in place. */
RL_RUNTIME_API int rl_dispatch_miss(rl_ctx *c, uint8_t *mem);

/** @brief Looks up the host function for guest address target; returns rl_dispatch_miss on a miss. */
static inline rl_fn rl_lookup(const rl_ctx *c, uint64_t target)
{
    uint64_t i = target - c->disp_lo;
    rl_fn f = 0;
    if (i < c->disp_count) {
#if defined(RL_HOTLOAD)
        f = __atomic_load_n(&c->disp_table[i], __ATOMIC_ACQUIRE);
#else
        f = c->disp_table[i];
#endif
    }
    return f ? f : rl_dispatch_miss;
}

/** @brief Indirect call or jump: records the target for the miss handler and returns the function to call. */
static inline rl_fn rl_indirect(rl_ctx *c, uint64_t target)
{
    c->pc = (uint16_t)target;
    return rl_lookup(c, target);
}

/**
 * @brief The guest executes an instruction the SM83 does not define, or one
 * the lifter cannot lift: reports it with the registers and stops the guest.
 * insn is the instruction bytes, first byte most significant. Returns RL_PENDING.
 */
RL_RUNTIME_API int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn);
/** @brief A trap the lifter placed for an operation with no lifted meaning (code is the lifter's): reports it and stops the guest. Returns RL_PENDING. */
RL_RUNTIME_API int rl_trap(rl_ctx *c, uint8_t *mem, uint32_t code);

/**
 * @brief EI: sets IME with a one-instruction delay. IME is 1 and the
 * scheduler mask follows at once, but the first interrupt entry is withheld
 * (ei_delay) and the budget ended, so the entry happens one sync point later.
 * A frontend that places a sync point after the instruction following EI
 * emits rl_sm83_ei_commit before it, which makes the entry exact.
 */
RL_RUNTIME_API void rl_sm83_ei(rl_ctx *c);
/** @brief DI: clears IME and the scheduler mask. */
RL_RUNTIME_API void rl_sm83_di(rl_ctx *c);
/** @brief RETI: sets IME with no delay. */
RL_RUNTIME_API void rl_sm83_reti(rl_ctx *c);
/** @brief Ends the EI delay: the instruction after EI has run. A frontend that places a sync point right after it emits this before the sync. */
static inline void rl_sm83_ei_commit(rl_ctx *c) { c->ei_delay = 0; }
/**
 * @brief The machine's IE register changed to lines (bits 0 to 4): the
 * scheduler mask becomes lines while IME is 1. The guest layer calls it from
 * its IE write handler.
 */
RL_RUNTIME_API void rl_sm83_set_ie(rl_ctx *c, uint8_t lines);
/**
 * @brief HALT: waits for a line of IE & IF, advancing the clock through the
 * scheduler. IME 1 enters the interrupt at the next sync point; IME 0
 * continues at the next instruction. Returns RL_OK, or RL_PENDING when
 * nothing could ever raise a line (the guest is stopped with a report).
 */
RL_RUNTIME_API int rl_sm83_halt(rl_ctx *c, uint8_t *mem);

#endif
