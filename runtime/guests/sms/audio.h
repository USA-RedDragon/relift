/**
 * @file audio.h
 * @brief The sound the console outputs, as 16-bit stereo at a host rate:
 * the PSG (one sample per 16 CPU clocks) and, on a Japanese Master System,
 * the FM unit (one per 72), each muted as the audio control port says,
 * mixed and resampled.
 *
 * Output frame k covers the CPU clocks from k * clock / rate up to
 * (k + 1) * clock / rate; it is the mean of the chip samples completed in
 * that span (a box filter), or the last sample when none completed. The
 * mean is taken in integers, so every host makes the same frames. A DC
 * blocker follows, as the console's output capacitor does.
 */
#ifndef RELIFT_SMS_AUDIO_H
#define RELIFT_SMS_AUDIO_H

#include <stddef.h>
#include <stdint.h>

#include "sms.h"

/** @brief The default host rate. */
#define SMS_AUDIO_RATE 44100u

/** @brief Chip samples drained from a chip's buffer and not yet resampled. */
#define SMS_AUDIO_HELD 256u

/** @brief The resampler's state. */
typedef struct sms_audio {
    uint32_t rate;
    uint64_t clock_hz;
    /** @brief Output frames made. */
    uint64_t frames;
    /** @brief Chip samples taken so far. */
    uint64_t psg_n, fm_n;
    /** @brief The sums and counts of the chip samples of the frame in progress. */
    int64_t psg_sum_l, psg_sum_r, fm_sum;
    uint32_t psg_count, fm_count;
    /** @brief The last chip values, held when a frame has no sample of a chip. */
    int32_t psg_l, psg_r, fm;
    /** @brief Samples taken from the chips' buffers, the next one to use first. */
    sms_psg_sample psg_held[SMS_AUDIO_HELD];
    int32_t fm_held[SMS_AUDIO_HELD];
    size_t psg_at, psg_len, fm_at, fm_len;
    /** @brief The DC blocker's state, per side. */
    int64_t dc_in[2], dc_out[2];
} sms_audio;

/** @brief Starts the resampler for m at rate frames per second. */
void sms_audio_init(sms_audio *a, const sms *m, uint32_t rate);
/**
 * @brief Catches the sound chips up to CPU clock now and writes up to max
 * stereo frames completed by then into out (interleaved left, right);
 * returns how many. Call often enough that the chips' buffers do not fill
 * (every frame is plenty).
 */
size_t sms_audio_take(sms_audio *a, sms *m, uint64_t now, int16_t *out, size_t max);

#endif
