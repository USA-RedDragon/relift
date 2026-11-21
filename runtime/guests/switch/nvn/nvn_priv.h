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
    NVN_DLSS_PROCS(X)                                                                                                  \
    X(nvnWindowInitialize)                                                                                             \
    X(nvnWindowFinalize)                                                                                               \
    X(nvnWindowAcquireTexture)                                                                                         \
    X(nvnQueueAcquireTexture)                                                                                          \
    X(nvnWindowSetPresentInterval)                                                                                     \
    X(nvnWindowGetPresentInterval)                                                                                     \
    X(nvnWindowGetNativeWindow)                                                                                        \
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
    X(nvnDeviceRegisterFastClearColor)                                                                                 \
    X(nvnDeviceRegisterFastClearDepth)                                                                                 \
    X(nvnCommandBufferSetColorReductionEnable)                                                                         \
    X(nvnCommandBufferSetColorReductionThresholds)                                                                     \
    X(nvnCommandBufferSetStencilCullCriteria)                                                                          \
    X(nvnCommandBufferOverrideSubtileSize)                                                                             \
    X(nvnCommandBufferSetSubpixelPrecisionBias)                                                                        \
    X(nvnCommandBufferDiscardColor)                                                                                    \
    X(nvnCommandBufferDiscardDepthStencil)

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
    X(nvnCommandBufferSetDebugLabel)                                                                                   \
    X(nvnCommandBufferInsertDebugMarker)                                                                               \
    X(nvnCommandBufferPushDebugGroup)                                                                                  \
    X(nvnCommandBufferPopDebugGroup)                                                                                   \
    X(nvnCommandBufferInsertDebugMarkerDynamic)                                                                        \
    X(nvnCommandBufferInsertDebugMarkerStatic)                                                                         \
    X(nvnCommandBufferPushDebugGroupDynamic)                                                                           \
    X(nvnCommandBufferPushDebugGroupStatic)                                                                            \
    X(nvnCommandBufferPopDebugGroupId)                                                                                 \
    X(nvnCommandBufferPushDebugGroupCompute)                                                                           \
    X(nvnCommandBufferPushDebugGroupGraphics)                                                                          \
    X(nvnCommandBufferPopDebugGroupCompute)                                                                            \
    X(nvnCommandBufferPopDebugGroupGraphics)                                                                           \
    X(nvnDeviceApplyDeferredFinalizes)

/** @brief Returns 0 from a proc of NVN_ZERO_PROCS. */
uint64_t nvn_zero(rl_ctx *c, uint8_t *mem);

/** @brief Frees the command set of handle; any other value does nothing. */
void nvn_cmdset_free(uint64_t handle);

/** @brief Marks the sync at guest address sync signaled; 0 does nothing. */
void nvn_sync_signal(uint64_t sync);
/** @brief Batches submitted to the host device so far, and the count of them known finished (nvn_vk.c). */
uint64_t nvn_vk_submitted(void);
uint64_t nvn_vk_finished(void);
/**
 * @brief Checks that a submit runs while the caller goes on: a long batch that ends by writing a buffer is submitted
 * with nvn_vk_submit, which must return before the device finished it, and after nvn_vk_settle the CPU must read
 * what the batch wrote. 0 when both hold, the failing step's number otherwise, -1 with no device.
 */
int nvn_vk_async_check(void);
/**
 * @brief Shows the crop rectangle crop (x, y, width, height, as
 * nvnWindowSetCrop takes it) of texture (an NVNtexture the game rendered) as
 * the next frame, after the work queue submitted. Returns RL_OK or
 * RL_PENDING.
 */
int nvn_present(rl_ctx *c, uint8_t *mem, uint64_t queue, uint64_t texture, const int32_t crop[4], uint32_t interval);
/**
 * @brief The part of a width x height texture that crop (x, y, width,
 * height) shows, into out (x, y, width, height): the whole texture when
 * crop's width or height is 0, else crop clipped to the texture. False when
 * nothing of the texture is left.
 */
bool nvn_crop_rect(const int32_t crop[4], uint32_t width, uint32_t height, uint32_t out[4]);

/** @brief Texture, sampler and descriptor pool procs (nvn_tex.c). */
#define NVN_TEXTURE_PROCS(X)                                                                                           \
    X(nvnTextureBuilderGetStorageSize)                                                                                 \
    X(nvnTextureBuilderGetStorageClass)                                                                                \
    X(nvnTextureBuilderGetStorageAlignment)                                                                            \
    X(nvnTextureBuilderGetZCullStorageSize)                                                                            \
    X(nvnTextureInitialize)                                                                                            \
    X(nvnTextureFinalize)                                                                                              \
    X(nvnTextureGetWidth)                                                                                              \
    X(nvnTextureGetHeight)                                                                                             \
    X(nvnTextureGetDepth)                                                                                              \
    X(nvnTextureGetFormat)                                                                                             \
    X(nvnTextureGetTarget)                                                                                             \
    X(nvnTextureGetFlags)                                                                                              \
    X(nvnTextureGetStorageSize)                                                                                        \
    X(nvnTextureGetSwizzle)                                                                                            \
    X(nvnTextureGetDepthStencilMode)                                                                                   \
    X(nvnTextureGetStorageClass)                                                                                       \
    X(nvnTextureCompare)                                                                                               \
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
    X(nvnTexturePoolGetMemoryPool)                                                                                     \
    X(nvnTexturePoolGetMemoryOffset)                                                                                   \
    X(nvnSamplerPoolGetMemoryPool)                                                                                     \
    X(nvnSamplerPoolGetMemoryOffset)                                                                                   \
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
    /** @brief NVNdepthStencilMode of the builder, as nvnTextureGetDepthStencilMode answers it. */
    uint32_t ds_mode;
    uint64_t stride, pool, offset;
    /** @brief Bytes of storage, its alignment and the bytes from one layer to the next. */
    uint64_t size, align, layer_stride;
    /** @brief Block height and depth of level 0 in GOBs; 0 for linear storage. */
    uint32_t block_h, block_d;
    /** @brief Offset of each level inside a layer. */
    uint64_t level_offset[NVN_TEX_MAX_LEVELS];
} nvn_tex_desc;

/**
 * @brief The block height and depth, in GOBs, that level level of the block-linear texture d is laid out with (the
 * level's own, as nvn_tex_copy uses: smaller than level 0's once the level is too small for it).
 */
void nvn_tex_level_blocks(const nvn_tex_desc *d, uint32_t level, uint32_t *gbh, uint32_t *gbd);

/** @brief NVNcopyRegion: a box of texels. */
typedef struct nvn_region {
    int32_t x, y, z, width, height, depth;
} nvn_region;

/** @brief Bytes per block and block size of an NVNformat; false for a format relift does not know. */
bool nvn_tex_format(uint32_t format, uint32_t *bpb, uint32_t *bw, uint32_t *bh);

/** @brief The buffer side of a copy between a buffer and a texture region. */
typedef struct nvn_copy_pitch {
    /** @brief Bytes from one row of blocks to the next, and from one layer or slice to the next. */
    uint64_t row_bytes, image_bytes;
    /** @brief The same as Vulkan counts them: texels per row, rows of texels per image. */
    uint32_t row_texels, image_rows;
    /** @brief Bytes the copy reads or writes, from the first block to the last. */
    uint64_t span;
} nvn_copy_pitch;

/**
 * @brief The buffer layout of a copy of region rg of a texture of NVNformat
 * format with the command buffer's copy strides, as the driver computes it:
 * a stride of 0 packs rows (blocks per row times bytes per block) or images
 * (row bytes times block rows). False when the format is unknown, the region
 * is empty, or a stride is not a whole number of blocks or rows (no Vulkan
 * copy can express it).
 */
bool nvn_copy_pitch_of(uint32_t format, const nvn_region *rg, uint32_t row_stride, uint32_t image_stride,
                       nvn_copy_pitch *out);
/** @brief Fills d's size, alignment, block and level fields from its target, format, size, levels, samples and flags; false for an unknown format. */
bool nvn_tex_layout(nvn_tex_desc *d);
/** @brief nvn_tex_layout with blocks block_h GOBs tall at level 0 (0: the driver's usual choice); 3D textures keep theirs. */
bool nvn_tex_layout_block(nvn_tex_desc *d, uint32_t block_h);
/**
 * @brief Whether a texture of d can be rendered to at level and layer: a
 * layer of an array or cube texture, or a depth slice of that level of a 3D
 * texture.
 */
bool nvn_vk_rt_inside(const nvn_tex_desc *d, uint32_t level, uint32_t layer);

/**
 * @brief Copies region r of level and layer of a texture laid out as d with
 * its storage at storage, to (to_storage) or from packed texels (rows of r's
 * width, tightly packed). False when r lies outside the level.
 */
bool nvn_tex_copy(const nvn_tex_desc *d, uint8_t *storage, uint32_t level, uint32_t layer, const nvn_region *r,
                  uint8_t *packed, bool to_storage);
/** @brief Copies the layout of the texture at guest address tex into out; false when there is none. No pointer into the object escapes. */
bool nvn_texture_desc(uint64_t tex, nvn_tex_desc *out);
/** @brief Bytes of an NVN1 texture builder the layer keeps with the texture it initializes (the driver's builder reads up to +108). */
#define NVN_TEXTURE_BUILDER_BYTES 128u
/**
 * @brief Records a texture of layout d at guest address tex, as nvnTextureInitialize does once it has the layout, with
 * the NVN_TEXTURE_BUILDER_BYTES bytes of the builder it came from (NULL for none); false when it cannot.
 */
bool nvn_texture_record(uint64_t tex, const nvn_tex_desc *d, const uint8_t *builder);
/**
 * @brief The NVNstorageClass the driver's own function answers for the texture builder at builder (0 when the build
 * read no driver layout), as nvnTextureBuilderGetStorageClass does. Returns RL_OK or a pending stop.
 */
int nvn_builder_storage_class(rl_ctx *c, uint8_t *mem, uint64_t builder, uint64_t *cls);
/**
 * @brief Texture id's registration: the texture's guest address (0 when the id is free), whether it was
 * registered as an image, and a copy of its view's bytes (has_view false for none).
 */
uint64_t nvn_tex_registered(uint32_t id, uint8_t view[16], bool *has_view, bool *image);
/** @brief Copies the builder bytes of the sampler registered as id (128 bytes) into out; false when none. */
bool nvn_sampler_registered(uint32_t id, uint8_t out[128]);
/**
 * @brief Takes up to max ids of the textures (sampler false) or samplers registered since the last call, oldest
 * first, one entry per registration; returns how many.
 */
uint32_t nvn_tex_pending(bool sampler, uint32_t *ids, uint32_t max);

/** @brief What the device probes measured (nvn_probe.c, run by the Vulkan layer before it writes device.json). */
typedef struct nvn_probes {
    /** @brief Every single-precision NaN of add, multiply, fused multiply-add, NMin and NMax is 0x7fffffff. */
    bool canonical_nan32;
    /** @brief Half-precision add, multiply, fused multiply-add and conversions are exact binary16, NaNs aside. */
    bool exact_f16;
    /**
     * @brief VK_KHR_cooperative_matrix half 16x8x16 and 16x8x8 multiply-adds, built from and read into each lane's
     * components in the translator's register order, give the SM86 HMMA.F16 results.
     */
    bool hmma_coop;
    /**
     * @brief What single-precision add and multiply do with denormals under the execution modes the translator emits
     * with no denormal mode: 1 flush as SM86 `.FTZ` does (inputs read as zero, results rounded to 24 bits and then
     * flushed), 2 keep them as IEEE 754 does, 0 neither (or SignedZeroInfNanPreserve is missing).
     */
    uint32_t denorm32;
    /** @brief Whether addition follows the flushing model (inputs read as zero, results rounded to 24 bits and flushed) exactly. */
    bool denorm_add_flush;
    /**
     * @brief The smallest correction of a multiplication result that makes it follow that model: bit 0 the sign of
     * a zero result, bit 1 a result of the smallest normal number that the model flushes (see den_fix); 255 when
     * no combination does.
     */
    uint32_t denorm_mul_fix;
    /** @brief The DenormPreserve execution mode, on top of those, keeps denormals as IEEE 754 does. */
    bool denorm_preserve_mode32;
} nvn_probes;
/** @brief Brings the host device up and gives its probe results; false without a host Vulkan device. */
bool nvn_vk_probes(nvn_probes *out);
/** @brief Rows of the single-precision NaN probe (three input words and five output words each). */
uint32_t nvn_probe_nan_rows(void);
/** @brief Writes the NaN probe's inputs to w[0 .. 3 * rows). */
void nvn_probe_nan_input(uint32_t *w);
/** @brief Whether the NaN probe's outputs (after its inputs in w) are all the guest's canonical NaN where NaN. */
bool nvn_probe_nan_check(const uint32_t *w);
/** @brief Rows of the denormal probe (two input words and two output words each). */
uint32_t nvn_probe_denorm_rows(void);
/** @brief Writes the denormal probe's inputs to w[0 .. 2 * rows). */
void nvn_probe_denorm_input(uint32_t *w);
/** @brief Writes rows random inputs of the denormal probe to w (see nvn_probe.c). */
void nvn_probe_denorm_random(uint32_t *w, uint32_t rows, uint32_t seed);
/** @brief What the denormal probe found: the models each operation matches on every row (bit 0 flush before, 1 IEEE, 2 flush after), and the multiplication correction. */
typedef struct nvn_denorm_class {
    uint32_t add_models, mul_models, mul_fix;
    /** @brief Bit f is set when correction f of the multiplication gives the flushing model on every row. */
    uint32_t mul_fix_ok;
} nvn_denorm_class;
/** @brief Runs the denormal probe on the host device over rows random rows of seed and says what it found (see nvn_probe_denorm_check); false without a device. */
bool nvn_vk_denorm_stress(uint32_t rows, uint32_t seed, nvn_denorm_class *out);
/**
 * @brief Checks nvn_untile_move against nvn_tex_copy on the host device over a set of layouts (block-linear 2D with
 * levels, 3D, arrays, 4, 8 and 16-byte texels, pitch-linear), with guest bytes from seed: 0 when every texel moved
 * exactly both ways and storage outside every imported pool is left to the CPU path (nvn_untile_can false), the
 * failing case's number (from 1, 100 for the fallback) otherwise, -1 with no device.
 */
int nvn_untile_check(uint32_t seed);

/** @brief Checks the outputs (after the rows inputs in w) of the denormal probe; the result is 1 when both operations follow the flushing model, 2 the IEEE one, else 0. */
uint32_t nvn_probe_denorm_check(const uint32_t *w, uint32_t rows, nvn_denorm_class *out);
/** @brief Half-precision probe sizes: arithmetic rows (two lanes each), conversions to half, words in all. */
uint32_t nvn_probe_f16_rows(void);
uint32_t nvn_probe_f16_convs(void);
uint32_t nvn_probe_f16_words(void);
/** @brief Writes the half-precision probe's inputs to w (nvn_probe_f16_words words). */
void nvn_probe_f16_input(uint32_t *w);
/** @brief The half-precision probe's results in w that are not exact binary16 (0 for an exact device). */
uint32_t nvn_probe_f16_check(const uint32_t *w);
/** @brief One fused step of the SM86 HMMA model: eight half products added to the half acc (see gpus/sm86 mma.rs). */
uint32_t nvn_hmma_step(uint32_t acc, const uint16_t a[8], const uint16_t b[8]);
/**
 * @brief HMMA.16816.F16 (k16) or HMMA.1688.F16 over a warp: lane l's A registers at a[4l..], B at b[2l..], C at
 * c[2l..], D written to d[2l..] (the HMMA.1688 A takes a[4l], a[4l + 1] and B b[2l]).
 */
void nvn_hmma_warp(bool k16, const uint32_t *a, const uint32_t *b, const uint32_t *c, uint32_t *d);
/** @brief HMMA probe sizes: tiles of each shape, words in all (inputs, then two output words a lane). */
uint32_t nvn_probe_hmma_tiles(void);
uint32_t nvn_probe_hmma_words(void);
/** @brief Writes the HMMA probe's inputs to w (nvn_probe_hmma_words words). */
void nvn_probe_hmma_input(uint32_t *w);
/** @brief The HMMA probe's output words in w that differ from nvn_hmma_warp (0 for a device that matches). */
uint32_t nvn_probe_hmma_check(const uint32_t *w);
/** @brief The value of half-precision bits h. */
double nvn_half_value(uint16_t h);
/** @brief x rounded to half precision, to nearest even, with subnormals and infinities. */
uint16_t nvn_half_of(double x);

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
/**
 * @brief For an NVN1 program at guest address code (nvn_prog.c): where its
 * bytes after the code header start and how many there are (the SPH if it
 * has one and the code through the last bundle control reaches). False when
 * code holds no NVN1 code header or control leaves the code.
 */
bool nvn_v1_program(const uint8_t *mem, uint64_t code, uint64_t *start, uint64_t *len);
/** @brief Whether b is an NVN1 (version 3) SPH; its NVNshaderStage to *stage when stage is not NULL. */
bool nvn_v1_sph(const uint8_t *b, int *stage);
/** @brief The NVNshaderStage of the program at code (vertex 0, fragment 1, geometry 2, tessellation control 3 and evaluation 4, compute 5). */
int nvn_shader_stage(const uint8_t *mem, uint64_t code);
/**
 * @brief Tells the launch warm-up of a program the game gave a stage: a compute
 * program's pipeline can only be made with the workgroup size its control
 * section holds. Before the warm-up has run the program is kept for it; after,
 * its pipeline is made at once (on a worker thread).
 */
void nvn_warm_program(const uint8_t *mem, uint64_t code, uint64_t control);
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
/** @brief The number of programs in the build's shader table (0 without one). */
uint32_t nvn_shader_count(void);
/** @brief Writes the key (hex) of table entry i; false past the end. */
bool nvn_shader_key_at(uint32_t i, char key[65]);
/** @brief The SHA-256 (hex) of the shader table's index, which names its contents; false without a table. */
bool nvn_shader_table_hash(char hex[65]);
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
/** @brief Record kinds of a task or mesh program's pipeline (see nvn_mesh_info). */
#define NVN_IFACE_MESH 4u
#define NVN_IFACE_MESH_LIMITS 5u
#define NVN_IFACE_MESH_RECORD 6u
/**
 * @brief What relift build's translation of a task or mesh program says of its pipeline: the stage (6 or 7), whether
 * the native back end (VK_EXT_mesh_shader) draws it, else the compute back end does, whether that draw is indexed,
 * the guest workgroups its compute workgroups run, the output limits and topology (1 points, 2 lines, 3 triangles),
 * the stage buffer, mesh record and index words, and the task and mesh workgroup sizes.
 */
typedef struct nvn_mesh_info {
    bool present, native, indexed;
    uint32_t stage, slots, topology, max_vertices, max_primitives, payload_words, record_words, index_words;
    uint32_t task_lanes, mesh_lanes;
} nvn_mesh_info;
/** @brief The pipeline the n records say a task or mesh program has; false when they say none. */
bool nvn_mesh_info_of(const nvn_iface_rec *recs, uint32_t n, nvn_mesh_info *out);
/**
 * @brief The shader table key of the compute back end's helper program of kind ("scan" or "vertex") for the mesh
 * program with key (relift build writes the helpers under keys made from the mesh program's).
 */
void nvn_shader_helper_key(const char *kind, const char *key, char helper_key[65]);
/** @brief Record kind: the program stores or does atomic read-modify-writes through 64-bit global addresses. */
#define NVN_IFACE_GLOBAL_STORE 7u
/** @brief Up to max interface records of the translation with key to out; the count (0 without an entry). */
uint32_t nvn_shader_iface(const char *key, nvn_iface_rec *out, uint32_t max);
/** @brief Shader stages a program holds. */
#define NVN_PROGRAM_STAGES 8u
/** @brief Guest memory imported as a host Vulkan buffer (nvn_vk.c); handles kept opaque here. */
typedef struct nvn_import {
    void *buf, *memory;
    /** @brief The buffer's device address: the GPU address the game sees for the memory. */
    uint64_t gpu;
    /** @brief The buffer's and the memory's opaque capture addresses (0 without address replay): what takes the address back. */
    uint64_t buf_opaque, mem_opaque;
} nvn_import;
/**
 * @brief Imports size guest bytes at guest as a buffer whose device address is
 * the GPU address (host Vulkan up first). Each import of the same bytes gets
 * addresses of its own over the same pages, except that again, the import
 * of the same bytes that was freed, has its address taken back when the host
 * can (a console's driver gives a pool that is created again the address it
 * had); NULL asks for no address in particular.
 */
bool nvn_vk_import(uint64_t guest, uint64_t size, const nvn_import *again, nvn_import *out);
/** @brief Whether the host gives an import the device address of an earlier one (host Vulkan up first). */
bool nvn_vk_address_replay(void);
/** @brief Frees an import (waits for the device). */
void nvn_vk_unimport(nvn_import *im);
/**
 * @brief Makes a device buffer of size bytes whose device address is the GPU address, kept equal to the size guest
 * bytes at guest (nvn_mirror.c), for a host that cannot import guest pages. False when the device refuses.
 */
bool nvn_mirror_make(uint64_t guest, uint64_t size, nvn_import *out);
/** @brief Frees a mirror made by nvn_mirror_make (waits for the device). */
void nvn_mirror_free(nvn_import *im);
/** @brief Whether the buffer is a mirror's: its bytes are the guest's only once nvn_mirror_sync recorded the copies. */
bool nvn_mirror_buffer(const void *buf);
/** @brief Whether the host device can import guest pages (VK_EXT_external_memory_host). */
bool nvn_vk_can_import(void);
/**
 * @brief Notes that the device may have written the size bytes at GPU address gpu, so that when the batch completes
 * they are copied to guest memory. Nothing for an address outside the mirrors.
 */
void nvn_mirror_written(uint64_t gpu, uint64_t size);
/** @brief As nvn_mirror_written, for guest bytes (a buffer texture's storage the device wrote). */
void nvn_mirror_written_guest(uint64_t guest, uint64_t size);
/** @brief Notes every byte of every mirror as possibly written: a program stores through addresses it builds itself. */
void nvn_mirror_written_all(void);
/** @brief The guest address of GPU address gpu (an address in a memory pool), or gpu when no pool holds it. */
uint64_t nvn_gpu_guest(uint64_t gpu);
/**
 * @brief Writes at most n bytes (NUL-terminated) naming what holds GPU address gpu: the live pool spans that hold it,
 * the freed ones that held it, else the nearest spans below and above. Returns the length it wanted.
 */
size_t nvn_gpu_describe(uint64_t gpu, char *out, size_t n);
/** @brief Whether the size bytes at GPU address gpu lie in one imported pool (a program that reads others loses the device). */
bool nvn_gpu_mapped(uint64_t gpu, uint64_t size);
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

/** @brief DLSS network procs relift implements (nvn_dlss.c); the builder and parameter procs stay the driver's. */
#define NVN_DLSS_PROCS(X)                                                                                              \
    X(nvnDlssNetworkBuilderGetOptimalSettings)                                                                         \
    X(nvnDlssNetworkBuilderGetStorageSize)                                                                             \
    X(nvnDlssNetworkBuilderSetTextureIDs)                                                                              \
    X(nvnDlssNetworkBuilderGetTextureIDs)                                                                              \
    X(nvnDlssNetworkFinalize)                                                                                          \
    X(nvnDlssNetworkInitialize)                                                                                        \
    X(nvnDlssNetworkResize)

/** @brief Bytes of an NVNdlssParameters, as the driver's setters lay it out (nvnDlssParametersSetDefaults clears them). */
#define NVN_DLSS_PARAMS_BYTES 192u
/** @brief Offsets in NVNdlssParameters of the six texture and view pointer pairs the setters store. */
#define NVN_DLSS_PARAM_TEXTURES 6u
extern const uint8_t nvn_dlss_texture_offsets[NVN_DLSS_PARAM_TEXTURES];

/** @brief An NVNdlssParameters as the evaluation reads it. */
typedef struct nvn_dlss_params {
    /** @brief Guest addresses of the textures and their views (0 for none): input color, output color, motion
     * vectors, depth, bias current color mask, exposure. */
    uint64_t color, color_view, out, out_view, motion, motion_view, depth, depth_view, bias, bias_view, exposure,
        exposure_view;
    float jitter[2], motion_scale[2], pre_exposure;
    bool reset;
    uint32_t evaluation_index;
    /** @brief The input subrectangle's size, and the bases of each subrectangle (x, y). */
    uint32_t in_size[2], color_base[2], depth_base[2], motion_base[2], bias_base[2], out_base[2];
} nvn_dlss_params;

/** @brief Reads raw (NVN_DLSS_PARAMS_BYTES of an NVNdlssParameters) into out. */
void nvn_dlss_params_of(const uint8_t *raw, nvn_dlss_params *out);

/**
 * @brief The driver's nvnDlssNetworkBuilderGetOptimalSettings for an output of width by height and a method: out gets
 * the optimal, smallest and largest render sizes (width, height each).
 */
void nvn_dlss_optimal(uint32_t width, uint32_t height, uint32_t method, uint32_t out[6]);

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
    X(nvnCommandBufferGetMemoryCallback)                                                                               \
    X(nvnCommandBufferGetMemoryCallbackData)                                                                           \
    X(nvnCommandBufferIsRecording)                                                                                     \
    X(nvnCommandBufferSetRecordingMode)                                                                                \
    X(nvnCommandBufferBeginRecording)                                                                                  \
    X(nvnCommandBufferEndRecording)                                                                                    \
    X(nvnCommandBufferSetCopyRowStride)                                                                                \
    X(nvnCommandBufferSetCopyImageStride)                                                                              \
    X(nvnCommandBufferGetCopyRowStride)                                                                                \
    X(nvnCommandBufferGetCopyImageStride)

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
    X(NVN_CMD_BIND_VERTEX_BUFFERS, nvnCommandBufferBindVertexBuffers) \
    X(NVN_CMD_BIND_STORAGE_BUFFERS, nvnCommandBufferBindStorageBuffers) \
    X(NVN_CMD_BIND_TEXTURES, nvnCommandBufferBindTextures) \
    X(NVN_CMD_BIND_SEPARATE_TEXTURE, nvnCommandBufferBindSeparateTexture) \
    X(NVN_CMD_BIND_SEPARATE_SAMPLER, nvnCommandBufferBindSeparateSampler) \
    X(NVN_CMD_BIND_SEPARATE_TEXTURES, nvnCommandBufferBindSeparateTextures) \
    X(NVN_CMD_BIND_SEPARATE_SAMPLERS, nvnCommandBufferBindSeparateSamplers) \
    X(NVN_CMD_BIND_TEXTURES_DEFERRED, nvnCommandBufferBindTexturesDeferred) \
    X(NVN_CMD_BIND_IMAGES_DEFERRED, nvnCommandBufferBindImagesDeferred) \
    X(NVN_CMD_BIND_SEPARATE_TEXTURES_DEFERRED, nvnCommandBufferBindSeparateTexturesDeferred) \
    X(NVN_CMD_BIND_SEPARATE_SAMPLERS_DEFERRED, nvnCommandBufferBindSeparateSamplersDeferred) \
    X(NVN_CMD_BIND_UNIFORM_BUFFERS_DEFERRED, nvnCommandBufferBindUniformBuffersDeferred) \
    X(NVN_CMD_BIND_STORAGE_BUFFERS_DEFERRED, nvnCommandBufferBindStorageBuffersDeferred) \
    X(NVN_CMD_BIND_IMAGES, nvnCommandBufferBindImages) \
    X(NVN_CMD_BIND_VERTEX_STREAM_STATE, nvnCommandBufferBindVertexStreamState) \
    X(NVN_CMD_CALL_COMMAND, nvnCommandBufferCallCommand) \
    X(NVN_CMD_CALL_COMMANDS, nvnCommandBufferCallCommands) \
    X(NVN_CMD_CLEAR_COLOR, nvnCommandBufferClearColor) \
    X(NVN_CMD_CLEAR_COLORI, nvnCommandBufferClearColori) \
    X(NVN_CMD_CLEAR_COLORUI, nvnCommandBufferClearColorui) \
    X(NVN_CMD_CLEAR_TEXTURE, nvnCommandBufferClearTexture) \
    X(NVN_CMD_CLEAR_TEXTUREI, nvnCommandBufferClearTexturei) \
    X(NVN_CMD_CLEAR_TEXTUREUI, nvnCommandBufferClearTextureui) \
    X(NVN_CMD_CLEAR_DEPTH_STENCIL, nvnCommandBufferClearDepthStencil) \
    X(NVN_CMD_COPY_BUFFER_TO_BUFFER, nvnCommandBufferCopyBufferToBuffer) \
    X(NVN_CMD_UPDATE_UNIFORM_BUFFER, nvnCommandBufferUpdateUniformBuffer) \
    X(NVN_CMD_COPY_BUFFER_TO_TEXTURE, nvnCommandBufferCopyBufferToTexture) \
    X(NVN_CMD_COPY_TEXTURE_TO_BUFFER, nvnCommandBufferCopyTextureToBuffer) \
    X(NVN_CMD_COPY_TEXTURE_TO_TEXTURE, nvnCommandBufferCopyTextureToTexture) \
    X(NVN_CMD_DISPATCH_COMPUTE, nvnCommandBufferDispatchCompute) \
    X(NVN_CMD_DISPATCH_COMPUTE_INDIRECT, nvnCommandBufferDispatchComputeIndirect) \
    X(NVN_CMD_DRAW_ARRAYS, nvnCommandBufferDrawArrays) \
    X(NVN_CMD_DRAW_ARRAYS_INSTANCED, nvnCommandBufferDrawArraysInstanced) \
    X(NVN_CMD_DRAW_ELEMENTS_INSTANCED, nvnCommandBufferDrawElementsInstanced) \
    X(NVN_CMD_DRAW_ELEMENTS, nvnCommandBufferDrawElements) \
    X(NVN_CMD_DRAW_ELEMENTS_BASE_VERTEX, nvnCommandBufferDrawElementsBaseVertex) \
    X(NVN_CMD_DRAW_MESH_TASKS, nvnCommandBufferDrawMeshTasks) \
    X(NVN_CMD_DRAW_MESH_TASKS_3D, nvnCommandBufferDrawMeshTasks3D) \
    X(NVN_CMD_DRAW_MESH_TASKS_INDIRECT, nvnCommandBufferDrawMeshTasksIndirect) \
    X(NVN_CMD_DRAW_MESH_TASKS_3D_INDIRECT, nvnCommandBufferDrawMeshTasks3DIndirect) \
    X(NVN_CMD_MULTI_DRAW_MESH_TASKS_INDIRECT, nvnCommandBufferMultiDrawMeshTasksIndirect) \
    X(NVN_CMD_MULTI_DRAW_MESH_TASKS_INDIRECT_COUNT, nvnCommandBufferMultiDrawMeshTasksIndirectCount) \
    X(NVN_CMD_MULTI_DRAW_MESH_TASKS_3D_INDIRECT, nvnCommandBufferMultiDrawMeshTasks3DIndirect) \
    X(NVN_CMD_MULTI_DRAW_MESH_TASKS_3D_INDIRECT_COUNT, nvnCommandBufferMultiDrawMeshTasks3DIndirectCount) \
    X(NVN_CMD_END_TRANSFORM_FEEDBACK, nvnCommandBufferEndTransformFeedback) \
    X(NVN_CMD_EVALUATE_DLSS, nvnCommandBufferEvaluateDlss) \
    X(NVN_CMD_FENCE_SYNC, nvnCommandBufferFenceSync) \
    X(NVN_CMD_REPORT_COUNTER, nvnCommandBufferReportCounter) \
    X(NVN_CMD_PUSH_COMPUTE_CONSTANTS, nvnCommandBufferPushComputeConstants) \
    X(NVN_CMD_PUSH_GRAPHICS_CONSTANTS, nvnCommandBufferPushGraphicsConstants) \
    X(NVN_CMD_RESET_COUNTER, nvnCommandBufferResetCounter) \
    X(NVN_CMD_RESTORE_ZCULL_DATA, nvnCommandBufferRestoreZCullData) \
    X(NVN_CMD_SAVE_ZCULL_DATA, nvnCommandBufferSaveZCullData) \
    X(NVN_CMD_SET_BLEND_COLOR, nvnCommandBufferSetBlendColor) \
    X(NVN_CMD_SET_COMPUTE_OVERLAP_MODE, nvnCommandBufferSetComputeOverlapMode) \
    X(NVN_CMD_SET_COMPUTE_WORKLOAD_PRIORITY, nvnCommandBufferSetComputeWorkloadPriority) \
    X(NVN_CMD_SET_DEPTH_BOUNDS, nvnCommandBufferSetDepthBounds) \
    X(NVN_CMD_SET_GRAPHICS_WORKLOAD_PRIORITY, nvnCommandBufferSetGraphicsWorkloadPriority) \
    X(NVN_CMD_SET_RESOURCE_OCCUPANCY, nvnCommandBufferSetResourceOccupancy) \
    X(NVN_CMD_SET_CONSERVATIVE_RASTER_ENABLE, nvnCommandBufferSetConservativeRasterEnable) \
    X(NVN_CMD_SET_CONSERVATIVE_RASTER_DILATE, nvnCommandBufferSetConservativeRasterDilate) \
    X(NVN_CMD_SET_SAMPLE_MASK, nvnCommandBufferSetSampleMask) \
    X(NVN_CMD_SET_ALPHA_REF, nvnCommandBufferSetAlphaRef) \
    X(NVN_CMD_SET_POINT_SIZE, nvnCommandBufferSetPointSize) \
    X(NVN_CMD_SET_RENDER_ENABLE, nvnCommandBufferSetRenderEnable) \
    X(NVN_CMD_SET_RENDER_ENABLE_CONDITIONAL, nvnCommandBufferSetRenderEnableConditional) \
    X(NVN_CMD_SET_VIEWPORT_SWIZZLES, nvnCommandBufferSetViewportSwizzles) \
    X(NVN_CMD_SET_INNER_TESSELLATION_LEVELS, nvnCommandBufferSetInnerTessellationLevels) \
    X(NVN_CMD_SET_OUTER_TESSELLATION_LEVELS, nvnCommandBufferSetOuterTessellationLevels) \
    X(NVN_CMD_SET_PROVOKING_VERTEX_MODE, nvnCommandBufferSetProvokingVertexMode) \
    X(NVN_CMD_BIND_COVERAGE_MODULATION_TABLE, nvnCommandBufferBindCoverageModulationTable) \
    X(NVN_CMD_SET_GEOMETRY_PROCESSING_MODE, nvnCommandBufferSetGeometryProcessingMode) \
    X(NVN_CMD_SET_DEPTH_CLAMP, nvnCommandBufferSetDepthClamp) \
    X(NVN_CMD_SET_DEPTH_RANGE, nvnCommandBufferSetDepthRange) \
    X(NVN_CMD_SET_DEPTH_RANGES, nvnCommandBufferSetDepthRanges) \
    X(NVN_CMD_SET_LINE_WIDTH, nvnCommandBufferSetLineWidth) \
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
    X(NVN_CMD_SET_VIEWPORT, nvnCommandBufferSetViewport) \
    X(NVN_CMD_SET_VIEWPORTS, nvnCommandBufferSetViewports) \
    X(NVN_CMD_SET_ZCULL_ZF32_COMPRESSION_ENABLE, nvnCommandBufferSetZCullZF32CompressionEnable) \
    X(NVN_CMD_WAIT_SYNC, nvnCommandBufferWaitSync) \
    X(NVN_CMD_WAIT_EVENT, nvnCommandBufferWaitEvent) \
    X(NVN_CMD_SIGNAL_EVENT, nvnCommandBufferSignalEvent)

/** @brief Recorded command ops. */
enum nvn_cmd_op {
    NVN_CMD_NONE = 0,
#define NVN_CMD_ENUM(op, proc) op,
    NVN_RECORDED(NVN_CMD_ENUM)
#undef NVN_CMD_ENUM
    NVN_CMD_COUNT
};

/** @brief One recorded command: its op and argument registers. */
/**
 * @brief The arguments of a recorded CopyBufferToTexture or
 * CopyTextureToBuffer that hold the command buffer's copy row and image
 * strides when it was recorded (the procs take six).
 */
#define NVN_COPY_ROW_STRIDE_ARG 6
#define NVN_COPY_IMAGE_STRIDE_ARG 7

/**
 * @brief The words of the value a generic vertex output holds before its program stores to it, (0, 0, 0, 1) as float
 * bits. The translator (gpus/nv translate io vtg.rs, position_defaults and finish) writes it to every output the
 * vertex program declares; a fragment input the vertex program does not declare reads the same value here.
 * Inferred: the console's vertex stage leaves it in the output buffer; no per component measurement exists.
 */
static const uint32_t UNWRITTEN_VARYING[4] = { 0x00000000u, 0x00000000u, 0x00000000u, 0x3f800000u };

/**
 * @brief Takes the layer's lock (nvn_vk.c g.lock) for a guest thread. A blocked wait for the lock, a wait on the host
 * device (a fence, the first compile of a pipeline) and the host display run on a guest thread that holds a core's
 * run token, which keeps every other guest thread of that core from running for as long as the host takes: a
 * console core does not stall while its GPU or display works. So a guest thread that would block first leaves its
 * core (hos_sched_leave) and stays off it until nvn_unlock, the end of the work, returns it (hos_sched_return waits
 * for a core with no lock held, which is why the thread stays off its core while it holds the lock: it cannot
 * wait for a core that a lock waiter holds).
 */
void nvn_lock(void);
/** @brief Releases the layer's lock and, when the thread left its core since nvn_lock, takes its core back. */
void nvn_unlock(void);
/** @brief Leaves the thread's core before a host wait (nvn_unlock returns it); a no-op when it already left. */
void nvn_core_leave(void);
/** @brief Takes the core back outside the lock (after the host display); a no-op when the thread did not leave. */
void nvn_core_return(void);

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
/**
 * @brief The guest address of the u32 an NVNevent (at guest address event) keeps its value in, or 0 when event is
 * not an event the layer made.
 */
uint64_t nvn_event_word(uint64_t event);
/** @brief Replays the submitted command sets that wait for an event and can go on now (nvn_vk.c); the CPU calls it after it signals an event. */
void nvn_vk_pump(rl_ctx *c, uint8_t *mem);
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
    X(nvnDeviceGetWindowOriginMode)                                                                                    \
    X(nvnDeviceGetDepthMode)                                                                                           \
    X(nvnDeviceGenerateDebugDomainId)                                                                                  \
    X(nvnDeviceGetImageHandle)                                                                                         \
    X(nvnDeviceGetTextureHandle)                                                                                       \
    X(nvnQueueInitialize)                                                                                              \
    X(nvnQueueFinalize)                                                                                                \
    X(nvnQueueFlush)                                                                                                   \
    X(nvnQueueFinish)                                                                                                  \
    X(nvnQueueSubmitCommands)                                                                                          \
    X(nvnQueueFenceSync)                                                                                               \
    X(nvnQueueWaitSync)                                                                                                \
    X(nvnMemoryPoolBuilderSetNativeHandleNVX)                                                                          \
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
    NVN_EVENT,
    NVN_DLSS_NETWORK
};

/** @brief The head of every relift NVN object; a kind's struct starts with it. */
typedef struct nvn_obj nvn_obj;
struct nvn_obj {
    /** @brief Guest address of the game's NVN object. */
    uint64_t addr;
    /** @brief enum nvn_kind. */
    uint32_t kind;
    /** @brief Holders: the table, and each nvn_obj_acquire not yet released. Guarded by the table's lock. */
    uint32_t refs;
    /** @brief Frees what the kind holds besides the struct itself; may be NULL. */
    void (*drop)(nvn_obj *o);
};

/** @brief A slot left by a freed object in the object table. */
#define NVN_TOMB ((nvn_obj *)(uintptr_t)1)

/**
 * @brief A new zeroed object of size bytes for the NVN object at addr, not in the table yet: no lookup finds it until
 * nvn_obj_publish, so the caller fills it first. NULL when out of memory.
 */
nvn_obj *nvn_obj_new(uint64_t addr, uint32_t kind, size_t size);
/**
 * @brief Puts o, already filled, in the table, replacing any object at its address, the way the console's object is
 * usable once its Initialize returns. Returns o, or NULL (o dropped and freed) when the table cannot grow; NULL gives
 * NULL.
 */
nvn_obj *nvn_obj_publish(nvn_obj *o);
/** @brief The object for the NVN object at addr, of kind (0: any kind), or NULL. */
nvn_obj *nvn_obj_get(uint64_t addr, uint32_t kind);
/** @brief Describes the last removal of the object at addr (kind, how, thread, age) into out; false when none is remembered. */
bool nvn_obj_gone(uint64_t addr, char *out, size_t n);
/** @brief The first vi vsync after t (CLOCK_MONOTONIC ns), for the headless pacing measurement mode. */
int64_t nvn_vsync_after(int64_t t);
/** @brief One line of the frame trace (stats.trace), with vi_trace's kinds and fields. */
void nvn_trace(char kind, uint64_t a, uint64_t b, uint64_t c);
/** @brief Whether the frame trace (stats.trace) is on. */
bool nvn_trace_on(void);
/**
 * @brief Removes o from the table (a lookup no longer finds it) and drops the table's reference; the object is freed
 * when the last reference is gone. NULL does nothing.
 */
void nvn_obj_free(nvn_obj *o);
/**
 * @brief Like nvn_obj_get, and the object stays allocated until nvn_obj_release, whatever the guest finalizes or
 * re-initializes meanwhile. For a thread that uses an object past the call that found it.
 */
nvn_obj *nvn_obj_acquire(uint64_t addr, uint32_t kind);
/** @brief Drops a reference nvn_obj_acquire took; NULL does nothing. */
void nvn_obj_release(nvn_obj *o);

/**
 * @brief Calls the driver's own proc name (one nvn_driver_proc accepts) with
 * up to 8 integer arguments; *ret gets x0. Returns RL_OK, or RL_PENDING,
 * which the calling proc returns at once. A name the driver does not have
 * stops the game.
 */
int nvn_driver_call(rl_ctx *c, uint8_t *mem, const char *name, const uint64_t *args, unsigned n, uint64_t *ret);


/** @brief The NVN generation of the program's driver (nvn_version.c): 1 or 2 (also without a build record). */
uint32_t nvn_generation(void);
/** @brief The driver record rl_nvn_set_driver registered, NULL without one; it names what nvn_impure answers from. */
const void *nvn_driver_record(void);
/** @brief Why the build found the driver's own code for proc name impure, or NULL when it is pure or unread. */
const char *nvn_impure(const char *name);
/** @brief Why the build could not read the driver's device layout, or NULL. */
const char *nvn_layout_error(void);
/** @brief The driver's generation and versions, for messages. */
const char *nvn_version_name(void);
/** @brief The bit the driver sets in texture, separate texture and texel fetch handles (bit 32 in NVN1, none in NVN2). */
uint64_t nvn_sampled_handle_mark(void);
/** @brief Offset of byte x of row y inside a 512-byte GOB, in the layout of the program's GPU (nvn_swizzle.c). */
uint32_t nvn_gob_offset(uint32_t x, uint32_t y);
/** @brief The GOB layout function of the program's NVN generation, to call in a loop without asking for the generation each time. */
uint32_t (*nvn_gob_offset_fn(void))(uint32_t, uint32_t);
/** @brief Number of NVNformat values the driver knows. */
uint32_t nvn_format_count(void);
/** @brief The NVN1 answer for an NVNdeviceInfo the driver measures at device start; false for NVN2 or another info. */
bool nvn_version_measured(uint32_t info, int32_t *out);

/** @brief Number of procs that stay the SDK driver's own code (nvn_driver.c). */
#define NVN_DRIVER_PROC_COUNT 322u

/**
 * @brief Whether the kept proc name reads what the driver's own
 * nvnTextureInitialize stores in a texture: only NVN2 textures run it, so on
 * NVN1 such a proc is a slot like any proc the layer lacks.
 */
bool nvn_driver_reads_texture(const char *name);

/** @brief The index of a proc that stays the SDK driver's own code (nvn_driver.c), or -1. */
/** @brief Frees the slots that hold texture tex (an NVNtexture's guest address), at its finalize. */
void nvn_texset_texture_gone(uint64_t tex);
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

/**
 * @brief Writes into the guest's device object at dev, once, the fields the
 * driver's own code reads: the flags word, the NvRmGpuDevice, and what the
 * driver's own nvnDeviceInitialize leaves in the others (constants, and
 * words it copies from NvRmGpuDeviceGetInfo); the layer keeps its own device
 * state, so it never reads the guest's object. Returns RL_OK or a pending
 * stop.
 */
int nvn_device_fields(rl_ctx *c, uint8_t *mem, uint64_t dev);

/** @brief nvn_device_fields on the device of the texture builder at builder. */
int nvn_builder_device_fields(rl_ctx *c, uint8_t *mem, uint64_t builder);

/** @brief The SDK's NvRmGpuMappingModify, on the pool mappings the layer owns (nvn_dev.c). */
uint64_t nvnh_NvRmGpuMappingModify(rl_ctx *c, uint8_t *mem);
uint64_t nvnh_NvRmMemHandleAllocNativeImage(rl_ctx *c, uint8_t *mem);

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
