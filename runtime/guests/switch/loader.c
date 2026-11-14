/**
 * @file loader.c
 * @brief rl_switch_main: process start-up for a lifted Switch title. Maps the
 * NSO modules relift lifted, registers the static units and fsp-srv, attaches
 * the main thread and runs the entry.
 */
#include "loader.h"

#include "hos.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/** @brief Weak so the runtime library links before the program defines its units. */

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

int rl_switch_main(const rl_switch_program *p, int argc, char **argv)
{
    rl_hos_config cfg;
    uint64_t lo = UINT64_MAX, hi = 0, top = 0;
    rl_ctx *c;
    rl_result rc;
    int ret;
    (void)argc;
    (void)argv;
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
