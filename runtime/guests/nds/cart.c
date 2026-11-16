/**
 * @file cart.c
 * @brief The cartridge slot of a decrypted image in the state the boot code
 * leaves it (main data mode): AUXSPICNT, ROMCTRL with its gaps and clock
 * rates, the eight command bytes, the data port at 0x04100010, the
 * transfer-complete IRQ and the DMA request (start timing 5 on the ARM9, 2 on
 * the ARM7), and the AUXSPI bus to the backup chip (backup.c).
 *
 * The cartridge is the image itself. B7 reads return its bytes (an address
 * below 0x8000 reads 0x8000 + (addr & 0x1FF), an address past the image
 * mirrors, a read wraps inside its 4 KiB block) and B8 the chip ID; any other
 * command returns zero. KEY2 data and command encryption are accepted and
 * transparent: the console decrypts what the cartridge encrypts, so a
 * decrypted dump shows plain bytes either way. KEY1 and the secure area
 * handshake are not modeled, the cartridge is already in main data mode.
 *
 * Timing (GBATEK "DS Cartridge I/O Ports", measured against the oracle): a
 * byte takes 5 bus cycles at 6.7 MHz and 8 at 4.2 MHz; the 8 command bytes,
 * then gap1 bytes' time, then per 0x200-byte block gap2 bytes' time and the
 * data. A word is ready 4 bytes' time after the previous one; the port and
 * one waiting word buffer two words, the transfer stalls when both are full.
 * The block ends when its last word has been read.
 *
 * Fresh C from GBATEK.
 */
#include "nds_int.h"

#include <stdlib.h>
#include <string.h>

enum {
    AUX_MASK = 0xE043,
    AUX_ENABLE = 0x8000,
    AUX_IRQ = 0x4000,
    AUX_SERIAL = 0x2000,
    AUX_HOLD = 0x40,
    ROM_MASK = 0x7F7F7FFFu,
    ROM_RESB = 0x20000000u,
    ROM_START = 0x80000000u,
    ROM_DRQ = 0x00800000u,
    BLOCK_WORDS = 0x200 / 4,
    OWNER_BIT = 0x0800,
};

/** @brief The CPU that owns the slot (EXMEMCNT bit 11). */
static unsigned owner(const rl_nds *m) { return (m->exmem[RL_NDS_ARM9] & OWNER_BIT) ? RL_NDS_ARM7 : RL_NDS_ARM9; }

static unsigned card_timing(unsigned cpu) { return cpu == RL_NDS_ARM9 ? NDS_DMA9_CARD : NDS_DMA7_CARD; }

static uint64_t step(const nds_cart *c, uint32_t p)
{
    return 4 * c->byte_ticks + (p % c->block_words == 0 ? c->gap2_ticks : 0);
}

/** @brief The byte at offset o of the image's mirrored address space (zero past the image). */
static uint8_t rom_byte(const nds_cart *c, uint32_t o)
{
    o &= (uint32_t)(c->cap - 1);
    return o < c->rom_len ? c->rom[o] : 0;
}

/** @brief Word i of the data stream of the open command. */
static uint32_t word_at(const nds_cart *c, uint32_t i)
{
    if (c->op == 0xB8)
        return c->chip_id;
    if (c->op != 0xB7)
        return 0;
    uint32_t a = c->addr;
    uint32_t o = (a & ~0xFFFu) | ((a + 4 * i) & 0xFFFu);
    return (uint32_t)rom_byte(c, o) | (uint32_t)rom_byte(c, o + 1) << 8 | (uint32_t)rom_byte(c, o + 2) << 16 |
           (uint32_t)rom_byte(c, o + 3) << 24;
}

static void repost(nds_cart *c)
{
    if (!c->m->sched)
        return;
    uint64_t t = !c->busy ? NDS_NEVER : c->words == 0 ? c->end : c->next;
    if (t == NDS_NEVER)
        rl_sched_cancel(c->m->sched, &c->ev);
    else if (!rl_event_queued(&c->ev) || c->ev.when != t)
        rl_sched_post(c->m->sched, &c->ev, t);
}

static void finish(nds_cart *c, uint64_t t)
{
    c->busy = false;
    c->next = NDS_NEVER;
    if (c->auxcnt & AUX_IRQ)
        nds_irq_set(c->m, owner(c->m), RL_NDS_IRQ_CARD_DONE, t);
}

void nds_cart_poke(rl_nds *m, uint64_t t)
{
    nds_cart *c = &m->cart;
    if (c->busy && c->produced > c->consumed)
        nds_dma_start(m, owner(m), card_timing(owner(m)), t);
}

static void produce(nds_cart *c, uint64_t t)
{
    c->produced++;
    c->at = t;
    if (c->produced >= c->words || c->produced - c->consumed >= 2)
        c->next = NDS_NEVER;
    else
        c->next = t + step(c, c->produced);
    nds_cart_poke(c->m, t);
}

static void settle(nds_cart *c, uint64_t now)
{
    while (c->busy) {
        if (c->words == 0) {
            if (now >= c->end)
                finish(c, c->end);
            break;
        }
        if (c->next == NDS_NEVER || c->next > now)
            break;
        produce(c, c->next);
    }
    repost(c);
}

static void fire(rl_sched *s, rl_event *e, uint64_t when)
{
    (void)s;
    nds_cart *c = e->opaque;
    settle(c, when);
}

static void start(nds_cart *c, uint64_t now)
{
    unsigned code = (c->romctrl >> 24) & 7u;
    uint32_t bytes = code == 0 ? 0 : code == 7 ? 4 : 0x100u << code;
    unsigned per = (c->romctrl & 0x08000000u) ? 8 : 5;
    c->byte_ticks = 2u * per;
    uint32_t g1 = c->romctrl & 0x1FFFu, g2 = (c->romctrl >> 16) & 0x3Fu;
    c->gap2_ticks = (uint64_t)g2 * c->byte_ticks;
    c->op = c->cmd[0];
    uint32_t a = (uint32_t)c->cmd[1] << 24 | (uint32_t)c->cmd[2] << 16 | (uint32_t)c->cmd[3] << 8 | c->cmd[4];
    a &= (uint32_t)(c->cap - 1);
    if (a < 0x8000u)
        a = 0x8000u + (a & 0x1FFu);
    c->addr = a;
    c->busy = true;
    c->words = bytes / 4;
    c->block_words = bytes == 4 ? 1 : BLOCK_WORDS;
    c->produced = c->consumed = 0;
    c->at = now + (8 + (uint64_t)g1) * c->byte_ticks;
    c->end = c->at;
    c->next = c->words ? c->at + step(c, 0) : NDS_NEVER;
}

uint32_t nds_cart_read(rl_nds *m, uint32_t off, uint64_t now)
{
    nds_cart *c = &m->cart;
    settle(c, now);
    switch (off) {
    case 0x1A0: {
        uint32_t aux = c->auxcnt | (now < c->spi_end ? 0x80u : 0);
        return aux | (uint32_t)c->spi_in << 16;
    }
    case 0x1A4:
        return (c->romctrl & ROM_MASK & ~ROM_DRQ) | (c->busy ? ROM_START : 0) |
               (c->busy && c->produced > c->consumed ? ROM_DRQ : 0);
    case NDS_CARDPORT:
        if (c->busy && c->produced > c->consumed) {
            c->last = word_at(c, c->consumed++);
            if (c->consumed == c->words)
                finish(c, now);
            else if (c->next == NDS_NEVER && c->produced < c->words)
                c->next = now + step(c, c->produced);
            repost(c);
            nds_cart_poke(m, now);
        }
        return c->last;
    default:
        return 0;
    }
}

void nds_cart_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask, uint64_t now)
{
    nds_cart *c = &m->cart;
    settle(c, now);
    switch (off) {
    case 0x1A0:
        if (mask & 0xFFFF) {
            uint16_t keep = (uint16_t)~mask;
            c->auxcnt = (uint16_t)(((c->auxcnt & keep) | (v & mask)) & AUX_MASK);
        }
        if ((mask & 0x00FF0000u) && (c->auxcnt & AUX_ENABLE) && (c->auxcnt & AUX_SERIAL) && now >= c->spi_end) {
            static const uint32_t bus[4] = {67, 134, 268, 536};
            c->spi_in = nds_bk_byte(&c->bk, (uint8_t)(v >> 16), (c->auxcnt & AUX_HOLD) != 0, now);
            c->spi_end = now + 2u * bus[c->auxcnt & 3u];
        }
        return;
    case 0x1A4: {
        if (c->busy)
            return;
        uint32_t keep = ~mask;
        uint32_t nv = (c->romctrl & keep) | (v & mask);
        c->romctrl = (nv & ROM_MASK & ~ROM_DRQ) | (c->romctrl & ROM_RESB) | (nv & ROM_RESB);
        if ((v & mask & ROM_START) && (c->auxcnt & AUX_ENABLE))
            start(c, now);
        repost(c);
        return;
    }
    case 0x1A8:
    case 0x1AC:
        for (unsigned i = 0; i < 4; i++)
            if (mask >> (8 * i) & 0xFF)
                c->cmd[(off - 0x1A8) + i] = (uint8_t)(v >> (8 * i));
        return;
    default:
        return;
    }
}

void nds_cart_reset(rl_nds *m)
{
    nds_cart *c = &m->cart;
    nds_bk_free(&c->bk);
    memset(c, 0, sizeof *c);
    c->m = m;
    c->auxcnt = AUX_ENABLE;
    c->romctrl = ROM_RESB;
    c->next = NDS_NEVER;
    c->block_words = BLOCK_WORDS;
    c->cap = 1;
    c->ev = (rl_event){.fire = fire, .opaque = c};
}

void nds_cart_attach(rl_nds *m) { repost(&m->cart); }

int rl_nds_set_cartridge(rl_nds *m, const uint8_t *rom, size_t len)
{
    nds_cart *c = &m->cart;
    if (!rom || len < 0x200 || len > 0x40000000u)
        return -1;
    c->rom = rom;
    c->rom_len = len;
    c->cap = 1;
    while (c->cap < len)
        c->cap <<= 1;
    uint32_t id = 0xC2;
    if (c->cap <= (size_t)128 << 20)
        id |= (uint32_t)((c->cap >> 20) - (c->cap >= (1u << 20) ? 1 : 0)) << 8;
    else
        id |= (0x100u - (uint32_t)(c->cap >> 28)) << 8;
    uint32_t gap1 = (uint32_t)rom[0x60] | (uint32_t)(rom[0x61] & 0x1F) << 8;
    if (gap1 != 0)
        id |= 0x80000000u;
    c->chip_id = id;
    return 0;
}

uint32_t rl_nds_chip_id(const rl_nds *m) { return m->cart.chip_id; }
