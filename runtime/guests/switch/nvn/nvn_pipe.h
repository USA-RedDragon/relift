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

/** @brief The VkResult of the calling thread's last pipeline the driver was asked to create (VK_SUCCESS before any). */
int nvn_pipe_last_result(void);

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

/**
 * @brief Remembers a graphics pipeline the game made (on a device that links libraries it adds nothing the libraries
 * do not already hold, so it is kept only without them, or when it has geometry or tessellation programs, which are
 * made whole).
 */
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

/**
 * @brief Tells the machinery that every pipeline the game will start with exists: a compile after this that a draw
 * waits for is late, and logged; the warm-up workers' own compiles (nvn_pipe_screen_quiet) are not.
 */
void nvn_pipe_warmed(void);
/** @brief Sets or clears what nvn_pipe_warmed set, without saving (tests). */
void nvn_pipe_set_warmed(bool warmed);
/**
 * @brief The calling thread makes the warm-up's own items from now on: the compile screen counts those itself, and
 * the compiles of every other thread (a late compile a draw waits for) are queued on it as they start.
 */
void nvn_pipe_screen_quiet(void);

/** @brief Where a vertex attribute's state comes from (nvn_vertex_attrib_source). */
enum { NVN_ATTRIB_BOUND, NVN_ATTRIB_CONSTANT, NVN_ATTRIB_UNKNOWN };

/**
 * @brief The GPU's attribute registers as nvnCommandBufferBindVertexAttribState leaves them: bit i of *state is set
 * when register i holds a state object's word (kept in nvn_gfx.attribs), bit i of *constant when it holds the
 * constant (unfetched) attribute, which reads (0, 0, 0, 1). A bind of n writes registers 0 to n - 1 from its state
 * objects; the driver sends the words in pairs and pads an odd n with the constant attribute (0x3a400040), which
 * lands in register n. Registers past that are not written and keep what an earlier bind left (registers keep their
 * values until written; inferred, as the macro the driver sends the words to was not found).
 */
void nvn_vertex_attribs_bound(uint32_t *state, uint32_t *constant, uint32_t n);

/** @brief Where attribute i reads from given the registers' state and constant masks (nvn_vertex_attribs_bound). */
uint32_t nvn_vertex_attrib_source(uint32_t state, uint32_t constant, uint32_t i);

/**
 * @brief The GPU's vertex stream registers as nvnCommandBufferBindVertexStreamState leaves them: a bind of n writes
 * the stride and divisor registers of streams 0 to n - 1 (bit s of *state set: nvn_gfx.streams[s] holds the state
 * object); streams past that keep what an earlier bind wrote, as the attribute registers do (registers keep their
 * values until written; inferred, like nvn_vertex_attribs_bound).
 */
void nvn_vertex_streams_bound(uint32_t *state, uint32_t n);

/** @brief Whether stream s holds a stride and divisor some bind on the queue wrote (nvn_vertex_streams_bound). */
bool nvn_vertex_stream_known(uint32_t state, uint32_t s);

/** @brief Where a stream's vertex bytes come from (nvn_vertex_stream_bytes). */
enum { NVN_STREAM_UNBOUND, NVN_STREAM_POOL, NVN_STREAM_ZEROS };

/**
 * @brief Where a draw fetches stream bytes from given the buffer address the stream's address register holds (0: no
 * bind on the queue wrote one) and whether that range still lies in a memory pool: a range in a pool is read from it;
 * one whose pool was finalized reads zeros, as the GPU's fetch from a GPU address with no pool mapped behind it does
 * (inferred: the driver maps nothing there, so the fetch reads the zero page the layer stands in for).
 */
uint32_t nvn_vertex_stream_bytes(uint64_t addr, bool mapped);

/**
 * @brief The vertex streams a draw with pipeline key k binds buffers to, in the key's order, into out; returns how
 * many. The streams need not be contiguous: each of the GPU's vertex streams is set apart, so a program may read
 * streams 0 and 2 with nothing bound to 1.
 */
uint32_t nvn_draw_vertex_streams(const nvn_pipe_key *k, uint32_t out[NVN_VERTEX_BUFFERS]);

/** @brief Writes the persistent cache when it holds pipelines made since it was last written. */
void nvn_pipe_save(void);

/**
 * @brief Sets every fetch texture kind constant among the n of map and vals (ids NVN_FETCH_KIND_SPEC onwards) to
 * NVN_KIND_AT_RUN_TIME; true when one held an exact kind. A draw or dispatch whose library or pipeline of exact kinds is
 * not made yet uses the one of these values (the warm-up's, which reads each texture's kind from the texture table)
 * while a worker makes the exact one, so a texture kind no launch saw before costs no compile on the game's thread.
 */
bool nvn_spec_at_run_time(uint32_t n, const VkSpecializationMapEntry *map, uint32_t *vals);
/**
 * @brief The warm-up's library key of stage of program m: the specialization a draw starts from (texture kinds and
 * dimensions as nvn_module_default_spec gives them, the alpha test always passing) and the pre-raster and fragment
 * state of a queue the game has not changed (nvn_gfx_init: every sample, no viewport swizzle, no depth clamp), so the
 * library a first draw links is the one the warm-up made.
 */
struct nvn_module;
void nvn_warm_key(nvn_pipe_key *k, const struct nvn_module *m, uint32_t stage);
/**
 * @brief Whether the keys and cache are due to be written at now (clock ns): something changed, and the last write is
 * a minute old, or a pipeline compiled during play (at late_at, 0 for none) two seconds ago. A saver thread writes
 * them then, off the game's threads, and the process writes them once more when it ends.
 */
bool nvn_pipe_save_due(uint64_t now, uint64_t saved, uint64_t late_at, bool dirty);

/** @brief Pipelines the driver compiled, and the time it took (ns), counting those made before and after nvn_pipe_warmed. */
void nvn_pipe_stats(uint64_t *made, uint64_t *made_ns, uint64_t *late, uint64_t *late_ns);

#endif
