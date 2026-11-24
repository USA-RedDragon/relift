/**
 * @file save_journal.h
 * @brief Journaled save data on host directories (save_journal.c): a guest
 * layer mounts a save directory, works on a copy and commits it atomically.
 */
#ifndef RELIFT_SAVE_JOURNAL_H
#define RELIFT_SAVE_JOURNAL_H

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

#endif
