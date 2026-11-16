/**
 * @file test_spi.c
 * @brief The ARM7's SPI bus and its devices, the generated firmware and the
 * real-time clock, with the values the oracle returned to a test program on
 * each of them (register readbacks, byte times, power management registers,
 * touch conversions, the RTC's status registers and date arithmetic).
 */
#include "ndst.h"

#include "nds_int.h"

#include <string.h>

#define SPICNT 0x040001C0u
#define SPIDATA 0x040001C2u
#define RTC 0x04000138u

/** @brief One byte to device dev at baud 0 (the byte time passes); returns the byte received. */
static uint32_t spi(ndst *t, unsigned dev, bool hold, uint8_t v)
{
    w7(t, SPICNT, 2, (uint16_t)(0x8000 | dev << 8 | (hold ? 0x800 : 0)));
    w7(t, SPIDATA, 2, v);
    ndst_to(t, RL_NDS_ARM7, rl_sched_now(t->k[RL_NDS_ARM7]) + 2 * 70);
    return r7(t, SPIDATA, 2);
}

static ndst *fixture(void)
{
    ndst *t = ndst_new();
    ndst_to(t, RL_NDS_ARM9, 1ull << 40);
    return t;
}

TEST(spicnt_keeps_bits_cf03_and_reports_busy_for_the_byte_time_of_each_baud)
{
    ndst *t = fixture();
    w7(t, SPICNT, 2, 0xFFFF);
    CHECK_EQ(r7(t, SPICNT, 2), 0xCF03);
    static const uint32_t bus[4] = {67, 134, 268, 536};
    for (unsigned baud = 0; baud < 4; baud++) {
        w7(t, SPICNT, 2, (uint16_t)(0x8000 | baud));
        uint64_t t0 = rl_sched_now(t->k[RL_NDS_ARM7]);
        w7(t, SPIDATA, 2, 0x80);
        CHECK(r7(t, SPICNT, 2) & 0x80);
        ndst_to(t, RL_NDS_ARM7, t0 + 2 * bus[baud] - 2);
        CHECK(r7(t, SPICNT, 2) & 0x80);
        ndst_to(t, RL_NDS_ARM7, t0 + 2 * bus[baud]);
        CHECK(!(r7(t, SPICNT, 2) & 0x80));
        ndst_to(t, RL_NDS_ARM7, rl_sched_now(t->k[RL_NDS_ARM7]) + 4);
    }
    w7(t, SPICNT, 2, 0);
    CHECK_EQ(r9(t, SPICNT, 2), 0);
}

TEST(the_transfer_complete_irq_needs_bit_14)
{
    ndst *t = fixture();
    w7(t, SPICNT, 2, 0x8000);
    w7(t, SPIDATA, 2, 0);
    ndst_to(t, RL_NDS_ARM7, rl_sched_now(t->k[RL_NDS_ARM7]) + 400);
    CHECK_EQ(r7(t, 0x04000214u, 4), 0);
    w7(t, SPICNT, 2, 0xC000);
    w7(t, SPIDATA, 2, 0);
    ndst_to(t, RL_NDS_ARM7, rl_sched_now(t->k[RL_NDS_ARM7]) + 400);
    CHECK_EQ(r7(t, 0x04000214u, 4), RL_NDS_IRQ_SPI);
}

TEST(power_management_registers_start_and_mask_as_the_oracle_shows)
{
    ndst *t = fixture();
    static const uint8_t init[8] = {0, 0, 0, 0, 0x40, 0, 0, 0};
    for (unsigned i = 0; i < 8; i++) {
        spi(t, 0, true, (uint8_t)(0x80 | i));
        CHECK_EQ(spi(t, 0, false, 0), init[i]);
    }
    static const struct {
        uint8_t reg, write, read;
    } w[] = {{0, 0x0D, 0x0D}, {2, 0xFF, 0x01}, {3, 0xFF, 0x03}, {4, 0xFF, 0x4F}};
    for (unsigned i = 0; i < 4; i++) {
        spi(t, 0, true, w[i].reg);
        spi(t, 0, false, w[i].write);
        spi(t, 0, true, (uint8_t)(0x80 | w[i].reg));
        CHECK_EQ(spi(t, 0, false, 0), w[i].read);
    }
}

TEST(firmware_flash_answers_the_jedec_id_status_and_reads_the_generated_header)
{
    ndst *t = fixture();
    spi(t, 1, true, 0x9F);
    CHECK_EQ(spi(t, 1, true, 0), 0x20);
    CHECK_EQ(spi(t, 1, true, 0), 0x40);
    CHECK_EQ(spi(t, 1, false, 0), 0x12);
    spi(t, 1, true, 0x05);
    CHECK_EQ(spi(t, 1, false, 0), 0);
    spi(t, 1, false, 0x06);
    spi(t, 1, true, 0x05);
    CHECK_EQ(spi(t, 1, false, 0), 2);
    uint8_t hdr[0x2C];
    spi(t, 1, true, 0x03);
    spi(t, 1, true, 0);
    spi(t, 1, true, 0);
    spi(t, 1, true, 0);
    for (unsigned i = 0; i < sizeof hdr; i++)
        hdr[i] = (uint8_t)spi(t, 1, i + 1 < sizeof hdr, 0);
    CHECK_EQ(hdr[0x1D], 0xFF);
    CHECK_EQ(hdr[0x20] | hdr[0x21] << 8, 0x3FE00 / 8);
    const uint8_t *fw = t->m->spi.fw;
    CHECK_EQ(nds_crc16(0, fw + 0x2C, fw[0x2C] | fw[0x2D] << 8), fw[0x2A] | fw[0x2B] << 8);
    CHECK_EQ(memcmp(hdr, fw, sizeof hdr), 0);
}

TEST(both_user_settings_areas_have_valid_crcs_and_the_second_is_the_newer)
{
    ndst *t = fixture();
    const uint8_t *fw = t->m->spi.fw;
    for (unsigned a = 0; a < 2; a++) {
        const uint8_t *u = fw + 0x3FE00 + 0x100 * a;
        CHECK_EQ(u[0] | u[1] << 8, 5);
        CHECK_EQ(nds_crc16(0xFFFF, u, 0x70), u[0x72] | u[0x73] << 8);
        CHECK_EQ(u[0x70] | u[0x71] << 8, a);
        CHECK_EQ(u[0x64] | u[0x65] << 8, 0xEC01);
    }
    CHECK_EQ(fw[0x3FF70], (fw[0x3FE70] + 1) & 0x7F);
    uint8_t user[0x70];
    rl_nds_user_settings(t->m, user);
    CHECK_EQ(memcmp(user, fw + 0x3FF00, 0x70), 0);
}

TEST(firmware_writes_need_write_enable_and_erase_by_page_and_sector)
{
    ndst *t = fixture();
    uint8_t *fw = t->m->spi.fw;
    spi(t, 1, true, 0x0A);
    spi(t, 1, true, 0x01);
    spi(t, 1, true, 0x00);
    spi(t, 1, true, 0x10);
    spi(t, 1, false, 0x33);
    CHECK_EQ(fw[0x10010], 0xFF);
    spi(t, 1, false, 0x06);
    spi(t, 1, true, 0x0A);
    spi(t, 1, true, 0x01);
    spi(t, 1, true, 0x00);
    spi(t, 1, true, 0x10);
    spi(t, 1, false, 0x33);
    CHECK_EQ(fw[0x10010], 0x33);
    spi(t, 1, false, 0x06);
    spi(t, 1, true, 0xDB);
    spi(t, 1, true, 0x01);
    spi(t, 1, true, 0x00);
    spi(t, 1, false, 0x10);
    CHECK_EQ(fw[0x10010], 0xFF);
}

/** @brief One 12-bit conversion of channel ch the way the oracle's program did (control byte, two reads). */
static uint32_t convert(ndst *t, unsigned ch)
{
    spi(t, 2, true, (uint8_t)(0x80 | ch << 4));
    uint32_t h = spi(t, 2, true, 0), l = spi(t, 2, false, 0);
    return (h << 8 | l) >> 3;
}

TEST(the_touch_controller_converts_the_pen_as_sixteen_times_the_pixel)
{
    ndst *t = fixture();
    CHECK_EQ(r7(t, 0x04000136u, 2), 0x7F);
    CHECK_EQ(convert(t, 5), 0);
    CHECK_EQ(convert(t, 1), 0xFFF);
    CHECK_EQ(convert(t, 6), 0x800);
    static const struct {
        unsigned x, y, adc_x, adc_y;
    } p[] = {{100, 80, 0x640, 0x500}, {255, 191, 0xFF0, 0xBF0}, {10, 10, 0xA0, 0xA0}, {0, 0, 0, 0}};
    for (unsigned i = 0; i < 4; i++) {
        rl_nds_input in = {.touch = true, .touch_x = (uint8_t)p[i].x, .touch_y = (uint8_t)p[i].y};
        rl_nds_set_input(t->m, &in);
        CHECK_EQ(r7(t, 0x04000136u, 2), 0x3F);
        CHECK_EQ(convert(t, 5), p[i].adc_x);
        CHECK_EQ(convert(t, 1), p[i].adc_y);
    }
    rl_nds_input up = {0};
    rl_nds_set_input(t->m, &up);
    CHECK_EQ(convert(t, 5), 0);
}

TEST(the_calibration_in_the_user_settings_inverts_the_converter)
{
    ndst *t = fixture();
    const uint8_t *u = t->m->spi.fw + 0x3FF00;
    unsigned ax1 = u[0x58] | u[0x59] << 8, ay1 = u[0x5A] | u[0x5B] << 8, sx1 = u[0x5C], sy1 = u[0x5D];
    unsigned ax2 = u[0x5E] | u[0x5F] << 8, ay2 = u[0x60] | u[0x61] << 8, sx2 = u[0x62], sy2 = u[0x63];
    for (unsigned px = 0; px < 256; px += 17)
        for (unsigned py = 0; py < 192; py += 13) {
            unsigned ax = px * 16, ay = py * 16;
            unsigned sx = (ax - ax1) * (sx2 - sx1) / (ax2 - ax1) + (sx1 - 1);
            unsigned sy = (ay - ay1) * (sy2 - sy1) / (ay2 - ay1) + (sy1 - 1);
            CHECK_EQ(sx, px);
            CHECK_EQ(sy, py);
        }
}

TEST(an_8_bit_touch_conversion_returns_the_top_eight_bits)
{
    ndst *t = fixture();
    rl_nds_input in = {.touch = true, .touch_x = 100, .touch_y = 80};
    rl_nds_set_input(t->m, &in);
    spi(t, 2, true, 0x80 | 5 << 4 | 0x08);
    uint32_t h = spi(t, 2, true, 0), l = spi(t, 2, false, 0);
    CHECK_EQ((h << 8 | l) >> 7, 0x64);
}

/* RTC: the program drives the three lines; commands go most significant bit first, parameters least significant first. */

static void rtc_put(ndst *t, uint16_t v)
{
    w7(t, RTC, 2, v);
    ndst_to(t, RL_NDS_ARM7, rl_sched_now(t->k[RL_NDS_ARM7]) + 4);
}

static void rtc_bit_out(ndst *t, unsigned bit)
{
    rtc_put(t, (uint16_t)(0x74 | bit));
    rtc_put(t, (uint16_t)(0x76 | bit));
}

static void rtc_begin(ndst *t, unsigned cmd, bool read)
{
    rtc_put(t, 0x72);
    rtc_put(t, 0x76);
    unsigned byte = 0x60 | cmd << 1 | (read ? 1 : 0);
    for (int i = 7; i >= 0; i--)
        rtc_bit_out(t, (byte >> i) & 1);
}

static uint32_t rtc_in(ndst *t)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < 8; i++) {
        rtc_put(t, 0x64);
        rtc_put(t, 0x66);
        v |= (r7(t, RTC, 2) & 1) << i;
    }
    return v;
}

static void rtc_end(ndst *t) { rtc_put(t, 0x72); }

static uint32_t rtc_read(ndst *t, unsigned cmd, unsigned n, uint8_t *out)
{
    rtc_begin(t, cmd, true);
    rtc_put(t, 0x66);
    for (unsigned i = 0; i < n; i++)
        out[i] = (uint8_t)rtc_in(t);
    rtc_end(t);
    return out[0];
}

static void rtc_write(ndst *t, unsigned cmd, const uint8_t *in, unsigned n)
{
    rtc_begin(t, cmd, false);
    for (unsigned i = 0; i < n; i++)
        for (unsigned b = 0; b < 8; b++)
            rtc_bit_out(t, (in[i] >> b) & 1);
    rtc_end(t);
}

TEST(the_rtc_register_reads_back_all_16_bits_and_shows_the_idle_data_line)
{
    ndst *t = fixture();
    w7(t, RTC, 2, 0xFFFF);
    CHECK_EQ(r7(t, RTC, 2), 0xFFFF);
    w7(t, RTC, 2, 0xFF8F);
    CHECK_EQ(r7(t, RTC, 2), 0xFF8F);
    w7(t, RTC, 2, 0x0000);
    CHECK_EQ(r7(t, RTC, 2), 1);
    w7(t, RTC, 2, 0x70);
    CHECK_EQ(r7(t, RTC, 2), 0x70);
    w7(t, RTC, 2, 0x07);
    CHECK_EQ(r7(t, RTC, 2), 7);
    CHECK_EQ(r9(t, RTC, 2), 0);
}

TEST(rtc_status_registers_follow_the_oracle_and_the_flags_clear_on_read)
{
    ndst *t = fixture();
    uint8_t v[7];
    CHECK_EQ(rtc_read(t, 0, 1, v), 0x82);
    CHECK_EQ(rtc_read(t, 0, 1, v), 0x02);
    CHECK_EQ(rtc_read(t, 1, 1, v), 0);
    uint8_t w = 0x55;
    rtc_write(t, 1, &w, 1);
    CHECK_EQ(rtc_read(t, 1, 1, v), 0x55);
    w = 0x02;
    rtc_write(t, 0, &w, 1);
    CHECK_EQ(rtc_read(t, 0, 1, v), 0x02);
}

TEST(rtc_date_time_start_at_2000_write_and_run_with_the_emulated_clock)
{
    ndst *t = fixture();
    uint8_t v[7];
    rtc_read(t, 2, 7, v);
    static const uint8_t start[7] = {0, 1, 1, 0, 0, 0, 0};
    CHECK_EQ(memcmp(v, start, 7), 0);
    static const uint8_t set[7] = {0x24, 0x12, 0x31, 0x04, 0x23, 0x59, 0x58};
    rtc_write(t, 2, set, 7);
    rtc_read(t, 2, 7, v);
    static const uint8_t pm[7] = {0x24, 0x12, 0x31, 0x04, 0x63, 0x59, 0x58};
    CHECK_EQ(memcmp(v, pm, 7), 0);
    uint64_t at = rl_sched_now(t->k[RL_NDS_ARM7]);
    ndst_to(t, RL_NDS_ARM7, at + 2 * (uint64_t)RL_NDS_TICK_HZ);
    rtc_read(t, 2, 7, v);
    CHECK_EQ(v[0], 0x25);
    CHECK_EQ(v[1], 0x01);
    CHECK_EQ(v[2], 0x01);
    CHECK_EQ(v[3], 0x05);
    CHECK_EQ(v[4], 0x00);
    CHECK_EQ(v[5], 0x00);
    CHECK(v[6] <= 0x01);
    rtc_read(t, 3, 3, v);
    CHECK_EQ(v[0], 0x00);
}

TEST(rtc_hours_follow_the_12_and_24_hour_modes_and_leap_years)
{
    ndst *t = fixture();
    rl_nds_time when = {2024, 2, 28, 13, 5, 9};
    rl_nds_set_time(t->m, &when);
    uint8_t v[7];
    rtc_read(t, 2, 7, v);
    CHECK_EQ(v[0], 0x24);
    CHECK_EQ(v[1], 0x02);
    CHECK_EQ(v[2], 0x28);
    CHECK_EQ(v[3], 2);
    CHECK_EQ(v[4], 0x53);
    CHECK_EQ(v[5], 0x05);
    uint8_t st = 0;
    rtc_write(t, 0, &st, 1);
    rtc_read(t, 2, 7, v);
    CHECK_EQ(v[4], 0x41 + 0);
    ndst_to(t, RL_NDS_ARM7, rl_sched_now(t->k[RL_NDS_ARM7]) + 2 * 86400ull * RL_NDS_TICK_HZ);
    st = 2;
    rtc_write(t, 0, &st, 1);
    rtc_read(t, 2, 7, v);
    CHECK_EQ(v[1], 0x03);
    CHECK_EQ(v[2], 0x01);
}

TEST(the_free_register_the_adjustment_and_the_alarms_store_what_is_written)
{
    ndst *t = fixture();
    uint8_t v[7], w[3] = {0x81, 0x92, 0xA3};
    rtc_write(t, 5, w, 3);
    rtc_read(t, 5, 3, v);
    CHECK_EQ(memcmp(v, w, 3), 0);
    w[0] = 0x77;
    rtc_write(t, 7, w, 1);
    rtc_read(t, 7, 1, v);
    CHECK_EQ(v[0], 0x77);
    w[0] = 0x04;
    rtc_write(t, 1, w, 1);
    uint8_t a[3] = {0x01, 0x02, 0x03};
    rtc_write(t, 4, a, 3);
    rtc_read(t, 4, 3, v);
    CHECK_EQ(memcmp(v, a, 3), 0);
}

TEST(the_boot_state_leaves_the_chip_id_the_header_crcs_and_the_user_settings_in_main_ram)
{
    ndst *t = fixture();
    static uint8_t img[0x200];
    img[0x6C] = 0x34;
    img[0x6D] = 0x12;
    img[0x15E] = 0xE2;
    img[0x15F] = 0x43;
    CHECK(rl_nds_set_cartridge(t->m, img, (size_t)128 << 20) == 0);
    rl_nds_boot_state(t->m);
    CHECK_EQ(r9(t, 0x027FF800u, 4), 0x7FC2);
    CHECK_EQ(r9(t, 0x027FF804u, 4), 0x7FC2);
    CHECK_EQ(r9(t, 0x027FFC00u, 4), 0x7FC2);
    CHECK_EQ(r9(t, 0x027FFC04u, 4), 0x7FC2);
    CHECK_EQ(r9(t, 0x027FF808u, 2), 0x43E2);
    CHECK_EQ(r9(t, 0x027FFC08u, 2), 0x43E2);
    CHECK_EQ(r9(t, 0x027FFC0Au, 2), 0x1234);
    CHECK_EQ(r9(t, 0x027FFC10u, 2), 0x5835);
    uint8_t user[0x70];
    rl_nds_user_settings(t->m, user);
    for (unsigned i = 0; i < 0x70; i += 4)
        CHECK_EQ(r9(t, 0x027FFC80u + i, 4), (uint32_t)user[i] | (uint32_t)user[i + 1] << 8 | (uint32_t)user[i + 2] << 16 | (uint32_t)user[i + 3] << 24);
    CHECK_EQ(r9(t, 0x04000204u, 2), 0xE880);
    CHECK_EQ(r7(t, 0x04000204u, 2), 0xE880);
}
