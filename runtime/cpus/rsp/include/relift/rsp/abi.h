/**
 * @file relift/rsp/abi.h
 * @brief Runtime ABI contract between relift-emitted C for RSP microcode
 * (the Nintendo 64 Reality Signal Processor, lifted by relift-cpu-rsp
 * through guests/n64/guest rsp.rs) and the RSP layer of the C runtime.
 *
 * Every lifted translation unit of an RSP image includes this header and no
 * other CPU layer's: its struct rl_ctx is the RSP's register file, not the
 * program's main CPU. The core never sees an RSP context: lifted RSP code
 * calls only the entries below, never rl_sync, rl_io_read or the core's
 * dispatch, and the RSP layer runs it (rsp.c). A program links RSP units
 * next to the units of its main CPU; the two kinds of unit name the same
 * types (rl_unit, rl_fn) over different struct rl_ctx definitions, which
 * only ever meet through pointers.
 *
 * - Registers: the slots of cpus/rsp/src/regs.rs in slot order (RL_RSP_SLOTS).
 * - Memory: mem is DMEM, 4 KiB; lifted code masks every address to it, and
 *   the guest's big-endian accesses are swapped by the emitted C.
 * - Addresses: an image is lifted at its own offsets (IMEM address minus
 *   the base it is bound at, modulo 4 KiB), so one image runs wherever the
 *   program loads it. Lifted code holds offsets in c->pc and in its
 *   transfers; c->base is the base of the image now running, which
 *   rl_rsp_base gives the code where an IMEM address is a value (the link
 *   of JAL, JALR, BGEZAL and BLTZAL, and the targets of J, JAL, JR and
 *   JALR, which are absolute).
 * - Control: every transfer is a jump. A transfer the image's function does
 *   not hold goes through rl_indirect to the context's dispatch table, one
 *   slot per IMEM word; a slot the layer has not checked holds rl_rsp_enter,
 *   which binds the code there or stops the program (missed code).
 * - Time: c->time counts RSP cycles; every block ends in a sync point whose
 *   slow path is rl_rsp_sync. A block's cycles are those of the RSP's issue
 *   pipeline from the pipeline its image's code leaves before it; a block
 *   that leaves through dispatch or halts gives the layer its pipeline
 *   (rl_rsp_pipe, rl_rsp_pipe_hold).
 *
 * Any change to a type, layout, signature or constant here bumps
 * RL_ABI_VERSION.
 */
#ifndef RELIFT_RSP_ABI_H
#define RELIFT_RSP_ABI_H

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** @brief Bytes of IMEM and of DMEM. */
#define RL_RSP_MEM_SIZE 0x1000u
/** @brief Dispatch slots: one per IMEM word. */
#define RL_RSP_SLOTS_COUNT (RL_RSP_MEM_SIZE / 4u)

/**
 * @brief The register slots of relift-cpu-rsp in slot order:
 * X(model name, rl_ctx field, IR width in bits). A 1-bit slot is a byte
 * holding 0 or 1.
 */
#define RL_RSP_SLOTS(X)                                                                                                \
    X(r0, r[0], 32)                                                                                                    \
    X(r1, r[1], 32)                                                                                                    \
    X(r2, r[2], 32)                                                                                                    \
    X(r3, r[3], 32)                                                                                                    \
    X(r4, r[4], 32)                                                                                                    \
    X(r5, r[5], 32)                                                                                                    \
    X(r6, r[6], 32)                                                                                                    \
    X(r7, r[7], 32)                                                                                                    \
    X(r8, r[8], 32)                                                                                                    \
    X(r9, r[9], 32)                                                                                                    \
    X(r10, r[10], 32)                                                                                                  \
    X(r11, r[11], 32)                                                                                                  \
    X(r12, r[12], 32)                                                                                                  \
    X(r13, r[13], 32)                                                                                                  \
    X(r14, r[14], 32)                                                                                                  \
    X(r15, r[15], 32)                                                                                                  \
    X(r16, r[16], 32)                                                                                                  \
    X(r17, r[17], 32)                                                                                                  \
    X(r18, r[18], 32)                                                                                                  \
    X(r19, r[19], 32)                                                                                                  \
    X(r20, r[20], 32)                                                                                                  \
    X(r21, r[21], 32)                                                                                                  \
    X(r22, r[22], 32)                                                                                                  \
    X(r23, r[23], 32)                                                                                                  \
    X(r24, r[24], 32)                                                                                                  \
    X(r25, r[25], 32)                                                                                                  \
    X(r26, r[26], 32)                                                                                                  \
    X(r27, r[27], 32)                                                                                                  \
    X(r28, r[28], 32)                                                                                                  \
    X(r29, r[29], 32)                                                                                                  \
    X(r30, r[30], 32)                                                                                                  \
    X(r31, r[31], 32)                                                                                                  \
    X(v0, v[0], 128)                                                                                                   \
    X(v1, v[1], 128)                                                                                                   \
    X(v2, v[2], 128)                                                                                                   \
    X(v3, v[3], 128)                                                                                                   \
    X(v4, v[4], 128)                                                                                                   \
    X(v5, v[5], 128)                                                                                                   \
    X(v6, v[6], 128)                                                                                                   \
    X(v7, v[7], 128)                                                                                                   \
    X(v8, v[8], 128)                                                                                                   \
    X(v9, v[9], 128)                                                                                                   \
    X(v10, v[10], 128)                                                                                                 \
    X(v11, v[11], 128)                                                                                                 \
    X(v12, v[12], 128)                                                                                                 \
    X(v13, v[13], 128)                                                                                                 \
    X(v14, v[14], 128)                                                                                                 \
    X(v15, v[15], 128)                                                                                                 \
    X(v16, v[16], 128)                                                                                                 \
    X(v17, v[17], 128)                                                                                                 \
    X(v18, v[18], 128)                                                                                                 \
    X(v19, v[19], 128)                                                                                                 \
    X(v20, v[20], 128)                                                                                                 \
    X(v21, v[21], 128)                                                                                                 \
    X(v22, v[22], 128)                                                                                                 \
    X(v23, v[23], 128)                                                                                                 \
    X(v24, v[24], 128)                                                                                                 \
    X(v25, v[25], 128)                                                                                                 \
    X(v26, v[26], 128)                                                                                                 \
    X(v27, v[27], 128)                                                                                                 \
    X(v28, v[28], 128)                                                                                                 \
    X(v29, v[29], 128)                                                                                                 \
    X(v30, v[30], 128)                                                                                                 \
    X(v31, v[31], 128)                                                                                                 \
    X(ACCH, acch, 128)                                                                                                 \
    X(ACCM, accm, 128)                                                                                                 \
    X(ACCL, accl, 128)                                                                                                 \
    X(VCOL, vcol, 128)                                                                                                 \
    X(VCOH, vcoh, 128)                                                                                                 \
    X(VCCL, vccl, 128)                                                                                                 \
    X(VCCH, vcch, 128)                                                                                                 \
    X(VCE, vce, 128)                                                                                                   \
    X(DIVIN, divin, 16)                                                                                                \
    X(DIVOUT, divout, 16)                                                                                              \
    X(DIVDP, divdp, 1)

/**
 * @brief The RSP context. The fields from r to divdp are relift-cpu-rsp's
 * register slots (RL_RSP_SLOTS). A vector register holds the RSP's element
 * 0 in its top 16 bits; a flag register holds all ones in the lane of each
 * element whose flag is set. Lifted code also reads and writes pc and time;
 * the rest is the layer's.
 */
struct rl_ctx {
    /** @brief v0 to v31. */
    rl_u128 v[32];
    /** @brief The accumulator's high, middle and low slices. */
    rl_u128 acch, accm, accl;
    /** @brief The flag masks: carry and not-equal, compare and clip, extension. */
    rl_u128 vcol, vcoh, vccl, vcch, vce;
    /** @brief r0 to r31; r0 stays 0. */
    uint32_t r[32];
    /** @brief The divider's input and output halves. */
    uint16_t divin, divout;
    /** @brief The divider's double-precision flag, 0 or 1. */
    uint8_t divdp;
    /** @brief Reserved, zero. */
    uint8_t reserved0[3];
    /** @brief The offset of the next instruction, or of the target of the current transfer, in the running image. */
    uint64_t pc;
    /** @brief The clock in RSP cycles: the RSP is at cycle deadline - budget. */
    rl_time time;
    /** @brief The dispatch table: RL_RSP_SLOTS_COUNT functions, one per IMEM word. */
    rl_fn *disp;
    /** @brief The base of the image each dispatch slot is bound to (0 for an unbound slot). */
    const uint16_t *bases;
    /** @brief The layer's RSP. */
    struct rl_rsp *rsp;
    /** @brief The IMEM address the running image's offset 0 is at. */
    uint32_t base;
    /** @brief Reserved, zero. */
    uint32_t reserved1;
};

#define RL_RSP_SLOT_ASSERT(name, field, bits)                                                                          \
    _Static_assert(sizeof(((rl_ctx *)0)->field) * 8 == ((bits) < 8 ? 8 : (bits)), #name " width");
RL_RSP_SLOTS(RL_RSP_SLOT_ASSERT)
#undef RL_RSP_SLOT_ASSERT

_Static_assert(offsetof(rl_ctx, v) == 0, "v");
_Static_assert(offsetof(rl_ctx, r) == 640, "r");
_Static_assert(offsetof(rl_ctx, pc) == 776, "pc");
_Static_assert(offsetof(rl_ctx, time) == 784, "time");

/** @brief Raw accessors over DMEM as the emitted C uses them: the bytes in host order. */
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
static inline rl_u128 rl_ld128(const uint8_t *mem, uint64_t a)
{
    rl_u128 v;
    memcpy(&v, mem + a, sizeof v);
    return v;
}
static inline void rl_st8(uint8_t *mem, uint64_t a, uint8_t v) { mem[a] = v; }
static inline void rl_st16(uint8_t *mem, uint64_t a, uint16_t v) { memcpy(mem + a, &v, sizeof v); }
static inline void rl_st32(uint8_t *mem, uint64_t a, uint32_t v) { memcpy(mem + a, &v, sizeof v); }
static inline void rl_st64(uint8_t *mem, uint64_t a, uint64_t v) { memcpy(mem + a, &v, sizeof v); }
static inline void rl_st128(uint8_t *mem, uint64_t a, rl_u128 v) { memcpy(mem + a, &v, sizeof v); }

/**
 * @brief The entry of an IMEM word the layer has not checked since IMEM last
 * changed: binds the lifted code whose bytes are in IMEM there now and
 * enters it, or stops the program with a report (missed code). Reads the
 * target from c->pc, an IMEM address (an unbound slot's base is 0).
 */
RL_RUNTIME_API uint64_t rl_rsp_enter(rl_ctx *c, uint8_t *mem);

/**
 * @brief A transfer to offset target of the running image: takes the base
 * of the slot's image, records the target as its offset and returns the
 * slot's function.
 */
static inline rl_fn rl_indirect(rl_ctx *c, uint64_t target)
{
    uint32_t a = ((uint32_t)target + c->base) & 0xFFCu;
    c->base = c->bases[a >> 2];
    c->pc = (a - c->base) & 0xFFCu;
    return c->disp[a >> 2];
}

/**
 * @brief RL_JUMP of the RSP: rl_indirect and a tail call. Every dispatch
 * slot holds a function (an unbound slot's checks IMEM and reports missed
 * code with its own address), so the jump never misses and the site is not
 * needed.
 */
#undef RL_JUMP
#define RL_JUMP(target, site)                                                                           \
    do {                                                                                                \
        (void)(site);                                                                                   \
        RL_MUSTTAIL return rl_indirect(c, (target))(c, mem);                                            \
    } while (0)

/** @brief The intrinsic rsp_base: the IMEM address of the running image's offset 0. */
static inline uint32_t rl_rsp_base(const rl_ctx *c) { return c->base; }

/**
 * @brief The f64 operations of the VRSQ ROM formula (relift-cpu-rsp
 * rsq_rom), on bit patterns: from a signed integer, the correctly rounded
 * square root, and to an integer rounding up. The formula's operands are
 * positive and below 2^53, the only domain rl_rsp_f2i_ceil64 handles.
 */
static inline double rl_rsp_f64(uint64_t b)
{
    double d;
    memcpy(&d, &b, sizeof d);
    return d;
}
static inline uint64_t rl_rsp_b64(double d)
{
    uint64_t b;
    memcpy(&b, &d, sizeof b);
    return b;
}
static inline uint64_t rl_rsp_i2f64(uint64_t a) { return rl_rsp_b64((double)(int64_t)a); }
static inline uint64_t rl_rsp_fsqrt64(uint64_t a) { return rl_rsp_b64(__builtin_sqrt(rl_rsp_f64(a))); }
static inline uint64_t rl_rsp_f2i_ceil64(uint64_t a)
{
    double d = rl_rsp_f64(a);
    int64_t t = (int64_t)d;
    return (uint64_t)(t + ((double)t < d));
}

/**
 * @brief The slow path of a sync point, with the block's register writes
 * done and the clock stored: returns RL_OK to go on to offset next when the
 * slice has cycles left and the RSP still runs, or else stores next's IMEM
 * address in c->pc (and base 0) and returns RL_PENDING, which every lifted
 * frame passes up to the layer's run loop.
 */
RL_RUNTIME_API int rl_rsp_sync(rl_ctx *c, uint8_t *mem, uint64_t next);
/** @brief The intrinsic rsp_mfc0(rd): the SP (rd 0 to 7) or DP (8 to 15) register rd now. */
RL_RUNTIME_API uint32_t rl_rsp_mfc0(rl_ctx *c, uint32_t rd);
/** @brief The intrinsic rsp_mtc0(rd, value, run): writes the register when run is 1, then ends the budget. */
RL_RUNTIME_API void rl_rsp_mtc0(rl_ctx *c, uint32_t rd, uint32_t value, uint32_t run);
/** @brief The intrinsic rsp_break(run): BREAK when run is 1, which halts the RSP at the block's sync point. */
RL_RUNTIME_API void rl_rsp_break(rl_ctx *c, uint32_t run);
/**
 * @brief The intrinsic rsp_pipe(target, a, b, d, taken): the block leaves
 * through dispatch to offset target with the pipeline a, b, d (packed as
 * relift_cpu_rsp::pipe::State::pack), after the stall of a taken branch
 * when taken is 1. Charges what the target's first words cost more (or
 * less) than its block assumes.
 */
RL_RUNTIME_API void rl_rsp_pipe(rl_ctx *c, uint32_t target, uint64_t a, uint64_t b, uint64_t d, uint32_t taken);
/** @brief The intrinsic rsp_pipe_hold(a, b, d): the pipeline at a halt, which the next start charges against. */
RL_RUNTIME_API void rl_rsp_pipe_hold(rl_ctx *c, uint64_t a, uint64_t b, uint64_t d);
/** @brief The intrinsic rsp_tick(n): n cycles, which may be fewer than none. */
RL_RUNTIME_API void rl_rsp_tick(rl_ctx *c, int64_t n);
/** @brief The word at offset pc did not lift: stops the program with a report. */
RL_RUNTIME_API int rl_rsp_undefined(rl_ctx *c, uint8_t *mem, uint64_t pc, uint32_t insn);

#endif
