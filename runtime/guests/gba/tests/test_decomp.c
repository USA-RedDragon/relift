/**
 * @file test_decomp.c
 * @brief BitUnPack, LZ77, Huffman, run-length and difference unfiltering,
 * their 8-bit and 16-bit write forms and the registers they leave.
 */
#include "gbat.h"

static void bytes(gbat *g, uint32_t a, const uint8_t *b, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        gbat_w8(g, a + i, b[i]);
}

static const uint8_t lz[] = {0x10, 0x0B, 0x00, 0x00, 0x20, 'a', 'b', 0x50, 0x01, 'c', 0, 0, 0, 0, 0, 0, 0};

TEST(lz77_wram_expands_back_references)
{
    gbat *g = gbat_new();
    bytes(g, 0x02000000u, lz, sizeof lz);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 0, 0x1234);
    CHECK_EQ(gbat_swi(g, 0x11, &c), RL_GBA_SWI_OK);
    static const char want[] = "abababababc";
    for (unsigned i = 0; i < 11; i++)
        CHECK_EQ(gbat_r8(g, 0x02001000u + i), (uint8_t)want[i]);
    CHECK_EQ(c.r[0], 0x0200000Au);
    CHECK_EQ(c.r[1], 0x0200100Bu);
    CHECK_EQ(c.r[3], 0);
}

TEST(lz77_vram_writes_halfwords_and_keeps_the_odd_byte)
{
    gbat *g = gbat_new();
    bytes(g, 0x02000000u, lz, sizeof lz);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x06001000u, 0, 0x1234);
    gbat_swi(g, 0x12, &c);
    CHECK_EQ(gbat_r16(g, 0x06001000u), 0x6261);
    CHECK_EQ(gbat_r16(g, 0x06001006u), 0x6261);
    CHECK_EQ(gbat_r16(g, 0x06001008u), 0x6261);
    CHECK_EQ(gbat_r16(g, 0x0600100Au), 0);
    CHECK_EQ(c.r[1], 0x0600100Au);
    CHECK_EQ(c.r[3], 'c');
}

TEST(decompressors_refuse_empty_and_bios_sources)
{
    gbat *g = gbat_new();
    gbat_w32(g, 0x02000000u, 0x00000010u);
    for (unsigned n = 0x11; n <= 0x15; n++) {
        rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 0, 0x1234);
        CHECK_EQ(gbat_swi(g, n, &c), RL_GBA_SWI_OK);
        CHECK_EQ(gbat_r32(g, 0x02001000u), 0);
    }
    rl_gba_swi_cpu c = gbat_cpu(0x00000100u, 0x02001000u, 0, 0x1234);
    gbat_swi(g, 0x11, &c);
    CHECK_EQ(gbat_r32(g, 0x02001000u), 0);
    CHECK_EQ(c.r[0], 0x00000104u);
}

TEST(run_length_expands_runs_and_literals)
{
    gbat *g = gbat_new();
    static const uint8_t rl[] = {0x30, 0x07, 0x00, 0x00, 0x81, 'x', 0x02, 'a', 'b', 'c'};
    bytes(g, 0x02000000u, rl, sizeof rl);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 0, 0x1234);
    gbat_swi(g, 0x14, &c);
    static const char want[] = "xxxxabc";
    for (unsigned i = 0; i < 7; i++)
        CHECK_EQ(gbat_r8(g, 0x02001000u + i), (uint8_t)want[i]);
    CHECK_EQ(c.r[0], 0x0200000Au);
    CHECK_EQ(c.r[1], 0x02001007u);
    CHECK_EQ(c.r[3], RL_GBA_BIOS_SWI_RETURN);
    c = gbat_cpu(0x02000000u, 0x06000000u, 0, 0);
    gbat_swi(g, 0x15, &c);
    CHECK_EQ(gbat_r16(g, 0x06000004u), 0x6261);
    CHECK_EQ(gbat_r16(g, 0x06000006u), 0);
    CHECK_EQ(c.r[1], 0x06000006u);
    CHECK_EQ(gbat_r32(g, 0x03007EDCu), 0x02);
    CHECK_EQ(gbat_r32(g, 0x03007EE0u), 'x');
}

TEST(huffman_decodes_four_and_eight_bit_data)
{
    gbat *g = gbat_new();
    static const uint8_t hf[] = {0x28, 0x04, 0x00, 0x00, 0x01, 0xC0, 0x41, 0x42, 0x00, 0x00, 0x00, 0x50};
    bytes(g, 0x02000000u, hf, sizeof hf);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 0, 0x1234);
    gbat_swi(g, 0x13, &c);
    CHECK_EQ(gbat_r32(g, 0x02001000u), 0x42414241u);
    CHECK_EQ(c.r[0], 0x0200000Cu);
    CHECK_EQ(c.r[1], 0x02001004u);
    CHECK_EQ(c.r[3], 0x42414241u);
}

TEST(huffman_stores_size_mod_eight_plus_four_leaves_per_word)
{
    gbat *g = gbat_new();
    static const uint8_t hf[] = {0x22, 0x08, 0x00, 0x00, 0x01, 0xC0, 0x01, 0x02, 0xAA, 0xAA, 0xAA, 0xAA};
    bytes(g, 0x02000000u, hf, sizeof hf);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 0, 0x1234);
    gbat_swi(g, 0x13, &c);
    CHECK_EQ(gbat_r32(g, 0x02001000u), 0x66600000u);
    CHECK_EQ(gbat_r32(g, 0x02001004u), 0x66666600u);
    CHECK_EQ(c.r[0], 0x0200000Cu);
    CHECK_EQ(c.r[1], 0x02001008u);
    CHECK_EQ(c.r[3], 0x66666600u);
}

TEST(bit_unpack_adds_the_offset)
{
    gbat *g = gbat_new();
    gbat_w8(g, 0x02000000u, 0xB4);
    gbat_w16(g, 0x02000100u, 1);
    gbat_w8(g, 0x02000102u, 2);
    gbat_w8(g, 0x02000103u, 8);
    gbat_w32(g, 0x02000104u, 0x10);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 0x02000100u, 0x1234);
    gbat_swi(g, 0x10, &c);
    CHECK_EQ(gbat_r32(g, 0x02001000u), 0x12131100u);
    CHECK_EQ(c.r[3], 0);
    gbat_w32(g, 0x02000104u, 0x80000010u);
    c = gbat_cpu(0x02000000u, 0x02001000u, 0x02000100u, 0x1234);
    gbat_swi(g, 0x10, &c);
    CHECK_EQ(gbat_r32(g, 0x02001000u), 0x12131110u);
}

TEST(diff_unfilters_sum_and_leave_r3)
{
    gbat *g = gbat_new();
    static const uint8_t d8[] = {0x81, 0x04, 0x00, 0x00, 0x10, 0x01, 0x02, 0xFF};
    bytes(g, 0x02000000u, d8, sizeof d8);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 0, 0x1234);
    gbat_swi(g, 0x16, &c);
    CHECK_EQ(gbat_r32(g, 0x02001000u), 0x12131110u);
    CHECK_EQ(c.r[3], RL_GBA_BIOS_SWI_RETURN);
    static const uint8_t d16[] = {0x82, 0x04, 0x00, 0x00, 0x00, 0x10, 0x01, 0x00};
    bytes(g, 0x02000000u, d16, sizeof d16);
    c = gbat_cpu(0x02000000u, 0x02002000u, 0, 0x1234);
    gbat_swi(g, 0x18, &c);
    CHECK_EQ(gbat_r32(g, 0x02002000u), 0x10011000u);
    CHECK_EQ(c.r[3], 1);
}
