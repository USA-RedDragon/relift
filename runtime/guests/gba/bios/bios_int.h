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

/** @brief Event counters per call. */
#define BIOS_EVENTS 20

/**
 * @brief What a call counts for the cycle model in swi.c. The model charges
 * a fixed cost per SWI plus a cost per event; the data accesses are charged
 * separately, by the machine's bus. Slots are shared: a name belongs to the
 * routine in its prefix.
 */
enum bios_event {
    BE_REFUSED = 0,
    BE_ZERO = 6,
    BE_SQRT_SHIFT = 0,
    BE_SQRT_ROUND = 2,
    BE_ATAN2_PATH = 0,
    BE_AFFINE_ITER = 0,
    BE_SET_H_FILL = 1,
    BE_SET_W_COPY = 2,
    BE_SET_W_FILL = 3,
    BE_SET_H_COPY = 5,
    BE_SET_W_CALL = 7,
    BE_SET_H_CALL = 8,
    BE_FAST_COPY = 2,
    BE_FAST_FILL = 3,
    BE_FAST_FILL_START = 4,
    BE_FAST_CALL = 7,
    BE_BIT_BYTE = 1,
    BE_BIT_UNIT = 2,
    BE_BIT_OFFSET = 3,
    BE_BIT_WORD = 4,
    BE_LZ_FLAG = 1,
    BE_LZ_LITERAL = 2,
    BE_LZ_REF = 3,
    BE_LZ_REF_BYTE = 4,
    BE_HUFF_WORD = 1,
    BE_HUFF_BIT = 2,
    BE_HUFF_LEAF = 3,
    BE_HUFF_OUT = 4,
    BE_HUFF_ODD_SIZE = 5,
    BE_RL_COMP_RUN = 2,
    BE_RL_COMP_BYTE = 3,
    BE_RL_RAW_RUN = 4,
    BE_RL_RAW_BYTE = 5,
    BE_RL_ANY_RUN = 19,
    BE_PUT_EVEN = 14,
    BE_PUT_ODD = 15,
    BE_DIFF_ITEM = 1,
    BE_MIDI_CLAMP = 0,
    BE_MIDI_MUL_FP = 1,
    BE_MIDI_MUL_R1 = 3,
    BE_RESET_PALETTE = 2,
    BE_RESET_VRAM = 3,
    BE_RESET_OAM = 4,
    BE_RESET_SIO = 5,
    BE_RESET_SOUND = 6,
    BE_RESET_IO = 7,
    BE_RESET_BLOCK = 9,
    BE_SHIFT_STEP = 16,
    BE_SUB_STEP = 17,
    BE_MUL = 18
};

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
    uint64_t bus_cyc;
    /** @brief Event counts the cycle model charges (see swi.c). */
    uint32_t ev[BIOS_EVENTS];
    /** @brief Accesses are not charged (the model charges their routine as a whole). */
    bool quiet;
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

static inline void bios_ev(bios_call *c, unsigned i, uint32_t n) { c->ev[i] += n; }

static inline uint32_t bios_mul_m(uint32_t rs)
{
    return rs >> 8 == 0 || rs >> 8 == 0xFFFFFFu ? 1 : rs >> 16 == 0 || rs >> 16 == 0xFFFFu ? 2 : rs >> 24 == 0 || rs >> 24 == 0xFFu ? 3 : 4;
}

static inline uint32_t bios_mulu_m(uint32_t rs) { return rs >> 8 == 0 ? 1 : rs >> 16 == 0 ? 2 : rs >> 24 == 0 ? 3 : 4; }

static inline uint32_t bios_mul(bios_call *c, uint32_t a, uint32_t b)
{
    c->ev[BE_MUL] += bios_mul_m(b);
    return a * b;
}

/** @brief The quotient and remainder loop the BIOS's Div and Sqrt share. */
typedef struct bios_udiv {
    uint32_t q, rem;
    unsigned steps, shift_steps;
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
