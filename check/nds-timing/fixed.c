/* hwtest 3c: the fixed cost of BIOS calls (both CPUs; included by each main.c after swi.c). Each case
   times one call on a minimal input with a t_ stub of svc.s: the timer read, the SVC and the timer read,
   the same instructions in every build; t_null times the two reads alone. Rows from idx 6000: t16/t48 =
   the least and the most ticks of HW_REPS calls (after one untimed call), bus_x100 = t16 * 100. */
u32 t_null(void);
u32 t_waitbyloop(u32 n);
u32 t_div(s32 num, s32 den);
u32 t_cpuset(const void *src, void *dst, u32 mode);
u32 t_cpufastset(const void *src, void *dst, u32 mode);
u32 t_sqrt(u32 x);
u32 t_crc16(u32 crc, const void *p, u32 n);
u32 t_bitunpack(const void *src, void *dst, const void *info);
u32 t_lz77wram(const void *src, void *dst);
u32 t_rlwram(const void *src, void *dst);

/* LZ77 and run-length streams of one literal 'A' */
static const u32 fx_lz1[2] __attribute__((aligned(4))) = {0x00000110u, 0x00004100u};
static const u32 fx_rl1[2] __attribute__((aligned(4))) = {0x00000130u, 0x00004100u};
static const u32 fx_bup_info[2] __attribute__((aligned(4))) = {1u | 1u << 16 | 8u << 24, 0x30};

enum { FX_NULL, FX_WAIT1, FX_SET16_1, FX_SET32_1, FX_FAST1, FX_FAST8, FX_DIV1, FX_DIVBIG, FX_SQRT0, FX_SQRTMAX,
       FX_CRC2, FX_LZ1, FX_RL1, FX_BUP1, FX_KINDS };
static const char *const fx_name[FX_KINDS] = {"null", "waitbyloop_1", "cpuset16_1", "cpuset32_1", "cpufastset_1",
                                              "cpufastset_8", "div_1_1", "div_100000_37", "sqrt_0", "sqrt_max",
                                              "crc16_2", "lz77_1", "rl_1", "bitunpack_1"};

/* the ARM7 runs these after all others (swi.c's sw_last7) */
static bool fx_last7(unsigned i) { return i == FX_DIV1 || i == FX_DIVBIG; }

/* one timed call of case i, writing to dst */
static u32 fx_once(unsigned i, u8 *dst)
{
    switch (i) {
    case FX_WAIT1: return t_waitbyloop(1);
    case FX_SET16_1: return t_cpuset(swi_src, dst, 1);
    case FX_SET32_1: return t_cpuset(swi_src, dst, 1 | 1u << 26);
    case FX_FAST1: return t_cpufastset(swi_src, dst, 1);
    case FX_FAST8: return t_cpufastset(swi_src, dst, 8);
    case FX_DIV1: return t_div(1, 1);
    case FX_DIVBIG: return t_div(100000, 37);
    case FX_SQRT0: return t_sqrt(0);
    case FX_SQRTMAX: return t_sqrt(0xFFFFFFFFu);
    case FX_CRC2: return t_crc16(0xFFFF, swi_src, 2);
    case FX_LZ1: return t_lz77wram(fx_lz1, dst);
    case FX_RL1: return t_rlwram(fx_rl1, dst);
    case FX_BUP1: return t_bitunpack(swi_src, dst, fx_bup_info);
    default: return t_null();
    }
}

/* runs case i: the least and the most ticks */
static void fx_measure(unsigned i, u8 *dst, u32 *lo, u32 *hi)
{
    fx_once(i, dst);
    *lo = ~0u;
    *hi = 0;
    for (int r = 0; r < HW_REPS; r++) {
        u32 t = fx_once(i, dst);
        if (t < *lo)
            *lo = t;
        if (t > *hi)
            *hi = t;
    }
}
