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

/** @brief A run of nop instructions [start, end) in a unit's bank, run as one loop (rl_gbx_add_sled). */
typedef struct rl_gb_sled {
    uint32_t bank;
    uint16_t start;
    uint16_t end;
} rl_gb_sled;

/** @brief An instruction lifted at a RAM address, for the opcode key op (0x100 plus the byte after a CB prefix for those). */
typedef struct rl_gb_ram {
    uint16_t pc;
    uint16_t op;
    rl_fn fn;
} rl_gb_ram;

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
    /** @brief The file of ROM locations run without lifted code, which the next build lifts (--learn); "" for none. */
    const char *learn;
    const rl_gb_sled *sleds;
    uint32_t sled_count;
    /** @brief Instructions lifted at RAM addresses, from the learned file of earlier runs. */
    const rl_gb_ram *ram;
    uint32_t ram_count;
} rl_gb_program;

/**
 * @brief Runs the ROM. Options: --model NAME, --headless, --frames N (stop
 * after N frames and print the frame hash and serial output), --case FILE (a
 * corpus case spec, rlgbd_case_open; exit 0 pass, 1 fail, 3 unsupported),
 * --input SCRIPT (frame[@ly]:buttons,... as a case's input), --png FILE (the
 * last frame at exit), --png-dir DIR with --png-every N (every Nth frame),
 * --wav FILE (the sound as 16-bit stereo at RLGBD_AUDIO_RATE), --no-audio,
 * --save FILE, --no-save, --stats (counters on stderr), --interp-log FILE
 * (every instruction the interpreter ran and every entry into it, with
 * counts), --learn FILE, --no-learn (the ROM locations run without lifted
 * code are added to FILE at exit, for the next build), --check (lockstep
 * against the library's own CPU, stopping at the first difference), and the
 * runtime's --relift-set and --relift-settings.
 */
RL_RUNTIME_API int rl_gb_main(const rl_gb_program *p, int argc, char **argv);

#endif
