/**
 * @file post.h
 * @brief Player picture options applied to a frame before the window shows
 * it: anti-aliasing passes on the guest's picture, run on the host's Vulkan
 * device by the core picture pipeline. Every option changes the picture
 * from the console's and is off by default; with every option off and no
 * upscaler stage, rl_post_apply returns the frame it was given, untouched,
 * and the pipeline never starts.
 */
#ifndef RELIFT_POST_H
#define RELIFT_POST_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief Choices of the runtime's `picture.aa` setting, as rl_setting_enum returns them. */
enum { RL_POST_AA_OFF, RL_POST_AA_FXAA, RL_POST_AA_SMAA };

/** @brief Choices of `picture.aa_quality` (SMAA's presets). */
enum { RL_POST_QUALITY_LOW, RL_POST_QUALITY_MEDIUM, RL_POST_QUALITY_HIGH, RL_POST_QUALITY_ULTRA };

/** @brief Choices of `picture.filter`: how windows scale the picture. */
enum { RL_POST_FILTER_DEFAULT, RL_POST_FILTER_INTEGER, RL_POST_FILTER_NEAREST, RL_POST_FILTER_SHARP_BILINEAR };

/** @brief How a window samples the picture it scales (rl_post_sampling). */
enum { RL_POST_SAMPLE_NEAREST, RL_POST_SAMPLE_LINEAR, RL_POST_SAMPLE_SHARP };

/**
 * @brief Whether a window scales pictures by whole multiples only, after picture.filter, for a window whose own way
 * is fit (rl_window_set_fit) or whole multiples (not fit).
 */
RL_RUNTIME_API bool rl_post_integer(bool fit);

/**
 * @brief How a window samples a src_w by src_h picture drawn at dst_w by dst_h, after picture.filter: nearest at
 * whole multiples whatever the setting; else nearest for nearest and integer, sharp bilinear (bilinear from a
 * whole-multiple nearest prescale) for sharp_bilinear, and for default sharp bilinear at 2x and up and plain
 * bilinear below.
 */
RL_RUNTIME_API int rl_post_sampling(uint32_t src_w, uint32_t src_h, uint32_t dst_w, uint32_t dst_h);

/** @brief The pipeline's Vulkan objects, for a stage that records into it; handles are the API's, as 64-bit values. */
typedef struct rl_post_device {
    /** @brief VkInstance. */
    uint64_t instance;
    /** @brief VkPhysicalDevice. */
    uint64_t physical_device;
    /** @brief VkDevice. */
    uint64_t device;
    /** @brief VkQueue the pipeline submits to (a compute queue). */
    uint64_t queue;
    /** @brief Its queue family. */
    uint32_t queue_family;
    /** @brief The device's PFN_vkGetDeviceProcAddr. */
    void *get_device_proc_addr;
} rl_post_device;

/**
 * @brief The upscaler stage: the first stage of the pipeline, ahead of anti-aliasing (an upscaler, or a resolve at
 * ratio 1). Pictures are VkBuffers (storage and transfer use) of one RGBA8 word per pixel, red in the low byte, rows
 * packed, row 0 at the top; the pipeline puts the barriers before and after record. Calls come on the presenting
 * thread, one at a time.
 */
typedef struct rl_post_stage {
    /** @brief A short name for the log. */
    const char *name;
    /** @brief Once, on the pipeline's device, before the first plan; false turns the stage off for the run. */
    bool (*start)(void *user, const rl_post_device *device);
    /** @brief The output size for a width by height picture; false skips the stage for this frame. */
    bool (*plan)(void *user, uint32_t width, uint32_t height, uint32_t *out_width, uint32_t *out_height);
    /** @brief Records the stage into cmd (a VkCommandBuffer being recorded): reads in, writes every pixel of out. */
    void (*record)(void *user, uint64_t cmd, uint64_t in, uint64_t out, uint32_t width, uint32_t height,
                   uint32_t out_width, uint32_t out_height);
} rl_post_stage;

/** @brief Sets the upscaler stage (NULL for none); stage and user must outlive the run. Any thread. */
RL_RUNTIME_API void rl_post_set_upscaler(const rl_post_stage *stage, void *user);

/**
 * @brief The frame the window should show for width by height RGB888 pixels (rows packed), and its size: rgb itself at
 * width by height with every picture option off and no upscaler (or when the pipeline cannot run), else a buffer owned
 * by the pipeline, valid until the next call.
 */
RL_RUNTIME_API const uint8_t *rl_post_apply(const uint8_t *rgb, uint32_t width, uint32_t height, uint32_t *out_width,
                                            uint32_t *out_height);

#endif
