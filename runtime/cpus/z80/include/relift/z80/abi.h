/**
 * @file relift/z80/abi.h
 * @brief Runtime ABI contract between relift-emitted C for Z80 guest code
 * and the relift C runtime: the register context, the guest cycle clock
 * (T-states), dispatch, the interrupt model and the entries lifted code
 * calls.
 *
 * Every lifted translation unit for a Z80 guest includes this header and
 * nothing else from the runtime; it includes relift/abi_core.h, the
 * CPU-neutral half. The clock and the interrupt model are docs/abi.md
 * section 13. Any change to a type, layout, signature or constant here bumps
 * RL_ABI_VERSION.
 *
 * Guest addresses that name lifted code are 16-bit instruction addresses
 * and two synthetic ranges (cpus/z80/src/lift): the address of a conditional
 * CALL or RET plus RL_Z80_HALF is the block of its taken case, and
 * RL_Z80_ENTRY plus an RL_Z80_ENTRY_* is an interrupt entry block.
 */
#ifndef RELIFT_Z80_ABI_H
#define RELIFT_Z80_ABI_H

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** @brief Added to the address of a conditional CALL or RET: the block of its taken case. */
#define RL_Z80_HALF 0x10000u
/** @brief The address of the interrupt entry blocks: this plus an RL_Z80_ENTRY_* kind. */
#define RL_Z80_ENTRY 0x20000u
/** @brief The non-maskable interrupt: a call to 0x0066. */
#define RL_Z80_ENTRY_NMI 0u
/** @brief Mode 0 with 0xFF on the data bus: RST 38H. */
#define RL_Z80_ENTRY_IM0 1u
/** @brief Mode 1: a call to 0x0038. */
#define RL_Z80_ENTRY_IM1 2u
/** @brief Mode 2 with 0xFF on the data bus: a call through the word at I:0xFF. */
#define RL_Z80_ENTRY_IM2 3u

/** @brief The scheduler line of the maskable interrupt input (INT). */
#define RL_Z80_LINE_INT 0u
/** @brief The scheduler line of the non-maskable interrupt input (NMI, edge triggered). */
#define RL_Z80_LINE_NMI 1u
/** @brief Bit of rl_z80_ints.pending for the INT input being asserted. */
#define RL_Z80_PEND_INT 1u
/** @brief Bit of rl_z80_ints.pending for a latched NMI edge. */
#define RL_Z80_PEND_NMI 2u

/**
 * @brief The machine's interrupt inputs and I/O ports, as the Z80 layer sees
 * them. The guest layer fills one in and gives it to rl_z80_attach; it must
 * outlive the thread.
 */
typedef struct rl_z80_ints {
    /** @brief Passed to every callback. */
    void *opaque;
    /**
     * @brief The inputs that are asserted, as RL_Z80_PEND_* bits: INT while the line is low (level, so it stays set
     * until the device releases it) and NMI once a falling edge was latched.
     */
    uint8_t (*pending)(void *opaque);
    /** @brief The CPU takes an interrupt: for RL_Z80_PEND_NMI the machine clears the latched edge; for INT it may acknowledge the device. Optional. */
    void (*ack)(void *opaque, uint32_t pend_bit);
    /** @brief An IN: reads the port at the cycle rl_sched_cycles gives. NULL reads 0xFF. */
    uint8_t (*in)(void *opaque, uint16_t port);
    /** @brief An OUT. NULL drops the write. */
    void (*out)(void *opaque, uint16_t port, uint8_t value);
} rl_z80_ints;

/**
 * @brief Guest thread context. One per guest thread, allocated by the runtime.
 *
 * Holds the architectural state at every call boundary (docs/abi.md,
 * "Locals and the context"). f is the flag byte as the CPU has it: bit 7 S,
 * 6 Z, 5 Y, 4 H, 3 X, 2 P/V, 1 N, 0 C; the lifted code's one-bit flag slots
 * are bits of f. af_, bc_, de_ and hl_ are the shadow pairs (af_ has A' in
 * the high byte); q, ei and p are the model's Q, EIDelay (the previous
 * instruction was EI: no maskable interrupt is taken now) and LDAIR (the
 * previous instruction was LD A,I or LD A,R); ret is the address an
 * interrupt entry returns to, which the runtime stores before it runs one.
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
    /** @brief Flags. */
    uint8_t f;
    /** @brief Interrupt vector base. */
    uint8_t i;
    /** @brief Refresh counter. */
    uint8_t r;
    /** @brief Interrupt enable flip-flop 1: maskable interrupts are taken while 1. */
    uint8_t iff1;
    /** @brief Interrupt enable flip-flop 2. */
    uint8_t iff2;
    /** @brief Interrupt mode, 0 to 2. */
    uint8_t im;
    /** @brief The previous instruction set the flags. */
    uint8_t q;
    /** @brief The previous instruction was EI. */
    uint8_t ei;
    /** @brief The previous instruction was LD A,I or LD A,R. */
    uint8_t p;
    /** @brief IX. */
    uint16_t ix;
    /** @brief IY. */
    uint16_t iy;
    /** @brief Stack pointer. */
    uint16_t sp;
    /** @brief MEMPTR. */
    uint16_t wz;
    /** @brief A' and F'. */
    uint16_t af_;
    /** @brief B' and C'. */
    uint16_t bc_;
    /** @brief D' and E'. */
    uint16_t de_;
    /** @brief H' and L'. */
    uint16_t hl_;
    /** @brief The address the next interrupt entry returns to. */
    uint16_t ret;
    /** @brief Target guest address of the current indirect call or jump; read by rl_dispatch_miss. */
    uint16_t pc;
    /** @brief 1 while the CPU waits in HALT. */
    uint8_t halted;
    /** @brief Reserved, zero. */
    uint8_t reserved0[3];
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
    /** @brief Guest cycle clock in T-states (docs/abi.md section 13). */
    rl_time time;
    /** @brief The machine's interrupt inputs and ports (rl_z80_attach), or NULL. */
    const rl_z80_ints *ints;
};

_Static_assert(offsetof(rl_ctx, a) == 0, "a");
_Static_assert(offsetof(rl_ctx, f) == 7, "f");
_Static_assert(offsetof(rl_ctx, i) == 8, "i");
_Static_assert(offsetof(rl_ctx, iff1) == 10, "iff1");
_Static_assert(offsetof(rl_ctx, p) == 15, "p");
_Static_assert(offsetof(rl_ctx, ix) == 16, "ix");
_Static_assert(offsetof(rl_ctx, sp) == 20, "sp");
_Static_assert(offsetof(rl_ctx, af_) == 24, "af_");
_Static_assert(offsetof(rl_ctx, ret) == 32, "ret");
_Static_assert(offsetof(rl_ctx, pc) == 34, "pc");
_Static_assert(offsetof(rl_ctx, halted) == 36, "halted");
_Static_assert(offsetof(rl_ctx, pend) == 40, "pend");
_Static_assert(offsetof(rl_ctx, thread) == 64, "thread");
_Static_assert(offsetof(rl_ctx, disp_table) == 72, "disp_table");
_Static_assert(offsetof(rl_ctx, disp_lo) == 80, "disp_lo");
_Static_assert(offsetof(rl_ctx, disp_count) == 88, "disp_count");
_Static_assert(offsetof(rl_ctx, time) == 96, "time");
_Static_assert(offsetof(rl_ctx, ints) == 112, "ints");
_Static_assert(sizeof(rl_ctx) == 120, "rl_ctx size");
_Static_assert(_Alignof(rl_ctx) == 8, "rl_ctx alignment");

/** @brief Offset of the core's block (rl_ctx_common) in rl_ctx: pend to disp_count. */
#define RL_Z80_COMMON_OFFSET 40u
/** @brief Offset of the rl_time clock in rl_ctx. */
#define RL_Z80_TIME_OFFSET 96u
/** @brief log2 of the dispatch granule: Z80 instructions are byte aligned. */
#define RL_Z80_CODE_SHIFT 0u

_Static_assert(offsetof(rl_ctx, pend) == RL_Z80_COMMON_OFFSET + offsetof(rl_ctx_common, pend), "common pend");
_Static_assert(offsetof(rl_ctx, thread) == RL_Z80_COMMON_OFFSET + offsetof(rl_ctx_common, thread), "common thread");
_Static_assert(offsetof(rl_ctx, disp_table) == RL_Z80_COMMON_OFFSET + offsetof(rl_ctx_common, disp_table), "common disp_table");
_Static_assert(offsetof(rl_ctx, disp_lo) == RL_Z80_COMMON_OFFSET + offsetof(rl_ctx_common, disp_lo), "common disp_lo");
_Static_assert(offsetof(rl_ctx, disp_count) == RL_Z80_COMMON_OFFSET + offsetof(rl_ctx_common, disp_count), "common disp_count");
_Static_assert(sizeof(rl_ctx_common) == offsetof(rl_ctx, time) - RL_Z80_COMMON_OFFSET, "common block size");
_Static_assert(offsetof(rl_ctx, time) == RL_Z80_TIME_OFFSET, "time offset");

/** @brief Guest memory accessors the lifted code uses: the guest's 64 KiB window is host memory at mem, little endian. */
static inline uint8_t rl_ld8(const uint8_t *mem, uint64_t a) { return mem[a]; }
static inline uint16_t rl_ld16(const uint8_t *mem, uint64_t a) { uint16_t v; memcpy(&v, mem + a, 2); return v; }
static inline uint32_t rl_ld32(const uint8_t *mem, uint64_t a) { uint32_t v; memcpy(&v, mem + a, 4); return v; }
static inline uint64_t rl_ld64(const uint8_t *mem, uint64_t a) { uint64_t v; memcpy(&v, mem + a, 8); return v; }
static inline rl_u128 rl_ld128(const uint8_t *mem, uint64_t a) { rl_u128 v; memcpy(&v, mem + a, 16); return v; }
static inline void rl_st8(uint8_t *mem, uint64_t a, uint8_t v) { mem[a] = v; }
static inline void rl_st16(uint8_t *mem, uint64_t a, uint16_t v) { memcpy(mem + a, &v, 2); }
static inline void rl_st32(uint8_t *mem, uint64_t a, uint32_t v) { memcpy(mem + a, &v, 4); }
static inline void rl_st64(uint8_t *mem, uint64_t a, uint64_t v) { memcpy(mem + a, &v, 8); }
static inline void rl_st128(uint8_t *mem, uint64_t a, rl_u128 v) { memcpy(mem + a, &v, 16); }

/** @brief Dispatch miss handler. Reads the target from c->pc. Has the lifted signature so a miss is a call in place. */
RL_RUNTIME_API int rl_dispatch_miss(rl_ctx *c, uint8_t *mem);

/** @brief Looks up the host function for guest address target; NULL on a miss. */
static inline rl_fn rl_find(const rl_ctx *c, uint64_t target)
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
    return f;
}

/** @brief Looks up the host function for guest address target; returns rl_dispatch_miss on a miss. */
static inline rl_fn rl_lookup(const rl_ctx *c, uint64_t target)
{
    rl_fn f = rl_find(c, target);
    return f ? f : rl_dispatch_miss;
}

/** @brief Indirect call: records the target for the miss handler and returns the function to call. */
static inline rl_fn rl_indirect(rl_ctx *c, uint64_t target)
{
    c->pc = (uint16_t)target;
    return rl_lookup(c, target);
}

/** @brief The lookup of RL_JUMP: records the target and returns its function, or NULL on a miss. */
static inline rl_fn rl_jump_find(rl_ctx *c, uint64_t target)
{
    c->pc = (uint16_t)target;
    return rl_find(c, target);
}

/**
 * @brief The miss path of RL_JUMP: the target is in c->pc and the guest
 * address of the transferring instruction in c->pend.pc. Has the lifted
 * signature so the miss stays a tail call.
 */
RL_RUNTIME_API int rl_dispatch_jump_miss(rl_ctx *c, uint8_t *mem);

/**
 * @brief The guest executes an instruction the lifter cannot lift: reports
 * it with the registers and stops the guest. insn is the instruction bytes,
 * first byte most significant. Returns RL_PENDING.
 */
RL_RUNTIME_API int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn);
/** @brief A trap the lifter placed for an operation with no lifted meaning (code is the lifter's): reports it and stops the guest. Returns RL_PENDING. */
RL_RUNTIME_API int rl_trap(rl_ctx *c, uint8_t *mem, uint32_t code);

/**
 * @brief The value an instruction (EI, DI, RETI, RETN, an interrupt entry)
 * gives IFF1, called just before the sync point that follows it: the
 * scheduler mask becomes the NMI line, plus the INT line while iff1 is 1. A
 * line raised in the new mask ends the clock's budget, so the sync point
 * takes the interrupt (the entry hook withholds a maskable one after EI).
 * A raise of INT while iff1 is 0 does not end the budget.
 */
RL_RUNTIME_API void rl_z80_iff(rl_ctx *c, uint8_t iff1);
/**
 * @brief IN: the byte the machine gives for port (0xFF without a machine).
 * The lifted code has stored the clock, so the machine reads the cycle with
 * rl_sched_cycles.
 */
RL_RUNTIME_API uint8_t rl_z80_in(rl_ctx *c, uint16_t port);
/** @brief OUT: hands the byte to the machine. */
RL_RUNTIME_API void rl_z80_out(rl_ctx *c, uint16_t port, uint8_t value);
/**
 * @brief HALT, after the lifter charged the instruction's 4 T-states, with the
 * values of IFF1 and R it left (the lifted code keeps registers in locals,
 * so the context is not current): the CPU executes NOPs until an interrupt
 * can be taken (an NMI, or INT with iff1 set), advancing the clock through
 * the scheduler by whole NOP cycles of 4 T-states. Returns the R after the
 * NOPs: r with one more in its low 7 bits per NOP. The interrupt is entered
 * at the sync point that ends the HALT block, with the address after HALT as
 * its return address. A HALT that nothing can end stops the guest with a
 * report.
 */
RL_RUNTIME_API uint8_t rl_z80_halt(rl_ctx *c, uint8_t *mem, uint8_t iff1, uint8_t r);

#endif
