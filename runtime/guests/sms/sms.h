/**
 * @file sms.h
 * @brief The Master System and Game Gear machine: the memory map and the
 * cartridge mappers, the I/O ports and controllers, and the time that ties
 * the CPU's T-states to the VDP's clock.
 *
 * The machine counts CPU T-states from power on. The VDP runs at three
 * times the CPU's rate, and its work is placed on that timeline the way
 * ares places it: each chip has an integer frequency (the colour burst plus
 * one half, truncated) and a clock step of (2^63 - 1) / frequency, so the
 * VDP work of count p is visible to a CPU access at T-state t when
 * p * vdp_step <= t * cpu_step. The machine brings the VDP up to the CPU at
 * every port access and when the CPU reaches the next interrupt or frame
 * edge (sms_next_event).
 *
 * Inputs change at frame edges: the buttons held during frame n (counted
 * from 0 at power on) are the n-th entry of the input script, or, without
 * one, what the input function gives at the edge before frame n.
 */
#ifndef RELIFT_SMS_MACHINE_H
#define RELIFT_SMS_MACHINE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "psg.h"
#include "tms9918.h"
#include "vdp.h"
#include "ym2413.h"

/** @brief The console. */
typedef enum sms_system {
    /** @brief A Master System II. */
    SMS_SYSTEM_MS = 0,
    /** @brief A Game Gear running a Game Gear cartridge. */
    SMS_SYSTEM_GG = 1,
    /** @brief A Game Gear running a Master System cartridge (the adapter's mode). */
    SMS_SYSTEM_GG_MS = 2,
    /** @brief An SG-1000: the TMS9918A, 1 KiB of RAM, a linear cartridge. */
    SMS_SYSTEM_SG = 3,
} sms_system;

/** @brief The video standard and region of the console. */
typedef enum sms_region {
    SMS_REGION_NTSC_U = 0,
    SMS_REGION_NTSC_J = 1,
    SMS_REGION_PAL = 2,
} sms_region;

/** @brief The cartridge's bank hardware. */
typedef enum sms_mapper {
    /** @brief The Sega mapper (registers at 0xFFFC-0xFFFF), which also runs a cartridge of 48 KiB or less as it is. */
    SMS_MAPPER_SEGA = 0,
    /** @brief The Codemasters mapper (writes to 0x0000-0x3FFF, 0x4000-0x7FFF and 0x8000-0xBFFF select the banks). */
    SMS_MAPPER_CODEMASTERS = 1,
    /** @brief The Korean mapper (writes to 0xA000-0xBFFF select the bank at 0x8000). */
    SMS_MAPPER_KOREA = 2,
} sms_mapper;

/** @brief Bits of an input mask: one per button, set while it is held. */
enum {
    SMS_BUTTON_UP = 1u << 0,
    SMS_BUTTON_DOWN = 1u << 1,
    SMS_BUTTON_LEFT = 1u << 2,
    SMS_BUTTON_RIGHT = 1u << 3,
    SMS_BUTTON_1 = 1u << 4,
    SMS_BUTTON_2 = 1u << 5,
    /** @brief Start on a Game Gear, the Pause button on a Master System. */
    SMS_BUTTON_START = 1u << 6,
    /** @brief The Reset button of a Master System. */
    SMS_BUTTON_RESET = 1u << 7,
};

/** @brief The CPU's T-states before the VDP work of a count is visible: the clock steps of ares. */
typedef struct sms_clock {
    uint64_t cpu_step;
    uint64_t vdp_step;
    /** @brief A base point: T-state c0 is the time of VDP count q0 plus r0 / vdp_step. */
    uint64_t c0;
    uint64_t q0;
    uint64_t r0;
} sms_clock;

/** @brief Gamepad direction latches: opposite directions held together keep the earlier one. */
typedef struct sms_pad {
    bool up, down, left, right;
    bool y_hold, x_hold;
} sms_pad;

/** @brief Gives the buttons held now (SMS_BUTTON_* bits), for a machine without an input script. */
typedef uint32_t sms_input_fn(void *user);

/** @brief Called at each frame edge with the number of the frame that just completed (0 for the first). */
typedef void sms_frame_fn(void *user, uint64_t frame);

/** @brief The machine. */
typedef struct sms {
    sms_system system;
    sms_region region;
    sms_mapper mapper;

    /** @brief The ROM, padded to a power of two the way ares mirrors it. */
    uint8_t *rom;
    uint32_t rom_mask;
    /** @brief The ROM's size before mirroring. */
    uint32_t rom_size;
    uint8_t ram[0x2000];
    /** @brief The cartridge RAM: 32 KiB on the Sega mapper, 8 KiB on the Codemasters one. */
    uint8_t cart_ram[0x8000];
    uint32_t cart_ram_mask;

    /** @brief Sega mapper registers. */
    uint8_t bank[3];
    uint8_t bank_upper;
    uint8_t bank_shift;
    bool ram_bank2_select;
    bool ram_bank2;
    bool ram_bank3;
    bool rom_write;
    /** @brief Codemasters: cartridge RAM at 0xA000. */
    bool cm_ram;

    /** @brief The memory control port (0x3E). */
    bool io_enable, bios_enable, ram_enable, card_enable, cart_enable, exp_enable;
    /** @brief Pull-up on the data bus: 0xFF on a Master System II and a Game Gear. */
    uint8_t pullup;

    /** @brief The I/O control port (0x3F): TR and TH directions and levels of each controller port. */
    bool tr_dir[2], th_dir[2], tr_level[2], th_level[2];
    sms_pad pad;
    /** @brief The Game Gear's serial and parallel port registers. */
    uint8_t gg_parallel, gg_direction, gg_transmit, gg_receive, gg_serial;
    bool gg_nmi_enable;

    /** @brief The buttons of the current frame, and the values the console's own buttons last read. */
    uint32_t buttons;
    bool start_value, reset_value;
    /** @brief The input script: the mask of frame n is inputs[n]; frames past the end use the last one. */
    const uint8_t *inputs;
    size_t input_count;
    /** @brief With no input script: read at power on and at each frame edge. */
    sms_input_fn *input;
    void *input_user;

    sms_vdp vdp;
    /** @brief The SG-1000's video chip, in place of vdp. */
    sms_tms tms;
    sms_psg psg;
    /** @brief The FM unit, present on a Japanese Master System (fm). */
    sms_ym2413 fm_chip;
    bool fm;
    /** @brief The audio control port (0xF2): the PSG and the FM unit muted. */
    bool psg_mute, fm_mute;
    sms_clock clock;

    /** @brief The NMI edge latched and not yet taken. */
    bool nmi;
    /** @brief T-states since power on at the current instruction boundary. */
    uint64_t cycles;
    /** @brief The earliest T-state at which the interrupt line or a frame edge may change on its own. */
    uint64_t event_cycle;
    /** @brief Frames completed. */
    uint64_t frames;
    /** @brief The VDP count of the next frame edge (the start of line 240). */
    uint64_t frame_pos;

    /** @brief The picture of the last completed frame, SMS_VDP_W wide, and its lines. */
    uint16_t picture[SMS_VDP_W * SMS_VDP_H];
    uint32_t picture_lines;

    sms_frame_fn *on_frame;
    void *user;
} sms;

/** @brief The settings of sms_init. */
typedef struct sms_config {
    sms_system system;
    sms_region region;
    sms_mapper mapper;
} sms_config;

/** @brief Powers the machine on with the ROM rom (copied, mirrored to a power of two). Returns 0, or -1 when out of memory. */
int sms_init(sms *m, const sms_config *cfg, const uint8_t *rom, size_t rom_len);
/** @brief Frees the ROM copy. */
void sms_free(sms *m);

/** @brief A memory read at T-state now. */
uint8_t sms_read(sms *m, uint16_t addr, uint64_t now);
/** @brief A memory write at T-state now. */
void sms_write(sms *m, uint16_t addr, uint8_t data, uint64_t now);
/** @brief The byte at addr with no side effects, for reports and code checks. */
uint8_t sms_peek(const sms *m, uint16_t addr);
/** @brief A port read at T-state now (the end of the CPU's I/O cycle). */
uint8_t sms_in(sms *m, uint16_t port, uint64_t now);
/** @brief A port write at T-state now. */
void sms_out(sms *m, uint16_t port, uint8_t data, uint64_t now);

/** @brief Brings the VDP and the frame edges up to T-state now. */
void sms_catch_up(sms *m, uint64_t now);
/** @brief Whether the maskable interrupt input is asserted at T-state now (the machine is caught up first). */
bool sms_int_line(sms *m, uint64_t now);
/** @brief The 16 KiB ROM bank the CPU sees in slot s (0 to 2) for code, or -1 when the slot shows RAM or nothing. */
int32_t sms_slot_bank(const sms *m, unsigned s);
/** @brief The VDP count whose work is visible at T-state now. */
uint64_t sms_vdp_count(sms *m, uint64_t now);
/** @brief The first T-state at which the VDP work of count p is visible. */
uint64_t sms_cycle_of(sms *m, uint64_t p);

/** @brief The picture size the console shows: 256 by the VDP's lines, or the Game Gear's 160x144 window. */
void sms_screen(const sms *m, uint32_t *w, uint32_t *h);
/** @brief Copies the visible picture of the last completed frame (sms_screen) as VDP colours. */
void sms_picture(const sms *m, uint16_t *out);
/** @brief The colour of a VDP colour value as 8-bit RGB. */
void sms_rgb(const sms *m, uint16_t color, uint8_t rgb[3]);

#endif
