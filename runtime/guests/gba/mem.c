/**
 * @file mem.c
 * @brief The address map: BIOS, work RAM, I/O, palette, video RAM, OAM, the
 * Game Pak ROM and SRAM, their mirrors, the 8-bit write rules and open bus;
 * creating and attaching the machine.
 *
 * Ported in part from ares (VRAM mirror and bitmap-mode rules, byte writes
 * to palette, VRAM and OAM, the memory control register), which carries
 * this notice:
 *
 * Copyright (c) 2004-2025 ares team, Near et al
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "gba_int.h"

#include <stdlib.h>
#include <string.h>

static uint32_t mask_of(unsigned size) { return size >= 4 ? UINT32_MAX : (1u << (8 * size)) - 1u; }

static uint32_t ld(const uint8_t *p, unsigned size)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

static void st(uint8_t *p, unsigned size, uint32_t v)
{
    for (unsigned i = 0; i < size; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t open_bus(const rl_gba *g, uint32_t addr, unsigned size)
{
    return (g->mdr >> (8 * (addr & 3))) & mask_of(size);
}

static uint8_t *work_ram(rl_gba *g, uint32_t addr)
{
    if ((addr >> 24) == 0x2 && (g->memcnt & 0x20))
        return &g->ewram[addr & 0x3FFFFu];
    return &g->iwram[addr & 0x7FFFu];
}

static int vram_off(const rl_gba *g, uint32_t addr)
{
    uint32_t o = addr & 0x1FFFFu;
    if ((g->dispcnt & 7) >= 3 && o >= 0x18000u && o < 0x1C000u)
        return -1;
    return (int)(o & ((o & 0x10000u) ? 0x17FFFu : 0xFFFFu));
}

uint16_t gba_rom_half(rl_gba *g, uint32_t addr)
{
    uint32_t o = addr & 0x1FFFFFEu;
    if (o + 1 < g->cart.rom_len)
        return (uint16_t)(g->cart.rom[o] | g->cart.rom[o + 1] << 8);
    return (uint16_t)(o >> 1);
}

static uint32_t rom_read(rl_gba *g, uint32_t addr, unsigned size)
{
    if (size == 4)
        return gba_rom_half(g, addr & ~3u) | (uint32_t)gba_rom_half(g, (addr & ~3u) | 2) << 16;
    uint16_t h = gba_rom_half(g, addr);
    return size == 2 ? h : (uint32_t)(uint8_t)(h >> (8 * (addr & 1)));
}

uint32_t gba_mem_read(rl_gba *g, uint32_t addr, unsigned size)
{
    switch (addr >> 24) {
    case 0x0:
    case 0x1:
        return addr < 0x4000u ? rl_gba_bios_read(&g->bios, addr, size) & mask_of(size) : open_bus(g, addr, size);
    case 0x2:
    case 0x3:
        return ld(work_ram(g, addr), size);
    case 0x4:
        return gba_io_read(g, addr, size);
    case 0x5:
        return ld(&g->pal[addr & 0x3FFu], size);
    case 0x6: {
        int o = vram_off(g, addr);
        return o < 0 ? 0 : ld(&g->vram[o], size);
    }
    case 0x7:
        return ld(&g->oam[addr & 0x3FFu], size);
    case 0x8:
    case 0x9:
    case 0xA:
    case 0xB:
    case 0xC:
        return rom_read(g, addr, size);
    case 0xD:
        return gba_eeprom_hit(g, addr) ? gba_eeprom_read(g, size) : rom_read(g, addr, size);
    case 0xE:
    case 0xF:
        return gba_save_read(g, addr) * (0x01010101u & mask_of(size));
    default:
        return open_bus(g, addr, size);
    }
}

void gba_mem_write(rl_gba *g, uint32_t addr, unsigned size, uint32_t value)
{
    switch (addr >> 24) {
    case 0x2:
    case 0x3:
        st(work_ram(g, addr), size, value);
        return;
    case 0x4:
        gba_io_write(g, addr, size, value);
        return;
    case 0x5:
        if (size == 1)
            st(&g->pal[addr & 0x3FEu], 2, (value & 0xFFu) * 0x0101u);
        else
            st(&g->pal[addr & 0x3FFu], size, value);
        return;
    case 0x6: {
        int o = vram_off(g, addr);
        if (o < 0)
            return;
        if (size == 1) {
            if (o >= ((g->dispcnt & 7) >= 3 ? 0x14000 : 0x10000))
                return;
            st(&g->vram[o & ~1], 2, (value & 0xFFu) * 0x0101u);
        } else {
            st(&g->vram[o], size, value);
        }
        return;
    }
    case 0x7:
        if (size != 1)
            st(&g->oam[addr & 0x3FFu], size, value);
        return;
    case 0xD:
        if (gba_eeprom_hit(g, addr))
            gba_eeprom_write(g, size, value);
        return;
    case 0xE:
    case 0xF:
        gba_save_write(g, addr, (uint8_t)((size == 4 ? value : size == 2 ? (value & 0xFFFFu) * 0x10001u : (value & 0xFFu) * 0x01010101u) >> (8 * (addr & 3))));
        return;
    default:
        return;
    }
}

uint32_t rl_gba_peek(const rl_gba *g, uint32_t addr, unsigned size)
{
    if ((addr >> 24) == 0x4)
        return open_bus(g, addr, size);
    if ((addr >> 24) == 0xD)
        return rom_read((rl_gba *)(uintptr_t)g, addr & ~(size - 1), size);
    return gba_mem_read((rl_gba *)(uintptr_t)g, addr & ~(size - 1), size);
}

uint32_t rl_gba_read(rl_gba *g, uint32_t addr, unsigned size)
{
    addr = gba_align(addr, size);
    uint32_t v = gba_mem_read(g, addr, size);
    if ((addr >> 24) >= 0x2)
        g->mdr = size == 4 ? v : size == 2 ? v * 0x10001u : v * 0x01010101u;
    return v;
}

void rl_gba_write(rl_gba *g, uint32_t addr, unsigned size, uint32_t value)
{
    addr = gba_align(addr, size);
    gba_mem_write(g, addr, size, value);
    gba_after_write(g, addr, size);
}

static uint32_t bios_read(void *user, uint32_t addr, unsigned size) { return rl_gba_read(user, addr, size); }

static void bios_write(void *user, uint32_t addr, unsigned size, uint32_t value)
{
    rl_gba *g = user;
    g->in_bios = true;
    rl_gba_write(g, addr, size, value);
    g->in_bios = false;
    uint32_t end = addr + size;
    bool irq_reg = addr < 0x0400020Au && end > 0x04000200u;
    if (irq_reg && g->hooks.irq && gba_irq_wants(g))
        g->hooks.irq(g->hooks.user);
}

static unsigned bios_cycles(void *user, uint32_t addr, unsigned size, bool seq)
{
    return rl_gba_wait(&((rl_gba *)user)->waits, addr, size, seq);
}

rl_gba *rl_gba_create(const rl_gba_cart *cart)
{
    if (!cart || cart->rom_len > 0x2000000u)
        return NULL;
    if (cart->sram && (cart->sram_len == 0 || cart->sram_len > 0x10000u || (cart->sram_len & (cart->sram_len - 1))))
        return NULL;
    rl_gba *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->cart = *cart;
    if (!g->cart.rom)
        g->cart.rom_len = 0;
    if (gba_save_init(g, cart) != 0) {
        free(g);
        return NULL;
    }
    g->memcnt = 0x0D000020u;
    g->dma_last = 0;
    g->bios_bus = (rl_gba_bus){.user = g, .read = bios_read, .write = bios_write, .cycles = bios_cycles};
    rl_gba_bios_init(&g->bios);
    g->mdr = g->bios.open_bus;
    g->pf.stopped = true;
    g->pf.wait = 0;
    g->regs[0x088] = 0x00;
    g->regs[0x089] = 0x02;
    g->regs[0x130] = 0xFF;
    g->regs[0x131] = 0x03;
    gba_waits_rebuild(g);
    gba_irq_reset(g);
    gba_timer_reset(g);
    gba_dma_reset(g);
    gba_lcd_reset(g);
    return g;
}

void rl_gba_destroy(rl_gba *g)
{
    if (!g)
        return;
    if (g->sched) {
        rl_sched_cancel(g->sched, &g->irq.ev);
        rl_sched_cancel(g->sched, &g->lcd.ev);
        rl_sched_cancel(g->sched, &g->pad.ev);
        for (unsigned i = 0; i < 4; i++) {
            rl_sched_cancel(g->sched, &g->tm[i].ev);
            rl_sched_cancel(g->sched, &g->dma[i].ev);
        }
    }
    gba_save_free(g);
    free(g);
}

uint8_t *rl_gba_mem(rl_gba *g, rl_gba_region r, size_t *len)
{
    static const size_t lens[RL_GBA_REGIONS] = {0x40000, 0x8000, 0x400, 0x18000, 0x400};
    uint8_t *p[RL_GBA_REGIONS] = {g->ewram, g->iwram, g->pal, g->vram, g->oam};
    if ((unsigned)r >= RL_GBA_REGIONS)
        return NULL;
    if (len)
        *len = lens[r];
    return p[r];
}

rl_gba_bios *rl_gba_bios_of(rl_gba *g) { return &g->bios; }
const rl_gba_bus *rl_gba_bios_bus(rl_gba *g) { return &g->bios_bus; }
void rl_gba_set_cpu_hooks(rl_gba *g, const rl_gba_cpu_hooks *hooks) { g->hooks = hooks ? *hooks : (rl_gba_cpu_hooks){0}; }
uint32_t rl_gba_open_bus(const rl_gba *g) { return g->mdr; }
void rl_gba_set_open_bus(rl_gba *g, uint32_t value) { g->mdr = value; }
rl_sched_cpu *rl_gba_cpu(const rl_gba *g) { return g->cpu; }
uint64_t rl_gba_cycles(const rl_gba *g) { return g->cycles; }
const rl_gba_waits *rl_gba_wait_table(const rl_gba *g) { return &g->waits; }
bool rl_gba_halted(const rl_gba *g) { return g->halted; }

void rl_gba_set_timer_hook(rl_gba *g, rl_gba_timer_fn *fn, void *user)
{
    g->timer_fn = fn;
    g->timer_user = user;
}

void rl_gba_set_vblank_hook(rl_gba *g, rl_gba_line_fn *fn, void *user)
{
    g->vblank_fn = fn;
    g->vblank_user = user;
    gba_lcd_repost(g);
}

uint64_t gba_now(const rl_gba *g) { return g->cpu ? rl_sched_now(g->cpu) : g->cycles; }

void gba_charge(rl_gba *g, unsigned n)
{
    g->cycles += n;
    if (g->cpu)
        rl_sched_stall(g->cpu, n);
}
