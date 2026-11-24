/**
 * @file nvn_stats.h
 * @brief The NVN use histogram: which procs a run asked for and called, and which commands it recorded and how the
 * host device ran each one, written to a file when the run ends.
 *
 * RELIFT_NVN_STATS=<file> turns it on. Each proc the game looks up gets a line with its kind (impl: a host function,
 * zero: returns 0, recorded: a command buffer proc relift records but has no function for, missing: stops the game,
 * driver: the driver's own code runs) and the calls the layer saw. Each command op gets the count recorded and the
 * count replayed, split into ok, ignored (the host does nothing for it), partial (part of its effect is dropped),
 * stopped (the replay stopped there) and unhandled (no host handler). Every line whose proc is missing or recorded,
 * or whose op was ignored, partial, stopped or unhandled, carries the flag `gap`. The reader is
 * runtime/tools/nvn_stats.py.
 */
#ifndef RELIFT_NVN_STATS_H
#define RELIFT_NVN_STATS_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/** @brief What the layer does for a proc a game looks up. */
typedef enum nvn_stats_kind {
    NVN_STATS_IMPL,
    NVN_STATS_ZERO,
    NVN_STATS_RECORDED,
    NVN_STATS_MISSING,
    NVN_STATS_DRIVER,
} nvn_stats_kind;

/** @brief One event of a command op: recorded, or one replay outcome. */
typedef enum nvn_stats_event {
    NVN_STATS_REC,
    NVN_STATS_OK,
    NVN_STATS_IGNORED,
    NVN_STATS_PARTIAL,
    NVN_STATS_STOPPED,
    NVN_STATS_UNHANDLED,
    NVN_STATS_EVENTS,
} nvn_stats_event;

/** @brief Whether RELIFT_NVN_STATS is set (read once); every other call does nothing when it is not. */
bool nvn_stats_on(void);
/** @brief Turns the histogram on or off and clears it, writing to path at exit (NULL: nowhere). For tests. */
void nvn_stats_reset(const char *path, bool on);
/** @brief The game looked up proc name, which the layer serves as kind from slot (UINT32_MAX for driver procs). */
void nvn_stats_lookup(const char *name, nvn_stats_kind kind, uint32_t slot);
/** @brief One call of the proc in slot. */
void nvn_stats_call(uint32_t slot);
/** @brief One event of command op. */
void nvn_stats_cmd(uint32_t op, nvn_stats_event ev);
/**
 * @brief Marks the command the host device is running now as ignored or partial (the replay counts it so once it
 * returns; a later mark of the same command wins only over ok). Returns 0 (RL_OK) so a handler can return it.
 */
int nvn_stats_mark(nvn_stats_event ev);
/** @brief Starts the replay of one command: clears the mark. */
void nvn_stats_begin(void);
/** @brief Counts the replay of command op, which returned rc: stopped, unhandled (unhandled true), or the mark. */
void nvn_stats_end(uint32_t op, int rc, bool unhandled);
/** @brief Writes the histogram to f. */
void nvn_stats_print(FILE *f);
/** @brief Writes the histogram to the RELIFT_NVN_STATS file now; false when it cannot be written. */
bool nvn_stats_write(void);
/** @brief A command op's count of ev. */
uint64_t nvn_stats_count(uint32_t op, nvn_stats_event ev);
/** @brief The calls of the proc in slot. */
uint64_t nvn_stats_calls(uint32_t slot);

#endif
