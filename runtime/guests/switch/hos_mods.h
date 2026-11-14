/**
 * @file hos_mods.h
 * @brief Run-time data mods for a Switch program: romfs layers in front of
 * the base tree and Atmosphere dmnt cheats that write RAM every frame.
 *
 * The launcher passes the ordered mod list as repeated `--relift-mods <path>`
 * flags or the `mods` array of its settings file (rl_settings_mod, in order);
 * $RELIFT_MODS (host path list: ':' separated, ';' on Windows) is a debug
 * override used only when no flag was given. Later entries win. Each entry is
 * one of:
 *  - a mod folder holding romfs/ or cheats/ (an Atmosphere contents/<title id>
 *    folder, or one Ryujinx mod folder);
 *  - an Atmosphere contents folder (or the folder holding contents/): its
 *    <title id>/ folder for this program is the mod;
 *  - a Ryujinx mods folder: every sub-folder with romfs/ or cheats/ is a mod,
 *    in name order (a sub-folder named by this program's title id is entered
 *    first, as Ryujinx lays out mods/contents/<title id>/<mod>).
 * Exefs replacement and exefs patches are build-time mods and ignored here.
 */
#ifndef RELIFT_HOS_MODS_H
#define RELIFT_HOS_MODS_H

#include "loader.h"

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Folder layouts a mod can come in. */
enum rl_mod_layout {
    /** @brief contents/<title id>/ */
    RL_MOD_ATMOSPHERE = 0,
    /** @brief mods/<mod name>/ */
    RL_MOD_RYUJINX = 1
};

/** @brief One active data mod. */
typedef struct rl_hos_mod_info {
    /** @brief Mod name: the folder's name. */
    const char *name;
    /** @brief Mod folder. */
    const char *path;
    /** @brief enum rl_mod_layout. */
    int layout;
    /** @brief True when the mod has a romfs/ folder. */
    bool romfs;
    /** @brief True when the mod has a cheats/ folder. */
    bool has_cheats;
    /** @brief Cheats loaded for the running program (after rl_hos_mods_attach). */
    uint32_t cheats;
    /** @brief Cheats that write lifted code and need a rebuild. */
    uint32_t cheats_need_rebuild;
} rl_hos_mod_info;

/**
 * @brief Replaces the active mod list with the entries of list (see above),
 * resolved for program_id. Warns about entries that hold nothing. Returns
 * the number of mods.
 */
RL_RUNTIME_API size_t rl_hos_mods_set(const char *list, uint64_t program_id);
/**
 * @brief Like rl_hos_mods_set with the entries already split, so paths may
 * hold any character.
 */
RL_RUNTIME_API size_t rl_hos_mods_set_paths(const char *const *paths, size_t n, uint64_t program_id);
/** @brief Forgets all mods and cheats. */
RL_RUNTIME_API void rl_hos_mods_clear(void);
/** @brief Number of active mods. */
RL_RUNTIME_API size_t rl_hos_mods_count(void);
/** @brief Mod i in order, or NULL. */
RL_RUNTIME_API const rl_hos_mod_info *rl_hos_mods_info(size_t i);
/** @brief Number of romfs file conflicts found when the data storage was built. */
RL_RUNTIME_API size_t rl_hos_mods_conflict_count(void);
/** @brief Conflict i as the message that was logged, or NULL. */
RL_RUNTIME_API const char *rl_hos_mods_conflict(size_t i);

/**
 * @brief Serves the base romfs tree (may be NULL) with the active mods'
 * romfs folders layered in front, as the program's data storage. Call before
 * the guest opens the storage. Without romfs mods this is rl_hos_fs_set_romfs.
 */
RL_RUNTIME_API void rl_hos_mods_apply_romfs(const char *base);

/**
 * @brief Tells the mods which module is main and its build id (lower-case
 * hex, NULL when unknown), and loads the cheats whose file names match it.
 * rl_switch_main calls this once the modules are mapped.
 */
RL_RUNTIME_API void rl_hos_mods_attach(uint64_t main_base, const char *build_id);

/** @brief Runs every cheat once. The runtime calls it each time the guest queues a frame. */
RL_RUNTIME_API void rl_hos_mods_frame(void);

/** @brief Sets the function that returns the held keys for cheat key conditions, in dmnt's mask encoding. */
RL_RUNTIME_API void rl_hos_mods_set_keys(uint64_t (*fn)(void *user), void *user);

/**
 * @brief Writes the active data mods, their cheats and the romfs conflicts as
 * a JSON object into buf (NUL-terminated, cut at cap) and returns the length
 * the whole text needs, as snprintf does.
 */
RL_RUNTIME_API size_t rl_hos_mods_json(char *buf, size_t cap);

/** @brief Applies the launcher's mod list (rl_settings_mod; else $RELIFT_MODS) with RELIFT_ROMFS as the base; rl_srv_install_fs calls it. */
void rl_hos_mods_install(void);

#endif
