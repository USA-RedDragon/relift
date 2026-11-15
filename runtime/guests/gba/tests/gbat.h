/**
 * @file gbat.h
 * @brief A small Game Boy Advance memory for the BIOS tests: work RAM,
 * palette, video RAM and object memory as plain arrays, the I/O registers
 * the BIOS touches, open bus elsewhere, and an IRQ line that the tests can
 * raise from a write of IME or HALTCNT.
 */
#ifndef RELIFT_GBA_GBAT_H
#define RELIFT_GBA_GBAT_H

#include "bios/bios.h"
#include "runner.h"

/** @brief The test machine. */
typedef struct gbat {
    uint8_t ewram[0x40000], iwram[0x8000], pal[0x400], vram[0x18000], oam[0x400];
    uint16_t io[0x200];
    rl_gba_bios bios;
    rl_gba_bus bus;
    /** @brief IF bits a HALTCNT write raises (and the handler acknowledges). */
    uint16_t halt_raises;
    /** @brief Number of HALTCNT writes. */
    unsigned halts;
    /** @brief Number of 16-bit or 32-bit writes to 0x040000D4-0x040000DF. */
    unsigned dma_writes;
} gbat;

/** @brief A fresh machine (static storage, one per test process). */
gbat *gbat_new(void);

/** @brief A CPU state for a SWI from ARM code at 0x08000100 in system mode. */
rl_gba_swi_cpu gbat_cpu(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3);

uint32_t gbat_r32(gbat *g, uint32_t a);
uint32_t gbat_r16(gbat *g, uint32_t a);
uint32_t gbat_r8(gbat *g, uint32_t a);
void gbat_w32(gbat *g, uint32_t a, uint32_t v);
void gbat_w16(gbat *g, uint32_t a, uint32_t v);
void gbat_w8(gbat *g, uint32_t a, uint32_t v);

/** @brief Runs SWI n on g with cpu. */
rl_gba_swi_status gbat_swi(gbat *g, unsigned n, rl_gba_swi_cpu *cpu);

#endif
