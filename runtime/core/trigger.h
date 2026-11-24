/**
 * @file trigger.h
 * @brief The capture trigger: one place where any detector says "this present is bad, keep what made it".
 *
 * A detector calls rl_trigger_fire with the present number it found bad, its own name and a line of detail. The
 * consumer (a capture that keeps the last frames' GPU work in memory, such as the NVN differential capture) takes the
 * hits with rl_trigger_take at each present and saves what it still holds for that present. Detectors in core: the
 * frame jump detector (framejump.h, a one-frame luminance outlier) and the zero-band detector below (thin vertical
 * runs of exact black in a picture whose same pixels had content the present before). Two manual sources need no
 * detector: RELIFT_TRIGGER_AT=<present>[,<present>...] fires at those presents, and RELIFT_TRIGGER_FILE=<path> fires
 * at the next present after that file appears (it is removed so each touch fires once). rl_trigger_poll checks both.
 */
#ifndef RELIFT_CORE_TRIGGER_H
#define RELIFT_CORE_TRIGGER_H

#include "core.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Hits held until taken; more are dropped (and counted). */
#define RL_TRIGGER_QUEUE 16u

/** @brief One firing: the bad present, who found it and what it saw. */
typedef struct rl_trigger_hit {
    uint64_t present;
    char source[24];
    char detail[200];
} rl_trigger_hit;

/** @brief Notes that present is bad, found by source; detail is a printf format. Thread-safe. */
RL_RUNTIME_API void rl_trigger_fire(uint64_t present, const char *source, const char *fmt, ...) RL_PRINTF(3, 4);

/** @brief Takes the oldest hit not yet taken into *hit; false when none waits. Thread-safe. */
RL_RUNTIME_API bool rl_trigger_take(rl_trigger_hit *hit);

/** @brief Hits dropped because the queue was full. */
RL_RUNTIME_API uint64_t rl_trigger_dropped(void);

/**
 * @brief The manual sources at present: fires when RELIFT_TRIGGER_AT lists it or RELIFT_TRIGGER_FILE exists (then
 * removes the file). The first call reads the settings.
 */
RL_RUNTIME_API void rl_trigger_poll(uint64_t present);

/** @brief Forgets the settings and the queue (tests). */
RL_RUNTIME_API void rl_trigger_reset(void);

/** @brief Most bands one zero-band search reports. */
#define RL_ZBAND_MAX 16u

/** @brief A zero band: columns x0 to x1 and rows y0 to y1 (ends excluded) of the searched picture. */
typedef struct rl_zband {
    uint32_t x0, x1, y0, y1;
} rl_zband;

/** @brief Zero-band search settings. Zero fields take the defaults given. */
typedef struct rl_zband_opts {
    /** @brief A pixel is zero when no channel of R, G and B exceeds this (default 0: exact zeros). */
    uint8_t level;
    /** @brief A pixel had content when some channel exceeded this the present before (default 8). */
    uint8_t content;
    /** @brief Fewest rows of a band (default height / 16). */
    uint32_t min_rows;
    /** @brief Widest band in columns (default width / 8): wider black is a scene, not a band. */
    uint32_t max_cols;
    /** @brief Share of a band's pixels, in percent, that had content the present before (default 90). */
    uint32_t had_content_pct;
} rl_zband_opts;

/**
 * @brief Finds zero bands in a w by h picture of 4-byte pixels (RGBA, alpha ignored) with rows stride bytes apart,
 * against prev, the present before at the same size and stride: column runs of at least min_rows zero pixels whose
 * pixels had content in prev, joined across neighbouring columns, no wider than max_cols. Writes at most max bands to
 * out, largest first, and returns how many there are (which may exceed max).
 */
RL_RUNTIME_API uint32_t rl_zband_find(const uint8_t *px, const uint8_t *prev, uint32_t w, uint32_t h, size_t stride,
                                      const rl_zband_opts *opts, rl_zband *out, uint32_t max);

#endif
