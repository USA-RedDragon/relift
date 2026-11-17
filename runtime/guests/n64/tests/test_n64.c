/**
 * @file test_n64.c
 * @brief The Nintendo 64 machine: the memory map, MI, PI DMA and its
 * timing, SP DMA, VI's line counter and interrupt, the scan-out, the CPU's
 * read latency and the high-level boot. ROMs are synthetic.
 */
#include "runner.h"

#include "n64.h"
#include "n64_int.h"

#include <stdlib.h>
#include <string.h>

/** @brief Whether the n guest bytes of RDRAM at a equal b. */
static bool ram_eq(rl_n64 *m, uint32_t a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v;
        rl_n64_rdram_get(m, a + i, &v, 1);
        if (v != b[i])
            return false;
    }
    return true;
}

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
    rl_n64_mi_lower(t.m, RL_N64_IRQ_ALL);
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

TEST(n64_every_mi_interrupt_is_up_at_power_on)
{
    uint8_t rom[0x40] = {0x80, 0x37, 0x12, 0x40};
    rl_n64 *m = rl_n64_create(rom, sizeof rom);
    CHECK_EQ(rl_n64_read(m, 0x04300008u, 4), RL_N64_IRQ_ALL);
    CHECK_EQ(rl_n64_irq_pending(m), 0);
    rl_n64_write(m, 0x0430000Cu, 4, 0x0080u);
    CHECK_EQ(rl_n64_irq_pending(m), RL_N64_IRQ_VI);
    rl_n64_write(m, 0x04400010u, 4, 0);
    CHECK_EQ(rl_n64_irq_pending(m), 0);
    rl_n64_destroy(m);
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

TEST(n64_reads_wait_for_the_rcp_and_the_pi_bus)
{
    mach t = mach_new(0x2000);
    CHECK_EQ(rl_n64_read_stall(t.m, 0x00100000u), 0);
    CHECK_EQ(rl_n64_read_stall(t.m, 0xA4400010u), 20);
    CHECK_EQ(rl_n64_read_stall(t.m, 0x04001000u), 20);
    CHECK_EQ(rl_n64_read_stall(t.m, 0x1FC007FCu), 20);
    CHECK_EQ(rl_n64_read_stall(t.m, 0x10000000u), 270);
    rl_n64_write(t.m, 0x08000000u, 4, 0x12345678u);
    CHECK_EQ(rl_n64_read_stall(t.m, 0x10000000u), 20 + 2 * 200);
    advance(&t, 150);
    CHECK_EQ(rl_n64_read_stall(t.m, 0x10000000u), 20 + 2 * 50);
    advance(&t, 100);
    CHECK_EQ(rl_n64_read_stall(t.m, 0x10000000u), 270);
    mach_free(&t);
}

TEST(n64_rdram_is_big_endian_and_narrow_register_reads_take_their_bytes)
{
    mach t = mach_new(0x2000);
    rl_n64_write(t.m, 0x100u, 4, 0x11223344u);
    CHECK_EQ(rl_n64_rdram(t.m)[(0x100) ^ 3], 0x11);
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
    CHECK(ram_eq(t.m, 0x2000, t.rom + 0x1000, 0x1000));
    CHECK_EQ(rl_n64_read(t.m, 0x04600010u, 4) & 1u, 1u);
    CHECK_EQ(rl_n64_read(t.m, 0x04600000u, 4), 0x3000u);
    CHECK_EQ(rl_n64_read(t.m, 0x04600004u, 4), 0x10002000u);
    uint32_t pages = 0x1000u / 0x200u;
    uint32_t cycles = ((14u + 0x40u + 1u) * pages + (0x12u + 1u + 0x03u + 1u) * 0x1000u / 2u + 26u * 28u) * 3u / 2u;
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
        rl_n64_rdram(t.m)[(0x8000 + i) ^ 3] = (uint8_t)i;
    rl_n64_write(t.m, 0x04040000u, 4, 0x0100u);
    rl_n64_write(t.m, 0x04040004u, 4, 0x8000u);
    rl_n64_write(t.m, 0x04040008u, 4, (8u << 20) | (1u << 12) | 15u);
    CHECK_EQ(rl_n64_read(t.m, 0x04040018u, 4), 1u);
    CHECK_EQ(rl_n64_spmem(t.m)[0x101], 0);
    advance(&t, 64);
    CHECK_EQ(rl_n64_read(t.m, 0x04040018u, 4), 0u);
    CHECK_EQ(rl_n64_read(t.m, 0x04040008u, 4), 0x800FF8u);
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
typedef struct rdp_seen {
    unsigned commands, words, syncs;
    uint32_t first[2];
} rdp_seen;

static void rdp_command(void *user, const uint32_t *w, unsigned n)
{
    rdp_seen *r = user;
    if (!r->commands++) {
        r->first[0] = w[0];
        r->first[1] = w[1];
    }
    r->words += n;
}

static void rdp_sync(void *user) { ((rdp_seen *)user)->syncs++; }

static void put64(rl_n64 *m, uint32_t a, uint64_t v)
{
    for (int i = 0; i < 8; i++) {
        uint8_t b = (uint8_t)(v >> (56 - 8 * i));
        rl_n64_rdram_put(m, a + (uint32_t)i, &b, 1);
    }
}

TEST(n64_dp_runs_whole_commands_and_raises_its_interrupt_at_sync_full)
{
    static const rl_n64_rdp_ops ops = {rdp_command, rdp_sync, NULL, NULL, NULL, NULL};
    mach t = mach_new(0x2000);
    rdp_seen r = {0};
    rl_n64_set_rdp(t.m, &ops, &r);
    put64(t.m, 0x1000, 0x3600000000000000ull);
    put64(t.m, 0x1008, 0x0800000000000001ull);
    put64(t.m, 0x1010, 0);
    put64(t.m, 0x1018, 0);
    put64(t.m, 0x1020, 0);
    put64(t.m, 0x1028, 0x2900000000000000ull);
    rl_n64_write(t.m, 0x04100000u, 4, 0x1000u);
    rl_n64_write(t.m, 0x04100004u, 4, 0x1018u);
    CHECK_EQ(r.commands, 1);
    CHECK_EQ(r.syncs, 0);
    CHECK_EQ(rl_n64_read(t.m, 0x04100008u, 4), 0x1018u);
    CHECK_EQ(rl_n64_read(t.m, 0x04300008u, 4) & 0x20u, 0);
    CHECK_EQ(rl_n64_read(t.m, 0x0410000Cu, 4) & 0xE8u, 0xA8u);
    rl_n64_write(t.m, 0x04100004u, 4, 0x1030u);
    CHECK_EQ(r.commands, 3);
    CHECK_EQ(r.words, 2 + 8 + 2);
    CHECK_EQ(r.first[0], 0x36000000u);
    CHECK_EQ(r.syncs, 1);
    CHECK_EQ(rl_n64_read(t.m, 0x04100008u, 4), 0x1030u);
    CHECK_EQ(rl_n64_read(t.m, 0x04300008u, 4) & 0x20u, 0x20u);
    CHECK_EQ(rl_n64_read(t.m, 0x0410000Cu, 4) & 0xE8u, 0x80u);
    CHECK_EQ(rl_n64_get_stats(t.m)->dp_submits, 2);
    mach_free(&t);
}

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

typedef struct vi_seen {
    uint32_t regs[14];
    unsigned writes, frames;
    uint8_t picture[2 * 1 * 4];
} vi_seen;

static void vi_reg(void *user, unsigned index, uint32_t value)
{
    vi_seen *v = user;
    v->regs[index] = value;
    v->writes++;
}

static void vi_frame(void *user, unsigned field)
{
    (void)field;
    ((vi_seen *)user)->frames++;
}

static const uint8_t *vi_picture(void *user, uint32_t *w, uint32_t *h)
{
    vi_seen *v = user;
    if (!v->frames)
        return NULL;
    *w = 2;
    *h = 1;
    return v->picture;
}

TEST(n64_a_renderer_with_a_vi_side_sees_every_register_and_frame_and_gives_the_picture)
{
    static const rl_n64_rdp_ops ops = {rdp_command, rdp_sync, NULL, vi_reg, vi_frame, vi_picture};
    mach t = mach_new(0x2000);
    vi_seen v = {.picture = {1, 2, 3, 0xFF, 4, 5, 6, 0xFF}};
    rl_n64_set_rdp(t.m, &ops, &v);
    static uint8_t rgb[640 * 576 * 3];
    uint32_t w = 0, h = 0;
    vi_ntsc(t.m, 0x3002u, 0x100000u, 320u, 0x200u);
    CHECK_EQ(v.writes, 13);
    CHECK_EQ(v.regs[1], 0x100000u);
    CHECK_EQ(v.regs[10], 0x002501FFu);
    CHECK(!rl_n64_scanout(t.m, rgb, 640 * 576, &w, &h));
    advance(&t, RL_N64_HZ / 30u);
    CHECK(v.frames >= 1);
    rgb[0] = 0;
    CHECK(!rl_n64_scanout(t.m, rgb, 1, &w, &h));
    CHECK_EQ(w * h, 2);
    CHECK_EQ(rgb[0], 0);
    CHECK(rl_n64_scanout(t.m, rgb, 2, &w, &h));
    CHECK_EQ(w, 2);
    CHECK_EQ(h, 1);
    CHECK_EQ(rgb[0], 1);
    CHECK_EQ(rgb[3], 4);
    CHECK_EQ(rgb[5], 6);
    mach_free(&t);
}

TEST(n64_framebuffer_reads_rgba5551_and_rgba8888_raw)
{
    mach t = mach_new(0x2000);
    vi_ntsc(t.m, 0x3002u, 0x1000u, 320u, 0x200u);
    uint8_t *ram = rl_n64_rdram(t.m);
    ram[(0x1000) ^ 3] = 0xF8;
    ram[(0x1001) ^ 3] = 0x01;
    static uint8_t rgb[640 * 576 * 3];
    uint32_t w = 0, h = 0;
    CHECK(rl_n64_framebuffer(t.m, rgb, &w, &h));
    CHECK_EQ(w, 320);
    CHECK_EQ(h, 237);
    CHECK_EQ(rgb[0], 0xFF);
    CHECK_EQ(rgb[1], 0);
    vi_ntsc(t.m, 0x3003u, 0x2000u, 640u, 0x200u);
    rl_n64_write(t.m, 0x04400030u, 4, 0x400u);
    ram[(0x2000) ^ 3] = 1;
    ram[(0x2001) ^ 3] = 2;
    ram[(0x2002) ^ 3] = 3;
    CHECK(rl_n64_framebuffer(t.m, rgb, &w, &h));
    CHECK_EQ(w, 640);
    CHECK_EQ(rgb[0], 1);
    CHECK_EQ(rgb[2], 3);
    rl_n64_write(t.m, 0x04400000u, 4, 0);
    CHECK(!rl_n64_framebuffer(t.m, rgb, &w, &h));
    mach_free(&t);
}

/** @brief The RGB of output pixel (x, y). */
static uint32_t out_px(const uint8_t *rgb, uint32_t w, uint32_t x, uint32_t y)
{
    const uint8_t *p = rgb + ((size_t)y * w + x) * 3u;
    return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}

TEST(n64_scanout_places_the_picture_in_the_ntsc_output)
{
    mach t = mach_new(0x2000);
    static uint8_t rgb[640 * 576 * 3];
    uint32_t w = 0, h = 0;
    CHECK(!rl_n64_scanout(t.m, rgb, 640 * 576, &w, &h));
    vi_ntsc(t.m, 0x3002u, 0x1000u, 320u, 0x200u);
    uint8_t *ram = rl_n64_rdram(t.m);
    ram[(0x1008) ^ 3] = 0xF8;
    ram[(0x1009) ^ 3] = 0x01;
    CHECK(rl_n64_scanout(t.m, rgb, 640 * 576, &w, &h));
    CHECK_EQ(w, 640);
    CHECK_EQ(h, 480);
    CHECK_EQ(out_px(rgb, w, 8u, 3u), 0xFF0000u);
    CHECK_EQ(out_px(rgb, w, 9u, 3u), 0xFF0000u);
    CHECK_EQ(out_px(rgb, w, 7u, 3u), 0u);
    CHECK_EQ(out_px(rgb, w, 10u, 3u), 0u);
    CHECK_EQ(out_px(rgb, w, 8u, 2u), 0u);
    mach_free(&t);
}

TEST(n64_scanout_scales_with_the_scale_registers)
{
    mach t = mach_new(0x2000);
    static uint8_t rgb[640 * 576 * 3];
    uint32_t w = 0, h = 0;
    vi_ntsc(t.m, 0x3003u, 0x2000u, 640u, 0x200u);
    uint8_t *ram = rl_n64_rdram(t.m);
    for (unsigned i = 0; i < 32; i++) {
        ram[(0x2000 + i * 4 + 0) ^ 3] = (uint8_t)(10 + i);
        ram[(0x2000 + i * 4 + 1) ^ 3] = 0;
        ram[(0x2000 + i * 4 + 2) ^ 3] = 0;
    }
    rl_n64_write(t.m, 0x04400030u, 4, 0x400u);
    CHECK(rl_n64_scanout(t.m, rgb, 640 * 576, &w, &h));
    CHECK_EQ(out_px(rgb, w, 8u, 3u), 18u << 16);
    CHECK_EQ(out_px(rgb, w, 9u, 3u), 19u << 16);
    CHECK_EQ(out_px(rgb, w, 13u, 3u), 23u << 16);
    rl_n64_write(t.m, 0x04400030u, 4, 0x200u);
    CHECK(rl_n64_scanout(t.m, rgb, 640 * 576, &w, &h));
    CHECK_EQ(out_px(rgb, w, 8u, 3u), 14u << 16);
    CHECK_EQ(out_px(rgb, w, 9u, 3u), 14u << 16);
    CHECK_EQ(out_px(rgb, w, 10u, 3u), 15u << 16);
    CHECK_EQ(out_px(rgb, w, 15u, 3u), 17u << 16);
    mach_free(&t);
}

TEST(n64_scanout_of_an_interlaced_picture_draws_one_field_per_frame)
{
    mach t = mach_new(0x2000);
    static uint8_t rgb[640 * 576 * 3];
    uint32_t w = 0, h = 0;
    vi_ntsc(t.m, 0x3003u | 0x40u, 0x2000u, 640u, 0x200u);
    rl_n64_write(t.m, 0x04400018u, 4, 0x20Cu);
    uint8_t *ram = rl_n64_rdram(t.m);
    for (unsigned i = 0; i < 640u * 480u; i++) {
        ram[(0x2000 + i * 4u) ^ 3] = (uint8_t)(i / 640u);
        ram[(0x2000 + i * 4u + 2u) ^ 3] = 1;
    }
    rl_n64_write(t.m, 0x04400030u, 4, 0x400u);
    CHECK(rl_n64_scanout(t.m, rgb, 640 * 576, &w, &h));
    uint32_t drawn = 0, blank = 0;
    for (uint32_t y = 4; y < 20; y++) {
        if (out_px(rgb, w, 20, y))
            drawn++;
        else
            blank++;
    }
    CHECK_EQ(drawn, 8);
    CHECK_EQ(blank, 8);
    mach_free(&t);
}

TEST(n64_pal_roms_scan_out_640_by_576)
{
    mach t = mach_new(0x2000);
    t.rom[0x3E] = 'P';
    rl_n64_destroy(t.m);
    rl_sched_destroy(t.s);
    t.s = rl_sched_create(NULL);
    t.k = rl_sched_add_cpu(t.s, 1);
    t.m = rl_n64_create(t.rom, 0x2000);
    rl_n64_attach(t.m, t.s, t.k);
    rl_n64_mi_lower(t.m, RL_N64_IRQ_ALL);
    static uint8_t rgb[640 * 576 * 3];
    uint32_t w = 0, h = 0;
    vi_ntsc(t.m, 0x3002u, 0x1000u, 320u, 0x200u);
    CHECK(rl_n64_scanout(t.m, rgb, 640 * 576, &w, &h));
    CHECK_EQ(w, 640);
    CHECK_EQ(h, 576);
    mach_free(&t);
}

TEST(n64_boot_places_the_game_and_the_boot_words)
{
    mach t = mach_new(0x102000);
    t.rom[0x3E] = 'E';
    rl_n64_boot_info b = {.cic = 6102, .entry = 0x80000400u};
    rl_n64_boot(t.m, &b);
    CHECK(ram_eq(t.m, 0x400, t.rom + 0x1000, 0x100000));
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

TEST(n64_dma_sees_rdram_not_the_cpus_dirty_lines_and_the_cpu_sees_its_lines_until_dropped)
{
    mach t = mach_new(0x2000);
    static uint32_t tags[RL_N64_DCACHE_LINES];
    memset(tags, 0, sizeof tags);
    rl_n64_set_dcache(t.m, tags);
    uint8_t *ram = rl_n64_rdram(t.m);
    const uint8_t old[4] = {1, 2, 3, 4}, dma[4] = {9, 9, 9, 9};
    uint8_t got[4];
    rl_n64_rdram_put(t.m, 0x2100u, old, 4);

    uint32_t line = 0x2000u | RL_N64_DCACHE_VALID | RL_N64_DCACHE_DIRTY;
    rl_n64_dline_changed(t.m, 0x010u, 0, line, true);
    tags[0x010] = line;
    ram[0x2100u ^ 3u] = 0xAA;
    rl_n64_rdram_get(t.m, 0x2100u, got, 4);
    CHECK(memcmp(got, old, 4) == 0);

    rl_n64_rdram_put(t.m, 0x2104u, dma, 4);
    CHECK_EQ(ram[0x2104u ^ 3u], 0);
    rl_n64_rdram_get(t.m, 0x2104u, got, 4);
    CHECK(memcmp(got, dma, 4) == 0);

    rl_n64_dline_changed(t.m, 0x010u, line, line & ~RL_N64_DCACHE_DIRTY, true);
    tags[0x010] = line & ~RL_N64_DCACHE_DIRTY;
    rl_n64_rdram_get(t.m, 0x2100u, got, 4);
    CHECK_EQ(got[0], 0xAA);
    CHECK_EQ(ram[0x2104u ^ 3u], 0);

    rl_n64_rdram_put(t.m, 0x2108u, dma, 4);
    CHECK_EQ(ram[0x2108u ^ 3u], 0);
    rl_n64_rdram_get(t.m, 0x2108u, got, 4);
    CHECK(memcmp(got, dma, 4) == 0);
    rl_n64_dline_changed(t.m, 0x010u, tags[0x010], 0, true);
    tags[0x010] = 0;
    CHECK_EQ(ram[0x2108u ^ 3u], 9);
    CHECK_EQ(ram[0x2100u ^ 3u], 0xAA);

    rl_n64_dline_changed(t.m, 0x010u, 0, line, true);
    tags[0x010] = line;
    ram[0x210Cu ^ 3u] = 0x55;
    rl_n64_dline_changed(t.m, 0x010u, line, line & ~RL_N64_DCACHE_VALID, false);
    tags[0x010] = 0;
    CHECK_EQ(ram[0x210Cu ^ 3u], 0);

    rl_n64_set_dcache(t.m, NULL);
    mach_free(&t);
}
