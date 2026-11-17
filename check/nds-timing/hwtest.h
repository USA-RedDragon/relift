/* hwtest: DS CPU timing measurements (shared by the ARM9 and ARM7 programs). */
#pragma once
#ifdef HWTEST_BARE
#include <stdbool.h>
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int32_t s32;
typedef volatile uint8_t vu8;
typedef volatile uint16_t vu16;
typedef volatile uint32_t vu32;
#else
#include <nds/ndstypes.h>
#endif

/* data regions */
enum { RG_NONE, RG_DTCM, RG_ITCM, RG_MAIN, RG_UNC, RG_IO, RG_VRAM, RG_PAL, RG_OAM, RG_WRAM7, RG_MX, RG_M2W };
/* code placement: ARM9 main (cached, in place), ITCM copy, uncached main copy; ARM7 WRAM (in place) */
enum { PL_MAIN, PL_ITCM, PL_UNC, PL_WRAM };
/* warm: caches warmed by a first call; cold: data and instruction caches cleaned and invalidated
   before each timed call; drain: the timed interval includes a write-buffer drain */
enum { MD_WARM, MD_COLD, MD_DRAIN, MD_COLDDRAIN };
/* MPU attributes of the test region (data cache C, write buffer B) */
enum { AT_DEFAULT, AT_CB11, AT_CB10, AT_CB01, AT_CB00 };

typedef struct {
    const char *name;
    u16 kernel;
    u8 region, place, mode, attr, risky;
} hw_case;

/* one ARM7 measurement, in main RAM, handed over by address on PXI user channel 0 */
typedef struct {
    u32 ci;       /* case index into cases7 */
    u32 slot;     /* main RAM code slot (4 KiB) for PL_MAIN */
    u32 mainbuf;  /* main RAM data buffer (4 KiB) */
    u32 t16, t48; /* minimum timer ticks of 16 and 48 iterations */
    u32 kaddr;    /* where the kernel ran */
    u32 dbase;    /* data base address */
    u32 status;   /* 1 done, 2 skipped */
} __attribute__((aligned(32))) hw_job7;

#define HW_N1 16
#define HW_N2 48
#define HW_REPS 5

#define TM0_L (*(vu16 *)0x04000100)
#define TM0_C (*(vu16 *)0x04000102)
#define TM1_L (*(vu16 *)0x04000104)
#define TM1_C (*(vu16 *)0x04000106)

static inline void hw_timer_start(void)
{
    TM0_C = 0;
    TM1_C = 0;
    TM0_L = 0;
    TM1_L = 0;
    TM1_C = 0x0084; /* enable, count-up */
    TM0_C = 0x0080; /* enable, F/1 */
}

/* 33.513982 MHz ticks */
static inline u32 hw_now(void)
{
    u16 h1, lo, h2;
    do {
        h1 = TM1_L;
        lo = TM0_L;
        h2 = TM1_L;
    } while (h1 != h2);
    return (u32)h1 << 16 | lo;
}
