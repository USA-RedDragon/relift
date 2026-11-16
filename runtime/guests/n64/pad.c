/**
 * @file pad.c
 * @brief The standard controller on a joybus port: its buttons and stick
 * (set by the caller or read from the p1 port of the native port table),
 * the status, state and Controller Pak commands as a console with no pak
 * inserted answers them.
 *
 * The command answers are ported from ares (n64/controller/gamepad), which
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

#include <relift/ports.h>

#include <string.h>

enum {
    STICK_MAX = 85,
    STICK_DEADZONE = 7,
};

static const struct {
    const char *name;
    uint16_t bit;
} keys[] = {
    {"a", RL_N64_PAD_A},          {"b", RL_N64_PAD_B},          {"z", RL_N64_PAD_Z},
    {"start", RL_N64_PAD_START},  {"up", RL_N64_PAD_UP},        {"down", RL_N64_PAD_DOWN},
    {"left", RL_N64_PAD_LEFT},    {"right", RL_N64_PAD_RIGHT},  {"l", RL_N64_PAD_L},
    {"r", RL_N64_PAD_R},          {"c_up", RL_N64_PAD_C_UP},    {"c_down", RL_N64_PAD_C_DOWN},
    {"c_left", RL_N64_PAD_C_LEFT}, {"c_right", RL_N64_PAD_C_RIGHT},
};

/** @brief The CRC the controller pak protocol appends to 32 data bytes. */
static uint8_t data_crc(const uint8_t *data)
{
    uint8_t crc = 0;
    for (unsigned i = 0; i < 33; i++) {
        for (int j = 7; j >= 0; j--) {
            uint8_t x = crc & 0x80 ? 0x85 : 0x00;
            crc = (uint8_t)(crc << 1);
            if (i < 32 && (data[i] & (1u << j)))
                crc |= 1;
            crc ^= x;
        }
    }
    return crc;
}

/** @brief A stick axis (-1 to 1) in joybus units: the dead zone cut out and the rest stretched back to the full range. */
static int8_t stick_units(float v)
{
    float m = (v < 0 ? -v : v) * STICK_MAX;
    if (m < STICK_DEADZONE)
        return 0;
    m = (m - STICK_DEADZONE) * STICK_MAX / (STICK_MAX - STICK_DEADZONE);
    if (m > STICK_MAX)
        m = STICK_MAX;
    int r = (int)(m + 0.5f);
    return (int8_t)(v < 0 ? -r : r);
}

void rl_n64_set_pad(rl_n64 *m, unsigned port, uint16_t buttons, int8_t x, int8_t y)
{
    if (!m || port >= 4)
        return;
    n64_pad *p = &m->pif.pad[port];
    if ((buttons & RL_N64_PAD_UP) && (buttons & RL_N64_PAD_DOWN))
        buttons &= (uint16_t)~(RL_N64_PAD_UP | RL_N64_PAD_DOWN);
    if ((buttons & RL_N64_PAD_LEFT) && (buttons & RL_N64_PAD_RIGHT))
        buttons &= (uint16_t)~(RL_N64_PAD_LEFT | RL_N64_PAD_RIGHT);
    if ((buttons & (RL_N64_PAD_L | RL_N64_PAD_R | RL_N64_PAD_START)) == (RL_N64_PAD_L | RL_N64_PAD_R | RL_N64_PAD_START)) {
        buttons = (uint16_t)((buttons & ~RL_N64_PAD_START) | RL_N64_PAD_RESET);
        x = 0;
        y = 0;
    }
    p->present = true;
    p->buttons = buttons;
    p->x = x;
    p->y = y;
}

void rl_n64_pad_remove(rl_n64 *m, unsigned port)
{
    if (m && port < 4)
        memset(&m->pif.pad[port], 0, sizeof m->pif.pad[port]);
}

/** @brief Reads the p1 port of the port table into controller 1. */
static void sample_ports(rl_n64 *m)
{
    uint16_t buttons = 0;
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        rl_control_ref r = rl_ports_find("p1", keys[i].name);
        if (r != RL_PORT_NONE && rl_ports_read(r) >= 0.5f)
            buttons |= keys[i].bit;
    }
    rl_control_ref rx = rl_ports_find("p1", "stick_x"), ry = rl_ports_find("p1", "stick_y");
    float x = rx == RL_PORT_NONE ? 0.0f : rl_ports_read(rx);
    float y = ry == RL_PORT_NONE ? 0.0f : rl_ports_read(ry);
    rl_n64_set_pad(m, 0, buttons, stick_units(x), stick_units(y));
}

void rl_n64_pad_use_ports(rl_n64 *m, bool on)
{
    if (!m)
        return;
    m->pif.pad_ports = on;
    if (on)
        sample_ports(m);
    else
        rl_n64_pad_remove(m, 0);
}

void rl_n64_pad_refresh(rl_n64 *m)
{
    if (m->pif.pad_ports)
        sample_ports(m);
}

unsigned rl_n64_pad_comm(rl_n64 *m, unsigned port, unsigned send, unsigned recv, const uint8_t *input, uint8_t *output)
{
    bool valid = false, over = false;

    if (input[0] == 0x00 || input[0] == 0xFF) {
        output[0] = 0x05;
        output[1] = 0x00;
        output[2] = 0x02;
        valid = true;
    }

    if (input[0] == 0x01) {
        const n64_pad *p = &m->pif.pad[port];
        output[0] = (uint8_t)(p->buttons >> 8);
        output[1] = (uint8_t)p->buttons;
        output[2] = (uint8_t)p->x;
        output[3] = (uint8_t)p->y;
        over = recv > 4;
        valid = true;
    }

    if (input[0] == 0x02 && send >= 3 && recv >= 1) {
        unsigned n = recv < 33 ? recv : 33;
        memset(output, 0, n);
        valid = true;
        if (recv >= 33)
            output[32] = data_crc(output) ^ 0xFF;
    }

    if (input[0] == 0x03 && send >= 4 && recv >= 1) {
        unsigned n = send - 3 < 32 ? send - 3 : 32;
        output[0] = n == 32 ? data_crc(&input[3]) : 0x00;
        output[0] ^= 0xFF;
        valid = true;
    }

    return (valid ? 1u : 0u) | (over ? 2u : 0u);
}
