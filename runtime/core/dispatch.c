/**
 * @file dispatch.c
 * @brief Unit registration, the flat dispatch table keyed by guest address
 * (one slot per code granule of the CPU), the shipped-build miss path and the
 * bad-resume report of docs/abi.md sections 5 to 7.
 */
#include "core.h"
#include "crash.h"

#include <stdlib.h>
#include <string.h>

static rl_fn *g_table;
static uint64_t g_lo;
static uint64_t g_count;

void rl_dispatch_view(rl_ctx *c)
{
    rl_ctx_common *k = rl_common(c);
    k->disp_table = g_table;
    k->disp_lo = g_lo;
    k->disp_count = g_count;
}

rl_fn rl_dispatch_find(uint64_t a)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    if (!g_table || !cpu || a < g_lo || (a & ((1ull << cpu->code_shift) - 1)))
        return NULL;
    uint64_t i = (a - g_lo) >> cpu->code_shift;
    return i < g_count ? g_table[i] : NULL;
}

void rl_units_reset(void)
{
    rl_fn_maps_reset();
    free(g_table);
    g_table = NULL;
    g_lo = 0;
    g_count = 0;
}

static bool unit_ok(const rl_cpu_desc *cpu, const rl_unit *u)
{
    if (u->abi_version != RL_ABI_VERSION || u->ctx_size != cpu->ctx_size) {
        rl_log(RL_LOG_ERROR, "dispatch", "unit %s refused: abi %u ctx %u, runtime abi %u ctx %u",
               u->name ? u->name : "?", u->abi_version, u->ctx_size, RL_ABI_VERSION, cpu->ctx_size);
        return false;
    }
    for (uint32_t i = 0; i < u->fn_count; i++)
        if (u->fns[i].guest_addr & ((1ull << cpu->code_shift) - 1)) {
            rl_log(RL_LOG_ERROR, "dispatch", "unit %s: misaligned entry 0x%llx", u->name ? u->name : "?",
                   (unsigned long long)u->fns[i].guest_addr);
            return false;
        }
    return true;
}

int rl_units_register(const rl_unit *const *units, uint32_t count)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    if (!cpu) {
        rl_log(RL_LOG_ERROR, "dispatch", "units registered before rl_core_init");
        return -1;
    }
    uint32_t sh = cpu->code_shift;
    uint64_t gran = 1ull << sh;
    uint64_t lo = UINT64_MAX, hi = 0;
    for (uint32_t k = 0; k < count; k++) {
        if (!unit_ok(cpu, units[k]))
            return -1;
        for (uint32_t i = 0; i < units[k]->fn_count; i++) {
            uint64_t a = units[k]->fns[i].guest_addr;
            if (a < lo)
                lo = a;
            if (a + gran > hi)
                hi = a + gran;
        }
    }
    for (uint32_t k = 0; k < count; k++)
        if (units[k]->map_count && rl_fn_maps_register(units[k]->maps, units[k]->map_count) != 0) {
            rl_log(RL_LOG_ERROR, "dispatch", "unit %s: a function map is malformed", units[k]->name ? units[k]->name : "?");
            return -1;
        }
    if (g_table) {
        if (g_lo < lo)
            lo = g_lo;
        if (g_lo + (g_count << sh) > hi)
            hi = g_lo + (g_count << sh);
    }
    if (lo >= hi)
        return 0;
    uint64_t n = (hi - lo) >> sh;
    rl_fn *t = calloc(n, sizeof *t);
    if (!t)
        return -1;
    if (g_table)
        memcpy(t + ((g_lo - lo) >> sh), g_table, g_count * sizeof *t);
    for (uint32_t k = 0; k < count; k++)
        for (uint32_t i = 0; i < units[k]->fn_count; i++) {
            const rl_fn_entry *e = &units[k]->fns[i];
            rl_fn *slot = &t[(e->guest_addr - lo) >> sh];
            if (!*slot || (e->flags & RL_FN_OVERRIDE))
                *slot = e->fn;
        }
    free(g_table);
    g_table = t;
    g_lo = lo;
    g_count = n;
    return 0;
}

int rl_core_miss(rl_ctx *c, uint8_t *mem, uint64_t target)
{
    (void)mem;
    return rl_guest_fatal(c, "dispatch", "missed code: no function at guest 0x%llx", (unsigned long long)target);
}

_Noreturn void rl_bad_resume(rl_ctx *c, uint8_t *mem, uint64_t fn_guest_addr)
{
    (void)mem;
    rl_log(RL_LOG_ERROR, "unwind", "landing pad 0x%llx has no case in function 0x%llx",
           (unsigned long long)rl_common(c)->pend.pc, (unsigned long long)fn_guest_addr);
    rl_process_exit(RL_EXIT_SOFTWARE);
    abort();
}
