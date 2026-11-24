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
/** @brief The table as rl_dispatch_keep saw it, for the code watch to restore. */
static rl_fn *g_kept;
static uint64_t g_kept_lo;
static uint64_t g_kept_count;
static uint8_t g_image[RL_IMAGE_ID_BYTES];
static bool g_have_image;
/* Dev builds: the cold function of each RL_CODE_COLD slot not dispatched to yet. */
uint64_t rl_counted_insns;
static rl_fn *g_cold;
static rl_fn g_cold_first;
static const rl_code_map *const *g_maps;
static uint32_t g_map_count;

/**
 * @brief RELIFT_FORCE_MISS: empties the dispatch slots of the listed guest
 * addresses (hex, comma separated) when the first context takes its view of
 * the table, after every static unit and code map is registered, so a test
 * reaches the miss path of a shipped build at a chosen target. The first
 * caller does it (an atomic flag); every other returns at once.
 */
static void force_misses(void)
{
    static int done;
    const rl_cpu_desc *cpu = rl_core_cpu();
    const char *e = getenv("RELIFT_FORCE_MISS");
    if (!e || !*e || !g_table || !cpu || __atomic_exchange_n(&done, 1, __ATOMIC_ACQ_REL))
        return;
    while (*e) {
        char *end;
        unsigned long long a = strtoull(e, &end, 16);
        if (end == e)
            break;
        if (a >= g_lo && !(a & ((1ull << cpu->code_shift) - 1)) && ((a - g_lo) >> cpu->code_shift) < g_count) {
            __atomic_store_n(&g_table[(a - g_lo) >> cpu->code_shift], NULL, __ATOMIC_RELEASE);
            rl_log(RL_LOG_WARN, "dispatch", "RELIFT_FORCE_MISS: no function at 0x%llx", a);
        }
        e = *end == ',' ? end + 1 : end;
    }
}

void rl_dispatch_view(rl_ctx *c)
{
    force_misses();
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

void rl_dispatch_own(rl_ctx *c, rl_fn *table, uint64_t lo, uint64_t count)
{
    rl_ctx_common *k = rl_common(c);
    k->disp_table = table;
    k->disp_lo = lo;
    k->disp_count = count;
    if (k->thread)
        k->thread->own_view = true;
}

rl_fn rl_dispatch_find_ctx(rl_ctx *c, uint64_t a)
{
    const rl_ctx_common *k = rl_common(c);
    const rl_cpu_desc *cpu = rl_core_cpu();
    if (!k->thread || !k->thread->own_view || !cpu)
        return rl_dispatch_find(a);
    if (!k->disp_table || a < k->disp_lo || (a & ((1ull << cpu->code_shift) - 1)))
        return NULL;
    uint64_t i = (a - k->disp_lo) >> cpu->code_shift;
    return i < k->disp_count ? __atomic_load_n(&k->disp_table[i], __ATOMIC_ACQUIRE) : NULL;
}

const uint8_t *rl_dispatch_image(void) { return g_have_image ? g_image : NULL; }

void rl_dispatch_set_image(const uint8_t *id)
{
    if (!id)
        return;
    memcpy(g_image, id, RL_IMAGE_ID_BYTES);
    g_have_image = true;
}

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
    free(g_kept);
    g_kept = NULL;
    g_kept_count = 0;
    free(g_cold);
    g_cold = NULL;
    g_cold_first = NULL;
    g_maps = NULL;
    g_map_count = 0;
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

int rl_dispatch_set(uint64_t a, rl_fn fn)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    if (!g_table || !cpu || a < g_lo || (a & ((1ull << cpu->code_shift) - 1)))
        return -1;
    uint64_t i = (a - g_lo) >> cpu->code_shift;
    if (i >= g_count)
        return -1;
    __atomic_store_n(&g_table[i], fn, __ATOMIC_RELEASE);
    return 0;
}

int rl_dispatch_keep(void)
{
    if (!g_table)
        return 0;
    if (g_kept_count != g_count || g_kept_lo != g_lo) {
        rl_fn *k = calloc(g_count, sizeof *k);
        if (!k)
            return -1;
        if (g_kept) {
            for (uint64_t i = 0; i < g_kept_count; i++) {
                uint64_t a = g_kept_lo + (i << rl_core_cpu()->code_shift);
                if (a >= g_lo && ((a - g_lo) >> rl_core_cpu()->code_shift) < g_count)
                    k[(a - g_lo) >> rl_core_cpu()->code_shift] = g_kept[i];
            }
        }
        free(g_kept);
        g_kept = k;
        g_kept_lo = g_lo;
        g_kept_count = g_count;
    }
    for (uint64_t i = 0; i < g_count; i++) {
        rl_fn f = __atomic_load_n(&g_table[i], __ATOMIC_ACQUIRE);
        if (f)
            g_kept[i] = f;
    }
    return 0;
}

/** @brief The slot range of guest [lo, lo+size) in the table, clipped; false when none. */
static bool slots_of(uint64_t lo, uint64_t size, uint64_t *first, uint64_t *end)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    if (!g_table || !cpu || !size)
        return false;
    uint32_t sh = cpu->code_shift;
    uint64_t top = g_lo + (g_count << sh), hi = lo + size;
    if (hi <= g_lo || lo >= top)
        return false;
    *first = lo > g_lo ? (lo - g_lo) >> sh : 0;
    *end = hi < top ? (hi - g_lo + (1ull << sh) - 1) >> sh : g_count;
    return true;
}

void rl_dispatch_drop(uint64_t lo, uint64_t size)
{
    uint64_t a, b;
    if (slots_of(lo, size, &a, &b))
        for (uint64_t i = a; i < b; i++)
            __atomic_store_n(&g_table[i], NULL, __ATOMIC_RELEASE);
}

void rl_dispatch_restore(uint64_t lo, uint64_t size)
{
    uint64_t a, b;
    if (!g_kept || g_kept_lo != g_lo || g_kept_count != g_count || !slots_of(lo, size, &a, &b))
        return;
    for (uint64_t i = a; i < b; i++)
        if (g_kept[i])
            __atomic_store_n(&g_table[i], g_kept[i], __ATOMIC_RELEASE);
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
            const rl_fn_entry *e = &units[k]->fns[i];
            uint64_t a = e->guest_addr;
            uint64_t end = a + ((e->flags & RL_FN_RANGE) && e->guest_size > gran ? e->guest_size : gran);
            if (a < lo)
                lo = a;
            if (end > hi)
                hi = end;
        }
    }
    /* One registration for every map of the batch: the host index is
     * merged once, not once per unit (a program with hundreds of lifted
     * libraries re-sorted the whole index hundreds of times at start). */
    uint32_t nmaps = 0;
    for (uint32_t k = 0; k < count; k++)
        nmaps += units[k]->map_count;
    if (nmaps) {
        const rl_fn_map **all = malloc(nmaps * sizeof *all);
        if (!all)
            return -1;
        uint32_t at = 0;
        for (uint32_t k = 0; k < count; k++)
            for (uint32_t i = 0; i < units[k]->map_count; i++)
                all[at++] = units[k]->maps[i];
        int rc = rl_fn_maps_register(all, nmaps);
        free(all);
        if (rc != 0) {
            for (uint32_t k = 0; k < count; k++)
                if (units[k]->map_count && rl_fn_maps_check(units[k]->maps, units[k]->map_count) != 0)
                    rl_log(RL_LOG_ERROR, "dispatch", "unit %s: a function map is malformed", units[k]->name ? units[k]->name : "?");
            return -1;
        }
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
            uint64_t n = (e->flags & RL_FN_RANGE) ? (e->guest_size + gran - 1) >> sh : 1;
            for (uint64_t j = 0; j < (n ? n : 1); j++) {
                rl_fn *slot = &g_table[((e->guest_addr - g_lo) >> sh) + j];
                if (!*slot || (e->flags & RL_FN_OVERRIDE))
                    *slot = e->fn;
            }
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

/* The entry of u at guest address a, walking u's sorted entries from *k. */
static const rl_fn_entry *entry_at(const rl_unit *u, uint32_t *k, uint64_t a)
{
    while (*k < u->fn_count && u->fns[*k].guest_addr < a)
        (*k)++;
    return *k < u->fn_count && u->fns[*k].guest_addr == a ? &u->fns[*k] : NULL;
}

static const char *class_name(unsigned cls)
{
    switch (cls) {
    case RL_CODE_TRAP:
        return "trap";
    case RL_CODE_HOT:
        return "hot";
    case RL_CODE_COLD:
        return "cold";
    default:
        return "unknown";
    }
}

/* Checks map m against its units and the table; with fill, stores cold_first. */
static int code_map_walk(const rl_code_map *m, rl_fn cold_first, bool fill)
{
    uint32_t sh = rl_core_cpu()->code_shift;
    const char *name = m->module ? m->module : "?";
    if (m->version != RL_CODE_MAP_VERSION || !m->hot || !m->cold || !m->pages || !m->patterns) {
        rl_log(RL_LOG_ERROR, "dispatch", "code map of %s: version %u or its tables are wrong", name, m->version);
        return -1;
    }
    uint64_t span = (uint64_t)m->page_count * RL_CODE_PAGE_SLOTS;
    if (m->lo < g_lo || (m->lo & ((1ull << sh) - 1)) || ((m->lo - g_lo) >> sh) + span > g_count) {
        rl_log(RL_LOG_ERROR, "dispatch", "code map of %s at 0x%llx is outside the dispatch table", name,
               (unsigned long long)m->lo);
        return -1;
    }
    uint32_t kh = 0;
    uint64_t bad = 0, first = 0;
    unsigned first_cls = 0;
    bool first_hot = false, first_full = false;
    uint64_t base = (m->lo - g_lo) >> sh;
    for (uint32_t p = 0; p < m->page_count; p++) {
        if (m->pages[p] >= m->pattern_count) {
            rl_log(RL_LOG_ERROR, "dispatch", "code map of %s: page %u names pattern %u of %u", name, p, m->pages[p],
                   m->pattern_count);
            return -1;
        }
        const uint8_t *pat = m->patterns + (size_t)m->pages[p] * RL_CODE_PAGE_BYTES;
        for (uint32_t s = 0; s < RL_CODE_PAGE_SLOTS; s++) {
            uint64_t i = (uint64_t)p * RL_CODE_PAGE_SLOTS + s;
            uint64_t a = m->lo + (i << sh);
            unsigned cls = (pat[s / 4] >> (2 * (s % 4))) & 3u;
            const rl_fn_entry *h = entry_at(m->hot, &kh, a);
            rl_fn *slot = &g_table[base + i];
            if (fill) {
                if (cls == RL_CODE_COLD && cold_first) {
                    g_cold[base + i] = *slot;
                    *slot = cold_first;
                }
                continue;
            }
            bool ok = cls == RL_CODE_HOT    ? h && *slot
                      : cls == RL_CODE_COLD ? !h && *slot
                      : cls == RL_CODE_TRAP ? !h && !*slot
                                            : false;
            if (!ok && !bad++) {
                first = a;
                first_cls = cls;
                first_hot = h != NULL;
                first_full = *slot != NULL;
            }
        }
    }
    if (bad) {
        rl_log(RL_LOG_ERROR, "dispatch",
               "code map of %s: %llu slots disagree with its units, the first at 0x%llx (class %s, hot entry %s, slot %s)",
               name, (unsigned long long)bad, (unsigned long long)first, class_name(first_cls), first_hot ? "yes" : "no",
               first_full ? "set" : "empty");
        return -1;
    }
    return 0;
}

int rl_code_maps_register(const rl_code_map *const *maps, uint32_t count, rl_fn cold_first)
{
    if (!rl_core_cpu() || !g_table) {
        rl_log(RL_LOG_ERROR, "dispatch", "code maps registered before the units");
        return -1;
    }
    for (uint32_t k = 0; k < count; k++)
        if (code_map_walk(maps[k], cold_first, false) != 0)
            return -1;
    if (cold_first && count) {
        free(g_cold);
        g_cold = calloc(g_count, sizeof *g_cold);
        if (!g_cold)
            return -1;
        g_cold_first = cold_first;
    }
    for (uint32_t k = 0; k < count; k++)
        code_map_walk(maps[k], cold_first, true);
    g_maps = maps;
    g_map_count = count;
    return 0;
}

int rl_code_class(uint64_t a, const char **module)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    if (!cpu)
        return -1;
    for (uint32_t k = 0; k < g_map_count; k++) {
        const rl_code_map *m = g_maps[k];
        if (a < m->lo)
            continue;
        uint64_t i = (a - m->lo) >> cpu->code_shift;
        if (i >= (uint64_t)m->page_count * RL_CODE_PAGE_SLOTS)
            continue;
        uint32_t p = (uint32_t)(i / RL_CODE_PAGE_SLOTS), s = (uint32_t)(i % RL_CODE_PAGE_SLOTS);
        if (m->pages[p] >= m->pattern_count)
            return -1;
        const uint8_t *pat = m->patterns + (size_t)m->pages[p] * RL_CODE_PAGE_BYTES;
        if (module)
            *module = m->module;
        return (pat[s / 4] >> (2 * (s % 4))) & 3;
    }
    return -1;
}

rl_fn rl_core_cold(uint64_t target)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    if (!g_cold || !cpu || target < g_lo)
        return NULL;
    uint64_t i = (target - g_lo) >> cpu->code_shift;
    if (i >= g_count || !g_cold[i])
        return NULL;
    rl_fn f = g_cold[i];
    rl_fn expect = g_cold_first;
    if (__atomic_compare_exchange_n(&g_table[i], &expect, f, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        const char *mod = "?";
        uint64_t off = target;
        for (uint32_t k = 0; k < g_map_count; k++) {
            const rl_code_map *m = g_maps[k];
            if (target >= m->lo && ((target - m->lo) >> cpu->code_shift) < (uint64_t)m->page_count * RL_CODE_PAGE_SLOTS) {
                mod = m->module ? m->module : "?";
                off = target - m->lo;
            }
        }
        rl_log(RL_LOG_INFO, "cold", "first entry to cold code at 0x%llx (%s+0x%llx)", (unsigned long long)target, mod,
               (unsigned long long)off);
    }
    return f;
}

rl_miss_info rl_core_miss_classify(rl_ctx *c, uint64_t target, uintptr_t host_ret)
{
    const rl_cpu_desc *cpu = rl_core_cpu();
    rl_miss_info m = { target, RL_SITE_NONE, RL_MISS_ENTRY, 0 };
    rl_fn_hit h;
    if (host_ret && rl_fn_find_host(host_ret - 1, &h)) {
        m.reason = RL_MISS_CALL;
        m.host_ret = host_ret;
        if (cpu && cpu->call_site)
            m.site = cpu->call_site(c);
    }
    return m;
}

int rl_core_miss_at(rl_ctx *c, uint8_t *mem, const rl_miss_info *m)
{
    (void)mem;
    char what[160];
    if (rl_code_class(m->target, NULL) == RL_CODE_TRAP) {
        snprintf(what, sizeof what, "guest 0x%llx holds no instruction (padding or data)",
                 (unsigned long long)m->target);
        rl_report_stop(c, "no instruction", what, m, NULL, 0);
    } else {
        snprintf(what, sizeof what, "no function at guest 0x%llx", (unsigned long long)m->target);
        rl_report_stop(c, "missed code", what, m, NULL, 0);
    }
    return rl_pend_exit_process(c, RL_EXIT_SOFTWARE);
}

int rl_core_miss(rl_ctx *c, uint8_t *mem, uint64_t target, uintptr_t host_ret)
{
    rl_miss_info m = rl_core_miss_classify(c, target, host_ret);
    return rl_core_miss_at(c, mem, &m);
}

_Noreturn void rl_bad_resume(rl_ctx *c, uint8_t *mem, uint64_t fn_guest_addr)
{
    (void)mem;
    rl_miss_info m = { rl_common(c)->pend.pc, RL_SITE_NONE, RL_MISS_RESUME, 0 };
    char what[160];
    snprintf(what, sizeof what, "landing pad 0x%llx has no case in function 0x%llx",
             (unsigned long long)rl_common(c)->pend.pc, (unsigned long long)fn_guest_addr);
    rl_report_stop(c, "bad resume", what, &m, NULL, 0);
    rl_process_exit(RL_EXIT_SOFTWARE);
    abort();
}
