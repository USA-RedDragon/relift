/**
 * @file bios_int.h
 * @brief What the files of the high-level BIOS share: one call's context, the
 * memory accesses through the machine, and the routines of each file.
 */
#ifndef RELIFT_NDS_BIOS_INT_H
#define RELIFT_NDS_BIOS_INT_H

#include "bios.h"

/** @brief One call in progress. */
typedef struct nds_bios_env {
    /** @brief The machine. */
    rl_nds *m;
    /** @brief RL_NDS_ARM9 or RL_NDS_ARM7. */
    unsigned cpu;
    /** @brief What the CPU layer supplies. */
    const rl_nds_bios_hooks *hooks;
} nds_bios_env;

/** @brief A timed data read of size bytes at addr (aligned down); seq for the later words of a block. */
uint32_t nds_bios_ld(const nds_bios_env *e, uint32_t addr, unsigned size, bool seq);
/** @brief The write twin of nds_bios_ld. */
void nds_bios_st(const nds_bios_env *e, uint32_t addr, unsigned size, uint32_t v, bool seq);
/** @brief cycles of the CPU without a data access. */
void nds_bios_cycles(const nds_bios_env *e, uint64_t cycles);
/**
 * @brief n instructions of the BIOS's own code: their fetch and issue, without
 * their data accesses (the ARM7: a cycle each from the BIOS ROM; the ARM9: a
 * cycle each from its instruction cache, four when the BIOS region is not
 * cached).
 */
void nds_bios_ins(const nds_bios_env *e, unsigned n);
/** @brief A taken branch of the BIOS's code: the pipeline refill after its instruction. */
void nds_bios_jump(const nds_bios_env *e);
/** @brief What a load costs after its access: the ARM7's internal cycle, the ARM9's interlock on the loaded register. */
void nds_bios_ldi(const nds_bios_env *e);

/** @brief The routines whose own work nds_bios_work charges, besides their bus accesses. */
typedef enum nds_bios_kind {
    NDS_W_LZ77,
    NDS_W_LZ77_CB,
    NDS_W_HUFF,
    NDS_W_RL,
    NDS_W_RL_CB,
    NDS_W_CRC16,
    NDS_W_SQRT,
    NDS_W_DIV,
    NDS_W_KINDS
} nds_bios_kind;
/** @brief The cycles of routine k's own code for one call that made units of output (bytes, or 1 for Sqrt and Div). */
void nds_bios_work(const nds_bios_env *e, nds_bios_kind k, uint32_t units);

void nds_bios_cpu_set(const nds_bios_env *e, uint32_t r[4]);
void nds_bios_cpu_fast_set(const nds_bios_env *e, uint32_t r[4]);
void nds_bios_bit_unpack(const nds_bios_env *e, uint32_t r[4]);
void nds_bios_lz77(const nds_bios_env *e, uint32_t r[4], bool callback);
void nds_bios_huff(const nds_bios_env *e, uint32_t r[4]);
void nds_bios_rl(const nds_bios_env *e, uint32_t r[4], bool callback);
void nds_bios_diff8(const nds_bios_env *e, uint32_t r[4]);
void nds_bios_diff16(const nds_bios_env *e, uint32_t r[4]);
void nds_bios_boot_procs(const nds_bios_env *e, uint32_t r[4]);
rl_nds_swi_status nds_bios_custom_halt(const nds_bios_env *e, uint32_t value);

#endif
