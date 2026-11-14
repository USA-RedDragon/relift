/**
 * @file relift/a64/abi.h
 * @brief Runtime ABI contract between relift-emitted C for ARM64 (A64) guest
 * code and the relift C runtime: the register context, memory access,
 * exclusives, barriers, dispatch, system instructions and floating point.
 *
 * Every lifted translation unit for an A64 guest includes this header and
 * nothing else from the runtime; it includes relift/abi_core.h, the
 * CPU-neutral half. The rationale for each decision is in docs/abi.md. Any
 * change to a type, layout, signature or constant here bumps RL_ABI_VERSION.
 */
#ifndef RELIFT_A64_ABI_H
#define RELIFT_A64_ABI_H

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

#if defined(__x86_64__) && !defined(__GCC_HAVE_SYNC_COMPARE_AND_SWAP_16)
#error "relift/a64/abi.h needs -mcx16 on x86-64 so 16-byte compare-and-swap is lock-free"
#endif

/**
 * @brief CNTFRQ_EL0 value, in Hz. It must equal the tick rate the guest layer
 * gives the core (rl_guest_desc.tick_hz), which scales rl_mrs_cntpct. The
 * default is the rate of the Switch guest's counter, 19.2 MHz; relift defines
 * RL_CNTFRQ before this header for an A64 guest with another rate.
 */
#ifndef RL_CNTFRQ
#define RL_CNTFRQ 19200000u
#endif
/** @brief CTR_EL0 value reported to the guest (Cortex-A57). */
#define RL_CTR_EL0 0x8444C004u
/** @brief DCZID_EL0 value reported to the guest: DC ZVA allowed, 64-byte blocks. */
#define RL_DCZID_EL0 0x4u
/** @brief DC ZVA block size in bytes, from RL_DCZID_EL0. */
#define RL_DC_ZVA_BYTES 64u

/** @brief FPCR.AHP. */
#define RL_FPCR_AHP (1u << 26)
/** @brief FPCR.DN. */
#define RL_FPCR_DN (1u << 25)
/** @brief FPCR.FZ. */
#define RL_FPCR_FZ (1u << 24)
/** @brief FPCR.RMode field. */
#define RL_FPCR_RMODE (3u << 22)
/** @brief FPCR bits that change arithmetic results. */
#define RL_FPCR_MODE (RL_FPCR_AHP | RL_FPCR_DN | RL_FPCR_FZ | RL_FPCR_RMODE)
/**
 * @brief Exact-flags mode: a bit of c->fpcr that is RES0 in the architecture
 * and never reaches the guest or the host FPCR. While it is set, every
 * FPCR-guarded block leaves for rl_fp_slow, which produces FPSR's
 * cumulative flags exactly (docs/abi.md section 10.1).
 */
#define RL_FPCR_EXACT (1u << 31)
/** @brief FPCR bits the guest sees: AHP, DN, FZ, RMode, FZ16 (the Cortex-A57 implements no trap enables). */
#define RL_FPCR_GUEST 0x07C80000u
/** @brief FPSR cumulative exception and saturation bits (IOC DZC OFC UFC IXC IDC QC). */
#define RL_FPSR_MASK 0x0800009Fu

/** @brief FP rounding modes, encoded like FPCR.RMode, plus the A (ties away) mode of FCVTA and FRINTA. */
enum rl_rmode {
    RL_RM_NEAREST = 0,
    RL_RM_POSINF = 1,
    RL_RM_NEGINF = 2,
    RL_RM_ZERO = 3,
    RL_RM_AWAY = 4,
    RL_RM_FPCR = 5
};

/** @brief One SIMD and FP register: 128 bits, viewable as lanes. */
typedef union rl_v128 {
    /** @brief Whole register. */
    _Alignas(16) rl_u128 q;
    /** @brief 64-bit lanes; d[0] is Dn. */
    uint64_t d[2];
    /** @brief 32-bit lanes; s[0] is Sn. */
    uint32_t s[4];
    /** @brief 16-bit lanes; h[0] is Hn. */
    uint16_t h[8];
    /** @brief Byte lanes; b[0] is Bn. */
    uint8_t b[16];
} rl_v128;

/** @brief Local exclusive monitor of one guest thread. */
typedef struct rl_excl {
    /** @brief Guest address marked by the last LDXR/LDAXR/LDXP. */
    uint64_t addr;
    /** @brief Value loaded by the marking load (low 64 bits). */
    uint64_t lo;
    /** @brief High 64 bits of a 128-bit LDXP; 0 otherwise. */
    uint64_t hi;
    /** @brief Access size in bytes (1, 2, 4, 8, 16); 0 when the monitor is open. */
    uint32_t size;
    /** @brief Reserved, zero. */
    uint32_t reserved;
} rl_excl;

/**
 * @brief Guest thread context. One per guest thread, allocated by the runtime.
 *
 * Holds the architectural state at every call boundary (see docs/abi.md,
 * "Locals and the context"). Lifted code keeps registers in C locals between
 * boundaries.
 */
struct rl_ctx {
    /** @brief x0 to x30. */
    uint64_t x[31];
    /** @brief Guest stack pointer. */
    uint64_t sp;
    /** @brief v0 to v31. */
    rl_v128 v[32];
    /** @brief Target guest address of the current indirect call or jump; read by rl_dispatch_miss. */
    uint64_t pc;
    /** @brief NZCV in the MRS NZCV layout (bits 31..28). Stored only where docs/abi.md says. */
    uint32_t nzcv;
    /** @brief FPCR. */
    uint32_t fpcr;
    /** @brief FPSR. */
    uint32_t fpsr;
    /** @brief Reserved, zero. */
    uint32_t reserved0;
    /** @brief TPIDR_EL0 (guest read-write thread pointer). */
    uint64_t tpidr_el0;
    /** @brief TPIDRRO_EL0 (guest address of this thread's 0x200-byte TLS slot). */
    uint64_t tpidrro_el0;
    /** @brief Local exclusive monitor. */
    rl_excl excl;
    /** @brief Pending record, valid while lifted frames return RL_PENDING. First field of the core's block (rl_ctx_common). */
    rl_pending pend;
    /** @brief Runtime thread object. */
    struct rl_thread *thread;
    /** @brief Dispatch table: one entry per 4-byte A64 instruction word from disp_lo; NULL means miss. */
    rl_fn *disp_table;
    /** @brief Lowest guest code address covered by disp_table. */
    uint64_t disp_lo;
    /** @brief Number of entries in disp_table. */
    uint64_t disp_count;
} __attribute__((aligned(64)));

_Static_assert(sizeof(rl_v128) == 16, "rl_v128 size");
_Static_assert(_Alignof(rl_v128) == 16, "rl_v128 alignment");
_Static_assert(sizeof(rl_excl) == 32, "rl_excl size");
_Static_assert(offsetof(rl_ctx, x) == 0, "x");
_Static_assert(offsetof(rl_ctx, sp) == 248, "sp");
_Static_assert(offsetof(rl_ctx, v) == 256, "v");
_Static_assert(offsetof(rl_ctx, pc) == 768, "pc");
_Static_assert(offsetof(rl_ctx, nzcv) == 776, "nzcv");
_Static_assert(offsetof(rl_ctx, fpcr) == 780, "fpcr");
_Static_assert(offsetof(rl_ctx, fpsr) == 784, "fpsr");
_Static_assert(offsetof(rl_ctx, tpidr_el0) == 792, "tpidr_el0");
_Static_assert(offsetof(rl_ctx, tpidrro_el0) == 800, "tpidrro_el0");
_Static_assert(offsetof(rl_ctx, excl) == 808, "excl");
_Static_assert(offsetof(rl_ctx, pend) == 840, "pend");
_Static_assert(offsetof(rl_ctx, thread) == 864, "thread");
_Static_assert(offsetof(rl_ctx, disp_table) == 872, "disp_table");
_Static_assert(offsetof(rl_ctx, disp_lo) == 880, "disp_lo");
_Static_assert(offsetof(rl_ctx, disp_count) == 888, "disp_count");
_Static_assert(sizeof(rl_ctx) == 896, "rl_ctx size");
_Static_assert(_Alignof(rl_ctx) == 64, "rl_ctx alignment");

/** @brief Offset of the core's block (rl_ctx_common) in rl_ctx: pend to disp_count. */
#define RL_A64_COMMON_OFFSET 840u
/** @brief log2 of the dispatch granule: A64 instructions are 4 bytes and 4-byte aligned. */
#define RL_A64_CODE_SHIFT 2u

_Static_assert(offsetof(rl_ctx, pend) == RL_A64_COMMON_OFFSET + offsetof(rl_ctx_common, pend), "common pend");
_Static_assert(offsetof(rl_ctx, thread) == RL_A64_COMMON_OFFSET + offsetof(rl_ctx_common, thread), "common thread");
_Static_assert(offsetof(rl_ctx, disp_table) == RL_A64_COMMON_OFFSET + offsetof(rl_ctx_common, disp_table), "common disp_table");
_Static_assert(offsetof(rl_ctx, disp_lo) == RL_A64_COMMON_OFFSET + offsetof(rl_ctx_common, disp_lo), "common disp_lo");
_Static_assert(offsetof(rl_ctx, disp_count) == RL_A64_COMMON_OFFSET + offsetof(rl_ctx_common, disp_count), "common disp_count");

/** @brief Plain guest load, 1 byte. A64 guest data is little-endian, like every supported host, so plain accesses copy bytes as they are. */
static inline uint8_t rl_ld8(const uint8_t *mem, uint64_t a) { return mem[a]; }
/** @brief Plain guest load, 2 bytes, any alignment. */
static inline uint16_t rl_ld16(const uint8_t *mem, uint64_t a) { uint16_t v; memcpy(&v, mem + a, 2); return v; }
/** @brief Plain guest load, 4 bytes, any alignment. */
static inline uint32_t rl_ld32(const uint8_t *mem, uint64_t a) { uint32_t v; memcpy(&v, mem + a, 4); return v; }
/** @brief Plain guest load, 8 bytes, any alignment. */
static inline uint64_t rl_ld64(const uint8_t *mem, uint64_t a) { uint64_t v; memcpy(&v, mem + a, 8); return v; }
/** @brief Plain guest load, 16 bytes, any alignment. */
static inline rl_u128 rl_ld128(const uint8_t *mem, uint64_t a) { rl_u128 v; memcpy(&v, mem + a, 16); return v; }
/** @brief Plain guest store, 1 byte. */
static inline void rl_st8(uint8_t *mem, uint64_t a, uint8_t v) { mem[a] = v; }
/** @brief Plain guest store, 2 bytes, any alignment. */
static inline void rl_st16(uint8_t *mem, uint64_t a, uint16_t v) { memcpy(mem + a, &v, 2); }
/** @brief Plain guest store, 4 bytes, any alignment. */
static inline void rl_st32(uint8_t *mem, uint64_t a, uint32_t v) { memcpy(mem + a, &v, 4); }
/** @brief Plain guest store, 8 bytes, any alignment. */
static inline void rl_st64(uint8_t *mem, uint64_t a, uint64_t v) { memcpy(mem + a, &v, 8); }
/** @brief Plain guest store, 16 bytes, any alignment. */
static inline void rl_st128(uint8_t *mem, uint64_t a, rl_u128 v) { memcpy(mem + a, &v, 16); }

/** @brief LDAR/LDARB/LDARH: sequentially consistent load of size bytes (1, 2, 4, 8) at a naturally aligned address. */
static inline uint64_t rl_ldar(const uint8_t *mem, uint64_t a, unsigned size)
{
    const void *p = mem + a;
    switch (size) {
    case 1: return __atomic_load_n((const uint8_t *)p, __ATOMIC_SEQ_CST);
    case 2: return __atomic_load_n((const uint16_t *)p, __ATOMIC_SEQ_CST);
    case 4: return __atomic_load_n((const uint32_t *)p, __ATOMIC_SEQ_CST);
    default: return __atomic_load_n((const uint64_t *)p, __ATOMIC_SEQ_CST);
    }
}

/** @brief STLR/STLRB/STLRH: sequentially consistent store of size bytes (1, 2, 4, 8) at a naturally aligned address. */
static inline void rl_stlr(uint8_t *mem, uint64_t a, unsigned size, uint64_t v)
{
    void *p = mem + a;
    switch (size) {
    case 1: __atomic_store_n((uint8_t *)p, (uint8_t)v, __ATOMIC_SEQ_CST); break;
    case 2: __atomic_store_n((uint16_t *)p, (uint16_t)v, __ATOMIC_SEQ_CST); break;
    case 4: __atomic_store_n((uint32_t *)p, (uint32_t)v, __ATOMIC_SEQ_CST); break;
    default: __atomic_store_n((uint64_t *)p, v, __ATOMIC_SEQ_CST); break;
    }
}

/** @brief DMB or DSB with a full (ISH, OSH, NSH, SY) domain and both access types. */
static inline void rl_dmb_full(void) { __atomic_thread_fence(__ATOMIC_SEQ_CST); }
/** @brief DMB or DSB of the LD type (loads before, loads and stores after). */
static inline void rl_dmb_ld(void) { __atomic_thread_fence(__ATOMIC_ACQUIRE); }
/** @brief DMB or DSB of the ST type (stores before, stores after); lowered to a release fence, which is stronger. */
static inline void rl_dmb_st(void) { __atomic_thread_fence(__ATOMIC_RELEASE); }
/** @brief ISB: compiler barrier only; guest self-modifying code is not supported. */
static inline void rl_isb(void) { __atomic_signal_fence(__ATOMIC_SEQ_CST); }

/** @brief DC ZVA: zero the RL_DC_ZVA_BYTES block that contains guest address a. */
static inline void rl_dc_zva(uint8_t *mem, uint64_t a) { memset(mem + (a & ~(uint64_t)(RL_DC_ZVA_BYTES - 1)), 0, RL_DC_ZVA_BYTES); }
/** @brief DC CVAC, CVAU, CIVAC, CVAP and IC IVAU: no effect on a cache-coherent host without guest code generation. */
static inline void rl_cache_maint(uint64_t a) { (void)a; rl_isb(); }

/** @brief Default load or store order selector for the exclusive helpers: no ordering (LDXR, STXR). */
#define RL_ORD_PLAIN 0
/** @brief Acquire (LDAXR) or release (STLXR); lowered to sequentially consistent, see docs/abi.md. */
#define RL_ORD_SC 1

/** @brief LDXR/LDAXR (size 1, 2, 4, 8): load and mark the monitor. */
static inline uint64_t rl_ldx(rl_excl *m, const uint8_t *mem, uint64_t a, unsigned size, int ord)
{
    const void *p = mem + a;
    int o = ord ? __ATOMIC_SEQ_CST : __ATOMIC_RELAXED;
    uint64_t v;
    switch (size) {
    case 1: v = __atomic_load_n((const uint8_t *)p, o); break;
    case 2: v = __atomic_load_n((const uint16_t *)p, o); break;
    case 4: v = __atomic_load_n((const uint32_t *)p, o); break;
    default: v = __atomic_load_n((const uint64_t *)p, o); break;
    }
    m->addr = a;
    m->lo = v;
    m->hi = 0;
    m->size = size;
    return v;
}

/** @brief STXR/STLXR (size 1, 2, 4, 8): store if the monitor holds a and the value is unchanged. Returns the status register value (0 success, 1 fail) and opens the monitor. */
static inline uint32_t rl_stx(rl_excl *m, uint8_t *mem, uint64_t a, unsigned size, uint64_t v, int ord)
{
    void *p = mem + a;
    int o = ord ? __ATOMIC_SEQ_CST : __ATOMIC_RELAXED;
    uint32_t held = m->size == size && m->addr == a;
    uint64_t e = m->lo;
    m->size = 0;
    if (!held)
        return 1;
    switch (size) {
    case 1: { uint8_t x = (uint8_t)e; return !__atomic_compare_exchange_n((uint8_t *)p, &x, (uint8_t)v, 0, o, __ATOMIC_RELAXED); }
    case 2: { uint16_t x = (uint16_t)e; return !__atomic_compare_exchange_n((uint16_t *)p, &x, (uint16_t)v, 0, o, __ATOMIC_RELAXED); }
    case 4: { uint32_t x = (uint32_t)e; return !__atomic_compare_exchange_n((uint32_t *)p, &x, (uint32_t)v, 0, o, __ATOMIC_RELAXED); }
    default: return !__atomic_compare_exchange_n((uint64_t *)p, &e, v, 0, o, __ATOMIC_RELAXED);
    }
}

/** @brief LDXP/LDAXP with 64-bit registers: loads both halves, marks a 16-byte reservation, returns the low half and stores the high half in *hi. */
static inline uint64_t rl_ldxp64(rl_excl *m, const uint8_t *mem, uint64_t a, uint64_t *hi, int ord)
{
    const uint64_t *p = (const uint64_t *)(const void *)(mem + a);
    int o = ord ? __ATOMIC_SEQ_CST : __ATOMIC_RELAXED;
    uint64_t lo = __atomic_load_n(p, o);
    uint64_t h = __atomic_load_n(p + 1, __ATOMIC_RELAXED);
    m->addr = a;
    m->lo = lo;
    m->hi = h;
    m->size = 16;
    *hi = h;
    return lo;
}

/** @brief STXP/STLXP with 64-bit registers: 16-byte compare-and-swap against the marked pair. Needs -mcx16 on x86-64. */
static inline uint32_t rl_stxp64(rl_excl *m, uint8_t *mem, uint64_t a, uint64_t lo, uint64_t hi, int ord)
{
    rl_u128 *p = (rl_u128 *)(void *)(mem + a);
    int o = ord ? __ATOMIC_SEQ_CST : __ATOMIC_RELAXED;
    uint32_t held = m->size == 16 && m->addr == a;
    rl_u128 e = ((rl_u128)m->hi << 64) | m->lo;
    m->size = 0;
    if (!held)
        return 1;
    return !__atomic_compare_exchange_n(p, &e, ((rl_u128)hi << 64) | lo, 0, o, __ATOMIC_RELAXED);
}

/** @brief LDXP/LDAXP with 32-bit registers is rl_ldx with size 8 (lanes split by the caller); STXP 32-bit is rl_stx with size 8. */
#define RL_LDXP32_SIZE 8u

/** @brief CLREX, and the monitor clear on every runtime entry that models an exception return (svc). */
static inline void rl_clrex(rl_excl *m) { m->size = 0; }

/** @brief Dispatch miss handler. Reads the target from c->pc. Has the lifted signature so a miss is a call in place. */
RL_RUNTIME_API int rl_dispatch_miss(rl_ctx *c, uint8_t *mem);

/** @brief Looks up the host function for guest address target; returns rl_dispatch_miss on a miss or a misaligned target. */
static inline rl_fn rl_lookup(const rl_ctx *c, uint64_t target)
{
    uint64_t off = target - c->disp_lo;
    uint64_t i = (off >> RL_A64_CODE_SHIFT) | (off << (64 - RL_A64_CODE_SHIFT));
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
    c->pc = target;
    return rl_lookup(c, target);
}

/** @brief Register values installed by an unwinder or longjmp: the guest unwinder's install step converts its own layout to this. */
typedef struct rl_resume_regs {
    /** @brief x0 to x30. */
    uint64_t x[31];
    /** @brief Target guest sp (the CFA-derived sp of the frame that owns the landing pad). */
    uint64_t sp;
    /** @brief Landing pad or resume guest pc. */
    uint64_t pc;
    /** @brief d8 to d15 (low 64 bits of v8 to v15). */
    uint64_t d8_15[8];
} rl_resume_regs;

/** @brief Install r into c, set an RL_PEND_UNWIND record and return RL_PENDING. Replaces the guest unwinder's register-install step and guest longjmp. */
RL_RUNTIME_API int rl_resume(rl_ctx *c, uint8_t *mem, const rl_resume_regs *r);

/**
 * @brief Where a guest libc's jmp_buf keeps the registers setjmp saves:
 * x19 to x30 as 12 consecutive 8-byte slots, sp, and d8 to d15 as 8
 * consecutive slots (docs/abi.md section 7.1).
 */
typedef struct rl_a64_jmp_layout {
    /** @brief Byte offset of x19; x20 to x30 follow. */
    uint32_t x19;
    /** @brief Byte offset of sp. */
    uint32_t sp;
    /** @brief Byte offset of d8; d9 to d15 follow. */
    uint32_t d8;
    /** @brief Bytes of the jmp_buf the layout uses. */
    uint32_t size;
} rl_a64_jmp_layout;

/** @brief The newlib (devkitA64, libnx) layout: x19 at 0, sp at 96, d8 at 112, 176 bytes. The default. */
RL_RUNTIME_API extern const rl_a64_jmp_layout rl_a64_jmp_newlib;
/** @brief Sets the layout rl_a64_setjmp and rl_a64_longjmp use (the guest layer, from the guest's libc); NULL restores the default. */
RL_RUNTIME_API void rl_a64_set_jmp_layout(const rl_a64_jmp_layout *l);
/** @brief The layout in use. */
RL_RUNTIME_API const rl_a64_jmp_layout *rl_a64_jmp_layout_current(void);
/**
 * @brief Override of guest setjmp (`RL_FN_OVERRIDE`): saves x19 to x30, sp and
 * d8 to d15 into the jmp_buf at guest address x0 and returns with x0 = 0. The
 * caller's call site is a resume point at the return address (section 7.1).
 */
RL_RUNTIME_API int rl_a64_setjmp(rl_ctx *c, uint8_t *mem);
/**
 * @brief Override of guest longjmp (`RL_FN_OVERRIDE | RL_FN_MAY_UNWIND`): reads
 * the jmp_buf at guest address x0 and calls rl_resume with pc = the saved x30,
 * the saved sp and x0 = w1, or 1 when w1 is 0. Returns RL_PENDING.
 */
RL_RUNTIME_API int rl_a64_longjmp(rl_ctx *c, uint8_t *mem);
/** @brief rl_a64_setjmp with an explicit layout and jmp_buf address. */
RL_RUNTIME_API int rl_a64_setjmp_at(rl_ctx *c, uint8_t *mem, const rl_a64_jmp_layout *l, uint64_t buf);
/** @brief rl_a64_longjmp with an explicit layout, jmp_buf address and value. */
RL_RUNTIME_API int rl_a64_longjmp_at(rl_ctx *c, uint8_t *mem, const rl_a64_jmp_layout *l, uint64_t buf, uint32_t val);
/** @brief SVC #imm, implemented by the guest layer (the guest OS's system call). Arguments and results in c->x and c->v; clears the monitor. Returns RL_PENDING for thread or process exit. */
RL_RUNTIME_API int rl_svc(rl_ctx *c, uint8_t *mem, uint32_t imm);
/** @brief UDF, BRK, HLT, an instruction relift cannot lift, or MSR to a read-only register. Reports a guest crash; always returns RL_PENDING with RL_PEND_EXIT_PROCESS. */
RL_RUNTIME_API int rl_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn);

/** @brief MRS CNTPCT_EL0 and CNTVCT_EL0: host monotonic clock in RL_CNTFRQ ticks. */
RL_RUNTIME_API uint64_t rl_mrs_cntpct(rl_ctx *c);
/** @brief MSR FPCR. Stores the value; reports in dev builds when it selects a mode the lifted code was not generated for. */
RL_RUNTIME_API void rl_msr_fpcr(rl_ctx *c, uint64_t v);
/** @brief WFE: block briefly or yield the host thread. A spurious wake is allowed by the architecture. */
RL_RUNTIME_API void rl_wfe(rl_ctx *c);

/** @brief MRS TPIDR_EL0. */
static inline uint64_t rl_mrs_tpidr_el0(const rl_ctx *c) { return c->tpidr_el0; }
/** @brief MSR TPIDR_EL0. */
static inline void rl_msr_tpidr_el0(rl_ctx *c, uint64_t v) { c->tpidr_el0 = v; }
/** @brief MRS TPIDRRO_EL0. MSR to it is UNDEFINED at EL0 and goes to rl_undefined. */
static inline uint64_t rl_mrs_tpidrro_el0(const rl_ctx *c) { return c->tpidrro_el0; }
/** @brief MRS CNTFRQ_EL0. */
static inline uint64_t rl_mrs_cntfrq_el0(void) { return RL_CNTFRQ; }
/** @brief MRS CTR_EL0. */
static inline uint64_t rl_mrs_ctr_el0(void) { return RL_CTR_EL0; }
/** @brief MRS DCZID_EL0. */
static inline uint64_t rl_mrs_dczid_el0(void) { return RL_DCZID_EL0; }
/** @brief MRS FPCR. */
static inline uint64_t rl_mrs_fpcr(const rl_ctx *c) { return c->fpcr & RL_FPCR_GUEST; }
/** @brief MRS FPSR. */
static inline uint64_t rl_mrs_fpsr(const rl_ctx *c) { return c->fpsr; }
/** @brief MSR FPSR. */
static inline void rl_msr_fpsr(rl_ctx *c, uint64_t v) { c->fpsr = (uint32_t)v & RL_FPSR_MASK; }
/** @brief YIELD: host spin-wait hint. */
static inline void rl_yield(void)
{
#if defined(__x86_64__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    __asm__ volatile("yield" ::: "memory");
#endif
    __atomic_signal_fence(__ATOMIC_SEQ_CST);
}
/** @brief SEV and SEVL: no effect, because rl_wfe never sleeps longer than its bounded wait. */
static inline void rl_sev(void) { __atomic_signal_fence(__ATOMIC_SEQ_CST); }

/**
 * @brief FP cut layer, exact slow paths. Values are IEEE bit patterns.
 *
 * fpcr is the generation-time FPCR of the calling function. fpsr is NULL in
 * the default build; in the fp-exact build it points to the FPSR local and the
 * cumulative bits are ORed in.
 */
RL_RUNTIME_API uint32_t rl_fp32_add(uint32_t a, uint32_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPSub, single. */
RL_RUNTIME_API uint32_t rl_fp32_sub(uint32_t a, uint32_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPMul, single. */
RL_RUNTIME_API uint32_t rl_fp32_mul(uint32_t a, uint32_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPDiv, single. */
RL_RUNTIME_API uint32_t rl_fp32_div(uint32_t a, uint32_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPMulAdd, single: addend + a * b with one rounding. */
RL_RUNTIME_API uint32_t rl_fp32_muladd(uint32_t addend, uint32_t a, uint32_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPSqrt, single. */
RL_RUNTIME_API uint32_t rl_fp32_sqrt(uint32_t a, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPRoundInt, single (FRINT*): rmode from enum rl_rmode; exact set for FRINTX. */
RL_RUNTIME_API uint32_t rl_fp32_round_int(uint32_t a, uint32_t rmode, int exact, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPRSqrtEstimate, single (FRSQRTE). */
RL_RUNTIME_API uint32_t rl_fp32_rsqrt_est(uint32_t a, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPAdd, double. */
RL_RUNTIME_API uint64_t rl_fp64_add(uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPSub, double. */
RL_RUNTIME_API uint64_t rl_fp64_sub(uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPMul, double. */
RL_RUNTIME_API uint64_t rl_fp64_mul(uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPDiv, double. */
RL_RUNTIME_API uint64_t rl_fp64_div(uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPMulAdd, double. */
RL_RUNTIME_API uint64_t rl_fp64_muladd(uint64_t addend, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPSqrt, double. */
RL_RUNTIME_API uint64_t rl_fp64_sqrt(uint64_t a, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPRoundInt, double. */
RL_RUNTIME_API uint64_t rl_fp64_round_int(uint64_t a, uint32_t rmode, int exact, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPRSqrtEstimate, double. */
RL_RUNTIME_API uint64_t rl_fp64_rsqrt_est(uint64_t a, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPConvert between 16, 32 and 64-bit formats (FCVT). */
RL_RUNTIME_API uint64_t rl_fp_convert(uint64_t a, unsigned from_bits, unsigned to_bits, uint32_t rmode, uint32_t fpcr, uint32_t *fpsr);
/** @brief FPToFixed (FCVT[NPMZA][SU], fixed-point FCVTZ[SU]): fbits fraction bits, saturating, NaN gives 0. */
RL_RUNTIME_API uint64_t rl_fp_to_fixed(uint64_t a, unsigned from_bits, unsigned to_bits, unsigned fbits, int is_unsigned, uint32_t rmode, uint32_t fpcr, uint32_t *fpsr);
/** @brief FixedToFP (SCVTF, UCVTF, fixed-point forms). */
RL_RUNTIME_API uint64_t rl_fixed_to_fp(uint64_t a, unsigned from_bits, unsigned to_bits, unsigned fbits, int is_unsigned, uint32_t rmode, uint32_t fpcr, uint32_t *fpsr);

/*
 * FPSR flags on host hardware (docs/abi.md section 10.1). The lifted
 * functions the build's flags-live analysis marks are compiled with
 * RL_FP_FLAGS and strict FP semantics: host FP operations raise the host's
 * sticky flags, fp.c deposits its flags there too, and MRS FPSR ORs them,
 * mapped to Arm bits, into the FPSR slot. Other code never reads them.
 */

/** @brief 1 in lifted code of a program that produces FPSR flags (the home of the a64 frontend's FP_FLAGS slot). */
#if defined(RL_FP_FLAGS)
#define RL_FP_FLAGS_ON 1
#else
#define RL_FP_FLAGS_ON 0
#endif

/** @brief The host's sticky FP flags as FPSR bits IOC, DZC, OFC, UFC, IXC (and IDC on arm64). */
static inline uint32_t rl_fp_host_flags(void)
{
#if defined(__x86_64__)
    unsigned m = _mm_getcsr();
    return (m & 1u) | ((m >> 1) & 0x1Eu);
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, fpsr" : "=r"(v));
    return (uint32_t)v & 0x9Fu;
#else
    return 0;
#endif
}

/** @brief Clears the host's sticky FP flags. */
static inline void rl_fp_host_clear(void)
{
#if defined(__x86_64__)
    _mm_setcsr(_mm_getcsr() & ~0x3Fu);
#elif defined(__aarch64__)
    __asm__ volatile("msr fpsr, xzr" ::: "memory");
#endif
}

/** @brief ORs FPSR bits into the host's sticky FP flags (IDC only on arm64). */
static inline void rl_fp_host_raise(uint32_t f)
{
#if defined(__x86_64__)
    _mm_setcsr(_mm_getcsr() | (f & 1u) | ((f & 0x1Eu) << 1));
#elif defined(__aarch64__)
    uint64_t v;
    __asm__ volatile("mrs %0, fpsr" : "=r"(v));
    v |= f & 0x9Fu;
    __asm__ volatile("msr fpsr, %0" ::"r"(v) : "memory");
#else
    (void)f;
#endif
}

/** @brief MRS FPSR: the slot value with the host's flags in flag-producing programs. */
static inline uint32_t rl_mrs_fpsr_flags(uint32_t slot)
{
#if defined(RL_FP_FLAGS)
    return slot | rl_fp_host_flags();
#else
    return slot;
#endif
}

/**
 * @brief After MSR FPSR (the slot takes the value): clears the host's flags.
 * Also in code built without RL_FP_FLAGS, because a write there may start
 * the region of a read in flag-producing code; clearing flags nothing reads
 * changes nothing.
 */
static inline void rl_msr_fpsr_flags(void) { rl_fp_host_clear(); }

/** @brief Raises FPSR flags an IR-built FP operation computed (compares, min/max, estimates); free without RL_FP_FLAGS. */
static inline void rl_fp_raise(uint32_t f)
{
#if defined(RL_FP_FLAGS)
    if (f)
        rl_fp_host_raise(f);
#else
    (void)f;
#endif
}

/** @brief True when the host's native IEEE operation gives the guest result for this FPCR, except possibly for NaN bits. */
static inline int rl_fp_fast(uint32_t fpcr, const uint32_t *fpsr)
{
#if defined(__aarch64__)
    (void)fpcr;
    return fpsr == 0;
#else
    return fpsr == 0 && (fpcr & RL_FPCR_MODE) == 0;
#endif
}

/** @brief True when the native NaN result must be replaced by the slow path (x86-64 NaN rules differ from Arm). */
static inline int rl_fp_nan_fixup(void)
{
#if defined(__aarch64__)
    return 0;
#else
    return 1;
#endif
}

/**
 * @brief True when a host result needs the slow path for its flags: x86
 * detects tininess after rounding and Arm before, so a result of exactly the
 * smallest normal magnitude may lack UFC. Only in flag-producing programs.
 */
static inline int rl_fp_tiny_fixup(uint64_t u, unsigned bits)
{
#if defined(RL_FP_FLAGS) && !defined(__aarch64__)
    uint64_t mag = bits == 32 ? (u & 0x7FFFFFFFu) : (u & 0x7FFFFFFFFFFFFFFFull);
    return RL_UNLIKELY(mag == (bits == 32 ? 0x00800000u : 0x0010000000000000ull));
#else
    (void)u;
    (void)bits;
    return 0;
#endif
}

#define RL_FP_BINOP(name, slow, T, U, expr)                                             \
    static inline U name(U a, U b, uint32_t fpcr, uint32_t *fpsr)                      \
    {                                                                                  \
        if (!rl_fp_fast(fpcr, fpsr))                                                   \
            return slow(a, b, fpcr, fpsr);                                             \
        T x, y, r;                                                                     \
        U u;                                                                           \
        memcpy(&x, &a, sizeof x);                                                      \
        memcpy(&y, &b, sizeof y);                                                      \
        r = (expr);                                                                    \
        if (rl_fp_nan_fixup() && RL_UNLIKELY(r != r))                                  \
            return slow(a, b, fpcr, fpsr);                                             \
        memcpy(&u, &r, sizeof u);                                                      \
        if (rl_fp_tiny_fixup((uint64_t)u, sizeof(U) * 8))                              \
            return slow(a, b, fpcr, fpsr);                                             \
        return u;                                                                      \
    }

/** @brief FADD single, inline fast path. */
RL_FP_BINOP(rl_fadd32, rl_fp32_add, float, uint32_t, x + y)
/** @brief FSUB single, inline fast path. */
RL_FP_BINOP(rl_fsub32, rl_fp32_sub, float, uint32_t, x - y)
/** @brief FMUL single, inline fast path. */
RL_FP_BINOP(rl_fmul32, rl_fp32_mul, float, uint32_t, x * y)
/** @brief FDIV single, inline fast path. */
RL_FP_BINOP(rl_fdiv32, rl_fp32_div, float, uint32_t, x / y)
/** @brief FADD double, inline fast path. */
RL_FP_BINOP(rl_fadd64, rl_fp64_add, double, uint64_t, x + y)
/** @brief FSUB double, inline fast path. */
RL_FP_BINOP(rl_fsub64, rl_fp64_sub, double, uint64_t, x - y)
/** @brief FMUL double, inline fast path. */
RL_FP_BINOP(rl_fmul64, rl_fp64_mul, double, uint64_t, x * y)
/** @brief FDIV double, inline fast path. */
RL_FP_BINOP(rl_fdiv64, rl_fp64_div, double, uint64_t, x / y)

#undef RL_FP_BINOP

/** @brief FMADD/FMLA lane single, inline fast path: addend + a * b with one rounding. */
static inline uint32_t rl_fmuladd32(uint32_t addend, uint32_t a, uint32_t b, uint32_t fpcr, uint32_t *fpsr)
{
    if (!rl_fp_fast(fpcr, fpsr))
        return rl_fp32_muladd(addend, a, b, fpcr, fpsr);
    float z, x, y, r;
    uint32_t u;
    memcpy(&z, &addend, 4);
    memcpy(&x, &a, 4);
    memcpy(&y, &b, 4);
    r = __builtin_fmaf(x, y, z);
    if (rl_fp_nan_fixup() && RL_UNLIKELY(r != r))
        return rl_fp32_muladd(addend, a, b, fpcr, fpsr);
    memcpy(&u, &r, 4);
    return u;
}

/** @brief FMADD/FMLA lane double, inline fast path. */
static inline uint64_t rl_fmuladd64(uint64_t addend, uint64_t a, uint64_t b, uint32_t fpcr, uint32_t *fpsr)
{
    if (!rl_fp_fast(fpcr, fpsr))
        return rl_fp64_muladd(addend, a, b, fpcr, fpsr);
    double z, x, y, r;
    uint64_t u;
    memcpy(&z, &addend, 8);
    memcpy(&x, &a, 8);
    memcpy(&y, &b, 8);
    r = __builtin_fma(x, y, z);
    if (rl_fp_nan_fixup() && RL_UNLIKELY(r != r))
        return rl_fp64_muladd(addend, a, b, fpcr, fpsr);
    memcpy(&u, &r, 8);
    return u;
}

/** @brief FSQRT single, inline fast path. */
static inline uint32_t rl_fsqrt32(uint32_t a, uint32_t fpcr, uint32_t *fpsr)
{
    if (!rl_fp_fast(fpcr, fpsr))
        return rl_fp32_sqrt(a, fpcr, fpsr);
    float x, r;
    uint32_t u;
    memcpy(&x, &a, 4);
    r = __builtin_sqrtf(x);
    if (rl_fp_nan_fixup() && RL_UNLIKELY(r != r))
        return rl_fp32_sqrt(a, fpcr, fpsr);
    memcpy(&u, &r, 4);
    return u;
}

/** @brief FSQRT double, inline fast path. */
static inline uint64_t rl_fsqrt64(uint64_t a, uint32_t fpcr, uint32_t *fpsr)
{
    if (!rl_fp_fast(fpcr, fpsr))
        return rl_fp64_sqrt(a, fpcr, fpsr);
    double x, r;
    uint64_t u;
    memcpy(&x, &a, 8);
    r = __builtin_sqrt(x);
    if (rl_fp_nan_fixup() && RL_UNLIKELY(r != r))
        return rl_fp64_sqrt(a, fpcr, fpsr);
    memcpy(&u, &r, 8);
    return u;
}

#endif
