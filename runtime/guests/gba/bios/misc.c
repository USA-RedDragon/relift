/**
 * @file misc.c
 * @brief RegisterRamReset, IntrWait (and VBlankIntrWait), SoundBias and
 * MidiKey2Freq.
 */
#include "bios_int.h"

static const uint32_t reset_lr[8] = {0, 0, 0x0A99u, 0x0A81u, 0x0A8Du, 0x0A31u, 0x0A63u, 0x0A0Du};

static void reset_region(bios_call *c, uint32_t dst, uint32_t words)
{
    for (uint32_t i = 0; i < words; i++)
        bios_st32(c, dst + 4 * i, 0, i != 0);
    if (!c->quiet)
        bios_ev(c, BE_RESET_BLOCK, words / 8);
}

void bios_register_ram_reset(bios_call *c)
{
    static const uint32_t base[5] = {0x02000000u, 0x03000000u, 0x05000000u, 0x06000000u, 0x07000000u};
    static const uint32_t words[5] = {0x10000u, 0x1F80u, 0x100u, 0x6000u, 0x100u};
    static const unsigned order[8] = {7, 5, 6, 4, 3, 2, 1, 0};
    uint32_t flags = c->r[0];
    bios_save(c, 4, 7, true);
    uint32_t zero = 0;
    bios_push(c, 1, &zero);
    uint32_t zero_at = c->sp;
    int last = -1;
    for (unsigned k = 0; k < 8; k++) {
        unsigned bit = order[k];
        if (!(flags & (1u << bit)))
            continue;
        last = (int)bit;
        bios_ev(c, bit, bit < 5 || !(flags & 0x80u) || bit == 7);
        c->quiet = bit >= 5;
        switch (bit) {
        case 7:
            bios_st16(c, 0x04000000u, 0x0080u, false);
            for (uint32_t a = 0x04000004u; a < 0x04000060u; a += 2)
                bios_st16(c, a, a == 0x04000020u || a == 0x04000026u || a == 0x04000030u || a == 0x04000036u ? 0x100u : 0u,
                          a != 0x04000004u);
            reset_region(c, 0x040000B0u, 0x14);
            reset_region(c, 0x04000100u, 4);
            bios_st16(c, 0x04000130u, 0, false);
            bios_st16(c, 0x04000132u, 0, false);
            bios_st16(c, 0x04000200u, 0, false);
            bios_st16(c, 0x04000202u, 0, false);
            for (uint32_t a = 0x04000204u; a < 0x04000220u; a += 2)
                bios_st16(c, a, 0, a != 0x04000204u);
            break;
        case 6: {
            reset_region(c, 0x04000060u, 0x8);
            bios_st16(c, 0x04000080u, 0, false);
            bios_st16(c, 0x04000082u, 0, false);
            bios_st16(c, 0x04000084u, 0, false);
            bios_st16(c, 0x04000084u, 0x80, false);
            uint32_t bias = bios_ld16(c, 0x04000088u, false);
            bios_st16(c, 0x04000088u, bias & 0x3FFu, false);
            bios_st16(c, 0x04000070u, 0x70, false);
            bios_st16(c, 0x04000084u, 0, false);
            break;
        }
        case 5:
            reset_region(c, 0x04000110u, 0x8);
            reset_region(c, 0x04000140u, 0x8);
            bios_st16(c, 0x04000134u, 0x8000, false);
            bios_st16(c, 0x04000140u, 7, false);
            break;
        default:
            reset_region(c, base[bit], words[bit]);
            break;
        }
    }
    c->quiet = false;
    if (last >= 0) {
        uint32_t inner[8] = {0x04000000u, 0x85000000u, 1u << last, flags, c->hi[8], c->hi[9], c->hi[10], reset_lr[last]};
        bios_push(c, 8, inner);
    }
    c->r[0] = last == 7 ? 0x100u : zero_at;
    c->r[1] = 0x03000000u;
    c->r[3] = RL_GBA_BIOS_SWI_RETURN;
}

/** @brief The flags of a logical result r: N and Z from r, C and V as they were. */
static void logic_flags(bios_call *c, uint32_t r)
{
    bios_set_flags(c, (c->psr & 0x30000000u) | (r & 0x80000000u) | (r ? 0 : 0x40000000u));
}

/**
 * @brief IntrWait's flag check: IME off, the wanted flags taken from the
 * BIOS flags at 0x03007FF8, IME on again. r0 is left with the flags taken,
 * r2 with the BIOS flags as read and r12 with the I/O base; an interrupt
 * handler that runs later in the call finds them in its stack frame (the
 * stub stacks r0 to r3 and r12), so the values are the console's, measured
 * there. The check's cycles pass without interrupts (IME is off for most of
 * them on the console).
 */
static void check(bios_call *c)
{
    bios_flush(c, true);
    bios_point(c, RL_GBA_BIOS_SLIDE);
    c->hi[12] = 0x04000000u;
    bios_st8(c, 0x04000208u, 0, false);
    c->r[2] = bios_ld16(c, 0x03FFFFF8u, false);
    c->r[0] = c->r[1] & c->r[2];
    logic_flags(c, c->r[0]);
    if (c->r[0]) {
        c->r[2] ^= c->r[0];
        bios_st16(c, 0x03FFFFF8u, c->r[2], false);
    }
    bios_st8(c, 0x04000208u, c->hi[4], false);
    bios_tick(c, BIOS_CHECK_CYCLES);
    bios_flush(c, false);
}

/**
 * @brief IntrWait and VBlankIntrWait. The routine stacks r4 and lr on the
 * system stack (bytes a game finds below its stack pointer after the call),
 * checks the flags first when r0 asks to discard old ones, then halts until
 * an interrupt; the interrupt is taken inside the call, at relift's slide,
 * BIOS_CHECK_WAKE cycles after the wake, and its handler is expected to set
 * the BIOS flags. Each check after a handler either returns or halts again.
 * With r0 = 0 the first pass writes no HALTCNT: the console checks before it
 * halts (cycles measured on NanoBoyAdvance as a black box).
 */
void bios_intr_wait(bios_call *c)
{
    uint32_t saved[2] = {c->hi[4], c->lr};
    bios_push(c, 2, saved);
    c->r[3] = 0;
    c->hi[4] = 1;
    bios_set_flags(c, bios_nzcv_sub(c->r[0], 0));
    bool discard = c->r[0] != 0;
    bios_tick(c, discard ? BIOS_WAIT_ENTRY + 2 : BIOS_WAIT_ENTRY);
    if (discard)
        check(c);
    else
        bios_tick(c, BIOS_WAIT_FIRST);
    for (bool first = !discard;; first = false) {
        if (!first) {
            bios_tick(c, BIOS_WAIT_HALT);
            bios_halt(c, c->r[3]);
            bios_tick(c, BIOS_CHECK_WAKE);
            bios_flush(c, false);
        }
        check(c);
        if (c->r[0])
            break;
        bios_tick(c, BIOS_WAIT_AGAIN);
    }
    c->hi[4] = bios_ldm(c, c->sp, false);
    c->lr = bios_ldm(c, c->sp + 4, true);
    c->sp += 8;
    bios_tick(c, BIOS_WAIT_EXIT);
}

void bios_sound_bias(bios_call *c)
{
    uint32_t want = c->r[0] ? 0x200u : 0u;
    uint32_t v = bios_ld16(c, 0x04000088u, false);
    uint32_t level = v & 0x3FEu;
    while (level != want) {
        level = level < want ? level + 2 : level - 2;
        bios_st16(c, 0x04000088u, (v & ~0x3FEu) | level, false);
        bios_tick(c, c->r[1] * 4 + 8);
    }
}

void bios_midi_key2freq(bios_call *c)
{
    bios_save(c, 4, 7, true);
    static const uint8_t scale[180] = {
        0xE0, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xD0, 0xD1, 0xD2, 0xD3, 0xD4, 0xD5,
        0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xDB, 0xC0, 0xC1, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xCB,
        0xB0, 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xBB, 0xA0, 0xA1, 0xA2, 0xA3, 0xA4, 0xA5,
        0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xAB, 0x90, 0x91, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99, 0x9A, 0x9B,
        0x80, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x8B, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75,
        0x76, 0x77, 0x78, 0x79, 0x7A, 0x7B, 0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x6B,
        0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x5B, 0x40, 0x41, 0x42, 0x43, 0x44, 0x45,
        0x46, 0x47, 0x48, 0x49, 0x4A, 0x4B, 0x30, 0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x3B,
        0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x2B, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,
    };
    static const uint32_t freq[12] = {
        0x80000000u, 0x879C7C97u, 0x8FACD61Eu, 0x9837F052u, 0xA14517CCu, 0xAADC0848u,
        0xB504F334u, 0xBFC886BBu, 0xCB2FF52Au, 0xD744FCCBu, 0xE411F03Au, 0xF1A1BF39u,
    };
    uint32_t wave = c->r[0], fp = c->r[2] << 24;
    int32_t mk = (int32_t)c->r[1];
    if (mk > 0xB2) {
        mk = 0xB2;
        fp = 0xFF000000u;
    }
    uint32_t s1 = scale[mk], s2 = scale[mk + 1];
    uint32_t f1 = freq[s1 & 0xFu] >> (s1 >> 4), f2 = freq[s2 & 0xFu] >> (s2 >> 4);
    uint32_t r1 = (uint32_t)(((uint64_t)(f2 - f1) * fp) >> 32) + f1;
    uint32_t wf = bios_ld32(c, wave + 4, false);
    bios_ev(c, BE_MIDI_CLAMP, mk == 0xB2);
    bios_ev(c, BE_MIDI_MUL_FP, bios_mulu_m(fp));
    bios_ev(c, BE_MIDI_MUL_R1, bios_mulu_m(r1));
    c->r[0] = (uint32_t)(((uint64_t)wf * r1) >> 32);
    c->r[1] = r1;
    c->r[3] = RL_GBA_BIOS_SWI_RETURN;
}
