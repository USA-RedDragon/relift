/**
 * @file mach.h
 * @brief A machine on a scheduler with one CPU for the device tests, and
 * helpers that move the CPU's clock as instructions would.
 */
#ifndef RELIFT_GBA_MACH_H
#define RELIFT_GBA_MACH_H

#include "gba.h"
#include "runner.h"

#include <stdlib.h>

/** @brief The machine under test. */
typedef struct mach {
    rl_gba *g;
    rl_sched *s;
    rl_sched_cpu *c;
    uint8_t *rom;
    size_t rom_len;
} mach;

/** @brief A ROM of len bytes whose halfword at offset o holds o / 2 + 0x1000 (low 16 bits). */
static inline uint8_t *mach_rom(size_t len)
{
    uint8_t *r = malloc(len);
    for (size_t o = 0; o + 1 < len; o += 2) {
        uint16_t v = (uint16_t)(o / 2 + 0x1000);
        r[o] = (uint8_t)v;
        r[o + 1] = (uint8_t)(v >> 8);
    }
    return r;
}

/** @brief A fresh machine with rom (len bytes, taken over) and the backup of cart (its rom fields are replaced) at tick 0. */
static inline mach mach_new_cart(uint8_t *rom, size_t len, rl_gba_cart cart)
{
    mach m = {0};
    m.rom_len = len;
    m.rom = rom;
    cart.rom = m.rom;
    cart.rom_len = m.rom_len;
    m.g = rl_gba_create(&cart);
    m.s = rl_sched_create(NULL);
    m.c = rl_sched_add_cpu(m.s, 1);
    if (m.g && rl_gba_attach(m.g, m.s, m.c, NULL) != 0) {
        rl_gba_destroy(m.g);
        m.g = NULL;
    }
    return m;
}

/** @brief A fresh machine with rom (len bytes, taken over) and sram (may be NULL) at tick 0. */
static inline mach mach_new_with(uint8_t *rom, size_t len, uint8_t *sram, size_t sram_len)
{
    return mach_new_cart(rom, len, (rl_gba_cart){.sram = sram, .sram_len = sram_len});
}

/** @brief A fresh machine with a 1 MiB ROM (mach_rom) and sram (may be NULL) at tick 0. */
static inline mach mach_new_sram(uint8_t *sram, size_t sram_len) { return mach_new_with(mach_rom(0x100000), 0x100000, sram, sram_len); }

/** @brief A fresh machine without SRAM. */
static inline mach mach_new(void) { return mach_new_sram(NULL, 0); }

static inline void mach_free(mach *m)
{
    rl_gba_destroy(m->g);
    rl_sched_destroy(m->s);
    free(m->rom);
}

/** @brief The CPU's tick. */
static inline uint64_t mach_now(const mach *m) { return rl_sched_now(m->c); }

/** @brief Moves the clock to tick t (not backwards), runs the events due by then and a DMA transfer that is due (the CPU waits for it). */
static inline void mach_to(mach *m, uint64_t t)
{
    uint64_t now = mach_now(m);
    if (t > now)
        rl_sched_stall(m->c, t - now);
    rl_sched_run(m->c);
    rl_gba_service(m->g);
}

/** @brief An I/O read at tick t (the clock moves to t first, the access itself charges nothing). */
static inline uint32_t mach_rd(mach *m, uint64_t t, uint32_t addr, unsigned size)
{
    mach_to(m, t);
    return rl_gba_read(m->g, addr, size);
}

/** @brief An I/O write at tick t. */
static inline void mach_wr(mach *m, uint64_t t, uint32_t addr, unsigned size, uint32_t v)
{
    mach_to(m, t);
    rl_gba_write(m->g, addr, size, v);
}

/** @brief Bits of IF as the CPU reads them at tick t. */
static inline uint32_t mach_if(mach *m, uint64_t t) { return mach_rd(m, t, 0x04000202u, 2); }

#endif
