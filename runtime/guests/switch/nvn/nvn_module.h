/**
 * @file nvn_module.h
 * @brief The shader modules of the game's translated programs: made once per
 * program from the build's shader table, found by the program's key.
 */
#ifndef RELIFT_NVN_MODULE_H
#define RELIFT_NVN_MODULE_H

#include "nvn_vk.h"

/** @brief The specialization constant id of a fragment program's alpha test compare function (the translator's ALPHA_FUNC_SPEC). */
#define ALPHA_FUNC_SPEC 0x400u
/** @brief The first specialization constant id of a fetch's texture kind (the translator's TEXTURE_FETCH_KIND_SPEC). */
#define NVN_FETCH_KIND_SPEC 0x500u
/** @brief The value of a fetch kind constant that has the program read the kind at run time (KIND_AT_RUN_TIME). */
#define NVN_KIND_AT_RUN_TIME 3u
/** @brief The alpha test function that always passes (NVNalphaFunc ALWAYS), the one a queue starts with. */
#define NVN_ALPHA_ALWAYS 8u

/** @brief The set and binding of the compute back end's mesh buffer (the translator's GUEST_MEMORY_SET and BINDING). */
#define NVN_MESH_BUFFER_SET 3u
#define NVN_MESH_BUFFER_BINDING 2u

/** @brief A translated program as a shader module, with what its interface uses. */
typedef struct nvn_module {
    char key[65];
    int stage;
    VkShaderModule m;
    /** @brief User banks (bit n: c[3 + n]) and driver banks (bit n: c[n]) the program reads. */
    uint32_t user, driver;
    /** @brief Input locations (vertex attributes) the program reads. */
    uint32_t inputs;
    /** @brief Bindings of set 2 the program declares (bit n: binding n, for n < 32), and whether it has others. */
    uint32_t texture_bindings;
    bool images, other_sets;
    /** @brief The program reads or writes the compute back end's mesh buffer (set NVN_MESH_BUFFER_SET, binding NVN_MESH_BUFFER_BINDING). */
    bool mesh_buffer;
    /** @brief Words of its trace buffer (NVN_IFACE_TRACE); 0 for a translation not in trace mode. */
    uint32_t trace_words;
    /**
     * @brief Storage buffer slots (bit k: slot k of its stage, from its interface records) the program reaches through
     * element k of its stage's slot array (set 1 binding nvn_slot_binding(stage)) instead of a buffer device address.
     */
    uint32_t storage_slots;
    /** @brief It declares its stage's slot array: every element is then bound, to a blank buffer past storage_slots. */
    bool slot_array;
    /** @brief It searches the pool table (set NVN_POOL_SET, nvn_pools.c). */
    bool pools;
    /**
     * @brief A compute program's bases (the translator's compute_base_binding): base j is reached through set 1
     * binding NVN_SLOT_BINDING_BASE + j, a storage buffer over the memory from the address in c[0] at byte
     * cbase[j]; ncbase of them.
     */
    uint16_t cbase[NVN_STORAGE_SLOTS];
    uint32_t ncbase;
    /** @brief It reads the compute base table (set 1 binding NVN_COMPUTE_BASE_TABLE_BINDING). */
    bool cbase_table;
    /** @brief Its translation stores through 64-bit global addresses (NVN_IFACE_GLOBAL_STORE). */
    bool global_store;
    /** @brief Those stores also mark their pages in the written-page bitmap (the record's second word is 1). */
    bool marks_pages;
    /** @brief The program stores to guest memory: nvn_spirv_stores of its SPIR-V. */
    bool stores_guest;
    /** @brief Its translation reads texture handles from registers (NVN_IFACE_REGISTER_HANDLES). */
    bool register_handles;
    /** @brief Output locations the program declares (a vertex program's varyings). */
    uint32_t outputs;
    /** @brief The program writes the Layer or the ViewportIndex built-in (a geometry program's layered output). */
    bool writes_layer, writes_viewport;
    /** @brief The program has the alpha test (ALPHA_FUNC_SPEC), and reads the fixed-state push constants. */
    bool alpha_test, fixed_push;
    /**
     * @brief The transform feedback buffers it captures to (bit i: buffer i, from its NVN_IFACE_XFB records) and
     * each one's stride in bytes.
     */
    uint32_t xfb_mask;
    uint32_t xfb_stride[4];
    /**
     * @brief The memory path (NVN_IFACE_XFB_MEMORY; 0 bytes when it does not take it): the bytes of a record, the
     * records of an input primitive, and each buffer's layout select bytes (0xff: a word left as it is).
     */
    uint32_t xfb_record, xfb_slots;
    uint8_t xfb_select[4][512];
    /** @brief Its interface records (nvn_shader_iface), as many as the translation has. */
    nvn_iface_rec *recs;
    uint32_t nrecs;
    /**
     * @brief Variants by a mask of locations: of a fragment program, with the inputs there demoted to constants
     * (nvn_spirv_demote_inputs); of a vertex program, with outputs added there (nvn_spirv_add_outputs).
     */
    struct {
        uint32_t mask;
        VkShaderModule m;
    } variants[8];
    uint32_t nvariants;
    /** @brief For a fragment program: 0 not checked yet, 1 some input is not a 32-bit float scalar or vector, 2 all are. */
    uint8_t float_inputs;
} nvn_module;


/**
 * @brief The stage (NVNshaderStage order, 0 vertex to 5 compute) of the
 * SPIR-V w (n words) from its entry point's execution model; false when it
 * has none.
 */
bool nvn_spirv_stage(const uint32_t *w, size_t n, int *stage);

/**
 * @brief Whether the SPIR-V w (n words) can write guest memory: a store, a memory copy or an atomic that writes,
 * through a pointer of the PhysicalStorageBuffer, StorageBuffer or Uniform storage class, except into the compute
 * back end's mesh buffer (the layer's own). Image writes go to host images, not guest memory. True when memory runs
 * out, so a program is only ever taken as read-only when it is.
 */
bool nvn_spirv_stores(const uint32_t *w, size_t n);

/**
 * @brief The module of the program with key, made from the build's shader
 * table the first time: its bindings moved for stage (or, when stage is
 * negative, the stage its SPIR-V names), its banks, inputs and outputs read.
 * A program used in two stages (a mesh program of the compute back end is a
 * compute shader) has a module for each.
 * Safe from any thread. NULL, with the reason in why, when the table lacks
 * the program or the driver refuses the module.
 */
nvn_module *nvn_module_get(const char key[65], int stage, char *why, size_t why_n);

/**
 * @brief Appends to the n specialization constants in map and vals the ones a
 * draw of m with no texture bound sets: every handle's numeric kind float and
 * its dimension 2D. The values a draw sets for textures it binds replace them.
 */
void nvn_module_default_spec(const nvn_module *m, uint32_t *n, VkSpecializationMapEntry *map, uint32_t *vals);

/**
 * @brief The last stage before the rasterizer among a draw's programs mods (by NVNshaderStage, 0 vertex to 4
 * tessellation evaluation): the geometry program, else the tessellation evaluation one, else the vertex one (NULL
 * when there is none). The fragment program reads its outputs, and its Layer picks the layer drawn to.
 */
const nvn_module *nvn_module_last_stage(nvn_module *const mods[5]);

/**
 * @brief The fragment module of frag to draw with after vertex program vert:
 * frag itself when the vertex program writes every location frag reads, else
 * a variant of frag with the unwritten inputs demoted to the constants the
 * vertex stage leaves. NULL when it cannot be made.
 */
VkShaderModule nvn_module_fragment_for(const nvn_module *vert, nvn_module *frag);
/**
 * @brief A copy of the SPIR-V w (n words) in which every Input variable at a location in mask is a Private variable
 * initialized to UNWRITTEN_VARYING (see nvn_priv.h), with no location. The host driver needs every fragment input
 * matched by an output of the vertex program; an input the vertex program does not declare reads what the vertex
 * stage leaves in its output buffer, the value the translator gives every generic output component before the
 * program stores it. Only 32-bit float scalars and vectors take it; any other type reads zero. Returns NULL when
 * memory runs out; *out_n is the new length. The input's decorations go with it, and access chains into it point into the Private storage class, so the result
 * is valid SPIR-V.
 */
uint32_t *nvn_spirv_demote_inputs(const uint32_t *w, size_t n, uint32_t mask, size_t *out_n);
/** @brief Whether every Input variable with a location in the SPIR-V w (n words) is a 32-bit float scalar or vector. */
bool nvn_spirv_float_inputs(const uint32_t *w, size_t n);
/**
 * @brief A copy of the SPIR-V w (n words) with one vec4 Output variable at each location in mask, initialized to
 * UNWRITTEN_VARYING (see nvn_priv.h) and never stored, added to the entry point's interface: what a fragment input
 * at that location reads on the console when the vertex program does not write it. NULL when w has no entry point or
 * memory runs out; *out_n is the new length.
 */
uint32_t *nvn_spirv_add_outputs(const uint32_t *w, size_t n, uint32_t mask, size_t *out_n);
/**
 * @brief The vertex module of vert to draw with before fragment program frag: vert itself when it writes every
 * location frag reads, else a variant with the missing outputs added (nvn_spirv_add_outputs), so the fragment program
 * keeps its own module and the library the warm-up made of it. NULL when vert is not a vertex program, an input of
 * frag is not a float, or the variant cannot be made.
 */
VkShaderModule nvn_module_vertex_for(nvn_module *vert, nvn_module *frag);
/**
 * @brief Sets out[0] and out[1] (the vertex and fragment modules) of a draw with programs mods so every fragment input
 * has an output: a vertex program alone before the fragment one gets the outputs it lacks (nvn_module_vertex_for);
 * otherwise, or when that cannot be made, the fragment program gets its inputs demoted (nvn_module_fragment_for).
 * False when neither can be made.
 */
bool nvn_module_link(nvn_module *const mods[5], VkShaderModule out[5]);


/**
 * @brief Before a draw whose last stage before the rasterizer is last: notes its buffer 0 stride and, while a capture
 * is on and last captures, gathers the buffers it binds into *x and marks them written; through the memory path
 * (count vertices of primitive mode), makes the records' scratch. RL_OK or a stop.
 */
int nvn_xfb_draw_prepare(replay *r, const nvn_module *last, uint32_t mode, uint32_t count, nvn_xfb_draw *x);
/**
 * @brief The memory path's packing (nvn_xfb.c): each record of the slots in scratch that its program wrote goes, in
 * slot order, to buffer i at dst[i] + count[i] when it fits in size[i] bytes (the words whose layout select byte is not
 * 0xff), and count[i] grows by the stride.
 */
void nvn_xfb_pack(const nvn_module *m, const uint8_t *scratch, uint32_t slots, uint8_t *dst[4], const uint64_t size[4],
                  uint32_t count[4]);
/** @brief The module of the program bound to stage s (nvn_draw.c); NULL when it cannot be had (the replay stopped). */
const nvn_module *nvn_draw_module(replay *r, uint32_t s);
#endif
