/**
 * @file mods.c
 * @brief Bundled code mods: the choice at start, the slot swap and the data
 * writes (relift/mods.h).
 */
#include "core.h"

#include <relift/mods.h>

#include <string.h>

/** @brief The value of a setting word: 1 on, 0 off, -1 not a bool. */
static int bool_word(const char *v)
{
    static const char *const yes[] = {"on", "true", "1", "yes"};
    static const char *const no[] = {"off", "false", "0", "no"};
    for (unsigned i = 0; i < 4; i++) {
        if (!strcmp(v, yes[i]))
            return 1;
        if (!strcmp(v, no[i]))
            return 0;
    }
    return -1;
}

/** @brief Applies one `key=value` to the mods it names. */
static void set_one(const rl_mods *m, const char *kv, bool *on)
{
    const char *eq = strchr(kv, '=');
    if (!eq)
        return;
    size_t n = (size_t)(eq - kv);
    for (uint32_t i = 0; i < m->count; i++) {
        const char *key = m->mods[i].key;
        if (strlen(key) != n || strncmp(key, kv, n))
            continue;
        int v = bool_word(eq + 1);
        if (v < 0)
            rl_log(RL_LOG_WARN, "mods", "%s: %s is not on or off; left as it was", key, eq + 1);
        else
            on[i] = v;
    }
}

int rl_mods_choose(const rl_mods *m, int argc, char **argv, bool *on)
{
    bool local[RL_MODS_MAX];
    if (!m)
        return 0;
    if (m->count > RL_MODS_MAX) {
        rl_log(RL_LOG_ERROR, "mods", "%u bundled mods; at most %u are supported", m->count, RL_MODS_MAX);
        return -1;
    }
    for (uint32_t i = 0; i < m->count; i++)
        local[i] = m->mods[i].on_by_default;
    for (int a = 1; a < argc; a++) {
        if (!strcmp(argv[a], "--relift-set") && a + 1 < argc)
            set_one(m, argv[++a], local);
        else if (!strncmp(argv[a], "--relift-set=", 13))
            set_one(m, argv[a] + 13, local);
    }
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
    for (uint32_t i = 0; i < m->count; i++) {
        if (local[i])
            rl_log(RL_LOG_INFO, "mods", "bundled mod %s is on", m->mods[i].key);
        if (on)
            on[i] = local[i];
    }
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
