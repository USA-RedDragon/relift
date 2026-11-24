/**
 * @file changemask.h
 * @brief relift's moved-surface mask (shaders/changemask.comp) for the FSR 2 port: its transparency and
 * composition mask, 1 where another surface took a pixel than the motion vectors say, so objects a game moves without
 * motion vectors do not trail. Private to the core.
 */
#ifndef RELIFT_CORE_CHANGEMASK_H
#define RELIFT_CORE_CHANGEMASK_H

#include "ffx.h"

#include <relift/upscale.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief The mask's pass and images on one device. */
typedef struct rl_changemask rl_changemask;

/** @brief Starts the pass with the port's device functions vk (copied); NULL when it cannot (the log says why). */
rl_changemask *rl_changemask_new(const rl_ffx_vk *vk);

/**
 * @brief The camera FSR is told about, as the port computed it: device_to_view the first two of FSR's
 * deviceToViewDepth factors, tan_half the tangents of half the field of view (x, y).
 */
typedef struct rl_changemask_camera {
    float device_to_view[2], tan_half[2];
} rl_changemask_camera;

/**
 * @brief Records the pass for in's render rectangle into cmd and a barrier after it; *mask gets the view to bind as
 * FSR's transparency and composition mask and *image its image (render size, R16_SFLOAT, general layout). reset starts
 * from no previous depth (an empty mask). False when it recorded nothing (no descriptor set left, or the sizes changed
 * while records are pending): retire after the GPU is done.
 */
bool rl_changemask_record(rl_changemask *m, uint64_t cmd, const rl_upscale_inputs *in,
                          const rl_changemask_camera *cam, bool reset, rlvk_handle *mask, rlvk_handle *image);

/** @brief Says the GPU has finished everything recorded so far. */
void rl_changemask_retire(rl_changemask *m);

/** @brief Frees m (the device must be idle for it); NULL does nothing. */
void rl_changemask_free(rl_changemask *m);

#endif
