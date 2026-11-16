/**
 * @file psg.c
 * @brief The SN76489 and the console's mix of it, with catch-up timing.
 *
 * Ported from ares (component/audio/sn76489/sn76489.cpp and ms/psg/psg.cpp),
 * with the per-sample thread replaced by a catch-up loop over the same
 * sample step. It carries this notice:
 *
 * Copyright (c) 2004-2025 ares team, Near et al
 *
 * Permission to use, copy, modify, and/or distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */
#include "psg.h"

#include <math.h>
#include <string.h>

static void tone_clock(sms_psg_tone *t)
{
    if (t->counter == 0 || --t->counter == 0) {
        t->counter = t->pitch;
        t->output ^= 1u;
    }
}

static void noise_clock(sms_psg_noise *n)
{
    if (n->counter == 0 || --n->counter == 0) {
        static const uint16_t fixed[3] = {0x10, 0x20, 0x40};
        n->counter = n->rate < 3 ? fixed[n->rate] : n->pitch;
        n->flip ^= 1u;
        if (n->flip) {
            n->output = n->lfsr & 1u;
            unsigned tap = ((n->lfsr >> 3) & 1u) & n->enable;
            n->lfsr = (uint16_t)((((n->lfsr & 1u) ^ tap) << 15) | (n->lfsr >> 1));
        }
    }
}

static void make_sample(sms_psg *p)
{
    sms_psg_state *s = &p->s;
    tone_clock(&s->tone[0]);
    tone_clock(&s->tone[1]);
    tone_clock(&s->tone[2]);
    noise_clock(&s->noise);

    sms_psg_sample out = {{15, 15, 15, 15}, 0.0, 0.0};
    for (unsigned i = 0; i < 3; i++)
        if (s->tone[i].output)
            out.att[i] = s->tone[i].volume;
    if (s->noise.output)
        out.att[3] = s->noise.volume;

    if (s->game_gear) {
        double left = 0.0;
        double right = 0.0;
        for (unsigned i = 0; i < 4; i++) {
            if (s->stereo & (0x10u << i))
                left += p->level[out.att[i]];
        }
        for (unsigned i = 0; i < 4; i++) {
            if (s->stereo & (1u << i))
                right += p->level[out.att[i]];
        }
        out.left = left / 4.0;
        out.right = right / 4.0;
    } else {
        double mono = 0.0;
        for (unsigned i = 0; i < 4; i++)
            mono += p->level[out.att[i]];
        out.left = mono / 4.0;
        out.right = out.left;
    }

    s->samples++;
    if (p->count == SMS_PSG_BUF) {
        p->head = (p->head + 1) % SMS_PSG_BUF;
        p->count--;
        p->dropped++;
    }
    p->buf[(p->head + p->count) % SMS_PSG_BUF] = out;
    p->count++;
}

void sms_psg_reset(sms_psg *p, bool game_gear)
{
    memset(&p->s, 0, sizeof p->s);
    p->s.tone[0].volume = 15;
    p->s.tone[1].volume = 15;
    p->s.tone[2].volume = 15;
    p->s.noise.volume = 15;
    p->s.noise.lfsr = 0x8000;
    p->s.stereo = 0xFF;
    p->s.game_gear = game_gear;
    for (unsigned i = 0; i < 15; i++)
        p->level[i] = pow(2.0, (double)i * -2.0 / 6.0);
    p->level[15] = 0.0;
    p->head = 0;
    p->count = 0;
    p->dropped = 0;
}

void sms_psg_catch_up(sms_psg *p, uint64_t until)
{
    while ((p->s.samples + 1) * SMS_PSG_DIV <= until)
        make_sample(p);
}

static void noise_write(sms_psg_noise *n, uint8_t data)
{
    n->rate = data & 3u;
    n->enable = (data >> 2) & 1u;
    n->lfsr = 0x8000;
}

void sms_psg_write(sms_psg *p, uint64_t time, uint8_t data)
{
    sms_psg_state *s = &p->s;
    sms_psg_catch_up(p, time);
    if (data & 0x80u) {
        s->latch_type = (data >> 4) & 1u;
        s->latch_channel = (data >> 5) & 3u;
        if (s->latch_type) {
            if (s->latch_channel == 3)
                s->noise.volume = data & 15u;
            else
                s->tone[s->latch_channel].volume = data & 15u;
        } else if (s->latch_channel == 3) {
            noise_write(&s->noise, data);
        } else {
            sms_psg_tone *t = &s->tone[s->latch_channel];
            t->pitch = (uint16_t)((t->pitch & 0x3F0u) | (data & 15u));
            if (s->latch_channel == 2)
                s->noise.pitch = t->pitch;
        }
    } else if (!s->latch_type) {
        if (s->latch_channel == 3) {
            noise_write(&s->noise, data);
        } else {
            sms_psg_tone *t = &s->tone[s->latch_channel];
            t->pitch = (uint16_t)((t->pitch & 0x00Fu) | ((data & 0x3Fu) << 4));
            if (s->latch_channel == 2)
                s->noise.pitch = t->pitch;
        }
    }
}

void sms_psg_stereo(sms_psg *p, uint64_t time, uint8_t data)
{
    sms_psg_catch_up(p, time);
    if (p->s.game_gear)
        p->s.stereo = data;
}

uint64_t sms_psg_next(const sms_psg *p)
{
    return (p->s.samples + 1) * SMS_PSG_DIV;
}

size_t sms_psg_drain(sms_psg *p, sms_psg_sample *out, size_t max)
{
    size_t n = p->count < max ? p->count : max;
    for (size_t i = 0; i < n; i++)
        out[i] = p->buf[(p->head + i) % SMS_PSG_BUF];
    p->head = (p->head + n) % SMS_PSG_BUF;
    p->count -= n;
    return n;
}

void sms_psg_save(const sms_psg *p, sms_psg_state *out)
{
    *out = p->s;
}

void sms_psg_restore(sms_psg *p, const sms_psg_state *in)
{
    p->s = *in;
    p->head = 0;
    p->count = 0;
}
