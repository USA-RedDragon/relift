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
static const void *const swi_cb[5] = {(const void *)cb_open, (const void *)cb_close, (const void *)cb_get8,
                                      (const void *)cb_get16, (const void *)cb_get32};

enum { SW_WAIT, SW_SET16, SW_SET32, SW_FILL32, SW_FAST, SW_FASTFILL, SW_DIV, SW_SQRT, SW_CRC, SW_LZW, SW_LZCB,
       SW_HFCB, SW_RLW, SW_RLCB, SW_KINDS };
static const char *const sw_name[SW_KINDS] = {"waitbyloop", "cpuset16", "cpuset32", "cpuset32_fill", "cpufastset",
                                              "cpufastset_fill", "div", "sqrt", "crc16", "lz77_wram", "lz77_cb",
                                              "huff_cb", "rl_wram", "rl_cb"};

typedef struct {
    u8 kind, dst; /* dst: 0 main RAM, 1 video RAM (ARM9) or work RAM (ARM7) */
} sw_case;

#define SW_DST(k) {k, 0}, {k, 1}
static const sw_case sw_cases[] = {
    {SW_WAIT, 0},  SW_DST(SW_SET16), SW_DST(SW_SET32),  SW_DST(SW_FILL32), SW_DST(SW_FAST), SW_DST(SW_FASTFILL),
    {SW_SQRT, 0},  {SW_CRC, 0},      SW_DST(SW_LZW),    SW_DST(SW_LZCB),   SW_DST(SW_HFCB), SW_DST(SW_RLW),
    SW_DST(SW_RLCB), {SW_DIV, 0},
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

/* one timed run of case c at size (SWI_SMALL or SWI_LARGE; the call count 16 or 48 for fixed calls) */
static void sw_run(const sw_case *c, u8 *dst, u32 size)
{
    u32 n = size == SWI_SMALL ? HW_N1 : HW_N2;
    switch (c->kind) {
    case SW_WAIT: svc_waitbyloop(size); break;
    case SW_SET16: svc_cpuset(swi_src, dst, size / 2); break;
    case SW_SET32: svc_cpuset(swi_src, dst, size / 4 | 1u << 26); break;
    case SW_FILL32: svc_cpuset(swi_src, dst, size / 4 | 1u << 26 | 1u << 24); break;
    case SW_FAST: svc_cpufastset(swi_src, dst, size / 4); break;
    case SW_FASTFILL: svc_cpufastset(swi_src, dst, size / 4 | 1u << 24); break;
    case SW_DIV: for (u32 i = 0; i < n; i++) svc_div(100000 + (s32)i, 37); break;
    case SW_SQRT: for (u32 i = 0; i < n; i++) svc_sqrt(0x7FFFFFFFu - i); break;
    case SW_CRC: svc_crc16(0xFFFF, swi_src, size); break;
    case SW_LZW: svc_lz77wram(size == SWI_SMALL ? lz_256 : lz_4096, dst); break;
    case SW_LZCB: svc_lz77cb(size == SWI_SMALL ? lz_256 : lz_4096, dst, 0, swi_cb); break;
    case SW_HFCB: svc_huffcb(size == SWI_SMALL ? hf_256 : hf_4096, dst, 0, swi_cb); break;
    case SW_RLW: svc_rlwram(size == SWI_SMALL ? rl_256 : rl_4096, dst); break;
    case SW_RLCB: svc_rlcb(size == SWI_SMALL ? rl_256 : rl_4096, dst, 0, swi_cb); break;
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
        u32 size = c->kind == SW_WAIT ? (k ? 1088u : 64u) : sizes[k];
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
    default: return SWI_LARGE - SWI_SMALL;
    }
}
