/**
 * @file postgpu.h
 * @brief The picture pipeline for the window presenter (present.c): the upscaler stage and the anti-aliasing passes
 * of post.h, recorded into the presenter's own command buffer on the core's device with the picture and the result
 * both in GPU images, so no frame goes through host memory.
 *
 * Each presenter owns one pipeline (its pipelines, buffers and upscaler); calls come on that presenter's thread.
 */
#ifndef RELIFT_CORE_POSTGPU_H
#define RELIFT_CORE_POSTGPU_H

#include "vkdev.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct vk rl_post_gpu;

/** @brief Makes a pipeline on the core device; NULL when it cannot (the log says why). */
rl_post_gpu *rl_post_gpu_new(void);
/** @brief Waits for the device and frees the pipeline. */
void rl_post_gpu_free(rl_post_gpu *v);
/** @brief Whether any picture option asks for a pass (picture.aa on, an upscaler stage set or chosen). */
bool rl_post_gpu_wanted(void);
/**
 * @brief Whether the picture options ask for any pass on a width by height picture now (picture.aa, an upscaler
 * stage), and the size the passes' result has. Decides what the next rl_post_gpu_record does.
 */
bool rl_post_gpu_plan(rl_post_gpu *v, uint32_t width, uint32_t height, uint32_t *out_width, uint32_t *out_height);
/**
 * @brief Records the passes into cmd (a VkCommandBuffer being recorded): reads src (a VkImage of RGBA8 in
 * TRANSFER_SRC_OPTIMAL, sw by sh) and writes every pixel of dst (RGBA8 in TRANSFER_DST_OPTIMAL, the planned size).
 * Leaves dst in that layout; the caller's barrier follows.
 */
bool rl_post_gpu_record(rl_post_gpu *v, uint64_t cmd, uint64_t src, uint32_t sw, uint32_t sh, uint64_t dst, uint32_t w,
                        uint32_t h);

#endif
