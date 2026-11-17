/**
 * @file smsmain.h
 * @brief The program entry of a lifted Master System or Game Gear ROM
 * (program.c, written by guests/sms/guest program.rs) and the run of the
 * lifted code on the machine (run.c).
 */
#ifndef RELIFT_SMS_MAIN_H
#define RELIFT_SMS_MAIN_H

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "sms.h"

/**
 * @brief One instruction of a RAM group: its address, its length, the mask of
 * the bytes that must match (bit k for byte k: the opcode and prefix bytes;
 * the code reads its operands when it runs) and the bytes it was lifted from.
 */
typedef struct rl_sms_ram_insn {
    uint16_t addr;
    uint8_t len;
    uint8_t check;
    uint8_t bytes[4];
} rl_sms_ram_insn;

/** @brief Code lifted from bytes in work RAM: the instructions, sorted by address, and the unit that holds them. */
typedef struct rl_sms_ram_group {
    const rl_sms_ram_insn *insns;
    uint32_t count;
    const rl_unit *unit;
} rl_sms_ram_group;

/** @brief Code lifted from one ROM bank as the CPU sees it in one slot (0: 0x0000, 1: 0x4000, 2: 0x8000). */
typedef struct rl_sms_rom_unit {
    uint16_t bank;
    uint8_t slot;
    const rl_unit *unit;
} rl_sms_rom_unit;

/** @brief What program.c gives rl_sms_main. */
typedef struct rl_sms_program {
    const uint8_t *rom;
    size_t rom_len;
    /** @brief The title, for reports and the window. */
    const char *title;
    sms_system system;
    sms_region region;
    sms_mapper mapper;
    /** @brief The unit of the four interrupt entry blocks (RL_Z80_ENTRY plus a kind). */
    const rl_unit *entries;
    const rl_sms_rom_unit *roms;
    uint32_t rom_count;
    const rl_sms_ram_group *rams;
    uint32_t ram_count;
    /** @brief Code fetched from the open bus (0xFF bytes), or NULL. */
    const rl_unit *open;
} rl_sms_program;

/** @brief Counters of a run. */
typedef struct rl_sms_stats {
    uint64_t insns;
    uint64_t irqs;
    uint64_t nmis;
    uint64_t calls;
    uint64_t redirects;
    uint64_t dispatches;
    uint64_t halts;
} rl_sms_stats;

/** @brief Called at each frame edge with the machine and the number of the frame that completed (0 first); nonzero stops the run. */
typedef int rl_sms_frame_hook(void *user, sms *m, uint64_t frame);

/**
 * @brief Creates the machine for p and runs the lifted code from reset on
 * the calling thread until the frame hook asks to stop, the guest reaches
 * code with no lifted body (a stop report) or a fatal error. inputs (one
 * mask per frame, input_count of them) may be NULL. Returns 0 when the hook
 * stopped the run, else the exit code.
 */
RL_RUNTIME_API int rl_sms_run(const rl_sms_program *p, const uint8_t *inputs, size_t input_count, rl_sms_frame_hook *hook,
                              void *user);
/** @brief Makes the next run write its first count instructions to f: each boundary's T-state, pc and registers, interrupt entries and port accesses, in the form of the ares oracle's insn.tsv. */
RL_RUNTIME_API void rl_sms_trace(FILE *f, uint64_t count);
/** @brief Makes the next runs without an input script read the buttons from fn at each frame edge; NULL for none held. */
RL_RUNTIME_API void rl_sms_input(sms_input_fn *fn, void *user);
/** @brief The counters of the current or last run. */
RL_RUNTIME_API const rl_sms_stats *rl_sms_stats_get(void);

/**
 * @brief Runs the ROM headless or in a window. Options: --headless, --frames
 * N (stop after N frames and print the frame hash and memory hashes),
 * --input FILE (one line per frame: the frame number and the mask of
 * SMS_BUTTON_* bits in hex, as the ares oracle's input.tsv), --frame-log
 * FILE (each frame's number, the CRC-32 of its VDP colours as 16-bit
 * little-endian words, width and height), --png FILE (the last frame), --dump-ram FILE
 * (work RAM at the end of --frames N), --state-log FILE (each frame's work RAM,
 * VRAM and CRAM hashes and the VDP position), --insn-log FILE with
 * --insn-count N (rl_sms_trace),
 * --png-dir DIR with --png-every N, --raw-dir DIR with --raw-from A and
 * --raw-to B (the VDP colours of frames A to B as fNNNNNN.bin), --wav FILE
 * (the sound as 44100 Hz stereo WAV), --no-audio, --stats. In a window the
 * sound goes to the host output and the frames are paced on its queue; with
 * --frames or --wav the program prints the sound's length, peak and hash.
 */
RL_RUNTIME_API int rl_sms_main(const rl_sms_program *p, int argc, char **argv);

#endif
