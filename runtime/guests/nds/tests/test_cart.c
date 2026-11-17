/**
 * @file test_cart.c
 * @brief The cartridge slot: reads of the image with the secure-area rule,
 * mirrors and 4 KiB wrap, the chip ID, ROMCTRL's timing against the oracle
 * (a program started each transfer and a DMA channel in cartridge mode took
 * the data; the timer ran from before the ROMCTRL write to the end; a fixed
 * 11 bus cycles of the program's own accesses are in every oracle number), the
 * two-word buffering, the register masks, the transfer-complete IRQ and the
 * DMA requests.
 */
#include "ndst.h"

#include "nds_int.h"

#include <stdlib.h>
#include <string.h>

#define ROMCTRL 0x040001A4u
#define AUXCNT 0x040001A0u
#define PORT 0x04100010u

enum { ROM_LEN = 0x20000 };

static uint8_t rom[ROM_LEN];

static uint8_t pat(uint32_t i) { return (uint8_t)((i * 7u + 3u) ^ (i >> 8) ^ (i >> 15)); }

static ndst *fixture(void)
{
    ndst *t = ndst_new();
    for (uint32_t i = 0; i < ROM_LEN; i++)
        rom[i] = pat(i);
    memset(rom, 0, 0x200);
    rom[0x62] = 0x58;
    rom[0x63] = 0;
    rom[0x61] = 0x60;
    if (rl_nds_set_cartridge(t->m, rom, ROM_LEN) != 0)
        abort();
    ndst_to(t, RL_NDS_ARM7, 1ull << 40);
    return t;
}

static void command(ndst *t, uint8_t op, uint32_t addr)
{
    w9(t, 0x040001A8u, 4, (uint32_t)op | (addr >> 24) << 8 | ((addr >> 16) & 0xFF) << 16 | ((addr >> 8) & 0xFF) << 24);
    w9(t, 0x040001ACu, 4, addr & 0xFF);
}

/** @brief Starts a transfer with rc and reads every word as soon as it shows (polling each tick); returns the tick it ended. */
static uint64_t transfer(ndst *t, uint32_t rc, uint32_t *words, unsigned max, unsigned *count)
{
    uint64_t t0 = rl_sched_now(t->k[RL_NDS_ARM9]);
    w9(t, ROMCTRL, 4, rc | 0x80000000u);
    unsigned n = 0;
    uint64_t end = 0;
    for (unsigned i = 0; i < 400000; i++) {
        uint32_t v = r9(t, ROMCTRL, 4);
        if (v & 0x800000u) {
            uint32_t w = r9(t, PORT, 4);
            if (words && n < max)
                words[n] = w;
            n++;
        }
        if (!(r9(t, ROMCTRL, 4) & 0x80000000u)) {
            end = rl_sched_now(t->k[RL_NDS_ARM9]);
            break;
        }
        ndst_to(t, RL_NDS_ARM9, rl_sched_now(t->k[RL_NDS_ARM9]) + 1);
    }
    if (count)
        *count = n;
    return end - t0;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

TEST(b7_returns_the_image_with_the_secure_area_rule_mirrors_and_4k_wrap)
{
    ndst *t = fixture();
    uint32_t w[128];
    unsigned n;
    const uint32_t rc = (1u << 24);
    command(t, 0xB7, 0x9000);
    transfer(t, rc, w, 128, &n);
    CHECK_EQ(n, 128);
    for (unsigned i = 0; i < 128; i++)
        CHECK_EQ(w[i], le32(&rom[0x9000 + 4 * i]));
    static const struct {
        uint32_t addr, from;
    } c[] = {
        {0x0, 0x8000},          {0x200, 0x8000},        {0x4000, 0x8000},       {0x7FF0, 0x81F0},
        {0x8000, 0x8000},       {0x8200, 0x8200},       {0x29000, 0x9000},      {0x20010, 0x8010},
    };
    for (unsigned k = 0; k < 8; k++) {
        command(t, 0xB7, c[k].addr);
        transfer(t, 7u << 24, w, 1, &n);
        CHECK_EQ(n, 1);
        CHECK_EQ(w[0], le32(&rom[c[k].from]));
    }
    command(t, 0xB7, 0x9FFC);
    transfer(t, 2u << 24, w, 128, &n);
    CHECK_EQ(w[0], le32(&rom[0x9FFC]));
    CHECK_EQ(w[1], le32(&rom[0x9000]));
    CHECK_EQ(w[2], le32(&rom[0x9004]));
}

TEST(other_commands_return_zero_and_b8_the_chip_id)
{
    ndst *t = fixture();
    uint32_t w;
    unsigned n;
    static const uint8_t ops[] = {0x9F, 0x00, 0x90, 0x3C, 0xA0};
    for (unsigned i = 0; i < sizeof ops; i++) {
        command(t, ops[i], 0);
        transfer(t, 7u << 24, &w, 1, &n);
        CHECK_EQ(n, 1);
        CHECK_EQ(w, 0);
    }
    command(t, 0xB8, 0);
    transfer(t, 7u << 24, &w, 1, &n);
    CHECK_EQ(w, 0xC2);
    CHECK_EQ(rl_nds_chip_id(t->m), 0xC2);
}

TEST(the_chip_id_follows_the_size_and_the_header_clock)
{
    ndst *t = ndst_new();
    static uint8_t hdr[0x200];
    CHECK(rl_nds_set_cartridge(t->m, hdr, (size_t)128 << 20) == 0);
    CHECK_EQ(rl_nds_chip_id(t->m), 0x7FC2);
    CHECK(rl_nds_set_cartridge(t->m, hdr, (size_t)256 << 20) == 0);
    CHECK_EQ(rl_nds_chip_id(t->m), 0xFFC2);
    CHECK(rl_nds_set_cartridge(t->m, hdr, (size_t)16 << 20) == 0);
    CHECK_EQ(rl_nds_chip_id(t->m), 0x0FC2);
    hdr[0x60] = 0x57;
    hdr[0x61] = 0x66;
    CHECK(rl_nds_set_cartridge(t->m, hdr, (size_t)16 << 20) == 0);
    CHECK_EQ(rl_nds_chip_id(t->m), 0x80000FC2u);
    CHECK(rl_nds_set_cartridge(t->m, hdr, 0x100) != 0);
}

/**
 * @brief One row of the oracle's transfer times in bus cycles (all with the program's 11 cycles in them). The
 * consoles take 14 cycles more from the start bit, and a one-word transfer moves its word at the fast clock
 * whatever the clock bit says (check/nds-timing's cartridge cases); the test adds both to the oracle's times.
 */
typedef struct row {
    unsigned clk;
    uint32_t size_code;
    uint32_t g1, g2;
    uint32_t cycles;
} row;

TEST(romctrl_transfer_times_match_the_consoles_within_the_oracles_gap_jitter)
{
    ndst *t = fixture();
    static const uint32_t sizes[4] = {7, 1, 2, 3};
    static const uint32_t g1s[3] = {0, 0x100, 0x657};
    static const uint32_t g2s[2] = {0, 0x18};
    static const uint32_t oracle[2][4][3][2] = {
        {{{71, 191}, {1349, 1469}, {8189, 8309}},
         {{2611, 2731}, {3889, 4009}, {10729, 10849}},
         {{5171, 5411}, {6449, 6689}, {13289, 13529}},
         {{10291, 10771}, {11569, 12049}, {18409, 18889}}},
        {{{107, 299}, {2153, 2345}, {13091, 13283}},
         {{4171, 4363}, {6217, 6409}, {17155, 17347}},
         {{8267, 8651}, {10313, 10697}, {21251, 21635}},
         {{16459, 17227}, {18505, 19273}, {29443, 30211}}},
    };
    unsigned bad = 0;
    for (unsigned clk = 0; clk < 2; clk++)
        for (unsigned s = 0; s < 4; s++)
            for (unsigned a = 0; a < 3; a++)
                for (unsigned b = 0; b < 2; b++) {
                    command(t, 0xB7, 0x9000);
                    uint32_t rc = (clk << 27) | (sizes[s] << 24) | g1s[a] | (g2s[b] << 16);
                    uint64_t ticks = transfer(t, rc, NULL, 0, NULL);
                    int64_t got = (int64_t)(ticks / 2) + 11, want = oracle[clk][s][a][b] + 14;
                    if (clk && sizes[s] == 7)
                        want -= 4 * (8 - 5);
                    if (got < want - 3 || got > want + 3) {
                        fprintf(stderr, "clk %u size %u g1 %x g2 %x: %lld cycles, oracle %lld\n", clk, sizes[s], g1s[a], g2s[b],
                                (long long)got, (long long)want);
                        bad++;
                    }
                    ndst_to(t, RL_NDS_ARM9, rl_sched_now(t->k[RL_NDS_ARM9]) + 64);
                }
    CHECK_EQ(bad, 0);
}

TEST(a_command_without_data_ends_after_the_start_the_command_and_gap1)
{
    ndst *t = fixture();
    command(t, 0xB7, 0x9000);
    CHECK_EQ(transfer(t, 0, NULL, 0, NULL) / 2, 14 + 40);
    command(t, 0xB7, 0x9000);
    CHECK_EQ(transfer(t, 0x100, NULL, 0, NULL) / 2, 14 + 40 + 256 * 5);
    command(t, 0xB7, 0x9000);
    CHECK_EQ(transfer(t, 0x08000000u | 0x100, NULL, 0, NULL) / 2, 14 + 64 + 256 * 8);
    command(t, 0xB7, 0x9000);
    CHECK_EQ(transfer(t, 0x180000, NULL, 0, NULL) / 2, 14 + 40);
}

TEST(an_unread_word_stalls_the_transfer_after_two_buffered_words)
{
    ndst *t = fixture();
    command(t, 0xB7, 0x9000);
    uint64_t t0 = rl_sched_now(t->k[RL_NDS_ARM9]);
    w9(t, ROMCTRL, 4, 0x80000000u | (1u << 24));
    ndst_to(t, RL_NDS_ARM9, t0 + 1000000);
    uint32_t v = r9(t, ROMCTRL, 4);
    CHECK(v & 0x80000000u);
    CHECK(v & 0x800000u);
    uint32_t a = r9(t, PORT, 4), b = r9(t, PORT, 4);
    CHECK_EQ(a, le32(&rom[0x9000]));
    CHECK_EQ(b, le32(&rom[0x9004]));
    v = r9(t, ROMCTRL, 4);
    CHECK(!(v & 0x800000u));
    uint64_t now = rl_sched_now(t->k[RL_NDS_ARM9]);
    ndst_to(t, RL_NDS_ARM9, now + 40);
    v = r9(t, ROMCTRL, 4);
    CHECK(v & 0x800000u);
    for (unsigned i = 2; i < 128; i++) {
        ndst_to(t, RL_NDS_ARM9, rl_sched_now(t->k[RL_NDS_ARM9]) + 100);
        while (!(r9(t, ROMCTRL, 4) & 0x800000u))
            ndst_to(t, RL_NDS_ARM9, rl_sched_now(t->k[RL_NDS_ARM9]) + 4);
        CHECK_EQ(r9(t, PORT, 4), le32(&rom[0x9000 + 4 * i]));
    }
    CHECK(!(r9(t, ROMCTRL, 4) & 0x80000000u));
}

TEST(register_masks_and_write_only_registers_read_as_the_oracle_does)
{
    ndst *t = fixture();
    CHECK_EQ(r9(t, ROMCTRL, 4), 0x20000000u);
    CHECK_EQ(r9(t, AUXCNT, 2), 0x8000);
    w9(t, ROMCTRL, 4, 0x7FFFFFFFu);
    CHECK_EQ(r9(t, ROMCTRL, 4), 0x7F7F7FFFu);
    w9(t, ROMCTRL, 4, 0);
    CHECK_EQ(r9(t, ROMCTRL, 4), 0x20000000u);
    w9(t, AUXCNT, 2, 0xE0FF);
    CHECK_EQ(r9(t, AUXCNT, 2), 0xE043);
    w9(t, 0x040001A8u, 4, 0x12345678u);
    w9(t, 0x040001B0u, 4, 0xFFFFFFFFu);
    w9(t, 0x040001B4u, 4, 0xFFFFFFFFu);
    w9(t, 0x040001B8u, 2, 0xFFFF);
    w9(t, 0x040001BAu, 2, 0xFFFF);
    CHECK_EQ(r9(t, 0x040001A8u, 4), 0);
    CHECK_EQ(r9(t, 0x040001B0u, 4), 0);
    CHECK_EQ(r9(t, 0x040001B4u, 4), 0);
    CHECK_EQ(r9(t, 0x040001B8u, 2), 0);
    CHECK_EQ(r9(t, 0x040001BAu, 2), 0);
    CHECK_EQ(r7(t, ROMCTRL, 4), 0x20000000u);
}

TEST(a_disabled_slot_does_not_start_and_the_done_irq_goes_to_the_owner)
{
    ndst *t = fixture();
    command(t, 0xB7, 0x9000);
    w9(t, AUXCNT, 2, 0);
    w9(t, ROMCTRL, 4, 0x80000000u | (7u << 24));
    CHECK(!(r9(t, ROMCTRL, 4) & 0x80000000u));
    w9(t, AUXCNT, 2, 0xC000);
    w9(t, ROMCTRL, 4, 0x80000000u | (7u << 24));
    CHECK(r9(t, ROMCTRL, 4) & 0x80000000u);
    ndst_to(t, RL_NDS_ARM9, rl_sched_now(t->k[RL_NDS_ARM9]) + 400);
    CHECK(r9(t, ROMCTRL, 4) & 0x800000u);
    CHECK_EQ(r9(t, 0x04000214u, 4), 0);
    (void)r9(t, PORT, 4);
    CHECK_EQ(r9(t, 0x04000214u, 4), RL_NDS_IRQ_CARD_DONE);
    CHECK_EQ(r7(t, 0x04000214u, 4), 0);
    w9(t, 0x04000214u, 4, RL_NDS_IRQ_CARD_DONE);
    w9(t, 0x04000204u, 2, 0x0800);
    command(t, 0xB7, 0x9000);
    w9(t, ROMCTRL, 4, 0x80000000u | (7u << 24));
    ndst_to(t, RL_NDS_ARM9, rl_sched_now(t->k[RL_NDS_ARM9]) + 400);
    (void)r9(t, PORT, 4);
    CHECK_EQ(r9(t, 0x04000214u, 4), 0);
    CHECK_EQ(r7(t, 0x04000214u, 4), RL_NDS_IRQ_CARD_DONE);
}

TEST(a_dma_channel_in_cartridge_mode_takes_each_word_and_ends_the_block)
{
    ndst *t = ndst_new();
    for (uint32_t i = 0; i < ROM_LEN; i++)
        rom[i] = pat(i);
    if (rl_nds_set_cartridge(t->m, rom, ROM_LEN) != 0)
        abort();
    w9(t, 0x04000204u, 2, 0x0800);
    ndst_to(t, RL_NDS_ARM9, 1ull << 40);
    w7(t, 0x040001A8u, 4, 0xB7u | 0x90u << 24);
    w7(t, 0x040001ACu, 4, 0);
    w7(t, 0x040000B0u, 4, PORT);
    w7(t, 0x040000B4u, 4, 0x02200000u);
    w7(t, 0x040000B8u, 4, 0x80000000u | 0x20000000u | 0x04000000u | 0x02000000u | 0x01000000u | 1u);
    w7(t, AUXCNT, 2, 0xC000);
    w7(t, ROMCTRL, 4, 0x80000000u | (1u << 24));
    ndst_to(t, RL_NDS_ARM7, 20000);
    (void)r7(t, 0x04000214u, 4);
    for (unsigned i = 0; i < 128; i++)
        CHECK_EQ(r7(t, 0x02200000u + 4 * i, 4), le32(&rom[0x9000 + 4 * i]));
    CHECK(!(r7(t, ROMCTRL, 4) & 0x80000000u));
    CHECK_EQ(r7(t, 0x04000214u, 4), RL_NDS_IRQ_CARD_DONE);
}
