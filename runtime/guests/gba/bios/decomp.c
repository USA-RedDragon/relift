/**
 * @file decomp.c
 * @brief BitUnPack, the LZ77, Huffman and run-length decompressors and the
 * difference unfilters, in their 8-bit (WRAM) and 16-bit (VRAM) write forms.
 */
#include "bios_int.h"

static bool header_ok(uint32_t src, uint32_t len) { return len != 0 && (src >> 25) != 0; }

void bios_bit_unpack(bios_call *c)
{
    bios_save(c, 4, 11, true);
    uint32_t src = c->r[0], dst = c->r[1], info = c->r[2];
    int32_t len = (int32_t)bios_ld16(c, info, false);
    uint32_t sw = bios_ld8(c, info + 2, false), dw = bios_ld8(c, info + 3, true);
    uint32_t off = bios_ld32(c, info + 4, true);
    bios_tick(c, 20);
    if (!header_ok(src, (uint32_t)len))
        return;
    uint32_t local = off & 0x7FFFFFFFu;
    bios_push(c, 1, &local);
    bool zero = off >> 31;
    off &= 0x7FFFFFFFu;
    uint32_t shift = (8u - sw) & 0xFFu;
    uint32_t mask = shift >= 32 ? 0 : 0xFFu >> shift, buf = 0, bits = 0;
    while (--len >= 0) {
        uint32_t data = bios_ld8(c, src++, false);
        for (uint32_t sb = 0; sb < 8; sb += sw) {
            uint32_t v = (data >> sb) & mask;
            if (v != 0 || zero)
                v += off;
            buf |= v << bits;
            bits = (bits + dw) & 31u;
            if (bits == 0) {
                bios_st32(c, dst, buf, false);
                dst += 4;
                buf = 0;
            }
            bios_tick(c, 10);
        }
    }
    c->r[0] = src;
    c->r[1] = dst;
    c->r[3] = bits;
}

static void put(bios_call *c, bool vram, uint32_t *dst, uint32_t *half, uint32_t v)
{
    if (!vram) {
        bios_st8(c, *dst, v, false);
    } else if (*dst & 1u) {
        bios_st16(c, *dst & ~1u, *half | (v << 8), false);
    } else {
        *half = v & 0xFFu;
    }
    (*dst)++;
}

static uint32_t peek(bios_call *c, bool vram, uint32_t dst, uint32_t half, uint32_t back)
{
    (void)vram;
    (void)half;
    return bios_ld8(c, dst - back, false);
}

void bios_lz77(bios_call *c, bool vram)
{
    bios_save(c, 4, vram ? 10 : 6, true);
    uint32_t src = c->r[0], dst = c->r[1];
    uint32_t len = bios_ld32(c, src, false) >> 8, half = 0;
    src += 4;
    bios_tick(c, 12);
    if (vram)
        c->r[3] = 0;
    if (!header_ok(src, len)) {
        c->r[0] = src;
        return;
    }
    bool copied = false;
    uint32_t rem = 0, last = 0;
    while (len) {
        uint32_t flags = bios_ld8(c, src++, false);
        for (unsigned k = 0; k < 8 && len; k++, flags <<= 1) {
            if (flags & 0x80u) {
                uint32_t hi = bios_ld8(c, src, false), lo = bios_ld8(c, src + 1, true);
                src += 2;
                uint32_t back = (((hi << 8) | lo) & 0xFFFu) + 1;
                rem = (hi >> 4) + 3;
                copied = true;
                while (rem && len) {
                    last = peek(c, vram, dst, half, back);
                    put(c, vram, &dst, &half, last);
                    rem--;
                    len--;
                    bios_tick(c, 8);
                }
            } else {
                last = bios_ld8(c, src++, false);
                put(c, vram, &dst, &half, last);
                len--;
                bios_tick(c, 8);
            }
        }
    }
    c->r[0] = src;
    c->r[1] = vram ? dst & ~1u : dst;
    if (vram)
        c->r[3] = dst & 1u ? half : 0;
    else if (copied)
        c->r[3] = rem;
}

void bios_huff(bios_call *c)
{
    bios_save(c, 4, 11, true);
    uint32_t src = c->r[0], dst = c->r[1];
    uint32_t hdr = bios_ld32(c, src, false);
    src += 4;
    bios_tick(c, 16);
    int32_t len = (int32_t)(hdr >> 8);
    if (!header_ok(src, (uint32_t)len))
        return;
    uint32_t size = hdr & 0xFu;
    uint32_t per = size ? 32u / size : 0;
    bios_push(c, 1, &per);
    uint32_t tree = src + 1;
    uint32_t stream = tree + bios_ld8(c, src, false) * 2 + 1;
    uint32_t node = tree, buf = 0, bits = 0, word;
    while (len > 0) {
        word = bios_ld32(c, stream, false);
        stream += 4;
        for (int b = 31; b >= 0 && len > 0; b--) {
            uint32_t n = bios_ld8(c, node, false);
            uint32_t bit = (word >> b) & 1u;
            uint32_t next = (node & ~1u) + ((n & 0x3Fu) + 1) * 2 + bit;
            bool leaf = bit ? (n & 0x40u) : (n & 0x80u);
            node = next;
            bios_tick(c, 12);
            if (!leaf)
                continue;
            buf = (buf >> size) | (bios_ld8(c, node, false) << ((32u - size) & 31u));
            node = tree;
            if (++bits == per) {
                bits = 0;
                bios_st32(c, dst, buf, false);
                c->r[3] = buf;
                dst += 4;
                buf = 0;
                len -= 4;
            }
        }
    }
    c->r[0] = stream;
    c->r[1] = dst;
}

void bios_rl(bios_call *c, bool vram)
{
    bios_save(c, 4, 7, true);
    uint32_t src = c->r[0], dst = c->r[1];
    int32_t len = (int32_t)(bios_ld32(c, src, false) >> 8);
    uint32_t half = 0, flag = 0, run = 0;
    bool any = false, any_run = false;
    src += 4;
    bios_tick(c, 12);
    c->r[0] = src;
    c->r[3] = RL_GBA_BIOS_SWI_RETURN;
    if (!header_ok(src, (uint32_t)len))
        return;
    while (len > 0) {
        uint32_t f = bios_ld8(c, src++, false);
        flag = f;
        any = true;
        if (f & 0x80u) {
            uint32_t v = bios_ld8(c, src++, false);
            run = v;
            any_run = true;
            int32_t n = (int32_t)(f & 0x7Fu) + 3;
            len -= n;
            while (n--) {
                put(c, vram, &dst, &half, v);
                bios_tick(c, 6);
            }
        } else {
            int32_t n = (int32_t)(f & 0x7Fu) + 1;
            len -= n;
            while (n--) {
                put(c, vram, &dst, &half, bios_ld8(c, src++, false));
                bios_tick(c, 8);
            }
        }
    }
    if (vram && any)
        bios_st32(c, c->sp - 8, flag, false);
    if (vram && any_run)
        bios_st32(c, c->sp - 4, run, false);
    c->r[0] = src;
    c->r[1] = vram ? dst & ~1u : dst;
    c->r[3] = RL_GBA_BIOS_SWI_RETURN;
}

void bios_diff8(bios_call *c, bool vram)
{
    bios_save(c, 4, vram ? 7 : 4, true);
    uint32_t src = c->r[0], dst = c->r[1];
    int32_t len = (int32_t)(bios_ld32(c, src, false) >> 8);
    uint32_t half = 0;
    src += 4;
    bios_tick(c, 12);
    c->r[0] = src;
    c->r[3] = RL_GBA_BIOS_SWI_RETURN;
    if (!header_ok(src, (uint32_t)len))
        return;
    uint32_t v = bios_ld8(c, src++, false);
    for (;;) {
        put(c, vram, &dst, &half, v);
        bios_tick(c, 8);
        if (--len <= 0)
            break;
        v = (v + bios_ld8(c, src++, false)) & 0xFFu;
    }
    c->r[0] = src;
    c->r[1] = vram ? dst & ~1u : dst;
    c->r[3] = RL_GBA_BIOS_SWI_RETURN;
}

void bios_diff16(bios_call *c)
{
    bios_save(c, 4, 4, true);
    uint32_t src = c->r[0], dst = c->r[1];
    int32_t len = (int32_t)(bios_ld32(c, src, false) >> 8);
    src += 4;
    bios_tick(c, 12);
    c->r[0] = src;
    c->r[3] = BIOS_CHECK_ADDR;
    if (!header_ok(src, (uint32_t)len))
        return;
    uint32_t v = bios_ld16(c, src, false), d = BIOS_CHECK_ADDR;
    src += 2;
    for (;;) {
        bios_st16(c, dst, v, false);
        dst += 2;
        bios_tick(c, 8);
        len -= 2;
        if (len <= 0)
            break;
        d = bios_ld16(c, src, false);
        v = (v + d) & 0xFFFFu;
        src += 2;
    }
    c->r[0] = src;
    c->r[1] = dst;
    c->r[3] = d;
}
