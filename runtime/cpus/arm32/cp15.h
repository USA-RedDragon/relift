/**
 * @file cp15.h
 * @brief The ARM946E-S system control coprocessor (CP15) of the ARMv5TE
 * profile: the registers lifted MCR and MRC reach through the arm.cp_write
 * and arm.cp_read intrinsics, the protection unit's regions and the tightly
 * coupled memory regions. The layer keeps the registers and says what a
 * write changed; the guest layer maps the TCMs, picks the exception vector
 * base and waits for interrupts.
 *
 * Register keys are the lifter's: coprocessor bit 0, opc1, CRn, CRm, opc2
 * from bit 14 down (RL_CP15_KEY). Values and masks follow GBATEK's
 * "ARM CP15" chapter and the ARM946E-S manual; none are console-measured
 * yet.
 */
#ifndef RELIFT_ARM32_CP15_H
#define RELIFT_ARM32_CP15_H

#include "core.h"

#include <stdint.h>

/** @brief The lifter's key of CP15 register opc1, CRn, CRm, opc2. */
#define RL_CP15_KEY(opc1, crn, crm, opc2)                                               \
    ((1u << 14) | ((uint32_t)(opc1) << 11) | ((uint32_t)(crn) << 7) | ((uint32_t)(crm) << 3) | \
     (uint32_t)(opc2))

/** @brief Control register bits (c1, c0, 0). */
enum {
    RL_CP15_MPU = 1u << 0,
    RL_CP15_DCACHE = 1u << 2,
    RL_CP15_BIG_ENDIAN = 1u << 7,
    RL_CP15_ICACHE = 1u << 12,
    RL_CP15_HIGH_VECTORS = 1u << 13,
    RL_CP15_ROUND_ROBIN = 1u << 14,
    RL_CP15_PRE_V5 = 1u << 15,
    RL_CP15_DTCM_ON = 1u << 16,
    RL_CP15_DTCM_LOAD = 1u << 17,
    RL_CP15_ITCM_ON = 1u << 18,
    RL_CP15_ITCM_LOAD = 1u << 19,
};

/** @brief What a write changed (rl_arm32_cp15_write's result, a bit set). */
enum {
    /** @brief The TCM regions or their enable or load-mode bits. */
    RL_CP15_CHANGED_TCM = 1u << 0,
    /** @brief The exception vector base. */
    RL_CP15_CHANGED_VECTORS = 1u << 1,
    /** @brief The protection unit: its enable bit, a region, a permission, cacheable or bufferable bits. */
    RL_CP15_CHANGED_MPU = 1u << 2,
    /** @brief Wait for interrupt (c7, c0, 4 or c7, c8, 2). */
    RL_CP15_WAIT = 1u << 3,
    /**
     * @brief A mode the lifted code does not implement: big-endian data or
     * the pre-ARMv5 PC loads (control bits 7 and 15). The bits are kept.
     */
    RL_CP15_UNSUPPORTED = 1u << 4,
    /** @brief Not CP15 (the ARM946E-S has no CP14): the access is undefined. */
    RL_CP15_UNDEFINED = 1u << 5,
};

/** @brief The registers. */
typedef struct rl_arm32_cp15 {
    /** @brief c1, c0, 0. */
    uint32_t control;
    /** @brief c2, c0, 0 and 1: cacheable bits per region, data and instruction. */
    uint32_t dcache_bits, icache_bits;
    /** @brief c3, c0, 0: bufferable bits per region. */
    uint32_t write_buffer;
    /** @brief c5, c0, 2 and 3: access permissions, four bits per region, data and instruction. */
    uint32_t data_perm, insn_perm;
    /** @brief c6, c0 to c7: base, size and enable of the eight regions. */
    uint32_t region[8];
    /** @brief c9, c0, 0 and 1: cache lockdown. */
    uint32_t dcache_lock, icache_lock;
    /** @brief c9, c1, 0 and 1: TCM region registers. */
    uint32_t dtcm, itcm;
    /** @brief c13: trace process identifier. */
    uint32_t trace_id;
} rl_arm32_cp15;

/** @brief Where the TCMs are, from the control and region registers. */
typedef struct rl_arm32_tcm {
    /** @brief ITCM: enabled, load mode, and the size of its virtual area from 0 (the 32 KiB repeat over it). */
    uint8_t itcm_on, itcm_load;
    uint64_t itcm_size;
    /** @brief DTCM: enabled, load mode, base and size of its virtual area (the 16 KiB repeat over it). */
    uint8_t dtcm_on, dtcm_load;
    uint32_t dtcm_base;
    uint64_t dtcm_size;
} rl_arm32_tcm;

/** @brief ITCM and DTCM sizes of the ARM946E-S in the NDS. */
#define RL_CP15_ITCM_SIZE 0x8000u
#define RL_CP15_DTCM_SIZE 0x4000u

/** @brief The reset values. */
RL_RUNTIME_API void rl_arm32_cp15_reset(rl_arm32_cp15 *p);
/**
 * @brief MRC: the register at key. Registers with no state read 0; the ID
 * registers read the ARM946E-S values.
 */
RL_RUNTIME_API uint32_t rl_arm32_cp15_read(const rl_arm32_cp15 *p, uint32_t key);
/** @brief MCR: writes v to the register at key and returns what changed (RL_CP15_CHANGED_..., RL_CP15_WAIT, ...). */
RL_RUNTIME_API unsigned rl_arm32_cp15_write(rl_arm32_cp15 *p, uint32_t key, uint32_t v);
/** @brief The exception vector base: 0 or 0xFFFF0000. */
RL_RUNTIME_API uint32_t rl_arm32_cp15_vectors(const rl_arm32_cp15 *p);
/** @brief The TCM areas. */
RL_RUNTIME_API void rl_arm32_cp15_tcm(const rl_arm32_cp15 *p, rl_arm32_tcm *out);
/**
 * @brief The protection region that holds addr (the highest-numbered enabled
 * region wins), or -1 when none does.
 */
RL_RUNTIME_API int rl_arm32_cp15_region(const rl_arm32_cp15 *p, uint32_t addr);

#endif
