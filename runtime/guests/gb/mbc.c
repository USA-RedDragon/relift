/**
 * @file mbc.c
 * @brief Cartridge banking: the registers behind writes to $0000-$7FFF, the
 * ROM and RAM banks they select, and the pages the read path uses.
 */
#include "gb_int.h"

static void select_banks(rl_gb *g)
{
    g->bank0 = 0;
    g->bank1 = 1;
    g->rambank = 0;
    switch (g->cart.mbc) {
    default:
        break;
    }
    g->bank0 %= g->rom_banks;
    g->bank1 %= g->rom_banks;
}

static bool ram_direct(const rl_gb *g)
{
    return g->ram && g->ram_enable && g->ram_len >= GB_RAM_BANK && g->cart.mbc != RL_GB_MBC2;
}

void gb_remap(rl_gb *g)
{
    select_banks(g);
    const uint8_t *rom = g->cart.rom;
    for (unsigned i = 0; i < 4; i++) {
        g->rd[i] = rom + (size_t)g->bank0 * GB_ROM_BANK + i * 0x1000u;
        g->rd[4 + i] = rom + (size_t)g->bank1 * GB_ROM_BANK + i * 0x1000u;
    }
    g->rd[8] = g->vram + g->vbk * 0x2000u;
    g->rd[9] = g->rd[8] + 0x1000;
    if (ram_direct(g)) {
        size_t off = (size_t)(g->rambank % (g->ram_len / GB_RAM_BANK)) * GB_RAM_BANK;
        g->rd[0xA] = g->ram + off;
        g->rd[0xB] = g->ram + off + 0x1000;
    } else {
        g->rd[0xA] = NULL;
        g->rd[0xB] = NULL;
    }
    g->rd[0xC] = g->wram;
    g->rd[0xD] = g->wram + gb_wram_bank(g) * 0x1000u;
    g->rd[0xE] = g->wram;
    g->rd[0xF] = NULL;
}

void gb_reset_mapper(rl_gb *g)
{
    g->rom_reg = 1;
    g->ram_reg = 0;
    g->mode = false;
    g->ram_enable = g->cart.mbc == RL_GB_MBC_NONE;
    g->rumble = false;
    gb_remap(g);
}

void gb_mbc_write(rl_gb *g, uint16_t addr, uint8_t value)
{
    (void)g;
    (void)addr;
    (void)value;
}

uint8_t gb_ram_read(rl_gb *g, uint16_t addr)
{
    if (!g->ram || !g->ram_enable)
        return 0xFF;
    return g->ram[((size_t)(g->rambank % ((g->ram_len + GB_RAM_BANK - 1) / GB_RAM_BANK)) * GB_RAM_BANK + (addr & 0x1FFFu)) % g->ram_len];
}

void gb_ram_write(rl_gb *g, uint16_t addr, uint8_t value)
{
    if (!g->ram || !g->ram_enable)
        return;
    g->ram[((size_t)(g->rambank % ((g->ram_len + GB_RAM_BANK - 1) / GB_RAM_BANK)) * GB_RAM_BANK + (addr & 0x1FFFu)) % g->ram_len] = value;
}
