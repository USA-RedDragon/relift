/**
 * @file nvn_vk.h
 * @brief What the NVN2 layer's Vulkan files (nvn_vk.c, nvn_draw.c) share:
 * the host device, host images and the replay state of a queue.
 */
#ifndef RELIFT_NVN_VK_H
#define RELIFT_NVN_VK_H

#include "nvn_priv.h"
#include "nvn_watch.h"
#include "vkf.h"

#include <pthread.h>

/**
 * @brief The stable identity of a shader module: a program's module, or one of its variants, keeps its id for the run
 * though its VkShaderModule handle is destroyed after pipelines are made from it and remade when one is needed again.
 * 0 is none (nvn_module_acquire gives the handle of an id).
 */
typedef uint64_t nvn_shader_id;

/**
 * @brief Whether the layer counts its host Vulkan calls by command (set with the GPU profile, setting gpu.profile),
 * and the counts since the start: nvn_vk_calls by vkg_cmd, the NVN commands replayed and the draws made.
 */
extern uint32_t nvn_vk_count_on;
extern uint64_t nvn_vk_calls[VKG_COUNT], nvn_vk_count_cmds, nvn_vk_count_draws;

#define VK(name) ((PFN_##name)nvn_vk_fn(VKG_##name))

/**
 * @brief The most batches recorded or in flight at once. The layer records the next batch into a free slot while the
 * host device runs the ones before from the others, as the console's CPU builds commands while its GPU runs earlier
 * ones; a console submit never waits for an old batch, its queue holds what the game's command memory holds. So the
 * slots in use grow on demand: when the next slot's batch has not finished, a new slot is made (its upload arena and
 * descriptor pool on first use) up to this many, and only then does a submit wait for the oldest batch. A build may
 * set its own count.
 */
#ifndef NVN_SLOTS
#define NVN_SLOTS 16u
#endif
#define MAX_RT 8u
/** @brief Graphics, compute, task and mesh stages, in NVNshaderStage order. */
#define NVN_STAGES 8u
/** @brief The viewports a shading-rate palette can be given for. */
#define NVN_SHADING_VIEWPORTS 16u
/** @brief Uniform buffer slots of a stage (c[3] to c[16]). */
#define NVN_UBO_SLOTS 14u
/** @brief Bytes of a stage's driver bank c[2] the layer keeps: bound texture handles from 0x20, 8 bytes apart. */
#define NVN_BANK2_BYTES 0x968u
/**
 * @brief Byte of a compute program's driver bank c[0] where the dispatch's group counts x, y and z start (u32 each,
 * to 0x10): the light-cluster builder of one game linearizes its group id as (z * c[0][8] + y) * c[0][4] + x into
 * buffers of x * y * z cells that its lighting pass indexes in the same order.
 */
#define NVN_C0_GROUPS 0x4u
/** @brief Where a CUDA kernel's c[0] holds its parameters, and the local window it gets at c[0] byte 0x28 (the translator's default local words, times 4). */
#define NVN_CUDA_PARAMS 0x160u
#define NVN_CUDA_LOCAL_BYTES 0x1000u
/** @brief Offset in c[2] of the handle bound to texture slot 0 (nvnCommandBufferBindTexture, sdk 0x6022e0). */
/** @brief Bytes of a graphics stage's driver bank c[0] the layer fills. */
#define NVN_GFX_C0_BYTES 0x1000u
#define NVN_BANK2_TEXTURES 0x20u
/**
 * @brief Offsets in c[2] of the handle bound to separate texture slot 0 and to separate sampler slot 0, and the
 * slots of each (nvnCommandBufferBindSeparateTexture and BindSeparateSampler write method 0x8e3 at 0x168 + 8 * slot
 * and 0x568 + 8 * slot in both NVN1 16.2.0 and NVN2 22.2.0 drivers).
 */
#define NVN_BANK2_SEPARATE_TEXTURES 0x168u
#define NVN_BANK2_SEPARATE_SAMPLERS 0x568u
#define NVN_SEPARATE_SLOTS 128u
/** @brief Offset in c[2] of the handle bound to image slot 0 of a graphics stage (nvnCommandBufferBindImage, sdk 0x67ea20: method 0x8e3 at 0x120 + 8 * slot). */
#define NVN_BANK2_IMAGES 0x120u
#define NVN_VERTEX_BUFFERS 16u
/** @brief NVNtextureTarget of a buffer texture: a host 1D image the layer refills from its storage at each use. */
#define NVN_TARGET_BUFFER 10u
/** @brief NVNtextureTarget of a cubemap (six layers) and a cubemap array (depth layers, six per cube). */
#define NVN_TARGET_CUBEMAP 8u
/** @brief NVNtextureTarget of a 3D texture. */
#define NVN_TARGET_3D 2u
/** @brief NVNtextureTarget of a multisample 2D texture and a multisample 2D array (depth layers). */
#define NVN_TARGET_2D_MULTISAMPLE 5u
#define NVN_TARGET_2D_MULTISAMPLE_ARRAY 6u
#define NVN_TARGET_CUBEMAP_ARRAY 9u
/** @brief Pushed constant slots of a stage and bytes of each (the driver clamps a push to 0x100 bytes). */
#define NVN_PUSH_SLOTS 4u
#define NVN_PUSH_BYTES 0x100u
/** @brief Graphics push constant slots (the driver keeps slot & 7); every graphics stage reads slot n as c[24 + n]. */
#define NVN_GFX_PUSH_SLOTS 8u
#define NVN_GFX_PUSH_BANK 24u
/** @brief GPU counter types 0 (timestamp) to 14 (transform feedback primitives written); 15 (ZCull statistics) is not counted. */
#define NVN_COUNTERS 15u
/** @brief NVNcounterType of a timestamp taken at the top of the pipe. */
#define NVN_COUNTER_TIMESTAMP_TOP 16u
/** @brief The compute stage's NVNshaderStage, and its storage buffer slots. */
/** @brief The vertex stage, stage 0. */
#define NVN_STAGE_VERTEX 0u
#define NVN_STAGE_COMPUTE 5u
/** @brief VK_EXT_mesh_shader's shader stage bits, which the generated headers do not hold. */
#define VK_SHADER_STAGE_TASK_BIT_EXT 0x40u
#define VK_SHADER_STAGE_MESH_BIT_EXT 0x80u
/** @brief The task and mesh stages' NVNshaderStage (NVN2): the draw's programs when a game draws mesh tasks. */
#define NVN_STAGE_TASK 6u
#define NVN_STAGE_MESH 7u
#define NVN_STORAGE_SLOTS 16u
/** @brief Image (and compute texture) slots of a stage. */
#define NVN_IMAGE_SLOTS 32u
#define NVN_VERTEX_ATTRIBS 16u
/** @brief Bindings of set 0 (user banks) each stage owns: stage s's bank c[3 + n] is binding 22 s + n. */
#define USER_BINDINGS 22u
/** @brief Bindings of set 1 (driver banks) each stage owns: stage s's bank c[n] is binding 4 s + n. */
#define DRIVER_BINDINGS 4u
/** @brief First binding of set 1 that holds a storage buffer slot (the translator's SLOT_BINDING_BASE). */
#define NVN_SLOT_BINDING_BASE 64u
/** @brief The binding of set 1 that holds stage s's storage buffer slots, an array of NVN_STORAGE_SLOTS (slot k is element k). */
static inline uint32_t nvn_slot_binding(uint32_t s) { return NVN_SLOT_BINDING_BASE + s; }
/** @brief The translator's POOL_SET, POOL_ARRAY_BINDING, POOL_TABLE_BINDING, POOL_SEARCH_STEPS, POOL_TABLE_FIRST and POOL_TABLE_STRIDE (see nvn_pools.c). */
#define NVN_POOL_SET 4u
#define NVN_POOL_ARRAY_BINDING 0u
#define NVN_POOL_TABLE_BINDING 1u
#define NVN_POOL_SEARCH_STEPS 16u
#define NVN_POOL_TABLE_FIRST 4u
#define NVN_POOL_TABLE_STRIDE 4u
/** @brief The translator's COMPUTE_BASE_BINDING: binding NVN_COMPUTE_BASE_BINDING + offset / 8 of set 1 is the compute base at c[0] byte offset. */
#define NVN_COMPUTE_BASE_BINDING 128u
/** @brief The translator's COMPUTE_BASE_BYTES: compute bases lie below this c[0] byte. */
#define NVN_COMPUTE_BASE_BYTES 0x800u
/** @brief The translator's COMPUTE_BASE_TABLE_BINDING: set 1's uniform buffer of where each compute base's descriptor starts. */
#define NVN_COMPUTE_BASE_TABLE_BINDING 80u

/** @brief A texture's host image. */
typedef struct vimg {
    uint64_t storage;
    nvn_tex_desc d;
    /** @brief The serial of the memory pool d.pool names when the image was made (nvn_obj_serial), 0 for none. */
    uint64_t pool_serial;
    VkFormat format;
    VkImageAspectFlags aspect;
    /** @brief An ASTC texture the host device cannot sample: the image holds its decoded RGBA8 texels. */
    bool decoded;
    /** @brief The image was made with storage usage (an uncompressed color format). */
    bool storage_ok;
    VkImageUsageFlags usage;
    /** @brief When the GPU last wrote it (a render target, copy or storage binding), from a running count. */
    uint64_t written;
    /** @brief The write stamp its bytes were last written back to its storage at (0 for never), and that write-back's number. */
    uint64_t wb_written, wb_seq;
    /**
     * @brief Made or bound as a render target without taking the bytes of the newer images over its storage: it takes
     * those written after sync_since and before it when something reads what it held, and never when a clear of all
     * of it comes first.
     */
    bool sync_pending;
    /** @brief Its index + 1 in the list of images the GPU has written (nvn_vk_unsaved_over searches it), 0 when not in it. */
    uint32_t written_at;
    uint64_t sync_since;
    /**
     * @brief The other images over any byte of its storage, listed when the set of images was as g.image_epoch was at
     * alias_epoch (sync_alias lists them again once it changes).
     */
    struct vimg **aliases;
    uint32_t naliases;
    uint64_t alias_epoch;
    /** @brief The hash of its storage when last filled, and the frame (presents + 1) it was last checked in. */
    uint64_t hash, hash_frame;
    /** @brief Write tracking of the storage's whole pages, and the hash of the bytes beside them (nvn_watch.h). */
    nvn_watch watch;
    uint64_t edge_hash;
    /** @brief For a buffer texture in an imported pool: the write stamp last copied into the pool's bytes. */
    uint64_t flushed;
    /** @brief For an image whose pool is gone: the batch serial (nvn_vk_submitted) that must finish before it is freed. */
    uint64_t retire_after;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    /** @brief Views of single levels and of count layers (depth slices of a 3D image) from layer, for render targets. */
    struct vsub {
        uint32_t level, layer, count;
        VkImageView view;
        /** @brief The view's format when it is not the image's, else VK_FORMAT_UNDEFINED. */
        VkFormat format;
    } *sub;
    uint32_t nsub, capsub;
    /** @brief Sampling views by VkImageViewType (1D to cube array), made when first asked for. */
    VkImageView views[7];
    /** @brief The view a shading-rate image is bound through (nvn_vk_rate_view), made when first asked for. */
    VkImageView rate_view;
    /** @brief The texture set keeps it for a registered texture's id (nvn_texset_sync_registered). */
    bool synced;
    /** @brief For a multisample image: what its sample moves keep (nvn_msaa.c), made at its first move. */
    struct nvn_msaa_img *msaa;
} vimg;

/** @brief A render target: one level and layer of an image. */
typedef struct rt {
    vimg *v;
    /** @brief The level and first layer drawn to, and the layers from there a layered draw reaches (the view's, else the rest of the image). */
    uint32_t level, layer, layers;
    VkImageView view;
    /** @brief The format the target is drawn in: its image's, or an sRGB or linear twin a texture view names. */
    VkFormat format;
} rt;

/**
 * @brief The texture ad that a view of a block-compressed texture d in an uncompressed color format of the block's
 * size draws into: one texel per block over the same storage and layout (a game that compresses on the GPU draws the
 * blocks of a BC texture as the texels of an integer texture). Single-level textures only, and false when the
 * layout of ad does not fill the same bytes as d.
 */
bool nvn_vk_block_alias(const nvn_tex_desc *d, uint32_t view_format, nvn_tex_desc *ad);

/** @brief A host-visible buffer, mapped. */
typedef struct staging {
    VkBuffer buf;
    VkDeviceMemory memory;
    uint8_t *map;
    uint64_t size;
} staging;

/** @brief The host device and what lives as long as it. */
typedef struct nvn_vkdev {
    pthread_once_t once;
    pthread_mutex_t lock;
    bool ok;
    bool dev_made;
    bool depth_clip_control;
    /** @brief The device can clamp fragment depth to the viewport range instead of clipping it (depthClamp). */
    bool depth_clamp;
    /** @brief Line widths other than 1 are allowed (wideLines). */
    bool wide_lines;
    /** @brief The device has the depth bounds test (depthBounds). */
    bool depth_bounds;
    /** @brief VK_EXT_conservative_rasterization is on: nvnCommandBufferSetConservativeRasterEnable is honored. */
    bool conservative_raster, conservative_warned;
    /**
     * @brief The host rasterizes multisample draws at Vulkan's standard sample positions. NVN's defaults are those
     * same positions for 2, 4, 8 and 16 samples: the driver's BindMultisampleState writes them, in 1/16 pixel, to
     * SET_ANTI_ALIAS_SAMPLE_POSITIONS (NVC797 0x11e0, clc797.h) from its tables at sdk 0xc94fa8 to 0xc95008
     * (SDK 22.2.8 driver).
     */
    bool standard_samples;
    /** @brief The device shades per sample, and writes storage images declared without a format. */
    bool sample_shading, formatless_write;
    /**
     * @brief VK_EXT_sample_locations is on and places custom sample locations exactly on NVN's 1/16 pixel grid
     * (sampleLocationSubPixelBits of at least 4, a coordinate range of [0, 15/16] or wider) for the sample counts in
     * sample_location_counts.
     */
    bool sample_locations;
    uint32_t sample_location_counts;
    /**
     * @brief VK_NV_framebuffer_mixed_samples and VK_NV_fragment_coverage_to_color are on: NVIDIA's own target
     * independent rasterization (more raster samples than color samples, coverage modulation) and coverage to color,
     * the hardware states NVN's multisample state sets (nvn_tir).
     */
    bool mixed_samples, coverage_to_color;
    /**
     * @brief The device is NVIDIA's: its alpha to coverage is the console GPU's dithered one (measured on the RTX 3090
     * by test_nvn_a2c.c: 4n coverage levels spread over a 2x2 pixel footprint, the footprint the driver selects
     * with dither on, SET_ALPHA_TO_COVERAGE_DITHER_CONTROL PIXELS_2X2, clc797.h).
     */
    bool nvidia;
    /** @brief The largest extra overestimation (pixels) of VK_EXT_conservative_rasterization, and whether a dilate past it was warned of. */
    float conservative_extra_max;
    bool dilate_warned;
    /** @brief VK_NV_viewport_swizzle is on; whether a swizzle without it was warned of. */
    bool viewport_swizzle, swizzle_warned;
    /** @brief VK_EXT_provoking_vertex is on with provokingVertexLast; whether the last-vertex mode without it was warned of. */
    bool provoking_last, provoking_warned;
    /** @brief VK_EXT_robustness2 nullDescriptor is on: a descriptor can be written with no image view. */
    bool null_descriptor;
    /** @brief Graphics pipelines can be linked from libraries (VK_EXT_graphics_pipeline_library with fast linking). */
    bool gpl;
    /** @brief Whether imports can take back the device address of an earlier import (bufferDeviceAddressCaptureReplay). */
    bool address_replay;
    /** @brief Occlusion and pipeline statistics queries (all graphics stages, precise samples) work, and can be reset from the host. */
    bool query_ok, occlusion_precise;
    /** @brief VK_EXT_mesh_shader is on with task and mesh shaders: the native back end of task and mesh programs runs. */
    bool mesh_shader;
    /**
     * @brief VK_NV_shading_rate_image is on with 16x16 texels and 16 palette entries: a shading-rate image and its
     * palettes run as the console's (nvn_draw.c, shading_rate).
     */
    bool shading_rate_image;
    void (*bind_shading_rate_image)(VkCommandBuffer, VkImageView, VkImageLayout);
    void (*set_shading_rate_palette)(VkCommandBuffer, uint32_t, uint32_t, const void *);
    /** @brief The device has geometry shaders (geometryShader) and tessellation shaders (tessellationShader). */
    bool geometry_shader, tessellation_shader;
    /** @brief VK_EXT_device_fault's deviceFault is on: a lost device reports what faulted. */
    bool device_fault;
    /** @brief maxStorageBufferRange and maxDrawIndexedIndexValue of the device. */
    uint32_t max_storage_range, max_index_value;
    /** @brief maxPerStageDescriptorStorageBuffers of the device (31 on MoltenVK). */
    uint32_t max_stage_storage;
    /** @brief maxPerStageDescriptorUniformBuffers of the device (31 on MoltenVK). */
    uint32_t max_stage_uniform;
    /** @brief maxComputeSharedMemorySize of the device, in bytes. */
    uint32_t max_compute_shared;
    /** @brief VkPhysicalDeviceLimits::minStorageBufferOffsetAlignment. */
    uint64_t storage_align;
    /** @brief vkCmdDrawMeshTasksEXT (loaded with the device when the extension is on). */
    void (*draw_mesh_tasks)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
    /** @brief VK_EXT_transform_feedback's transformFeedback is on, with its commands (nvn_xfb.c). */
    bool transform_feedback;
    void (*bind_xfb)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *,
                     const VkDeviceSize *);
    void (*begin_xfb)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *);
    void (*end_xfb)(VkCommandBuffer, uint32_t, uint32_t, const VkBuffer *, const VkDeviceSize *);
    /** @brief VK_EXT_external_memory_host is on, with its pointer alignment. */
    bool host_import;
    uint64_t import_align;
    /** @brief VK_EXT_map_memory_placed is on with memoryMapPlaced and memoryUnmapReserve, with its placement alignment. */
    bool placed;
    uint64_t placed_align;
    vkf_fns fns;
    VkInstance instance;
    VkPhysicalDevice phys;
    VkDevice dev;
    uint32_t family;
    VkQueue queue;
    VkCommandPool pool;
    /** @brief The slot being recorded into (see nvn_vk_submit) and its command buffer and fence, of cbs and fences. */
    uint32_t slot;
    VkCommandBuffer cb;
    VkFence fence;
    VkCommandBuffer cbs[NVN_SLOTS];
    VkFence fences[NVN_SLOTS];
    /** @brief Nanoseconds per GPU timestamp tick; 0 when the queue has no timestamps. */
    double timestamp_period_ns;
    /** @brief Whether nvn_vk_calibrate reads the GPU clock and the guest clock at one moment. */
    bool calibrated;
    /** @brief VK_KHR_pipeline_executable_properties is on (RELIFT_PIPE_STATS=1): pipelines are made with statistics capture. */
    bool pipe_exec;
    /** @brief VK_KHR_performance_query is on (RELIFT_GPU_COUNTERS is set). */
    bool perf_query;
    /**
     * @brief A timeline semaphore that reaches each batch's value (timeline_value after a submit), so a thread other
     * than the one that submits can wait for a batch without the fence the next submit resets. The batch signals it,
     * or the host after the batch's fence where a GPU signal would wait for the GPU (tl in nvn_vk.c).
     */
    bool timeline_ok;
    /**
     * @brief The texture set's descriptors can be written while a batch that does not use them runs; without it every
     * batch counts as a writer, so replay waits for the one in flight as with one slot.
     */
    bool unused_while_pending;
    VkSemaphore timeline;
    uint64_t timeline_value;
    /** @brief Whether the host signals timeline_value once its batch's fence signals, not the batch (see tl). */
    bool timeline_host;
    VkPhysicalDeviceMemoryProperties memprops;
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    /** @brief Every host image, capimages of room; there is no limit but memory. */
    vimg **images;
    uint32_t capimages;
    uint32_t nimages;
    /** @brief nvn_obj_pools_removed when images were last checked for a removed pool (nvn_vk_retire_images). */
    uint64_t pools_seen;
    /** @brief Images of removed pools, waiting for the last batch that may use them before they are freed. */
    vimg **retired;
    uint32_t nretired, capretired;
    /** @brief The running count vimg.written takes its values from. */
    uint64_t write_stamp;
    /** @brief Counts the changes of the set of host images (one made, or some retired). */
    uint64_t image_epoch;
    uint64_t presents;
    /** @brief Guest bytes copied for the commands of the batch being recorded: buffers, indices, constants. */
    staging arena;
    uint64_t arena_used;
    /** @brief Each slot's arena and how much of it its batch used; arena and arena_used are the recording slot's. */
    staging arenas[NVN_SLOTS];
    uint64_t arena_used_of[NVN_SLOTS];
    /** @brief Batch slots made so far (they grow on demand, see NVN_SLOTS) and the most made at once. */
    uint32_t slots_made, slots_peak;
    /** @brief Whether the batch holds a GPU write of guest uniform data that guest memory does not show yet. */
    bool uniform_pending;
    /** @brief The device probes' results, as device.json reports them. */
    nvn_probes probes;
    /**
     * @brief Batches submitted so far. A submit empties the arena and the
     * descriptor pool, so what a command put there before it is gone.
     */
    uint64_t flushes;
} nvn_vkdev;

extern nvn_vkdev g;

/**
 * @brief A dynamic state of the graphics pipelines that the layer sets only when its value differs from the one the
 * recording command buffer last had set (the shadow is cleared when a command buffer begins or another pipeline
 * takes over the state).
 */
typedef enum nvn_dyn {
    NVN_DYN_VIEWPORT,
    NVN_DYN_SCISSOR,
    NVN_DYN_STENCIL_COMPARE_FRONT,
    NVN_DYN_STENCIL_COMPARE_BACK,
    NVN_DYN_STENCIL_WRITE_FRONT,
    NVN_DYN_STENCIL_WRITE_BACK,
    NVN_DYN_STENCIL_REF_FRONT,
    NVN_DYN_STENCIL_REF_BACK,
    NVN_DYN_DEPTH_BIAS,
    NVN_DYN_BLEND_CONSTANTS,
    NVN_DYN_LINE_WIDTH,
    NVN_DYN_DEPTH_BOUNDS,
    NVN_DYN_CULL_MODE,
    NVN_DYN_FRONT_FACE,
    NVN_DYN_DEPTH_TEST,
    NVN_DYN_DEPTH_WRITE,
    NVN_DYN_DEPTH_COMPARE,
    NVN_DYN_STENCIL_TEST,
    NVN_DYN_STENCIL_OP_FRONT,
    NVN_DYN_STENCIL_OP_BACK,
    NVN_DYN_DEPTH_BIAS_ENABLE,
    NVN_DYN_RASTERIZER_DISCARD,
    NVN_DYN_COUNT
} nvn_dyn;

/** @brief Whether the n bytes at val differ from the shadow of state (or none is kept); keeps val as the shadow. At most 32 bytes. */
bool nvn_dyn_changed(nvn_dyn state, const void *val, size_t n);
/** @brief Forgets every shadowed value: the next set of each state is made. */
void nvn_dyn_reset(void);

/** @brief The host function of cmd, counted when nvn_vk_count_on is set. */
static inline void (*nvn_vk_fn(unsigned cmd))(void)
{
    if (__builtin_expect(nvn_vk_count_on != 0, 0))
        __atomic_fetch_add(&nvn_vk_calls[cmd], 1, __ATOMIC_RELAXED);
    return g.fns.f[cmd];
}

/** @brief nvnCommandBufferSetRenderEnable and SetRenderEnableConditional's state: draws run, are skipped, or follow two counters. */
#define NVN_RENDER_ALWAYS 0u
#define NVN_RENDER_NEVER 1u
#define NVN_RENDER_IF_EQUAL 2u
#define NVN_RENDER_IF_NOT_EQUAL 3u
/** @brief The identity viewport swizzle (x, y, z, w as +X, +Y, +Z, +W), packed as the driver writes it. */
#define NVN_VIEWPORT_SWIZZLE_IDENTITY 0x6420u

/** @brief Makes the pipeline layouts (nvn_draw.c); false when the device refuses. */
bool nvn_draw_layouts(void);
/** @brief Starts the warm-up of the programs the build and earlier launches name, once (nvn_warm.c); returns when the game may go on. */
void nvn_warm_launch(void);
/**
 * @brief Opens the game's window ahead of its first picture, so the compile screen has somewhere to show; true when a
 * window with a presenter is up (false in a headless run). Any thread.
 */
bool nvn_vk_window_early(void);
/** @brief Brings the host device up if no call has, and makes the pipeline layouts; true when both worked. Safe from any thread. */
bool nvn_vk_ready(void);

/** @brief Draw state a queue keeps across command sets, as the GPU keeps it. */
/** @brief Transform feedback buffers of the GPU. */
#define NVN_XFB_BUFFERS 4u
/** @brief VK_EXT_transform_feedback's buffer usages (the generated headers do not hold the extension). */
#define NVN_BUFFER_USAGE_XFB 0x00000800u
#define NVN_BUFFER_USAGE_XFB_COUNTER 0x00001000u

typedef struct nvn_gfx {
    uint32_t ncolor;
    rt color[MAX_RT];
    rt depth;
    VkRect2D scissor;
    bool scissor_set;
    /** @brief GPU code address of the program bound to each stage, 0 for none. */
    uint64_t code[NVN_STAGES];
    /** @brief The compiler constants of each stage's program (its c[1]), and its control section's guest address. */
    nvn_consts consts[NVN_STAGES];
    uint64_t control[NVN_STAGES];
    /** @brief Storage buffers of the graphics stages, by stage and slot. */
    struct {
        uint64_t addr, size;
    } gstorage[NVN_STAGES][NVN_STORAGE_SLOTS];
    /** @brief Compute storage buffers: GPU address and size. */
    struct {
        uint64_t addr, size;
    } storage[NVN_STORAGE_SLOTS];
    /** @brief Image handles bound per stage and slot. */
    uint32_t image[NVN_STAGES][NVN_IMAGE_SLOTS];
    /**
     * @brief Constants pushed with nvnCommandBufferPushComputeConstants (the
     * slot the program names): NVN_PUSH_BYTES bytes each, the most the driver
     * writes at once. Only the compute stage's entries are used.
     */
    uint8_t push[NVN_STAGES][NVN_PUSH_SLOTS][NVN_PUSH_BYTES];
    uint32_t push_used[NVN_STAGES];
    /**
     * @brief Constants pushed with nvnCommandBufferPushGraphicsConstants, by
     * slot: one set for all graphics stages, which read slot n as constant
     * bank c[24 + n].
     */
    uint8_t gfx_push[NVN_GFX_PUSH_SLOTS][NVN_PUSH_BYTES];
    uint32_t gfx_push_used;
    /** @brief Each stage's c[2] as the driver fills it. */
    uint8_t bank2[NVN_STAGES][NVN_BANK2_BYTES];
    /** @brief bank2 as the bound programs read it: each texture handle names the layer's slot, not the texture id (nvn_texset_bind). */
    uint8_t bank2_host[NVN_STAGES][NVN_BANK2_BYTES];
    /** @brief The separate texture (0) and separate sampler (1) slots of each stage something was bound to, a bit each. */
    uint32_t separate_bound[NVN_STAGES][2][NVN_SEPARATE_SLOTS / 32];
    /** @brief Host copies of user banks whose texture handles carry slots (bind_user_textures), and which are in use by this draw. */
    uint8_t *ubo_host[NVN_STAGES][NVN_UBO_SLOTS];
    uint32_t ubo_patched[NVN_STAGES];
    struct {
        uint64_t addr, size;
    } ubo[NVN_STAGES][NVN_UBO_SLOTS], vb[NVN_VERTEX_BUFFERS];
    uint32_t nattribs, nstreams;
    /** @brief Which attribute registers hold a bound state (in attribs) and which the constant attribute (nvn_vertex_attribs_bound). */
    uint32_t attrib_state, attrib_constant;
    /** @brief Which vertex stream registers hold a bound state in streams (nvn_vertex_streams_bound). */
    uint32_t stream_state;
    uint8_t attribs[NVN_VERTEX_ATTRIBS][4];
    uint8_t streams[NVN_VERTEX_BUFFERS][8];
    uint8_t depth_stencil[8], color_state[4], channel_mask[4], polygon[4], multisample[24];
    uint8_t blend[MAX_RT][8];
    bool have_depth_stencil, have_color_state, have_channel_mask, have_polygon, have_multisample;
    bool have_blend[MAX_RT];
    float viewports[16][4], depth_ranges[16][2];
    VkRect2D scissors[16];
    uint32_t stencil_value_mask[2], stencil_mask[2], stencil_ref[2];
    bool primitive_restart;
    bool rasterizer_discard;
    bool conservative_raster;
    /** @brief The bound fragment program's sample shading (nvn_program_sample_shading). */
    uint32_t sample_shading;
    /** @brief A coverage modulation table is bound (not the driver's linear default). */
    bool modulation_table;
    /** @brief Conservative raster dilate in quarter pixels (0 to 3), as the driver stores it. */
    uint32_t conservative_dilate;
    /** @brief The sample mask (nvnCommandBufferSetSampleMask), 16 bits as the driver writes it. */
    uint32_t sample_mask;
    /** @brief The fixed point size and alpha reference value, which the graphics pipelines read as push constants. */
    float point_size, alpha_ref;
    /** @brief Render enable: NVN_RENDER_ALWAYS, NVN_RENDER_NEVER, or a conditional mode and its counter address. */
    uint32_t render_mode;
    uint64_t render_addr;
    /** @brief The swizzle of each viewport (x, y, z, w selectors packed 4 bits each, as the driver writes them). */
    uint32_t viewport_swizzle[16];
    /** @brief The provoking vertex is the last (NVN_PROVOKING_VERTEX_MODE_LAST); false until the game sets it. */
    bool provoking_last;
    /** @brief The default tessellation levels a pass-through control stage writes: inner then outer. */
    float tess_inner[2], tess_outer[4];
    uint32_t restart_index;
    float polygon_offset[3];
    float blend_color[4];
    float line_width;
    /** @brief GPU counters (nvn_counters.c): what each has counted since its reset, the types reset (bit n: type n), and the types a draw counted that the host cannot. */
    uint64_t counter_acc[NVN_COUNTERS];
    uint32_t counter_valid, counter_inexact;
    /** @brief Draws are counted (a counter was reset), and a draw ran that was not. */
    bool counting, drew_uncounted;
    /** @brief The instances of the draw being replayed: the first and how many. */
    uint32_t inst_base, inst_count;
    bool depth_clamp;
    /** @brief nvnCommandBufferSetDepthBounds: the test is on, and the range of stored depth that passes it. */
    bool depth_bounds;
    float depth_bounds_range[2];
    /**
     * @brief The console's SET_CONSERVATIVE_RASTER_CONTROL register (NVC797 0x0230) as nvnCommandBufferSetConservativeRasterMode
     * leaves it (nvn_draw.c, nvn_conservative_control); NVN_CONSERVATIVE_CONTROL_DEFAULT before the first.
     */
    uint32_t conservative_control;
    /** @brief nvnCommandBufferSetGeometryProcessingMode's mode: 0 (the default) vertex draws, 1 and 2 mesh draws. */
    uint32_t geometry_mode;
    /**
     * @brief The compute overlap mode the game set (nvnCommandBufferSetComputeOverlapMode, its driver's token 0x27):
     * nonzero lets dispatches with no barrier between them run side by side; 0, the mode a queue starts in, runs
     * each dispatch after the one before.
     */
    uint32_t compute_overlap;
    /**
     * @brief Variable-rate shading (nvnCommandBufferShadingRateImagePalette, BindShadingRateImage,
     * SetShadingRatePaletteMode): per viewport, whether a palette was given and its 16 four-bit rates (the hardware's
     * values, VK_NV_shading_rate_image's VkShadingRatePaletteEntryNV; 5 is one invocation per pixel); the texture bound
     * as the shading-rate image (0: none); and the palette mode the game set.
     */
    struct {
        uint16_t enabled;
        uint8_t palette[NVN_SHADING_VIEWPORTS][16];
        uint64_t image;
        uint32_t mode;
        /** @brief The view of image a draw binds (set where the draw's state is gathered, before its pass begins). */
        VkImageView view;
    } shading;
    /**
     * @brief Transform feedback (nvn_xfb.c): each buffer bound (GPU address and bytes, 0 for none), whether a
     * capture is on (from Begin or Resume to End or Pause), the shadow SET_STREAM_OUT_CONTROL_STRIDE(0) Begin writes
     * to the control buffer, and the counters: each buffer's byte count (u32 each), as the console's write pointer.
     */
    struct {
        uint64_t addr[NVN_XFB_BUFFERS], size[NVN_XFB_BUFFERS];
        bool on;
        uint32_t stride0;
        staging counters;
        /** @brief The memory path's records (host-visible, with a device address) and their address. */
        staging scratch;
        uint64_t scratch_gpu;
    } xfb;
} nvn_gfx;

/** @brief What one draw binds for transform feedback (nvn_xfb_draw_prepare); n 0 when it captures nothing. */
typedef struct nvn_xfb_draw {
    uint32_t n;
    VkBuffer buf[NVN_XFB_BUFFERS];
    VkDeviceSize off[NVN_XFB_BUFFERS], size[NVN_XFB_BUFFERS];
    VkBuffer counters;
    /** @brief The memory path: the program capturing, its records and their device address (0: the host's path). */
    const struct nvn_module *mem;
    uint32_t slots;
    uint64_t scratch;
} nvn_xfb_draw;

/** @brief Sets a queue's draw state to the GPU's state before any command (nvn_vk.c). */
void nvn_gfx_init(nvn_gfx *st);

/** @brief One command set being replayed. */
typedef struct replay {
    rl_ctx *c;
    uint8_t *mem;
    const nvn_cmdset *s;
    nvn_gfx *st;
    uint64_t syncs[64];
    uint32_t nsyncs;
    /**
     * @brief Where the replay stands, for the frame trace's order file: the queue the set was submitted to, the
     * submit's number (counting every submit of the process from 1), the index of the command being replayed in the
     * flattened set, and that command's line number in the order file (0 when no frame is traced).
     */
    uint64_t queue, submit, seq;
    uint32_t pos;
    /** @brief The float result (s0) of the last getter. */
    float fret;
} replay;

/**
 * @brief Runs a transform feedback command (nvn_xfb.c): the buffer binds, Begin, End, Pause, Resume and
 * DrawTransformFeedback; NVN_UNHANDLED for any other.
 */
int nvn_xfb_cmd(replay *r, const nvn_cmd *e);
/** @brief Binds x's buffers and begins transform feedback with the queue's counters (inside the draw's rendering). */
void nvn_xfb_draw_begin(VkCommandBuffer cb, const nvn_xfb_draw *x);
/** @brief Ends transform feedback after the draw, writing the counters. */
void nvn_xfb_draw_end(VkCommandBuffer cb, const nvn_xfb_draw *x);
/**
 * @brief After a draw through the memory path: waits for it and writes each record's captured words to its buffer at
 * the buffer's byte count, in record order, as the hardware streams them out. RL_OK or a stop.
 */
int nvn_xfb_draw_finish(replay *r, const nvn_xfb_draw *x);
/** @brief A host-visible buffer of size bytes with a device address (*gpu), for the memory path (nvn_vk.c). */
bool nvn_vk_xfb_scratch_new(staging *s, uint64_t size, uint64_t *gpu);
/** @brief A host-visible counter buffer for transform feedback: NVN_XFB_BUFFERS u32 byte counts (nvn_vk.c). */
bool nvn_vk_xfb_counters_new(staging *s);
/** @brief Draws count vertices from first as DrawArrays does, once (nvn_draw.c). */
int nvn_draw_arrays(replay *r, uint32_t mode, uint32_t first, uint32_t count);


/** @brief What a traced draw or dispatch tells the trace beyond the draw state (nvn_trace.c). */
typedef struct nvn_trace_cmd {
    const char *keys[NVN_STAGES];
    uint32_t mode, itype, first, count, groups[3];
    int32_t base;
    bool indexed;
    /** @brief A dispatch's driver bank c[0] as the program read it (c0_bytes of them), and c[1]'s guest range. */
    const uint8_t *c0;
    uint32_t c0_bytes;
    uint64_t c1_guest, c1_size;
    /** @brief A dispatch's specialization constants (the workgroup size is ids 0 to 2): nspec ids and values. */
    const VkSpecializationMapEntry *spec_map;
    const uint32_t *spec_vals;
    uint32_t nspec;
} nvn_trace_cmd;

/** @brief A frame is being traced (RELIFT_NVN_TRACE). */
extern bool nvn_trace_active;
/**
 * @brief Arms a frame trace of one present from outside the environment (the diagnostic snapshot): the first present
 * numbered from or later is traced into dir/frame-<present>, as RELIFT_NVN_TRACE=<dir>:<from> does.
 */
void nvn_trace_arm(const char *dir, uint64_t from);
/** @brief Ends what nvn_trace_arm started: closes a trace in progress and traces no more presents. */
void nvn_trace_disarm(void);
/**
 * @brief RELIFT_NVN_TRACE_WATCH names a guest range: outside a traced frame each change of it is logged before the
 * command that first sees it (without waiting for the device), inside one it is a trace line.
 */
extern bool nvn_trace_watching;
/** @brief Draws so far into four or more render targets at once; the trace's G-buffer trigger counts them. */
extern uint64_t nvn_trace_wide_draws;
/**
 * @brief Parses RELIFT_NVN_TRACE, <dir>:<present>[:<frames>]: the folder, the present the trace starts at and the
 * number of frames (default 1). False for anything else.
 */
bool nvn_trace_parse(const char *spec, char *dir, size_t dirsz, uint64_t *from, uint64_t *frames);
/** @brief At every present: ends the frame being traced and starts the next one when the trace is due. */
void nvn_trace_present(replay *r);
/**
 * @brief Before a draw, mesh draw or dispatch ("draw", "mesh", "dispatch"): submits the batch and saves what the
 * command will read.
 */
void nvn_trace_begin(replay *r, const char *kind);
/** @brief A texture or image the command being traced binds, saved as it is before the command. */
void nvn_trace_texture(replay *r, vimg *v, uint32_t id, uint32_t slot, uint32_t kind, uint32_t dim, bool image,
                       const uint8_t view[17]);
/** @brief The GPU trace report's line limit and PC window from the environment (nvn_draw.c). */
void trace_window(uint32_t *lines, uint32_t *from, uint32_t *to);
/** @brief Replays one command of a set onto r, as the scheduler does (tests replay single commands with it). */
int nvn_vk_cmd(replay *r, const nvn_cmd *e);
/** @brief A traced program's read through a register handle: an "image read" line in the frame trace (nvn_trace.c). */
void nvn_trace_image_read(const char *key, uint32_t pc, uint32_t handle, uint32_t reads);
/** @brief After the command is recorded: submits it and writes its line and its files. */
void nvn_trace_end(replay *r, const nvn_trace_cmd *c);
/**
 * @brief Each replayed command of a traced frame, before it runs (done false) and after (done true): a "cmd" line
 * with its name and arguments (an inline uniform update with its target and its bytes as a file), and with
 * RELIFT_NVN_TRACE_WATCH=<guest hex>:<size> a "watch" line with the range's bytes each time they change, naming
 * the command that changed them or "cpu" for a change between commands.
 */
void nvn_trace_command(replay *r, const nvn_cmd *e, bool done);
/** @brief The kind and program keys of the last draw or dispatch the layer recorded (\"draw 1234abcd ...\"). */
const char *nvn_draw_last(void);
/** @brief Keeps command e in the GPU trace's list of the present's commands (RELIFT_NVN_GPU_TRACE). */
void nvn_trace_ring_note(replay *r, const nvn_cmd *e);
/** @brief Logs the present's commands so far (the last most of them, 0 for up to 8192), after the line why. */
void nvn_trace_ring_dump(const char *why, uint32_t most);
/**
 * @brief Clears layers layers of level level of v from layer, within the rect clear, to the color val (16 bytes in
 * the image's format), one host pass per layer; what names the command in errors. The clear of nvnCommandBufferClearTexture.
 */
int nvn_vk_clear_image(replay *r, vimg *v, uint32_t level, uint32_t layer, uint32_t layers, VkRect2D clear,
                       const uint8_t val[16], const char *what);
/**
 * @brief Thumbnail k of frame number frame: what wrote it and the image's storage, level and layer; false past the
 * last one or for a frame the ring no longer holds.
 */
bool nvn_flash_thumb_of(uint64_t frame, uint32_t k, const char **what, uint64_t *storage, uint32_t *level,
                        uint32_t *layer);
/** @brief The flash trigger is on (RELIFT_NVN_FLASH, nvn_flash.c); the first call reads the setting. */
bool nvn_flash_on(void);
/** @brief Notes that the GPU wrote level and layer of v: the next nvn_flash_thumbs takes its thumbnail. */
void nvn_flash_wrote(const vimg *v, uint32_t level, uint32_t layer);
/** @brief Whether images wait for their thumbnails (nvn_flash_wrote since the last nvn_flash_thumbs). */
bool nvn_flash_pending(void);
/**
 * @brief Records the thumbnails of the images written since the last call, what naming the writer ("pass",
 * "dispatch", "copy", "upscale", "upload", "alias"). Outside a render pass; r begins a command buffer when none is
 * recording (NULL when one is).
 */
void nvn_flash_thumbs(replay *r, const char *what);
/** @brief v is freed: it leaves the images waiting for thumbnails. */
void nvn_flash_image_gone(const vimg *v);
/** @brief Records, before the present's wait, the copy of the presented rect of src and its signature for frame. */
void nvn_flash_present_record(replay *r, VkImage src, const uint32_t rect[4], uint64_t frame);
/** @brief After the present's wait: feeds frame's signature to the detector and saves an outlier's evidence. */
void nvn_flash_presented(replay *r, uint64_t frame);
/** @brief A bound buffer range of a dispatch for the differential capture: kind 0 storage, 1 uniform, 2 base. */
typedef struct nvn_diff_range {
    uint64_t gpu, size;
    uint32_t kind, slot;
    bool write;
} nvn_diff_range;
/** @brief What the differential capture keeps of a dispatch besides its memory (nvn_diff.c). */
typedef struct nvn_diff_dispatch {
    const char *key;
    uint32_t groups[3];
    bool indirect;
    const uint8_t *c0;
    uint32_t c0_bytes;
    uint64_t c1_guest;
    uint32_t c1_size;
    const VkSpecializationMapEntry *spec_map;
    const uint32_t *spec_vals;
    uint32_t nspec;
    /** @brief The guest push data (NVN_PUSH_BYTES), or NULL when the program pushes none. */
    const uint8_t *push;
    /** @brief The push constants the runtime itself pushes (its fixed_push), and their size. */
    const void *fixed_push;
    uint32_t fixed_push_bytes;
    /** @brief The indirect dispatch's argument buffer and offset (NULL for a direct dispatch). */
    VkBuffer ibuf;
    uint64_t ioff;
    const nvn_diff_range *range;
    uint32_t nrange;
} nvn_diff_dispatch;
/** @brief The differential capture is on (RELIFT_NVN_DIFF, nvn_diff.c); the first call reads the setting. */
bool nvn_diff_on(void);
/** @brief A dispatch starts binding: the textures noted so far belonged to other commands. */
void nvn_diff_dispatch_reset(void);
/** @brief A texture or storage image (image) the next dispatch binds, at its bound level and layer. */
void nvn_diff_texture(const vimg *v, uint32_t id, uint32_t slot, uint32_t kind, uint32_t dim, bool image,
                      uint32_t level, uint32_t layer, const uint8_t view[17], uint32_t sampler);
/** @brief A sampler id's host sampler as made: its create info (pNext dropped) and reduction mode. */
typedef struct nvn_sampler_info {
    VkSamplerCreateInfo ci;
    uint32_t reduction;
} nvn_sampler_info;
/** @brief The host sampler of sampler id `id` as last bound; false when none is. */
bool nvn_texset_sampler_info(uint32_t id, nvn_sampler_info *out);
/** @brief Just before the dispatch is recorded: records it and the copies of its not yet captured inputs. */
void nvn_diff_dispatch_begin(replay *r, const nvn_diff_dispatch *d);
/** @brief Just after the dispatch is recorded: the copies of what it may have written. */
void nvn_diff_dispatch_end(replay *r);
/** @brief Records, before the present's wait, the detectors' copy of the presented rect of src. */
void nvn_diff_present_record(replay *r, VkImage src, const uint32_t rect[4], uint64_t frame);
/** @brief After the present's wait: runs the detectors and the manual triggers, and saves each frame they name. */
void nvn_diff_presented(replay *r, uint64_t frame);
/** @brief Presents whose trace lines the ring keeps in memory. */
#define NVN_TRACE_RING 4u
/** @brief The trace ring is on: every draw and dispatch writes its line to memory, without files or submits. */
extern bool nvn_trace_ring_on;
/** @brief Turns the trace ring on (for good) and starts the present being drawn. */
void nvn_trace_ring_start(void);
/** @brief Lines written for the present being drawn so far. */
uint64_t nvn_trace_cmds(void);
/**
 * @brief Adds a line of kind to the ring (or the frame trace being written) with fmt's text as its other JSON fields,
 * such as "\"storage\":\"0x10\"". Nothing when neither is on.
 */
void nvn_trace_note(const char *kind, const char *fmt, ...) RL_PRINTF(2, 3);
/** @brief Writes the ring's lines of the present drawn under frame number frame to path; false when the ring no longer has them. */
bool nvn_trace_ring_write(uint64_t frame, const char *path);
/** @brief The levels and layers of an image's host image (nvn_vk.c). */
VkImageSubresourceRange nvn_vk_image_range(const vimg *v);
/**
 * @brief The range of a barrier over all of v: every level and every layer as VK_REMAINING_*, which for a 3D image
 * made 2D-array compatible means its whole depth whether or not the host has maintenance9.
 */
VkImageSubresourceRange nvn_vk_barrier_range(const vimg *v);

/** @brief The linear or sRGB twin of an 8-bit color format, which a texture view may name for a render target of the other; VK_FORMAT_UNDEFINED for any other format. */
VkFormat nvn_vk_color_twin(VkFormat f);
/** @brief Widens n 8-bit indices to 16 bits for the host device, which has no 8-bit index type; with restart, the 8-bit restart index 0xff becomes the 16-bit one, 0xffff. */
void nvn_widen_indices(const uint8_t *src, uint32_t n, bool restart, uint16_t *dst);
int32_t nvn_vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want);
bool nvn_vk_staging_new(staging *s, uint64_t size);
/** @brief A staging buffer the CPU reads after the device writes it: host-cached memory where the device has it. */
bool nvn_vk_readback_new(staging *s, uint64_t size);
/** @brief A device-local buffer of size bytes with the given usage (storage, index, indirect and transfer ones). */
bool nvn_vk_gpu_buffer(VkBuffer *buf, VkDeviceMemory *memory, uint64_t size);
void nvn_vk_staging_free(staging *s);
void nvn_vk_barrier(VkCommandBuffer cb);
/**
 * @brief vkCmdFillBuffer(cb, buf, off, size, value) with size a byte count (never VK_WHOLE_SIZE) that every driver
 * runs: the part of the range below NVN_VK_FILL_LIMIT is a fill, the part at or past it copies of a buffer filled with
 * value. The Adreno 740 driver 0676.53 fails a fill whose range reaches 512 MiB into its buffer (vkEndCommandBuffer
 * fails, or the next command crashes in the driver) while copies there work; a mirrored memory pool is up to 1 GiB.
 * Records barriers around the copies; false when the pattern buffer cannot be made.
 */
bool nvn_vk_fill(VkCommandBuffer cb, VkBuffer buf, uint64_t off, uint64_t size, uint32_t value);
/** @brief The offset into a buffer from which nvn_vk_fill copies instead of filling. */
#define NVN_VK_FILL_LIMIT (512ull << 20)
/**
 * @brief Records an indirect dispatch of the bound compute pipeline whose group counts are the three u32 at ioff in
 * ibuf, read by the GPU when it runs; when c0 is not NULL, a copy of the counts first fills the 12 bytes at c0_off in
 * c0 (the driver table's group counts, NVN_C0_GROUPS past its start), behind a barrier.
 */
void nvn_vk_dispatch_indirect(VkCommandBuffer cb, VkBuffer ibuf, uint64_t ioff, VkBuffer c0, uint64_t c0_off);
bool nvn_vk_begin(replay *r);
/**
 * @brief Whether the host command buffer holds commands not yet submitted. There is one command buffer for every
 * replay: what one command set recorded is submitted by whichever replay submits next, so commands that ran for a
 * set the scheduler resumed (after an event wait) are never dropped by another replay beginning a new buffer.
 */
bool nvn_vk_recording(void);
/** @brief Marks the command buffer as recording or not without touching it: for tests that replay over stand-ins of the device calls. */
void nvn_vk_assume_recording(bool on);

/**
 * @brief Writes n elements of an integer vertex attribute of format f, read from src stride bytes apart, as four
 * 32-bit words each (missing components 0, 0, 1; signed formats sign-extended) into out (nvn_draw.c).
 */
void nvn_expand_integer(const uint8_t *src, uint64_t stride, VkFormat f, uint64_t n, uint32_t *out);
/**
 * @brief Submits what is recorded (by any replay) and waits for it; the arena is free again
 * after. With nothing recorded, it frees what a command put in the arena
 * before recording (that counts as a submit for nvn_vk_stage).
 */
bool nvn_vk_flush(replay *r);
/**
 * @brief How many times nvn_vk_flush finished with nothing recorded. A thing noted at epoch e that is looked at again at
 * a later epoch was last used by work that has since been submitted and run to its end.
 */
uint64_t nvn_vk_flush_epoch(void);
/** @brief The meaning of a VkDeviceFaultAddressTypeEXT value, for the fault report and stops. */
const char *nvn_vk_fault_type(int type);
/**
 * @brief Notes the lost device's fault report (its description and n addresses with their types and what the layer's
 * pools held there), once; every later stop names it first (nvn_vk_lost_text).
 */
void nvn_vk_lost_note(const char *desc, const int *types, const uint64_t *addrs, const char *const *where, uint32_t n);
/** @brief What the lost device's fault report said, NULL while the device was not lost. */
const char *nvn_vk_lost_text(void);
/** @brief Stops the game as rl_guest_fatal does, naming the lost device and its fault first when the device was lost. */
int nvn_vk_fatal(rl_ctx *c, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/**
 * @brief Submits what is recorded (by any replay) without waiting for it, from the recording slot: the host device runs it while the
 * guest goes on and the layer records the next batch into the other slot (NVN_SLOTS). The submit first waits for the
 * batch the other slot still holds, and replay waits at nvn_lock only for batches that may write guest memory
 * (nvn_vk_mark_write); a flush, a sync point and nvn_vk_settle wait for every batch. With nothing recorded, as
 * nvn_vk_flush.
 */
bool nvn_vk_submit(replay *r);
/**
 * @brief Makes the next submit wait on the device until the timeline semaphore sem reaches value, which the host
 * signals: a self-check holds a batch to see what happens before the device runs it. Needs timeline semaphores.
 */
void nvn_vk_hold_next_submit(VkSemaphore sem, uint64_t value);
/**
 * @brief Marks the batch being recorded as one whose GPU work may write guest memory (a program that stores to a
 * guest buffer, a uniform update or a texture write-back into a pool): until it finished, replay waits for it before
 * reading guest memory (nvn_lock).
 */
void nvn_vk_mark_write(void);
/** @brief For nvn_vk_ring_check: records a timestamp report (type 0) as the batch's last command, into a scratch address. */
int nvn_counters_stamp_check(replay *r);
/** @brief Marks guest bytes [guest, guest + size) as ones the batch being recorded may write (a storage buffer a storing program has bound, a uniform update into a pool). */
void nvn_vk_mark_write_range(uint64_t guest, uint64_t size);
/**
 * @brief Before replay reads guest bytes [guest, guest + size): waits for the batches in flight that may write them,
 * so the read sees what the GPU wrote there, as the console's in-order GPU would have by then. False on a failure.
 */
bool nvn_vk_reading_guest(uint64_t guest, uint64_t size);
/** @brief Whether a batch in flight may write guest bytes [guest, guest + size). */
bool nvn_vk_pending_write(uint64_t guest, uint64_t size);
/** @brief Whether nvn_untile_move can move v's bytes on the GPU (nvn_untile.c). */
bool nvn_untile_can(const vimg *v);
/**
 * @brief Records the move of every level and layer of v between its guest storage and its host image on the GPU:
 * into the image (an upload) or, with tile, into the storage (a write-back), the storage read or written when the
 * batch runs. False, with nothing recorded, when v's format or storage does not allow it.
 */
bool nvn_untile_move(replay *r, vimg *v, bool tile);
/**
 * @brief nvn_untile_move into the storage (a write-back) for a read by later commands of the batch: through an
 * imported pool, or a mirror (nvn_mirror.c), which is brought up to the guest's writes first and whose moved bytes
 * are noted for its readback, so guest memory gets them when the batch completes. False, with nothing recorded, when
 * v's format or storage does not allow it.
 */
bool nvn_untile_write_back(replay *r, vimg *v);
/**
 * @brief Keeps buffer buf and its memory until the batch being recorded has run, then frees them (with the moves'
 * own buffers); false when the list is full and the caller must free them itself.
 */
bool nvn_untile_keep_buffer(VkBuffer buf, VkDeviceMemory mem);
/**
 * @brief Records an upload of v's storage into v's image through a packed device buffer: a shader reads the storage
 * through its pool's device address and writes the texels (or compressed blocks) packed, and a buffer copy puts them
 * into the image, for images no shader can store to (depth, compressed). False, with nothing recorded, when v's
 * format, aspect or layout does not allow it or its storage is outside every imported pool.
 */
bool nvn_untile_upload_packed(replay *r, vimg *v);
/**
 * @brief The imported pool buffer that holds guest GPU bytes [addr, addr + n) at a multiple of align, and the offset
 * there, so a draw reads its vertices and indices where the guest keeps them, when the GPU runs it, as the console's
 * does; NULL when no imported pool holds them (nvn_draw.c copies them into the arena then).
 */
VkBuffer nvn_draw_pool_span(uint64_t addr, uint64_t n, uint64_t align, VkDeviceSize *off);
/** @brief Frees what the moves of slot's batch used; called once that batch has finished. */
void nvn_untile_release(uint32_t slot);
/** @brief Makes slot the one moves record their descriptor sets, views and buffers for. */
void nvn_untile_slot(uint32_t slot);
/** @brief Moves recorded so far, for the stats. */
uint64_t nvn_untile_moves(void);
/**
 * @brief Submits the batch when it holds a GPU write of guest uniform data, so a host copy of guest memory taken
 * next shows it.
 */
bool nvn_vk_sync_uniforms(replay *r);
/**
 * @brief Runs stage(r, arg), which puts one command's data into the batch
 * being recorded (uploads, descriptor sets), before the command records
 * anything. When the batch had to be submitted partway, which drops what
 * stage put before, it runs stage again on the empty batch; when that
 * submits too, the data of what (a draw, a dispatch) does not fit one batch
 * and it stops. Returns RL_OK or a pending stop.
 */
int nvn_vk_stage(replay *r, int (*stage)(replay *r, void *arg), void *arg, const char *what);
/**
 * @brief Records into the batch r is recording (starting one) the copies that bring the mirrors up to the guest's
 * writes: only the pages written since the last copy. With may_flush and nothing recorded yet, a large upload is
 * submitted in slices. Call it before recording a command that can read a pool. False when the device failed.
 */
/** @brief Whether draws are labeled and images named for a capture tool (nvn_label.c). */
bool nvn_label_on(void);
/** @brief Notes the guest command being replayed: its set, its index in the set and its op, for the labels. */
void nvn_label_command(const void *set, uint32_t index, uint32_t op);
/** @brief Labels the next command in cb with the guest command and the program keys by stage (vs tcs tes gs fs cs). */
void nvn_label_draw(VkCommandBuffer cb, const char *kind, const char *const keys[6]);
/** @brief Names a Vulkan object for the capture tool (printf format). */
void nvn_label_object(VkObjectType type, uint64_t handle, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
bool nvn_mirror_sync(replay *r, bool may_flush);
/** @brief Why the last nvn_mirror_sync failed, for the stop report. */
const char *nvn_mirror_failure(void);
/** @brief Stops the replay: program key stores without page marks while pools are mirrored (the table's device imports). */
int nvn_mirror_refuse_unmarked(replay *r, const char *key);
/** @brief Frees the staging of uploads once the batch that used it completed. */
void nvn_mirror_flushed(void);
/**
 * @brief Whether any pool is mirrored. Mirror staging is shared by the batches in flight and freed only once all
 * finished, and every batch counts as one that writes guest memory while mirrors exist.
 */
bool nvn_mirror_any(void);
/** @brief Whether the bytes noted by nvn_mirror_written still wait for a readback. */
bool nvn_mirror_readback_pending(void);
/**
 * @brief Names to note the guest ranges that the readback just recorded (nvn_mirror_record_readback) writes when its
 * batch completes: the copied segments, and every mirror while a page bitmap is on its way back.
 */
void nvn_mirror_batch_writes(void (*note)(uint64_t guest, uint64_t size));
/** @brief Records copies of the next slice of the noted bytes into cb; they land in guest memory at nvn_mirror_apply_readback. */
bool nvn_mirror_record_readback(VkCommandBuffer cb);
/** @brief After the batch completed: writes the readback slice into guest memory, the pages that differ only. */
void nvn_mirror_apply_readback(void);
/** @brief Forgets the noted bytes (the device failed). */
void nvn_mirror_drop_readback(void);
/**
 * @brief Bytes uploaded, bytes read back that differed, uploads made, bytes copied back for compares, and pages the
 * written-page bitmap noted, for the stats line.
 */
/** @brief The mirrors held (the diagnostic snapshot): how many, the guest bytes they cover and the staging bytes their uploads and readbacks hold. */
void nvn_mirrors_held(uint32_t *n, uint64_t *bytes, uint64_t *staging_bytes);
void nvn_mirror_stats(uint64_t *up, uint64_t *down, uint64_t *syncs, uint64_t *read, uint64_t *marked);
/** @brief Stops the game with a report (after flushing what is recorded). Returns RL_PENDING. */
int nvn_vk_stop(replay *r, const char *fmt, ...) RL_PRINTF(2, 3);
/** @brief The host format of an NVNformat; VK_FORMAT_UNDEFINED for one the layer does not map. */
VkFormat nvn_vk_format(uint32_t f, VkImageAspectFlags *aspect);
/**
 * @brief Begins dynamic rendering on the queue's render targets, or keeps the open pass when it renders to the same
 * attachments; area gets their common extent. A target whose sync is pending (vimg.sync_pending) takes the bytes of
 * the newer images over its storage first, as the pass loads what it holds.
 */
bool nvn_vk_begin_rendering(replay *r, VkRect2D *area);
/**
 * @brief Whether a clear of render target t (its whole aspect) writes every byte of its image: one level and layer,
 * the bound level 0, no scissor cutting it and no smaller target narrowing the render area. Such a clear first
 * drops a pending sync (vimg.sync_pending): the bytes it would take are all overwritten.
 */
bool nvn_vk_clears_all(const replay *r, const rt *t);
/**
 * @brief nvn_vk_begin_rendering for a draw whose last pre-raster stage writes Layer when layered: the targets are
 * seen as 2D arrays over the layers every one of them spans (nvn_vk_begin_rendering when that is one).
 */
bool nvn_vk_begin_rendering_layered(replay *r, VkRect2D *area, bool layered);
/**
 * @brief Ends the render pass the draws left open, with a barrier on all commands after it; nothing when none is open.
 * Every command recorded outside a pass ends it first (nvn_vk_begin), as do a submit and the game's barriers.
 */
void nvn_vk_end_rendering(void);
/**
 * @brief A command about to read v (a texture or image binding of a draw or dispatch): when v is an attachment of
 * the open render pass, the pass ends with its barrier first, so the read sees what the pass drew.
 */
void nvn_vk_reading(const vimg *v);
/**
 * @brief The ranges a dispatch about to be recorded touches: nvn_vk_dispatch_reset starts them, the buffer and image
 * calls add guest address ranges it reads or writes: an image by the bytes of its storage that the levels (one, or
 * level and those after it) hold in each layer a view names (0 layers for all from layer; one range for up to 8
 * layers, past that one span over them).
 */
void nvn_vk_dispatch_reset(void);
void nvn_vk_dispatch_buffer(uint64_t addr, uint64_t size, bool write);
void nvn_vk_dispatch_image(const vimg *v, uint32_t level, bool one_level, uint32_t layer, uint32_t layers, bool write);
/** @brief The dispatch may read or write memory no binding names (global loads and stores): it waits and is waited for. */
void nvn_vk_dispatch_anywhere(void);
/**
 * @brief Whether a program with an interface record of this kind may touch memory no binding names: global loads or
 * stores, or a texture or image reached through a register handle. Such a dispatch is ordered against every other.
 */
bool nvn_dispatch_unbounded(uint32_t kind);
/**
 * @brief Begins recording a dispatch: while the game's compute overlap mode lets dispatches overlap and other
 * dispatches ran with no barrier since, it records no barrier when the ranges it touches meet none that they write and
 * it writes none they read; otherwise (the console's default mode runs each dispatch after the one before) as
 * nvn_vk_begin, which ends the open dispatches with a barrier.
 */
bool nvn_vk_begin_dispatch(replay *r);
/** @brief The dispatch was recorded: its ranges join the open dispatches until the next barrier. */
void nvn_vk_dispatched(void);
/** @brief The game's nvnCommandBufferBarrier: ends the open render pass (with its barrier), else a barrier alone. */
void nvn_vk_game_barrier(replay *r);
/** @brief Copies n guest bytes at addr into the arena (zero-filled to pad bytes); offset of the copy in *off. */
bool nvn_vk_upload(replay *r, uint64_t addr, uint64_t n, uint64_t pad, uint64_t align, uint64_t *off);
/**
 * @brief Calls the SDK driver's getter name on a copy of the state bytes blob (at most 256 bytes) with the integer
 * arguments args, then out_n (at most 4) output pointers 4 bytes apart; out gets 4 words (16 bytes from the single
 * pointer when out_n is 1, so an array output fits), so it must hold 4. The integer result goes to *ret, the float
 * result to r->fret.
 */
int nvn_vk_getter(replay *r, const char *name, const void *blob, size_t n, const uint64_t *args, unsigned nargs,
                  unsigned out_n, uint32_t *out, uint64_t *ret);
/** @brief The host image of the texture at guest address tex, made and filled from its storage the first time; NULL after a stop. */
/**
 * @brief The samples of each pixel of the render targets st binds (1 for single-sampled ones), 0 with none bound;
 * false when they differ.
 */
bool nvn_vk_target_samples(const nvn_gfx *st, uint32_t *samples);
/**
 * @brief The host sample locations of n samples NVN keeps as nibbles (x | y << 4 in 1/16 pixel, nvnMultisampleState
 * GetSampleLocations gives each as nibble / 16): each is exactly nibble / 16, which a device of
 * nvn_vkdev.sample_location_counts places on that 1/16 grid point.
 */
static inline void nvn_vk_sample_locations(const uint8_t nib[16], uint32_t n, VkSampleLocationEXT out[16])
{
    for (uint32_t i = 0; i < n && i < 16; i++)
        out[i] = (VkSampleLocationEXT){ (float)(nib[i] & 15u) / 16.0f, (float)(nib[i] >> 4) / 16.0f };
}
/** @brief The dither flag of nvn_pipe_key.alpha_to_coverage and of the A2C_SPEC constant. */
#define NVN_A2C_DITHER 0x100u
/**
 * @brief Whether the host's own alpha to coverage is the console's for a draw of a (nvn_pipe_key.alpha_to_coverage):
 * NVIDIA's, with the dither, at 2, 4 or 8 samples (the RTX 3090's Vulkan masks are the dithered ones; it has no
 * 16-sample color targets). Elsewhere the fragment program computes the mask (A2C_SPEC).
 */
static inline bool nvn_a2c_native(uint32_t a)
{
    return a && g.nvidia && (a & NVN_A2C_DITHER) && (a & 0xffu) <= 8;
}
/** @brief The sample count of the host image of a texture of d->samples samples (NVN's 0 and 1 are one sample). */
static inline VkSampleCountFlagBits nvn_vk_sample_bits(const nvn_tex_desc *d)
{
    return d->samples > 1 ? (VkSampleCountFlagBits)d->samples : VK_SAMPLE_COUNT_1_BIT;
}
/**
 * @brief Fills multisample image v from its guest storage, every sample's bits unchanged (nvn_msaa.c); false with
 * why set when it cannot.
 */
bool nvn_msaa_upload(replay *r, vimg *v, const char **why);
/** @brief Stores every sample of multisample image v to its guest storage, bits unchanged; false when it cannot. */
bool nvn_msaa_write_back(replay *r, vimg *v);
/**
 * @brief Downsample's taps for a source of samples samples: the sample indices it averages (bits 4i to 4i + 3) and
 * their count (nvn_msaa.c downsample_taps).
 */
bool nvn_msaa_downsample_taps(uint32_t samples, uint32_t *taps, uint32_t *n);
/** @brief Records Downsample's mean of src's taps into target (format f, w by h from the origin); false with why. */
bool nvn_msaa_resolve(replay *r, vimg *src, VkImageView target, VkFormat f, uint32_t w, uint32_t h, const char **why);
/** @brief Frees what v's sample moves keep (vimg.msaa). */
void nvn_msaa_free(vimg *v);
/**
 * @brief The guest's sample grid of a texture of samples samples, gw by gh texels a pixel, and the cell of each
 * sample (cx | cy << 2 in bits 4s to 4s + 3); false for other counts.
 */
bool nvn_msaa_cells(uint32_t samples, uint32_t *gw, uint32_t *gh, uint64_t *cells);
/** @brief The layer's single-sample upload of v from its storage (not for multisample images). */
bool nvn_vk_fill_plain(replay *r, vimg *v);
/** @brief The layer's single-sample write-back of v to its storage (not for multisample images). */
bool nvn_vk_store_plain(replay *r, vimg *v);
/** @brief Frees an image that is in no list (its views, memory and the struct itself). */
void nvn_vk_image_drop(vimg *v);
/** @brief Whether the layer can make a host image of the texture d describes (its format, target and samples). */
bool nvn_vk_can_image(const nvn_tex_desc *d);
vimg *nvn_vk_image(replay *r, uint64_t tex);
/** @brief Refills v from its storage when the CPU may have changed the bytes (nvn_vk.c); false when the refill fails. */
/**
 * @brief Brings up to date, by an image copy, the images the texture set keeps for every registered texture
 * (vimg.synced) over the same rows as an image the GPU wrote after write stamp since; one over the bytes in another
 * layout takes them when a draw binds it. False with the game stopped when one cannot be.
 */
bool nvn_vk_sync_aliases(replay *r, uint64_t since);
/**
 * @brief Whether bytes, v's storage, hash other than they did when v was last filled from them; keeps the new hash, so
 * the next call compares with these bytes.
 */
bool nvn_vk_bytes_changed(vimg *v, const uint8_t *bytes);
bool nvn_vk_refresh(replay *r, vimg *v);
/** @brief The single-level 2D R8_UINT view of v a shading-rate image binds through, or NULL when v cannot be one. */
VkImageView nvn_vk_rate_view(vimg *v);
/**
 * @brief A view of v of type t: over all its levels and the layers t takes
 * with the texture's swizzle (kept with v), or, given nv, with the levels,
 * layers, swizzle and depth-stencil mode of that texture view (the caller's
 * to destroy). A level or layer count of 0 in nv takes every level or layer
 * from its base on: the SDK driver's texture descriptor code (SDK 22.2.0,
 * NVN2) reads a view's level count of 0 as the texture's last level. NULL on
 * failure.
 */
VkImageView nvn_vk_view(vimg *v, VkImageViewType t, const nvn_view *nv);
/** @brief A new storage view of level level of v as type t in format f (the caller destroys it), or NULL. */
/** @brief Notes that v is written now (it becomes the image the textures sharing its storage copy from). */
void nvn_vk_written(vimg *v);
/**
 * @brief Whether u's storage holds what the GPU last wrote to u: u was written back at its current write stamp and no
 * write-back of older contents over any of its bytes came after (a newer image's bytes there are newer data, kept).
 * False when too many write-backs came after to tell.
 */
bool nvn_vk_wb_current(const vimg *u);
/** @brief Notes that u's bytes were just written back to its storage. */
void nvn_vk_wb_note(vimg *u);
/**
 * @brief The images over any byte of guest bytes [guest, guest + size) that the GPU wrote after their bytes were last
 * written back to their storage (nvn_vk_wb_current), oldest write first: at most max in out. Returns how many there
 * are, which is more than max when they do not all fit.
 */
uint32_t nvn_vk_unsaved_over(uint64_t guest, uint64_t size, vimg **out, uint32_t max);
/**
 * @brief Before a command reads guest bytes [guest, guest + size) through a buffer: writes back the images over them
 * that the GPU wrote since their last write-back (nvn_vk_unsaved_over), oldest first, so the read sees what was drawn
 * there, as on the console, whose render targets are those bytes. Each move is recorded on the GPU ahead of the read,
 * with no wait on the host, and only images in the list of those the GPU wrote are searched. An image whose texels
 * cannot move as raw words (nvn_untile_write_back) is reported and the read sees the older bytes. RL_OK, or a pending
 * stop.
 */
int nvn_vk_reading_buffer(replay *r, uint64_t guest, uint64_t size);
/**
 * @brief Takes out of the images a texture can find or copy from every image whose memory pool was removed after it
 * was made (finalized, or another object made at the pool's address). The pool's memory is the game's again, and the
 * console's GPU wrote the image's texels when it drew them, so nothing of the image may land in that memory later.
 * The image stays allocated, as a render target or texture binding may still name it.
 */
void nvn_vk_retire_images(void);
VkImageView nvn_vk_storage_view(const vimg *v, VkImageViewType t, VkFormat f, uint32_t level, uint32_t layer,
                                uint32_t layers);
/**
 * @brief The host format of a view of v as NVNformat format (aspect in
 * *aspect): a color format with v's texel block size and shape, or
 * VK_FORMAT_UNDEFINED.
 */
VkFormat nvn_vk_view_format(const vimg *v, uint32_t format, VkImageAspectFlags *aspect);
/** @brief Copies n host bytes into the arena (zero-filled to pad bytes); offset of the copy in *off. */
bool nvn_vk_upload_bytes(replay *r, const void *src, uint64_t n, uint64_t pad, uint64_t align, uint64_t *off);
/**
 * @brief nvn_vk_upload_bytes for a uniform bank, which the GPU only reads: when the batch being recorded already
 * holds the same n bytes padded the same way, their offset is given instead of a new copy.
 */
bool nvn_vk_upload_bank(replay *r, const void *src, uint64_t n, uint64_t pad, uint64_t align, uint64_t *off);
/** @brief The layout of the texture set (set 2); VK_NULL_HANDLE when it cannot be made (nvn_texset.c). */
VkDescriptorSetLayout nvn_texset_layout(void);
/**
 * @brief The texture set makes only its set layout, not its descriptor pool, set and per-id tables (about 1.7 GB on an
 * Adreno 740 with 621378 ids): for a process that makes pipelines and never draws (relift_compile_check). A pipeline
 * depends on the set layout only, which stays the same, so what the driver compiles is the same. Call before the first
 * nvn_texset_layout; nvn_texset is NULL then.
 */
void nvn_texset_layout_only(void);
/** @brief The texture set, to bind as set 2. */
VkDescriptorSet nvn_texset(void);
/**
 * @brief Makes the texture set hold texture handle's texture (in each array
 * its view type fits) and sampler; *kind gets its numeric kind (0 float,
 * 1 signed, 2 unsigned) and *dim its dimension (the translator's Dim).
 * *slot gets the slot the texture holds in the set: the arrays have room for
 * fewer ids than a handle can name, so the handle a program reads must carry
 * the slot in place of the id. A re-bound id keeps its slot. Returns RL_OK,
 * or RL_PENDING after a stop.
 */
int nvn_texset_bind(replay *r, uint32_t handle, uint32_t *kind, uint32_t *dim, uint32_t *slot);
/**
 * @brief nvn_texset_bind for a separate texture handle: the texture only, no sampler, whose id the handle leaves 0.
 * *slot gets UINT32_MAX when no texture is registered under the handle's id (the program then reads a null
 * descriptor instead of the draw stopping on a stale binding).
 */
int nvn_texset_separate_texture(replay *r, uint32_t handle, uint32_t *slot);
/**
 * @brief Makes the texture set hold every texture and sampler registered in the guest's pools, each texture in the slot
 * of its own id, and brings them up to date once per presented frame: a program that reads a texture handle out of
 * memory names the id, which the host cannot rewrite, so the pool's whole contents must be where the id points (the
 * console's model: a pool holds every descriptor). A texture registered under an id the set cannot hold in a slot of
 * its own stops, naming the id and the limit. Returns RL_OK, or RL_PENDING after a stop.
 */
int nvn_texset_sync_registered(replay *r);
/** @brief Image uploads from guest bytes since the start: the work a refresh did, which tests count. */
uint64_t nvn_vk_uploads(void);
/**
 * @brief Keeps in its slot, with its sampler, the registered texture each word of a constant bank's bytes (size of
 * them) names that a program may load as a handle from a register: the word at offset, or with indexed every word from
 * offset to the end. A word that names no registered texture, or an id past the ones with a slot of their own, is not
 * a handle the program can sample and is left alone.
 */
int nvn_texset_bind_words(replay *r, const uint8_t *bytes, uint32_t size, uint32_t offset, bool indexed);
/** @brief Whether the view nv of image v names a format the host can give the image (no format change, or one of the same block footprint). */
bool nvn_texset_view_fits(const vimg *v, const nvn_view *nv);
/** @brief How many texture ids sit in a slot of their own number (0 before the set is made). */
uint32_t nvn_texset_pinned(void);
/** @brief Whether the texture set's layout declares the storage image array at binding (true when there is no set). */
bool nvn_texset_declares_image(uint32_t binding);
/** @brief How formatted surface accesses reach a slot's storage image (see nvn_texset_surface_kind). */
enum { NVN_SURFACE_DIRECT, NVN_SURFACE_RGB9E5, NVN_SURFACE_NONE };
/**
 * @brief The value of a formatted surface access's kind constant (the translator's TEXTURE_KIND_SPEC constants) for
 * the storage image in slot whose numeric kind is kind: kind itself when the host device stores the image's format,
 * NVN_SURFACE_KIND_RGB9E5 for an E5B9G9R9 image, whose texels the access moves unchanged through the 32-bit raw view,
 * UINT32_MAX when the host can store the format neither way.
 */
uint32_t nvn_texset_surface_kind(uint32_t slot, uint32_t kind);
/** @brief The NVN format of the texture in slot (0 when it holds none). */
uint32_t nvn_texset_format(uint32_t slot);
/** @brief The kind constant value that has a formatted surface access move an E5B9G9R9 image's 32-bit texels
 * unchanged through its raw view, as the console does (the translator's SURFACE_KIND_RGB9E5). */
#define NVN_SURFACE_KIND_RGB9E5 4u
/** @brief Texture ids that also hold storage images (0 when the set could not be made). */
uint32_t nvn_texset_images(void);
/**
 * @brief The layers a sampling view of v sees: the view's count, else those of the image from the view's first one
 * (an image's layers as its host image has them: six for a cube, whose descriptor depth is 1). nv NULL for no view.
 */
uint32_t nvn_texset_view_layers(const vimg *v, const nvn_view *nv);
/**
 * @brief The dimensions (bits of the translator's Dim) a texture of Dim dim, width by height texels and layers array
 * layers is sampled as. A square 2D array of 6 or more layers also has a cube view, and a cube array view when its
 * layers are a multiple of 6: the guest makes cube maps as 2D arrays and samples them as cubes.
 */
uint32_t nvn_texset_fit(uint32_t dim, uint32_t width, uint32_t height, uint32_t layers);
/** @brief Makes the texture set's sampler id hold the sampler registered under it (a default sampler when none is). */
int nvn_texset_sampler(replay *r, uint32_t id);
/**
 * @brief Binds the image an image handle names as a storage image: *slot
 * gets the index the translated program reads it at (what the layer
 * writes for the handle into the program's bank), *kind and *dim as for
 * nvn_texset_bind.
 */
int nvn_texset_image(replay *r, uint32_t handle, uint32_t *slot, uint32_t *kind, uint32_t *dim);
/**
 * @brief Makes the texture set hold what stage s's separate handles (nvnCommandBufferBindSeparateTexture and
 * BindSeparateSampler) name, and puts the slot of each separate texture in place of its id in the stage's host copy of
 * c[2], bank2_host (the program combines the texture word and the sampler word into the handle it samples with, so
 * the combined word carries the slot and the sampler id). A texture that is no longer registered reads as 0. Returns
 * RL_OK, or RL_PENDING after a stop.
 */
int nvn_separate_handles(replay *r, uint32_t s);
/** @brief Frees what draws of slot's batch held (its descriptor pool); called once that batch has finished. */
void nvn_draw_reset(uint32_t slot);
/** @brief Makes slot the one draws take descriptor sets for. */
/** @brief Makes what batch slot slot needs from the draw module (on its first use); false when the device refused. */
bool nvn_draw_slot_ready(uint32_t slot);
void nvn_draw_slot(uint32_t slot);
/** @brief Runs a recorded nvnCommandBufferEvaluateDlss with the player's upscaler (nvn_dlss.c); RL_OK or a pending stop. */
int nvn_dlss_run(replay *r, const nvn_cmd *e);
/** @brief Replays an NVN_CMD_CUDA_LAUNCH of the console DLSS library (nvn_dlss.c). */
int nvn_dlss_launch(replay *r, const nvn_cmd *e);
/** @brief Frees what DLSS evaluations held for slot's batch (called once that batch has finished). */
void nvn_dlss_reset(uint32_t slot);
/** @brief Makes slot the one DLSS evaluations hold their views for. */
void nvn_dlss_slot(uint32_t slot);
/**
 * @brief Runs a draw-state or draw command of nvn_draw.c; RL_OK, RL_PENDING after a stop (the stop is raised and
 * reported already), or NVN_UNHANDLED when it is not one.
 * GPU counters: the driver's ResetCounter writes only a CLEAR_REPORT_VALUE
 * method, so a counter reported before any draw ran since its reset reads 0
 * (the report is the value then the timestamp); a counter no reset covers or
 * a draw has run on stops, since the host device does not count.
 */
int nvn_draw_cmd(replay *r, const nvn_cmd *e);
/** @brief The conservative raster mode (mode, n) that is post-snap rasterization, which the host has. */
/**
 * @brief The value the driver's default-state stream writes to SET_CONSERVATIVE_RASTER_CONTROL before any draw (a
 * constant at ro 0xbb6ec8 of one game's sdk): post-snap triangles, lines and points, uncertainty region 512, bloat 0.
 */
#define NVN_CONSERVATIVE_CONTROL_DEFAULT 0x00030000u
/**
 * @brief SET_VARIABLE_PIXEL_RATE_SHADING_TABLE_SELECT (NVC797 0x02cc) as the driver's default-state stream leaves it
 * (one game's sdk ro 0xbb88b8, an inline write of 1): the palette is picked by the VPRS table index.
 */
#define NVN_SHADING_TABLE_SELECT_DEFAULT 1u
/**
 * @brief Gathers the shading-rate state of a draw into k (nvn_draw.c): on, with the bound image's view in
 * st->shading.view, when an image is bound and viewport 0 has a palette and the host has VK_NV_shading_rate_image.
 */
int nvn_shading_rate_state(replay *r, nvn_gfx *st, uint32_t *on);
/** @brief Binds st's shading-rate image and viewport 0's palette for a draw whose pipeline has it on. */
void nvn_shading_rate_bind(const nvn_gfx *st);
/**
 * @brief True for a palette rate VK_KHR_fragment_shading_rate can give (one invocation per pixel or per 2x1 to 4x4
 * pixels, 5 to 11); cull and the per-sample rates (0 to 4) it cannot.
 */
bool nvn_shading_rate_khr_has(uint8_t rate);
/** @brief Draws of the mesh pipeline (mesh) or the vertex pipeline run in geometry processing mode mode. */
bool nvn_geometry_mode_draws(uint32_t mode, bool mesh);
/** @brief SET_CONSERVATIVE_RASTER_CONTROL after nvnCommandBufferSetConservativeRasterMode(mode, n) on a register holding cur. */
uint32_t nvn_conservative_control(uint32_t cur, uint32_t mode, uint32_t n);
/** @brief The register value rasterizes as the host's conservative rasterization does: both snap modes post-snap and no copy of inner to outer. */
bool nvn_conservative_control_is_post_snap(uint32_t control);
/** @brief One indirect draw record: the GL indirect command NVN reads. */
typedef struct nvn_indirect_draw {
    uint32_t count, instances, first, base_instance;
    int32_t base_vertex;
} nvn_indirect_draw;
/** @brief The bytes between indirect draw records: stride, or the packed record size (20 indexed, 16 not) for 0. */
uint32_t nvn_indirect_stride(bool indexed, int64_t stride);
/** @brief Reads the indirect draw record at rec (indexed: the elements form with a base vertex). */
void nvn_indirect_record(const uint8_t *rec, bool indexed, nvn_indirect_draw *out);
/** @brief Runs one recorded command of r's command set on the host device, as a submit replays it. */
int nvn_vk_cmd(replay *r, const nvn_cmd *e);
/** @brief What nvn_draw_cmd returns for a command it does not run; distinct from RL_OK and RL_PENDING. */
#define NVN_UNHANDLED (-1)
/**
 * @brief Whether the draw about to run is rendered: *run is false after nvnCommandBufferSetRenderEnable(false), and,
 * after a conditional render enable, when the two counters (the 64-bit values 16 bytes apart at its address) are not
 * in the relation it names. The counters are read after the host device finished what was recorded. RL_OK or a pending
 * stop.
 */
int nvn_draw_render_enabled(replay *r, bool *run);
/** @brief NVNdrawPrimitive values a draw takes: triangles, triangle strip, triangle fan and patches. */
enum { NVN_DRAW_TRIANGLES = 4, NVN_DRAW_TRIANGLE_STRIP = 5, NVN_DRAW_TRIANGLE_FAN = 6, NVN_DRAW_PATCHES = 14 };
/**
 * @brief Whether a geometry program runs after a draw of primitive mode: points, lines, line strips, triangles, their
 * strips and fans and the adjacency forms. NVIDIA's geometry stage takes its input primitive from the draw, so the
 * table holds a translation for each (the triangles one under the program's key, the others under nvn_shader_gs_key).
 */
bool nvn_draw_geometry_input_ok(uint32_t mode);
/**
 * @brief The compute program's driver bank c[0] for a dispatch of groups (see nvn_draw.c): its counts of uniform buffers,
 * storage buffers, texture handles and image handles, and the end byte of its table. *img_lo is where the image
 * handles start and *table_end where the table ends; false when the counts do not fit the end byte.
 */
/** @brief The driver bank c[0] of graphics stage s (see nvn_draw.c): its storage buffers where the driver's per-stage table puts them. */
void nvn_graphics_table(const nvn_gfx *st, uint32_t s, uint8_t c0[NVN_GFX_C0_BYTES]);
/** @brief Where a GLSLC control section holds a compute program's shared memory size in bytes. */
#define NVN_CONTROL_SHARED_BYTES 0x64u
/** @brief The translator's specialization constant of the shared memory length in words (SHARED_SPEC). */
#define NVN_SHARED_SPEC 3u

/**
 * @brief The shared memory words of the compute program whose GLSLC control section is at guest address ctl, for the
 * translation's SHARED_SPEC: the size the compiler declared (control section +0x64, the size the console's driver
 * gives the dispatch; read in four games, it covers every constant shared offset each program's guarded accesses
 * reach), rounded up to a word, at least one word. False when it is more than limit bytes, the device's
 * maxComputeSharedMemorySize: the program cannot run with its shared memory there.
 */
/**
 * @brief Records a CUDA kernel launch into the batch: the translation key, its parameter bytes (at c[0] byte
 * NVN_CUDA_PARAMS), the grid and block sizes (c[0] bytes 0 and 0xc as the CUDA ABI has them) and the dynamic shared
 * bytes (added to the kernel's declared static size, nvn_cuda_shared_words). RL_OK or a stop.
 */
int nvn_cuda_dispatch(replay *r, const char *key, const uint8_t *params, uint32_t nparams, const uint32_t grid[3],
                      const uint32_t block[3], uint32_t dynamic_shared);

bool nvn_compute_shared_words(const uint8_t *mem, uint64_t ctl, uint32_t limit, uint32_t *words);

bool nvn_compute_table(const nvn_gfx *st, const uint32_t groups[3], uint32_t nubo, uint32_t nssbo, uint32_t ntex,
                       uint32_t nimg, uint32_t end, uint8_t c0[NVN_BANK2_BYTES], uint32_t *img_lo,
                       uint32_t *table_end);

/**
 * @brief GPU counters (nvnCommandBufferResetCounter and ReportCounter, nvn_counters.c). The timestamp is the guest
 * clock. The others count on the host device: each draw of a counting queue runs inside an occlusion and a pipeline
 * statistics query, and a counter is the sum of its statistic over the draws since its reset. A counter the host has
 * no statistic for, on a draw that feeds it, stops when it is reported.
 */
int nvn_counters_reset(replay *r, uint32_t type);
/** @brief The reports the game has made so far, by counter type (for the call-count log line). */
void nvn_counters_report_counts(uint64_t out[NVN_COUNTERS]);
int nvn_counters_report(replay *r, uint32_t type, uint64_t addr);
/** @brief Before the batch's command buffer ends: writes the timestamp its reports are put on the guest clock by. */
void nvn_counters_close(VkCommandBuffer cb);
/**
 * @brief Reads the GPU's timestamp clock and the host clock the guest clock runs on at one moment
 * (VK_KHR_calibrated_timestamps); false where the host has no such pair.
 */
bool nvn_vk_calibrate(uint64_t *gpu, uint64_t *host_ns);
/** @brief After the batch was submitted to signal timeline value (0 without a timeline): hands its reports on. */
void nvn_counters_submitted(uint64_t value);
/**
 * @brief After the batch's fence signaled: adds its draws' queries to their counters and writes its reports, unless
 * the reports thread already did; returns once they are written.
 */
void nvn_counters_settle(uint32_t slot);
/** @brief Makes slot the one the counters record into (its query pools and events). */
void nvn_counters_slot(uint32_t slot);
/** @brief Timestamp reports the batch being recorded or run holds. */
uint32_t nvn_counters_reports_pending(void);
/**
 * @brief Whether a counter report recorded or in flight, whose completion has not written it yet, writes into guest
 * bytes [guest, guest + size).
 */
bool nvn_counters_report_due(uint64_t guest, uint64_t size);
/**
 * @brief Whether work recorded into the open batch or still in flight may write guest bytes [guest, guest + size)
 * (nvn_vk_mark_write and the write ranges of nvn_vk_pending_write), so a read of them on the CPU must wait.
 */
bool nvn_vk_unsettled(uint64_t guest, uint64_t size);
/** @brief Before a draw's rendering opens: makes room for its queries (submits and reads back a full batch). */
int nvn_counters_prepare(replay *r);
/** @brief Inside the draw's rendering, before the draw: starts its queries; the slot to hand to nvn_counters_end, or UINT32_MAX when the queue is not counting. */
uint32_t nvn_counters_begin(nvn_gfx *st, bool has_geometry, bool has_tessellation);
/** @brief After a draw: nothing; the draw's queries stay open for the next draw of the queue (nvn_counters_close_query). */
void nvn_counters_end(uint32_t slot);
/** @brief Ends the open query pair: before a report or reset is recorded, and where the rendering ends. */
void nvn_counters_close_query(void);

/**
 * @brief Reads v's level 0 back as RGBA8 pixels (rows of 4 * width bytes), for looking at it: RGBA8 images as they
 * are, 4-byte integer formats bit for bit, other color formats converted by a blit (not exact). NULL for depth
 * formats, integer formats of other sizes and failed submits; the caller frees the pixels.
 */
uint8_t *nvn_vk_read_rgba8(replay *r, vimg *v);
/**
 * @brief Checks scissored clears on color image v (RGBA8, at least 8x8): a clear with a scissor opens its pass over the
 * scissor only and leaves the pixels outside it as they were. 0 when both hold, else the failing step's number.
 */
int nvn_vk_clear_check(replay *r, vimg *v);

/** @brief Images of removed pools not freed yet: a batch that may use them has not finished, or state still names them. */
uint32_t nvn_vk_retired_count(void);

/**
 * @brief Drops every slot that holds image v before v is freed: its views made for the slots are destroyed and the
 * slots' descriptors made null. Called under the NVN lock once no batch that may use them is in flight.
 */
void nvn_texset_image_gone(const vimg *v);
/**
 * @brief The draw or dispatch that bound texture slots is now in its batch: they were last used at the current flush epoch.
 * Until then (and for a use no site notes) a slot counts as busy, and rewriting it waits for the device.
 */
void nvn_texset_uses_recorded(void);
/** @brief How many slot rewrites waited for the device and how many did not because no unfinished work used the slot. */
void nvn_texset_rebinds(uint64_t *flushed, uint64_t *free_of_wait);
/** @brief Forgets image v in the trace's capture cache before v is freed. */
void nvn_trace_image_gone(const vimg *v);

/** @brief A piece of the pool table: the GPU address its descriptor starts at, its bytes, buffer and offset in it. */
typedef struct nvn_pool_piece {
    uint64_t gpu, size, off;
    void *buf;
} nvn_pool_piece;
/**
 * @brief The pieces of the n spans sp (at most cap into pc, sorted by address; *dropped those left out): each span cut
 * into pieces of at most range bytes, the first starting at the align boundary at or below the span's first byte in
 * its buffer, so piece k reaches GPU address a at byte a - gpu of its descriptor.
 */
uint32_t nvn_pool_pieces(const nvn_span *sp, uint32_t n, uint64_t range, uint64_t align, nvn_pool_piece *pc,
                         uint32_t cap, uint32_t *dropped);
/** @brief The pool table's set layout (set NVN_POOL_SET), made once; NULL when the device cannot make it. */
VkDescriptorSetLayout nvn_pools_layout(void);
/** @brief An empty set layout, for the set numbers a pipeline layout skips. */
VkDescriptorSetLayout nvn_empty_layout(void);
/**
 * @brief The pool table's descriptor set of batch slot slot, its pieces and table written for the current pools (a
 * change since it was last written rewrites it); NULL when it cannot be made.
 */
VkDescriptorSet nvn_pools_set(uint32_t slot);
/** @brief The pieces of the pool table as written last, and the spans left out of it (no room), for the log. */
void nvn_pools_counts(uint32_t *pieces, uint32_t *spans, uint32_t *dropped);
/** @brief Counts a draw into the open render pass, naming the pass by its first fragment program (gpu.profile). */
void nvn_vk_pass_drew(const char *vs, const char *fs, uint32_t count);
/** @brief With RELIFT_GPU_PROFILE_DRAWS=1 (2 adds the vertex count): a timestamp after a draw, labelled by its programs and the pass's size. */
void nvn_gpuprof_draw(const char *vs, const char *fs, uint32_t w, uint32_t h, uint32_t count);
/**
 * @brief Hardware counters per render pass (nvn_gpucount.c, RELIFT_GPU_COUNTERS): whether they are on, the submit
 * info a batch with counting queries is submitted with (chained in front of next), the batch's queries reset at its
 * start, a pass's query begun before and ended after it (named by fmt), the batch's results read once it ran, and
 * the per-pass sums reported with the profile.
 */
bool nvn_gpucount_on(void);
const void *nvn_gpucount_submit_next(const void *next);
void nvn_gpucount_batch_begin(void);
void nvn_gpucount_pass_begin(void);
void nvn_gpucount_pass_end(const char *fmt, ...) RL_PRINTF(1, 2);
void nvn_gpucount_done(uint32_t s);
void nvn_gpucount_report(double frames);
/** @brief Whether gpu.profile is on (nvn_gpuprof.c); made on first use, with the device up. */
bool nvn_gpuprof_on(void);
/** @brief Starts the profile of the batch g.cb begins (after vkBeginCommandBuffer). */
void nvn_gpuprof_batch_begin(void);
/** @brief Names a kind of command recorded since the last mark, for an interval without a name of its own. */
void nvn_gpuprof_note(const char *kind);
/** @brief Ends an interval with a timestamp, named by fmt. Outside a render pass only. */
void nvn_gpuprof_mark(const char *fmt, ...) RL_PRINTF(1, 2);
/** @brief Ends an interval with a timestamp, named by the kinds noted since the last mark ("other" for none). */
void nvn_gpuprof_mark_kinds(void);
/** @brief Reads slot s's timestamps once its batch finished. */
void nvn_gpuprof_done(uint32_t s);
/** @brief Logs the ranking of the parts since the last report, per present over frames presents. */
void nvn_gpuprof_report(double frames);
/** @brief The batches' spans, the sum of their parts and the time between batches since the last report (ns). */
void nvn_gpuprof_sums(double *span_ns, double *parts_ns, double *between_ns);
/** @brief The GPU time (ns) and intervals counted under label lab since the last report; false when none were. */
bool nvn_gpuprof_total(const char *lab, double *ns, uint64_t *count);
/** @brief Forgets the profile and reads gpu.profile again at the next use (a settings change, tests). */
void nvn_gpuprof_reset(void);
/** @brief Logs the per-present report (timings, GPU profile, batch enders, host Vulkan calls) now, for the presents since the last one. */
void nvn_vk_report_now(uint64_t total);
/**
 * @brief The Switch diagnostic snapshot's step after each present (nvn_snapshot.c), outside the layer's lock: arms the
 * GPU profile and a frame trace when a snapshot window has begun, reports each present of it, and writes the layer's
 * files when it is over. One atomic load when no snapshot runs. total is the number of presents so far.
 */
void nvn_snapshot_step(uint64_t total);
/** @brief Registers the layer's snapshot provider (nvn_snapshot.c); once the device is up. */
void nvn_snapshot_register(void);
/** @brief The snapshot's phase (nvn_snapshot.c): 0 idle, 1 requested, 2 warm-up present (profile on), 3 reporting, 4 done. */
int nvn_snapshot_phase(void);
/** @brief The host's monotonic clock in ns, the one the profile's calibrated GPU times are on. */
int64_t nvn_gpuprof_now(void);
/** @brief The batch in slot s is handed to the queue (or the submit thread) now. */
void nvn_gpuprof_queued(uint32_t s);
/** @brief vkQueueSubmit of slot s's batch is called now (any thread). */
void nvn_gpuprof_entered(uint32_t s);
/** @brief vkQueueSubmit of slot s's batch returned now (any thread). */
void nvn_gpuprof_handed(uint32_t s);
/** @brief What a thread was doing for gpu.profile's split of the time between batches, by priority. */
enum { SPAN_WAIT, SPAN_LOCK, SPAN_CORE, SPAN_BLOCK, SPAN_PACE, SPAN_REPLAY, SPAN_PRESENT, SPAN_KINDS };
/** @brief The calling thread did kind from lo to hi (host clock, nvn_gpuprof_now); kept while gpu.profile is on. */
void nvn_gpuprof_span(int kind, int64_t lo, int64_t hi);
/** @brief A wait for a batch blocked from start_ns to end_ns, at the site nvn_wait_site names. */
void nvn_gpuprof_waited(int64_t start_ns, int64_t end_ns);
#define nvn_vk_flush(r) (nvn_wait_site = NVN_SITE, nvn_vk_flush(r))
#define nvn_vk_submit(r) (nvn_wait_site = NVN_SITE, nvn_vk_submit(r))
/**
 * @brief Counts a batch ended at the site nvn_wait_site names (the flush, submit or wait that asked for it), for the
 * ranking of what splits the work into batches.
 */
void nvn_gpuprof_batch_site(void);
/** @brief The batches counted at site since the last report (tests). */
uint64_t nvn_gpuprof_batches_at(const char *site);
#define nvn_vk_reading_guest(a, n) (nvn_wait_site = NVN_SITE, nvn_vk_reading_guest(a, n))
#define nvn_vk_sync_uniforms(r) (nvn_wait_site = NVN_SITE, nvn_vk_sync_uniforms(r))

#endif
