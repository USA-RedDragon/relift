/**
 * @file exact_int.h
 * @brief The exact Game Boy mode inside the layer: one machine per process,
 * driven from lifted code (relift/gb/exact.h) and from gbmain.c.
 */
#ifndef RELIFT_GB_EXACT_INT_H
#define RELIFT_GB_EXACT_INT_H

#include "core.h"
#include "gbdev_abi.h"
#include <relift/gb/exact.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Why lifted frames were unwound to the run loop. */
enum rl_gbx_redirect {
    RL_GBX_RET = 0,     /**< a return went elsewhere than its call returns to */
    RL_GBX_BANK = 1,    /**< a direct transfer into the window found another bank mapped */
    RL_GBX_TRAP = 2,    /**< an instruction of the library's CPU left the lifted path (STOP) */
    RL_GBX_NATIVE = 3,  /**< an instruction ran on the library's CPU at its boundary */
    RL_GBX_DEPTH = 4,   /**< the host frames reached their bound */
    RL_GBX_REDIRECTS = 5
};

/** @brief Why an instruction ran on the library's CPU at its boundary. */
enum rl_gbx_native {
    RL_GBX_HALT_BUG = 0, /**< the fetch after HALT did not advance pc */
    RL_GBX_OAM_BUG = 1,  /**< not run natively: counts lifted INC rr, DEC rr and PUSH on a DMG whose increment unit puts an OAM address on the bus */
    RL_GBX_FETCH = 2,    /**< the fetched opcode is not the ROM byte the code was lifted from */
    RL_GBX_BANK0 = 3,    /**< MBC1 maps another bank at 0x0000 */
    RL_GBX_NATIVES = 4
};

/** @brief Where interpreted code ran. */
enum rl_gbx_region { RL_GBX_ROM = 0, RL_GBX_RAM = 1, RL_GBX_REGIONS = 2 };

/** @brief Counters of one run. */
typedef struct rl_gbx_stats {
    uint64_t lifted;                       /**< instructions run by lifted code */
    uint64_t traps;                        /**< instructions lifted code left to the library's CPU (EI, DI, HALT, STOP) */
    uint64_t steps;                        /**< boundaries */
    uint64_t irqs;                         /**< interrupt entries */
    uint64_t native[RL_GBX_NATIVES];       /**< instructions run on the library's CPU at their boundary, by reason */
    uint64_t redirects[RL_GBX_REDIRECTS];  /**< unwinds to the run loop, by reason */
    uint64_t interp_entries[RL_GBX_REGIONS]; /**< entries into the interpreter, by where the code is */
    uint64_t interp[RL_GBX_REGIONS];       /**< instructions the interpreter ran, by where the code is */
    uint64_t max_depth;                    /**< most host frames open at once */
} rl_gbx_stats;

/**
 * @brief Called before every boundary step with the pc of the instruction
 * about to begin and the registers in c. Returns 0 to go on, else the exit
 * status that ends the run.
 */
typedef int rl_gbx_poll_fn(void *user, rl_ctx *c, uint16_t pc);

/** @brief The machine the lifted code runs on: binds h, the ROM and the units, fills the dispatch table. Returns 0 or -1. */
int rl_gbx_init(rlgbd *h, const uint8_t *rom, size_t rom_len, bool dmg, bool mbc1);
/** @brief Registers a unit of the window bank bank (UINT32_MAX for the fixed window). Returns 0 or -1. */
int rl_gbx_add_unit(const rl_unit *u, uint32_t bank);
/** @brief Sets the boundary callback. */
void rl_gbx_set_poll(rl_gbx_poll_fn *fn, void *user);
/** @brief Runs the guest from pc with the registers in c until the poll callback ends it; returns its status. */
int rl_gbx_run(rl_ctx *c, uint8_t *mem, uint16_t pc);
/** @brief The counters. */
const rl_gbx_stats *rl_gbx_stats_get(void);
/** @brief Copies the registers of c, with pc and the library's interrupt enable, into r. */
void rl_gbx_regs(const rl_ctx *c, uint16_t pc, rlgbd_regs *r);

#endif
