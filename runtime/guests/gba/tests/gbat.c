/**
 * @file gbat.c
 * @brief The BIOS tests' memory map.
 */
#include "gbat.h"

static gbat machine;

static uint8_t *ptr(gbat *g, uint32_t a)
{
    switch ((a >> 24) & 0xFu) {
    case 2:
        return &g->ewram[a & 0x3FFFFu];
    case 3:
        return &g->iwram[a & 0x7FFFu];
    case 5:
        return &g->pal[a & 0x3FFu];
    case 6: {
        uint32_t o = a & 0x1FFFFu;
        return &g->vram[o >= 0x18000u ? o - 0x8000u : o];
    }
    case 7:
        return &g->oam[a & 0x3FFu];
    default:
        return NULL;
    }
}

static void raise_irq(gbat *g)
{
    uint16_t pend = g->io[0x202 / 2] & g->io[0x200 / 2];
    if (!pend || !(g->io[0x208 / 2] & 1u))
        return;
    rl_gba_irq_cpu cpu = {.sp = 0x03007FA0u, .lr = 0x0000016Cu};
    uint32_t handler = rl_gba_bios_irq_entry(&g->bios, &g->bus, &cpu);
    if (handler) {
        uint32_t flags = gbat_r16(g, 0x03007FF8u) | pend;
        gbat_w16(g, 0x03007FF8u, flags);
        g->io[0x202 / 2] &= (uint16_t)~pend;
    }
    rl_gba_bios_irq_return(&g->bios, &g->bus, &cpu);
}

static uint32_t io_read(gbat *g, uint32_t o) { return o < 0x400u ? g->io[o / 2] : 0; }

static void halt(gbat *g, uint32_t v)
{
    if (v & 0x80u)
        return;
    g->halts++;
    g->io[0x202 / 2] |= g->halt_raises;
    raise_irq(g);
}

static void io_write(gbat *g, uint32_t o, uint32_t v)
{
    if (o >= 0x400u)
        return;
    if (o >= 0xD4u && o < 0xE0u)
        g->dma_writes++;
    if (o == 0x202u) {
        g->io[o / 2] &= (uint16_t)~v;
        return;
    }
    g->io[o / 2] = (uint16_t)v;
    if (o == 0x208u)
        raise_irq(g);
}

static uint32_t bus_read(void *u, uint32_t a, unsigned size)
{
    gbat *g = u;
    if (a < 0x4000u)
        return rl_gba_bios_read(&g->bios, a, size);
    if ((a >> 24) == 4) {
        uint32_t v = io_read(g, a & 0x3FEu);
        if (size == 4)
            v |= io_read(g, (a & 0x3FEu) + 2) << 16;
        else if (size == 1)
            v = (v >> ((a & 1u) * 8)) & 0xFFu;
        return v;
    }
    uint8_t *p = ptr(g, a);
    if (!p)
        return g->bios.open_bus;
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint32_t)ptr(g, a + i)[0] << (8 * i);
    return v;
}

static void bus_write(void *u, uint32_t a, unsigned size, uint32_t v)
{
    gbat *g = u;
    if ((a >> 24) == 4) {
        uint32_t o = a & 0x3FFu;
        if (o == 0x301u && size == 1) {
            halt(g, v);
        } else if (size == 1) {
            uint32_t cur = io_read(g, o & ~1u);
            v = (o & 1u) ? (cur & 0xFFu) | (v << 8) : (cur & 0xFF00u) | v;
            io_write(g, o & ~1u, v & 0xFFFFu);
        } else {
            io_write(g, o, v & 0xFFFFu);
            if (size == 4)
                io_write(g, o + 2, v >> 16);
        }
        return;
    }
    if (!ptr(g, a))
        return;
    for (unsigned i = 0; i < size; i++)
        ptr(g, a + i)[0] = (uint8_t)(v >> (8 * i));
}

gbat *gbat_new(void)
{
    gbat *g = &machine;
    memset(g, 0, sizeof *g);
    g->bus = (rl_gba_bus){.user = g, .read = bus_read, .write = bus_write};
    rl_gba_bios_init(&g->bios);
    return g;
}

rl_gba_swi_cpu gbat_cpu(uint32_t r0, uint32_t r1, uint32_t r2, uint32_t r3)
{
    rl_gba_swi_cpu c = {
        .r = {r0, r1, r2, r3, 0x44444444u, 0x55555555u, 0x66666666u, 0x77777777u, 0x88888888u, 0x99999999u,
              0xAAAAAAAAu, 0xBBBBBBBBu, 0xCCCCCCCCu},
        .ret = 0x08000104u,
        .cpsr = 0x6000009Fu,
        .sp_svc = 0x03007FE0u,
        .sp_sys = 0x03007F00u,
        .lr_sys = 0x08000200u,
    };
    return c;
}

uint32_t gbat_r32(gbat *g, uint32_t a) { return bus_read(g, a, 4); }
uint32_t gbat_r16(gbat *g, uint32_t a) { return bus_read(g, a, 2) & 0xFFFFu; }
uint32_t gbat_r8(gbat *g, uint32_t a) { return bus_read(g, a, 1) & 0xFFu; }
void gbat_w32(gbat *g, uint32_t a, uint32_t v) { bus_write(g, a, 4, v); }
void gbat_w16(gbat *g, uint32_t a, uint32_t v) { bus_write(g, a, 2, v); }
void gbat_w8(gbat *g, uint32_t a, uint32_t v) { bus_write(g, a, 1, v); }

rl_gba_swi_status gbat_swi(gbat *g, unsigned n, rl_gba_swi_cpu *cpu) { return rl_gba_swi(&g->bios, &g->bus, n, cpu); }
