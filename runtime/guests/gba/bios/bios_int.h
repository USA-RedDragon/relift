/**
 * @file bios_int.h
 * @brief What the BIOS routines share: one call's context, the bus accesses
 * with the ARM7TDMI's alignment rules and their cycle charge, and the
 * routines themselves.
 */
#ifndef RELIFT_GBA_BIOS_INT_H
#define RELIFT_GBA_BIOS_INT_H

#include "bios.h"

#include <setjmp.h>
#include <stddef.h>

/** @brief The address of the BIOS's source check, which some routines leave in r3. */
#define BIOS_CHECK_ADDR 0x00000BA4u

/** @brief Cycles of the IRQ stub before it stacks the registers. */
#define BIOS_STUB_PRE 2
#define BIOS_STUB_MID 6
#define BIOS_STUB_POST 3
#define BIOS_STUB_RETURN 5
/** @brief How many more cycles the stub takes from running code than from inside a BIOS call. */
#define BIOS_STUB_CODE_EXTRA 1
/** @brief The cycles of the refill after the stub's return that BIOS_STUB_RETURN counts. */
#define BIOS_STUB_RETURN_REFILL 2
/**
 * @brief A SWI's cycles before its routine, besides the stack frames and
 * the comment read, and the first part of them, in which an interrupt that
 * rises waits (the console takes none there).
 */
#define BIOS_ENTRY_CYCLES 22
#define BIOS_ENTRY_MASKED 18
/** @brief A SWI's cycles after its routine, and the last part of them, in which an interrupt waits for the return. */
#define BIOS_EXIT_CYCLES 16
#define BIOS_EXIT_MASKED 10
/**
 * @brief Halt, Stop and CustomHalt: cycles before HALTCNT is written
 * (CustomHalt; Halt and Stop take more), and after the wake.
 */
#define BIOS_HALT_CYCLES 2
#define BIOS_HALT_WAKE 3
/** @brief The first cycles after the wake, in which an interrupt waits (measured as the handler's entry time). */
#define BIOS_HALT_WAKE_MASKED 3
/**
 * @brief The cycles the fitted bases count for the read of the SWI's comment
 * byte and the return's reload of the caller's code at the probe's call sites
 * in IWRAM, where each access takes one cycle; both are now accesses of
 * their own, at the caller's wait states.
 */
#define BIOS_SITE_FIT 3
/**
 * @brief IntrWait's cycles besides its data accesses: before the first pass,
 * the first pass when r0 = 0 (no halt), before each halt, from the wake to
 * the interrupt, each flag check, before the next halt and at the end.
 */
#define BIOS_WAIT_ENTRY 5
#define BIOS_WAIT_FIRST 5
#define BIOS_WAIT_HALT 2
#define BIOS_CHECK_WAKE 3
#define BIOS_CHECK_CYCLES 11
#define BIOS_WAIT_AGAIN 3
#define BIOS_WAIT_EXIT 6

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
    /** @brief The system mode stack pointer and link register as the routine sees them. */
    uint32_t sp, lr;
    /** @brief The supervisor mode stack pointer meanwhile (below the dispatcher's frame inside a SWI). */
    uint32_t sp_svc;
    /** @brief Cycles so far, and the part of them already spent on the machine's clock. */
    uint64_t cyc, spent;
    uint64_t bus_cyc;
    /** @brief Event counts the cycle model charges (see swi.c). */
    uint32_t ev[BIOS_EVENTS];
    /** @brief Accesses are not charged (the model charges their routine as a whole). */
    bool quiet;
    /** @brief The status the routine ends with. */
    rl_gba_swi_status status;
    /** @brief The bus runs the call on the machine's clock (rl_gba_bus.load and the others are set). */
    bool timed;
    /**
     * @brief The CPSR an interrupt taken now saves: system mode (the
     * caller's I bit), the T bit of the code the console runs there and the
     * flags its last flag-setting instruction left.
     */
    uint32_t psr;
    /** @brief The fitted model of the call while its stores are kept for the slide (NULL: none). */
    const struct bios_model *model;
    /** @brief The data accesses noted for the machine's clock (SIZE_MAX: too many to note), and how many were made there. */
    size_t nacc, done;
    /** @brief Interrupts taken inside the call. */
    unsigned irqs;
    /** @brief Where bios_point leaves the call (RL_GBA_POINT_LEFT); NULL outside a SWI. */
    jmp_buf *out;
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

/** @brief Counts n cycles of the call (bios_flush spends them). */
void bios_tick(bios_call *c, unsigned n);
/**
 * @brief On a timed bus, takes a pending interrupt before the instruction at
 * pc unless c->psr masks it: LR_irq = pc + 4, SPSR_irq = c->psr, the
 * registers as c holds them. A handler that changes its return leaves the
 * call.
 */
void bios_point(bios_call *c, uint32_t pc);
/**
 * @brief Spends the cycles counted and not yet spent on the machine's clock:
 * open, as relift's slide (one interrupt check per cycle at
 * RL_GBA_BIOS_SLIDE), else without interrupts.
 */
void bios_flush(bios_call *c, bool open);
/** @brief Flushes (open), then writes value to HALTCNT; on a timed bus the machine stops there until an interrupt. */
void bios_halt(bios_call *c, uint32_t value);
/** @brief Sets the N, Z, C and V bits of c->psr from bits 28-31 of nzcv. */
void bios_set_flags(bios_call *c, uint32_t nzcv);
/** @brief The flags of a - b (CMP, SUBS). */
uint32_t bios_nzcv_sub(uint32_t a, uint32_t b);
/** @brief The flags of a + b (CMN, ADDS). */
uint32_t bios_nzcv_add(uint32_t a, uint32_t b);
/** @brief The flags of a logical result r with carry bit 29 of carry_in (V unchanged reads as 0). */
uint32_t bios_nzcv_logic(uint32_t r, uint32_t carry_in);

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
