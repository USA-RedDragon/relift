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

#endif
