/**
 * @file test_diff.c
 * @brief Timers, the interrupt controller, DMA, HALTCNT and the display
 * timing against the NanoBoyAdvance oracle, tick by tick (see oracle.h).
 * Each scenario runs with several no-op pads, so that its reads land on
 * every cycle of the interval between them.
 */
#include "oracle.h"

#define TM0 0x04000100u
#define TM0H 0x04000102u
#define TM1 0x04000104u
#define TM1H 0x04000106u
#define TM2 0x04000108u
#define IE 0x04000200u
#define IF 0x04000202u
#define IME 0x04000208u
#define WAITCNT 0x04000204u
#define DISPSTAT 0x04000004u
#define VCOUNT 0x04000006u
#define DMA0 0x040000B0u
#define DMA3 0x040000D4u

static bool run_pads(const char *name, const dop *ops, unsigned pads, uint32_t mem, uint32_t len, bool check_stall)
{
    unsigned reads = 0, diffs = 0;
    for (unsigned pad = 0; pad < pads; pad++) {
        dres r = oracle_run(name, ops, pad, mem, len);
        if (r.skipped) {
            printf("  %s: oracle absent, skipped\n", name);
            return true;
        }
        if (r.broken) {
            printf("  %s pad %u: oracle protocol failure\n", name, pad);
            return false;
        }
        reads += r.reads;
        diffs += r.diffs;
        if (check_stall && r.stall_here != r.stall_oracle) {
            printf("  %s pad %u: DMA stall here %llu, oracle %llu\n", name, pad, (unsigned long long)r.stall_here,
                   (unsigned long long)r.stall_oracle);
            diffs++;
        }
        if (r.halt_oracle != r.halt_here) {
            printf("  %s pad %u: halt here %llu, oracle %llu\n", name, pad, (unsigned long long)r.halt_here,
                   (unsigned long long)r.halt_oracle);
            diffs++;
        }
    }
    printf("  %s: %u reads, %u differ\n", name, reads, diffs);
    return diffs == 0;
}

static const dop timer_start[] = {
    DW(4, TM0, 0x00800000u), DR(2, TM0), DR(2, TM0), DR(2, TM0), DN(1), DR(2, TM0), DR(4, TM0),
    DW(2, TM0H, 0), DR(2, TM0), DR(2, TM0), DW(2, TM0H, 0x80), DR(2, TM0), DR(2, TM0), DEND,
};

TEST(diff_timer_start_and_stop) { CHECK(run_pads("timer start", timer_start, 9, 0, 0, false)); }

static const dop timer_prescale[] = {
    DW(4, TM0, 0x0081FF00u), DW(4, TM1, 0x0082FFF0u), DW(4, TM2, 0x0083FFFEu),
    DR(2, TM0), DL(10), DR(2, TM0), DR(2, TM1), DL(100), DR(2, TM0), DR(2, TM1), DR(2, TM2),
    DL(300), DR(2, TM0), DR(2, TM1), DR(2, TM2), DL(700), DR(2, TM0), DR(2, TM1), DR(2, TM2), DEND,
};

TEST(diff_timer_prescalers) { CHECK(run_pads("timer prescalers", timer_prescale, 9, 0, 0, false)); }

static const dop timer_overflow[] = {
    DW(4, TM0, 0x00C0FFE0u), DR(2, TM0), DR(2, IF), DR(2, IF), DR(2, IF), DR(2, IF), DR(2, IF), DR(2, TM0),
    DW(2, IF, 0x0008u), DR(2, IF), DR(2, IF), DR(2, IF), DR(2, IF), DR(2, TM0), DR(2, IF), DEND,
};

TEST(diff_timer_overflow_and_irq_flag) { CHECK(run_pads("timer overflow", timer_overflow, 9, 0, 0, false)); }

static const dop timer_cascade[] = {
    DW(4, TM1, 0x00C4FFFEu), DW(4, TM0, 0x0080FFF0u), DR(2, TM1), DR(2, TM0), DR(2, TM1), DR(2, IF), DR(2, TM1),
    DR(2, IF), DR(2, TM1), DR(2, IF), DR(2, TM1), DR(2, IF), DR(2, TM1), DL(30), DR(2, TM1), DR(2, IF), DEND,
};

TEST(diff_timer_cascade) { CHECK(run_pads("timer cascade", timer_cascade, 9, 0, 0, false)); }

static const dop timer_reload[] = {
    DW(4, TM0, 0x0080FF00u), DR(2, TM0), DW(2, TM0, 0xFFF8u), DR(2, TM0), DR(2, TM0), DL(60), DR(2, TM0), DR(2, TM0),
    DW(2, TM0, 0x0000u), DL(5), DR(2, TM0), DEND,
};

TEST(diff_timer_reload_latch) { CHECK(run_pads("timer reload", timer_reload, 9, 0, 0, false)); }

static const dop timer_tick_before_reload[] = {
    DW(4, TM0, 0x0080FFFFu), DN(4), DW(2, TM0H, 0), DW(2, TM0, 0x0000u), DW(2, IF, 0xFFFFu), DR(2, TM0), DR(2, IF),
    DW(2, TM0H, 0x00C0u), DR(2, TM0), DR(2, IF), DR(2, IF), DR(2, TM0), DEND,
};

TEST(diff_timer_tick_before_reload) { CHECK(run_pads("timer tick before reload", timer_tick_before_reload, 9, 0, 0, false)); }

static const dop irq_regs[] = {
    DW(2, IE, 0x0008u), DR(2, IE), DW(2, IME, 1), DR(2, IME), DW(4, TM0, 0x0040FFFFu), DW(2, TM0H, 0xC0), DR(2, IF),
    DR(2, IF), DW(2, IF, 0x0008u), DR(2, IF), DW(2, TM0H, 0), DW(2, IF, 0xFFFFu), DR(2, IF), DW(2, IME, 0), DR(4, IE), DEND,
};

TEST(diff_irq_registers) { CHECK(run_pads("irq registers", irq_regs, 9, 0, 0, false)); }

static const dop dma_immediate[] = {
    DW(4, 0x03004000u, 0x11223344u), DW(4, 0x03004004u, 0x55667788u), DW(4, 0x03004008u, 0x99AABBCCu),
    DW(4, 0x0300400Cu, 0xDDEEFF00u), DW(4, TM0, 0x00800000u), DW(4, DMA3, 0x03004000u), DW(4, DMA3 + 4, 0x02000000u),
    DW(4, DMA3 + 8, 0xC4000004u), DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DR(2, TM0), DR(2, IF),
    DR(4, DMA3 + 8), DW(4, DMA3 + 4, 0x02000040u), DW(4, DMA3 + 8, 0x80000006u), DM, DM, DM, DM, DM, DM, DM, DM, DM, DM,
    DM, DM, DM, DM, DR(2, TM0), DEND,
};

TEST(diff_dma_immediate) { CHECK(run_pads("dma immediate", dma_immediate, 4, 0x02000000u, 0x50, true)); }

static const dop dma_rom[] = {
    DW(2, WAITCNT, 0x0014u), DW(4, DMA3, 0x08000000u), DW(4, DMA3 + 4, 0x02000000u), DW(4, DMA3 + 8, 0x80000008u),
    DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM,
    DEND,
};

static const dop dma_rom_boundary[] = {
    DW(2, WAITCNT, 0x0014u), DW(4, DMA3, 0x0801FFF8u), DW(4, DMA3 + 4, 0x02000020u), DW(4, DMA3 + 8, 0x84000004u),
    DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM,
    DEND,
};

static const dop dma_rom_slow[] = {
    DW(2, WAITCNT, 0x0000u), DW(4, DMA3, 0x0A000100u), DW(4, DMA3 + 4, 0x02000000u), DW(4, DMA3 + 8, 0x84000003u),
    DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM, DM,
    DEND,
};

TEST(diff_dma_from_the_game_pak)
{
    CHECK(run_pads("dma rom", dma_rom, 2, 0x02000000u, 0x10, true));
    CHECK(run_pads("dma rom 128k boundary", dma_rom_boundary, 2, 0x02000020u, 0x10, true));
    CHECK(run_pads("dma rom ws1", dma_rom_slow, 2, 0x02000000u, 0x0C, true));
}

static const dop dma_priority[] = {
    DW(4, 0x03004000u, 0x01010101u), DW(4, 0x03004004u, 0x03030303u), DW(4, 0x03004100u, 0x02020202u),
    DW(4, DMA0, 0x03004000u), DW(4, DMA0 + 4, 0x02000000u), DW(4, DMA3, 0x03004100u), DW(4, DMA3 + 4, 0x02000000u),
    DW(4, DMA3 + 8, 0x95000004u), DW(4, DMA0 + 8, 0x94000002u), DL(70300), DR(2, VCOUNT), DR(4, DMA0 + 8), DR(4, DMA3 + 8),
    DEND,
};

TEST(diff_dma_priority) { CHECK(run_pads("dma priority", dma_priority, 1, 0x02000000u, 0x10, false)); }

static const dop dma_hblank[] = {
    DW(4, 0x03004000u, 0xA5A5A5A5u), DW(4, DMA0, 0x03004000u), DW(4, DMA0 + 4, 0x02000100u), DW(4, DMA0 + 8, 0xA6000001u),
    DW(4, TM0, 0x00800000u), DL(400), DR(2, TM0), DR(2, VCOUNT), DL(400), DR(2, TM0), DL(400), DR(4, DMA0 + 8),
    DW(2, DMA0 + 10, 0), DL(400), DR(2, VCOUNT), DEND,
};

TEST(diff_dma_hblank) { CHECK(run_pads("dma hblank", dma_hblank, 2, 0x02000100u, 0x20, false)); }

static const dop lcd_irq[] = {
    DW(2, DISPSTAT, 0x0238u), DR(2, DISPSTAT), DR(2, VCOUNT), DL(200), DR(2, IF), DW(2, IF, 0xFFFFu), DL(400), DR(2, IF),
    DR(2, DISPSTAT), DW(2, IF, 0xFFFFu), DL(500), DR(2, IF), DR(2, VCOUNT), DR(2, DISPSTAT), DL(10000), DR(2, IF),
    DR(2, VCOUNT), DEND,
};

TEST(diff_display_flags_and_irqs) { CHECK(run_pads("display", lcd_irq, 3, 0, 0, false)); }
