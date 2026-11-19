/**
 * @file render.h
 * @brief A guest's line renderer on a host thread of its own.
 *
 * The run thread keeps the state its CPUs and devices read and write; the
 * renderer keeps a mirror of the part a line reads. The run thread puts a
 * record in an ordered log for every change of that part, and for every line
 * to draw and frame to finish, at the point it happens; the render thread
 * applies the records in order through the guest's apply function. The mirror
 * at a line's record is the run thread's state at that point, so the pixels
 * are the ones a renderer on the run thread would draw, and the run thread
 * never waits on them: its timing does not depend on rendering at all.
 *
 * Record kinds and payloads are the guest's. One thread puts, the render
 * thread applies; a full log makes the putter wait. Where rendered pixels
 * feed back into state the guest's CPUs read, the guest drains the log at
 * that point (rl_render_drain) and copies the result from the mirror, or
 * switches to sync mode, where every record applies at once on the putter's
 * thread. Sync and async give the same pixels; tests use sync mode, and
 * RELIFT_RENDER=sync selects it for every renderer.
 */
#ifndef RELIFT_RENDER_H
#define RELIFT_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <relift/abi_core.h>

typedef struct rl_render rl_render;

/** @brief Applies one record (kind, payload of len bytes) to the guest's mirror; it may draw a line or end a frame. */
typedef void rl_render_apply_fn(void *user, uint32_t kind, const void *data, uint32_t len);

/** @brief How a renderer runs. */
typedef struct rl_render_config {
    /** @brief The guest's replay of a record. */
    rl_render_apply_fn *apply;
    /** @brief Its argument: the guest's mirror and renderer. */
    void *user;
    /** @brief Log capacity in bytes (0: 4 MiB); a record may take at most a quarter of it. */
    size_t log_bytes;
    /** @brief Apply each record at once on the putter's thread (RELIFT_RENDER=sync forces it). */
    bool sync;
} rl_render_config;

/** @brief The largest payload rl_render_put takes for log capacity log_bytes (0: the default). */
RL_RUNTIME_API size_t rl_render_max_record(size_t log_bytes);
/** @brief A renderer; its thread starts unless it is in sync mode. NULL when it cannot start. */
RL_RUNTIME_API rl_render *rl_render_create(const rl_render_config *cfg);
/** @brief Applies every record put so far, stops the thread and frees r. */
RL_RUNTIME_API void rl_render_free(rl_render *r);
/** @brief Puts a record of kind with len bytes of data (len at most rl_render_max_record). */
RL_RUNTIME_API void rl_render_put(rl_render *r, uint32_t kind, const void *data, uint32_t len);
/** @brief Room for a record of kind with len bytes, to fill before rl_render_commit (no other put in between). */
RL_RUNTIME_API void *rl_render_begin(rl_render *r, uint32_t kind, uint32_t len);
/** @brief Puts the record rl_render_begin made room for. */
RL_RUNTIME_API void rl_render_commit(rl_render *r);
/**
 * @brief Lets the render thread start on the records put so far. A put alone wakes it only once the log is an
 * eighth full, so a guest flushes where a batch ends (a line): a wake costs a system call when the thread sleeps.
 */
RL_RUNTIME_API void rl_render_flush(rl_render *r);
/** @brief Returns once every record put so far has been applied. */
RL_RUNTIME_API void rl_render_drain(rl_render *r);
/** @brief Drains, then applies later records at once on the putter's thread (true) or on the render thread. */
RL_RUNTIME_API void rl_render_set_sync(rl_render *r, bool sync);
/** @brief True in sync mode. */
RL_RUNTIME_API bool rl_render_is_sync(const rl_render *r);

#endif
