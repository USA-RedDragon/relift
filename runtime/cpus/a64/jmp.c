/**
 * @file jmp.c
 * @brief Guest setjmp and longjmp on the unwind primitive (docs/abi.md
 * section 7.1): setjmp stores the callee-saved registers in the guest
 * jmp_buf in guest memory, longjmp reads them back into an rl_resume_regs
 * and calls rl_resume, so the host stack unwinds by RL_PENDING returns and
 * never by a host longjmp.
 */
#include "a64.h"

#include <string.h>

const rl_a64_jmp_layout rl_a64_jmp_newlib = {
    .x19 = 0,
    .sp = 96,
    .d8 = 112,
    .size = 176,
};

static const rl_a64_jmp_layout *g_layout = &rl_a64_jmp_newlib;

void rl_a64_set_jmp_layout(const rl_a64_jmp_layout *l)
{
    g_layout = l ? l : &rl_a64_jmp_newlib;
}

const rl_a64_jmp_layout *rl_a64_jmp_layout_current(void)
{
    return g_layout;
}

int rl_a64_setjmp_at(rl_ctx *c, uint8_t *mem, const rl_a64_jmp_layout *l, uint64_t buf)
{
    for (int i = 0; i < 12; i++)
        rl_st64(mem, buf + l->x19 + 8u * (uint64_t)i, c->x[19 + i]);
    rl_st64(mem, buf + l->sp, c->sp);
    for (int i = 0; i < 8; i++)
        rl_st64(mem, buf + l->d8 + 8u * (uint64_t)i, c->v[8 + i].d[0]);
    c->x[0] = 0;
    return RL_OK;
}

int rl_a64_longjmp_at(rl_ctx *c, uint8_t *mem, const rl_a64_jmp_layout *l, uint64_t buf, uint32_t val)
{
    rl_resume_regs r;
    memcpy(r.x, c->x, sizeof r.x);
    for (int i = 0; i < 12; i++)
        r.x[19 + i] = rl_ld64(mem, buf + l->x19 + 8u * (uint64_t)i);
    r.sp = rl_ld64(mem, buf + l->sp);
    for (int i = 0; i < 8; i++)
        r.d8_15[i] = rl_ld64(mem, buf + l->d8 + 8u * (uint64_t)i);
    r.pc = r.x[30];
    r.x[0] = val ? val : 1u;
    return rl_resume(c, mem, &r);
}

int rl_a64_setjmp(rl_ctx *c, uint8_t *mem)
{
    return rl_a64_setjmp_at(c, mem, g_layout, c->x[0]);
}

int rl_a64_longjmp(rl_ctx *c, uint8_t *mem)
{
    return rl_a64_longjmp_at(c, mem, g_layout, c->x[0], (uint32_t)c->x[1]);
}
