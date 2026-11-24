/**
 * @file nvn_pipe.c
 * @brief Pipelines and the persistent pipeline cache (see nvn_pipe.h).
 *
 * The cache file is "RLPLC001", the identity it was made for (device and
 * driver UUIDs, driver version, vendor and device ids, the SHA-256 of the
 * build's shader table index), the SHA-256 of the driver's cache data, and
 * that data. Any difference discards the file.
 */
#include "nvn_module.h"
#include "nvn_pipe.h"

#include <relift/datacheck.h>
#include <relift/settings.h>
#include <relift/warmup.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#define CACHE_MAGIC "RLPLC001"
#define CACHE_IDENTITY 112u
#define CACHE_HEAD (8u + 64u + CACHE_IDENTITY + 8u)
#define SAVE_INTERVAL_NS (60ull * 1000000000ull)
/** @brief How soon after a pipeline compiled during play the keys and cache are written again. */
#define SAVE_AFTER_LATE_NS (2ull * 1000000000ull)

/** @brief One made thing (a library or a pipeline) in a table keyed by bytes. */
typedef struct entry {
    uint8_t *key;
    size_t len;
    uint32_t hash;
    VkPipeline p;
    /** @brief 0 while a thread is making it, 1 made, 2 the driver refused. */
    int state;
} entry;

typedef struct table {
    entry **slot;
    uint32_t cap, n;
} table;


static struct {
    VkPipelineLayout layout, clayout, mlayout;
    VkPipelineCache cache;
    bool gpl, warmed, dirty, cache_ctl;
    pthread_mutex_t lock;
    pthread_cond_t cv;
    table vi, pr, fs, fo, full, comp, mcomp;
    uint64_t made, made_ns, late, late_ns, saved_ns, loaded;
    /** @brief When the last pipeline was compiled after warm-up (0: none since the last save), and the saver thread. */
    uint64_t late_at;
    bool saver;
    pthread_mutex_t save_lock;
    nvn_pipe_rec *old, *fresh;
    uint32_t nold, nfresh, capfresh;
    table seen;
    uint8_t identity[CACHE_IDENTITY];
} P = { .lock = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER, .save_lock = PTHREAD_MUTEX_INITIALIZER };

void nvn_spec_sort(uint32_t *n, VkSpecializationMapEntry *map, uint32_t *vals)
{
    for (uint32_t i = 1; i < *n; i++) {
        VkSpecializationMapEntry e = map[i];
        uint32_t v = vals[i], j = i;
        while (j && map[j - 1].constantID > e.constantID) {
            map[j] = map[j - 1];
            vals[j] = vals[j - 1];
            j--;
        }
        map[j] = e;
        vals[j] = v;
    }
    uint32_t out = 0;
    for (uint32_t i = 0; i < *n; i++) {
        if (out && map[out - 1].constantID == map[i].constantID)
            continue;
        map[out] = map[i];
        vals[out++] = vals[i];
    }
    for (uint32_t i = 0; i < out; i++)
        map[i] = (VkSpecializationMapEntry){ map[i].constantID, 4 * i, 4 };
    *n = out;
}

void nvn_pipe_set_layouts(VkPipelineLayout graphics, VkPipelineLayout compute)
{
    P.layout = graphics;
    P.clayout = compute;
}

void nvn_pipe_set_mesh_layout(VkPipelineLayout mesh) { P.mlayout = mesh; }

bool nvn_pipe_uses_libraries(void) { return P.gpl; }

/** @brief A hash of n bytes for the in-memory tables, eight bytes a step (keys are a few hundred bytes). */
static uint32_t hash_bytes(const void *p, size_t n)
{
    const uint8_t *b = p;
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 0xff51afd7ed558ccdull;
        h ^= h >> 32;
    }
    uint64_t w = 0;
    memcpy(&w, b + i, n - i);
    h = (h ^ w) * 0xc4ceb9fe1a85ec53ull;
    h ^= h >> 29;
    return (uint32_t)h ^ (uint32_t)(h >> 32);
}

/** @brief Why the calling thread's last make failed (nvn_pipe_why). */
static _Thread_local char t_why[160];

/** @brief The entry for key in t; P.lock held. NULL when there is none. */
static entry *find_locked(table *t, const void *key, size_t len, uint32_t h)
{
    if (!t->cap)
        return NULL;
    for (uint32_t i = h & (t->cap - 1);; i = (i + 1) & (t->cap - 1)) {
        entry *e = t->slot[i];
        if (!e)
            return NULL;
        if (e->hash == h && e->len == len && !memcmp(e->key, key, len))
            return e;
    }
}

/** @brief Adds e to t; P.lock held. False when memory runs out. */
static bool add_locked(table *t, entry *e)
{
    if ((t->n + 1) * 2 > t->cap) {
        uint32_t cap = t->cap ? t->cap * 2 : 256;
        entry **slot = calloc(cap, sizeof *slot);
        if (!slot)
            return false;
        for (uint32_t i = 0; i < t->cap; i++)
            if (t->slot[i]) {
                uint32_t j = t->slot[i]->hash & (cap - 1);
                while (slot[j])
                    j = (j + 1) & (cap - 1);
                slot[j] = t->slot[i];
            }
        free(t->slot);
        t->slot = slot;
        t->cap = cap;
    }
    uint32_t j = e->hash & (t->cap - 1);
    while (t->slot[j])
        j = (j + 1) & (t->cap - 1);
    t->slot[j] = e;
    t->n++;
    return true;
}

/**
 * @brief The entry for key in t. When no thread has started making it, this
 * one claims it (*mine) and must call finish; when another is making it,
 * waits until that one has finished. NULL when memory runs out.
 */
/** @brief The pipeline made for key in t, or NULL when nobody has, or somebody is making it now; never waits. */
static VkPipeline peek(table *t, const void *key, size_t len)
{
    uint32_t h = hash_bytes(key, len);
    pthread_mutex_lock(&P.lock);
    entry *e = find_locked(t, key, len, h);
    VkPipeline p = e && e->state == 1 ? e->p : NULL;
    pthread_mutex_unlock(&P.lock);
    return p;
}

static entry *claim(table *t, const void *key, size_t len, bool *mine)
{
    uint32_t h = hash_bytes(key, len);
    pthread_mutex_lock(&P.lock);
    entry *e = find_locked(t, key, len, h);
    *mine = false;
    if (!e) {
        e = calloc(1, sizeof *e);
        uint8_t *k = malloc(len);
        if (e && k) {
            memcpy(k, key, len);
            e->key = k;
            e->len = len;
            e->hash = h;
        }
        if (!e || !k || !add_locked(t, e)) {
            free(e);
            free(k);
            pthread_mutex_unlock(&P.lock);
            return NULL;
        }
        *mine = true;
    } else {
        while (e->state == 0)
            pthread_cond_wait(&P.cv, &P.lock);
        if (e->state == 2)
            snprintf(t_why, sizeof t_why, "an earlier make of the same pipeline or library failed");
    }
    pthread_mutex_unlock(&P.lock);
    return e;
}

static void finish(entry *e, VkPipeline p)
{
    pthread_mutex_lock(&P.lock);
    e->p = p;
    e->state = p ? 1 : 2;
    pthread_cond_broadcast(&P.cv);
    pthread_mutex_unlock(&P.lock);
}

/** @brief The calling thread makes the warm-up's own items, which the compile screen counts itself. */
static _Thread_local bool t_quiet;


const char *nvn_pipe_why(void) { return t_why[0] ? t_why : "no reason was noted"; }

/** @brief Notes why a make failed, for nvn_pipe_why. */
static void note_why(const char *what, VkResult vr)
{
    snprintf(t_why, sizeof t_why, "the driver returned %d making the %s", (int)vr, what);
}

void nvn_pipe_screen_quiet(void) { t_quiet = true; }

/** @brief A compile the calling thread waits for is queued: the compile screen counts it. */
static void screen_queue(void)
{
    if (!t_quiet)
        rl_warmup_queue(1);
}

/** @brief That compile is done; a pass that has run long enough with no window gets one opened. */
static void screen_done(bool failed)
{
    if (t_quiet)
        return;
    rl_warmup_done(1, failed ? 1u : 0u);
    if (rl_warmup_window_due())
        (void)nvn_vk_window_early();
}

/** @brief Creates a graphics pipeline, timing it and counting it as late when it compiles after warm-up. */
/** @brief The result of the calling thread's last driver pipeline create (nvn_pipe_last_result). */
static _Thread_local VkResult t_last_result = VK_SUCCESS;

int nvn_pipe_last_result(void) { return (int)t_last_result; }

static VkResult create_graphics(VkGraphicsPipelineCreateInfo *ci, const char *what, VkPipeline *out)
{
    bool probe = P.warmed && P.cache_ctl;
    uint64_t t0 = rl_clock_ns();
    VkPipelineCreateFlags flags = ci->flags;
    if (probe)
        ci->flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    else
        screen_queue();
    VkResult vr = VK(vkCreateGraphicsPipelines)(g.dev, P.cache, 1, ci, NULL, out);
    bool compiled = false;
    if (vr == VK_PIPELINE_COMPILE_REQUIRED) {
        ci->flags = flags;
        screen_queue();
        t0 = rl_clock_ns();
        vr = VK(vkCreateGraphicsPipelines)(g.dev, P.cache, 1, ci, NULL, out);
        compiled = true;
    }
    /* Late: a compile after the warm-up that a draw waits for; the warm-up's own workers are not late. */
    bool late = compiled && !t_quiet;
    uint64_t dt = rl_clock_ns() - t0;
    t_last_result = vr;
    if (compiled || !probe)
        screen_done(vr != VK_SUCCESS);
    pthread_mutex_lock(&P.lock);
    if (compiled || !probe) {
        P.made++;
        P.made_ns += dt;
        P.dirty = true;
    }
    if (late) {
        P.late++;
        P.late_ns += dt;
        P.late_at = rl_clock_ns();
    }
    pthread_mutex_unlock(&P.lock);
    if (late || rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "%s%s created in %.1f ms", late ? "late compile: " : "", what, dt / 1e6);
    if (vr != VK_SUCCESS)
        note_why(what, vr);
    return vr;
}

static const VkDynamicState dynamic_states[] = {
    VK_DYNAMIC_STATE_VIEWPORT,
    VK_DYNAMIC_STATE_SCISSOR,
    VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
    VK_DYNAMIC_STATE_STENCIL_WRITE_MASK,
    VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    VK_DYNAMIC_STATE_DEPTH_BIAS,
    VK_DYNAMIC_STATE_BLEND_CONSTANTS,
    VK_DYNAMIC_STATE_LINE_WIDTH,
    VK_DYNAMIC_STATE_CULL_MODE,
    VK_DYNAMIC_STATE_FRONT_FACE,
    VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE,
    VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
    VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE,
    VK_DYNAMIC_STATE_STENCIL_OP,
    VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE,
    VK_DYNAMIC_STATE_RASTERIZER_DISCARD_ENABLE,
};

void nvn_pipe_set_dynamic(VkCommandBuffer cb, const nvn_pipe_key *k)
{
    VK(vkCmdSetCullMode)(cb, k->cull);
    VK(vkCmdSetFrontFace)(cb, (VkFrontFace)k->front);
    VK(vkCmdSetDepthTestEnable)(cb, k->depth_test);
    VK(vkCmdSetDepthWriteEnable)(cb, k->depth_write);
    VK(vkCmdSetDepthCompareOp)(cb, (VkCompareOp)k->depth_func);
    VK(vkCmdSetStencilTestEnable)(cb, k->stencil_test);
    VK(vkCmdSetStencilOp)(cb, VK_STENCIL_FACE_FRONT_BIT, k->front_ops.failOp, k->front_ops.passOp,
                          k->front_ops.depthFailOp, k->front_ops.compareOp);
    VK(vkCmdSetStencilOp)(cb, VK_STENCIL_FACE_BACK_BIT, k->back_ops.failOp, k->back_ops.passOp, k->back_ops.depthFailOp,
                          k->back_ops.compareOp);
    VK(vkCmdSetDepthBiasEnable)(cb, k->bias_enable);
    VK(vkCmdSetRasterizerDiscardEnable)(cb, k->rasterizer_discard);
}

static const VkShaderStageFlagBits stage_bits[NVN_STAGES] = { VK_SHADER_STAGE_VERTEX_BIT,
                                                              VK_SHADER_STAGE_FRAGMENT_BIT,
                                                              VK_SHADER_STAGE_GEOMETRY_BIT,
                                                              VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT,
                                                              VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT,
                                                              0,
                                                              VK_SHADER_STAGE_TASK_BIT_EXT,
                                                              VK_SHADER_STAGE_MESH_BIT_EXT };

/** @brief The create infos that several libraries and the monolithic pipeline share. */
typedef struct state {
    VkPipelineShaderStageCreateInfo stages[NVN_STAGES];
    VkSpecializationInfo si[NVN_STAGES];
    uint32_t ns;
    VkPipelineVertexInputStateCreateInfo vi;
    VkPipelineInputAssemblyStateCreateInfo ia;
    VkPipelineTessellationStateCreateInfo ts;
    int swizzle_axes[4];
    struct {
        VkStructureType sType;
        const void *pNext;
        VkFlags flags;
        uint32_t viewportCount;
        const int *pViewportSwizzles;
    } swizzle;
    struct {
        VkStructureType sType;
        const void *pNext;
        VkBool32 negativeOneToOne;
    } clip;
    VkPipelineViewportStateCreateInfo vp;
    struct {
        VkStructureType sType;
        const void *pNext;
        VkFlags flags;
        int conservativeRasterizationMode;
        float extraPrimitiveOverestimationSize;
    } conservative;
    struct {
        VkStructureType sType;
        const void *pNext;
        int provokingVertexMode;
    } provoking;
    VkSampleMask sample_mask;
    VkPipelineRasterizationStateCreateInfo rs;
    VkPipelineMultisampleStateCreateInfo ms;
    VkPipelineDepthStencilStateCreateInfo ds;
    VkPipelineColorBlendStateCreateInfo cb;
    VkPipelineDynamicStateCreateInfo dy;
    VkPipelineRenderingCreateInfo ri, ri_view;
} state;

static void fill_state(state *s, const nvn_pipe_key *k)
{
    s->ns = 0;
    for (uint32_t i = 0; i < NVN_STAGES; i++)
        if (k->mods[i] && stage_bits[i]) {
            s->si[i] = (VkSpecializationInfo){ k->nspec[i], k->spec_map[i], 4u * k->nspec[i], k->spec[i] };
            s->stages[s->ns++] = (VkPipelineShaderStageCreateInfo){
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = stage_bits[i],
                .module = k->mods[i],
                .pName = "main",
                .pSpecializationInfo = k->nspec[i] ? &s->si[i] : NULL
            };
        }
    s->vi = (VkPipelineVertexInputStateCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
                                                    .vertexBindingDescriptionCount = k->nbind,
                                                    .pVertexBindingDescriptions = k->bind,
                                                    .vertexAttributeDescriptionCount = k->nattr,
                                                    .pVertexAttributeDescriptions = k->attr };
    s->ia = (VkPipelineInputAssemblyStateCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
                                                      .topology = (VkPrimitiveTopology)k->topology,
                                                      .primitiveRestartEnable = k->primitive_restart };
    s->ts = (VkPipelineTessellationStateCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
                                                     .patchControlPoints = k->patch_points ? k->patch_points : 1 };
    uint32_t sw = k->viewport_swizzle;
    s->swizzle_axes[0] = (int)(sw & 7);
    s->swizzle_axes[1] = (int)(sw >> 4 & 7);
    s->swizzle_axes[2] = (int)(sw >> 8 & 7);
    s->swizzle_axes[3] = (int)(sw >> 12 & 7);
    s->swizzle.sType = (VkStructureType)1000098000;
    s->swizzle.pNext = NULL;
    s->swizzle.flags = 0;
    s->swizzle.viewportCount = 1;
    s->swizzle.pViewportSwizzles = s->swizzle_axes;
    bool swizzled = sw != NVN_VIEWPORT_SWIZZLE_IDENTITY;
    s->clip.sType = (VkStructureType)1000355001;
    s->clip.pNext = swizzled ? &s->swizzle : NULL;
    s->clip.negativeOneToOne = VK_TRUE;
    s->vp = (VkPipelineViewportStateCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
                                                 .pNext = k->negative_one_to_one ? (const void *)&s->clip
                                                          : swizzled             ? (const void *)&s->swizzle
                                                                                 : NULL,
                                                 .viewportCount = 1,
                                                 .scissorCount = 1 };
    s->conservative.sType = (VkStructureType)1000101001;
    s->conservative.pNext = NULL;
    s->conservative.flags = 0;
    s->conservative.conservativeRasterizationMode = 1;
    s->conservative.extraPrimitiveOverestimationSize = 0.25f * (float)k->conservative_dilate;
    s->provoking.sType = (VkStructureType)1000254001;
    s->provoking.pNext = k->conservative_raster ? &s->conservative : NULL;
    s->provoking.provokingVertexMode = 1;
    s->rs = (VkPipelineRasterizationStateCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
                                                      .pNext = k->provoking_last ? (const void *)&s->provoking
                                                               : k->conservative_raster ? (const void *)&s->conservative
                                                                                        : NULL,
                                                      .polygonMode = (VkPolygonMode)k->polygon,
                                                      .cullMode = VK_CULL_MODE_NONE,
                                                      .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
                                                      .depthClampEnable = k->depth_clamp,
                                                      .lineWidth = 1.0f };
    s->sample_mask = k->sample_mask;
    s->ms = (VkPipelineMultisampleStateCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
                                                     .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
                                                     .pSampleMask = &s->sample_mask };
    s->ds = (VkPipelineDepthStencilStateCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    s->cb = (VkPipelineColorBlendStateCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
                                                   .attachmentCount = k->ncolor,
                                                   .pAttachments = k->blend };
    s->dy = (VkPipelineDynamicStateCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
                                                .dynamicStateCount = sizeof dynamic_states / sizeof dynamic_states[0],
                                                .pDynamicStates = dynamic_states };
    s->ri = (VkPipelineRenderingCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO,
                                             .colorAttachmentCount = k->ncolor,
                                             .pColorAttachmentFormats = k->color,
                                             .depthAttachmentFormat = k->depth,
                                             .stencilAttachmentFormat = k->stencil };
    s->ri_view = (VkPipelineRenderingCreateInfo){ .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO };
}

/** @brief The key bytes of each library kind: what the driver compiles in. */
typedef struct vi_key {
    uint32_t topology, primitive_restart, nattr, nbind;
    VkVertexInputAttributeDescription attr[NVN_VERTEX_ATTRIBS];
    VkVertexInputBindingDescription bind[NVN_VERTEX_BUFFERS + NVN_VERTEX_ATTRIBS];
} vi_key;

typedef struct stage_key {
    VkShaderModule m;
    uint32_t nspec;
    VkSpecializationMapEntry map[NVN_MAX_SPECS];
    uint32_t val[NVN_MAX_SPECS];
    uint32_t polygon, depth_clamp, conservative, negative_one_to_one, conservative_dilate, viewport_swizzle, provoking_last, sample_mask;
} stage_key;

typedef struct fo_key {
    uint32_t ncolor;
    VkFormat color[MAX_RT], depth, stencil;
    VkPipelineColorBlendAttachmentState blend[MAX_RT];
    uint32_t sample_mask;
} fo_key;

static void vi_of(const nvn_pipe_key *k, vi_key *o)
{
    memset(o, 0, sizeof *o);
    o->topology = k->topology;
    o->primitive_restart = k->primitive_restart;
    o->nattr = k->nattr;
    o->nbind = k->nbind;
    memcpy(o->attr, k->attr, k->nattr * sizeof o->attr[0]);
    memcpy(o->bind, k->bind, k->nbind * sizeof o->bind[0]);
}

static void stage_of(const nvn_pipe_key *k, uint32_t s, stage_key *o)
{
    memset(o, 0, sizeof *o);
    o->m = k->mods[s];
    o->nspec = k->nspec[s];
    o->sample_mask = s == 1 ? k->sample_mask : 0;
    memcpy(o->map, k->spec_map[s], k->nspec[s] * sizeof o->map[0]);
    memcpy(o->val, k->spec[s], k->nspec[s] * sizeof o->val[0]);
    nvn_spec_sort(&o->nspec, o->map, o->val);
    if (s == 0) {
        o->polygon = k->polygon;
        o->depth_clamp = k->depth_clamp;
        o->conservative = k->conservative_raster;
        o->negative_one_to_one = k->negative_one_to_one;
        o->conservative_dilate = k->conservative_dilate;
        o->viewport_swizzle = k->viewport_swizzle;
        o->provoking_last = k->provoking_last;
    }
}

static void fo_of(const nvn_pipe_key *k, fo_key *o)
{
    memset(o, 0, sizeof *o);
    o->ncolor = k->ncolor;
    memcpy(o->color, k->color, k->ncolor * sizeof o->color[0]);
    o->depth = k->depth;
    o->stencil = k->stencil;
    o->sample_mask = k->sample_mask;
    memcpy(o->blend, k->blend, k->ncolor * sizeof o->blend[0]);
}

/** @brief The library of kind bits for k (flags is one VkGraphicsPipelineLibraryFlagBitsEXT), made or waited for. */
static VkPipeline library(table *t, const void *key, size_t len, VkGraphicsPipelineLibraryFlagsEXT part, const nvn_pipe_key *k,
                          const char *what)
{
    bool mine;
    entry *e = claim(t, key, len, &mine);
    if (!e)
        return NULL;
    if (!mine)
        return e->p;
    state s;
    fill_state(&s, k);
    VkGraphicsPipelineLibraryCreateInfoEXT lib = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
                                                   .flags = part };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .pNext = &lib,
                                        .flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR,
                                        .layout = P.layout };
    if (part == VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT) {
        ci.pVertexInputState = &s.vi;
        ci.pInputAssemblyState = &s.ia;
    } else if (part == VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT) {
        lib.pNext = &s.ri_view;
        ci.stageCount = 1;
        ci.pStages = s.stages;
        ci.pViewportState = &s.vp;
        ci.pRasterizationState = &s.rs;
        ci.pDynamicState = &s.dy;
    } else if (part == VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT) {
        lib.pNext = &s.ri_view;
        VkPipelineShaderStageCreateInfo *fs = NULL;
        for (uint32_t i = 0; i < s.ns; i++)
            if (s.stages[i].stage == VK_SHADER_STAGE_FRAGMENT_BIT)
                fs = &s.stages[i];
        ci.stageCount = fs ? 1 : 0;
        ci.pStages = fs;
        ci.pDepthStencilState = &s.ds;
        ci.pMultisampleState = &s.ms;
        ci.pDynamicState = &s.dy;
    } else {
        lib.pNext = &s.ri;
        ci.pColorBlendState = &s.cb;
        ci.pMultisampleState = &s.ms;
        ci.pDynamicState = &s.dy;
    }
    VkPipeline p = NULL;
    if (create_graphics(&ci, what, &p) != VK_SUCCESS)
        p = NULL;
    finish(e, p);
    return p;
}

bool nvn_spec_at_run_time(uint32_t n, const VkSpecializationMapEntry *map, uint32_t *vals)
{
    bool any = false;
    for (uint32_t i = 0; i < n; i++)
        if (map[i].constantID >= NVN_FETCH_KIND_SPEC && map[i].constantID < NVN_FETCH_KIND_SPEC + 0x100u &&
            vals[i] != NVN_KIND_AT_RUN_TIME) {
            vals[i] = NVN_KIND_AT_RUN_TIME;
            any = true;
        }
    return any;
}

/** @brief A library or compute pipeline of exact texture kinds the exact-kind worker is to make. */
typedef struct exact_job {
    int what;
    nvn_pipe_key k;
    stage_key sk;
    table *t;
    VkPipelineLayout layout;
} exact_job;

static struct {
    pthread_mutex_t lock;
    pthread_cond_t cv;
    exact_job *q;
    uint32_t n, cap;
    bool started;
} X = { .lock = PTHREAD_MUTEX_INITIALIZER, .cv = PTHREAD_COND_INITIALIZER };

static VkPipeline vertex_library(const nvn_pipe_key *k);
static VkPipeline fragment_library(const nvn_pipe_key *k);
static VkPipeline compute_in(table *t, VkPipelineLayout layout, VkShaderModule m, uint32_t n,
                             const VkSpecializationMapEntry *map, const uint32_t *vals);

/** @brief The exact-kind worker: makes the queued libraries and compute pipelines, newest first, off the game's threads. */
static void *exact_main(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&X.lock);
        while (!X.n)
            pthread_cond_wait(&X.cv, &X.lock);
        exact_job *j = malloc(sizeof *j);
        if (j)
            *j = X.q[--X.n];
        pthread_mutex_unlock(&X.lock);
        if (!j)
            continue;
        if (j->what == 0)
            vertex_library(&j->k);
        else if (j->what == 1)
            fragment_library(&j->k);
        else
            compute_in(j->t, j->layout, j->sk.m, j->sk.nspec, j->sk.map, j->sk.val);
        free(j);
    }
    return NULL;
}

/** @brief Queues j for the exact-kind worker, starting it the first time. */
static void exact_queue(const exact_job *j)
{
    pthread_mutex_lock(&X.lock);
    if (X.n == X.cap) {
        uint32_t cap = X.cap ? X.cap * 2 : 16;
        exact_job *q = realloc(X.q, cap * sizeof *q);
        if (!q) {
            pthread_mutex_unlock(&X.lock);
            return;
        }
        X.q = q;
        X.cap = cap;
    }
    X.q[X.n++] = *j;
    bool start = !X.started;
    X.started = true;
    pthread_cond_signal(&X.cv);
    pthread_mutex_unlock(&X.lock);
    if (start) {
        pthread_t t;
        if (pthread_create(&t, NULL, exact_main, NULL) == 0)
            pthread_detach(t);
    }
}

static VkPipeline vertex_library(const nvn_pipe_key *k)
{
    nvn_pipe_key v = *k;
    v.mods[1] = NULL;
    stage_key sk;
    stage_of(&v, 0, &sk);
    return library(&P.pr, &sk, sizeof sk, VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT, k,
                   "pre-raster library");
}

static VkPipeline fragment_library(const nvn_pipe_key *k)
{
    nvn_pipe_key f = *k;
    f.mods[0] = NULL;
    stage_key sk;
    stage_of(&f, 1, &sk);
    return library(&P.fs, &sk, sizeof sk, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT, &f, "fragment library");
}

bool nvn_pipe_warm_vertex(const nvn_pipe_key *k) { return P.gpl && vertex_library(k) != NULL; }

bool nvn_pipe_warm_fragment(const nvn_pipe_key *k) { return P.gpl && fragment_library(k) != NULL; }

/** @brief The monolithic pipeline for k, which a device without libraries makes at the draw. */
static VkPipeline monolithic(const nvn_pipe_key *k)
{
    state s;
    fill_state(&s, k);
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .pNext = &s.ri,
                                        .stageCount = s.ns,
                                        .pStages = s.stages,
                                        .pVertexInputState = k->mesh_mode == 1 ? NULL : &s.vi,
                                        .pInputAssemblyState = k->mesh_mode == 1 ? NULL : &s.ia,
                                        .pTessellationState = k->mods[3] ? &s.ts : NULL,
                                        .pViewportState = &s.vp,
                                        .pRasterizationState = &s.rs,
                                        .pMultisampleState = &s.ms,
                                        .pDepthStencilState = &s.ds,
                                        .pColorBlendState = &s.cb,
                                        .pDynamicState = &s.dy,
                                        .layout = k->mesh_mode == 2 ? P.mlayout : P.layout };
    VkPipeline p = NULL;
    if (create_graphics(&ci, k->mesh_mode ? "mesh pipeline" : "pipeline", &p) != VK_SUCCESS)
        return NULL;
    return p;
}

bool nvn_pipe_save_due(uint64_t now, uint64_t saved, uint64_t late_at, bool dirty)
{
    if (!dirty)
        return false;
    return now - saved > SAVE_INTERVAL_NS || (late_at && now - late_at > SAVE_AFTER_LATE_NS);
}

/** @brief The saver thread: writes the keys and the cache once nvn_pipe_save_due says so, off the game's threads. */
static void *saver_main(void *arg)
{
    (void)arg;
    for (;;) {
        rl_sleep_ns(250000000ull);
        pthread_mutex_lock(&P.lock);
        bool due = nvn_pipe_save_due(rl_clock_ns(), P.saved_ns, P.late_at, P.dirty);
        pthread_mutex_unlock(&P.lock);
        if (due)
            nvn_pipe_save();
    }
    return NULL;
}

static void maybe_save(void)
{
    if (__atomic_load_n(&P.saver, __ATOMIC_ACQUIRE))
        return;
    pthread_mutex_lock(&P.lock);
    bool start = !P.saver;
    P.saver = true;
    pthread_mutex_unlock(&P.lock);
    if (!start)
        return;
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (pthread_create(&t, &attr, saver_main, NULL) != 0)
        rl_log(RL_LOG_WARN, "nvn", "cannot start the pipeline cache saver: pipelines are saved at exit only");
    pthread_attr_destroy(&attr);
    rl_at_process_exit(nvn_pipe_save);
}

/** @brief A key with geometry or tessellation programs, which the pre-raster library (the vertex program alone) leaves out. */
static bool extra_stages(const nvn_pipe_key *k) { return k->mods[2] || k->mods[3] || k->mods[4]; }

VkPipeline nvn_pipe_graphics(const nvn_pipe_key *k)
{
    maybe_save();
    if (!P.gpl || k->mesh_mode || extra_stages(k)) {
        bool mine;
        entry *e = claim(&P.full, k, sizeof *k, &mine);
        if (!e || !mine)
            return e ? e->p : NULL;
        VkPipeline p = monolithic(k);
        finish(e, p);
        return p;
    }
    VkPipeline libs[4];
    vi_key vk;
    vi_of(k, &vk);
    fo_key fk;
    fo_of(k, &fk);
    libs[0] = library(&P.vi, &vk, sizeof vk, VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT, k,
                      "vertex input library");
    for (uint32_t s = 0; s < 2; s++) {
        nvn_pipe_key part = *k;
        part.mods[1 - s] = NULL;
        stage_key sk;
        stage_of(&part, s, &sk);
        VkPipeline have = peek(s ? &P.fs : &P.pr, &sk, sizeof sk);
        nvn_pipe_key rt = *k;
        if (!have && P.warmed && nvn_spec_at_run_time(rt.nspec[s], rt.spec_map[s], rt.spec[s])) {
            exact_job j = { .what = (int)s, .k = *k };
            exact_queue(&j);
            libs[1 + s] = s ? fragment_library(&rt) : vertex_library(&rt);
        } else {
            libs[1 + s] = have ? have : s ? fragment_library(k) : vertex_library(k);
        }
    }
    libs[3] = library(&P.fo, &fk, sizeof fk, VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT, k,
                      "fragment output library");
    if (!libs[0] || !libs[1] || !libs[2] || !libs[3])
        return NULL;
    bool mine;
    entry *e = claim(&P.full, libs, sizeof libs, &mine);
    if (!e || !mine)
        return e ? e->p : NULL;
    VkPipelineLibraryCreateInfoKHR li = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
                                          .libraryCount = 4,
                                          .pLibraries = libs };
    VkGraphicsPipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
                                        .pNext = &li,
                                        .layout = P.layout };
    VkPipeline p = NULL;
    if (create_graphics(&ci, "linked pipeline", &p) != VK_SUCCESS)
        p = NULL;
    finish(e, p);
    return p;
}

/** @brief The compute pipeline of m with n specialization constants in layout, kept in t. */
static VkPipeline compute_in(table *t, VkPipelineLayout layout, VkShaderModule m, uint32_t n,
                             const VkSpecializationMapEntry *map, const uint32_t *vals)
{
    maybe_save();
    stage_key sk;
    memset(&sk, 0, sizeof sk);
    sk.m = m;
    sk.nspec = n;
    memcpy(sk.map, map, n * sizeof sk.map[0]);
    memcpy(sk.val, vals, n * sizeof sk.val[0]);
    bool mine;
    entry *e = claim(t, &sk, sizeof sk, &mine);
    if (!e || !mine)
        return e ? e->p : NULL;
    VkSpecializationInfo si = { n, map, 4u * n, vals };
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                       .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                                  .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                                  .module = m,
                                                  .pName = "main",
                                                  .pSpecializationInfo = &si },
                                       .layout = layout };
    bool probe = P.warmed && P.cache_ctl;
    uint64_t t0 = rl_clock_ns();
    if (probe)
        ci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    else
        screen_queue();
    VkPipeline p = NULL;
    VkResult vr = VK(vkCreateComputePipelines)(g.dev, P.cache, 1, &ci, NULL, &p);
    bool compiled = false;
    if (vr == VK_PIPELINE_COMPILE_REQUIRED) {
        ci.flags = 0;
        screen_queue();
        t0 = rl_clock_ns();
        vr = VK(vkCreateComputePipelines)(g.dev, P.cache, 1, &ci, NULL, &p);
        compiled = true;
    }
    bool late = compiled && !t_quiet;
    uint64_t dt = rl_clock_ns() - t0;
    t_last_result = vr;
    if (compiled || !probe)
        screen_done(vr != VK_SUCCESS);
    pthread_mutex_lock(&P.lock);
    if (compiled || !probe) {
        P.made++;
        P.made_ns += dt;
        P.dirty = true;
    }
    if (late) {
        P.late++;
        P.late_ns += dt;
        P.late_at = rl_clock_ns();
    }
    pthread_mutex_unlock(&P.lock);
    if (late || rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "%scompute pipeline created in %.1f ms", late ? "late compile: " : "", dt / 1e6);
    if (vr != VK_SUCCESS) {
        note_why("compute pipeline", vr);
        p = NULL;
    }
    finish(e, p);
    return p;
}

/**
 * @brief The compute pipeline of m in t: the exact one when it is made; while it is not (after warm-up), the one that
 * reads its texture kinds at run time, with the exact one queued for the exact-kind worker.
 */
static VkPipeline compute_exact_or_run_time(table *t, VkPipelineLayout layout, VkShaderModule m, uint32_t n,
                                            const VkSpecializationMapEntry *map, const uint32_t *vals)
{
    stage_key sk;
    memset(&sk, 0, sizeof sk);
    sk.m = m;
    sk.nspec = n;
    memcpy(sk.map, map, n * sizeof sk.map[0]);
    memcpy(sk.val, vals, n * sizeof sk.val[0]);
    VkPipeline have = peek(t, &sk, sizeof sk);
    if (have)
        return have;
    uint32_t rt[NVN_MAX_SPECS];
    memcpy(rt, vals, n * sizeof rt[0]);
    if (!P.warmed || !nvn_spec_at_run_time(n, map, rt))
        return compute_in(t, layout, m, n, map, vals);
    exact_job j = { .what = 2, .sk = sk, .t = t, .layout = layout };
    exact_queue(&j);
    return compute_in(t, layout, m, n, map, rt);
}

VkPipeline nvn_pipe_compute(VkShaderModule m, uint32_t n, const VkSpecializationMapEntry *map, const uint32_t *vals)
{
    return compute_exact_or_run_time(&P.comp, P.clayout, m, n, map, vals);
}

VkPipeline nvn_pipe_mesh_compute(VkShaderModule m, uint32_t n, const VkSpecializationMapEntry *map, const uint32_t *vals)
{
    return compute_in(&P.mcomp, P.mlayout, m, n, map, vals);
}

/** @brief Adds rec to the records of this launch unless one with the same bytes is there; true when it was new. */
static bool record(const nvn_pipe_rec *rec)
{
    bool mine;
    entry *e = claim(&P.seen, rec, sizeof *rec, &mine);
    if (!e || !mine)
        return false;
    finish(e, NULL);
    pthread_mutex_lock(&P.lock);
    if (P.nfresh == P.capfresh) {
        uint32_t cap = P.capfresh ? P.capfresh * 2 : 256;
        nvn_pipe_rec *r = realloc(P.fresh, cap * sizeof *r);
        if (!r) {
            pthread_mutex_unlock(&P.lock);
            return false;
        }
        P.fresh = r;
        P.capfresh = cap;
    }
    P.fresh[P.nfresh++] = *rec;
    P.dirty = true;
    pthread_mutex_unlock(&P.lock);
    return true;
}

bool nvn_pipe_record_program(const char *key, uint32_t stage, const uint32_t wg[3])
{
    if (stage != NVN_STAGE_COMPUTE && !P.gpl)
        return false;
    nvn_pipe_rec rec;
    memset(&rec, 0, sizeof rec);
    rec.kind = NVN_REC_PROGRAM;
    memcpy(rec.keys[5], key, 65);
    rec.k.topology = stage;
    memcpy(rec.k.spec[0], wg, 3 * sizeof wg[0]);
    return record(&rec);
}

void nvn_pipe_record_graphics(const char *const keys[5], const nvn_pipe_key *k)
{
    if (P.gpl && !extra_stages(k))
        return;
    nvn_pipe_rec rec;
    memset(&rec, 0, sizeof rec);
    rec.kind = NVN_REC_GRAPHICS;
    for (uint32_t i = 0; i < 5; i++)
        if (keys[i])
            memcpy(rec.keys[i], keys[i], 65);
    rec.k = *k;
    memset(rec.k.mods, 0, sizeof rec.k.mods);
    record(&rec);
}

void nvn_pipe_record_compute(const char *key, uint32_t n, const VkSpecializationMapEntry *map, const uint32_t *vals)
{
    nvn_pipe_rec rec;
    memset(&rec, 0, sizeof rec);
    rec.kind = NVN_REC_COMPUTE;
    memcpy(rec.keys[5], key, 65);
    rec.k.nspec[0] = n;
    memcpy(rec.k.spec_map[0], map, n * sizeof map[0]);
    memcpy(rec.k.spec[0], vals, n * sizeof vals[0]);
    record(&rec);
}

uint32_t nvn_pipe_recorded(void) { return P.nold; }

const nvn_pipe_rec *nvn_pipe_recorded_at(uint32_t i) { return i < P.nold ? &P.old[i] : NULL; }

void nvn_pipe_warmed(void)
{
    P.warmed = true;
    nvn_pipe_save();
}

void nvn_pipe_set_warmed(bool warmed) { P.warmed = warmed; }

void nvn_pipe_stats(uint64_t *made, uint64_t *made_ns, uint64_t *late, uint64_t *late_ns)
{
    pthread_mutex_lock(&P.lock);
    *made = P.made;
    *made_ns = P.made_ns;
    *late = P.late;
    *late_ns = P.late_ns;
    pthread_mutex_unlock(&P.lock);
}

void nvn_pipe_held(uint32_t *libraries, uint32_t *graphics, uint32_t *compute)
{
    pthread_mutex_lock(&P.lock);
    *libraries = P.vi.n + P.pr.n + P.fs.n + P.fo.n;
    *graphics = P.full.n;
    *compute = P.comp.n + P.mcomp.n;
    pthread_mutex_unlock(&P.lock);
}

static void le32w(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/** @brief The cache file's path, in the folder of the app's per-user data when the app is named, else the shader folder. */
static bool cache_path(char *out, size_t n)
{
    const char *env = getenv("RELIFT_PIPELINE_CACHE");
    char dir[1024];
    if (env && *env) {
        snprintf(dir, sizeof dir, "%s", env);
    } else if (rl_settings_app() && rl_settings_user_dir(dir, sizeof dir, rl_settings_app())) {
        size_t l = strlen(dir);
        snprintf(dir + l, sizeof dir - l, "/pipeline-cache");
    } else {
        snprintf(dir, sizeof dir, "%s", nvn_shader_dir());
    }
    nvn_mkdirs(dir);
    return snprintf(out, n, "%s/pipeline-cache.bin", dir) < (int)n;
}

/** @brief Fills P.identity: what a cache file must have been made for to be read now. */
static bool make_identity(void)
{
    VkPhysicalDeviceVulkan11Properties p11 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_PROPERTIES };
    VkPhysicalDeviceVulkan12Properties p12 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES,
                                               .pNext = &p11 };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &p12 };
    VK(vkGetPhysicalDeviceProperties2)(g.phys, &p2);
    char hex[65];
    if (!nvn_shader_table_hash(hex)) {
        memset(hex, '0', 64);
        hex[64] = 0;
    }
    uint8_t *o = P.identity;
    memset(o, 0, sizeof P.identity);
    memcpy(o, p11.deviceUUID, 16);
    memcpy(o + 16, p11.driverUUID, 16);
    le32w(o + 32, p2.properties.driverVersion);
    le32w(o + 36, p2.properties.vendorID);
    le32w(o + 40, p2.properties.deviceID);
    le32w(o + 44, p2.properties.apiVersion);
    memcpy(o + 48, hex, 64);
    return true;
}

static void keys_load(void);

static void cache_load(void **data, size_t *len)
{
    *data = NULL;
    *len = 0;
    char path[1100];
    if (!cache_path(path, sizeof path))
        return;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    uint8_t head[CACHE_HEAD];
    bool ok = fread(head, 1, CACHE_HEAD, f) == CACHE_HEAD && !memcmp(head, CACHE_MAGIC, 8) &&
              !memcmp(head + 8 + 64, P.identity, sizeof P.identity);
    uint64_t n = 0;
    for (int i = 0; ok && i < 8; i++)
        n |= (uint64_t)head[8 + 64 + sizeof P.identity + i] << (8 * i);
    void *buf = ok && n && n < (8ull << 30) ? malloc((size_t)n) : NULL;
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        char hex[65];
        rl_data_sha256(buf, (size_t)n, hex);
        if (!memcmp(hex, head + 8, 64)) {
            *data = buf;
            *len = (size_t)n;
            buf = NULL;
        }
    }
    free(buf);
    fclose(f);
    if (!*data)
        rl_log(RL_LOG_INFO, "nvn", "discarded the pipeline cache %s: made for another device, driver or shader table", path);
}

uint64_t nvn_pipe_cache_bytes(void) { return P.loaded; }

void nvn_pipe_init(void)
{
    make_identity();
    if (P.cache)
        VK(vkDestroyPipelineCache)(g.dev, P.cache, NULL);
    void *data;
    size_t len;
    cache_load(&data, &len);
    VkPipelineCacheCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
                                     .initialDataSize = len,
                                     .pInitialData = data };
    if (VK(vkCreatePipelineCache)(g.dev, &ci, NULL, &P.cache) != VK_SUCCESS) {
        P.cache = NULL;
        ci.initialDataSize = 0;
        ci.pInitialData = NULL;
        if (VK(vkCreatePipelineCache)(g.dev, &ci, NULL, &P.cache) != VK_SUCCESS)
            P.cache = NULL;
    }
    free(data);
    P.loaded = len;
    keys_load();
    VkPhysicalDeviceVulkan13Features f13 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
    VkPhysicalDeviceFeatures2 f2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f13 };
    VK(vkGetPhysicalDeviceFeatures2)(g.phys, &f2);
    P.cache_ctl = f13.pipelineCreationCacheControl;
    const char *mode = getenv("RELIFT_PIPELINE_MODE");
    P.gpl = g.gpl && !(mode && !strcmp(mode, "monolithic"));
    rl_log(RL_LOG_INFO, "nvn", "pipelines: %s, cache %s (%.1f MiB read)", P.gpl ? "linked from libraries" : "monolithic",
           P.cache ? "on" : "off", len / 1048576.0);
}

#define KEYS_MAGIC "RLPLK001"
#define KEYS_HEAD (8u + CACHE_IDENTITY + 8u)

/** @brief The keys file's path, beside the cache file. */
static bool keys_path(char *out, size_t n)
{
    char cache[1100];
    if (!cache_path(cache, sizeof cache))
        return false;
    char *slash = strrchr(cache, '/');
    if (!slash)
        return false;
    *slash = 0;
    return snprintf(out, n, "%s/pipeline-keys.bin", cache) < (int)n;
}

/** @brief Reads the pipelines the earlier launches recorded into P.old, and into this launch's records, when the file was made for this device, driver, shader table and layout of a record. */
static void keys_load(void)
{
    free(P.old);
    P.old = NULL;
    P.nold = 0;
    char path[1100];
    if (!keys_path(path, sizeof path))
        return;
    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    uint8_t head[KEYS_HEAD];
    uint32_t count = 0, size = 0;
    bool ok = fread(head, 1, sizeof head, f) == sizeof head && !memcmp(head, KEYS_MAGIC, 8) &&
              !memcmp(head + 8, P.identity, sizeof P.identity);
    if (ok) {
        size = (uint32_t)head[8 + CACHE_IDENTITY] | (uint32_t)head[9 + CACHE_IDENTITY] << 8 |
               (uint32_t)head[10 + CACHE_IDENTITY] << 16 | (uint32_t)head[11 + CACHE_IDENTITY] << 24;
        count = (uint32_t)head[12 + CACHE_IDENTITY] | (uint32_t)head[13 + CACHE_IDENTITY] << 8 |
                (uint32_t)head[14 + CACHE_IDENTITY] << 16 | (uint32_t)head[15 + CACHE_IDENTITY] << 24;
        ok = size == sizeof(nvn_pipe_rec) && count < (1u << 20);
    }
    nvn_pipe_rec *r = ok && count ? malloc((size_t)count * sizeof *r) : NULL;
    if (r && fread(r, sizeof *r, count, f) == count) {
        P.old = r;
        P.nold = count;
        r = NULL;
    }
    free(r);
    fclose(f);
    for (uint32_t i = 0; i < P.nold; i++)
        record(&P.old[i]);
    P.dirty = false;
}

/** @brief Writes this launch's records (those read and those new) to the keys file. */
static void keys_save(void)
{
    pthread_mutex_lock(&P.lock);
    uint32_t count = P.nfresh;
    nvn_pipe_rec *copy = count ? malloc((size_t)count * sizeof *copy) : NULL;
    if (copy)
        memcpy(copy, P.fresh, (size_t)count * sizeof *copy);
    pthread_mutex_unlock(&P.lock);
    char path[1100], tmp[1120];
    if (!copy || !keys_path(path, sizeof path)) {
        free(copy);
        return;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    uint8_t head[KEYS_HEAD];
    memset(head, 0, sizeof head);
    memcpy(head, KEYS_MAGIC, 8);
    memcpy(head + 8, P.identity, sizeof P.identity);
    le32w(head + 8 + CACHE_IDENTITY, (uint32_t)sizeof(nvn_pipe_rec));
    le32w(head + 12 + CACHE_IDENTITY, count);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(head, 1, sizeof head, f) == sizeof head && fwrite(copy, sizeof *copy, count, f) == count;
    if (f && fclose(f) != 0)
        ok = false;
    free(copy);
    if (ok) {
#if defined(_WIN32)
        remove(path);
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok)
        remove(tmp);
}

static void save_locked(void);

void nvn_pipe_save(void)
{
    pthread_mutex_lock(&P.save_lock);
    save_locked();
    pthread_mutex_unlock(&P.save_lock);
}

static void save_locked(void)
{
    if (!P.cache)
        return;
    pthread_mutex_lock(&P.lock);
    bool dirty = P.dirty;
    P.dirty = false;
    P.late_at = 0;
    P.saved_ns = rl_clock_ns();
    pthread_mutex_unlock(&P.lock);
    if (!dirty && P.loaded)
        return;
    keys_save();
    size_t n = 0;
    if (VK(vkGetPipelineCacheData)(g.dev, P.cache, &n, NULL) != VK_SUCCESS || !n)
        return;
    uint8_t *buf = malloc(n);
    if (!buf)
        return;
    if (VK(vkGetPipelineCacheData)(g.dev, P.cache, &n, buf) != VK_SUCCESS) {
        free(buf);
        return;
    }
    char path[1100], tmp[1120];
    if (!cache_path(path, sizeof path)) {
        free(buf);
        return;
    }
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    uint8_t head[CACHE_HEAD];
    memset(head, 0, sizeof head);
    memcpy(head, CACHE_MAGIC, 8);
    rl_data_sha256(buf, n, (char *)head + 8);
    memcpy(head + 8 + 64, P.identity, sizeof P.identity);
    for (int i = 0; i < 8; i++)
        head[8 + 64 + sizeof P.identity + i] = (uint8_t)((uint64_t)n >> (8 * i));
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(head, 1, sizeof head, f) == sizeof head && fwrite(buf, 1, n, f) == n;
    if (f && fclose(f) != 0)
        ok = false;
    free(buf);
    if (ok) {
#if defined(_WIN32)
        remove(path);
#endif
        ok = rename(tmp, path) == 0;
    }
    if (!ok) {
        rl_log(RL_LOG_ERROR, "nvn", "cannot write the pipeline cache %s: %s", path, strerror(errno));
        remove(tmp);
    } else {
        P.loaded = n;
        rl_log(RL_LOG_INFO, "nvn", "wrote the pipeline cache: %.1f MiB", n / 1048576.0);
    }
}
