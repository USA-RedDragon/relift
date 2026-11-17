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
    X(nvnProgramSetShaders)                                                                                            \
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
    X(nvnWindowSetNumActiveTextures)

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
    X(nvnTexturePoolRegisterTexture)                                                                                   \
    X(nvnTexturePoolRegisterImage)                                                                                     \
    X(nvnSamplerPoolRegisterSampler)

/** @brief Most mip levels relift records offsets for. */
#define NVN_TEX_MAX_LEVELS 16u

/** @brief A texture as relift lays it out. */
typedef struct nvn_tex_desc {
    uint32_t target, format, width, height, depth, levels, samples, flags;
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
    X(NVN_CMD_CLEAR_DEPTH_STENCIL, nvnCommandBufferClearDepthStencil) \
    X(NVN_CMD_COPY_BUFFER_TO_BUFFER, nvnCommandBufferCopyBufferToBuffer) \
    X(NVN_CMD_COPY_BUFFER_TO_TEXTURE, nvnCommandBufferCopyBufferToTexture) \
    X(NVN_CMD_COPY_TEXTURE_TO_BUFFER, nvnCommandBufferCopyTextureToBuffer) \
    X(NVN_CMD_COPY_TEXTURE_TO_TEXTURE, nvnCommandBufferCopyTextureToTexture) \
    X(NVN_CMD_DRAW_ELEMENTS_BASE_VERTEX, nvnCommandBufferDrawElementsBaseVertex) \
    X(NVN_CMD_END_TRANSFORM_FEEDBACK, nvnCommandBufferEndTransformFeedback) \
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
    X(NVN_CMD_SET_SCISSOR, nvnCommandBufferSetScissor) \
    X(NVN_CMD_SET_SCISSORS, nvnCommandBufferSetScissors) \
    X(NVN_CMD_SET_STENCIL_MASK, nvnCommandBufferSetStencilMask) \
    X(NVN_CMD_SET_STENCIL_REF, nvnCommandBufferSetStencilRef) \
    X(NVN_CMD_SET_STENCIL_VALUE_MASK, nvnCommandBufferSetStencilValueMask) \
    X(NVN_CMD_SET_TILED_CACHE_ACTION, nvnCommandBufferSetTiledCacheAction) \
    X(NVN_CMD_SET_TILED_CACHE_TILE_SIZE, nvnCommandBufferSetTiledCacheTileSize) \
    X(NVN_CMD_SET_VIEWPORTS, nvnCommandBufferSetViewports)

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
    nvn_cmd cmds[];
} nvn_cmdset;

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
#define NVN_DRIVER_PROC_COUNT 245u

/** @brief The index of a proc that stays the SDK driver's own code (nvn_driver.c), or -1. */
int nvn_driver_index(const char *name);

/**
 * @brief Runs the guest function at fn with up to 8 integer arguments, as a
 * call from the proc running on c; the proc's own return address and pc are
 * kept. Stores x0 in *ret (when ret is not NULL). Returns RL_OK, or
 * RL_PENDING, which the proc must return at once.
 */
int nvn_guest_call(rl_ctx *c, uint8_t *mem, uint64_t fn, const uint64_t *args, unsigned n, uint64_t *ret);

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
