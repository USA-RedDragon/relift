/* hwtest: BIOS call cases, shared by both CPUs (included by each main.c after hwtest.h). Each case
   times one call on the small and on the large input (the minimum of HW_REPS runs, interrupts
   off), or 16 and 48 calls of a call whose input is fixed; the result's CRC16 is checked against
   the one gen_swi.py computed. */
#include "swidata.h"

void svc_waitbyloop(u32 n);
void svc_cpuset(const void *src, void *dst, u32 mode);
void svc_cpufastset(const void *src, void *dst, u32 mode);
u32 svc_div(s32 num, s32 den);
u32 svc_sqrt(u32 x);
u32 svc_crc16(u32 crc, const void *p, u32 n);
void svc_lz77wram(const void *src, void *dst);
void svc_lz77cb(const void *src, void *dst, u32 param, const void *cb);
void svc_huffcb(const void *src, void *dst, u32 param, const void *cb);
void svc_rlwram(const void *src, void *dst);
void svc_rlcb(const void *src, void *dst, u32 param, const void *cb);
void svc_bitunpack(const void *src, void *dst, const void *info);
void svc_diff8(const void *src, void *dst);
void svc_diff16(const void *src, void *dst);

#ifdef HWTEST_BARE
/* bare.S: the callbacks and the Div and Sqrt call loops as the SDK build compiled them */
s32 cb_open(const u8 *src, void *dst, u32 param);
s32 cb_close(const u8 *p);
u8 cb_get8(const u8 *p);
u16 cb_get16(const u8 *p);
u32 cb_get32(const u8 *p);
void sw_div_calls(u32 n);
void sw_sqrt_calls(u32 n);
#else
/* the read callbacks of the ...Callback calls: the source is a plain pointer in main RAM */
static s32 cb_open(const u8 *src, void *dst, u32 param)
{
    (void)dst;
    (void)param;
    return *(const u32 *)src;
}
static s32 cb_close(const u8 *p) { (void)p; return 0; }
static u8 cb_get8(const u8 *p) { return *p; }
static u16 cb_get16(const u8 *p) { return *(const u16 *)p; }
static u32 cb_get32(const u8 *p) { return *(const u32 *)p; }
#endif
/* the Huffman call's copy of the tree (the BIOS takes the callback parameter as its buffer) */
static u32 huff_tmp[0x200 / 4];
static const void *const swi_cb[5] = {(const void *)cb_open, (const void *)cb_close, (const void *)cb_get8,
                                      (const void *)cb_get16, (const void *)cb_get32};

/* SW_LZLIT on (hwtest 3c): 256 and 1024 bytes of inputs that split the per-byte costs, and the calls
   hwtest 3 did not time */
enum { SW_WAIT, SW_SET16, SW_SET32, SW_FILL32, SW_FAST, SW_FASTFILL, SW_DIV, SW_SQRT, SW_CRC, SW_LZW, SW_LZCB,
       SW_HFCB, SW_RLW, SW_RLCB, SW_LZLIT, SW_LZREP, SW_RLLIT, SW_RLRUN, SW_HF2, SW_DF8, SW_DF16, SW_BUP,
       SW_FILL16, SW_KINDS };
static const char *const sw_name[SW_KINDS] = {"waitbyloop", "cpuset16", "cpuset32", "cpuset32_fill", "cpufastset",
                                              "cpufastset_fill", "div", "sqrt", "crc16", "lz77_wram", "lz77_cb",
                                              "huff_cb", "rl_wram", "rl_cb", "lz77_lit", "lz77_rep", "rl_lit",
                                              "rl_run", "huff2_cb", "diff8", "diff16", "bitunpack", "cpuset16_fill"};
/* BitUnPack's info: 1 bit to 8 bits, zero stays zero, offset 0x30 */
static const u32 bup_info[2] __attribute__((aligned(4))) = {1u << 16 | 8u << 24, 0x30};

typedef struct {
    u8 kind, dst; /* dst: 0 main RAM, 1 video RAM (ARM9) or work RAM (ARM7) */
    u8 only9;     /* the ARM7's BIOS has no such call */
} sw_case;

#define SW_DST(k) {k, 0}, {k, 1}
static const sw_case sw_cases[] = {
    {SW_WAIT, 0},  SW_DST(SW_SET16), SW_DST(SW_SET32),  SW_DST(SW_FILL32), SW_DST(SW_FAST), SW_DST(SW_FASTFILL),
    {SW_SQRT, 0},  {SW_CRC, 0},      SW_DST(SW_LZW),    SW_DST(SW_LZCB),   SW_DST(SW_HFCB), SW_DST(SW_RLW),
    SW_DST(SW_RLCB), {SW_DIV, 0},
    SW_DST(SW_LZLIT), SW_DST(SW_LZREP), SW_DST(SW_RLLIT), SW_DST(SW_RLRUN), SW_DST(SW_HF2),
    {SW_DF8, 0, 1}, {SW_DF8, 1, 1}, {SW_DF16, 0, 1}, {SW_DF16, 1, 1}, SW_DST(SW_BUP), SW_DST(SW_FILL16),
};
#define NSWI (sizeof sw_cases / sizeof sw_cases[0])

static u16 crc16_sw(const u8 *p, u32 n)
{
    u16 c = 0xFFFF;
    while (n--) {
        c ^= *p++;
        for (int i = 0; i < 8; i++)
            c = c & 1 ? (c >> 1) ^ 0xA001 : c >> 1;
    }
    return c;
}

/* the ARM7 runs this case after all others: the melonDS BIOS replacement never returns from the ARM7's Div,
   and a case that does not return ends the ARM7's part of the program */
static bool sw_last7(unsigned i) { return sw_cases[i].kind == SW_DIV; }

/* the second input set (hwtest 3c) */
static bool sw_set2(unsigned kind) { return kind >= SW_LZLIT && kind != SW_FILL16; }

/* one timed run of case c at size (SWI_SMALL or SWI_LARGE, SWI_SMALL2 or SWI_LARGE2; the call count 16 or 48 for
   fixed calls) */
static void sw_run(const sw_case *c, u8 *dst, u32 size)
{
    u32 n = size == SWI_SMALL ? HW_N1 : HW_N2;
    bool s2 = size == SWI_SMALL2;
    switch (c->kind) {
    case SW_WAIT: svc_waitbyloop(size); break;
    case SW_SET16: svc_cpuset(swi_src, dst, size / 2); break;
    case SW_SET32: svc_cpuset(swi_src, dst, size / 4 | 1u << 26); break;
    case SW_FILL32: svc_cpuset(swi_src, dst, size / 4 | 1u << 26 | 1u << 24); break;
    case SW_FAST: svc_cpufastset(swi_src, dst, size / 4); break;
    case SW_FASTFILL: svc_cpufastset(swi_src, dst, size / 4 | 1u << 24); break;
#ifdef HWTEST_BARE
    case SW_DIV: sw_div_calls(n); break;
    case SW_SQRT: sw_sqrt_calls(n); break;
#else
    case SW_DIV: for (u32 i = 0; i < n; i++) svc_div(100000 + (s32)i, 37); break;
    case SW_SQRT: for (u32 i = 0; i < n; i++) svc_sqrt(0x7FFFFFFFu - i); break;
#endif
    case SW_CRC: svc_crc16(0xFFFF, swi_src, size); break;
    case SW_LZW: svc_lz77wram(size == SWI_SMALL ? lz_256 : lz_4096, dst); break;
    case SW_LZCB: svc_lz77cb(size == SWI_SMALL ? lz_256 : lz_4096, dst, 0, swi_cb); break;
    case SW_HFCB: svc_huffcb(size == SWI_SMALL ? hf_256 : hf_4096, dst, (u32)huff_tmp, swi_cb); break;
    case SW_RLW: svc_rlwram(size == SWI_SMALL ? rl_256 : rl_4096, dst); break;
    case SW_RLCB: svc_rlcb(size == SWI_SMALL ? rl_256 : rl_4096, dst, 0, swi_cb); break;
    case SW_LZLIT: svc_lz77wram(s2 ? lzlit_256 : lzlit_1024, dst); break;
    case SW_LZREP: svc_lz77wram(s2 ? lzrep_256 : lzrep_1024, dst); break;
    case SW_RLLIT: svc_rlwram(s2 ? rllit_256 : rllit_1024, dst); break;
    case SW_RLRUN: svc_rlwram(s2 ? rlrun_256 : rlrun_1024, dst); break;
    case SW_HF2: svc_huffcb(s2 ? hf2_256 : hf2_1024, dst, (u32)huff_tmp, swi_cb); break;
    case SW_DF8: svc_diff8(s2 ? df8_256 : df8_1024, dst); break;
    case SW_DF16: svc_diff16(s2 ? df16_256 : df16_1024, dst); break;
    case SW_BUP: {
        static u32 info[2];
        info[0] = bup_info[0] | size / 8;
        info[1] = bup_info[1];
        svc_bitunpack(s2 ? bup_256 : bup_1024, dst, info);
        break;
    }
    case SW_FILL16: svc_cpuset(swi_src, dst, size / 2 | 1u << 24); break;
    }
}

/* the CRC16 a correct run leaves in dst, or 0 when the case has no output to check */
static u16 sw_want(const sw_case *c, u32 size)
{
    bool small = size == SWI_SMALL;
    switch (c->kind) {
    case SW_SET16:
    case SW_SET32:
    case SW_FAST: return small ? SWI_SRC_CRC_SMALL : SWI_SRC_CRC_LARGE;
    case SW_LZW:
    case SW_LZCB: return small ? LZ_256_CRC : LZ_4096_CRC;
    case SW_HFCB: return small ? HF_256_CRC : HF_4096_CRC;
    case SW_RLW:
    case SW_RLCB: return small ? RL_256_CRC : RL_4096_CRC;
    case SW_LZLIT:
    case SW_RLLIT: return size == SWI_SMALL2 ? LZLIT_256_CRC : LZLIT_1024_CRC;
    case SW_LZREP: return size == SWI_SMALL2 ? LZREP_256_CRC : LZREP_1024_CRC;
    case SW_RLRUN: return size == SWI_SMALL2 ? RLRUN_256_CRC : RLRUN_1024_CRC;
    case SW_HF2: return size == SWI_SMALL2 ? HF2_256_CRC : HF2_1024_CRC;
    case SW_DF8:
    case SW_DF16: return size == SWI_SMALL2 ? DF8_256_CRC : DF8_1024_CRC;
    case SW_BUP: return size == SWI_SMALL2 ? BUP_256_CRC : BUP_1024_CRC;
    default: return 0;
    }
}

/* runs case i into dst (4096 bytes): ticks of the small and large runs, and the CRC check
   (want << 16 | got, both 0 when the case has no output) */
static void sw_measure(unsigned i, u8 *dst, u32 *t_small, u32 *t_large, u32 *check)
{
    const sw_case *c = &sw_cases[i];
    static const u32 sizes[2] = {SWI_SMALL, SWI_LARGE};
    u32 best[2] = {~0u, ~0u};
    u32 got = 0;
    for (int k = 0; k < 2; k++) {
        u32 size = c->kind == SW_WAIT ? (k ? 1088u : 64u) : sw_set2(c->kind) ? (k ? SWI_LARGE2 : SWI_SMALL2) : sizes[k];
        for (u32 j = 0; j < SWI_LARGE; j += 4)
            *(vu32 *)(dst + j) = 0;
        sw_run(c, dst, size);
        for (int r = 0; r < HW_REPS; r++) {
            u32 t0 = hw_now();
            sw_run(c, dst, size);
            u32 t1 = hw_now();
            if (t1 - t0 < best[k])
                best[k] = t1 - t0;
        }
        if (k == 1 && sw_want(c, size)) {
            for (u32 j = 0; j < SWI_LARGE; j += 4)
                *(vu32 *)(dst + j) = 0;
            sw_run(c, dst, size);
            got = (u32)sw_want(c, size) << 16 | crc16_sw(dst, size);
        }
    }
    *t_small = best[0];
    *t_large = best[1];
    *check = got;
}

/* units between the small and large run of case i: output bytes, calls, or loop counts */
static u32 sw_units(unsigned i)
{
    switch (sw_cases[i].kind) {
    case SW_WAIT: return 1024;
    case SW_DIV:
    case SW_SQRT: return HW_N2 - HW_N1;
    default: return sw_set2(sw_cases[i].kind) ? SWI_LARGE2 - SWI_SMALL2 : SWI_LARGE - SWI_SMALL;
    }
}
