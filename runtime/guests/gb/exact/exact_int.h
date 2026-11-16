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
    RL_GBX_TRAP = 2,    /**< a control instruction left the lifted path (STOP, HALT with the HALT bug) */
    RL_GBX_NATIVE = 3,  /**< an instruction ran on the library's CPU at its boundary */
    RL_GBX_DEPTH = 4,   /**< the host frames reached their bound */
    RL_GBX_FORM = 5,    /**< the open instruction runs as its lifted HALT-bug or live form */
    RL_GBX_REDIRECTS = 6
};

/** @brief Why an instruction ran on the library's CPU at its boundary. */
enum rl_gbx_native {
    RL_GBX_HALT_BUG = 0, /**< the fetch after HALT did not advance pc, and no lifted HALT-bug form fits */
    RL_GBX_OAM_BUG = 1,  /**< not run natively: counts lifted INC rr, DEC rr and PUSH on a DMG whose increment unit puts an OAM address on the bus */
    RL_GBX_FETCH = 2,    /**< the fetched opcode is not the ROM byte the code was lifted from */
    RL_GBX_BANK0 = 3,    /**< MBC1 maps another bank at 0x0000 */
    RL_GBX_DMA = 4,      /**< operand bytes fetched while an OAM DMA may meet the bus, so they may not be the ROM's */
    RL_GBX_NATIVES = 5
};

/**
 * @brief Why the run met an instruction with no lifted code; a build without
 * the interpreter (RL_GB_LIFTED_ONLY, a release build) stops there. The
 * first values are the enum rl_gbx_native reasons the instruction would have
 * run on the library's CPU for.
 */
enum rl_gbx_miss {
    RL_GBX_MISS_HALT_BUG = RL_GBX_HALT_BUG, /**< the HALT bug ran an instruction with no lifted HALT-bug form */
    RL_GBX_MISS_FETCH = RL_GBX_FETCH,       /**< the opcode fetched is not the ROM byte the code was lifted from */
    RL_GBX_MISS_BANK0 = RL_GBX_BANK0,       /**< MBC1 maps another bank at 0x0000 */
    RL_GBX_MISS_DMA = RL_GBX_DMA,           /**< an OAM DMA may meet the bus for the instruction's operand bytes */
    RL_GBX_MISS_ROM = RL_GBX_NATIVES,       /**< no lifted code at the ROM address */
    RL_GBX_MISS_RAM,                        /**< no lifted code for the instruction at the RAM address */
    RL_GBX_MISS_CONTROL,                    /**< a lifted control instruction found another opcode */
    RL_GBX_MISSES
};

/** @brief The name of a enum rl_gbx_miss reason, for reports. */
const char *rl_gbx_miss_name(int why);

/** @brief Where interpreted code ran. */
enum rl_gbx_region { RL_GBX_ROM = 0, RL_GBX_RAM = 1, RL_GBX_REGIONS = 2 };

/** @brief Counters of one run. */
typedef struct rl_gbx_stats {
    uint64_t lifted;                       /**< instructions run by lifted code */
    uint64_t traps;                        /**< control instructions lifted code left to the library (EI, DI, HALT, STOP, unused opcodes) */
    uint64_t forms;                        /**< instructions run as their lifted HALT-bug form */
    uint64_t live;                         /**< ROM instructions run as their live form, fetched from a bus the OAM DMA held */
    uint64_t sled;                         /**< of lifted, nop instructions run by a sled loop */
    uint64_t steps;                        /**< boundaries */
    uint64_t irqs;                         /**< interrupt entries */
    uint64_t native[RL_GBX_NATIVES];       /**< instructions run on the library's CPU at their boundary, by reason */
    uint64_t redirects[RL_GBX_REDIRECTS];  /**< unwinds to the run loop, by reason */
    uint64_t interp_entries[RL_GBX_REGIONS]; /**< entries into the interpreter, by where the code is */
    uint64_t interp[RL_GBX_REGIONS];       /**< instructions the interpreter ran, by where the code is */
    uint64_t max_depth;                    /**< most host frames open at once */
} rl_gbx_stats;

/** @brief What a rl_gbx_seen counts: an interpreted instruction, or an entry into the interpreter by its cause. */
enum rl_gbx_seen_kind {
    RL_GBX_SEEN_INSN = 0,     /**< an instruction the interpreter ran */
    RL_GBX_SEEN_DISPATCH = 1, /**< entered from lifted code's transfer to an address with no function */
    RL_GBX_SEEN_IRQ = 2,      /**< entered at an interrupt vector */
    RL_GBX_SEEN_START = 3,    /**< entered at the first instruction */
    RL_GBX_SEEN_RETURNED = 4, /**< entered after the outermost frame returned */
    RL_GBX_SEEN_REDIRECT = 5, /**< entered after an unwind; plus enum rl_gbx_redirect */
    RL_GBX_SEEN_TARGET = 16,  /**< the target of a transfer the interpreter ran */
    RL_GBX_SEEN_RAMOP = 17    /**< an instruction the interpreter ran in RAM; the bank field holds its RAM table key */
};

/** @brief One lifted instruction at a RAM address: op is the opcode, or 0x100 plus the byte after a CB prefix. */
typedef struct rl_gbx_ram_fn {
    uint16_t op;
    rl_fn fn;
} rl_gbx_ram_fn;

/**
 * @brief Registers fn as the lifted instruction at RAM address pc for the
 * opcode key op (see rl_gbx_ram_fn); dispatch there runs it when the opcode
 * fetched is op, and the interpreter otherwise. Returns 0 or -1.
 */
int rl_gbx_add_ram(uint16_t pc, uint16_t op, rl_fn fn);

/**
 * @brief Registers fn as the HALT-bug form of the instruction at pc of the
 * window bank bank (UINT32_MAX for the fixed window): what the boundary runs
 * when the opcode there was fetched with pc left on it. Returns 0 or -1.
 */
int rl_gbx_add_halt_bug(uint32_t bank, uint16_t pc, rl_fn fn);

/**
 * @brief Registers fn as the instruction with opcode op at pc of the window
 * bank bank (UINT32_MAX for the fixed window) lifted with its operands read
 * as it runs, for when the CPU fetched op there from a bus the OAM DMA held
 * or a speed switch left; op plus 0x100 for a fetch that did not move the
 * program counter, lifted at pc - 1 (pc may then be 0x8000, after a HALT or
 * STOP at the window's end). Returns 0 or -1.
 */
int rl_gbx_add_live(uint32_t bank, uint16_t pc, uint16_t op, rl_fn fn);

/** @brief The key of a rl_gbx_seen: kind, the ROM bank mapped in the window (0 elsewhere) and pc. */
#define RL_GBX_SEEN_KEY(kind, bank, pc) ((uint64_t)(kind) << 32 | (uint64_t)(bank) << 16 | (uint64_t)(pc))

/** @brief One entry of the interpreter log; count 0 marks a free slot. */
typedef struct rl_gbx_seen {
    uint64_t key;
    uint64_t count;
    uint64_t insns;   /**< instructions run from this entry until the interpreter left (entries only) */
    uint8_t bytes[3]; /**< the instruction's bytes when first run (instructions only) */
    uint8_t varied;   /**< the bytes differed at a later run */
} rl_gbx_seen;

/** @brief Starts or stops logging every interpreted instruction and every entry into the interpreter. */
void rl_gbx_log_interpreted(bool on);
/** @brief Starts or stops logging the entries into the interpreter and the targets of its transfers only. */
void rl_gbx_log_entries(bool on);
/** @brief The log as a hash table of n slots; free slots have count 0. */
const rl_gbx_seen *rl_gbx_seen_get(size_t *n);

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
/**
 * @brief Registers [start, end) of the window bank bank (UINT32_MAX for the
 * fixed window) as a run of nop instructions: dispatch there runs each one
 * at its own boundary in one loop, where no lifted function is. A run at RAM
 * addresses (bank ignored) is the instruction for opcode 0 there. Call after
 * the units. Returns 0 or -1.
 */
int rl_gbx_add_sled(uint32_t bank, uint16_t start, uint16_t end);
/** @brief Sets the boundary callback. */
void rl_gbx_set_poll(rl_gbx_poll_fn *fn, void *user);
/** @brief Runs the guest from pc with the registers in c until the poll callback ends it; returns its status. */
int rl_gbx_run(rl_ctx *c, uint8_t *mem, uint16_t pc);
/** @brief The counters. */
const rl_gbx_stats *rl_gbx_stats_get(void);
/** @brief Copies the registers of c, with pc and the library's interrupt enable, into r. */
void rl_gbx_regs(const rl_ctx *c, uint16_t pc, rlgbd_regs *r);

#endif
