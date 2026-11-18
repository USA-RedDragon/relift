/**
 * @file nvn_tex.c
 * @brief NVN2 textures, samplers and their descriptor pools: storage layout
 * (block linear and linear), and the state relift keeps for each.
 *
 * Block-linear storage is laid out in GOBs of 64 bytes by 8 rows, stacked
 * into blocks of 1 to 16 GOBs high (and 1 to 16 deep for 3D textures); each
 * mip level picks the tallest block height that its height still fills to
 * more than half, never taller than level 0's.
 */
#include "nvn_priv.h"

#include <pthread.h>
#include <stdlib.h>

#define GOB_BYTES_W 64u
#define GOB_ROWS 8u
#define GOB_BYTES 512u

/** @brief Bytes per block and block width and height of each NVNformat, read from the SDK driver's format table. */
static const struct fmt_info {
    uint8_t bpb, bw, bh;
} formats[] = {
    { 0, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 1, 1, 1 }, { 2, 1, 1 },
    { 2, 1, 1 }, { 2, 1, 1 }, { 2, 1, 1 }, { 2, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 },
    { 4, 1, 1 }, { 2, 1, 1 }, { 2, 1, 1 }, { 2, 1, 1 }, { 2, 1, 1 }, { 4, 1, 1 },
    { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 8, 1, 1 }, { 8, 1, 1 },
    { 8, 1, 1 }, { 3, 1, 1 }, { 3, 1, 1 }, { 3, 1, 1 }, { 3, 1, 1 }, { 6, 1, 1 },
    { 6, 1, 1 }, { 6, 1, 1 }, { 6, 1, 1 }, { 6, 1, 1 }, { 12, 1, 1 }, { 12, 1, 1 },
    { 12, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 8, 1, 1 },
    { 8, 1, 1 }, { 8, 1, 1 }, { 8, 1, 1 }, { 8, 1, 1 }, { 16, 1, 1 }, { 16, 1, 1 },
    { 16, 1, 1 }, { 1, 1, 1 }, { 2, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 },
    { 8, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 2, 1, 1 }, { 2, 1, 1 }, { 2, 1, 1 },
    { 2, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 8, 4, 4 },
    { 8, 4, 4 }, { 16, 4, 4 }, { 16, 4, 4 }, { 8, 4, 4 }, { 8, 4, 4 }, { 16, 4, 4 },
    { 16, 4, 4 }, { 8, 4, 4 }, { 8, 4, 4 }, { 16, 4, 4 }, { 16, 4, 4 }, { 16, 4, 4 },
    { 16, 4, 4 }, { 16, 4, 4 }, { 16, 4, 4 }, { 1, 1, 1 }, { 1, 1, 1 }, { 2, 1, 1 },
    { 2, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 2, 1, 1 }, { 2, 1, 1 }, { 4, 1, 1 },
    { 4, 1, 1 }, { 8, 1, 1 }, { 8, 1, 1 }, { 3, 1, 1 }, { 3, 1, 1 }, { 6, 1, 1 },
    { 6, 1, 1 }, { 12, 1, 1 }, { 12, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 8, 1, 1 },
    { 8, 1, 1 }, { 16, 1, 1 }, { 16, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 },
    { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 8, 1, 1 },
    { 8, 1, 1 }, { 8, 1, 1 }, { 8, 1, 1 }, { 8, 1, 1 }, { 16, 1, 1 }, { 16, 1, 1 },
    { 16, 1, 1 }, { 16, 4, 4 }, { 16, 5, 4 }, { 16, 5, 5 }, { 16, 6, 5 }, { 16, 6, 6 },
    { 16, 8, 5 }, { 16, 8, 6 }, { 16, 8, 8 }, { 16, 10, 5 }, { 16, 10, 6 }, { 16, 10, 8 },
    { 16, 10, 10 }, { 16, 12, 10 }, { 16, 12, 12 }, { 16, 4, 4 }, { 16, 5, 4 }, { 16, 5, 5 },
    { 16, 6, 5 }, { 16, 6, 6 }, { 16, 8, 5 }, { 16, 8, 6 }, { 16, 8, 8 }, { 16, 10, 5 },
    { 16, 10, 6 }, { 16, 10, 8 }, { 16, 10, 10 }, { 16, 12, 10 }, { 16, 12, 12 }, { 2, 1, 1 },
    { 2, 1, 1 }, { 2, 1, 1 }, { 2, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 }, { 4, 1, 1 },
    { 4, 1, 1 }, { 8, 4, 4 }, { 16, 4, 4 }, { 8, 4, 4 }, { 16, 4, 4 }, { 8, 1, 1 },
    { 8, 1, 1 }, { 0, 0, 0 },
};

/** @brief NVNtextureTarget values. */
enum {
    TARGET_1D = 0,
    TARGET_2D = 1,
    TARGET_3D = 2,
    TARGET_1D_ARRAY = 3,
    TARGET_2D_ARRAY = 4,
    TARGET_2D_MULTISAMPLE = 5,
    TARGET_2D_MULTISAMPLE_ARRAY = 6,
    TARGET_RECTANGLE = 7,
    TARGET_CUBEMAP = 8,
    TARGET_CUBEMAP_ARRAY = 9,
    TARGET_BUFFER = 10
};

/** @brief NVNtextureFlags bits relift reads. */
enum { FLAG_LINEAR = 1u << 4, FLAG_LINEAR_RENDER_TARGET = 1u << 6 };

typedef struct nvn_texture {
    nvn_obj o;
    nvn_tex_desc d;
} nvn_texture;

typedef struct nvn_desc_pool {
    nvn_obj o;
    uint64_t pool, offset;
    int32_t count;
} nvn_desc_pool;

typedef struct nvn_sampler {
    nvn_obj o;
    uint64_t builder_copy[16];
} nvn_sampler;

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }
static uint64_t align_up(uint64_t a, uint64_t b) { return (a + b - 1) / b * b; }

/** @brief The tallest block (in GOBs, a power of two up to 16) that n GOBs fill to more than half, at most limit. */
static uint32_t block_gobs(uint32_t n, uint32_t limit)
{
    uint32_t b = limit;
    while (b > 1 && n <= b / 2)
        b /= 2;
    return b;
}

/**
 * @brief The driver's block size (in GOBs) for a level 0 of n rows of
 * blocks (or n slices): the tallest of 1 to 16 whose step t (16 rows, or 2
 * slices, for a block of one GOB) the count n + n / 2 reaches, as the
 * driver's layout code chooses it.
 */
static uint32_t driver_block(uint32_t n, uint32_t t)
{
    uint32_t x = n + n / 2, b = 1;
    while (b < 16 && x >= t) {
        b *= 2;
        t *= 2;
    }
    return b;
}

bool nvn_tex_format(uint32_t format, uint32_t *bpb, uint32_t *bw, uint32_t *bh)
{
    if (format >= sizeof formats / sizeof formats[0] || !formats[format].bpb)
        return false;
    *bpb = formats[format].bpb;
    *bw = formats[format].bw;
    *bh = formats[format].bh;
    return true;
}

/** @brief Width and height multipliers of a sample count in a multisample texture's storage. */
static void sample_grid(uint32_t samples, uint32_t *sx, uint32_t *sy)
{
    *sx = samples >= 2 ? 2 : 1;
    *sy = samples >= 4 ? 2 : 1;
    if (samples >= 8)
        *sx = 4;
    if (samples >= 16)
        *sy = 4;
}

bool nvn_tex_layout(nvn_tex_desc *d)
{
    uint32_t bpb, bw, bh;
    if (!nvn_tex_format(d->format, &bpb, &bw, &bh))
        return false;
    uint32_t sx, sy;
    sample_grid(d->samples, &sx, &sy);
    uint32_t w = d->width * sx, h = d->height * sy;
    uint32_t depth = d->target == TARGET_3D ? d->depth : 1;
    uint32_t layers = d->target == TARGET_3D ? 1 : d->depth ? d->depth : 1;
    if (d->target == TARGET_CUBEMAP)
        layers = 6;
    uint32_t levels = d->levels ? d->levels : 1;
    d->align = GOB_BYTES;
    if (d->target == TARGET_BUFFER) {
        d->align = bpb == 12 ? 4 : bpb;
        d->size = (uint64_t)w * bpb;
        return true;
    }
    if (d->flags & (FLAG_LINEAR | FLAG_LINEAR_RENDER_TARGET)) {
        d->align = d->flags & FLAG_LINEAR_RENDER_TARGET ? 128 : 32;
        uint64_t stride = d->stride ? d->stride : align_up((uint64_t)div_up(w, bw) * bpb, d->align);
        d->size = stride * div_up(h, bh) * depth * layers;
        d->block_h = d->block_d = 0;
        return true;
    }
    uint32_t hb0 = div_up(h, bh);
    d->block_h = d->target == TARGET_3D ? 1 : driver_block(hb0, 16);
    d->block_d = d->target == TARGET_3D ? driver_block(depth, 2) : 1;
    uint64_t layer = 0;
    for (uint32_t l = 0; l < levels; l++) {
        uint32_t wl = w >> l ? w >> l : 1, hl = h >> l ? h >> l : 1, dl = depth >> l ? depth >> l : 1;
        uint32_t wb = div_up(wl, bw), hb = div_up(hl, bh);
        uint32_t gh = block_gobs(div_up(hb, GOB_ROWS), d->block_h);
        uint32_t gd = d->target == TARGET_3D ? block_gobs(dl, d->block_d) : 1;
        uint64_t row = align_up((uint64_t)wb * bpb, GOB_BYTES_W);
        uint64_t size = row * align_up(hb, (uint64_t)GOB_ROWS * gh) * align_up(dl, gd);
        if (l < NVN_TEX_MAX_LEVELS)
            d->level_offset[l] = layer;
        layer += size;
    }
    if (layers > 1)
        layer = align_up(layer, (uint64_t)GOB_BYTES * d->block_h * d->block_d);
    d->layer_stride = layer;
    d->size = layer * layers;
    return true;
}

/** @brief Reads a texture builder into d through the driver's getters. */
static int read_builder(rl_ctx *c, uint8_t *mem, uint64_t builder, nvn_tex_desc *d)
{
    static const char *const getters[] = {
        "nvnTextureBuilderGetTarget", "nvnTextureBuilderGetFormat",  "nvnTextureBuilderGetWidth",
        "nvnTextureBuilderGetHeight", "nvnTextureBuilderGetDepth",   "nvnTextureBuilderGetLevels",
        "nvnTextureBuilderGetSamples", "nvnTextureBuilderGetFlags",  "nvnTextureBuilderGetStride",
        "nvnTextureBuilderGetMemoryPool", "nvnTextureBuilderGetMemoryOffset",
    };
    uint64_t v[sizeof getters / sizeof getters[0]];
    for (unsigned i = 0; i < sizeof getters / sizeof getters[0]; i++) {
        int rc = nvn_driver_call(c, mem, getters[i], &builder, 1, &v[i]);
        if (rc != RL_OK)
            return rc;
    }
    uint64_t sp = c->sp, out = (sp - 32) & ~15ull;
    uint64_t sw[5] = { builder, out, out + 4, out + 8, out + 12 };
    c->sp = out;
    int src = nvn_driver_call(c, mem, "nvnTextureBuilderGetSwizzle", sw, 5, NULL);
    c->sp = sp;
    if (src != RL_OK)
        return src;
    uint32_t swz[4];
    memcpy(swz, mem + out, sizeof swz);
    memset(d, 0, sizeof *d);
    d->target = (uint32_t)v[0];
    d->format = (uint32_t)v[1];
    d->width = (uint32_t)v[2];
    d->height = (uint32_t)v[3];
    d->depth = (uint32_t)v[4];
    d->levels = (uint32_t)v[5];
    d->samples = (uint32_t)v[6];
    d->flags = (uint32_t)v[7];
    d->stride = v[8];
    d->pool = v[9];
    d->offset = v[10];
    memcpy(d->swizzle, swz, sizeof swz);
    if (!nvn_tex_layout(d))
        return rl_guest_fatal(c, "nvn", "texture format %u is not in relift's format table", d->format);
    return RL_OK;
}

/** @brief nvnTextureBuilderGetStorageSize(const NVNtextureBuilder *): size_t. */
uint64_t nvnh_nvnTextureBuilderGetStorageSize(rl_ctx *c, uint8_t *mem)
{
    nvn_tex_desc d;
    int rc = read_builder(c, mem, c->x[0], &d);
    if (rc != RL_OK)
        return (uint64_t)rc;
    c->x[0] = d.size;
    return nvn_ret(c);
}

/** @brief nvnTextureBuilderGetStorageAlignment(const NVNtextureBuilder *): size_t. */
uint64_t nvnh_nvnTextureBuilderGetStorageAlignment(rl_ctx *c, uint8_t *mem)
{
    nvn_tex_desc d;
    int rc = read_builder(c, mem, c->x[0], &d);
    if (rc != RL_OK)
        return (uint64_t)rc;
    c->x[0] = d.align;
    return nvn_ret(c);
}

/** @brief nvnTextureInitialize(NVNtexture *, const NVNtextureBuilder *): NVNboolean. */
uint64_t nvnh_nvnTextureInitialize(rl_ctx *c, uint8_t *mem)
{
    uint64_t tex = c->x[0];
    nvn_tex_desc d;
    int rc = read_builder(c, mem, c->x[1], &d);
    if (rc != RL_OK)
        return (uint64_t)rc;
    nvn_texture *t = (nvn_texture *)nvn_obj_new(tex, NVN_TEXTURE, sizeof *t);
    if (t)
        t->d = d;
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "nvnTextureInitialize(0x%llx): target %u format %u %ux%ux%u levels %u flags 0x%x, size 0x%llx",
               (unsigned long long)tex, d.target, d.format, d.width, d.height, d.depth, d.levels, d.flags,
               (unsigned long long)d.size);
    c->x[0] = t != NULL;
    return nvn_ret(c);
}

/** @brief nvnTextureFinalize(NVNtexture *). */
uint64_t nvnh_nvnTextureFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_TEXTURE));
    return nvn_ret(c);
}

int nvn_view_read(rl_ctx *c, uint8_t *mem, const uint8_t view[NVN_VIEW_BYTES], nvn_view *out)
{
    memset(out, 0, sizeof *out);
    uint64_t sp = c->sp, base = (sp - 64) & ~15ull;
    uint64_t v = base, o = base + NVN_VIEW_BYTES;
    memcpy(mem + v, view, NVN_VIEW_BYTES);
    c->sp = base;
    static const char *const getters[] = { "nvnTextureViewGetLevels",  "nvnTextureViewGetLayers",
                                           "nvnTextureViewGetFormat",  "nvnTextureViewGetSwizzle",
                                           "nvnTextureViewGetDepthStencilMode", "nvnTextureViewGetTarget" };
    bool *has[] = { &out->has_levels, &out->has_layers, &out->has_format,
                    &out->has_swizzle, &out->has_ds_mode, &out->has_target };
    int rc = RL_OK;
    for (unsigned i = 0; i < 6 && rc == RL_OK; i++) {
        memset(mem + o, 0, 16);
        uint64_t args[5] = { v, o, o + 4, o + 8, o + 12 }, ret = 0;
        rc = nvn_driver_call(c, mem, getters[i], args, 5, &ret);
        if (rc != RL_OK)
            break;
        *has[i] = (ret & 0xff) != 0;
        uint32_t w[4];
        memcpy(w, mem + o, sizeof w);
        switch (i) {
        case 0: out->base_level = (int32_t)w[0]; out->num_levels = (int32_t)w[1]; break;
        case 1: out->min_layer = (int32_t)w[0]; out->num_layers = (int32_t)w[1]; break;
        case 2: out->format = w[0]; break;
        case 3: memcpy(out->swizzle, w, sizeof w); break;
        case 4: out->ds_mode = w[0]; break;
        default: out->target = w[0]; break;
        }
    }
    c->sp = sp;
    return rc;
}

const nvn_tex_desc *nvn_texture_desc(uint64_t tex)
{
    nvn_texture *t = (nvn_texture *)nvn_obj_get(tex, NVN_TEXTURE);
    return t ? &t->d : NULL;
}

static uint64_t tex_get(rl_ctx *c, int field)
{
    nvn_texture *t = (nvn_texture *)nvn_obj_get(c->x[0], NVN_TEXTURE);
    if (!t)
        return (uint64_t)rl_guest_fatal(c, "nvn", "texture query on 0x%llx, not a texture", (unsigned long long)c->x[0]);
    const nvn_tex_desc *d = &t->d;
    const uint64_t v[] = { d->width, d->height, d->depth, d->format, d->target, d->flags, d->size, d->levels,
                           d->pool, d->offset, d->samples, d->stride };
    c->x[0] = v[field];
    return nvn_ret(c);
}

uint64_t nvnh_nvnTextureGetWidth(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 0); }
uint64_t nvnh_nvnTextureGetHeight(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 1); }
uint64_t nvnh_nvnTextureGetDepth(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 2); }
uint64_t nvnh_nvnTextureGetFormat(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 3); }
uint64_t nvnh_nvnTextureGetTarget(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 4); }
uint64_t nvnh_nvnTextureGetFlags(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 5); }
uint64_t nvnh_nvnTextureGetStorageSize(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 6); }
uint64_t nvnh_nvnTextureGetLevels(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 7); }
uint64_t nvnh_nvnTextureGetMemoryPool(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 8); }
uint64_t nvnh_nvnTextureGetMemoryOffset(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 9); }
uint64_t nvnh_nvnTextureGetSamples(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 10); }
uint64_t nvnh_nvnTextureGetStride(rl_ctx *c, uint8_t *mem) { (void)mem; return tex_get(c, 11); }

/**
 * @brief nvnSamplerInitialize(NVNsampler *, const NVNsamplerBuilder *):
 * NVNboolean. The driver's own initialization fills the sampler object (its
 * getters read it back), and the layer keeps the builder's bytes.
 */
uint64_t nvnh_nvnSamplerInitialize(rl_ctx *c, uint8_t *mem)
{
    uint64_t args[2] = { c->x[0], c->x[1] }, init = 0, ok = 0;
    int rc = nvn_driver_lookup(c, mem, "nvnSamplerInitialize", &init);
    if (rc == RL_OK && !init)
        rc = rl_guest_fatal(c, "nvn", "the driver has no nvnSamplerInitialize");
    if (rc == RL_OK)
        rc = nvn_guest_call(c, mem, init, args, 2, &ok);
    if (rc != RL_OK)
        return (uint64_t)rc;
    c->x[0] = args[0];
    c->x[1] = args[1];
    nvn_sampler *s = (nvn_sampler *)nvn_obj_new(c->x[0], NVN_SAMPLER, sizeof *s);
    if (s)
        memcpy(s->builder_copy, mem + c->x[1], sizeof s->builder_copy);
    c->x[0] = s != NULL;
    return nvn_ret(c);
}

/** @brief nvnSamplerFinalize(NVNsampler *). */
uint64_t nvnh_nvnSamplerFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_SAMPLER));
    return nvn_ret(c);
}

static uint64_t pool_init(rl_ctx *c, uint32_t kind)
{
    nvn_desc_pool *p = (nvn_desc_pool *)nvn_obj_new(c->x[0], kind, sizeof *p);
    if (p) {
        p->pool = c->x[1];
        p->offset = c->x[2];
        p->count = (int32_t)c->x[3];
        if (rl_cfg()->trace_os)
            rl_log(RL_LOG_INFO, "nvn", "%s pool 0x%llx: %d descriptors", kind == NVN_TEXTURE_POOL ? "texture" : "sampler",
                   (unsigned long long)c->x[0], p->count);
    }
    c->x[0] = p != NULL;
    return nvn_ret(c);
}

/** @brief nvnTexturePoolInitialize(NVNtexturePool *, const NVNmemoryPool *, ptrdiff_t offset, int count): NVNboolean. */
uint64_t nvnh_nvnTexturePoolInitialize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return pool_init(c, NVN_TEXTURE_POOL);
}

/** @brief nvnSamplerPoolInitialize(NVNsamplerPool *, const NVNmemoryPool *, ptrdiff_t offset, int count): NVNboolean. */
uint64_t nvnh_nvnSamplerPoolInitialize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return pool_init(c, NVN_SAMPLER_POOL);
}

/** @brief nvnTexturePoolFinalize(NVNtexturePool *). */
uint64_t nvnh_nvnTexturePoolFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_TEXTURE_POOL));
    return nvn_ret(c);
}

/** @brief nvnSamplerPoolFinalize(NVNsamplerPool *). */
uint64_t nvnh_nvnSamplerPoolFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_obj_free(nvn_obj_get(c->x[0], NVN_SAMPLER_POOL));
    return nvn_ret(c);
}

/**
 * @brief The descriptor count a pool was initialized with, or 0 for a pool
 * relift does not know. The driver keeps that count at +16 of the pool and
 * returns it (verified: SDK 22.2.0 nvnTexturePoolGetSize and
 * nvnSamplerPoolGetSize are both ldr w0, [x0, #16]; ret).
 */
static uint64_t pool_size(rl_ctx *c, uint32_t kind)
{
    nvn_desc_pool *p = (nvn_desc_pool *)nvn_obj_get(c->x[0], kind);
    c->x[0] = p ? (uint32_t)p->count : 0;
    return nvn_ret(c);
}

/** @brief nvnTexturePoolGetSize(const NVNtexturePool *): int, the descriptor count. */
uint64_t nvnh_nvnTexturePoolGetSize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return pool_size(c, NVN_TEXTURE_POOL);
}

/** @brief nvnSamplerPoolGetSize(const NVNsamplerPool *): int, the descriptor count. */
uint64_t nvnh_nvnSamplerPoolGetSize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return pool_size(c, NVN_SAMPLER_POOL);
}

/** @brief Registrations by descriptor id (relift keeps one table for all texture pools and one for all sampler pools). */
static struct {
    uint64_t *tex, *sampler;
    uint8_t (*view)[17];
    bool *image;
    uint32_t ntex, nsampler;
} reg;

static void *grow(void *a, size_t elem, uint32_t old, uint32_t cap)
{
    uint8_t *p = realloc(a, cap * elem);
    if (p)
        memset(p + (size_t)old * elem, 0, (size_t)(cap - old) * elem);
    return p;
}

static uint32_t cap_for(uint32_t have, uint32_t want)
{
    uint32_t cap = have ? have : 1024;
    while (cap < want)
        cap *= 2;
    return cap;
}

static bool tex_room(uint32_t id)
{
    if (id < reg.ntex)
        return true;
    uint32_t cap = cap_for(reg.ntex, id + 1);
    uint64_t *t = grow(reg.tex, sizeof *t, reg.ntex, cap);
    if (t)
        reg.tex = t;
    uint8_t (*v)[17] = grow(reg.view, sizeof *v, reg.ntex, cap);
    if (v)
        reg.view = v;
    bool *im = grow(reg.image, sizeof *im, reg.ntex, cap);
    if (im)
        reg.image = im;
    if (!t || !v || !im)
        return false;
    reg.ntex = cap;
    return true;
}

static pthread_mutex_t reg_lock = PTHREAD_MUTEX_INITIALIZER;

static uint64_t register_texture(rl_ctx *c, uint8_t *mem, bool image)
{
    uint32_t id = (uint32_t)c->x[1];
    pthread_mutex_lock(&reg_lock);
    if (!tex_room(id)) {
        pthread_mutex_unlock(&reg_lock);
        return (uint64_t)rl_guest_fatal(c, "nvn", "out of memory registering texture %u", id);
    }
    reg.tex[id] = c->x[2];
    reg.view[id][16] = c->x[3] != 0;
    if (c->x[3])
        memcpy(reg.view[id], mem + c->x[3], 16);
    reg.image[id] = image;
    pthread_mutex_unlock(&reg_lock);
    return nvn_ret(c);
}

uint64_t nvn_tex_registered(uint32_t id, uint8_t view[16], bool *has_view, bool *image)
{
    pthread_mutex_lock(&reg_lock);
    uint64_t t = id < reg.ntex ? reg.tex[id] : 0;
    if (t) {
        memcpy(view, reg.view[id], 16);
        *has_view = reg.view[id][16] != 0;
        *image = reg.image[id];
    }
    pthread_mutex_unlock(&reg_lock);
    return t;
}

const uint8_t *nvn_sampler_registered(uint32_t id)
{
    pthread_mutex_lock(&reg_lock);
    uint64_t s = id < reg.nsampler ? reg.sampler[id] : 0;
    pthread_mutex_unlock(&reg_lock);
    nvn_sampler *o = s ? (nvn_sampler *)nvn_obj_get(s, NVN_SAMPLER) : NULL;
    return o ? (const uint8_t *)o->builder_copy : NULL;
}

/** @brief nvnTexturePoolRegisterTexture(const NVNtexturePool *, int id, const NVNtexture *, const NVNtextureView *). */
uint64_t nvnh_nvnTexturePoolRegisterTexture(rl_ctx *c, uint8_t *mem)
{
    return register_texture(c, mem, false);
}

/** @brief nvnTexturePoolRegisterImage(const NVNtexturePool *, int id, const NVNtexture *, const NVNtextureView *). */
uint64_t nvnh_nvnTexturePoolRegisterImage(rl_ctx *c, uint8_t *mem)
{
    return register_texture(c, mem, true);
}

/** @brief nvnSamplerPoolRegisterSampler(const NVNsamplerPool *, int id, const NVNsampler *). */
uint64_t nvnh_nvnSamplerPoolRegisterSampler(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    uint32_t id = (uint32_t)c->x[1];
    pthread_mutex_lock(&reg_lock);
    if (id >= reg.nsampler) {
        uint32_t cap = cap_for(reg.nsampler, id + 1);
        uint64_t *sm = grow(reg.sampler, sizeof *sm, reg.nsampler, cap);
        if (!sm) {
            pthread_mutex_unlock(&reg_lock);
            return (uint64_t)rl_guest_fatal(c, "nvn", "out of memory registering sampler %u", id);
        }
        reg.sampler = sm;
        reg.nsampler = cap;
    }
    reg.sampler[id] = c->x[2];
    pthread_mutex_unlock(&reg_lock);
    return nvn_ret(c);
}

/** @brief The first level and layer a texture view selects (0 and 0 for no view), read through the driver's getters. */
static int view_base(rl_ctx *c, uint8_t *mem, uint64_t view, uint32_t *level, uint32_t *layer)
{
    *level = *layer = 0;
    if (!view)
        return RL_OK;
    uint64_t sp = c->sp;
    c->sp = (sp - 32) & ~15ull;
    uint64_t out = c->sp, ok = 0;
    uint64_t a[3] = { view, out, out + 4 };
    int rc = nvn_driver_call(c, mem, "nvnTextureViewGetLevels", a, 3, &ok);
    if (rc == RL_OK && (ok & 0xff))
        *level = rl_ld32(mem, out);
    uint64_t b[3] = { view, out + 8, out + 12 };
    if (rc == RL_OK)
        rc = nvn_driver_call(c, mem, "nvnTextureViewGetLayers", b, 3, &ok);
    if (rc == RL_OK && (ok & 0xff))
        *layer = rl_ld32(mem, out + 8);
    c->sp = sp;
    return rc;
}

static uint64_t texels(rl_ctx *c, uint8_t *mem, bool write, const char *proc)
{
    const nvn_tex_desc *d = nvn_texture_desc(c->x[0]);
    if (!d)
        return (uint64_t)rl_guest_fatal(c, "nvn", "%s on 0x%llx, not a texture", proc, (unsigned long long)c->x[0]);
    uint64_t view = c->x[1], region = c->x[2], data = c->x[3];
    uint32_t level, layer;
    int rc = view_base(c, mem, view, &level, &layer);
    if (rc != RL_OK)
        return (uint64_t)rc;
    nvn_region r;
    memcpy(&r, mem + region, sizeof r);
    uint64_t storage = nvn_pool_memory(d->pool, NULL) + d->offset;
    if (!storage || !nvn_tex_copy(d, mem + storage, level, layer, &r, mem + data, write))
        return (uint64_t)rl_guest_fatal(c, "nvn", "%s: region %d,%d,%d %dx%dx%d of level %u layer %u is outside texture 0x%llx",
                                        proc, r.x, r.y, r.z, r.width, r.height, r.depth, level, layer,
                                        (unsigned long long)c->x[0]);
    return nvn_ret(c);
}

/** @brief nvnTextureWriteTexels(const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *, const void *data). */
uint64_t nvnh_nvnTextureWriteTexels(rl_ctx *c, uint8_t *mem) { return texels(c, mem, true, "nvnTextureWriteTexels"); }

/** @brief nvnTextureReadTexels(const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *, void *data). */
uint64_t nvnh_nvnTextureReadTexels(rl_ctx *c, uint8_t *mem) { return texels(c, mem, false, "nvnTextureReadTexels"); }

/** @brief nvnTextureFlushTexels(const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *): the storage is guest memory. */
uint64_t nvnh_nvnTextureFlushTexels(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return nvn_ret(c);
}

/** @brief nvnTextureInvalidateTexels(const NVNtexture *, const NVNtextureView *, const NVNcopyRegion *). */
uint64_t nvnh_nvnTextureInvalidateTexels(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return nvn_ret(c);
}
