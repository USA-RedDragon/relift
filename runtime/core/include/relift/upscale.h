/**
 * @file upscale.h
 * @brief The upscaler stage: one implementation for the two places a picture is upscaled. Place 1 is inside the
 * game's frame, where the game asks its GPU driver for an upscaler (a GPU driver's DLSS calls): the stage
 * writes the game's output texture and the game's UI and post effects run after it. Place 2 is the window pipeline
 * ahead of anti-aliasing (post.h), for every guest, with only the picture to work from.
 *
 * A stage records compute work into the caller's command buffer on the caller's Vulkan device. Inputs are image
 * views in VK_IMAGE_LAYOUT_GENERAL that the stage samples; the output is a storage view it writes. The caller puts
 * the barriers before and after. Every choice but RL_UPSCALE_GAME changes the picture from the console's.
 */
#ifndef RELIFT_UPSCALE_H
#define RELIFT_UPSCALE_H

#include <relift/abi_core.h>
#include <relift/post.h>

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Choices of the runtime's `picture.upscaler` setting, as rl_setting_enum returns them. RL_UPSCALE_AUTO is
 * resolved per place by rl_upscale_resolve; RL_UPSCALE_GAME runs the game's own upscaler; RL_UPSCALE_OFF copies the
 * picture at its own size where a stage must write an output, and adds no stage in the window.
 */
enum {
    RL_UPSCALE_AUTO,
    RL_UPSCALE_GAME,
    RL_UPSCALE_OFF,
    RL_UPSCALE_NEAREST,
    RL_UPSCALE_BILINEAR,
    RL_UPSCALE_SHARP_BILINEAR,
    RL_UPSCALE_LANCZOS,
    RL_UPSCALE_FSR1,
    RL_UPSCALE_FSR2,
    RL_UPSCALE_FSR3,
    RL_UPSCALE_TAA,
    RL_UPSCALE_DLSS,
    RL_UPSCALE_CHOICES
};

/**
 * @brief The best temporal choice relift has, which auto runs in the frame when the game's own upscaler cannot run:
 * NVIDIA DLSS, which runs only where the NVIDIA driver and the user's DLSS library are there. A stage for it that does
 * not start runs RL_UPSCALE_FALLBACK_TEMPORAL instead, and one for that which does not start its spatial sibling
 * (rl_upscale_spatial).
 */
#define RL_UPSCALE_BEST_TEMPORAL RL_UPSCALE_DLSS

/** @brief The temporal choice that runs where DLSS does not: one that runs on every GPU. */
#define RL_UPSCALE_FALLBACK_TEMPORAL RL_UPSCALE_FSR2

/** @brief How a stage reaches its pictures. */
enum {
    /** @brief Image views: a sampled input and a storage output (needs shaderStorageImageWriteWithoutFormat). */
    RL_UPSCALE_IMAGES,
    /** @brief VkBuffers of one RGBA8 word per pixel, red in the low byte, rows packed (the window pipeline's). */
    RL_UPSCALE_BUFFERS
};

/** @brief One picture of the stage: a VkImageView (or a VkBuffer for an RL_UPSCALE_BUFFERS stage) and its size. */
typedef struct rl_upscale_image {
    /** @brief The handle, 0 for none. */
    uint64_t handle;
    /** @brief The size of the whole image or buffer picture, in pixels. */
    uint32_t width, height;
    /**
     * @brief For a view: the VkImage it views (0 when the caller does not give it), the view's VkFormat, and the
     * level and layer it shows. Choices that hand pictures to a vendor library (DLSS) need them.
     */
    uint64_t image;
    int32_t format;
    uint32_t level, layer;
} rl_upscale_image;

/** @brief A rectangle of a picture, in pixels. */
typedef struct rl_upscale_rect {
    uint32_t x, y, width, height;
} rl_upscale_rect;

/**
 * @brief What a stage reads and writes. Place 1 fills it from the game's upscaler parameters; place 2 sets color, out
 * and the rectangles only, and a temporal choice uses its spatial sibling there (rl_upscale_spatial).
 */
typedef struct rl_upscale_inputs {
    /** @brief The rendered picture, at render size. */
    rl_upscale_image color;
    /** @brief Color holds linear values above 1 (an HDR target). */
    bool hdr;
    /** @brief Depth at render size, or none; inverted: 1 is near. */
    rl_upscale_image depth;
    bool depth_inverted;
    /** @brief Motion vectors, or none; at display size instead of render size; with the jitter in them; their scale
     * to pixels (x, y). */
    rl_upscale_image motion;
    bool motion_display_size;
    bool motion_jittered;
    float motion_scale[2];
    /** @brief A 1x1 exposure image, or none; the pre-exposure the color was multiplied by (1 for none). */
    rl_upscale_image exposure;
    float pre_exposure;
    /** @brief The stage works the exposure out itself (no exposure image). */
    bool auto_exposure;
    /** @brief The projection's jitter this frame, in render pixels (x, y). */
    float jitter[2];
    /** @brief The history is not valid this frame (a camera cut). */
    bool reset;
    /** @brief The part of color read and the part of out written. */
    rl_upscale_rect in, out_rect;
    /** @brief Sharpening from 0 (none) to 1, for choices that sharpen. */
    float sharpness;
    /** @brief A mask of pixels whose history should count less (the game's reactive or bias mask), or none. */
    rl_upscale_image reactive;
    /** @brief The picture written, at display size. */
    rl_upscale_image out;
} rl_upscale_inputs;

/** @brief A stage started on one device for one choice. */
typedef struct rl_upscaler rl_upscaler;

/** @brief The `picture.upscaler` value names, in choice order. */
RL_RUNTIME_API const char *rl_upscale_name(uint32_t choice);

/**
 * @brief The choice that stands in for choice where only the picture is known (place 2): itself for a spatial one, no
 * stage (RL_UPSCALE_OFF) for auto and game.
 */
RL_RUNTIME_API uint32_t rl_upscale_spatial(uint32_t choice);

/** @brief Whether choice is temporal: it reads depth, motion vectors and the jitter, and keeps a history. */
RL_RUNTIME_API bool rl_upscale_temporal(uint32_t choice);

/**
 * @brief The device extensions the upscaler stage needs enabled on a device of PCI vendor vendor_id, for the
 * picture.upscaler setting as it is: the ones NVIDIA's NGX library asks for when the choice may be DLSS and the
 * driver's NGX library is installed, else none. *names stays valid while the runtime runs.
 */
RL_RUNTIME_API uint32_t rl_upscale_device_extensions(uint32_t vendor_id, const char *const **names);

/**
 * @brief The choice that runs for choice: in_frame for place 1, where game_ok says whether the game's own upscaler
 * can run on this device. RL_UPSCALE_AUTO is the game's own where it can run, else RL_UPSCALE_BEST_TEMPORAL, in the
 * frame; in the window it is rl_upscale_spatial(choice).
 */
RL_RUNTIME_API uint32_t rl_upscale_resolve(uint32_t choice, bool in_frame, bool game_ok);

/** @brief Says a guest upscales inside its frame (place 1), so the window adds no upscaler stage of its own. */
RL_RUNTIME_API void rl_upscale_in_frame(void);

/** @brief Whether rl_upscale_in_frame was called. */
RL_RUNTIME_API bool rl_upscale_window_off(void);

/**
 * @brief Starts choice (a relift-written spatial choice) on device for pictures reached as kind (RL_UPSCALE_IMAGES or
 * RL_UPSCALE_BUFFERS); NULL when it cannot (the log says why). Any thread; one stage is used by one thread at a time.
 */
RL_RUNTIME_API rl_upscaler *rl_upscaler_new(const rl_post_device *device, uint32_t choice, uint32_t kind);

/**
 * @brief Records the stage into cmd (a VkCommandBuffer being recorded): reads in->color over in->in and writes every
 * pixel of in->out over in->out_rect. False when it recorded nothing: bad sizes, or every descriptor set is in use
 * (retire, after the GPU has finished what was recorded, and record again).
 */
RL_RUNTIME_API bool rl_upscaler_record(rl_upscaler *u, uint64_t cmd, const rl_upscale_inputs *in);

/** @brief Says the GPU has finished everything recorded so far, so the stage may reuse what those records held. */
RL_RUNTIME_API void rl_upscaler_retire(rl_upscaler *u);

/** @brief Frees the stage (the device must be idle for it); NULL does nothing. */
RL_RUNTIME_API void rl_upscaler_free(rl_upscaler *u);

/** @brief The input pixels per output pixel (step) and output pixels per input pixel (ratio) of in to out, x and y. */
RL_RUNTIME_API void rl_upscale_steps(const rl_upscale_rect *in, const rl_upscale_rect *out, float step[2],
                                     float ratio[2]);

/**
 * @brief The point of the input (src: x, y in the input's pixels) that the output rectangle's pixel x, y (its center)
 * samples: the mapping every relift-written choice uses, for tests.
 */
RL_RUNTIME_API void rl_upscale_map(const rl_upscale_rect *in, const rl_upscale_rect *out, uint32_t x, uint32_t y,
                                   float src[2]);

#endif
