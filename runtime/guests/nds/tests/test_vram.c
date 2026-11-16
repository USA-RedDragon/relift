/**
 * @file test_vram.c
 * @brief The video RAM banks: the probe ROM's op streams (filled through
 * LCDC, read back through every mapping, overlaps, mirrors, unused MST and
 * OFS values, byte writes, VRAMCNT and VRAMSTAT reads) replayed on the
 * machine, against the words the oracle's ARM9 read.
 */
#include "ndst.h"
#include "scene.h"

/** @brief Every bank to LCDC. */
#define LCDC_ALL                                                                                                    \
  W8(VRAMCNT_A, 0x80), W8(VRAMCNT_B, 0x80), W8(VRAMCNT_C, 0x80), W8(VRAMCNT_D, 0x80), W8(VRAMCNT_E, 0x80),        \
      W8(VRAMCNT_F, 0x80), W8(VRAMCNT_G, 0x80), W8(VRAMCNT_H, 0x80), W8(VRAMCNT_I, 0x80)

/** @brief The first probe: every bank through LCDC, BG-A, OBJ-A, engine B, the unused modes and the ARM7. */
static const uint32_t probe1[] = {
  LCDC_ALL,
  W32(0x06800000, 0x000000A1), W32(0x06800004, 0x0000A100), W32(0x0681FFFC, 0xA1A1A1A1),
  W32(0x06820000, 0x000000B2), W32(0x06820004, 0x0000B200), W32(0x06830000, 0x0B0B0B0B),
  W32(0x06840000, 0x000000C4), W32(0x06840004, 0x0000C400),
  W32(0x06860000, 0x000000D8), W32(0x06860004, 0x0000D800),
  W32(0x06880000, 0x000000E1), W32(0x06884000, 0x000000E2), W32(0x06888000, 0x000000E3), W32(0x0688C000, 0x000000E4),
  W32(0x06890000, 0x000000F1), W32(0x06894000, 0x00000061), W32(0x06898000, 0x00000081), W32(0x0689C000, 0x00000082),
  W32(0x068A0000, 0x00000091),
  /* 0: LCDC reads and its mirrors */
  RD32(0x06800000), RD32(0x06820000), RD32(0x06840000), RD32(0x06860000), RD32(0x06880000), RD32(0x06890000),
  RD32(0x06894000), RD32(0x06898000), RD32(0x068A0000), RD32(0x068A4000), RD32(0x068B0000), RD32(0x06900000),
  RD32(0x06A00000), RD32(0x06C00000), RD32(0x06E00000), RD32(0x06FA0000),
  /* 16: byte writes to VRAM are dropped on the ARM9 */
  W8(0x06800008, 0x55), RD32(0x06800008), W16(0x0680000A, 0x6677), RD32(0x06800008),
  /* 18: A at BG-A ofs 0, B at ofs 1, C ofs 2, D ofs 3 */
  W8(VRAMCNT_A, 0x81), W8(VRAMCNT_B, 0x89), W8(VRAMCNT_C, 0x91), W8(VRAMCNT_D, 0x99),
  RD32(0x06000000), RD32(0x06020000), RD32(0x06040000), RD32(0x06060000), RD32(0x06080000), RD32(0x06160000),
  RD32(0x06800000), RD32(0x06200000), RD16(0x04000240), RD32(0x04000240),
  /* 28: overlap: A and B both at BG-A ofs 0, then a write lands in both */
  W8(VRAMCNT_B, 0x81), RD32(0x06000000), RD32(0x06000004), W32(0x06000010, 0x12345678), W8(VRAMCNT_B, 0x89),
  RD32(0x06000010), RD32(0x06020010),
  /* 32: E at BG-A (64K), F at ofs 3 (0x14000), G at ofs 1 (0x4000) */
  W8(VRAMCNT_A, 0x80), W8(VRAMCNT_B, 0x80), W8(VRAMCNT_C, 0x80), W8(VRAMCNT_D, 0x80),
  W8(VRAMCNT_E, 0x81), W8(VRAMCNT_F, 0x99), W8(VRAMCNT_G, 0x89),
  RD32(0x06000000), RD32(0x06004000), RD32(0x06008000), RD32(0x06014000), RD32(0x06010000), RD32(0x06018000),
  /* 38: OBJ-A: A at ofs 1, E, F ofs 2; engine B: C BG, H BG, I BG; D OBJ B */
  W8(VRAMCNT_E, 0x80), W8(VRAMCNT_F, 0x80), W8(VRAMCNT_G, 0x80),
  W8(VRAMCNT_A, 0x8A), W8(VRAMCNT_E, 0x82), W8(VRAMCNT_F, 0x92),
  RD32(0x06400000), RD32(0x06420000), RD32(0x06410000), RD32(0x06440000), RD32(0x06460000),
  W8(VRAMCNT_A, 0x80), W8(VRAMCNT_E, 0x80), W8(VRAMCNT_F, 0x80),
  W8(VRAMCNT_C, 0x84), W8(VRAMCNT_D, 0x84), W8(VRAMCNT_H, 0x81), W8(VRAMCNT_I, 0x81),
  RD32(0x06200000), RD32(0x06208000), RD32(0x06600000), RD32(0x06220000), RD32(0x06620000), RD32(0x06640000),
  W8(VRAMCNT_C, 0x80), W8(VRAMCNT_H, 0x80), W8(VRAMCNT_I, 0x82),
  RD32(0x06200000), RD32(0x06208000), RD32(0x06600000), RD32(0x06604000), RD32(0x06608000),
  /* 54: unused MST values: A MST 4 (bit 2 ignored?), E MST 5, ext palette and texture slots off the bus */
  W8(VRAMCNT_D, 0x80), W8(VRAMCNT_I, 0x80),
  W8(VRAMCNT_A, 0x84), RD32(0x06800000), RD32(0x06000000),
  W8(VRAMCNT_A, 0x83), RD32(0x06800000), RD32(0x06000000),
  W8(VRAMCNT_A, 0x01), RD32(0x06800000), RD32(0x06000000),
  W8(VRAMCNT_A, 0x80), W8(VRAMCNT_E, 0x85), RD32(0x06880000), RD32(0x06000000),
  W8(VRAMCNT_E, 0x86), RD32(0x06880000), RD32(0x06000000), W8(VRAMCNT_E, 0x87), RD32(0x06880000), RD32(0x06000000),
  W8(VRAMCNT_E, 0x84), RD32(0x06880000), RD32(0x06000000), W8(VRAMCNT_E, 0x80),
  /* 68: C and D to the ARM7 */
  W8(VRAMCNT_C, 0x82), W8(VRAMCNT_D, 0x8A), RD32(0x06840000), RD32(0x06860000), RD16(0x04000240), RD32(0x04000244), RD32(0x04000248),
  OP_END
};

/** @brief What the oracle read for probe1. */
static const uint32_t want1[] = {
    0x000000a1u, 0x000000b2u, 0x000000c4u, 0x000000d8u, 0x000000e1u, 0x000000f1u,
    0x00000061u, 0x00000081u, 0x00000091u, 0x00000000u, 0x00000000u, 0x000000a1u,
    0x000000a1u, 0x000000a1u, 0x000000a1u, 0x00000091u, 0x00000000u, 0x66770000u,
    0x000000a1u, 0x000000b2u, 0x000000c4u, 0x000000d8u, 0x000000a1u, 0x000000d8u,
    0x00000000u, 0x00000000u, 0x00008981u, 0x99918981u, 0x000000b3u, 0x0000b300u,
    0x12345678u, 0x12345678u, 0x000000e1u, 0x000000e3u, 0x000000e3u, 0x000000f1u,
    0x00000000u, 0x00000000u, 0x000000e1u, 0x000000a1u, 0x000000f1u, 0x000000e1u,
    0x000000a1u, 0x000000c5u, 0x00000091u, 0x000000d8u, 0x000000c5u, 0x000000d8u,
    0x000000d8u, 0x00000000u, 0x00000000u, 0x000000d9u, 0x00000091u, 0x00000091u,
    0x000000a1u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00008080u, 0x03808080u,
    0x00008080u,
};

/** @brief The second probe: mirrors of the small banks, OFS bits the modes ignore or refuse. */
static const uint32_t probe2[] = {
  LCDC_ALL,
  W32(0x06880000, 0x000000E1), W32(0x06884000, 0x000000E2), W32(0x06888000, 0x000000E3), W32(0x0688C000, 0x000000E4),
  W32(0x06890000, 0x000000F1), W32(0x06894000, 0x00000061), W32(0x06898000, 0x00000081), W32(0x0689C000, 0x00000082),
  W32(0x068A0000, 0x00000091), W32(0x06840000, 0x000000C4), W32(0x06860000, 0x000000D8),
  /* 0: H and I at BG-B */
  W8(VRAMCNT_H, 0x81), W8(VRAMCNT_I, 0x81),
  RD32(0x06200000), RD32(0x06204000), RD32(0x06208000), RD32(0x0620C000), RD32(0x06210000), RD32(0x06214000),
  RD32(0x06218000), RD32(0x0621C000),
  /* 8: only I at BG-B */
  W8(VRAMCNT_H, 0x80),
  RD32(0x06200000), RD32(0x06204000), RD32(0x06208000), RD32(0x0620C000), RD32(0x06210000), RD32(0x06218000),
  /* 14: E, F, G at OBJ-A; then A to OBJ-A ofs 2 (OFS.1 set) */
  W8(VRAMCNT_I, 0x80), W8(VRAMCNT_E, 0x82), W8(VRAMCNT_F, 0x8A), W8(VRAMCNT_G, 0x9A),
  RD32(0x06400000), RD32(0x06404000), RD32(0x06410000), RD32(0x06414000), RD32(0x06418000), RD32(0x06420000),
  RD32(0x06424000), RD32(0x06434000), RD32(0x0643C000),
  W8(VRAMCNT_E, 0x80), W8(VRAMCNT_F, 0x80), W8(VRAMCNT_G, 0x80),
  W8(VRAMCNT_A, 0x92), RD32(0x06400000), RD32(0x06420000), RD32(0x06440000), W8(VRAMCNT_A, 0x80),
  /* 26: F, G at BG-A all offsets; E with OFS bits set */
  W8(VRAMCNT_F, 0x91), RD32(0x06000000), RD32(0x06010000), RD32(0x06014000), RD32(0x06020000), RD32(0x06030000),
  W8(VRAMCNT_F, 0x80), W8(VRAMCNT_E, 0x99), RD32(0x06000000), RD32(0x06010000), W8(VRAMCNT_E, 0x80),
  /* 33: B at OBJ-A ofs 3, ofs 2 */
  W8(VRAMCNT_B, 0x9A), RD32(0x06400000), RD32(0x06420000), W8(VRAMCNT_B, 0x92), RD32(0x06400000), RD32(0x06420000),
  W8(VRAMCNT_B, 0x80),
  /* 37: C at BG-B with OFS bits; D at OBJ-B with OFS; H, I with OFS */
  W8(VRAMCNT_C, 0x9C), RD32(0x06200000), W8(VRAMCNT_C, 0x80), W8(VRAMCNT_D, 0x8C), RD32(0x06600000), W8(VRAMCNT_D, 0x80),
  W8(VRAMCNT_H, 0x99), RD32(0x06200000), W8(VRAMCNT_H, 0x80), W8(VRAMCNT_I, 0x99), RD32(0x06208000), W8(VRAMCNT_I, 0x80),
  /* 41: VRAMCNT bits 5-6 readback */
  W8(VRAMCNT_A, 0xE0), RD8(VRAMCNT_A), W8(VRAMCNT_A, 0x80),
  W8(VRAMCNT_C, 0x82), W8(VRAMCNT_D, 0x82),
  OP_END
};

/** @brief What the oracle read for probe2. */
static const uint32_t want2[] = {
    0x00000081u, 0x00000082u, 0x00000091u, 0x00000091u, 0x00000081u, 0x00000082u,
    0x00000091u, 0x00000091u, 0x00000000u, 0x00000000u, 0x00000091u, 0x00000091u,
    0x00000000u, 0x00000091u, 0x000000e1u, 0x000000f3u, 0x00000000u, 0x00000061u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000000u, 0x000000f1u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x000000e1u, 0x00000000u, 0x00000000u, 0x00000000u, 0x00000000u,
    0x00000000u, 0x000000c4u, 0x000000d8u, 0x00000081u, 0x00000091u, 0x00000080u,
};

/** @brief A machine as the cartridge boot leaves it for these probes: the shared work RAM given to the ARM7. */
static ndst *boot(void)
{
    ndst *t = ndst_new();
    w9(t, 0x04000247u, 1, 3);
    return t;
}

static void check_probe(const uint32_t *ops, const uint32_t *want, uint32_t n)
{
    ndst *t = boot();
    uint32_t got[128];
    CHECK_EQ(ndst_scene(t, ops, got, 128), n);
    for (uint32_t i = 0; i < n; i++) {
        if (got[i] != want[i])
            fprintf(stderr, "probe read %u: got 0x%08x, the oracle 0x%08x\n", i, got[i], want[i]);
        CHECK_EQ(got[i], want[i]);
    }
}

TEST(vram_banks_map_as_the_oracle_reads_them)
{
    check_probe(probe1, want1, (uint32_t)(sizeof want1 / sizeof want1[0]));
}

TEST(vram_small_banks_mirror_as_the_oracle_reads_them)
{
    check_probe(probe2, want2, (uint32_t)(sizeof want2 / sizeof want2[0]));
}

TEST(vram_c_and_d_give_the_arm7_work_ram_and_overlap_as_or)
{
    ndst *t = boot();
    uint32_t got[128];
    ndst_scene(t, probe2, got, 128);
    CHECK_EQ(r7(t, 0x06000000u, 4), 0xDCu);
    CHECK_EQ(r7(t, 0x06020000u, 4), 0);
    CHECK_EQ(r7(t, 0x04000240u, 2), 0x0303u);
    w7(t, 0x06000001u, 1, 0x5A);
    CHECK_EQ(r7(t, 0x06000000u, 4), 0x5ADCu);
    w9(t, VRAMCNT_D, 1, 0x80);
    CHECK_EQ(r7(t, 0x06000000u, 4), 0x5AC4u);
    CHECK_EQ(r9(t, 0x06860000u, 4), 0x5AD8u);
    CHECK_EQ(r7(t, 0x04000240u, 1), 1);
}

TEST(vram_code_in_arm7_banks_is_watched)
{
    ndst *t = ndst_new();
    w9(t, VRAMCNT_C, 1, 0x82);
    rl_nds_code_watch(t->m, RL_NDS_ARM7, 0x06000000u, 4);
    uint64_t e = rl_nds_code_epoch(t->m);
    w7(t, 0x06000000u, 4, 0xE12FFF1Eu);
    CHECK_EQ(rl_nds_code_epoch(t->m), e + 1);
    const uint8_t want[4] = {0x1E, 0xFF, 0x2F, 0xE1};
    CHECK(rl_nds_code_equal(t->m, RL_NDS_ARM7, 0x06000000u, want, 4));
}
