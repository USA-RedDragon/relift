/**
 * @file nds_int.h
 * @brief The machine record and the helpers the files of this layer share.
 */
#ifndef RELIFT_NDS_INT_H
#define RELIFT_NDS_INT_H

#include "nds.h"

/** @brief A tick that never comes. */
#define NDS_NEVER UINT64_MAX

/** @brief Bytes of each CPU's I/O register file stub (0x04000000 to 0x04001FFF). */
#define NDS_REGS 0x2000u

/** @brief What an address decodes to. */
typedef enum nds_kind {
    /** @brief Nothing: reads 0, writes are dropped. */
    NDS_K_NONE,
    /** @brief Memory at nds_where.p. */
    NDS_K_MEM,
    /** @brief The CPU's I/O registers. */
    NDS_K_IO,
    /** @brief A BIOS region: a high-level stub with no image, reads 0. */
    NDS_K_BIOS,
    /** @brief The GBA slot with no cartridge (open bus). */
    NDS_K_SLOT,
} nds_kind;

/** @brief The decode of one address. */
typedef struct nds_where {
    /** @brief The byte at the aligned address, for NDS_K_MEM; NULL for writes that are dropped. */
    uint8_t *p;
    /** @brief The kind. */
    uint8_t kind;
    /** @brief True for main RAM (the contended region). */
    bool main;
    /** @brief True for an ARM9 TCM hit. */
    bool tcm;
} nds_where;

/** @brief A device of this layer. */
typedef struct nds_dev {
    rl_device dev;
    rl_nds *m;
} nds_dev;

/** @brief The machine. */
struct rl_nds {
    uint8_t main[RL_NDS_MAIN_SIZE];
    uint8_t swram[RL_NDS_SWRAM_SIZE];
    uint8_t wram7[RL_NDS_WRAM7_SIZE];
    uint8_t itcm[RL_CP15_ITCM_SIZE];
    uint8_t dtcm[RL_CP15_DTCM_SIZE];
    uint8_t pal[RL_NDS_PAL_SIZE];
    uint8_t oam[RL_NDS_OAM_SIZE];
    uint8_t vram[RL_NDS_VRAM_SIZE];
    /** @brief Register file stubs of the registers no device models yet, per CPU. */
    uint8_t regs[2][NDS_REGS];

    rl_sched *sched;
    rl_sched_cpu *cpu[2];

    rl_arm32_cp15 cp15;
    rl_arm32_tcm tcm;

    uint8_t wramcnt;
    /** @brief EXMEMCNT of the ARM9 and the ARM7's own bits 0 to 6. */
    uint16_t exmem[2];
    uint8_t postflg[2];
    uint8_t vramcnt[10];
    /** @brief The ARM7 wrote HALTCNT: the store ends in a halt. */
    bool halt_req;

    /** @brief The tick from which main RAM is free. */
    uint64_t ram_free;
    rl_nds_costs costs;

    nds_dev d_io;
};

/** @brief The low size bytes set (size 1, 2 or 4). */
static inline uint32_t nds_mask(unsigned size) { return size >= 4 ? UINT32_MAX : (1u << (8 * size)) - 1u; }

/** @brief A little-endian load of size bytes at p. */
static inline uint32_t nds_ld(const uint8_t *p, unsigned size)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

/** @brief A little-endian store of size bytes of v at p. */
static inline void nds_st(uint8_t *p, unsigned size, uint32_t v)
{
    for (unsigned i = 0; i < size; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/** @brief The tick of cpu now (0 before rl_nds_attach). */
static inline uint64_t nds_now(const rl_nds *m, unsigned cpu) { return m->cpu[cpu] ? rl_sched_now(m->cpu[cpu]) : 0; }
/** @brief Charges ticks to cpu (an ARM7 charge is rounded up to whole cycles). */
void nds_charge(rl_nds *m, unsigned cpu, uint64_t ticks);

/** @brief Decodes addr (aligned) for cpu; code true for an instruction fetch, write for a store. */
nds_where nds_decode(rl_nds *m, unsigned cpu, uint32_t addr, bool code, bool write);
/** @brief Rebuilds the GBA slot rows of the cost table from EXMEMCNT. */
void nds_costs_rebuild(rl_nds *m);
/** @brief The open bus value of the GBA slot for cpu at addr. */
uint32_t nds_slot_read(const rl_nds *m, unsigned cpu, uint32_t addr, unsigned size);

/** @brief An I/O read of size bytes at addr of cpu at tick now (any side effect included). */
uint32_t nds_io_read(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint64_t now);
/** @brief An I/O write. */
void nds_io_write(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value, uint64_t now);
/** @brief Power-on values of the I/O registers. */
void nds_io_reset(rl_nds *m);
/** @brief Adds the I/O device to the scheduler and maps both CPUs' I/O. */
int nds_io_attach(rl_nds *m);

/** @brief The scheduler address of cpu's I/O address addr: the ARM7's registers sit 4 GiB above the ARM9's. */
static inline uint64_t nds_io_key(unsigned cpu, uint32_t addr) { return (uint64_t)cpu << 32 | addr; }

#endif
