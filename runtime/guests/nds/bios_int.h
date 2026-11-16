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
