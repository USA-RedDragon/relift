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

#define VK(name) VKF_FN(&g.fns, name)

/**
 * @brief Batches recorded or in flight at once: the layer records the next batch into one slot while the host device
 * runs the one before from the other, as the console's CPU builds commands while its GPU runs earlier ones.
 */
#define NVN_SLOTS 2u
#define MAX_RT 8u
/** @brief Graphics, compute, task and mesh stages, in NVNshaderStage order. */
#define NVN_STAGES 8u
/** @brief The viewports a shading-rate palette can be given for. */
#define NVN_SHADING_VIEWPORTS 16u
/** @brief Uniform buffer slots of a stage (c[3] to c[16]). */
#define NVN_UBO_SLOTS 14u
/** @brief Bytes of a stage's driver bank c[2] the layer keeps: bound texture handles from 0x20, 8 bytes apart. */
#define NVN_BANK2_BYTES 0x968u
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
    /** @brief Views of single levels and layers (depth slices of a 3D image), for render targets. */
    struct vsub {
        uint32_t level, layer;
        VkImageView view;
        /** @brief The view's format when it is not the image's, else VK_FORMAT_UNDEFINED. */
        VkFormat format;
    } *sub;
    uint32_t nsub, capsub;
    /** @brief Sampling views by VkImageViewType (1D to cube array), made when first asked for. */
    VkImageView views[7];
    /** @brief The texture set keeps it for a registered texture's id (nvn_texset_sync_registered). */
    bool synced;
} vimg;

/** @brief A render target: one level and layer of an image. */
typedef struct rt {
    vimg *v;
    uint32_t level, layer;
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
    /** @brief VK_EXT_conservative_rasterization is on: nvnCommandBufferSetConservativeRasterEnable is honored. */
    bool conservative_raster, conservative_warned;
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
    /** @brief VK_EXT_device_fault's deviceFault is on: a lost device reports what faulted. */
    bool device_fault;
    /** @brief maxStorageBufferRange and maxDrawIndexedIndexValue of the device. */
    uint32_t max_storage_range, max_index_value;
    /** @brief vkCmdDrawMeshTasksEXT (loaded with the device when the extension is on). */
    void (*draw_mesh_tasks)(VkCommandBuffer, uint32_t, uint32_t, uint32_t);
    /** @brief VK_EXT_external_memory_host is on, with its pointer alignment. */
    bool host_import;
    uint64_t import_align;
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
    /**
     * @brief A timeline semaphore every batch signals with the next value (timeline_value after a submit), so a
     * thread other than the one that submits can wait for a batch without the fence the next submit resets.
     */
    bool timeline_ok;
    /**
     * @brief The texture set's descriptors can be written while a batch that does not use them runs; without it every
     * batch counts as a writer, so replay waits for the one in flight as with one slot.
     */
    bool unused_while_pending;
    VkSemaphore timeline;
    uint64_t timeline_value;
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
/** @brief Brings the host device up if no call has, and makes the pipeline layouts; true when both worked. Safe from any thread. */
bool nvn_vk_ready(void);

/** @brief Draw state a queue keeps across command sets, as the GPU keeps it. */
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
    } shading;
} nvn_gfx;

/** @brief Sets a queue's draw state to the GPU's state before any command (nvn_vk.c). */
void nvn_gfx_init(nvn_gfx *st);

/** @brief One command set being replayed. */
typedef struct replay {
    rl_ctx *c;
    uint8_t *mem;
    const nvn_cmdset *s;
    nvn_gfx *st;
    bool recording;
    uint64_t syncs[64];
    uint32_t nsyncs;
    /** @brief The float result (s0) of the last getter. */
    float fret;
} replay;

/** @brief What a traced draw or dispatch tells the trace beyond the draw state (nvn_trace.c). */
typedef struct nvn_trace_cmd {
    const char *keys[NVN_STAGES];
    uint32_t mode, itype, first, count, groups[3];
    int32_t base;
    bool indexed;
} nvn_trace_cmd;

/** @brief A frame is being traced (RELIFT_NVN_TRACE). */
extern bool nvn_trace_active;
/** @brief Draws so far into four or more render targets at once; the trace's G-buffer trigger counts them. */
extern uint64_t nvn_trace_wide_draws;
/**
 * @brief Parses RELIFT_NVN_TRACE, <dir>:<present>[:<frames>]: the folder, the present the trace starts at and the
 * number of frames (default 1). False for anything else.
 */
bool nvn_trace_parse(const char *spec, char *dir, size_t dirsz, uint64_t *from, uint64_t *frames);
/** @brief At every present: ends the frame being traced and starts the next one when the trace is due. */
void nvn_trace_present(replay *r);
/** @brief Before a draw or dispatch ("draw", "dispatch"): submits the batch and saves what the command will read. */
void nvn_trace_begin(replay *r, const char *kind);
/** @brief A texture or image the command being traced binds, saved as it is before the command. */
void nvn_trace_texture(replay *r, vimg *v, uint32_t id, uint32_t slot, uint32_t kind, uint32_t dim, bool image,
                       const uint8_t view[17]);
/** @brief After the command is recorded: submits it and writes its line and its files. */
void nvn_trace_end(replay *r, const nvn_trace_cmd *c);
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
 * @brief Records an indirect dispatch of the bound compute pipeline whose group counts are the three u32 at ioff in
 * ibuf, read by the GPU when it runs; when c0 is not NULL, a copy of the counts first fills the 12 bytes at c0_off in
 * c0 (the driver table's group counts), behind a barrier.
 */
void nvn_vk_dispatch_indirect(VkCommandBuffer cb, VkBuffer ibuf, uint64_t ioff, VkBuffer c0, uint64_t c0_off);
bool nvn_vk_begin(replay *r);

/**
 * @brief Writes n elements of an integer vertex attribute of format f, read from src stride bytes apart, as four
 * 32-bit words each (missing components 0, 0, 1; signed formats sign-extended) into out (nvn_draw.c).
 */
void nvn_expand_integer(const uint8_t *src, uint64_t stride, VkFormat f, uint64_t n, uint32_t *out);
/**
 * @brief Submits what r recorded and waits for it; the arena is free again
 * after. With nothing recorded, it frees what a command put in the arena
 * before recording (that counts as a submit for nvn_vk_stage).
 */
bool nvn_vk_flush(replay *r);
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
 * @brief Submits what r recorded without waiting for it, from the recording slot: the host device runs it while the
 * guest goes on and the layer records the next batch into the other slot (NVN_SLOTS). The submit first waits for the
 * batch the other slot still holds, and replay waits at nvn_lock only for batches that may write guest memory
 * (nvn_vk_mark_write); a flush, a sync point and nvn_vk_settle wait for every batch. With nothing recorded, as
 * nvn_vk_flush.
 */
bool nvn_vk_submit(replay *r);
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
bool nvn_mirror_sync(replay *r, bool may_flush);
/** @brief Frees the staging of uploads once the batch that used it completed. */
void nvn_mirror_flushed(void);
/**
 * @brief Whether any pool is mirrored. Mirror staging is shared by the batches in flight and freed only once all
 * finished, and every batch counts as one that writes guest memory while mirrors exist.
 */
bool nvn_mirror_any(void);
/** @brief Whether the bytes noted by nvn_mirror_written still wait for a readback. */
bool nvn_mirror_readback_pending(void);
/** @brief Records copies of the next slice of the noted bytes into cb; they land in guest memory at nvn_mirror_apply_readback. */
bool nvn_mirror_record_readback(VkCommandBuffer cb);
/** @brief After the batch completed: writes the readback slice into guest memory, the pages that differ only. */
void nvn_mirror_apply_readback(void);
/** @brief Forgets the noted bytes (the device failed). */
void nvn_mirror_drop_readback(void);
/** @brief Bytes uploaded and read back and uploads made, for the stats line. */
void nvn_mirror_stats(uint64_t *up, uint64_t *down, uint64_t *syncs);
/** @brief Stops the game with a report (after flushing what r recorded). Returns RL_PENDING. */
int nvn_vk_stop(replay *r, const char *fmt, ...) RL_PRINTF(2, 3);
/** @brief The host format of an NVNformat; VK_FORMAT_UNDEFINED for one the layer does not map. */
VkFormat nvn_vk_format(uint32_t f, VkImageAspectFlags *aspect);
/**
 * @brief Begins dynamic rendering on the queue's render targets, or keeps the open pass when it renders to the same
 * attachments; area gets their common extent.
 */
bool nvn_vk_begin_rendering(replay *r, VkRect2D *area);
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
bool nvn_vk_refresh(replay *r, vimg *v);
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
void nvn_draw_slot(uint32_t slot);
/** @brief Runs a recorded nvnCommandBufferEvaluateDlss with the player's upscaler (nvn_dlss.c); RL_OK or a pending stop. */
int nvn_dlss_run(replay *r, const nvn_cmd *e);
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
/** @brief What nvn_draw_cmd returns for a command it does not run; distinct from RL_OK and RL_PENDING. */
#define NVN_UNHANDLED (-1)
/**
 * @brief Whether the draw about to run is rendered: *run is false after nvnCommandBufferSetRenderEnable(false), and,
 * after a conditional render enable, when the two counters (the 64-bit values 16 bytes apart at its address) are not
 * in the relation it names. The counters are read after the host device finished what was recorded. RL_OK or a pending
 * stop.
 */
int nvn_draw_render_enabled(replay *r, bool *run);
/**
 * @brief The compute program's driver bank c[0] for a dispatch of groups (see nvn_draw.c): its counts of uniform buffers,
 * storage buffers, texture handles and image handles, and the end byte of its table. *img_lo is where the image
 * handles start and *table_end where the table ends; false when the counts do not fit the end byte.
 */
/** @brief The driver bank c[0] of graphics stage s (see nvn_draw.c): its storage buffers where the driver's per-stage table puts them. */
void nvn_graphics_table(const nvn_gfx *st, uint32_t s, uint8_t c0[NVN_GFX_C0_BYTES]);
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
/** @brief Before a draw's rendering opens: makes room for its queries (submits and reads back a full batch). */
int nvn_counters_prepare(replay *r);
/** @brief Inside the draw's rendering, before the draw: starts its queries; the slot to hand to nvn_counters_end, or UINT32_MAX when the queue is not counting. */
uint32_t nvn_counters_begin(nvn_gfx *st, bool has_geometry, bool has_tessellation);
void nvn_counters_end(uint32_t slot);

/**
 * @brief Reads v's level 0 back as RGBA8 pixels (rows of 4 * width bytes), for looking at it: RGBA8 images as they
 * are, 4-byte integer formats bit for bit, other color formats converted by a blit (not exact). NULL for depth
 * formats, integer formats of other sizes and failed submits; the caller frees the pixels.
 */
uint8_t *nvn_vk_read_rgba8(replay *r, vimg *v);

/** @brief Images of removed pools not freed yet: a batch that may use them has not finished, or state still names them. */
uint32_t nvn_vk_retired_count(void);

/**
 * @brief Drops every slot that holds image v before v is freed: its views made for the slots are destroyed and the
 * slots' descriptors made null. Called under the NVN lock once no batch that may use them is in flight.
 */
void nvn_texset_image_gone(const vimg *v);
/** @brief Forgets image v in the trace's capture cache before v is freed. */
void nvn_trace_image_gone(const vimg *v);

#endif
