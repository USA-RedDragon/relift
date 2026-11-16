/**
 * @file test_n64.c
 * @brief The Nintendo 64 machine: the memory map, MI, PI DMA and its
 * timing, SP DMA, VI's line counter and interrupt, the scan-out and the
 * high-level boot. ROMs are synthetic.
 */
#include "runner.h"

#include "n64.h"

#include <stdlib.h>
#include <string.h>

typedef struct mach {
    rl_sched *s;
    rl_sched_cpu *k;
    rl_n64 *m;
    uint8_t *rom;
} mach;

static mach mach_new(size_t rom_len)
{
    mach t;
    t.rom = calloc(1, rom_len);
    for (size_t i = 0; i < rom_len; i++)
        t.rom[i] = (uint8_t)(i * 7u + (i >> 8));
    t.rom[0] = 0x80;
    t.rom[1] = 0x37;
    t.rom[2] = 0x12;
    t.rom[3] = 0x40;
    t.s = rl_sched_create(NULL);
    t.k = rl_sched_add_cpu(t.s, 1);
    t.m = rl_n64_create(t.rom, rom_len);
    rl_n64_attach(t.m, t.s, t.k);
    return t;
}

static void mach_free(mach *t)
{
    rl_n64_destroy(t->m);
    rl_sched_destroy(t->s);
    free(t->rom);
}

/** @brief Moves the CPU on by n cycles and runs the events due. */
static void advance(mach *t, uint64_t n)
{
    rl_sched_stall(t->k, n);
    rl_sched_run(t->k);
}

TEST(n64_mi_mask_and_version)
{
    mach t = mach_new(0x2000);
    CHECK_EQ(rl_n64_read(t.m, 0x04300004u, 4), 0x02020102u);
    rl_n64_write(t.m, 0x0430000Cu, 4, 0x0AAAu);
    CHECK_EQ(rl_n64_read(t.m, 0x0430000Cu, 4), 0x3Fu);
    rl_n64_write(t.m, 0x0430000Cu, 4, 0x0001u | 0x0004u);
    CHECK_EQ(rl_n64_read(t.m, 0x0430000Cu, 4), 0x3Cu);
    CHECK_EQ(rl_n64_irq_pending(t.m), 0);
    mach_free(&t);
}

TEST(n64_rdram_is_big_endian_and_narrow_register_reads_take_their_bytes)
{
    mach t = mach_new(0x2000);
    rl_n64_write(t.m, 0x100u, 4, 0x11223344u);
    CHECK_EQ(rl_n64_rdram(t.m)[0x100], 0x11);
    CHECK_EQ(rl_n64_read(t.m, 0x102u, 2), 0x3344u);
    CHECK_EQ(rl_n64_read(t.m, 0x04300004u, 1), 0x02u);
    CHECK_EQ(rl_n64_read(t.m, 0x04300007u, 1), 0x02u);
    CHECK_EQ(rl_n64_read(t.m, 0x04300006u, 2), 0x0102u);
    CHECK_EQ(rl_n64_read(t.m, 0x10000000u, 4), 0x80371240u);
    mach_free(&t);
}

TEST(n64_pi_dma_copies_the_cartridge_and_raises_its_interrupt_later)
{
    mach t = mach_new(0x10000);
    rl_n64_write(t.m, 0x04600014u, 4, 0x40);
    rl_n64_write(t.m, 0x04600018u, 4, 0x12);
    rl_n64_write(t.m, 0x0460001Cu, 4, 0x07);
    rl_n64_write(t.m, 0x04600020u, 4, 0x03);
    rl_n64_write(t.m, 0x0430000Cu, 4, 0x200u);
    rl_n64_write(t.m, 0x04600000u, 4, 0x2000u);
    rl_n64_write(t.m, 0x04600004u, 4, 0x10001000u);
    rl_n64_write(t.m, 0x0460000Cu, 4, 0xFFFu);
    CHECK_EQ(memcmp(rl_n64_rdram(t.m) + 0x2000, t.rom + 0x1000, 0x1000), 0);
    CHECK_EQ(rl_n64_read(t.m, 0x04600010u, 4) & 1u, 1u);
    CHECK_EQ(rl_n64_read(t.m, 0x04600000u, 4), 0x3000u);
    CHECK_EQ(rl_n64_read(t.m, 0x04600004u, 4), 0x10002000u);
    uint32_t pages = 0x1000u / 0x200u;
    uint32_t cycles = ((14u + 0x40u + 1u) * pages + (0x12u + 1u + 0x03u + 1u) * 0x1000u / 2u + 26u * 28u) * 3u;
    advance(&t, cycles - 1u);
    CHECK_EQ(rl_n64_irq_pending(t.m), 0);
    advance(&t, 2);
    CHECK_EQ(rl_n64_read(t.m, 0x04600010u, 4), 0x8u);
    CHECK_EQ(rl_n64_irq_pending(t.m), RL_N64_IRQ_PI);
    rl_n64_write(t.m, 0x04600010u, 4, 2u);
    CHECK_EQ(rl_n64_irq_pending(t.m), 0);
    CHECK_EQ(rl_n64_get_stats(t.m)->pi_dmas, 1);
    mach_free(&t);
}

TEST(n64_pi_registers_are_ignored_while_busy)
{
    mach t = mach_new(0x10000);
    rl_n64_write(t.m, 0x04600000u, 4, 0x2000u);
    rl_n64_write(t.m, 0x04600004u, 4, 0x10001000u);
    rl_n64_write(t.m, 0x0460000Cu, 4, 0x7Fu);
    rl_n64_write(t.m, 0x04600000u, 4, 0x4000u);
    CHECK_EQ(rl_n64_read(t.m, 0x04600000u, 4), 0x2080u);
    CHECK_EQ(rl_n64_read(t.m, 0x04600010u, 4), 0x5u);
    rl_n64_write(t.m, 0x04600010u, 4, 1u);
    CHECK_EQ(rl_n64_read(t.m, 0x04600010u, 4), 0u);
    mach_free(&t);
}

TEST(n64_sp_dma_moves_rows_between_rdram_and_dmem)
{
    mach t = mach_new(0x2000);
    for (unsigned i = 0; i < 64; i++)
        rl_n64_rdram(t.m)[0x8000 + i] = (uint8_t)i;
    rl_n64_write(t.m, 0x04040000u, 4, 0x0100u);
    rl_n64_write(t.m, 0x04040004u, 4, 0x8000u);
    rl_n64_write(t.m, 0x04040008u, 4, (8u << 20) | (1u << 12) | 15u);
    CHECK_EQ(rl_n64_spmem(t.m)[0x100], 0);
    CHECK_EQ(rl_n64_spmem(t.m)[0x10F], 15);
    CHECK_EQ(rl_n64_spmem(t.m)[0x110], 24);
    CHECK_EQ(rl_n64_read(t.m, 0x04000110u, 4), 0x18191A1Bu);
    CHECK_EQ(rl_n64_read(t.m, 0x0404001Cu, 4), 0);
    CHECK_EQ(rl_n64_read(t.m, 0x0404001Cu, 4), 1);
    rl_n64_write(t.m, 0x0404001Cu, 4, 0);
    CHECK_EQ(rl_n64_read(t.m, 0x0404001Cu, 4), 0);
    CHECK_EQ(rl_n64_read(t.m, 0x04040010u, 4) & 1u, 1u);
    mach_free(&t);
}

/** @brief The NTSC progressive setup krom's tests write. */
static void vi_ntsc(rl_n64 *m, uint32_t control, uint32_t origin, uint32_t width, uint32_t intr)
{
    rl_n64_write(m, 0x04400000u, 4, control);
    rl_n64_write(m, 0x04400004u, 4, origin);
    rl_n64_write(m, 0x04400008u, 4, width);
    rl_n64_write(m, 0x0440000Cu, 4, intr);
    rl_n64_write(m, 0x04400014u, 4, 0x03E52239u);
    rl_n64_write(m, 0x04400018u, 4, 0x20Du);
    rl_n64_write(m, 0x0440001Cu, 4, 0xC15u);
    rl_n64_write(m, 0x04400020u, 4, 0x0C150C15u);
    rl_n64_write(m, 0x04400024u, 4, 0x006C02ECu);
    rl_n64_write(m, 0x04400028u, 4, 0x002501FFu);
    rl_n64_write(m, 0x0440002Cu, 4, 0x000E0204u);
    rl_n64_write(m, 0x04400030u, 4, 0x200u);
    rl_n64_write(m, 0x04400034u, 4, 0x400u);
}

TEST(n64_vi_counts_lines_and_interrupts_at_its_line)
{
    mach t = mach_new(0x2000);
    rl_n64_write(t.m, 0x0430000Cu, 4, 0x80u);
    vi_ntsc(t.m, 0x3002u, 0x100000u, 320u, 0x200u);
    uint64_t line = (uint64_t)0xC16u * RL_N64_HZ / 48681818u;
    uint32_t seen = 0, last = 0;
    bool irq = false;
    for (unsigned i = 0; i < 600 && !irq; i++) {
        advance(&t, line);
        uint32_t v = rl_n64_read(t.m, 0x04400010u, 4);
        if (v != last)
            seen++;
        last = v;
        irq = rl_n64_irq_pending(t.m) == RL_N64_IRQ_VI;
    }
    CHECK(irq);
    CHECK_EQ(last >> 1, 0x100u);
    CHECK(seen > 200);
    rl_n64_write(t.m, 0x04400010u, 4, 0);
    CHECK_EQ(rl_n64_irq_pending(t.m), 0);
    mach_free(&t);
}

TEST(n64_scanout_reads_rgba5551_and_rgba8888)
{
    mach t = mach_new(0x2000);
    vi_ntsc(t.m, 0x3002u, 0x1000u, 320u, 0x200u);
    uint8_t *ram = rl_n64_rdram(t.m);
    ram[0x1000] = 0xF8;
    ram[0x1001] = 0x01;
    static uint8_t rgb[640 * 576 * 3];
    uint32_t w = 0, h = 0;
    CHECK(rl_n64_scanout(t.m, rgb, &w, &h));
    CHECK_EQ(w, 320);
    CHECK_EQ(h, 237);
    CHECK_EQ(rgb[0], 0xFF);
    CHECK_EQ(rgb[1], 0);
    vi_ntsc(t.m, 0x3003u, 0x2000u, 640u, 0x200u);
    rl_n64_write(t.m, 0x04400030u, 4, 0x400u);
    ram[0x2000] = 1;
    ram[0x2001] = 2;
    ram[0x2002] = 3;
    CHECK(rl_n64_scanout(t.m, rgb, &w, &h));
    CHECK_EQ(w, 640);
    CHECK_EQ(rgb[0], 1);
    CHECK_EQ(rgb[2], 3);
    rl_n64_write(t.m, 0x04400000u, 4, 0);
    CHECK(!rl_n64_scanout(t.m, rgb, &w, &h));
    mach_free(&t);
}

TEST(n64_boot_places_the_game_and_the_boot_words)
{
    mach t = mach_new(0x102000);
    t.rom[0x3E] = 'E';
    rl_n64_boot_info b = {.cic = 6102, .entry = 0x80000400u};
    rl_n64_boot(t.m, &b);
    uint8_t *ram = rl_n64_rdram(t.m);
    CHECK_EQ(memcmp(ram + 0x400, t.rom + 0x1000, 0x100000), 0);
    CHECK_EQ(rl_n64_read(t.m, 0x318u, 4), RL_N64_RDRAM_SIZE);
    CHECK_EQ(rl_n64_read(t.m, 0x300u, 4), 1u);
    CHECK_EQ(rl_n64_read(t.m, 0x04000000u, 4), 0x80371240u);
    CHECK_EQ(rl_n64_read(t.m, 0x04600014u, 4), 0x40u);
    CHECK_EQ(rl_n64_read(t.m, 0x04600018u, 4), 0x12u);
    mach_free(&t);
    t = mach_new(0x102000);
    b.cic = 6105;
    rl_n64_boot(t.m, &b);
    CHECK_EQ(rl_n64_read(t.m, 0x3F0u, 4), RL_N64_RDRAM_SIZE);
    CHECK_EQ(rl_n64_read(t.m, 0x318u, 4), 0);
    mach_free(&t);
}
