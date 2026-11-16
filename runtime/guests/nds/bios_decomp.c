/**
 * @file bios_decomp.c
 * @brief The LZ77, run-length and Huffman decompressors, in their direct
 * (SWI 0x11, 0x14) and callback (0x12, 0x13, 0x15) forms.
 *
 * Fresh C from GBATEK ("BIOS Decompression Functions", "NDS/DSi
 * Decompression Callbacks"); the stream handling follows relift's GBA BIOS
 * (runtime/guests/gba/bios/decomp.c). A callback form reads its source one
 * unit at a time through the game's callbacks, writes halfwords (a byte at
 * an even address waits for its odd partner), and returns the decompressed
 * length or the negative code its Open or Close callback gave.
 */
#include "bios_int.h"

/** @brief A source of bytes and words: memory, or the game's callbacks. */
typedef struct reader {
    const nds_bios_env *e;
    bool callback;
    uint32_t fn8, fn32, src;
} reader;

static uint32_t callback(const reader *s, uint32_t fn, uint32_t a)
{
    const uint32_t args[4] = {a, 0, 0, 0};
    return s->e->hooks->call(s->e->hooks->user, fn, args);
}

static uint32_t get8(reader *s)
{
    uint32_t v = s->callback ? callback(s, s->fn8, s->src) & 0xFFu : nds_bios_ld(s->e, s->src, 1, false);
    s->src++;
    return v;
}

static uint32_t get32(reader *s)
{
    uint32_t v = s->callback ? callback(s, s->fn32, s->src) : nds_bios_ld(s->e, s->src, 4, false);
    s->src += 4;
    return v;
}

/** @brief The callback structure at table (five function pointers). */
static void table(const nds_bios_env *e, uint32_t table, uint32_t fn[5])
{
    for (unsigned i = 0; i < 5; i++)
        fn[i] = nds_bios_ld(e, table + 4u * i, 4, i != 0);
}

/**
 * @brief The Open callback: returns the header word, or the negative code the
 * routine returns. open_r2 is r2 as Open takes it.
 */
static uint32_t open(const nds_bios_env *e, const uint32_t fn[5], uint32_t src, uint32_t dst, uint32_t r2)
{
    const uint32_t args[4] = {src, dst, r2, 0};
    return e->hooks->call(e->hooks->user, fn[0], args);
}

/** @brief The Close callback, when the game gave one: returns its result. */
static int32_t close_cb(const nds_bios_env *e, const uint32_t fn[5], uint32_t src)
{
    if (fn[1] == 0)
        return 0;
    const uint32_t args[4] = {src, 0, 0, 0};
    return (int32_t)e->hooks->call(e->hooks->user, fn[1], args);
}

/** @brief The output of a decompressor: bytes (direct form) or halfwords (callback form). */
typedef struct sink {
    const nds_bios_env *e;
    bool half;
    uint32_t dst, pending;
} sink;

static void put(sink *k, uint32_t v)
{
    if (!k->half) {
        nds_bios_st(k->e, k->dst, 1, v, false);
    } else if (k->dst & 1u) {
        nds_bios_st(k->e, k->dst & ~1u, 2, k->pending | (v << 8), false);
    } else {
        k->pending = v & 0xFFu;
    }
    k->dst++;
}

/** @brief The byte at back bytes before the output position (a copy source). */
static uint32_t peek(const sink *k, uint32_t back) { return nds_bios_ld(k->e, k->dst - back, 1, false); }

/** @brief The header checks of the direct forms: a zero length or an ARM7 source in the BIOS region ends the call. */
static bool direct_refused(const nds_bios_env *e, uint32_t len, uint32_t src)
{
    return len == 0 || (e->cpu == RL_NDS_ARM7 && (src >> 25) == 0);
}

void nds_bios_lz77(const nds_bios_env *e, uint32_t r[4], bool cb)
{
    uint32_t fn[5] = {0};
    reader s = {.e = e, .callback = cb, .src = r[0]};
    uint32_t hdr;
    if (cb) {
        table(e, r[3], fn);
        s.fn8 = fn[2];
        hdr = open(e, fn, r[0], r[1], r[2]);
        if ((int32_t)hdr < 0) {
            r[0] = hdr;
            return;
        }
    } else {
        hdr = get32(&s);
        s.src = r[0] + 4;
    }
    if (cb)
        s.src = r[0] + 4;
    uint32_t len = hdr >> 8;
    sink k = {.e = e, .half = cb, .dst = r[1]};
    if (!cb && direct_refused(e, len, s.src))
        return;
    uint32_t left = len;
    while (left) {
        uint32_t flags = get8(&s);
        for (unsigned i = 0; i < 8 && left; i++, flags <<= 1) {
            if (flags & 0x80u) {
                uint32_t hi = get8(&s), lo = get8(&s);
                uint32_t back = (((hi << 8) | lo) & 0xFFFu) + 1;
                uint32_t n = (hi >> 4) + 3;
                while (n && left) {
                    put(&k, peek(&k, back));
                    n--;
                    left--;
                }
            } else {
                put(&k, get8(&s));
                left--;
            }
        }
    }
    if (cb) {
        int32_t c = close_cb(e, fn, s.src);
        r[0] = c < 0 ? (uint32_t)c : len;
    }
}

void nds_bios_rl(const nds_bios_env *e, uint32_t r[4], bool cb)
{
    uint32_t fn[5] = {0};
    reader s = {.e = e, .callback = cb, .src = r[0]};
    uint32_t hdr;
    if (cb) {
        table(e, r[3], fn);
        s.fn8 = fn[2];
        hdr = open(e, fn, r[0], r[1], r[2]);
        if ((int32_t)hdr < 0) {
            r[0] = hdr;
            return;
        }
    } else {
        hdr = get32(&s);
    }
    s.src = r[0] + 4;
    int32_t len = (int32_t)(hdr >> 8);
    sink k = {.e = e, .half = cb, .dst = r[1]};
    if (!cb && direct_refused(e, (uint32_t)len, s.src))
        return;
    while (len > 0) {
        uint32_t f = get8(&s);
        if (f & 0x80u) {
            uint32_t v = get8(&s);
            int32_t n = (int32_t)(f & 0x7Fu) + 3;
            len -= n;
            while (n--)
                put(&k, v);
        } else {
            int32_t n = (int32_t)(f & 0x7Fu) + 1;
            len -= n;
            while (n--)
                put(&k, get8(&s));
        }
    }
    if (cb) {
        int32_t c = close_cb(e, fn, s.src);
        r[0] = c < 0 ? (uint32_t)c : hdr >> 8;
    }
}

void nds_bios_huff(const nds_bios_env *e, uint32_t r[4])
{
    uint32_t fn[5];
    table(e, r[3], fn);
    reader s = {.e = e, .callback = true, .fn8 = fn[2], .fn32 = fn[4], .src = r[0]};
    uint32_t tmp = r[2];
    uint32_t hdr = open(e, fn, r[0], r[1], r[2]);
    if ((int32_t)hdr < 0) {
        r[0] = hdr;
        return;
    }
    s.src = r[0] + 4;
    int32_t len = (int32_t)(hdr >> 8);
    uint32_t size = hdr & 0xFu;
    uint32_t per = (size & 7u) + 4u;
    uint32_t tree_bytes = (get8(&s) + 1u) * 2u;
    nds_bios_st(e, tmp, 1, tree_bytes / 2u - 1u, false);
    for (uint32_t i = 1; i < tree_bytes; i++)
        nds_bios_st(e, tmp + i, 1, get8(&s), false);
    uint32_t node = tmp + 1, dst = r[1], buf = 0, leaves = 0;
    while (len > 0) {
        uint32_t word = get32(&s);
        for (int b = 31; b >= 0 && len > 0; b--) {
            uint32_t n = nds_bios_ld(e, node, 1, false);
            uint32_t bit = (word >> b) & 1u;
            uint32_t next = (node & ~1u) + ((n & 0x3Fu) + 1) * 2 + bit;
            bool leaf = bit ? (n & 0x40u) : (n & 0x80u);
            node = next;
            if (!leaf)
                continue;
            uint32_t v = nds_bios_ld(e, node, 1, false);
            buf = (buf >> size) | (size ? v << (32u - size) : 0);
            node = tmp + 1;
            if (++leaves == per) {
                leaves = 0;
                nds_bios_st(e, dst, 4, buf, false);
                dst += 4;
                len -= 4;
            }
        }
    }
    int32_t c = close_cb(e, fn, s.src);
    r[0] = c < 0 ? (uint32_t)c : hdr >> 8;
}
