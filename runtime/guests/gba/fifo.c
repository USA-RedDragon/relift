/**
 * @file fifo.c
 * @brief The two sound FIFOs of the built-in stand-in for the sound unit
 * (GBATEK "GBA Sound Channel A and B - DMA Sound"), as far as they time the
 * machine: the sound DMA requests. The bytes are not played; a sound unit
 * plugged into the slot replaces all of this.
 *
 * FIFO A (0x040000A0) and FIFO B (0x040000A4) queue words; every write
 * there queues one word, whatever its width (a narrower write replaces its
 * bytes of the word in the next slot). GBATEK gives the queue 8 words; the
 * NanoBoyAdvance oracle's holds 7 and a write to a full queue empties it,
 * which this follows until a console probe decides. With the
 * master enable (SOUNDCNT_X bit 7) on, an overflow of the timer SOUNDCNT_H
 * selects for a FIFO (bit 10 for A, bit 14 for B) first requests its sound
 * DMA (rl_gba_dma_fifo) when 3 words or fewer are queued, then moves the
 * next word to the output when the 4 bytes of the last one are used, and
 * uses one byte. Bits 11 and 15 of SOUNDCNT_H empty FIFO A and B.
 */
#include "gba_int.h"

static void fifo_clear(gba_fifo *f)
{
    f->rd = 0;
    f->wr = 0;
    f->count = 0;
    for (unsigned i = 0; i < GBA_FIFO_WORDS; i++)
        f->q[i] = 0;
}

void gba_fifo_reset(rl_gba *g)
{
    for (unsigned i = 0; i < 2; i++) {
        fifo_clear(&g->fifo[i]);
        g->fifo[i].out = 0;
        g->fifo[i].out_n = 0;
    }
}

void gba_fifo_write(rl_gba *g, uint32_t off, unsigned size, uint32_t value)
{
    gba_fifo *f = &g->fifo[(off >> 2) & 1u];
    unsigned shift = 8u * (off & 3u);
    uint32_t mask = size >= 4 ? 0xFFFFFFFFu : ((1u << (8u * size)) - 1u) << shift;
    uint32_t w = (f->q[f->wr] & ~mask) | ((value << shift) & mask);
    if (f->count == GBA_FIFO_WORDS) {
        fifo_clear(f);
        return;
    }
    f->q[f->wr] = w;
    f->wr = (uint8_t)((f->wr + 1) % GBA_FIFO_WORDS);
    f->count++;
}

void gba_fifo_control(rl_gba *g, uint8_t hi)
{
    for (unsigned i = 0; i < 2; i++)
        if (hi & (0x08u << (4 * i)))
            fifo_clear(&g->fifo[i]);
}

void gba_fifo_timer(rl_gba *g, unsigned timer)
{
    if (g->slot[RL_GBA_SLOT_APU] != &g->stub[RL_GBA_SLOT_APU].dev || !(g->regs[0x84] & 0x80))
        return;
    for (unsigned i = 0; i < 2; i++) {
        if (((g->regs[0x83] >> (2 + 4 * i)) & 1u) != timer)
            continue;
        gba_fifo *f = &g->fifo[i];
        if (f->count <= 3)
            rl_gba_dma_fifo(g, i);
        if (!f->out_n && f->count) {
            f->out = f->q[f->rd];
            f->rd = (uint8_t)((f->rd + 1) % GBA_FIFO_WORDS);
            f->count--;
            f->out_n = 4;
        }
        if (f->out_n) {
            f->out >>= 8;
            f->out_n--;
        }
    }
}
