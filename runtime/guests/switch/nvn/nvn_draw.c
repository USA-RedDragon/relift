/**
 * @file nvn_draw.c
 * @brief NVN2 draws on the host device: the state commands a queue keeps,
 * programs from their SPIR-V translations, pipelines and the draws.
 *
 * Guest buffers a draw reads (vertices, indices, uniform buffers) are copied
 * into the upload arena when the draw is replayed, which is when the GPU
 * would read them. The state objects are read through the SDK driver's own
 * getters, so the layer depends on no bit layout of them.
 *
 * The translations bind each stage's constant bank c[3 + n] as (set 0,
 * binding n) and the driver banks c[n] as (set 1, binding n). A pipeline
 * holds several stages, so the layer moves stage s's user banks to set 0
 * binding 16 s + n and its driver banks to set 1 binding 4 s + n. Of the
 * driver banks, c[1] holds the program's compiler constants and c[2] the
 * texture handles nvnCommandBufferBindTexture binds; set 2 is the texture
 * set (nvn_texset.c), and the numeric kind and dimension of each bound
 * handle's texture go to the specialization constants the translation's
 * interface names.
 *
 * Fixed-function state the command buffer sets reaches the host three ways.
 * The sample mask (bit 0), the first viewport's swizzle (VK_NV_viewport_swizzle),
 * the provoking vertex (VK_EXT_provoking_vertex) and the conservative dilate are
 * in the pipeline key. The point size and the alpha reference are push constants
 * every draw sets (the translations of a vertex program that stores no point
 * size and of a fragment program that writes render target 0's alpha read
 * them), and the alpha test's compare function, which the bound color state
 * holds, is the specialization constant ALPHA_FUNC_SPEC of such a fragment
 * program. Render enable skips draws, and a conditional one reads its counters
 * when the draw replays. The tessellation levels are kept for the control stage
 * a draw without one writes (draws with tessellation stop until they are
 * supported). Each optional extension a setter needs, when the device has none,
 * logs one warning and draws without the effect, except a swizzle that is not
 * the identity, which stops.
 */
#include "nvn_module.h"
#include "nvn_pipe.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#define UBO_BYTES 65536u
#define SET_POOL_SETS 4096u
/** @brief The user binding of constant bank c[24]: graphics push constant slot 0 (nvnCommandBufferPushGraphicsConstants). */
#define PUSH_BINDING (NVN_GFX_PUSH_BANK - 3u)
#define MAX_SPECS NVN_MAX_SPECS
/**
 * @brief Byte offset in driver bank c[0] of a vertex program's base instance: the program adds it to the zero-based
 * instance id the hardware gives (the translated vertex program of a game that draws its glyph quads reads it as
 * c[0] word 0x191 and indexes its instance records with the sum), so it holds the draw's first instance.
 */
#define NVN_C0_BASE_INSTANCE 0x644u
/** @brief Byte offset in driver bank c[0] of the first task of a mesh draw (the translator's MESH_FIRST_TASK_OFFSET). */
#define MESH_FIRST_TASK_OFFSET 0x1f4u
/** @brief The byte of a GLSLC control section that names the program's stage (NVNshaderStage). */
#define GLSLC_STAGE_BYTE 0x56u
/** @brief Words of the mesh buffer's header and the offsets in it the translator's passes read and write (H_*). */
#define MESH_HEADER_WORDS 32u
#define MESH_H_TASKS 0u
#define MESH_H_MESHES 1u
#define MESH_H_OVERFLOW 3u
#define MESH_H_DISPATCH 4u
#define MESH_H_DRAW 8u
#define MESH_H_CAP 12u
#define MESH_H_TASK_RECS 13u
#define MESH_H_TASK_SCAN 14u
#define MESH_H_MESH_RECS 15u
#define MESH_H_INDEXES 16u
#define MESH_H_DRAW_INDEXED 17u
/** @brief Bytes of the mesh buffer, and the draws of a batch whose overflow flag is kept. */
#define MESH_BUFFER_BYTES (128ull << 20)
#define MESH_STATUS_SLOTS 4096u
#define MESH_DISPATCH_X 65535u
/**
 * @brief The graphics stages that may read the fixed-state push constants: the point size and the alpha reference, and
 * in any stage the written-page bitmap's address.
 */
#define FIXED_STAGES                                                                                                   \
    (VK_SHADER_STAGE_ALL_GRAPHICS | (g.draw_mesh_tasks ? VK_SHADER_STAGE_TASK_BIT_EXT | VK_SHADER_STAGE_MESH_BIT_EXT : 0))
/**
 * @brief The fixed-state push constant block (the translator's: point size, then alpha reference, then the address of
 * the written-page bitmap, PUSH_WRITTEN_PAGES); compute programs read only the address.
 */
typedef struct fixed_push {
    float point_size, alpha_ref;
    /** @brief The vertex attributes (bit per location) bound with an unsigned or a signed scaled format. */
    uint32_t scaled_unsigned, scaled_signed;
    uint64_t written_pages;
    /** @brief The device address of a memory path capture's records (nvn_xfb.c), at PUSH_XFB_SCRATCH. */
    uint64_t xfb_scratch;
} fixed_push;

typedef nvn_module module;

typedef nvn_pipe_key pipe_key;

static struct {
    VkDescriptorSetLayout set_layout[5], cset_layout[5];
    VkPipelineLayout layout, clayout, mlayout;
    /** @brief The compute back end of mesh draws: descriptor set layouts, the buffer all its passes share and its set. */
    VkDescriptorSetLayout mset_layout[5];
    VkBuffer mbuf;
    VkDeviceMemory mmem;
    /**
     * @brief GPU trace (RELIFT_NVN_GPU_TRACE): the compute layout's set 3 with the trace buffer, the buffer (host
     * visible, read after each traced dispatch) and its set; trace_on once the layout has the set.
     */
    VkDescriptorSetLayout tset_layout;
    VkDescriptorPool tpool;
    VkDescriptorSet tset;
    staging tbuf;
    bool trace_on;
    /** @brief The present and the draw or dispatch index in it of the last command (the trace's command index). */
    uint64_t cmd_present, cmd_index;
    /** @brief The program keys of the last draw or dispatch recorded (nvn_draw_last), for a device loss report. */
    char last[512];
    /** @brief A 16-byte buffer for the storage slots a program never reaches (blank_buffer). */
    VkBuffer blank;
    VkDeviceMemory blank_mem;
    VkDescriptorPool mpool;
    VkDescriptorSet mset;
    /**
     * @brief The overflow word of each draw's mesh buffer in the batch being recorded, copied out for the host; each
     * slot has its own (mstatus_of), and mstatus is the recording slot's.
     */
    staging mstatus;
    uint32_t mstatus_n;
    staging mstatus_of[NVN_SLOTS];
    uint32_t mstatus_n_of[NVN_SLOTS];
    /** @brief The descriptor pool of each slot's batch; pool is the recording slot's. */
    VkDescriptorPool pools[NVN_SLOTS];
    uint32_t slot;
    bool moverflow;
    bool mready;
    /** @brief Storage buffer slots per task and mesh program in the mesh layout, where both are compute stages. */
    uint32_t mesh_slots;
    /** @brief Uniform buffer slots (c[3] on) per task and mesh program in the mesh layout, for the same reason. */
    uint32_t mesh_ubos;
    VkDescriptorPool pool;
    VkBuffer zero;
    bool ready;
} d;

static module *module_of(replay *r, uint64_t code, int stage)
{
    char key[65];
    uint64_t len;
    if (!nvn_shader_key(r->mem, code, key, &len)) {
        nvn_vk_stop(r, "the program at 0x%llx has no end", (unsigned long long)code);
        return NULL;
    }
    if (nvn_vk_pending_write(code, len)) {
        if (!nvn_vk_reading_guest(code, len) || !nvn_shader_key(r->mem, code, key, &len)) {
            nvn_vk_stop(r, "the program at 0x%llx has no end", (unsigned long long)code);
            return NULL;
        }
    }
    char why[800];
    module *m = nvn_module_get(key, stage, why, sizeof why);
    if (!m) {
        nvn_shader_note(r->mem, code, key);
        nvn_vk_stop(r,
                    "program %s (at 0x%llx): %s; a program missing from the table is recorded in %s/programs for the next "
                    "relift build (%s/failed.txt names programs the translator refused)",
                    key, (unsigned long long)code, why, nvn_shader_dir(), nvn_shader_dir());
    } else if (m->global_store) {
        nvn_vk_mark_write();
    }
    return m;
}

/** @brief The helper module of kind ("scan" or "vertex") of the compute back end for mesh module mesh. */
static module *module_helper(replay *r, const char *kind, const module *mesh, int stage)
{
    char key[65], why[200];
    nvn_shader_helper_key(kind, mesh->key, key);
    module *m = nvn_module_get(key, stage, why, sizeof why);
    if (!m)
        nvn_vk_stop(r, "the %s helper of mesh program %s: %s", kind, mesh->key, why);
    else if (m->global_store)
        nvn_vk_mark_write();
    return m;
}

static bool slot_pool(uint32_t i);
static bool slot_mstatus(uint32_t i);

/** @brief Binds the batch slot's pool table (set NVN_POOL_SET) where the layout has it. */
static void bind_pools(VkPipelineBindPoint at, VkPipelineLayout layout, VkDescriptorSetLayout has)
{
    VkDescriptorSet s = has ? nvn_pools_set(d.slot) : NULL;
    if (s)
        VK(vkCmdBindDescriptorSets)(g.cb, at, layout, NVN_POOL_SET, 1, &s, 0, NULL);
}

static bool layouts(void)
{
    if (d.ready)
        return true;
    VkDescriptorSetLayoutBinding ub[USER_BINDINGS * NVN_STAGES], db[(DRIVER_BINDINGS + NVN_STORAGE_SLOTS) * NVN_STAGES];
    static const VkShaderStageFlags stage_bits[NVN_STAGES] = { VK_SHADER_STAGE_VERTEX_BIT,
                                                               VK_SHADER_STAGE_FRAGMENT_BIT,
                                                               VK_SHADER_STAGE_GEOMETRY_BIT,
                                                               VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                               VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                                               0,
                                                               VK_SHADER_STAGE_TASK_BIT_EXT,
                                                               VK_SHADER_STAGE_MESH_BIT_EXT };
    uint32_t nu = 0, nd = 0;
    for (uint32_t s = 0; s < NVN_STAGES; s++) {
        if (!stage_bits[s] || (s >= NVN_STAGE_TASK && !g.mesh_shader))
            continue;
        for (uint32_t b = 0; b < USER_BINDINGS; b++)
            ub[nu++] = (VkDescriptorSetLayoutBinding){ USER_BINDINGS * s + b, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                       stage_bits[s], NULL };
        for (uint32_t b = 0; b < DRIVER_BINDINGS; b++)
            db[nd++] = (VkDescriptorSetLayoutBinding){ DRIVER_BINDINGS * s + b, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                       stage_bits[s], NULL };
    }
    for (uint32_t s = 0; s < NVN_STAGES; s++) {
        if (!stage_bits[s] || (s >= NVN_STAGE_TASK && !g.mesh_shader))
            continue;
        db[nd++] = (VkDescriptorSetLayoutBinding){ nvn_slot_binding(s), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NVN_STORAGE_SLOTS,
                                                       stage_bits[s], NULL };
    }
    VkDescriptorSetLayoutCreateInfo li[2] = {
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nu, .pBindings = ub },
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nd, .pBindings = db },
    };
    for (int i = 0; i < 2; i++)
        if (VK(vkCreateDescriptorSetLayout)(g.dev, &li[i], NULL, &d.set_layout[i]) != VK_SUCCESS)
            return false;
    if (!(d.set_layout[2] = nvn_texset_layout()))
        return false;
    const char *gpu_trace = getenv("RELIFT_NVN_GPU_TRACE");
    if (gpu_trace && *gpu_trace && strcmp(gpu_trace, "0")) {
        VkDescriptorSetLayoutBinding tb = { NVN_TRACE_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                            VK_SHADER_STAGE_ALL, NULL };
        VkDescriptorSetLayoutCreateInfo ti = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                               .bindingCount = 1,
                                               .pBindings = &tb };
        if (VK(vkCreateDescriptorSetLayout)(g.dev, &ti, NULL, &d.tset_layout) != VK_SUCCESS)
            return false;
        d.trace_on = true;
    }
    /* set 3: the trace buffer in a GPU trace, else the empty set before the pools */
    d.set_layout[3] = d.trace_on ? d.tset_layout : nvn_empty_layout();
    d.set_layout[4] = nvn_pools_layout();
    VkPushConstantRange fixed = { FIXED_STAGES, 0, sizeof(fixed_push) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = d.set_layout[3] && d.set_layout[4] ? 5 : d.trace_on ? 4 : 3,
                                       .pSetLayouts = d.set_layout,
                                       .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &fixed };
    if (VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &d.layout) != VK_SUCCESS)
        return false;
    if (!slot_pool(d.slot))
        return false;
    d.pool = d.pools[d.slot];
    d.ready = true;
    nvn_pipe_set_layouts(d.layout, d.clayout);
    return true;
}

/** @brief Makes batch slot i's descriptor pool, the first time the slot is used. */
static bool slot_pool(uint32_t i)
{
    if (d.pools[i])
        return true;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SET_POOL_SETS * 8 },
                                   { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, SET_POOL_SETS / 2 * NVN_STORAGE_SLOTS * NVN_STAGES } };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = SET_POOL_SETS,
                                       .poolSizeCount = 2,
                                       .pPoolSizes = ps };
    if (VK(vkCreateDescriptorPool)(g.dev, &pci, NULL, &d.pools[i]) != VK_SUCCESS) {
        d.pools[i] = NULL;
        return false;
    }
    return true;
}

/** @brief Makes batch slot i's mesh status buffer, the first time the slot is used. */
static bool slot_mstatus(uint32_t i)
{
    return d.mstatus_of[i].buf || nvn_vk_staging_new(&d.mstatus_of[i], 16ull * MESH_STATUS_SLOTS);
}

/**
 * @brief The layouts and buffers of the compute back end of mesh draws: sets 0 and 1 hold the constant banks of the
 * fragment program (stage 1) and of the task and mesh programs (stages 6 and 7, visible to compute), set 2 the
 * textures, set 3 the mesh buffer (binding 2) the passes and the draw's vertex program share.
 */
static bool mesh_layouts(void)
{
    if (d.mready)
        return true;
    if (!layouts())
        return false;
    VkDescriptorSetLayoutBinding ub[USER_BINDINGS * 3], db[(DRIVER_BINDINGS + NVN_STORAGE_SLOTS) * 3];
    static const uint32_t groups[3] = { 1, NVN_STAGE_TASK, NVN_STAGE_MESH };
    static const VkShaderStageFlags bits[3] = { VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_COMPUTE_BIT,
                                                VK_SHADER_STAGE_COMPUTE_BIT };
    /* task and mesh are both compute stages here: half of the per-stage storage buffers each, less the mesh buffer */
    uint32_t half = g.max_stage_storage > 1 ? (g.max_stage_storage - 1) / 2 : 0;
    d.mesh_slots = half < NVN_STORAGE_SLOTS ? half : NVN_STORAGE_SLOTS;
    /* and half of the per-stage uniform buffers each: the driver banks, the push bank, then the slots that fit */
    uint32_t uhalf = g.max_stage_uniform / 2, ufixed = DRIVER_BINDINGS + 1;
    d.mesh_ubos = uhalf <= ufixed ? 0 : uhalf - ufixed < NVN_UBO_SLOTS ? uhalf - ufixed : NVN_UBO_SLOTS;
    uint32_t nd = DRIVER_BINDINGS * 3, nu = 0;
    for (uint32_t i = 0; i < 3; i++) {
        for (uint32_t b = 0; b < USER_BINDINGS; b++)
            if (!i || b < d.mesh_ubos || b == PUSH_BINDING)
                ub[nu++] = (VkDescriptorSetLayoutBinding){ USER_BINDINGS * groups[i] + b, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                           bits[i], NULL };
        for (uint32_t b = 0; b < DRIVER_BINDINGS; b++)
            db[i * DRIVER_BINDINGS + b] = (VkDescriptorSetLayoutBinding){ DRIVER_BINDINGS * groups[i] + b,
                                                                          VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, bits[i], NULL };
        if (!i || d.mesh_slots == NVN_STORAGE_SLOTS)
            db[nd++] = (VkDescriptorSetLayoutBinding){ nvn_slot_binding(groups[i]), VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                       NVN_STORAGE_SLOTS, bits[i], NULL };
    }
    VkDescriptorSetLayoutBinding mb = { NVN_MESH_BUFFER_BINDING, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                                        VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo li[3] = {
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nu, .pBindings = ub },
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = nd, .pBindings = db },
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = 1, .pBindings = &mb },
    };
    if (VK(vkCreateDescriptorSetLayout)(g.dev, &li[0], NULL, &d.mset_layout[0]) != VK_SUCCESS ||
        VK(vkCreateDescriptorSetLayout)(g.dev, &li[1], NULL, &d.mset_layout[1]) != VK_SUCCESS ||
        VK(vkCreateDescriptorSetLayout)(g.dev, &li[2], NULL, &d.mset_layout[3]) != VK_SUCCESS)
        return false;
    d.mset_layout[2] = d.set_layout[2];
    d.mset_layout[4] = d.set_layout[4];
    VkPushConstantRange fixed[2] = { { FIXED_STAGES, 0, sizeof(fixed_push) },
                                     { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fixed_push) } };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = d.mset_layout[4] ? 5 : 4,
                                       .pSetLayouts = d.mset_layout,
                                       .pushConstantRangeCount = 2,
                                       .pPushConstantRanges = fixed };
    if (VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &d.mlayout) != VK_SUCCESS)
        return false;
    nvn_pipe_set_mesh_layout(d.mlayout);
    uint64_t bytes = MESH_BUFFER_BYTES;
    if (bytes > g.max_storage_range)
        bytes = g.max_storage_range & ~3u;
    if (bytes > (uint64_t)g.max_index_value * 4)
        bytes = (uint64_t)g.max_index_value * 4;
    if (!nvn_vk_gpu_buffer(&d.mbuf, &d.mmem, bytes))
        return false;
    if (!slot_mstatus(d.slot))
        return false;
    d.mstatus = d.mstatus_of[d.slot];
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1,
                                       .poolSizeCount = 1,
                                       .pPoolSizes = &ps };
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &d.mset_layout[3] };
    if (VK(vkCreateDescriptorPool)(g.dev, &pci, NULL, &d.mpool) != VK_SUCCESS)
        return false;
    ai.descriptorPool = d.mpool;
    if (VK(vkAllocateDescriptorSets)(g.dev, &ai, &d.mset) != VK_SUCCESS)
        return false;
    VkDescriptorBufferInfo bi = { d.mbuf, 0, bytes };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = d.mset,
                                .dstBinding = NVN_MESH_BUFFER_BINDING,
                                .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                .pBufferInfo = &bi };
    VK(vkUpdateDescriptorSets)(g.dev, 1, &wr, 0, NULL);
    d.mready = true;
    return true;
}

/** @brief Reads what the draws of the batch that just finished left in their status words: a mesh buffer that overflowed. */
static void mesh_flushed(uint32_t slot)
{
    const uint32_t *w = (const uint32_t *)(const void *)d.mstatus_of[slot].map;
    uint32_t n = slot == d.slot ? d.mstatus_n : d.mstatus_n_of[slot];
    for (uint32_t i = 0; w && i < n; i++)
        if (w[4 * i + 2]) {
            rl_log(RL_LOG_ERROR, "nvn", "a mesh draw made %u mesh workgroups, more than the %llu MiB mesh buffer holds",
                   w[4 * i], (unsigned long long)(MESH_BUFFER_BYTES >> 20));
            d.moverflow = true;
        }
    if (slot == d.slot)
        d.mstatus_n = 0;
    else
        d.mstatus_n_of[slot] = 0;
}

void nvn_draw_reset(uint32_t slot)
{
    if (d.mready)
        mesh_flushed(slot);
    if (d.pools[slot])
        VK(vkResetDescriptorPool)(g.dev, d.pools[slot], 0);
}

bool nvn_draw_slot_ready(uint32_t slot)
{
    return (!d.ready || slot_pool(slot)) && (!d.mready || slot_mstatus(slot));
}

void nvn_draw_slot(uint32_t slot)
{
    d.mstatus_n_of[d.slot] = d.mstatus_n;
    d.slot = slot;
    d.pool = d.pools[slot];
    d.mstatus = d.mstatus_of[slot];
    d.mstatus_n = d.mstatus_n_of[slot];
}

/** @brief NVNdrawPrimitive to a topology; false for one Vulkan has none of. */
static bool topology(uint32_t mode, VkPrimitiveTopology *t)
{
    static const int map[] = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST,
                               VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                               -1,
                               VK_PRIMITIVE_TOPOLOGY_LINE_STRIP,
                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP,
                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN,
                               -1,
                               -1,
                               -1,
                               VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY,
                               VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY,
                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY,
                               VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY,
                               VK_PRIMITIVE_TOPOLOGY_PATCH_LIST };
    if (mode >= sizeof map / sizeof map[0] || map[mode] < 0)
        return false;
    *t = (VkPrimitiveTopology)map[mode];
    return true;
}

/**
 * @brief A vertex format: the color formats, the three-component ones, and the formats that convert integers to
 * floats (NVN_FORMAT_*_UI2F and *_I2F, 0x51 to 0x68 and 0x6b, 0x6c) and the signed normalized 10-bit packed one (0x69),
 * which only vertex attributes take. The 32-bit component conversions and the signed integer 10-bit one have no host vertex format.
 */
static VkFormat vertex_format(uint32_t f)
{
    static const VkFormat three[] = {
        VK_FORMAT_R8G8B8_UNORM,   VK_FORMAT_R8G8B8_SNORM,    VK_FORMAT_R8G8B8_UINT,     VK_FORMAT_R8G8B8_SINT,
        VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT,
        VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT,  VK_FORMAT_R32G32B32_SINT,
    };
    if (f >= 25 && f <= 36)
        return three[f - 25];
    static const VkFormat converted[] = {
        VK_FORMAT_R8_USCALED,           VK_FORMAT_R8_SSCALED,           VK_FORMAT_R16_USCALED,
        VK_FORMAT_R16_SSCALED,          VK_FORMAT_UNDEFINED,            VK_FORMAT_UNDEFINED,
        VK_FORMAT_R8G8_USCALED,         VK_FORMAT_R8G8_SSCALED,         VK_FORMAT_R16G16_USCALED,
        VK_FORMAT_R16G16_SSCALED,       VK_FORMAT_UNDEFINED,            VK_FORMAT_UNDEFINED,
        VK_FORMAT_R8G8B8_USCALED,       VK_FORMAT_R8G8B8_SSCALED,       VK_FORMAT_R16G16B16_USCALED,
        VK_FORMAT_R16G16B16_SSCALED,    VK_FORMAT_UNDEFINED,            VK_FORMAT_UNDEFINED,
        VK_FORMAT_R8G8B8A8_USCALED,     VK_FORMAT_R8G8B8A8_SSCALED,     VK_FORMAT_R16G16B16A16_USCALED,
        VK_FORMAT_R16G16B16A16_SSCALED, VK_FORMAT_UNDEFINED,            VK_FORMAT_UNDEFINED,
        VK_FORMAT_A2B10G10R10_SNORM_PACK32, VK_FORMAT_UNDEFINED, VK_FORMAT_A2B10G10R10_USCALED_PACK32,
        VK_FORMAT_A2B10G10R10_SSCALED_PACK32,
    };
    if (f >= 0x51 && f <= 0x6C)
        return converted[f - 0x51];
    VkImageAspectFlags a;
    return nvn_vk_format(f, &a);
}

/**
 * @brief The scaled format of the same layout as integer format f (UINT to USCALED, SINT to SSCALED; 8- and 16-bit
 * components only, whose every value a float holds exactly), with *sign when signed; VK_FORMAT_UNDEFINED for one without
 * or one the host device does not read vertices of. A float input over a scaled format reads each component's integer
 * value, which the vertex program turns back into the integer (the fixed-state push constants say which attributes).
 */
static VkFormat scaled_format(VkFormat f, bool *sign)
{
    VkFormat s;
    switch (f) {
    case VK_FORMAT_R8_UINT: s = VK_FORMAT_R8_USCALED; break;
    case VK_FORMAT_R8_SINT: s = VK_FORMAT_R8_SSCALED; break;
    case VK_FORMAT_R8G8_UINT: s = VK_FORMAT_R8G8_USCALED; break;
    case VK_FORMAT_R8G8_SINT: s = VK_FORMAT_R8G8_SSCALED; break;
    case VK_FORMAT_R8G8B8_UINT: s = VK_FORMAT_R8G8B8_USCALED; break;
    case VK_FORMAT_R8G8B8_SINT: s = VK_FORMAT_R8G8B8_SSCALED; break;
    case VK_FORMAT_R8G8B8A8_UINT: s = VK_FORMAT_R8G8B8A8_USCALED; break;
    case VK_FORMAT_R8G8B8A8_SINT: s = VK_FORMAT_R8G8B8A8_SSCALED; break;
    case VK_FORMAT_R16_UINT: s = VK_FORMAT_R16_USCALED; break;
    case VK_FORMAT_R16_SINT: s = VK_FORMAT_R16_SSCALED; break;
    case VK_FORMAT_R16G16_UINT: s = VK_FORMAT_R16G16_USCALED; break;
    case VK_FORMAT_R16G16_SINT: s = VK_FORMAT_R16G16_SSCALED; break;
    case VK_FORMAT_R16G16B16_UINT: s = VK_FORMAT_R16G16B16_USCALED; break;
    case VK_FORMAT_R16G16B16_SINT: s = VK_FORMAT_R16G16B16_SSCALED; break;
    case VK_FORMAT_R16G16B16A16_UINT: s = VK_FORMAT_R16G16B16A16_USCALED; break;
    case VK_FORMAT_R16G16B16A16_SINT: s = VK_FORMAT_R16G16B16A16_SSCALED; break;
    default: return VK_FORMAT_UNDEFINED;
    }
    *sign = f == VK_FORMAT_R8_SINT || f == VK_FORMAT_R8G8_SINT || f == VK_FORMAT_R8G8B8_SINT ||
            f == VK_FORMAT_R8G8B8A8_SINT || f == VK_FORMAT_R16_SINT || f == VK_FORMAT_R16G16_SINT ||
            f == VK_FORMAT_R16G16B16_SINT || f == VK_FORMAT_R16G16B16A16_SINT;
    static int8_t known[VK_FORMAT_R16G16B16A16_SSCALED + 1];
    if (!known[s]) {
        VkFormatProperties fp;
        VK(vkGetPhysicalDeviceFormatProperties)(g.phys, s, &fp);
        known[s] = fp.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT ? 1 : -1;
    }
    return known[s] > 0 ? s : VK_FORMAT_UNDEFINED;
}

static bool integer_format(VkFormat f)
{
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT:
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT:
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT:
    case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT: case VK_FORMAT_R16G16B16_UINT:
    case VK_FORMAT_R16G16B16_SINT: case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT:
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: case VK_FORMAT_R16G16B16A16_UINT:
    case VK_FORMAT_R16G16B16A16_SINT: case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT:
    case VK_FORMAT_A2B10G10R10_UINT_PACK32:
        return true;
    default:
        return false;
    }
}

/** @brief Components, bytes per component and signedness of an integer vertex format; false for others. */
static bool integer_layout(VkFormat f, uint32_t *n, uint32_t *bytes, bool *sign)
{
    switch (f) {
    case VK_FORMAT_R8_UINT: case VK_FORMAT_R8_SINT: *n = 1, *bytes = 1; break;
    case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8_SINT: *n = 2, *bytes = 1; break;
    case VK_FORMAT_R8G8B8_UINT: case VK_FORMAT_R8G8B8_SINT: *n = 3, *bytes = 1; break;
    case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_R8G8B8A8_SINT: *n = 4, *bytes = 1; break;
    case VK_FORMAT_R16_UINT: case VK_FORMAT_R16_SINT: *n = 1, *bytes = 2; break;
    case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16_SINT: *n = 2, *bytes = 2; break;
    case VK_FORMAT_R16G16B16_UINT: case VK_FORMAT_R16G16B16_SINT: *n = 3, *bytes = 2; break;
    case VK_FORMAT_R16G16B16A16_UINT: case VK_FORMAT_R16G16B16A16_SINT: *n = 4, *bytes = 2; break;
    case VK_FORMAT_R32_UINT: case VK_FORMAT_R32_SINT: *n = 1, *bytes = 4; break;
    case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32_SINT: *n = 2, *bytes = 4; break;
    case VK_FORMAT_R32G32B32_UINT: case VK_FORMAT_R32G32B32_SINT: *n = 3, *bytes = 4; break;
    case VK_FORMAT_R32G32B32A32_UINT: case VK_FORMAT_R32G32B32A32_SINT: *n = 4, *bytes = 4; break;
    default: return false;
    }
    *sign = f == VK_FORMAT_R8_SINT || f == VK_FORMAT_R8G8_SINT || f == VK_FORMAT_R8G8B8_SINT ||
            f == VK_FORMAT_R8G8B8A8_SINT || f == VK_FORMAT_R16_SINT || f == VK_FORMAT_R16G16_SINT ||
            f == VK_FORMAT_R16G16B16_SINT || f == VK_FORMAT_R16G16B16A16_SINT || f == VK_FORMAT_R32_SINT ||
            f == VK_FORMAT_R32G32_SINT || f == VK_FORMAT_R32G32B32_SINT || f == VK_FORMAT_R32G32B32A32_SINT;
    return true;
}

/**
 * @brief Writes elements 0 to n-1 (vertices, or the one instance a draw
 * has) of an integer attribute, read from the
 * stream's guest bytes at src (stride bytes apart), as four 32-bit words
 * each (missing components 0, 0, 1) into out.
 *
 * The translated vertex programs take every attribute as a float vector
 * and use its bits as the guest's registers do, and a Vulkan integer
 * format feeds only an integer-typed input; an R32G32B32A32_SFLOAT input
 * over these words gives the program the integer's bits unchanged.
 */
void nvn_expand_integer(const uint8_t *src, uint64_t stride, VkFormat f, uint64_t n, uint32_t *out)
{
    uint32_t comps, bytes;
    bool sign;
    integer_layout(f, &comps, &bytes, &sign);
    for (uint64_t v = 0; v < n; v++) {
        const uint8_t *p = src + v * stride;
        uint32_t w[4] = { 0, 0, 0, 1 };
        for (uint32_t c = 0; c < comps; c++) {
            uint32_t x;
            if (bytes == 1) {
                x = sign ? (uint32_t)(int32_t)(int8_t)p[c] : p[c];
            } else if (bytes == 2) {
                uint16_t h;
                memcpy(&h, p + 2 * c, 2);
                x = sign ? (uint32_t)(int32_t)(int16_t)h : h;
            } else {
                memcpy(&x, p + 4 * c, 4);
            }
            w[c] = x;
        }
        memcpy(out + 4 * v, w, sizeof w);
    }
}

/** @brief NVNblendFunc (1 ZERO, 2 ONE, ...) to a blend factor; -1 for one the layer does not map. */
static int blend_factor(uint32_t f)
{
    switch (f) {
    case 1: return VK_BLEND_FACTOR_ZERO;
    case 2: return VK_BLEND_FACTOR_ONE;
    case 3: return VK_BLEND_FACTOR_SRC_COLOR;
    case 4: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 6: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_DST_ALPHA;
    case 8: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 9: return VK_BLEND_FACTOR_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 11: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    case 16: return VK_BLEND_FACTOR_SRC1_COLOR;
    case 17: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
    case 18: return VK_BLEND_FACTOR_SRC1_ALPHA;
    case 19: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
    case 97: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 98: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 99: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 100: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    default: return -1;
    }
}

/** @brief Fills the blend and channel mask of target t of k; RL_OK or a stop. */
static int blend_of(replay *r, uint32_t t, VkPipelineColorBlendAttachmentState *b)
{
    nvn_gfx *st = r->st;
    uint64_t on = 0, idx = t;
    uint32_t m[4] = { 1, 1, 1, 1 };
    int rc;
    memset(b, 0, sizeof *b);
    if (st->have_channel_mask &&
        (rc = nvn_vk_getter(r, "nvnChannelMaskStateGetChannelMask", st->channel_mask, 4, &idx, 1, 4, m, NULL)) != RL_OK)
        return rc;
    b->colorWriteMask = (m[0] & 0xff ? VK_COLOR_COMPONENT_R_BIT : 0) | (m[1] & 0xff ? VK_COLOR_COMPONENT_G_BIT : 0) |
                        (m[2] & 0xff ? VK_COLOR_COMPONENT_B_BIT : 0) | (m[3] & 0xff ? VK_COLOR_COMPONENT_A_BIT : 0);
    if (st->have_color_state &&
        (rc = nvn_vk_getter(r, "nvnColorStateGetBlendEnable", st->color_state, 4, &idx, 1, 0, NULL, &on)) != RL_OK)
        return rc;
    if (!(on & 0xff))
        return RL_OK;
    if (!st->have_blend[t])
        return nvn_vk_stop(r, "blending on target %u without a blend state bound for it", t);
    uint32_t f[4], e[2];
    uint64_t adv = 0;
    if ((rc = nvn_vk_getter(r, "nvnBlendStateGetBlendFunc", st->blend[t], 8, NULL, 0, 4, f, NULL)) != RL_OK ||
        (rc = nvn_vk_getter(r, "nvnBlendStateGetBlendEquation", st->blend[t], 8, NULL, 0, 2, e, NULL)) != RL_OK ||
        (rc = nvn_vk_getter(r, "nvnBlendStateGetAdvancedMode", st->blend[t], 8, NULL, 0, 0, NULL, &adv)) != RL_OK)
        return rc;
    if ((uint32_t)adv)
        return nvn_vk_stop(r, "advanced blend mode %u on target %u", (uint32_t)adv, t);
    int fs[4];
    for (int i = 0; i < 4; i++)
        if ((fs[i] = blend_factor(f[i])) < 0)
            return nvn_vk_stop(r, "blend function %u on target %u", f[i], t);
    static const int ops[] = { -1, VK_BLEND_OP_ADD, VK_BLEND_OP_SUBTRACT, VK_BLEND_OP_REVERSE_SUBTRACT, VK_BLEND_OP_MIN,
                               VK_BLEND_OP_MAX };
    if (e[0] < 1 || e[0] > 5 || e[1] < 1 || e[1] > 5)
        return nvn_vk_stop(r, "blend equation %u/%u on target %u", e[0], e[1], t);
    b->blendEnable = VK_TRUE;
    b->srcColorBlendFactor = (VkBlendFactor)fs[0];
    b->dstColorBlendFactor = (VkBlendFactor)fs[1];
    b->srcAlphaBlendFactor = (VkBlendFactor)fs[2];
    b->dstAlphaBlendFactor = (VkBlendFactor)fs[3];
    b->colorBlendOp = (VkBlendOp)ops[e[0]];
    b->alphaBlendOp = (VkBlendOp)ops[e[1]];
    return RL_OK;
}

static int stencil_ops(replay *r, uint64_t face, VkStencilOpState *s)
{
    uint32_t o[3];
    uint64_t func = 0;
    int rc;
    if ((rc = nvn_vk_getter(r, "nvnDepthStencilStateGetStencilOp", r->st->depth_stencil, 8, &face, 1, 3, o, NULL)) !=
            RL_OK ||
        (rc = nvn_vk_getter(r, "nvnDepthStencilStateGetStencilFunc", r->st->depth_stencil, 8, &face, 1, 0, NULL,
                            &func)) != RL_OK)
        return rc;
    for (int i = 0; i < 3; i++)
        if (o[i] < 1 || o[i] > 8)
            return nvn_vk_stop(r, "stencil op %u", o[i]);
    if ((uint32_t)func < 1 || (uint32_t)func > 8)
        return nvn_vk_stop(r, "stencil function %u", (uint32_t)func);
    *s = (VkStencilOpState){ (VkStencilOp)(o[0] - 1), (VkStencilOp)(o[2] - 1), (VkStencilOp)(o[1] - 1),
                             (VkCompareOp)((uint32_t)func - 1), 0, 0, 0 };
    return RL_OK;
}

/** @brief Fills the fixed-function part of k from the queue's state objects. */
/**
 * @brief For a draw whose host alpha to coverage is not the console's (nvn_a2c_native), gives fragment program frag
 * the draw's samples and dither as its A2C_SPEC, which makes it compute the console's mask; a stop when it has no
 * such constant (it writes no alpha to render target 0).
 */
static int a2c_spec(replay *r, const module *frag, pipe_key *k)
{
    if (!k->alpha_to_coverage || nvn_a2c_native(k->alpha_to_coverage))
        return RL_OK;
    if (!frag || !frag->a2c)
        return nvn_vk_stop(r, "alpha to coverage with fragment program %s, which computes no coverage of its own",
                           frag ? frag->key : "none");
    if (k->nspec[1] == MAX_SPECS)
        return nvn_vk_stop(r, "program %s has no room for its alpha to coverage constant", frag->key);
    k->spec_map[1][k->nspec[1]] = (VkSpecializationMapEntry){ A2C_SPEC, 4 * k->nspec[1], 4 };
    k->spec[1][k->nspec[1]++] = k->alpha_to_coverage;
    nvn_spec_sort(&k->nspec[1], k->spec_map[1], k->spec[1]);
    return RL_OK;
}

/** @brief What a draw takes from the bound multisample state. */
typedef struct ms_read {
    uint32_t samples;
    bool a2c, dither, locations_on;
    /** @brief The programmable sample locations (x | y << 4 in 1/16 pixel), the same for every pixel. */
    uint8_t locations[16];
} ms_read;

/**
 * @brief Reads the bound multisample state through the driver's getters, once for each distinct state (the last
 * one read is kept with its bytes). Its fields the layer does not replay stop: each is off in
 * nvnMultisampleStateSetDefaults (SDK 22.2.8 sdk 0x668bf0): raster samples (bits 15 to 19), coverage modulation (13,
 * 14), coverage to color (20), coverage truncation (5) and the sample location grid (4). Programmable sample
 * locations (3) without the grid are the first n of the state's 16, which the driver's BindMultisampleState copies to
 * every pixel's slots of SET_ANTI_ALIAS_SAMPLE_POSITIONS (sdk 0x69a210 to 0x69a268); with the grid it writes all 16
 * slots as they are, whose pixel order is in no public source (NVK writes slot n for pixel n / samples of a 2x2 grid,
 * the driver's GetSampleLocationsGrid names a 16 / samples pixel grid), so the grid stops.
 */
static int read_multisample(replay *r, ms_read *out)
{
    static uint8_t last[24];
    static ms_read last_read;
    static bool have;
    nvn_gfx *st = r->st;
    if (have && !memcmp(last, st->multisample, sizeof last)) {
        *out = last_read;
        return RL_OK;
    }
    static const char *const getters[] = {
        "nvnMultisampleStateGetSamples",
        "nvnMultisampleStateGetAlphaToCoverageEnable",
        "nvnMultisampleStateGetAlphaToCoverageDither",
        "nvnMultisampleStateGetRasterSamples",
        "nvnMultisampleStateGetCoverageModulationMode",
        "nvnMultisampleStateGetCoverageToColorEnable",
        "nvnMultisampleStateGetCoverageTruncationEnable",
        "nvnMultisampleStateGetSampleLocationsGridEnable",
        "nvnMultisampleStateGetSampleLocationsEnable",
    };
    /* whether the driver has each getter: older NVN1 drivers have no coverage truncation, so their states have none */
    static int8_t present[sizeof getters / sizeof getters[0]];
    uint64_t v[sizeof getters / sizeof getters[0]];
    for (uint32_t i = 0; i < sizeof getters / sizeof getters[0]; i++) {
        v[i] = 0;
        if (i >= 3 && !present[i]) {
            uint64_t a = 0;
            int rc = nvn_driver_lookup(r->c, r->mem, getters[i], &a);
            if (rc != RL_OK)
                return rc;
            present[i] = a ? 1 : -1;
        }
        if (present[i] < 0)
            continue;
        int rc = nvn_vk_getter(r, getters[i], st->multisample, 24, NULL, 0, 0, NULL, &v[i]);
        if (rc != RL_OK)
            return rc;
        if (i >= 3 && i < 8 && ((uint32_t)v[i] & 0xff))
            return nvn_vk_stop(r, "multisample state with %s %u, which is not replayed yet", getters[i] + 23,
                               (uint32_t)v[i] & 0xff);
    }
    memset(out->locations, 0, sizeof out->locations);
    out->locations_on = (v[8] & 0xff) != 0;
    for (uint32_t i = 0; out->locations_on && i < 16; i += 2) {
        uint64_t args[2] = { i, 2 };
        uint32_t f[4];
        int rc = nvn_vk_getter(r, "nvnMultisampleStateGetSampleLocations", st->multisample, 24, args, 2, 1, f, NULL);
        if (rc != RL_OK)
            return rc;
        for (uint32_t k = 0; k < 2; k++) {
            float x, y;
            memcpy(&x, &f[2 * k], 4);
            memcpy(&y, &f[2 * k + 1], 4);
            out->locations[i + k] = (uint8_t)((uint32_t)(x * 16.0f) | (uint32_t)(y * 16.0f) << 4);
        }
    }
    out->samples = (uint32_t)v[0] > 1 ? (uint32_t)v[0] : 1;
    out->a2c = (v[1] & 0xff) != 0;
    out->dither = (v[2] & 0xff) != 0;
    memcpy(last, st->multisample, sizeof last);
    last_read = *out;
    have = true;
    return RL_OK;
}

static int fixed_state(replay *r, pipe_key *k)
{
    nvn_gfx *st = r->st;
    int rc;
    k->ncolor = st->ncolor;
    for (uint32_t i = 0; i < st->ncolor; i++) {
        k->color[i] = st->color[i].v ? st->color[i].format : VK_FORMAT_UNDEFINED;
        if ((rc = blend_of(r, i, &k->blend[i])) != RL_OK)
            return rc;
    }
    if (st->depth.v) {
        k->depth = st->depth.v->aspect & VK_IMAGE_ASPECT_DEPTH_BIT ? st->depth.v->format : VK_FORMAT_UNDEFINED;
        k->stencil = st->depth.v->aspect & VK_IMAGE_ASPECT_STENCIL_BIT ? st->depth.v->format : VK_FORMAT_UNDEFINED;
    }
    k->cull = VK_CULL_MODE_NONE;
    k->front = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    k->polygon = VK_POLYGON_MODE_FILL;
    if (st->have_polygon) {
        uint64_t cull, front, mode, bias;
        if ((rc = nvn_vk_getter(r, "nvnPolygonStateGetCullFace", st->polygon, 4, NULL, 0, 0, NULL, &cull)) != RL_OK ||
            (rc = nvn_vk_getter(r, "nvnPolygonStateGetFrontFace", st->polygon, 4, NULL, 0, 0, NULL, &front)) != RL_OK ||
            (rc = nvn_vk_getter(r, "nvnPolygonStateGetPolygonMode", st->polygon, 4, NULL, 0, 0, NULL, &mode)) !=
                RL_OK ||
            (rc = nvn_vk_getter(r, "nvnPolygonStateGetPolygonOffsetEnables", st->polygon, 4, NULL, 0, 0, NULL,
                                &bias)) != RL_OK)
            return rc;
        if ((uint32_t)mode > 2)
            return nvn_vk_stop(r, "polygon mode %u", (uint32_t)mode);
        k->cull = (uint32_t)cull & 3;
        bool ccw = ((uint32_t)front & 1) != (nvn_device_origin_mode() != NVN_ORIGIN_UPPER_LEFT);
        k->front = ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
        k->polygon = (uint32_t)mode == 0 ? VK_POLYGON_MODE_POINT
                     : (uint32_t)mode == 1 ? VK_POLYGON_MODE_LINE
                                           : VK_POLYGON_MODE_FILL;
        uint32_t want = k->polygon == VK_POLYGON_MODE_POINT ? 1u : k->polygon == VK_POLYGON_MODE_LINE ? 2u : 4u;
        k->bias_enable = ((uint32_t)bias & want) != 0;
    }
    k->depth_func = VK_COMPARE_OP_LESS;
    if (st->have_depth_stencil) {
        uint64_t test, write, func, stencil;
        const uint8_t *ds = st->depth_stencil;
        if ((rc = nvn_vk_getter(r, "nvnDepthStencilStateGetDepthTestEnable", ds, 8, NULL, 0, 0, NULL, &test)) !=
                RL_OK ||
            (rc = nvn_vk_getter(r, "nvnDepthStencilStateGetDepthWriteEnable", ds, 8, NULL, 0, 0, NULL, &write)) !=
                RL_OK ||
            (rc = nvn_vk_getter(r, "nvnDepthStencilStateGetDepthFunc", ds, 8, NULL, 0, 0, NULL, &func)) != RL_OK ||
            (rc = nvn_vk_getter(r, "nvnDepthStencilStateGetStencilTestEnable", ds, 8, NULL, 0, 0, NULL, &stencil)) !=
                RL_OK)
            return rc;
        if ((uint32_t)func < 1 || (uint32_t)func > 8)
            return nvn_vk_stop(r, "depth function %u", (uint32_t)func);
        k->depth_test = (test & 0xff) != 0;
        k->depth_write = (write & 0xff) != 0;
        k->depth_func = (uint32_t)func - 1;
        k->stencil_test = (stencil & 0xff) != 0;
        if (k->stencil_test &&
            ((rc = stencil_ops(r, 1, &k->front_ops)) != RL_OK || (rc = stencil_ops(r, 2, &k->back_ops)) != RL_OK))
            return rc;
    }
    uint32_t state_samples = 1;
    bool a2c_on = false, a2c_dither = false;
    k->has_locations = 0;
    if (st->have_multisample) {
        ms_read m;
        if ((rc = read_multisample(r, &m)) != RL_OK)
            return rc;
        a2c_on = m.a2c;
        a2c_dither = m.dither;
        state_samples = m.samples;
        k->has_locations = m.locations_on;
        memcpy(k->locations, m.locations, sizeof k->locations);
    }
    uint32_t drawn;
    if (!nvn_vk_target_samples(st, &drawn))
        return nvn_vk_stop(r, "render targets of different sample counts are bound together");
    if (!drawn)
        drawn = state_samples;
    else if (state_samples != drawn)
        return nvn_vk_stop(r, "a multisample state of %u samples draws into render targets of %u", state_samples,
                           drawn);
    if (drawn > 1 && !g.standard_samples)
        return nvn_vk_stop(r, "a draw of %u samples on a host device without the standard sample positions", drawn);
    k->samples = drawn > 1 ? drawn : 0;
    if (k->has_locations && drawn > 1 && !(g.sample_locations && (g.sample_location_counts & drawn)))
        return nvn_vk_stop(r, "programmable sample locations at %u samples on a host device that cannot place them", drawn);
    if (drawn < 2)
        k->has_locations = 0;
    k->sample_shading = drawn > 1 && st->sample_shading == 1;
    if (k->sample_shading && !g.sample_shading)
        return nvn_vk_stop(r, "a program forced to shade per sample on a host device without sample rate shading");
    /*
     * The driver's BindMultisampleState sets SET_ANTI_ALIAS_ALPHA_CONTROL from the enable and the dither footprint
     * from the dither flag (PIXELS_2X2 with it, PIXELS_1X1 without; SDK 22.2.8, sdk 0x69a0f0). Only the dithered
     * pattern of 2, 4 and 8 samples is measured, and only on an NVIDIA host (nvn_vkdev.nvidia).
     */
    if (a2c_on && drawn < 2)
        return nvn_vk_stop(r, "alpha to coverage at one sample, which is not measured");
    k->alpha_to_coverage = a2c_on ? drawn | (a2c_dither ? NVN_A2C_DITHER : 0) : 0;
    k->primitive_restart = st->primitive_restart;
    k->rasterizer_discard = st->rasterizer_discard;
    k->conservative_raster = st->conservative_raster && g.conservative_raster;
    if (st->conservative_raster && !nvn_conservative_control_is_post_snap(st->conservative_control))
        return nvn_vk_stop(r, "conservative raster control 0x%x, which programs pre-snap rasterization: the host has "
                           "only post-snap conservative rasterization", st->conservative_control);
    if (k->conservative_raster) {
        uint32_t most = (uint32_t)(g.conservative_extra_max * 4.0f);
        k->conservative_dilate = st->conservative_dilate;
        if (k->conservative_dilate > most) {
            k->conservative_dilate = most;
            if (!g.dilate_warned) {
                g.dilate_warned = true;
                rl_log(RL_LOG_WARN, "nvn",
                       "conservative raster dilate of %u quarter pixels is past what the host device allows "
                       "(%u): the largest it allows is used",
                       st->conservative_dilate, most);
            }
        }
    }
    k->sample_mask = st->sample_mask & ((1u << drawn) - 1u);
    k->viewport_swizzle = st->viewport_swizzle[0];
    if (k->viewport_swizzle != NVN_VIEWPORT_SWIZZLE_IDENTITY && !g.viewport_swizzle)
        return nvn_vk_stop(r, "viewport swizzle 0x%x, and the host device has no VK_NV_viewport_swizzle",
                           k->viewport_swizzle);
    k->provoking_last = st->provoking_last;
    if (k->provoking_last && !g.provoking_last) {
        k->provoking_last = 0;
        if (!g.provoking_warned) {
            g.provoking_warned = true;
            rl_log(RL_LOG_WARN, "nvn", "the last vertex is the provoking vertex and the host device has no "
                   "VK_EXT_provoking_vertex with provokingVertexLast: the first vertex provokes");
        }
    }
    k->depth_clamp = st->depth_clamp;
    k->depth_bounds = st->depth_bounds && k->depth != VK_FORMAT_UNDEFINED;
    if (k->depth_bounds && !g.depth_bounds)
        return nvn_vk_stop(r, "depth bounds test, which the host device does not have");
    if ((rc = nvn_shading_rate_state(r, st, &k->shading_rate)) != RL_OK)
        return rc;
    k->negative_one_to_one = nvn_device_depth_mode() == 0;
    return RL_OK;
}

/**
 * @brief The binding of attributes whose format is NVN_FORMAT_NONE (the
 * game disabled them while the program still reads them): a stride-0
 * buffer of (0, 0, 0, 1). The driver's nvnCommandBufferBindVertexAttribState
 * programs such an attribute as 0x3a400040, a constant (unfetched) single
 * 32-bit float, which reads as x = 0 with the missing components filled
 * as (0, 0, 1).
 */
#define DEFAULT_BINDING (NVN_VERTEX_BUFFERS - 1u)

void nvn_vertex_attribs_bound(uint32_t *state, uint32_t *constant, uint32_t n)
{
    uint32_t written = n >= 32 ? ~0u : (1u << n) - 1u;
    *state |= written;
    *constant &= ~written;
    if (n % 2 && n < 32) {
        *constant |= 1u << n;
        *state &= ~(1u << n);
    }
}

uint32_t nvn_vertex_attrib_source(uint32_t state, uint32_t constant, uint32_t i)
{
    if (i >= 32)
        return NVN_ATTRIB_UNKNOWN;
    if (state & (1u << i))
        return NVN_ATTRIB_BOUND;
    return constant & (1u << i) ? NVN_ATTRIB_CONSTANT : NVN_ATTRIB_UNKNOWN;
}

void nvn_vertex_streams_bound(uint32_t *state, uint32_t n) { *state |= n >= 32 ? ~0u : (1u << n) - 1u; }

bool nvn_vertex_stream_known(uint32_t state, uint32_t s) { return s < 32 && (state & (1u << s)); }

uint32_t nvn_vertex_stream_bytes(uint64_t addr, bool mapped)
{
    if (!addr)
        return NVN_STREAM_UNBOUND;
    return mapped ? NVN_STREAM_POOL : NVN_STREAM_ZEROS;
}

/** @brief Fills the vertex input of k for the attributes vs reads. */
static int vertex_state(replay *r, const module *vs, pipe_key *k, fixed_push *fp)
{
    fp->scaled_unsigned = fp->scaled_signed = 0;
    nvn_gfx *st = r->st;
    uint32_t streams = 0;
    bool real15 = false, def = false;
    uint32_t int_streams = 0;
    int rc;
    for (uint32_t i = 0; i < NVN_VERTEX_ATTRIBS && i < 32; i++) {
        if (!(vs->inputs & (1u << i)))
            continue;
        uint32_t source = nvn_vertex_attrib_source(st->attrib_state, st->attrib_constant, i);
        if (source == NVN_ATTRIB_UNKNOWN)
            return nvn_vk_stop(r, "the vertex program reads attribute %u, which no bind on this queue wrote (%u bound)",
                               i, st->nattribs);
        uint32_t fo[2] = { 0, 0 };
        uint64_t stream = 0;
        if (source == NVN_ATTRIB_BOUND &&
            ((rc = nvn_vk_getter(r, "nvnVertexAttribStateGetFormat", st->attribs[i], 4, NULL, 0, 2, fo, NULL)) !=
                 RL_OK ||
             (rc = nvn_vk_getter(r, "nvnVertexAttribStateGetStreamIndex", st->attribs[i], 4, NULL, 0, 0, NULL,
                                 &stream)) != RL_OK))
            return rc;
        if (fo[0] == 0) {
            def = true;
            if (!(streams & (1u << DEFAULT_BINDING)))
                k->bind[k->nbind++] = (VkVertexInputBindingDescription){ DEFAULT_BINDING, 0,
                                                                         VK_VERTEX_INPUT_RATE_VERTEX };
            streams |= 1u << DEFAULT_BINDING;
            k->attr[k->nattr++] = (VkVertexInputAttributeDescription){ i, DEFAULT_BINDING,
                                                                       VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
            continue;
        }
        VkFormat f = vertex_format(fo[0]);
        if (f == VK_FORMAT_UNDEFINED || f == VK_FORMAT_A2B10G10R10_UINT_PACK32)
            return nvn_vk_stop(r, "vertex attribute %u has format %u, which the float input cannot take", i, fo[0]);
        if ((uint32_t)stream >= NVN_VERTEX_BUFFERS || !nvn_vertex_stream_known(st->stream_state, (uint32_t)stream))
            return nvn_vk_stop(r, "vertex attribute %u reads stream %u, which no bind on this queue wrote (%u bound)", i,
                               (uint32_t)stream, st->nstreams);
        bool sign = false;
        VkFormat sf = integer_format(f) ? scaled_format(f, &sign) : VK_FORMAT_UNDEFINED;
        if (sf != VK_FORMAT_UNDEFINED) {
            if (sign)
                fp->scaled_signed |= 1u << i;
            else
                fp->scaled_unsigned |= 1u << i;
            if ((uint32_t)stream == DEFAULT_BINDING)
                real15 = true;
            k->attr[k->nattr++] = (VkVertexInputAttributeDescription){ i, (uint32_t)stream, sf, fo[1] };
            streams |= 1u << stream;
            continue;
        }
        if (integer_format(f)) {
            uint32_t b = NVN_VERTEX_BUFFERS + k->nint;
            k->ints[k->nint].stream = (uint32_t)stream;
            k->ints[k->nint].offset = fo[1];
            k->ints[k->nint++].format = f;
            k->bind[k->nbind++] = (VkVertexInputBindingDescription){ b, 16, VK_VERTEX_INPUT_RATE_VERTEX };
            k->ints[k->nint - 1].per_instance = k->nbind - 1;
            k->attr[k->nattr++] = (VkVertexInputAttributeDescription){ i, b, VK_FORMAT_R32G32B32A32_SFLOAT, 0 };
            int_streams |= 1u << stream;
            continue;
        }
        if ((uint32_t)stream == DEFAULT_BINDING)
            real15 = true;
        k->attr[k->nattr++] = (VkVertexInputAttributeDescription){ i, (uint32_t)stream, f, fo[1] };
        streams |= 1u << stream;
    }
    if (def && real15)
        return nvn_vk_stop(r, "a disabled vertex attribute and stream %u in one draw", DEFAULT_BINDING);
    for (uint32_t s = 0; s < NVN_VERTEX_BUFFERS; s++) {
        if (!((streams | int_streams) & (1u << s)) || (def && s == DEFAULT_BINDING))
            continue;
        uint64_t stride = 0, div = 0;
        if ((rc = nvn_vk_getter(r, "nvnVertexStreamStateGetStride", st->streams[s], 8, NULL, 0, 0, NULL, &stride)) !=
                RL_OK ||
            (rc = nvn_vk_getter(r, "nvnVertexStreamStateGetDivisor", st->streams[s], 8, NULL, 0, 0, NULL, &div)) !=
                RL_OK)
            return rc;
        if ((uint32_t)div > 1)
            return nvn_vk_stop(r, "vertex stream %u with divisor %u", s, (uint32_t)div);
        for (uint32_t j = 0; j < k->nint; j++)
            if (k->ints[j].stream == s) {
                k->ints[j].stride = (uint32_t)stride;
                uint32_t bi = k->ints[j].per_instance;
                k->ints[j].per_instance = div != 0;
                if (div)
                    k->bind[bi].inputRate = VK_VERTEX_INPUT_RATE_INSTANCE;
            }
        if (streams & (1u << s))
            k->bind[k->nbind++] = (VkVertexInputBindingDescription){
                s, (uint32_t)stride, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX
            };
    }
    return RL_OK;
}

/** @brief A uniform buffer descriptor for n bytes at src (host bytes) in the arena, padded to a whole bank. */
static int bank_of(replay *r, const void *src, uint64_t n, VkDescriptorBufferInfo *bi)
{
    static const uint8_t none[1];
    uint64_t off;
    if (!src) {
        src = none;
        n = 0;
    }
    if (n > UBO_BYTES)
        n = UBO_BYTES;
    if (!nvn_vk_upload_bank(r, src, n, UBO_BYTES, 256, &off))
        return nvn_vk_stop(r, "the upload arena is full");
    *bi = (VkDescriptorBufferInfo){ g.arena.buf, off, UBO_BYTES };
    return RL_OK;
}

static const void *user_bank(replay *r, uint32_t s, uint32_t b, uint64_t addr);

/**
 * @brief The uniform buffer descriptor for user bank b of stage s at GPU address addr (size bytes): the pool's own
 * buffer when the bytes lie in imported guest memory, so the draw reads them when it executes, after the GPU
 * writes that precede it in the batch, as the hardware reads a bound buffer; else a copy of the bytes (bank_of).
 */
static int nvn_ubo_bind(replay *r, uint32_t s, uint32_t b, uint64_t addr, uint64_t size, VkDescriptorBufferInfo *bi)
{
    void *pool;
    uint64_t off;
    if (addr && size && !(r->st->ubo_patched[s] & 1u << b) && nvn_guest_buffer(nvn_gpu_guest(addr), size, &pool, &off) &&
        off % 256 == 0) {
        *bi = (VkDescriptorBufferInfo){ (VkBuffer)pool, off, size > UBO_BYTES ? UBO_BYTES : size };
        return RL_OK;
    }
    if (!nvn_vk_sync_uniforms(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    return bank_of(r, user_bank(r, s, b, addr), addr ? size : 0, bi);
}

/**
 * @brief The driver bank c[0] of graphics stage s: each storage buffer slot k of the stage is 16 bytes
 * {GPU address, u32 size} at 16 * (first[s] + k), where first is the driver's per-stage table (sdk 0xbb0570,
 * read by nvnCommandBufferBindStorageBuffer, sdk 0x67be50: vertex 0x11, fragment 0x51, geometry 0x41, tessellation
 * control 0x21, tessellation evaluation 0x31, and on NVN2 task 0x67 and mesh 0x77); vertex programs read their slots at 0x110 and up. Every other word
 * is 0 (the layer does not know the driver's other c[0] fields yet).
 */
void nvn_graphics_table(const nvn_gfx *st, uint32_t s, uint8_t c0[NVN_GFX_C0_BYTES])
{
    static const uint32_t first[NVN_STAGES] = { 0x11, 0x51, 0x41, 0x21, 0x31, 0, 0x67, 0x77 };
    memset(c0, 0, NVN_GFX_C0_BYTES);
    for (uint32_t k = 0; k < NVN_STORAGE_SLOTS && s < NVN_STAGES && first[s]; k++) {
        uint32_t at = 16 * (first[s] + k), size = (uint32_t)st->gstorage[s][k].size;
        if (at + 16 > NVN_GFX_C0_BYTES)
            break;
        memcpy(c0 + at, &st->gstorage[s][k].addr, 8);
        memcpy(c0 + at + 8, &size, 4);
    }
    if (s == 0)
        memcpy(c0 + NVN_C0_BASE_INSTANCE, &st->inst_base, 4);
}

/** @brief The 16-byte buffer the slots a program never reaches are bound to, made at the first use. */
static bool blank_buffer(void)
{
    return d.blank || nvn_vk_gpu_buffer(&d.blank, &d.blank_mem, 16);
}

/**
 * @brief Writes the constant banks the bound modules read into new
 * descriptor sets, sets[0] and sets[1], with the texture set in sets[2].
 * A bank that does not fit the upload arena submits the batch, which resets the pool and frees the sets; the sets
 * are then not written, and nvn_vk_stage runs the stage again with new ones.
 */
static int write_constants(replay *r, module *const *mods, uint32_t first_task, const VkDescriptorSetLayout *layouts_in,
                           VkDescriptorSet sets[3])
{
    uint64_t flushes;
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = d.pool,
                                       .descriptorSetCount = 2,
                                       .pSetLayouts = layouts_in };
    if (VK(vkAllocateDescriptorSets)(g.dev, &ai, sets) != VK_SUCCESS) {
        if (!nvn_vk_flush(r) || VK(vkAllocateDescriptorSets)(g.dev, &ai, sets) != VK_SUCCESS)
            return nvn_vk_stop(r, "cannot allocate descriptor sets");
    }
    flushes = g.flushes;
    sets[2] = nvn_texset();
    VkDescriptorBufferInfo bi[(USER_BINDINGS + DRIVER_BINDINGS + NVN_STORAGE_SLOTS) * NVN_STAGES];
    VkWriteDescriptorSet wr[(USER_BINDINGS + DRIVER_BINDINGS + NVN_STORAGE_SLOTS) * NVN_STAGES];
    uint32_t n = 0;
    for (uint32_t s = 0; s < NVN_STAGES; s++) {
        if (!mods[s])
            continue;
        for (uint32_t k = 0; mods[s]->slot_array && k < NVN_STORAGE_SLOTS; k++) {
            if (layouts_in != d.set_layout && layouts_in != d.mset_layout)
                return nvn_vk_stop(r, "program %s of stage %u reaches its storage slots through descriptors, which only draws bind",
                                   mods[s]->key, s);
            if (layouts_in == d.mset_layout && s >= NVN_STAGE_TASK && d.mesh_slots < NVN_STORAGE_SLOTS)
                return nvn_vk_stop(r, "program %s of stage %u reaches its storage slots through descriptors, which the host "
                                      "device's storage buffer limit leaves no room for in a mesh pass",
                                   mods[s]->key, s);
            VkDeviceSize off = 0;
            uint64_t size = r->st->gstorage[s][k].size;
            VkBuffer pb = size ? nvn_draw_pool_span(r->st->gstorage[s][k].addr, size, g.storage_align, &off) : NULL;
            if (!pb && (!size || !(mods[s]->storage_slots & (1u << k)))) {
                /* a slot the program does not reach, or an empty one: never read through */
                if (!blank_buffer())
                    return nvn_vk_stop(r, "the host device cannot make a buffer");
                pb = d.blank;
                off = 0;
                size = VK_WHOLE_SIZE;
            } else if (!pb) {
                return nvn_vk_stop(r, "program %s of stage %u reaches storage slot %u (0x%llx+0x%llx), which no memory pool holds whole",
                                   mods[s]->key, s, k, (unsigned long long)r->st->gstorage[s][k].addr,
                                   (unsigned long long)size);
            } else {
                nvn_mirror_written(r->st->gstorage[s][k].addr, size);
            }
            bi[n] = (VkDescriptorBufferInfo){ pb, off, size };
            wr[n] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = sets[1],
                                            .dstBinding = nvn_slot_binding(s),
                                            .dstArrayElement = k,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                            .pBufferInfo = &bi[n] };
            n++;
        }
        for (uint32_t b = 0; b < DRIVER_BINDINGS; b++) {
            if (!(mods[s]->driver & (1u << b)))
                continue;
            int rc;
            if (b == 1) {
                const nvn_consts *k = &r->st->consts[s];
                rc = k->addr && !nvn_vk_reading_guest(k->addr, k->size)
                         ? nvn_vk_stop(r, "the host device failed a submit")
                         : bank_of(r, k->addr ? r->mem + k->addr : NULL, k->addr ? k->size : 0, &bi[n]);
            } else if (b == 2) {
                rc = bank_of(r, r->st->bank2_host[s], NVN_BANK2_BYTES, &bi[n]);
            } else if (b == 0) {
                uint8_t c0[NVN_GFX_C0_BYTES];
                nvn_graphics_table(r->st, s, c0);
                if (s >= NVN_STAGE_TASK)
                    memcpy(c0 + MESH_FIRST_TASK_OFFSET, &first_task, 4);
                for (uint32_t k = 0; k < NVN_STORAGE_SLOTS && s != NVN_STAGE_COMPUTE; k++)
                    nvn_mirror_written(r->st->gstorage[s][k].addr, r->st->gstorage[s][k].size);
                rc = bank_of(r, c0, sizeof c0, &bi[n]);
            } else {
                rc = nvn_vk_stop(r, "program %s reads driver constant bank c[%u], which the layer does not fill yet",
                                 mods[s]->key, b);
            }
            if (rc != RL_OK)
                return rc;
            wr[n] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = sets[1],
                                            .dstBinding = DRIVER_BINDINGS * s + b,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            .pBufferInfo = &bi[n] };
            n++;
        }
        for (uint32_t b = 0; b < USER_BINDINGS; b++) {
            if (!(mods[s]->user & (1u << b)))
                continue;
            if (b == PUSH_BINDING) {
                int trc = bank_of(r, r->st->gfx_push[0], NVN_PUSH_BYTES, &bi[n]);
                if (trc != RL_OK)
                    return trc;
                wr[n] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                                .dstSet = sets[0],
                                                .dstBinding = USER_BINDINGS * s + b,
                                                .descriptorCount = 1,
                                                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                .pBufferInfo = &bi[n] };
                n++;
                continue;
            }
            if (b >= NVN_UBO_SLOTS)
                return nvn_vk_stop(r, "program %s reads constant bank c[%u]", mods[s]->key, b + 3);
            if (layouts_in == d.mset_layout && s >= NVN_STAGE_TASK && b >= d.mesh_ubos)
                return nvn_vk_stop(r, "program %s of stage %u reads constant bank c[%u], past the %u the host device's "
                                      "uniform buffer limit leaves each mesh pass",
                                   mods[s]->key, s, b + 3, d.mesh_ubos);
            uint64_t addr = r->st->ubo[s][b].addr, size = r->st->ubo[s][b].size;
            int rc = nvn_ubo_bind(r, s, b, addr, size, &bi[n]);
            if (rc != RL_OK)
                return rc;
            wr[n] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                            .dstSet = sets[0],
                                            .dstBinding = USER_BINDINGS * s + b,
                                            .descriptorCount = 1,
                                            .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                            .pBufferInfo = &bi[n] };
            n++;
        }
    }
    if (g.flushes != flushes)
        return RL_OK;
    if (n)
        VK(vkUpdateDescriptorSets)(g.dev, n, wr, 0, NULL);
    return RL_OK;
}

/**
 * @brief Makes the texture set hold the texture of each handle m reads from
 * driver bank bank_no (whose bytes are bank, len of them), and gives m's
 * kind and dimension constants their values (n of them so far in map and
 * vals). Handles at offsets img_lo to img_hi are image handles: they are
 * bound as storage images and replaced in the bank by their slots.
 */
static int bind_textures(replay *r, const module *m, uint32_t bank_no, uint8_t *bank, uint32_t len, uint32_t *n,
                         VkSpecializationMapEntry *map, uint32_t *vals, uint32_t img_lo, uint32_t img_hi)
{
    for (uint32_t i = 0; i < m->nrecs; i++) {
        const nvn_iface_rec *h = &m->recs[i];
        if (h->kind != NVN_IFACE_TEXTURE)
            continue;
        if (h->bank != bank_no)
            continue;
        if (h->offset + 4 > len)
            return nvn_vk_stop(r, "program %s reads a texture handle at c[%u][0x%x], past the %u bytes bound", m->key,
                               h->bank, h->offset, len);
        uint32_t handle, kind, dim;
        memcpy(&handle, bank + h->offset, 4);
        bool image = h->offset >= img_lo && h->offset < img_hi;
        uint32_t slot;
        int rc = image ? nvn_texset_image(r, handle, &slot, &kind, &dim) : nvn_texset_bind(r, handle, &kind, &dim, &slot);
        if (rc != RL_OK)
            return rc;
        if (!image)
            slot = (handle & 0xfff00000u) | slot;
        memcpy(bank + h->offset, &slot, 4);
        for (uint32_t j = 0; j < m->nrecs; j++) {
            const nvn_iface_rec *c = &m->recs[j];
            if ((c->kind != NVN_IFACE_TEXTURE_KIND && c->kind != NVN_IFACE_TEXTURE_DIM) || c->bank != h->bank ||
                c->offset != h->offset)
                continue;
            if (*n == MAX_SPECS)
                return nvn_vk_stop(r, "program %s has more than %u specialization constants", m->key, MAX_SPECS);
            uint32_t v = c->kind != NVN_IFACE_TEXTURE_KIND ? dim : image ? nvn_texset_surface_kind(slot, kind) : kind;
            if (v == UINT32_MAX)
                return nvn_vk_stop(r,
                                   "program %s reads or writes image id %u through its format, which the host device "
                                   "cannot store (format %u) and relift does not move raw",
                                   m->key, handle & 0xfffffu, nvn_texset_format(slot));
            map[*n] = (VkSpecializationMapEntry){ c->spec, 4 * *n, 4 };
            vals[(*n)++] = v;
        }
    }
    return RL_OK;
}

/**
 * @brief The bytes of user bank b of stage s as the program reads them: the guest's uniform buffer, or when a
 * texture handle in it was given a slot (bind_user_textures), the host copy with the slot in place of the id.
 */
static const void *user_bank(replay *r, uint32_t s, uint32_t b, uint64_t addr)
{
    if (!addr)
        return NULL;
    if (r->st->ubo_patched[s] & 1u << b)
        return r->st->ubo_host[s][b];
    return r->mem + nvn_gpu_guest(addr);
}

/**
 * @brief Gives the texture handles module m reads from user banks (c[3] and up: handles the game keeps in its own
 * uniform buffers) their slots in the texture set, in a host copy of each bank.
 */
static int bind_user_textures(replay *r, const module *m, uint32_t s, uint32_t *n, VkSpecializationMapEntry *map,
                              uint32_t *vals)
{
    nvn_gfx *st = r->st;
    for (uint32_t i = 0; i < m->nrecs; i++) {
        const nvn_iface_rec *h = &m->recs[i];
        if (h->kind == NVN_IFACE_TEXTURE && (h->bank == 1 || h->bank > 2 + NVN_UBO_SLOTS))
            return nvn_vk_stop(r, "program %s reads a texture handle at c[%u][0x%x]", m->key, h->bank, h->offset);
        if (h->kind != NVN_IFACE_TEXTURE || h->bank < 3 || st->ubo_patched[s] & 1u << (h->bank - 3))
            continue;
        uint32_t b = h->bank - 3;
        uint64_t addr = st->ubo[s][b].addr, size = st->ubo[s][b].size;
        if (!addr)
            return nvn_vk_stop(r, "program %s reads a texture handle from unbound uniform buffer c[%u]", m->key,
                               h->bank);
        if (size > UBO_BYTES)
            size = UBO_BYTES;
        if (!st->ubo_host[s][b] && !(st->ubo_host[s][b] = malloc(UBO_BYTES)))
            return nvn_vk_stop(r, "out of memory");
        if (!nvn_vk_sync_uniforms(r))
            return nvn_vk_stop(r, "the host device failed a submit");
        if (!nvn_vk_reading_guest(nvn_gpu_guest(addr), size))
            return nvn_vk_stop(r, "the host device failed a submit");
        memcpy(st->ubo_host[s][b], r->mem + nvn_gpu_guest(addr), size);
        memset(st->ubo_host[s][b] + size, 0, UBO_BYTES - size);
        int rc = bind_textures(r, m, h->bank, st->ubo_host[s][b], (uint32_t)size, n, map, vals, 0, 0);
        if (rc != RL_OK)
            return rc;
        st->ubo_patched[s] |= 1u << b;
    }
    return RL_OK;
}

/**
 * @brief Keeps in their slots the textures m's handles in registers may name (NVN_IFACE_HANDLE_SOURCE records): the
 * words of the banks they are loaded from, read as the guest left them, not as the layer rewrote bound handles in its
 * copies. bank0 (compute's driver table, size0 bytes) stands in for c[0]; draws have none.
 */
static int bind_handle_sources(replay *r, const module *m, uint32_t s, const uint8_t *bank0, uint32_t size0)
{
    nvn_gfx *st = r->st;
    for (uint32_t i = 0; i < m->nrecs; i++) {
        const nvn_iface_rec *h = &m->recs[i];
        if (h->kind != NVN_IFACE_HANDLE_SOURCE)
            continue;
        const uint8_t *bytes;
        uint32_t size;
        if (h->bank == 0 && bank0) {
            bytes = bank0;
            size = size0;
        } else if (h->bank == 2 && s < NVN_STAGE_COMPUTE) {
            bytes = st->bank2[s];
            size = NVN_BANK2_BYTES;
        } else if (h->bank >= 3 && h->bank < 3 + NVN_UBO_SLOTS) {
            uint64_t addr = st->ubo[s][h->bank - 3].addr, ubo = st->ubo[s][h->bank - 3].size;
            if (!addr)
                continue;
            if (!nvn_vk_sync_uniforms(r))
                return nvn_vk_stop(r, "the host device failed a submit");
            bytes = r->mem + nvn_gpu_guest(addr);
            size = ubo > UBO_BYTES ? UBO_BYTES : (uint32_t)ubo;
        } else {
            return nvn_vk_stop(r, "program %s loads a texture handle from c[%u][0x%x]", m->key, h->bank, h->offset);
        }
        int rc = nvn_texset_bind_words(r, bytes, size, h->offset, h->spec != 0);
        if (rc != RL_OK)
            return rc;
    }
    return RL_OK;
}

static void dynamic_state(replay *r, VkRect2D area)
{
    nvn_gfx *st = r->st;
    uint32_t origin = nvn_device_origin_mode();
    float full[4] = { 0, 0, (float)area.extent.width, (float)area.extent.height };
    const float *v = st->viewports[0][2] != 0 && st->viewports[0][3] != 0 ? st->viewports[0] : full;
    float w[4];
    nvn_window_viewport(origin, area.extent.height, v, w);
    VkViewport vp = { w[0], w[1], w[2], w[3], v == full ? 0 : st->depth_ranges[0][0], v == full ? 1 : st->depth_ranges[0][1] };
    VK(vkCmdSetViewport)(g.cb, 0, 1, &vp);
    VkRect2D sc = area;
    if (st->scissor_set) {
        int64_t x0 = st->scissor.offset.x;
        int64_t y0 = nvn_window_top(origin, area.extent.height, st->scissor.offset.y, st->scissor.extent.height);
        int64_t x1 = x0 + st->scissor.extent.width, y1 = y0 + st->scissor.extent.height;
        x0 = x0 < 0 ? 0 : x0;
        y0 = y0 < 0 ? 0 : y0;
        x1 = x1 > area.extent.width ? area.extent.width : x1;
        y1 = y1 > area.extent.height ? area.extent.height : y1;
        sc = x1 > x0 && y1 > y0 ? (VkRect2D){ { (int32_t)x0, (int32_t)y0 }, { (uint32_t)(x1 - x0), (uint32_t)(y1 - y0) } }
                                : (VkRect2D){ { 0, 0 }, { 0, 0 } };
    }
    VK(vkCmdSetScissor)(g.cb, 0, 1, &sc);
    VK(vkCmdSetStencilCompareMask)(g.cb, VK_STENCIL_FACE_FRONT_BIT, st->stencil_value_mask[0]);
    VK(vkCmdSetStencilCompareMask)(g.cb, VK_STENCIL_FACE_BACK_BIT, st->stencil_value_mask[1]);
    VK(vkCmdSetStencilWriteMask)(g.cb, VK_STENCIL_FACE_FRONT_BIT, st->stencil_mask[0]);
    VK(vkCmdSetStencilWriteMask)(g.cb, VK_STENCIL_FACE_BACK_BIT, st->stencil_mask[1]);
    VK(vkCmdSetStencilReference)(g.cb, VK_STENCIL_FACE_FRONT_BIT, st->stencil_ref[0]);
    VK(vkCmdSetStencilReference)(g.cb, VK_STENCIL_FACE_BACK_BIT, st->stencil_ref[1]);
    VK(vkCmdSetDepthBias)(g.cb, st->polygon_offset[1], st->polygon_offset[2], st->polygon_offset[0]);
    VK(vkCmdSetBlendConstants)(g.cb, st->blend_color);
    VK(vkCmdSetLineWidth)(g.cb, g.wide_lines ? st->line_width : 1.0f);
    if (st->depth_bounds && g.depth_bounds) {
        float lo = st->depth_bounds_range[0], hi = st->depth_bounds_range[1];
        lo = lo < 0.0f ? 0.0f : lo > 1.0f ? 1.0f : lo;
        hi = hi < 0.0f ? 0.0f : hi > 1.0f ? 1.0f : hi;
        VK(vkCmdSetDepthBounds)(g.cb, lo, hi);
    }
}

void nvn_widen_indices(const uint8_t *src, uint32_t n, bool restart, uint16_t *dst)
{
    for (uint32_t i = 0; i < n; i++)
        dst[i] = restart && src[i] == 0xffu ? 0xffffu : src[i];
}

uint32_t nvn_draw_vertex_streams(const pipe_key *k, uint32_t out[NVN_VERTEX_BUFFERS])
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < k->nbind && n < NVN_VERTEX_BUFFERS; i++)
        if (k->bind[i].binding < NVN_VERTEX_BUFFERS)
            out[n++] = k->bind[i].binding;
    return n;
}

/** @brief Whether k reads the default attribute values (0, 0, 0, 1) through DEFAULT_BINDING with stride 0. */
static bool default_stream(const pipe_key *k)
{
    for (uint32_t i = 0; i < k->nbind; i++)
        if (k->bind[i].binding == DEFAULT_BINDING)
            return k->bind[i].stride == 0;
    return false;
}

/** @brief What a draw puts into the batch being recorded, and where. */
typedef struct draw_data {
    const pipe_key *k;
    module *const *mods;
    bool indexed, has_def;
    uint32_t itype, first, count, nb;
    uint32_t binding[NVN_VERTEX_BUFFERS];
    /** @brief The Vulkan index type the uploaded indices have (8-bit indices are widened to 16 bits). */
    VkIndexType ivk;
    int32_t base;
    uint64_t iaddr, def_off, ioff;
    VkDeviceSize offs[NVN_VERTEX_BUFFERS], int_off[NVN_VERTEX_ATTRIBS];
    /** @brief The buffer each vertex stream and the indices are read from: an imported pool's, or the arena. */
    VkBuffer vbuf[NVN_VERTEX_BUFFERS], ibuf;
    VkDescriptorSet sets[3];
} draw_data;

VkBuffer nvn_draw_pool_span(uint64_t addr, uint64_t n, uint64_t align, VkDeviceSize *off)
{
    void *buf;
    uint64_t o;
    if (!n || !nvn_guest_buffer(nvn_gpu_guest(addr), n, &buf, &o) || o % align)
        return NULL;
    *off = o;
    return (VkBuffer)buf;
}

/** @brief Puts a draw's vertices, indices and constant banks into the batch being recorded (an nvn_vk_stage step). */
static int stage_draw(replay *r, void *arg)
{
    draw_data *dd = arg;
    const pipe_key *kp = dd->k;
    const nvn_gfx *st = r->st;
    bool indexed = dd->indexed;
    uint32_t itype = dd->itype, first = dd->first, count = dd->count;
    int32_t base = dd->base;
    dd->nb = 0;
    dd->has_def = false;
    uint32_t streams[NVN_VERTEX_BUFFERS], nstreams = nvn_draw_vertex_streams(kp, streams);
    for (uint32_t i = 0; i < nstreams; i++) {
        uint32_t s = streams[i];
        uint64_t off;
        if (s == DEFAULT_BINDING && !st->vb[s].addr && default_stream(kp)) {
            static const float one[4] = { 0, 0, 0, 1 };
            if (!nvn_vk_upload_bytes(r, (const uint8_t *)one, sizeof one, 0, 16, &off))
                return nvn_vk_stop(r, "the upload arena is full");
            dd->def_off = off;
            dd->has_def = true;
            continue;
        }
        uint32_t bytes = nvn_vertex_stream_bytes(st->vb[s].addr, nvn_gpu_mapped(st->vb[s].addr, st->vb[s].size));
        if (bytes == NVN_STREAM_UNBOUND)
            return nvn_vk_stop(r, "vertex stream %u has no buffer bound", s);
        if (bytes == NVN_STREAM_ZEROS) {
            (void)rl_report_unknown(0x4e564e5354524d5aull ^ st->vb[s].addr, "nvn",
                                    "vertex stream %u reads 0x%llx bytes at 0x%llx, which no memory pool holds any "
                                    "more: it reads zeros",
                                    s, (unsigned long long)st->vb[s].size, (unsigned long long)st->vb[s].addr);
            uint8_t *zero = calloc(1, st->vb[s].size ? st->vb[s].size : 16);
            bool ok = zero && nvn_vk_upload_bytes(r, zero, st->vb[s].size ? st->vb[s].size : 16, 0, 16, &off);
            free(zero);
            if (!ok)
                return nvn_vk_stop(r, "the upload arena is full");
            dd->binding[dd->nb] = s;
            dd->vbuf[dd->nb] = g.arena.buf;
            dd->offs[dd->nb++] = off;
            continue;
        }
        VkDeviceSize po;
        VkBuffer pb = nvn_draw_pool_span(st->vb[s].addr, st->vb[s].size, 4, &po);
        if (pb)
            off = po;
        else if (!nvn_vk_upload(r, st->vb[s].addr, st->vb[s].size, 0, 16, &off))
            return nvn_vk_stop(r, "the upload arena is full");
        dd->binding[dd->nb] = s;
        dd->vbuf[dd->nb] = pb ? pb : g.arena.buf;
        dd->offs[dd->nb++] = off;
    }
    static const uint32_t isize[] = { 1, 2, 4 };
    dd->ibuf = g.arena.buf;
    if (indexed) {
        if (itype > 2)
            return nvn_vk_stop(r, "index type %u", itype);
        if (itype == 0) {
            uint16_t *wide = malloc(count ? (size_t)count * 2 : 2);
            if (!wide)
                return nvn_vk_stop(r, "out of memory widening %u 8-bit indices", count);
            if (!nvn_vk_reading_guest(nvn_gpu_guest(dd->iaddr), count))
                return nvn_vk_stop(r, "the host device failed a submit");
            const uint8_t *narrow = r->mem + nvn_gpu_guest(dd->iaddr);
            nvn_widen_indices(narrow, count, kp->primitive_restart, wide);
            bool ok = nvn_vk_upload_bytes(r, wide, (uint64_t)count * 2, 0, 16, &dd->ioff);
            free(wide);
            if (!ok)
                return nvn_vk_stop(r, "the upload arena is full");
            dd->ivk = VK_INDEX_TYPE_UINT16;
        } else {
            VkDeviceSize po;
            VkBuffer pb = nvn_draw_pool_span(dd->iaddr, (uint64_t)count * isize[itype], isize[itype], &po);
            if (pb) {
                dd->ibuf = pb;
                dd->ioff = po;
            } else if (!nvn_vk_upload(r, dd->iaddr, (uint64_t)count * isize[itype], 0, 16, &dd->ioff)) {
                return nvn_vk_stop(r, "the upload arena is full");
            }
            dd->ivk = itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        }
    }
    if (kp->nint) {
        uint64_t nv = (uint64_t)first + count;
        if (indexed) {
            if (!nvn_vk_reading_guest(nvn_gpu_guest(dd->iaddr), (uint64_t)count * isize[itype]))
                return nvn_vk_stop(r, "the host device failed a submit");
            const uint8_t *ip = r->mem + nvn_gpu_guest(dd->iaddr);
            int64_t hi = -1;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t x = itype == 0   ? ip[i]
                             : itype == 1 ? (uint32_t)ip[2 * i] | (uint32_t)ip[2 * i + 1] << 8
                                          : rl_ld32(ip, 4 * (uint64_t)i);
                uint32_t last = itype == 0 ? 0xffu : itype == 1 ? 0xffffu : 0xffffffffu;
                bool restart = kp->primitive_restart && x == last;
                if (!restart && (int64_t)x + base > hi)
                    hi = (int64_t)x + base;
            }
            nv = (uint64_t)(hi + 1);
        }
        if (nv > (16u << 20))
            return nvn_vk_stop(r, "a draw reads %llu vertices of an integer attribute", (unsigned long long)nv);
        for (uint32_t j = 0; j < kp->nint; j++) {
            uint32_t s = kp->ints[j].stream;
            if (!st->vb[s].addr)
                return nvn_vk_stop(r, "vertex stream %u has no buffer bound", s);
            uint64_t n = kp->ints[j].per_instance ? (uint64_t)st->inst_base + st->inst_count : nv;
            uint32_t *w = malloc(n ? n * 16 : 16);
            if (!w)
                return nvn_vk_stop(r, "out of memory expanding an integer vertex attribute");
            if (!nvn_vk_reading_guest(nvn_gpu_guest(st->vb[s].addr), st->vb[s].size)) {
                free(w);
                return nvn_vk_stop(r, "the host device failed a submit");
            }
            nvn_expand_integer(r->mem + nvn_gpu_guest(st->vb[s].addr) + kp->ints[j].offset, kp->ints[j].stride, kp->ints[j].format,
                           n, w);
            bool ok = nvn_vk_upload_bytes(r, w, n ? n * 16 : 16, 0, 16, &dd->int_off[j]);
            free(w);
            if (!ok)
                return nvn_vk_stop(r, "the upload arena is full");
        }
    }
    return write_constants(r, dd->mods, 0, d.set_layout, dd->sets);
}

/**
 * @brief Whether draws run now: nvnCommandBufferSetRenderEnable(false) skips them, and a conditional render enable
 * runs them when the two 64-bit counter values at its address (a ReportCounter pair, 16 bytes apart) are equal or
 * unequal. The values are read after the host device finished what was recorded, the way the GPU reads them when the
 * draw reaches it, so a counter the GPU wrote earlier in the stream counts. The wait is skipped when nothing recorded or
 * in flight can still write the 32 bytes (no report due there and no write range of the device's work covers them):
 * guest memory then already holds what the GPU will read.
 */
int nvn_draw_render_enabled(replay *r, bool *run)
{
    nvn_gfx *st = r->st;
    *run = true;
    if (st->render_mode == NVN_RENDER_ALWAYS)
        return RL_OK;
    if (st->render_mode == NVN_RENDER_NEVER) {
        *run = false;
        return RL_OK;
    }
    uint64_t at = nvn_gpu_guest(st->render_addr);
    if ((nvn_counters_report_due(at, 32) || nvn_vk_unsettled(at, 32)) && !nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    const uint8_t *p = r->mem + at;
    bool equal = rl_ld64(p, 0) == rl_ld64(p, 16);
    *run = equal == (st->render_mode == NVN_RENDER_IF_EQUAL);
    uint64_t seen[3] = { st->render_addr, rl_ld64(p, 0), rl_ld64(p, 16) };
    nvn_trace_order("render", "conditional render", r->queue, r->submit, r->pos, seen, 3, *run, true);
    return RL_OK;
}

/**
 * @brief Finds the translation of the program bound to stage s (none leaves mods[s] NULL), checks what it reads,
 * binds its textures and notes its module and specialization constants in k.
 */
static int prepare_stage(replay *r, pipe_key *k, module **mods, uint32_t s)
{
    nvn_gfx *st = r->st;
    if (!st->code[s])
        return RL_OK;
    if (!(mods[s] = module_of(r, st->code[s], (int)s)))
        return RL_PENDING;
    if (mods[s]->user >> USER_BINDINGS)
        return nvn_vk_stop(r, "program %s reads constant bank c[%d], past the %u user banks the layer binds", mods[s]->key,
                           3 + __builtin_ctz(mods[s]->user >> USER_BINDINGS) + (int)USER_BINDINGS, USER_BINDINGS);
    if (mods[s]->other_sets)
        return nvn_vk_stop(r, "program %s uses a guest memory set, which draws do not bind yet", mods[s]->key);
    if (mods[s]->register_handles) {
        int src = nvn_texset_sync_registered(r);
        if (src != RL_OK)
            return src;
    }
    memcpy(st->bank2_host[s], st->bank2[s], NVN_BANK2_BYTES);
    int trc = bind_textures(r, mods[s], 2, st->bank2_host[s], NVN_BANK2_BYTES, &k->nspec[s], k->spec_map[s], k->spec[s],
                            NVN_BANK2_IMAGES, NVN_BANK2_IMAGES + 8u * NVN_IMAGE_SLOTS);
    if (trc != RL_OK)
        return trc;
    if (mods[s]->driver & 1u << 2)
        trc = nvn_separate_handles(r, s);
    if (trc != RL_OK)
        return trc;
    trc = bind_user_textures(r, mods[s], s, &k->nspec[s], k->spec_map[s], k->spec[s]);
    if (trc != RL_OK)
        return trc;
    trc = bind_handle_sources(r, mods[s], s, NULL, 0);
    if (trc != RL_OK)
        return trc;
    nvn_spec_sort(&k->nspec[s], k->spec_map[s], k->spec[s]);
    k->mods[s] = mods[s]->m;
    return RL_OK;
}

bool nvn_draw_geometry_input_ok(uint32_t mode)
{
    return mode == NVN_DRAW_TRIANGLES || mode == NVN_DRAW_TRIANGLE_STRIP || mode == NVN_DRAW_TRIANGLE_FAN;
}

/**
 * @brief Checks that the host can run geometry program gs after a draw of primitive mode: the device has geometry
 * shaders, the program writes no viewport index (the layer sets one viewport), and the draw's primitives are the
 * triangles its translation takes as input (the translator's default geometry input, as NVIDIA takes the input
 * primitive from the draw and not from the program header).
 */
static int geometry_draw(replay *r, const module *gs, uint32_t mode)
{
    if (!g.geometry_shader)
        return nvn_vk_stop(r, "geometry program %s: the host device has no geometry shaders", gs->key);
    if (gs->writes_viewport)
        return nvn_vk_stop(r, "geometry program %s writes the viewport index", gs->key);
    if (!nvn_draw_geometry_input_ok(mode))
        return nvn_vk_stop(r, "geometry program %s after primitive mode %u; its translation takes triangles", gs->key,
                           mode);
    return RL_OK;
}

static void count_command(void);

/**
 * @brief RELIFT_NVN_CHECK_ADDRS: before a draw or dispatch, every storage buffer slot of its stages first to last (the
 * compute slots for NVN_STAGE_COMPUTE) whose range is not inside one current memory pool is logged once per program and
 * slot (nvn-check), with the program, stage, slot and range: an access through it reads or writes outside the pools.
 */
static void check_slots(const nvn_gfx *st, module *const *mods, uint32_t first, uint32_t last)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RELIFT_NVN_CHECK_ADDRS");
        on = e && *e && strcmp(e, "0");
    }
    if (!on)
        return;
    static char seen[4096][24];
    static uint32_t nseen;
    for (uint32_t s = first; s <= last && s < NVN_STAGES; s++) {
        if (!mods[s])
            continue;
        for (uint32_t k = 0; k < NVN_STORAGE_SLOTS; k++) {
            uint64_t addr = s == NVN_STAGE_COMPUTE ? st->storage[k].addr : st->gstorage[s][k].addr;
            uint64_t size = s == NVN_STAGE_COMPUTE ? st->storage[k].size : st->gstorage[s][k].size;
            if (!addr || !size || nvn_gpu_mapped(addr, size))
                continue;
            char tag[24];
            snprintf(tag, sizeof tag, "%.8s:%u:%u", mods[s]->key, s, k);
            bool dup = false;
            for (uint32_t i = 0; i < nseen && !dup; i++)
                dup = !strcmp(seen[i], tag);
            if (dup)
                continue;
            if (nseen < 4096)
                memcpy(seen[nseen++], tag, sizeof tag);
            char where[512];
            nvn_gpu_describe(addr, where, sizeof where);
            rl_log(RL_LOG_WARN, "nvn-check", "present %llu: program %.8s stage %u storage slot %u at 0x%llx+0x%llx is not in one memory pool: %s",
                   (unsigned long long)g.presents, mods[s]->key, s, k, (unsigned long long)addr,
                   (unsigned long long)size, where);
        }
    }
}

/**
 * @brief What the graphics stages first to last (task and mesh included) of a draw may write: their global stores
 * (the written-page marks or every page), and their storage buffer slots, as written for the mirror and the guest.
 */
static int mark_stage_writes(replay *r, const nvn_gfx *st, module *const *mods, uint32_t first, uint32_t last)
{
    for (uint32_t s = first; s <= last && s < NVN_STAGES; s++) {
        if (s == NVN_STAGE_COMPUTE || !mods[s])
            continue;
        if (mods[s]->marks_pages)
            nvn_mirror_written_marked();
        else if (mods[s]->global_store && !nvn_mirror_takes_unmarked())
            return nvn_mirror_refuse_unmarked(r, mods[s]->key);
        else if (mods[s]->global_store)
            nvn_mirror_written_all();
        for (uint32_t j = 0; mods[s]->stores_guest && j < NVN_STORAGE_SLOTS; j++)
            nvn_vk_mark_write_range(nvn_gpu_guest(st->gstorage[s][j].addr), st->gstorage[s][j].size);
    }
    return RL_OK;
}
static void note_last(const char *kind, module *const *mods, uint32_t n);
static bool trace_buffer(void);
static void trace_report(const replay *r, const char *key, uint32_t stage, uint32_t words);
/** @brief Bytes of the GPU trace buffer: room for a program of 16383 instructions. */
/** @brief Words of each stage's region of the GPU trace buffer (the translator's TRACE_STAGE_WORDS). */
#define TRACE_STAGE_WORDS (16u + 16u * 16384u)
/** @brief Bytes of the GPU trace buffer: a region for each stage (vertex 0 to mesh 7), programs of up to 16383 instructions. */
#define TRACE_BUF_BYTES (4u * NVN_STAGES * TRACE_STAGE_WORDS)

/** @brief A draw: mode, then indexed (index type, index address, base vertex) or not (first vertex), and count. */
static int draw(replay *r, uint32_t mode, bool indexed, uint32_t itype, uint64_t iaddr, int32_t base, uint32_t first,
                uint32_t count)
{
    nvn_gfx *st = r->st;
    bool run;
    int cond = nvn_draw_render_enabled(r, &run);
    if (cond != RL_OK || !run)
        return cond;
    if (!nvn_geometry_mode_draws(st->geometry_mode, false))
        return nvn_vk_stop(r, "a vertex draw in geometry processing mode %u, which runs the mesh pipeline",
                           st->geometry_mode);
    memset(st->ubo_patched, 0, sizeof st->ubo_patched);
    if (!layouts())
        return nvn_vk_stop(r, "cannot make the pipeline layout");
    if (st->ncolor >= 4)
        nvn_trace_wide_draws++;
    count_command();
    if (nvn_trace_active)
        nvn_trace_begin(r, "draw");
    pipe_key k;
    memset(&k, 0, sizeof k);
    VkPrimitiveTopology topo;
    if (!topology(mode, &topo))
        return nvn_vk_stop(r, "draw primitive %u", mode);
    k.topology = topo;
    module *mods[NVN_STAGES] = { NULL };
    for (uint32_t s = 0; s < 5; s++) {
        int prc = prepare_stage(r, &k, mods, s);
        if (prc != RL_OK)
            return prc;
    }
    int rc;
    if (mods[1] && mods[1]->alpha_test) {
        uint64_t func = NVN_ALPHA_ALWAYS;
        if (st->have_color_state &&
            (rc = nvn_vk_getter(r, "nvnColorStateGetAlphaTest", st->color_state, 4, NULL, 0, 0, NULL, &func)) != RL_OK)
            return rc;
        if ((uint32_t)func < 1 || (uint32_t)func > 8)
            return nvn_vk_stop(r, "alpha test function %u", (uint32_t)func);
        if (k.nspec[1] == MAX_SPECS)
            return nvn_vk_stop(r, "program %s has no room for its alpha test constant", mods[1]->key);
        k.spec_map[1][k.nspec[1]] = (VkSpecializationMapEntry){ ALPHA_FUNC_SPEC, 4 * k.nspec[1], 4 };
        k.spec[1][k.nspec[1]++] = (uint32_t)func;
    }
    if (!mods[0])
        return nvn_vk_stop(r, "draw without a vertex program");
    if (mods[3] || mods[4])
        return nvn_vk_stop(r, "draw with tessellation programs (control %s, evaluation %s, geometry %s, mode %u)",
                           mods[3] ? mods[3]->key : "none", mods[4] ? mods[4]->key : "none",
                           mods[2] ? mods[2]->key : "none", mode);
    if (mods[2] && (rc = geometry_draw(r, mods[2], mode)) != RL_OK)
        return rc;
    const module *last = nvn_module_last_stage(mods);
    if (!nvn_module_link(mods, k.mods))
        return nvn_vk_stop(r, "cannot make the fragment program %s for the varyings the stage before it writes",
                           mods[1]->key);
    fixed_push fp = {
        .point_size = st->point_size,
        .alpha_ref = st->alpha_ref,
        .written_pages = nvn_mirror_written_pages_address(),
    };
    rc = fixed_state(r, &k);
    if (rc == RL_OK)
        rc = a2c_spec(r, mods[1], &k);
    if (rc == RL_OK)
        rc = vertex_state(r, mods[0], &k, &fp);
    if (rc != RL_OK)
        return rc;
    VkPipeline p = nvn_pipe_graphics(&k);
    if (!p)
        return nvn_vk_stop(r, "the host driver cannot make a pipeline for this draw (vertex %s, fragment %s, geometry %s)",
                           mods[0] ? mods[0]->key : "none", mods[1] ? mods[1]->key : "none",
                           mods[2] ? mods[2]->key : "none");
    const char *program_keys[5];
    for (uint32_t s = 0; s < 5; s++)
        program_keys[s] = mods[s] ? mods[s]->key : NULL;
    nvn_pipe_record_graphics(program_keys, &k);
    note_last("draw", mods, 5);
    check_slots(st, mods, 0, 4);
    /* GPU trace: each traced stage records into its own region of the buffer */
    bool traced = false;
    uint32_t trace_words = 0;
    for (uint32_t s = 0; s < 5; s++)
        if (mods[s] && mods[s]->trace_words) {
            if (!d.trace_on || mods[s]->trace_words * 4u > TRACE_BUF_BYTES)
                return nvn_vk_stop(r, "program %s is a trace-mode translation: run with RELIFT_NVN_GPU_TRACE=1 "
                                   "(programs of up to 16383 instructions)", mods[s]->key);
            traced = true;
            if (mods[s]->trace_words > trace_words)
                trace_words = mods[s]->trace_words;
        }
    if (traced && (!trace_buffer() || !nvn_vk_flush(r)))
        return nvn_vk_stop(r, "cannot make or clear the GPU trace buffer");
    if (traced)
        memset(d.tbuf.map, 0, 4u * trace_words);
    draw_data dd = { .k = &k, .mods = mods, .indexed = indexed, .itype = itype, .first = first, .count = count,
                     .base = base, .iaddr = iaddr };
    if ((rc = nvn_vk_stage(r, stage_draw, &dd, "a draw")) != RL_OK)
        return rc;
    if ((rc = mark_stage_writes(r, st, mods, 0, 4)) != RL_OK)
        return rc;
    nvn_xfb_draw xd;
    if ((rc = nvn_xfb_draw_prepare(r, last, mode, count, &xd)) != RL_OK)
        return rc;
    fp.xfb_scratch = xd.scratch;
    VkRect2D area;
    if ((rc = nvn_counters_prepare(r)) != RL_OK)
        return rc;
    if (!nvn_vk_begin_rendering_layered(r, &area, last->writes_layer))
        return nvn_vk_stop(r, "draw without render targets (%u color, first %p, depth %p)", st->ncolor,
                           st->ncolor ? (void *)st->color[0].v : NULL, (void *)st->depth.v);
    uint32_t counted = nvn_counters_begin(st, mods[2] != NULL, mods[3] != NULL || mods[4] != NULL);
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    nvn_pipe_set_dynamic(g.cb, &k);
    if (k.shading_rate)
        nvn_shading_rate_bind(st);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 3, dd.sets, 0, NULL);
    bind_pools(VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, d.set_layout[4]);
    if (traced)
        VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, NVN_TRACE_SET, 1, &d.tset, 0, NULL);
    VK(vkCmdPushConstants)(g.cb, d.layout, FIXED_STAGES, 0, sizeof fp, &fp);
    dynamic_state(r, area);
    for (uint32_t i = 0; i < dd.nb; i++)
        VK(vkCmdBindVertexBuffers)(g.cb, dd.binding[i], 1, &dd.vbuf[i], &dd.offs[i]);
    if (dd.has_def)
        VK(vkCmdBindVertexBuffers)(g.cb, DEFAULT_BINDING, 1, &g.arena.buf, &dd.def_off);
    for (uint32_t j = 0; j < k.nint; j++)
        VK(vkCmdBindVertexBuffers)(g.cb, NVN_VERTEX_BUFFERS + j, 1, &g.arena.buf, &dd.int_off[j]);
    if (nvn_label_on()) {
        const char *keys[6] = { NULL };
        for (uint32_t s = 0; s < 5; s++)
            keys[s] = mods[s] ? mods[s]->key : NULL;
        nvn_label_draw(g.cb, "draw", keys);
    }
    nvn_xfb_draw_begin(g.cb, &xd);
    if (indexed) {
        VK(vkCmdBindIndexBuffer)(g.cb, dd.ibuf, dd.ioff, dd.ivk);
        VK(vkCmdDrawIndexed)(g.cb, count, st->inst_count, 0, base, st->inst_base);
    } else {
        VK(vkCmdDraw)(g.cb, count, st->inst_count, first, st->inst_base);
    }
    nvn_vk_pass_drew(mods[1] ? mods[1]->key : NULL);
    nvn_xfb_draw_end(g.cb, &xd);
    nvn_counters_end(counted);
    if (traced) {
        if (!nvn_vk_flush(r))
            return nvn_vk_stop(r, "the host device failed the traced draw (%s)", d.last);
        for (uint32_t s = 0; s < 5; s++)
            if (mods[s] && mods[s]->trace_words)
                trace_report(r, mods[s]->key, s, mods[s]->trace_words);
    }
    if (xd.mem && (rc = nvn_xfb_draw_finish(r, &xd)) != RL_OK)
        return rc;
    if (nvn_trace_active) {
        nvn_trace_cmd tc = { .mode = mode, .itype = itype, .first = first, .count = count, .base = base, .indexed = indexed };
        for (uint32_t s = 0; s < 5; s++)
            tc.keys[s] = mods[s] ? mods[s]->key : NULL;
        nvn_trace_end(r, &tc);
    }
    return RL_OK;
}

const nvn_module *nvn_draw_module(replay *r, uint32_t s) { return module_of(r, r->st->code[s], (int)s); }

int nvn_draw_arrays(replay *r, uint32_t mode, uint32_t first, uint32_t count)
{
    r->st->inst_base = 0;
    r->st->inst_count = 1;
    return draw(r, mode, false, 0, 0, 0, first, count);
}

static VkPipeline mesh_pass_pipeline(replay *r, const module *m, uint32_t n, const VkSpecializationMapEntry *map,
                                     const uint32_t *vals);

/** @brief What a mesh draw puts into the batch being recorded. */
typedef struct mesh_data {
    module *const *mods;
    uint32_t first;
    const VkDescriptorSetLayout *layouts;
    VkDescriptorSet sets[3];
} mesh_data;

/** @brief Puts a mesh draw's constant banks into the batch being recorded (an nvn_vk_stage step). */
static int stage_mesh(replay *r, void *arg)
{
    mesh_data *dd = arg;
    return write_constants(r, dd->mods, dd->first, dd->layouts, dd->sets);
}

/** @brief The alpha test function constant of the fragment program, in k (as a draw makes it). */
static int mesh_alpha(replay *r, pipe_key *k, module *frag)
{
    nvn_gfx *st = r->st;
    if (!frag || !frag->alpha_test)
        return RL_OK;
    uint64_t func = 8;
    int rc;
    if (st->have_color_state &&
        (rc = nvn_vk_getter(r, "nvnColorStateGetAlphaTest", st->color_state, 4, NULL, 0, 0, NULL, &func)) != RL_OK)
        return rc;
    if ((uint32_t)func < 1 || (uint32_t)func > 8)
        return nvn_vk_stop(r, "alpha test function %u", (uint32_t)func);
    if (k->nspec[1] == MAX_SPECS)
        return nvn_vk_stop(r, "program %s has no room for its alpha test constant", frag->key);
    k->spec_map[1][k->nspec[1]] = (VkSpecializationMapEntry){ ALPHA_FUNC_SPEC, 4 * k->nspec[1], 4 };
    k->spec[1][k->nspec[1]++] = (uint32_t)func;
    return RL_OK;
}

/** @brief The draw of the native back end: vkCmdDrawMeshTasksEXT over the task workgroups. */
static int draw_mesh_native(replay *r, module *const *mods, pipe_key *k, uint32_t first, uint32_t count)
{
    nvn_gfx *st = r->st;
    int rc;
    k->mesh_mode = 1;
    if ((rc = fixed_state(r, k)) != RL_OK || (rc = a2c_spec(r, mods[1], k)) != RL_OK)
        return rc;
    nvn_core_leave();
    VkPipeline p = nvn_pipe_graphics(k);
    if (!p)
        return nvn_vk_stop(r, "the host driver cannot make a pipeline for this mesh draw");
    bool traced = (mods[NVN_STAGE_TASK] && mods[NVN_STAGE_TASK]->trace_words) ||
                  (mods[NVN_STAGE_MESH] && mods[NVN_STAGE_MESH]->trace_words) || (mods[1] && mods[1]->trace_words);
    if (traced && (!d.trace_on || !trace_buffer() || !nvn_vk_flush(r)))
        return nvn_vk_stop(r, "a mesh draw with trace-mode programs needs RELIFT_NVN_GPU_TRACE=1");
    if (traced)
        memset(d.tbuf.map, 0, TRACE_BUF_BYTES);
    mesh_data dd = { .mods = mods, .first = first, .layouts = d.set_layout };
    if ((rc = nvn_vk_stage(r, stage_mesh, &dd, "a mesh draw")) != RL_OK)
        return rc;
    VkRect2D area;
    if ((rc = nvn_counters_prepare(r)) != RL_OK)
        return rc;
    if (!nvn_vk_begin_rendering(r, &area))
        return nvn_vk_stop(r, "draw without render targets (%u color, first %p, depth %p)", st->ncolor,
                           st->ncolor ? (void *)st->color[0].v : NULL, (void *)st->depth.v);
    uint32_t counted = nvn_counters_begin(st, false, false);
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    nvn_pipe_set_dynamic(g.cb, k);
    if (k->shading_rate)
        nvn_shading_rate_bind(st);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 3, dd.sets, 0, NULL);
    bind_pools(VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, d.set_layout[4]);
    fixed_push fp = {
        .point_size = st->point_size,
        .alpha_ref = st->alpha_ref,
        .written_pages = nvn_mirror_written_pages_address(),
    };
    if (traced)
        VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, NVN_TRACE_SET, 1, &d.tset, 0, NULL);
    VK(vkCmdPushConstants)(g.cb, d.layout, FIXED_STAGES, 0, sizeof fp, &fp);
    dynamic_state(r, area);
    g.draw_mesh_tasks(g.cb, count, 1, 1);
    nvn_counters_end(counted);
    nvn_vk_end_rendering();
    if (traced) {
        if (!nvn_vk_flush(r))
            return nvn_vk_stop(r, "the host device failed the traced mesh draw (%s)", d.last);
        for (uint32_t s = 1; s < NVN_STAGES; s++)
            if (mods[s] && mods[s]->trace_words && (s == 1 || s >= NVN_STAGE_TASK))
                trace_report(r, mods[s]->key, s, mods[s]->trace_words);
    }
    return RL_OK;
}

/** @brief Guest workgroups of slots per compute workgroup that cover n of them, as an x and y of a dispatch (x at most MESH_DISPATCH_X). */
static void dispatch_dims(uint32_t n, uint32_t slots, uint32_t *x, uint32_t *y)
{
    uint32_t groups = (uint32_t)(((uint64_t)n + (slots ? slots : 1) - 1) / (slots ? slots : 1));
    *x = groups < MESH_DISPATCH_X ? (groups ? groups : 1) : MESH_DISPATCH_X;
    *y = groups <= MESH_DISPATCH_X ? 1 : (groups + MESH_DISPATCH_X - 1) / MESH_DISPATCH_X;
}

/**
 * @brief The draw of the compute back end (the translator's mesh_fallback): the task program over count workgroups,
 * the scan of its mesh counts, the mesh program by indirect dispatch, then an indirect draw of the records with the
 * helper vertex program, all in the buffer every pass shares and in submission order. mi and ti are the mesh and
 * task programs' pipelines (ti NULL without a task program).
 */
static int draw_mesh_compute(replay *r, module *const *mods, pipe_key *k, const nvn_mesh_info *mi,
                             const nvn_mesh_info *ti, uint32_t first, uint32_t count)
{
    nvn_gfx *st = r->st;
    int rc;
    if (!mesh_layouts())
        return nvn_vk_stop(r, "cannot make the mesh pass layouts");
    if (d.moverflow)
        return nvn_vk_stop(r, "an earlier mesh draw overflowed the mesh buffer");
    module *mesh = mods[NVN_STAGE_MESH], *task = mods[NVN_STAGE_TASK];
    module *scan_m = module_helper(r, "scan", mesh, NVN_STAGE_MESH), *vert = module_helper(r, "vertex", mesh, 0);
    if (!scan_m || !vert)
        return RL_PENDING;
    if (mesh->stores_guest || (task && task->stores_guest) || scan_m->stores_guest || vert->stores_guest ||
        (mods[1] && mods[1]->stores_guest))
        nvn_vk_mark_write();
    if (vert->other_sets || scan_m->other_sets)
        return nvn_vk_stop(r, "a helper of mesh program %s uses a set the layer does not bind", mesh->key);
    for (uint32_t s = NVN_STAGE_TASK; s <= NVN_STAGE_MESH; s++)
        for (uint32_t i = 0; mods[s] && i < NVN_STORAGE_SLOTS; i++)
            if (st->gstorage[s][i].size && !nvn_gpu_mapped(st->gstorage[s][i].addr, (uint32_t)st->gstorage[s][i].size)) {
                char where[512];
                nvn_gpu_describe(st->gstorage[s][i].addr, where, sizeof where);
                return nvn_vk_stop(r, "program %s: storage buffer %u at 0x%llx (0x%x bytes) is not in one memory pool: %s",
                                   mods[s]->key, i, (unsigned long long)st->gstorage[s][i].addr,
                                   (uint32_t)st->gstorage[s][i].size, where);
            }
    uint64_t total = d.mstatus.size ? MESH_BUFFER_BYTES : 0;
    if (total > g.max_storage_range)
        total = g.max_storage_range & ~3u;
    if (total > (uint64_t)g.max_index_value * 4)
        total = (uint64_t)g.max_index_value * 4;
    uint64_t words = total / 4, pw = ti ? ti->payload_words : 0, n = ti ? count : 0;
    uint64_t scan_at = MESH_HEADER_WORDS + n * pw, mesh_at = scan_at + (ti ? n + 1 : 0);
    uint64_t per = (uint64_t)mi->record_words + (mi->indexed ? mi->index_words : 0);
    if (!per || mesh_at >= words || (words - mesh_at) / per < 1)
        return nvn_vk_stop(r, "a mesh draw of %u tasks needs more than the %llu MiB mesh buffer", count,
                           (unsigned long long)(MESH_BUFFER_BYTES >> 20));
    uint64_t cap = (words - mesh_at) / per;
    uint64_t verts = (uint64_t)mi->max_vertices + 1;
    if (cap * verts > (uint64_t)g.max_index_value)
        cap = g.max_index_value / verts;
    uint32_t hdr[MESH_HEADER_WORDS] = { 0 };
    hdr[MESH_H_TASKS] = (uint32_t)n;
    hdr[MESH_H_CAP] = (uint32_t)cap;
    hdr[MESH_H_TASK_RECS] = MESH_HEADER_WORDS;
    hdr[MESH_H_TASK_SCAN] = (uint32_t)scan_at;
    hdr[MESH_H_MESH_RECS] = (uint32_t)mesh_at;
    hdr[MESH_H_INDEXES] = (uint32_t)(mesh_at + cap * mi->record_words);
    uint32_t mx, my;
    dispatch_dims(count, mi->slots, &mx, &my);
    if (!ti) {
        hdr[MESH_H_MESHES] = count;
        hdr[MESH_H_DISPATCH] = mx;
        hdr[MESH_H_DISPATCH + 1] = my;
        hdr[MESH_H_DISPATCH + 2] = 1;
    }
    pipe_key kg;
    memset(&kg, 0, sizeof kg);
    static const uint32_t list[4] = { 0, VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
                                      VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    if (mi->topology < 1 || mi->topology > 3)
        return nvn_vk_stop(r, "mesh program %s has output topology %u", mesh->key, mi->topology);
    kg.topology = list[mi->topology];
    kg.mods[0] = vert->m;
    kg.nspec[1] = k->nspec[1];
    memcpy(kg.spec_map[1], k->spec_map[1], sizeof kg.spec_map[1]);
    memcpy(kg.spec[1], k->spec[1], sizeof kg.spec[1]);
    if (mods[1] && !(kg.mods[1] = nvn_module_fragment_for(vert, mods[1])))
        return nvn_vk_stop(r, "cannot make the fragment program %s for the varyings the helper vertex program writes",
                           mods[1]->key);
    kg.mesh_mode = 2;
    if ((rc = fixed_state(r, &kg)) != RL_OK || (rc = a2c_spec(r, mods[1], &kg)) != RL_OK)
        return rc;
    kg.topology = list[mi->topology];
    nvn_core_leave();
    VkPipeline gp = nvn_pipe_graphics(&kg);
    if (!gp)
        return nvn_vk_stop(r, "the host driver cannot make a pipeline for this mesh draw");
    VkPipeline tp = NULL, sp, mp;
    if (task && !(tp = mesh_pass_pipeline(r, task, k->nspec[NVN_STAGE_TASK], k->spec_map[NVN_STAGE_TASK],
                                          k->spec[NVN_STAGE_TASK])))
        return RL_PENDING;
    if (!(sp = mesh_pass_pipeline(r, scan_m, 0, NULL, NULL)) ||
        !(mp = mesh_pass_pipeline(r, mesh, k->nspec[NVN_STAGE_MESH], k->spec_map[NVN_STAGE_MESH],
                                  k->spec[NVN_STAGE_MESH])))
        return RL_PENDING;
    mesh_data dd = { .mods = mods, .first = first, .layouts = d.mset_layout };
    if ((rc = nvn_vk_stage(r, stage_mesh, &dd, "a mesh draw")) != RL_OK)
        return rc;
    if (d.mstatus_n == MESH_STATUS_SLOTS && !nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    VkRect2D area;
    if ((rc = nvn_counters_prepare(r)) != RL_OK)
        return rc;
    if (!nvn_vk_begin(r))
        return nvn_vk_stop(r, "cannot begin a command buffer");
    VkDescriptorSet sets[4] = { dd.sets[0], dd.sets[1], dd.sets[2], d.mset };
    VK(vkCmdUpdateBuffer)(g.cb, d.mbuf, 0, sizeof hdr, hdr);
    nvn_vk_barrier(g.cb);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.mlayout, 0, 4, sets, 0, NULL);
    bind_pools(VK_PIPELINE_BIND_POINT_COMPUTE, d.mlayout, d.mset_layout[4]);
    fixed_push cfp = { .written_pages = nvn_mirror_written_pages_address() };
    VK(vkCmdPushConstants)(g.cb, d.mlayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof cfp, &cfp);
    if (task) {
        uint32_t tx, ty;
        dispatch_dims((uint32_t)n, ti->slots, &tx, &ty);
        VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, tp);
        VK(vkCmdDispatch)(g.cb, tx, ty, 1);
        nvn_vk_barrier(g.cb);
        VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, sp);
        VK(vkCmdDispatch)(g.cb, 1, 1, 1);
        nvn_vk_barrier(g.cb);
    }
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, mp);
    VK(vkCmdDispatchIndirect)(g.cb, d.mbuf, 4ull * MESH_H_DISPATCH);
    nvn_vk_barrier(g.cb);
    VkBufferCopy cp = { 4ull * MESH_H_MESHES, 16ull * d.mstatus_n, 12 };
    VK(vkCmdCopyBuffer)(g.cb, d.mbuf, d.mstatus.buf, 1, &cp);
    d.mstatus_n++;
    nvn_vk_barrier(g.cb);
    if (!nvn_vk_begin_rendering(r, &area))
        return nvn_vk_stop(r, "draw without render targets (%u color, first %p, depth %p)", st->ncolor,
                           st->ncolor ? (void *)st->color[0].v : NULL, (void *)st->depth.v);
    uint32_t counted = nvn_counters_begin(st, false, false);
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, gp);
    nvn_pipe_set_dynamic(g.cb, &kg);
    if (kg.shading_rate)
        nvn_shading_rate_bind(st);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.mlayout, 0, 4, sets, 0, NULL);
    bind_pools(VK_PIPELINE_BIND_POINT_GRAPHICS, d.mlayout, d.mset_layout[4]);
    fixed_push fp = {
        .point_size = st->point_size,
        .alpha_ref = st->alpha_ref,
        .written_pages = nvn_mirror_written_pages_address(),
    };
    VK(vkCmdPushConstants)(g.cb, d.mlayout, FIXED_STAGES, 0, sizeof fp, &fp);
    dynamic_state(r, area);
    if (mi->indexed) {
        VK(vkCmdBindIndexBuffer)(g.cb, d.mbuf, 0, VK_INDEX_TYPE_UINT32);
        VK(vkCmdDrawIndexedIndirect)(g.cb, d.mbuf, 4ull * MESH_H_DRAW_INDEXED, 1, 20);
    } else {
        VK(vkCmdDrawIndirect)(g.cb, d.mbuf, 4ull * MESH_H_DRAW, 1, 16);
    }
    nvn_counters_end(counted);
    nvn_vk_end_rendering();
    return RL_OK;
}

/**
 * @brief One draw of count task workgroups (mesh workgroups without a task program) from workgroup first, on the
 * programs bound to stages 6 and 7 and the fragment program: by VK_EXT_mesh_shader when the build translated them
 * for it, else by compute passes and an indirect draw.
 */
static int draw_mesh(replay *r, uint32_t first, uint32_t count)
{
    nvn_gfx *st = r->st;
    bool run;
    int rc = nvn_draw_render_enabled(r, &run);
    if (rc != RL_OK || !run || !count)
        return rc;
    if (!nvn_geometry_mode_draws(st->geometry_mode, true))
        return nvn_vk_stop(r, "a mesh draw in geometry processing mode %u, which does not run the mesh pipeline",
                           st->geometry_mode);
    memset(st->ubo_patched, 0, sizeof st->ubo_patched);
    if (!layouts())
        return nvn_vk_stop(r, "cannot make the pipeline layout");
    if (!st->code[NVN_STAGE_MESH])
        return nvn_vk_stop(r, "a mesh draw without a mesh program");
    if (nvn_trace_active)
        nvn_trace_begin(r, "mesh");
    pipe_key k;
    memset(&k, 0, sizeof k);
    module *mods[NVN_STAGES] = { NULL };
    static const uint32_t used[] = { NVN_STAGE_TASK, NVN_STAGE_MESH, 1 };
    for (uint32_t i = 0; i < sizeof used / sizeof used[0]; i++)
        if ((rc = prepare_stage(r, &k, mods, used[i])) != RL_OK)
            return rc;
    module *mesh = mods[NVN_STAGE_MESH], *task = mods[NVN_STAGE_TASK];
    nvn_mesh_info mi, ti;
    if (!nvn_mesh_info_of(mesh->recs, mesh->nrecs, &mi))
        return nvn_vk_stop(r, "mesh program %s has no pipeline in the build's shader table", mesh->key);
    if (task && (!nvn_mesh_info_of(task->recs, task->nrecs, &ti) || ti.native != mi.native))
        return nvn_vk_stop(r, "task program %s and mesh program %s were not translated for one back end", task->key,
                           mesh->key);
    if ((rc = mesh_alpha(r, &k, mods[1])) != RL_OK)
        return rc;
    note_last("mesh draw", mods, NVN_STAGES);
    check_slots(st, mods, NVN_STAGE_TASK, NVN_STAGE_MESH);
    {
        size_t at = strlen(d.last);
        for (uint32_t s = NVN_STAGES; s-- > NVN_STAGE_TASK && at < sizeof d.last;) {
            const nvn_consts *cb = &st->consts[s];
            uint32_t w[4] = { 0 };
            if (cb->addr && cb->size >= 16)
                memcpy(w, r->mem + cb->addr, 16);
            at += (size_t)snprintf(d.last + at, sizeof d.last - at, "; %s c1 0x%llx+0x%x: %08x %08x %08x %08x",
                                   s == NVN_STAGE_TASK ? "task" : "mesh", (unsigned long long)cb->addr, cb->size, w[0],
                                   w[1], w[2], w[3]);
            for (uint32_t j = 0; j < 4 && at < sizeof d.last; j++)
                at += (size_t)snprintf(d.last + at, sizeof d.last - at, "; slot %u 0x%llx+0x%llx", j,
                                       (unsigned long long)st->gstorage[s][j].addr,
                                       (unsigned long long)st->gstorage[s][j].size);
        }
    }
    if (mi.native) {
        if (!g.mesh_shader)
            return nvn_vk_stop(r, "mesh program %s was translated for VK_EXT_mesh_shader, which the host device lacks",
                               mesh->key);
        if (mods[1] && !(k.mods[1] = nvn_module_fragment_for(mesh, mods[1])))
            return nvn_vk_stop(r, "cannot make the fragment program %s for the varyings the mesh program writes",
                               mods[1]->key);
        rc = draw_mesh_native(r, mods, &k, first, count);
    } else {
        rc = draw_mesh_compute(r, mods, &k, &mi, task ? &ti : NULL, first, count);
    }
    if (rc == RL_OK)
        rc = mark_stage_writes(r, st, mods, 1, NVN_STAGES - 1);
    if (rc == RL_OK && nvn_trace_active) {
        nvn_trace_cmd tc = { .first = first, .count = count };
        for (uint32_t s = 0; s < NVN_STAGES; s++)
            tc.keys[s] = mods[s] ? mods[s]->key : NULL;
        nvn_trace_end(r, &tc);
    }
    return rc;
}

/**
 * @brief The draws of an indirect mesh command: up to max records of stride bytes at the GPU address indirect, each
 * {task count, first task} (NV_mesh_shader's VkDrawMeshTasksIndirectCommandNV), as many as the u32 at count_addr says
 * when it is not 0. They are read now, after the host device finished what was recorded before the command, the way
 * the GPU reads them when the draw reaches it.
 */
static int draw_mesh_indirect(replay *r, uint64_t indirect, uint64_t count_addr, uint32_t max, uint32_t stride)
{
    bool run;
    int rc = nvn_draw_render_enabled(r, &run);
    if (rc != RL_OK || !run)
        return rc;
    if (!nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    uint32_t n = max;
    if (count_addr && !nvn_gpu_mapped(count_addr, 4)) {
        char where[512];
        nvn_gpu_describe(count_addr, where, sizeof where);
        return nvn_vk_stop(r, "an indirect mesh draw's count at 0x%llx is in no pool: %s", (unsigned long long)count_addr,
                           where);
    }
    if (count_addr) {
        uint32_t c = rl_ld32(r->mem + nvn_gpu_guest(count_addr), 0);
        n = c < max ? c : max;
    }
    if (n && !nvn_gpu_mapped(indirect, (uint64_t)(n - 1) * stride + 8)) {
        char where[512];
        nvn_gpu_describe(indirect, where, sizeof where);
        return nvn_vk_stop(r, "an indirect mesh draw's %u records of %u bytes at 0x%llx are in no one pool: %s", n,
                           stride, (unsigned long long)indirect, where);
    }
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *rec = r->mem + nvn_gpu_guest(indirect + (uint64_t)i * stride);
        rc = draw_mesh(r, rl_ld32(rec, 4), rl_ld32(rec, 0));
        if (rc != RL_OK)
            return rc;
    }
    return RL_OK;
}

/** @brief The compute pipeline layout: set 0 the stage's user banks, set 1 its driver banks, set 2 the texture set. */
static bool compute_layouts(void)
{
    if (d.clayout)
        return true;
    if (!layouts())
        return false;
    VkDescriptorSetLayoutBinding ub[USER_BINDINGS], db[DRIVER_BINDINGS + NVN_STORAGE_SLOTS + 1];
    for (uint32_t b = 0; b < USER_BINDINGS; b++)
        ub[b] = (VkDescriptorSetLayoutBinding){ USER_BINDINGS * NVN_STAGE_COMPUTE + b, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    for (uint32_t b = 0; b < DRIVER_BINDINGS; b++)
        db[b] = (VkDescriptorSetLayoutBinding){ DRIVER_BINDINGS * NVN_STAGE_COMPUTE + b,
                                                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    for (uint32_t j = 0; j < NVN_STORAGE_SLOTS; j++)
        db[DRIVER_BINDINGS + j] = (VkDescriptorSetLayoutBinding){ NVN_SLOT_BINDING_BASE + j, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                                                  1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    db[DRIVER_BINDINGS + NVN_STORAGE_SLOTS] = (VkDescriptorSetLayoutBinding){
        NVN_COMPUTE_BASE_TABLE_BINDING, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL
    };
    VkDescriptorSetLayoutCreateInfo li[2] = {
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = USER_BINDINGS, .pBindings = ub },
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
          .bindingCount = DRIVER_BINDINGS + NVN_STORAGE_SLOTS + 1,
          .pBindings = db },
    };
    for (int i = 0; i < 2; i++)
        if (VK(vkCreateDescriptorSetLayout)(g.dev, &li[i], NULL, &d.cset_layout[i]) != VK_SUCCESS)
            return false;
    d.cset_layout[2] = d.set_layout[2];
    d.cset_layout[3] = d.set_layout[3];
    d.cset_layout[4] = d.set_layout[4];
    VkPushConstantRange pages = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fixed_push) };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = d.cset_layout[3] && d.set_layout[4] ? 5 : d.trace_on ? 4 : 3,
                                       .pSetLayouts = d.cset_layout,
                                       .pushConstantRangeCount = 1,
                                       .pPushConstantRanges = &pages };
    if (VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &d.clayout) != VK_SUCCESS)
        return false;
    nvn_pipe_set_layouts(d.layout, d.clayout);
    return true;
}

bool nvn_draw_layouts(void) { return layouts() && compute_layouts(); }

/** @brief The compute pipeline of mesh pass m in the mesh pass layout, with n specialization constants in map and vals. */
static VkPipeline mesh_pass_pipeline(replay *r, const module *m, uint32_t n, const VkSpecializationMapEntry *map,
                                     const uint32_t *vals)
{
    VkSpecializationMapEntry sm[MAX_SPECS];
    uint32_t sv[MAX_SPECS];
    memcpy(sm, map, n * sizeof sm[0]);
    memcpy(sv, vals, n * sizeof sv[0]);
    nvn_spec_sort(&n, sm, sv);
    nvn_core_leave();
    VkPipeline p = nvn_pipe_mesh_compute(m->m, n, sm, sv);
    if (!p)
        nvn_vk_stop(r, "the host driver cannot make a compute pipeline for %s", m->key);
    return p;
}

/** @brief What a dispatch puts into the batch being recorded. */
typedef struct dispatch_data {
    const module *m;
    const uint8_t *c0;
    VkDescriptorSet sets[3];
    /** @brief Where c[0] went in the batch (buffer NULL when the program does not read it). */
    VkDescriptorBufferInfo c0_at;
} dispatch_data;

/** @brief Puts a dispatch's constant banks into the batch being recorded (an nvn_vk_stage step). */
/**
 * @brief Where the descriptor of a compute program's base at c[0] byte off lies: the buffer (*buf) and offset (*boff)
 * of the memory from *start (a GPU address at or below the base) for *n bytes. It covers the base's pool from as far
 * below the base as the device's storage buffer range and alignment allow, so an access at any offset into the pool
 * reads what the console reads; false when no pool holds the base's address.
 */
static bool compute_base_span(const uint8_t *c0, uint32_t off, void **buf, uint64_t *boff, uint64_t *start, uint64_t *n)
{
    uint64_t gpu = rl_ld64(c0, off), o, before, rest;
    if (!gpu || !nvn_guest_buffer_rest(nvn_gpu_guest(gpu), buf, &o, &before, &rest))
        return false;
    uint64_t range = g.max_storage_range ? g.max_storage_range : UINT32_MAX, align = g.storage_align;
    if (rest > range - align)
        rest = range - align; /* room to start the descriptor at the alignment below the base */
    uint64_t below = before < range - rest ? before : range - rest;
    uint64_t lead = (o - below) % align;
    if (lead && below >= align - lead)
        below -= align - lead;
    else if (lead && below + lead <= before && below + lead + rest <= range)
        below += lead;
    else if (lead)
        return false;
    *boff = o - below;
    *start = gpu - below;
    *n = below + rest;
    return true;
}

/**
 * @brief The storage buffer over a compute program's base at c[0] byte off (module.h cbase), and in *start the low word
 * of the GPU address it starts at (the base table's word). An address of 0 is a null descriptor where the device
 * allows it.
 */
static int nvn_compute_base(replay *r, const module *m, const uint8_t *c0, uint32_t off, VkDescriptorBufferInfo *bi,
                            uint32_t *start)
{
    void *buf = NULL;
    uint64_t boff, gpu, n;
    if (!rl_ld64(c0, off) && g.null_descriptor) {
        *bi = (VkDescriptorBufferInfo){ NULL, 0, VK_WHOLE_SIZE };
        *start = 0;
        return RL_OK;
    }
    if (!compute_base_span(c0, off, &buf, &boff, &gpu, &n))
        return nvn_vk_stop(r, "compute program %s reaches the address 0x%llx in c[0] at 0x%x, which no memory pool holds "
                              "at the storage buffer alignment",
                           m->key, (unsigned long long)rl_ld64(c0, off), off);
    *bi = (VkDescriptorBufferInfo){ (VkBuffer)buf, boff, n };
    *start = (uint32_t)gpu;
    return RL_OK;
}

static int stage_dispatch(replay *r, void *arg)
{
    dispatch_data *dd = arg;
    const nvn_gfx *st = r->st;
    const module *m = dd->m;
    VkDescriptorSet *sets = dd->sets;
    int rc = RL_OK;
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = d.pool,
                                       .descriptorSetCount = 2,
                                       .pSetLayouts = d.cset_layout };
    if (VK(vkAllocateDescriptorSets)(g.dev, &ai, sets) != VK_SUCCESS) {
        if (!nvn_vk_flush(r) || VK(vkAllocateDescriptorSets)(g.dev, &ai, sets) != VK_SUCCESS)
            return nvn_vk_stop(r, "cannot allocate descriptor sets");
    }
    uint64_t flushes = g.flushes;
    sets[2] = nvn_texset();
    VkDescriptorBufferInfo bi[USER_BINDINGS + DRIVER_BINDINGS + NVN_STORAGE_SLOTS + 1];
    VkWriteDescriptorSet wr[USER_BINDINGS + DRIVER_BINDINGS + NVN_STORAGE_SLOTS + 1];
    uint32_t nw = 0;
    uint32_t starts[NVN_COMPUTE_BASE_BYTES / 8] = { 0 };
    for (uint32_t j = 0; j < m->ncbase; j++) {
        if ((rc = nvn_compute_base(r, m, dd->c0, m->cbase[j], &bi[nw], &starts[m->cbase[j] / 8])) != RL_OK)
            return rc;
        wr[nw] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = sets[1],
                                         .dstBinding = NVN_SLOT_BINDING_BASE + j,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                         .pBufferInfo = &bi[nw] };
        nw++;
    }
    if (m->cbase_table) {
        if ((rc = bank_of(r, starts, sizeof starts, &bi[nw])) != RL_OK)
            return rc;
        wr[nw] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = sets[1],
                                         .dstBinding = NVN_COMPUTE_BASE_TABLE_BINDING,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         .pBufferInfo = &bi[nw] };
        nw++;
    }
    for (uint32_t b = 0; b < DRIVER_BINDINGS; b++) {
        if (!(m->driver & (1u << b)))
            continue;
        const nvn_consts *k = &st->consts[NVN_STAGE_COMPUTE];
        if (b == 0) {
            rc = bank_of(r, dd->c0, NVN_BANK2_BYTES, &bi[nw]);
            dd->c0_at = bi[nw];
        }
        else if (b == 1)
            rc = k->addr && !nvn_vk_reading_guest(k->addr, k->size)
                     ? nvn_vk_stop(r, "the host device failed a submit")
                     : bank_of(r, k->addr ? r->mem + k->addr : NULL, k->addr ? k->size : 0, &bi[nw]);
        else
            rc = nvn_vk_stop(r, "compute program %s reads driver constant bank c[%u]", m->key, b);
        if (rc != RL_OK)
            return rc;
        wr[nw] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = sets[1],
                                         .dstBinding = DRIVER_BINDINGS * NVN_STAGE_COMPUTE + b,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         .pBufferInfo = &bi[nw] };
        nw++;
    }
    for (uint32_t b = 0; b < USER_BINDINGS; b++) {
        if (!(m->user & (1u << b)))
            continue;
        if (b >= NVN_UBO_SLOTS)
            return nvn_vk_stop(r, "compute program %s reads constant bank c[%u]", m->key, b + 3);
        uint64_t addr = st->ubo[NVN_STAGE_COMPUTE][b].addr, size = st->ubo[NVN_STAGE_COMPUTE][b].size;
        if ((rc = nvn_ubo_bind(r, NVN_STAGE_COMPUTE, b, addr, size, &bi[nw])) != RL_OK)
            return rc;
        wr[nw] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = sets[0],
                                         .dstBinding = USER_BINDINGS * NVN_STAGE_COMPUTE + b,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         .pBufferInfo = &bi[nw] };
        nw++;
    }
    if (g.flushes != flushes)
        return RL_OK;
    if (nw)
        VK(vkUpdateDescriptorSets)(g.dev, nw, wr, 0, NULL);
    return RL_OK;
}

bool nvn_compute_table(const nvn_gfx *st, const uint32_t groups[3], uint32_t nubo, uint32_t nssbo, uint32_t ntex,
                       uint32_t nimg, uint32_t end, uint8_t c0[NVN_BANK2_BYTES], uint32_t *img_lo,
                       uint32_t *table_end)
{
    memset(c0, 0, NVN_BANK2_BYTES);
    memcpy(c0 + NVN_C0_GROUPS, groups, 12);
    uint32_t at = 0x10, handles = 8 * (ntex + nimg), buffers = 16 * (nubo + nssbo);
    if (nubo > NVN_UBO_SLOTS || nssbo > NVN_STORAGE_SLOTS || ntex > NVN_IMAGE_SLOTS || nimg > NVN_IMAGE_SLOTS ||
        at + buffers + handles > end || end - at - buffers - handles > NVN_PUSH_BYTES || end > NVN_BANK2_BYTES ||
        (end > at + buffers + handles && st->push_used[NVN_STAGE_COMPUTE] & ~1u))
        return false;
    uint32_t pushed = end - at - buffers - handles;
    memcpy(c0 + at, st->push[NVN_STAGE_COMPUTE][0], pushed);
    at += pushed;
    for (uint32_t i = 0; i < nubo; i++, at += 16) {
        uint32_t size = (uint32_t)st->ubo[NVN_STAGE_COMPUTE][i].size;
        memcpy(c0 + at, &st->ubo[NVN_STAGE_COMPUTE][i].addr, 8);
        memcpy(c0 + at + 8, &size, 4);
    }
    for (uint32_t i = 0; i < nssbo; i++, at += 16) {
        uint32_t size = (uint32_t)st->storage[i].size;
        memcpy(c0 + at, &st->storage[i].addr, 8);
        memcpy(c0 + at + 8, &size, 4);
    }
    for (uint32_t i = 0; i < ntex; i++, at += 8)
        memcpy(c0 + at, st->bank2[NVN_STAGE_COMPUTE] + NVN_BANK2_TEXTURES + 8 * i, 4);
    *img_lo = at;
    for (uint32_t i = 0; i < nimg; i++, at += 8)
        memcpy(c0 + at, &st->image[NVN_STAGE_COMPUTE][i], 4);
    *table_end = at;
    return true;
}

/**
 * @brief nvnCommandBufferDispatchCompute: the bound compute program's bank
 * c[0] is the driver's per-dispatch table as the compiled code reads it:
 * word 0 is 0 and the next three hold the dispatch's group counts x, y and z
 * (NVN_C0_GROUPS; the only per-dispatch values in the driver's dispatch
 * record; the compute programs that read c[0][0..0xc] as a CUDA kernel's
 * block size also read its stack and parameter words, which NVN never
 * fills); from 0x10 come the constants
 * the game pushed (nvnCommandBufferPushComputeConstants, slot 0), then the program's uniform
 * buffers and its storage buffers, 16 bytes each ({GPU address, u32 size}),
 * then its texture handles and its image handles, 8 bytes each; the counts
 * are bytes 0xb1, 0xb2, 0xb3 and 0xb6 of the program's control section and
 * the table ends at its 16-bit word at 0xba (a program with 9 uniform buffers, 8 storage buffers and 6
 * handles ends at 0x150, past a byte), so the pushed constants fill what lies
 * before the buffers: a program with 2 uniform and 5 storage buffers and one
 * handle that ends at 0xf8 has 0x70 bytes of them at 0x10, its buffers at
 * 0x80 and its handle at 0xf0 (the offsets its translation reads them at).
 * The workgroup size comes from the control section (0x58, 0x5c, 0x60).
 */
/** @brief The GPU trace buffer and its set, made at the first traced dispatch. */
static bool trace_buffer(void)
{
    if (d.tset)
        return true;
    if (!nvn_vk_staging_new(&d.tbuf, TRACE_BUF_BYTES))
        return false;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                      .maxSets = 1,
                                      .poolSizeCount = 1,
                                      .pPoolSizes = &ps };
    if (VK(vkCreateDescriptorPool)(g.dev, &pi, NULL, &d.tpool) != VK_SUCCESS)
        return false;
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = d.tpool,
                                       .descriptorSetCount = 1,
                                       .pSetLayouts = &d.tset_layout };
    if (VK(vkAllocateDescriptorSets)(g.dev, &ai, &d.tset) != VK_SUCCESS)
        return false;
    VkDescriptorBufferInfo bi = { d.tbuf.buf, 0, TRACE_BUF_BYTES };
    VkWriteDescriptorSet wr = { .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                .dstSet = d.tset,
                                .dstBinding = NVN_TRACE_BINDING,
                                .descriptorCount = 1,
                                .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
                                .pBufferInfo = &bi };
    VK(vkUpdateDescriptorSets)(g.dev, 1, &wr, 0, NULL);
    return true;
}

/** @brief Counts a draw or dispatch: the command index the GPU trace and RELIFT_NVN_TRACE give it in its present. */
static void count_command(void)
{
    if (d.cmd_present != g.presents) {
        d.cmd_present = g.presents;
        d.cmd_index = 0;
    } else {
        d.cmd_index++;
    }
}

/**
 * @brief Which hits a GPU trace report shows: at most RELIFT_NVN_GPU_TRACE_LINES of them (default 64), those whose
 * instruction lies at RELIFT_NVN_GPU_TRACE_PCS=<from>-<to> (hex, inclusive; default every one), so a report can reach
 * past a long run of early instructions (a fragment program's handle loads) to the ones in question.
 */
void trace_window(uint32_t *lines, uint32_t *from, uint32_t *to)
{
    const char *l = getenv("RELIFT_NVN_GPU_TRACE_LINES"), *pcs = getenv("RELIFT_NVN_GPU_TRACE_PCS");
    *lines = l && *l ? (uint32_t)strtoul(l, NULL, 0) : 64u;
    *from = 0;
    *to = UINT32_MAX;
    if (pcs && *pcs) {
        char *end = NULL;
        *from = (uint32_t)strtoul(pcs, &end, 16);
        *to = end && *end == '-' ? (uint32_t)strtoul(end + 1, NULL, 16) : *from;
    }
}

/** @brief A word name for a trace source or destination: R<n>, UR<n> or c<bank>[0x<offset>]. */
static void trace_name(uint32_t id, char *s, size_t n)
{
    if (id & 0x80000000u)
        snprintf(s, n, "c%u[0x%x]", (id >> 16) & 0x7fffu, id & 0xffffu);
    else if (id == 0x200u)
        snprintf(s, n, "miss address low");
    else if (id == 0x201u)
        snprintf(s, n, "high");
    else if (id == 0x202u)
        snprintf(s, n, "handle");
    else if (id >= 0x100u)
        snprintf(s, n, "UR%u", id - 0x100u);
    else
        snprintf(s, n, "R%u", id);
}

/**
 * @brief Logs what a traced dispatch of program key found: each instruction with hits, in program order, with the
 * first hit's value, sources and invocation, and which hit came first in time (the lowest sequence number).
 */
static void trace_report(const replay *r, const char *key, uint32_t stage, uint32_t words)
{
    const uint32_t *t = (const uint32_t *)(const void *)d.tbuf.map + stage * TRACE_STAGE_WORDS;
    if (words < stage * TRACE_STAGE_WORDS + 32u)
        return;
    words -= stage * TRACE_STAGE_WORDS;
    uint32_t slots = (words - 16u) / 16u - 1u, shown = 0, first = UINT32_MAX, first_seq = UINT32_MAX;
    for (uint32_t k = 0; k < slots; k++) {
        const uint32_t *s = t + 16 + 16 * k;
        if (s[0] && s[1] < first_seq) {
            first_seq = s[1];
            first = k;
        }
    }
    if (first == UINT32_MAX) {
        rl_log(RL_LOG_INFO, "gpu-trace", "present %llu command %llu program %.8s: no hits",
               (unsigned long long)d.cmd_present, (unsigned long long)d.cmd_index, key);
        return;
    }
    static bool dumped;
    if (!dumped) {
        dumped = true;
        char why[96];
        snprintf(why, sizeof why, "first traced dispatch with hits (program %.8s)", key);
        nvn_trace_ring_dump(why, 0);
        for (uint32_t b = 0; b < NVN_UBO_SLOTS; b++) {
            uint64_t addr = r->st->ubo[NVN_STAGE_COMPUTE][b].addr, size = r->st->ubo[NVN_STAGE_COMPUTE][b].size;
            if (!addr)
                continue;
            uint32_t w[4] = { 0 };
            if (nvn_gpu_mapped(addr, 16))
                memcpy(w, rl_mem_base() + nvn_gpu_guest(addr), 16);
            char where[512];
            nvn_gpu_describe(addr, where, sizeof where);
            rl_log(RL_LOG_INFO, "gpu-trace", "  compute uniform slot %u (c[%u]): gpu 0x%llx guest 0x%llx size %llu: %08x %08x %08x %08x; %s",
                   b, b + 3, (unsigned long long)addr, (unsigned long long)nvn_gpu_guest(addr), (unsigned long long)size,
                   w[0], w[1], w[2], w[3], where);
        }
    }
    uint32_t lines, from, to;
    trace_window(&lines, &from, &to);
    for (uint32_t k = 0; k < slots && shown < lines; k++) {
        const uint32_t *s = t + 16 + 16 * k;
        if (!s[0] || s[2] < from || s[2] > to)
            continue;
        char line[512], name[32];
        float f;
        memcpy(&f, &s[4], 4);
        trace_name(s[3], name, sizeof name);
        int at = snprintf(line, sizeof line, "pc 0x%04x %s = 0x%08x (%g), %u hits, seq %u, invocation (%u, %u, %u):",
                          s[2], name, s[4], (double)f, s[0], s[1], s[5], s[6], s[7]);
        for (uint32_t j = 0; j < 4 && at > 0 && (size_t)at < sizeof line; j++) {
            if (s[8 + 2 * j] == UINT32_MAX)
                continue;
            float v;
            memcpy(&v, &s[9 + 2 * j], 4);
            trace_name(s[8 + 2 * j], name, sizeof name);
            at += snprintf(line + at, sizeof line - (size_t)at, " %s=0x%08x (%g)", name, s[9 + 2 * j], (double)v);
        }
        rl_log(RL_LOG_INFO, "gpu-trace", "present %llu command %llu program %.8s %s%s",
               (unsigned long long)d.cmd_present, (unsigned long long)d.cmd_index, key, line,
               k == first ? " [first in time]" : "");
        if (s[3] == 0x200u) {
            /* A texture or surface read through a handle from a register: the handle and what it names. */
            rl_log(RL_LOG_INFO, "gpu-trace", "present %llu command %llu program %.8s image read at pc 0x%04x: handle 0x%08x, "
                   "texture id %u, sampler id %u, %u reads",
                   (unsigned long long)d.cmd_present, (unsigned long long)d.cmd_index, key, s[2], s[4], s[4] & 0xfffffu,
                   s[4] >> 20, s[0]);
            nvn_trace_image_read(key, s[2], s[4], s[0]);
        }
        shown++;
    }
}

const char *nvn_draw_last(void) { return d.last; }

/** @brief Notes the programs of the draw or dispatch being recorded (nvn_draw_last). */
static void note_last(const char *kind, module *const *mods, uint32_t n)
{
    int at = snprintf(d.last, sizeof d.last, "%s", kind);
    for (uint32_t s = 0; s < n && at > 0 && (size_t)at < sizeof d.last; s++)
        if (mods[s])
            at += snprintf(d.last + at, sizeof d.last - (size_t)at, " %.8s", mods[s]->key);
}

bool nvn_compute_shared_words(const uint8_t *mem, uint64_t ctl, uint32_t limit, uint32_t *words)
{
    uint32_t bytes = rl_ld32(mem, ctl + NVN_CONTROL_SHARED_BYTES);
    if (bytes > limit)
        return false;
    *words = bytes ? (bytes + 3u) / 4u : 1u;
    return true;
}

static int dispatch_from(replay *r, uint32_t x, uint32_t y, uint32_t z, VkBuffer ibuf, VkDeviceSize ioff)
{
    nvn_gfx *st = r->st;
    memset(st->ubo_patched, 0, sizeof st->ubo_patched);
    count_command();
    if (!compute_layouts())
        return nvn_vk_stop(r, "cannot make the compute pipeline layout");
    if (nvn_trace_active)
        nvn_trace_begin(r, "dispatch");
    uint64_t code = st->code[NVN_STAGE_COMPUTE], ctl = st->control[NVN_STAGE_COMPUTE];
    if (!code)
        return nvn_vk_stop(r, "dispatch without a compute program");
    if (!ctl)
        return nvn_vk_stop(r, "the compute program at 0x%llx has no GLSLC control section", (unsigned long long)code);
    module *m = module_of(r, code, NVN_STAGE_COMPUTE);
    if (!m)
        return RL_PENDING;
    if (m->other_sets)
        return nvn_vk_stop(r, "compute program %s uses a guest memory set, which the layer does not bind yet", m->key);
    note_last("dispatch", &m, 1);
    {
        module *cm[NVN_STAGES] = { NULL };
        cm[NVN_STAGE_COMPUTE] = m;
        check_slots(st, cm, NVN_STAGE_COMPUTE, NVN_STAGE_COMPUTE);
    }
    if (m->trace_words && (!d.trace_on || m->trace_words * 4u > TRACE_BUF_BYTES))
        return nvn_vk_stop(r, "compute program %s is a trace-mode translation of %u trace words: run with "
                           "RELIFT_NVN_GPU_TRACE=1 (programs up to %u words)", m->key, m->trace_words,
                           TRACE_BUF_BYTES / 4u);
    if (m->trace_words && (!trace_buffer() || !nvn_vk_flush(r)))
        return nvn_vk_stop(r, "cannot make or clear the GPU trace buffer");
    if (m->trace_words)
        memset(d.tbuf.map, 0, 4u * m->trace_words);
    uint32_t nubo = r->mem[ctl + 0xb1], nssbo = r->mem[ctl + 0xb2], ntex = r->mem[ctl + 0xb3];
    uint32_t nimg = r->mem[ctl + 0xb6], end = r->mem[ctl + 0xba] | r->mem[ctl + 0xbb] << 8;
    uint8_t c0[NVN_BANK2_BYTES];
    uint32_t img_lo, at;
    if (!nvn_compute_table(st, (uint32_t[3]){ x, y, z }, nubo, nssbo, ntex, nimg, end, c0, &img_lo, &at))
        return nvn_vk_stop(r,
                           "compute program %s has a driver table of %u, %u, %u and %u entries ending at 0x%x, with "
                           "constants pushed in slots 0x%x",
                           m->key, nubo, nssbo, ntex, nimg, end, st->push_used[NVN_STAGE_COMPUTE]);
    for (uint32_t i = 0; i < nssbo; i++)
        if ((uint32_t)st->storage[i].size && !nvn_gpu_mapped(st->storage[i].addr, (uint32_t)st->storage[i].size)) {
            char where[512];
            nvn_gpu_describe(st->storage[i].addr, where, sizeof where);
            return nvn_vk_stop(r,
                               "compute program %s: storage buffer %u at 0x%llx (0x%x bytes) is not in one memory pool: %s",
                               m->key, i, (unsigned long long)st->storage[i].addr, (uint32_t)st->storage[i].size, where);
        }
    VkSpecializationMapEntry map[MAX_SPECS];
    uint32_t vals[MAX_SPECS], n = 4;
    for (uint32_t i = 0; i < 3; i++) {
        map[i] = (VkSpecializationMapEntry){ i, 4 * i, 4 };
        vals[i] = rl_ld32(r->mem, ctl + 0x58 + 4 * i);
    }
    map[3] = (VkSpecializationMapEntry){ NVN_SHARED_SPEC, 4 * 3, 4 };
    if (!nvn_compute_shared_words(r->mem, ctl, g.max_compute_shared, &vals[3]))
        return nvn_vk_stop(r, "compute program %s declares %u bytes of shared memory, more than the device's %u", m->key,
                           rl_ld32(r->mem, ctl + NVN_CONTROL_SHARED_BYTES), g.max_compute_shared);
    nvn_vk_dispatch_reset();
    for (uint32_t i = 0; i < m->nrecs; i++)
        if (nvn_dispatch_unbounded(m->recs[i].kind))
            nvn_vk_dispatch_anywhere();
    for (uint32_t i = 0; i < nssbo; i++)
        if (st->storage[i].addr)
            nvn_vk_dispatch_buffer(nvn_gpu_guest(st->storage[i].addr), (uint32_t)st->storage[i].size, true);
    for (uint32_t i = 0; i < nubo; i++)
        if (st->ubo[NVN_STAGE_COMPUTE][i].addr)
            nvn_vk_dispatch_buffer(nvn_gpu_guest(st->ubo[NVN_STAGE_COMPUTE][i].addr), st->ubo[NVN_STAGE_COMPUTE][i].size,
                                   false);
    for (uint32_t j = 0; j < m->ncbase; j++) {
        void *buf;
        uint64_t boff, gpu, bytes;
        if (compute_base_span(c0, m->cbase[j], &buf, &boff, &gpu, &bytes))
            nvn_vk_dispatch_buffer(nvn_gpu_guest(gpu), (uint32_t)(bytes > UINT32_MAX ? UINT32_MAX : bytes),
                                   m->stores_guest);
    }
    int rc = RL_OK;
    for (uint32_t i = 0; i < nssbo && rc == RL_OK; i++)
        if (st->storage[i].addr)
            rc = nvn_vk_reading_buffer(r, nvn_gpu_guest(st->storage[i].addr), (uint32_t)st->storage[i].size);
    for (uint32_t i = 0; i < nubo && rc == RL_OK; i++)
        if (st->ubo[NVN_STAGE_COMPUTE][i].addr)
            rc = nvn_vk_reading_buffer(r, nvn_gpu_guest(st->ubo[NVN_STAGE_COMPUTE][i].addr),
                                       st->ubo[NVN_STAGE_COMPUTE][i].size);
    for (uint32_t j = 0; j < m->ncbase && rc == RL_OK; j++) {
        void *buf;
        uint64_t boff, gpu, bytes;
        if (compute_base_span(c0, m->cbase[j], &buf, &boff, &gpu, &bytes))
            rc = nvn_vk_reading_buffer(r, nvn_gpu_guest(gpu), bytes);
    }
    if (rc != RL_OK)
        return rc;
    rc = m->register_handles ? nvn_texset_sync_registered(r) : RL_OK;
    if (rc != RL_OK)
        return rc;
    if ((rc = bind_handle_sources(r, m, NVN_STAGE_COMPUTE, c0, sizeof c0)) != RL_OK)
        return rc;
    if ((rc = bind_textures(r, m, 0, c0, sizeof c0, &n, map, vals, img_lo, at)) != RL_OK)
        return rc;
    if ((rc = bind_user_textures(r, m, NVN_STAGE_COMPUTE, &n, map, vals)) != RL_OK)
        return rc;
    nvn_spec_sort(&n, map, vals);
    VkPipeline p = nvn_pipe_compute(m->m, n, map, vals);
    if (!p)
        return nvn_vk_stop(r, "the host driver cannot make a compute pipeline for %s", m->key);
    nvn_pipe_record_compute(m->key, n, map, vals);
    dispatch_data dd = { .m = m, .c0 = c0, .c0_at = { 0 } };
    if ((rc = nvn_vk_stage(r, stage_dispatch, &dd, "a dispatch")) != RL_OK)
        return rc;
    for (uint32_t i = 0; i < nssbo; i++) {
        nvn_mirror_written(st->storage[i].addr, st->storage[i].size);
        if (m->stores_guest)
            nvn_vk_mark_write_range(nvn_gpu_guest(st->storage[i].addr), st->storage[i].size);
    }
    uint32_t storage_at = end - 8 * (ntex + nimg) - 16 * nssbo;
    for (uint32_t j = 0; m->stores_guest && j < m->ncbase; j++) {
        void *buf;
        uint64_t boff, gpu, bytes;
        uint32_t off = m->cbase[j];
        if (off >= storage_at && off < storage_at + 16 * nssbo && (off - storage_at) % 16 == 0)
            continue; /* a storage buffer of the dispatch, marked whole above */
        if (compute_base_span(c0, off, &buf, &boff, &gpu, &bytes)) {
            nvn_mirror_written(gpu, bytes);
            nvn_vk_mark_write_range(nvn_gpu_guest(gpu), bytes);
        }
    }
    if (m->marks_pages)
        nvn_mirror_written_marked();
    else if (m->global_store && !nvn_mirror_takes_unmarked())
        return nvn_mirror_refuse_unmarked(r, m->key);
    if (ibuf)
        nvn_vk_begin(r);
    else
        nvn_vk_begin_dispatch(r);
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.clayout, 0, 3, dd.sets, 0, NULL);
    bind_pools(VK_PIPELINE_BIND_POINT_COMPUTE, d.clayout, d.cset_layout[4]);
    if (m->trace_words)
        VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.clayout, NVN_TRACE_SET, 1, &d.tset, 0, NULL);
    fixed_push fp = { .written_pages = nvn_mirror_written_pages_address() };
    VK(vkCmdPushConstants)(g.cb, d.clayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof fp, &fp);
    if (nvn_label_on()) {
        const char *keys[6] = { NULL, NULL, NULL, NULL, NULL, m->key };
        nvn_label_draw(g.cb, "dispatch", keys);
    }
    if (ibuf) {
        nvn_vk_dispatch_indirect(g.cb, ibuf, ioff, dd.c0_at.buffer, dd.c0_at.offset + NVN_C0_GROUPS);
    } else {
        VK(vkCmdDispatch)(g.cb, x, y, z);
    }
    nvn_vk_dispatched();
    if (m->trace_words) {
        if (!nvn_vk_flush(r))
            return nvn_vk_stop(r, "the host device failed the traced dispatch of %s", m->key);
        trace_report(r, m->key, NVN_STAGE_COMPUTE, m->trace_words);
    }
    if (nvn_gpuprof_on()) {
        char k[16];
        snprintf(k, sizeof k, "cs %.8s", m->key);
        nvn_gpuprof_note(k);
    }
    if (nvn_trace_active) {
        const nvn_consts *k = &st->consts[NVN_STAGE_COMPUTE];
        nvn_trace_cmd tc = { .groups = { x, y, z },
                             .c0 = c0,
                             .c0_bytes = sizeof c0,
                             .c1_guest = k->addr,
                             .c1_size = k->size,
                             .spec_map = map,
                             .spec_vals = vals,
                             .nspec = n };
        tc.keys[NVN_STAGE_COMPUTE] = m->key;
        nvn_trace_end(r, &tc);
    }
    return RL_OK;
}

int nvn_cuda_dispatch(replay *r, const char *key, const uint8_t *params, uint32_t nparams, const uint32_t grid[3],
                      const uint32_t block[3], uint32_t dynamic_shared)
{
    char why[256] = "";
    count_command();
    if (!compute_layouts())
        return nvn_vk_stop(r, "cannot make the compute pipeline layout");
    module *m = nvn_module_get(key, NVN_STAGE_COMPUTE, why, sizeof why);
    if (!m)
        return nvn_vk_stop(r, "CUDA kernel %s: %s", key, why);
    if (m->other_sets || m->user)
        return nvn_vk_stop(r, "CUDA kernel %s reads %s, which the CUDA launch does not bind yet", key,
                           m->other_sets ? "a guest memory set" : "a constant bank of its module");
    note_last("cuda launch", &m, 1);
    uint8_t c0[NVN_BANK2_BYTES];
    memset(c0, 0, sizeof c0);
    if (nparams > sizeof c0 - NVN_CUDA_PARAMS)
        return nvn_vk_stop(r, "CUDA kernel %s takes %u parameter bytes, more than c[0] holds", key, nparams);
    for (int i = 0; i < 3; i++) {
        rl_st32(c0, 4u * (uint32_t)i, block[i]);
        rl_st32(c0, 0xcu + 4u * (uint32_t)i, grid[i]);
    }
    rl_st32(c0, 0x28, NVN_CUDA_LOCAL_BYTES);
    memcpy(c0 + NVN_CUDA_PARAMS, params, nparams);
    VkSpecializationMapEntry map[MAX_SPECS];
    uint32_t vals[MAX_SPECS], n = 4;
    for (uint32_t i = 0; i < 3; i++) {
        map[i] = (VkSpecializationMapEntry){ i, 4 * i, 4 };
        vals[i] = block[i];
    }
    map[3] = (VkSpecializationMapEntry){ NVN_SHARED_SPEC, 4 * 3, 4 };
    if (!nvn_cuda_shared_words(m, dynamic_shared, g.max_compute_shared, &vals[3]))
        return nvn_vk_stop(r, "CUDA kernel %s: no declared shared size, or %u dynamic bytes past the device's %u", key,
                           dynamic_shared, g.max_compute_shared);
    nvn_vk_dispatch_reset();
    for (uint32_t i = 0; i < m->nrecs; i++)
        if (nvn_dispatch_unbounded(m->recs[i].kind))
            nvn_vk_dispatch_anywhere();
    for (uint32_t j = 0; j < m->ncbase; j++) {
        void *buf;
        uint64_t boff, gpu, bytes;
        if (compute_base_span(c0, m->cbase[j], &buf, &boff, &gpu, &bytes))
            nvn_vk_dispatch_buffer(nvn_gpu_guest(gpu), (uint32_t)(bytes > UINT32_MAX ? UINT32_MAX : bytes),
                                   m->stores_guest);
    }
    int rc = m->register_handles ? nvn_texset_sync_registered(r) : RL_OK;
    if (rc != RL_OK)
        return rc;
    if ((rc = bind_handle_sources(r, m, NVN_STAGE_COMPUTE, c0, sizeof c0)) != RL_OK)
        return rc;
    nvn_spec_sort(&n, map, vals);
    VkPipeline p = nvn_pipe_compute(m->m, n, map, vals);
    if (!p)
        return nvn_vk_stop(r, "the host driver cannot make a compute pipeline for CUDA kernel %s", m->key);
    nvn_pipe_record_compute(m->key, n, map, vals);
    dispatch_data dd = { .m = m, .c0 = c0, .c0_at = { 0 } };
    if ((rc = nvn_vk_stage(r, stage_dispatch, &dd, "a CUDA launch")) != RL_OK)
        return rc;
    for (uint32_t j = 0; m->stores_guest && j < m->ncbase; j++) {
        void *buf;
        uint64_t boff, gpu, bytes;
        if (compute_base_span(c0, m->cbase[j], &buf, &boff, &gpu, &bytes)) {
            nvn_mirror_written(gpu, bytes);
            nvn_vk_mark_write_range(nvn_gpu_guest(gpu), bytes);
        }
    }
    if (m->marks_pages)
        nvn_mirror_written_marked();
    else if (m->global_store)
        nvn_mirror_written_all();
    nvn_vk_begin_dispatch(r);
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.clayout, 0, 3, dd.sets, 0, NULL);
    bind_pools(VK_PIPELINE_BIND_POINT_COMPUTE, d.clayout, d.cset_layout[4]);
    fixed_push fp = { .written_pages = nvn_mirror_written_pages_address() };
    VK(vkCmdPushConstants)(g.cb, d.clayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof fp, &fp);
    VK(vkCmdDispatch)(g.cb, grid[0], grid[1], grid[2]);
    nvn_vk_dispatched();
    return RL_OK;
}

static int dispatch(replay *r, uint32_t x, uint32_t y, uint32_t z) { return dispatch_from(r, x, y, z, NULL, 0); }

/**
 * @brief nvnCommandBufferDispatchComputeIndirect(NVNcommandBuffer *, NVNbufferAddress): the group counts x, y and z
 * are the three u32 at the GPU address indirect, which the GPU reads when the dispatch runs (an earlier dispatch in
 * the same command buffer may write them). The host dispatch reads them from the pool's buffer, and a copy of them
 * fills c[0]'s first 12 bytes, where the driver's per-dispatch table has them. The barrier after every dispatch makes
 * earlier writes visible to both. Counts outside one memory pool stop, naming what holds the address.
 */
static int dispatch_indirect(replay *r, uint64_t indirect)
{
    void *buf = NULL;
    uint64_t off = 0;
    if (!nvn_gpu_mapped(indirect, 12) || !nvn_guest_buffer(nvn_gpu_guest(indirect), 12, &buf, &off)) {
        char where[512];
        nvn_gpu_describe(indirect, where, sizeof where);
        return nvn_vk_stop(r, "an indirect dispatch's group counts at 0x%llx are in no one pool: %s",
                           (unsigned long long)indirect, where);
    }
    return dispatch_from(r, 0, 0, 0, (VkBuffer)buf, off);
}

/**
 * @brief The stage of the program at code whose GLSLC control section is at ctl (0 for none): the stage byte of the
 * section when it names the task or mesh stage (their SPH types are the vertex and tessellation evaluation ones, and
 * only the section tells them apart), else the SPH's.
 */
static int stage_of(const replay *r, uint64_t code, uint64_t ctl)
{
    if (ctl && nvn_generation() != 1) {
        uint8_t s = r->mem[ctl + GLSLC_STAGE_BYTE];
        if (s == NVN_STAGE_TASK || s == NVN_STAGE_MESH)
            return s;
    }
    return nvn_shader_stage(r->mem, code);
}

static int bind_program(replay *r, const nvn_cmd *e)
{
    uint32_t n = 0, mask = (uint32_t)e->x[2] & 0xffu;
    const nvn_consts *consts = NULL;
    const uint64_t *control = NULL;
    const uint64_t *code = e->x[1] ? nvn_program_code(e->x[1], &n, &consts, &control) : NULL;
    if (e->x[1] && !code)
        return nvn_vk_stop(r, "nvnCommandBufferBindProgram: 0x%llx is not a program", (unsigned long long)e->x[1]);
    if (mask & (1u << 1))
        r->st->sample_shading = e->x[1] ? nvn_program_sample_shading(e->x[1]) : 0;
    for (uint32_t s = 0; s < NVN_STAGES; s++) {
        if (!(mask & (1u << s)))
            continue;
        r->st->code[s] = 0;
        r->st->consts[s] = (nvn_consts){ 0, 0 };
        r->st->control[s] = 0;
        for (uint32_t i = 0; i < n; i++)
            if (stage_of(r, code[i], control ? control[i] : 0) == (int)s) {
                r->st->code[s] = code[i];
                r->st->consts[s] = consts[i];
                r->st->control[s] = control[i];
            }
    }
    return RL_OK;
}

static int keep(replay *r, const nvn_cmd *e, unsigned reg, void *dst, size_t n, bool *have)
{
    const void *b = nvn_cmd_bytes(r->s, e->x[reg]);
    if (!b)
        return nvn_vk_stop(r, "%s without its state", nvn_cmd_name(e->op));
    memcpy(dst, b, n);
    if (have)
        *have = true;
    return RL_OK;
}

static void faces(uint32_t face, uint32_t v, uint32_t out[2])
{
    if (face & 1)
        out[0] = v;
    if (face & 2)
        out[1] = v;
}

/** @brief The single bind a bulk bind (BindUniformBuffers, BindTextures and so on) makes of each slot, or NVN_CMD_NONE. */
static uint32_t single_of(uint32_t op)
{
    switch (op) {
    case NVN_CMD_BIND_UNIFORM_BUFFERS:
        return NVN_CMD_BIND_UNIFORM_BUFFER;
    case NVN_CMD_BIND_STORAGE_BUFFERS:
        return NVN_CMD_BIND_STORAGE_BUFFER;
    case NVN_CMD_BIND_VERTEX_BUFFERS:
        return NVN_CMD_BIND_VERTEX_BUFFER;
    case NVN_CMD_BIND_TEXTURES:
        return NVN_CMD_BIND_TEXTURE;
    case NVN_CMD_BIND_IMAGES:
        return NVN_CMD_BIND_IMAGE;
    case NVN_CMD_BIND_SEPARATE_TEXTURES:
        return NVN_CMD_BIND_SEPARATE_TEXTURE;
    case NVN_CMD_BIND_SEPARATE_SAMPLERS:
    case NVN_CMD_BIND_SEPARATE_SAMPLERS_DEFERRED:
        return NVN_CMD_BIND_SEPARATE_SAMPLER;
    case NVN_CMD_BIND_UNIFORM_BUFFERS_DEFERRED:
        return NVN_CMD_BIND_UNIFORM_BUFFER;
    case NVN_CMD_BIND_STORAGE_BUFFERS_DEFERRED:
        return NVN_CMD_BIND_STORAGE_BUFFER;
    case NVN_CMD_BIND_TEXTURES_DEFERRED:
        return NVN_CMD_BIND_TEXTURE;
    case NVN_CMD_BIND_IMAGES_DEFERRED:
        return NVN_CMD_BIND_IMAGE;
    case NVN_CMD_BIND_SEPARATE_TEXTURES_DEFERRED:
        return NVN_CMD_BIND_SEPARATE_TEXTURE;
    default:
        return NVN_CMD_NONE;
    }
}

/**
 * @brief The single binds of count slots from first, element i taken from v (16 bytes of buffer address and size, or
 * 8 bytes of handle, or for vertex buffers 16 bytes of address and size at stream first + i), as the e->x[0] command
 * buffer's binds of each slot in turn.
 */
static int bind_slots(replay *r, const nvn_cmd *e, uint32_t single, uint64_t first, uint32_t count, const uint8_t *v)
{
    bool vertex = single == NVN_CMD_BIND_VERTEX_BUFFER,
         buffers = single == NVN_CMD_BIND_UNIFORM_BUFFER || single == NVN_CMD_BIND_STORAGE_BUFFER;
    for (uint32_t i = 0; i < count; i++) {
        nvn_cmd one = { .op = single, .x = { e->x[0], vertex ? first + i : e->x[1], vertex ? 0 : first + i } };
        if (vertex) {
            memcpy(&one.x[2], v + 16u * i, 16);
        } else if (buffers) {
            memcpy(&one.x[3], v + 16u * i, 16);
        } else {
            memcpy(&one.x[3], v + 8u * i, 8);
        }
        int rc = nvn_draw_cmd(r, &one);
        if (rc != RL_OK)
            return rc;
    }
    return RL_OK;
}

/**
 * @brief The bulk binds (nvnCommandBufferBindUniformBuffers, BindStorageBuffers, BindVertexBuffers, BindTextures,
 * BindImages, BindSeparateTextures and BindSeparateSamplers: a stage, a first slot, a count and an array of 16 byte
 * buffer ranges or 8 byte handles) as the single binds of each slot in turn.
 */
static int bind_many(replay *r, const nvn_cmd *e)
{
    uint32_t single = single_of(e->op);
    unsigned reg = single == NVN_CMD_BIND_VERTEX_BUFFER ? 1 : 2;
    uint32_t count = (uint32_t)e->x[reg + 1];
    const uint8_t *v = nvn_cmd_bytes(r->s, e->x[reg + 2]);
    if (count && !v)
        return nvn_vk_stop(r, "%s without its array", nvn_cmd_name(e->op));
    return bind_slots(r, e, single, e->x[reg], count, v);
}

/** @brief The most slots a deferred bind may name: the arrays the console's binds fill have far fewer. */
#define DEFERRED_SLOTS 0x1000u

/**
 * @brief The deferred binds (nvnCommandBufferBindTexturesDeferred, BindImagesDeferred, BindSeparateTexturesDeferred,
 * BindSeparateSamplersDeferred, BindUniformBuffersDeferred and BindStorageBuffersDeferred: a stage, a first slot, a
 * count and the GPU address of the array of buffer ranges or handles). The driver records only the address and the
 * GPU reads the array when it runs the command, so the array is read here, as the replay reaches the command, not
 * when the guest recorded it, and then bound as the same single binds of each slot.
 */
static int bind_deferred(replay *r, const nvn_cmd *e)
{
    uint32_t single = single_of(e->op), count = (uint32_t)e->x[3];
    uint64_t addr = e->x[4];
    if (count > DEFERRED_SLOTS)
        return nvn_vk_stop(r, "%s of %u slots", nvn_cmd_name(e->op), count);
    if (count && !addr)
        return nvn_vk_stop(r, "%s without its array", nvn_cmd_name(e->op));
    if (count && !nvn_vk_reading_guest(nvn_gpu_guest(addr), (uint64_t)count * 16))
        return nvn_vk_stop(r, "the host device failed a submit");
    return bind_slots(r, e, single, e->x[2], count, count ? r->mem + nvn_gpu_guest(addr) : NULL);
}

/**
 * @brief The separate texture or separate sampler (the e->op) of stage x1, slot x2: the handle x3 goes where the
 * driver writes it in the stage's c[2] and the slot is marked bound. A compute stage's separate handles go to a
 * place of the driver's dispatch table the layer does not know, so they stop.
 */
static int bind_separate(replay *r, const nvn_cmd *e)
{
    bool sampler = e->op == NVN_CMD_BIND_SEPARATE_SAMPLER;
    uint32_t s = (uint32_t)e->x[1], slot = (uint32_t)e->x[2], h = (uint32_t)e->x[3];
    if (s >= NVN_STAGES || slot >= NVN_SEPARATE_SLOTS)
        return nvn_vk_stop(r, "separate %s of stage %u slot %u", sampler ? "sampler" : "texture", s, slot);
    if (s == NVN_STAGE_COMPUTE)
        return nvn_vk_stop(r, "separate %s bound for the compute stage, whose dispatch table the layer knows no place for them in",
                           sampler ? "sampler" : "texture");
    uint32_t at = (sampler ? NVN_BANK2_SEPARATE_SAMPLERS : NVN_BANK2_SEPARATE_TEXTURES) + 8u * slot;
    memcpy(r->st->bank2[s] + at, &h, 4);
    r->st->separate_bound[s][sampler][slot / 32] |= 1u << (slot % 32);
    return RL_OK;
}

static void palette_seen(const uint8_t pal[16]);

bool nvn_shading_rate_khr_has(uint8_t rate) { return rate >= 5 && rate <= 11; }

/**
 * @brief The shading-rate image runs on the host as on the console with VK_NV_shading_rate_image, whose palette
 * entries are the hardware's rate codes (NVIDIA's Ampere class header, open-gpu-doc clc797.h,
 * SET_VARIABLE_PIXEL_RATE_SHADING_INDEX_TO_RATE: 0 cull, 1 to 4 per sample, 5 one per pixel, 6 to 11 one per 2x1 to
 * 4x4 pixels, the order of VkShadingRatePaletteEntryNV). The image is the bound R8_UINT texture, one index per 16x16
 * pixels (the host checks its texel size is 16x16; inferred: the console's Ampere tile matches the 3090's). The
 * console picks the palette by viewport index or by the VPRS table index a program writes
 * (SET_VARIABLE_PIXEL_RATE_SHADING_TABLE_SELECT); relift draws one viewport, and the translated programs write no
 * table index, which reads as 0 (inferred), so both pick viewport 0's palette, the one the host gets. Without the
 * extension every pixel is shaded, which is not the console's result; each palette is logged once with whether
 * VK_KHR_fragment_shading_rate could give its rates.
 */
int nvn_shading_rate_state(replay *r, nvn_gfx *st, uint32_t *on)
{
    *on = 0;
    if (!st->shading.image || !(st->shading.enabled & 1u) || !g.shading_rate_image)
        return RL_OK;
    vimg *v = nvn_vk_image(r, st->shading.image);
    if (!v)
        return RL_PENDING;
    VkImageView view = nvn_vk_rate_view(v);
    if (!view)
        return nvn_vk_stop(r, "shading-rate image 0x%llx is not a 2D R8_UINT texture the host can use as one",
                           (unsigned long long)st->shading.image);
    if (!nvn_vk_refresh(r, v))
        return nvn_vk_stop(r, "cannot refill shading-rate image 0x%llx from its storage",
                           (unsigned long long)st->shading.image);
    st->shading.view = view;
    *on = 1;
    return RL_OK;
}

void nvn_shading_rate_bind(const nvn_gfx *st)
{
    uint32_t entries[16];
    for (uint32_t i = 0; i < 16; i++)
        entries[i] = st->shading.palette[0][i];
    struct {
        uint32_t count;
        const uint32_t *entries;
    } pal = { 16, entries };
    g.bind_shading_rate_image(g.cb, st->shading.view, VK_IMAGE_LAYOUT_GENERAL);
    g.set_shading_rate_palette(g.cb, 0, 1, &pal);
}

/**
 * @brief nvnCommandBufferShadingRateImagePalette(NVNcommandBuffer *, int viewport, int count, const int *rates): the
 * driver packs a 16-entry palette of four-bit rates for the viewport, rates[0..count) then 5 (one invocation per pixel)
 * for the rest, all 5 and the palette off when rates is NULL. Draws use it through nvn_shading_rate_state.
 */
static int shading_rate_palette(replay *r, const nvn_cmd *e)
{
    nvn_gfx *st = r->st;
    uint32_t vp = (uint32_t)e->x[1] & 0xfu;
    int32_t n = (int32_t)e->x[2];
    const uint8_t *rates = nvn_cmd_bytes(r->s, e->x[3]);
    bool coarse = false;
    for (uint32_t i = 0; i < 16; i++) {
        uint8_t v = rates && (int32_t)i < n ? (uint8_t)(rl_ld32(rates, 4 * i) & 0xfu) : 5u;
        st->shading.palette[vp][i] = v;
        coarse |= v != 5u;
    }
    if (rates)
        st->shading.enabled |= (uint16_t)(1u << vp);
    else
        st->shading.enabled &= (uint16_t)~(1u << vp);
    if (rates && coarse && !g.shading_rate_image)
        nvn_stats_mark(NVN_STATS_PARTIAL);
    if (rates)
        palette_seen(st->shading.palette[vp]);
    return RL_OK;
}

/** @brief Logs each distinct palette once per run (up to 32): which rates the game uses, and which the host lacks. */
static void palette_seen(const uint8_t pal[16])
{
    static uint64_t seen[32];
    static uint32_t nseen;
    uint64_t key = 0;
    char text[64], *p = text;
    bool khr = true;
    for (uint32_t i = 0; i < 16; i++) {
        key = key << 4 | pal[i];
        p += snprintf(p, 4, "%x", pal[i]);
        khr = khr && nvn_shading_rate_khr_has(pal[i]);
    }
    for (uint32_t i = 0; i < nseen; i++)
        if (seen[i] == key)
            return;
    if (nseen == 32)
        return;
    seen[nseen++] = key;
    const char *host = g.shading_rate_image ? "applied exactly (VK_NV_shading_rate_image)"
                       : khr ? "not applied: every pixel is shaded; VK_KHR_fragment_shading_rate could give these rates"
                             : "not applied: every pixel is shaded; it has rates VK_KHR_fragment_shading_rate lacks";
    rl_log(RL_LOG_INFO, "nvn", "shading-rate palette %s (index 0 first): %s", text, host);
}

/** @brief nvn_separate_handles: the bound separate texture words get their slots, the bound sampler ids their sampler. */
int nvn_separate_handles(replay *r, uint32_t s)
{
    nvn_gfx *st = r->st;
    for (uint32_t i = 0; i < NVN_SEPARATE_SLOTS; i++) {
        uint8_t *word = st->bank2_host[s] + NVN_BANK2_SEPARATE_TEXTURES + 8u * i;
        if (st->separate_bound[s][0][i / 32] & 1u << (i % 32)) {
            uint32_t handle, slot;
            memcpy(&handle, word, 4);
            int rc = nvn_texset_separate_texture(r, handle, &slot);
            if (rc != RL_OK)
                return rc;
            slot = slot == UINT32_MAX ? 0 : (handle & 0xfff00000u) | slot;
            memcpy(word, &slot, 4);
        }
        if (st->separate_bound[s][1][i / 32] & 1u << (i % 32)) {
            uint32_t handle;
            memcpy(&handle, st->bank2_host[s] + NVN_BANK2_SEPARATE_SAMPLERS + 8u * i, 4);
            int rc = nvn_texset_sampler(r, handle >> 20);
            if (rc != RL_OK)
                return rc;
        }
    }
    return RL_OK;
}

/**
 * @brief nvnCommandBufferSetConservativeRasterMode(cb, mode, n): the driver (SDK 22.2.0, sdk 0x684470 of one game)
 * calls macro 0xe60 with p = (n & 0xf) << 16 | (mode 0: 0, mode 2: 0x1100, any other: 0x1110), and the macro writes
 * SET_CONSERVATIVE_RASTER_CONTROL = (p & ~0xf) | (current & 0xf), keeping the bloat (decoded from the driver's macro
 * code). By NVIDIA's Ampere class header (open-gpu-doc clc797.h): bits 10:8 the triangle snap mode, 14:12 the line and
 * point snap mode (0 post-snap, 1 pre-snap), bit 4 copy inner to outer, 19:16 the uncertainty region size, so mode 0
 * is post-snap and the others pre-snap.
 */
/**
 * @brief nvnCommandBufferSetGeometryProcessingMode(cb, mode) calls macro 0xe26 with the mode, then 0xed6 and 0xe3e
 * with 0 (one game's sdk 0x681400). Run on the driver's own macros over its device-init state (the xfb lane's MME
 * decoder): mode 0 writes nothing over the init state, the vertex pipeline; modes 1 and 2 set SET_MESH_CONTROL to 1
 * and SET_GS_MODE to 1, turn the vertex, tessellation and geometry pipeline shaders off (SET_PIPELINE_SHADER 0 to
 * 4), clear the post-VTG attribute skip masks and bind the stage constant buffer groups; 0xed6 writes
 * SET_OFFSET_RENDER_TARGET_INDEX 0 and 0xe3e SET_USER_CLIP_OP 0, both the init values, for every mode. relift picks
 * the pipeline by the draw command, so what the mode changes is which draws run: vertex draws in mode 0, mesh draws
 * in 1 and 2 (one game's frame trace: 1020 vertex draws all in mode 0, 49 mesh draws all in 1 or 2). A draw of the
 * other kind has the console run its stages with the pipeline off, which relift does not model, so it stops; any
 * other mode stops at its first draw.
 */
bool nvn_geometry_mode_draws(uint32_t mode, bool mesh) { return mesh ? mode == 1 || mode == 2 : mode == 0; }

uint32_t nvn_conservative_control(uint32_t cur, uint32_t mode, uint32_t n)
{
    uint32_t p = (n & 0xfu) << 16 | (mode == 0 ? 0u : mode == 2 ? 0x1100u : 0x1110u);
    return (p & ~0xfu) | (cur & 0xfu);
}

/**
 * @brief Post-snap is what VK_EXT_conservative_rasterization rasterizes; the uncertainty region size is not checked,
 * as it sizes the region around unsnapped vertices that only pre-snap rasterization uses (inferred from the class
 * header's grouping of the field with the snap modes; no capture compares two sizes in post-snap).
 */
bool nvn_conservative_control_is_post_snap(uint32_t control) { return (control & 0x7710u) == 0; }

uint32_t nvn_indirect_stride(bool indexed, int64_t stride)
{
    return stride ? (uint32_t)stride : indexed ? 20u : 16u;
}

void nvn_indirect_record(const uint8_t *rec, bool indexed, nvn_indirect_draw *out)
{
    out->count = rl_ld32(rec, 0);
    out->instances = rl_ld32(rec, 4);
    out->first = rl_ld32(rec, 8);
    out->base_vertex = indexed ? (int32_t)rl_ld32(rec, 12) : 0;
    out->base_instance = rl_ld32(rec, indexed ? 16 : 12);
}

/**
 * @brief nvnCommandBufferDrawArraysIndirect(cb, mode, indirect), DrawElementsIndirect(cb, mode, type, index, indirect),
 * MultiDrawArraysIndirectCount(cb, mode, indirect, parameter, max, stride) and MultiDrawElementsIndirectCount(cb, mode,
 * type, index, indirect, parameter, max, stride). The records at the GPU address indirect are the GL indirect commands
 * NVN takes (NVNdrawArraysIndirectData: count, instance count, first, base instance; NVNdrawElementsIndirectData: count,
 * instance count, first index, base vertex, base instance), stride bytes apart (0: packed, 16 or 20 bytes); the multi
 * forms draw the smaller of max and the u32 at parameter. The GPU reads them when the draw runs, after the work before
 * it (which may write them), so the replay submits and waits for that work first, then draws each record as the direct
 * draw with the same values: first index i reads the index buffer from index + i * the index size. Records or a count
 * outside one memory pool stop, naming what holds the address.
 */
static int draw_indirect(replay *r, uint32_t mode, bool indexed, uint32_t itype, uint64_t index, uint64_t indirect,
                         uint64_t count_addr, uint32_t max, uint64_t stride_in)
{
    nvn_gfx *st = r->st;
    uint32_t stride = nvn_indirect_stride(indexed, (int64_t)stride_in);
    uint32_t size = indexed ? 20u : 16u;
    if (!nvn_vk_flush(r))
        return nvn_vk_stop(r, "the host device failed a submit");
    uint32_t n = max;
    if (count_addr) {
        if (!nvn_gpu_mapped(count_addr, 4)) {
            char where[512];
            nvn_gpu_describe(count_addr, where, sizeof where);
            return nvn_vk_stop(r, "an indirect draw's count at 0x%llx is in no pool: %s",
                               (unsigned long long)count_addr, where);
        }
        uint32_t c = rl_ld32(r->mem + nvn_gpu_guest(count_addr), 0);
        n = c < max ? c : max;
    }
    if (n && !nvn_gpu_mapped(indirect, (uint64_t)(n - 1) * stride + size)) {
        char where[512];
        nvn_gpu_describe(indirect, where, sizeof where);
        return nvn_vk_stop(r, "an indirect draw's %u records of %u bytes at 0x%llx are in no one pool: %s", n, stride,
                           (unsigned long long)indirect, where);
    }
    for (uint32_t i = 0; i < n; i++) {
        nvn_indirect_draw rec;
        nvn_indirect_record(r->mem + nvn_gpu_guest(indirect + (uint64_t)i * stride), indexed, &rec);
        if (!rec.instances || !rec.count)
            continue;
        st->inst_base = rec.base_instance;
        st->inst_count = rec.instances;
        int rc = indexed ? draw(r, mode, true, itype, index + ((uint64_t)rec.first << (itype > 2 ? 2 : itype)),
                                rec.base_vertex, 0, rec.count)
                         : draw(r, mode, false, 0, 0, 0, rec.first, rec.count);
        if (rc != RL_OK)
            return rc;
    }
    return RL_OK;
}

/**
 * @brief nvnCommandBufferSetDepthBounds(NVNcommandBuffer *, NVNboolean enable, float n, float f): with the test on, a
 * fragment whose depth target already holds a value outside [n, f] is discarded, the test Vulkan's depth bounds test
 * makes (the stored value, not the fragment's). The range goes to the host clamped to [0, 1], which Vulkan requires
 * without VK_EXT_depth_range_unrestricted; stored depth is in [0, 1] there, so the result is the same. A draw with no
 * depth target passes the test, as in Vulkan.
 */
static int set_depth_bounds(nvn_gfx *st, const nvn_cmd *e)
{
    st->depth_bounds = (e->x[1] & 0xff) != 0;
    memcpy(&st->depth_bounds_range[0], &e->d[0], 4);
    memcpy(&st->depth_bounds_range[1], &e->d[1], 4);
    return RL_OK;
}

int nvn_draw_cmd(replay *r, const nvn_cmd *e)
{
    nvn_gfx *st = r->st;
    switch (e->op) {
    case NVN_CMD_BIND_UNIFORM_BUFFERS:
    case NVN_CMD_BIND_STORAGE_BUFFERS:
    case NVN_CMD_BIND_VERTEX_BUFFERS:
    case NVN_CMD_BIND_TEXTURES:
    case NVN_CMD_BIND_IMAGES:
    case NVN_CMD_BIND_SEPARATE_TEXTURES:
    case NVN_CMD_BIND_SEPARATE_SAMPLERS:
        return bind_many(r, e);
    case NVN_CMD_BIND_TEXTURES_DEFERRED:
    case NVN_CMD_BIND_IMAGES_DEFERRED:
    case NVN_CMD_BIND_SEPARATE_TEXTURES_DEFERRED:
    case NVN_CMD_BIND_SEPARATE_SAMPLERS_DEFERRED:
    case NVN_CMD_BIND_UNIFORM_BUFFERS_DEFERRED:
    case NVN_CMD_BIND_STORAGE_BUFFERS_DEFERRED:
        return bind_deferred(r, e);
    case NVN_CMD_BIND_SEPARATE_TEXTURE:
    case NVN_CMD_BIND_SEPARATE_SAMPLER:
        return bind_separate(r, e);
    case NVN_CMD_BIND_PROGRAM:
        return bind_program(r, e);
    case NVN_CMD_BIND_UNIFORM_BUFFER: {
        uint32_t s = (uint32_t)e->x[1], slot = (uint32_t)e->x[2];
        if (s >= NVN_STAGES || slot >= NVN_UBO_SLOTS)
            return nvn_vk_stop(r, "uniform buffer of stage %u slot %u", s, slot);
        st->ubo[s][slot].addr = e->x[3];
        st->ubo[s][slot].size = e->x[4];
        return RL_OK;
    }
    case NVN_CMD_BIND_TEXTURE: {
        uint32_t s = (uint32_t)e->x[1], slot = (uint32_t)e->x[2], h = (uint32_t)e->x[3];
        if (s >= NVN_STAGES || NVN_BANK2_TEXTURES + 8u * slot + 4 > NVN_BANK2_BYTES)
            return nvn_vk_stop(r, "texture of stage %u slot %u", s, slot);
        memcpy(st->bank2[s] + NVN_BANK2_TEXTURES + 8u * slot, &h, 4);
        return RL_OK;
    }
    case NVN_CMD_BIND_STORAGE_BUFFER: {
        uint32_t s = (uint32_t)e->x[1], slot = (uint32_t)e->x[2];
        if (s >= NVN_STAGES)
            return nvn_vk_stop(r, "storage buffer of stage %u", s);
        if (slot >= NVN_STORAGE_SLOTS)
            return nvn_vk_stop(r, "storage buffer slot %u", slot);
        if (s == NVN_STAGE_COMPUTE) {
            st->storage[slot].addr = e->x[3];
            st->storage[slot].size = e->x[4];
        } else {
            st->gstorage[s][slot].addr = e->x[3];
            st->gstorage[s][slot].size = e->x[4];
        }
        return RL_OK;
    }
    case NVN_CMD_BIND_IMAGE: {
        uint32_t s = (uint32_t)e->x[1], slot = (uint32_t)e->x[2];
        if (s >= NVN_STAGES)
            return nvn_vk_stop(r, "image of stage %u", s);
        if (slot >= NVN_IMAGE_SLOTS)
            return nvn_vk_stop(r, "image slot %u", slot);
        if (s == NVN_STAGE_COMPUTE) {
            st->image[s][slot] = (uint32_t)e->x[3];
        } else {
            uint32_t h = (uint32_t)e->x[3];
            memcpy(st->bank2[s] + NVN_BANK2_IMAGES + 8u * slot, &h, 4);
        }
        return RL_OK;
    }
    case NVN_CMD_DISPATCH_COMPUTE:
        return dispatch(r, (uint32_t)e->x[1], (uint32_t)e->x[2], (uint32_t)e->x[3]);
    case NVN_CMD_DISPATCH_COMPUTE_INDIRECT:
        return dispatch_indirect(r, e->x[1]);
    case NVN_CMD_BIND_VERTEX_BUFFER: {
        uint32_t i = (uint32_t)e->x[1];
        if (i >= NVN_VERTEX_BUFFERS)
            return nvn_vk_stop(r, "vertex buffer %u", i);
        st->vb[i].addr = e->x[2];
        st->vb[i].size = e->x[3];
        return RL_OK;
    }
    case NVN_CMD_BIND_VERTEX_ATTRIB_STATE:
        st->nattribs = (uint32_t)e->x[1] > NVN_VERTEX_ATTRIBS ? NVN_VERTEX_ATTRIBS : (uint32_t)e->x[1];
        nvn_vertex_attribs_bound(&st->attrib_state, &st->attrib_constant, st->nattribs);
        return st->nattribs ? keep(r, e, 2, st->attribs, 4u * st->nattribs, NULL) : RL_OK;
    case NVN_CMD_BIND_VERTEX_STREAM_STATE:
        st->nstreams = (uint32_t)e->x[1] > NVN_VERTEX_BUFFERS ? NVN_VERTEX_BUFFERS : (uint32_t)e->x[1];
        nvn_vertex_streams_bound(&st->stream_state, st->nstreams);
        return st->nstreams ? keep(r, e, 2, st->streams, 8u * st->nstreams, NULL) : RL_OK;
    case NVN_CMD_BIND_DEPTH_STENCIL_STATE:
        return keep(r, e, 1, st->depth_stencil, 8, &st->have_depth_stencil);
    case NVN_CMD_BIND_COLOR_STATE:
        return keep(r, e, 1, st->color_state, 4, &st->have_color_state);
    case NVN_CMD_BIND_CHANNEL_MASK_STATE:
        return keep(r, e, 1, st->channel_mask, 4, &st->have_channel_mask);
    case NVN_CMD_BIND_POLYGON_STATE:
        return keep(r, e, 1, st->polygon, 4, &st->have_polygon);
    case NVN_CMD_BIND_MULTISAMPLE_STATE:
        return keep(r, e, 1, st->multisample, 24, &st->have_multisample);
    case NVN_CMD_BIND_BLEND_STATE: {
        uint8_t b[8];
        uint64_t t = 0;
        int rc = keep(r, e, 1, b, 8, NULL);
        if (rc == RL_OK)
            rc = nvn_vk_getter(r, "nvnBlendStateGetBlendTarget", b, 8, NULL, 0, 0, NULL, &t);
        if (rc != RL_OK)
            return rc;
        if ((uint32_t)t >= MAX_RT)
            return nvn_vk_stop(r, "blend state for target %u", (uint32_t)t);
        memcpy(st->blend[(uint32_t)t], b, 8);
        st->have_blend[(uint32_t)t] = true;
        return RL_OK;
    }
    case NVN_CMD_SET_VIEWPORTS: {
        uint32_t first = (uint32_t)e->x[1], n = (uint32_t)e->x[2];
        const float *v = nvn_cmd_bytes(r->s, e->x[3]);
        for (uint32_t i = 0; v && i < n && first + i < 16; i++)
            memcpy(st->viewports[first + i], v + 4 * i, 16);
        return RL_OK;
    }
    case NVN_CMD_PUSH_COMPUTE_CONSTANTS:
    case NVN_CMD_PUSH_GRAPHICS_CONSTANTS: {
        bool compute = e->op == NVN_CMD_PUSH_COMPUTE_CONSTANTS;
        uint32_t slot = compute ? (uint32_t)e->x[1] : (uint32_t)e->x[1] & 7u, off = (uint32_t)e->x[2];
        uint32_t size = (uint32_t)e->x[3] < NVN_PUSH_BYTES ? (uint32_t)e->x[3] & ~3u : NVN_PUSH_BYTES;
        const uint8_t *data = nvn_cmd_bytes(r->s, e->x[4]);
        if (slot >= (compute ? NVN_PUSH_SLOTS : NVN_GFX_PUSH_SLOTS) || off % 4 || off + size > NVN_PUSH_BYTES)
            return nvn_vk_stop(r, "%s of %u bytes at offset %u into slot %u", nvn_cmd_name(e->op), size, off, slot);
        if (size && !data)
            return nvn_vk_stop(r, "%s without its data", nvn_cmd_name(e->op));
        uint8_t *dst = compute ? st->push[NVN_STAGE_COMPUTE][slot] : st->gfx_push[slot];
        if (size)
            memcpy(dst + off, data, size);
        if (compute)
            st->push_used[NVN_STAGE_COMPUTE] |= 1u << slot;
        else
            st->gfx_push_used |= 1u << slot;
        return RL_OK;
    }
    case NVN_CMD_SET_COMPUTE_OVERLAP_MODE:
        st->compute_overlap = (uint32_t)e->x[1];
        return RL_OK;
    case NVN_CMD_SET_DEPTH_CLAMP:
        if ((e->x[1] & 0xff) && !g.depth_clamp)
            return nvn_vk_stop(r, "depth clamp, which the host device does not have");
        st->depth_clamp = (e->x[1] & 0xff) != 0;
        return RL_OK;
    case NVN_CMD_SET_VIEWPORT:
        for (int i = 0; i < 4; i++)
            st->viewports[0][i] = (float)(int32_t)e->x[1 + i];
        return RL_OK;
    case NVN_CMD_SET_DEPTH_RANGE:
        for (int i = 0; i < 2; i++)
            memcpy(&st->depth_ranges[0][i], &e->d[i], 4);
        return RL_OK;
    case NVN_CMD_SET_DEPTH_RANGES: {
        uint32_t first = (uint32_t)e->x[1], n = (uint32_t)e->x[2];
        const float *v = nvn_cmd_bytes(r->s, e->x[3]);
        for (uint32_t i = 0; v && i < n && first + i < 16; i++)
            memcpy(st->depth_ranges[first + i], v + 2 * i, 8);
        return RL_OK;
    }
    case NVN_CMD_SET_SCISSORS: {
        uint32_t first = (uint32_t)e->x[1], n = (uint32_t)e->x[2];
        const int32_t *v = nvn_cmd_bytes(r->s, e->x[3]);
        for (uint32_t i = 0; v && i < n && first + i < 16; i++)
            st->scissors[first + i] = (VkRect2D){ { v[4 * i], v[4 * i + 1] },
                                                  { (uint32_t)v[4 * i + 2], (uint32_t)v[4 * i + 3] } };
        if (v && first == 0 && n) {
            st->scissor = st->scissors[0];
            st->scissor_set = true;
        }
        return RL_OK;
    }
    case NVN_CMD_SET_STENCIL_VALUE_MASK:
        faces((uint32_t)e->x[1], (uint32_t)e->x[2], st->stencil_value_mask);
        return RL_OK;
    case NVN_CMD_SET_STENCIL_MASK:
        faces((uint32_t)e->x[1], (uint32_t)e->x[2], st->stencil_mask);
        return RL_OK;
    case NVN_CMD_SET_STENCIL_REF:
        faces((uint32_t)e->x[1], (uint32_t)e->x[2], st->stencil_ref);
        return RL_OK;
    case NVN_CMD_SET_DEPTH_BOUNDS:
        return set_depth_bounds(st, e);
    case NVN_CMD_SET_CONSERVATIVE_RASTER_MODE:
        st->conservative_control = nvn_conservative_control(st->conservative_control, (uint32_t)e->x[1], (uint32_t)e->x[2]);
        return RL_OK;
    case NVN_CMD_SET_POLYGON_OFFSET_CLAMP:
        for (int i = 0; i < 3; i++)
            memcpy(&st->polygon_offset[i], &e->d[i], 4);
        return RL_OK;
    case NVN_CMD_SET_PRIMITIVE_RESTART:
        st->primitive_restart = (e->x[1] & 0xff) != 0;
        st->restart_index = (uint32_t)e->x[2];
        if (st->primitive_restart && st->restart_index != 0xffffffffu && st->restart_index != 0xffffu)
            return nvn_vk_stop(r, "primitive restart index 0x%x", st->restart_index);
        return RL_OK;
    case NVN_CMD_SET_RASTERIZER_DISCARD:
        st->rasterizer_discard = (e->x[1] & 0xff) != 0;
        return RL_OK;
    case NVN_CMD_SET_CONSERVATIVE_RASTER_ENABLE:
        st->conservative_raster = (e->x[1] & 0xff) != 0;
        if (st->conservative_raster && !g.conservative_raster)
            nvn_stats_mark(NVN_STATS_PARTIAL);
        if (st->conservative_raster && !g.conservative_raster && !g.conservative_warned) {
            g.conservative_warned = true;
            rl_log(RL_LOG_WARN, "nvn", "conservative rasterization is requested and the host device has no VK_EXT_conservative_rasterization: "
                   "primitives are rasterized normally");
        }
        return RL_OK;
    case NVN_CMD_SET_CONSERVATIVE_RASTER_DILATE: {
        float v;
        memcpy(&v, &e->d[0], 4);
        float q = ceilf(v * 4.0f);
        st->conservative_dilate = q >= 3.0f ? 3u : q > 0.0f ? (uint32_t)q : 0u;
        return RL_OK;
    }
    case NVN_CMD_SET_SAMPLE_MASK:
        st->sample_mask = (uint32_t)e->x[1] & 0xffffu;
        return RL_OK;
    case NVN_CMD_SET_ALPHA_REF:
        memcpy(&st->alpha_ref, &e->d[0], 4);
        return RL_OK;
    case NVN_CMD_SET_POINT_SIZE:
        memcpy(&st->point_size, &e->d[0], 4);
        return RL_OK;
    case NVN_CMD_SET_RENDER_ENABLE:
        st->render_mode = (e->x[1] & 0xff) ? NVN_RENDER_ALWAYS : NVN_RENDER_NEVER;
        return RL_OK;
    case NVN_CMD_SET_RENDER_ENABLE_CONDITIONAL:
        if ((uint32_t)e->x[1] > 1)
            return nvn_stats_mark(NVN_STATS_IGNORED);
        st->render_mode = (uint32_t)e->x[1] == 0 ? NVN_RENDER_IF_EQUAL : NVN_RENDER_IF_NOT_EQUAL;
        st->render_addr = e->x[2];
        return RL_OK;
    case NVN_CMD_SET_VIEWPORT_SWIZZLES: {
        uint32_t first = (uint32_t)e->x[1], n = (uint32_t)e->x[2];
        const uint32_t *v = nvn_cmd_bytes(r->s, e->x[3]);
        for (uint32_t i = 0; v && i < n && first + i < 16; i++)
            st->viewport_swizzle[first + i] = (v[4 * i] & 7u) | (v[4 * i + 1] & 7u) << 4 | (v[4 * i + 2] & 7u) << 8 |
                                              (v[4 * i + 3] & 7u) << 12;
        return RL_OK;
    }
    case NVN_CMD_SET_INNER_TESSELLATION_LEVELS: {
        const float *v = nvn_cmd_bytes(r->s, e->x[1]);
        if (v)
            memcpy(st->tess_inner, v, sizeof st->tess_inner);
        return RL_OK;
    }
    case NVN_CMD_SET_OUTER_TESSELLATION_LEVELS: {
        const float *v = nvn_cmd_bytes(r->s, e->x[1]);
        if (v)
            memcpy(st->tess_outer, v, sizeof st->tess_outer);
        return RL_OK;
    }
    case NVN_CMD_SET_PROVOKING_VERTEX_MODE:
        st->provoking_last = (uint32_t)e->x[1] == 1;
        return RL_OK;
    case NVN_CMD_SHADING_RATE_IMAGE_PALETTE:
        return shading_rate_palette(r, e);
    case NVN_CMD_BIND_SHADING_RATE_IMAGE:
        st->shading.image = e->x[1];
        return e->x[1] && !g.shading_rate_image ? nvn_stats_mark(NVN_STATS_PARTIAL) : RL_OK;
    case NVN_CMD_SET_SHADING_RATE_PALETTE_MODE:
        st->shading.mode = (uint32_t)e->x[1] == 1 ? 0u : 1u;
        return RL_OK;
    case NVN_CMD_SET_GEOMETRY_PROCESSING_MODE:
        st->geometry_mode = (uint32_t)e->x[1];
        return RL_OK;
    case NVN_CMD_BIND_COVERAGE_MODULATION_TABLE:
    case NVN_CMD_SET_PATCH_SIZE:
        return nvn_stats_mark(NVN_STATS_IGNORED);
    case NVN_CMD_SET_LINE_WIDTH:
        memcpy(&st->line_width, &e->d[0], 4);
        return st->line_width != 1.0f && !g.wide_lines ? nvn_stats_mark(NVN_STATS_PARTIAL) : RL_OK;
    case NVN_CMD_SET_BLEND_COLOR:
        for (int i = 0; i < 4; i++)
            memcpy(&st->blend_color[i], &e->d[i], 4);
        return RL_OK;
    case NVN_CMD_REPORT_COUNTER:
        return nvn_counters_report(r, (uint32_t)e->x[1], e->x[2]);
    case NVN_CMD_RESET_COUNTER:
        return nvn_counters_reset(r, (uint32_t)e->x[1]);
    case NVN_CMD_DRAW_MESH_TASKS:
        return draw_mesh(r, (uint32_t)e->x[1], (uint32_t)e->x[2]);
    case NVN_CMD_DRAW_MESH_TASKS_INDIRECT:
        return draw_mesh_indirect(r, e->x[1], 0, 1, 8);
    case NVN_CMD_MULTI_DRAW_MESH_TASKS_INDIRECT:
        return draw_mesh_indirect(r, e->x[1], 0, (uint32_t)e->x[2], (uint32_t)e->x[3]);
    case NVN_CMD_MULTI_DRAW_MESH_TASKS_INDIRECT_COUNT:
        return draw_mesh_indirect(r, e->x[1], e->x[2], (uint32_t)e->x[3], (uint32_t)e->x[4]);
    case NVN_CMD_DRAW_MESH_TASKS_3D:
    case NVN_CMD_DRAW_MESH_TASKS_3D_INDIRECT:
    case NVN_CMD_MULTI_DRAW_MESH_TASKS_3D_INDIRECT:
    case NVN_CMD_MULTI_DRAW_MESH_TASKS_3D_INDIRECT_COUNT:
        return nvn_vk_stop(r, "%s(0x%llx, 0x%llx, 0x%llx, 0x%llx, 0x%llx)", nvn_cmd_name(e->op),
                           (unsigned long long)e->x[1], (unsigned long long)e->x[2], (unsigned long long)e->x[3],
                           (unsigned long long)e->x[4], (unsigned long long)e->x[5]);
    case NVN_CMD_DRAW_ARRAYS:
        st->inst_base = 0;
        st->inst_count = 1;
        return draw(r, (uint32_t)e->x[1], false, 0, 0, 0, (uint32_t)e->x[2], (uint32_t)e->x[3]);
    case NVN_CMD_DRAW_ARRAYS_INSTANCED:
        st->inst_base = (uint32_t)e->x[4];
        st->inst_count = (uint32_t)e->x[5];
        return st->inst_count ? draw(r, (uint32_t)e->x[1], false, 0, 0, 0, (uint32_t)e->x[2], (uint32_t)e->x[3]) : RL_OK;
    case NVN_CMD_DRAW_ELEMENTS:
        st->inst_base = 0;
        st->inst_count = 1;
        return draw(r, (uint32_t)e->x[1], true, (uint32_t)e->x[2], e->x[4], 0, 0, (uint32_t)e->x[3]);
    case NVN_CMD_DRAW_ELEMENTS_BASE_VERTEX:
        st->inst_base = 0;
        st->inst_count = 1;
        return draw(r, (uint32_t)e->x[1], true, (uint32_t)e->x[2], e->x[4], (int32_t)e->x[5], 0, (uint32_t)e->x[3]);
    case NVN_CMD_DRAW_ARRAYS_INDIRECT:
        return draw_indirect(r, (uint32_t)e->x[1], false, 0, 0, e->x[2], 0, 1, 0);
    case NVN_CMD_DRAW_ELEMENTS_INDIRECT:
        return draw_indirect(r, (uint32_t)e->x[1], true, (uint32_t)e->x[2], e->x[3], e->x[4], 0, 1, 0);
    case NVN_CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT:
        return draw_indirect(r, (uint32_t)e->x[1], false, 0, 0, e->x[2], e->x[3], (uint32_t)e->x[4], e->x[5]);
    case NVN_CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT:
        return draw_indirect(r, (uint32_t)e->x[1], true, (uint32_t)e->x[2], e->x[3], e->x[4], e->x[5],
                             (uint32_t)e->x[6], e->x[7]);
    case NVN_CMD_DRAW_ELEMENTS_INSTANCED:
        st->inst_base = (uint32_t)e->x[6];
        st->inst_count = (uint32_t)e->x[7];
        return st->inst_count ? draw(r, (uint32_t)e->x[1], true, (uint32_t)e->x[2], e->x[4], (int32_t)e->x[5], 0,
                                     (uint32_t)e->x[3])
                              : RL_OK;
    default:
        return nvn_xfb_cmd(r, e);
    }
}
