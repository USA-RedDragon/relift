/**
 * @file mem.c
 * @brief The address map: dispatch of a 16-bit address to ROM, video RAM,
 * external RAM, work RAM, echo RAM, OAM, the unusable cells, I/O and high RAM.
 */
#include "gb_int.h"

#include <stdlib.h>
#include <string.h>

rl_gb *rl_gb_create(rl_gb_model model, const rl_gb_cart *cart, rl_gb_status *status)
{
    rl_gb_status st = RL_GB_OK;
    rl_gb *g = NULL;
    if (!cart || !cart->rom || cart->rom_len < 2 * GB_ROM_BANK || cart->rom_len % GB_ROM_BANK != 0 || (unsigned)model >= RL_GB_MODELS)
        st = RL_GB_ERR_ROM;
    else if (cart->mbc >= RL_GB_MBC_UNSUPPORTED)
        st = RL_GB_ERR_UNSUPPORTED;
    else if (!(g = calloc(1, sizeof *g)))
        st = RL_GB_ERR_MEMORY;
    if (g) {
        g->model = model;
        g->cart = *cart;
        g->rom_banks = (uint32_t)(cart->rom_len / GB_ROM_BANK);
        g->color = rl_gb_model_is_cgb(model) && cart->cgb;
        g->ram_len = cart->mbc == RL_GB_MBC2 ? GB_MBC2_RAM : cart->ram_len;
        if (g->ram_len) {
            g->ram = calloc(1, g->ram_len);
            if (!g->ram) {
                free(g);
                g = NULL;
                st = RL_GB_ERR_MEMORY;
            }
        }
    }
    if (g)
        rl_gb_reset(g);
    if (status)
        *status = st;
    return g;
}

void rl_gb_destroy(rl_gb *g)
{
    if (!g)
        return;
    free(g->ram);
    free(g);
}

rl_gb_model rl_gb_get_model(const rl_gb *g) { return g->model; }
bool rl_gb_color_mode(const rl_gb *g) { return g->color; }

void rl_gb_set_bus(rl_gb *g, const rl_gb_bus *bus)
{
    g->has_bus = bus != NULL;
    if (bus)
        g->bus = *bus;
    gb_remap(g);
}

static uint8_t read_slow(rl_gb *g, rl_sched_cpu *c, uint16_t addr)
{
    if (g->has_bus && !gb_is_io(addr))
        return g->bus.read(g->bus.ctx, addr);
    if (gb_is_io(addr))
        return g->sched && c ? (uint8_t)rl_sched_read(c, addr, 1) : 0xFF;
    if (addr >= 0xA000 && addr < 0xC000)
        return gb_ram_read(g, addr);
    if (addr < 0xFE00)
        return g->wram[(addr & 0x1000 ? gb_wram_bank(g) * 0x1000u : 0u) + (addr & 0xFFF)];
    if (addr < 0xFEA0)
        return g->oam[addr - 0xFE00];
    if (addr >= 0xFF80 && addr != 0xFFFF)
        return g->hram[addr - 0xFF80];
    if (addr < 0xFF00)
        return g->color ? g->unusable[(unsigned)(addr & 0xE7) - 0xA0u] : 0x00;
    if (addr == 0xFF4F)
        return g->color ? (uint8_t)(0xFE | g->vbk) : 0xFF;
    if (addr == 0xFF70)
        return g->color ? (uint8_t)(0xF8 | g->svbk) : 0xFF;
    return 0xFF;
}

uint8_t rl_gb_read(rl_gb *g, rl_sched_cpu *c, uint16_t addr)
{
    const uint8_t *p = g->rd[addr >> 12];
    if (p)
        return p[addr & 0xFFF];
    return read_slow(g, c, addr);
}

uint8_t rl_gb_peek(const rl_gb *g, uint16_t addr)
{
    if (g->has_bus)
        return !gb_is_io(addr) && g->bus.peek ? g->bus.peek(g->bus.ctx, addr) : 0xFF;
    return rl_gb_read((rl_gb *)(uintptr_t)g, NULL, addr);
}

void rl_gb_write(rl_gb *g, rl_sched_cpu *c, uint16_t addr, uint8_t value)
{
    if (g->has_bus && !gb_is_io(addr)) {
        g->bus.write(g->bus.ctx, addr, value);
        return;
    }
    switch (addr >> 12) {
    case 0x0:
    case 0x1:
    case 0x2:
    case 0x3:
    case 0x4:
    case 0x5:
    case 0x6:
    case 0x7:
        gb_mbc_write(g, addr, value);
        return;
    case 0x8:
    case 0x9:
        g->vram[g->vbk * 0x2000u + (addr & 0x1FFF)] = value;
        return;
    case 0xA:
    case 0xB:
        gb_ram_write(g, addr, value);
        return;
    case 0xC:
    case 0xE:
        g->wram[addr & 0xFFF] = value;
        return;
    case 0xD:
        g->wram[gb_wram_bank(g) * 0x1000u + (addr & 0xFFF)] = value;
        return;
    default:
        break;
    }
    if (addr < 0xFE00)
        g->wram[gb_wram_bank(g) * 0x1000u + (addr & 0xFFF)] = value;
    else if (addr < 0xFEA0)
        g->oam[addr - 0xFE00] = value;
    else if (addr < 0xFF00) {
        if (g->color)
            g->unusable[(unsigned)(addr & 0xE7) - 0xA0u] = value;
    } else if (gb_is_io(addr)) {
        if (g->sched && c)
            rl_sched_write(c, addr, 1, value);
    } else if (addr >= 0xFF80 && addr != 0xFFFF)
        g->hram[addr - 0xFF80] = value;
    else if (addr == 0xFF4F && g->color) {
        g->vbk = value & 1u;
        gb_remap(g);
    } else if (addr == 0xFF70 && g->color) {
        g->svbk = value & 7u;
        gb_remap(g);
    }
}

uint8_t *rl_gb_vram(rl_gb *g, unsigned bank) { return g->vram + (bank & 1u) * 0x2000u; }
unsigned rl_gb_vram_bank(const rl_gb *g) { return g->vbk; }
uint8_t *rl_gb_oam(rl_gb *g) { return g->oam; }
uint8_t *rl_gb_hram(rl_gb *g) { return g->hram; }
unsigned rl_gb_wram_bank(const rl_gb *g) { return gb_wram_bank(g); }
unsigned rl_gb_rom_bank0(const rl_gb *g) { return g->bank0; }
unsigned rl_gb_rom_bank1(const rl_gb *g) { return g->bank1; }
unsigned rl_gb_ram_bank(const rl_gb *g) { return g->rambank; }
bool rl_gb_ram_enabled(const rl_gb *g) { return g->ram_enable; }
bool rl_gb_rumble(const rl_gb *g) { return g->rumble; }

uint8_t *rl_gb_ram(rl_gb *g, size_t *len)
{
    if (len)
        *len = g->ram_len;
    return g->ram;
}
