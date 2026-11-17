/**
 * @file relift/mips/abi.h
 * @brief Runtime ABI contract between relift-emitted C for VR4300 guest code
 * (the Nintendo 64 CPU) and the relift C runtime: the register context in
 * relift-cpu-mips's slot order, big-endian guest memory, dispatch with a
 * return address stack, and the entries lifted code calls (the FPU
 * intrinsics, Random, the TLB instructions, CACHE and the guard slow paths).
 *
 * Every lifted translation unit for a VR4300 guest includes this header.
 * The register slots follow relift-cpu-mips's register file
 * (cpus/mips/src/vr4300/regs.rs): RL_MIPS_SLOTS lists them in slot order with the
 * context field that holds each one and its IR width, and
 * cpus/mips/tests/ctx_layout.rs checks the list against the register file.
 * Any change to a type, layout, signature or constant here bumps
 * RL_ABI_VERSION.
 *
 * Calls and returns. JAL and JALR are host calls, and JR to the address in
 * the link register is a computed jump. As on ARMv4T, the context keeps a
 * stack of the return addresses of the open host frames (shadow): a lifted
 * call pushes its return address (rl_mips_call) and a computed jump to the
 * address on top of it returns from the host frame (rl_indirect).
 */
#ifndef RELIFT_MIPS_ABI_H
#define RELIFT_MIPS_ABI_H

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** @brief Entries of the return address stack (rl_ctx.shadow). */
#define RL_MIPS_SHADOW 1024u
/** @brief Entries of the VR4300 TLB. */
#define RL_MIPS_TLB_ENTRIES 32u
/** @brief Lines of the instruction cache (16 KiB of 32-byte lines). */
#define RL_MIPS_ICACHE_LINES 512u
/** @brief Lines of the data cache (8 KiB of 16-byte lines). */
#define RL_MIPS_DCACHE_LINES 512u
/** @brief The valid bit of a cache line's tag word; bits 31 to 12 are the line's physical address. */
#define RL_MIPS_LINE_VALID 1u
/** @brief The dirty bit of a data cache line's tag word. */
#define RL_MIPS_LINE_DIRTY 2u

/**
 * @brief The register slots of relift-cpu-mips in slot order:
 * X(model name, rl_ctx field, IR width in bits). A 1-bit slot is a byte
 * holding 0 or 1.
 */
#define RL_MIPS_SLOTS(X)                                                                                               \
    X(r0, r[0], 64)                                                                                                    \
    X(r1, r[1], 64)                                                                                                    \
    X(r2, r[2], 64)                                                                                                    \
    X(r3, r[3], 64)                                                                                                    \
    X(r4, r[4], 64)                                                                                                    \
    X(r5, r[5], 64)                                                                                                    \
    X(r6, r[6], 64)                                                                                                    \
    X(r7, r[7], 64)                                                                                                    \
    X(r8, r[8], 64)                                                                                                    \
    X(r9, r[9], 64)                                                                                                    \
    X(r10, r[10], 64)                                                                                                  \
    X(r11, r[11], 64)                                                                                                  \
    X(r12, r[12], 64)                                                                                                  \
    X(r13, r[13], 64)                                                                                                  \
    X(r14, r[14], 64)                                                                                                  \
    X(r15, r[15], 64)                                                                                                  \
    X(r16, r[16], 64)                                                                                                  \
    X(r17, r[17], 64)                                                                                                  \
    X(r18, r[18], 64)                                                                                                  \
    X(r19, r[19], 64)                                                                                                  \
    X(r20, r[20], 64)                                                                                                  \
    X(r21, r[21], 64)                                                                                                  \
    X(r22, r[22], 64)                                                                                                  \
    X(r23, r[23], 64)                                                                                                  \
    X(r24, r[24], 64)                                                                                                  \
    X(r25, r[25], 64)                                                                                                  \
    X(r26, r[26], 64)                                                                                                  \
    X(r27, r[27], 64)                                                                                                  \
    X(r28, r[28], 64)                                                                                                  \
    X(r29, r[29], 64)                                                                                                  \
    X(r30, r[30], 64)                                                                                                  \
    X(r31, r[31], 64)                                                                                                  \
    X(HI, hi, 64)                                                                                                      \
    X(LO, lo, 64)                                                                                                      \
    X(f0, f[0], 64)                                                                                                    \
    X(f1, f[1], 64)                                                                                                    \
    X(f2, f[2], 64)                                                                                                    \
    X(f3, f[3], 64)                                                                                                    \
    X(f4, f[4], 64)                                                                                                    \
    X(f5, f[5], 64)                                                                                                    \
    X(f6, f[6], 64)                                                                                                    \
    X(f7, f[7], 64)                                                                                                    \
    X(f8, f[8], 64)                                                                                                    \
    X(f9, f[9], 64)                                                                                                    \
    X(f10, f[10], 64)                                                                                                  \
    X(f11, f[11], 64)                                                                                                  \
    X(f12, f[12], 64)                                                                                                  \
    X(f13, f[13], 64)                                                                                                  \
    X(f14, f[14], 64)                                                                                                  \
    X(f15, f[15], 64)                                                                                                  \
    X(f16, f[16], 64)                                                                                                  \
    X(f17, f[17], 64)                                                                                                  \
    X(f18, f[18], 64)                                                                                                  \
    X(f19, f[19], 64)                                                                                                  \
    X(f20, f[20], 64)                                                                                                  \
    X(f21, f[21], 64)                                                                                                  \
    X(f22, f[22], 64)                                                                                                  \
    X(f23, f[23], 64)                                                                                                  \
    X(f24, f[24], 64)                                                                                                  \
    X(f25, f[25], 64)                                                                                                  \
    X(f26, f[26], 64)                                                                                                  \
    X(f27, f[27], 64)                                                                                                  \
    X(f28, f[28], 64)                                                                                                  \
    X(f29, f[29], 64)                                                                                                  \
    X(f30, f[30], 64)                                                                                                  \
    X(f31, f[31], 64)                                                                                                  \
    X(FCR31, fcr31, 32)                                                                                                \
    X(CP0Index, cp0_index, 64)                                                                                         \
    X(CP0EntryLo0, cp0_entry_lo0, 64)                                                                                  \
    X(CP0EntryLo1, cp0_entry_lo1, 64)                                                                                  \
    X(CP0Context, cp0_context, 64)                                                                                     \
    X(CP0PageMask, cp0_page_mask, 64)                                                                                  \
    X(CP0Wired, cp0_wired, 64)                                                                                         \
    X(CP0BadVAddr, cp0_bad_vaddr, 64)                                                                                  \
    X(CP0Count, cp0_count, 32)                                                                                         \
    X(CP0EntryHi, cp0_entry_hi, 64)                                                                                    \
    X(CP0Compare, cp0_compare, 32)                                                                                     \
    X(CP0Status, cp0_status, 32)                                                                                       \
    X(CP0Cause, cp0_cause, 32)                                                                                         \
    X(CP0EPC, cp0_epc, 64)                                                                                             \
    X(CP0Config, cp0_config, 64)                                                                                       \
    X(CP0LLAddr, cp0_ll_addr, 64)                                                                                      \
    X(CP0WatchLo, cp0_watch_lo, 64)                                                                                    \
    X(CP0WatchHi, cp0_watch_hi, 64)                                                                                    \
    X(CP0XContext, cp0_xcontext, 64)                                                                                   \
    X(CP0ParityErr, cp0_parity_err, 64)                                                                                \
    X(CP0TagLo, cp0_tag_lo, 64)                                                                                        \
    X(CP0ErrorEPC, cp0_error_epc, 64)                                                                                  \
    X(CP0Latch, cp0_latch, 64)                                                                                         \
    X(CP0LLBit, cp0_ll_bit, 1)

/** @brief Status register bits. */
enum {
    RL_MIPS_SR_IE = 1u << 0,
    RL_MIPS_SR_EXL = 1u << 1,
    RL_MIPS_SR_ERL = 1u << 2,
    RL_MIPS_SR_KSU = 3u << 3,
    RL_MIPS_SR_UX = 1u << 5,
    RL_MIPS_SR_SX = 1u << 6,
    RL_MIPS_SR_KX = 1u << 7,
    RL_MIPS_SR_IM = 0xFFu << 8,
    RL_MIPS_SR_SR = 1u << 20,
    RL_MIPS_SR_TS = 1u << 21,
    RL_MIPS_SR_BEV = 1u << 22,
    RL_MIPS_SR_RE = 1u << 25,
    RL_MIPS_SR_FR = 1u << 26,
    RL_MIPS_SR_CU0 = 1u << 28,
    RL_MIPS_SR_CU1 = 1u << 29,
};

/** @brief Cause register bits. */
enum {
    RL_MIPS_CAUSE_EXC = 0x1Fu << 2,
    RL_MIPS_CAUSE_IP = 0xFFu << 8,
    RL_MIPS_CAUSE_IP7 = 1u << 15,
    RL_MIPS_CAUSE_CE = 3u << 28,
    RL_MIPS_CAUSE_BD = 1u << 31,
};

/** @brief Cause.ExcCode values. */
enum {
    RL_MIPS_EXC_INT = 0,
    RL_MIPS_EXC_MOD = 1,
    RL_MIPS_EXC_TLBL = 2,
    RL_MIPS_EXC_TLBS = 3,
    RL_MIPS_EXC_ADEL = 4,
    RL_MIPS_EXC_ADES = 5,
    RL_MIPS_EXC_IBE = 6,
    RL_MIPS_EXC_DBE = 7,
    RL_MIPS_EXC_SYS = 8,
    RL_MIPS_EXC_BP = 9,
    RL_MIPS_EXC_RI = 10,
    RL_MIPS_EXC_CPU = 11,
    RL_MIPS_EXC_OV = 12,
    RL_MIPS_EXC_TR = 13,
    RL_MIPS_EXC_FPE = 15,
    RL_MIPS_EXC_WATCH = 23,
};

/** @brief FCR31 bits: the rounding mode, then flags, enables and causes in the order I, U, O, Z, V (cause also E). */
enum {
    RL_MIPS_FCR_RM = 3u,
    RL_MIPS_FCR_FLAGS = 0x1Fu << 2,
    RL_MIPS_FCR_ENABLES = 0x1Fu << 7,
    RL_MIPS_FCR_CAUSE = 0x3Fu << 12,
    RL_MIPS_FCR_E = 1u << 17,
    RL_MIPS_FCR_C = 1u << 23,
    RL_MIPS_FCR_FS = 1u << 24,
    RL_MIPS_FCR_MASK = 0x0183FFFFu,
};

/** @brief The guard codes of relift-cpu-mips (cpus/mips/src/vr4300/lift/rt.rs): EXC + ExcCode, FP_SLOW, plus IN_DELAY in a delay slot. */
enum {
    RL_MIPS_GUARD_EXC = 0x1000,
    RL_MIPS_GUARD_IN_DELAY = 0x100,
    RL_MIPS_GUARD_FP_SLOW = 0x2000,
};

/** @brief One TLB entry, kept in the CP0 register formats TLBR returns. */
typedef struct rl_mips_tlb_entry {
    /** @brief PageMask: bits 24 to 13. */
    uint64_t page_mask;
    /** @brief EntryHi: R, VPN2 (masked by the page mask) and ASID. */
    uint64_t entry_hi;
    /** @brief EntryLo0 and EntryLo1: PFN, C, D, V, and G (both hold the entry's G). */
    uint64_t entry_lo[2];
} rl_mips_tlb_entry;

/**
 * @brief Guest thread context.
 *
 * The fields from r to cp0_ll_bit are relift-cpu-mips's register slots
 * (RL_MIPS_SLOTS); the reserved fields pad them to their natural alignment.
 * The fields after the core's block belong to the runtime: lifted code reads
 * only pc, time and the frame stack.
 */
struct rl_ctx {
    /** @brief r0 to r31; r0 stays 0. */
    uint64_t r[32];
    /** @brief HI. */
    uint64_t hi;
    /** @brief LO. */
    uint64_t lo;
    /** @brief f0 to f31, 64 bits each (the FR = 1 view). */
    uint64_t f[32];
    /** @brief FCR31. */
    uint32_t fcr31;
    /** @brief Reserved, zero. */
    uint32_t reserved0;
    /** @brief Index. */
    uint64_t cp0_index;
    /** @brief EntryLo0. */
    uint64_t cp0_entry_lo0;
    /** @brief EntryLo1. */
    uint64_t cp0_entry_lo1;
    /** @brief Context. */
    uint64_t cp0_context;
    /** @brief PageMask. */
    uint64_t cp0_page_mask;
    /** @brief Wired. */
    uint64_t cp0_wired;
    /** @brief BadVAddr. */
    uint64_t cp0_bad_vaddr;
    /** @brief Count as of the runtime's last update (rl_mips_count gives the current value). */
    uint32_t cp0_count;
    /** @brief Reserved, zero. */
    uint32_t reserved1;
    /** @brief EntryHi. */
    uint64_t cp0_entry_hi;
    /** @brief Compare. */
    uint32_t cp0_compare;
    /** @brief Status. */
    uint32_t cp0_status;
    /** @brief Cause. */
    uint32_t cp0_cause;
    /** @brief Reserved, zero. */
    uint32_t reserved2;
    /** @brief EPC. */
    uint64_t cp0_epc;
    /** @brief Config as written (reads mix in fixed bits). */
    uint64_t cp0_config;
    /** @brief LLAddr. */
    uint64_t cp0_ll_addr;
    /** @brief WatchLo. */
    uint64_t cp0_watch_lo;
    /** @brief WatchHi. */
    uint64_t cp0_watch_hi;
    /** @brief XContext. */
    uint64_t cp0_xcontext;
    /** @brief ParityError. */
    uint64_t cp0_parity_err;
    /** @brief TagLo. */
    uint64_t cp0_tag_lo;
    /** @brief ErrorEPC. */
    uint64_t cp0_error_epc;
    /** @brief The last value written to any CP0 register (reads of unimplemented numbers return it). */
    uint64_t cp0_latch;
    /** @brief The LL/SC link bit, 0 or 1. */
    uint8_t cp0_ll_bit;
    /** @brief Reserved, zero. */
    uint8_t reserved3[7];
    /** @brief Target guest address of the current indirect call or jump; read by rl_dispatch_miss. */
    uint64_t pc;
    /** @brief Pending record, valid while lifted frames return RL_PENDING. First field of the core's block (rl_ctx_common). */
    rl_pending pend;
    /** @brief Runtime thread object. */
    struct rl_thread *thread;
    /** @brief Dispatch table: one entry per word from disp_lo; NULL means miss. */
    rl_fn *disp_table;
    /** @brief Lowest guest code address covered by disp_table. */
    uint64_t disp_lo;
    /** @brief Number of entries in disp_table. */
    uint64_t disp_count;
    /** @brief Guest cycle clock (docs/abi.md section 13), in PClock cycles. */
    rl_time time;
    /** @brief The guest layer's machine. */
    void *machine;
    /** @brief The TLB. */
    rl_mips_tlb_entry tlb[RL_MIPS_TLB_ENTRIES];
    /** @brief Count at count_cycle (a write of Count or the start). */
    uint64_t count_base;
    /** @brief The cycle Count was count_base at. */
    uint64_t count_cycle;
    /** @brief Count at the last timer check, not wrapped. */
    uint64_t count_last;
    /** @brief The cycle Wired was last written at (Random counts from it). */
    uint64_t wired_cycle;
    /** @brief The values of cp0_count and cp0_wired the runtime last saw, to notice guest writes. */
    uint32_t count_seen;
    /** @brief See count_seen. */
    uint32_t wired_seen;
    /** @brief Interrupt entries whose handler has not returned yet. */
    uint32_t irq_open;
    /** @brief Open entries of shadow. */
    uint32_t depth;
    /** @brief Return addresses of the open host frames of lifted calls, innermost last. */
    uint64_t shadow[RL_MIPS_SHADOW];
    /** @brief The instruction cache's tags (RL_MIPS_LINE_VALID and the physical address), indexed by vaddr bits 13 to 5. */
    uint32_t icache[RL_MIPS_ICACHE_LINES];
    /** @brief The data cache's tags (RL_MIPS_LINE_VALID, RL_MIPS_LINE_DIRTY and the physical address), indexed by vaddr bits 12 to 4. */
    uint32_t dcache[RL_MIPS_DCACHE_LINES];
};

#define RL_MIPS_SLOT_ASSERT(name, field, bits)                                                                         \
    _Static_assert(sizeof(((rl_ctx *)0)->field) * 8 == ((bits) < 8 ? 8 : (bits)), #name " width");
RL_MIPS_SLOTS(RL_MIPS_SLOT_ASSERT)
#undef RL_MIPS_SLOT_ASSERT

_Static_assert(offsetof(rl_ctx, r) == 0, "r");
_Static_assert(offsetof(rl_ctx, hi) == 256, "hi");
_Static_assert(offsetof(rl_ctx, f) == 272, "f");
_Static_assert(offsetof(rl_ctx, fcr31) == 528, "fcr31");
_Static_assert(offsetof(rl_ctx, cp0_index) == 536, "cp0_index");
_Static_assert(offsetof(rl_ctx, cp0_count) == 592, "cp0_count");
_Static_assert(offsetof(rl_ctx, cp0_entry_hi) == 600, "cp0_entry_hi");
_Static_assert(offsetof(rl_ctx, cp0_compare) == 608, "cp0_compare");
_Static_assert(offsetof(rl_ctx, cp0_status) == 612, "cp0_status");
_Static_assert(offsetof(rl_ctx, cp0_cause) == 616, "cp0_cause");
_Static_assert(offsetof(rl_ctx, cp0_epc) == 624, "cp0_epc");
_Static_assert(offsetof(rl_ctx, cp0_error_epc) == 688, "cp0_error_epc");
_Static_assert(offsetof(rl_ctx, cp0_ll_bit) == 704, "cp0_ll_bit");
_Static_assert(offsetof(rl_ctx, pc) == 712, "pc");
_Static_assert(offsetof(rl_ctx, pend) == 720, "pend");
_Static_assert(offsetof(rl_ctx, time) == 776, "time");
_Static_assert(offsetof(rl_ctx, machine) == 792, "machine");
_Static_assert(offsetof(rl_ctx, tlb) == 800, "tlb");
_Static_assert(offsetof(rl_ctx, shadow) == 1872, "shadow");
_Static_assert(offsetof(rl_ctx, icache) == 10064, "icache");
_Static_assert(offsetof(rl_ctx, dcache) == 12112, "dcache");
_Static_assert(_Alignof(rl_ctx) == 8, "rl_ctx alignment");

/** @brief Offset of the core's block (rl_ctx_common) in rl_ctx: pend to disp_count. */
#define RL_MIPS_COMMON_OFFSET 720u
/** @brief Offset of the rl_time clock in rl_ctx. */
#define RL_MIPS_TIME_OFFSET 776u
/** @brief log2 of the dispatch granule: instructions are words. */
#define RL_MIPS_CODE_SHIFT 2u

_Static_assert(offsetof(rl_ctx, pend) == RL_MIPS_COMMON_OFFSET + offsetof(rl_ctx_common, pend), "common pend");
_Static_assert(offsetof(rl_ctx, disp_count) == RL_MIPS_COMMON_OFFSET + offsetof(rl_ctx_common, disp_count),
               "common disp_count");
_Static_assert(sizeof(rl_ctx_common) == offsetof(rl_ctx, time) - RL_MIPS_COMMON_OFFSET, "common block size");

/**
 * @brief Raw accessors over the host window at physical addresses, as the
 * emitted C uses them: the bytes in host order (little-endian on every
 * host relift targets); the lifted code swaps them for the guest's
 * big-endian accesses.
 */
static inline uint8_t rl_ld8(const uint8_t *mem, uint64_t a) { return mem[a]; }
static inline uint16_t rl_ld16(const uint8_t *mem, uint64_t a)
{
    uint16_t v;
    memcpy(&v, mem + a, sizeof v);
    return v;
}
static inline uint32_t rl_ld32(const uint8_t *mem, uint64_t a)
{
    uint32_t v;
    memcpy(&v, mem + a, sizeof v);
    return v;
}
static inline uint64_t rl_ld64(const uint8_t *mem, uint64_t a)
{
    uint64_t v;
    memcpy(&v, mem + a, sizeof v);
    return v;
}
static inline void rl_st8(uint8_t *mem, uint64_t a, uint8_t v) { mem[a] = v; }
static inline void rl_st16(uint8_t *mem, uint64_t a, uint16_t v) { memcpy(mem + a, &v, sizeof v); }
static inline void rl_st32(uint8_t *mem, uint64_t a, uint32_t v) { memcpy(mem + a, &v, sizeof v); }
static inline void rl_st64(uint8_t *mem, uint64_t a, uint64_t v) { memcpy(mem + a, &v, sizeof v); }

/** @brief The big-endian value of size bytes (1, 2, 4 or 8) at mem + a: the guest's view, for the runtime's own accesses. */
static inline uint64_t rl_mips_ld(const uint8_t *mem, uint64_t a, uint32_t size)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; i++)
        v = v << 8 | mem[a + i];
    return v;
}
/** @brief Stores the big-endian value v of size bytes at mem + a. */
static inline void rl_mips_st(uint8_t *mem, uint64_t a, uint32_t size, uint64_t v)
{
    for (uint32_t i = 0; i < size; i++)
        mem[a + i] = (uint8_t)(v >> (8u * (size - 1u - i)));
}
/** @brief v (size bytes) in the other byte order: a device value as the emitted C passes it. */
static inline uint64_t rl_mips_swap(uint64_t v, uint32_t size)
{
    switch (size) {
    case 2:
        return __builtin_bswap16((uint16_t)v);
    case 4:
        return __builtin_bswap32((uint32_t)v);
    case 8:
        return __builtin_bswap64(v);
    default:
        return v & 0xFFu;
    }
}

/*
 * The FPU fast path (relift-cpu-mips's FpOp): the host's IEEE operations in
 * round to nearest. The lifted code takes it only for normal operands whose
 * result is normal and with no enable set, so no host flag or special value
 * matters. Operands and results are bit patterns.
 */
static inline float rl_mips_f32(uint32_t b)
{
    float f;
    memcpy(&f, &b, sizeof f);
    return f;
}
static inline uint32_t rl_mips_b32(float f)
{
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}
static inline double rl_mips_f64(uint64_t b)
{
    double f;
    memcpy(&f, &b, sizeof f);
    return f;
}
static inline uint64_t rl_mips_b64(double f)
{
    uint64_t b;
    memcpy(&b, &f, sizeof b);
    return b;
}
/** @brief a + b, binary32. */
static inline uint32_t rl_mips_fadd32(uint32_t a, uint32_t b) { return rl_mips_b32(rl_mips_f32(a) + rl_mips_f32(b)); }
/** @brief a - b, binary32. */
static inline uint32_t rl_mips_fsub32(uint32_t a, uint32_t b) { return rl_mips_b32(rl_mips_f32(a) - rl_mips_f32(b)); }
/** @brief a * b, binary32. */
static inline uint32_t rl_mips_fmul32(uint32_t a, uint32_t b) { return rl_mips_b32(rl_mips_f32(a) * rl_mips_f32(b)); }
/** @brief a / b, binary32. */
static inline uint32_t rl_mips_fdiv32(uint32_t a, uint32_t b) { return rl_mips_b32(rl_mips_f32(a) / rl_mips_f32(b)); }
/** @brief The square root of a, binary32. */
static inline uint32_t rl_mips_fsqrt32(uint32_t a) { return rl_mips_b32(__builtin_sqrtf(rl_mips_f32(a))); }
/** @brief a + b, binary64. */
static inline uint64_t rl_mips_fadd64(uint64_t a, uint64_t b) { return rl_mips_b64(rl_mips_f64(a) + rl_mips_f64(b)); }
/** @brief a - b, binary64. */
static inline uint64_t rl_mips_fsub64(uint64_t a, uint64_t b) { return rl_mips_b64(rl_mips_f64(a) - rl_mips_f64(b)); }
/** @brief a * b, binary64. */
static inline uint64_t rl_mips_fmul64(uint64_t a, uint64_t b) { return rl_mips_b64(rl_mips_f64(a) * rl_mips_f64(b)); }
/** @brief a / b, binary64. */
static inline uint64_t rl_mips_fdiv64(uint64_t a, uint64_t b) { return rl_mips_b64(rl_mips_f64(a) / rl_mips_f64(b)); }
/** @brief The square root of a, binary64. */
static inline uint64_t rl_mips_fsqrt64(uint64_t a) { return rl_mips_b64(__builtin_sqrt(rl_mips_f64(a))); }

/** @brief The float x of fbits (32 or 64) bits as a double. */
static inline double rl_mips_fval(uint64_t x, unsigned fbits)
{
    return fbits == 32 ? (double)rl_mips_f32((uint32_t)x) : rl_mips_f64(x);
}

/**
 * @brief The float x of fbits bits rounded to a signed integer of ibits (32
 * or 64) bits by rm (0 nearest even, 1 nearest away, 2 floor, 3 ceil, 4
 * toward zero), saturating; NaN gives 0. The result is the integer's bits.
 */
static inline uint64_t rl_mips_f2i(uint64_t x, unsigned fbits, unsigned ibits, unsigned rm)
{
    double v = rl_mips_fval(x, fbits);
    if (v != v)
        return 0;
    switch (rm) {
    case 1:
        v = __builtin_round(v);
        break;
    case 2:
        v = __builtin_floor(v);
        break;
    case 3:
        v = __builtin_ceil(v);
        break;
    case 4:
        v = __builtin_trunc(v);
        break;
    default:
        v = __builtin_rint(v);
        break;
    }
    if (ibits == 32) {
        if (v >= 2147483648.0)
            return 0x7FFFFFFFu;
        if (v < -2147483648.0)
            return 0x80000000u;
        return (uint32_t)(int32_t)v;
    }
    if (v >= 9223372036854775808.0)
        return UINT64_C(0x7FFFFFFFFFFFFFFF);
    if (v < -9223372036854775808.0)
        return UINT64_C(0x8000000000000000);
    return (uint64_t)(int64_t)v;
}

/** @brief The signed integer x of ibits (32 or 64) bits as a float of fbits bits, rounded to nearest even. */
static inline uint64_t rl_mips_i2f(uint64_t x, unsigned ibits, unsigned fbits)
{
    int64_t v = ibits == 32 ? (int64_t)(int32_t)(uint32_t)x : (int64_t)x;
    return fbits == 32 ? rl_mips_b32((float)v) : rl_mips_b64((double)v);
}

/** @brief The float x of from bits in the other format (to bits), rounded to nearest even. */
static inline uint64_t rl_mips_fcvt(uint64_t x, unsigned from, unsigned to)
{
    if (from == to)
        return x;
    return to == 32 ? rl_mips_b32((float)rl_mips_f64(x)) : rl_mips_b64((double)rl_mips_f32((uint32_t)x));
}

/** @brief Dispatch miss handler. Reads the target from c->pc. Has the lifted signature so a miss is a call in place. */
RL_RUNTIME_API uint64_t rl_dispatch_miss(rl_ctx *c, uint8_t *mem);
/** @brief Closes the innermost host frame: the target of rl_indirect for a return. */
RL_RUNTIME_API uint64_t rl_mips_return(rl_ctx *c, uint8_t *mem);
/** @brief Returns RL_PENDING: the target of a transfer that leaves with the pending record set. */
RL_RUNTIME_API uint64_t rl_mips_pending(rl_ctx *c, uint8_t *mem);

/** @brief The host function for guest address target, or NULL when none is mapped. */
static inline rl_fn rl_find(const rl_ctx *c, uint64_t target)
{
    uint64_t i = (target - c->disp_lo) >> RL_MIPS_CODE_SHIFT;
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
    c->pc = target;
    if (c->depth - 1u < RL_MIPS_SHADOW && c->shadow[c->depth - 1u] == target)
        return rl_mips_return;
    return rl_lookup(c, target);
}

/** @brief The lookup of RL_JUMP: rl_indirect, but NULL on a miss. */
static inline rl_fn rl_jump_find(rl_ctx *c, uint64_t target)
{
    c->pc = target;
    if (c->depth - 1u < RL_MIPS_SHADOW && c->shadow[c->depth - 1u] == target)
        return rl_mips_return;
    return rl_find(c, target);
}

/**
 * @brief The miss path of RL_JUMP: the target is in c->pc and the guest
 * address of the transferring instruction in c->pend.pc. Has the lifted
 * signature so the miss stays a tail call.
 */
RL_RUNTIME_API uint64_t rl_dispatch_jump_miss(rl_ctx *c, uint8_t *mem);
#ifdef RL_N64_TRACE
/** @brief Called at every lifted call with its return address when set (a development trace); NULL by default. Exists only in builds that define RL_N64_TRACE. */
RL_RUNTIME_API extern void (*rl_mips_call_trace)(rl_ctx *c, uint64_t ret);
#endif

/** @brief A lifted call that returns to ret opens a host frame. */
static inline void rl_mips_call(rl_ctx *c, uint64_t ret)
{
#ifdef RL_N64_TRACE
    if (RL_UNLIKELY(rl_mips_call_trace != 0))
        rl_mips_call_trace(c, ret);
#endif
    if (c->depth < RL_MIPS_SHADOW)
        c->shadow[c->depth] = ret;
    c->depth++;
}

/**
 * @brief ERET's jump to target (EPC or ErrorEPC, which lifted code has
 * already chosen and cleared EXL or ERL for). A return to the address an
 * open interrupt entry was given closes that entry's host frame; any other
 * target while an interrupt entry is open (an exception handler that
 * switches threads) leaves every host frame through a redirect, so the host
 * stack does not grow with each switch.
 */
RL_RUNTIME_API rl_fn rl_mips_eret_to(rl_ctx *c, uint64_t target);
/** @brief ERET's state change and its cycle: clears ERL (going to ErrorEPC) or else EXL (going to EPC), and LLbit. Returns the target; a frontend that leaves before ERET's lifted effects passes it to rl_mips_eret_to. */
RL_RUNTIME_API uint64_t rl_mips_eret(rl_ctx *c);

/**
 * @brief The intrinsic mips_fcr31(op, fmt, fcr31, a, b): FCR31 after the
 * fast path ran COP1 operation op (the funct field) on format fmt (0x10 S,
 * 0x11 D, 0x14 W, 0x15 L) with source bits a and b: the cause bits cleared,
 * inexact set in cause and flags when the result was rounded. The fast path
 * runs only normal operands to normal results in round-to-nearest with no
 * enable set.
 */
RL_RUNTIME_API uint32_t rl_mips_fcr31(uint32_t op, uint32_t fmt, uint32_t fcr31, uint64_t a, uint64_t b);
/**
 * @brief The intrinsic mips_fcmp(cond, fmt, fcr31, a, b): FCR31 after
 * C.cond.fmt of a and b, the condition bit set or cleared. With a NaN
 * operand it also sets the invalid-operation cause and flag where the
 * VR4300 does (the slow path raises the exception when it is enabled).
 */
RL_RUNTIME_API uint32_t rl_mips_fcmp(uint32_t cond, uint32_t fmt, uint32_t fcr31, uint64_t a, uint64_t b);

/** @brief The intrinsic mips_random(): the CP0 Random register now, counting down from 31 to Wired. */
RL_RUNTIME_API uint64_t rl_mips_random(rl_ctx *c);
/** @brief The intrinsic mips_tlb(op, run): TLBR (0), TLBWI (1), TLBWR (2) or TLBP (3) when run is 1. */
RL_RUNTIME_API void rl_mips_tlb(rl_ctx *c, uint32_t op, uint32_t run);
/** @brief The intrinsic mips_cache(op, vaddr, run): CACHE with operation op at vaddr when run is 1: the cache's tags and its cycles (cache.c), then the cache hook. */
RL_RUNTIME_API void rl_mips_cache(rl_ctx *c, uint32_t op, uint64_t vaddr, uint32_t run);
/** @brief The instruction cache's line fill for a fetch at pc (cache.c): takes its cycles off the budget; nothing outside cached memory. */
RL_RUNTIME_API void rl_mips_icache_fill(rl_ctx *c, uint64_t pc);
/**
 * @brief The data cache's part of a load or store (write 1) at vaddr
 * (cache.c), for any cached address: a hit takes one cycle and marks a
 * stored line dirty; a miss writes a dirty line back, fills the line and
 * takes their cycles. Nothing for kseg1 or an address that does not
 * translate.
 */
RL_RUNTIME_API void rl_mips_dcache_access(rl_ctx *c, uint64_t vaddr, uint32_t write);

/**
 * @brief The intrinsic mips_icache(pc): a fetch at pc, where lifted code may
 * start fetching a line it has not fetched before (the block's entry, a
 * delay slot that starts a line). A line that is not in the instruction
 * cache is filled. The fast path is a kseg0 hit, which costs nothing.
 */
static inline void rl_mips_icache(rl_ctx *c, uint64_t pc)
{
    uint32_t a = (uint32_t)pc;
    if ((a >> 29) == 4u && c->icache[(a >> 5) & (RL_MIPS_ICACHE_LINES - 1u)] == ((a & 0x1FFFF000u) | RL_MIPS_LINE_VALID))
        return;
    rl_mips_icache_fill(c, pc);
}

/**
 * @brief The intrinsic mips_dcache(vaddr, write, run): the data cache's part
 * of a load or store (write 1) at vaddr when run is 1. Only kseg0 is cached
 * here (lifted code leaves through a guard for any other cached address): a
 * hit takes one cycle and marks a stored line dirty, a miss goes to
 * rl_mips_dcache_access. kseg1 takes nothing (a device read takes the
 * guest bus's cycles).
 */
static inline void rl_mips_dcache(rl_ctx *c, uint64_t vaddr, uint32_t write, uint32_t run)
{
    uint32_t a = (uint32_t)vaddr;
    if (!run || (a >> 29) != 4u)
        return;
    uint32_t *line = &c->dcache[(a >> 4) & (RL_MIPS_DCACHE_LINES - 1u)];
    if (RL_UNLIKELY((*line & ~RL_MIPS_LINE_DIRTY) != ((a & 0x1FFFF000u) | RL_MIPS_LINE_VALID))) {
        rl_mips_dcache_access(c, vaddr, write);
        return;
    }
    *line |= write ? RL_MIPS_LINE_DIRTY : 0u;
    c->time.budget -= 1;
}

/**
 * @brief After a lifted write of Status, Cause or Compare (and at any sync
 * point): reads the guest's writes of Count and Wired, checks the timer,
 * updates the scheduler mask and ends the budget when an interrupt can be
 * taken now. Count reads of lifted code see the value this stores.
 */
RL_RUNTIME_API void rl_mips_cp0_sync(rl_ctx *c);
/** @brief Count now (Count advances every second PClock cycle); also stores it in cp0_count. */
RL_RUNTIME_API uint32_t rl_mips_count(rl_ctx *c);

/**
 * @brief The slow path of a guard of the block at pc with code (EXC + ExcCode
 * or FP_SLOW) whose instruction word is w0: runs the instruction the guard
 * left at, which may raise an exception (a TLB-mapped access that hits is
 * done here).
 * Returns RL_OK when the lifted code goes on after the block, or RL_PENDING
 * with a redirect to the exception vector. A guard in a delay slot leaves
 * through rl_mips_guard_exit instead.
 */
RL_RUNTIME_API int rl_mips_guard(rl_ctx *c, uint8_t *mem, uint32_t code, uint64_t pc, uint32_t w0);
/**
 * @brief The guard exit of a block at pc whose delay slot left with code:
 * runs the branch w0 (its link write and its target) and the slot w1, then
 * returns the function the block continues in (the branch target or the
 * next instruction through dispatch, or a function that returns RL_PENDING
 * after an exception entry).
 */
RL_RUNTIME_API rl_fn rl_mips_guard_exit(rl_ctx *c, uint8_t *mem, uint32_t code, uint64_t pc, uint32_t w0,
                                        uint32_t w1);

/**
 * @brief The guest executes an instruction the VR4300 does not define, or
 * one the lifter cannot lift: reports it with the registers and stops the
 * guest. Returns RL_PENDING.
 */
RL_RUNTIME_API int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn);

#endif
