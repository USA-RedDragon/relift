/**
 * @file backup.c
 * @brief The cartridge's backup chip on the AUXSPI bus: the EEPROM (0.5 KiB
 * with the ninth address bit in the opcode, 8 and 64 KiB with two address
 * bytes, 128 KiB with three), FRAM (two address bytes, no page limit) and
 * FLASH command sets, the status register, write enable, page writes, and
 * the save as a plain image.
 *
 * Which chip a cartridge has is not in the image. The rule: a save image
 * that was loaded names it by its length (512 B EEPROM, 8 and 64 KiB
 * EEPROM, 32 KiB FRAM, 128 KiB EEPROM, 256 KiB to 1 MiB FLASH); without one
 * the first command that only one family sends decides (JEDEC ID, page
 * erase, sector erase or deep power-down: FLASH; opcode 0x0A or 0x0B: the
 * 0.5 KiB EEPROM), and a plain read or write opcode (0x02, 0x03) selects the
 * EEPROM with three address bytes, the one chip whose size is a superset of
 * the others. A chip is never changed once chosen.
 *
 * Fresh C from GBATEK ("DS Cartridge Backup", "DS Firmware Serial Flash
 * Memory").
 */
#include "nds_int.h"

#include <stdlib.h>
#include <string.h>

enum {
    OP_WRSR = 0x01,
    OP_WRITE = 0x02,
    OP_READ = 0x03,
    OP_WRDI = 0x04,
    OP_RDSR = 0x05,
    OP_WREN = 0x06,
    OP_PW = 0x0A,
    OP_FAST = 0x0B,
    OP_RDID = 0x9F,
    OP_DP = 0xB9,
    OP_RDP = 0xAB,
    OP_SE = 0xD8,
    OP_PE = 0xDB,
};

bool nds_bk_set(nds_backup *b, nds_bk_kind kind, uint32_t len)
{
    unsigned addr = 0;
    uint32_t page = 0;
    switch (kind) {
    case NDS_BK_EEPROM:
        if (len == 512)
            addr = 1, page = 16;
        else if (len == 8192)
            addr = 2, page = 32;
        else if (len == 65536)
            addr = 2, page = 128;
        else if (len == 131072)
            addr = 3, page = 256;
        break;
    case NDS_BK_FRAM:
        if (len == 8192 || len == 32768)
            addr = 2;
        break;
    case NDS_BK_FLASH:
        if (len == 262144 || len == 524288 || len == 1048576)
            addr = 3, page = 256;
        break;
    default:
        break;
    }
    if (!addr)
        return false;
    if (!b->data) {
        b->data = malloc(NDS_BACKUP_MAX);
        if (!b->data)
            return false;
    }
    memset(b->data, 0xFF, NDS_BACKUP_MAX);
    b->kind = kind;
    b->addr_bytes = addr;
    b->len = len;
    b->page = page;
    return true;
}

void nds_bk_free(nds_backup *b)
{
    free(b->data);
    memset(b, 0, sizeof *b);
}

void nds_bk_close(nds_backup *b)
{
    b->open = false;
    b->pos = 0;
    b->op = 0;
}

static void touch(nds_backup *b, uint64_t now)
{
    b->dirty = true;
    b->changed = now;
}

/** @brief The chip a first command picks when none is fixed yet; false if the command says nothing. */
static bool choose(nds_backup *b, uint8_t op)
{
    switch (op) {
    case OP_RDID:
    case OP_DP:
    case OP_SE:
    case OP_PE:
        return nds_bk_set(b, NDS_BK_FLASH, 524288);
    case OP_PW:
    case OP_FAST:
        return nds_bk_set(b, NDS_BK_EEPROM, 512);
    case OP_WRITE:
    case OP_READ:
        return nds_bk_set(b, NDS_BK_EEPROM, 131072);
    default:
        return false;
    }
}

static uint8_t status(const nds_backup *b)
{
    uint8_t s = (uint8_t)(b->wel << 1 | (b->wp & 3) << 2);
    if (b->kind == NDS_BK_EEPROM && b->len == 512)
        s |= 0xF0;
    return s;
}

static uint8_t jedec(const nds_backup *b, unsigned i)
{
    if (b->kind != NDS_BK_FLASH)
        return 0xFF;
    static const uint8_t id[2] = {0x20, 0x40};
    if (i == 2)
        return b->len == 262144 ? 0x12 : b->len == 524288 ? 0x13 : 0x14;
    return id[i];
}

/** @brief Whether op addresses memory with addr_bytes bytes (and in the 0.5 KiB EEPROM carries bit 8 in the opcode). */
static bool is_mem_op(const nds_backup *b, uint8_t op)
{
    if (b->kind == NDS_BK_EEPROM && b->len == 512)
        return op == OP_READ || op == OP_WRITE || op == OP_FAST || op == OP_PW;
    if (b->kind == NDS_BK_FLASH)
        return op == OP_READ || op == OP_FAST || op == OP_PW || op == OP_WRITE || op == OP_PE || op == OP_SE;
    return op == OP_READ || op == OP_WRITE;
}

static bool is_write_op(const nds_backup *b, uint8_t op)
{
    if (b->kind == NDS_BK_EEPROM && b->len == 512)
        return op == OP_WRITE || op == OP_PW;
    return op == OP_WRITE || (b->kind == NDS_BK_FLASH && op == OP_PW);
}

static uint8_t data_byte(nds_backup *b, uint8_t in, uint64_t now)
{
    uint32_t mask = b->len - 1;
    if (b->op == OP_READ || b->op == OP_FAST) {
        uint8_t v = b->data[b->addr & mask];
        b->addr++;
        return v;
    }
    if (!b->wel)
        return 0xFF;
    if (b->kind == NDS_BK_FLASH && b->op == OP_WRITE)
        b->data[b->addr & mask] &= in;
    else
        b->data[b->addr & mask] = in;
    touch(b, now);
    b->addr++;
    if (b->page && (b->addr % b->page) == 0)
        b->addr -= b->page;
    return 0xFF;
}

/** @brief The op's address is complete: erase commands act now. */
static void addressed(nds_backup *b, uint64_t now)
{
    if (b->kind != NDS_BK_FLASH || !b->wel)
        return;
    uint32_t mask = b->len - 1;
    if (b->op == OP_PE) {
        memset(&b->data[(b->addr & mask) & ~0xFFu], 0xFF, 0x100);
        touch(b, now);
    } else if (b->op == OP_SE) {
        memset(&b->data[(b->addr & mask) & ~0xFFFFu], 0xFF, 0x10000);
        touch(b, now);
    }
}

uint8_t nds_bk_byte(nds_backup *b, uint8_t in, bool hold, uint64_t now)
{
    uint8_t out = 0xFF;
    if (b->pos == 0) {
        b->op = in;
        b->addr = 0;
        if (!b->data)
            choose(b, in);
        if (b->deep && in != OP_RDP)
            goto done;
        if (in == OP_WREN)
            b->wel = true;
        else if (in == OP_WRDI)
            b->wel = false;
        else if (in == OP_RDP)
            b->deep = false;
        else if (in == OP_DP)
            b->deep = true;
        else if (b->data && b->kind == NDS_BK_EEPROM && b->len == 512 && (in == OP_FAST || in == OP_PW))
            b->addr = 0x100;
        b->open = in == OP_WREN || in == OP_WRDI || in == OP_RDP || in == OP_DP || in == OP_RDSR || in == OP_WRSR ||
                  in == OP_RDID || b->data != NULL;
        b->pos = b->open ? 1 : 0;
        goto done;
    }
    if (b->deep)
        goto done;
    unsigned n = b->pos - 1;
    if (b->op == OP_RDSR) {
        out = status(b);
    } else if (b->op == OP_RDID) {
        out = n < 3 ? jedec(b, n) : 0xFF;
    } else if (b->op == OP_WRSR) {
        if (b->wel && n == 0)
            b->wp = (uint8_t)((in >> 2) & 3);
    } else if (b->data && is_mem_op(b, b->op)) {
        bool small = b->kind == NDS_BK_EEPROM && b->len == 512;
        unsigned ab = small ? 1 : b->addr_bytes;
        if (n < ab) {
            b->addr = small ? (b->addr | in) : (b->addr << 8 | in);
            if (n + 1 == ab)
                addressed(b, now);
        } else if (b->op == OP_PE || b->op == OP_SE) {
            out = 0xFF;
        } else if (b->op == OP_FAST && b->kind == NDS_BK_FLASH && n == ab) {
            out = 0xFF;
        } else {
            out = data_byte(b, in, now);
        }
    }
    b->pos++;
done:
    if (!hold) {
        if (b->open && b->wel && (b->op == OP_WRSR || b->op == OP_PE || b->op == OP_SE || is_write_op(b, b->op)))
            b->wel = false;
        nds_bk_close(b);
    }
    return out;
}
