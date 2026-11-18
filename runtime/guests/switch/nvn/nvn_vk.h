/**
 * @file nvn_vk.h
 * @brief What the NVN2 layer's Vulkan files (nvn_vk.c, nvn_draw.c) share:
 * the host device, host images and the replay state of a queue.
 */
#ifndef RELIFT_NVN_VK_H
#define RELIFT_NVN_VK_H

#include "nvn_priv.h"
#include "vkf.h"

#include <pthread.h>

#define VK(name) VKF_FN(&g.fns, name)
#define MAX_RT 8u
#define MAX_IMAGES 4096u
/** @brief Graphics and compute stages, in NVNshaderStage order. */
#define NVN_STAGES 6u
/** @brief Uniform buffer slots of a stage (c[3] to c[16]). */
#define NVN_UBO_SLOTS 14u
/** @brief Bytes of a stage's driver bank c[2] the layer keeps: bound texture handles from 0x20, 8 bytes apart. */
#define NVN_BANK2_BYTES 0x400u
/** @brief Offset in c[2] of the handle bound to texture slot 0 (nvnCommandBufferBindTexture, sdk 0x6022e0). */
#define NVN_BANK2_TEXTURES 0x20u
#define NVN_VERTEX_BUFFERS 16u
/** @brief NVNtextureTarget of a buffer texture: a host 1D image the layer refills from its storage at each use. */
#define NVN_TARGET_BUFFER 10u
/** @brief NVNtextureTarget of a cubemap (six layers) and a cubemap array (depth layers, six per cube). */
#define NVN_TARGET_CUBEMAP 8u
/** @brief NVNtextureTarget of a 3D texture. */
#define NVN_TARGET_3D 2u
#define NVN_TARGET_CUBEMAP_ARRAY 9u
/** @brief The compute stage's NVNshaderStage, and its storage buffer slots. */
#define NVN_STAGE_COMPUTE 5u
#define NVN_STORAGE_SLOTS 16u
/** @brief Image (and compute texture) slots of a stage. */
#define NVN_IMAGE_SLOTS 32u
#define NVN_VERTEX_ATTRIBS 16u

/** @brief A texture's host image. */
typedef struct vimg {
    uint64_t storage;
    nvn_tex_desc d;
    VkFormat format;
    VkImageAspectFlags aspect;
    /** @brief The image was made with storage usage (an uncompressed color format). */
    bool storage_ok;
    VkImageUsageFlags usage;
    /** @brief When the GPU last wrote it (a render target, copy or storage binding), from a running count. */
    uint64_t written;
    /** @brief The hash of its storage when last filled, and the frame (presents + 1) it was last checked in. */
    uint64_t hash, hash_frame;
    /** @brief For a buffer texture in an imported pool: the write stamp last copied into the pool's bytes. */
    uint64_t flushed;
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view;
    /** @brief Views of single levels and layers (depth slices of a 3D image), for render targets. */
    struct vsub {
        uint32_t level, layer;
        VkImageView view;
    } *sub;
    uint32_t nsub, capsub;
    /** @brief Sampling views by VkImageViewType (1D to cube array), made when first asked for. */
    VkImageView views[7];
} vimg;

/** @brief A render target: one level and layer of an image. */
typedef struct rt {
    vimg *v;
    uint32_t level, layer;
    VkImageView view;
} rt;

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
    bool depth_clip_control;
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
    VkCommandBuffer cb;
    VkFence fence;
    VkPhysicalDeviceMemoryProperties memprops;
    char name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE];
    vimg *images[MAX_IMAGES];
    uint32_t nimages;
    /** @brief The running count vimg.written takes its values from. */
    uint64_t write_stamp;
    uint64_t presents;
    /** @brief Guest bytes copied for the commands of the batch being recorded: buffers, indices, constants. */
    staging arena;
    uint64_t arena_used;
    /** @brief The device probes' results, as device.json reports them. */
    nvn_probes probes;
    /**
     * @brief Batches submitted so far. A submit empties the arena and the
     * descriptor pool, so what a command put there before it is gone.
     */
    uint64_t flushes;
} nvn_vkdev;

extern nvn_vkdev g;

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
    /** @brief Compute storage buffers: GPU address and size. */
    struct {
        uint64_t addr, size;
    } storage[NVN_STORAGE_SLOTS];
    /** @brief Image handles bound per stage and slot. */
    uint32_t image[NVN_STAGES][NVN_IMAGE_SLOTS];
    /** @brief Each stage's c[2] as the driver fills it. */
    uint8_t bank2[NVN_STAGES][NVN_BANK2_BYTES];
    struct {
        uint64_t addr, size;
    } ubo[NVN_STAGES][NVN_UBO_SLOTS], vb[NVN_VERTEX_BUFFERS];
    uint32_t nattribs, nstreams;
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
    uint32_t restart_index;
    float polygon_offset[3];
    float blend_color[4];
    float line_width;
    /** @brief Counter types a ResetCounter named, each replayed as nothing (bit n: type n). */
    uint32_t counters_reset;
} nvn_gfx;

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

int32_t nvn_vk_mem_type(uint32_t bits, VkMemoryPropertyFlags want);
bool nvn_vk_staging_new(staging *s, uint64_t size);
void nvn_vk_staging_free(staging *s);
void nvn_vk_barrier(VkCommandBuffer cb);
bool nvn_vk_begin(replay *r);
/**
 * @brief Submits what r recorded and waits for it; the arena is free again
 * after. With nothing recorded, it frees what a command put in the arena
 * before recording (that counts as a submit for nvn_vk_stage).
 */
bool nvn_vk_flush(replay *r);
/**
 * @brief Runs stage(r, arg), which puts one command's data into the batch
 * being recorded (uploads, descriptor sets), before the command records
 * anything. When the batch had to be submitted partway, which drops what
 * stage put before, it runs stage again on the empty batch; when that
 * submits too, the data of what (a draw, a dispatch) does not fit one batch
 * and it stops. Returns RL_OK or a pending stop.
 */
int nvn_vk_stage(replay *r, int (*stage)(replay *r, void *arg), void *arg, const char *what);
/** @brief Stops the game with a report (after flushing what r recorded). Returns RL_PENDING. */
int nvn_vk_stop(replay *r, const char *fmt, ...) RL_PRINTF(2, 3);
/** @brief The host format of an NVNformat; VK_FORMAT_UNDEFINED for one the layer does not map. */
VkFormat nvn_vk_format(uint32_t f, VkImageAspectFlags *aspect);
/** @brief Begins dynamic rendering on the queue's render targets; area gets their common extent. */
bool nvn_vk_begin_rendering(replay *r, VkRect2D *area);
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
vimg *nvn_vk_image(replay *r, uint64_t tex);
/** @brief Refills v from its storage when the CPU may have changed the bytes (nvn_vk.c); false when the refill fails. */
bool nvn_vk_refresh(replay *r, vimg *v);
/**
 * @brief A view of v of type t: over all its levels and the layers t takes
 * with the texture's swizzle (kept with v), or, given nv, with the levels,
 * layers, swizzle and depth-stencil mode of that texture view (the caller's
 * to destroy). NULL on failure.
 */
VkImageView nvn_vk_view(vimg *v, VkImageViewType t, const nvn_view *nv);
/** @brief A new storage view of level level of v as type t in format f (the caller destroys it), or NULL. */
/** @brief Notes that v is written now (it becomes the image the textures sharing its storage copy from). */
void nvn_vk_written(vimg *v);
VkImageView nvn_vk_storage_view(const vimg *v, VkImageViewType t, VkFormat f, uint32_t level);
/**
 * @brief The host format of a view of v as NVNformat format (aspect in
 * *aspect): a color format with v's texel block size and shape, or
 * VK_FORMAT_UNDEFINED.
 */
VkFormat nvn_vk_view_format(const vimg *v, uint32_t format, VkImageAspectFlags *aspect);
/** @brief Copies n host bytes into the arena (zero-filled to pad bytes); offset of the copy in *off. */
bool nvn_vk_upload_bytes(replay *r, const void *src, uint64_t n, uint64_t pad, uint64_t align, uint64_t *off);
/** @brief The layout of the texture set (set 2); VK_NULL_HANDLE when it cannot be made (nvn_texset.c). */
VkDescriptorSetLayout nvn_texset_layout(void);
/** @brief The texture set, to bind as set 2. */
VkDescriptorSet nvn_texset(void);
/**
 * @brief Makes the texture set hold texture handle's texture (in each array
 * its view type fits) and sampler; *kind gets its numeric kind (0 float,
 * 1 signed, 2 unsigned) and *dim its dimension (the translator's Dim).
 * Returns RL_OK, or RL_PENDING after a stop.
 */
int nvn_texset_bind(replay *r, uint32_t handle, uint32_t *kind, uint32_t *dim);
/**
 * @brief Binds the image an image handle names as a storage image: *slot
 * gets the index the translated program reads it at (what the layer
 * writes for the handle into the program's bank), *kind and *dim as for
 * nvn_texset_bind.
 */
int nvn_texset_image(replay *r, uint32_t handle, uint32_t *slot, uint32_t *kind, uint32_t *dim);
/** @brief Frees what draws of the batch just finished held (descriptor sets); called after each wait. */
void nvn_draw_reset(void);
/** @brief Runs a draw-state or draw command of nvn_draw.c; RL_OK, RL_PENDING after a stop, or 1 when it is not one. */
int nvn_draw_cmd(replay *r, const nvn_cmd *e);

#endif
