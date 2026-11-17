/**
 * @file test_si.c
 * @brief The Nintendo 64 serial interface and the PIF's joybus: SI DMA and
 * its timing, the status bits and the interrupt, the command block parse
 * (skips, resets, padding, the end code), the standard controller's
 * answers and the cycles a block takes.
 */
#include "runner.h"

#include "n64.h"
#include "n64_int.h"

#include <stdlib.h>
#include <string.h>

enum {
    SI_DRAM = 0x04800000u,
    SI_PIF_RD = 0x04800004u,
    SI_PIF_WR = 0x04800010u,
    SI_STATUS = 0x04800018u,
    PIF_RAM = 0x1FC007C0u,
    BLOCK = 0x1000,
    REPLY = 0x2000,
};

typedef struct mach {
    rl_sched *s;
    rl_sched_cpu *k;
    rl_n64 *m;
    uint8_t rom[0x2000];
} mach;

static void mach_new(mach *t)
{
    memset(t->rom, 0, sizeof t->rom);
    t->s = rl_sched_create(NULL);
    t->k = rl_sched_add_cpu(t->s, 1);
    t->m = rl_n64_create(t->rom, sizeof t->rom);
    rl_n64_attach(t->m, t->s, t->k);
    rl_n64_mi_lower(t->m, RL_N64_IRQ_ALL);
    rl_n64_write(t->m, 0x0430000Cu, 4, 0x8u);
}

static void mach_free(mach *t)
{
    rl_n64_destroy(t->m);
    rl_sched_destroy(t->s);
}

static void advance(mach *t, uint64_t n)
{
    rl_sched_stall(t->k, n);
    rl_sched_run(t->k);
}

/** @brief Sends the 64-byte block through an SI DMA to the PIF and waits for it. */
static void send_block(mach *t, const uint8_t *block)
{
    rl_n64_rdram_put(t->m, BLOCK, block, 64);
    rl_n64_write(t->m, SI_DRAM, 4, BLOCK);
    rl_n64_write(t->m, SI_PIF_WR, 4, PIF_RAM);
    advance(t, 4065u * 3u / 2u);
    rl_n64_write(t->m, SI_STATUS, 4, 0);
}

/** @brief Reads the PIF back through an SI DMA into REPLY and waits for it; returns the cycles waited. */
static uint64_t read_block(mach *t, uint8_t *out)
{
    rl_n64_write(t->m, SI_DRAM, 4, REPLY);
    rl_n64_write(t->m, SI_PIF_RD, 4, PIF_RAM);
    uint64_t waited = 0;
    while (rl_n64_irq_pending(t->m) == 0 && waited < 1000000) {
        advance(t, 1);
        waited++;
    }
    rl_n64_write(t->m, SI_STATUS, 4, 0);
    rl_n64_rdram_get(t->m, REPLY, out, 64);
    return waited;
}

static void block_new(uint8_t *b)
{
    memset(b, 0xFF, 64);
}

TEST(n64_si_dma_to_the_pif_takes_its_time_then_raises_the_interrupt)
{
    mach t;
    mach_new(&t);
    uint8_t block[64];
    block_new(block);
    block[0] = 0xFE;
    block[63] = 0;
    rl_n64_rdram_put(t.m, BLOCK, block, 64);
    rl_n64_write(t.m, SI_DRAM, 4, BLOCK);
    rl_n64_write(t.m, SI_PIF_WR, 4, PIF_RAM);
    CHECK_EQ(rl_n64_read(t.m, SI_STATUS, 4), 0x0411u);
    CHECK_EQ(rl_n64_read(t.m, SI_PIF_WR, 4), PIF_RAM);
    advance(&t, 4065u * 3u / 2u - 1u);
    CHECK_EQ(rl_n64_irq_pending(t.m), 0);
    CHECK_EQ(rl_n64_read(t.m, PIF_RAM, 4), 0u);
    advance(&t, 1);
    CHECK_EQ(rl_n64_irq_pending(t.m), RL_N64_IRQ_SI);
    CHECK_EQ(rl_n64_read(t.m, SI_STATUS, 4), 0x1000u);
    CHECK_EQ(rl_n64_read(t.m, PIF_RAM, 4), 0xFEFFFFFFu);
    rl_n64_write(t.m, SI_STATUS, 4, 0);
    CHECK_EQ(rl_n64_irq_pending(t.m), 0);
    CHECK_EQ(rl_n64_read(t.m, SI_STATUS, 4), 0u);
    mach_free(&t);
}

TEST(n64_si_dma_from_the_pif_lasts_as_long_as_the_block_on_the_wire)
{
    mach t;
    mach_new(&t);
    rl_n64_set_pad(t.m, 0, 0, 0, 0);
    uint8_t block[64];
    block_new(block);
    block[0] = 0x01;
    block[1] = 0x03;
    block[2] = 0x00;
    block[6] = 0xFE;
    block[63] = 1;
    send_block(&t, block);
    uint64_t cycles = (13600u + 22000u + 1420u) * 3u / 2u;
    rl_n64_write(t.m, SI_DRAM, 4, REPLY);
    rl_n64_write(t.m, SI_PIF_RD, 4, PIF_RAM);
    CHECK_EQ(rl_n64_read(t.m, SI_STATUS, 4), 0x0141u);
    advance(&t, cycles - 1u);
    CHECK_EQ(rl_n64_irq_pending(t.m), 0);
    advance(&t, 1);
    CHECK_EQ(rl_n64_irq_pending(t.m), RL_N64_IRQ_SI);
    CHECK_EQ(rl_n64_rdram(t.m)[(REPLY + 3) ^ 3], 0x05);
    mach_free(&t);
}

TEST(n64_controller_status_and_state_commands)
{
    mach t;
    mach_new(&t);
    rl_n64_set_pad(t.m, 0, 0, 0, 0);
    rl_n64_set_pad(t.m, 1, RL_N64_PAD_A | RL_N64_PAD_START | RL_N64_PAD_C_LEFT, 20, -30);
    uint8_t b[64], out[64];
    block_new(b);
    b[0] = 0x01;
    b[1] = 0x03;
    b[2] = 0x00;
    b[6] = 0x01;
    b[7] = 0x04;
    b[8] = 0x01;
    b[13] = 0xFE;
    b[63] = 1;
    send_block(&t, b);
    read_block(&t, out);
    CHECK_EQ(out[0], 0x01);
    CHECK_EQ(out[1], 0x03);
    CHECK_EQ(out[3], 0x05);
    CHECK_EQ(out[4], 0x00);
    CHECK_EQ(out[5], 0x02);
    CHECK_EQ(out[6], 0x01);
    CHECK_EQ(out[7], 0x04);
    CHECK_EQ(out[9], 0x90);
    CHECK_EQ(out[10], 0x02);
    CHECK_EQ(out[11], 20);
    CHECK_EQ(out[12], 0xE2);
    CHECK_EQ(out[13], 0xFE);
    CHECK_EQ(out[63], 0x00);
    mach_free(&t);
}

TEST(n64_an_empty_port_flags_its_command_and_a_long_reply_flags_over)
{
    mach t;
    mach_new(&t);
    rl_n64_set_pad(t.m, 0, 0, 0, 0);
    uint8_t b[64], out[64];
    block_new(b);
    b[0] = 0x01;
    b[1] = 0x05;
    b[2] = 0x01;
    b[8] = 0x01;
    b[9] = 0x03;
    b[10] = 0x00;
    b[14] = 0xFE;
    b[63] = 1;
    send_block(&t, b);
    read_block(&t, out);
    CHECK_EQ(out[1], 0x45);
    CHECK_EQ(out[9], 0x83);
    mach_free(&t);
}

TEST(n64_the_block_parse_honors_padding_skips_resets_and_the_end_code)
{
    mach t;
    mach_new(&t);
    rl_n64_set_pad(t.m, 0, RL_N64_PAD_B, 0, 0);
    rl_n64_set_pad(t.m, 1, RL_N64_PAD_Z, 0, 0);
    rl_n64_set_pad(t.m, 2, RL_N64_PAD_L, 0, 0);
    uint8_t b[64], out[64];
    block_new(b);
    b[0] = 0xFF;
    b[1] = 0x00;
    b[2] = 0xFD;
    b[3] = 0x01;
    b[4] = 0x04;
    b[5] = 0x01;
    b[10] = 0xFE;
    b[11] = 0x01;
    b[12] = 0x04;
    b[13] = 0x01;
    b[63] = 1;
    send_block(&t, b);
    read_block(&t, out);
    CHECK_EQ(out[0], 0xFF);
    CHECK_EQ(out[1], 0x00);
    CHECK_EQ(out[2], 0xFD);
    CHECK_EQ(out[4], 0x04);
    CHECK_EQ(out[6], 0x00);
    CHECK_EQ(out[7], 0x20);
    CHECK_EQ(out[10], 0xFE);
    CHECK_EQ(out[11], 0x01);
    CHECK_EQ(out[12], 0x04);
    CHECK_EQ(out[13], 0x01);
    CHECK_EQ(out[14], 0xFF);
    mach_free(&t);
}

TEST(n64_a_block_is_parsed_only_when_it_carries_the_flag)
{
    mach t;
    mach_new(&t);
    rl_n64_set_pad(t.m, 0, RL_N64_PAD_START, 0, 0);
    uint8_t b[64], out[64];
    block_new(b);
    b[0] = 0x01;
    b[1] = 0x04;
    b[2] = 0x01;
    b[7] = 0xFE;
    b[63] = 1;
    send_block(&t, b);
    read_block(&t, out);
    CHECK_EQ(out[63], 0x00);
    CHECK_EQ(out[3], 0x10);
    rl_n64_set_pad(t.m, 0, RL_N64_PAD_B, 0, 0);
    block_new(b);
    b[0] = 0x01;
    b[1] = 0x04;
    b[2] = 0x01;
    b[7] = 0xFE;
    b[63] = 0;
    send_block(&t, b);
    read_block(&t, out);
    CHECK_EQ(out[3], 0x40);
    block_new(b);
    b[2] = 0x01;
    b[3] = 0x04;
    b[4] = 0x01;
    b[9] = 0xFE;
    b[63] = 0;
    send_block(&t, b);
    read_block(&t, out);
    CHECK_EQ(out[5], 0xFF);
    b[63] = 1;
    send_block(&t, b);
    read_block(&t, out);
    CHECK_EQ(out[5], 0x40);
    mach_free(&t);
}

TEST(n64_pif_ram_by_the_cpu_and_the_write_window_of_the_bus)
{
    mach t;
    mach_new(&t);
    rl_n64_write(t.m, PIF_RAM + 4, 4, 0x11223344u);
    CHECK_EQ(rl_n64_read(t.m, SI_STATUS, 4), 0x09B3u);
    rl_n64_write(t.m, PIF_RAM + 8, 4, 0xAABBCCDDu);
    CHECK_EQ(rl_n64_read(t.m, PIF_RAM + 12, 4), 0x11223344u);
    CHECK_EQ(rl_n64_read(t.m, SI_STATUS, 4) & 2u, 0u);
    advance(&t, 2150u * 3u / 2u);
    CHECK_EQ(rl_n64_irq_pending(t.m), 0);
    CHECK_EQ(rl_n64_read(t.m, PIF_RAM + 8, 4), 0u);
    CHECK_EQ(rl_n64_read(t.m, PIF_RAM + 6, 2), 0x3344u);
    CHECK_EQ(rl_n64_read(t.m, 0x1FC00000u, 4), 0u);
    rl_n64_write(t.m, PIF_RAM + 8, 4, 0xAABBCCDDu);
    advance(&t, 2150u * 3u / 2u - 1u);
    CHECK_EQ(rl_n64_irq_pending(t.m), 0);
    advance(&t, 1);
    CHECK_EQ(rl_n64_irq_pending(t.m), RL_N64_IRQ_SI);
    CHECK_EQ(rl_n64_read(t.m, PIF_RAM + 8, 4), 0xAABBCCDDu);
    mach_free(&t);
}

TEST(n64_the_cpu_can_write_a_command_block_word_by_word)
{
    mach t;
    mach_new(&t);
    rl_n64_set_pad(t.m, 0, RL_N64_PAD_UP, 0, 0);
    uint8_t out[64];
    rl_n64_write(t.m, PIF_RAM + 0, 4, 0x01040100u);
    advance(&t, 4000);
    rl_n64_write(t.m, PIF_RAM + 4, 4, 0xFFFFFFFEu);
    advance(&t, 4000);
    rl_n64_write(t.m, PIF_RAM + 60, 4, 0x00000001u);
    advance(&t, 4000);
    rl_n64_write(t.m, SI_STATUS, 4, 0);
    read_block(&t, out);
    CHECK_EQ(out[3], 0x08);
    mach_free(&t);
}

TEST(n64_controller_pak_commands_answer_as_an_empty_slot)
{
    mach t;
    mach_new(&t);
    rl_n64_set_pad(t.m, 0, 0, 0, 0);
    uint8_t b[64], out[64];
    block_new(b);
    b[0] = 0x03;
    b[1] = 0x21;
    b[2] = 0x02;
    b[3] = 0x80;
    b[4] = 0x01;
    b[38] = 0xFE;
    b[63] = 1;
    send_block(&t, b);
    read_block(&t, out);
    for (unsigned i = 5; i < 37; i++)
        CHECK_EQ(out[i], 0x00);
    CHECK_EQ(out[37], 0xFF);

    block_new(b);
    b[0] = 0x23;
    b[1] = 0x01;
    b[2] = 0x03;
    b[3] = 0x80;
    b[4] = 0x01;
    for (unsigned i = 0; i < 32; i++)
        b[5 + i] = (uint8_t)i;
    b[38] = 0xFE;
    b[63] = 1;
    send_block(&t, b);
    read_block(&t, out);
    CHECK_EQ(out[37], 0x33 ^ 0xFF);
    mach_free(&t);
}

TEST(n64_the_pad_cancels_opposite_directions_and_reads_the_reset_combination)
{
    mach t;
    mach_new(&t);
    rl_n64_set_pad(t.m, 0, RL_N64_PAD_UP | RL_N64_PAD_DOWN | RL_N64_PAD_LEFT, 5, 6);
    uint8_t b[64], out[64];
    block_new(b);
    b[0] = 0x01;
    b[1] = 0x04;
    b[2] = 0x01;
    b[7] = 0xFE;
    b[63] = 1;
    send_block(&t, b);
    read_block(&t, out);
    CHECK_EQ(out[3], 0x02);
    CHECK_EQ(out[4], 0x00);
    CHECK_EQ(out[5], 5);
    CHECK_EQ(out[6], 6);
    rl_n64_set_pad(t.m, 0, RL_N64_PAD_L | RL_N64_PAD_R | RL_N64_PAD_START | RL_N64_PAD_A, 40, 40);
    read_block(&t, out);
    CHECK_EQ(out[3], 0x80);
    CHECK_EQ(out[4], 0x80 | 0x20 | 0x10);
    CHECK_EQ(out[5], 0);
    CHECK_EQ(out[6], 0);
    mach_free(&t);
}

TEST(n64_the_first_controller_follows_the_port_table_when_asked)
{
    mach t;
    mach_new(&t);
    rl_ports_reset();
    rl_ports_script_reset();
    CHECK(rl_ports_declare(rl_n64_ports, rl_n64_ports_count));
    rl_ports_bind_defaults();
    CHECK(rl_ports_set_script("0+100:p1.a,0+100:p1.c_up,0+100:p1.stick_x=1,0+100:p1.stick_y=-0.5"));
    rl_ports_script_frame(1);
    rl_n64_pad_use_ports(t.m, true);
    uint8_t b[64], out[64];
    block_new(b);
    b[0] = 0x01;
    b[1] = 0x04;
    b[2] = 0x01;
    b[7] = 0xFE;
    b[63] = 1;
    send_block(&t, b);
    read_block(&t, out);
    CHECK_EQ(out[3], 0x80);
    CHECK_EQ(out[4], 0x08);
    CHECK_EQ(out[5], 85);
    CHECK_EQ(out[6], 0xD9);
    rl_n64_pad_use_ports(t.m, false);
    read_block(&t, out);
    CHECK_EQ(out[1], 0x84);
    rl_ports_script_reset();
    rl_ports_reset();
    mach_free(&t);
}
