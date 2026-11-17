/**
 * @file vdp.h
 * @brief The video display processor of the Master System and Game Gear
 * (315-5124 and 315-5246) with the TMS9918 modes the SG-1000 uses: VRAM,
 * CRAM, the registers, the counters, the line and frame interrupts, the
 * background and sprite units and the colour output, run in catch-up form.
 *
 * Time is the VDP's own clock (684 counts per line, three per CPU T-state
 * on the console). A position p is the work the chip does at count p since
 * power on: the start of a line, a pixel, the counter step. sms_vdp_run
 * does the work of every position up to one; the machine layer maps CPU
 * cycles to positions (sms.h).
 */
#ifndef RELIFT_SMS_VDP_H
#define RELIFT_SMS_VDP_H

#include <stdbool.h>
#include <stdint.h>

/** @brief VDP counts per line. */
#define SMS_VDP_LINE 684u
/** @brief Widest picture, and the most lines of one. */
#define SMS_VDP_W 256u
#define SMS_VDP_H 240u

/** @brief How the chip shows its colours. */
typedef enum sms_vdp_mode {
    /** @brief A Master System console: 6-bit CRAM, 256 pixels wide. */
    SMS_VDP_MS = 0,
    /** @brief A Game Gear in its own mode: 12-bit CRAM through a two-byte latch, a 160x144 window. */
    SMS_VDP_GG = 1,
    /** @brief A Game Gear running a Master System cartridge: 6-bit CRAM widened to 12 bits. */
    SMS_VDP_GG_MS = 2,
} sms_vdp_mode;

/** @brief Called at the start of line 240, before its work: the frame's picture is complete. */
typedef void sms_vdp_frame_fn(void *user);
/** @brief Called when the chip's interrupt output changes. */
typedef void sms_vdp_irq_fn(void *user, bool level);

/** @brief A sprite the line holds. */
typedef struct sms_vdp_object {
    int16_t x;
    int16_t y;
    uint8_t pattern;
    uint8_t color;
} sms_vdp_object;

/** @brief The chip. */
typedef struct sms_vdp {
    sms_vdp_mode mode;
    /** @brief 1 for the 315-5124 (one picture height, the name table mask), 2 for the later chips. */
    uint8_t revision;
    bool pal;

    uint8_t vram[0x4000];
    /** @brief 6 bits per entry on a Master System, 12 on a Game Gear. */
    uint16_t cram[32];

    uint8_t code;
    uint16_t address;
    bool display;
    /** @brief Mode bits: bit 0 M1 (register 1 bit 4), bit 1 M2 (register 0 bit 1), bit 2 M3 (register 1 bit 3), bit 3 M4 (register 0 bit 2). */
    uint8_t video_mode;

    uint32_t vcounter;
    uint32_t hcounter;
    /** @brief The line the chip is on draws pixels: decided at its start, as the chip does. */
    bool line_draws;
    uint16_t ccounter;

    bool latch_control;
    uint32_t latch_hcounter;
    uint8_t latch_vram;
    uint8_t latch_cram;

    bool hscroll_lock;
    bool vscroll_lock;
    uint8_t name_table;
    uint8_t color_table;
    uint8_t pattern_table;
    uint8_t hscroll;
    uint8_t vscroll;
    uint8_t latch_hscroll;
    uint8_t latch_vscroll;

    sms_vdp_object objects[8];
    bool sprite_zoom;
    bool sprite_size;
    bool sprite_shift;
    uint8_t sprite_attr;
    uint8_t sprite_pattern;
    uint8_t overflow_index;
    bool overflow;
    bool collision;

    bool external_sync;
    bool left_clip;
    uint8_t backdrop;

    bool line_enable;
    bool line_pending;
    uint8_t line_counter;
    uint8_t line_coincidence;
    bool frame_enable;
    bool frame_pending;
    bool irq_level;

    /** @brief Positions done: every position below this one. */
    uint64_t pos;
    /** @brief Frames completed. */
    uint64_t frames;

    /** @brief The picture, row by row, SMS_VDP_W wide; a Master System colour is 6 bits, a Game Gear one 12. */
    uint16_t pixels[SMS_VDP_W * SMS_VDP_H];

    sms_vdp_frame_fn *on_frame;
    sms_vdp_irq_fn *on_irq;
    void *user;
} sms_vdp;

/** @brief Power on. */
void sms_vdp_reset(sms_vdp *v, sms_vdp_mode mode, uint8_t revision, bool pal);
/** @brief Does the work of every position up to and including last. */
void sms_vdp_run(sms_vdp *v, uint64_t last);
/** @brief The first position at or after the next one whose work raises the interrupt output from low, or UINT64_MAX when the registers allow none. */
uint64_t sms_vdp_next_irq(const sms_vdp *v);
/** @brief The next position, at or after the next one to do, that starts line 240 (its work calls on_frame first). */
uint64_t sms_vdp_frame_edge(const sms_vdp *v);

/** @brief Lines per frame: 262, or 313 on a PAL console. */
uint32_t sms_vdp_lines(const sms_vdp *v);
/** @brief Lines of picture in the current mode: 192, 224 or 240. */
uint32_t sms_vdp_vlines(const sms_vdp *v);

/** @brief A read of the V counter port (0x7E). */
uint8_t sms_vdp_vcounter(const sms_vdp *v);
/** @brief A read of the H counter port (0x7F): the count latched by the last TH rise. */
uint8_t sms_vdp_hcounter(const sms_vdp *v);
/** @brief Latches the H counter (a TH line rose). */
void sms_vdp_latch_h(sms_vdp *v);
/** @brief A read of the data port (0xBE). */
uint8_t sms_vdp_data_read(sms_vdp *v);
/** @brief A read of the status port (0xBF): returns the flags and clears them. */
uint8_t sms_vdp_status(sms_vdp *v);
/** @brief A write of the data port. */
void sms_vdp_data_write(sms_vdp *v, uint8_t data);
/** @brief A write of the control port. */
void sms_vdp_control(sms_vdp *v, uint8_t data);
/** @brief The C-sync counter (bits 3, 7 and 11 read through the FM unit's port). */
uint16_t sms_vdp_ccounter(const sms_vdp *v);

/** @brief The colour a CRAM index (16 to 31: sprites and backdrop) shows in the current mode. */
uint16_t sms_vdp_palette(const sms_vdp *v, unsigned index);

#endif
