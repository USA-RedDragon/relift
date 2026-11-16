/**
 * @file flash.c
 * @brief The cartridge's 128 KiB Flash chip: the command register at
 * 0x08010000 (two halfwords), the array, status and silicon ID reads, the
 * page buffer, sector and chip erase and page program with the time the
 * chip stays busy. The chip answers as a Macronix MX29L1101, which the
 * console's flash library accepts; nothing about the game is needed.
 *
 * The command set and its quirks (the doubled status command, the stale
 * first status read, the open bus past the status page) are ported from
 * ares (n64/cartridge/flash.cpp), which carries this notice:
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

#include <string.h>

enum {
    MODE_ARRAY,
    MODE_STATUS,
    MODE_ID,
    MODE_PAGE,
    ERASE_NONE = 0,
    ERASE_CHIP,
    ERASE_SECTOR,
    BUSY_NONE = 0,
    BUSY_ERASE,
    BUSY_PROGRAM,
    FLASH_SIZE = 0x20000,
    WRAP_MASK = 0x7FFF,
    ST_READY = 0x80,
    ST_OK = 0x0C,
    ST_BUSY = 0x03,
    ST_ERASE = 0x02,
    ST_PROGRAM = 0x01,
    MAKER = 0x00C2,
    DEVICE = 0x001D,
};

enum {
    TICKS_PER_MS = 93750,
    ERASE_TICKS = 85 * TICKS_PER_MS,
    PROGRAM_TICKS = 3500 * TICKS_PER_MS / 1000,
};

static uint16_t silicon_half(unsigned index)
{
    static const uint16_t id[4] = {0x1111, 0x8001, MAKER, DEVICE};
    return id[index & 3u];
}

void rl_n64_flash_power(rl_n64 *m)
{
    n64_flash *f = &m->cart.fl;
    if (m->s)
        rl_sched_cancel(m->s, &f->done);
    rl_event done = f->done;
    memset(f, 0, sizeof *f);
    f->done = done;
    f->mode = MODE_ARRAY;
    f->status = ST_READY | ST_OK;
    memset(f->page, 0xFF, sizeof f->page);
}

static void finished(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)when;
    rl_n64 *m = e->opaque;
    n64_flash *f = &m->cart.fl;
    f->status = (uint8_t)((f->status | ST_READY) & ~ST_BUSY);
    f->busy = BUSY_NONE;
}

void rl_n64_flash_attach(rl_n64 *m)
{
    m->cart.fl.done.fire = finished;
    m->cart.fl.done.opaque = m;
}

void rl_n64_flash_select(rl_n64 *m, uint32_t offset)
{
    n64_flash *f = &m->cart.fl;
    f->offset = offset;
    f->high_valid = false;
    f->burst = 0;
}

bool rl_n64_flash_read(rl_n64 *m, uint16_t *out)
{
    n64_flash *f = &m->cart.fl;
    if (f->open_bus)
        return false;

    if (f->mode == MODE_STATUS) {
        if (f->offset & 0x20000u) {
            f->open_bus = true;
            return false;
        }
        uint16_t data = f->status;
        if (f->stale) {
            f->stale = false;
            data = f->stale_value;
        } else {
            f->stale_value = data;
        }
        f->offset += 2;
        f->burst++;
        *out = data;
        return true;
    }

    if (f->mode == MODE_ID) {
        uint16_t data = silicon_half(f->burst);
        f->stale_value = data;
        f->offset += 2;
        f->burst++;
        *out = data;
        return true;
    }

    if (f->mode == MODE_PAGE) {
        uint32_t i = (f->offset & 0x7Fu) & ~1u;
        uint16_t data = (uint16_t)(f->page[i] << 8 | f->page[i + 1]);
        f->stale_value = data;
        f->offset += 2;
        *out = data;
        return true;
    }

    uint16_t data = 0;
    if (f->offset < FLASH_SIZE)
        data = (uint16_t)(m->cart.flash[f->offset] << 8 | m->cart.flash[f->offset + 1]);
    f->stale_value = data;
    f->offset = (f->offset & ~(uint32_t)WRAP_MASK) | ((f->offset + 2) & WRAP_MASK);
    *out = data;
    return true;
}

static void start(rl_n64 *m, uint8_t busy, uint8_t status, uint64_t ticks)
{
    n64_flash *f = &m->cart.fl;
    f->mode = MODE_STATUS;
    f->status = (uint8_t)((f->status & ~ST_READY) | status);
    f->stale = true;
    f->busy = busy;
    rl_sched_post(m->s, &f->done, rl_n64_now(m) + ticks);
    rl_n64_cart_touch(m, SAVE_FLASH);
}

static void command(rl_n64 *m, uint32_t data)
{
    n64_flash *f = &m->cart.fl;
    uint8_t cmd = (uint8_t)(data >> 24);
    f->open_bus = false;
    if (f->busy != BUSY_NONE)
        return;

    if (cmd == 0xD2) {
        if (f->pending_cmd != cmd) {
            f->pending_cmd = cmd;
            f->pending_count = 1;
            return;
        }
        f->pending_count++;
        if (f->pending_count < 2)
            return;
    }
    f->pending_cmd = 0;
    f->pending_count = 0;

    switch (cmd) {
    case 0x3C:
        f->erase_setup = ERASE_CHIP;
        return;
    case 0x4B:
        f->erase_setup = ERASE_SECTOR;
        f->erase_sector = (uint8_t)((data & 0x3FFu) >> 7);
        return;
    case 0x78:
        if (f->erase_setup == ERASE_NONE)
            return;
        if (f->erase_setup == ERASE_CHIP)
            memset(m->cart.flash, 0xFF, FLASH_SIZE);
        else
            memset(m->cart.flash + ((uint32_t)f->erase_sector << 14), 0xFF, 0x4000);
        f->erase_setup = ERASE_NONE;
        start(m, BUSY_ERASE, ST_ERASE, ERASE_TICKS);
        return;
    case 0xA5: {
        uint32_t base = (data & 0x3FFu) << 7;
        for (uint32_t i = 0; i < 128; i++)
            m->cart.flash[base + i] &= f->page[i];
        memset(f->page, 0xFF, sizeof f->page);
        start(m, BUSY_PROGRAM, ST_PROGRAM, PROGRAM_TICKS);
        return;
    }
    case 0xB4:
        f->mode = MODE_PAGE;
        return;
    case 0xD2:
        f->mode = MODE_STATUS;
        f->stale = true;
        return;
    case 0xE1:
        f->mode = MODE_ID;
        return;
    case 0xF0:
        f->mode = MODE_ARRAY;
        return;
    default:
        return;
    }
}

void rl_n64_flash_write(rl_n64 *m, uint16_t data)
{
    n64_flash *f = &m->cart.fl;
    uint32_t offset = f->offset & ~1u;

    if (offset == 0x10000u || offset == 0x10002u) {
        f->offset += 2;
        if (!f->high_valid) {
            f->high = data;
            f->high_valid = true;
            return;
        }
        f->high_valid = false;
        command(m, (uint32_t)f->high << 16 | data);
        return;
    }

    if (f->mode == MODE_STATUS) {
        if (offset == 0 && data == 0)
            f->status |= ST_OK;
        f->offset += 2;
        return;
    }

    if (f->mode == MODE_PAGE) {
        uint32_t i = offset & 0x7Fu;
        f->page[i] = (uint8_t)(data >> 8);
        f->page[i + 1] = (uint8_t)data;
        f->offset += 2;
        return;
    }

    f->offset += 2;
}
