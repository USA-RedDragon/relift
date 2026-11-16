/**
 * @file rtc.c
 * @brief The real-time clock behind 0x04000138: the 16-bit register of data,
 * clock and select lines with their directions, the 3-wire serial protocol
 * (an 8-bit command sent most significant bit first, parameter bytes least
 * significant bit first), the two status registers, date and time in BCD
 * with the 12 and 24 hour modes, the weekday counter, the alarm, frequency,
 * adjustment and free registers as stored values.
 *
 * The time is a count of seconds since 2000-01-01 at tick 0, set by
 * rl_nds_set_time (the program picks the host clock or a pinned time), and
 * runs with the emulated ticks. The /INT output (alarms, frequency
 * interrupts) is not modeled.
 *
 * Fresh C from GBATEK ("DS Real-Time Clock (RTC)"); the status register
 * values and the register's readback were compared with the oracle.
 */
#include "nds_int.h"

#include <string.h>

enum {
    BIT_DATA = 1,
    BIT_CLK = 2,
    BIT_CS = 4,
    BIT_OUT = 0x10,
};

static const unsigned days_in[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

static bool leap(unsigned y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

/** @brief Days from 2000-01-01 to the given date. */
static int64_t days_of(unsigned y, unsigned mo, unsigned d)
{
    int64_t days = 0;
    for (unsigned i = 2000; i < y; i++)
        days += leap(i) ? 366 : 365;
    for (unsigned i = 1; i < mo; i++)
        days += days_in[i - 1] + (i == 2 && leap(y) ? 1 : 0);
    return days + (d - 1);
}

static uint8_t bcd(unsigned v) { return (uint8_t)((v / 10) << 4 | (v % 10)); }
static unsigned unbcd(uint8_t v) { return (unsigned)(v >> 4) * 10 + (v & 15); }

void nds_rtc_reset(rl_nds *m)
{
    memset(&m->rtc, 0, sizeof m->rtc);
    m->rtc.stat1 = 0x82;
    m->rtc.reg = 0;
}

void rl_nds_set_time(rl_nds *m, const rl_nds_time *t)
{
    nds_rtc *r = &m->rtc;
    unsigned y = t->year < 2000 ? 2000 : t->year > 2099 ? 2099 : t->year;
    unsigned mo = t->month < 1 ? 1 : t->month > 12 ? 12 : t->month;
    unsigned d = t->day < 1 ? 1 : t->day > 31 ? 31 : t->day;
    int64_t day = days_of(y, mo, d);
    r->base = day * 86400 + (int64_t)(t->hour % 24) * 3600 + (int64_t)(t->minute % 60) * 60 + t->second % 60;
    r->dow_val = (uint8_t)((day + 5) % 7);
    r->dow_day = day;
}

/** @brief Date, weekday and time at tick now as the seven BCD bytes (year, month, day, weekday, hour, minute, second). */
static void clock_at(const nds_rtc *r, uint64_t now, uint8_t out[7])
{
    int64_t secs = r->base + (int64_t)(now / RL_NDS_TICK_HZ);
    int64_t day = secs / 86400, days = day;
    unsigned rem = (unsigned)(secs % 86400);
    unsigned y = 2000;
    while (days >= (leap(y) ? 366 : 365))
        days -= leap(y) ? 366 : 365, y++;
    unsigned mo = 1;
    while (days >= (int64_t)(days_in[mo - 1] + (mo == 2 && leap(y) ? 1 : 0)))
        days -= days_in[mo - 1] + (mo == 2 && leap(y) ? 1 : 0), mo++;
    unsigned h = rem / 3600;
    out[0] = bcd(y % 100);
    out[1] = bcd(mo);
    out[2] = bcd((unsigned)days + 1);
    out[3] = (uint8_t)((r->dow_val + (day - r->dow_day) % 7 + 7) % 7);
    out[4] = bcd((r->stat1 & 2) ? h : h % 12);
    if (h >= 12)
        out[4] |= 0x40;
    out[5] = bcd((rem / 60) % 60);
    out[6] = bcd(rem % 60);
}

/** @brief A new date and time written as n bytes of the seven (from the year, or from the hour for 3) at tick now. */
static void set_clock(rl_nds *m, uint64_t now, const uint8_t *b, unsigned n)
{
    nds_rtc *r = &m->rtc;
    uint8_t cur[7];
    clock_at(r, now, cur);
    memcpy(n == 3 ? cur + 4 : cur, b, n);
    unsigned h = unbcd(cur[4] & 0x3F);
    if (!(r->stat1 & 2) && (cur[4] & 0x40))
        h += 12;
    rl_nds_time t = {(uint16_t)(2000 + unbcd(cur[0])), (uint8_t)unbcd(cur[1] & 0x1F), (uint8_t)unbcd(cur[2] & 0x3F),
                     (uint8_t)h, (uint8_t)unbcd(cur[5] & 0x7F), (uint8_t)unbcd(cur[6] & 0x7F)};
    uint8_t dow = n == 7 ? (uint8_t)(cur[3] & 7u) % 7 : cur[3];
    rl_nds_set_time(m, &t);
    r->base -= (int64_t)(now / RL_NDS_TICK_HZ);
    r->dow_val = dow;
    r->dow_day = (r->base + (int64_t)(now / RL_NDS_TICK_HZ)) / 86400;
}

static unsigned param_len(unsigned cmd)
{
    static const unsigned len[8] = {1, 1, 7, 3, 3, 3, 1, 1};
    return len[cmd];
}

/** @brief Loads the bytes a read command returns. */
static void load_read(nds_rtc *r, unsigned cmd, uint64_t now)
{
    uint8_t t[7];
    switch (cmd) {
    case 0:
        r->buf[0] = r->stat1;
        r->stat1 &= (uint8_t)~0xF0;
        break;
    case 1:
        r->buf[0] = r->stat2;
        break;
    case 2:
        clock_at(r, now, r->buf);
        break;
    case 3:
        clock_at(r, now, t);
        memcpy(r->buf, t + 4, 3);
        break;
    case 4:
        if ((r->stat2 & 0x0F) == 4)
            memcpy(r->buf, r->alarm1, 3);
        else
            r->buf[0] = r->freq;
        break;
    case 5:
        memcpy(r->buf, r->alarm2, 3);
        break;
    case 6:
        r->buf[0] = r->adjust;
        break;
    default:
        r->buf[0] = r->free_reg;
        break;
    }
}

/** @brief Applies a finished write command. */
static void commit(rl_nds *m, unsigned cmd, uint64_t now)
{
    nds_rtc *r = &m->rtc;
    switch (cmd) {
    case 0:
        if (r->buf[0] & 1) {
            r->stat1 = 0;
            r->stat2 = 0;
            memset(r->alarm1, 0, sizeof r->alarm1);
            memset(r->alarm2, 0, sizeof r->alarm2);
            r->freq = r->adjust = r->free_reg = 0;
            static const uint8_t zero[7] = {0, 1, 1, 0, 0, 0, 0};
            set_clock(m, now, zero, 7);
        } else {
            r->stat1 = (uint8_t)((r->stat1 & 0xF0) | (r->buf[0] & 0x0E));
        }
        break;
    case 1:
        r->stat2 = r->buf[0];
        break;
    case 2:
        set_clock(m, now, r->buf, 7);
        break;
    case 3:
        set_clock(m, now, r->buf, 3);
        break;
    case 4:
        if ((r->stat2 & 0x0F) == 4)
            memcpy(r->alarm1, r->buf, 3);
        else
            r->freq = r->buf[0];
        break;
    case 5:
        memcpy(r->alarm2, r->buf, 3);
        break;
    case 6:
        r->adjust = r->buf[0];
        break;
    default:
        r->free_reg = r->buf[0];
        break;
    }
}

/** @brief One rising clock edge with the data line d. */
static void clock_rise(nds_rtc *r, bool d, uint64_t now)
{
    if (r->nbits < 8) {
        r->in = (uint8_t)(r->in << 1 | d);
        if (++r->nbits == 8) {
            if ((r->in >> 4) != 6) {
                r->nbits = 99;
                return;
            }
            r->cmd = (uint8_t)((r->in >> 1) & 7);
            r->reading = r->in & 1;
            r->len = param_len(r->cmd);
            r->byte = 0;
            r->bit = 0;
            memset(r->buf, 0, sizeof r->buf);
            if (r->reading)
                load_read(r, r->cmd, now);
        }
        return;
    }
    if (r->nbits == 99 || r->reading || r->byte >= r->len)
        return;
    if (d)
        r->buf[r->byte] |= (uint8_t)(1u << r->bit);
    if (++r->bit == 8) {
        r->bit = 0;
        r->byte++;
    }
}

static void clock_fall(nds_rtc *r)
{
    if (r->nbits < 8 || r->nbits == 99 || !r->reading || r->byte >= r->len) {
        r->out = true;
        return;
    }
    r->out = (r->buf[r->byte] >> r->bit) & 1;
    if (++r->bit == 8) {
        r->bit = 0;
        r->byte++;
    }
}

uint32_t nds_rtc_read(rl_nds *m, uint64_t now)
{
    (void)now;
    nds_rtc *r = &m->rtc;
    uint16_t v = r->reg;
    if (!(r->reg & BIT_OUT))
        v = (uint16_t)((v & ~BIT_DATA) | (r->open && r->reading && r->nbits >= 8 ? r->out : 1));
    return v;
}

void nds_rtc_write(rl_nds *m, uint32_t v, uint32_t mask, uint64_t now)
{
    nds_rtc *r = &m->rtc;
    uint16_t old = r->reg;
    uint16_t nv = (uint16_t)((old & ~mask) | (v & mask));
    r->reg = nv;
    if (!(mask & 0xFF))
        return;
    bool cs = nv & BIT_CS, ocs = old & BIT_CS;
    if (cs && !ocs) {
        r->open = true;
        r->nbits = 0;
        r->in = 0;
        r->reading = false;
        r->len = 0;
        r->out = true;
        return;
    }
    if (!cs && ocs) {
        if (r->open && r->nbits >= 8 && r->nbits != 99 && !r->reading && r->byte >= r->len)
            commit(m, r->cmd, now);
        r->open = false;
        return;
    }
    if (!cs || !r->open)
        return;
    bool clk = nv & BIT_CLK, oclk = old & BIT_CLK;
    bool d = (nv & BIT_OUT) ? (nv & BIT_DATA) : true;
    if (clk && !oclk)
        clock_rise(r, d, now);
    else if (!clk && oclk)
        clock_fall(r);
}
