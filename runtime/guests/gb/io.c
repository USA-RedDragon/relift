/**
 * @file io.c
 * @brief The I/O registers as scheduler devices: the range table, the
 * stand-in device for a slot nobody supplies, and attaching to a scheduler.
 */
#include "gb_int.h"

const rl_gb_io_range rl_gb_io_ranges[] = {
    {0xFF00, 0xFF00, RL_GB_SLOT_JOYPAD},
    {0xFF01, 0xFF02, RL_GB_SLOT_SERIAL},
    {0xFF04, 0xFF07, RL_GB_SLOT_TIMER},
    {0xFF0F, 0xFF0F, RL_GB_SLOT_SYSTEM},
    {0xFF10, 0xFF26, RL_GB_SLOT_APU},
    {0xFF30, 0xFF3F, RL_GB_SLOT_APU},
    {0xFF40, 0xFF45, RL_GB_SLOT_PPU},
    {0xFF46, 0xFF46, RL_GB_SLOT_DMA},
    {0xFF47, 0xFF4B, RL_GB_SLOT_PPU},
    {0xFF4C, 0xFF4D, RL_GB_SLOT_SYSTEM},
    {0xFF50, 0xFF50, RL_GB_SLOT_SYSTEM},
    {0xFF51, 0xFF55, RL_GB_SLOT_DMA},
    {0xFF56, 0xFF56, RL_GB_SLOT_SYSTEM},
    {0xFF68, 0xFF6C, RL_GB_SLOT_PPU},
    {0xFF72, 0xFF75, RL_GB_SLOT_SYSTEM},
    {0xFF76, 0xFF77, RL_GB_SLOT_APU},
    {0xFFFF, 0xFFFF, RL_GB_SLOT_SYSTEM},
    {0, 0, RL_GB_SLOT_JOYPAD},
};

static const char *const slot_names[RL_GB_SLOTS] = {"joypad", "serial", "timer", "apu", "ppu", "dma", "system"};

static uint64_t stub_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now)
{
    (void)size;
    (void)now;
    return ((gb_stub *)d->opaque)->regs[addr & 0xFFu];
}

static void stub_write(rl_device *d, uint64_t addr, uint32_t size, uint64_t value, uint64_t now)
{
    (void)size;
    (void)now;
    ((gb_stub *)d->opaque)->regs[addr & 0xFFu] = (uint8_t)value;
}

int rl_gb_attach(rl_gb *g, rl_sched *s, rl_device *const devs[RL_GB_SLOTS])
{
    if (!s || g->sched)
        return -1;
    for (unsigned i = 0; i < RL_GB_SLOTS; i++) {
        rl_device *d = devs ? devs[i] : NULL;
        if (!d) {
            d = &g->stub[i].dev;
            d->name = slot_names[i];
            d->opaque = &g->stub[i];
            d->line = RL_LINE_NONE;
            d->read = stub_read;
            d->write = stub_write;
        }
        if (rl_sched_add_device(s, d) != 0)
            return -1;
        g->dev[i] = d;
    }
    for (const rl_gb_io_range *r = rl_gb_io_ranges; r->hi; r++)
        if (rl_sched_map(s, r->lo, (uint64_t)r->hi - r->lo + 1, g->dev[r->slot], 0) != 0)
            return -1;
    g->sched = s;
    return 0;
}

rl_device *rl_gb_device(rl_gb *g, rl_gb_slot slot) { return g->sched && (unsigned)slot < RL_GB_SLOTS ? g->dev[slot] : NULL; }
