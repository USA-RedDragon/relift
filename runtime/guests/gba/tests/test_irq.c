/**
 * @file test_irq.c
 * @brief The IRQ stub at 0x18, IntrWait and VBlankIntrWait, Halt, the
 * post-boot state, the BIOS region's reads, RegisterRamReset, MidiKey2Freq
 * and the numbers this BIOS does not implement.
 */
#include "gbat.h"

TEST(irq_stub_stacks_and_restores_six_registers)
{
    gbat *g = gbat_new();
    gbat_w32(g, 0x03007FFCu, 0x03000101u);
    rl_gba_irq_cpu c = {.r = {1, 2, 3, 4}, .r12 = 12, .lr = 0x08000204u, .sp = 0x03007FA0u};
    CHECK_EQ(rl_gba_bios_irq_entry(&g->bios, &g->bus, &c), 0x03000100u);
    CHECK_EQ(c.sp, 0x03007F88u);
    CHECK_EQ(c.r[0], 0x04000000u);
    CHECK_EQ(c.lr, RL_GBA_BIOS_IRQ_RETURN);
    CHECK_EQ(gbat_r32(g, 0x03007F88u), 1);
    CHECK_EQ(gbat_r32(g, 0x03007F98u), 12);
    CHECK_EQ(gbat_r32(g, 0x03007F9Cu), 0x08000204u);
    CHECK_EQ(rl_gba_bios_read(&g->bios, 0, 4), RL_GBA_BIOS_OPEN_IRQ);
    c.r[0] = c.r[1] = c.r[2] = c.r[3] = c.r12 = 0;
    CHECK_EQ(rl_gba_bios_irq_return(&g->bios, &g->bus, &c), 0x08000200u);
    CHECK_EQ(c.sp, 0x03007FA0u);
    CHECK_EQ(c.r[0], 1);
    CHECK_EQ(c.r[3], 4);
    CHECK_EQ(c.r12, 12);
    CHECK_EQ(rl_gba_bios_read(&g->bios, 2, 2), RL_GBA_BIOS_OPEN_IRQ_DONE >> 16);
}

TEST(intr_wait_returns_at_once_when_the_flag_is_set)
{
    gbat *g = gbat_new();
    gbat_w32(g, 0x03007FFCu, 0x03000100u);
    gbat_w16(g, 0x03007FF8u, 0x0009);
    rl_gba_swi_cpu c = gbat_cpu(0, 1, 0, 0x1234);
    CHECK_EQ(gbat_swi(g, 0x04, &c), RL_GBA_SWI_OK);
    CHECK_EQ(g->halts, 0);
    CHECK_EQ(gbat_r16(g, 0x03007FF8u), 0x0008);
    CHECK_EQ(c.r[0], 1);
    CHECK_EQ(c.r[1], 1);
    CHECK_EQ(c.r[3], 0);
    CHECK_EQ(gbat_r16(g, 0x04000208u), 1);
}

TEST(intr_wait_discards_old_flags_and_halts)
{
    gbat *g = gbat_new();
    gbat_w32(g, 0x03007FFCu, 0x03000100u);
    gbat_w16(g, 0x03007FF8u, 0x0001);
    gbat_w16(g, 0x04000200u, 0x0001);
    g->halt_raises = 1;
    rl_gba_swi_cpu c = gbat_cpu(1, 1, 0, 0);
    CHECK_EQ(gbat_swi(g, 0x04, &c), RL_GBA_SWI_OK);
    CHECK_EQ(g->halts, 1);
    CHECK_EQ(gbat_r16(g, 0x03007FF8u), 0);
    CHECK_EQ(c.r[0], 1);
}

TEST(vblank_intr_wait_is_intr_wait_1_1)
{
    gbat *g = gbat_new();
    gbat_w32(g, 0x03007FFCu, 0x03000100u);
    gbat_w16(g, 0x03007FF8u, 0x0001);
    gbat_w16(g, 0x04000200u, 0x0001);
    g->halt_raises = 1;
    rl_gba_swi_cpu c = gbat_cpu(0x55, 0x66, 0x77, 0x88);
    CHECK_EQ(gbat_swi(g, 0x05, &c), RL_GBA_SWI_OK);
    CHECK_EQ(g->halts, 1);
    CHECK_EQ(c.r[0], 1);
    CHECK_EQ(c.r[1], 1);
    CHECK_EQ(c.r[2], 0x77);
    CHECK_EQ(c.r[3], 0);
}

TEST(halt_and_stop_write_haltcnt)
{
    gbat *g = gbat_new();
    rl_gba_swi_cpu c = gbat_cpu(0, 0, 0, 0);
    gbat_swi(g, 0x02, &c);
    CHECK_EQ(g->halts, 1);
    gbat_swi(g, 0x03, &c);
    CHECK_EQ(g->halts, 1);
}

TEST(boot_state_and_bios_reads)
{
    rl_gba_boot b = rl_gba_bios_boot_state();
    CHECK_EQ(b.pc, 0x08000000u);
    CHECK_EQ(b.cpsr, 0x1F);
    CHECK_EQ(b.sp_sys, 0x03007F00u);
    CHECK_EQ(b.sp_irq, 0x03007FA0u);
    CHECK_EQ(b.sp_svc, 0x03007FE0u);
    gbat *g = gbat_new();
    rl_gba_bios_boot_io(&g->bios, &g->bus);
    CHECK_EQ(gbat_r16(g, 0x04000000u), 0x0080);
    CHECK_EQ(gbat_r16(g, 0x04000088u), 0x0200);
    CHECK_EQ(gbat_r16(g, 0x04000134u), 0x8000);
    CHECK_EQ(gbat_r8(g, 0x04000300u), 1);
    CHECK_EQ(rl_gba_bios_read(&g->bios, 0x100, 4), RL_GBA_BIOS_OPEN_BOOT);
    CHECK_EQ(rl_gba_bios_read(&g->bios, 0x101, 1), (RL_GBA_BIOS_OPEN_BOOT >> 8) & 0xFF);
    rl_gba_swi_cpu c = gbat_cpu(0, 0, 0, 0);
    gbat_swi(g, 0x0D, &c);
    CHECK_EQ(c.r[0], 0xBAAE187Fu);
    CHECK_EQ(rl_gba_bios_read(&g->bios, 0x3FFC, 4), RL_GBA_BIOS_OPEN_SWI);
}

TEST(register_ram_reset_clears_the_selected_areas)
{
    gbat *g = gbat_new();
    gbat_w32(g, 0x05000000u, 0x12345678u);
    gbat_w32(g, 0x06000000u, 0x12345678u);
    gbat_w32(g, 0x07000000u, 0x12345678u);
    gbat_w16(g, 0x04000132u, 0x0123);
    gbat_w16(g, 0x04000204u, 0x4317);
    rl_gba_swi_cpu c = gbat_cpu(0x14, 0, 0, 0);
    gbat_swi(g, 0x01, &c);
    CHECK_EQ(gbat_r32(g, 0x05000000u), 0);
    CHECK_EQ(gbat_r32(g, 0x06000000u), 0x12345678u);
    CHECK_EQ(gbat_r32(g, 0x07000000u), 0);
    CHECK_EQ(c.r[1], 0x03000000u);
    CHECK_EQ(c.r[3], RL_GBA_BIOS_SWI_RETURN);
    c = gbat_cpu(0x80, 0, 0, 0);
    gbat_swi(g, 0x01, &c);
    CHECK_EQ(c.r[0], 0x100);
    CHECK_EQ(gbat_r16(g, 0x04000000u), 0x0080);
    CHECK_EQ(gbat_r16(g, 0x04000020u), 0x0100);
    CHECK_EQ(gbat_r16(g, 0x04000132u), 0);
    CHECK_EQ(gbat_r16(g, 0x04000204u), 0x4317);
    gbat_w16(g, 0x04000134u, 0);
    c = gbat_cpu(0x20, 0, 0, 0);
    gbat_swi(g, 0x01, &c);
    CHECK_EQ(gbat_r16(g, 0x04000134u), 0x8000);
}

TEST(midi_key2freq_scales_the_wave_frequency)
{
    gbat *g = gbat_new();
    gbat_w32(g, 0x02000004u, 0x80000000u);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 180 - 12, 0, 0);
    gbat_swi(g, 0x1F, &c);
    CHECK_EQ(c.r[0], 0x40000000u);
    CHECK_EQ(c.r[1], 0x80000000u);
    CHECK_EQ(c.r[3], RL_GBA_BIOS_SWI_RETURN);
    c = gbat_cpu(0x02000000u, 180 - 24, 0, 0);
    gbat_swi(g, 0x1F, &c);
    CHECK_EQ(c.r[0], 0x20000000u);
}

TEST(unknown_numbers_say_so_and_keep_registers)
{
    gbat *g = gbat_new();
    rl_gba_swi_cpu c = gbat_cpu(1, 2, 3, 4);
    CHECK_EQ(gbat_swi(g, 0x1A, &c), RL_GBA_SWI_UNKNOWN);
    CHECK_EQ(c.r[0], 1);
    CHECK_EQ(c.r[3], 4);
    CHECK_EQ(gbat_swi(g, 0x2B, &c), RL_GBA_SWI_UNKNOWN);
}

TEST(soft_reset_reports_a_reset)
{
    gbat *g = gbat_new();
    gbat_w8(g, 0x03007FFAu, 0);
    gbat_w32(g, 0x03007E00u, 0xFFFFFFFFu);
    rl_gba_swi_cpu c = gbat_cpu(1, 2, 3, 4);
    CHECK_EQ(gbat_swi(g, 0x00, &c), RL_GBA_SWI_RESET);
    CHECK_EQ(c.ret, 0x08000000u);
    CHECK_EQ(gbat_r32(g, 0x03007E00u), 0);
    gbat_w8(g, 0x03007FFAu, 1);
    c = gbat_cpu(1, 2, 3, 4);
    gbat_swi(g, 0x00, &c);
    CHECK_EQ(c.ret, 0x02000000u);
}
