/**
 * @file psg.h
 * @brief The SN76489 sound chip of the Sega Master System and Game Gear: three
 * tone channels, one noise channel with a 16-bit shift register, the Game Gear
 * stereo mask, and the samples it makes at the chip's own rate.
 *
 * Time is in CPU clocks (the Z80's T-states, 3.579545 MHz on NTSC consoles),
 * counted from reset. The chip makes one sample every 16 clocks; sample k is
 * complete at clock 16k. A write at clock t first catches the chip up, so it
 * is heard by the samples completed after t, and by those only.
 *
 * Each sample holds what the chip computes, the four 4-bit attenuations
 * (15 is off), and the mix the console outputs, before any resampling. The
 * caller drains the samples; relift's audio layer resamples them later.
 */
#ifndef RELIFT_SMS_PSG_H
#define RELIFT_SMS_PSG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief CPU clocks per sample. */
#define SMS_PSG_DIV 16u
/** @brief Samples the buffer holds before it drops the oldest. */
#define SMS_PSG_BUF 4096u

/** @brief One sample: the attenuation of each channel as the chip outputs it. */
typedef struct sms_psg_sample {
    /** @brief Channels 0-2 are the tones, 3 the noise; 15 is silent. */
    uint8_t att[4];
    /** @brief The left output, 0.0 to 1.0 (the mono output on a Master System). */
    double left;
    /** @brief The right output (equal to left on a Master System). */
    double right;
} sms_psg_sample;

/** @brief A tone channel. */
typedef struct sms_psg_tone {
    uint8_t volume;
    uint16_t counter;
    uint16_t pitch;
    uint8_t output;
} sms_psg_tone;

/** @brief The noise channel. */
typedef struct sms_psg_noise {
    uint8_t volume;
    uint16_t counter;
    uint16_t pitch;
    uint8_t enable;
    uint8_t rate;
    uint16_t lfsr;
    uint8_t flip;
    uint8_t output;
} sms_psg_noise;

/** @brief Everything a save state holds: all registers and the clock. */
typedef struct sms_psg_state {
    sms_psg_tone tone[3];
    sms_psg_noise noise;
    uint8_t latch_type;
    uint8_t latch_channel;
    /** @brief The Game Gear stereo port 0x06: bits 4-7 left, bits 0-3 right. */
    uint8_t stereo;
    bool game_gear;
    /** @brief Samples made since reset. */
    uint64_t samples;
} sms_psg_state;

/** @brief The device: the state, the output table and the sample buffer. */
typedef struct sms_psg {
    sms_psg_state s;
    double level[16];
    sms_psg_sample buf[SMS_PSG_BUF];
    size_t head;
    size_t count;
    /** @brief Samples dropped from a full buffer since reset. */
    uint64_t dropped;
} sms_psg;

/** @brief Power on: silent, clock 0, empty buffer; game_gear selects stereo output. */
void sms_psg_reset(sms_psg *p, bool game_gear);
/** @brief Makes every sample completed by clock until. */
void sms_psg_catch_up(sms_psg *p, uint64_t until);
/** @brief Catches up to clock time, then writes the chip's data port (0x7E or 0x7F). */
void sms_psg_write(sms_psg *p, uint64_t time, uint8_t data);
/** @brief Catches up to clock time, then writes the Game Gear stereo port 0x06 (ignored on a Master System). */
void sms_psg_stereo(sms_psg *p, uint64_t time, uint8_t data);
/** @brief The clock at which the next sample completes. */
uint64_t sms_psg_next(const sms_psg *p);
/** @brief Moves up to max samples, oldest first, out of the buffer; returns how many. */
size_t sms_psg_drain(sms_psg *p, sms_psg_sample *out, size_t max);
/** @brief Copies the registers and the clock out. */
void sms_psg_save(const sms_psg *p, sms_psg_state *out);
/** @brief Loads a saved state and empties the buffer. */
void sms_psg_restore(sms_psg *p, const sms_psg_state *in);

#endif
