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
 * @brief Runs the ROM in a window (the SDL3 backend when built and a display
 * exists) or headless. Options: --headless, --frames N (stop at the end of
 * the Nth frame and print the frame count, the frame hash as
 * rl_gba_frame_hash gives it, the registers and memory hashes), --png FILE
 * (the last frame at exit), --png-dir DIR with --png-every N (every Nth
 * frame), --save FILE (the backup file instead of the one rl_gba_save_path
 * names), --no-save, --stats (counters on stderr), and the runtime's
 * --relift-set and --relift-settings. The backup loads at start, is written
 * a second after the game last changed it and at exit.
 */
RL_RUNTIME_API int rl_gba_main(const rl_gba_program *p, int argc, char **argv);

#endif
