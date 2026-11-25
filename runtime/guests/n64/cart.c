/**
 * @file cart.c
 * @brief The cartridge bus as the PI sees it: the ROM, and in domain 2
 * the save memory the game uses. The type is never looked up: SRAM answers
 * at 0x08000000 until the game writes the Flash chip's command register
 * (0x08010000), which makes the chip answer there from then on; EEPROM
 * answers on joybus channel 4 (eeprom.c). The device is selected by the
 * address, reads and writes move a halfword at a time, and an access to
 * nothing gives the bus latch (the address).
 *
 * The device selection, the latch and the memory views are ported from
 * ares (n64/pi/bus.hpp, device.hpp and n64/cartridge/cartridge.hpp), which
 * carries this notice:
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
    DOMAIN2 = 0x08000000u,
    DOMAIN2_END = 0x0FFFFFFFu,
    ROM_BASE = 0x10000000u,
    SRAM_SIZE = 0x8000,
    WRITE_TICKS = 200,
    FLASH_COMMAND = 0x10000u,
};

void rl_n64_cart_init(rl_n64 *m)
{
    memset(m->cart.sram, 0, sizeof m->cart.sram);
    memset(m->cart.flash, 0xFF, sizeof m->cart.flash);
    memset(m->cart.eeprom, 0xFF, sizeof m->cart.eeprom);
    m->cart.eeprom_len = sizeof m->cart.eeprom;
    rl_n64_flash_power(m);
}

static void bus_write_done(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)when;
    rl_n64 *m = e->opaque;
    m->pi.io_busy = false;
}

void rl_n64_cart_attach(rl_n64 *m)
{
    m->pi.bus_write.fire = bus_write_done;
    m->pi.bus_write.opaque = m;
    rl_n64_flash_attach(m);
    rl_n64_eeprom_attach(m);
}

void rl_n64_cart_detach(rl_n64 *m)
{
    if (!m->s)
        return;
    rl_sched_cancel(m->s, &m->pi.bus_write);
    rl_sched_cancel(m->s, &m->cart.fl.done);
    rl_sched_cancel(m->s, &m->cart.eeprom_done);
}

void rl_n64_cart_touch(rl_n64 *m, uint8_t stores)
{
    m->cart.dirty |= stores;
    m->cart.changed = rl_n64_now(m);
}

void rl_n64_bus_select(rl_n64 *m, uint32_t a)
{
    a &= ~1u;
    m->pi.latch = (uint32_t)(a & 0xFFFFu) << 16 | (a & 0xFFFFu);
    m->cart.dev = BUS_NONE;
    if (a >= ROM_BASE && a - ROM_BASE < m->rom_len) {
        m->cart.dev = BUS_ROM;
        m->cart.view = a - ROM_BASE;
    } else if (a >= DOMAIN2 && a <= DOMAIN2_END) {
        m->cart.raw = a - DOMAIN2;
        if (m->cart.flash_armed) {
            m->cart.dev = BUS_FLASH;
            rl_n64_flash_select(m, m->cart.raw);
        } else {
            m->cart.dev = BUS_SRAM;
            m->cart.view = m->cart.raw & (SRAM_SIZE - 1u);
        }
    }
}

/** @brief Makes data the bus latch's halfword: a device that gives data sets the latch. */
static uint16_t give(rl_n64 *m, uint16_t data)
{
    m->pi.latch = (uint32_t)data << 16 | data;
    return data;
}

uint16_t rl_n64_bus_read_half(rl_n64 *m)
{
    uint16_t data;
    if (m->cart.dev == BUS_ROM) {
        uint32_t o = m->cart.view;
        if (o < m->rom_len) {
            m->cart.view += 2;
            return give(m, (uint16_t)(m->rom[o] << 8 | (o + 1 < m->rom_len ? m->rom[o + 1] : 0)));
        }
    } else if (m->cart.dev == BUS_SRAM) {
        uint32_t o = m->cart.view;
        if (o < SRAM_SIZE) {
            m->cart.view += 2;
            m->cart.raw += 2;
            return give(m, (uint16_t)(m->cart.sram[o] << 8 | m->cart.sram[o + 1]));
        }
    } else if (m->cart.dev == BUS_FLASH && rl_n64_flash_read(m, &data)) {
        return give(m, data);
    }
    return (uint16_t)m->pi.latch;
}

void rl_n64_bus_write_half(rl_n64 *m, uint16_t v)
{
    if (!m->pi.io_busy)
        m->pi.latch = (uint32_t)v << 16 | v;
    if (m->cart.dev == BUS_SRAM) {
        if ((m->cart.raw & ~1u) == FLASH_COMMAND || (m->cart.raw & ~1u) == FLASH_COMMAND + 2) {
            m->cart.flash_armed = true;
            m->cart.dev = BUS_FLASH;
            rl_n64_flash_select(m, m->cart.raw);
        }
    }
    if (m->cart.dev == BUS_FLASH) {
        rl_n64_flash_write(m, v);
    } else if (m->cart.dev == BUS_SRAM) {
        uint32_t o = m->cart.view;
        if (o < SRAM_SIZE) {
            if (m->cart.sram[o] != (uint8_t)(v >> 8) || m->cart.sram[o + 1] != (uint8_t)v) {
                m->cart.sram[o] = (uint8_t)(v >> 8);
                m->cart.sram[o + 1] = (uint8_t)v;
                rl_n64_cart_touch(m, SAVE_SRAM);
            }
            m->cart.view += 2;
        }
        m->cart.raw += 2;
    } else if (m->cart.dev == BUS_ROM) {
        m->cart.view += 2;
    }
}

bool rl_n64_cart_peek(const rl_n64 *m, uint32_t paddr, uint8_t *out)
{
    if (paddr < ROM_BASE || paddr - ROM_BASE >= m->rom_len)
        return false;
    *out = m->rom[paddr - ROM_BASE];
    return true;
}

uint32_t rl_n64_cart_read(rl_n64 *m, uint32_t paddr)
{
    if (m->pi.io_busy) {
        rl_sched_cancel(m->s, &m->pi.bus_write);
        m->pi.io_busy = false;
        return m->pi.latch;
    }
    rl_n64_bus_select(m, paddr);
    uint32_t hi = rl_n64_bus_read_half(m);
    m->pi.latch = hi << 16 | rl_n64_bus_read_half(m);
    m->pi.cart = (paddr + 4u) & ~1u;
    return m->pi.latch;
}

void rl_n64_cart_write(rl_n64 *m, uint32_t paddr, uint32_t v)
{
    if (m->pi.io_busy)
        return;
    m->pi.io_busy = true;
    m->pi.cart = (paddr + 4u) & ~1u;
    rl_sched_post(m->s, &m->pi.bus_write, rl_n64_now(m) + WRITE_TICKS);
    rl_n64_bus_select(m, paddr);
    m->pi.latch = v;
    rl_n64_bus_write_half(m, (uint16_t)(v >> 16));
    rl_n64_bus_write_half(m, (uint16_t)v);
}
