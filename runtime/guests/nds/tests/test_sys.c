/**
 * @file test_sys.c
 * @brief The ARM7's system registers (POWCNT2, WIFIWAITCNT, BIOSPROT) and
 * the sound controller's register file, with the values the oracle returns
 * for all-ones writes.
 */
#include "ndst.h"

#define POWCNT 0x04000304u
#define WIFIWAIT 0x04000206u
#define BIOSPROT 0x04000308u
#define SND(n) (0x04000400u + 16u * (n))

TEST(powcnt2_gates_wifiwaitcnt)
{
    ndst *t = ndst_new();
    CHECK_EQ(r7(t, POWCNT, 4), 1);
    CHECK_EQ(r7(t, WIFIWAIT, 2), 0);
    w7(t, WIFIWAIT, 2, 0xFFFF);
    w7(t, POWCNT, 4, 0xFFFFFFFFu);
    CHECK_EQ(r7(t, POWCNT, 4), 3);
    CHECK_EQ(r7(t, WIFIWAIT, 2), 0x30);
    w7(t, WIFIWAIT, 2, 0xFFFF);
    CHECK_EQ(r7(t, WIFIWAIT, 2), 0x3F);
    w7(t, WIFIWAIT, 2, 0x30);
    w7(t, POWCNT, 4, 1);
    CHECK_EQ(r7(t, WIFIWAIT, 2), 0);
    w7(t, POWCNT, 4, 3);
    CHECK_EQ(r7(t, WIFIWAIT, 2), 0x30);
}

TEST(biosprot_reads_1204_and_ignores_writes)
{
    ndst *t = ndst_new();
    CHECK_EQ(r7(t, BIOSPROT, 4), 0x1204);
    w7(t, BIOSPROT, 4, 0);
    CHECK_EQ(r7(t, BIOSPROT, 4), 0x1204);
}

TEST(sound_channel_registers_keep_their_masks_and_hide_address_registers)
{
    ndst *t = ndst_new();
    for (unsigned n = 0; n < 16; n++) {
        for (unsigned o = 0; o < 16; o += 4)
            w7(t, SND(n) + o, 4, 0xFFFFFFFFu);
        CHECK_EQ(r7(t, SND(n), 4), 0xFF7F837Fu);
        CHECK_EQ(r7(t, SND(n) + 4, 4), 0);
        CHECK_EQ(r7(t, SND(n) + 8, 4), 0);
        CHECK_EQ(r7(t, SND(n) + 12, 4), 0);
    }
    w7(t, SND(1) + 3, 1, 0);
    CHECK_EQ(r7(t, SND(1), 4), 0x007F837Fu);
    CHECK_EQ(r9(t, SND(1), 4), 0);
}

TEST(sound_control_capture_and_bias_masks)
{
    ndst *t = ndst_new();
    CHECK_EQ(r7(t, 0x04000504u, 4), 0x200);
    w7(t, 0x04000500u, 4, 0xFFFFFFFFu);
    CHECK_EQ(r7(t, 0x04000500u, 4), 0xBF7F);
    w7(t, 0x04000504u, 4, 0xFFFFFFFFu);
    CHECK_EQ(r7(t, 0x04000504u, 4), 0x3FF);
    w7(t, 0x04000508u, 4, 0xFFFFFFFFu);
    CHECK_EQ(r7(t, 0x04000508u, 4), 0x8F8F);
    w7(t, 0x04000510u, 4, 0xFFFFFFFFu);
    w7(t, 0x04000514u, 4, 0xFFFFFFFFu);
    w7(t, 0x04000518u, 4, 0xFFFFFFFFu);
    w7(t, 0x0400051Cu, 4, 0xFFFFFFFFu);
    CHECK_EQ(r7(t, 0x04000510u, 4), 0x07FFFFFCu);
    CHECK_EQ(r7(t, 0x04000514u, 4), 0);
    CHECK_EQ(r7(t, 0x04000518u, 4), 0x07FFFFFCu);
    CHECK_EQ(r7(t, 0x0400051Cu, 4), 0);
    CHECK_EQ(r9(t, 0x04000500u, 4), 0);
}
