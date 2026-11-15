/**
 * @file gbamain.h
 * @brief The program entry of a lifted Game Boy Advance ROM (program.c,
 * written by guests/gba/guest program.rs).
 */
#ifndef RELIFT_GBA_MAIN_H
#define RELIFT_GBA_MAIN_H

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>

/** @brief What program.c gives rl_gba_main. */
typedef struct rl_gba_program {
    const uint8_t *rom;
    size_t rom_len;
    const rl_unit *const *units;
    uint32_t unit_count;
    /** @brief The title, for reports. */
    const char *title;
    /** @brief The path of the ROM the project imported, which names the save (rl_gba_save_path); "" for none. */
    const char *rom_path;
} rl_gba_program;

/**
 * @brief Runs the ROM headless. Options: --headless (the default until a
 * picture unit draws frames), --frames N (stop at the start of the Nth
 * VBlank and print the frame count, the video memory hash and the
 * registers), --save FILE (the backup file instead of the one
 * rl_gba_save_path names), --no-save, --stats (counters on stderr), and the
 * runtime's --relift-set and --relift-settings. The backup loads at start,
 * is written a second after the game last changed it and at exit.
 */
RL_RUNTIME_API int rl_gba_main(const rl_gba_program *p, int argc, char **argv);

#endif
