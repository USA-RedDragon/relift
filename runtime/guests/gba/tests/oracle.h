/**
 * @file oracle.h
 * @brief Differential runs against the NanoBoyAdvance oracle
 * (check/oracle/nba, an external process): a scenario is a list of I/O
 * reads and writes with delays, assembled into ARM code in internal work
 * RAM, run on the oracle with its per-instruction cycle trace, then replayed
 * on this layer at the ticks the trace gives. The tests skip when the
 * oracle binary is absent (RELIFT_NBA_ORACLE, else
 * ~/.cache/relift/gba-oracle/bin/nba-oracle).
 */
#ifndef RELIFT_GBA_ORACLE_H
#define RELIFT_GBA_ORACLE_H

#include "mach.h"

/** @brief One step of a scenario. */
typedef struct dop {
    /** @brief 'w' write, 'r' read (compared), 'n' val untimed no-ops, 'l' a delay loop of val turns (4 cycles each), 'm' one no-op whose cycles are kept, 'h' a halt (HALTCNT write). */
    char k;
    uint8_t size;
    uint32_t addr, val;
} dop;

#define DW(sz, a, v) {'w', sz, a, v}
#define DR(sz, a) {'r', sz, a, 0}
#define DN(n) {'n', 0, 0, n}
#define DL(n) {'l', 0, 0, n}
#define DM {'m', 0, 0, 0}
#define DH {'h', 1, 0x04000301u, 0}
#define DEND {0, 0, 0, 0}

/** @brief The outcome of one scenario. */
typedef struct dres {
    /** @brief The oracle is absent (the scenario did not run). */
    bool skipped;
    /** @brief Reads compared, and those that differ. */
    unsigned reads, diffs;
    /** @brief Cycles the oracle's CPU lost in 'm' steps (DMA), and what this layer charged for DMA there. */
    uint64_t stall_oracle, stall_here;
    /** @brief Cycles the oracle stayed halted, and this layer. */
    uint64_t halt_oracle, halt_here;
    /** @brief The oracle failed (protocol error). */
    bool broken;
} dres;

/** @brief What the scenario's machine and the oracle start with besides the program. */
typedef struct dsetup {
    /** @brief Keys held from the start, in KEYINPUT order (set bit = pressed). */
    uint16_t keys;
    /** @brief The oracle's --save type (such as "flash128"), or NULL for none. */
    const char *nba_save;
    /** @brief The file the oracle keeps that save in (needed with nba_save). */
    const char *save_file;
} dsetup;

/** @brief oracle_run with a setup. */
dres oracle_run_cfg(const char *name, const dop *ops, unsigned pad, uint32_t mem_addr, uint32_t mem_len, const dsetup *cfg);

/**
 * @brief Runs ops on the oracle and replays them on a fresh machine; with
 * pad no-ops first, to move every access by pad cycles. Prints each
 * differing read. When mem_len is set, compares mem_len bytes at mem_addr
 * after the run (counted as one read).
 */
dres oracle_run(const char *name, const dop *ops, unsigned pad, uint32_t mem_addr, uint32_t mem_len);

#endif
