/**
 * @file mods.h
 * @brief Bundled code mods: mods the game repo declares, built into the
 * binary and switched on or off at start.
 *
 * relift lifts every function a bundled mod patches once per combination of
 * the mods that patch it (the original is combination 0), and routes every
 * call and dispatch entry of such a function through one slot. At start the
 * guest layer calls rl_mods_choose, which reads each mod's setting, refuses
 * two exclusive mods on together, and points every slot at the variant for
 * the mods that are on; then rl_mods_each_data gives it the bytes the mods
 * that are on write into guest memory, in mod order.
 */
#ifndef RELIFT_MODS_H
#define RELIFT_MODS_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief Most bundled mods in one binary. */
#define RL_MODS_MAX 64u

/** @brief Bytes a mod writes into guest memory when it is on. */
typedef struct rl_mod_data {
    /** @brief Guest address. */
    uint64_t addr;
    /** @brief Number of bytes. */
    uint32_t size;
    /** @brief The bytes. */
    const uint8_t *bytes;
} rl_mod_data;

/** @brief One bundled mod. */
typedef struct rl_mod {
    /** @brief Its setting key (`mod.<name>`). */
    const char *key;
    /** @brief Whether it is on when no setting names it. */
    bool on_by_default;
    /** @brief Number of entries in data. */
    uint32_t data_count;
    /** @brief What it writes, in patch order. */
    const rl_mod_data *data;
} rl_mod;

/**
 * @brief A function some bundled mods patch: its slot and its variants,
 * indexed by a mask over mods (bit k set when mods[k] is on).
 */
typedef struct rl_mod_fn {
    /** @brief Guest entry address, for logs. */
    uint64_t guest_addr;
    /** @brief The slot every call and the dispatch entry go through. */
    rl_fn *slot;
    /** @brief Number of mods that patch the function (at most 8). */
    uint32_t mod_count;
    /** @brief Their indices in rl_mods.mods, in mod order. */
    const uint16_t *mods;
    /** @brief 1 << mod_count variants; 0 for a combination that cannot be on (exclusive mods). */
    const rl_fn *variants;
} rl_mod_fn;

/** @brief Every bundled mod of a binary. */
typedef struct rl_mods {
    /** @brief Number of mods. */
    uint32_t count;
    /** @brief The mods, in their declared order. */
    const rl_mod *mods;
    /** @brief Number of patched functions. */
    uint32_t fn_count;
    /** @brief The patched functions. */
    const rl_mod_fn *fns;
    /** @brief Number of exclusive pairs. */
    uint32_t exclusive_count;
    /** @brief Pairs of mod indices that cannot be on together, two entries per pair. */
    const uint16_t *exclusive;
} rl_mods;

/**
 * @brief Chooses which mods are on and points every slot at its variant.
 *
 * A mod is on when `--relift-set <key>=on` (or true, 1, yes) is given, off
 * with off (false, 0, no), and otherwise as its default; later arguments win.
 * on, when not NULL, receives one flag per mod. Returns 0, or -1 after
 * logging why when two exclusive mods are on or a combination has no
 * variant.
 */
RL_RUNTIME_API int rl_mods_choose(const rl_mods *m, int argc, char **argv, bool *on);

/** @brief Called by rl_mods_each_data for each write of a mod that is on. */
typedef void rl_mod_write_fn(void *user, uint64_t addr, const uint8_t *bytes, uint32_t size);

/** @brief Calls write for every rl_mod_data of the mods on, in mod order. */
RL_RUNTIME_API void rl_mods_each_data(const rl_mods *m, const bool *on, rl_mod_write_fn *write, void *user);

#endif
