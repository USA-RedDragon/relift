/**
 * @file machine_test.c
 * @brief The machine: the Sega, Codemasters and Korean mappers, cartridge
 * RAM, the memory control port, the controller ports and the Game Gear
 * start button, the pause NMI, the TH latch of the H counter, and the
 * CPU-to-VDP timeline.
 */
#include "runner.h"
#include "sms.h"

#include <stdlib.h>

static sms m;
static uint8_t rom[0x40000];

static void boot(sms_system system, sms_mapper mapper, size_t len)
{
    for (size_t b = 0; b < len / 0x4000; b++)
        for (size_t o = 0; o < 0x4000; o++)
            rom[b * 0x4000 + o] = (uint8_t)(b ^ (o & 0xF0));
    sms_config cfg = {.system = system, .region = SMS_REGION_NTSC_U, .mapper = mapper};
    sms_free(&m);
    CHECK_EQ(sms_init(&m, &cfg, rom, len), 0);
}

static uint8_t bank_byte(unsigned b) { return (uint8_t)b; }

TEST(machine_sega_mapper_switches_the_three_slots_but_not_the_first_kilobyte)
{
    boot(SMS_SYSTEM_MS, SMS_MAPPER_SEGA, 0x40000);
    CHECK_EQ(sms_read(&m, 0x8000, 0), bank_byte(2));
    sms_write(&m, 0xFFFF, 5, 0);
    CHECK_EQ(sms_read(&m, 0x8000, 0), bank_byte(5));
    CHECK_EQ(sms_slot_bank(&m, 2), 5);
    sms_write(&m, 0xFFFD, 7, 0);
    CHECK_EQ(sms_read(&m, 0x0000, 0), bank_byte(0));
    CHECK_EQ(sms_read(&m, 0x0400, 0), bank_byte(7));
    sms_write(&m, 0xFFFE, 17, 0);
    CHECK_EQ(sms_read(&m, 0x4000, 0), bank_byte(1));
    CHECK_EQ(sms_slot_bank(&m, 1), 1);
    CHECK_EQ(m.ram[0x1FFE], 17);
}

TEST(machine_sega_mapper_maps_cartridge_ram_at_0x8000)
{
    boot(SMS_SYSTEM_MS, SMS_MAPPER_SEGA, 0x40000);
    sms_write(&m, 0xFFFC, 0x08, 0);
    sms_write(&m, 0x8123, 0x5A, 0);
    CHECK_EQ(sms_read(&m, 0x8123, 0), 0x5A);
    CHECK_EQ(sms_slot_bank(&m, 2), -1);
    sms_write(&m, 0xFFFC, 0x0C, 0);
    CHECK_EQ(sms_read(&m, 0x8123, 0), 0);
    sms_write(&m, 0xFFFC, 0x00, 0);
    CHECK_EQ(sms_read(&m, 0x8123, 0), (uint8_t)(2 ^ 0x20));
}

TEST(machine_codemasters_mapper_banks_on_writes_to_each_slot)
{
    boot(SMS_SYSTEM_MS, SMS_MAPPER_CODEMASTERS, 0x40000);
    CHECK_EQ(sms_read(&m, 0x8000, 0), bank_byte(0));
    sms_write(&m, 0x8000, 9, 0);
    CHECK_EQ(sms_read(&m, 0x8000, 0), bank_byte(9));
    sms_write(&m, 0x4000, 3, 0);
    CHECK_EQ(sms_read(&m, 0x4000, 0), bank_byte(3));
    sms_write(&m, 0x0000, 4, 0);
    CHECK_EQ(sms_read(&m, 0x0000, 0), bank_byte(4));
    sms_write(&m, 0x4000, 0x83, 0);
    sms_write(&m, 0xA010, 0x77, 0);
    CHECK_EQ(sms_read(&m, 0xA010, 0), 0x77);
    CHECK_EQ(sms_read(&m, 0x8000, 0), bank_byte(9));
}

TEST(machine_korean_mapper_banks_slot_2_from_0xa000)
{
    boot(SMS_SYSTEM_MS, SMS_MAPPER_KOREA, 0x40000);
    sms_write(&m, 0xA000, 6, 0);
    CHECK_EQ(sms_read(&m, 0x8000, 0), bank_byte(6));
    CHECK_EQ(sms_read(&m, 0x4000, 0), bank_byte(1));
}

TEST(machine_work_ram_mirrors_and_the_memory_control_port_disables_it)
{
    boot(SMS_SYSTEM_MS, SMS_MAPPER_SEGA, 0x8000);
    sms_write(&m, 0xC010, 0x11, 0);
    CHECK_EQ(sms_read(&m, 0xE010, 0), 0x11);
    sms_out(&m, 0x3E, 0x10, 0);
    CHECK_EQ(sms_read(&m, 0xC010, 0), 0xFF);
    sms_out(&m, 0x3E, 0x00, 0);
    CHECK_EQ(sms_read(&m, 0xC010, 0), 0x11);
}

TEST(machine_controller_port_reads_the_buttons_active_low)
{
    boot(SMS_SYSTEM_MS, SMS_MAPPER_SEGA, 0x8000);
    m.buttons = SMS_BUTTON_UP | SMS_BUTTON_1;
    CHECK_EQ(sms_in(&m, 0xDC, 0), 0xEE);
    m.buttons = SMS_BUTTON_UP | SMS_BUTTON_DOWN;
    CHECK_EQ(sms_in(&m, 0xDC, 0) & 3, 1);
    CHECK_EQ(sms_in(&m, 0xDD, 0), 0xFF);
}

TEST(machine_game_gear_start_reads_through_port_0)
{
    boot(SMS_SYSTEM_GG, SMS_MAPPER_SEGA, 0x8000);
    CHECK_EQ(sms_in(&m, 0x00, 0), 0xC0);
    m.buttons = SMS_BUTTON_START;
    CHECK_EQ(sms_in(&m, 0x00, 0), 0x40);
    m.buttons = SMS_BUTTON_2;
    CHECK_EQ(sms_in(&m, 0xDC, 0), 0xDF);
}

TEST(machine_pause_raises_the_nmi_at_the_frame_edge_it_is_first_held)
{
    boot(SMS_SYSTEM_MS, SMS_MAPPER_SEGA, 0x8000);
    static const uint8_t inputs[3] = {0, SMS_BUTTON_START, SMS_BUTTON_START};
    m.inputs = inputs;
    m.input_count = 3;
    uint64_t frame = sms_cycle_of(&m, 240u * SMS_VDP_LINE);
    sms_catch_up(&m, frame);
    CHECK_EQ(m.frames, 1);
    CHECK(!m.nmi);
    CHECK_EQ(m.buttons, SMS_BUTTON_START);
    uint64_t next = sms_cycle_of(&m, (240u + 262u) * SMS_VDP_LINE);
    sms_catch_up(&m, next);
    CHECK(m.nmi);
    m.nmi = false;
    sms_catch_up(&m, sms_cycle_of(&m, (240u + 2 * 262u) * SMS_VDP_LINE));
    CHECK(!m.nmi);
}

TEST(machine_th_rising_latches_the_h_counter)
{
    boot(SMS_SYSTEM_MS, SMS_MAPPER_SEGA, 0x8000);
    sms_out(&m, 0x3F, 0x55, 0);
    sms_out(&m, 0x3F, 0xF5, 100);
    CHECK_EQ(m.vdp.latch_hcounter, (uint32_t)(sms_vdp_count(&m, 100) + 1) % SMS_VDP_LINE);
}

TEST(machine_vdp_counts_and_cpu_cycles_round_trip)
{
    boot(SMS_SYSTEM_MS, SMS_MAPPER_SEGA, 0x8000);
    for (uint64_t p = 0; p < 5000000; p += 997) {
        uint64_t t = sms_cycle_of(&m, p);
        CHECK(sms_vdp_count(&m, t) >= p);
        CHECK(t == 0 || sms_vdp_count(&m, t - 1) < p);
    }
    uint64_t second = sms_vdp_count(&m, 3579545);
    CHECK(second >= 10738634 && second <= 10738638);
}

TEST(machine_fm_unit_is_on_japanese_master_systems_only)
{
    boot(SMS_SYSTEM_MS, SMS_MAPPER_SEGA, 0x8000);
    CHECK(!m.fm);
    sms_config cfg = {.system = SMS_SYSTEM_MS, .region = SMS_REGION_NTSC_J, .mapper = SMS_MAPPER_SEGA};
    sms_free(&m);
    CHECK_EQ(sms_init(&m, &cfg, rom, 0x8000), 0);
    CHECK(m.fm);
    CHECK_EQ(sms_in(&m, 0xF2, 0) & 3, 0);
    sms_out(&m, 0xF2, 0x03, 0);
    CHECK_EQ(sms_in(&m, 0xF2, 0) & 0x1F, 3);
    sms_out(&m, 0xF0, 0x30, 10);
    sms_out(&m, 0xF1, 0x15, 20);
    CHECK_EQ(m.fm_chip.s.voice[0].tone, 1);
    CHECK_EQ(m.fm_chip.s.voice[0].level, 5);
}
