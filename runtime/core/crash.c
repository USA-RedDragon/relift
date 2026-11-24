/**
 * @file crash.c
 * @brief The function maps and the crash report: guest address of the
 * fault, the guest function at the faulting pc, a guest backtrace from the
 * host frames that land in lifted code, and the host backtrace. Formatting
 * uses only stack buffers, so it runs inside a signal or exception handler.
 */
#include "crash.h"
#include "fiber.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct host_ix {
    uintptr_t host;
    const rl_fn_map *map;
    const rl_fn_info *fn;
} host_ix;

static const rl_fn_map **g_maps;
static uint32_t g_map_count;
static host_ix *g_ix;
static uint32_t g_ix_count;
static const uint8_t *g_base;
static uintptr_t g_lo;
static uint64_t g_len;
static rl_crash_extra *g_extra;

/** @brief A host function without a lifted-code section is matched only this far past its start. */
#define RL_CRASH_NEAR_LIMIT 0x10000u

static int ix_cmp(const void *a, const void *b)
{
    uintptr_t x = ((const host_ix *)a)->host, y = ((const host_ix *)b)->host;
    return x < y ? -1 : x > y;
}

void rl_fn_maps_reset(void)
{
    free(g_maps);
    free(g_ix);
    g_maps = NULL;
    g_ix = NULL;
    g_map_count = 0;
    g_ix_count = 0;
}

int rl_fn_maps_check(const rl_fn_map *const *maps, uint32_t count)
{
    for (uint32_t k = 0; k < count; k++) {
        const rl_fn_map *m = maps[k];
        if (m->version != RL_FN_MAP_VERSION)
            return -1;
        for (uint32_t i = 1; i < m->count; i++)
            if (m->fns[i].guest_addr < m->fns[i - 1].guest_addr + m->fns[i - 1].guest_size)
                return -1;
    }
    return 0;
}

/** @brief Whether the entries are in host address order (a unit's functions usually are, as its compiler laid them out). */
static bool ix_sorted(const host_ix *a, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++)
        if (a[i].host < a[i - 1].host)
            return false;
    return true;
}

int rl_fn_maps_register(const rl_fn_map *const *maps, uint32_t count)
{
    uint64_t fns = g_ix_count;
    if (rl_fn_maps_check(maps, count) != 0)
        return -1;
    for (uint32_t k = 0; k < count; k++)
        fns += maps[k]->count;
    const rl_fn_map **nm = malloc(((size_t)g_map_count + count + 1) * sizeof *nm);
    host_ix *ni = malloc((size_t)(fns + 1) * sizeof *ni);
    if (!nm || !ni) {
        free(nm);
        free(ni);
        return -1;
    }
    if (g_map_count)
        memcpy(nm, g_maps, g_map_count * sizeof *nm);
    if (g_ix_count)
        memcpy(ni, g_ix, g_ix_count * sizeof *ni);
    uint32_t n = g_ix_count;
    for (uint32_t k = 0; k < count; k++) {
        nm[g_map_count + k] = maps[k];
        for (uint32_t i = 0; i < maps[k]->count; i++)
            ni[n++] = (host_ix){ (uintptr_t)maps[k]->fns[i].fn, maps[k], &maps[k]->fns[i] };
    }
    /* The old index is sorted; sort only what is new (often already in
     * order) and merge the two runs. */
    host_ix *fresh = ni + g_ix_count;
    uint32_t added = n - g_ix_count;
    if (!ix_sorted(fresh, added))
        qsort(fresh, added, sizeof *fresh, ix_cmp);
    if (g_ix_count && added && fresh[0].host < ni[g_ix_count - 1].host) {
        host_ix *tmp = malloc((size_t)added * sizeof *tmp);
        if (!tmp) {
            free(nm);
            free(ni);
            return -1;
        }
        memcpy(tmp, fresh, (size_t)added * sizeof *tmp);
        uint32_t i = g_ix_count, j = added, o = n;
        while (j > 0) {
            if (i > 0 && ni[i - 1].host > tmp[j - 1].host)
                ni[--o] = ni[--i];
            else
                ni[--o] = tmp[--j];
        }
        free(tmp);
    }
    free(g_maps);
    free(g_ix);
    g_maps = nm;
    g_map_count += count;
    g_ix = ni;
    g_ix_count = n;
    return 0;
}

static bool in_text(const rl_fn_map *m, uintptr_t pc)
{
    return m->text_lo && m->text_hi && pc >= (uintptr_t)m->text_lo && pc < (uintptr_t)m->text_hi;
}

bool rl_fn_find_host(uintptr_t pc, rl_fn_hit *out)
{
    uint32_t lo = 0, hi = g_ix_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (g_ix[mid].host <= pc)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == 0)
        return false;
    const host_ix *e = &g_ix[lo - 1];
    bool exact = in_text(e->map, pc);
    if (!exact) {
        if (e->map->text_lo && e->map->text_hi)
            return false;
        if (pc - e->host >= RL_CRASH_NEAR_LIMIT || (lo < g_ix_count && pc >= g_ix[lo].host))
            return false;
    }
    out->map = e->map;
    out->fn = e->fn;
    out->off = pc - e->host;
    out->exact = exact;
    return true;
}

bool rl_fn_find_guest(uint64_t a, rl_fn_hit *out)
{
    for (uint32_t k = 0; k < g_map_count; k++) {
        const rl_fn_map *m = g_maps[k];
        uint32_t lo = 0, hi = m->count;
        while (lo < hi) {
            uint32_t mid = lo + (hi - lo) / 2;
            if (m->fns[mid].guest_addr <= a)
                lo = mid + 1;
            else
                hi = mid;
        }
        if (lo && a - m->fns[lo - 1].guest_addr < m->fns[lo - 1].guest_size) {
            out->map = m;
            out->fn = &m->fns[lo - 1];
            out->off = a - m->fns[lo - 1].guest_addr;
            out->exact = true;
            return true;
        }
    }
    return false;
}

void rl_crash_set_space(const uint8_t *base, uintptr_t lo, uint64_t len)
{
    g_base = base;
    g_lo = lo;
    g_len = len;
}

void rl_crash_set_extra(rl_crash_extra *fn)
{
    g_extra = fn;
}

static rl_crash_file_fn *volatile g_file;

void rl_crash_set_file(rl_crash_file_fn *fn) { g_file = fn; }

typedef struct out_buf {
    char *p;
    size_t n;
    size_t len;
} out_buf;

static void put(out_buf *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(out_buf *o, const char *fmt, ...)
{
    if (o->len + 1 >= o->n)
        return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(o->p + o->len, o->n - o->len, fmt, ap);
    va_end(ap);
    if (w > 0)
        o->len = o->len + (size_t)w < o->n ? o->len + (size_t)w : o->n - 1;
}

static const char *fn_name(const rl_fn_info *f)
{
    return f->guest_name ? f->guest_name : f->host_sym ? f->host_sym : "?";
}

static void put_guest_fn(out_buf *o, const rl_fn_hit *h)
{
    const rl_fn_map *m = h->map;
    put(o, "guest 0x%llx %s+0x%llx %s%s", (unsigned long long)h->fn->fn_addr, m->module ? m->module : "?",
        (unsigned long long)(h->fn->fn_addr - m->module_base), fn_name(h->fn), h->exact ? "" : " (nearest)");
}

static uintptr_t lookup_pc(const rl_crash_info *ci, int i)
{
    return i == 0 ? ci->frames[0] : ci->frames[i] - 1;
}

size_t rl_crash_format(const rl_crash_info *ci, char *buf, size_t n)
{
    out_buf o = { buf, n, 0 };
    buf[0] = 0;
    const char *t = "[relift crash] ";
    put(&o, "%s%s", t, ci->kind);
    if (ci->has_addr) {
        put(&o, " at host %p", (void *)ci->addr);
        if (g_len && ci->addr >= g_lo && ci->addr - g_lo < g_len)
            put(&o, " = guest address 0x%llx", (unsigned long long)(ci->addr - (uintptr_t)g_base));
        else
            put(&o, " (outside guest memory)");
    }
    put(&o, ", host pc %p\n", (void *)ci->pc);
    bool guard = false;
    const rl_fiber *f = ci->sp ? rl_fiber_find(ci->sp, &guard) : NULL;
    bool addr_guard = false;
    const rl_fiber *af = ci->has_addr ? rl_fiber_find(ci->addr, &addr_guard) : NULL;
    if (af && addr_guard)
        f = af;
    if (f && (ci->stack_overflow || addr_guard))
        put(&o, "%sstack overflow on %s (fiber stack %p-%p, guard below)\n", t, f->label[0] ? f->label : "a fiber",
            (void *)f->limit, (void *)f->top);
    else if (f)
        put(&o, "%son %s (fiber stack %p-%p)\n", t, f->label[0] ? f->label : "a fiber", (void *)f->limit, (void *)f->top);
    else if (ci->stack_overflow)
        put(&o, "%sstack overflow on the host thread's own stack\n", t);
    rl_fn_hit h;
    if (rl_fn_find_host(ci->pc, &h)) {
        put(&o, "%sin guest function ", t);
        put_guest_fn(&o, &h);
        put(&o, ", host %s+0x%llx\n", h.fn->host_sym ? h.fn->host_sym : "?", (unsigned long long)h.off);
    } else {
        put(&o, "%sin host code, not in a lifted function\n", t);
    }
    put(&o, "%sguest backtrace (lifted frames, innermost first; tail calls leave no frame):\n", t);
    int g = 0;
    for (int i = 0; i < ci->nframes; i++)
        if (rl_fn_find_host(lookup_pc(ci, i), &h)) {
            put(&o, "%s  #%d ", t, g++);
            put_guest_fn(&o, &h);
            put(&o, "\n");
        }
    if (g == 0)
        put(&o, "%s  (no lifted frame)\n", t);
    put(&o, "%shost backtrace:\n", t);
    for (int i = 0; i < ci->nframes; i++) {
        char sym[256];
        uintptr_t pc = ci->frames[i];
        put(&o, "%s  #%d %p ", t, i, (void *)pc);
        if (rl_fn_find_host(lookup_pc(ci, i), &h))
            put(&o, "%s+0x%llx (%s)", h.fn->host_sym ? h.fn->host_sym : "?",
                (unsigned long long)(pc - (uintptr_t)h.fn->fn), fn_name(h.fn));
        else if (rl_crash_os_symbolize(pc, sym, sizeof sym))
            put(&o, "%s", sym);
        else
            put(&o, "?");
        put(&o, "\n");
    }
    if (g_extra) {
        char extra[512];
        extra[0] = 0;
        g_extra(extra, sizeof extra);
        if (extra[0])
            put(&o, "%s%s\n", t, extra);
    }
    return o.len;
}

void rl_crash_report(const rl_crash_info *ci)
{
    char buf[16384];
    size_t len = rl_crash_format(ci, buf, sizeof buf);
    rl_crash_os_write(buf, len);
    rl_crash_file_fn *f = g_file;
    if (f)
        f(buf, len);
}
