/**
 * @file dispatch.c
 * @brief Unit registration, the flat dispatch table keyed by guest address
 * (one slot per code granule of the CPU), the shipped-build miss path and the
 * bad-resume report of docs/abi.md sections 5 to 7, and the registration of
 * hot-loaded units (devload.c).
 */
#include "core.h"
#include "crash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static rl_fn *g_table;
static uint64_t g_lo;
static uint64_t g_count;
static uint8_t g_image[RL_IMAGE_ID_BYTES];
static bool g_have_image;

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
    return i < g_count ? __atomic_load_n(&g_table[i], __ATOMIC_ACQUIRE) : NULL;
}

const uint8_t *rl_dispatch_image(void) { return g_have_image ? g_image : NULL; }

static bool image_zero(const uint8_t *id)
{
    for (uint32_t i = 0; i < RL_IMAGE_ID_BYTES; i++)
        if (id[i])
            return false;
    return true;
}

void rl_units_reset(void)
{
    rl_fn_maps_reset();
    free(g_table);
    g_table = NULL;
    g_lo = 0;
    g_count = 0;
    g_have_image = false;
    memset(g_image, 0, sizeof g_image);
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
    if (!image_zero(u->image_id) && g_have_image && memcmp(u->image_id, g_image, RL_IMAGE_ID_BYTES)) {
        rl_log(RL_LOG_ERROR, "dispatch", "unit %s refused: lifted from a different image set", u->name ? u->name : "?");
        return false;
    }
    return true;
}

static int grow(uint64_t lo, uint64_t hi)
{
    uint32_t sh = rl_core_cpu()->code_shift;
    if (g_table) {
        if (g_lo < lo)
            lo = g_lo;
        if (g_lo + (g_count << sh) > hi)
            hi = g_lo + (g_count << sh);
        if (lo == g_lo && hi == g_lo + (g_count << sh))
            return 0;
    }
    if (lo >= hi)
        return 0;
    uint64_t n = (hi - lo) >> sh;
    rl_fn *t = calloc(n, sizeof *t);
    if (!t)
        return -1;
    if (g_table)
        memcpy(t + ((g_lo - lo) >> sh), g_table, g_count * sizeof *t);
    free(g_table);
    g_table = t;
    g_lo = lo;
    g_count = n;
    return 0;
}

int rl_dispatch_reserve(uint64_t lo, uint64_t hi)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    if (!cpu)
        return -1;
    uint64_t gran = 1ull << cpu->code_shift;
    return grow(lo & ~(gran - 1), (hi + gran - 1) & ~(gran - 1));
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
    if (grow(lo, hi) != 0)
        return -1;
    for (uint32_t k = 0; k < count; k++) {
        if (!g_have_image && !image_zero(units[k]->image_id)) {
            memcpy(g_image, units[k]->image_id, RL_IMAGE_ID_BYTES);
            g_have_image = true;
        }
        for (uint32_t i = 0; i < units[k]->fn_count; i++) {
            const rl_fn_entry *e = &units[k]->fns[i];
            rl_fn *slot = &g_table[(e->guest_addr - g_lo) >> sh];
            if (!*slot || (e->flags & RL_FN_OVERRIDE))
                *slot = e->fn;
        }
    }
    return 0;
}

int rl_unit_add(const rl_unit *u, char *why, size_t n)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    const char *name = u && u->name ? u->name : "?";
    if (!u || !cpu) {
        snprintf(why, n, "no unit");
        return -1;
    }
    if (u->abi_version != RL_ABI_VERSION || u->ctx_size != cpu->ctx_size) {
        snprintf(why, n, "unit %s built for ABI version %u and a %u-byte context, the runtime has %u and %u", name,
                 u->abi_version, u->ctx_size, RL_ABI_VERSION, cpu->ctx_size);
        return -1;
    }
    if (!g_have_image || memcmp(u->image_id, g_image, RL_IMAGE_ID_BYTES)) {
        snprintf(why, n, "unit %s was lifted from a different image set", name);
        return -1;
    }
    uint32_t sh = cpu->code_shift;
    for (uint32_t i = 0; i < u->fn_count; i++) {
        uint64_t a = u->fns[i].guest_addr;
        if (!u->fns[i].fn || a < g_lo || ((a - g_lo) >> sh) >= g_count || (a & ((1ull << sh) - 1))) {
            snprintf(why, n, "unit %s: entry 0x%llx is outside the code range or misaligned", name, (unsigned long long)a);
            return -1;
        }
        if (i && u->fns[i - 1].guest_addr >= a) {
            snprintf(why, n, "unit %s: entries are not sorted at 0x%llx", name, (unsigned long long)a);
            return -1;
        }
    }
    if (u->map_count && rl_fn_maps_register(u->maps, u->map_count) != 0) {
        snprintf(why, n, "unit %s: a function map is malformed", name);
        return -1;
    }
    for (uint32_t i = 0; i < u->fn_count; i++) {
        const rl_fn_entry *e = &u->fns[i];
        rl_fn *slot = &g_table[(e->guest_addr - g_lo) >> sh];
        if (!__atomic_load_n(slot, __ATOMIC_ACQUIRE) || (e->flags & RL_FN_OVERRIDE))
            __atomic_store_n(slot, e->fn, __ATOMIC_RELEASE);
    }
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
