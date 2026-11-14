/**
 * @file hos_fs.h
 * @brief The fsp-srv service with an SD card file system on a host directory.
 */
#ifndef RELIFT_HOS_FS_H
#define RELIFT_HOS_FS_H

#include <relift/abi_core.h>

/**
 * @brief Registers the "fsp-srv" service (IFileSystemProxy, IFileSystem,
 * IFile, IDirectory). OpenSdCardFileSystem returns a file system whose root
 * is the host directory sdmc_host_dir; guest paths are resolved under it and
 * any ".." component is refused. The directory must exist. NULL or an empty
 * string selects "./sdmc". Call after rl_hos_init.
 */
RL_RUNTIME_API void rl_hos_fs_init(const char *sdmc_host_dir);

/**
 * @brief Sets the extracted romfs tree that OpenDataStorageByCurrentProcess
 * serves as one read-only RomFS image (romfs_image.h); NULL or an empty string
 * leaves the program without data storage. rl_srv_install_fs reads it from
 * $RELIFT_ROMFS. Call before the guest opens the storage.
 */
RL_RUNTIME_API void rl_hos_fs_set_romfs(const char *dir);

/**
 * @brief Sets the directory of the add-on content trees, laid out as
 * relift import writes them: <16 hex digit add-on id>/<index>/romfs. The
 * guest opens an add-on by its id (index 0). rl_srv_install_fs reads it from
 * $RELIFT_DLC.
 */
RL_RUNTIME_API void rl_hos_fs_set_addons(const char *dir);

#endif
