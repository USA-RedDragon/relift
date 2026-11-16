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
 * --stats (counters on stderr), and the runtime's --relift-set and
 * --relift-settings. Exits 0 when both CPUs stopped by halting.
 */
RL_RUNTIME_API int rl_nds_main(const rl_nds_program *p, int argc, char **argv);

#endif
