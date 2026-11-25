/**
 * @file nvn_async.h
 * @brief The replay worker of RELIFT_NVN_ASYNC_REPLAY: in-order items and a drain.
 */
#ifndef NVN_ASYNC_H
#define NVN_ASYNC_H

#include <stdbool.h>
#include <stdint.h>

typedef void (*nvn_async_fn)(void *arg);

/** @brief Whether RELIFT_NVN_ASYNC_REPLAY=1 (default off). */
bool nvn_async_enabled(void);
/** @brief Queues run(arg) behind every earlier item; the worker thread starts on the first. False when it cannot be queued. */
bool nvn_async_post(nvn_async_fn run, void *arg);
/** @brief Waits until every posted item has finished; at once from the worker itself or when none is pending. */
void nvn_async_drain(void);
/** @brief Whether the calling thread is the worker. */
bool nvn_async_on_worker(void);
/** @brief Items posted and not finished. */
uint32_t nvn_async_pending(void);

struct rl_ctx;
/** @brief The guest address just above the worker's scratch stack (0 until the layer reserved it). */
void nvn_async_set_stack(uint64_t top);
/** @brief A copy of the guest context for a queued item, its stack pointer on the worker's own stack; free it after. */
struct rl_ctx *nvn_async_snapshot(const struct rl_ctx *c);

#endif
