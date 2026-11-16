/**
 * @file eeprom.c
 * @brief The cartridge's EEPROM on joybus channel 4: the status answer
 * (4 or 16 Kbit), block reads and writes of 8 bytes, and the 6 ms the chip
 * stays busy after a write. The cartridge's other joybus device, the
 * real-time clock, is not present: its commands are invalid.
 *
 * The command answers are ported from ares (n64/cartridge/joybus.cpp),
 * which carries this notice:
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

enum {
    WRITE_TICKS = 6 * 93750,
    SIZE_4K = 512,
};

static void write_done(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    (void)when;
    rl_n64 *m = e->opaque;
    m->cart.eeprom_busy = false;
}

void rl_n64_eeprom_attach(rl_n64 *m)
{
    m->cart.eeprom_done.fire = write_done;
    m->cart.eeprom_done.opaque = m;
}

unsigned rl_n64_cart_joybus(rl_n64 *m, unsigned send, unsigned recv, const uint8_t *input, uint8_t *output)
{
    bool valid = false;
    uint32_t mask = m->cart.eeprom_len - 1u;

    if (input[0] == 0x00 || input[0] == 0xFF) {
        output[0] = 0x00;
        output[1] = m->cart.eeprom_len == SIZE_4K ? 0x80 : 0xC0;
        output[2] = m->cart.eeprom_busy ? 0x80 : 0x00;
        valid = true;
    }

    if (input[0] == 0x04 && send >= 2) {
        uint32_t address = input[1] * 8u;
        for (unsigned i = 0; i < recv; i++)
            output[i] = !m->cart.eeprom_busy ? m->cart.eeprom[address++ & mask] : 0xFF;
        valid = true;
    }

    if (input[0] == 0x05 && send >= 2 && recv >= 1) {
        output[0] = m->cart.eeprom_busy ? 0x80 : 0x00;
        valid = true;
        if (!m->cart.eeprom_busy) {
            uint32_t address = input[1] * 8u;
            bool changed = false;
            for (unsigned i = 0; i < send - 2; i++, address++) {
                uint8_t *p = &m->cart.eeprom[address & mask];
                changed = changed || *p != input[2 + i];
                *p = input[2 + i];
            }
            if (changed)
                rl_n64_cart_touch(m, SAVE_EEPROM);
            m->cart.eeprom_busy = true;
            rl_sched_post(m->s, &m->cart.eeprom_done, rl_n64_now(m) + WRITE_TICKS);
        }
    }

    return valid ? 1u : 0u;
}
