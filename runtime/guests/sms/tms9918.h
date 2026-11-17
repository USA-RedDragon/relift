/**
 * @file tms9918.h
 * @brief The TMS9918A video display processor of the SG-1000: VRAM, the
 * eight registers, the frame interrupt, the graphics 1 and 2, text and
 * multicolor modes and the sprites, run in catch-up form.
 *
 * Time is the chip's own clock (684 counts per line, 262 lines, three per
 * CPU T-state). A position p is the work the chip does at count p since
 * power on, as for the Master System VDP (vdp.h).
 */
#ifndef RELIFT_SMS_TMS9918_H
#define RELIFT_SMS_TMS9918_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Counts per line and lines per frame. */
#define SMS_TMS_LINE 684u
#define SMS_TMS_LINES 262u
/** @brief The picture: 256 by 192. */
#define SMS_TMS_W 256u
#define SMS_TMS_H 192u

/** @brief Called when line 192 starts: the frame's picture is complete. */
typedef void sms_tms_frame_fn(void *user);

/** @brief A sprite the line holds. */
typedef struct sms_tms_object {
    int16_t x;
    int16_t y;
    uint8_t pattern;
    uint8_t color;
} sms_tms_object;

/** @brief The chip. */
typedef struct sms_tms {
    uint8_t vram[0x4000];
    uint16_t control;
    uint8_t control_latch;
    uint8_t vram_latch;
    uint8_t video_mode;
    bool vram_mode;
    uint8_t name_table;
    uint8_t color_table;
    uint8_t pattern_table;
    uint8_t sprite_attr;
    uint8_t sprite_pattern;
    bool sprite_zoom;
    bool sprite_size;
    uint8_t overflow_index;
    bool overflow;
    bool collision;
    sms_tms_object objects[4];
    bool external_sync;
    bool display;
    uint8_t background;
    uint8_t foreground;
    bool frame_enable;
    bool frame_pending;
    bool irq_level;
    uint32_t vcounter;
    uint32_t hcounter;
    uint64_t pos;
    uint64_t frames;
    /** @brief The picture, 4-bit colour indices. */
    uint8_t pixels[SMS_TMS_W * SMS_TMS_H];
    sms_tms_frame_fn *on_frame;
    void *user;
} sms_tms;

/** @brief Power on. */
void sms_tms_reset(sms_tms *t);
/** @brief Does the work of every position up to and including last. */
void sms_tms_run(sms_tms *t, uint64_t last);
/** @brief The first position, at or after the next one, whose work raises the interrupt output, or UINT64_MAX. */
uint64_t sms_tms_next_irq(const sms_tms *t);
/** @brief The next position, at or after the next one to do, that starts line 192. */
uint64_t sms_tms_frame_edge(const sms_tms *t);
/** @brief A read of the data port. */
uint8_t sms_tms_data_read(sms_tms *t);
/** @brief A read of the status port: returns the flags and clears them. */
uint8_t sms_tms_status(sms_tms *t);
/** @brief A write of the data port. */
void sms_tms_data_write(sms_tms *t, uint8_t data);
/** @brief A write of the control port. */
void sms_tms_control(sms_tms *t, uint8_t data);
/** @brief The colour of index c as 8-bit RGB. */
void sms_tms_rgb(uint8_t c, uint8_t rgb[3]);

#endif
