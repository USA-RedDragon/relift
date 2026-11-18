/**
 * @file nvn_priv.h
 * @brief Shared parts of the NVN2 layer: the procs it implements and the
 * helpers they read their arguments with.
 */
#ifndef RELIFT_SWITCH_NVN_PRIV_H
#define RELIFT_SWITCH_NVN_PRIV_H

#include "hos.h"
#include "nvn.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/** @brief The procs the layer implements, one X(name) each; nvnh_<name> is the host function. */
#define NVN_PROCS(X)                                                                                                   \
    X(nvnBootstrapLoader)                                                                                              \
    X(nvnDeviceGetProcAddress)                                                                                         \
    NVN_DEVICE_PROCS(X)                                                                                                \
    NVN_CMDBUF_PROCS(X)                                                                                                \
    NVN_TEXTURE_PROCS(X)                                                                                               \
    X(nvnWindowInitialize)                                                                                             \
    X(nvnWindowFinalize)                                                                                               \
    X(nvnWindowAcquireTexture)                                                                                         \
    X(nvnWindowSetPresentInterval)                                                                                     \
    X(nvnWindowGetPresentInterval)                                                                                     \
    X(nvnWindowSetCrop)                                                                                                \
    X(nvnWindowGetCrop)                                                                                                \
    X(nvnTextureWriteTexels)                                                                                           \
    X(nvnTextureReadTexels)                                                                                            \
    X(nvnTextureFlushTexels)                                                                                           \
    X(nvnTextureInvalidateTexels)                                                                                      \
    X(nvnQueuePresentTexture)                                                                                          \
    X(nvnProgramInitialize)                                                                                            \
    X(nvnProgramFinalize)                                                                                              \
    X(nvnProgramInitializeFromBuilder)                                                                                 \
    X(nvnProgramSetShaders)                                                                                            \
    X(nvnProgramSetShadersExt)                                                                                         \
    X(nvnProgramSetDebugLabel)                                                                                         \
    X(nvnProgramSetSampleShading)                                                                                      \
    X(nvnDeviceGetSeparateTextureHandle)                                                                               \
    X(nvnDeviceGetSeparateSamplerHandle)                                                                               \
    X(nvnDeviceGetTexelFetchHandle)                                                                                    \
    X(nvnDeviceGetUniformBufferHandle)                                                                                 \
    X(nvnDeviceGetCommandHandleMemorySize)                                                                             \
    X(nvnDeviceFinalizeCommandHandle)                                                                                  \
    X(nvnDeviceGetCurrentTimestampInNanoseconds)                                                                       \
    X(nvnDeviceGetTimestampInNanoseconds)                                                                              \
    X(nvnEventInitialize)                                                                                              \
    X(nvnEventFinalize)                                                                                                \
    X(nvnEventGetValue)                                                                                                \
    X(nvnEventSignal)                                                                                                  \
    X(nvnEventGetMemoryPool)                                                                                           \
    X(nvnEventGetMemoryOffset)                                                                                         \
    X(nvnTextureGetTextureAddress)                                                                                     \
    X(nvnWindowGetNumTextures)                                                                                         \
    X(nvnWindowGetNumActiveTextures)                                                                                   \
    X(nvnWindowSetNumActiveTextures)                                                                                   \
    X(nvnDeviceRegisterFastClearColor)

/** @brief Procs that do nothing relift needs and return 0 (nvn_zero). */
#define NVN_ZERO_PROCS(X)                                                                                              \
    X(nvnQueueGetError)                                                                                                \
    X(nvnQueueGetTotalCommandMemoryUsed)                                                                               \
    X(nvnQueueGetTotalComputeMemoryUsed)                                                                               \
    X(nvnQueueGetTotalControlMemoryUsed)                                                                               \
    X(nvnQueueResetMemoryUsageCounts)                                                                                  \
    X(nvnDeviceIsExternalDebuggerAttached)                                                                             \
    X(nvnDeviceInstallDebugCallback)                                                                                   \
    X(nvnDeviceSetIntermediateShaderCache)                                                                             \
    X(nvnDeviceSetAutomaticCheckpointsByDebugGroup)                                                                    \
    X(nvnDeviceSetAutomaticCheckpointsEnable)                                                                          \
    X(nvnDeviceSetAutomaticCheckpointsMask)                                                                            \
    X(nvnDeviceSetAutomaticCheckpointsSamplingInterval)                                                                \
    X(nvnDeviceSetCheckpointCallStacksEnable)                                                                          \
    X(nvnDeviceSetCheckpointMaxCallStacksDepth)                                                                        \
    X(nvnDeviceSetDebugLabel)                                                                                          \
    X(nvnBufferSetDebugLabel)                                                                                          \
    X(nvnMemoryPoolSetDebugLabel)                                                                                      \
    X(nvnQueueSetDebugLabel)                                                                                           \
    X(nvnSamplerPoolSetDebugLabel)                                                                                     \
    X(nvnSamplerSetDebugLabel)                                                                                         \
    X(nvnSyncSetDebugLabel)                                                                                            \
    X(nvnTexturePoolSetDebugLabel)                                                                                     \
    X(nvnTextureSetDebugLabel)                                                                                         \
    X(nvnWindowSetDebugLabel)                                                                                          \
    X(nvnCommandBufferSetDebugLabel)

/** @brief Returns 0 from a proc of NVN_ZERO_PROCS. */
uint64_t nvn_zero(rl_ctx *c, uint8_t *mem);

/** @brief Frees the command set of handle; any other value does nothing. */
void nvn_cmdset_free(uint64_t handle);

/** @brief Marks the sync at guest address sync signaled; 0 does nothing. */
void nvn_sync_signal(uint64_t sync);
/** @brief Shows texture (an NVNtexture the game rendered) as the next frame, after the work queue submitted. Returns RL_OK or RL_PENDING. */
int nvn_present(rl_ctx *c, uint8_t *mem, uint64_t queue, uint64_t texture);

/** @brief Texture, sampler and descriptor pool procs (nvn_tex.c). */
#define NVN_TEXTURE_PROCS(X)                                                                                           \
    X(nvnTextureBuilderGetStorageSize)                                                                                 \
    X(nvnTextureBuilderGetStorageClass)                                                                                \
    X(nvnTextureBuilderGetStorageAlignment)                                                                            \
    X(nvnTextureInitialize)                                                                                            \
    X(nvnTextureFinalize)                                                                                              \
    X(nvnTextureGetWidth)                                                                                              \
    X(nvnTextureGetHeight)                                                                                             \
    X(nvnTextureGetDepth)                                                                                              \
    X(nvnTextureGetFormat)                                                                                             \
    X(nvnTextureGetTarget)                                                                                             \
    X(nvnTextureGetFlags)                                                                                              \
    X(nvnTextureGetStorageSize)                                                                                        \
    X(nvnTextureGetLevels)                                                                                             \
    X(nvnTextureGetMemoryPool)                                                                                         \
    X(nvnTextureGetMemoryOffset)                                                                                       \
    X(nvnTextureGetSamples)                                                                                            \
    X(nvnTextureGetStride)                                                                                             \
    X(nvnSamplerInitialize)                                                                                            \
    X(nvnSamplerFinalize)                                                                                              \
    X(nvnTexturePoolInitialize)                                                                                        \
    X(nvnTexturePoolFinalize)                                                                                          \
    X(nvnSamplerPoolInitialize)                                                                                        \
    X(nvnSamplerPoolFinalize)                                                                                          \
    X(nvnTexturePoolGetSize)                                                                                           \
    X(nvnSamplerPoolGetSize)                                                                                           \
    X(nvnTexturePoolRegisterTexture)                                                                                   \
    X(nvnTexturePoolRegisterImage)                                                                                     \
    X(nvnSamplerPoolRegisterSampler)

/** @brief Most mip levels relift records offsets for. */
#define NVN_TEX_MAX_LEVELS 16u

/** @brief A texture as relift lays it out. */
typedef struct nvn_tex_desc {
    uint32_t target, format, width, height, depth, levels, samples, flags;
    /** @brief NVNtextureSwizzle of each channel (the builder's), as the texture is sampled without a view. */
    uint32_t swizzle[4];
    uint64_t stride, pool, offset;
    /** @brief Bytes of storage, its alignment and the bytes from one layer to the next. */
    uint64_t size, align, layer_stride;
    /** @brief Block height and depth of level 0 in GOBs; 0 for linear storage. */
    uint32_t block_h, block_d;
    /** @brief Offset of each level inside a layer. */
    uint64_t level_offset[NVN_TEX_MAX_LEVELS];
} nvn_tex_desc;

/** @brief Bytes per block and block size of an NVNformat; false for a format relift does not know. */
bool nvn_tex_format(uint32_t format, uint32_t *bpb, uint32_t *bw, uint32_t *bh);
/** @brief Fills d's size, alignment, block and level fields from its target, format, size, levels, samples and flags; false for an unknown format. */
bool nvn_tex_layout(nvn_tex_desc *d);
/** @brief NVNcopyRegion: a box of texels. */
typedef struct nvn_region {
    int32_t x, y, z, width, height, depth;
} nvn_region;

/**
 * @brief Copies region r of level and layer of a texture laid out as d with
 * its storage at storage, to (to_storage) or from packed texels (rows of r's
 * width, tightly packed). False when r lies outside the level.
 */
bool nvn_tex_copy(const nvn_tex_desc *d, uint8_t *storage, uint32_t level, uint32_t layer, const nvn_region *r,
                  uint8_t *packed, bool to_storage);
/** @brief The layout of the texture at guest address tex, or NULL. */
const nvn_tex_desc *nvn_texture_desc(uint64_t tex);
/**
 * @brief Texture id's registration: the texture's guest address (0 when the id is free), whether it was
 * registered as an image, and a copy of its view's bytes (has_view false for none).
 */
uint64_t nvn_tex_registered(uint32_t id, uint8_t view[16], bool *has_view, bool *image);
/** @brief The builder bytes of the sampler registered as id (128 bytes), or NULL. */
const uint8_t *nvn_sampler_registered(uint32_t id);

/** @brief Makes folder dir and the folders above it. */
void nvn_mkdirs(const char *dir);
/**
 * @brief The shader folder, where programs the table lacks and device.json are written: $RELIFT_NVN_SHADERS, else
 * "shaders" beside the executable (nvn_shader.c).
 */
const char *nvn_shader_dir(void);
/** @brief The folder the translation tables are read from: $RELIFT_NVN_TABLES, else the shader folder. */
const char *nvn_shader_table_dir(void);
/** @brief Bytes of the program at guest address code: its header if it has one and its code through the first BRA to itself; 0 when none ends within 4 MiB. */
uint64_t nvn_shader_extent(const uint8_t *mem, uint64_t code);
/** @brief The key (SHA-256 hex) of the program at code and its length; false when it has no end. */
bool nvn_shader_key(const uint8_t *mem, uint64_t code, char key[65], uint64_t *len);
/** @brief Records the program at code in the shader folder unless the table has it; its key goes to key. False when it has no end. */
bool nvn_shader_note(const uint8_t *mem, uint64_t code, char key[65]);
/** @brief The NVNshaderStage of the program at code (vertex 0, fragment 1, geometry 2, tessellation control 3 and evaluation 4, compute 5). */
int nvn_shader_stage(const uint8_t *mem, uint64_t code);
/** @brief NVNdepthMode and NVNwindowOriginMode of the last device initialized (nvn_dev.c). */
uint32_t nvn_device_depth_mode(void);
uint32_t nvn_device_origin_mode(void);
/**
 * @brief Counts one guest frame for a texture the game presented through
 * NVN2, as vi counts a queued buffer: the input script, mods and frame
 * statistics step on it (nvn_vk.c).
 */
void nvn_frame_presented(void);
/** @brief NVNwindowOriginMode: lower left (the default) or upper left. */
#define NVN_ORIGIN_LOWER_LEFT 0u
#define NVN_ORIGIN_UPPER_LEFT 1u
/**
 * @brief The host viewport (x, y, width, height in out) of NVN viewport v
 * on a render area height rows tall.
 *
 * The driver programs the viewport scale and offset as x + w/2 +- w/2 and
 * y + h/2 + s h/2 with s = -1 in upper-left origin mode and +1 in
 * lower-left mode, where the hardware counts rows from the bottom; either
 * way clip-space +y lands on the image's first row in memory. Vulkan maps
 * +y to the last row, so the host viewport has a negative height.
 */
void nvn_window_viewport(uint32_t origin, uint32_t height, const float v[4], float out[4]);
/** @brief The first memory row of a window rectangle at y, h rows tall, on a render area height rows tall. */
int64_t nvn_window_top(uint32_t origin, uint32_t height, int64_t y, int64_t h);
/** @brief The translation of the program with key (malloc'd SPIR-V words), or NULL. */
uint32_t *nvn_shader_spirv(const char *key, size_t *words);
/** @brief An interface record of a translation (relift build's "RLSPI001" table). */
typedef struct nvn_iface_rec {
    /** @brief NVN_IFACE_*. */
    uint32_t kind;
    /** @brief The constant bank and byte offset of the bound texture handle. */
    uint32_t bank, offset;
    /** @brief The specialization constant id (kinds and dims). */
    uint32_t spec;
} nvn_iface_rec;
/** @brief Record kinds: a bound handle's numeric kind constant, its dimension constant, guest memory reads, a bound handle. */
#define NVN_IFACE_TEXTURE_KIND 0u
#define NVN_IFACE_TEXTURE_DIM 1u
#define NVN_IFACE_GLOBAL_MEMORY 2u
#define NVN_IFACE_TEXTURE 3u
/** @brief Up to max interface records of the translation with key to out; the count (0 without an entry). */
uint32_t nvn_shader_iface(const char *key, nvn_iface_rec *out, uint32_t max);
/** @brief Shader stages a program holds. */
#define NVN_PROGRAM_STAGES 6u
/** @brief Guest memory imported as a host Vulkan buffer (nvn_vk.c); handles kept opaque here. */
typedef struct nvn_import {
    void *buf, *memory;
    /** @brief The buffer's device address: the GPU address the game sees for the memory. */
    uint64_t gpu;
} nvn_import;
/**
 * @brief Imports size guest bytes at guest as a buffer whose device address is
 * the GPU address (host Vulkan up first). Each import of the same bytes gets
 * addresses of its own over the same pages.
 */
bool nvn_vk_import(uint64_t guest, uint64_t size, nvn_import *out);
/** @brief Frees an import (waits for the device). */
void nvn_vk_unimport(nvn_import *im);
/** @brief The guest address of GPU address gpu (an address in a memory pool), or gpu when no pool holds it. */
uint64_t nvn_gpu_guest(uint64_t gpu);
/**
 * @brief The imported buffer (*buf, a VkBuffer) and offset (*off) that hold
 * the size guest bytes at guest; false when no imported pool holds them all.
 */
bool nvn_guest_buffer(uint64_t guest, uint64_t size, void **buf, uint64_t *off);
/** @brief A stage's compiler constants (its constant bank c[1]): GPU address and bytes, 0 and 0 for none. */
typedef struct nvn_consts {
    uint64_t addr;
    uint32_t size;
} nvn_consts;
/**
 * @brief The code (guest addresses) of the stages of the program at guest
 * address prog (count in *n), or NULL; *consts and *control (when not NULL)
 * get each stage's compiler constants and the guest address of its GLSLC
 * control section (0 for none).
 */
const uint64_t *nvn_program_code(uint64_t prog, uint32_t *n, const nvn_consts **consts, const uint64_t **control);

/** @brief Bytes of an NVNtextureView. */
#define NVN_VIEW_BYTES 16u

/** @brief What an NVNtextureView changes about its texture; a field the view leaves alone has its has_ flag false. */
typedef struct nvn_view {
    bool has_levels, has_layers, has_format, has_swizzle, has_ds_mode, has_target;
    int32_t base_level, num_levels, min_layer, num_layers;
    uint32_t format, swizzle[4], ds_mode, target;
} nvn_view;

/**
 * @brief Reads the NVNtextureView whose bytes are view (a copy) through the SDK driver's getters, as a call from
 * the proc running on c. Returns RL_OK, or RL_PENDING, which the caller must return at once.
 */
int nvn_view_read(rl_ctx *c, uint8_t *mem, const uint8_t view[NVN_VIEW_BYTES], nvn_view *out);

/** @brief Command buffer procs that are not recorded (nvn_cmd.c). */
#define NVN_CMDBUF_PROCS(X)                                                                                            \
    X(nvnCommandBufferInitialize)                                                                                      \
    X(nvnCommandBufferFinalize)                                                                                        \
    X(nvnCommandBufferAddCommandMemory)                                                                                \
    X(nvnCommandBufferAddControlMemory)                                                                                \
    X(nvnCommandBufferGetCommandMemorySize)                                                                            \
    X(nvnCommandBufferGetCommandMemoryUsed)                                                                            \
    X(nvnCommandBufferGetCommandMemoryFree)                                                                            \
    X(nvnCommandBufferGetControlMemorySize)                                                                            \
    X(nvnCommandBufferGetControlMemoryUsed)                                                                            \
    X(nvnCommandBufferGetControlMemoryFree)                                                                            \
    X(nvnCommandBufferSetMemoryCallback)                                                                               \
    X(nvnCommandBufferSetMemoryCallbackData)                                                                           \
    X(nvnCommandBufferSetRecordingMode)                                                                                \
    X(nvnCommandBufferBeginRecording)                                                                                  \
    X(nvnCommandBufferEndRecording)

/** @brief Command buffer procs recorded as commands (nvn_cmd.c), as X(op, proc). */
#define NVN_RECORDED(X) \
    X(NVN_CMD_BARRIER, nvnCommandBufferBarrier) \
    X(NVN_CMD_BEGIN_TRANSFORM_FEEDBACK, nvnCommandBufferBeginTransformFeedback) \
    X(NVN_CMD_BIND_BLEND_STATE, nvnCommandBufferBindBlendState) \
    X(NVN_CMD_BIND_CHANNEL_MASK_STATE, nvnCommandBufferBindChannelMaskState) \
    X(NVN_CMD_BIND_COLOR_STATE, nvnCommandBufferBindColorState) \
    X(NVN_CMD_BIND_DEPTH_STENCIL_STATE, nvnCommandBufferBindDepthStencilState) \
    X(NVN_CMD_BIND_IMAGE, nvnCommandBufferBindImage) \
    X(NVN_CMD_BIND_MULTISAMPLE_STATE, nvnCommandBufferBindMultisampleState) \
    X(NVN_CMD_BIND_POLYGON_STATE, nvnCommandBufferBindPolygonState) \
    X(NVN_CMD_BIND_PROGRAM, nvnCommandBufferBindProgram) \
    X(NVN_CMD_BIND_STORAGE_BUFFER, nvnCommandBufferBindStorageBuffer) \
    X(NVN_CMD_BIND_TEXTURE, nvnCommandBufferBindTexture) \
    X(NVN_CMD_BIND_TRANSFORM_FEEDBACK_BUFFER, nvnCommandBufferBindTransformFeedbackBuffer) \
    X(NVN_CMD_BIND_UNIFORM_BUFFER, nvnCommandBufferBindUniformBuffer) \
    X(NVN_CMD_BIND_UNIFORM_BUFFERS, nvnCommandBufferBindUniformBuffers) \
    X(NVN_CMD_BIND_VERTEX_ATTRIB_STATE, nvnCommandBufferBindVertexAttribState) \
    X(NVN_CMD_BIND_VERTEX_BUFFER, nvnCommandBufferBindVertexBuffer) \
    X(NVN_CMD_BIND_VERTEX_STREAM_STATE, nvnCommandBufferBindVertexStreamState) \
    X(NVN_CMD_CLEAR_COLOR, nvnCommandBufferClearColor) \
    X(NVN_CMD_CLEAR_COLORI, nvnCommandBufferClearColori) \
    X(NVN_CMD_CLEAR_COLORUI, nvnCommandBufferClearColorui) \
    X(NVN_CMD_CLEAR_DEPTH_STENCIL, nvnCommandBufferClearDepthStencil) \
    X(NVN_CMD_COPY_BUFFER_TO_BUFFER, nvnCommandBufferCopyBufferToBuffer) \
    X(NVN_CMD_COPY_BUFFER_TO_TEXTURE, nvnCommandBufferCopyBufferToTexture) \
    X(NVN_CMD_COPY_TEXTURE_TO_BUFFER, nvnCommandBufferCopyTextureToBuffer) \
    X(NVN_CMD_COPY_TEXTURE_TO_TEXTURE, nvnCommandBufferCopyTextureToTexture) \
    X(NVN_CMD_DISPATCH_COMPUTE, nvnCommandBufferDispatchCompute) \
    X(NVN_CMD_DRAW_ARRAYS, nvnCommandBufferDrawArrays) \
    X(NVN_CMD_DRAW_ELEMENTS, nvnCommandBufferDrawElements) \
    X(NVN_CMD_DRAW_ELEMENTS_BASE_VERTEX, nvnCommandBufferDrawElementsBaseVertex) \
    X(NVN_CMD_END_TRANSFORM_FEEDBACK, nvnCommandBufferEndTransformFeedback) \
    X(NVN_CMD_FENCE_SYNC, nvnCommandBufferFenceSync) \
    X(NVN_CMD_REPORT_COUNTER, nvnCommandBufferReportCounter) \
    X(NVN_CMD_RESET_COUNTER, nvnCommandBufferResetCounter) \
    X(NVN_CMD_SET_COPY_IMAGE_STRIDE, nvnCommandBufferSetCopyImageStride) \
    X(NVN_CMD_SET_COPY_ROW_STRIDE, nvnCommandBufferSetCopyRowStride) \
    X(NVN_CMD_SET_DEPTH_BOUNDS, nvnCommandBufferSetDepthBounds) \
    X(NVN_CMD_SET_DEPTH_RANGES, nvnCommandBufferSetDepthRanges) \
    X(NVN_CMD_SET_PATCH_SIZE, nvnCommandBufferSetPatchSize) \
    X(NVN_CMD_SET_POLYGON_OFFSET_CLAMP, nvnCommandBufferSetPolygonOffsetClamp) \
    X(NVN_CMD_SET_PRIMITIVE_RESTART, nvnCommandBufferSetPrimitiveRestart) \
    X(NVN_CMD_SET_RASTERIZER_DISCARD, nvnCommandBufferSetRasterizerDiscard) \
    X(NVN_CMD_SET_RENDER_TARGETS, nvnCommandBufferSetRenderTargets) \
    X(NVN_CMD_SET_SAMPLER_POOL, nvnCommandBufferSetSamplerPool) \
    X(NVN_CMD_SET_SCISSOR, nvnCommandBufferSetScissor) \
    X(NVN_CMD_SET_SCISSORS, nvnCommandBufferSetScissors) \
    X(NVN_CMD_SET_SHADER_SCRATCH_MEMORY, nvnCommandBufferSetShaderScratchMemory) \
    X(NVN_CMD_SET_STENCIL_MASK, nvnCommandBufferSetStencilMask) \
    X(NVN_CMD_SET_STENCIL_REF, nvnCommandBufferSetStencilRef) \
    X(NVN_CMD_SET_STENCIL_VALUE_MASK, nvnCommandBufferSetStencilValueMask) \
    X(NVN_CMD_SET_TILED_CACHE_ACTION, nvnCommandBufferSetTiledCacheAction) \
    X(NVN_CMD_SET_TEXTURE_POOL, nvnCommandBufferSetTexturePool) \
    X(NVN_CMD_SET_TILED_CACHE_TILE_SIZE, nvnCommandBufferSetTiledCacheTileSize) \
    X(NVN_CMD_SET_VIEWPORTS, nvnCommandBufferSetViewports) \
    X(NVN_CMD_WAIT_SYNC, nvnCommandBufferWaitSync)

/** @brief Recorded command ops. */
enum nvn_cmd_op {
    NVN_CMD_NONE = 0,
#define NVN_CMD_ENUM(op, proc) op,
    NVN_RECORDED(NVN_CMD_ENUM)
#undef NVN_CMD_ENUM
    NVN_CMD_COUNT
};

/** @brief One recorded command: its op and argument registers. */
typedef struct nvn_cmd {
    uint32_t op;
    uint64_t x[8];
    uint64_t d[8];
} nvn_cmd;

/** @brief The commands of one NVNcommandHandle. */
typedef struct nvn_cmdset {
    uint32_t n;
    /** @brief Guest bytes the commands pointed at, copied when they were recorded. */
    uint32_t nbytes;
    uint8_t *bytes;
    nvn_cmd cmds[];
} nvn_cmdset;

/** @brief Marks a recorded argument that is an offset into its set's bytes instead of a guest pointer. */
#define NVN_SNAP (1ull << 63)
/** @brief The bytes a recorded argument v points at (one marked NVN_SNAP), or NULL for a guest NULL. */
const void *nvn_cmd_bytes(const nvn_cmdset *s, uint64_t v);
/** @brief Runs command set s on relift's Vulkan device for queue (nvn_vk.c). Returns RL_OK, or RL_PENDING after a stop. */
int nvn_vk_run(rl_ctx *c, uint8_t *mem, uint64_t queue, const nvn_cmdset *s);

/** @brief Records the calling proc as op in its command buffer (x0) and returns as the proc. */
uint64_t nvn_cmd_record(rl_ctx *c, uint8_t *mem, uint32_t op);
/** @brief The proc name of slot (one relift gave a name it has no function for), or NULL. */
const char *nvn_slot_name(uint32_t slot);
/** @brief The proc name of op. */
const char *nvn_cmd_name(uint32_t op);
/** @brief The command set of handle, or NULL. */
const nvn_cmdset *nvn_cmdset_of(uint64_t handle);
/** @brief Runs the command set of handle on queue. Returns RL_OK, or RL_PENDING after a stop. */
int nvn_cmd_submit(rl_ctx *c, uint8_t *mem, uint64_t queue, uint64_t handle);
/** @brief The guest address of a memory pool's storage and its size, or 0. */
uint64_t nvn_pool_memory(uint64_t pool, uint64_t *size);
/** @brief The GPU address of a memory pool's first byte. */
/** @brief Makes the layer's record of a memory pool the driver initialized (imported into the host device unless virtual). */
bool nvn_pool_make(uint64_t pool, uint64_t memory, uint64_t size, uint32_t flags);
uint64_t nvn_pool_gpu(uint64_t pool);

/** @brief Device, queue, memory and object procs (nvn_dev.c). */
#define NVN_DEVICE_PROCS(X)                                                                                            \
    X(nvnDeviceInitialize)                                                                                             \
    X(nvnDeviceFinalize)                                                                                               \
    X(nvnDeviceGetInteger)                                                                                             \
    X(nvnDeviceSetWindowOriginMode)                                                                                    \
    X(nvnDeviceSetDepthMode)                                                                                           \
    X(nvnDeviceGetImageHandle)                                                                                         \
    X(nvnDeviceGetTextureHandle)                                                                                       \
    X(nvnQueueInitialize)                                                                                              \
    X(nvnQueueFinalize)                                                                                                \
    X(nvnQueueFlush)                                                                                                   \
    X(nvnQueueFinish)                                                                                                  \
    X(nvnQueueSubmitCommands)                                                                                          \
    X(nvnQueueFenceSync)                                                                                               \
    X(nvnQueueWaitSync)                                                                                                \
    X(nvnMemoryPoolInitialize)                                                                                         \
    X(nvnMemoryPoolFinalize)                                                                                           \
    X(nvnMemoryPoolMap)                                                                                                \
    X(nvnMemoryPoolFlushMappedRange)                                                                                   \
    X(nvnMemoryPoolInvalidateMappedRange)                                                                              \
    X(nvnMemoryPoolGetBufferAddress)                                                                                   \
    X(nvnMemoryPoolGetFlags)                                                                                           \
    X(nvnMemoryPoolGetSize)                                                                                            \
    X(nvnMemoryPoolMapVirtual)                                                                                         \
    X(nvnBufferInitialize)                                                                                             \
    X(nvnBufferFinalize)                                                                                               \
    X(nvnBufferMap)                                                                                                    \
    X(nvnBufferGetAddress)                                                                                             \
    X(nvnBufferGetSize)                                                                                                \
    X(nvnBufferGetMemoryPool)                                                                                          \
    X(nvnBufferGetMemoryOffset)                                                                                        \
    X(nvnBufferFlushMappedRange)                                                                                       \
    X(nvnBufferInvalidateMappedRange)                                                                                  \
    X(nvnSyncInitialize)                                                                                               \
    X(nvnSyncFinalize)                                                                                                 \
    X(nvnSyncWait)

/** @brief Kinds of relift's NVN objects (nvn_obj.kind). */
enum nvn_kind {
    NVN_DEVICE = 1,
    NVN_QUEUE,
    NVN_POOL,
    NVN_BUFFER,
    NVN_SYNC,
    NVN_TEXTURE,
    NVN_SAMPLER,
    NVN_TEXTURE_POOL,
    NVN_SAMPLER_POOL,
    NVN_WINDOW,
    NVN_CMDBUF,
    NVN_PROGRAM,
    NVN_EVENT
};

/** @brief The head of every relift NVN object; a kind's struct starts with it. */
typedef struct nvn_obj nvn_obj;
struct nvn_obj {
    /** @brief Guest address of the game's NVN object. */
    uint64_t addr;
    /** @brief enum nvn_kind. */
    uint32_t kind;
    /** @brief Frees what the kind holds besides the struct itself; may be NULL. */
    void (*drop)(nvn_obj *o);
};

/** @brief A slot left by a freed object in the object table. */
#define NVN_TOMB ((nvn_obj *)(uintptr_t)1)

/** @brief A new zeroed object of size bytes for the NVN object at addr, replacing any object there; NULL when out of memory. */
nvn_obj *nvn_obj_new(uint64_t addr, uint32_t kind, size_t size);
/** @brief The object for the NVN object at addr, of kind (0: any kind), or NULL. */
nvn_obj *nvn_obj_get(uint64_t addr, uint32_t kind);
/** @brief Removes and frees o; NULL does nothing. */
void nvn_obj_free(nvn_obj *o);

/**
 * @brief Calls the driver's own proc name (one nvn_driver_proc accepts) with
 * up to 8 integer arguments; *ret gets x0. Returns RL_OK, or RL_PENDING,
 * which the calling proc returns at once. A name the driver does not have
 * stops the game.
 */
int nvn_driver_call(rl_ctx *c, uint8_t *mem, const char *name, const uint64_t *args, unsigned n, uint64_t *ret);


/** @brief Number of procs that stay the SDK driver's own code (nvn_driver.c). */
#define NVN_DRIVER_PROC_COUNT 308u

/** @brief The index of a proc that stays the SDK driver's own code (nvn_driver.c), or -1. */
int nvn_driver_index(const char *name);

/**
 * @brief Runs the guest function at fn with up to 8 integer arguments, as a
 * call from the proc running on c; the proc's own return address and pc are
 * kept. Stores x0 in *ret (when ret is not NULL). Returns RL_OK, or
 * RL_PENDING, which the proc must return at once.
 */
int nvn_guest_call(rl_ctx *c, uint8_t *mem, uint64_t fn, const uint64_t *args, unsigned n, uint64_t *ret);

/**
 * @brief Opens the GPU as the SDK driver does when its device starts: the
 * program's own NvRmGpuLibOpen, then NvRmGpuDeviceOpen of the default
 * device, which ask nvdrv about the GPU. Once per process; RL_OK, RL_PENDING,
 * or a stop when either fails.
 */
int nvn_gpu_open(rl_ctx *c, uint8_t *mem);

/** @brief The driver's own function for any proc name, through its loader (0 when it has none); RL_OK or RL_PENDING. */
int nvn_driver_lookup(rl_ctx *c, uint8_t *mem, const char *name, uint64_t *addr);

/** @brief The NvRmGpuDevice nvn_gpu_open opened (a guest address), 0 before. */
uint64_t nvn_gpu_device(void);
/**
 * @brief The u32 at byte off of what the program's NvRmGpuDeviceGetInfo
 * returns for the GPU (opened first). Returns RL_OK or a pending stop.
 */
int nvn_gpu_info_u32(rl_ctx *c, uint8_t *mem, int64_t off, uint32_t *out);

/** @brief A normal return from a proc: the guest address the return goes to (docs/abi.md section 5.4). */
static inline uint64_t nvn_ret(const rl_ctx *c) { return c->x[30]; }

/** @brief Longest proc name the layer reads from the guest. */
#define NVN_NAME_MAX 128u

/** @brief The NUL-terminated guest string at a, or NULL when a is 0 or the string does not end within max bytes. */
static inline const char *nvn_str(uint8_t *mem, uint64_t a, size_t max)
{
    if (!a)
        return NULL;
    const char *s = (const char *)(mem + a);
    return memchr(s, 0, max) ? s : NULL;
}

#define NVN_DECLARE(name) uint64_t nvnh_##name(rl_ctx *c, uint8_t *mem);
#define NVN_DECLARE_RECORDED(op, name) NVN_DECLARE(name)
NVN_PROCS(NVN_DECLARE)
NVN_RECORDED(NVN_DECLARE_RECORDED)
#undef NVN_DECLARE_RECORDED
#undef NVN_DECLARE

#endif
