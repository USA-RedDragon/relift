/**
 * @file io.c
 * @brief The I/O registers as scheduler devices: the range table, open bus
 * outside it, the stand-in devices of the slots nobody supplies, the system
 * registers (IE, IF, IME, WAITCNT, POSTFLG, HALTCNT, the memory control
 * register and its mirrors) and attaching to a scheduler.
 *
 * Ported in part from ares (gba/cpu/io.cpp: the system registers' read and
 * write values, gba/memory/memory.cpp: byte-wise register access), which
 * carries this notice:
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

enum { OWN_NONE, OWN_LCD, OWN_TIMER, OWN_DMA, OWN_SYS, OWN_SLOT };

typedef struct io_range {
    uint16_t lo, hi;
    uint8_t own, slot;
} io_range;

static const io_range ranges[] = {
    {0x000, 0x003, OWN_SLOT, RL_GBA_SLOT_PPU},    {0x004, 0x007, OWN_LCD, 0},
    {0x008, 0x05F, OWN_SLOT, RL_GBA_SLOT_PPU},    {0x060, 0x0AF, OWN_SLOT, RL_GBA_SLOT_APU},
    {0x0B0, 0x0DF, OWN_DMA, 0},                   {0x100, 0x10F, OWN_TIMER, 0},
    {0x120, 0x12F, OWN_SLOT, RL_GBA_SLOT_SERIAL}, {0x130, 0x133, OWN_SLOT, RL_GBA_SLOT_KEYPAD},
    {0x134, 0x15B, OWN_SLOT, RL_GBA_SLOT_SERIAL}, {0x200, 0x20B, OWN_SYS, 0},
    {0x300, 0x303, OWN_SYS, 0},                   {0x800, 0x803, OWN_SYS, 0},
};

static bool mapped(uint32_t off)
{
    for (unsigned i = 0; i < sizeof ranges / sizeof ranges[0]; i++)
        if (off >= ranges[i].lo && off <= ranges[i].hi)
            return true;
    return false;
}

static uint32_t io_addr(uint32_t addr)
{
    if ((addr & 0xFF00FFFCu) == 0x04000800u)
        return 0x04000800u | (addr & 3);
    return addr;
}

uint32_t gba_io_read(rl_gba *g, uint32_t addr, unsigned size)
{
    uint32_t a = io_addr(addr);
    uint32_t off = a - 0x04000000u;
    if (!g->cpu || off >= 0x804u || !mapped(off)) {
        uint32_t v = g->mdr >> (8 * (addr & 3));
        return size == 4 ? v : v & ((1u << (8 * size)) - 1u);
    }
    return (uint32_t)rl_sched_read(g->cpu, a, size);
}

void gba_io_write(rl_gba *g, uint32_t addr, unsigned size, uint32_t value)
{
    uint32_t a = io_addr(addr);
    uint32_t off = a - 0x04000000u;
    if (!g->cpu || off >= 0x804u || !mapped(off))
        return;
    for (unsigned i = 0; i < size; i++)
        if (off + i < 2)
            g->dispcnt = (uint16_t)((g->dispcnt & ~(0xFFu << (8 * (off + i)))) | ((value >> (8 * i)) & 0xFFu) << (8 * (off + i)));
    rl_sched_write(g->cpu, a, size, value);
}

void gba_after_write(rl_gba *g, uint32_t addr, unsigned size)
{
    (void)addr;
    (void)size;
    if (g->halt_req || g->stop_req) {
        gba_halt(g);
        if (g->hooks.irq && gba_irq_wants(g))
            g->hooks.irq(g->hooks.user);
    }
}

typedef uint8_t byte_read(rl_gba *g, uint32_t addr, uint64_t now);
typedef void byte_write(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now);

static uint64_t split_read(rl_gba *g, byte_read *f, uint64_t addr, uint32_t size, uint64_t now)
{
    uint64_t v = 0;
    for (uint32_t i = 0; i < size; i++)
        v |= (uint64_t)f(g, (uint32_t)addr + i, now) << (8 * i);
    return v;
}

static void split_write(rl_gba *g, byte_write *f, uint64_t addr, uint32_t size, uint64_t value, uint64_t now)
{
    for (uint32_t i = 0; i < size; i++)
        f(g, (uint32_t)addr + i, (uint8_t)(value >> (8 * i)), now);
}

static uint8_t stub_byte(rl_gba *g, uint32_t addr, uint64_t now)
{
    (void)now;
    return g->regs[addr & 0x3FFu];
}

static void stub_set(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now)
{
    (void)now;
    uint32_t off = addr & 0x3FFu;
    if (off == 0x130 || off == 0x131)
        return;
    g->regs[off] = v;
}

#define DEVICE_RW(name, rd, wr)                                                                                        \
    static uint64_t name##_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now)                              \
    {                                                                                                                  \
        return split_read(gba_of(d), rd, addr, size, now);                                                             \
    }                                                                                                                  \
    static void name##_write(rl_device *d, uint64_t addr, uint32_t size, uint64_t value, uint64_t now)                 \
    {                                                                                                                  \
        split_write(gba_of(d), wr, addr, size, value, now);                                                            \
    }

static bool stub_open(uint32_t off)
{
    return (off >= 0x12C && off <= 0x12F) || (off >= 0x138 && off <= 0x13F) || (off >= 0x144 && off <= 0x14F);
}

static uint8_t serial_byte(rl_gba *g, uint32_t addr, uint64_t now)
{
    uint32_t off = addr & 0x3FFu;
    if (stub_open(off))
        return gba_open_byte(g, addr);
    uint8_t v = stub_byte(g, addr, now);
    switch (off) {
    case 0x128:
        return v | 0x04;
    case 0x134: {
        if (!(g->regs[0x135] & 0x80))
            return 0x05;
        uint8_t dir = v >> 4 & 0xFu;
        return (uint8_t)((v & 0xF0u) | (v & dir) | (0xFu & ~dir));
    }
    case 0x158:
        return v | 0x08;
    default:
        return v;
    }
}

static void serial_set(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now)
{
    switch (addr & 0x3FFu) {
    case 0x134:
        v &= 0xF0u | (g->regs[0x135] & 0x80 ? 0x0Fu : 0u);
        break;
    case 0x135:
        v &= 0xC0u;
        break;
    case 0x140:
        v &= 0x40u;
        break;
    case 0x141:
        v = 0;
        break;
    case 0x158:
        v &= 0x30u;
        break;
    case 0x159:
        v = 0;
        break;
    default:
        break;
    }
    stub_set(g, addr, v, now);
}

static void apu_set(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now)
{
    if ((addr & 0x3FFu) == 0x83) {
        gba_fifo_control(g, v);
        v &= 0x77u;
    }
    stub_set(g, addr, v, now);
}

DEVICE_RW(stub, stub_byte, stub_set)

static uint64_t apu_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now)
{
    return split_read(gba_of(d), stub_byte, addr, size, now);
}

static void apu_write(rl_device *d, uint64_t addr, uint32_t size, uint64_t value, uint64_t now)
{
    uint32_t off = (uint32_t)addr & 0x3FFu;
    if (off >= 0xA0 && off <= 0xA7)
        gba_fifo_write(gba_of(d), off, size, (uint32_t)value);
    split_write(gba_of(d), apu_set, addr, size, value, now);
}
DEVICE_RW(serial, serial_byte, serial_set)
DEVICE_RW(lcd, gba_lcd_read, gba_lcd_write)

static uint8_t ppu_byte(rl_gba *g, uint32_t addr, uint64_t now)
{
    (void)now;
    return gba_ppu_dev_read(g, addr);
}

static void ppu_set(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now)
{
    (void)now;
    gba_ppu_dev_write(g, addr, v);
}

DEVICE_RW(ppu, ppu_byte, ppu_set)
DEVICE_RW(timer, gba_timer_read, gba_timer_write)
DEVICE_RW(sys, gba_sys_read, gba_sys_write)

static uint8_t pad_byte(rl_gba *g, uint32_t addr, uint64_t now) { return gba_keypad_read(g, addr, now); }

DEVICE_RW(pad, pad_byte, gba_keypad_write)

static uint8_t dma_byte(rl_gba *g, uint32_t addr, uint64_t now)
{
    (void)now;
    return gba_dma_read(g, addr);
}

DEVICE_RW(dma, dma_byte, gba_dma_write)

uint8_t gba_sys_read(rl_gba *g, uint32_t addr, uint64_t now)
{
    uint32_t off = addr & 0xFFFu;
    gba_irq_state s = gba_irq_at(g, now ? now - 1 : 0);
    switch (off) {
    case 0x200:
        return (uint8_t)s.ie;
    case 0x201:
        return (uint8_t)(s.ie >> 8);
    case 0x202:
        return (uint8_t)s.iflag;
    case 0x203:
        return (uint8_t)(s.iflag >> 8);
    case 0x204:
        return (uint8_t)g->waitcnt;
    case 0x205:
        return (uint8_t)((g->waitcnt >> 8) & 0x7F);
    case 0x208:
        return s.ime;
    case 0x300:
        return g->postflg;
    case 0x800:
        return (uint8_t)(g->memcnt & 0x2F);
    case 0x803:
        return (uint8_t)(g->memcnt >> 24);
    case 0x206:
    case 0x207:
    case 0x209:
    case 0x20A:
    case 0x20B:
    case 0x301:
    case 0x302:
    case 0x303:
    case 0x801:
    case 0x802:
        return 0;
    default:
        return gba_open_byte(g, addr);
    }
}

void gba_sys_write(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now)
{
    uint32_t off = addr & 0xFFFu;
    gba_irq_state s = gba_irq_at(g, now);
    switch (off) {
    case 0x200:
        gba_irq_ie(g, (uint16_t)((s.ie & 0xFF00) | v), now);
        return;
    case 0x201:
        gba_irq_ie(g, (uint16_t)((s.ie & 0x00FF) | (v & 0x3F) << 8), now);
        return;
    case 0x202:
        gba_irq_ack(g, v, now);
        return;
    case 0x203:
        gba_irq_ack(g, (uint16_t)(v << 8), now);
        return;
    case 0x204:
        g->waitcnt = (uint16_t)((g->waitcnt & 0xFF00) | v);
        gba_waits_rebuild(g);
        return;
    case 0x205:
        g->waitcnt = (uint16_t)((g->waitcnt & 0x00FF) | (v & 0x5F) << 8);
        gba_waits_rebuild(g);
        return;
    case 0x208:
        gba_irq_ime(g, v & 1, now);
        return;
    case 0x300:
        if (v & 1)
            g->postflg = 1;
        return;
    case 0x301:
        if (!g->in_bios)
            return;
        if (v & 0x80)
            g->stop_req = true;
        else
            g->halt_req = true;
        return;
    case 0x800:
        g->memcnt = (g->memcnt & ~0xFFu) | (v & 0x2Fu);
        gba_waits_rebuild(g);
        return;
    case 0x803:
        g->memcnt = (g->memcnt & 0x00FFFFFFu) | (uint32_t)v << 24;
        gba_waits_rebuild(g);
        return;
    default:
        return;
    }
}

static const char *const slot_names[RL_GBA_SLOTS] = {"gba ppu", "gba apu", "gba serial", "gba keypad"};

static int add_dev(rl_gba *g, gba_dev *d, const char *name, rl_device_read *rd, rl_device_write *wr)
{
    d->g = g;
    d->dev.name = name;
    d->dev.opaque = d;
    d->dev.line = RL_LINE_NONE;
    d->dev.read = rd;
    d->dev.write = wr;
    return rl_sched_add_device(g->sched, &d->dev);
}

int rl_gba_attach(rl_gba *g, rl_sched *s, rl_sched_cpu *cpu, rl_device *const devs[RL_GBA_SLOTS])
{
    if (!s || !cpu || g->sched)
        return -1;
    g->sched = s;
    if (add_dev(g, &g->d_lcd, "gba lcd", lcd_read, lcd_write) || add_dev(g, &g->d_timer, "gba timers", timer_read, timer_write) ||
        add_dev(g, &g->d_dma, "gba dma", dma_read, dma_write) || add_dev(g, &g->d_ppu, "gba ppu", ppu_read, ppu_write) || add_dev(g, &g->d_sys, "gba system", sys_read, sys_write))
        return -1;
    for (unsigned i = 0; i < RL_GBA_SLOTS; i++) {
        rl_device *d = devs ? devs[i] : NULL;
        if (!d && i == RL_GBA_SLOT_KEYPAD) {
            if (add_dev(g, &g->d_pad, "gba keypad", pad_read, pad_write))
                return -1;
            g->slot[i] = &g->d_pad.dev;
            continue;
        }
        if (!d && i == RL_GBA_SLOT_PPU) {
            g->slot[i] = &g->d_ppu.dev;
            continue;
        }
        if (!d) {
            gba_stub *st = &g->stub[i];
            st->g = g;
            d = &st->dev;
            d->name = slot_names[i];
            d->opaque = st;
            d->line = RL_LINE_NONE;
            d->read = i == RL_GBA_SLOT_SERIAL ? serial_read : i == RL_GBA_SLOT_APU ? apu_read : stub_read;
            d->write = i == RL_GBA_SLOT_SERIAL ? serial_write : i == RL_GBA_SLOT_APU ? apu_write : stub_write;
        }
        if (rl_sched_add_device(s, d) != 0)
            return -1;
        g->slot[i] = d;
    }
    for (unsigned i = 0; i < sizeof ranges / sizeof ranges[0]; i++) {
        const io_range *r = &ranges[i];
        rl_device *d = r->own == OWN_LCD     ? &g->d_lcd.dev
                       : r->own == OWN_TIMER ? &g->d_timer.dev
                       : r->own == OWN_DMA   ? &g->d_dma.dev
                       : r->own == OWN_SYS   ? &g->d_sys.dev
                                             : g->slot[r->slot];
        if (rl_sched_map(s, 0x04000000u + r->lo, (uint64_t)r->hi - r->lo + 1, d, 0) != 0)
            return -1;
    }
    g->cpu = cpu;
    rl_sched_set_mask(cpu, (1ull << RL_GBA_LINE_IRQ) | (1ull << RL_GBA_LINE_DMA));
    gba_irq_attach(g);
    gba_timer_attach(g);
    gba_dma_attach(g);
    gba_lcd_attach(g);
    gba_keypad_attach(g);
    if (g->slot[RL_GBA_SLOT_PPU] == &g->d_ppu.dev)
        gba_ppu_dev_attach(g);
    return 0;
}

rl_device *rl_gba_device(rl_gba *g, rl_gba_slot slot) { return g->sched && (unsigned)slot < RL_GBA_SLOTS ? g->slot[slot] : NULL; }
