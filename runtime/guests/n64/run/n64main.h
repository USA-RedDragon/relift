/**
 * @file n64main.h
 * @brief The program entry of a lifted Nintendo 64 ROM (program.c, written
 * by guests/n64/guest program.rs).
 */
#ifndef RELIFT_N64_MAIN_H
#define RELIFT_N64_MAIN_H

#include <relift/abi_core.h>

#include "n64.h"
#include "rsp.h"

#include <stddef.h>
#include <stdint.h>

/** @brief What program.c gives rl_n64_main. */
typedef struct rl_n64_program {
    /** @brief The big-endian ROM. */
    const uint8_t *rom;
    size_t rom_len;
    /** @brief The units, one per lifted segment. */
    const rl_unit *const *units;
    uint32_t unit_count;
    /** @brief The code runs the machine binds. */
    const rl_n64_code *code;
    uint32_t code_count;
    /** @brief The lifted RSP images. */
    const rl_rsp_image *const *rsp;
    uint32_t rsp_count;
    /** @brief The title, for reports. */
    const char *title;
    /** @brief The boot variant (6101 to 8303, 0 when unknown). */
    uint32_t cic;
    /** @brief The entry point the boot jumps to. */
    uint32_t entry;
    /** @brief The path of the ROM the project imported, which names the save files (rl_n64_save_path); "" for none. */
    const char *rom_path;
    /** @brief The renderer the program links (NULL: the RDP draws nothing). */
    const rl_n64_renderer *renderer;
    /** @brief The developer's picture defaults, as rl_n64_declare_picture takes them (NULL: the console's). */
    const char *const *picture;
} rl_n64_program;

/**
 * @brief Runs the ROM in a window (the SDL3 backend when built and a display
 * exists) or headless. Options: --headless, --frames N (stop at the start of
 * the Nth VI frame and print the frame count, the hash of the scanned-out
 * framebuffer, its size, the registers and hashes of RDRAM, DMEM and IMEM), --png FILE (the VI picture at exit; --raw-png FILE the raw
 * framebuffer at exit; --shown-png FILE the picture at exit as the window shows it, after the picture
 * options of post.h), --png-dir DIR with --png-every N (every Nth frame),
 * --wav FILE (the sound as 16-bit stereo at RL_N64_AUDIO_RATE, written at the
 * stop; the sound's hash, peak and frame count are on stderr with --frames),
 * --no-audio (open no sound output), --dump PREFIX (PREFIX.rdram as the CPU sees it, .dmem and .imem at the stop; RDRAM when CPU
 * code is missed, IMEM and DMEM when RSP code is),
 * --save BASE (the save files BASE.eep, BASE.sra and BASE.fla instead of
 * the ones rl_n64_save_path names), --no-save, --stats (counters on stderr)
 * and the runtime's --relift-set and --relift-settings.
 */
RL_RUNTIME_API int rl_n64_main(const rl_n64_program *p, int argc, char **argv);

#endif
