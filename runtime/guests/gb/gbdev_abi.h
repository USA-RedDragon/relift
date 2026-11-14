/**
 * @file gbdev_abi.h
 * @brief The C ABI of the device library (guests/gb/devices, a Rust static
 * library over rustyboi-core). Hand-written to match src/lib.rs; the
 * lockstep check exercises every function.
 */
#ifndef RELIFT_GBDEV_ABI_H
#define RELIFT_GBDEV_ABI_H

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

/** @brief Hardware codes of rlgbd_new: 0 DMG, 1 CGB-C, 2 CGB-E, 3 AGB, 4 MGB, else CGB-B. */
rlgbd *rlgbd_new(uint32_t model, const uint8_t *rom, size_t len);
void rlgbd_free(rlgbd *h);
/** @brief The instruction boundary: the opcode to run (fetch charged, pc past it), or -1 when the boundary used the step. */
int32_t rlgbd_begin(rlgbd *h);
uint8_t rlgbd_read(rlgbd *h, uint16_t addr);
void rlgbd_write(rlgbd *h, uint16_t addr, uint8_t value);
void rlgbd_internal(rlgbd *h);
void rlgbd_idu(rlgbd *h, uint16_t pre);
void rlgbd_end(rlgbd *h, uint32_t total);
uint32_t rlgbd_exec_native(rlgbd *h, uint8_t op);
uint32_t rlgbd_step_native(rlgbd *h);
void rlgbd_advance(rlgbd *h, uint64_t dots);
uint64_t rlgbd_run_to(rlgbd *h, uint64_t when);
uint64_t rlgbd_now(rlgbd *h);
void rlgbd_get_regs(rlgbd *h, rlgbd_regs *out);
void rlgbd_set_regs(rlgbd *h, const rlgbd_regs *in);
uint8_t rlgbd_peek(rlgbd *h, uint16_t addr);
uint64_t rlgbd_frame_hash(rlgbd *h);
uint64_t rlgbd_state_hash(rlgbd *h);

#endif
