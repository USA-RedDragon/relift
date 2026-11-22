/**
 * @file fsr3.h
 * @brief The AMD FidelityFX FSR 3 upscaler for the upscaler stage (upscale.c): a temporal choice, for callers that
 * give depth and motion vectors with image views (place 1). Private to the core.
 */
#ifndef RELIFT_CORE_FSR3_H
#define RELIFT_CORE_FSR3_H

#include <relift/upscale.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief The FSR 3 upscaler started on one device. */
typedef struct rl_fsr3 rl_fsr3;

/** @brief Starts the FSR 3 upscaler on device; NULL when the device cannot run it (the log says why). */
rl_fsr3 *rl_fsr3_new(const rl_post_device *device);

/**
 * @brief Whether in has what the FSR 3 upscaler needs: depth and motion vectors, the input and output rectangles at
 * the origin. Without them the caller runs a spatial choice instead.
 */
bool rl_fsr3_takes(const rl_upscale_inputs *in);

/**
 * @brief Records one frame into cmd: reads in's color, depth, motion vectors and optional exposure and reactive mask,
 * writes in->out over in->out_rect. False when it recorded nothing (descriptor sets in use, or the sizes changed while
 * records are pending): retire after the GPU is done and record again.
 */
bool rl_fsr3_record(rl_fsr3 *f, uint64_t cmd, const rl_upscale_inputs *in);

/** @brief Says the GPU has finished everything recorded so far. */
void rl_fsr3_retire(rl_fsr3 *f);

/** @brief Frees f (the device must be idle for it); NULL does nothing. */
void rl_fsr3_free(rl_fsr3 *f);

#endif
