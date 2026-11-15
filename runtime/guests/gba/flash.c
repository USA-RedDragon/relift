/**
 * @file flash.c
 * @brief The Flash backup chips: the command sequences at 0x5555 and 0x2AAA
 * (chip id, byte program, 4 KiB sector erase, chip erase, bank switch),
 * the Atmel chip's 128-byte page write, and the busy time during which a
 * read returns the status bits (bit 7 and the toggling bit 6) instead of
 * data.
 */
#include "gba_int.h"

#include <string.h>

enum { MODE_NONE, MODE_PROGRAM, MODE_BANK, MODE_PAGE };
enum { BUSY_ERASE = 1, BUSY_PROGRAM = 2 };
enum {
    PROGRAM_CYCLES = 300,
    SECTOR_ERASE_CYCLES = 150000,
    CHIP_ERASE_CYCLES = 600000,
    PAGE_WRITE_CYCLES = 168000,
    PAGE_WINDOW_CYCLES = 2520,
    PAGE_SIZE = 128
};

bool gba_flash_valid(rl_gba_save_kind kind, uint16_t id)
{
    if (id == 0)
        return true;
    if (kind == RL_GBA_SAVE_FLASH64)
        return id == RL_GBA_FLASH_ATMEL || id == RL_GBA_FLASH_PANASONIC || id == RL_GBA_FLASH_MACRONIX64 || id == RL_GBA_FLASH_SST;
    return id == RL_GBA_FLASH_SANYO || id == RL_GBA_FLASH_MACRONIX128;
}

static bool two_banks(const gba_save *s) { return s->len > 0x10000; }

static size_t index_of(const gba_save *s, uint32_t a) { return ((size_t)s->fl.bank << 16 | (a & 0xFFFFu)) & (s->len - 1); }

static void commit_page(rl_gba *g, uint64_t when)
{
    gba_save *s = &g->save;
    gba_flash *f = &s->fl;
    size_t base = ((size_t)f->bank << 16 | f->page_base) & (s->len - 1);
    memcpy(&s->data[base], f->page, PAGE_SIZE);
    gba_save_touch(s, when);
    f->page_open = false;
    f->mode = MODE_NONE;
    f->busy_until = when + PAGE_WRITE_CYCLES;
}

static void settle(rl_gba *g, uint64_t now)
{
    gba_flash *f = &g->save.fl;
    if (f->page_open && now >= f->page_deadline)
        commit_page(g, f->page_deadline);
}

uint8_t gba_flash_read(rl_gba *g, uint32_t addr)
{
    gba_save *s = &g->save;
    gba_flash *f = &s->fl;
    uint64_t now = gba_now(g);
    settle(g, now);
    if (now < f->busy_until || f->page_open) {
        f->toggle ^= 1;
        uint8_t status = (uint8_t)(f->toggle << 6);
        if (f->busy_kind == BUSY_PROGRAM)
            status |= (uint8_t)(~f->busy_data & 0x80);
        return status;
    }
    uint32_t a = addr & 0xFFFFu;
    if (f->ident && a < 2)
        return (uint8_t)(f->id >> (8 * a));
    return s->data[index_of(s, a)];
}

static void erase_range(rl_gba *g, size_t from, size_t n, uint64_t now, unsigned cycles)
{
    gba_save *s = &g->save;
    memset(&s->data[from], 0xFF, n);
    gba_save_touch(s, now);
    s->fl.busy_kind = BUSY_ERASE;
    s->fl.busy_until = now + cycles;
}

static void program_byte(rl_gba *g, uint32_t a, uint8_t v, uint64_t now)
{
    gba_save *s = &g->save;
    gba_flash *f = &s->fl;
    s->data[index_of(s, a)] &= v;
    gba_save_touch(s, now);
    f->busy_kind = BUSY_PROGRAM;
    f->busy_data = v;
    f->busy_until = now + PROGRAM_CYCLES;
}

static void page_byte(rl_gba *g, uint32_t a, uint8_t v, uint64_t now)
{
    gba_flash *f = &g->save.fl;
    uint32_t base = a & ~(uint32_t)(PAGE_SIZE - 1);
    if (f->page_open && base != f->page_base)
        commit_page(g, now);
    if (now < f->busy_until && !f->page_open)
        return;
    if (!f->page_open) {
        f->page_open = true;
        f->page_base = base;
        memset(f->page, 0xFF, sizeof f->page);
    }
    f->page[a & (PAGE_SIZE - 1)] = v;
    f->page_deadline = now + PAGE_WINDOW_CYCLES;
    f->busy_kind = BUSY_PROGRAM;
    f->busy_data = v;
}

static void command(rl_gba *g, uint32_t a, uint8_t v, uint64_t now)
{
    gba_save *s = &g->save;
    gba_flash *f = &s->fl;
    bool atmel = f->id == RL_GBA_FLASH_ATMEL;
    if (a == 0x5555) {
        bool armed = f->erase;
        f->erase = false;
        switch (v) {
        case 0x90:
            f->ident = true;
            break;
        case 0xF0:
            f->ident = false;
            break;
        case 0x80:
            f->erase = true;
            break;
        case 0xA0:
            f->mode = atmel ? MODE_PAGE : MODE_PROGRAM;
            break;
        case 0xB0:
            if (two_banks(s))
                f->mode = MODE_BANK;
            break;
        case 0x10:
            if (armed)
                erase_range(g, 0, s->len, now, CHIP_ERASE_CYCLES);
            break;
        default:
            break;
        }
        return;
    }
    if (v == 0x30 && f->erase && !atmel && (a & 0xFFFu) == 0)
        erase_range(g, index_of(s, a & 0xF000u), 0x1000, now, SECTOR_ERASE_CYCLES);
    f->erase = false;
}

void gba_flash_write(rl_gba *g, uint32_t addr, uint8_t v)
{
    gba_save *s = &g->save;
    gba_flash *f = &s->fl;
    uint64_t now = gba_now(g);
    uint32_t a = addr & 0xFFFFu;
    settle(g, now);
    if (f->mode == MODE_PAGE && (f->page_open || now >= f->busy_until)) {
        page_byte(g, a, v, now);
        return;
    }
    if (now < f->busy_until || f->page_open)
        return;
    if (f->mode == MODE_BANK) {
        f->mode = MODE_NONE;
        if (a == 0)
            f->bank = v & 1;
        return;
    }
    if (f->mode == MODE_PROGRAM) {
        f->mode = MODE_NONE;
        program_byte(g, a, v, now);
        return;
    }
    switch (f->phase) {
    case 0:
        if (v == 0xF0) {
            f->ident = false;
            f->erase = false;
        } else if (a == 0x5555 && v == 0xAA) {
            f->phase = 1;
        }
        break;
    case 1:
        f->phase = a == 0x2AAA && v == 0x55 ? 2 : a == 0x5555 && v == 0xAA ? 1 : 0;
        break;
    default:
        f->phase = 0;
        command(g, a, v, now);
        break;
    }
}
