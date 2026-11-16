/**
 * @file test_code.c
 * @brief Code memory for the lifted-code checks: reading and comparing the
 * bytes a CPU fetches, the watched lines whose writes move the code epoch.
 */
#include "ndst.h"

TEST(code_bytes_are_read_through_each_cpus_map)
{
    ndst *t = ndst_new();
    static const uint8_t code[] = {0x1E, 0xFF, 0x2F, 0xE1, 0x70, 0x47};
    for (unsigned i = 0; i < sizeof code; i++)
        w9(t, 0x0200001Eu + i, 1, code[i]);
    uint8_t got[sizeof code];
    rl_nds_code_read(t->m, RL_NDS_ARM7, 0x0240001Eu, got, sizeof got);
    CHECK(memcmp(got, code, sizeof code) == 0);
    CHECK(rl_nds_code_equal(t->m, RL_NDS_ARM9, 0x0200001Eu, code, sizeof code));
    CHECK(!rl_nds_code_equal(t->m, RL_NDS_ARM9, 0x0200001Fu, code, sizeof code));
    CHECK(!rl_nds_code_equal(t->m, RL_NDS_ARM7, 0x0700001Eu, code, 2));
    rl_nds_code_read(t->m, RL_NDS_ARM7, 0x0700001Eu, got, 2);
    CHECK_EQ(got[0] | got[1], 0);
}

TEST(only_writes_to_watched_lines_move_the_epoch)
{
    ndst *t = ndst_new();
    rl_nds *m = t->m;
    rl_nds_code_watch(m, RL_NDS_ARM9, 0x02000040u, 8);
    uint64_t e = rl_nds_code_epoch(m);
    w9(t, 0x02000060u, 4, 1);
    w7(t, 0x02000020u, 4, 1);
    CHECK_EQ(rl_nds_code_epoch(m), e);
    w7(t, 0x0240005Cu, 4, 1);
    CHECK_EQ(rl_nds_code_epoch(m), e + 1);
    ndst_to(t, RL_NDS_ARM7, 1000);
    rl_nds_store(m, RL_NDS_ARM9, 0x02000044u, 2, 7, false);
    CHECK_EQ(rl_nds_code_epoch(m), e + 2);
}

TEST(itcm_writes_are_watched_where_the_itcm_is_mapped)
{
    ndst *t = ndst_new();
    rl_nds *m = t->m;
    rl_nds_cp15_write(m, RL_CP15_KEY(0, 9, 1, 1), 0x20);
    rl_nds_cp15_write(m, RL_CP15_KEY(0, 1, 0, 0), rl_nds_cp15(m)->control | RL_CP15_ITCM_ON);
    rl_nds_code_watch(m, RL_NDS_ARM9, 0x01FF8000u, 4);
    uint64_t e = rl_nds_code_epoch(m);
    ndst_to(t, RL_NDS_ARM7, 1000);
    rl_nds_store(m, RL_NDS_ARM9, 0x00000004u, 4, 0xE12FFF1Eu, false);
    CHECK_EQ(rl_nds_code_epoch(m), e + 1);
    static const uint8_t bx[] = {0x1E, 0xFF, 0x2F, 0xE1};
    CHECK(rl_nds_code_equal(m, RL_NDS_ARM9, 0x01FF8004u, bx, 4));
    CHECK(!rl_nds_code_equal(m, RL_NDS_ARM7, 0x01FF8004u, bx, 4));
}
