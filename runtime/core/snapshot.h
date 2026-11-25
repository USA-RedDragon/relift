/**
 * @file snapshot.h
 * docgen:region overview
 * @brief The diagnostic snapshot, a complement to the GDB server for a development build: one trigger writes one
 * folder that says what the process held and did around that moment, without stopping it.
 *
 * Triggers (all dev builds only, where the debug stub is built in; a player package has none of this code):
 *  - F12 on the keyboard, or holding Minus and clicking the right stick on a controller (the window backend calls
 *    rl_snapshot_request);
 *  - a file named snapshot-now in the save folder (the setting save.dir, else the app's user data folder): created
 *    by hand or by `adb shell run-as <package> touch files/snapshot-now` on a phone, taken and removed at the next
 *    present.
 * The setting debug.snapshot is the number of presents a snapshot covers after the trigger (default 3); 0 turns the
 * triggers off. Untriggered, a present costs one atomic load, one clock read, and a file check every half second.
 *
 * The folder is <save folder>/snapshots/<date>-<time>-p<present>/ and always holds, from the core: index.txt (what
 * every file is, in plain words), settings.txt, build.txt, log_tail.txt (the last 500 log lines), picture.png (the
 * presented frame), memory.txt and heap.txt (the process's memory, with call sites when the heapsample library
 * is preloaded), threads_host.txt (every host thread and what it waits in). A guest registers a provider for its own
 * parts (a guest layer: guest threads, NVN batches, the GPU profile, a frame trace, the holdings of the
 * Vulkan layer); the core knows no guest, CPU or GPU concept.
 * docgen:end
 */
#ifndef RELIFT_CORE_SNAPSHOT_H
#define RELIFT_CORE_SNAPSHOT_H

#include "core.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief What a guest adds to a snapshot. Every hook is optional and runs on the thread that finished a present. */
typedef struct rl_snapshot_provider {
    /** @brief Short name for the log. */
    const char *name;
    /** @brief A snapshot starts: arm what only a triggered window should pay for. presents is the window length. */
    void (*begin)(void *user, const char *dir, uint32_t presents);
    /** @brief A present of the window finished (index 1 to presents). */
    void (*present)(void *user, const char *dir, uint32_t index);
    /**
     * @brief Whether the provider has what it needs from the window: asked at each present once the window's presents are
     * done, and the window stays open until every provider that has this hook says true (or 60 more presents pass).
     */
    bool (*ready)(void *user);
    /** @brief The window is over: write the provider's files into dir (each announced with rl_snapshot_file) and disarm. */
    void (*end)(void *user, const char *dir);
    void *user;
} rl_snapshot_provider;

/** @brief Adds a provider (copied; at most 8). Safe to call again with the same name: the later one replaces the earlier. */
RL_RUNTIME_API void rl_snapshot_register(const rl_snapshot_provider *p);

/**
 * @brief Announces a file a provider wrote in the snapshot folder, with what it is in plain words, for index.txt. Only
 * valid inside a provider's end hook.
 */
RL_RUNTIME_API void rl_snapshot_file(const char *name, const char *what);

/** @brief Asks for a snapshot at the next present; any thread, ignored while one is running or debug.snapshot is 0. */
RL_RUNTIME_API void rl_snapshot_request(void);

/** @brief A guest present has finished (called by rl_capture_present_end): starts, advances and ends a snapshot. */
RL_RUNTIME_API void rl_snapshot_present(void);

/** @brief Whether a snapshot window is open now (providers use it to skip work outside the window). */
RL_RUNTIME_API bool rl_snapshot_active(void);

/** @brief Sets the folder that holds snapshots/ and snapshot-now, replacing the save folder (tests, tools). NULL: the default. */
RL_RUNTIME_API void rl_snapshot_set_root(const char *dir);

/** @brief The folder of the last finished snapshot, or "" when none finished. */
RL_RUNTIME_API const char *rl_snapshot_last_dir(void);

#endif
