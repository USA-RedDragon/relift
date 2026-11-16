/**
 * @file unwind.c
 * @brief The register install of LLVM libunwind's arm64 resume routine
 * (docs/abi.md section 7): the routine loads a register block and ends in
 * `ret` with the saved pc in x30, a return no lifted function can use as a
 * jump to a landing pad, so the build replaces it with this.
 */
#include "arm64.h"

enum {
    UNW_SP = 0xf8,
    UNW_PC = 0x100,
    UNW_D0 = 0x110,
};

uint64_t rl_arm64_unw_jumpto(rl_ctx *c, uint8_t *mem)
{
    uint64_t b = c->x[0];
    rl_resume_regs r;
    for (int i = 0; i < 30; i++)
        r.x[i] = rl_ld64(mem, b + 8u * (uint64_t)i);
    r.sp = rl_ld64(mem, b + UNW_SP);
    r.pc = rl_ld64(mem, b + UNW_PC);
    r.x[30] = r.pc;
    for (int i = 0; i < 32; i++) {
        c->v[i].d[0] = rl_ld64(mem, b + UNW_D0 + 8u * (uint64_t)i);
        c->v[i].d[1] = 0;
    }
    for (int i = 0; i < 8; i++)
        r.d8_15[i] = c->v[8 + i].d[0];
    return rl_resume(c, mem, &r);
}
