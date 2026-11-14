/**
 * @file gbt.h
 * @brief Helpers for the Game Boy memory tests: synthetic cartridges whose
 * every ROM bank carries its own number.
 */
#ifndef RELIFT_GBT_H
#define RELIFT_GBT_H

#include "gb.h"
#include "runner.h"

#include <stdlib.h>
#include <string.h>

/** @brief A synthetic image: bank b holds b (low byte) at offset 0, b >> 8 at 1 and $A5 at $3FFF. */
static inline uint8_t *gbt_rom(unsigned banks, uint8_t type, uint8_t ram_code, uint8_t cgb_flag)
{
    uint8_t *rom = calloc(banks, 0x4000);
    for (unsigned b = 0; b < banks; b++) {
        rom[(size_t)b * 0x4000] = (uint8_t)b;
        rom[(size_t)b * 0x4000 + 1] = (uint8_t)(b >> 8);
        rom[(size_t)b * 0x4000 + 0x3FFF] = 0xA5;
    }
    unsigned code = 0;
    while (((2u << code) < banks) && code < 8)
        code++;
    rom[0x143] = cgb_flag;
    rom[0x147] = type;
    rom[0x148] = (uint8_t)code;
    rom[0x149] = ram_code;
    return rom;
}

/** @brief Parses rom and creates a machine of model m; fails the test when either does not work. */
static inline rl_gb *gbt_machine(rl_gb_model m, const uint8_t *rom, unsigned banks)
{
    rl_gb_cart cart;
    if (rl_gb_cart_parse(rom, (size_t)banks * 0x4000, &cart) != 0)
        return NULL;
    return rl_gb_create(m, &cart, NULL);
}

/** @brief Reads the number the bank at $0000 or $4000 carries. */
static inline unsigned gbt_bank_at(rl_gb *g, uint16_t base)
{
    return rl_gb_read(g, NULL, base) | ((unsigned)rl_gb_read(g, NULL, (uint16_t)(base + 1)) << 8);
}

#endif
