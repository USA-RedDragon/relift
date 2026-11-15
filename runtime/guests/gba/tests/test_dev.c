/**
 * @file test_dev.c
 * @brief Bus timing and the prefetch buffer, the IRQ line, HALTCNT through
 * the BIOS, the DMA corner cases and the display hook.
 */
#include "mach.h"

static unsigned cost_of_fetch(mach *m, uint32_t addr, unsigned size, bool seq)
{
    uint64_t c = rl_gba_cycles(m->g);
    rl_gba_fetch(m->g, addr, size, seq);
    return (unsigned)(rl_gba_cycles(m->g) - c);
}

TEST(wait_table_follows_waitcnt)
{
    mach m = mach_new();
    const rl_gba_waits *t = rl_gba_wait_table(m.g);
    CHECK_EQ(rl_gba_wait(t, 0x08000000u, 2, false), 5u);
    CHECK_EQ(rl_gba_wait(t, 0x08000000u, 2, true), 3u);
    CHECK_EQ(rl_gba_wait(t, 0x08000000u, 4, false), 8u);
    CHECK_EQ(rl_gba_wait(t, 0x0A000000u, 2, true), 5u);
    CHECK_EQ(rl_gba_wait(t, 0x0C000000u, 2, true), 9u);
    CHECK_EQ(rl_gba_wait(t, 0x0E000000u, 1, false), 5u);
    CHECK_EQ(rl_gba_wait(t, 0x02000000u, 4, false), 6u);
    CHECK_EQ(rl_gba_wait(t, 0x03000000u, 4, false), 1u);
    CHECK_EQ(rl_gba_wait(t, 0x05000000u, 4, false), 2u);
    CHECK_EQ(rl_gba_wait(t, 0x07000000u, 4, false), 1u);
    CHECK_EQ(rl_gba_wait(t, 0x20000000u, 4, false), 1u);
    mach_wr(&m, 10, 0x04000204u, 2, 0x4317u);
    CHECK_EQ(rl_gba_wait(t, 0x08000000u, 2, false), 4u);
    CHECK_EQ(rl_gba_wait(t, 0x08000000u, 2, true), 2u);
    CHECK_EQ(rl_gba_wait(t, 0x08000000u, 4, true), 4u);
    CHECK_EQ(rl_gba_wait(t, 0x0A000000u, 2, false), 5u);
    CHECK_EQ(rl_gba_wait(t, 0x0C000000u, 2, false), 9u);
    CHECK_EQ(rl_gba_wait(t, 0x0E000000u, 1, false), 9u);
    mach_free(&m);
}

TEST(game_pak_fetches_without_prefetch_pay_n_then_s)
{
    mach m = mach_new();
    CHECK_EQ(cost_of_fetch(&m, 0x08000000u, 4, false), 8u);
    CHECK_EQ(cost_of_fetch(&m, 0x08000004u, 4, true), 6u);
    CHECK_EQ(cost_of_fetch(&m, 0x08000008u, 2, true), 3u);
    CHECK_EQ(cost_of_fetch(&m, 0x08000100u, 2, false), 5u);
    CHECK_EQ(cost_of_fetch(&m, 0x0801FFFEu, 2, false), 5u);
    CHECK_EQ(cost_of_fetch(&m, 0x08020000u, 2, true), 5u);
    CHECK_EQ(cost_of_fetch(&m, 0x08020002u, 2, true), 3u);
    rl_gba_load(m.g, 0x03000000u, 4, false);
    CHECK_EQ(cost_of_fetch(&m, 0x08020004u, 2, true), 5u);
    CHECK_EQ(rl_gba_fetch(m.g, 0x08000010u, 2, false), 0x1008u);
    CHECK_EQ(cost_of_fetch(&m, 0x03000000u, 4, false), 1u);
    CHECK_EQ(cost_of_fetch(&m, 0x02000000u, 4, false), 6u);
    mach_free(&m);
}

TEST(the_prefetch_buffer_fills_while_the_bus_is_free)
{
    mach m = mach_new();
    mach_wr(&m, 10, 0x04000204u, 2, 0x4014u);
    CHECK_EQ(cost_of_fetch(&m, 0x08000000u, 2, false), 4u);
    rl_gba_idle(m.g, 6);
    CHECK_EQ(cost_of_fetch(&m, 0x08000002u, 2, true), 1u);
    CHECK_EQ(cost_of_fetch(&m, 0x08000004u, 2, true), 1u);
    CHECK_EQ(cost_of_fetch(&m, 0x08000006u, 2, true), 1u);
    CHECK_EQ(cost_of_fetch(&m, 0x08000008u, 2, true), 1u);
    rl_gba_idle(m.g, 40);
    unsigned total = 0;
    for (uint32_t a = 0x0800000Au; a < 0x0800001Au; a += 2)
        total += cost_of_fetch(&m, a, 2, true);
    CHECK_EQ(total, 8u);
    CHECK_EQ(cost_of_fetch(&m, 0x0800001Au, 2, true), 4u);
    rl_gba_idle(m.g, 3);
    uint64_t c = rl_gba_cycles(m.g);
    CHECK_EQ(rl_gba_load(m.g, 0x08000400u, 2, false), 0x1200u);
    CHECK_EQ(rl_gba_cycles(m.g) - c, 5u);
    CHECK_EQ(cost_of_fetch(&m, 0x0800001Cu, 2, false), 4u);
    mach_free(&m);
}

TEST(the_irq_line_rises_two_cycles_after_the_request)
{
    mach m = mach_new();
    mach_wr(&m, 10, 0x04000200u, 2, 0x0001u);
    mach_wr(&m, 12, 0x04000208u, 2, 1u);
    mach_to(&m, 20);
    CHECK(!rl_gba_irq_line(m.g));
    rl_gba_request_irq(m.g, RL_GBA_IRQ_VBLANK);
    mach_to(&m, 21);
    CHECK(!rl_gba_irq_line(m.g));
    CHECK_EQ(mach_if(&m, 21), 1u);
    mach_to(&m, 22);
    CHECK(rl_gba_irq_line(m.g));
    CHECK(rl_sched_pending(m.c) & (1ull << RL_GBA_LINE_IRQ));
    mach_wr(&m, 30, 0x04000208u, 2, 0u);
    mach_to(&m, 31);
    CHECK(rl_gba_irq_line(m.g));
    mach_to(&m, 32);
    CHECK(!rl_gba_irq_line(m.g));
    mach_wr(&m, 40, 0x04000208u, 2, 1u);
    mach_wr(&m, 41, 0x04000202u, 2, 1u);
    mach_to(&m, 50);
    CHECK(!rl_gba_irq_line(m.g));
    CHECK_EQ(mach_if(&m, 50), 0u);
    mach_free(&m);
}

TEST(the_cpu_sees_the_irq_line_as_it_was_when_its_last_access_began)
{
    mach m = mach_new();
    mach_wr(&m, 10, 0x04000200u, 2, 0x0001u);
    mach_wr(&m, 12, 0x04000208u, 2, 1u);
    mach_to(&m, 50);
    rl_gba_request_irq(m.g, RL_GBA_IRQ_VBLANK);
    mach_to(&m, 51);
    rl_gba_store(m.g, 0x02000000u, 2, 0, false);
    CHECK_EQ(mach_now(&m), 54u);
    CHECK(!rl_gba_irq_sample(m.g));
    CHECK(rl_gba_irq_line(m.g));
    rl_gba_fetch(m.g, 0x03000000u, 4, false);
    CHECK(rl_gba_irq_sample(m.g));
    rl_gba_write(m.g, 0x04000202u, 2, 1u);
    mach_to(&m, 100);
    rl_gba_request_irq(m.g, RL_GBA_IRQ_VBLANK);
    mach_to(&m, 102);
    rl_gba_store(m.g, 0x02000000u, 2, 0, false);
    CHECK(rl_gba_irq_sample(m.g));
    rl_gba_write(m.g, 0x04000202u, 2, 1u);
    mach_to(&m, 200);
    rl_gba_request_irq(m.g, RL_GBA_IRQ_VBLANK);
    rl_gba_idle(m.g, 2);
    CHECK(!rl_gba_irq_sample(m.g));
    rl_gba_idle(m.g, 1);
    CHECK(rl_gba_irq_sample(m.g));
    mach_free(&m);
}

TEST(only_the_bios_halts_and_the_halt_ends_at_the_interrupt)
{
    mach m = mach_new();
    mach_wr(&m, 4, 0x04000200u, 2, 0x0008u);
    mach_wr(&m, 10, 0x04000100u, 4, 0x00C0FF00u);
    mach_wr(&m, 20, 0x04000301u, 1, 0u);
    CHECK_EQ(mach_now(&m), 20u);
    const rl_gba_bus *b = rl_gba_bios_bus(m.g);
    b->write(b->user, 0x04000301u, 1, 0u);
    CHECK_EQ(mach_now(&m), 271u);
    CHECK(!rl_gba_halted(m.g));
    CHECK_EQ(mach_if(&m, 272), 0x0008u);
    mach_free(&m);
}

TEST(a_halt_nothing_can_end_returns)
{
    mach m = mach_new();
    mach_to(&m, 5);
    const rl_gba_bus *b = rl_gba_bios_bus(m.g);
    b->write(b->user, 0x04000301u, 1, 0u);
    CHECK(!rl_gba_halted(m.g));
    mach_free(&m);
}

typedef struct hook_log {
    unsigned calls;
    rl_gba *g;
} hook_log;

static void on_irq(void *user)
{
    hook_log *h = user;
    h->calls++;
    rl_gba_write(h->g, 0x04000202u, 2, 0xFFFFu);
}

TEST(the_bios_bus_calls_the_cpu_hook_when_an_interrupt_may_enter)
{
    mach m = mach_new();
    hook_log h = {0, m.g};
    rl_gba_set_cpu_hooks(m.g, &(rl_gba_cpu_hooks){.user = &h, .irq = on_irq});
    const rl_gba_bus *b = rl_gba_bios_bus(m.g);
    mach_wr(&m, 4, 0x04000200u, 2, 0x0008u);
    mach_wr(&m, 10, 0x04000100u, 4, 0x00C0FF00u);
    b->write(b->user, 0x04000208u, 2, 1u);
    CHECK_EQ(h.calls, 0u);
    b->write(b->user, 0x04000301u, 1, 0u);
    CHECK_EQ(h.calls, 1u);
    rl_gba_set_cpu_hooks(m.g, NULL);
    mach_free(&m);
}

TEST(an_immediate_dma_waits_two_cycles_and_stalls_the_cpu)
{
    mach m = mach_new();
    for (unsigned i = 0; i < 4; i++)
        rl_gba_write(m.g, 0x03001000u + 4 * i, 4, 0x10101010u * (i + 1));
    mach_wr(&m, 100, 0x040000D4u, 4, 0x03001000u);
    mach_wr(&m, 100, 0x040000D8u, 4, 0x02000000u);
    mach_wr(&m, 100, 0x040000DCu, 4, 0xC4000004u);
    uint64_t c = rl_gba_cycles(m.g);
    mach_to(&m, 101);
    CHECK_EQ(rl_gba_cycles(m.g), c);
    mach_to(&m, 102);
    CHECK_EQ(rl_gba_cycles(m.g) - c, 2u + 4u * (1u + 6u));
    CHECK_EQ(rl_gba_read(m.g, 0x0200000Cu, 4), 0x40404040u);
    CHECK_EQ(rl_gba_read(m.g, 0x040000DEu, 2), 0x4400u);
    CHECK_EQ(mach_if(&m, mach_now(&m) + 1), RL_GBA_IRQ_DMA0 << 3);
    mach_free(&m);
}

TEST(a_dma_runs_alongside_the_cpus_internal_cycles)
{
    mach m = mach_new();
    mach_wr(&m, 100, 0x040000D4u, 4, 0x03001000u);
    mach_wr(&m, 100, 0x040000D8u, 4, 0x02000000u);
    mach_wr(&m, 100, 0x040000DCu, 4, 0xC4000004u);
    rl_gba_idle(m.g, 40);
    CHECK_EQ(mach_now(&m), 140u);
    CHECK_EQ(rl_gba_read(m.g, 0x040000DEu, 2), 0x4400u);
    mach_wr(&m, 200, 0x040000DCu, 4, 0xC4000004u);
    rl_gba_idle(m.g, 10);
    CHECK_EQ(mach_now(&m), 232u);
    mach_free(&m);
}

TEST(disabling_a_triggered_dma_lowers_its_line)
{
    mach m = mach_new();
    mach_wr(&m, 10, 0x040000B0u, 4, 0x03001000u);
    mach_wr(&m, 10, 0x040000B4u, 4, 0x02000000u);
    mach_wr(&m, 10, 0x040000B8u, 4, 0xA2000001u);
    rl_sched_stall(m.c, 1232);
    rl_sched_run(m.c);
    CHECK(rl_sched_pending(m.c) & (1ull << RL_GBA_LINE_DMA));
    rl_gba_write(m.g, 0x040000BAu, 2, 0);
    CHECK(!(rl_sched_pending(m.c) & (1ull << RL_GBA_LINE_DMA)));
    mach_free(&m);
}

TEST(a_dma_from_the_bios_region_writes_its_last_value)
{
    mach m = mach_new();
    rl_gba_write(m.g, 0x03001000u, 4, 0xCAFEBABEu);
    mach_wr(&m, 10, 0x040000D4u, 4, 0x03001000u);
    mach_wr(&m, 10, 0x040000D8u, 4, 0x02000000u);
    mach_wr(&m, 10, 0x040000DCu, 4, 0x84000001u);
    mach_to(&m, 20);
    mach_wr(&m, 40, 0x040000D4u, 4, 0x00000100u);
    mach_wr(&m, 40, 0x040000D8u, 4, 0x02000010u);
    mach_wr(&m, 40, 0x040000DCu, 4, 0x84000002u);
    mach_to(&m, 50);
    CHECK_EQ(rl_gba_read(m.g, 0x02000010u, 4), 0xCAFEBABEu);
    CHECK_EQ(rl_gba_read(m.g, 0x02000014u, 4), 0xCAFEBABEu);
    mach_free(&m);
}

TEST(a_zero_length_moves_the_channels_maximum)
{
    mach m = mach_new();
    mach_wr(&m, 10, 0x040000B0u, 4, 0x03000000u);
    mach_wr(&m, 10, 0x040000B4u, 4, 0x02000000u);
    mach_wr(&m, 10, 0x040000B8u, 4, 0x81000000u);
    uint64_t c = rl_gba_cycles(m.g);
    mach_to(&m, 20);
    CHECK_EQ(rl_gba_cycles(m.g) - c, 2u + 0x4000u * (1u + 3u));
    mach_free(&m);
}

TEST(the_sound_fifo_request_moves_four_words)
{
    mach m = mach_new();
    for (unsigned i = 0; i < 8; i++)
        rl_gba_write(m.g, 0x03002000u + 4 * i, 4, i + 1);
    mach_wr(&m, 10, 0x040000BCu, 4, 0x03002000u);
    mach_wr(&m, 10, 0x040000C0u, 4, 0x040000A0u);
    mach_wr(&m, 10, 0x040000C4u, 4, 0xB6000000u);
    mach_to(&m, 30);
    CHECK_EQ(rl_gba_read(m.g, 0x040000A0u, 4), 0u);
    rl_gba_dma_fifo(m.g, 0);
    uint64_t c = rl_gba_cycles(m.g);
    mach_to(&m, 31);
    CHECK_EQ(rl_gba_cycles(m.g), c);
    mach_to(&m, 32);
    CHECK_EQ(rl_gba_cycles(m.g) - c, 2u + 4u * 2u);
    CHECK_EQ(rl_gba_read(m.g, 0x040000A0u, 4), 4u);
    rl_gba_dma_fifo(m.g, 0);
    mach_to(&m, mach_now(&m) + 2);
    CHECK_EQ(rl_gba_read(m.g, 0x040000A0u, 4), 8u);
    CHECK_EQ(rl_gba_read(m.g, 0x040000C6u, 2) & 0x8000u, 0x8000u);
    rl_gba_dma_fifo(m.g, 1);
    mach_to(&m, mach_now(&m) + 4);
    CHECK_EQ(rl_gba_read(m.g, 0x040000A0u, 4), 8u);
    mach_free(&m);
}

typedef struct frame_log {
    unsigned n;
    uint64_t when;
    unsigned line;
} frame_log;

static void on_vblank(void *user, unsigned line, uint64_t when)
{
    frame_log *f = user;
    f->n++;
    f->when = when;
    f->line = line;
}

TEST(the_vblank_hook_runs_once_a_frame)
{
    mach m = mach_new();
    frame_log f = {0};
    rl_gba_set_vblank_hook(m.g, on_vblank, &f);
    mach_to(&m, 160u * RL_GBA_LINE_CYCLES);
    CHECK_EQ(f.n, 0u);
    mach_to(&m, 160u * RL_GBA_LINE_CYCLES + 1);
    CHECK_EQ(f.n, 1u);
    CHECK_EQ(f.when, 160u * RL_GBA_LINE_CYCLES + 1);
    CHECK_EQ(f.line, 160u);
    CHECK_EQ(rl_gba_vcount(m.g), 160u);
    mach_to(&m, 3u * RL_GBA_FRAME_CYCLES);
    CHECK_EQ(f.n, 3u);
    rl_gba_lcd_set(m.g, 159, 1231);
    CHECK_EQ(rl_gba_vcount(m.g), 159u);
    mach_to(&m, mach_now(&m) + 2);
    CHECK_EQ(f.n, 4u);
    rl_gba_set_vblank_hook(m.g, NULL, NULL);
    mach_free(&m);
}

TEST(timer_reads_follow_the_clock_without_events)
{
    mach m = mach_new();
    mach_wr(&m, 1000, 0x04000100u, 4, 0x00830000u);
    CHECK_EQ(mach_rd(&m, 1000u + 1024u * 5u, 0x04000100u, 2), 5u);
    mach_wr(&m, 9000, 0x0400010Cu, 4, 0x00840000u);
    mach_wr(&m, 9000, 0x04000108u, 4, 0x0080FFFEu);
    CHECK_EQ(mach_rd(&m, 9204, 0x0400010Cu, 2), 101u);
    CHECK_EQ(mach_rd(&m, 9205, 0x04000108u, 2), 0xFFFFu);
    mach_free(&m);
}
