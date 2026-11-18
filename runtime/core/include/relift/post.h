/**
 * @file post.h
 * @brief Player picture options applied to a frame before the window shows
 * it: anti-aliasing passes on the guest's picture, run on the host's Vulkan
 * device by the core picture pipeline. Every option changes the picture
 * from the console's and is off by default; with every option off,
 * rl_post_apply returns the frame it was given, untouched, and the pipeline
 * never starts.
 */
#ifndef RELIFT_POST_H
#define RELIFT_POST_H

#include <relift/abi_core.h>

#include <stdint.h>

/** @brief Choices of the runtime's `picture.aa` setting, as rl_setting_enum returns them. */
enum { RL_POST_AA_OFF, RL_POST_AA_FXAA, RL_POST_AA_SMAA };

/** @brief Choices of `picture.aa_quality` (SMAA's presets). */
enum { RL_POST_QUALITY_LOW, RL_POST_QUALITY_MEDIUM, RL_POST_QUALITY_HIGH, RL_POST_QUALITY_ULTRA };

/**
 * @brief The frame the window should show for width by height RGB888 pixels (rows packed): rgb itself with every
 * picture option off (or when the pipeline cannot run), else a buffer of the same size owned by the pipeline, valid
 * until the next call.
 */
RL_RUNTIME_API const uint8_t *rl_post_apply(const uint8_t *rgb, uint32_t width, uint32_t height);

#endif
