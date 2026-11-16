/**
 * @file pif.c
 * @brief The PIF after the high-level boot: its RAM, the joybus command
 * block the CPU leaves there and how the console runs it (the channel
 * parse, the skip, reset and padding codes, the commands run when the
 * block is read back by DMA), and the time the block takes on the wire.
 * The boot handshake with the cartridge's boot chip is not modeled: the
 * high-level boot ends it, and PIF ROM reads are 0 as after the boot's
 * lockout.
 *
 * The block parse, the run and the timing estimate are ported from ares
 * (n64/pif/hle.cpp, io.cpp and pif.cpp), which carries this notice:
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
    CMD_BYTE = 0x3F,
    CMD_JOY = 0x01,
    CMD_CHALLENGE = 0x02,
    CHANNELS = 5,
};

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void rl_n64_pif_init(rl_n64 *m)
{
    for (unsigned i = 0; i < CHANNELS; i++) {
        m->pif.joy_skip[i] = true;
        m->pif.joy_reset[i] = false;
    }
}

/** @brief Walks the command block and records where each channel's command is. */
static void joy_parse(rl_n64 *m)
{
    const uint8_t *ram = m->pif_ram;
    uint32_t offset = 0;
    unsigned channel = 0;
    while (channel < CHANNELS && offset < 64) {
        uint8_t send = ram[offset++];
        if (send == 0xFE)
            break;
        if (send == 0xFF)
            continue;
        if (send == 0x00) {
            channel++;
            continue;
        }
        if (send == 0xFD) {
            m->pif.joy_reset[channel++] = true;
            continue;
        }
        uint32_t at = offset - 1;
        uint8_t recv = offset < 64 ? ram[offset] : 0;
        offset++;
        send &= 0x3F;
        recv &= 0x3F;
        offset += (uint32_t)send + recv;
        if (offset < 64) {
            m->pif.joy_at[channel] = (uint8_t)at;
            m->pif.joy_skip[channel] = false;
            channel++;
        }
    }
}

/** @brief Runs the commands of the block, channel 4 (the cartridge) first and channel 0 last. */
static void joy_run(rl_n64 *m)
{
    uint8_t *ram = m->pif_ram;
    rl_n64_pad_refresh(m);
    for (int channel = CHANNELS - 1; channel >= 0; channel--) {
        if (m->pif.joy_reset[channel])
            continue;
        if (m->pif.joy_skip[channel])
            continue;
        uint32_t offset = m->pif.joy_at[channel];
        uint8_t send = ram[offset++];
        if (send & 0x80)
            continue;
        if (send & 0x40)
            continue;
        uint32_t recv_at = offset;
        uint8_t recv = ram[offset] & 0x3F;
        send &= 0x3F;
        ram[offset++] = recv;

        uint8_t input[64] = {0}, output[64] = {0};
        for (unsigned i = 0; i < send; i++)
            input[i] = ram[(offset++) & 63u];

        unsigned status = 0;
        if (channel < 4 && m->pif.pad[channel].present)
            status = rl_n64_pad_comm(m, (unsigned)channel, send, recv, input, output);
        else if (channel == 4)
            status = rl_n64_cart_joybus(m, send, recv, input, output);
        if (!(status & 1))
            ram[recv_at] = (uint8_t)(0x80 | recv);
        if (status & 2)
            ram[recv_at] = (uint8_t)(0x40 | recv);
        if (status & 1)
            for (unsigned i = 0; i < recv; i++)
                ram[(offset++) & 63u] = output[i];
    }
}

uint32_t rl_n64_pif_estimate(const rl_n64 *m)
{
    const uint8_t *ram = m->pif_ram;
    uint32_t cycles = 13600, short_cmds = 0, offset = 0;
    unsigned channel = 0;
    while (offset < 64 && channel < CHANNELS) {
        uint8_t send = ram[offset++];
        if (send == 0xFE) {
            short_cmds++;
            break;
        }
        if (send == 0x00 || send == 0xFD) {
            short_cmds++;
            channel++;
            continue;
        }
        if (send == 0xFF) {
            short_cmds++;
            continue;
        }
        uint8_t recv = offset < 64 ? ram[offset] : 0;
        offset++;
        send &= 0x3F;
        recv &= 0x3F;
        offset += (uint32_t)send + recv;
        if (channel < 4)
            cycles += m->pif.pad[channel].present ? 22000 : 18000;
        else
            cycles += 20000;
        channel++;
    }
    return cycles + 1420 * short_cmds;
}

/** @brief The CPU wrote PIF RAM (a word or a block): a command block flagged for the joybus is parsed. */
static void written(rl_n64 *m)
{
    if (m->pif_ram[CMD_BYTE] & CMD_JOY) {
        m->pif_ram[CMD_BYTE] &= (uint8_t)~CMD_JOY;
        rl_n64_pif_init(m);
        joy_parse(m);
    }
}

uint32_t rl_n64_pif_read(rl_n64 *m, uint32_t a)
{
    a &= 0x7FFu;
    return a < 0x7C0u ? 0 : ld32(m->pif_ram + (a - 0x7C0u));
}

void rl_n64_pif_write(rl_n64 *m, uint32_t a, uint32_t v)
{
    a &= 0x7FFu;
    if (a < 0x7C0u)
        return;
    st32(m->pif_ram + (a - 0x7C0u), v);
    written(m);
}

void rl_n64_pif_dma_read(rl_n64 *m, uint32_t a, uint32_t dram)
{
    if (!(m->pif_ram[CMD_BYTE] & CMD_CHALLENGE))
        joy_run(m);
    for (uint32_t i = 0; i < 64; i += 4)
        st32(m->rdram + ((dram + i) & (RL_N64_RDRAM_SIZE - 1u)), rl_n64_pif_read(m, a + i));
    rl_n64_code_written(m, dram & (RL_N64_RDRAM_SIZE - 1u), 64);
}

void rl_n64_pif_dma_write(rl_n64 *m, uint32_t a, uint32_t dram)
{
    for (uint32_t i = 0; i < 64; i += 4) {
        uint32_t at = (a + i) & 0x7FFu;
        if (at >= 0x7C0u)
            st32(m->pif_ram + (at - 0x7C0u), ld32(m->rdram + ((dram + i) & (RL_N64_RDRAM_SIZE - 1u))));
    }
    written(m);
}
