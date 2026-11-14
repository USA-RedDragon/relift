/**
 * @file rtc.c
 * @brief The MBC3 real-time clock: seconds, minutes, hours and a 9-bit day
 * counter with a halt bit and a sticky carry, advanced from host time.
 */
#include "gb_int.h"

#include <time.h>

#define DAY 86400ull
#define DAYS 512ull

static uint64_t system_time(void *ctx)
{
    (void)ctx;
    return (uint64_t)time(NULL);
}

uint64_t gb_host_time(const rl_gb *g) { return g->time_fn ? g->time_fn(g->time_ctx) : system_time(NULL); }

void rl_gb_set_time_source(rl_gb *g, rl_gb_time_fn *fn, void *ctx)
{
    gb_rtc_update(g);
    g->time_fn = fn;
    g->time_ctx = ctx;
    g->rtc.stamp = gb_host_time(g);
}

void gb_rtc_update(rl_gb *g)
{
    if (!g->cart.rtc)
        return;
    uint64_t now = gb_host_time(g);
    if (!g->rtc.halt && now > g->rtc.stamp)
        g->rtc.seconds += now - g->rtc.stamp;
    g->rtc.stamp = now;
    if (g->rtc.seconds >= DAYS * DAY) {
        g->rtc.carry = true;
        g->rtc.seconds %= DAYS * DAY;
    }
}

uint8_t gb_rtc_peek(const rl_gb *g, unsigned reg)
{
    uint64_t s = g->rtc.seconds;
    uint64_t days = s / DAY;
    switch (reg) {
    case 0:
        return (uint8_t)(s % 60);
    case 1:
        return (uint8_t)(s / 60 % 60);
    case 2:
        return (uint8_t)(s / 3600 % 24);
    case 3:
        return (uint8_t)(days & 0xFF);
    default:
        return (uint8_t)(((days >> 8) & 1u) | (g->rtc.halt ? 0x40u : 0u) | (g->rtc.carry ? 0x80u : 0u));
    }
}

void gb_rtc_write(rl_gb *g, unsigned reg, uint8_t value)
{
    gb_rtc_update(g);
    uint64_t s = g->rtc.seconds;
    uint64_t sec = s % 60, min = s / 60 % 60, hour = s / 3600 % 24, days = s / DAY;
    switch (reg) {
    case 0:
        sec = value & 0x3Fu;
        break;
    case 1:
        min = value & 0x3Fu;
        break;
    case 2:
        hour = value & 0x1Fu;
        break;
    case 3:
        days = (days & 0x100u) | value;
        break;
    default:
        days = (days & 0xFFu) | ((uint64_t)(value & 1u) << 8);
        g->rtc.halt = (value & 0x40u) != 0;
        g->rtc.carry = (value & 0x80u) != 0;
        break;
    }
    g->rtc.seconds = (days * 24 + hour) * 3600 + min * 60 + sec;
}
