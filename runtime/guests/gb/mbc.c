/**
 * @file mbc.c
 * @brief Cartridge banking: the registers behind writes to $0000-$7FFF, the
 * ROM and RAM banks they select, and the pages the read path uses.
 */
#include "gb_int.h"

#include <string.h>

static bool rtc_selected(const rl_gb *g) { return g->cart.mbc == RL_GB_MBC3 && g->ram_reg >= 8 && g->ram_reg <= 0xC; }

static void select_banks(rl_gb *g)
{
    g->bank0 = 0;
    g->bank1 = 1;
    g->rambank = 0;
    switch (g->cart.mbc) {
    case RL_GB_MBC1: {
        uint32_t bits = g->cart.multicart ? 4u : 5u;
        uint32_t low = g->rom_reg & 0x1Fu;
        if (low == 0)
            low = 1;
        low &= (1u << bits) - 1u;
        uint32_t high = (g->ram_reg & 3u) << bits;
        g->bank0 = g->mode ? high : 0;
        g->bank1 = high | low;
        g->rambank = g->mode ? g->ram_reg & 3u : 0;
        break;
    }
    case RL_GB_MBC2:
        g->bank1 = g->rom_reg & 0xFu ? g->rom_reg & 0xFu : 1u;
        break;
    case RL_GB_MBC3:
        g->bank1 = g->rom_reg & 0x7Fu ? g->rom_reg & 0x7Fu : 1u;
        g->rambank = g->ram_reg & 3u;
        break;
    case RL_GB_MBC5:
        g->bank1 = g->rom_reg & 0x1FFu;
        g->rambank = g->ram_reg & (g->cart.rumble ? 7u : 0xFu);
        break;
    default:
        break;
    }
    g->bank0 %= g->rom_banks;
    g->bank1 %= g->rom_banks;
}

static bool ram_direct(const rl_gb *g)
{
    if (!g->ram || !g->ram_enable || g->ram_len < GB_RAM_BANK || g->cart.mbc == RL_GB_MBC2)
        return false;
    return g->cart.mbc != RL_GB_MBC3 || g->ram_reg < 4;
}

static size_t ram_index(const rl_gb *g, uint16_t addr)
{
    size_t banks = (g->ram_len + GB_RAM_BANK - 1) / GB_RAM_BANK;
    return ((size_t)(g->rambank % banks) * GB_RAM_BANK + (addr & 0x1FFFu)) % g->ram_len;
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
        size_t off = ram_index(g, 0xA000);
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
    if (g->has_bus)
        memset(g->rd, 0, sizeof g->rd);
}

void gb_reset_mapper(rl_gb *g)
{
    g->rom_reg = g->cart.mbc == RL_GB_MBC5 ? 1 : 0;
    g->ram_reg = 0;
    g->mode = false;
    g->ram_enable = g->cart.mbc == RL_GB_MBC_NONE;
    g->rumble = false;
    memset(&g->rtc, 0, sizeof g->rtc);
    if (g->cart.rtc)
        g->rtc.stamp = gb_host_time(g);
    gb_remap(g);
}

void gb_mbc_write(rl_gb *g, uint16_t addr, uint8_t value)
{
    switch (g->cart.mbc) {
    case RL_GB_MBC1:
        if (addr < 0x2000)
            g->ram_enable = (value & 0xFu) == 0xA;
        else if (addr < 0x4000)
            g->rom_reg = value & 0x1Fu;
        else if (addr < 0x6000)
            g->ram_reg = value & 3u;
        else
            g->mode = (value & 1u) != 0;
        break;
    case RL_GB_MBC2:
        if (addr >= 0x4000)
            return;
        if (addr & 0x100)
            g->rom_reg = value & 0xFu;
        else
            g->ram_enable = (value & 0xFu) == 0xA;
        break;
    case RL_GB_MBC3:
        if (addr < 0x2000)
            g->ram_enable = (value & 0xFu) == 0xA;
        else if (addr < 0x4000)
            g->rom_reg = value & 0x7Fu;
        else if (addr < 0x6000)
            g->ram_reg = value & 0xFu;
        else {
            if (g->cart.rtc && g->rtc.latch_last == 0 && value == 1) {
                gb_rtc_update(g);
                for (unsigned r = 0; r < 5; r++)
                    g->rtc.latched[r] = gb_rtc_peek(g, r);
            }
            g->rtc.latch_last = value;
        }
        break;
    case RL_GB_MBC5:
        if (addr < 0x2000)
            g->ram_enable = value == 0x0A;
        else if (addr < 0x3000)
            g->rom_reg = (g->rom_reg & 0x100u) | value;
        else if (addr < 0x4000)
            g->rom_reg = (g->rom_reg & 0xFFu) | ((uint32_t)(value & 1u) << 8);
        else if (addr < 0x6000) {
            g->ram_reg = value & 0xFu;
            g->rumble = g->cart.rumble && (value & 8u) != 0;
        }
        break;
    default:
        return;
    }
    gb_remap(g);
}

uint8_t gb_ram_read(rl_gb *g, uint16_t addr)
{
    if (!g->ram_enable)
        return 0xFF;
    if (g->cart.mbc == RL_GB_MBC2)
        return (uint8_t)(g->ram[addr & 0x1FFu] | 0xF0u);
    if (rtc_selected(g))
        return g->cart.rtc ? g->rtc.latched[g->ram_reg - 8] : 0xFF;
    if (!g->ram || (g->cart.mbc == RL_GB_MBC3 && g->ram_reg >= 4))
        return 0xFF;
    return g->ram[ram_index(g, addr)];
}

void gb_ram_write(rl_gb *g, uint16_t addr, uint8_t value)
{
    if (!g->ram_enable)
        return;
    g->dirty = true;
    if (g->cart.mbc == RL_GB_MBC2) {
        g->ram[addr & 0x1FFu] = value & 0xFu;
        return;
    }
    if (rtc_selected(g)) {
        if (g->cart.rtc)
            gb_rtc_write(g, g->ram_reg - 8, value);
        return;
    }
    if (!g->ram || (g->cart.mbc == RL_GB_MBC3 && g->ram_reg >= 4))
        return;
    g->ram[ram_index(g, addr)] = value;
}
