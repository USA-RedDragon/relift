/**
 * @file fs_priv.h
 * @brief Shared by the parts of fsp-srv: nn::fs result codes and the data
 * storage commands of fs_romfs.c.
 */
#ifndef RELIFT_FS_PRIV_H
#define RELIFT_FS_PRIV_H

#include "ipc.h"

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

/** @brief How a save data directory is mounted. */
enum {
    RL_SAVE_JOURNALED = 0,
    RL_SAVE_READONLY = 1,
    RL_SAVE_DIRECT = 2
};

/** @brief A mounted save data file system (save_journal.c). */
typedef struct rl_save_mount rl_save_mount;

/**
 * @brief Mounts the save directory dir, whose parent exists. A journaled
 * mount recovers an interrupted commit and works on a copy of dir that
 * rl_save_close discards; a second journaled mount of the same directory
 * fails with EBUSY. A read-only mount serves the committed state; a direct
 * mount serves dir itself. Returns 0 or an errno value.
 */
int rl_save_open(const char *dir, int mode, rl_save_mount **out);
/** @brief The host directory a mount's file operations use. */
const char *rl_save_root(const rl_save_mount *m);
/** @brief Whether the mount refuses writes. */
int rl_save_read_only(const rl_save_mount *m);
/** @brief Makes the working copy the committed state, atomically. Returns 0 or an errno value. */
int rl_save_commit(rl_save_mount *m);
/** @brief Ends a mount; uncommitted changes are discarded. */
void rl_save_close(rl_save_mount *m);
/**
 * @brief Test hook: the next commit stops at step 1 to 5 as if the process
 * died there (1 after the new copy is synced, 2 after the marker, 3 after
 * the old state is moved aside, 4 after the new state is moved in, 5 after
 * the marker is removed). 0 clears it.
 */
void rl_save_set_fault(int step);

/** @brief Results of the data storages. */
#define FS_TARGET_NOT_FOUND RL_MAKE_RESULT(2, 1002)
#define FS_OUT_OF_RANGE RL_MAKE_RESULT(2, 3005)

/** @brief Reads RELIFT_ROMFS and RELIFT_DLC into the data storage sources. */
void rl_fs_romfs_env(void);
/** @brief OpenDataStorageByCurrentProcess (200). */
rl_result rl_fs_open_data_storage_current(rl_ipc_req *r);
/** @brief OpenDataStorageByProgramId (201). */
rl_result rl_fs_open_data_storage_program(rl_ipc_req *r);
/** @brief OpenDataStorageByDataId (202). */
rl_result rl_fs_open_data_storage_data_id(rl_ipc_req *r);
/** @brief OpenPatchDataStorageByCurrentProcess (203): there is no separate patch storage. */
rl_result rl_fs_open_patch_data_storage(rl_ipc_req *r);

#endif
