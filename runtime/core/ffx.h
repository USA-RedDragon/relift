/**
 * @file ffx.h
 * @brief The Vulkan plumbing the FidelityFX ports (fsr2.c, fsr3.c) share: the device functions they call, memory,
 * images, compute pipelines from a generated module, barriers, clears and the RCAS constants. Private to the core.
 */
#ifndef RELIFT_CORE_FFX_H
#define RELIFT_CORE_FFX_H

#include "vkmin.h"

#include <relift/post.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief The device functions the ports call, as X(name, return type, parameters). */
#define RL_FFX_FNS(X)                                                                                                  \
    X(vkCreateShaderModule, rlvk_result,                                                                              \
      (rlvk_handle, const rlvk_shader_module_create_info *, const void *, rlvk_handle *))                             \
    X(vkDestroyShaderModule, void, (rlvk_handle, rlvk_handle, const void *))                                          \
    X(vkCreateDescriptorSetLayout, rlvk_result,                                                                       \
      (rlvk_handle, const rlvk_descriptor_set_layout_create_info *, const void *, rlvk_handle *))                     \
    X(vkDestroyDescriptorSetLayout, void, (rlvk_handle, rlvk_handle, const void *))                                   \
    X(vkCreatePipelineLayout, rlvk_result,                                                                            \
      (rlvk_handle, const rlvk_pipeline_layout_create_info *, const void *, rlvk_handle *))                           \
    X(vkDestroyPipelineLayout, void, (rlvk_handle, rlvk_handle, const void *))                                        \
    X(vkCreateComputePipelines, rlvk_result,                                                                          \
      (rlvk_handle, rlvk_handle, uint32_t, const rlvk_compute_pipeline_create_info *, const void *, rlvk_handle *))   \
    X(vkDestroyPipeline, void, (rlvk_handle, rlvk_handle, const void *))                                              \
    X(vkCreateDescriptorPool, rlvk_result,                                                                            \
      (rlvk_handle, const rlvk_descriptor_pool_create_info *, const void *, rlvk_handle *))                           \
    X(vkDestroyDescriptorPool, void, (rlvk_handle, rlvk_handle, const void *))                                        \
    X(vkResetDescriptorPool, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_flags))                                     \
    X(vkAllocateDescriptorSets, rlvk_result, (rlvk_handle, const rlvk_descriptor_set_allocate_info *, rlvk_handle *))  \
    X(vkUpdateDescriptorSets, void,                                                                                   \
      (rlvk_handle, uint32_t, const rlvk_write_descriptor_set *, uint32_t, const void *))                             \
    X(vkCreateSampler, rlvk_result, (rlvk_handle, const rlvk_sampler_create_info *, const void *, rlvk_handle *))     \
    X(vkDestroySampler, void, (rlvk_handle, rlvk_handle, const void *))                                               \
    X(vkCreateBuffer, rlvk_result, (rlvk_handle, const rlvk_buffer_create_info *, const void *, rlvk_handle *))       \
    X(vkDestroyBuffer, void, (rlvk_handle, rlvk_handle, const void *))                                                \
    X(vkGetBufferMemoryRequirements, void, (rlvk_handle, rlvk_handle, rlvk_memory_requirements *))                    \
    X(vkBindBufferMemory, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_handle, rlvk_size))                            \
    X(vkCreateImage, rlvk_result, (rlvk_handle, const rlvk_image_create_info *, const void *, rlvk_handle *))         \
    X(vkDestroyImage, void, (rlvk_handle, rlvk_handle, const void *))                                                 \
    X(vkGetImageMemoryRequirements, void, (rlvk_handle, rlvk_handle, rlvk_memory_requirements *))                     \
    X(vkBindImageMemory, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_handle, rlvk_size))                             \
    X(vkCreateImageView, rlvk_result, (rlvk_handle, const rlvk_image_view_create_info *, const void *, rlvk_handle *)) \
    X(vkDestroyImageView, void, (rlvk_handle, rlvk_handle, const void *))                                             \
    X(vkAllocateMemory, rlvk_result, (rlvk_handle, const rlvk_memory_allocate_info *, const void *, rlvk_handle *))   \
    X(vkFreeMemory, void, (rlvk_handle, rlvk_handle, const void *))                                                   \
    X(vkCmdBindPipeline, void, (rlvk_handle, int32_t, rlvk_handle))                                                   \
    X(vkCmdBindDescriptorSets, void,                                                                                  \
      (rlvk_handle, int32_t, rlvk_handle, uint32_t, uint32_t, const rlvk_handle *, uint32_t, const uint32_t *))       \
    X(vkCmdDispatch, void, (rlvk_handle, uint32_t, uint32_t, uint32_t))                                               \
    X(vkCmdPipelineBarrier, void,                                                                                     \
      (rlvk_handle, rlvk_flags, rlvk_flags, rlvk_flags, uint32_t, const rlvk_memory_barrier *, uint32_t, const void *, \
       uint32_t, const rlvk_image_memory_barrier *))                                                                  \
    X(vkCmdUpdateBuffer, void, (rlvk_handle, rlvk_handle, rlvk_size, rlvk_size, const void *))                        \
    X(vkCmdClearColorImage, void,                                                                                     \
      (rlvk_handle, rlvk_handle, int32_t, const void *, uint32_t, const rlvk_image_subresource_range *))              \
    X(vkCmdCopyImageToBuffer, void,                                                                                   \
      (rlvk_handle, rlvk_handle, int32_t, rlvk_handle, uint32_t, const rlvk_buffer_image_copy *))                     \
    X(vkMapMemory, rlvk_result, (rlvk_handle, rlvk_handle, rlvk_size, rlvk_size, rlvk_flags, void **))                \
    X(vkUnmapMemory, void, (rlvk_handle, rlvk_handle))

/** @brief A device and the functions a port calls on it. */
typedef struct rl_ffx_vk {
#define RL_FFX_FN_FIELD(name, ret, args) ret(RLVK_CALL *name) args;
    RL_FFX_FNS(RL_FFX_FN_FIELD)
#undef RL_FFX_FN_FIELD
    rlvk_handle device;
    rlvk_memory_properties mem;
} rl_ffx_vk;

/** @brief An image with its memory and a view of all its levels. */
typedef struct rl_ffx_image {
    rlvk_handle image, memory, view;
} rl_ffx_image;

/** @brief One descriptor of a generated module: set, binding, VkDescriptorType. */
typedef struct rl_ffx_bind {
    uint32_t set, binding, type;
} rl_ffx_bind;

/** @brief A generated module made into a compute pipeline. */
typedef struct rl_ffx_pipeline {
    rlvk_handle set_layout, layout, pipe;
} rl_ffx_pipeline;

/**
 * @brief Fills vk from d: the device functions and the memory properties. False when one is missing; the log names
 * it with who (the port's name).
 */
bool rl_ffx_vk_load(rl_ffx_vk *vk, const rl_post_device *d, const char *who);

/** @brief Whether the physical device runs quad subgroup operations in compute shaders, as SPD passes need. */
bool rl_ffx_has_quads(const rl_post_device *d);

/** @brief Allocates device memory for req into *memory (device local where it can). */
bool rl_ffx_bind_memory(const rl_ffx_vk *vk, const rlvk_memory_requirements *req, rlvk_handle *memory);

/** @brief A 2D view of levels levels of img from level, or 0. */
rlvk_handle rl_ffx_view(const rl_ffx_vk *vk, rlvk_handle img, int32_t format, uint32_t level, uint32_t levels);

/** @brief Makes a w by h image of format with mips levels (sampled, storage, transfer destination) into *im. */
bool rl_ffx_image_new(const rl_ffx_vk *vk, int32_t format, uint32_t w, uint32_t h, uint32_t mips, rl_ffx_image *im);

/** @brief Frees *im and zeroes it. */
void rl_ffx_image_free(const rl_ffx_vk *vk, rl_ffx_image *im);

/**
 * @brief Makes the pipeline of SPIR-V code (size bytes): set set's layout from the n binds in that set, after the
 * layouts before (nbefore of them); samplers in the set are immutable, sampler. False on any failure.
 */
bool rl_ffx_pipeline_new(const rl_ffx_vk *vk, const uint32_t *code, uint32_t size, const rl_ffx_bind *binds,
                         uint32_t n, uint32_t set, const rlvk_handle *before, uint32_t nbefore, rlvk_handle sampler,
                         rl_ffx_pipeline *p);

/** @brief Frees *p and zeroes it. */
void rl_ffx_pipeline_free(const rl_ffx_vk *vk, rl_ffx_pipeline *p);

/** @brief A memory barrier from src (accesses at src_stage) to dst (at dst_stage). */
void rl_ffx_barrier(const rl_ffx_vk *vk, uint64_t cmd, uint32_t src_stage, uint32_t src, uint32_t dst_stage,
                    uint32_t dst);

/** @brief Clears every level of image to (r, g, 0, 0), read as the format's own type, in the general layout. */
void rl_ffx_clear(const rl_ffx_vk *vk, uint64_t cmd, rlvk_handle image, float r, float g);

/** @brief The RCAS constants (FsrRcasCon) for sharpness in [0, 1], as the passes' cbRCAS reads them. */
void rl_ffx_rcas(float sharpness, uint32_t rcas[4]);

/**
 * @brief One of a port's own images to dump: its FidelityFX resource name, image, format and the size copied (at most
 * the image's own), and whether it is a render-size image (allocated at the maximum render size, used over the frame's
 * render size).
 */
typedef struct rl_ffx_dump_image {
    const char *name;
    rlvk_handle image;
    int32_t format;
    uint32_t width, height;
    bool render_sized;
} rl_ffx_dump_image;

/**
 * @brief The debug dump of a port's own images (RELIFT_UPSCALER_DUMP=<folder>:<first>[-<last>], evaluations counted
 * from 1 by each port, at most 16): after the passes of each chosen evaluation the port's images are copied to host
 * memory, and when the GPU has finished them each is written as <folder>/<who>-<evaluation>-<name>.raw (rows packed,
 * level 0) with one line in <folder>/<who>-manifest.txt: evaluation, name, Vulkan format, width, height, render
 * width and height, "render" or "other" (whether the image is render-size), file, then the frame's constants as
 * 32-bit words in hex. runtime/tools/fsr_dump.py turns them into
 * PNGs and reads pixels.
 */
typedef struct rl_ffx_dump rl_ffx_dump;

/** @brief The dump of port who (fsr2, fsr3) when RELIFT_UPSCALER_DUMP is set, else NULL. */
rl_ffx_dump *rl_ffx_dump_new(const char *who);

/**
 * @brief Counts one evaluation and, when it is chosen, records copies of the n images (in the general layout, written
 * by compute passes) into cmd with the barriers around them; render is the frame's render size, consts the constants
 * (bytes, a multiple of 4). Does nothing for NULL.
 */
void rl_ffx_dump_record(rl_ffx_dump *d, const rl_ffx_vk *vk, uint64_t cmd, const rl_ffx_dump_image *images, uint32_t n,
                        const uint32_t render[2], const void *consts, uint32_t bytes);

/** @brief Writes the files of every recorded evaluation (the GPU has finished them) and frees their buffers. */
void rl_ffx_dump_retire(rl_ffx_dump *d, const rl_ffx_vk *vk);

/** @brief Frees d (the device must be idle); NULL does nothing. */
void rl_ffx_dump_free(rl_ffx_dump *d, const rl_ffx_vk *vk);

#endif
