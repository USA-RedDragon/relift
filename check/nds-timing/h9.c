/* hwtest, bare-metal ARM9 (no SDK): the same kernels, cases and harness as sdk/arm9/main.c, set
   up like the SDK leaves the console (MPU regions, cache bits, TCMs), printing the CSV through the
   no$gba debug port. For emulators that cannot run the SDK's startup. */
#include "hwtest.h"
#include "cases.h"
#include "k9tab.h"

#define R16(a) (*(vu16 *)(a))
#define R32(a) (*(vu32 *)(a))

__attribute__((noinline)) void *hw_memcpy(void *d, const void *s, unsigned n)
{
    unsigned char *a = d;
    const unsigned char *b = s;
    while (n--)
        *a++ = *b++;
    return d;
}

__attribute__((noinline)) void *hw_memset(void *d, int c, unsigned n)
{
    unsigned char *a = d;
    while (n--)
        *a++ = (unsigned char)c;
    return d;
}

#define CP15_R(op) ({ u32 v_; __asm__ volatile("mrc p15, 0, %0, " op : "=r"(v_)); v_; })
#define CP15_W(op, v) __asm__ volatile("mcr p15, 0, %0, " op ::"r"(v) : "memory")

#define UNC_CODE ((u32)reg_buf + 16384u)
#define DTCM_BUF 0x02FF0000u
#define ITCM_CODE 0x01FF8020u
#define ITCM_DATA 0x01FF8820u
static u8 main_buf[4096] __attribute__((aligned(4096)));
static u8 reg_buf[32768] __attribute__((aligned(32768)));
static u8 a7_slot[4096] __attribute__((aligned(32)));
static u8 a7_buf[4096] __attribute__((aligned(32)));
static hw_job7 job_mem __attribute__((aligned(32)));
static const int test_region = 7;

typedef void kfn(unsigned, unsigned, unsigned);
static kfn *volatile g_kf;
static volatile u32 g_zero;
/* calls g_kf: the kernel is reached through memory, as a run-time choice */
__attribute__((naked, noinline)) static void callk(unsigned n, unsigned b1, unsigned b2)
{
    __asm__ volatile("push {r4, lr}\n ldr r12, =g_kf\n ldr r12, [r12]\n blx r12\n pop {r4, pc}\n .ltorg\n");
}

static void dcache_flush_all(void)
{
    for (u32 set = 0; set < 32; set++)
        for (u32 way = 0; way < 4; way++)
            CP15_W("c7, c14, 2", way << 30 | set << 5);
    CP15_W("c7, c10, 4", 0);
}
static void icache_invalidate_all(void) { CP15_W("c7, c5, 0", 0); }
static void drain(void) { CP15_W("c7, c10, 4", 0); }
static void dcache_flush(void *p, u32 n)
{
    for (u32 a = (u32)p & ~31u; a < (u32)p + n; a += 32)
        CP15_W("c7, c14, 1", a);
    drain();
}
static void dcache_invalidate(void *p, u32 n)
{
    for (u32 a = (u32)p & ~31u; a < (u32)p + n; a += 32)
        CP15_W("c7, c6, 1", a);
}

static u32 region_get(int i)
{
    switch (i) {
    case 0: return CP15_R("c6, c0, 0");
    case 1: return CP15_R("c6, c1, 0");
    case 2: return CP15_R("c6, c2, 0");
    case 3: return CP15_R("c6, c3, 0");
    case 4: return CP15_R("c6, c4, 0");
    case 5: return CP15_R("c6, c5, 0");
    case 6: return CP15_R("c6, c6, 0");
    default: return CP15_R("c6, c7, 0");
    }
}

static void region_set(int i, u32 v)
{
    switch (i) {
    case 0: CP15_W("c6, c0, 0", v); break;
    case 1: CP15_W("c6, c1, 0", v); break;
    case 2: CP15_W("c6, c2, 0", v); break;
    case 3: CP15_W("c6, c3, 0", v); break;
    case 4: CP15_W("c6, c4, 0", v); break;
    case 5: CP15_W("c6, c5, 0", v); break;
    case 6: CP15_W("c6, c6, 0", v); break;
    default: CP15_W("c6, c7, 0", v); break;
    }
}

typedef struct {
    u32 reg, dc, ic, wb, dp, ip;
} mpu_save;

static void mpu_test_on(mpu_save *s, unsigned attr, bool code_uncached)
{
    int i = test_region;
    s->reg = region_get(i);
    s->dc = CP15_R("c2, c0, 0");
    s->ic = CP15_R("c2, c0, 1");
    s->wb = CP15_R("c3, c0, 0");
    s->dp = CP15_R("c5, c0, 2");
    s->ip = CP15_R("c5, c0, 3");
    bool c = attr == AT_CB11 || attr == AT_CB10, b = attr == AT_CB11 || attr == AT_CB01;
    dcache_flush_all();
    region_set(i, (u32)reg_buf | (0x0E << 1) | 1);
    CP15_W("c2, c0, 0", (s->dc & ~(1u << i)) | (u32)c << i);
    CP15_W("c2, c0, 1", (s->ic & ~(1u << i) & ~(code_uncached ? 2u : 0u)) | 1u << i);
    CP15_W("c3, c0, 0", (s->wb & ~(1u << i)) | (u32)b << i);
    CP15_W("c5, c0, 2", (s->dp & ~(15u << 4 * i)) | 3u << 4 * i);
    CP15_W("c5, c0, 3", (s->ip & ~(15u << 4 * i)) | 3u << 4 * i);
    icache_invalidate_all();
}

static void mpu_test_off(const mpu_save *s)
{
    int i = test_region;
    dcache_flush_all();
    CP15_W("c2, c0, 0", s->dc);
    CP15_W("c2, c0, 1", s->ic);
    CP15_W("c3, c0, 0", s->wb);
    CP15_W("c5, c0, 2", s->dp);
    CP15_W("c5, c0, 3", s->ip);
    region_set(i, s->reg);
    icache_invalidate_all();
}

static char obuf[256] __attribute__((aligned(32)));
static unsigned on;
static void put_c(char c)
{
    if (on < sizeof obuf - 1)
        obuf[on++] = c;
}
static void put_s(const char *s)
{
    while (*s)
        put_c(*s++);
}
static void put_u(u32 v)
{
    char t[12];
    int n = 0;
    do
        t[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n)
        put_c(t[--n]);
}
static void put_d(s32 v)
{
    if (v < 0) {
        put_c('-');
        put_u((u32)-v);
    } else
        put_u((u32)v);
}
static void put_x(u32 v, int digits)
{
    for (int i = digits - 1; i >= 0; i--)
        put_c("0123456789abcdef"[v >> (4 * i) & 15]);
}
static void flush_line(void)
{
    put_c('\n');
    obuf[on] = 0;
    dcache_flush(obuf, sizeof obuf);
    R32(0x04FFFA10) = (u32)obuf;
    on = 0;
}

static u32 dbase9(unsigned region)
{
    switch (region) {
    case RG_DTCM: return DTCM_BUF;
    case RG_ITCM: return ITCM_DATA;
    case RG_MAIN: return (u32)main_buf;
    case RG_UNC:
    case RG_MX: return (u32)reg_buf;
    case RG_IO: return 0x04000400;
    case RG_VRAM: return 0x06800000;
    case RG_PAL: return 0x05000200;
    case RG_OAM: return 0x07000000;
    default: return DTCM_BUF;
    }
}

static bool run9(const hw_case *c, u32 *t16, u32 *t48, u32 *kaddr, u32 *dbase)
{
    kfn *f = k9_fn[c->kernel];
    u32 len = (u32)k9_end[c->kernel] - (u32)f;
    bool test_mpu = c->region == RG_UNC || c->region == RG_MX || c->place == PL_UNC;
    if (c->place == PL_ITCM)
        return false;
    (void)len;
    dcache_flush_all();
    icache_invalidate_all();
    u32 base = dbase9(c->region), base2 = c->region == RG_IO ? base : base + 1024;
    *kaddr = (u32)f;
    *dbase = base;
    mpu_save s;
    unsigned attr = c->region == RG_MX ? c->attr : AT_CB00;
    if (test_mpu)
        mpu_test_on(&s, attr, c->place == PL_UNC);
    g_kf = (kfn *)((u32)f ^ g_zero);
    callk(4, base, base2);
    u32 best[2] = {~0u, ~0u};
    static const unsigned ns[2] = {HW_N1, HW_N2};
    bool cold = c->mode == MD_COLD || c->mode == MD_COLDDRAIN, drn = c->mode == MD_DRAIN || c->mode == MD_COLDDRAIN;
    for (int r = 0; r < HW_REPS; r++)
        for (int k = 0; k < 2; k++) {
            if (cold) {
                dcache_flush_all();
                icache_invalidate_all();
            }
            drain();
            u32 t0 = hw_now();
            callk(ns[k], base, base2);
            if (drn)
                drain();
            u32 t1 = hw_now();
            if (t1 - t0 < best[k])
                best[k] = t1 - t0;
        }
    if (test_mpu)
        mpu_test_off(&s);
    *t16 = best[0];
    *t48 = best[1];
    return true;
}

static bool run7(unsigned ci, u32 *t16, u32 *t48, u32 *kaddr, u32 *dbase)
{
    hw_job7 *job = &job_mem;
    hw_memset(job, 0, sizeof *job);
    job->ci = ci;
    job->mainbuf = (u32)a7_buf;
    dcache_flush(job, sizeof *job);
    __asm__ volatile("mrs r0, cpsr\n orr r0, r0, #0xc0\n msr cpsr_c, r0" ::: "r0", "memory");
    R32(0x04000210) = 1u << 16;
    R32(0x04000208) = 1;
    R16(0x04000180) = 1 << 8 | 1 << 14;
    while ((R16(0x04000180) & 15) != 1) {
        CP15_W("c7, c0, 4", 0);
        R32(0x04000214) = 1u << 16;
    }
    R16(0x04000180) = 0;
    while ((R16(0x04000180) & 15) != 0)
        ;
    R32(0x04000214) = 1u << 16;
    dcache_invalidate(job, sizeof *job);
    *t16 = job->t16;
    *t48 = job->t48;
    *kaddr = job->kaddr;
    *dbase = job->dbase;
    return job->status == 1;
}

static const char *const rg_name[] = {"none", "dtcm", "itcm", "main", "unc", "io", "vram", "pal", "oam", "wram7", "mx", "m2w"};
static const char *const pl_name[] = {"main", "itcm", "unc", "wram"};
static const char *const md_name[] = {"warm", "cold", "drain", "colddrain"};
static const char *const at_name[] = {"-", "cb11", "cb10", "cb01", "cb00"};

static void result(int cpu, unsigned i, const hw_case *c, bool ok, u32 t16, u32 t48, u32 kaddr, u32 dbase)
{
    put_u((u32)cpu);
    put_c(',');
    put_u(i);
    put_c(',');
    put_s(c->name);
    put_c(',');
    put_s(rg_name[c->region]);
    put_c(',');
    put_s(pl_name[c->place]);
    put_c(',');
    put_s(md_name[c->mode]);
    put_c(',');
    put_s(at_name[c->attr]);
    put_c(',');
    put_u(c->risky);
    put_c(',');
    if (!ok) {
        put_s("skipped,,,,,");
        flush_line();
        return;
    }
    s32 d = (s32)(t48 - t16);
    s32 bus100 = d * 100 / (HW_N2 - HW_N1);
    s32 cpu100 = cpu == 9 ? bus100 * 2 : bus100;
    put_x(kaddr, 8);
    put_c(',');
    put_x(dbase, 8);
    put_c(',');
    put_u(t16);
    put_c(',');
    put_u(t48);
    put_c(',');
    put_d(bus100);
    put_c(',');
    put_d(cpu100);
    flush_line();
}

__attribute__((naked, section(".text.start"))) void _start(void)
{
    __asm__ volatile(
        "ldr r0, =0x04000247\n mov r1, #3\n strb r1, [r0]\n"
        "ldr r0, =0x00002078\n mcr p15, 0, r0, c1, c0, 0\n"
        "mov r0, #0\n mcr p15, 0, r0, c7, c5, 0\n mcr p15, 0, r0, c7, c6, 0\n mcr p15, 0, r0, c7, c10, 4\n"
        "ldr r0, =0x04000033\n mcr p15, 0, r0, c6, c0, 0\n"
        "ldr r0, =0x0200002b\n mcr p15, 0, r0, c6, c1, 0\n"
        "mov r0, #0\n mcr p15, 0, r0, c6, c2, 0\n mcr p15, 0, r0, c6, c3, 0\n"
        "ldr r0, =0x02ff001f\n mcr p15, 0, r0, c6, c4, 0\n"
        "ldr r0, =0x01ff801d\n mcr p15, 0, r0, c6, c5, 0\n"
        "ldr r0, =0xffff001d\n mcr p15, 0, r0, c6, c6, 0\n"
        "ldr r0, =0x00000017\n mcr p15, 0, r0, c6, c7, 0\n"
        "ldr r0, =0x00000020\n mcr p15, 0, r0, c9, c1, 1\n"
        "ldr r0, =0x02ff000a\n mcr p15, 0, r0, c9, c1, 0\n"
        "ldr r0, =0x42\n mcr p15, 0, r0, c2, c0, 1\n mcr p15, 0, r0, c2, c0, 0\n"
        "ldr r0, =0x02\n mcr p15, 0, r0, c3, c0, 0\n"
        "ldr r0, =0x33333333\n mcr p15, 0, r0, c5, c0, 3\n mcr p15, 0, r0, c5, c0, 2\n"
        "ldr r0, =0x0005507d\n mcr p15, 0, r0, c1, c0, 0\n"
        "ldr sp, =0x02ff3f00\n"
        "b main\n");
}

int main(void)
{
    on = 0;
    R16(0x04000204) = 0xE880;
    *(vu8 *)0x04000247 = 3;
    hw_timer_start();
    hw_memset(main_buf, 0x5A, sizeof main_buf);
    hw_memset(reg_buf, 0x5A, sizeof reg_buf);
    hw_memset(a7_buf, 0x5A, sizeof a7_buf);
    dcache_flush_all();
    put_s("# relift hwtest 1");
    flush_line();
    put_s("# variant=bare");
    flush_line();
    put_s("# cp15 control=");
    put_x(CP15_R("c1, c0, 0"), 8);
    put_s(" test_region=7");
    flush_line();
    put_s("# buffers main=");
    put_x((u32)main_buf, 8);
    put_s(" test=");
    put_x((u32)reg_buf, 8);
    flush_line();
    put_s("cpu,idx,name,region,place,mode,attr,may_differ_dsi_3ds,kaddr,dbase,t16,t48,bus_x100,cpu_x100");
    flush_line();
    for (unsigned i = 0; i < NCASES9; i++) {
        u32 t16 = 0, t48 = 0, ka = 0, db = 0;
        bool ok = run9(&cases9[i], &t16, &t48, &ka, &db);
        result(9, i, &cases9[i], ok, t16, t48, ka, db);
    }
    R32(0x023FFE00) = (u32)&job_mem;
    dcache_flush_all();
    for (unsigned i = 0; i < NCASES7; i++) {
        u32 t16 = 0, t48 = 0, ka = 0, db = 0;
        bool ok = run7(i, &t16, &t48, &ka, &db);
        result(7, i, &cases7[i], ok, t16, t48, ka, db);
    }
    put_s("# end");
    flush_line();
    R16(0x04000180) = 15 << 8;
    for (;;)
        CP15_W("c7, c0, 4", 0);
}
