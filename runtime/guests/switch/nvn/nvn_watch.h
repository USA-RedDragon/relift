/**
 * @file nvn_watch.h
 * @brief Write tracking for the guest memory a texture was read from: the layer asks to be told when the guest (or
 * the runtime on its behalf) writes a range, instead of reading the range again every frame to compare.
 *
 * A watched range has its whole host pages made read-only; the first write to one faults into a fault hook, which
 * stamps the page, makes it writable again and lets the write run. The layer re-reads a range only when a page of it
 * was stamped after the watch began (or the guest map changed), and watches it again.
 */
#ifndef RELIFT_NVN_WATCH_H
#define RELIFT_NVN_WATCH_H

#include <stdbool.h>
#include <stdint.h>

/** @brief A watch on one range: the whole host pages inside it. Zeroed means not watching. */
typedef struct nvn_watch {
    bool on;
    /** @brief The guest range of the whole pages watched. */
    uint64_t addr, size;
    /** @brief The write stamp and guest map generation when the watch began. */
    uint64_t stamp, map_gen;
} nvn_watch;

/**
 * @brief Starts watching the whole host pages inside [addr, addr + size). Call it before reading the range, so a
 * write that lands after the read is seen. False (w left off) where tracking is unavailable (no fault handler, no
 * whole page in the range, pages that are not read-write). The pages' bytes before addr's page start and after the
 * last whole page are not watched (the caller compares those by their bytes: w->addr and w->size say what is).
 */
bool nvn_watch_start(nvn_watch *w, uint64_t addr, uint64_t size);
/**
 * @brief Opens every watched page of [addr, addr + size) for writing and stamps it as written, so the watches on it
 * read their ranges again. Importing the range into the host device as a buffer needs it: the host driver pins the
 * pages for writing and refuses a range with a read-only page.
 */
void nvn_watch_release(uint64_t addr, uint64_t size);
/** @brief True when a watched page was written since the watch began, or the guest map changed. */
bool nvn_watch_changed(const nvn_watch *w);
/**
 * @brief Marks the pages of [addr, addr + size) as written now, for a write that did not go through the CPU (the GPU
 * wrote the guest's bytes): a watch that covers one of them then reports a change.
 */
void nvn_watch_touch(uint64_t addr, uint64_t size);
/** @brief Number of faults the hook turned into stamps, and watches started, for the layer's stats. */
void nvn_watch_stats(uint64_t *faults, uint64_t *starts);

#endif
