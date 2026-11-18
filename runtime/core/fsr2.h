/**
 * @file fsr2.h
 * @brief AMD FidelityFX Super Resolution 2 for the upscaler stage (upscale.c): the temporal choice, for callers that
 * give depth and motion vectors with image views (place 1). Private to the core.
 */
#ifndef RELIFT_CORE_FSR2_H
#define RELIFT_CORE_FSR2_H

#include <relift/upscale.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief FSR 2 started on one device. */
typedef struct rl_fsr2 rl_fsr2;

/** @brief Starts FSR 2 on device; NULL when the device cannot run it (the log says why). */
rl_fsr2 *rl_fsr2_new(const rl_post_device *device);

/**
 * @brief Whether in has what FSR 2 needs: depth and motion vectors, the input and output rectangles at the origin.
 * Without them the caller runs a spatial choice instead.
 */
bool rl_fsr2_takes(const rl_upscale_inputs *in);

/**
 * @brief Records one frame of FSR 2 into cmd: reads in's color, depth, motion vectors and optional exposure and
 * reactive mask, writes in->out over in->out_rect. False when it recorded nothing (descriptor sets in use, or the
 * sizes changed while records are pending): retire after the GPU is done and record again.
 */
bool rl_fsr2_record(rl_fsr2 *f, uint64_t cmd, const rl_upscale_inputs *in);

/** @brief Says the GPU has finished everything recorded so far. */
void rl_fsr2_retire(rl_fsr2 *f);

/** @brief Frees f (the device must be idle for it); NULL does nothing. */
void rl_fsr2_free(rl_fsr2 *f);

#endif
