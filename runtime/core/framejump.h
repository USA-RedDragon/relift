/**
 * @file framejump.h
 * @brief The frame jump detector: finds a single presented frame that differs sharply from the frames on both sides of
 * it while those two agree (a one-frame flash of wrong bytes between correct frames).
 *
 * Each present gives a signature: the luminance of RL_JUMP_SIG_W by RL_JUMP_SIG_H points sampled at the centers of a
 * grid over the picture (rl_jump_sig_rgba, or a GPU nearest-filter blit to that size). Present n is an outlier once
 * present n + 1 is in: its mean absolute difference from both n - 1 and n + 1 is at least the threshold, and n - 1
 * and n + 1 differ by at most half the smaller of those two. A fade or a cut moves the picture once and does not
 * come back, so it is not an outlier.
 */
#ifndef RELIFT_CORE_FRAMEJUMP_H
#define RELIFT_CORE_FRAMEJUMP_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Signature columns. */
#define RL_JUMP_SIG_W 64u
/** @brief Signature rows. */
#define RL_JUMP_SIG_H 36u
/** @brief Bytes of a signature: one luminance byte per point, row by row. */
#define RL_JUMP_SIG (RL_JUMP_SIG_W * RL_JUMP_SIG_H)
/**
 * @brief The default threshold, in luminance steps (0 to 255) of mean absolute difference, times 16. Measured on a
 * movie's fade-in: neighbouring frames differ by at most 7 steps, flashed frames by 15.2 to 39.6.
 */
#define RL_JUMP_DEFAULT_THRESHOLD (10u * 16u)

/** @brief The detector's state: the last two signatures (older first) and their present numbers. */
typedef struct rl_jump {
    uint8_t sig[2][RL_JUMP_SIG];
    uint64_t present[2];
    /** @brief Signatures held (0 to 2); with fewer than 2 the newest is sig[count - 1]. */
    uint32_t count;
    /** @brief Threshold in sixteenths of a luminance step (RL_JUMP_DEFAULT_THRESHOLD when 0). */
    uint32_t threshold;
    /** @brief The differences of the last push, in sixteenths of a step: newest to middle, middle to oldest, newest to oldest. */
    uint32_t d_next, d_prev, d_skip;
} rl_jump;

/**
 * @brief The signature of a w by h picture of 4-byte pixels (RGBA, or BGRA when bgra) with rows stride bytes apart:
 * Rec. 601 luminance at the pixel nearest each grid cell's center, as a nearest-filter blit to RL_JUMP_SIG_W by
 * RL_JUMP_SIG_H samples.
 */
RL_RUNTIME_API void rl_jump_sig_rgba(const uint8_t *px, uint32_t w, uint32_t h, size_t stride, bool bgra,
                                     uint8_t sig[RL_JUMP_SIG]);

/** @brief The luminance (0 to 255) of an RGB triple, Rec. 601 weights, rounded. */
RL_RUNTIME_API uint8_t rl_jump_luma(uint8_t r, uint8_t g, uint8_t b);

/** @brief The mean absolute difference of two signatures, in sixteenths of a luminance step. */
RL_RUNTIME_API uint32_t rl_jump_dist(const uint8_t a[RL_JUMP_SIG], const uint8_t b[RL_JUMP_SIG]);

/**
 * @brief Adds present number present's signature. True when the present before it is an outlier (see the file
 * comment); *outlier then gets that present's number. The differences are kept in j for the caller's report.
 */
RL_RUNTIME_API bool rl_jump_push(rl_jump *j, uint64_t present, const uint8_t sig[RL_JUMP_SIG], uint64_t *outlier);

#endif
