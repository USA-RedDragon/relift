/**
 * @file nvn_sched.h
 * @brief The order the NVN2 layer replays submitted command sets in. The
 * console runs a queue's work asynchronously, so a command buffer may wait
 * (nvnCommandBufferWaitEvent) for a value that another queue, or the CPU,
 * writes after the submit returned. The layer replays at submit, so a run
 * whose wait is not met stops there and holds its queue's later submits
 * behind it; whenever any run signals an event, or the CPU does, the stopped
 * runs go on from where they stopped, in submission order.
 */
#ifndef RELIFT_NVN_SCHED_H
#define RELIFT_NVN_SCHED_H

#include "nvn_priv.h"

/** @brief NVNeventWaitMode: the value is equal, or at or past the wait's value (a signed difference). */
enum { NVN_EVENT_WAIT_EQUAL = 0, NVN_EVENT_WAIT_GEQUAL_WRAP = 1 };
/** @brief NVNeventSignalMode: write the value, or add it. */
enum { NVN_EVENT_SIGNAL_WRITE = 0, NVN_EVENT_SIGNAL_ADD = 1 };

/** @brief True when an event holding cur satisfies a wait of mode and value. */
bool nvn_event_wait_met(uint32_t cur, uint32_t mode, uint32_t value);

/** @brief One command of a flattened command set, with the set whose bytes it points into. */
typedef struct nvn_sched_item {
    const nvn_cmd *e;
    const nvn_cmdset *s;
} nvn_sched_item;

/** @brief Lets go the command sets the first n items hold: each run of items of one set holds it once (nvn_sched_flatten takes them). */
void nvn_sched_items_release(const nvn_sched_item *items, uint32_t n);

/** @brief One submitted command set, flattened, and where its replay stands. */
typedef struct nvn_sched_run {
    uint64_t queue;
    nvn_sched_item *items;
    uint32_t n, pos;
    /** @brief The caller's replay state for this run. */
    void *user;
    struct nvn_sched_run *next;
} nvn_sched_run;

/** @brief The submitted runs not yet finished, oldest first. */
typedef struct nvn_sched {
    nvn_sched_run *head, *tail;
    uint32_t pending;
} nvn_sched;

/** @brief What the scheduler asks of the replay. */
typedef struct nvn_sched_ops {
    /** @brief Replays one command other than an event wait or signal; RL_OK, or a stop. */
    int (*exec)(void *user, const nvn_cmd *e, const nvn_cmdset *s);
    /** @brief Finishes the device work recorded so far, so the event values are as the commands before left them; false on a device failure. */
    bool (*flush)(void *user);
    /** @brief The guest word at address addr (an event's value), or NULL. */
    uint32_t *(*word)(void *user, uint64_t addr);
    /**
     * @brief Optional: told of each item of a run as the replay reaches it, in replay order: pos is its index in the
     * run, and for an event wait or signal, value is the event's word after it (a wait's word as it was checked) and
     * met whether a wait let the run through (always true for other items). A wait that holds the run is told each
     * time it is checked.
     */
    void (*note)(void *user, const nvn_cmd *e, uint32_t pos, uint32_t value, bool met);
    /**
     * @brief Optional: whether sync (an NVNsync) is signaled, for a WAIT_SYNC item (nvnCommandBufferWaitSync, or a
     * queue's nvnQueueWaitSync queued as a run of its own): the run waits there, holding its queue's later runs, until
     * it is. Without it WAIT_SYNC items pass.
     */
    bool (*signaled)(void *user, uint64_t sync);
    /** @brief Optional: run user finished its last item; its fence syncs are signaled here, before later waits are checked. */
    void (*finished)(void *user);
} nvn_sched_ops;

/**
 * @brief Flattens s (its CALL_COMMAND and CALL_COMMANDS entries replaced by the commands of the sets they name, in
 * place) into *items and *n. Returns 0, or the handle that names no command set in *bad.
 */
int nvn_sched_flatten(const nvn_cmdset *s, nvn_sched_item **items, uint32_t *n, uint64_t *bad);

/** @brief Whether s holds an unfinished run of queue. */
bool nvn_sched_queue_busy(const nvn_sched *s, uint64_t queue);

/** @brief Queues a run (items, n, taken over) of queue, behind that queue's earlier runs. */
bool nvn_sched_add(nvn_sched *s, uint64_t queue, nvn_sched_item *items, uint32_t n, void *user);

/**
 * @brief Replays every run as far as its waits allow, oldest first, until none can go on. Finished runs are taken
 * off and returned as a list through *done (the caller finishes and frees them). Returns RL_OK, or the first stop
 * a command returned.
 */
int nvn_sched_pump(nvn_sched *s, const nvn_sched_ops *ops, nvn_sched_run **done);

/** @brief Frees a run and its items. */
void nvn_sched_run_free(nvn_sched_run *r);

#endif
