/**
 * @file audio.c
 * @brief The console's sound mixed and resampled to the host rate.
 */
#include "audio.h"

#include <string.h>

#define PSG_SCALE 16384.0
#define DC_KEEP 32604

void sms_audio_init(sms_audio *a, const sms *m, uint32_t rate)
{
    memset(a, 0, sizeof *a);
    a->rate = rate ? rate : SMS_AUDIO_RATE;
    a->clock_hz = m->region == SMS_REGION_PAL ? 3546895u : 3579545u;
    a->psg_n = m->psg.s.samples - m->psg.count;
    a->fm_n = m->fm_chip.s.samples - m->fm_chip.count;
}

/** @brief The CPU clock at which output frame k ends. */
static uint64_t frame_end(const sms_audio *a, uint64_t k) { return (k + 1) * a->clock_hz / a->rate; }

static int32_t mean(int64_t sum, uint32_t n, int32_t last) { return n ? (int32_t)(sum / (int64_t)n) : last; }

static int16_t clamp16(int64_t v) { return (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }

static int16_t dc_block(sms_audio *a, int side, int64_t x)
{
    int64_t y = x - a->dc_in[side] + a->dc_out[side] * DC_KEEP / 32768;
    a->dc_in[side] = x;
    a->dc_out[side] = y;
    return clamp16(y);
}

static void emit(sms_audio *a, const sms *m, int16_t *out)
{
    int32_t pl = mean(a->psg_sum_l, a->psg_count, a->psg_l);
    int32_t pr = mean(a->psg_sum_r, a->psg_count, a->psg_r);
    int32_t fm = mean(a->fm_sum, a->fm_count, a->fm);
    if (m->psg_mute)
        pl = pr = 0;
    if (!m->fm || m->fm_mute)
        fm = 0;
    out[0] = dc_block(a, 0, (int64_t)pl + 2 * (int64_t)fm);
    out[1] = dc_block(a, 1, (int64_t)pr + 2 * (int64_t)fm);
    a->psg_sum_l = a->psg_sum_r = a->fm_sum = 0;
    a->psg_count = a->fm_count = 0;
    a->frames++;
}

size_t sms_audio_take(sms_audio *a, sms *m, uint64_t now, int16_t *out, size_t max)
{
    sms_psg_catch_up(&m->psg, now);
    if (m->fm)
        sms_ym2413_catch_up(&m->fm_chip, now);
    size_t made = 0;
    for (;;) {
        if (a->psg_at == a->psg_len) {
            a->psg_len = sms_psg_drain(&m->psg, a->psg_held, SMS_AUDIO_HELD);
            a->psg_at = 0;
        }
        if (m->fm && a->fm_at == a->fm_len) {
            a->fm_len = sms_ym2413_drain(&m->fm_chip, a->fm_held, SMS_AUDIO_HELD);
            a->fm_at = 0;
        }
        uint64_t tp = a->psg_at < a->psg_len ? (a->psg_n + 1) * SMS_PSG_DIV : UINT64_MAX;
        uint64_t tf = a->fm_at < a->fm_len ? (a->fm_n + 1) * SMS_YM2413_DIV : UINT64_MAX;
        uint64_t t = tp < tf ? tp : tf;
        uint64_t end = frame_end(a, a->frames);
        if (end < t && end <= now) {
            if (made == max)
                break;
            emit(a, m, out + 2 * made);
            made++;
            continue;
        }
        if (t == UINT64_MAX)
            break;
        if (tp <= tf) {
            const sms_psg_sample *p = &a->psg_held[a->psg_at++];
            a->psg_l = (int32_t)(p->left * PSG_SCALE);
            a->psg_r = (int32_t)(p->right * PSG_SCALE);
            a->psg_sum_l += a->psg_l;
            a->psg_sum_r += a->psg_r;
            a->psg_count++;
            a->psg_n++;
        } else {
            a->fm = a->fm_held[a->fm_at++];
            a->fm_sum += a->fm;
            a->fm_count++;
            a->fm_n++;
        }
    }
    return made;
}
