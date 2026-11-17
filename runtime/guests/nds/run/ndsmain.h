/**
 * @file ndsmain.h
 * @brief The program entry of a lifted Nintendo DS cartridge (program.c,
 * written by guests/nds/guest program.rs).
 */
#ifndef RELIFT_NDS_MAIN_H
#define RELIFT_NDS_MAIN_H

#include "nds.h"

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>

/** @brief A lifted function and the bytes its entry checks. */
typedef struct rl_nds_fn {
    /** @brief The entry, with bit 0 set in Thumb state. */
    uint32_t at;
    /** @brief Where its bytes start in the unit's blob. */
    uint32_t offset;
    /** @brief How many bytes, from the entry. */
    uint32_t len;
    /** @brief The host function. */
    rl_fn fn;
} rl_nds_fn;

/** @brief A run of consecutive bytes one function was lifted from. */
typedef struct rl_nds_run_bytes {
    /** @brief The first address. */
    uint32_t at;
    /** @brief Where the bytes start in the unit's blob. */
    uint32_t offset;
    /** @brief How many bytes. */
    uint32_t len;
    /** @brief The function's index in fns. */
    uint32_t function;
} rl_nds_run_bytes;

/** @brief One lifted unit: a static binary or an overlay of one CPU. */
typedef struct rl_nds_unit {
    /** @brief RL_NDS_ARM9 or RL_NDS_ARM7. */
    uint32_t cpu;
    /** @brief The emitted unit. */
    const rl_unit *unit;
    /** @brief The bytes its functions were lifted from. */
    const uint8_t *blob;
    /** @brief Its functions, as the entry checks index them. */
    const rl_nds_fn *fns;
    uint32_t fn_count;
    /** @brief The unit's name, for reports. */
    const char *name;
    /**
     * @brief The cold copy, or NULL: every function again, entered at any
     * instruction (RL_FN_RANGE entries whose pieces switch on c->pc).
     */
    const rl_unit *cold;
    /** @brief With a cold copy, the runs of bytes of each function, which a cold entry compares. */
    const rl_nds_run_bytes *runs;
    uint32_t run_count;
} rl_nds_unit;

/** @brief What program.c gives rl_nds_main. */
typedef struct rl_nds_program {
    /** @brief The cartridge image. */
    const uint8_t *rom;
    size_t rom_len;
    /** @brief The units, each CPU's static binary first. */
    const rl_nds_unit *units;
    uint32_t unit_count;
    /** @brief The title, for reports. */
    const char *title;
    /** @brief Options taken before the command line's (game.toml's [nds]). */
    const char *const *args;
    uint32_t arg_count;
} rl_nds_program;

/**
 * @brief Boots the cartridge without a BIOS image and runs both CPUs
 * headless until each has stopped: a halt nothing can end, or the end of the
 * run. Options: --max-ticks N (stop with a report once a CPU passes tick N),
 * --peek ADDR (print the ARM9's word at ADDR at the end; repeatable),
 * --stats (counters on stderr), --frames N (stop after N frames),
 * --frame-log (one line per frame on stdout: "F <n> tick=<ARM9 tick>
 * h=<frame hash>" and the --peek words in hex), --png FILE (the last frame),
 * --png-dir DIR with --png-every N or --png-frames LIST, --save FILE,
 * --no-save, and the runtime's --relift-set and --relift-settings. Exits 0
 * when both CPUs stopped by halting or the program turned the console off.
 */
RL_RUNTIME_API int rl_nds_main(const rl_nds_program *p, int argc, char **argv);

#endif
