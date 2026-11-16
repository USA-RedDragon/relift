/**
 * @file mods.c
 * @brief Bundled code mods: the choice at start, the slot swap and the data
 * writes (relift/mods.h).
 */
#include "core.h"

#include <relift/mods.h>
#include <relift/settings.h>

#include <stdio.h>

int rl_mods_choose(const rl_mods *m, bool *on)
{
    bool local[RL_MODS_MAX];
    if (!m)
        return 0;
    if (m->count > RL_MODS_MAX) {
        rl_log(RL_LOG_ERROR, "mods", "%u bundled mods; at most %u are supported", m->count, RL_MODS_MAX);
        return -1;
    }
    for (uint32_t i = 0; i < m->count; i++)
        local[i] = rl_setting_bool(m->mods[i].key);
    for (uint32_t k = 0; k < m->exclusive_count; k++) {
        uint16_t x = m->exclusive[2 * k], y = m->exclusive[2 * k + 1];
        if (x < m->count && y < m->count && local[x] && local[y]) {
            rl_log(RL_LOG_ERROR, "mods", "%s and %s are exclusive; turn one of them off", m->mods[x].key,
                   m->mods[y].key);
            return -1;
        }
    }
    for (uint32_t f = 0; f < m->fn_count; f++) {
        const rl_mod_fn *fn = &m->fns[f];
        uint32_t mask = 0;
        for (uint32_t k = 0; k < fn->mod_count && k < 8; k++)
            if (fn->mods[k] < m->count && local[fn->mods[k]])
                mask |= 1u << k;
        rl_fn v = fn->variants[mask];
        if (!v) {
            rl_log(RL_LOG_ERROR, "mods", "no variant of 0x%llx for the mods on", (unsigned long long)fn->guest_addr);
            return -1;
        }
        __atomic_store_n(fn->slot, v, __ATOMIC_RELEASE);
    }
    char set[256];
    size_t k = 0;
    set[0] = 0;
    for (uint32_t i = 0; i < m->count; i++) {
        if (local[i]) {
            rl_log(RL_LOG_INFO, "mods", "bundled mod %s is on", m->mods[i].key);
            int w = snprintf(set + k, sizeof set - k, "%s%s", k ? " " : "", m->mods[i].key);
            if (w > 0 && (size_t)w < sizeof set - k)
                k += (size_t)w;
        }
        if (on)
            on[i] = local[i];
    }
    rl_report_info("mods", k ? set : "none");
    return 0;
}

void rl_mods_each_data(const rl_mods *m, const bool *on, rl_mod_write_fn *write, void *user)
{
    if (!m || !on || !write)
        return;
    for (uint32_t i = 0; i < m->count; i++) {
        if (!on[i])
            continue;
        for (uint32_t d = 0; d < m->mods[i].data_count; d++) {
            const rl_mod_data *x = &m->mods[i].data[d];
            write(user, x->addr, x->bytes, x->size);
        }
    }
}
