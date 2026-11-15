/**
 * @file eeprom.c
 * @brief The serial EEPROM on the Game Pak bus: the bit stream of a request
 * (two command bits, 6 or 14 address bits, 64 data bits for a write, a
 * stop bit), the read reply (four dummy bits, 64 data bits, MSB first), the
 * busy time after a write during which a read returns 0, and the size
 * (512 bytes or 8 KiB) from a loaded file or the first DMA3 transfer length.
 */
#include "gba_int.h"

#include <string.h>

enum { S_CMD, S_ADDR, S_DATA, S_STOP_WRITE, S_STOP_READ, S_REPLY };
enum { WRITE_CYCLES = 101400, SMALL = 512, LARGE = 8192 };

bool gba_eeprom_hit(const rl_gba *g, uint32_t addr)
{
    if (g->save.kind != RL_GBA_SAVE_EEPROM || (addr >> 24) != 0xD)
        return false;
    return g->cart.rom_len <= 0x1000000u || (addr & 0x01FFFF00u) == 0x01FFFF00u;
}

void gba_eeprom_size(rl_gba *g, size_t len)
{
    gba_save *s = &g->save;
    s->len = len;
    s->ee.sized = true;
}

void gba_eeprom_hint(rl_gba *g, uint32_t dst, uint32_t count)
{
    gba_save *s = &g->save;
    if (s->kind != RL_GBA_SAVE_EEPROM || s->ee.sized || !gba_eeprom_hit(g, dst))
        return;
    if (count == 9 || count == 73)
        gba_eeprom_size(g, SMALL);
    else if (count == 17 || count == 81)
        gba_eeprom_size(g, LARGE);
}

static void sized(rl_gba *g)
{
    if (!g->save.ee.sized)
        gba_eeprom_size(g, LARGE);
}

static void restart(gba_eeprom *e)
{
    e->state = S_CMD;
    e->nbits = 0;
    e->shift = 0;
}

static void put_bit(rl_gba *g, unsigned bit)
{
    gba_save *s = &g->save;
    gba_eeprom *e = &s->ee;
    uint64_t now = gba_now(g);
    if (now < e->busy_until || e->state == S_REPLY)
        return;
    sized(g);
    unsigned addr_bits = s->len == SMALL ? 6 : 14;
    switch (e->state) {
    case S_CMD:
        e->shift = e->shift << 1 | bit;
        if (++e->nbits == 2) {
            unsigned c = (unsigned)e->shift & 3;
            e->nbits = 0;
            e->shift = 0;
            if (c >= 2) {
                e->cmd = (uint8_t)c;
                e->state = S_ADDR;
            }
        }
        break;
    case S_ADDR:
        e->shift = e->shift << 1 | bit;
        if (++e->nbits == addr_bits) {
            e->addr = (uint32_t)(e->shift * 8) & (uint32_t)(s->len - 1);
            e->nbits = 0;
            e->shift = 0;
            memset(e->block, 0, sizeof e->block);
            e->state = e->cmd == 2 ? S_DATA : S_STOP_READ;
        }
        break;
    case S_DATA:
        e->block[e->nbits / 8] = (uint8_t)(e->block[e->nbits / 8] | bit << (7 - e->nbits % 8));
        if (++e->nbits == 64)
            e->state = S_STOP_WRITE;
        break;
    case S_STOP_WRITE:
        memcpy(&s->data[e->addr], e->block, sizeof e->block);
        gba_save_touch(s, now);
        e->busy_until = now + WRITE_CYCLES;
        restart(e);
        break;
    default:
        e->state = S_REPLY;
        e->nread = 0;
        break;
    }
}

static unsigned get_bit(rl_gba *g)
{
    gba_save *s = &g->save;
    gba_eeprom *e = &s->ee;
    if (e->state != S_REPLY)
        return gba_now(g) < e->busy_until ? 0u : 1u;
    unsigned i = e->nread++;
    unsigned bit = 0;
    if (i >= 4)
        bit = (s->data[e->addr + (i - 4) / 8] >> (7 - (i - 4) % 8)) & 1u;
    if (e->nread == 68)
        restart(e);
    return bit;
}

uint32_t gba_eeprom_read(rl_gba *g, unsigned size)
{
    uint32_t lo = get_bit(g);
    if (size == 4)
        return lo | get_bit(g) << 16;
    return lo;
}

void gba_eeprom_write(rl_gba *g, unsigned size, uint32_t value)
{
    put_bit(g, value & 1u);
    if (size == 4)
        put_bit(g, (value >> 16) & 1u);
}
