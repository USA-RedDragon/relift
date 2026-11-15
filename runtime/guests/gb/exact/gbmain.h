/**
 * @file gbmain.h
 * @brief The program entry of a lifted Game Boy ROM (program.c, written by
 * guests/gb/guest program.rs).
 */
#ifndef RELIFT_GB_MAIN_H
#define RELIFT_GB_MAIN_H

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>

/** @brief rl_gb_unit.bank of the unit of the fixed window. */
#define RL_GB_FIXED UINT32_MAX

/** @brief One lifted unit and the window bank its functions belong to. */
typedef struct rl_gb_unit {
    const rl_unit *unit;
    uint32_t bank;
} rl_gb_unit;

/** @brief What program.c gives rl_gb_main. */
typedef struct rl_gb_program {
    const uint8_t *rom;
    size_t rom_len;
    const rl_gb_unit *units;
    uint32_t unit_count;
    /** @brief The title, for the window. */
    const char *title;
    /** @brief The hardware unless --model says otherwise (dmg, dmg0, mgb, sgb, sgb2, cgb0, cgbb, cgb, cgbe, agb). */
    const char *model;
    /** @brief The battery save file unless --save says otherwise; "" for none. */
    const char *save;
} rl_gb_program;

/**
 * @brief Runs the ROM. Options: --model NAME, --headless, --frames N (stop
 * after N frames and print the frame hash and serial output), --case FILE (a
 * corpus case spec, rlgbd_case_open; exit 0 pass, 1 fail, 3 unsupported),
 * --input SCRIPT (frame[@ly]:buttons,... as a case's input), --png FILE (the
 * last frame at exit), --png-dir DIR with --png-every N (every Nth frame),
 * --wav FILE (the sound as 16-bit stereo at RLGBD_AUDIO_RATE), --no-audio,
 * --save FILE, --no-save, --stats (counters on stderr), --check (lockstep
 * against the library's own CPU, stopping at the first difference), and the
 * runtime's --relift-set and --relift-settings.
 */
RL_RUNTIME_API int rl_gb_main(const rl_gb_program *p, int argc, char **argv);

#endif
