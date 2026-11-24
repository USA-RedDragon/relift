/**
 * @file fs_priv.h
 * @brief Shared by the parts of fsp-srv: nn::fs result codes and the data
 * storage commands of fs_romfs.c.
 */
#ifndef RELIFT_FS_PRIV_H
#define RELIFT_FS_PRIV_H

#include "ipc.h"
#include "save_journal.h"

/** @brief nn::fs results (module 2). */
#define FS_PATH_NOT_FOUND RL_MAKE_RESULT(2, 1)
#define FS_PATH_ALREADY_EXISTS RL_MAKE_RESULT(2, 2)
#define FS_TARGET_LOCKED RL_MAKE_RESULT(2, 7)
#define FS_DIRECTORY_NOT_EMPTY RL_MAKE_RESULT(2, 8)
#define FS_USABLE_SPACE_NOT_ENOUGH RL_MAKE_RESULT(2, 34)
#define FS_UNEXPECTED RL_MAKE_RESULT(2, 5000)
#define FS_INVALID_ARGUMENT RL_MAKE_RESULT(2, 6001)
#define FS_INVALID_PATH RL_MAKE_RESULT(2, 6002)
#define FS_INVALID_PATH_FORMAT RL_MAKE_RESULT(2, 6004)
#define FS_PATH_TOO_LONG RL_MAKE_RESULT(2, 6005)
#define FS_INVALID_OFFSET RL_MAKE_RESULT(2, 6061)
#define FS_INVALID_SIZE RL_MAKE_RESULT(2, 6062)
#define FS_INVALID_OPEN_MODE RL_MAKE_RESULT(2, 6072)
#define FS_APPEND_REQUIRED RL_MAKE_RESULT(2, 6201)
#define FS_READ_NOT_PERMITTED RL_MAKE_RESULT(2, 6202)
#define FS_WRITE_NOT_PERMITTED RL_MAKE_RESULT(2, 6203)

/** @brief Results of the data storages. */
#define FS_TARGET_NOT_FOUND RL_MAKE_RESULT(2, 1002)
#define FS_OUT_OF_RANGE RL_MAKE_RESULT(2, 3005)

/** @brief Reads RELIFT_ROMFS and RELIFT_DLC into the data storage sources. */
void rl_fs_romfs_env(void);
/** @brief OpenDataStorageByCurrentProcess (200). */
rl_result rl_fs_open_data_storage_current(rl_ipc_req *r);
/** @brief OpenDataStorageByProgramId (201). */
rl_result rl_fs_open_data_storage_program(rl_ipc_req *r);
rl_result rl_fs_open_data_storage_program_index(rl_ipc_req *r);
/** @brief OpenDataStorageByDataId (202). */
rl_result rl_fs_open_data_storage_data_id(rl_ipc_req *r);
/** @brief OpenPatchDataStorageByCurrentProcess (203): there is no separate patch storage. */
rl_result rl_fs_open_patch_data_storage(rl_ipc_req *r);

#endif
