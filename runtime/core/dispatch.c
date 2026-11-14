/**
 * @file dispatch.c
 * @brief Unit registration, the flat dispatch table, the shipped-build miss
 * handler and the resume entries of docs/abi.md sections 5 to 7.
 */
#include "core.h"

#include <stdlib.h>
#include <string.h>

static rl_fn *g_table;
static uint64_t g_lo;
static uint64_t g_count;

void rl_dispatch_view(rl_ctx *c)
{
    c->disp_table = g_table;
    c->disp_lo = g_lo;
    c->disp_count = g_count;
}

void rl_units_reset(void)
{
    free(g_table);
    g_table = NULL;
    g_lo = 0;
    g_count = 0;
}

static bool unit_ok(const rl_unit *u)
{
    if (u->abi_version != RL_ABI_VERSION || u->ctx_size != sizeof(rl_ctx)) {
        rl_log(RL_LOG_ERROR, "dispatch", "unit %s refused: abi %u ctx %u, runtime abi %u ctx %u",
               u->name ? u->name : "?", u->abi_version, u->ctx_size, RL_ABI_VERSION, (unsigned)sizeof(rl_ctx));
        return false;
    }
    for (uint32_t i = 0; i < u->fn_count; i++)
        if (u->fns[i].guest_addr & 3) {
            rl_log(RL_LOG_ERROR, "dispatch", "unit %s: misaligned entry 0x%llx", u->name ? u->name : "?",
                   (unsigned long long)u->fns[i].guest_addr);
            return false;
        }
    return true;
}

int rl_units_register(const rl_unit *const *units, uint32_t count)
{
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t k = 0; k < count; k++) {
        if (!unit_ok(units[k]))
            return -1;
        for (uint32_t i = 0; i < units[k]->fn_count; i++) {
            uint64_t a = units[k]->fns[i].guest_addr;
            if (a < lo)
                lo = a;
            if (a + 4 > hi)
                hi = a + 4;
        }
    }
    if (g_table) {
        if (g_lo < lo)
            lo = g_lo;
        if (g_lo + g_count * 4 > hi)
            hi = g_lo + g_count * 4;
    }
    if (lo >= hi)
        return 0;
    uint64_t n = (hi - lo) / 4;
    rl_fn *t = calloc(n, sizeof *t);
    if (!t)
        return -1;
    if (g_table)
        memcpy(t + (g_lo - lo) / 4, g_table, g_count * sizeof *t);
    for (uint32_t k = 0; k < count; k++)
        for (uint32_t i = 0; i < units[k]->fn_count; i++) {
            const rl_fn_entry *e = &units[k]->fns[i];
            rl_fn *slot = &t[(e->guest_addr - lo) / 4];
            if (!*slot || (e->flags & RL_FN_OVERRIDE))
                *slot = e->fn;
        }
    free(g_table);
    g_table = t;
    g_lo = lo;
    g_count = n;
    return 0;
}

int rl_call_guest(rl_ctx *c, uint8_t *mem, uint64_t entry)
{
    c->pc = entry;
    return rl_lookup(c, entry)(c, mem);
}

int rl_dispatch_miss(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    rl_log(RL_LOG_ERROR, "dispatch", "missed code: no function at guest 0x%llx (lr 0x%llx, sp 0x%llx)",
           (unsigned long long)c->pc, (unsigned long long)c->x[30], (unsigned long long)c->sp);
    return rl_pend_exit_process(c, RL_EXIT_SOFTWARE);
}

int rl_resume(rl_ctx *c, uint8_t *mem, const rl_resume_regs *r)
{
    (void)mem;
    memcpy(c->x, r->x, sizeof c->x);
    c->sp = r->sp;
    for (int i = 0; i < 8; i++)
        c->v[8 + i].d[0] = r->d8_15[i];
    c->pend.pc = r->pc;
    c->pend.sp = r->sp;
    c->pend.kind = RL_PEND_UNWIND;
    c->pend.code = 0;
    return RL_PENDING;
}

_Noreturn void rl_bad_resume(rl_ctx *c, uint8_t *mem, uint64_t fn_guest_addr)
{
    (void)mem;
    rl_log(RL_LOG_ERROR, "unwind", "landing pad 0x%llx has no case in function 0x%llx", (unsigned long long)c->pend.pc,
           (unsigned long long)fn_guest_addr);
    rl_process_exit(RL_EXIT_SOFTWARE);
    abort();
}
