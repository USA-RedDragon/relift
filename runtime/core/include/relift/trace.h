/**
 * @file relift/trace.h
 * @brief The frame trace, a debugging aid for every guest: with stats.trace
 * naming a file, each trace point writes one line there, to find why a
 * vsync went without a new frame (trace.c).
 *
 * A line is "<kind> <CLOCK_MONOTONIC ns> <guest thread id or 0> <a> <b> <c>"
 * with a, b and c in hex. "<file>|<kinds>" writes only the kinds listed.
 * Each guest documents the kinds it writes. Off (the default), each trace
 * point costs one load.
 */
#ifndef RELIFT_TRACE_H
#define RELIFT_TRACE_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Whether stats.trace names a frame trace file; read once. */
bool rl_trace_on(void);
/** @brief CLOCK_MONOTONIC nanoseconds while the frame trace is on, else 0. */
int64_t rl_trace_now(void);
/** @brief Whether a trace limited to kinds (the letters after its path's bar; empty for all) writes kind. */
bool rl_trace_kind_listed(const char *kinds, char kind);
/** @brief Writes one frame trace line of this kind when the trace is on. */
void rl_trace(char kind, uint64_t a, uint64_t b, uint64_t c);
/**
 * @brief Writes a frame trace line with a and the calling thread's CPU time; reads neither for a kind the trace
 * leaves out.
 */
void rl_trace_cpu(char kind, uint64_t a);
/** @brief The guest thread id a line names: fn gives the calling thread's, 0 when it runs no guest thread. */
void rl_trace_set_thread_id(uint64_t (*fn)(void));

#endif
