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
 */
#include "nvn_vk.h"

#include <stdlib.h>

#define MAX_PIPELINES 1024u
#define MAX_MODULES 4096u
#define UBO_BYTES 65536u
#define SET_POOL_SETS 4096u
#define USER_BINDINGS 16u
#define DRIVER_BINDINGS 4u
#define MAX_RECS 48u
#define MAX_SPECS 32u

/** @brief A translated program as a shader module, with what its interface uses. */
typedef struct module {
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
    /** @brief Its interface records (nvn_shader_iface). */
    nvn_iface_rec recs[MAX_RECS];
    uint32_t nrecs;
} module;

/** @brief Everything a pipeline is made from. */
typedef struct pipe_key {
    VkShaderModule mods[5];
    uint32_t topology;
    uint32_t nattr, nbind;
    VkVertexInputAttributeDescription attr[NVN_VERTEX_ATTRIBS];
    VkVertexInputBindingDescription bind[NVN_VERTEX_BUFFERS];
    uint32_t ncolor;
    VkFormat color[MAX_RT], depth, stencil;
    VkPipelineColorBlendAttachmentState blend[MAX_RT];
    uint32_t cull, front, polygon, bias_enable;
    uint32_t depth_test, depth_write, depth_func, stencil_test;
    VkStencilOpState front_ops, back_ops;
    uint32_t primitive_restart, negative_one_to_one, patch_points;
    /** @brief Specialization constants of each stage: id and value. */
    uint32_t nspec[5];
    VkSpecializationMapEntry spec_map[5][MAX_SPECS];
    uint32_t spec[5][MAX_SPECS];
} pipe_key;

static struct {
    module *mods[MAX_MODULES];
    uint32_t nmods;
    struct {
        pipe_key k;
        VkPipeline p;
    } *pipes;
    uint32_t npipes;
    VkDescriptorSetLayout set_layout[3];
    VkPipelineLayout layout;
    VkDescriptorPool pool;
    VkBuffer zero;
    bool ready;
} d;

/** @brief What patch_spirv learns of one id: its decorations and, plus one, its storage class. */
typedef struct spv_var {
    uint32_t set, binding, location, storage;
    bool has_set, has_binding, has_location;
} spv_var;

/** @brief Reads which banks, inputs and sets words (n of them) uses, and moves its bindings for stage. */
static bool patch_spirv(uint32_t *w, size_t n, int stage, module *m)
{
    if (n < 5 || w[0] != 0x07230203)
        return false;
    uint32_t bound = w[3];
    spv_var *v = calloc(bound, sizeof *v);
    if (!v)
        return false;
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n)
            break;
        if (op == 71 && len >= 4 && w[i + 1] < bound) {
            spv_var *x = &v[w[i + 1]];
            if (w[i + 2] == 34)
                x->set = w[i + 3], x->has_set = true;
            else if (w[i + 2] == 33)
                x->binding = w[i + 3], x->has_binding = true;
            else if (w[i + 2] == 30)
                x->location = w[i + 3], x->has_location = true;
        } else if (op == 59 && len >= 4 && w[i + 2] < bound) {
            v[w[i + 2]].storage = w[i + 3] + 1u;
        }
        i += len;
    }
    for (size_t i = 5; i < n;) {
        uint32_t op = w[i] & 0xffff, len = w[i] >> 16;
        if (!len || i + len > n)
            break;
        if (op == 71 && len >= 4 && w[i + 2] == 33 && w[i + 1] < bound) {
            spv_var *x = &v[w[i + 1]];
            if (x->set == 0) {
                m->user |= 1u << (x->binding & 31);
                w[i + 3] = USER_BINDINGS * (uint32_t)stage + x->binding;
            } else if (x->set == 1) {
                m->driver |= 1u << (x->binding & 31);
                w[i + 3] = DRIVER_BINDINGS * (uint32_t)stage + x->binding;
            } else if (x->set == 2) {
                if (x->binding < 32)
                    m->texture_bindings |= 1u << x->binding;
                else
                    m->images = true;
            } else {
                m->other_sets = true;
            }
        }
        i += len;
    }
    for (uint32_t id = 0; id < bound; id++)
        if (v[id].storage == 2 && v[id].has_location && v[id].location < 32)
            m->inputs |= 1u << v[id].location;
    free(v);
    return true;
}

static module *module_of(replay *r, uint64_t code)
{
    char key[65];
    uint64_t len;
    if (!nvn_shader_key(r->mem, code, key, &len)) {
        nvn_vk_stop(r, "the program at 0x%llx has no end", (unsigned long long)code);
        return NULL;
    }
    for (uint32_t i = 0; i < d.nmods; i++)
        if (!strcmp(d.mods[i]->key, key))
            return d.mods[i];
    nvn_shader_note(r->mem, code, key);
    size_t n;
    uint32_t *w = nvn_shader_spirv(key, &n);
    if (!w) {
        nvn_vk_stop(r,
                    "program %s (at 0x%llx) is not in the build's shader table; it is recorded in %s/programs for "
                    "the next relift build (%s/failed.txt names programs the translator refused)",
                    key, (unsigned long long)code, nvn_shader_dir(), nvn_shader_dir());
        return NULL;
    }
    module *m = calloc(1, sizeof *m);
    if (!m || d.nmods == MAX_MODULES) {
        free(w);
        free(m);
        nvn_vk_stop(r, "out of shader module slots");
        return NULL;
    }
    memcpy(m->key, key, sizeof m->key);
    m->stage = nvn_shader_stage(r->mem, code);
    m->nrecs = nvn_shader_iface(key, m->recs, MAX_RECS);
    if (!patch_spirv(w, n, m->stage, m)) {
        free(w);
        free(m);
        nvn_vk_stop(r, "translation %s is not SPIR-V", key);
        return NULL;
    }
    VkShaderModuleCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                    .codeSize = n * 4,
                                    .pCode = w };
    VkResult vr = VK(vkCreateShaderModule)(g.dev, &ci, NULL, &m->m);
    free(w);
    if (vr != VK_SUCCESS) {
        free(m);
        nvn_vk_stop(r, "vkCreateShaderModule failed for %s (%d)", key, (int)vr);
        return NULL;
    }
    d.mods[d.nmods++] = m;
    return m;
}

static bool layouts(void)
{
    if (d.ready)
        return true;
    VkDescriptorSetLayoutBinding ub[USER_BINDINGS * 5], db[DRIVER_BINDINGS * 5];
    static const VkShaderStageFlags stage_bits[5] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                                                      VK_SHADER_STAGE_GEOMETRY_BIT,
                                                      VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                      VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT };
    for (uint32_t s = 0; s < 5; s++) {
        for (uint32_t b = 0; b < USER_BINDINGS; b++)
            ub[s * USER_BINDINGS + b] = (VkDescriptorSetLayoutBinding){ USER_BINDINGS * s + b,
                                                                         VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                                         stage_bits[s], NULL };
        for (uint32_t b = 0; b < DRIVER_BINDINGS; b++)
            db[s * DRIVER_BINDINGS + b] = (VkDescriptorSetLayoutBinding){ DRIVER_BINDINGS * s + b,
                                                                           VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1,
                                                                           stage_bits[s], NULL };
    }
    VkDescriptorSetLayoutCreateInfo li[2] = {
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = USER_BINDINGS * 5, .pBindings = ub },
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = DRIVER_BINDINGS * 5, .pBindings = db },
    };
    for (int i = 0; i < 2; i++)
        if (VK(vkCreateDescriptorSetLayout)(g.dev, &li[i], NULL, &d.set_layout[i]) != VK_SUCCESS)
            return false;
    if (!(d.set_layout[2] = nvn_texset_layout()))
        return false;
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 3,
                                       .pSetLayouts = d.set_layout };
    if (VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &d.layout) != VK_SUCCESS)
        return false;
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, SET_POOL_SETS * 8 };
    VkDescriptorPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = SET_POOL_SETS,
                                       .poolSizeCount = 1,
                                       .pPoolSizes = &ps };
    if (VK(vkCreateDescriptorPool)(g.dev, &pci, NULL, &d.pool) != VK_SUCCESS)
        return false;
    d.pipes = calloc(MAX_PIPELINES, sizeof *d.pipes);
    d.ready = d.pipes != NULL;
    return d.ready;
}

void nvn_draw_reset(void)
{
    if (d.pool)
        VK(vkResetDescriptorPool)(g.dev, d.pool, 0);
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

/** @brief A vertex format: the color formats, plus the three-component ones. */
static VkFormat vertex_format(uint32_t f)
{
    static const VkFormat three[] = {
        VK_FORMAT_R8G8B8_UNORM,   VK_FORMAT_R8G8B8_SNORM,    VK_FORMAT_R8G8B8_UINT,     VK_FORMAT_R8G8B8_SINT,
        VK_FORMAT_R16G16B16_SFLOAT, VK_FORMAT_R16G16B16_UNORM, VK_FORMAT_R16G16B16_SNORM, VK_FORMAT_R16G16B16_UINT,
        VK_FORMAT_R16G16B16_SINT, VK_FORMAT_R32G32B32_SFLOAT, VK_FORMAT_R32G32B32_UINT,  VK_FORMAT_R32G32B32_SINT,
    };
    if (f >= 25 && f <= 36)
        return three[f - 25];
    VkImageAspectFlags a;
    return nvn_vk_format(f, &a);
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
static int fixed_state(replay *r, pipe_key *k)
{
    nvn_gfx *st = r->st;
    int rc;
    k->ncolor = st->ncolor;
    for (uint32_t i = 0; i < st->ncolor; i++) {
        k->color[i] = st->color[i].v ? st->color[i].v->format : VK_FORMAT_UNDEFINED;
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
        k->front = (uint32_t)front & 1 ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
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
    if (st->have_multisample) {
        uint64_t samples = 0, a2c = 0;
        if ((rc = nvn_vk_getter(r, "nvnMultisampleStateGetSamples", st->multisample, 24, NULL, 0, 0, NULL,
                                &samples)) != RL_OK ||
            (rc = nvn_vk_getter(r, "nvnMultisampleStateGetAlphaToCoverageEnable", st->multisample, 24, NULL, 0, 0,
                                NULL, &a2c)) != RL_OK)
            return rc;
        if ((uint32_t)samples > 1 || (a2c & 0xff))
            return nvn_vk_stop(r, "multisample state with %u samples, alpha to coverage %u", (uint32_t)samples,
                               (uint32_t)(a2c & 0xff));
    }
    k->primitive_restart = st->primitive_restart;
    k->negative_one_to_one = nvn_device_depth_mode() == 0;
    return RL_OK;
}

/** @brief Fills the vertex input of k for the attributes vs reads. */
static int vertex_state(replay *r, const module *vs, pipe_key *k)
{
    nvn_gfx *st = r->st;
    uint32_t streams = 0;
    int rc;
    for (uint32_t i = 0; i < NVN_VERTEX_ATTRIBS && i < 32; i++) {
        if (!(vs->inputs & (1u << i)))
            continue;
        if (i >= st->nattribs)
            return nvn_vk_stop(r, "the vertex program reads attribute %u, beyond the %u bound", i, st->nattribs);
        uint32_t fo[2];
        uint64_t stream = 0;
        if ((rc = nvn_vk_getter(r, "nvnVertexAttribStateGetFormat", st->attribs[i], 4, NULL, 0, 2, fo, NULL)) !=
                RL_OK ||
            (rc = nvn_vk_getter(r, "nvnVertexAttribStateGetStreamIndex", st->attribs[i], 4, NULL, 0, 0, NULL,
                                &stream)) != RL_OK)
            return rc;
        VkFormat f = vertex_format(fo[0]);
        if (f == VK_FORMAT_UNDEFINED || integer_format(f))
            return nvn_vk_stop(r, "vertex attribute %u has format %u, which the float input cannot take", i, fo[0]);
        if ((uint32_t)stream >= st->nstreams)
            return nvn_vk_stop(r, "vertex attribute %u reads stream %u of %u", i, (uint32_t)stream, st->nstreams);
        k->attr[k->nattr++] = (VkVertexInputAttributeDescription){ i, (uint32_t)stream, f, fo[1] };
        streams |= 1u << stream;
    }
    for (uint32_t s = 0; s < NVN_VERTEX_BUFFERS; s++) {
        if (!(streams & (1u << s)))
            continue;
        uint64_t stride = 0, div = 0;
        if ((rc = nvn_vk_getter(r, "nvnVertexStreamStateGetStride", st->streams[s], 8, NULL, 0, 0, NULL, &stride)) !=
                RL_OK ||
            (rc = nvn_vk_getter(r, "nvnVertexStreamStateGetDivisor", st->streams[s], 8, NULL, 0, 0, NULL, &div)) !=
                RL_OK)
            return rc;
        if ((uint32_t)div > 1)
            return nvn_vk_stop(r, "vertex stream %u with divisor %u", s, (uint32_t)div);
        k->bind[k->nbind++] = (VkVertexInputBindingDescription){
            s, (uint32_t)stride, div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX
        };
    }
    return RL_OK;
}

static VkPipeline pipeline_of(replay *r, const pipe_key *k)
{
    for (uint32_t i = 0; i < d.npipes; i++)
        if (!memcmp(&d.pipes[i].k, k, sizeof *k))
            return d.pipes[i].p;
    if (d.npipes == MAX_PIPELINES) {
        nvn_vk_stop(r, "more than %u pipelines", MAX_PIPELINES);
        return NULL;
    }
    static const VkShaderStageFlagBits bits[5] = { VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT,
                                                   VK_SHADER_STAGE_GEOMETRY_BIT,
                                                   VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                   VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT };
    VkPipelineShaderStageCreateInfo stages[5];
    VkSpecializationInfo si[5];
    uint32_t ns = 0;
    for (uint32_t s = 0; s < 5; s++)
        if (k->mods[s]) {
            si[s] = (VkSpecializationInfo){ k->nspec[s], k->spec_map[s], 4u * k->nspec[s], k->spec[s] };
            stages[ns++] = (VkPipelineShaderStageCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                              .stage = bits[s],
                                                              .module = k->mods[s],
                                                              .pName = "main",
                                                              .pSpecializationInfo = k->nspec[s] ? &si[s] : NULL };
        }
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                .vertexBindingDescriptionCount = k->nbind,
                                                .pVertexBindingDescriptions = k->bind,
                                                .vertexAttributeDescriptionCount = k->nattr,
                                                .pVertexAttributeDescriptions = k->attr };
    VkPipelineInputAssemblyStateCreateInfo ia = { .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                  .topology = (VkPrimitiveTopology)k->topology,
                                                  .primitiveRestartEnable = k->primitive_restart };
    VkPipelineTessellationStateCreateInfo ts = { .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
                                                 .patchControlPoints = k->patch_points ? k->patch_points : 1 };
    struct {
        VkStructureType sType;
        const void *pNext;
        VkBool32 negativeOneToOne;
    } clip = { (VkStructureType)1000355001, NULL, VK_TRUE };
    VkPipelineViewportStateCreateInfo vp = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                             .pNext = k->negative_one_to_one ? &clip : NULL,
                                             .viewportCount = 1,
                                             .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .polygonMode = (VkPolygonMode)k->polygon,
                                                  .cullMode = k->cull,
                                                  .frontFace = (VkFrontFace)k->front,
                                                  .depthBiasEnable = k->bias_enable,
                                                  .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = { .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineDepthStencilStateCreateInfo ds = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
                                                 .depthTestEnable = k->depth_test,
                                                 .depthWriteEnable = k->depth_write,
                                                 .depthCompareOp = (VkCompareOp)k->depth_func,
                                                 .stencilTestEnable = k->stencil_test,
                                                 .front = k->front_ops,
                                                 .back = k->back_ops };
    VkPipelineColorBlendStateCreateInfo cb = { .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                               .attachmentCount = k->ncolor,
                                               .pAttachments = k->blend };
    VkDynamicState dyn[] = { VK_DYNAMIC_STATE_VIEWPORT,          VK_DYNAMIC_STATE_SCISSOR,
                             VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
                             VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS,
                             VK_DYNAMIC_STATE_BLEND_CONSTANTS,   VK_DYNAMIC_STATE_LINE_WIDTH };
    VkPipelineDynamicStateCreateInfo dy = { .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                            .dynamicStateCount = sizeof dyn / sizeof dyn[0],
                                            .pDynamicStates = dyn };
    VkPipelineRenderingCreateInfo ri = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                         .colorAttachmentCount = k->ncolor,
                                         .pColorAttachmentFormats = k->color,
                                         .depthAttachmentFormat = k->depth,
                                         .stencilAttachmentFormat = k->stencil };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .pNext = &ri,
                                        .stageCount = ns,
                                        .pStages = stages,
                                        .pVertexInputState = &vi,
                                        .pInputAssemblyState = &ia,
                                        .pTessellationState = k->mods[3] ? &ts : NULL,
                                        .pViewportState = &vp,
                                        .pRasterizationState = &rs,
                                        .pMultisampleState = &ms,
                                        .pDepthStencilState = &ds,
                                        .pColorBlendState = &cb,
                                        .pDynamicState = &dy,
                                        .layout = d.layout };
    VkPipeline p;
    VkResult vr = VK(vkCreateGraphicsPipelines)(g.dev, NULL, 1, &ci, NULL, &p);
    if (vr != VK_SUCCESS) {
        nvn_vk_stop(r, "vkCreateGraphicsPipelines failed (%d)", (int)vr);
        return NULL;
    }
    d.pipes[d.npipes].k = *k;
    d.pipes[d.npipes++].p = p;
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "pipeline %u: %u stages, %u attributes, %u color targets", d.npipes, ns, k->nattr,
               k->ncolor);
    return p;
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
    if (!nvn_vk_upload_bytes(r, src, n, UBO_BYTES, 256, &off))
        return nvn_vk_stop(r, "the upload arena is full");
    *bi = (VkDescriptorBufferInfo){ g.arena.buf, off, UBO_BYTES };
    return RL_OK;
}

/** @brief Writes the constant banks the bound modules read into new descriptor sets and binds them with the texture set. */
static int bind_constants(replay *r, module *const mods[5])
{
    VkDescriptorSet sets[3];
    VkDescriptorSetAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                       .descriptorPool = d.pool,
                                       .descriptorSetCount = 2,
                                       .pSetLayouts = d.set_layout };
    if (VK(vkAllocateDescriptorSets)(g.dev, &ai, sets) != VK_SUCCESS) {
        if (!nvn_vk_flush(r) || VK(vkAllocateDescriptorSets)(g.dev, &ai, sets) != VK_SUCCESS)
            return nvn_vk_stop(r, "cannot allocate descriptor sets");
    }
    sets[2] = nvn_texset();
    VkDescriptorBufferInfo bi[(USER_BINDINGS + DRIVER_BINDINGS) * 5];
    VkWriteDescriptorSet wr[(USER_BINDINGS + DRIVER_BINDINGS) * 5];
    uint32_t n = 0;
    for (uint32_t s = 0; s < 5; s++) {
        if (!mods[s])
            continue;
        for (uint32_t b = 0; b < DRIVER_BINDINGS; b++) {
            if (!(mods[s]->driver & (1u << b)))
                continue;
            int rc;
            if (b == 1) {
                const nvn_consts *k = &r->st->consts[s];
                rc = bank_of(r, k->addr ? r->mem + k->addr : NULL, k->addr ? k->size : 0, &bi[n]);
            } else if (b == 2) {
                rc = bank_of(r, r->st->bank2[s], NVN_BANK2_BYTES, &bi[n]);
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
            if (b >= NVN_UBO_SLOTS)
                return nvn_vk_stop(r, "program %s reads constant bank c[%u]", mods[s]->key, b + 3);
            uint64_t addr = r->st->ubo[s][b].addr, size = r->st->ubo[s][b].size;
            int rc = bank_of(r, addr ? r->mem + addr : NULL, addr ? size : 0, &bi[n]);
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
    if (n)
        VK(vkUpdateDescriptorSets)(g.dev, n, wr, 0, NULL);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 3, sets, 0, NULL);
    return RL_OK;
}

/**
 * @brief Makes the texture set hold the texture of each handle stage s's
 * module reads from its banks, and gives the module's kind and dimension
 * constants their values in k.
 */
static int bind_textures(replay *r, uint32_t s, const module *m, pipe_key *k)
{
    for (uint32_t i = 0; i < m->nrecs; i++) {
        const nvn_iface_rec *h = &m->recs[i];
        if (h->kind != NVN_IFACE_TEXTURE)
            continue;
        if (h->bank != 2 || h->offset + 4 > NVN_BANK2_BYTES)
            return nvn_vk_stop(r, "program %s reads a texture handle at c[%u][0x%x]", m->key, h->bank, h->offset);
        uint32_t handle, kind, dim;
        memcpy(&handle, r->st->bank2[s] + h->offset, 4);
        int rc = nvn_texset_bind(r, handle, &kind, &dim);
        if (rc != RL_OK)
            return rc;
        for (uint32_t j = 0; j < m->nrecs; j++) {
            const nvn_iface_rec *c = &m->recs[j];
            if ((c->kind != NVN_IFACE_TEXTURE_KIND && c->kind != NVN_IFACE_TEXTURE_DIM) || c->bank != h->bank ||
                c->offset != h->offset)
                continue;
            if (k->nspec[s] == MAX_SPECS)
                return nvn_vk_stop(r, "program %s has more than %u specialization constants", m->key, MAX_SPECS);
            uint32_t n = k->nspec[s]++;
            k->spec_map[s][n] = (VkSpecializationMapEntry){ c->spec, 4 * n, 4 };
            k->spec[s][n] = c->kind == NVN_IFACE_TEXTURE_KIND ? kind : dim;
        }
    }
    return RL_OK;
}

static void dynamic_state(replay *r, VkRect2D area)
{
    nvn_gfx *st = r->st;
    VkViewport vp = { st->viewports[0][0], st->viewports[0][1], st->viewports[0][2], st->viewports[0][3],
                      st->depth_ranges[0][0], st->depth_ranges[0][1] };
    if (vp.width == 0 || vp.height == 0)
        vp = (VkViewport){ 0, 0, (float)area.extent.width, (float)area.extent.height, 0, 1 };
    VK(vkCmdSetViewport)(g.cb, 0, 1, &vp);
    VkRect2D sc = area;
    if (st->scissor_set) {
        int64_t x0 = st->scissor.offset.x, y0 = st->scissor.offset.y;
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
    VK(vkCmdSetLineWidth)(g.cb, st->line_width);
}

/** @brief A draw: mode, then indexed (index type, index address, base vertex) or not (first vertex), and count. */
static int draw(replay *r, uint32_t mode, bool indexed, uint32_t itype, uint64_t iaddr, int32_t base, uint32_t first,
                uint32_t count)
{
    nvn_gfx *st = r->st;
    if (!layouts())
        return nvn_vk_stop(r, "cannot make the pipeline layout");
    pipe_key k;
    memset(&k, 0, sizeof k);
    VkPrimitiveTopology topo;
    if (!topology(mode, &topo))
        return nvn_vk_stop(r, "draw primitive %u", mode);
    k.topology = topo;
    module *mods[5] = { NULL };
    for (uint32_t s = 0; s < 5; s++) {
        if (!st->code[s])
            continue;
        if (!(mods[s] = module_of(r, st->code[s])))
            return RL_PENDING;
        if (mods[s]->images || mods[s]->other_sets)
            return nvn_vk_stop(r, "program %s uses images or guest memory, which draws do not bind yet",
                               mods[s]->key);
        bool bound = false;
        for (uint32_t i = 0; i < mods[s]->nrecs; i++)
            bound |= mods[s]->recs[i].kind == NVN_IFACE_TEXTURE;
        if ((mods[s]->texture_bindings & ~(1u << 31)) && !bound)
            return nvn_vk_stop(r, "program %s samples textures only through register handles", mods[s]->key);
        int trc = bind_textures(r, s, mods[s], &k);
        if (trc != RL_OK)
            return trc;
        k.mods[s] = mods[s]->m;
    }
    if (!mods[0])
        return nvn_vk_stop(r, "draw without a vertex program");
    if (mods[3] || mods[4] || mods[2])
        return nvn_vk_stop(r, "draw with tessellation or geometry programs");
    int rc = fixed_state(r, &k);
    if (rc == RL_OK)
        rc = vertex_state(r, mods[0], &k);
    if (rc != RL_OK)
        return rc;
    VkPipeline p = pipeline_of(r, &k);
    if (!p)
        return RL_PENDING;
    VkBuffer bufs[NVN_VERTEX_BUFFERS];
    VkDeviceSize offs[NVN_VERTEX_BUFFERS];
    uint32_t nb = 0, firstb = 0;
    for (uint32_t i = 0; i < k.nbind; i++) {
        uint32_t s = k.bind[i].binding;
        uint64_t off;
        if (!st->vb[s].addr)
            return nvn_vk_stop(r, "vertex stream %u has no buffer bound", s);
        if (!nvn_vk_upload(r, st->vb[s].addr, st->vb[s].size, 0, 16, &off))
            return nvn_vk_stop(r, "the upload arena is full");
        if (nb == 0)
            firstb = s;
        if (s != firstb + nb)
            return nvn_vk_stop(r, "vertex streams that are not contiguous");
        bufs[nb] = g.arena.buf;
        offs[nb++] = off;
    }
    uint64_t ioff = 0;
    static const uint32_t isize[] = { 1, 2, 4 };
    if (indexed) {
        if (itype > 2)
            return nvn_vk_stop(r, "index type %u", itype);
        if (itype == 0)
            return nvn_vk_stop(r, "8-bit indices");
        if (!nvn_vk_upload(r, iaddr, (uint64_t)count * isize[itype], 0, 16, &ioff))
            return nvn_vk_stop(r, "the upload arena is full");
    }
    VkRect2D area;
    if (!nvn_vk_begin_rendering(r, &area))
        return nvn_vk_stop(r, "draw without render targets");
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    if ((rc = bind_constants(r, mods)) != RL_OK)
        return rc;
    dynamic_state(r, area);
    if (nb)
        VK(vkCmdBindVertexBuffers)(g.cb, firstb, nb, bufs, offs);
    if (indexed) {
        VK(vkCmdBindIndexBuffer)(g.cb, g.arena.buf, ioff, itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        VK(vkCmdDrawIndexed)(g.cb, count, 1, 0, base, 0);
    } else {
        VK(vkCmdDraw)(g.cb, count, 1, first, 0);
    }
    VK(vkCmdEndRendering)(g.cb);
    nvn_vk_barrier(g.cb);
    return RL_OK;
}

static int bind_program(replay *r, const nvn_cmd *e)
{
    uint32_t n = 0, mask = (uint32_t)e->x[2] & 0x3fu;
    const nvn_consts *consts = NULL;
    const uint64_t *code = e->x[1] ? nvn_program_code(e->x[1], &n, &consts) : NULL;
    if (e->x[1] && !code)
        return nvn_vk_stop(r, "nvnCommandBufferBindProgram: 0x%llx is not a program", (unsigned long long)e->x[1]);
    for (uint32_t s = 0; s < NVN_STAGES; s++) {
        if (!(mask & (1u << s)))
            continue;
        r->st->code[s] = 0;
        r->st->consts[s] = (nvn_consts){ 0, 0 };
        for (uint32_t i = 0; i < n; i++)
            if (nvn_shader_stage(r->mem, code[i]) == (int)s) {
                r->st->code[s] = code[i];
                r->st->consts[s] = consts[i];
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

int nvn_draw_cmd(replay *r, const nvn_cmd *e)
{
    nvn_gfx *st = r->st;
    switch (e->op) {
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
        return st->nattribs ? keep(r, e, 2, st->attribs, 4u * st->nattribs, NULL) : RL_OK;
    case NVN_CMD_BIND_VERTEX_STREAM_STATE:
        st->nstreams = (uint32_t)e->x[1] > NVN_VERTEX_BUFFERS ? NVN_VERTEX_BUFFERS : (uint32_t)e->x[1];
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
        if ((uint32_t)e->x[1] & 0xff)
            return nvn_vk_stop(r, "depth bounds test");
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
    case NVN_CMD_SET_PATCH_SIZE:
        return RL_OK;
    case NVN_CMD_REPORT_COUNTER:
        if ((uint32_t)e->x[1] != 0)
            return nvn_vk_stop(r, "nvnCommandBufferReportCounter of counter type %u", (uint32_t)e->x[1]);
        if (!nvn_vk_flush(r))
            return nvn_vk_stop(r, "the host device failed a submit");
        rl_st64(r->mem, e->x[2], 0);
        rl_st64(r->mem, e->x[2] + 8, rl_guest_clock_ns());
        return RL_OK;
    case NVN_CMD_DRAW_ARRAYS:
        return draw(r, (uint32_t)e->x[1], false, 0, 0, 0, (uint32_t)e->x[2], (uint32_t)e->x[3]);
    case NVN_CMD_DRAW_ELEMENTS:
        return draw(r, (uint32_t)e->x[1], true, (uint32_t)e->x[2], e->x[4], 0, 0, (uint32_t)e->x[3]);
    case NVN_CMD_DRAW_ELEMENTS_BASE_VERTEX:
        return draw(r, (uint32_t)e->x[1], true, (uint32_t)e->x[2], e->x[4], (int32_t)e->x[5], 0, (uint32_t)e->x[3]);
    default:
        return 1;
    }
}
