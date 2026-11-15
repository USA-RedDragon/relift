/**
 * @file test_copy.c
 * @brief CpuSet and CpuFastSet: copy and fill, counts, the ignored control
 * bits, misaligned addresses, the BIOS-region check and the registers left.
 */
#include "gbat.h"

static void pattern(gbat *g, uint32_t a, unsigned n)
{
    for (unsigned i = 0; i < n; i++)
        gbat_w8(g, a + i, 0x10 + i);
}

TEST(cpu_set_copies_halfwords_and_words)
{
    gbat *g = gbat_new();
    pattern(g, 0x02000000u, 64);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 3, 0);
    CHECK_EQ(gbat_swi(g, 0x0B, &c), RL_GBA_SWI_OK);
    CHECK_EQ(gbat_r16(g, 0x02001004u), 0x1514);
    CHECK_EQ(gbat_r16(g, 0x02001006u), 0);
    CHECK_EQ(c.r[0], 0x02000000u);
    CHECK_EQ(c.r[1], 0x02001000u);
    CHECK_EQ(c.r[3], RL_GBA_BIOS_SWI_RETURN);
    c = gbat_cpu(0x02000000u, 0x02002000u, 0x04000003u, 0);
    gbat_swi(g, 0x0B, &c);
    CHECK_EQ(gbat_r32(g, 0x02002008u), 0x1B1A1918u);
    CHECK_EQ(gbat_r32(g, 0x0200200Cu), 0);
    CHECK_EQ(c.r[0], 0x0200000Cu);
    CHECK_EQ(c.r[1], 0x0200200Cu);
}

TEST(cpu_set_fills_and_ignores_the_other_control_bits)
{
    gbat *g = gbat_new();
    gbat_w32(g, 0x02000000u, 0xCAFEF00Du);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 0xFAE00002u | 0x05000000u, 0);
    gbat_swi(g, 0x0B, &c);
    CHECK_EQ(gbat_r32(g, 0x02001000u), 0xCAFEF00Du);
    CHECK_EQ(gbat_r32(g, 0x02001004u), 0xCAFEF00Du);
    CHECK_EQ(gbat_r32(g, 0x02001008u), 0);
    CHECK_EQ(c.r[0], 0x02000004u);
    c = gbat_cpu(0x02000001u, 0x02003001u, 0x01000002u, 0);
    gbat_swi(g, 0x0B, &c);
    CHECK_EQ(gbat_r32(g, 0x02003000u), 0x00F000F0u);
}

TEST(cpu_set_rotates_misaligned_halfword_reads)
{
    gbat *g = gbat_new();
    gbat_w16(g, 0x02000000u, 0x1234);
    rl_gba_swi_cpu c = gbat_cpu(0x02000001u, 0x02001000u, 0x01000001u, 0);
    gbat_swi(g, 0x0B, &c);
    CHECK_EQ(gbat_r16(g, 0x02001000u), 0x0012);
}

TEST(cpu_set_refuses_sources_in_the_bios)
{
    gbat *g = gbat_new();
    static const uint32_t src[] = {0, 0x100, 0x00003FF0u, 0x01FFFFF0u};
    for (unsigned i = 0; i < 4; i++) {
        gbat_w32(g, 0x02001000u, 0x55555555u);
        rl_gba_swi_cpu c = gbat_cpu(src[i], 0x02001000u, 0x04000008u, 0x1234);
        CHECK_EQ(gbat_swi(g, 0x0B, &c), RL_GBA_SWI_OK);
        CHECK_EQ(gbat_r32(g, 0x02001000u), 0x55555555u);
        CHECK_EQ(c.r[0], src[i]);
        CHECK_EQ(c.r[3], RL_GBA_BIOS_SWI_RETURN);
        c = gbat_cpu(src[i], 0x02001000u, 0x00000008u, 0x1234);
        gbat_swi(g, 0x0C, &c);
        CHECK_EQ(gbat_r32(g, 0x02001000u), 0x55555555u);
        CHECK_EQ(c.r[3], 0x1234);
    }
    gbat_w32(g, 0x02001000u, 0x55555555u);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 0x04000000u, 0x1234);
    gbat_swi(g, 0x0B, &c);
    CHECK_EQ(gbat_r32(g, 0x02001000u), 0x55555555u);
}

TEST(cpu_set_checks_only_address_bits_25_to_27)
{
    gbat *g = gbat_new();
    gbat_w32(g, 0x02001000u, 0x55555555u);
    rl_gba_swi_cpu c = gbat_cpu(0x10000000u, 0x02001000u, 0x04000001u, 0);
    gbat_swi(g, 0x0B, &c);
    CHECK_EQ(gbat_r32(g, 0x02001000u), 0x55555555u);
    gbat_w32(g, 0x03000000u, 0x01020304u);
    c = gbat_cpu(0x11000000u, 0x02001000u, 0x04000001u, 0);
    gbat_swi(g, 0x0B, &c);
    CHECK_EQ(gbat_r32(g, 0x02001000u), 0x55555555u);
    c = gbat_cpu(0x23000000u, 0x02001000u, 0x04000001u, 0);
    gbat_swi(g, 0x0B, &c);
    CHECK_EQ(gbat_r32(g, 0x02001000u), 0x01020304u);
}

TEST(cpu_fast_set_rounds_up_to_eight_words)
{
    gbat *g = gbat_new();
    pattern(g, 0x02000000u, 128);
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 9, 0x1234);
    gbat_swi(g, 0x0C, &c);
    CHECK_EQ(gbat_r32(g, 0x0200103Cu), 0x4F4E4D4Cu);
    CHECK_EQ(gbat_r32(g, 0x02001040u), 0);
    CHECK_EQ(c.r[0], 0x02000040u);
    CHECK_EQ(c.r[1], 0x02001040u);
    CHECK_EQ(c.r[3], 0x37363534u);
    c = gbat_cpu(0x02000000u, 0x02002000u, 0x01000001u, 0x1234);
    gbat_swi(g, 0x0C, &c);
    CHECK_EQ(gbat_r32(g, 0x0200201Cu), 0x13121110u);
    CHECK_EQ(c.r[0], 0x02000000u);
    CHECK_EQ(c.r[1], 0x02002020u);
    CHECK_EQ(c.r[3], 0x13121110u);
    c = gbat_cpu(0x02000000u, 0x02002000u, 0, 0x1234);
    gbat_swi(g, 0x0C, &c);
    CHECK_EQ(c.r[3], 0x1234);
}

TEST(cpu_set_pushes_its_frame)
{
    gbat *g = gbat_new();
    rl_gba_swi_cpu c = gbat_cpu(0x02000000u, 0x02001000u, 1, 0);
    gbat_swi(g, 0x0B, &c);
    CHECK_EQ(gbat_r32(g, 0x03007EFCu), 0x08000200u);
    CHECK_EQ(gbat_r32(g, 0x03007EF8u), 1);
    CHECK_EQ(gbat_r32(g, 0x03007EF4u), RL_GBA_BIOS_SWI_RETURN);
    CHECK_EQ(gbat_r32(g, 0x03007EF0u), 0x55555555u);
    CHECK_EQ(gbat_r32(g, 0x03007EECu), 0x44444444u);
    CHECK_EQ(gbat_r32(g, 0x03007FD0u), 0x6000009Fu);
    CHECK_EQ(gbat_r32(g, 0x03007FD4u), 0xBBBBBBBBu);
    CHECK_EQ(gbat_r32(g, 0x03007FD8u), 0xCCCCCCCCu);
    CHECK_EQ(gbat_r32(g, 0x03007FDCu), 0x08000104u);
}
