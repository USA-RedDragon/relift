/**
 * @file test_map.c
 * @brief The memory maps of both CPUs: mirrors, the shared work RAM banks,
 * the TCMs, the BIOS stubs, the GBA slot's open bus, the system registers,
 * the cost table and main RAM contention.
 */
#include "ndst.h"

TEST(main_ram_is_shared_and_mirrored_every_4_mib)
{
    ndst *t = ndst_new();
    w9(t, 0x02000010u, 4, 0x11223344u);
    CHECK_EQ(r7(t, 0x02000010u, 4), 0x11223344u);
    CHECK_EQ(r7(t, 0x02400010u, 4), 0x11223344u);
    CHECK_EQ(r9(t, 0x02C00010u, 2), 0x3344u);
    CHECK_EQ(r9(t, 0x02000013u, 1), 0x11u);
    w7(t, 0x02FFFFFEu, 2, 0xBEEF);
    size_t len;
    uint8_t *main = rl_nds_mem(t->m, RL_NDS_MAIN, &len);
    CHECK_EQ(len, 0x400000u);
    CHECK_EQ(main[0x3FFFFE], 0xEF);
    CHECK_EQ(main[0x3FFFFF], 0xBE);
}

TEST(misaligned_accesses_are_aligned_down)
{
    ndst *t = ndst_new();
    w9(t, 0x02000102u, 4, 0xAABBCCDDu);
    CHECK_EQ(r9(t, 0x02000100u, 4), 0xAABBCCDDu);
    CHECK_EQ(r9(t, 0x02000103u, 2), 0xAABBu);
}

TEST(shared_wram_follows_wramcnt_on_both_cpus)
{
    ndst *t = ndst_new();
    uint8_t *sw = rl_nds_mem(t->m, RL_NDS_SWRAM, NULL);
    uint8_t *own7 = rl_nds_mem(t->m, RL_NDS_WRAM7, NULL);
    sw[0x0000] = 0xA1;
    sw[0x4000] = 0xB2;
    own7[0x0000] = 0xC3;
    own7[0x8000] = 0xD4;

    CHECK_EQ(r9(t, 0x04000247u, 1), 0);
    CHECK_EQ(r9(t, 0x03000000u, 1), 0xA1);
    CHECK_EQ(r9(t, 0x03004000u, 1), 0xB2);
    CHECK_EQ(r9(t, 0x03FF8000u, 1), 0xA1);
    CHECK_EQ(r7(t, 0x03000000u, 1), 0xC3);
    CHECK_EQ(r7(t, 0x03008000u, 1), 0xD4);
    CHECK_EQ(r7(t, 0x04000241u, 1), 0);

    w9(t, 0x04000247u, 1, 1);
    CHECK_EQ(r7(t, 0x04000241u, 1), 1);
    CHECK_EQ(r9(t, 0x03000000u, 1), 0xB2);
    CHECK_EQ(r9(t, 0x03004000u, 1), 0xB2);
    CHECK_EQ(r7(t, 0x03000000u, 1), 0xA1);
    CHECK_EQ(r7(t, 0x037FC000u, 1), 0xA1);

    w9(t, 0x04000247u, 1, 2);
    CHECK_EQ(r9(t, 0x03000000u, 1), 0xA1);
    CHECK_EQ(r7(t, 0x03000000u, 1), 0xB2);

    w9(t, 0x04000247u, 1, 0xFF);
    CHECK_EQ(r9(t, 0x04000247u, 1), 3);
    CHECK_EQ(r7(t, 0x04000241u, 1), 3);
    CHECK_EQ(r9(t, 0x03000000u, 1), 0);
    w9(t, 0x03000000u, 1, 0x55);
    CHECK_EQ(sw[0], 0xA1);
    CHECK_EQ(r7(t, 0x03000000u, 1), 0xA1);
    CHECK_EQ(r7(t, 0x037F8000u, 1), 0xA1);
    CHECK_EQ(r7(t, 0x037FC000u, 1), 0xB2);

    w7(t, 0x04000241u, 1, 0);
    CHECK_EQ(r7(t, 0x04000241u, 1), 3);
}

TEST(arm7_wram_is_private_and_mirrored)
{
    ndst *t = ndst_new();
    w7(t, 0x0380FFFCu, 4, 0x12345678u);
    CHECK_EQ(r7(t, 0x03FFFFFCu, 4), 0x12345678u);
    CHECK_EQ(r7(t, 0x0381FFFCu, 4), 0x12345678u);
    CHECK_EQ(r9(t, 0x0380FFFCu, 4), 0);
    CHECK(!rl_nds_ordered(t->m, RL_NDS_ARM7, 0x03800000u));
    CHECK(rl_nds_ordered(t->m, RL_NDS_ARM7, 0x037F8000u));
}

TEST(tcms_come_from_cp15_with_itcm_priority_and_load_mode)
{
    ndst *t = ndst_new();
    rl_nds *m = t->m;
    ndst_to(t, RL_NDS_ARM7, 1000);
    w9(t, 0x027C0000u, 4, 0x0BADF00Du);
    w9(t, 0x00000000u, 4, 0xDEADu);
    CHECK_EQ(r9(t, 0x00000000u, 4), 0);
    rl_nds_cp15_write(m, RL_CP15_KEY(0, 9, 1, 1), 0x20);
    rl_nds_cp15_write(m, RL_CP15_KEY(0, 9, 1, 0), 0x027C000Au);
    unsigned what = rl_nds_cp15_write(m, RL_CP15_KEY(0, 1, 0, 0), rl_nds_cp15(m)->control | RL_CP15_ITCM_ON | RL_CP15_DTCM_ON);
    CHECK(what & RL_CP15_CHANGED_TCM);

    w9(t, 0x00000004u, 4, 0x11111111u);
    CHECK_EQ(r9(t, 0x00008004u, 4), 0x11111111u);
    CHECK_EQ(r9(t, 0x01FF8004u, 4), 0x11111111u);
    CHECK_EQ(r7(t, 0x00000004u, 4), 0);

    CHECK_EQ(r9(t, 0x027C0000u, 4), 0);
    w9(t, 0x027C3FFCu, 4, 0x22222222u);
    CHECK_EQ(r7(t, 0x027C3FFCu, 4), 0);
    CHECK_EQ(rl_nds_mem(m, RL_NDS_DTCM, NULL)[0x3FFC], 0x22);
    CHECK_EQ(r7(t, 0x027C0000u, 4), 0x0BADF00Du);
    CHECK_EQ(rl_nds_fetch(m, RL_NDS_ARM9, 0x027C0000u, 4, false), 0x0BADF00Du);
    CHECK(!rl_nds_ordered(m, RL_NDS_ARM9, 0x027C0000u));
    CHECK(rl_nds_ordered(m, RL_NDS_ARM9, 0x027C4000u));

    rl_nds_cp15_write(m, RL_CP15_KEY(0, 9, 1, 0), 0x0000000Au);
    w9(t, 0x00000008u, 4, 0x33333333u);
    CHECK_EQ(rl_nds_mem(m, RL_NDS_ITCM, NULL)[8], 0x33);
    CHECK_EQ(rl_nds_mem(m, RL_NDS_DTCM, NULL)[8], 0);

    rl_nds_cp15_write(m, RL_CP15_KEY(0, 9, 1, 0), 0x027C000Au);
    rl_nds_cp15_write(m, RL_CP15_KEY(0, 1, 0, 0), rl_nds_cp15(m)->control | RL_CP15_DTCM_LOAD);
    w9(t, 0x027C0010u, 4, 0x44444444u);
    CHECK_EQ(r9(t, 0x027C0010u, 4), 0);
    CHECK_EQ(rl_nds_mem(m, RL_NDS_DTCM, NULL)[0x10], 0x44);
}

TEST(bios_regions_are_stubs_and_undefined_regions_read_zero)
{
    ndst *t = ndst_new();
    w9(t, 0xFFFF0000u, 4, 0x12345678u);
    CHECK_EQ(r9(t, 0xFFFF0000u, 4), 0);
    CHECK_EQ(r9(t, 0xFFFF7FFCu, 4), 0);
    w7(t, 0x00000000u, 4, 0x12345678u);
    CHECK_EQ(r7(t, 0x00000000u, 4), 0);
    CHECK_EQ(r9(t, 0x01000000u, 4), 0);
    CHECK_EQ(r7(t, 0x05000000u, 4), 0);
    CHECK_EQ(r7(t, 0x07000000u, 4), 0);
    CHECK_EQ(r9(t, 0x0B000000u, 4), 0);
    CHECK(!rl_nds_ordered(t->m, RL_NDS_ARM9, 0xFFFF0000u));
    CHECK(!rl_nds_ordered(t->m, RL_NDS_ARM7, 0x00000000u));
}

TEST(palette_and_oam_mirror_and_take_no_bytes_from_the_arm9)
{
    ndst *t = ndst_new();
    w9(t, 0x050007FEu, 2, 0x7FFF);
    CHECK_EQ(r9(t, 0x05FFFFFEu, 2), 0x7FFF);
    w9(t, 0x070003F0u, 4, 0x01020304u);
    CHECK_EQ(r9(t, 0x07000BF0u, 4), 0x01020304u);
    w9(t, 0x070003F0u, 1, 0xFF);
    w9(t, 0x05000000u, 1, 0xFF);
    CHECK_EQ(r9(t, 0x070003F0u, 4), 0x01020304u);
    CHECK_EQ(r9(t, 0x05000000u, 4), 0);
    for (uint32_t a = 0x06000000u; a < 0x07000000u; a += 0x40000u)
        CHECK_EQ(r9(t, a, 4), 0);
    CHECK_EQ(r7(t, 0x06000000u, 4), 0);
    CHECK(!rl_nds_ordered(t->m, RL_NDS_ARM9, 0x06000000u));
    CHECK(rl_nds_ordered(t->m, RL_NDS_ARM7, 0x06000000u));
}

TEST(io_registers_are_per_cpu_and_unknown_ones_are_register_files)
{
    ndst *t = ndst_new();
    w9(t, 0x04000130u, 2, 0x1234);
    w7(t, 0x04000130u, 2, 0x5678);
    CHECK_EQ(r9(t, 0x04000130u, 2), 0x1234);
    CHECK_EQ(r7(t, 0x04000130u, 2), 0x5678);
    CHECK_EQ(r9(t, 0x04002000u, 4), 0);
    CHECK_EQ(r9(t, 0x04000300u, 1), 0);
    w9(t, 0x04000300u, 1, 0xFF);
    w7(t, 0x04000300u, 1, 0xFF);
    CHECK_EQ(r9(t, 0x04000300u, 1), 3);
    CHECK_EQ(r7(t, 0x04000300u, 1), 1);
    w9(t, 0x04000240u, 4, 0x84838281u);
    CHECK_EQ(r9(t, 0x04000240u, 4), 0x84838281u);
    CHECK_EQ(r9(t, 0x04000247u, 1), 0);
}

TEST(exmemcnt_high_bits_belong_to_the_arm9)
{
    ndst *t = ndst_new();
    w9(t, 0x04000204u, 2, 0xE8FFu);
    CHECK_EQ(r9(t, 0x04000204u, 2), 0xE8FFu);
    CHECK_EQ(r7(t, 0x04000204u, 2), 0xE880u);
    w7(t, 0x04000204u, 2, 0x0055u);
    CHECK_EQ(r7(t, 0x04000204u, 2), 0xE8D5u);
    CHECK_EQ(r9(t, 0x04000204u, 2), 0xE8FFu);
    w7(t, 0x04000204u, 2, 0xFF00u);
    CHECK_EQ(r9(t, 0x04000204u, 2), 0xE8FFu);
}

TEST(gba_slot_reads_open_bus_for_its_owner_and_zero_for_the_other)
{
    ndst *t = ndst_new();
    CHECK_EQ(r9(t, 0x08000000u, 2), 0xFE08u);
    CHECK_EQ(r9(t, 0x08000102u, 2), 0xFE89u);
    CHECK_EQ(r9(t, 0x0A000000u, 1), 0xFFu);
    CHECK_EQ(r7(t, 0x08000000u, 2), 0);
    w9(t, 0x04000204u, 2, 0x0004u);
    CHECK_EQ(r9(t, 0x08000104u, 4), 0x00830082u);
    w9(t, 0x04000204u, 2, 0x008Cu);
    CHECK_EQ(r9(t, 0x08000000u, 2), 0);
    CHECK_EQ(r7(t, 0x08000000u, 2), 0xFE08u);
    w7(t, 0x04000204u, 2, 0x000Cu);
    CHECK_EQ(r7(t, 0x08000000u, 2), 0xFFFFu);
}

TEST(costs_follow_gbatek_and_charge_the_clock)
{
    ndst *t = ndst_new();
    rl_nds *m = t->m;
    const rl_nds_costs *c = rl_nds_cost_table(m);
    CHECK_EQ(c->t[RL_NDS_ARM9][RL_NDS_DATA][0x02][2][0], 20);
    CHECK_EQ(c->t[RL_NDS_ARM9][RL_NDS_DATA][0x02][1][1], 2);
    CHECK_EQ(c->t[RL_NDS_ARM9][RL_NDS_CODE][0x02][1][0], 9);
    CHECK_EQ(c->t[RL_NDS_ARM9][RL_NDS_DATA][0x04][2][0], 8);
    CHECK_EQ(c->t[RL_NDS_ARM9][RL_NDS_DATA][15][2][0], 8);
    CHECK_EQ(c->t[RL_NDS_ARM7][RL_NDS_CODE][0x02][2][1], 4);
    CHECK_EQ(c->t[RL_NDS_ARM7][RL_NDS_DATA][0x03][2][0], 2);
    CHECK_EQ(c->t[RL_NDS_ARM9][RL_NDS_CODE][0x08][2][0], 38);
    CHECK_EQ(c->t[RL_NDS_ARM7][RL_NDS_DATA][0x08][1][0], 18);
    CHECK_EQ(c->t[RL_NDS_ARM7][RL_NDS_DATA][0x0A][0][0], 18);
    CHECK_EQ(c->t[RL_NDS_ARM9][RL_NDS_DATA][0x0A][0][1], 20);
    w9(t, 0x04000204u, 2, 0x001Au);
    ndst_to(t, RL_NDS_ARM7, 1000);
    CHECK_EQ(c->t[RL_NDS_ARM9][RL_NDS_DATA][0x08][1][0], 18);
    CHECK_EQ(c->t[RL_NDS_ARM9][RL_NDS_DATA][0x08][1][1], 8);
    CHECK_EQ(c->t[RL_NDS_ARM9][RL_NDS_DATA][0x0A][0][0], 18);

    rl_nds_load(m, RL_NDS_ARM7, 0x03800000u, 4, false);
    CHECK_EQ(rl_nds_now(m, RL_NDS_ARM7), 1002);
    rl_nds_idle(m, RL_NDS_ARM9, 3);
    rl_nds_load(m, RL_NDS_ARM9, 0x04000130u, 2, false);
    CHECK_EQ(rl_nds_now(m, RL_NDS_ARM9), 4 + 8);
    rl_nds_store(m, RL_NDS_ARM9, 0x05000000u, 4, 1, false);
    CHECK_EQ(rl_nds_now(m, RL_NDS_ARM9), 12 + 10);
    rl_nds_fetch(m, RL_NDS_ARM9, 0x02000000u, 4, true);
    CHECK_EQ(rl_nds_now(m, RL_NDS_ARM9), 22 + 18);
}

TEST(tcm_accesses_cost_one_arm9_cycle_off_the_bus)
{
    ndst *t = ndst_new();
    rl_nds *m = t->m;
    ndst_to(t, RL_NDS_ARM7, 1000);
    rl_nds_cp15_write(m, RL_CP15_KEY(0, 9, 1, 1), 0x0C);
    rl_nds_cp15_write(m, RL_CP15_KEY(0, 1, 0, 0), rl_nds_cp15(m)->control | RL_CP15_ITCM_ON);
    rl_nds_store(m, RL_NDS_ARM9, 0x100u, 4, 7, false);
    rl_nds_load(m, RL_NDS_ARM9, 0x100u, 4, false);
    rl_nds_fetch(m, RL_NDS_ARM9, 0x100u, 2, false);
    CHECK_EQ(rl_nds_now(m, RL_NDS_ARM9), 3);
    rl_nds_load(m, RL_NDS_ARM9, 0x02000000u, 2, true);
    CHECK_EQ(rl_nds_now(m, RL_NDS_ARM9), 4 + 2);
}

TEST(main_ram_serves_one_cpu_at_a_time)
{
    ndst *t = ndst_new();
    rl_nds *m = t->m;
    rl_nds_load(m, RL_NDS_ARM9, 0x02000000u, 4, false);
    CHECK_EQ(rl_nds_now(m, RL_NDS_ARM9), 20);
    rl_nds_load(m, RL_NDS_ARM7, 0x02000000u, 4, false);
    CHECK_EQ(rl_nds_now(m, RL_NDS_ARM7), 40);
    rl_nds_load(m, RL_NDS_ARM7, 0x03800000u, 4, false);
    CHECK_EQ(rl_nds_now(m, RL_NDS_ARM7), 42);
    ndst_to(t, RL_NDS_ARM7, 200);
    ndst_to(t, RL_NDS_ARM9, 100);
    rl_nds_store(m, RL_NDS_ARM9, 0x02000000u, 2, 1, true);
    CHECK_EQ(rl_nds_now(m, RL_NDS_ARM9), 102);
}

TEST(an_untimed_haltcnt_write_does_not_halt_a_later_store)
{
    ndst *t = ndst_new();
    rl_nds *m = t->m;
    w7(t, 0x04000301u, 1, 0x80);
    ndst_to(t, RL_NDS_ARM9, 1000);
    rl_nds_store(m, RL_NDS_ARM7, 0x03800000u, 4, 1, false);
    CHECK_EQ(rl_nds_now(m, RL_NDS_ARM7), 2);
    CHECK_EQ(r7(t, 0x03800000u, 4), 1);
}
