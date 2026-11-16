/**
 * @file bios_cases.h
 * @brief The cases of the BIOS tests: seeded generators for the inputs of every
 * call with a data stream, the same code that ran as a DS test ROM in the
 * oracle (whose results are the digests in bios_oracle.h). Each case writes
 * its inputs to the test memory, calls the SWI and records r0 to r3, a hash
 * of the memory the call wrote and, for the callback forms, a hash of the
 * callback calls.
 */
#ifndef RELIFT_NDS_BIOS_CASES_H
#define RELIFT_NDS_BIOS_CASES_H

#include <stdint.h>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;

/** @brief A byte read of the test memory. */
u32 kit_hld(u32 a);
/** @brief A byte write of the test memory. */
void kit_hst(u32 a, u32 v);

/** @brief The CPU under test: 9 or 7 (the arenas differ, as in the ROM). */
static unsigned kit_cpu;
#define KIT_CPU kit_cpu

#define ARENA (KIT_CPU == 9 ? 0x02100000u : 0x02200000u)
#define LOGADDR (KIT_CPU == 9 ? 0x02180000u : 0x02280000u)
#define PROG (KIT_CPU == 9 ? 0x03000000u : 0x0380F000u)
#define A_SRC (ARENA + 0x0000u)
#define A_DST (ARENA + 0x8000u)
#define A_TMP (ARENA + 0x10000u)
#define A_CB (ARENA + 0x11000u)
#define A_INFO (ARENA + 0x12000u)

/** @brief One recorded case. */
typedef struct rec {
    u32 r[4];
    u32 h;
    u32 aux;
    u32 x;
} rec;

/** @brief A byte read of the test memory (the machine's, as the CPU under test sees it). */
static inline u32 ld8(u32 a) { return kit_hld(a); }
/** @brief A byte write of the test memory. */
static inline void st8(u32 a, u32 v) { kit_hst(a, v & 0xFF); }
/** @brief A little-endian halfword read. */
static inline u32 ld16(u32 a) { return ld8(a) | ld8(a + 1) << 8; }
/** @brief A little-endian halfword write. */
static inline void st16(u32 a, u32 v) { st8(a, v); st8(a + 1, v >> 8); }
/** @brief A little-endian word read. */
static inline u32 ld32(u32 a) { return ld16(a) | ld16(a + 2) << 16; }
/** @brief A little-endian word write. */
static inline void st32(u32 a, u32 v) { st16(a, v); st16(a + 2, v >> 16); }

/** @brief The generator state. */
static u32 kit_rng;
/** @brief The next xorshift value of the case generator. */
static inline u32 rnd(void)
{
    u32 x = kit_rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return kit_rng = x;
}
/** @brief A value below n. */
static inline u32 rndn(u32 n) { return rnd() % n; }

/** @brief One byte folded into an FNV-1a hash. */
static inline u32 fnv(u32 h, u32 b) { return (h ^ (b & 0xFF)) * 16777619u; }
/** @brief The FNV-1a hash of n bytes of the test memory. */
static inline u32 hash_mem(u32 a, u32 n)
{
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++)
        h = fnv(h, ld8(a + i));
    return h;
}
/** @brief Fills n bytes with generator noise. */
static inline void fill_noise(u32 a, u32 n)
{
    for (u32 i = 0; i < n; i++)
        st8(a + i, rnd() >> 11);
}
/** @brief Fills n bytes with one value. */
static inline void fill_byte(u32 a, u32 n, u32 v)
{
    for (u32 i = 0; i < n; i++)
        st8(a + i, v);
}

/** @brief The test fixture runs the SWI. */
void kit_host_swi(unsigned n, u32 *r);
/** @brief Runs SWI n with the registers r[0] to r[3]. */
static inline void do_swi(unsigned n, u32 *r) { kit_host_swi(n, r); }

/** @brief The callback trace hash and the callback error mode (0 none, 1 Open fails, 2 Close fails). */
static u32 cb_trace, cb_mode;
/** @brief Folds one callback call into the callback trace. */
static inline void cbt(u32 id, u32 a, u32 b, u32 c)
{
    cb_trace = fnv(fnv(fnv(fnv(cb_trace, id), a), b), c);
    cb_trace = fnv(fnv(fnv(cb_trace, a >> 8), a >> 16), a >> 24);
    cb_trace = fnv(fnv(fnv(cb_trace, b >> 8), b >> 16), b >> 24);
    cb_trace = fnv(fnv(fnv(cb_trace, c >> 8), c >> 16), c >> 24);
}
/** @brief The Open callback: returns the header word, or an error code in the error mode. */
static u32 cb_open(u32 src, u32 dst, u32 param)
{
    cbt(1, src, dst, param);
    return cb_mode == 1 ? (u32)-5 : ld32(src);
}
/** @brief The Close callback. */
static u32 cb_close(u32 src)
{
    cbt(2, src, 0, 0);
    return cb_mode == 2 ? (u32)-7 : 0;
}
/** @brief The Get_8bit callback. */
static u32 cb_get8(u32 src)
{
    cbt(3, src, 0, 0);
    return ld8(src);
}
/** @brief The Get_16bit callback. */
static u32 cb_get16(u32 src)
{
    cbt(4, src, 0, 0);
    return ld16(src);
}
/** @brief The Get_32bit callback. */
static u32 cb_get32(u32 src)
{
    cbt(5, src, 0, 0);
    return ld32(src);
}
/** @brief The pseudo address of callback id (bit 0: Thumb). */
#define KFN(id, thumb) (0x02F00000u + (id)*8u + (thumb))
/** @brief Runs the callback a guest function address names. */
static inline u32 kit_callback_dispatch(u32 fn, u32 a, u32 b, u32 c)
{
    u32 id = (fn - 0x02F00000u) / 8u;
    switch (id) {
    case 1: return cb_open(a, b, c);
    case 2: return cb_close(a);
    case 3: return cb_get8(a);
    case 4: return cb_get16(a);
    default: return cb_get32(a);
    }
}
/** @brief Builds the callback structure at A_CB (the Get_8bit and Get_32bit entries Thumb when thumb is set). */
static inline void make_callbacks(u32 thumb)
{
    st32(A_CB + 0, KFN(1, 0));
    st32(A_CB + 4, KFN(2, 0));
    st32(A_CB + 8, KFN(3, thumb));
    st32(A_CB + 12, KFN(4, 0));
    st32(A_CB + 16, KFN(5, thumb));
}

#define PRE_R2 0xDEAD0002u
#define PRE_R3 0xCAFE0003u

/** @brief Calls SWI n with four register arguments and records r0 to r3 after it. */
static inline void call_rec(unsigned n, rec *o, u32 a, u32 b, u32 c, u32 d)
{
    u32 r[4] = {a, b, c, d};
    do_swi(n, r);
    for (int i = 0; i < 4; i++)
        o->r[i] = r[i];
}

/** @brief Edge values for the arithmetic cases. */
static const u32 edge_vals[] = {0u, 1u, 2u, 3u, 7u, 0x7Fu, 0x80u, 0xFFu, 0x100u, 0xFFFFu, 0x10000u, 0x7FFFFFFFu, 0x80000000u, 0x80000001u,
                                0xFFFFFFFFu, 0xFFFFFFFEu, 0x55555555u, 0xAAAAAAAAu, 0x12345678u, 0xFFFF0000u};
#define NEDGE (sizeof edge_vals / sizeof edge_vals[0])

/** @brief The absolute value of a signed word, as unsigned. */
static u32 absu(u32 v) { return (s32)v < 0 ? 0u - v : v; }
/** @brief A Div case: the edge grid, then random pairs with a bounded quotient. */
static void k_div(u32 i, rec *o)
{
    u32 n, d;
    if (i < NEDGE * NEDGE) {
        n = edge_vals[i / NEDGE];
        d = edge_vals[i % NEDGE];
        if (d == 0)
            d = 5;
    } else {
        n = rnd() >> rndn(32);
        d = rnd() >> rndn(32);
        if (rnd() & 1)
            n = 0u - n;
        if (rnd() & 1)
            d = 0u - d;
        if (d == 0)
            d = 3;
    }
    if (absu(n) / absu(d) > 3000u && absu(n) != 0x80000000u) {
        d = (d & 0x80000000u) ? 0u - (absu(n) / 2000u + 1u) : absu(n) / 2000u + 1u;
    } else if (absu(n) / absu(d) > 3000u) {
        d = 0x00100000u;
    }
    call_rec(0x09, o, n, d, PRE_R2, PRE_R3);
}
#define N_DIV (NEDGE * NEDGE + 200)

/** @brief A Sqrt case: edge values, perfect squares and their neighbors, then random values. */
static void k_sqrt(u32 i, rec *o)
{
    u32 n;
    if (i < NEDGE)
        n = edge_vals[i];
    else if (i < NEDGE + 130)
        n = (i - NEDGE) * (i - NEDGE) + (i & 1 ? 0 : (i - NEDGE) / 2);
    else
        n = rnd() >> rndn(32);
    call_rec(0x0D, o, n, PRE_R2 + 1, PRE_R2, PRE_R3);
    o->aux = n;
}
#define N_SQRT (NEDGE + 130 + 150)

/** @brief A CpuSet case: random mode, count and addresses; the record's hash covers the destination. */
static void k_cpuset(u32 i, rec *o)
{
    u32 w32 = rnd() & 1, fill = rnd() & 1, cnt = 1 + rndn(i < 100 ? 12 : 48);
    u32 ctl = cnt | fill << 24 | w32 << 26;
    if (i % 11 == 5)
        ctl |= 1u << 25;
    u32 unit = w32 ? 4 : 2;
    u32 src = A_SRC + (rndn(64) * unit), dst = A_DST + (rndn(64) * unit);
    fill_noise(A_SRC, 0x400);
    fill_byte(A_DST - 16, 0x400, 0x5C);
    u32 span = (ctl & 0x1FFFFFu) * unit;
    call_rec(0x0B, o, src, dst, ctl, PRE_R3);
    o->h = hash_mem(dst - 16, (ctl & 0x1FFFFFu) * unit + 32);
    (void)span;
}
#define N_CPUSET 300

/** @brief A CpuSet case with a source in the BIOS region. */
static void k_cpuset_bios(u32 i, rec *o)
{
    u32 w32 = i & 1;
    u32 src = 0x00000100u + (i >> 1) * 0x20u;
    fill_byte(A_DST - 16, 0x100, 0x5C);
    call_rec(0x0B, o, src, A_DST, 8u | (w32 << 26), PRE_R3);
    o->h = hash_mem(A_DST - 16, 0x100);
}
#define N_CPUSET_BIOS 12

/** @brief A CpuFastSet case. */
static void k_fastset(u32 i, rec *o)
{
    u32 fill = rnd() & 1, cnt = 1 + rndn(i < 100 ? 20 : 90);
    u32 ctl = cnt | fill << 24;
    u32 src = A_SRC + rndn(32) * 4, dst = A_DST + rndn(32) * 4;
    fill_noise(A_SRC, 0x600);
    fill_byte(A_DST - 16, 0x500, 0x5C);
    call_rec(0x0C, o, src, dst, ctl, PRE_R3);
    o->h = hash_mem(dst - 16, (ctl & 0x1FFFFFu) * 4 + 32);
}
#define N_FAST 300

/** @brief A GetCRC16 case. */
static void k_crc(u32 i, rec *o)
{
    u32 len = i < 8 ? i * 2 : rndn(150) * 2;
    u32 start = A_SRC + rndn(8) * 2;
    u32 init = i % 3 == 0 ? 0xFFFFu : rnd() & 0xFFFFu;
    if (i % 17 == 16)
        init |= 0xABCD0000u;
    fill_noise(A_SRC, 0x400);
    call_rec(0x0E, o, init, start, len, PRE_R3);
}
#define N_CRC 120

/** @brief A BitUnPack case with random widths and offset. */
static void k_bitunpack(u32 i, rec *o)
{
    static const u8 sws[] = {1, 2, 4, 8};
    static const u8 dws[] = {1, 2, 4, 8, 16, 32};
    u32 sw = sws[rndn(4)], dw = dws[rndn(6)];
    if (dw < sw)
        dw = sw;
    u32 len = 1 + rndn(24);
    for (;;) {
        u32 units = len * 8 / sw;
        if ((units * dw / 8) % 4 == 0 && (units * dw) % 8 == 0)
            break;
        len++;
    }
    u32 off = rnd();
    if (dw < 32)
        off &= (1u << dw) - 1u;
    if (i % 3 == 0)
        off &= 0xFu;
    u32 info = A_INFO;
    st16(info, len);
    st8(info + 2, sw);
    st8(info + 3, dw);
    st32(info + 4, (off & 0x7FFFFFFFu) | (rnd() & 1 ? 0x80000000u : 0u));
    fill_noise(A_SRC, 0x100);
    fill_byte(A_DST - 16, 0x800, 0x5C);
    u32 srcoff = i % 4;
    u32 total = len * 8 / sw * dw / 8;
    call_rec(0x10, o, A_SRC + srcoff, A_DST, info, PRE_R3);
    o->h = hash_mem(A_DST - 16, total + 32);
}
#define N_BITUNPACK 300

/** @brief Writes a random LZ77 stream of L output bytes at A_SRC; returns its size. */
static u32 gen_lz(u32 L, u32 safe16)
{
    u32 s = A_SRC;
    st32(s, 0x10u | L << 8);
    u32 sp = 4, pos = 0;
    while (pos < L) {
        u32 fp = s + sp++, flags = 0;
        for (u32 k = 0; k < 8 && pos < L; k++) {
            if (pos >= (safe16 ? 2u : 1u) && rndn(3) == 0) {
                u32 len = 3 + rndn(16), maxd = pos < 4096 ? pos : 4096;
                u32 d = rndn(maxd);
                if (safe16 && d == 0)
                    d = 1;
                st8(s + sp++, (len - 3) << 4 | d >> 8);
                st8(s + sp++, d & 0xFF);
                flags |= 0x80u >> k;
                pos += len;
            } else {
                st8(s + sp++, rnd() >> 9);
                pos++;
            }
        }
        st8(fp, flags);
    }
    while (sp & 3)
        st8(s + sp++, 0);
    return sp;
}

/** @brief Writes a random run-length stream of L output bytes at A_SRC; returns its size. */
static u32 gen_rl(u32 L)
{
    u32 s = A_SRC;
    st32(s, 0x30u | L << 8);
    u32 sp = 4, pos = 0;
    while (pos < L) {
        u32 left = L - pos;
        if (left >= 3 && (rnd() & 1)) {
            u32 n = 3 + rndn(left < 125 ? left - 2 : 125);
            st8(s + sp++, 0x80u | (n - 3));
            st8(s + sp++, rnd() >> 7);
            pos += n;
        } else {
            u32 n = 1 + rndn(left < 128 ? left : 128);
            st8(s + sp++, n - 1);
            for (u32 j = 0; j < n; j++)
                st8(s + sp++, rnd() >> 7);
            pos += n;
        }
    }
    while (sp & 3)
        st8(s + sp++, 0);
    return sp;
}

/** @brief An LZ77 case, direct form. */
static void k_lz8(u32 i, rec *o)
{
    u32 L = i < 3 ? i : rndn(i < 100 ? 40 : 400);
    u32 sz = gen_lz(L, 0);
    u32 dst = A_DST + (i % 5 == 4 ? rndn(4) : 0);
    fill_byte(A_DST - 16, 0x600, 0x5C);
    for (u32 j = 0; j < 16; j++)
        st8(dst - 16 + j, 0x20 + j);
    call_rec(0x11, o, A_SRC, dst, PRE_R2, PRE_R3);
    o->h = hash_mem(dst - 16, L + 40);
    o->aux = sz;
}
#define N_LZ8 200

/** @brief Runs a callback-form decompression and records the result, the destination hash and the callback trace. */
static void run_cb(unsigned swi, rec *o, u32 dst, u32 param, u32 tmpr2, u32 thumb, u32 hash_len, u32 stream_size)
{
    cb_trace = 2166136261u;
    make_callbacks(thumb);
    call_rec(swi, o, A_SRC, dst, tmpr2 ? tmpr2 : param, A_CB);
    o->h = hash_mem(dst - 16, hash_len + 32);
    o->aux = cb_trace;
    (void)stream_size;
}

/** @brief An LZ77 case, callback form. */
static void k_lzcb(u32 i, rec *o)
{
    u32 L = (i < 3 ? i + 1 : 1 + rndn(i < 60 ? 40 : 300)) & ~1u;
    if (L == 0)
        L = 2;
    u32 sz = gen_lz(L, 1);
    u32 dst = A_DST + (i % 4 == 3 ? 2 : 0);
    fill_byte(A_DST - 16, 0x600, 0x5C);
    cb_mode = i % 20 == 18 ? 1 : i % 20 == 19 ? 2 : 0;
    run_cb(0x12, o, dst, 0x77770000u + i, 0, i & 1, L, sz);
    cb_mode = 0;
}
#define N_LZCB 120

/** @brief A run-length case, direct form. */
static void k_rl8(u32 i, rec *o)
{
    u32 L = i < 3 ? i + 1 : 1 + rndn(i < 100 ? 40 : 400);
    u32 sz = gen_rl(L);
    u32 dst = A_DST + (i % 5 == 4 ? rndn(4) : 0);
    fill_byte(A_DST - 16, 0x600, 0x5C);
    call_rec(0x14, o, A_SRC, dst, PRE_R2, PRE_R3);
    o->h = hash_mem(dst - 16, L + 40);
    o->aux = sz;
}
#define N_RL8 150

/** @brief A run-length case, callback form. */
static void k_rlcb(u32 i, rec *o)
{
    u32 L = (i < 3 ? i + 1 : 1 + rndn(i < 60 ? 40 : 300)) & ~1u;
    if (L == 0)
        L = 2;
    u32 sz = gen_rl(L);
    u32 dst = A_DST + (i % 4 == 3 ? 2 : 0);
    fill_byte(A_DST - 16, 0x600, 0x5C);
    cb_mode = i % 20 == 18 ? 1 : i % 20 == 19 ? 2 : 0;
    run_cb(0x15, o, dst, 0x66660000u + i, 0, i & 1, L, sz);
    cb_mode = 0;
}
#define N_RLCB 100

/** @brief A Diff8bitUnFilter case. */
static void k_diff8(u32 i, rec *o)
{
    u32 L = i < 3 ? i + 1 : 1 + rndn(200);
    st32(A_SRC, 0x81u | L << 8);
    for (u32 j = 0; j < L + 8; j++)
        st8(A_SRC + 4 + j, rnd() >> 8);
    fill_byte(A_DST - 16, 0x400, 0x5C);
    call_rec(0x16, o, A_SRC, A_DST, PRE_R2, PRE_R3);
    o->h = hash_mem(A_DST - 16, L + 32);
}
#define N_DIFF8 60

/** @brief A Diff16bitUnFilter case. */
static void k_diff16(u32 i, rec *o)
{
    u32 L = (i < 3 ? i + 1 : 1 + rndn(100)) * 2;
    st32(A_SRC, 0x82u | L << 8);
    for (u32 j = 0; j < L + 8; j++)
        st8(A_SRC + 4 + j, rnd() >> 8);
    fill_byte(A_DST - 16, 0x400, 0x5C);
    call_rec(0x18, o, A_SRC, A_DST, PRE_R2, PRE_R3);
    o->h = hash_mem(A_DST - 16, L + 32);
}
#define N_DIFF16 60

/** @brief The bytes the last Huffman stream decodes to, and their count. */
static u8 huff_exp[1024];
static u32 huff_exp_len;
/** @brief Writes a random Huffman stream of L output bytes (data size ds) at A_SRC and the bytes it must decode to in huff_exp; returns the stream size. */
static u32 gen_huff(u32 L, u32 ds, u32 *tmp_len)
{
    u32 s = A_SRC;
    u32 nsym = 2 + rndn(ds == 4 ? 15 : 22), maxv = ds == 4 ? 16 : 256;
    u8 sym[32];
    u32 w[64], c0[64], c1[64], leaf[64], val[64];
    u32 used[256] = {0};
    for (u32 k = 0; k < nsym; k++) {
        u32 v;
        do
            v = rndn(maxv);
        while (used[v]);
        used[v] = 1;
        sym[k] = (u8)v;
        w[k] = 1 + rndn(30);
        leaf[k] = 1;
        val[k] = v;
        c0[k] = c1[k] = 0;
    }
    u32 n = nsym, alive[64], na = nsym;
    for (u32 k = 0; k < nsym; k++)
        alive[k] = k;
    while (na > 1) {
        u32 a = 0, b = 1;
        if (w[alive[b]] < w[alive[a]]) {
            a = 1;
            b = 0;
        }
        for (u32 k = 2; k < na; k++) {
            if (w[alive[k]] < w[alive[a]]) {
                b = a;
                a = k;
            } else if (w[alive[k]] < w[alive[b]]) {
                b = k;
            }
        }
        w[n] = w[alive[a]] + w[alive[b]];
        leaf[n] = 0;
        c0[n] = alive[a];
        c1[n] = alive[b];
        u32 ia = a < b ? a : b, ib = a < b ? b : a;
        alive[ia] = n;
        alive[ib] = alive[na - 1];
        na--;
        n++;
    }
    u32 root = alive[0];
    /* layout: bytes relative to s+4: [0] size byte, [1] root, pairs from [2] */
    u8 tbl[600] = {0};
    u32 tpos[64], queue[64], qh = 0, qt = 0, slot = 2;
    tpos[root] = 1;
    queue[qt++] = root;
    while (qh < qt) {
        u32 x = queue[qh++], p = tpos[x], pair = slot;
        slot += 2;
        u32 ch[2] = {c0[x], c1[x]}, byte = 0;
        u32 off = (pair - ((p & ~1u) + 2)) / 2;
        for (u32 k = 0; k < 2; k++) {
            if (leaf[ch[k]]) {
                tbl[pair + k] = (u8)val[ch[k]];
                byte |= k == 0 ? 0x80u : 0x40u;
            } else {
                tpos[ch[k]] = pair + k;
                queue[qt++] = ch[k];
            }
        }
        tbl[p] = (u8)(byte | off);
    }
    u32 T = slot / 2 - 1; /* pairs count = slot/2 - 1 */
    while ((T & 1) == 0) {
        slot += 2;
        T = slot / 2 - 1;
    }
    /* codes */
    u32 code[64], clen[64], stack[64][3], sp = 0;
    stack[sp][0] = root;
    stack[sp][1] = 0;
    stack[sp][2] = 0;
    sp++;
    while (sp) {
        sp--;
        u32 x = stack[sp][0], cb = stack[sp][1], cl = stack[sp][2];
        if (leaf[x]) {
            code[x] = cb;
            clen[x] = cl;
            continue;
        }
        stack[sp][0] = c0[x];
        stack[sp][1] = cb << 1;
        stack[sp][2] = cl + 1;
        sp++;
        stack[sp][0] = c1[x];
        stack[sp][1] = cb << 1 | 1;
        stack[sp][2] = cl + 1;
        sp++;
    }
    /* write */
    st32(s, 0x20u | ds | L << 8);
    st8(s + 4, T);
    u32 end = s + 4 + 1 + slot - 2 + 1 - 1; /* stream address, see below */
    for (u32 k = 1; k < slot; k++)
        st8(s + 4 + k, tbl[k]);
    end = s + 4 + 2 + 2 * T; /* header + (src4 + 2T + 2) */
    u32 nsymbols = L * 8 / ds;
    u32 acc = 0, nb = 0, wp = end;
    huff_exp_len = L;
    for (u32 k = 0; k < L; k++)
        huff_exp[k] = 0;
    for (u32 k = 0; k < nsymbols; k++) {
        u32 x = rndn(nsym);
        if (ds == 8)
            huff_exp[k] = (u8)val[x];
        else
            huff_exp[k / 2] |= (u8)(val[x] << (4 * (k & 1)));
        for (u32 b = 0; b < clen[x]; b++) {
            acc |= ((code[x] >> (clen[x] - 1 - b)) & 1u) << (31 - nb);
            if (++nb == 32) {
                st32(wp, acc);
                wp += 4;
                acc = 0;
                nb = 0;
            }
        }
    }
    if (nb)
        st32(wp, acc), wp += 4;
    st32(wp, 0);
    *tmp_len = 2 * T + 2;
    (void)sym;
    return wp - s;
}

/** @brief A Huffman case: the record's hash covers the decoded bytes. */
static void k_huff(u32 i, rec *o)
{
    u32 ds = i & 1 ? 8 : 4;
    u32 L = (i < 2 ? i : rndn(i < 40 ? 6 : 60)) * 4;
    u32 tl = 0, sz = gen_huff(L, ds, &tl);
    fill_byte(A_DST - 16, 0x400, 0x5C);
    fill_byte(A_TMP, 0x200, 0xE7);
    u32 dst = A_DST;
    cb_trace = 2166136261u;
    cb_mode = i % 25 == 23 ? 1 : i % 25 == 24 ? 2 : 0;
    make_callbacks(i & 2 ? 1 : 0);
    call_rec(0x13, o, A_SRC, dst, A_TMP, A_CB);
    o->h = hash_mem(dst, L);
    o->aux = cb_trace;
    o->x = hash_mem(dst + L, 16);
    cb_mode = 0;
    (void)sz;
    (void)tl;
}
#define N_HUFF 100

/* exhaustive tables, ARM7 only; one case per index, 8 per record group */
/** @brief A GetSineTable case. */
static void k_sine(u32 i, rec *o) { call_rec(0x1A, o, i, PRE_R2, PRE_R2, PRE_R3); }
#define N_SINE 64
/** @brief A GetPitchTable case. */
static void k_pitch(u32 i, rec *o) { call_rec(0x1B, o, i, PRE_R2, PRE_R2, PRE_R3); }
#define N_PITCH 768
/** @brief A GetVolumeTable case. */
static void k_vol(u32 i, rec *o) { call_rec(0x1C, o, i, PRE_R2, PRE_R2, PRE_R3); }
#define N_VOL 724

/** @brief An IsDebugger case: the hash covers the two halfwords around the scratch halfword. */
static void k_dbg(u32 i, rec *o)
{
    st16(0x027FFFF8u, 0x1234);
    st16(0x027FFFFAu, 0x5678);
    call_rec(0x0F, o, PRE_R2 + i, PRE_R2 + 1, PRE_R2 + 2, PRE_R3);
    o->h = ld16(0x027FFFF8u) | ld16(0x027FFFFAu) << 16;
}
#define N_DBG 1

/** @brief A WaitByLoop case. */
static void k_wait(u32 i, rec *o)
{
    static const u32 v[] = {1, 2, 3, 10, 100, 0, 0x80000000u, 0xFFFFFFFFu};
    u32 n = v[i];
    if (n == 0 || n >= 0x80000000u)
        n = 1; /* the loop would run for a long time */
    call_rec(0x03, o, n, PRE_R2, PRE_R2 + 1, PRE_R3);
}
#define N_WAIT 6

/** @brief One group of cases. */
typedef struct group {
    const char *name;
    void (*fn)(u32, rec *);
    u32 n;
    u32 cpus; /* bit 9 / bit 7 as 1<<0 for ARM9, 1<<1 for ARM7 */
    u32 seed;
} group;

#define G9 1u
#define G7 2u
/** @brief Every group: name, case function, count, CPUs (G9, G7), seed. */
static const group kit_groups[] = {
    {"sqrt", k_sqrt, N_SQRT, G9 | G7, 102},
    {"cpuset", k_cpuset, N_CPUSET, G9 | G7, 103},
    {"cpuset_bios", k_cpuset_bios, N_CPUSET_BIOS, G7, 104},
    {"fastset", k_fastset, N_FAST, G9 | G7, 105},
    {"crc16", k_crc, N_CRC, G9 | G7, 106},
    {"bitunpack", k_bitunpack, N_BITUNPACK, G9 | G7, 107},
    {"lz8", k_lz8, N_LZ8, G9 | G7, 108},
    {"lzcb", k_lzcb, N_LZCB, G9 | G7, 109},
    {"rl8", k_rl8, N_RL8, G9 | G7, 110},
    {"rlcb", k_rlcb, N_RLCB, G9 | G7, 111},
    {"diff8", k_diff8, N_DIFF8, G9, 112},
    {"diff16", k_diff16, N_DIFF16, G9, 113},
    {"huff", k_huff, N_HUFF, G9 | G7, 114},
    {"sine", k_sine, N_SINE, G7, 115},
    {"pitch", k_pitch, N_PITCH, G7, 116},
    {"vol", k_vol, N_VOL, G7, 117},
    {"dbg", k_dbg, N_DBG, G9 | G7, 118},
    {"wait", k_wait, N_WAIT, G9 | G7, 119},
    {"div", k_div, N_DIV, G9 | G7, 101},
};
#define KIT_NGROUPS (sizeof kit_groups / sizeof kit_groups[0])

/** @brief The generator seed of case i of a group. */
static inline u32 kit_seed(const group *g, u32 i) { return g->seed * 2654435761u + i * 40503u + 1u; }
/** @brief Runs case i of group g. */
static inline void kit_case(const group *g, u32 i, rec *o)
{
    kit_rng = kit_seed(g, i);
    if (kit_rng == 0)
        kit_rng = 1;
    for (int k = 0; k < 6; k++)
        rnd();
    o->h = 0;
    o->aux = 0;
    o->x = 0;
    g->fn(i, o);
}

#endif
