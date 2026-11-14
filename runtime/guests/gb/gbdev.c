/**
 * @file gbdev.c
 * @brief The slot devices over the device library, and the bus that hands
 * the rest of the memory map to it.
 */
#include "gbdev.h"
#include "gb_int.h"

#include <stdlib.h>

struct rl_gbdev {
    rlgbd *h;
    rl_device dev[RL_GB_SLOTS];
};

static const uint32_t hardware[RL_GB_MODELS] = {
    [RL_GB_DMG] = 0, [RL_GB_DMG0] = 0, [RL_GB_MGB] = 4, [RL_GB_SGB] = 0, [RL_GB_SGB2] = 0,
    [RL_GB_CGB0] = 5, [RL_GB_CGBB] = 5, [RL_GB_CGB] = 1, [RL_GB_CGBE] = 2, [RL_GB_AGB] = 3,
};

static uint64_t slot_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now)
{
    (void)size;
    (void)now;
    return rlgbd_read(((rl_gbdev *)d->opaque)->h, (uint16_t)addr);
}

static void slot_write(rl_device *d, uint64_t addr, uint32_t size, uint64_t value, uint64_t now)
{
    (void)size;
    (void)now;
    rlgbd_write(((rl_gbdev *)d->opaque)->h, (uint16_t)addr, (uint8_t)value);
}

static uint8_t bus_read(void *ctx, uint16_t addr) { return rlgbd_read(((rl_gbdev *)ctx)->h, addr); }
static void bus_write(void *ctx, uint16_t addr, uint8_t value) { rlgbd_write(((rl_gbdev *)ctx)->h, addr, value); }
static uint8_t bus_peek(void *ctx, uint16_t addr) { return rlgbd_peek(((rl_gbdev *)ctx)->h, addr); }

static const char *const slot_names[RL_GB_SLOTS] = {"joypad", "serial", "timer", "apu", "ppu", "dma", "system"};

rl_gbdev *rl_gbdev_create(rl_gb *g, rl_sched *s)
{
    rl_gbdev *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    d->h = rlgbd_new(hardware[g->model], g->cart.rom, g->cart.rom_len);
    rl_device *devs[RL_GB_SLOTS];
    for (unsigned i = 0; i < RL_GB_SLOTS; i++) {
        d->dev[i] = (rl_device){.name = slot_names[i], .opaque = d, .line = RL_LINE_NONE, .read = slot_read, .write = slot_write};
        devs[i] = &d->dev[i];
    }
    if (!d->h || rl_gb_attach(g, s, devs) != 0) {
        rl_gbdev_destroy(d);
        return NULL;
    }
    rl_gb_set_bus(g, &(rl_gb_bus){bus_read, bus_write, bus_peek, d});
    return d;
}

void rl_gbdev_destroy(rl_gbdev *d)
{
    if (!d)
        return;
    rlgbd_free(d->h);
    free(d);
}

rlgbd *rl_gbdev_handle(rl_gbdev *d) { return d->h; }
uint64_t rl_gbdev_now(rl_gbdev *d) { return rlgbd_now(d->h); }

void rl_gbdev_sync(rl_gbdev *d, rl_sched_cpu *c)
{
    uint64_t at = rl_sched_cycles(c), now = rlgbd_now(d->h);
    if (now > at)
        rl_sched_stall(c, now - at);
}
