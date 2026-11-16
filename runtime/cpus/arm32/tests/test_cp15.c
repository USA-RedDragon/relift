/**
 * @file test_cp15.c
 * @brief The ARM946E-S CP15 registers: ID values, control bits, the TCM
 * areas and protection regions an SDK start-up sequence sets, and what each
 * write reports.
 */
#include "cp15.h"
#include "runner.h"

#define CTRL RL_CP15_KEY(0, 1, 0, 0)

TEST(cp15_reset_reads_the_arm946_ids)
{
    rl_arm32_cp15 p;
    rl_arm32_cp15_reset(&p);
    CHECK_EQ(rl_arm32_cp15_read(&p, RL_CP15_KEY(0, 0, 0, 0)), 0x41059461u);
    CHECK_EQ(rl_arm32_cp15_read(&p, RL_CP15_KEY(0, 0, 0, 1)), 0x0F0D2112u);
    CHECK_EQ(rl_arm32_cp15_read(&p, RL_CP15_KEY(0, 0, 0, 2)), 0x00140180u);
    CHECK_EQ(rl_arm32_cp15_read(&p, CTRL), 0x78u);
    CHECK_EQ(rl_arm32_cp15_vectors(&p), 0);
}

TEST(cp15_control_keeps_fixed_bits_and_reports_changes)
{
    rl_arm32_cp15 p;
    rl_arm32_cp15_reset(&p);
    CHECK_EQ(rl_arm32_cp15_write(&p, CTRL, 0xFFFFFFFFu) & RL_CP15_UNSUPPORTED, RL_CP15_UNSUPPORTED);
    CHECK_EQ(rl_arm32_cp15_read(&p, CTRL), 0x000FF0FDu);
    rl_arm32_cp15_reset(&p);
    unsigned r = rl_arm32_cp15_write(&p, CTRL, 0x0005707Du);
    CHECK_EQ(r, RL_CP15_CHANGED_TCM | RL_CP15_CHANGED_VECTORS | RL_CP15_CHANGED_MPU);
    CHECK_EQ(rl_arm32_cp15_read(&p, CTRL), 0x0005707Du);
    CHECK_EQ(rl_arm32_cp15_vectors(&p), 0xFFFF0000u);
    CHECK_EQ(rl_arm32_cp15_write(&p, CTRL, 0x0005707Du), 0);
}

TEST(cp15_tcm_areas_follow_the_region_registers)
{
    rl_arm32_cp15 p;
    rl_arm32_cp15_reset(&p);
    CHECK_EQ(rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 9, 1, 0), 0x027E000Au), RL_CP15_CHANGED_TCM);
    CHECK_EQ(rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 9, 1, 1), 0x00000020u), RL_CP15_CHANGED_TCM);
    rl_arm32_cp15_write(&p, CTRL, RL_CP15_DTCM_ON | RL_CP15_ITCM_ON);
    rl_arm32_tcm t;
    rl_arm32_cp15_tcm(&p, &t);
    CHECK(t.itcm_on && t.dtcm_on && !t.itcm_load && !t.dtcm_load);
    CHECK_EQ(t.itcm_size, 0x02000000u);
    CHECK_EQ(t.dtcm_base, 0x027E0000u);
    CHECK_EQ(t.dtcm_size, 0x4000u);
    rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 9, 1, 1), 0x12345620u);
    CHECK_EQ(rl_arm32_cp15_read(&p, RL_CP15_KEY(0, 9, 1, 1)), 0x20u);
    rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 9, 1, 0), 0x027E1008u);
    rl_arm32_cp15_tcm(&p, &t);
    CHECK_EQ(t.dtcm_size, 0x2000u);
    CHECK_EQ(t.dtcm_base, 0x027E0000u);
}

TEST(cp15_the_highest_enabled_region_holds_an_address)
{
    rl_arm32_cp15 p;
    rl_arm32_cp15_reset(&p);
    rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 6, 0, 0), 0x04000033u);
    rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 6, 1, 0), 0x0200002Bu);
    rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 6, 2, 0), 0x027E0021u);
    rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 6, 3, 0), 0x02040000u);
    CHECK_EQ(rl_arm32_cp15_region(&p, 0x04000130u), 0);
    CHECK_EQ(rl_arm32_cp15_region(&p, 0x02100000u), 1);
    CHECK_EQ(rl_arm32_cp15_region(&p, 0x027E0010u), 2);
    CHECK_EQ(rl_arm32_cp15_region(&p, 0x02040000u), 1);
    CHECK_EQ(rl_arm32_cp15_region(&p, 0x08000000u), -1);
    CHECK_EQ(rl_arm32_cp15_read(&p, RL_CP15_KEY(0, 6, 1, 1)), 0x0200002Bu);
}

TEST(cp15_short_and_extended_permissions_are_one_register)
{
    rl_arm32_cp15 p;
    rl_arm32_cp15_reset(&p);
    CHECK_EQ(rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 5, 0, 2), 0x36636333u), RL_CP15_CHANGED_MPU);
    CHECK_EQ(rl_arm32_cp15_read(&p, RL_CP15_KEY(0, 5, 0, 0)), 0xEBBFu);
    rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 5, 0, 1), 0x0000FFFFu);
    CHECK_EQ(rl_arm32_cp15_read(&p, RL_CP15_KEY(0, 5, 0, 3)), 0x33333333u);
}

TEST(cp15_wait_for_interrupt_and_cache_operations)
{
    rl_arm32_cp15 p;
    rl_arm32_cp15_reset(&p);
    CHECK_EQ(rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 7, 0, 4), 0), RL_CP15_WAIT);
    CHECK_EQ(rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 7, 8, 2), 0), RL_CP15_WAIT);
    CHECK_EQ(rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 7, 5, 0), 0), 0);
    CHECK_EQ(rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 7, 10, 4), 0), 0);
    CHECK_EQ(rl_arm32_cp15_write(&p, RL_CP15_KEY(0, 7, 0, 4) & ~(1u << 14), 0), RL_CP15_UNDEFINED);
}
