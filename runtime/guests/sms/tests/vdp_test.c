/**
 * @file vdp_test.c
 * @brief The VDP: the control and data ports, VRAM and CRAM writes, the
 * counters, the frame and line interrupts and their predicted positions,
 * the frame edge and a mode 4 picture.
 */
#include "runner.h"
#include "vdp.h"

static sms_vdp v;
static int frames;
static int irq_changes;

static void on_frame(void *user)
{
    (void)user;
    frames++;
}

static void on_irq(void *user, bool level)
{
    (void)user;
    (void)level;
    irq_changes++;
}

static void reset(sms_vdp_mode mode)
{
    v.on_frame = on_frame;
    v.on_irq = on_irq;
    v.user = NULL;
    sms_vdp_reset(&v, mode, 2, false);
    frames = 0;
    irq_changes = 0;
}

static void reg(unsigned r, uint8_t value)
{
    sms_vdp_control(&v, value);
    sms_vdp_control(&v, (uint8_t)(0x80u | r));
}

static void vram_at(uint16_t a)
{
    sms_vdp_control(&v, (uint8_t)a);
    sms_vdp_control(&v, (uint8_t)(0x40u | (a >> 8)));
}

static uint64_t line_pos(uint32_t line) { return (uint64_t)line * SMS_VDP_LINE; }

TEST(vdp_registers_and_vram_go_through_the_two_byte_control_port)
{
    reset(SMS_VDP_MS);
    reg(1, 0x60);
    CHECK(v.display);
    CHECK(v.frame_enable);
    reg(7, 0x13);
    CHECK_EQ(v.backdrop, 3);
    vram_at(0x1234);
    sms_vdp_data_write(&v, 0xAB);
    sms_vdp_data_write(&v, 0xCD);
    CHECK_EQ(v.vram[0x1234], 0xAB);
    CHECK_EQ(v.vram[0x1235], 0xCD);
    sms_vdp_control(&v, 0x34);
    sms_vdp_control(&v, 0x12);
    CHECK_EQ(sms_vdp_data_read(&v), 0xAB);
    CHECK_EQ(sms_vdp_data_read(&v), 0xCD);
}

TEST(vdp_cram_is_six_bits_on_a_master_system_and_latched_pairs_on_a_game_gear)
{
    reset(SMS_VDP_MS);
    sms_vdp_control(&v, 0x05);
    sms_vdp_control(&v, 0xC0);
    sms_vdp_data_write(&v, 0xFF);
    CHECK_EQ(v.cram[5], 0x3F);
    reset(SMS_VDP_GG);
    sms_vdp_control(&v, 0x0A);
    sms_vdp_control(&v, 0xC0);
    sms_vdp_data_write(&v, 0x34);
    CHECK_EQ(v.cram[5], 0);
    sms_vdp_data_write(&v, 0xF2);
    CHECK_EQ(v.cram[5], 0x234);
}

TEST(vdp_frame_interrupt_rises_at_the_line_after_the_picture_and_status_clears_it)
{
    reset(SMS_VDP_MS);
    reg(1, 0x20);
    uint64_t at = sms_vdp_next_irq(&v);
    CHECK_EQ(at, line_pos(193));
    sms_vdp_run(&v, at - 1);
    CHECK(!v.irq_level);
    sms_vdp_run(&v, at);
    CHECK(v.irq_level);
    CHECK_EQ(sms_vdp_next_irq(&v), UINT64_MAX);
    CHECK_EQ(sms_vdp_status(&v) & 0x80, 0x80);
    CHECK(!v.irq_level);
    CHECK_EQ(irq_changes, 2);
}

TEST(vdp_line_interrupt_counts_down_from_register_10)
{
    reset(SMS_VDP_MS);
    reg(10, 2);
    sms_vdp_run(&v, 0);
    CHECK(v.line_pending);
    sms_vdp_status(&v);
    reg(0, 0x10);
    CHECK(!v.irq_level);
    uint64_t at = sms_vdp_next_irq(&v);
    CHECK_EQ(at, line_pos(3));
    sms_vdp_run(&v, at);
    CHECK(v.irq_level);
    sms_vdp_status(&v);
    CHECK_EQ(sms_vdp_next_irq(&v), line_pos(6));
}

TEST(vdp_v_counter_skips_back_after_line_218)
{
    reset(SMS_VDP_MS);
    sms_vdp_run(&v, line_pos(218));
    CHECK_EQ(sms_vdp_vcounter(&v), 218);
    sms_vdp_run(&v, line_pos(219));
    CHECK_EQ(sms_vdp_vcounter(&v), 213);
    sms_vdp_run(&v, line_pos(261));
    CHECK_EQ(sms_vdp_vcounter(&v), 255);
}

TEST(vdp_h_counter_reads_the_latched_count)
{
    reset(SMS_VDP_MS);
    sms_vdp_run(&v, 99);
    sms_vdp_latch_h(&v);
    CHECK_EQ(sms_vdp_hcounter(&v), 25);
    sms_vdp_run(&v, 599);
    sms_vdp_latch_h(&v);
    CHECK_EQ(sms_vdp_hcounter(&v), (600 + 340) >> 2);
}

TEST(vdp_frame_edge_is_the_start_of_line_240)
{
    reset(SMS_VDP_MS);
    CHECK_EQ(sms_vdp_frame_edge(&v), line_pos(240));
    sms_vdp_run(&v, line_pos(240) - 1);
    CHECK_EQ(frames, 0);
    sms_vdp_run(&v, line_pos(240));
    CHECK_EQ(frames, 1);
    CHECK_EQ(sms_vdp_frame_edge(&v), line_pos(240 + 262));
}

TEST(vdp_mode_4_draws_a_tile_with_its_palette)
{
    reset(SMS_VDP_MS);
    reg(0, 0x06);
    reg(1, 0x40);
    reg(2, 0xFF);
    vram_at(0x3800);
    sms_vdp_data_write(&v, 0x01);
    sms_vdp_data_write(&v, 0x00);
    vram_at(0x0020);
    for (int row = 0; row < 8; row++) {
        sms_vdp_data_write(&v, 0x80);
        sms_vdp_data_write(&v, 0x80);
        sms_vdp_data_write(&v, 0x00);
        sms_vdp_data_write(&v, 0x00);
    }
    sms_vdp_control(&v, 0x03);
    sms_vdp_control(&v, 0xC0);
    sms_vdp_data_write(&v, 0x2A);
    sms_vdp_run(&v, line_pos(1));
    CHECK_EQ(v.pixels[0], 0x2A);
    CHECK_EQ(v.pixels[1], 0);
}

TEST(vdp_lines_below_the_picture_hold_nothing_from_earlier_frames)
{
    reset(SMS_VDP_MS);
    reg(0, 0x06);
    reg(1, 0x40);
    v.pixels[200 * SMS_VDP_W + 3] = 0x15;
    v.pixels[230 * SMS_VDP_W + 3] = 0x15;
    sms_vdp_run(&v, line_pos(201));
    CHECK_EQ(v.pixels[200 * SMS_VDP_W + 3], 0);
    CHECK_EQ(v.pixels[230 * SMS_VDP_W + 3], 0x15);
}
