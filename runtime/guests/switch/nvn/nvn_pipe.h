/**
 * @file nvn_pipe.h
 * @brief Host pipelines of the NVN2 layer: a persistent pipeline cache, and
 * pipelines built from libraries (VK_EXT_graphics_pipeline_library) so that
 * the driver's compile of each stage happens once, ahead of the draw that
 * first needs it, and a draw only links.
 *
 * Draws set the state that varies most from draw to draw (culling, depth and
 * stencil tests, bias, discard) with dynamic state commands, so it is not
 * part of any library. What stays in a key is what the driver compiles in:
 * a stage's module and specialization constants, the vertex input, the
 * render target formats and blends, and the fixed raster state.
 */
#ifndef RELIFT_NVN_PIPE_H
#define RELIFT_NVN_PIPE_H

#include "nvn_vk.h"

#define NVN_MAX_SPECS 32u

/** @brief Everything a graphics pipeline is made from. All of it is zero before a draw fills it in. */
typedef struct nvn_pipe_key {
    VkShaderModule mods[NVN_STAGES];
    /** @brief 1 for a native task and mesh pipeline (no vertex input), 2 for the compute back end's draw of a mesh program (the mesh pass layout). */
    uint32_t mesh_mode;
    uint32_t topology;
    uint32_t nattr, nbind;
    VkVertexInputAttributeDescription attr[NVN_VERTEX_ATTRIBS];
    VkVertexInputBindingDescription bind[NVN_VERTEX_BUFFERS + NVN_VERTEX_ATTRIBS];
    /** @brief Integer attributes, fed expanded (see expand_integer): stream, offset in the stream, format. */
    uint32_t nint;
    struct {
        uint32_t stream, offset;
        VkFormat format;
        uint32_t stride, per_instance;
    } ints[NVN_VERTEX_ATTRIBS];
    uint32_t ncolor;
    VkFormat color[MAX_RT], depth, stencil;
    VkPipelineColorBlendAttachmentState blend[MAX_RT];
    uint32_t cull, front, polygon, bias_enable;
    uint32_t depth_test, depth_write, depth_func, stencil_test;
    VkStencilOpState front_ops, back_ops;
    uint32_t primitive_restart, negative_one_to_one, patch_points, depth_clamp, rasterizer_discard, conservative_raster;
    /** @brief The sample mask's bit for the one sample, the conservative dilate in quarter pixels, the first viewport's swizzle and the provoking vertex. */
    uint32_t sample_mask, conservative_dilate, viewport_swizzle, provoking_last;
    /** @brief Specialization constants of each stage: id and value, sorted by id (nvn_spec_sort). */
    uint32_t nspec[NVN_STAGES];
    VkSpecializationMapEntry spec_map[NVN_STAGES][NVN_MAX_SPECS];
    uint32_t spec[NVN_STAGES][NVN_MAX_SPECS];
} nvn_pipe_key;

/** @brief Sorts n specialization constants by id, drops repeated ids, and lays the data out in order (4 bytes each). */
void nvn_spec_sort(uint32_t *n, VkSpecializationMapEntry *map, uint32_t *vals);

/** @brief The layouts of graphics and compute pipelines; set once, before any pipeline is made. */
void nvn_pipe_set_layouts(VkPipelineLayout graphics, VkPipelineLayout compute);

/** @brief The layout of the compute back end of mesh draws (its passes and its draw); set before any mesh pipeline is made. */
void nvn_pipe_set_mesh_layout(VkPipelineLayout mesh);

/**
 * @brief Brings up the pipeline machinery after the device is made: reads
 * the persistent cache. Call once.
 */
void nvn_pipe_init(void);

/** @brief The bytes of driver cache data the last nvn_pipe_init read from the cache file or nvn_pipe_save wrote. */
uint64_t nvn_pipe_cache_bytes(void);

/** @brief Whether graphics pipelines are linked from libraries on this device. */
bool nvn_pipe_uses_libraries(void);

/**
 * @brief The pipeline for k, made if it is new. Safe from any thread; when
 * another thread is making a library k needs, waits for it instead of making
 * it again. A mesh pipeline (k->mesh_mode set) is made whole. NULL when the
 * driver refuses.
 */
VkPipeline nvn_pipe_graphics(const nvn_pipe_key *k);

/** @brief Makes (or waits for) the pre-raster library of vertex module k->mods[0] with k's specialization constants and fixed state; false without libraries. */
bool nvn_pipe_warm_vertex(const nvn_pipe_key *k);

/** @brief Makes (or waits for) the fragment library of k->mods[1] with k's specialization constants; false without libraries. */
bool nvn_pipe_warm_fragment(const nvn_pipe_key *k);

/** @brief The compute pipeline of module m with n specialization constants (sorted by id). NULL when the driver refuses. */
VkPipeline nvn_pipe_compute(VkShaderModule m, uint32_t n, const VkSpecializationMapEntry *map, const uint32_t *vals);

/** @brief The compute pipeline of mesh pass module m in the mesh pass layout, with n specialization constants (sorted by id). NULL when the driver refuses. */
VkPipeline nvn_pipe_mesh_compute(VkShaderModule m, uint32_t n, const VkSpecializationMapEntry *map, const uint32_t *vals);

/** @brief Sets the dynamic state of graphics pipelines that k's pipeline leaves to the command buffer. */
void nvn_pipe_set_dynamic(VkCommandBuffer cb, const nvn_pipe_key *k);

/**
 * @brief While warming is on, a thread that is not a warm-up worker and
 * needs a pipeline or library nobody has started making waits for the warm-up
 * to end instead of compiling it itself.
 */
void nvn_pipe_warming(bool on);

/** @brief Marks the calling thread as a warm-up worker, which compiles even while warming. */
void nvn_pipe_worker(void);

/**
 * @brief One pipeline the game made, kept so that a later launch can make it
 * ahead of the draw: the programs by key (the base program of each stage; a
 * fragment program's demoted variant is worked out again) and the key with its
 * module handles cleared. A compute pipeline has only keys[5] and its
 * specialization constants.
 */
typedef struct nvn_pipe_rec {
    uint32_t kind;
    char keys[6][65];
    nvn_pipe_key k;
} nvn_pipe_rec;

#define NVN_REC_GRAPHICS 1u
#define NVN_REC_COMPUTE 2u
#define NVN_REC_PROGRAM 3u

/** @brief Remembers a graphics pipeline the game made (on a device that links libraries it adds nothing the libraries do not already hold, so it is kept only without them). */
void nvn_pipe_record_graphics(const char *const keys[5], const nvn_pipe_key *k);

/** @brief Remembers a compute pipeline the game made: its program, and n specialization constants sorted by id. */
void nvn_pipe_record_compute(const char *key, uint32_t n, const VkSpecializationMapEntry *map, const uint32_t *vals);

/**
 * @brief Remembers a program the game registered, with its stage and, for a
 * compute program, workgroup size. True when it is new (not registered in an
 * earlier launch or earlier in this one), which is when it needs warming;
 * programs that warm nothing on this device (not compute, and no libraries)
 * are never new.
 */
bool nvn_pipe_record_program(const char *key, uint32_t stage, const uint32_t wg[3]);

/** @brief The number of pipelines the previous launches recorded (read from the keys file at nvn_pipe_init). */
uint32_t nvn_pipe_recorded(void);

/** @brief Pipeline i of the previous launches' records. */
const nvn_pipe_rec *nvn_pipe_recorded_at(uint32_t i);

/** @brief Tells the machinery that every pipeline the game will start with exists: a compile after this is late, and logged. */
void nvn_pipe_warmed(void);

/** @brief Writes the persistent cache when it holds pipelines made since it was last written. */
void nvn_pipe_save(void);

/** @brief Pipelines the driver compiled, and the time it took (ns), counting those made before and after nvn_pipe_warmed. */
void nvn_pipe_stats(uint64_t *made, uint64_t *made_ns, uint64_t *late, uint64_t *late_ns);

#endif
