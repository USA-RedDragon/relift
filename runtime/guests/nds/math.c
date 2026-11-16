/**
 * @file math.c
 * @brief The ARM9's math unit: DIVCNT with the 32/32, 64/32 and 64/64
 * modes, the division by zero and -MAX/-1 results and the busy time, and
 * SQRTCNT with 32-bit and 64-bit inputs. A write to a control or parameter
 * register starts the unit; the result registers hold the new result at
 * once and the busy bit stays set for the unit's time (18 or 34 bus cycles
 * for a division, 13 for a square root).
 *
 * Fresh C from GBATEK ("DS Maths").
 */
#include "nds_int.h"

#include <string.h>

enum {
    DIV_BUSY_32 = 18,
    DIV_BUSY_64 = 34,
    SQRT_BUSY = 13,
};

void nds_math_reset(rl_nds *m) { memset(&m->math, 0, sizeof m->math); }

static void divide(nds_math *u, uint64_t now)
{
    unsigned mode = u->divcnt & 3;
    u->divcnt = (uint16_t)((u->divcnt & 3) | (u->denom == 0 ? 0x4000 : 0));
    u->div_done = now + 2 * (mode == 0 ? DIV_BUSY_32 : DIV_BUSY_64);
    int64_t n, d;
    if (mode == 0) {
        n = (int32_t)(uint32_t)u->numer;
        d = (int32_t)(uint32_t)u->denom;
    } else {
        n = (int64_t)u->numer;
        d = mode == 2 ? (int64_t)u->denom : (int32_t)(uint32_t)u->denom;
    }
    uint64_t q, r;
    bool over = false;
    if (d == 0) {
        q = n < 0 ? 1 : UINT64_MAX;
        r = (uint64_t)n;
        over = true;
    } else if (d == -1 && n == (mode == 0 ? INT32_MIN : INT64_MIN)) {
        q = (uint64_t)n;
        r = 0;
        over = true;
    } else {
        q = (uint64_t)(n / d);
        r = (uint64_t)(n % d);
    }
    if (mode == 0 && over)
        q ^= 0xFFFFFFFF00000000ull;
    u->result = q;
    u->rem = r;
}

static uint32_t isqrt(uint64_t x)
{
    uint64_t r = 0, bit = 1ull << 62;
    while (bit > x)
        bit >>= 2;
    while (bit) {
        if (x >= r + bit) {
            x -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return (uint32_t)r;
}

static void square_root(nds_math *u, uint64_t now)
{
    u->sqrt_done = now + 2 * SQRT_BUSY;
    u->sqrt_result = isqrt(u->sqrtcnt & 1 ? u->param : (uint32_t)u->param);
}

static void put64(uint64_t *x, bool high, uint32_t v, uint32_t mask)
{
    unsigned sh = high ? 32 : 0;
    uint64_t m64 = (uint64_t)mask << sh;
    *x = (*x & ~m64) | ((uint64_t)v << sh & m64);
}

uint32_t nds_math_read(rl_nds *m, uint32_t off, uint64_t now)
{
    const nds_math *u = &m->math;
    switch (off) {
    case 0x280:
        return u->divcnt | (now < u->div_done ? 0x8000u : 0);
    case 0x290:
    case 0x294:
        return (uint32_t)(u->numer >> (off & 4 ? 32 : 0));
    case 0x298:
    case 0x29C:
        return (uint32_t)(u->denom >> (off & 4 ? 32 : 0));
    case 0x2A0:
    case 0x2A4:
        return (uint32_t)(u->result >> (off & 4 ? 32 : 0));
    case 0x2A8:
    case 0x2AC:
        return (uint32_t)(u->rem >> (off & 4 ? 32 : 0));
    case 0x2B0:
        return u->sqrtcnt | (now < u->sqrt_done ? 0x8000u : 0);
    case 0x2B4:
        return u->sqrt_result;
    case 0x2B8:
    case 0x2BC:
        return (uint32_t)(u->param >> (off & 4 ? 32 : 0));
    default:
        return 0;
    }
}

void nds_math_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask, uint64_t now)
{
    nds_math *u = &m->math;
    switch (off) {
    case 0x280:
        if (!(mask & 3))
            return;
        u->divcnt = (uint16_t)((u->divcnt & ~3u) | (v & 3));
        divide(u, now);
        return;
    case 0x290:
    case 0x294:
        put64(&u->numer, off & 4, v, mask);
        divide(u, now);
        return;
    case 0x298:
    case 0x29C:
        put64(&u->denom, off & 4, v, mask);
        divide(u, now);
        return;
    case 0x2B0:
        if (!(mask & 1))
            return;
        u->sqrtcnt = (uint16_t)(v & 1);
        square_root(u, now);
        return;
    case 0x2B8:
    case 0x2BC:
        put64(&u->param, off & 4, v, mask);
        square_root(u, now);
        return;
    default:
        return;
    }
}
