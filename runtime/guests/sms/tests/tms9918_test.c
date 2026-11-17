/**
 * @file tms9918_test.c
 * @brief The TMS9918A: the two-byte control port, the frame interrupt at
 * line 192 and its prediction, graphics 1 and text mode pixels, and a
 * sprite over the background.
 */
#include "runner.h"
#include "tms9918.h"

static sms_tms t;
static int frames;

static void on_frame(void *user)
{
    (void)user;
    frames++;
}

static void reg(unsigned r, uint8_t v)
{
    sms_tms_control(&t, v);
    sms_tms_control(&t, (uint8_t)(0x80u | r));
}

static void at(uint16_t a)
{
    sms_tms_control(&t, (uint8_t)a);
    sms_tms_control(&t, (uint8_t)(0x40u | (a >> 8)));
}

static void reset(void)
{
    t.on_frame = on_frame;
    sms_tms_reset(&t);
    frames = 0;
}

TEST(tms_frame_interrupt_rises_when_line_192_starts)
{
    reset();
    reg(1, 0x20);
    uint64_t edge = 192u * SMS_TMS_LINE;
    CHECK_EQ(sms_tms_next_irq(&t), edge);
    sms_tms_run(&t, edge - 1);
    CHECK(!t.irq_level);
    CHECK_EQ(frames, 0);
    sms_tms_run(&t, edge);
    CHECK(t.irq_level);
    CHECK_EQ(frames, 1);
    CHECK_EQ(sms_tms_status(&t) & 0x80, 0x80);
    CHECK(!t.irq_level);
    CHECK_EQ(sms_tms_frame_edge(&t), edge + 262u * SMS_TMS_LINE);
}

TEST(tms_graphics_1_and_text_mode_pixels)
{
    reset();
    reg(1, 0x40);
    reg(2, 0x0E);
    reg(3, 0x80);
    reg(4, 0x00);
    reg(7, 0x41);
    at(0x3800);
    sms_tms_data_write(&t, 0x02);
    at(0x0010);
    sms_tms_data_write(&t, 0xF0);
    sms_tms_data_write(&t, 0xF0);
    at(0x2000);
    sms_tms_data_write(&t, 0xA3);
    sms_tms_run(&t, 20);
    CHECK_EQ(t.pixels[0], 0xA);
    CHECK_EQ(t.pixels[4], 0x3);
    reg(1, 0x50);
    sms_tms_run(&t, SMS_TMS_LINE + 20);
    CHECK_EQ(t.pixels[SMS_TMS_W + 0], 0x4);
    CHECK_EQ(t.pixels[SMS_TMS_W + 5], 0x1);
}

TEST(tms_sprite_covers_the_background)
{
    reset();
    reg(1, 0x40);
    reg(5, 0x10);
    reg(6, 0x00);
    reg(7, 0x07);
    at(0x0800);
    sms_tms_data_write(&t, 0x00);
    sms_tms_data_write(&t, 0x08);
    sms_tms_data_write(&t, 0x01);
    sms_tms_data_write(&t, 0x0F);
    sms_tms_data_write(&t, 0xD0);
    at(0x0008);
    sms_tms_data_write(&t, 0x80);
    sms_tms_run(&t, 2u * SMS_TMS_LINE - 1);
    CHECK_EQ(t.pixels[SMS_TMS_W + 8], 0xF);
    CHECK_EQ(t.pixels[SMS_TMS_W + 9], 0x7);
}
