/**
 * @file test_nvn_xfb.c
 * @brief Transform feedback: what the replay keeps of the buffer binds, and the memory path's packing of the records a
 * capturing program wrote (nvn_xfb_pack) as the hardware streams the vertices out.
 */
#include "nvn/nvn_module.h"
#include "tc.h"

#include <string.h>

TEST(nvn_xfb_records_pack_in_slot_order_with_only_the_selected_words)
{
    static nvn_module m;
    memset(&m, 0, sizeof m);
    memset(m.xfb_select, 0xff, sizeof m.xfb_select);
    m.xfb_mask = 1;
    m.xfb_stride[0] = 12;
    m.xfb_record = 16;
    m.xfb_select[0][0] = 0x20;
    m.xfb_select[0][2] = 0x22;
    uint8_t scratch[4 * 16];
    memset(scratch, 0, sizeof scratch);
    for (uint32_t s = 0; s < 4; s++) {
        for (uint32_t w = 0; w < 3; w++)
            rl_st32(scratch, 16 * s + 4 * w, 100 * s + w);
        rl_st32(scratch, 16 * s + 12, s != 1);
    }
    uint8_t buf[64];
    memset(buf, 0xee, sizeof buf);
    uint8_t *dst[4] = { buf, NULL, NULL, NULL };
    uint64_t size[4] = { 32, 0, 0, 0 };
    uint32_t count[4] = { 4, 0, 0, 0 };
    nvn_xfb_pack(&m, scratch, 4, dst, size, count);
    CHECK_EQ(count[0], 28u);
    CHECK_EQ(rl_ld32(buf, 0), 0xeeeeeeeeu);
    CHECK_EQ(rl_ld32(buf, 4), 0u);
    CHECK_EQ(rl_ld32(buf, 8), 0xeeeeeeeeu);
    CHECK_EQ(rl_ld32(buf, 12), 2u);
    CHECK_EQ(rl_ld32(buf, 16), 200u);
    CHECK_EQ(rl_ld32(buf, 20), 0xeeeeeeeeu);
    CHECK_EQ(rl_ld32(buf, 24), 202u);
    CHECK_EQ(rl_ld32(buf, 28), 0xeeeeeeeeu);
}

TEST(nvn_xfb_binds_are_kept_until_a_draw_reads_them)
{
    nvn_gfx st;
    memset(&st, 0, sizeof st);
    replay r = { .st = &st };
    nvn_cmd e = { .op = NVN_CMD_BIND_TRANSFORM_FEEDBACK_BUFFER, .x = { 0, 2, 0x10000, 0x400 } };
    CHECK_EQ(nvn_draw_cmd(&r, &e), RL_OK);
    CHECK_EQ(st.xfb.addr[2], 0x10000u);
    CHECK_EQ(st.xfb.size[2], 0x400u);
    nvn_cmd z = { .op = NVN_CMD_BIND_TRANSFORM_FEEDBACK_BUFFER, .x = { 0, 2, 0, 0x400 } };
    CHECK_EQ(nvn_draw_cmd(&r, &z), RL_OK);
    CHECK_EQ(st.xfb.size[2], 0u);
}
