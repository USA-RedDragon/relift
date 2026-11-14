/**
 * @file loader.c
 * @brief rl_switch_main: process start-up for a lifted Switch title. Maps the
 * NSO modules relift lifted, registers the static units and fsp-srv, links
 * the hooks' symbols, attaches the main thread and runs the entry.
 */
#include "loader.h"

#include "hos.h"

#include <relift/window.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#include <stdio.h>
#endif

static const rl_switch_hook *g_hooks[RL_SWITCH_HOOKS];
static uint32_t g_nhooks;

static const rl_switch_hook *g_auto[RL_SWITCH_HOOKS];
static uint32_t g_nauto;

int rl_switch_register_auto_hook(const rl_switch_hook *h)
{
    if (!h || !h->wants || g_nauto >= RL_SWITCH_HOOKS)
        return -1;
    g_auto[g_nauto++] = h;
    return 0;
}

int rl_switch_add_hook(const rl_switch_hook *h)
{
    if (!h || g_nhooks >= RL_SWITCH_HOOKS)
        return -1;
    g_hooks[g_nhooks++] = h;
    return 0;
}

static uint64_t align_up(uint64_t v) { return (v + HOS_PAGE - 1) & ~(HOS_PAGE - 1); }

static uint64_t align_down(uint64_t v) { return v & ~(HOS_PAGE - 1); }

static int in_range(uint64_t off, uint64_t size, uint64_t whole)
{
    return off <= whole && size <= whole - off;
}

/** @brief The mapped extent of a module: from its lowest segment to the end of data and bss. */
static void module_extent(const rl_switch_module *m, uint64_t *lo, uint64_t *hi)
{
    uint64_t end = m->data_off + m->data_size + m->bss_size;
    uint64_t t = m->text_off + m->text_size, r = m->ro_off + m->ro_size;
    *lo = m->base + m->text_off;
    *hi = m->base + align_up(end > t ? (end > r ? end : r) : (t > r ? t : r));
}

/** @brief Checks one module's description; prints why and returns -1 when it cannot be loaded. */
static int check_module(const rl_switch_module *m)
{
    uint64_t text_end, ro_end;
    if (!m->name)
        return -1;
    if ((m->base & (HOS_PAGE - 1)) || (m->text_off & (HOS_PAGE - 1)) || (m->ro_off & (HOS_PAGE - 1)) ||
        (m->data_off & (HOS_PAGE - 1))) {
        rl_log(RL_LOG_ERROR, "loader", "module %s: base and segment offsets must be 0x1000 aligned", m->name);
        return -1;
    }
    if (!m->image || !in_range(m->text_off, m->text_size, m->image_size) ||
        !in_range(m->ro_off, m->ro_size, m->image_size) || !in_range(m->data_off, m->data_size, m->image_size)) {
        rl_log(RL_LOG_ERROR, "loader", "module %s: a segment lies outside its image", m->name);
        return -1;
    }
    text_end = m->text_off + align_up(m->text_size);
    ro_end = m->ro_off + align_up(m->ro_size);
    if (text_end > m->ro_off || ro_end > m->data_off) {
        rl_log(RL_LOG_ERROR, "loader", "module %s: text, rodata and data must not overlap and must be in that order",
               m->name);
        return -1;
    }
    return 0;
}

static int map_segment(const rl_switch_module *m, const char *what, uint64_t off, uint64_t size, uint32_t perm,
                       uint32_t state)
{
    rl_result rc;
    if (!size)
        return 0;
    rc = rl_hos_map_segment(m->base + off, align_up(size), perm, state);
    if (rc != KR_SUCCESS) {
        rl_log(RL_LOG_ERROR, "loader", "module %s: mapping %s at 0x%llx (0x%llx bytes) failed: result 0x%x", m->name,
               what, (unsigned long long)(m->base + off), (unsigned long long)align_up(size), rc);
        return -1;
    }
    return 0;
}

static int load_module(const rl_switch_module *m)
{
    if (map_segment(m, "text", m->text_off, m->text_size, HOS_PERM_RX, MS_CODE_STATIC) != 0 ||
        map_segment(m, "rodata", m->ro_off, m->ro_size, HOS_PERM_R, MS_CODE_STATIC) != 0 ||
        map_segment(m, "data", m->data_off, m->data_size + m->bss_size, HOS_PERM_RW, MS_CODE_MUTABLE) != 0)
        return -1;
    if (m->text_size)
        rl_vm_kernel_write(&hos.vm, m->base + m->text_off, m->image + m->text_off, m->text_size);
    if (m->ro_size)
        rl_vm_kernel_write(&hos.vm, m->base + m->ro_off, m->image + m->ro_off, m->ro_size);
    if (m->data_size)
        rl_vm_kernel_write(&hos.vm, m->base + m->data_off, m->image + m->data_off, m->data_size);
    return 0;
}

enum {
    DT_NULL = 0,
    DT_PLTRELSZ = 2,
    DT_HASH = 4,
    DT_STRTAB = 5,
    DT_SYMTAB = 6,
    DT_RELA = 7,
    DT_RELASZ = 8,
    DT_STRSZ = 10,
    DT_JMPREL = 23,
    R_ABS64 = 257,
    R_GLOB_DAT = 1025,
    R_JUMP_SLOT = 1026,
    SYM_SIZE = 24,
    RELA_SIZE = 24
};

typedef struct dyn_info {
    uint64_t symtab, nsyms, strtab, strsz;
    uint64_t rela[2], relasz[2];
} dyn_info;

static bool img64(const rl_switch_module *m, uint64_t off, uint64_t *v)
{
    if (!in_range(off, 8, m->image_size))
        return false;
    memcpy(v, m->image + off, 8);
    return true;
}

static bool img32(const rl_switch_module *m, uint64_t off, uint32_t *v)
{
    if (!in_range(off, 4, m->image_size))
        return false;
    memcpy(v, m->image + off, 4);
    return true;
}

/** @brief Reads the dynamic section MOD0 names; false when the module has none. */
static bool dyn_of(const rl_switch_module *m, dyn_info *d)
{
    uint32_t mod0, magic, rel;
    if (!img32(m, m->text_off + 4, &mod0) || !img32(m, mod0, &magic) || memcmp(&magic, "MOD0", 4) ||
        !img32(m, (uint64_t)mod0 + 4, &rel))
        return false;
    uint64_t at = (uint64_t)mod0 + (uint64_t)(int64_t)(int32_t)rel, hash = 0;
    memset(d, 0, sizeof *d);
    for (;; at += 16) {
        uint64_t tag, val;
        if (!img64(m, at, &tag) || !img64(m, at + 8, &val))
            return false;
        if (tag == DT_NULL)
            break;
        switch (tag) {
        case DT_SYMTAB:
            d->symtab = val;
            break;
        case DT_STRTAB:
            d->strtab = val;
            break;
        case DT_STRSZ:
            d->strsz = val;
            break;
        case DT_HASH:
            hash = val;
            break;
        case DT_RELA:
            d->rela[0] = val;
            break;
        case DT_RELASZ:
            d->relasz[0] = val;
            break;
        case DT_JMPREL:
            d->rela[1] = val;
            break;
        case DT_PLTRELSZ:
            d->relasz[1] = val;
            break;
        default:
            break;
        }
    }
    uint32_t nchain;
    if (hash && img32(m, hash + 4, &nchain))
        d->nsyms = nchain;
    else if (d->strtab > d->symtab)
        d->nsyms = (d->strtab - d->symtab) / SYM_SIZE;
    return d->symtab && d->strtab && in_range(d->strtab, d->strsz, m->image_size);
}

/** @brief Name, section index and value of symbol i; NULL when it is out of range. */
static const char *sym_of(const rl_switch_module *m, const dyn_info *d, uint64_t i, uint16_t *shndx, uint64_t *value)
{
    uint64_t at = d->symtab + i * SYM_SIZE;
    uint32_t name;
    if (i >= d->nsyms || !in_range(at, SYM_SIZE, m->image_size) || !img32(m, at, &name) || name >= d->strsz)
        return NULL;
    memcpy(shndx, m->image + at + 6, 2);
    img64(m, at + 8, value);
    const char *s = (const char *)m->image + d->strtab + name;
    return memchr(s, 0, d->strsz - name) ? s : NULL;
}

static rl_fn hook_replace(const char *name)
{
    for (uint32_t h = 0; h < g_nhooks; h++)
        if (g_hooks[h]->replace) {
            rl_fn f = g_hooks[h]->replace(g_hooks[h]->user, name);
            if (f)
                return f;
        }
    return NULL;
}

static uint64_t hook_import(const char *name)
{
    for (uint32_t h = 0; h < g_nhooks; h++)
        if (g_hooks[h]->import) {
            uint64_t a = g_hooks[h]->import(g_hooks[h]->user, name);
            if (a)
                return a;
        }
    return 0;
}

/** @brief Replaces the module's definitions the hooks name; returns the number replaced or -1. */
static int replace_exports(const rl_switch_module *m, const dyn_info *d)
{
    rl_fn_entry *e = NULL;
    uint32_t n = 0;
    for (uint64_t i = 1; i < d->nsyms; i++) {
        uint16_t shndx;
        uint64_t value;
        const char *name = sym_of(m, d, i, &shndx, &value);
        rl_fn f = name && shndx && value ? hook_replace(name) : NULL;
        if (!f)
            continue;
        rl_fn_entry *ne = realloc(e, (n + 1) * sizeof *e);
        if (!ne) {
            free(e);
            return -1;
        }
        e = ne;
        e[n++] = (rl_fn_entry){ m->base + value, 4, RL_FN_OVERRIDE, f };
    }
    if (!n)
        return 0;
    rl_unit *u = calloc(1, sizeof *u);
    if (!u) {
        free(e);
        return -1;
    }
    *u = (rl_unit){
        .abi_version = RL_ABI_VERSION,
        .ctx_size = sizeof(rl_ctx),
        .fn_count = n,
        .fns = e,
        .name = m->name,
    };
    const rl_unit *list[1] = { u };
    return rl_units_register(list, 1) == 0 ? (int)n : -1;
}

/** @brief True when a module of p other than m defines name: the guest's own loader binds those. */
static bool defined_elsewhere(const rl_switch_program *p, const rl_switch_module *m, const char *name)
{
    for (uint32_t i = 0; i < p->module_count; i++) {
        const rl_switch_module *o = &p->modules[i];
        dyn_info d;
        if (o == m || !o->image || !dyn_of(o, &d))
            continue;
        for (uint64_t k = 1; k < d.nsyms; k++) {
            uint16_t shndx;
            uint64_t value;
            const char *s = sym_of(o, &d, k, &shndx, &value);
            if (s && shndx && value && !strcmp(s, name))
                return true;
        }
    }
    return false;
}

/**
 * @brief Writes the module's relocations against undefined symbols the hooks
 * resolve and no other module defines; returns how many. An import another
 * module defines is left to the guest's dynamic loader, which binds it to that
 * module's export (replaced by the hook when the hook replaces it) and which
 * checks that every PLT slot still holds its link-time value before binding.
 */
static int resolve_imports(const rl_switch_program *p, const rl_switch_module *m, const dyn_info *d)
{
    uint64_t lo, hi;
    int n = 0;
    module_extent(m, &lo, &hi);
    for (int t = 0; t < 2; t++)
        for (uint64_t at = d->rela[t]; d->rela[t] && at + RELA_SIZE <= d->rela[t] + d->relasz[t]; at += RELA_SIZE) {
            uint64_t off, info, addend;
            if (!img64(m, at, &off) || !img64(m, at + 8, &info) || !img64(m, at + 16, &addend))
                break;
            uint32_t type = (uint32_t)info;
            if (type != R_ABS64 && type != R_GLOB_DAT && type != R_JUMP_SLOT)
                continue;
            uint16_t shndx;
            uint64_t value;
            const char *name = sym_of(m, d, info >> 32, &shndx, &value);
            uint64_t where = m->base + off;
            if (!name || shndx || where < lo || where + 8 > hi)
                continue;
            uint64_t a = hook_import(name);
            if (!a || defined_elsewhere(p, m, name))
                continue;
            a += addend;
            rl_vm_kernel_write(&hos.vm, where, &a, 8);
            n++;
        }
    return n;
}

static int link_module(const rl_switch_program *p, const rl_switch_module *m)
{
    dyn_info d;
    if (!g_nhooks || !dyn_of(m, &d))
        return 0;
    int replaced = replace_exports(m, &d);
    if (replaced < 0) {
        rl_log(RL_LOG_ERROR, "loader", "module %s: replacing its functions failed", m->name);
        return -1;
    }
    int imported = resolve_imports(p, m, &d);
    if (replaced || imported)
        rl_log(RL_LOG_INFO, "loader", "module %s: hooks replaced functions: %d, resolved imports: %d", m->name,
               replaced, imported);
    return 0;
}

typedef struct main_args {
    const rl_switch_program *p;
} main_args;

static int run_program(void *arg)
{
    const rl_switch_program *p = ((main_args *)arg)->p;
    rl_hos_config cfg;
    uint64_t lo = UINT64_MAX, hi = 0, top = 0;
    rl_ctx *c;
    rl_result rc;
    int ret;
    if (!p || !p->modules || p->module_count == 0) {
        rl_log(RL_LOG_ERROR, "loader", "the program has no modules");
        return 1;
    }
    for (uint32_t i = 0; i < p->module_count; i++) {
        uint64_t mlo, mhi;
        if (check_module(&p->modules[i]) != 0)
            return 1;
        module_extent(&p->modules[i], &mlo, &mhi);
        if (mlo < lo)
            lo = mlo;
        if (mhi > hi)
            hi = mhi;
    }
    {
        const rl_switch_module *f = &p->modules[0];
        if (p->entry < f->base + f->text_off || p->entry >= f->base + f->text_off + align_up(f->text_size)) {
            rl_log(RL_LOG_ERROR, "loader", "entry 0x%llx is outside the text of module %s", (unsigned long long)p->entry,
                   f->name);
            return 1;
        }
    }
    rl_hos_default_config(&cfg);
    cfg.address_space_type = p->address_space_type;
    cfg.code_start = align_down(p->modules[0].base);
    if (hi - cfg.code_start > cfg.code_size)
        cfg.code_size = hi - cfg.code_start;
    cfg.program_id = p->program_id;
    cfg.main_prio = p->main_prio;
    cfg.main_core = p->main_core;
    if (p->main_stack_size)
        cfg.main_stack_size = p->main_stack_size;
    if (rl_hos_init(&cfg) != 0) {
        rl_log(RL_LOG_ERROR, "loader", "cannot start the process (address space type %u)", p->address_space_type);
        return 1;
    }
    if (lo < hos.lay.code_start || hi > hos.lay.code_end) {
        rl_log(RL_LOG_ERROR, "loader", "modules span 0x%llx-0x%llx but the code region is 0x%llx-0x%llx",
               (unsigned long long)lo, (unsigned long long)hi, (unsigned long long)hos.lay.code_start,
               (unsigned long long)hos.lay.code_end);
        return 1;
    }
    uint64_t code_free = align_up(hi);
    for (uint32_t h = 0; h < g_nhooks; h++)
        if (g_hooks[h]->start && g_hooks[h]->start(g_hooks[h]->user, &code_free) != 0) {
            rl_log(RL_LOG_ERROR, "loader", "hook %s failed to start", g_hooks[h]->name ? g_hooks[h]->name : "?");
            return 1;
        }
    if (rl_dispatch_reserve(lo, hi) != 0) {
        rl_log(RL_LOG_ERROR, "loader", "cannot allocate the dispatch table");
        return 1;
    }
    if (p->units && p->unit_count) {
        if (rl_units_register(p->units, p->unit_count) != 0) {
            rl_log(RL_LOG_ERROR, "loader", "a static unit was refused");
            return 1;
        }
    } else {
        rl_log(RL_LOG_WARN, "loader", "no static units are linked into this program");
    }
    for (uint32_t i = 0; i < p->module_count; i++)
        if (load_module(&p->modules[i]) != 0)
            return 1;
    for (uint32_t i = 0; i < p->module_count; i++)
        if (link_module(p, &p->modules[i]) != 0)
            return 1;
    c = rl_hos_attach_thread(p->main_prio, p->main_core);
    if (!c) {
        rl_log(RL_LOG_ERROR, "loader", "cannot attach the main thread");
        return 1;
    }
    rc = rl_hos_map_main_stack(cfg.main_stack_size, &top);
    if (rc != KR_SUCCESS) {
        rl_log(RL_LOG_ERROR, "loader", "cannot map the main stack (0x%llx bytes): result 0x%x",
               (unsigned long long)cfg.main_stack_size, rc);
        return 1;
    }
    ret = rl_hos_run_main(p->entry, top);
    if (ret != RL_OK && rl_common(c)->pend.kind == RL_PEND_EXIT_PROCESS)
        return (int)rl_common(c)->pend.code;
    return 0;
}

static bool is_added(const rl_switch_hook *h)
{
    for (uint32_t i = 0; i < g_nhooks; i++)
        if (g_hooks[i] == h)
            return true;
    return false;
}

/** @brief True when a dynamic symbol of any module (defined or imported) is one the hook wants. */
static bool wants_hook(const rl_switch_program *p, const rl_switch_hook *h)
{
    if (!h->wants || !p || !p->modules)
        return false;
    for (uint32_t i = 0; i < p->module_count; i++) {
        const rl_switch_module *m = &p->modules[i];
        dyn_info d;
        if (!m->image || !dyn_of(m, &d))
            continue;
        for (uint64_t k = 1; k < d.nsyms; k++) {
            uint16_t shndx;
            uint64_t value;
            const char *name = sym_of(m, &d, k, &shndx, &value);
            if (name && *name && h->wants(h->user, name))
                return true;
        }
    }
    return false;
}

int rl_switch_main(const rl_switch_program *p, int argc, char **argv)
{
    (void)argc;
    (void)argv;
#if defined(_WIN32)
    _setmode(_fileno(stdout), _O_BINARY);
    _setmode(_fileno(stderr), _O_BINARY);
#endif
    main_args a = { p };
    uint32_t base = g_nhooks;
    for (uint32_t i = 0; i < g_nauto; i++) {
        const rl_switch_hook *h = g_auto[i];
        if (is_added(h) || g_nhooks >= RL_SWITCH_HOOKS || !wants_hook(p, h))
            continue;
        g_hooks[g_nhooks++] = h;
        rl_log(RL_LOG_INFO, "loader", "hook %s turned on by the program's symbols", h->name ? h->name : "?");
    }
    int ret = g_nhooks ? rl_window_main(run_program, &a) : run_program(&a);
    while (g_nhooks > base)
        g_hooks[--g_nhooks] = NULL;
    return ret;
}
