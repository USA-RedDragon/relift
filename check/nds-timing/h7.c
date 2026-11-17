/* hwtest, bare-metal ARM7 (no SDK): runs the ARM7 cases the ARM9 hands over (job address at the
   end of main RAM, IPCSYNC handshake), as sdk/arm7/main.c does. Linked to run in WRAM. */
#include "hwtest.h"
#include "cases.h"
#include "k7tab.h"

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

void *hw_memset(void *d, int c, unsigned n)
{
    unsigned char *a = d;
    while (n--)
        *a++ = (unsigned char)c;
    return d;
}

static u8 wram_buf[4096] __attribute__((aligned(32)));
/* the ARM7 program's link address, and where the cartridge image the boot stub copied it from sits */
#define BASE7 0x037F8000u
#define IMAGE7 0x02380100u
#define BASE7_ASM "0x037F8000"
#define IMAGE7_ASM "0x02380100"
#include "calls7.h"

static void run(hw_job7 *j)
{
    if (j->ci >= NCASES7) {
        j->status = 2;
        return;
    }
    const hw_case *c = &cases7[j->ci];
    int in_main = c->place == PL_MAIN;
    u32 base, base2;
    switch (c->region) {
    case RG_MAIN: base = j->mainbuf; base2 = base + 1024; break;
    case RG_IO: base = base2 = 0x04000520; break;
    case RG_M2W: base = j->mainbuf; base2 = (u32)wram_buf; break;
    default: base = (u32)wram_buf; base2 = base + 1024; break;
    }
    j->dbase = base;
    j->kaddr = call7(c->kernel, in_main, 4, base, base2);
    u32 best[2] = {~0u, ~0u};
    static const unsigned ns[2] = {HW_N1, HW_N2};
    for (int r = 0; r < HW_REPS; r++)
        for (int k = 0; k < 2; k++) {
            u32 t0 = hw_now();
            call7(c->kernel, in_main, ns[k], base, base2);
            u32 t1 = hw_now();
            if (t1 - t0 < best[k])
                best[k] = t1 - t0;
        }
    j->t16 = best[0];
    j->t48 = best[1];
    j->status = 1;
}

__attribute__((naked, section(".text.start"))) void _start(void)
{
    __asm__ volatile("ldr sp, =0x0380fd00\n b main\n");
}

int main(void)
{
    hw_timer_start();
    for (;;) {
        unsigned in;
        while ((in = R16(0x04000180) & 15) == 0)
            ;
        if (in == 15)
            break;
        hw_job7 *j = (hw_job7 *)R32(0x023FFE00);
        run(j);
        R16(0x04000180) = 1 << 8 | 1 << 13;
        while ((R16(0x04000180) & 15) != 0)
            ;
        R16(0x04000180) = 0;
    }
    R16(0x040001C0) = 0x8800;
    R16(0x040001C2) = 0x00;
    while (R16(0x040001C0) & 0x80)
        ;
    R16(0x040001C0) = 0x8000;
    R16(0x040001C2) = 0x40;
    for (;;)
        ;
}
