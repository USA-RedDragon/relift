/**
 * @file sound.c
 * @brief The ARM7's sound controller: 16 channels (PCM8, PCM16 and
 * IMA-ADPCM from memory; square waves on channels 8 to 13 and noise on 14
 * and 15), their timers, start delays and repeat modes, volume, dividers and
 * panning, the mixer with SOUNDCNT's output selection and master volume,
 * SOUNDBIAS and the 10-bit clip, and the two capture units. The unit runs
 * behind the CPUs and catches up on each access to its registers, on each
 * take of its output, and every few milliseconds while a channel or capture
 * runs; it puts out one stereo sample every 1024 ARM7 cycles
 * (the mixer's 32.73 kHz) into a ring the host takes with
 * rl_nds_audio_take.
 *
 * Channel timers count at half the ARM7 clock (four ticks); a channel reads
 * its next sample from memory when its timer overflows, and the mixer takes
 * every channel's current sample at each output tick. Fresh C from GBATEK
 * ("DS Sound Channels 0..15", "DS Sound Control Registers", "DS Sound
 * Capture", "DS Sound Notes").
 */
#include "nds_int.h"

#include <string.h>

#define CH_CNT_MASK 0xFF7F837Fu
#define CNT_START 0x80000000u

enum {
    TIMER_TICKS = 4,
    CATCH_UP = 64,
    SAD_MASK = 0x07FFFFFC,
    LEN_MASK = 0x3FFFFF,
    CAP_CNT_MASK = 0x8F,
    CAP_DAD_MASK = 0x07FFFFFC,
    SOUNDCNT_MASK = 0xBF7F,
    BIAS_MASK = 0x3FF,
    CNT_HOLD = 1 << 15,
    CAP_ADD = 0x01,
    CAP_SRC = 0x02,
    CAP_ONESHOT = 0x04,
    CAP_PCM8 = 0x08,
    CAP_START = 0x80,
    FMT_PCM8 = 0,
    FMT_PCM16 = 1,
    FMT_ADPCM = 2,
    FMT_PSG = 3,
};

static const int8_t g_index_step[8] = {-1, -1, -1, -1, 2, 4, 6, 8};

static const uint16_t g_adpcm[89] = {
    0x0007, 0x0008, 0x0009, 0x000A, 0x000B, 0x000C, 0x000D, 0x000E, 0x0010, 0x0011, 0x0013, 0x0015, 0x0017, 0x0019,
    0x001C, 0x001F, 0x0022, 0x0025, 0x0029, 0x002D, 0x0032, 0x0037, 0x003C, 0x0042, 0x0049, 0x0050, 0x0058, 0x0061,
    0x006B, 0x0076, 0x0082, 0x008F, 0x009D, 0x00AD, 0x00BE, 0x00D1, 0x00E6, 0x00FD, 0x0117, 0x0133, 0x0151, 0x0173,
    0x0198, 0x01C1, 0x01EE, 0x0220, 0x0256, 0x0292, 0x02D4, 0x031C, 0x036C, 0x03C3, 0x0424, 0x048E, 0x0502, 0x0583,
    0x0610, 0x06AB, 0x0756, 0x0812, 0x08E0, 0x09C3, 0x0ABD, 0x0BD0, 0x0CFF, 0x0E4C, 0x0FBA, 0x114C, 0x1307, 0x14EE,
    0x1706, 0x1954, 0x1BDC, 0x1EA5, 0x21B6, 0x2515, 0x28CA, 0x2CDF, 0x315B, 0x364B, 0x3BB9, 0x41B2, 0x4844, 0x4F7E,
    0x5771, 0x602F, 0x69CE, 0x7462, 0x7FFF,
};

static uint32_t merge(uint32_t old, uint32_t v, uint32_t mask) { return (old & ~mask) | (v & mask); }

/** @brief A 7-bit volume or panning value as a factor of 128 (127 counts as 128). */
static int32_t factor(uint32_t v) { return v == 127 ? 128 : (int32_t)v; }

static unsigned format(const nds_snd_ch *c) { return (c->cnt >> 29) & 3; }

static uint64_t period(uint16_t tmr) { return (uint64_t)(0x10000u - tmr) * TIMER_TICKS; }

/** @brief A read by the sound unit's FIFO: memory only, no side effects (I/O and unmapped read zero). */
static uint32_t load(rl_nds *m, uint32_t addr, unsigned size)
{
    nds_where w = nds_decode_bus(m, RL_NDS_ARM7, addr & ~(uint32_t)(size - 1), false);
    return w.kind == NDS_K_MEM && w.p ? nds_ld(w.p, size) : 0;
}

/** @brief A write by a capture unit: memory only. */
static void store(rl_nds *m, uint32_t addr, uint32_t v)
{
    nds_where w = nds_decode_bus(m, RL_NDS_ARM7, addr & ~3u, true);
    if (w.kind == NDS_K_MEM && w.p) {
        nds_st(w.p, 4, v);
        nds_watch_store(m, w.p, 4);
    }
}

static void start(nds_snd_ch *c, unsigned n, uint64_t now)
{
    unsigned fmt = format(c);
    uint32_t words = (uint32_t)c->pnt + c->len;
    c->src = c->sad;
    c->last = false;
    c->loop_saved = false;
    c->index = 0xFF;
    c->lfsr = 0x7FFF;
    c->held = (c->cnt & CNT_HOLD) && c->sounding;
    if (!c->held)
        c->out = 0;
    switch (fmt) {
    case FMT_PCM8:
        c->total = words * 4;
        c->loop = (uint32_t)c->pnt * 4;
        break;
    case FMT_PCM16:
        c->total = words * 2;
        c->loop = (uint32_t)c->pnt * 2;
        break;
    case FMT_ADPCM:
        c->total = words ? (words - 1) * 8 : 0;
        c->loop = c->pnt ? ((uint32_t)c->pnt - 1) * 8 : 0;
        break;
    default:
        c->total = c->loop = 0;
        break;
    }
    bool silent = fmt == FMT_PSG ? n < 8 : words < 4;
    c->sounding = !silent || c->held;
    c->pos = fmt == FMT_PSG ? -1 : fmt == FMT_ADPCM ? -11 : -3;
    c->next = silent ? NDS_NEVER : now + period(c->tmr);
}

static int16_t adpcm_next(rl_nds *m, nds_snd_ch *c, uint32_t pos)
{
    if (c->index == 0xFF) {
        uint32_t h = load(m, c->src, 4);
        c->pcm = (int16_t)(h & 0xFFFF);
        c->index = (uint8_t)((h >> 16) & 0x7F);
        if (c->index > 88)
            c->index = 88;
    }
    if (pos == c->loop && !c->loop_saved) {
        c->loop_pcm = c->pcm;
        c->loop_index = c->index;
        c->loop_saved = true;
    }
    uint32_t b = load(m, c->src + 4 + pos / 2, 1);
    unsigned nib = (pos & 1) ? b >> 4 : b & 15;
    int32_t t = g_adpcm[c->index], diff = t / 8;
    if (nib & 1)
        diff += t / 4;
    if (nib & 2)
        diff += t / 2;
    if (nib & 4)
        diff += t;
    int32_t p = c->pcm;
    if (nib & 8)
        p = p - diff < -0x7FFF ? -0x7FFF : p - diff;
    else
        p = p + diff > 0x7FFF ? 0x7FFF : p + diff;
    c->pcm = (int16_t)p;
    int idx = (int)c->index + g_index_step[nib & 7];
    c->index = (uint8_t)(idx < 0 ? 0 : idx > 88 ? 88 : idx);
    return c->pcm;
}

static int16_t psg_next(nds_snd_ch *c, unsigned n)
{
    if (n >= 14) {
        bool carry = c->lfsr & 1;
        c->lfsr >>= 1;
        if (carry) {
            c->lfsr ^= 0x6000;
            return -0x7FFF;
        }
        return 0x7FFF;
    }
    unsigned duty = (c->cnt >> 24) & 7, step = (unsigned)c->pos & 7;
    return duty < 7 && step >= 7 - duty ? 0x7FFF : -0x7FFF;
}

/** @brief The channel's timer overflows at tick when: it plays its next sample. */
static void step(rl_nds *m, nds_snd_ch *c, unsigned n, uint64_t when)
{
    c->next = when + period(c->tmr);
    if (c->last) {
        c->last = false;
        c->next = NDS_NEVER;
        if (!(c->cnt & CNT_HOLD)) {
            c->sounding = false;
            c->out = 0;
        }
        return;
    }
    c->held = false;
    if (c->pos < 0) {
        c->out = 0;
        c->pos++;
        return;
    }
    unsigned fmt = format(c);
    if (fmt == FMT_PSG) {
        c->out = psg_next(c, n);
        c->pos = (c->pos + 1) & 7;
        return;
    }
    unsigned mode = (c->cnt >> 27) & 3;
    uint32_t pos = (uint32_t)c->pos;
    if (pos == c->total && mode == 1) {
        if (c->loop >= c->total) {
            c->next = NDS_NEVER;
            c->sounding = false;
            c->out = 0;
            return;
        }
        pos = c->loop;
        if (fmt == FMT_ADPCM && c->loop_saved) {
            c->pcm = c->loop_pcm;
            c->index = c->loop_index;
        }
    }
    switch (fmt) {
    case FMT_PCM8:
        c->out = (int16_t)(load(m, c->src + pos, 1) << 8);
        break;
    case FMT_PCM16:
        c->out = (int16_t)load(m, c->src + 2 * pos, 2);
        break;
    default:
        c->out = adpcm_next(m, c, pos);
        break;
    }
    if ((mode == 2 || mode == 3) && pos + 1 >= c->total) {
        c->cnt &= ~CNT_START;
        c->last = true;
    }
    c->pos = (int32_t)(pos + 1);
}

/** @brief A channel's sample after the divider and volume (16.11 bits), 0 when it puts out none. */
static int32_t volume(const nds_snd_ch *c)
{
    static const unsigned shift[4] = {0, 1, 2, 4};
    if (!c->sounding)
        return 0;
    int32_t v = (int32_t)c->out * 16;
    v >>= shift[(c->cnt >> 8) & 3];
    return v * factor(c->cnt & 0x7F);
}

/** @brief The mixer's view of the channels at one moment. */
typedef struct mix {
    /** @brief Each channel after volume (16.11). */
    int32_t v[16];
    /** @brief The left and right mixers (20.8). */
    int64_t l, r;
    /** @brief Channels 1 and 3 panned (16.8), for SOUNDCNT's output selection. */
    int64_t c1l, c1r, c3l, c3r;
} mix;

static void pan(int64_t v, uint32_t cnt, int64_t *l, int64_t *r)
{
    int32_t p = factor((cnt >> 16) & 0x7F);
    *l = (v * (128 - p)) >> 10;
    *r = (v * p) >> 10;
}

static bool adds(const nds_sound *s, unsigned x) { return (s->cap[x].cnt & (CAP_ADD | CAP_START)) == (CAP_ADD | CAP_START); }

static void mix_now(const nds_sound *s, mix *x)
{
    memset(x, 0, sizeof *x);
    for (unsigned n = 0; n < 16; n++)
        x->v[n] = volume(&s->ch[n]);
    bool to_mixer[2] = {!(s->soundcnt & 0x1000), !(s->soundcnt & 0x2000)};
    pan(x->v[1], s->ch[1].cnt, &x->c1l, &x->c1r);
    pan(x->v[3], s->ch[3].cnt, &x->c3l, &x->c3r);
    for (unsigned n = 0; n < 16; n++) {
        int64_t v = x->v[n];
        if (n == 1 || n == 3) {
            unsigned k = n >> 1;
            if (!to_mixer[k] || adds(s, k))
                continue;
        }
        if ((n == 0 || n == 2) && adds(s, n >> 1) && s->ch[n].sounding && to_mixer[n >> 1])
            v += x->v[n + 1];
        int64_t l, r;
        pan(v, s->ch[n].cnt, &l, &r);
        x->l += l;
        x->r += r;
    }
}

/** @brief One speaker's 10-bit level from the selected source (20.8 bits). */
static int32_t speaker(const nds_sound *s, int64_t v)
{
    int64_t o = 0;
    if (s->soundcnt & 0x8000)
        o = (v * factor(s->soundcnt & 0x7F)) >> 21;
    o += s->bias;
    return (int32_t)(o < 0 ? 0 : o > 0x3FF ? 0x3FF : o);
}

static int64_t select_out(const mix *x, unsigned sel, bool right)
{
    switch (sel) {
    case 1:
        return right ? x->c1r : x->c1l;
    case 2:
        return right ? x->c3r : x->c3l;
    case 3:
        return right ? x->c1r + x->c3r : x->c1l + x->c3l;
    default:
        return right ? x->r : x->l;
    }
}

/** @brief The host's 16-bit sample of a speaker level: the distance from the bias (the speakers block DC). */
static int16_t host_sample(const nds_sound *s, int32_t level)
{
    int32_t v = (level - (int32_t)s->bias) * 64;
    return (int16_t)(v < -0x8000 ? -0x8000 : v > 0x7FFF ? 0x7FFF : v);
}

static void output(rl_nds *m)
{
    nds_sound *s = &m->snd;
    mix x;
    mix_now(s, &x);
    int16_t l = 0, r = 0;
    if (m->powcnt2 & 1) {
        l = host_sample(s, speaker(s, select_out(&x, (s->soundcnt >> 8) & 3, false)));
        r = host_sample(s, speaker(s, select_out(&x, (s->soundcnt >> 10) & 3, true)));
    }
    s->made++;
    if (s->n == NDS_SND_RING) {
        s->dropped++;
        return;
    }
    uint32_t w = (s->rd + s->n) % NDS_SND_RING;
    s->ring[2 * w] = l;
    s->ring[2 * w + 1] = r;
    s->n++;
}

/** @brief An integer from fixed point with sh fraction bits: a set fraction MSB rounds toward zero, else down. */
static int32_t capture_int(int64_t v, unsigned sh)
{
    int64_t q = v >> sh;
    if (v < 0 && ((v >> (sh - 1)) & 1))
        q += 1;
    return (int32_t)q;
}

static uint32_t capture_sample(const nds_sound *s, unsigned k)
{
    const nds_snd_cap *cp = &s->cap[k];
    mix x;
    mix_now(s, &x);
    unsigned sh = (cp->cnt & CAP_PCM8) ? 16 : 8;
    int32_t q;
    if (cp->cnt & CAP_SRC) {
        int32_t a = x.v[2 * k], b = x.v[2 * k + 1];
        if (cp->cnt & CAP_ADD) {
            q = capture_int(((int64_t)a + b) >> 3, sh);
        } else if (a < 0 && b < 0) {
            q = (cp->cnt & CAP_PCM8) ? -0x80 : -0x8000;
        } else {
            q = capture_int((int64_t)a >> 3, sh);
        }
    } else {
        int64_t v = k ? x.r : x.l;
        v = v < -0x800000 ? -0x800000 : v > 0x7FFFFF ? 0x7FFFFF : v;
        q = capture_int(v, sh);
    }
    return (uint32_t)q & ((cp->cnt & CAP_PCM8) ? 0xFFu : 0xFFFFu);
}

static void capture_step(rl_nds *m, unsigned k, uint64_t when)
{
    nds_sound *s = &m->snd;
    nds_snd_cap *cp = &s->cap[k];
    cp->next = when + period(s->ch[2 * k + 1].tmr);
    bool pcm8 = cp->cnt & CAP_PCM8;
    cp->word |= capture_sample(s, k) << (pcm8 ? 8 * cp->fill : 16 * cp->fill);
    cp->fill++;
    if (cp->fill < (pcm8 ? 4 : 2))
        return;
    store(m, (cp->dad + cp->off) & CAP_DAD_MASK, cp->word);
    cp->word = 0;
    cp->fill = 0;
    cp->off += 4;
    if (cp->off < (cp->len ? cp->len : 1) * 4)
        return;
    cp->off = 0;
    if (cp->cnt & CAP_ONESHOT) {
        cp->cnt &= (uint8_t)~CAP_START;
        cp->next = NDS_NEVER;
    }
}

void nds_snd_run(rl_nds *m, uint64_t to)
{
    nds_sound *s = &m->snd;
    if (to <= s->at)
        return;
    for (;;) {
        uint64_t e = s->out_next;
        for (unsigned n = 0; n < 16; n++)
            if (s->ch[n].next < e)
                e = s->ch[n].next;
        for (unsigned k = 0; k < 2; k++)
            if (s->cap[k].next < e)
                e = s->cap[k].next;
        if (e > to)
            break;
        for (unsigned n = 0; n < 16; n++)
            if (s->ch[n].next == e)
                step(m, &s->ch[n], n, e);
        for (unsigned k = 0; k < 2; k++)
            if (s->cap[k].next == e)
                capture_step(m, k, e);
        if (s->out_next == e) {
            output(m);
            s->out_next += RL_NDS_AUDIO_TICKS;
        }
    }
    s->at = to;
}

/** @brief True while a channel or a capture unit runs, the time the catch-up event is queued. */
static bool busy(const nds_sound *s)
{
    for (unsigned n = 0; n < 16; n++)
        if (s->ch[n].next != NDS_NEVER)
            return true;
    return s->cap[0].next != NDS_NEVER || s->cap[1].next != NDS_NEVER;
}

static void fire(rl_sched *sch, rl_event *e, uint64_t when)
{
    rl_nds *m = e->opaque;
    nds_snd_run(m, when);
    if (busy(&m->snd))
        rl_sched_post(sch, e, when + (uint64_t)CATCH_UP * RL_NDS_AUDIO_TICKS);
}

/** @brief Queues the catch-up event once something runs, so captures and channel ends land on time without accesses. */
static void wake(rl_nds *m, uint64_t now)
{
    nds_sound *s = &m->snd;
    if (m->sched && s->ev.fire && !rl_event_queued(&s->ev) && busy(s))
        rl_sched_post(m->sched, &s->ev, now + (uint64_t)CATCH_UP * RL_NDS_AUDIO_TICKS);
}

void nds_snd_reset(rl_nds *m)
{
    nds_sound *s = &m->snd;
    memset(s, 0, sizeof *s);
    s->bias = 0x200;
    for (unsigned n = 0; n < 16; n++)
        s->ch[n].next = NDS_NEVER;
    s->cap[0].next = s->cap[1].next = NDS_NEVER;
    s->out_next = RL_NDS_AUDIO_TICKS;
}

void nds_snd_attach(rl_nds *m)
{
    nds_sound *s = &m->snd;
    uint64_t now = nds_now(m, RL_NDS_ARM7);
    s->at = now;
    s->out_next = now + RL_NDS_AUDIO_TICKS;
    s->ev = (rl_event){.fire = fire, .opaque = m};
}

uint32_t nds_snd_read(rl_nds *m, uint32_t off, uint64_t now)
{
    nds_sound *s = &m->snd;
    nds_snd_run(m, now);
    if (off < 0x500)
        return (off & 0xF) == 0 ? s->ch[(off - 0x400) >> 4].cnt : 0;
    switch (off) {
    case 0x500:
        return s->soundcnt;
    case 0x504:
        return s->bias;
    case 0x508:
        return (uint32_t)s->cap[0].cnt | (uint32_t)s->cap[1].cnt << 8;
    case 0x510:
        return s->cap[0].dad;
    case 0x518:
        return s->cap[1].dad;
    default:
        return 0;
    }
}

static void write_cnt(nds_snd_ch *c, unsigned n, uint32_t v, uint32_t mask, uint64_t now)
{
    uint32_t old = c->cnt;
    c->cnt = merge(c->cnt, v, mask) & CH_CNT_MASK;
    bool was = old & CNT_START, on = c->cnt & CNT_START;
    if (on && !was) {
        start(c, n, now);
    } else if (!on && was) {
        c->next = NDS_NEVER;
        c->sounding = false;
        c->last = false;
        c->out = 0;
    }
}

static void write_capcnt(nds_snd_cap *cp, uint8_t v, uint64_t now, uint16_t tmr)
{
    bool was = cp->cnt & CAP_START;
    cp->cnt = (uint8_t)(v & CAP_CNT_MASK);
    if ((cp->cnt & CAP_START) && !was) {
        cp->off = 0;
        cp->word = 0;
        cp->fill = 0;
        cp->next = now + period(tmr);
    } else if (!(cp->cnt & CAP_START)) {
        cp->next = NDS_NEVER;
    }
}

void nds_snd_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask, uint64_t now)
{
    nds_sound *s = &m->snd;
    nds_snd_run(m, now);
    if (off < 0x500) {
        unsigned n = (off - 0x400) >> 4;
        nds_snd_ch *c = &s->ch[n];
        switch (off & 0xF) {
        case 0:
            write_cnt(c, n, v, mask, now);
            break;
        case 4:
            c->sad = merge(c->sad, v, mask) & SAD_MASK;
            break;
        case 8:
            c->tmr = (uint16_t)merge(c->tmr, v, mask);
            c->pnt = (uint16_t)merge(c->pnt, v >> 16, mask >> 16);
            break;
        default:
            c->len = merge(c->len, v, mask) & LEN_MASK;
            break;
        }
        wake(m, now);
        return;
    }
    switch (off) {
    case 0x500:
        s->soundcnt = (uint16_t)(merge(s->soundcnt, v, mask) & SOUNDCNT_MASK);
        break;
    case 0x504:
        s->bias = (uint16_t)(merge(s->bias, v, mask) & BIAS_MASK);
        break;
    case 0x508:
        for (unsigned k = 0; k < 2; k++)
            if ((mask >> (8 * k)) & 0xFF)
                write_capcnt(&s->cap[k], (uint8_t)(v >> (8 * k)), now, s->ch[2 * k + 1].tmr);
        break;
    case 0x510:
    case 0x518:
        s->cap[(off - 0x510) >> 3].dad = merge(s->cap[(off - 0x510) >> 3].dad, v, mask) & CAP_DAD_MASK;
        break;
    case 0x514:
    case 0x51C:
        s->cap[(off - 0x514) >> 3].len = merge(s->cap[(off - 0x514) >> 3].len, v, mask) & 0xFFFF;
        break;
    default:
        break;
    }
    wake(m, now);
}

uint32_t rl_nds_audio_take(rl_nds *m, int16_t *pcm, uint32_t max)
{
    nds_sound *s = &m->snd;
    if (m->sched)
        nds_snd_run(m, nds_now(m, RL_NDS_ARM7));
    uint32_t n = s->n < max ? s->n : max;
    for (uint32_t i = 0; i < n; i++) {
        uint32_t r = (s->rd + i) % NDS_SND_RING;
        pcm[2 * i] = s->ring[2 * r];
        pcm[2 * i + 1] = s->ring[2 * r + 1];
    }
    s->rd = (s->rd + n) % NDS_SND_RING;
    s->n -= n;
    return n;
}

uint64_t rl_nds_audio_dropped(const rl_nds *m) { return m->snd.dropped; }
