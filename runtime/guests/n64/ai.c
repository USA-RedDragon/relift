/**
 * @file ai.c
 * @brief AI: the two-entry DMA FIFO, its status, the interrupt at the start
 * of each buffer, the DAC's sample clock from AI_DACRATE and the samples
 * (16-bit big-endian stereo read from RDRAM) resampled to the output rate
 * into a ring the program drains.
 *
 * The samples run in closed form: a register access, the event at the end
 * of a buffer or a drain catches the DAC up to the CPU's tick, one sample at
 * a time. The register rules, the FIFO, the address carry at 8 KiB, the
 * decay of the DAC level when no buffer plays and the sample period are
 * ported from ares (n64/ai/ai.cpp and io.cpp), which carries this notice:
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
#include "n64_int.h"

#include <math.h>
#include <stdlib.h>

/** @brief The NTSC and PAL video clocks, in Hz: AI_DACRATE divides one of them. */
#define VIDEO_HZ_NTSC 48681818u
#define VIDEO_HZ_PAL 49656530u
/** @brief Ares counts its clocks at twice the CPU's rate; the sample period is in these half cycles. */
#define HALF_HZ (RL_N64_HZ * 2u)

static uint32_t ring_used(const rl_n64 *m) { return m->ai.tail - m->ai.head; }

static void ring_push(rl_n64 *m, int32_t l, int32_t r)
{
    if (ring_used(m) >= RL_N64_AUDIO_RING) {
        m->ai.dropped++;
        return;
    }
    int16_t *o = m->ai.ring + (size_t)(m->ai.tail++ % RL_N64_AUDIO_RING) * 2u;
    o[0] = (int16_t)(l >> 8);
    o[1] = (int16_t)(r >> 8);
}

/** @brief Takes the DAC's level for one sample at the DAC's rate and emits the output frames it spans (linear interpolation). */
static void resample(rl_n64 *m, int32_t l, int32_t r)
{
    const uint64_t one = 1ull << 32;
    uint64_t step = ((uint64_t)m->ai.freq << 32) / RL_N64_AUDIO_RATE;
    while (m->ai.pos < one) {
        int64_t f = (int64_t)m->ai.pos;
        ring_push(m, m->ai.prev_l + (int32_t)(((int64_t)(l - m->ai.prev_l) * f) >> 32),
                  m->ai.prev_r + (int32_t)(((int64_t)(r - m->ai.prev_r) * f) >> 32));
        m->ai.pos += step;
    }
    m->ai.pos -= one;
    m->ai.prev_l = l;
    m->ai.prev_r = r;
}

static int32_t decay(int32_t v, uint32_t factor)
{
    return v < 0 ? -(int32_t)(((int64_t)-v * factor) >> 16) : (int32_t)(((int64_t)v * factor) >> 16);
}

static void set_rate(rl_n64 *m, uint32_t f)
{
    m->ai.freq = f ? f : 1u;
    m->ai.period = HALF_HZ / m->ai.freq;
    m->ai.decay = (uint32_t)(exp(-1.0 / ((double)m->ai.freq * 0.003)) * 65536.0);
}

static void update_rate(rl_n64 *m) { set_rate(m, (m->pal ? VIDEO_HZ_PAL : VIDEO_HZ_NTSC) / (m->ai.dacrate + 1u)); }

/** @brief One sample of the DAC: the next stereo word of the buffer playing, or the decaying level; moves to the second buffer when the first is spent. */
static void sample(rl_n64 *m)
{
    bool active = false;
    if (m->ai.count && m->ai.len[0] && m->ai.enable) {
        m->ai.addr[0] = (m->ai.addr[0] & 0x1FFFu) | (((m->ai.addr[0] >> 13) + m->ai.carry) & 0x7FFu) << 13;
        uint32_t a = m->ai.addr[0] & (RL_N64_RDRAM_SIZE - 4u);
        uint32_t w = rl_n64_rd32(m, a);
        m->ai.left = (int32_t)(int16_t)(w >> 16) * 256;
        m->ai.right = (int32_t)(int16_t)w * 256;
        uint32_t low = (m->ai.addr[0] + 4u) & 0x1FFFu;
        m->ai.addr[0] = (m->ai.addr[0] & ~0x1FFFu) | low;
        m->ai.carry = low == 0;
        m->ai.len[0] -= 4u;
        active = true;
    }
    if (m->ai.count && m->ai.len[0] == 0) {
        if (--m->ai.count) {
            m->ai.addr[0] = m->ai.addr[1];
            m->ai.len[0] = m->ai.len[1];
            rl_n64_mi_raise(m, RL_N64_IRQ_AI);
        }
    }
    if (!active) {
        m->ai.left = decay(m->ai.left, m->ai.decay);
        m->ai.right = decay(m->ai.right, m->ai.decay);
    }
    resample(m, m->ai.left, m->ai.right);
    m->ai.samples++;
}

static void catch_up(rl_n64 *m, uint64_t now)
{
    while (m->ai.next <= now * 2u) {
        sample(m);
        m->ai.next += m->ai.period;
    }
}

/** @brief Posts the event at the tick the current buffer is spent at, if it is going to be. */
static void reschedule(rl_n64 *m)
{
    rl_sched_cancel(m->s, &m->ai.done);
    if (!m->ai.count)
        return;
    uint64_t at;
    if (m->ai.len[0] == 0)
        at = m->ai.next;
    else if (m->ai.enable)
        at = m->ai.next + (uint64_t)(m->ai.len[0] / 4u - 1u) * m->ai.period;
    else
        return;
    rl_sched_post(m->s, &m->ai.done, (at + 1u) / 2u);
}

static void done_event(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    rl_n64 *m = e->opaque;
    catch_up(m, when);
    reschedule(m);
}

void rl_n64_ai_init(rl_n64 *m)
{
    m->ai.ring = calloc((size_t)RL_N64_AUDIO_RING * 2u, sizeof *m->ai.ring);
    m->ai.done.fire = done_event;
    m->ai.done.opaque = m;
    set_rate(m, 44100u);
}

void rl_n64_ai_attach(rl_n64 *m) { m->ai.next = rl_n64_now(m) * 2u; }

void rl_n64_ai_detach(rl_n64 *m) { rl_sched_cancel(m->s, &m->ai.done); }

uint32_t rl_n64_ai_peek(rl_n64 *m, unsigned i)
{
    if (i != 3u)
        return m->ai.len[0] & 0x3FFFFu;
    return (m->ai.count > 1u ? 1u : 0u) | 1u << 20 | 1u << 24 | (m->ai.enable ? 1u : 0u) << 25 |
           (m->ai.count > 0u ? 1u : 0u) << 30 | (m->ai.count > 1u ? 1u : 0u) << 31;
}

uint32_t rl_n64_ai_read(rl_n64 *m, unsigned i)
{
    catch_up(m, rl_n64_now(m));
    return rl_n64_ai_peek(m, i);
}

void rl_n64_ai_write(rl_n64 *m, unsigned i, uint32_t v)
{
    catch_up(m, rl_n64_now(m));
    switch (i) {
    case 0:
        if (m->ai.count < 2u)
            m->ai.addr[m->ai.count] = v & 0xFFFFF8u;
        break;
    case 1:
        if (m->ai.count < 2u) {
            if (m->ai.count == 0)
                rl_n64_mi_raise(m, RL_N64_IRQ_AI);
            m->ai.len[m->ai.count] = v & 0x3FFF8u;
            m->ai.count++;
        }
        break;
    case 2:
        m->ai.enable = v & 1u;
        break;
    case 3:
        rl_n64_mi_lower(m, RL_N64_IRQ_AI);
        break;
    case 4:
        m->ai.dacrate = v & 0x3FFFu;
        update_rate(m);
        break;
    case 5:
        m->ai.bitrate = v & 0xFu;
        break;
    default:
        break;
    }
    reschedule(m);
}

uint32_t rl_n64_ai_rate(const rl_n64 *m) { return m->ai.freq; }

size_t rl_n64_audio_take(rl_n64 *m, int16_t *pcm, size_t max_frames)
{
    catch_up(m, rl_n64_now(m));
    size_t n = ring_used(m);
    if (n > max_frames)
        n = max_frames;
    for (size_t i = 0; i < n; i++) {
        const int16_t *s = m->ai.ring + (size_t)(m->ai.head++ % RL_N64_AUDIO_RING) * 2u;
        pcm[i * 2u] = s[0];
        pcm[i * 2u + 1u] = s[1];
    }
    return n;
}
