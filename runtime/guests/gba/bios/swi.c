/**
 * @file swi.c
 * @brief The SWI dispatcher and its stack frames, the bus accesses, the IRQ
 * stub, the post-boot state and the small calls (resets, halts, checksum).
 */
#include "bios_int.h"

static unsigned access_cycles(bios_call *c, uint32_t a, unsigned size, bool seq)
{
    return c->bus->cycles ? c->bus->cycles(c->bus->user, a, size, seq) : 1u;
}

static uint32_t rd(bios_call *c, uint32_t a, unsigned size, bool seq)
{
    c->cyc += access_cycles(c, a, size, seq);
    return c->bus->read(c->bus->user, a, size);
}

static void wr(bios_call *c, uint32_t a, unsigned size, uint32_t v, bool seq)
{
    c->cyc += access_cycles(c, a, size, seq);
    c->bus->write(c->bus->user, a, size, v);
}

static uint32_t ror(uint32_t v, unsigned n) { return n ? (v >> n) | (v << (32 - n)) : v; }

uint32_t bios_ld32(bios_call *c, uint32_t a, bool seq) { return ror(rd(c, a & ~3u, 4, seq), (a & 3u) * 8); }

uint32_t bios_ldm(bios_call *c, uint32_t a, bool seq) { return rd(c, a & ~3u, 4, seq); }

uint32_t bios_ld16(bios_call *c, uint32_t a, bool seq) { return ror(rd(c, a & ~1u, 2, seq) & 0xFFFFu, (a & 1u) * 8); }

uint32_t bios_ld16s(bios_call *c, uint32_t a, bool seq)
{
    uint32_t v = rd(c, a & ~1u, 2, seq) & 0xFFFFu;
    if (a & 1u)
        return (uint32_t)(int32_t)(int8_t)(v >> 8);
    return (uint32_t)(int32_t)(int16_t)v;
}

uint32_t bios_ld8(bios_call *c, uint32_t a, bool seq) { return rd(c, a, 1, seq) & 0xFFu; }

void bios_st32(bios_call *c, uint32_t a, uint32_t v, bool seq) { wr(c, a & ~3u, 4, v, seq); }

void bios_st16(bios_call *c, uint32_t a, uint32_t v, bool seq) { wr(c, a & ~1u, 2, v & 0xFFFFu, seq); }

void bios_st8(bios_call *c, uint32_t a, uint32_t v, bool seq) { wr(c, a, 1, v & 0xFFu, seq); }

void bios_push(bios_call *c, unsigned n, const uint32_t *v)
{
    c->sp -= 4 * n;
    for (unsigned i = 0; i < n; i++)
        bios_st32(c, c->sp + 4 * i, v[i], i != 0);
}

void bios_save(bios_call *c, unsigned first, unsigned last, bool lr)
{
    uint32_t v[10];
    unsigned n = 0;
    for (unsigned i = first; i <= last; i++)
        v[n++] = c->hi[i];
    if (lr)
        v[n++] = RL_GBA_BIOS_SWI_RETURN;
    bios_push(c, n, v);
}

void rl_gba_bios_init(rl_gba_bios *b)
{
    b->open_bus = RL_GBA_BIOS_OPEN_BOOT;
    b->cycles = 0;
}

rl_gba_boot rl_gba_bios_boot_state(void)
{
    return (rl_gba_boot){
        .pc = 0x08000000u,
        .cpsr = 0x0000001Fu,
        .sp_sys = 0x03007F00u,
        .sp_irq = 0x03007FA0u,
        .sp_svc = 0x03007FE0u,
    };
}

void rl_gba_bios_boot_io(rl_gba_bios *b, const rl_gba_bus *bus)
{
    bios_call c = {.b = b, .bus = bus};
    static const uint16_t regs[][2] = {
        {0x0000, 0x0080}, {0x0020, 0x0100}, {0x0026, 0x0100}, {0x0030, 0x0100}, {0x0036, 0x0100},
        {0x0082, 0x880E}, {0x0088, 0x0200}, {0x0134, 0x8000},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        bios_st16(&c, 0x04000000u + regs[i][0], regs[i][1], false);
    bios_st8(&c, 0x04000300u, 1, false);
    b->open_bus = RL_GBA_BIOS_OPEN_BOOT;
}

uint32_t rl_gba_bios_read(const rl_gba_bios *b, uint32_t addr, unsigned size)
{
    uint32_t v = b->open_bus >> ((addr & 3u) * 8);
    return size == 4 ? b->open_bus : size == 2 ? v & 0xFFFFu : v & 0xFFu;
}

static void halt_write(bios_call *c, uint32_t v)
{
    bios_tick(c, 3);
    bios_st8(c, 0x04000301u, v, false);
}

static void checksum(bios_call *c)
{
    c->r[0] = 0xBAAE187Fu;
    c->r[1] = 1;
    c->r[3] = 0x4000;
    bios_tick(c, 41010 - 40);
}

static void soft_reset(bios_call *c, rl_gba_swi_cpu *cpu)
{
    uint32_t flag = bios_ld8(c, 0x03007FFAu, false);
    for (uint32_t a = 0x03007E00u; a < 0x03008000u; a += 4)
        bios_st32(c, a, 0, a != 0x03007E00u);
    bios_tick(c, 0x80 * 3 + 40);
    cpu->ret = flag ? 0x02000000u : 0x08000000u;
    for (unsigned i = 0; i < 4; i++)
        c->r[i] = 0;
    c->status = RL_GBA_SWI_RESET;
}

static void custom_halt(bios_call *c, uint32_t v) { halt_write(c, v); }

static void multiboot(bios_call *c)
{
    c->r[0] = 1;
    bios_tick(c, 20);
}

static const int32_t cost_fit[0x20][2] = {
    {0, 1024},
    {194, 672},
    {0, 1024},
    {0, 1024},
    {0, 1024},
    {0, 1024},
    {-34, 1666},
    {-35, 1666},
    {-34, 1609},
    {15, 1024},
    {-22, 1739},
    {19, 1266},
    {24, 1060},
    {-6, 1024},
    {0, 1124},
    {9, 951},
    {106, 2248},
    {28, 1170},
    {-90, 2584},
    {4, 2346},
    {36, 1093},
    {42, 2118},
    {5, 1243},
    {-42, 1914},
    {5, 1243},
    {0, 1024},
    {0, 1024},
    {0, 1024},
    {0, 1024},
    {0, 1024},
    {0, 1024},
    {-10, 1024},
};

static uint64_t calibrated(unsigned number, uint64_t raw)
{
    if (number >= 0x20)
        return raw;
    int64_t v = cost_fit[number][0] + (int64_t)((raw * (uint64_t)cost_fit[number][1]) >> 10);
    return v > 0 ? (uint64_t)v : 0;
}

rl_gba_swi_status rl_gba_swi(rl_gba_bios *b, const rl_gba_bus *bus, unsigned number, rl_gba_swi_cpu *cpu)
{
    bios_call c = {.b = b, .bus = bus, .sp = cpu->sp_sys, .status = RL_GBA_SWI_OK};
    for (unsigned i = 0; i < 4; i++)
        c.r[i] = cpu->r[i];
    uint32_t r2 = cpu->r[2];
    for (unsigned i = 4; i < 13; i++)
        c.hi[i] = cpu->r[i];
    c.hi[11] = (cpu->cpsr & 0x80u) | 0x1Fu;
    uint32_t svc[4] = {cpu->cpsr, cpu->r[11], cpu->r[12], cpu->ret};
    for (unsigned i = 0; i < 4; i++)
        bios_st32(&c, cpu->sp_svc - 16 + 4 * i, svc[i], i != 0);
    uint32_t sys[2] = {r2, cpu->lr_sys};
    bios_push(&c, 2, sys);
    bios_tick(&c, 22);
    switch (number & 0xFFu) {
    case 0x00:
        soft_reset(&c, cpu);
        break;
    case 0x01:
        bios_register_ram_reset(&c);
        break;
    case 0x02:
        halt_write(&c, 0);
        break;
    case 0x03:
        halt_write(&c, 0x80);
        break;
    case 0x04:
        bios_intr_wait(&c);
        break;
    case 0x05:
        c.r[0] = 1;
        c.r[1] = 1;
        bios_intr_wait(&c);
        break;
    case 0x06:
        bios_div(&c);
        break;
    case 0x07:
        bios_div_arm(&c);
        break;
    case 0x08:
        bios_sqrt(&c);
        break;
    case 0x09:
        bios_arctan(&c);
        break;
    case 0x0A:
        bios_arctan2(&c);
        break;
    case 0x0B:
        bios_cpu_set(&c);
        break;
    case 0x0C:
        bios_cpu_fast_set(&c);
        break;
    case 0x0D:
        checksum(&c);
        break;
    case 0x0E:
        bios_bg_affine(&c);
        break;
    case 0x0F:
        bios_obj_affine(&c);
        break;
    case 0x10:
        bios_bit_unpack(&c);
        break;
    case 0x11:
        bios_lz77(&c, false);
        break;
    case 0x12:
        bios_lz77(&c, true);
        break;
    case 0x13:
        bios_huff(&c);
        break;
    case 0x14:
        bios_rl(&c, false);
        break;
    case 0x15:
        bios_rl(&c, true);
        break;
    case 0x16:
        bios_diff8(&c, false);
        break;
    case 0x17:
        bios_diff8(&c, true);
        break;
    case 0x18:
        bios_diff16(&c);
        break;
    case 0x19:
        bios_sound_bias(&c);
        break;
    case 0x1F:
        bios_midi_key2freq(&c);
        break;
    case 0x25:
        multiboot(&c);
        break;
    case 0x26:
        soft_reset(&c, cpu);
        cpu->ret = 0x08000000u;
        break;
    case 0x27:
        custom_halt(&c, c.r[2]);
        break;
    default:
        c.status = RL_GBA_SWI_UNKNOWN;
        break;
    }
    bios_tick(&c, 18);
    cpu->r[0] = c.r[0];
    cpu->r[1] = c.r[1];
    cpu->r[2] = r2;
    cpu->r[3] = c.r[3];
    b->open_bus = RL_GBA_BIOS_OPEN_SWI;
    b->cycles = calibrated(number & 0xFFu, c.cyc);
    return c.status;
}

uint32_t rl_gba_bios_irq_entry(rl_gba_bios *b, const rl_gba_bus *bus, rl_gba_irq_cpu *cpu)
{
    bios_call c = {.b = b, .bus = bus, .sp = cpu->sp};
    uint32_t v[6] = {cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3], cpu->r12, cpu->lr};
    bios_tick(&c, 4);
    bios_push(&c, 6, v);
    bios_tick(&c, 6);
    cpu->sp = c.sp;
    cpu->r[0] = 0x04000000u;
    cpu->lr = RL_GBA_BIOS_IRQ_RETURN;
    uint32_t target = bios_ld32(&c, 0x03FFFFFCu, false);
    bios_tick(&c, 3);
    b->open_bus = RL_GBA_BIOS_OPEN_IRQ;
    b->cycles = c.cyc;
    return target & ~3u;
}

uint32_t rl_gba_bios_irq_return(rl_gba_bios *b, const rl_gba_bus *bus, rl_gba_irq_cpu *cpu)
{
    bios_call c = {.b = b, .bus = bus, .sp = cpu->sp};
    uint32_t v[6];
    for (unsigned i = 0; i < 6; i++)
        v[i] = bios_ldm(&c, c.sp + 4 * i, i != 0);
    bios_tick(&c, 6);
    cpu->sp = c.sp + 24;
    for (unsigned i = 0; i < 4; i++)
        cpu->r[i] = v[i];
    cpu->r12 = v[4];
    cpu->lr = v[5];
    b->open_bus = RL_GBA_BIOS_OPEN_IRQ_DONE;
    b->cycles = c.cyc;
    return v[5] - 4;
}
