/**
 * @file taa.h
 * @brief relift's plain temporal upscaler (TAA) for the upscaler stage (upscale.c): a temporal choice, for callers
 * that give depth and motion vectors with image views (place 1). Private to the core.
 */
#ifndef RELIFT_CORE_TAA_H
#define RELIFT_CORE_TAA_H

#include <relift/upscale.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief TAA started on one device. */
typedef struct rl_taa rl_taa;

/** @brief Starts TAA on device; NULL when the device cannot run it (the log says why). */
rl_taa *rl_taa_new(const rl_post_device *device);

/** @brief Whether in has what TAA needs: depth and motion vectors, the rectangles at the origin. */
bool rl_taa_takes(const rl_upscale_inputs *in);

/**
 * @brief Records one frame into cmd: writes in->out over in->out_rect from in's color, depth and motion vectors and
 * the history. False when it recorded nothing (records in use, or the sizes changed while records are pending):
 * retire after the GPU is done and record again.
 */
bool rl_taa_record(rl_taa *t, uint64_t cmd, const rl_upscale_inputs *in);

/** @brief Says the GPU has finished everything recorded so far. */
void rl_taa_retire(rl_taa *t);

/** @brief Frees t (the device must be idle for it); NULL does nothing. */
void rl_taa_free(rl_taa *t);

#endif
