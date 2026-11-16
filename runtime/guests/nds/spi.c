/**
 * @file spi.c
 * @brief The ARM7's SPI bus: SPICNT and SPIDATA (baud rates, busy time,
 * chip select hold, the transfer-complete IRQ) and its three devices: the
 * power management device (registers 0 to 4), the firmware flash (read,
 * fast read, JEDEC ID, status, write enable and the page and sector
 * commands, over the generated image of firmware.c) and the touch screen
 * controller (12-bit and 8-bit conversions of the pen, with the next
 * command allowed inside the reply, from the host's touch input).
 *
 * Fresh C from GBATEK ("DS Serial Peripheral Interface Bus (SPI)", "DS
 * Power Management Device", "DS Touch Screen Controller (TSC)", "DS Firmware
 * Serial Flash Memory"); register readbacks, the byte times and the touch
 * values were compared with the oracle (tests/test_spi.c).
 */
#include "nds_int.h"

#include <stdlib.h>
#include <string.h>

enum {
    CNT_MASK = 0xCF03,
    CNT_ENABLE = 0x8000,
    CNT_IRQ = 0x4000,
    CNT_HOLD = 0x0800,
    FLASH_PAGE = 0x100,
    FLASH_SECTOR = 0x10000,
};

static const uint32_t byte_bus[4] = {67, 134, 268, 536};

void nds_spi_reset(rl_nds *m)
{
    nds_spi *s = &m->spi;
    uint8_t *fw = s->fw;
    memset(s, 0, sizeof *s);
    s->m = m;
    s->fw = fw;
    s->pm[4] = 0x40;
    if (!s->fw) {
        s->fw = malloc(NDS_FIRMWARE_SIZE);
        if (s->fw)
            nds_firmware_build(s->fw);
    }
}

void nds_spi_free(rl_nds *m)
{
    free(m->spi.fw);
    m->spi.fw = NULL;
}

static void fire(rl_sched *sc, rl_event *e, uint64_t when)
{
    (void)sc;
    nds_spi *s = e->opaque;
    if (s->end != NDS_NEVER && when >= s->end) {
        s->end = NDS_NEVER;
        if (s->cnt & CNT_IRQ)
            nds_irq_set(s->m, RL_NDS_ARM7, RL_NDS_IRQ_SPI, when);
    }
}

void nds_spi_attach(rl_nds *m)
{
    m->spi.ev = (rl_event){.fire = fire, .opaque = &m->spi};
}

/** @brief The reply bits of the touch controller as 12 or 8 bits of value after a zero bit. */
static uint32_t touch_value(const rl_nds *m, unsigned channel)
{
    const rl_nds_input *in = &m->pad.in;
    switch (channel) {
    case 0:
        return 0x2EE;
    case 1:
        return in->touch ? (uint32_t)in->touch_y << 4 : 0xFFF;
    case 3:
        return in->touch ? 0x400 : 0;
    case 4:
        return in->touch ? 0x800 : 0xFFF;
    case 5:
        return in->touch ? (uint32_t)in->touch_x << 4 : 0;
    case 6:
        return 0x800;
    case 7:
        return 0x380;
    default:
        return 0;
    }
}

static uint8_t touch_byte(nds_spi *s, uint8_t in)
{
    uint8_t out = 0;
    if (s->reply_bits >= 8) {
        out = (uint8_t)(s->reply >> (s->reply_bits - 8));
        s->reply_bits -= 8;
        s->reply &= (1u << s->reply_bits) - 1u;
    } else {
        out = (uint8_t)(s->reply << (8 - s->reply_bits));
        s->reply_bits = 0;
        s->reply = 0;
    }
    if (in & 0x80) {
        unsigned ch = (in >> 4) & 7u;
        uint32_t v = touch_value(s->m, ch);
        if (in & 0x08) {
            s->reply = (v >> 4) & 0xFFu;
            s->reply_bits = 9;
        } else {
            s->reply = v & 0xFFFu;
            s->reply_bits = 13;
        }
    }
    return out;
}

static uint8_t power_byte(nds_spi *s, unsigned pos, uint8_t in)
{
    if (pos == 0) {
        s->op = in;
        return 0;
    }
    unsigned reg = (s->op & 0x7Fu) & 7u;
    if (s->op & 0x80)
        return reg == 1 ? 0 : s->pm[reg];
    static const uint8_t mask[8] = {0x7F, 0, 0x01, 0x03, 0x0F, 0, 0, 0};
    if (pos == 1 && mask[reg])
        s->pm[reg] = (uint8_t)((reg == 4 ? 0x40 : 0) | (in & mask[reg]));
    return 0;
}

static uint8_t flash_byte(nds_spi *s, unsigned pos, uint8_t in)
{
    uint8_t out = 0;
    if (pos == 0) {
        s->op = in;
        s->addr = 0;
        if (in == 0x06)
            s->wel = true;
        else if (in == 0x04)
            s->wel = false;
        return 0;
    }
    unsigned n = pos - 1;
    switch (s->op) {
    case 0x05:
        return (uint8_t)(s->wel << 1);
    case 0x9F:
        return n == 0 ? 0x20 : n == 1 ? 0x40 : n == 2 ? 0x12 : 0;
    case 0x03:
    case 0x0B:
    case 0x0A:
    case 0x02:
    case 0xDB:
    case 0xD8: {
        unsigned skip = s->op == 0x0B ? 4 : 3;
        if (n < 3) {
            s->addr = s->addr << 8 | in;
            if (n == 2 && s->wel && s->op == 0xDB)
                memset(&s->fw[s->addr & (NDS_FIRMWARE_SIZE - 1) & ~(uint32_t)(FLASH_PAGE - 1)], 0xFF, FLASH_PAGE);
            if (n == 2 && s->wel && s->op == 0xD8)
                memset(&s->fw[s->addr & (NDS_FIRMWARE_SIZE - 1) & ~(uint32_t)(FLASH_SECTOR - 1)], 0xFF, FLASH_SECTOR);
        } else if (n < skip) {
            out = 0;
        } else if (s->op == 0x03 || s->op == 0x0B) {
            out = s->fw[s->addr++ & (NDS_FIRMWARE_SIZE - 1)];
        } else if (s->wel && (s->op == 0x0A || s->op == 0x02)) {
            uint32_t a = s->addr & (NDS_FIRMWARE_SIZE - 1);
            s->fw[a] = s->op == 0x02 ? (uint8_t)(s->fw[a] & in) : in;
            s->addr = (s->addr & ~(uint32_t)(FLASH_PAGE - 1)) | ((s->addr + 1) & (FLASH_PAGE - 1));
        }
        break;
    }
    default:
        break;
    }
    return out;
}

uint32_t nds_spi_read(rl_nds *m, uint64_t now)
{
    nds_spi *s = &m->spi;
    uint32_t cnt = s->cnt;
    if (s->end != NDS_NEVER && now < s->end)
        cnt |= 0x80;
    return cnt | (uint32_t)s->data << 16;
}

void nds_spi_write(rl_nds *m, uint32_t v, uint32_t mask, uint64_t now)
{
    nds_spi *s = &m->spi;
    bool busy = s->end != NDS_NEVER && now < s->end;
    if (mask & 0xFFFF) {
        uint16_t keep = (uint16_t)~mask;
        s->cnt = (uint16_t)(((s->cnt & keep) | (v & mask)) & CNT_MASK);
    }
    if (!(mask & 0x00FF0000u) || busy || !(s->cnt & CNT_ENABLE))
        return;
    unsigned dev = (s->cnt >> 8) & 3u;
    if (dev > NDS_SPI_TOUCH)
        return;
    for (unsigned d = 0; d < 3; d++)
        if (d != dev && s->open[d]) {
            s->open[d] = false;
            s->pos[d] = 0;
        }
    uint8_t in = (uint8_t)(v >> 16), out = 0;
    unsigned pos = s->pos[dev];
    switch (dev) {
    case NDS_SPI_POWER:
        out = power_byte(s, pos, in);
        break;
    case NDS_SPI_FIRMWARE:
        out = flash_byte(s, pos, in);
        break;
    default:
        out = touch_byte(s, in);
        break;
    }
    bool hold = (s->cnt & CNT_HOLD) != 0;
    s->open[dev] = hold;
    s->pos[dev] = hold ? pos + 1 : 0;
    if (!hold && dev == NDS_SPI_FIRMWARE)
        s->wel = s->wel && (s->op == 0x06 || s->op == 0x05 || s->op == 0x9F || s->op == 0x03 || s->op == 0x0B);
    if (!hold && dev == NDS_SPI_TOUCH) {
        s->reply = 0;
        s->reply_bits = 0;
    }
    s->data = out;
    s->end = now + 2u * byte_bus[s->cnt & 3u];
    if (m->sched)
        rl_sched_post(m->sched, &s->ev, s->end);
}
