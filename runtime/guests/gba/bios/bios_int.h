/**
 * @file bios_int.h
 * @brief What the BIOS routines share: one call's context, the bus accesses
 * with the ARM7TDMI's alignment rules and their cycle charge, and the
 * routines themselves.
 */
#ifndef RELIFT_GBA_BIOS_INT_H
#define RELIFT_GBA_BIOS_INT_H

#include "bios.h"

/** @brief The address of the BIOS's source check, which some routines leave in r3. */
#define BIOS_CHECK_ADDR 0x00000BA4u

/** @brief One call in progress. */
typedef struct bios_call {
    /** @brief The BIOS state. */
    rl_gba_bios *b;
    /** @brief The memory. */
    const rl_gba_bus *bus;
    /** @brief r0 to r3 as the routine leaves them. */
    uint32_t r[4];
    /** @brief r4 to r12 as the routine sees them (r11 and r12 are the dispatcher's). */
    uint32_t hi[13];
    /** @brief The system mode stack pointer as the routine sees it. */
    uint32_t sp;
    /** @brief Cycles so far. */
    uint64_t cyc;
    /** @brief The status the routine ends with. */
    rl_gba_swi_status status;
} bios_call;

uint32_t bios_ld32(bios_call *c, uint32_t a, bool seq);
uint32_t bios_ldm(bios_call *c, uint32_t a, bool seq);
uint32_t bios_ld16(bios_call *c, uint32_t a, bool seq);
uint32_t bios_ld16s(bios_call *c, uint32_t a, bool seq);
uint32_t bios_ld8(bios_call *c, uint32_t a, bool seq);
void bios_st32(bios_call *c, uint32_t a, uint32_t v, bool seq);
void bios_st16(bios_call *c, uint32_t a, uint32_t v, bool seq);
void bios_st8(bios_call *c, uint32_t a, uint32_t v, bool seq);
void bios_push(bios_call *c, unsigned n, const uint32_t *v);
void bios_save(bios_call *c, unsigned first, unsigned last, bool lr);

static inline void bios_tick(bios_call *c, unsigned n) { c->cyc += n; }

/** @brief The quotient and remainder loop the BIOS's Div and Sqrt share. */
typedef struct bios_udiv {
    uint32_t q, rem;
    unsigned steps;
    bool hang;
} bios_udiv;

bios_udiv bios_udiv_run(uint32_t n, uint32_t d, uint32_t q0);

void bios_div(bios_call *c);
void bios_div_arm(bios_call *c);
void bios_sqrt(bios_call *c);
void bios_arctan(bios_call *c);
void bios_arctan2(bios_call *c);
void bios_bg_affine(bios_call *c);
void bios_obj_affine(bios_call *c);
void bios_cpu_set(bios_call *c);
void bios_cpu_fast_set(bios_call *c);
void bios_bit_unpack(bios_call *c);
void bios_lz77(bios_call *c, bool vram);
void bios_huff(bios_call *c);
void bios_rl(bios_call *c, bool vram);
void bios_diff8(bios_call *c, bool vram);
void bios_diff16(bios_call *c);
void bios_midi_key2freq(bios_call *c);
void bios_register_ram_reset(bios_call *c);
void bios_intr_wait(bios_call *c);
void bios_sound_bias(bios_call *c);

extern const int16_t bios_sin[256];

#endif
