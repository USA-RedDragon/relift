/**
 * @file ym2413.h
 * @brief The YM2413 (OPLL) FM sound unit of the Japanese Master System:
 * nine two-operator voices or six voices and five rhythm sounds, at the
 * chip's own rate.
 *
 * Time is in CPU clocks (the Z80's T-states), counted from reset. The chip
 * makes one sample every 72 clocks; sample k is complete at clock 72k. A
 * write at clock t first catches the chip up, so it is heard by the samples
 * completed after t, and by those only. Each sample is the chip's mix
 * (melody plus twice the rhythm, a signed integer); sms_ym2413_level scales
 * it as the console outputs it.
 */
#ifndef RELIFT_SMS_YM2413_H
#define RELIFT_SMS_YM2413_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief CPU clocks per sample. */
#define SMS_YM2413_DIV 72u
/** @brief Samples the buffer holds before it drops the oldest. */
#define SMS_YM2413_BUF 4096u

/** @brief One operator. */
typedef struct sms_ym2413_op {
    uint8_t slot;
    bool key_on;
    bool sustain_on;
    uint8_t multiple;
    bool scale_rate;
    bool sustainable;
    bool vibrato;
    bool tremolo;
    uint8_t scale_level;
    bool waveform;
    uint8_t attack;
    uint8_t decay;
    uint8_t sustain;
    uint8_t release;
    uint8_t total_level;
    bool audible;
    uint8_t state;
    uint8_t rate;
    uint8_t divider;
    uint64_t sequence;
    uint8_t envelope;
    uint8_t level;
    uint16_t fnumber;
    uint8_t block;
    uint32_t pitch[8];
    uint32_t phase;
    int16_t output;
    int16_t prior;
} sms_ym2413_op;

/** @brief One voice: a modulator and a carrier. */
typedef struct sms_ym2413_voice {
    uint8_t tone;
    uint16_t fnumber;
    uint8_t block;
    uint8_t level;
    uint8_t feedback;
    sms_ym2413_op mod;
    sms_ym2413_op car;
} sms_ym2413_voice;

/** @brief Everything a save state holds. */
typedef struct sms_ym2413_state {
    uint8_t custom[8];
    sms_ym2413_voice voice[9];
    uint32_t clock;
    uint8_t address;
    bool rhythm;
    uint32_t noise;
    /** @brief Samples made since reset. */
    uint64_t samples;
} sms_ym2413_state;

/** @brief The device: the state and the sample buffer. */
typedef struct sms_ym2413 {
    sms_ym2413_state s;
    int32_t buf[SMS_YM2413_BUF];
    size_t head;
    size_t count;
    uint64_t dropped;
} sms_ym2413;

/** @brief Power on: silent, clock 0, empty buffer. */
void sms_ym2413_reset(sms_ym2413 *y);
/** @brief Makes every sample completed by clock until. */
void sms_ym2413_catch_up(sms_ym2413 *y, uint64_t until);
/** @brief Catches up to clock time, then writes the address port (0xF0). */
void sms_ym2413_address(sms_ym2413 *y, uint64_t time, uint8_t data);
/** @brief Catches up to clock time, then writes the data port (0xF1). */
void sms_ym2413_write(sms_ym2413 *y, uint64_t time, uint8_t data);
/** @brief Moves up to max samples, oldest first, out of the buffer; returns how many. */
size_t sms_ym2413_drain(sms_ym2413 *y, int32_t *out, size_t max);
/** @brief A sample as the console outputs it (about -1.0 to 1.0). */
double sms_ym2413_level(int32_t sample);

#endif
