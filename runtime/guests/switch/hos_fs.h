/**
 * @file hos_fs.h
 * @brief The fsp-srv service with an SD card file system on a host directory.
 */
#ifndef RELIFT_HOS_FS_H
#define RELIFT_HOS_FS_H

#include "romfs_image.h"

#include <relift/abi_core.h>

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Registers the "fsp-srv" service (IFileSystemProxy, IFileSystem,
 * IFile, IDirectory). OpenSdCardFileSystem returns a file system whose root
 * is the host directory sdmc_host_dir; guest paths are resolved under it and
 * any ".." component is refused. The directory must exist. NULL or an empty
 * string selects "./sdmc". Call after rl_hos_init.
 */
RL_RUNTIME_API void rl_hos_fs_init(const char *sdmc_host_dir);

/**
 * @brief Sets the host directory that holds save data; NULL or an empty
 * string selects "./save". OpenSaveDataFileSystem serves
 * <dir>/<program id>/user-<uid> for account saves (device, bcat, temporary
 * and cache-<index> for the other kinds, system/<id> for system saves) and
 * creates it on first open. Save data is journaled as on the console: a
 * mount works on a copy, IFileSystem::Commit makes the copy the saved state
 * atomically (a crash at any point leaves the old or the new state), and a
 * mount closed without Commit discards its writes. The saved state stays a
 * plain directory tree. rl_srv_install_fs reads it from the save.dir setting (RELIFT_SAVE).
 */
RL_RUNTIME_API void rl_hos_fs_set_save(const char *dir);
/** @brief The save folder in use. */
RL_RUNTIME_API const char *rl_hos_fs_save_dir(void);

/**
 * @brief Sets the extracted romfs tree that OpenDataStorageByCurrentProcess
 * serves as one read-only RomFS image (romfs_image.h); NULL or an empty string
 * leaves the program without data storage. rl_srv_install_fs reads it from
 * $RELIFT_ROMFS. Call before the guest opens the storage.
 */
RL_RUNTIME_API void rl_hos_fs_set_romfs(const char *dir);

/**
 * @brief Like rl_hos_fs_set_romfs for several trees layered in order (see
 * rl_romfs_image_open_layers): the first is normally the base tree, later ones
 * win. warn is called while the image is built, on first use of the storage.
 * The layers and their strings are copied.
 */
RL_RUNTIME_API void rl_hos_fs_set_romfs_layers(const rl_romfs_layer *layers, size_t n, rl_romfs_conflict_fn warn,
                                               void *user);

/**
 * @brief Sets the directory of the add-on content trees, laid out as
 * relift import writes them: <16 hex digit add-on id>/<index>/romfs. The
 * guest opens an add-on by its id (index 0). rl_srv_install_fs reads it from
 * $RELIFT_DLC.
 */
RL_RUNTIME_API void rl_hos_fs_set_addons(const char *dir);

/**
 * @brief Lists the ids of the add-ons in the add-on directory: the names of
 * its sub-directories that are 16 lower-case hex digits and hold index 0's
 * romfs. Writes up to cap ids in ascending order to out (which may be NULL)
 * and returns how many there are. A missing directory has none.
 */
RL_RUNTIME_API size_t rl_hos_fs_addon_ids(uint64_t *out, size_t cap);

#endif
