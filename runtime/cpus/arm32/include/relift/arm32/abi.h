/**
 * @file relift/arm32/abi.h
 * @brief Runtime ABI contract between relift-emitted C for ARMv4T guest code
 * (the ARM7TDMI of the Game Boy Advance) and the relift C runtime: the
 * register context with the mode banks, dispatch with its return check, the
 * code fetch costs and the entries lifted code calls.
 *
 * Every lifted translation unit for an ARMv4T guest includes this header and
 * the guest layer's bus header. The register slots follow relift-cpu-arm32's
 * register file (cpus/arm32/src/regs.rs): r0 to r14 as the current mode sees
 * them, the CPSR in parts, the five SPSRs and the banked copies of r8 to r14
 * the current mode does not see. Any change to a type, layout, signature or
 * constant here bumps RL_ABI_VERSION.
 *
 * Calls and returns. A lifted call is a host call, and ARMv4T has no return
 * instruction: a return is a computed jump to the address the call left in
 * LR. The context keeps a stack of the return addresses of the open host
 * frames (shadow); a lifted call pushes its return address (rl_arm32_call)
 * and a computed jump to the address on top of it returns from the host
 * frame (rl_indirect). Any other computed jump goes through dispatch.
 */
#ifndef RELIFT_ARM32_ABI_H
#define RELIFT_ARM32_ABI_H

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** @brief Mode bits of the CPSR. */
enum {
    RL_ARM32_USR = 0x10,
    RL_ARM32_FIQ = 0x11,
    RL_ARM32_IRQ = 0x12,
    RL_ARM32_SVC = 0x13,
    RL_ARM32_ABT = 0x17,
    RL_ARM32_UND = 0x1B,
    RL_ARM32_SYS = 0x1F,
};

/** @brief Entries of the return address stack (rl_ctx.shadow). */
#define RL_ARM32_SHADOW 1024u
/** @brief Entries of rl_ctx.hot. */
#define RL_ARM32_HOT_LINES 256u

/**
 * @brief Guest thread context.
 *
 * The flags, the Thumb bit and the interrupt masks are one byte each, 0 or
 * 1; m holds CPSR bits 4 to 0 and psr_rsvd CPSR bits 27 to 8. spsr is
 * indexed FIQ, IRQ, supervisor, abort, undefined. bank_usr and bank_fiq hold
 * r8 to r14 of the user and FIQ banks, the other banks r13 and r14. A bank
 * entry is valid only while its register is not in r.
 */
struct rl_ctx {
    /** @brief r0 to r14 of the current mode. */
    uint32_t r[15];
    /** @brief Target guest address of the current indirect call or jump; read by rl_dispatch_miss. */
    uint32_t pc;
    /** @brief Negative flag. */
    uint8_t n;
    /** @brief Zero flag. */
    uint8_t z;
    /** @brief Carry flag. */
    uint8_t c;
    /** @brief Overflow flag. */
    uint8_t v;
    /** @brief Thumb state. */
    uint8_t t;
    /** @brief Mode bits. */
    uint8_t m;
    /** @brief IRQ mask. */
    uint8_t i;
    /** @brief FIQ mask. */
    uint8_t f;
    /** @brief CPSR bits 27 to 8. */
    uint32_t psr_rsvd;
    /** @brief The SPSRs of FIQ, IRQ, supervisor, abort and undefined mode. */
    uint32_t spsr[5];
    /** @brief r8 to r14 of user and system mode while another mode runs. */
    uint32_t bank_usr[7];
    /** @brief r8 to r14 of FIQ mode while another mode runs. */
    uint32_t bank_fiq[7];
    /** @brief r13 and r14 of IRQ mode. */
    uint32_t bank_irq[2];
    /** @brief r13 and r14 of supervisor mode. */
    uint32_t bank_svc[2];
    /** @brief r13 and r14 of abort mode. */
    uint32_t bank_abt[2];
    /** @brief r13 and r14 of undefined mode. */
    uint32_t bank_und[2];
    /** @brief The address after the last data access of the current instruction, for its sequential signal. */
    uint32_t data_next;
    /** @brief 1 after a data access of the current instruction (the next one may be sequential). */
    uint8_t data_open;
    /** @brief 1 when the next code fetch is nonsequential (after a data access or an internal cycle). */
    uint8_t code_ns;
    /** @brief Reserved, zero. */
    uint8_t reserved0[2];
    /** @brief Pending record, valid while lifted frames return RL_PENDING. First field of the core's block (rl_ctx_common). */
    rl_pending pend;
    /** @brief Runtime thread object. */
    struct rl_thread *thread;
    /** @brief Dispatch table: one entry per halfword from disp_lo; NULL means miss. */
    rl_fn *disp_table;
    /** @brief Lowest guest code address covered by disp_table. */
    uint64_t disp_lo;
    /** @brief Number of entries in disp_table. */
    uint64_t disp_count;
    /** @brief Guest cycle clock (docs/abi.md section 13). */
    rl_time time;
    /** @brief The guest layer's machine, for its bus calls. */
    void *machine;
    /**
     * @brief Cycles of one access by region: waits[(addr >> 24) * 6 + log2(size) * 2 + seq] for
     * addresses below 0x10000000 (the guest layer's table), or NULL for one cycle everywhere. An
     * entry of 0 asks the guest layer's fetch hook (rl_arm32_fetch_slow), as a cache does.
     */
    const uint8_t *waits;
    /** @brief Bit r set: a sequential code fetch from region r costs one cycle (a prefetch buffer keeps up). */
    uint32_t fast_fetch;
    /** @brief Open entries of shadow. */
    uint32_t depth;
    /** @brief Return addresses of the open host frames of lifted calls, innermost last. */
    uint32_t shadow[RL_ARM32_SHADOW];
    /**
     * @brief Code lines whose fetches in a region whose waits entry is 0 cost hot_cost cycles each and
     * change nothing else, so they skip the fetch hook: entry (addr >> 5) % RL_ARM32_HOT_LINES holds the
     * 32-byte line of addr with bit 0 set when it is one (0 when none). A nonsequential fetch is always
     * the first of a refill's pair, whose second fetch in the same line follows at once. The guest
     * layer's fetch hook fills and empties it.
     */
    uint32_t hot[RL_ARM32_HOT_LINES];
    /** @brief The cycles of a fetch in a line of hot. */
    uint32_t hot_cost;
};

_Static_assert(offsetof(rl_ctx, r) == 0, "r");
_Static_assert(offsetof(rl_ctx, pc) == 60, "pc");
_Static_assert(offsetof(rl_ctx, n) == 64, "n");
_Static_assert(offsetof(rl_ctx, t) == 68, "t");
_Static_assert(offsetof(rl_ctx, m) == 69, "m");
_Static_assert(offsetof(rl_ctx, i) == 70, "i");
_Static_assert(offsetof(rl_ctx, f) == 71, "f");
_Static_assert(offsetof(rl_ctx, psr_rsvd) == 72, "psr_rsvd");
_Static_assert(offsetof(rl_ctx, spsr) == 76, "spsr");
_Static_assert(offsetof(rl_ctx, bank_usr) == 96, "bank_usr");
_Static_assert(offsetof(rl_ctx, bank_fiq) == 124, "bank_fiq");
_Static_assert(offsetof(rl_ctx, bank_irq) == 152, "bank_irq");
_Static_assert(offsetof(rl_ctx, bank_und) == 176, "bank_und");
_Static_assert(offsetof(rl_ctx, data_next) == 184, "data_next");
_Static_assert(offsetof(rl_ctx, pend) == 192, "pend");
_Static_assert(offsetof(rl_ctx, time) == 248, "time");
_Static_assert(offsetof(rl_ctx, machine) == 264, "machine");
_Static_assert(offsetof(rl_ctx, waits) == 272, "waits");
_Static_assert(offsetof(rl_ctx, fast_fetch) == 280, "fast_fetch");
_Static_assert(offsetof(rl_ctx, depth) == 284, "depth");
_Static_assert(offsetof(rl_ctx, shadow) == 288, "shadow");
_Static_assert(offsetof(rl_ctx, hot) == 288 + 4 * RL_ARM32_SHADOW, "hot");
_Static_assert(_Alignof(rl_ctx) == 8, "rl_ctx alignment");

/** @brief Offset of the core's block (rl_ctx_common) in rl_ctx: pend to disp_count. */
#define RL_ARM32_COMMON_OFFSET 192u
/** @brief Offset of the rl_time clock in rl_ctx. */
#define RL_ARM32_TIME_OFFSET 248u
/** @brief log2 of the dispatch granule: Thumb instructions are halfword aligned. */
#define RL_ARM32_CODE_SHIFT 1u

_Static_assert(offsetof(rl_ctx, pend) == RL_ARM32_COMMON_OFFSET + offsetof(rl_ctx_common, pend), "common pend");
_Static_assert(offsetof(rl_ctx, disp_count) == RL_ARM32_COMMON_OFFSET + offsetof(rl_ctx_common, disp_count), "common disp_count");
_Static_assert(sizeof(rl_ctx_common) == offsetof(rl_ctx, time) - RL_ARM32_COMMON_OFFSET, "common block size");

/** @brief Guest memory accessors over a host window (unused by guests whose every access is a bus call). */
static inline uint8_t rl_ld8(const uint8_t *mem, uint64_t a) { return mem[a]; }
static inline uint16_t rl_ld16(const uint8_t *mem, uint64_t a) { uint16_t v; memcpy(&v, mem + a, 2); return v; }
static inline uint32_t rl_ld32(const uint8_t *mem, uint64_t a) { uint32_t v; memcpy(&v, mem + a, 4); return v; }
static inline uint64_t rl_ld64(const uint8_t *mem, uint64_t a) { uint64_t v; memcpy(&v, mem + a, 8); return v; }
static inline void rl_st8(uint8_t *mem, uint64_t a, uint8_t v) { mem[a] = v; }
static inline void rl_st16(uint8_t *mem, uint64_t a, uint16_t v) { memcpy(mem + a, &v, 2); }
static inline void rl_st32(uint8_t *mem, uint64_t a, uint32_t v) { memcpy(mem + a, &v, 4); }
static inline void rl_st64(uint8_t *mem, uint64_t a, uint64_t v) { memcpy(mem + a, &v, 8); }

/** @brief Dispatch miss handler. Reads the target from c->pc. Has the lifted signature so a miss is a call in place. */
RL_RUNTIME_API uint64_t rl_dispatch_miss(rl_ctx *c, uint8_t *mem);
/** @brief Closes the innermost host frame: the target of rl_indirect for a return. */
RL_RUNTIME_API uint64_t rl_arm32_return(rl_ctx *c, uint8_t *mem);

/** @brief Looks up the host function for guest address target; NULL on a miss. */
static inline rl_fn rl_find(const rl_ctx *c, uint64_t target)
{
    uint64_t i = (target - c->disp_lo) >> RL_ARM32_CODE_SHIFT;
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

/**
 * @brief Indirect call or jump: records the target for the miss handler and
 * returns the function to call. A transfer to the return address of the
 * innermost open host frame returns from that frame.
 */
static inline rl_fn rl_indirect(rl_ctx *c, uint64_t target)
{
    c->pc = (uint32_t)target;
    if (c->depth - 1u < RL_ARM32_SHADOW && c->shadow[c->depth - 1u] == (uint32_t)target)
        return rl_arm32_return;
    return rl_lookup(c, target);
}

/** @brief The lookup of RL_JUMP: rl_indirect, but NULL on a miss. */
static inline rl_fn rl_jump_find(rl_ctx *c, uint64_t target)
{
    c->pc = (uint32_t)target;
    if (c->depth - 1u < RL_ARM32_SHADOW && c->shadow[c->depth - 1u] == (uint32_t)target)
        return rl_arm32_return;
    return rl_find(c, target);
}

/**
 * @brief The miss path of RL_JUMP: the target is in c->pc and the guest
 * address of the transferring instruction in c->pend.pc. Has the lifted
 * signature so the miss stays a tail call.
 */
RL_RUNTIME_API uint64_t rl_dispatch_jump_miss(rl_ctx *c, uint8_t *mem);

/** @brief A lifted call that returns to ret opens a host frame. */
static inline void rl_arm32_call(rl_ctx *c, uint32_t ret)
{
    if (c->depth < RL_ARM32_SHADOW)
        c->shadow[c->depth] = ret;
    c->depth++;
}

/**
 * @brief The cycles of a code fetch of 1 << log2 bytes at addr whose
 * region's entry in waits is 0, from the guest layer's fetch hook
 * (rl_arm32_set_fetch_hook), which may also charge the clock itself.
 */
RL_RUNTIME_API int64_t rl_arm32_fetch_slow(rl_ctx *c, uint32_t addr, unsigned log2, unsigned seq);

/** @brief Cycles of an access of 1 << log2 bytes at addr. */
static inline int64_t rl_arm32_wait(rl_ctx *c, uint32_t addr, unsigned log2, unsigned seq)
{
    if (!c->waits || addr >= 0x10000000u)
        return 1;
    if (seq && (c->fast_fetch >> (addr >> 24) & 1u))
        return 1;
    uint8_t w = c->waits[(addr >> 24) * 6u + log2 * 2u + seq];
    if (w)
        return w;
    if (c->hot[(addr >> 5) % RL_ARM32_HOT_LINES] == ((addr & ~31u) | 1u))
        return c->hot_cost;
    return rl_arm32_fetch_slow(c, addr, log2, seq);
}

/**
 * @brief The instruction at pc fetched the next one: one sequential code
 * access of 2 (thumb) or 4 bytes in pc's region. Ends the instruction's data
 * accesses.
 */
static inline void rl_arm32_fetch(rl_ctx *c, uint32_t pc, unsigned thumb)
{
    int64_t w = rl_arm32_wait(c, pc, thumb ? 1u : 2u, 1u);
    c->time.budget -= w;
    c->data_open = 0;
}

/** @brief A taken branch to target refills the pipeline: a nonsequential and a sequential code access there. */
static inline void rl_arm32_refill(rl_ctx *c, uint32_t target, unsigned thumb, unsigned taken)
{
    if (!taken)
        return;
    unsigned w = thumb ? 1u : 2u;
    int64_t n = rl_arm32_wait(c, target, w, 0u);
    n += rl_arm32_wait(c, target, w, 1u);
    c->time.budget -= n;
}

/**
 * @brief The ARM7TDMI's sequential signal of a data access of size bytes at
 * addr: true for an access that follows another of the same instruction at
 * the next address (LDM, STM).
 */
static inline unsigned rl_arm32_data_seq(rl_ctx *c, uint32_t addr, uint32_t size)
{
    unsigned seq = c->data_open && c->data_next == addr;
    c->data_open = 1;
    c->data_next = addr + size;
    return seq;
}

/**
 * @brief The CPSR's I bit changes to i (MSR, an exception return): the
 * guest layer's interrupt lines follow. Lifted code calls it with the new
 * value before the write takes effect.
 */
RL_RUNTIME_API void rl_arm32_set_i(rl_ctx *c, uint32_t i);

/**
 * @brief An instruction at pc whose condition failed: it costs its fetch
 * and does nothing (the slow path of relift-cpu-arm32's COND_FAIL guard).
 */
RL_RUNTIME_API int rl_arm32_cond_fail(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t thumb);

/**
 * @brief The guest executes an instruction ARMv4T does not define, or one
 * the lifter cannot lift: reports it with the registers and stops the guest.
 * Returns RL_PENDING.
 */
RL_RUNTIME_API int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn);

#endif
