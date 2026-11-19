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
#define USER_BINDINGS 22u
/** @brief The user binding (constant bank c[24]) a fragment program reads the stage's storage buffer table from. */
#define STORAGE_TABLE_BINDING 21u
/** @brief Storage buffer entries of the table in c[24]: each is {GPU address, u32 size, u32 0}, 16 bytes. */
#define STORAGE_TABLE_ENTRIES 5u
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
    VkDescriptorSetLayout set_layout[3], cset_layout[3];
    VkPipelineLayout layout, clayout;
    struct {
        VkShaderModule m;
        uint32_t n, ids[MAX_SPECS], vals[MAX_SPECS];
        VkPipeline p;
    } cpipes[MAX_PIPELINES];
    uint32_t ncpipes;
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
static void expand_integer(const uint8_t *src, uint64_t stride, VkFormat f, uint64_t n, uint32_t *out)
{
    uint32_t comps, bytes;
    bool sign;
    integer_layout(f, &comps, &bytes, &sign);
    for (uint64_t v = 0; v < n; v++) {
        const uint8_t *p = src + v * stride;
        uint32_t w[4] = { 0, 0, 0, 1 };
        for (uint32_t c = 0; c < comps; c++) {
            uint32_t x = 0;
            memcpy(&x, p + c * bytes, bytes);
            if (sign && bytes < 4 && (x >> (8 * bytes - 1)) & 1)
                x |= ~0u << (8 * bytes);
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
    k->rasterizer_discard = st->rasterizer_discard;
    k->conservative_raster = st->conservative_raster && g.conservative_raster;
    k->depth_clamp = st->depth_clamp;
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

/** @brief Fills the vertex input of k for the attributes vs reads. */
static int vertex_state(replay *r, const module *vs, pipe_key *k)
{
    nvn_gfx *st = r->st;
    uint32_t streams = 0;
    bool real15 = false, def = false;
    uint32_t int_streams = 0;
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
        if ((uint32_t)stream >= st->nstreams)
            return nvn_vk_stop(r, "vertex attribute %u reads stream %u of %u", i, (uint32_t)stream, st->nstreams);
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
    struct {
        VkStructureType sType;
        const void *pNext;
        VkFlags flags;
        int conservativeRasterizationMode;
        float extraPrimitiveOverestimationSize;
    } conservative = { (VkStructureType)1000101001, NULL, 0, 1, 0.0f };
    VkPipelineRasterizationStateCreateInfo rs = { .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                  .pNext = k->conservative_raster ? &conservative : NULL,
                                                  .polygonMode = (VkPolygonMode)k->polygon,
                                                  .cullMode = k->cull,
                                                  .frontFace = (VkFrontFace)k->front,
                                                  .depthClampEnable = k->depth_clamp,
                                                  .depthBiasEnable = k->bias_enable,
                                                  .rasterizerDiscardEnable = k->rasterizer_discard,
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

static const void *user_bank(replay *r, uint32_t s, uint32_t b, uint64_t addr);

/**
 * @brief The driver bank c[0] of graphics stage s: each storage buffer slot k of the stage is 16 bytes
 * {GPU address, u32 size} at 16 * (first[s] + k), where first is the driver's per-stage table (sdk 0xbb0570,
 * read by nvnCommandBufferBindStorageBuffer, sdk 0x67be50: vertex 0x11, fragment 0x51, geometry 0x41, tessellation
 * control 0x21, tessellation evaluation 0x31); vertex programs read their slots at 0x110 and up. Every other word
 * is 0 (the layer does not know the driver's other c[0] fields yet).
 */
void nvn_graphics_table(const nvn_gfx *st, uint32_t s, uint8_t c0[NVN_GFX_C0_BYTES])
{
    static const uint32_t first[5] = { 0x11, 0x51, 0x41, 0x21, 0x31 };
    memset(c0, 0, NVN_GFX_C0_BYTES);
    for (uint32_t k = 0; k < NVN_STORAGE_SLOTS && s < 5; k++) {
        uint32_t at = 16 * (first[s] + k), size = (uint32_t)st->gstorage[s][k].size;
        if (at + 16 > NVN_GFX_C0_BYTES)
            break;
        memcpy(c0 + at, &st->gstorage[s][k].addr, 8);
        memcpy(c0 + at + 8, &size, 4);
    }
}

/**
 * @brief Writes the constant banks the bound modules read into new
 * descriptor sets, sets[0] and sets[1], with the texture set in sets[2].
 */
static int write_constants(replay *r, module *const *mods, VkDescriptorSet sets[3])
{
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
                rc = bank_of(r, r->st->bank2_host[s], NVN_BANK2_BYTES, &bi[n]);
            } else if (b == 0) {
                uint8_t c0[NVN_GFX_C0_BYTES];
                nvn_graphics_table(r->st, s, c0);
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
            if (b == STORAGE_TABLE_BINDING && s == 1u) {
                uint8_t table[16 * STORAGE_TABLE_ENTRIES] = { 0 };
                for (uint32_t k = 0; k < STORAGE_TABLE_ENTRIES; k++) {
                    uint32_t sz = (uint32_t)r->st->gstorage[s][k].size;
                    memcpy(table + 16 * k, &r->st->gstorage[s][k].addr, 8);
                    memcpy(table + 16 * k + 8, &sz, 4);
                }
                int trc = bank_of(r, table, sizeof table, &bi[n]);
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
            uint64_t addr = r->st->ubo[s][b].addr, size = r->st->ubo[s][b].size;
            int rc = bank_of(r, user_bank(r, s, b, addr), addr ? size : 0, &bi[n]);
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
            map[*n] = (VkSpecializationMapEntry){ c->spec, 4 * *n, 4 };
            vals[(*n)++] = c->kind == NVN_IFACE_TEXTURE_KIND ? kind : dim;
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
        memcpy(st->ubo_host[s][b], r->mem + nvn_gpu_guest(addr), size);
        memset(st->ubo_host[s][b] + size, 0, UBO_BYTES - size);
        int rc = bind_textures(r, m, h->bank, st->ubo_host[s][b], (uint32_t)size, n, map, vals, 0, 0);
        if (rc != RL_OK)
            return rc;
        st->ubo_patched[s] |= 1u << b;
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
}

/** @brief What a draw puts into the batch being recorded, and where. */
typedef struct draw_data {
    const pipe_key *k;
    module *const *mods;
    bool indexed, has_def;
    uint32_t itype, first, count, nb, firstb;
    int32_t base;
    uint64_t iaddr, def_off, ioff;
    VkDeviceSize offs[NVN_VERTEX_BUFFERS], int_off[NVN_VERTEX_ATTRIBS];
    VkDescriptorSet sets[3];
} draw_data;

/** @brief Puts a draw's vertices, indices and constant banks into the batch being recorded (an nvn_vk_stage step). */
static int stage_draw(replay *r, void *arg)
{
    draw_data *dd = arg;
    const pipe_key *kp = dd->k;
    const nvn_gfx *st = r->st;
    bool indexed = dd->indexed;
    uint32_t itype = dd->itype, first = dd->first, count = dd->count;
    int32_t base = dd->base;
    dd->nb = dd->firstb = 0;
    dd->has_def = false;
    for (uint32_t i = 0; i < kp->nbind; i++) {
        uint32_t s = kp->bind[i].binding;
        uint64_t off;
        if (s >= NVN_VERTEX_BUFFERS)
            continue;
        if (s == DEFAULT_BINDING && kp->bind[i].stride == 0 && !st->vb[s].addr) {
            static const float one[4] = { 0, 0, 0, 1 };
            if (!nvn_vk_upload_bytes(r, (const uint8_t *)one, sizeof one, 0, 16, &off))
                return nvn_vk_stop(r, "the upload arena is full");
            dd->def_off = off;
            dd->has_def = true;
            continue;
        }
        if (!st->vb[s].addr)
            return nvn_vk_stop(r, "vertex stream %u has no buffer bound", s);
        if (!nvn_vk_upload(r, st->vb[s].addr, st->vb[s].size, 0, 16, &off))
            return nvn_vk_stop(r, "the upload arena is full");
        if (dd->nb == 0)
            dd->firstb = s;
        if (s != dd->firstb + dd->nb)
            return nvn_vk_stop(r, "vertex streams that are not contiguous");
        dd->offs[dd->nb++] = off;
    }
    static const uint32_t isize[] = { 1, 2, 4 };
    if (indexed) {
        if (itype > 2)
            return nvn_vk_stop(r, "index type %u", itype);
        if (itype == 0)
            return nvn_vk_stop(r, "8-bit indices");
        if (!nvn_vk_upload(r, dd->iaddr, (uint64_t)count * isize[itype], 0, 16, &dd->ioff))
            return nvn_vk_stop(r, "the upload arena is full");
    }
    if (kp->nint) {
        uint64_t nv = (uint64_t)first + count;
        if (indexed) {
            const uint8_t *ip = r->mem + nvn_gpu_guest(dd->iaddr);
            int64_t hi = -1;
            for (uint32_t i = 0; i < count; i++) {
                uint32_t x = itype == 1 ? (uint32_t)ip[2 * i] | (uint32_t)ip[2 * i + 1] << 8 : rl_ld32(ip, 4 * (uint64_t)i);
                bool restart = kp->primitive_restart && x == (itype == 1 ? 0xffffu : 0xffffffffu);
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
            expand_integer(r->mem + nvn_gpu_guest(st->vb[s].addr) + kp->ints[j].offset, kp->ints[j].stride, kp->ints[j].format,
                           n, w);
            bool ok = nvn_vk_upload_bytes(r, w, n ? n * 16 : 16, 0, 16, &dd->int_off[j]);
            free(w);
            if (!ok)
                return nvn_vk_stop(r, "the upload arena is full");
        }
    }
    return write_constants(r, dd->mods, dd->sets);
}

/** @brief A draw: mode, then indexed (index type, index address, base vertex) or not (first vertex), and count. */
static int draw(replay *r, uint32_t mode, bool indexed, uint32_t itype, uint64_t iaddr, int32_t base, uint32_t first,
                uint32_t count)
{
    nvn_gfx *st = r->st;
    memset(st->ubo_patched, 0, sizeof st->ubo_patched);
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
        if (mods[s]->user >> USER_BINDINGS)
            return nvn_vk_stop(r, "program %s reads constant bank c[%d], past the %u user banks the layer binds", mods[s]->key,
                               3 + __builtin_ctz(mods[s]->user >> USER_BINDINGS) + (int)USER_BINDINGS, USER_BINDINGS);
        if (mods[s]->other_sets)
            return nvn_vk_stop(r, "program %s uses a guest memory set, which draws do not bind yet",
                               mods[s]->key);
        bool bound = false;
        for (uint32_t i = 0; i < mods[s]->nrecs; i++)
            bound |= mods[s]->recs[i].kind == NVN_IFACE_TEXTURE;
        if ((mods[s]->texture_bindings & ~(1u << 31)) && !bound)
            return nvn_vk_stop(r, "program %s samples textures only through register handles", mods[s]->key);
        memcpy(st->bank2_host[s], st->bank2[s], NVN_BANK2_BYTES);
        int trc = bind_textures(r, mods[s], 2, st->bank2_host[s], NVN_BANK2_BYTES, &k.nspec[s], k.spec_map[s], k.spec[s],
                           NVN_BANK2_IMAGES, NVN_BANK2_IMAGES + 8u * NVN_IMAGE_SLOTS);
        if (trc != RL_OK)
            return trc;
        trc = bind_user_textures(r, mods[s], s, &k.nspec[s], k.spec_map[s], k.spec[s]);
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
    draw_data dd = { .k = &k, .mods = mods, .indexed = indexed, .itype = itype, .first = first, .count = count,
                     .base = base, .iaddr = iaddr };
    if ((rc = nvn_vk_stage(r, stage_draw, &dd, "a draw")) != RL_OK)
        return rc;
    VkRect2D area;
    if ((rc = nvn_counters_prepare(r)) != RL_OK)
        return rc;
    if (!nvn_vk_begin_rendering(r, &area))
        return nvn_vk_stop(r, "draw without render targets (%u color, first %p, depth %p)", st->ncolor,
                           st->ncolor ? (void *)st->color[0].v : NULL, (void *)st->depth.v);
    uint32_t counted = nvn_counters_begin(st, mods[2] != NULL, mods[3] != NULL || mods[4] != NULL);
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_GRAPHICS, d.layout, 0, 3, dd.sets, 0, NULL);
    dynamic_state(r, area);
    VkBuffer bufs[NVN_VERTEX_BUFFERS];
    for (uint32_t i = 0; i < dd.nb; i++)
        bufs[i] = g.arena.buf;
    if (dd.nb)
        VK(vkCmdBindVertexBuffers)(g.cb, dd.firstb, dd.nb, bufs, dd.offs);
    if (dd.has_def)
        VK(vkCmdBindVertexBuffers)(g.cb, DEFAULT_BINDING, 1, &g.arena.buf, &dd.def_off);
    for (uint32_t j = 0; j < k.nint; j++)
        VK(vkCmdBindVertexBuffers)(g.cb, NVN_VERTEX_BUFFERS + j, 1, &g.arena.buf, &dd.int_off[j]);
    if (indexed) {
        VK(vkCmdBindIndexBuffer)(g.cb, g.arena.buf, dd.ioff, itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
        VK(vkCmdDrawIndexed)(g.cb, count, st->inst_count, 0, base, st->inst_base);
    } else {
        VK(vkCmdDraw)(g.cb, count, st->inst_count, first, st->inst_base);
    }
    nvn_counters_end(counted);
    VK(vkCmdEndRendering)(g.cb);
    nvn_vk_barrier(g.cb);
    return RL_OK;
}

/** @brief The compute pipeline layout: set 0 the stage's user banks, set 1 its driver banks, set 2 the texture set. */
static bool compute_layouts(void)
{
    if (d.clayout)
        return true;
    if (!layouts())
        return false;
    VkDescriptorSetLayoutBinding ub[USER_BINDINGS], db[DRIVER_BINDINGS];
    for (uint32_t b = 0; b < USER_BINDINGS; b++)
        ub[b] = (VkDescriptorSetLayoutBinding){ USER_BINDINGS * NVN_STAGE_COMPUTE + b, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                                1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    for (uint32_t b = 0; b < DRIVER_BINDINGS; b++)
        db[b] = (VkDescriptorSetLayoutBinding){ DRIVER_BINDINGS * NVN_STAGE_COMPUTE + b,
                                                VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo li[2] = {
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = USER_BINDINGS, .pBindings = ub },
        { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .bindingCount = DRIVER_BINDINGS, .pBindings = db },
    };
    for (int i = 0; i < 2; i++)
        if (VK(vkCreateDescriptorSetLayout)(g.dev, &li[i], NULL, &d.cset_layout[i]) != VK_SUCCESS)
            return false;
    d.cset_layout[2] = d.set_layout[2];
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 3,
                                       .pSetLayouts = d.cset_layout };
    return VK(vkCreatePipelineLayout)(g.dev, &pli, NULL, &d.clayout) == VK_SUCCESS;
}

/** @brief The compute pipeline of module m with specialization constants (n of them in map and vals). */
static VkPipeline compute_pipeline_of(replay *r, const module *m, uint32_t n, const VkSpecializationMapEntry *map,
                                      const uint32_t *vals)
{
    for (uint32_t i = 0; i < d.ncpipes; i++) {
        if (d.cpipes[i].m != m->m || d.cpipes[i].n != n || memcmp(d.cpipes[i].vals, vals, 4u * n))
            continue;
        bool same = true;
        for (uint32_t j = 0; j < n && same; j++)
            same = d.cpipes[i].ids[j] == map[j].constantID;
        if (same)
            return d.cpipes[i].p;
    }
    if (d.ncpipes == MAX_PIPELINES) {
        nvn_vk_stop(r, "more than %u compute pipelines", MAX_PIPELINES);
        return NULL;
    }
    VkSpecializationInfo si = { n, map, 4u * n, vals };
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                  .module = m->m,
                                                  .pName = "main",
                                                  .pSpecializationInfo = &si },
                                       .layout = d.clayout };
    VkPipeline p;
    if (VK(vkCreateComputePipelines)(g.dev, NULL, 1, &ci, NULL, &p) != VK_SUCCESS) {
        nvn_vk_stop(r, "vkCreateComputePipelines failed for %s", m->key);
        return NULL;
    }
    d.cpipes[d.ncpipes].m = m->m;
    d.cpipes[d.ncpipes].n = n;
    for (uint32_t j = 0; j < n; j++) {
        d.cpipes[d.ncpipes].ids[j] = map[j].constantID;
        d.cpipes[d.ncpipes].vals[j] = vals[j];
    }
    d.cpipes[d.ncpipes++].p = p;
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "compute pipeline %u: %s", d.ncpipes, m->key);
    return p;
}

/** @brief What a dispatch puts into the batch being recorded. */
typedef struct dispatch_data {
    const module *m;
    const uint8_t *c0;
    VkDescriptorSet sets[3];
} dispatch_data;

/** @brief Puts a dispatch's constant banks into the batch being recorded (an nvn_vk_stage step). */
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
    sets[2] = nvn_texset();
    VkDescriptorBufferInfo bi[USER_BINDINGS + DRIVER_BINDINGS];
    VkWriteDescriptorSet wr[USER_BINDINGS + DRIVER_BINDINGS];
    uint32_t nw = 0;
    for (uint32_t b = 0; b < DRIVER_BINDINGS; b++) {
        if (!(m->driver & (1u << b)))
            continue;
        const nvn_consts *k = &st->consts[NVN_STAGE_COMPUTE];
        if (b == 0)
            rc = bank_of(r, dd->c0, NVN_BANK2_BYTES, &bi[nw]);
        else if (b == 1)
            rc = bank_of(r, k->addr ? r->mem + k->addr : NULL, k->addr ? k->size : 0, &bi[nw]);
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
        if ((rc = bank_of(r, user_bank(r, NVN_STAGE_COMPUTE, b, addr), addr ? size : 0, &bi[nw])) != RL_OK)
            return rc;
        wr[nw] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                                         .dstSet = sets[0],
                                         .dstBinding = USER_BINDINGS * NVN_STAGE_COMPUTE + b,
                                         .descriptorCount = 1,
                                         .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                                         .pBufferInfo = &bi[nw] };
        nw++;
    }
    if (nw)
        VK(vkUpdateDescriptorSets)(g.dev, nw, wr, 0, NULL);
    return RL_OK;
}

bool nvn_compute_table(const nvn_gfx *st, const uint32_t groups[3], uint32_t nubo, uint32_t nssbo, uint32_t ntex,
                       uint32_t nimg, uint32_t end, uint8_t c0[NVN_BANK2_BYTES], uint32_t *img_lo,
                       uint32_t *table_end)
{
    memset(c0, 0, NVN_BANK2_BYTES);
    memcpy(c0, groups, 12);
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
 * its first 12 bytes hold the dispatch's group counts x, y and z (the only
 * per-dispatch values in the driver's dispatch record; how the hardware
 * expands that record into c[0][0..0xc] is not visible in the driver, and 140
 * of the FE compute programs read these words); from 0x10 come the constants
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
static int dispatch(replay *r, uint32_t x, uint32_t y, uint32_t z)
{
    nvn_gfx *st = r->st;
    memset(st->ubo_patched, 0, sizeof st->ubo_patched);
    if (!compute_layouts())
        return nvn_vk_stop(r, "cannot make the compute pipeline layout");
    uint64_t code = st->code[NVN_STAGE_COMPUTE], ctl = st->control[NVN_STAGE_COMPUTE];
    if (!code)
        return nvn_vk_stop(r, "dispatch without a compute program");
    if (!ctl)
        return nvn_vk_stop(r, "the compute program at 0x%llx has no GLSLC control section", (unsigned long long)code);
    module *m = module_of(r, code);
    if (!m)
        return RL_PENDING;
    if (m->other_sets)
        return nvn_vk_stop(r, "compute program %s uses a guest memory set, which the layer does not bind yet", m->key);
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
        if (!nvn_gpu_mapped(st->storage[i].addr, (uint32_t)st->storage[i].size))
            return nvn_vk_stop(r, "compute program %s: storage buffer %u at 0x%llx (0x%x bytes) is not in one memory pool",
                               m->key, i, (unsigned long long)st->storage[i].addr, (uint32_t)st->storage[i].size);
    VkSpecializationMapEntry map[MAX_SPECS];
    uint32_t vals[MAX_SPECS], n = 3;
    for (uint32_t i = 0; i < 3; i++) {
        map[i] = (VkSpecializationMapEntry){ i, 4 * i, 4 };
        vals[i] = rl_ld32(r->mem, ctl + 0x58 + 4 * i);
    }
    int rc = bind_textures(r, m, 0, c0, sizeof c0, &n, map, vals, img_lo, at);
    if (rc != RL_OK)
        return rc;
    if ((rc = bind_user_textures(r, m, NVN_STAGE_COMPUTE, &n, map, vals)) != RL_OK)
        return rc;
    VkPipeline p = compute_pipeline_of(r, m, n, map, vals);
    if (!p)
        return RL_PENDING;
    dispatch_data dd = { .m = m, .c0 = c0 };
    if ((rc = nvn_vk_stage(r, stage_dispatch, &dd, "a dispatch")) != RL_OK)
        return rc;
    nvn_vk_begin(r);
    VK(vkCmdBindPipeline)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
    VK(vkCmdBindDescriptorSets)(g.cb, VK_PIPELINE_BIND_POINT_COMPUTE, d.clayout, 0, 3, dd.sets, 0, NULL);
    VK(vkCmdDispatch)(g.cb, x, y, z);
    nvn_vk_barrier(g.cb);
    return RL_OK;
}

static int bind_program(replay *r, const nvn_cmd *e)
{
    uint32_t n = 0, mask = (uint32_t)e->x[2] & 0x3fu;
    const nvn_consts *consts = NULL;
    const uint64_t *control = NULL;
    const uint64_t *code = e->x[1] ? nvn_program_code(e->x[1], &n, &consts, &control) : NULL;
    if (e->x[1] && !code)
        return nvn_vk_stop(r, "nvnCommandBufferBindProgram: 0x%llx is not a program", (unsigned long long)e->x[1]);
    for (uint32_t s = 0; s < NVN_STAGES; s++) {
        if (!(mask & (1u << s)))
            continue;
        r->st->code[s] = 0;
        r->st->consts[s] = (nvn_consts){ 0, 0 };
        r->st->control[s] = 0;
        for (uint32_t i = 0; i < n; i++)
            if (nvn_shader_stage(r->mem, code[i]) == (int)s) {
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
    case NVN_CMD_PUSH_COMPUTE_CONSTANTS:
    case NVN_CMD_PUSH_GRAPHICS_CONSTANTS: {
        bool compute = e->op == NVN_CMD_PUSH_COMPUTE_CONSTANTS;
        uint32_t stage = compute ? NVN_STAGE_COMPUTE : (uint32_t)e->x[1] & 7u;
        uint32_t slot = compute ? (uint32_t)e->x[1] : 0, off = (uint32_t)e->x[2];
        uint32_t size = (uint32_t)e->x[3] < NVN_PUSH_BYTES ? (uint32_t)e->x[3] & ~3u : NVN_PUSH_BYTES;
        const uint8_t *data = nvn_cmd_bytes(r->s, e->x[4]);
        if (stage >= NVN_STAGES || slot >= NVN_PUSH_SLOTS || off % 4 || off + size > NVN_PUSH_BYTES)
            return nvn_vk_stop(r, "%s of %u bytes at offset %u into slot %u of stage %u", nvn_cmd_name(e->op), size,
                               off, slot, stage);
        if (size && !data)
            return nvn_vk_stop(r, "%s without its data", nvn_cmd_name(e->op));
        if (size)
            memcpy(st->push[stage][slot] + off, data, size);
        st->push_used[stage] |= 1u << slot;
        return RL_OK;
    }
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
    case NVN_CMD_SET_RASTERIZER_DISCARD:
        st->rasterizer_discard = (e->x[1] & 0xff) != 0;
        return RL_OK;
    case NVN_CMD_SET_CONSERVATIVE_RASTER_ENABLE:
        st->conservative_raster = (e->x[1] & 0xff) != 0;
        if (st->conservative_raster && !g.conservative_raster && !g.conservative_warned) {
            g.conservative_warned = true;
            rl_log(RL_LOG_WARN, "nvn", "conservative rasterization is requested and the host device has no VK_EXT_conservative_rasterization: "
                   "primitives are rasterized normally");
        }
        return RL_OK;
    case NVN_CMD_SET_PATCH_SIZE:
        return RL_OK;
    case NVN_CMD_SET_LINE_WIDTH:
        memcpy(&st->line_width, &e->d[0], 4);
        return RL_OK;
    case NVN_CMD_SET_BLEND_COLOR:
        for (int i = 0; i < 4; i++)
            memcpy(&st->blend_color[i], &e->d[i], 4);
        return RL_OK;
    case NVN_CMD_REPORT_COUNTER:
        return nvn_counters_report(r, (uint32_t)e->x[1], e->x[2]);
    case NVN_CMD_RESET_COUNTER:
        return nvn_counters_reset(r, (uint32_t)e->x[1]);
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
    case NVN_CMD_DRAW_ELEMENTS_INSTANCED:
        st->inst_base = (uint32_t)e->x[6];
        st->inst_count = (uint32_t)e->x[7];
        return st->inst_count ? draw(r, (uint32_t)e->x[1], true, (uint32_t)e->x[2], e->x[4], (int32_t)e->x[5], 0,
                                     (uint32_t)e->x[3])
                              : RL_OK;
    default:
        return 1;
    }
}
