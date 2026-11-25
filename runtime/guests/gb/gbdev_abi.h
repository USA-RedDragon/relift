/**
 * @file gbdev_abi.h
 * @brief The C ABI of the device library (guests/gb/devices, a Rust static
 * library over rustyboi-core). Hand-written to match src/lib.rs; the
 * lockstep check exercises every function.
 */
#ifndef RELIFT_GBDEV_ABI_H
#define RELIFT_GBDEV_ABI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The devices and bus of one machine; opaque. */
typedef struct rlgbd rlgbd;

/** @brief The CPU registers. */
typedef struct rlgbd_regs {
    uint8_t a, f, b, c, d, e, h, l;
    uint16_t pc, sp;
    uint8_t ime;
} rlgbd_regs;

/** @brief Dots in one frame of the picture unit. */
#define RLGBD_FRAME_DOTS 70224u

/** @brief Joypad bits of rlgbd_set_buttons and of a case's input script; a set bit is a held button. */
#define RLGBD_BTN_A 0x01u
#define RLGBD_BTN_B 0x02u
#define RLGBD_BTN_SELECT 0x04u
#define RLGBD_BTN_START 0x08u
#define RLGBD_BTN_RIGHT 0x10u
#define RLGBD_BTN_LEFT 0x20u
#define RLGBD_BTN_UP 0x40u
#define RLGBD_BTN_DOWN 0x80u

/** @brief Creation flags of rlgbd_new_with. */
#define RLGBD_NEW_BOOT_RESIDUE 1u /**< seed the boot program's residue (the state the SRAM dump references were captured in) */
#define RLGBD_NEW_LAZY_SRAM_CS 2u /**< the cartridge's SRAM chip select is decoded lazily */

/** @brief Verdicts of rlgbd_case_poll. */
#define RLGBD_CASE_CONTINUE 0
#define RLGBD_CASE_PASS 1
#define RLGBD_CASE_FAIL 2
#define RLGBD_CASE_UNSUPPORTED 3 /**< the case cannot run on the external-CPU path */
#define RLGBD_CASE_NEED_REGS 4   /**< the verdict reads the CPU registers: rlgbd_case_regs, then poll again */

/**
 * @brief Hardware codes of rlgbd_new: 0 DMG, 1 CGB-C, 2 CGB-E, 3 AGB, 4 MGB,
 * 5 CGB-B, 6 DMG0, 7 SGB, 8 SGB2, 9 CGB0; anything else CGB-B.
 */
rlgbd *rlgbd_new(uint32_t model, const uint8_t *rom, size_t len);
/** @brief rlgbd_new with creation flags (RLGBD_NEW_*). */
rlgbd *rlgbd_new_with(uint32_t model, const uint8_t *rom, size_t len, uint32_t flags);
void rlgbd_free(rlgbd *h);
/** @brief The instruction boundary: the opcode to run (fetch charged, pc past it), or -1 when the boundary used the step. */
int32_t rlgbd_begin(rlgbd *h);
uint8_t rlgbd_read(rlgbd *h, uint16_t addr);
void rlgbd_write(rlgbd *h, uint16_t addr, uint8_t value);
void rlgbd_internal(rlgbd *h);
void rlgbd_idu(rlgbd *h, uint16_t pre);
void rlgbd_end(rlgbd *h, uint32_t total);
/**
 * @brief Runs the body of the control instruction rlgbd_begin returned (STOP,
 * HALT, DI, EI or an unused opcode, which locks the CPU); the instruction
 * ends. Returns its dots, or -1, with nothing done, for any other opcode.
 */
int32_t rlgbd_control(rlgbd *h, uint8_t op);
/**
 * @brief 1 when a CPU read of addr now may see the OAM DMA rather than memory
 * (the DMA is armed or running and addr is on its source's bus), else 0: 0
 * holds until the next write that starts a DMA.
 */
uint8_t rlgbd_dma_conflict(rlgbd *h, uint16_t addr);
/** @brief Runs the opcode rlgbd_begin returned on the library's CPU interpreter; only in a library built with it. */
uint32_t rlgbd_exec_native(rlgbd *h, uint8_t op);
/** @brief Runs a CB-prefixed instruction rlgbd_begin returned whose second byte cb the caller already read with rlgbd_read; only in a library built with the interpreter. */
uint32_t rlgbd_exec_native_cb(rlgbd *h, uint8_t cb);
/** @brief One whole instruction on the library's CPU interpreter; only in a library built with it. */
uint32_t rlgbd_step_native(rlgbd *h);
void rlgbd_advance(rlgbd *h, uint64_t dots);
uint64_t rlgbd_run_to(rlgbd *h, uint64_t when);
uint64_t rlgbd_now(rlgbd *h);
void rlgbd_get_regs(rlgbd *h, rlgbd_regs *out);
void rlgbd_set_regs(rlgbd *h, const rlgbd_regs *in);
uint8_t rlgbd_peek(rlgbd *h, uint16_t addr);
/**
 * @brief A write for a debugger that charges no dots. Writes video RAM, cartridge RAM, work RAM, OAM and high RAM;
 * returns false, with nothing written, for ROM and mapper registers, the I/O registers and IE.
 */
bool rlgbd_poke(rlgbd *h, uint16_t addr, uint8_t value);
/** @brief FNV-1a of the last taken frame (the bytes of rlgbd_frame_rgb); consumes nothing. */
uint64_t rlgbd_frame_hash(rlgbd *h);
/**
 * @brief FNV-1a of the frame the picture unit presents now, for lockstep checks of
 * machines that never poll frames. It leaves the taken frame alone, but
 * presenting clears the picture unit's frame-ready flag.
 */
uint64_t rlgbd_present_hash(rlgbd *h);
uint64_t rlgbd_state_hash(rlgbd *h);

/**
 * @brief rlgbd_begin after loading pc, sp and ime (0 or 1) into the CPU: for code
 * that keeps its registers elsewhere and calls this at every instruction boundary.
 */
int32_t rlgbd_enter(rlgbd *h, uint16_t pc, uint16_t sp, uint8_t ime);

/**
 * @brief Between instructions: when the picture unit completed an LCD frame since
 * the last poll (the condition run_until_lcd_frame stops on), takes the presented
 * frame into the handle. Returns the number of frames taken so far.
 */
uint32_t rlgbd_frame_poll(rlgbd *h);
/** @brief The 160 x 144 x 3 RGB888 bytes of the last taken frame (zeros before the first). */
const uint8_t *rlgbd_frame_rgb(rlgbd *h);
/** @brief Writes the last taken frame to path as an RGB PNG. Returns 0, or -1. */
int32_t rlgbd_write_png(rlgbd *h, const char *path);
/** @brief Sets the joypad state, RLGBD_BTN_* bits. */
void rlgbd_set_buttons(rlgbd *h, uint8_t mask);
/** @brief A read with no lazy sync and no timing effect (what a test runner polls). */
uint8_t rlgbd_read_memory(rlgbd *h, uint16_t addr);
/** @brief The ROM bank mapped at 0x4000-0x7FFF now. */
uint32_t rlgbd_rom_bank(rlgbd *h);
/** @brief The ROM bank mapped at 0x0000-0x3FFF now (non-zero only for MBC1 in banking mode 1). */
uint32_t rlgbd_rom_bank0(rlgbd *h);
/** @brief 1 while a CGB runs at double speed (KEY1 bit 7), else 0. */
uint8_t rlgbd_double_speed(rlgbd *h);

/**
 * @brief The size of the battery save, a raw .sav file: the cartridge RAM, then the
 * 48-byte clock footer of an MBC3 with a timer. 0 without a battery.
 */
size_t rlgbd_save_size(rlgbd *h);
/** @brief Copies the battery save into out (at most cap bytes); returns the bytes copied. */
size_t rlgbd_save_bytes(rlgbd *h, uint8_t *out, size_t cap);
/** @brief Loads a .sav file: the RAM prefix, then the clock footer when present. 0, or -1. */
int32_t rlgbd_load_save(rlgbd *h, const uint8_t *data, size_t len);

/**
 * @brief Opens a test case before the first instruction. spec is newline-separated
 * key=value lines: protocol (run, mem, mooneye, mooneye_ed, serial, serial_text,
 * blargg_mem, png, png_fixed, png_layout, png_shootout, sram, region_dump,
 * gambatte_hex, gambatte_audio, gambatte_png), frames, cycles, marker, addr,
 * expect, capture (sc_edge|sb_change), pass_text, fail_text (repeatable), ref
 * (repeatable, absolute paths), skip (repeatable, start-end half-open bytes),
 * region (oam|vram), input (frame[@ly]:buttons,...), expect_hex, audible,
 * cart (lazy_sram_cs) and mode (dmg|cgb|agb). Returns 0, or -1 with the reason
 * in rlgbd_case_detail.
 */
int32_t rlgbd_case_open(rlgbd *h, const char *spec);
/**
 * @brief At every instruction boundary, before the instruction at pc runs (repeated
 * boundaries of HALT and stalls included). Returns an RLGBD_CASE_* verdict; a
 * nonzero answer other than NEED_REGS is final.
 */
int32_t rlgbd_case_poll(rlgbd *h, uint16_t pc);
/** @brief The CPU registers (pc and sp ignored) after rlgbd_case_poll returned NEED_REGS. */
void rlgbd_case_regs(rlgbd *h, const rlgbd_regs *regs);
/** @brief The NUL-terminated detail of the verdict (or of a failed open); valid until the next case call. */
const char *rlgbd_case_detail(rlgbd *h);

/** @brief rlgbd_begin after loading pc and sp into the CPU, keeping the interrupt enable the library tracks. */
int32_t rlgbd_at(rlgbd *h, uint16_t pc, uint16_t sp);
/** @brief Applies (nonzero, the default) or leaves out the LCD colour correction of the RGB888 frames. A case that grades the screen against a reference sets the conversion the reference needs. */
void rlgbd_set_color_correction(rlgbd *h, uint8_t on);
/** @brief Sample rate of rlgbd_audio_take, in Hz. */
#define RLGBD_AUDIO_RATE 44100u
/** @brief Starts (nonzero) or stops collecting the sound unit's samples; stopping drops the ones not taken. Off in a new machine. */
void rlgbd_audio_enable(rlgbd *h, uint8_t on);
/** @brief Moves up to cap collected sample frames into out as interleaved left and right floats; returns how many. */
size_t rlgbd_audio_take(rlgbd *h, float *out, size_t cap);

/** @brief The CPU's program counter. */
uint16_t rlgbd_pc(rlgbd *h);
/** @brief The CPU's stack pointer. */
uint16_t rlgbd_sp(rlgbd *h);
/** @brief The number of frames taken so far by rlgbd_frame_poll or a case, without taking one. */
uint32_t rlgbd_frames(rlgbd *h);
/** @brief Sets the interrupt master enable (0 or 1), as RETI does. */
void rlgbd_set_ime(rlgbd *h, uint8_t on);

#endif
