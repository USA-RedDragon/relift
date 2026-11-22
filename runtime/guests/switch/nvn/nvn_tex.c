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
    /** @brief The builder's bytes on NVN1 (nvn_texture_record), which the driver's storage class function reads at a call. */
    uint8_t builder[NVN_TEXTURE_BUILDER_BYTES];
    bool has_builder;
    /** @brief The game finalized the texture; its description stays (see nvnh_nvnTextureFinalize). */
    bool finalized;
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

bool nvn_copy_pitch_of(uint32_t format, const nvn_region *rg, uint32_t row_stride, uint32_t image_stride,
                       nvn_copy_pitch *out)
{
    uint32_t bpb, bw, bh;
    if (!nvn_tex_format(format, &bpb, &bw, &bh) || rg->width <= 0 || rg->height <= 0 || rg->depth <= 0)
        return false;
    uint64_t blocks_w = ((uint64_t)rg->width + bw - 1) / bw, blocks_h = ((uint64_t)rg->height + bh - 1) / bh;
    uint64_t row = row_stride ? row_stride : blocks_w * bpb;
    uint64_t image = image_stride ? image_stride : row * blocks_h;
    if (row % bpb || row < blocks_w * bpb || image % row || image < row * blocks_h)
        return false;
    *out = (nvn_copy_pitch){ row,
                             image,
                             (uint32_t)(row / bpb * bw),
                             (uint32_t)(image / row * bh),
                             image * ((uint64_t)rg->depth - 1) + row * (blocks_h - 1) + blocks_w * bpb };
    return true;
}

bool nvn_tex_format(uint32_t format, uint32_t *bpb, uint32_t *bw, uint32_t *bh)
{
    if (format >= sizeof formats / sizeof formats[0] || format >= nvn_format_count() || !formats[format].bpb)
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

bool nvn_tex_layout(nvn_tex_desc *d) { return nvn_tex_layout_block(d, 0); }

bool nvn_tex_layout_block(nvn_tex_desc *d, uint32_t block_h)
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
    d->block_h = d->target == TARGET_3D ? 1 : block_h ? block_h : driver_block(hb0, 16);
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
    uint64_t ds = 0;
    src = nvn_driver_call(c, mem, "nvnTextureBuilderGetDepthStencilMode", &builder, 1, &ds);
    if (src != RL_OK)
        return src;
    memset(d, 0, sizeof *d);
    d->ds_mode = (uint32_t)ds;
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

/**
 * @brief The texture the builder at builder describes, with the storage size
 * and alignment the driver's own nvnTextureBuilderGetStorageSize and
 * nvnTextureBuilderGetStorageAlignment answer (on the device fields
 * nvn_builder_device_fields writes): the driver's nvnTextureInitialize
 * refuses storage placed by anything else, such as a compressible texture
 * off its pool's page. The driver's storage must hold the layer's own layout
 * of the texture (its size at least, its alignment a multiple), or the game
 * stops. d keeps the layer's own layout, which is what the host reads. NVN1
 * textures keep the layer's own size and alignment. The driver sometimes
 * takes a shorter block than the usual choice (a 2D array of 64x56 ASTC 5x5
 * texels, 12 block rows, gets blocks one GOB tall, which changes its layer
 * stride); when the usual choice does not give the driver's size and a
 * shorter block gives exactly that size, d takes the shorter block.
 */
static int read_storage(rl_ctx *c, uint8_t *mem, uint64_t builder, nvn_tex_desc *d, uint64_t *size_out,
                        uint64_t *align_out)
{
    static const char *const procs[2] = { "nvnTextureBuilderGetStorageSize", "nvnTextureBuilderGetStorageAlignment" };
    uint64_t got[2] = { 0, 0 };
    int rc = read_builder(c, mem, builder, d);
    if (rc != RL_OK || nvn_generation() == 1) {
        if (size_out)
            *size_out = d->size;
        if (align_out)
            *align_out = d->align;
        return rc;
    }
    rc = nvn_builder_device_fields(c, mem, builder);
    for (int i = 0; i < 2 && rc == RL_OK; i++) {
        uint64_t a = 0;
        rc = nvn_driver_lookup(c, mem, procs[i], &a);
        if (rc == RL_OK && !a)
            rc = rl_guest_fatal(c, "nvn", "the NVN2 driver has no %s", procs[i]);
        if (rc == RL_OK)
            rc = nvn_guest_call(c, mem, a, &builder, 1, &got[i]);
    }
    if (rc != RL_OK)
        return rc;
    uint64_t size = got[0], align = got[1];
    for (uint32_t b = d->block_h / 2; d->size != size && b; b /= 2) {
        nvn_tex_desc t = *d;
        if (nvn_tex_layout_block(&t, b) && t.size == size)
            *d = t;
    }
    if (size < d->size || !align || align % d->align)
        return rl_guest_fatal(c, "nvn",
                              "texture builder 0x%llx (target %u, format %u, %ux%ux%u, %u levels, flags 0x%x): the driver's "
                              "storage (0x%llx bytes, aligned to 0x%llx) does not hold relift's layout (0x%llx bytes, "
                              "aligned to 0x%llx)",
                              (unsigned long long)builder, d->target, d->format, d->width, d->height, d->depth, d->levels,
                              d->flags,
                              (unsigned long long)size, (unsigned long long)align, (unsigned long long)d->size,
                              (unsigned long long)d->align);
    if (size_out)
        *size_out = size;
    if (align_out)
        *align_out = align;
    if (rl_cfg()->trace_os && (size != d->size || align != d->align))
        rl_log(RL_LOG_INFO, "nvn", "texture builder 0x%llx storage: driver 0x%llx bytes aligned 0x%llx, relift 0x%llx/0x%llx",
               (unsigned long long)builder, (unsigned long long)size, (unsigned long long)align,
               (unsigned long long)d->size, (unsigned long long)d->align);
    return RL_OK;
}

/** @brief nvnTextureBuilderGetStorageSize(const NVNtextureBuilder *): size_t, the driver's (read_storage). */
uint64_t nvnh_nvnTextureBuilderGetStorageSize(rl_ctx *c, uint8_t *mem)
{
    nvn_tex_desc d;
    uint64_t size = 0;
    int rc = read_storage(c, mem, c->x[0], &d, &size, NULL);
    if (rc != RL_OK)
        return (uint64_t)rc;
    c->x[0] = size;
    return nvn_ret(c);
}

/** @brief nvnTextureBuilderGetStorageAlignment(const NVNtextureBuilder *): size_t, the driver's (read_storage). */
uint64_t nvnh_nvnTextureBuilderGetStorageAlignment(rl_ctx *c, uint8_t *mem)
{
    nvn_tex_desc d;
    uint64_t align = 0;
    int rc = read_storage(c, mem, c->x[0], &d, NULL, &align);
    if (rc != RL_OK)
        return (uint64_t)rc;
    c->x[0] = align;
    return nvn_ret(c);
}

/**
 * @brief nvnTextureBuilderGetZCullStorageSize(const NVNtextureBuilder *): size_t, the driver's own function on the
 * device fields it reads. The host has no ZCull storage and keeps no layout of it, so a game that places the
 * storage gets the size the console's driver names.
 */
uint64_t nvnh_nvnTextureBuilderGetZCullStorageSize(rl_ctx *c, uint8_t *mem)
{
    uint64_t builder = c->x[0], fn = 0, size = 0;
    int rc = nvn_builder_device_fields(c, mem, builder);
    if (rc == RL_OK)
        rc = nvn_driver_lookup(c, mem, "nvnTextureBuilderGetZCullStorageSize", &fn);
    if (rc == RL_OK && !fn)
        rc = rl_guest_fatal(c, "nvn", "the NVN2 driver has no nvnTextureBuilderGetZCullStorageSize");
    if (rc == RL_OK)
        rc = nvn_guest_call(c, mem, fn, &builder, 1, &size);
    if (rc != RL_OK)
        return (uint64_t)rc;
    c->x[0] = size;
    return nvn_ret(c);
}

/**
 * @brief Runs the NVN2 driver's own nvnTextureInitialize on the texture at
 * tex: it fills the texture object (its getters that stay driver code, such
 * as nvnTextureGetSparseTileLayout, read it back) and decides the result, on
 * the device fields nvn_builder_device_fields writes; *ok tells whether the
 * driver took the builder. NVN1 textures keep the layer's own
 * initialization, and their driver getters that read the texture do not stay
 * driver code (nvn_driver_reads_texture).
 */
static int driver_texture_init(rl_ctx *c, uint8_t *mem, uint64_t tex, uint64_t builder, bool *ok)
{
    uint64_t args[2] = { tex, builder }, init = 0, ret = 0;
    int rc = nvn_driver_lookup(c, mem, "nvnTextureInitialize", &init);
    if (rc == RL_OK && !init)
        rc = rl_guest_fatal(c, "nvn", "the NVN2 driver has no nvnTextureInitialize");
    if (rc == RL_OK)
        rc = nvn_builder_device_fields(c, mem, builder);
    if (rc == RL_OK)
        rc = nvn_guest_call(c, mem, init, args, 2, &ret);
    *ok = ret & 0xff;
    if (rc == RL_OK && !*ok && rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "nvnTextureInitialize(0x%llx): the driver refused the builder", (unsigned long long)tex);
    return rc;
}

/**
 * @brief nvnTextureInitialize(NVNtexture *, const NVNtextureBuilder *):
 * NVNboolean. On NVN2 the driver's own initialization runs first and decides
 * the result (driver_texture_init); the layer then records the texture.
 */
uint64_t nvnh_nvnTextureInitialize(rl_ctx *c, uint8_t *mem)
{
    uint64_t tex = c->x[0], builder = c->x[1];
    bool ok = true;
    int rc = nvn_generation() == 1 ? RL_OK : driver_texture_init(c, mem, tex, builder, &ok);
    if (rc != RL_OK)
        return (uint64_t)rc;
    if (!ok) {
        c->x[0] = 0;
        return nvn_ret(c);
    }
    nvn_tex_desc d;
    rc = read_storage(c, mem, builder, &d, NULL, NULL);
    if (rc != RL_OK)
        return (uint64_t)rc;
    bool recorded = nvn_texture_record(tex, &d, nvn_generation() == 1 ? mem + builder : NULL);
    if (rl_cfg()->trace_os)
        rl_log(RL_LOG_INFO, "nvn", "nvnTextureInitialize(0x%llx): target %u format %u %ux%ux%u levels %u flags 0x%x, size 0x%llx",
               (unsigned long long)tex, d.target, d.format, d.width, d.height, d.depth, d.levels, d.flags,
               (unsigned long long)d.size);
    c->x[0] = recorded;
    return nvn_ret(c);
}

/**
 * @brief nvnTextureFinalize(NVNtexture *). The driver's finalize only retires the object; the texture's layout,
 * address and storage stay what the game's own commands and descriptors named, and a command buffer recorded
 * before the finalize (replayed by a CallCommands, or submitted after it) still reads or writes that storage. So
 * the layer keeps the texture's description until the same address is initialized again (nvn_obj_publish replaces it),
 * and only the texture set's slots of it go (nvn_texset_texture_gone). Dropping it here stopped the game with
 * "is not a texture" at the first replay of such a command.
 */
uint64_t nvnh_nvnTextureFinalize(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_texset_texture_gone(c->x[0]);
    nvn_texture *t = (nvn_texture *)nvn_obj_get(c->x[0], NVN_TEXTURE);
    if (t)
        t->finalized = true;
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

bool nvn_texture_desc(uint64_t tex, nvn_tex_desc *out)
{
    nvn_texture *t = (nvn_texture *)nvn_obj_acquire(tex, NVN_TEXTURE);
    if (!t)
        return false;
    *out = t->d;
    nvn_obj_release(&t->o);
    return true;
}

bool nvn_texture_record(uint64_t tex, const nvn_tex_desc *d, const uint8_t *builder)
{
    nvn_texture *t = (nvn_texture *)nvn_obj_new(tex, NVN_TEXTURE, sizeof *t);
    if (t) {
        t->d = *d;
        if (builder) {
            memcpy(t->builder, builder, sizeof t->builder);
            t->has_builder = true;
        }
    }
    return nvn_obj_publish((nvn_obj *)t) != NULL;
}

static bool tex_of(rl_ctx *c, uint64_t a, const char *proc, nvn_tex_desc *out)
{
    bool ok = nvn_texture_desc(a, out);
    if (!ok)
        rl_guest_fatal(c, "nvn", "%s on 0x%llx, not a texture", proc, (unsigned long long)a);
    return ok;
}

/**
 * @brief nvnTextureGetSwizzle(const NVNtexture *, NVNtextureSwizzle *r, *g, *b, *a): the builder's swizzle. An
 * NVN2 driver's own code reads it back from the texture its nvnTextureInitialize filled; on NVN1, whose textures
 * the layer initializes itself, the driver's getter would read fields nothing wrote, so the layer answers with the
 * swizzle it read from the builder (the driver's NVN1 texture holds a copy of the builder's four words).
 */
uint64_t nvnh_nvnTextureGetSwizzle(rl_ctx *c, uint8_t *mem)
{
    nvn_tex_desc dd;
    const nvn_tex_desc *d = tex_of(c, c->x[0], "nvnTextureGetSwizzle", &dd) ? &dd : NULL;
    if (!d)
        return RL_PENDING;
    for (unsigned i = 0; i < 4; i++)
        rl_st32(mem, c->x[1 + i], d->swizzle[i]);
    return nvn_ret(c);
}

/** @brief nvnTextureGetDepthStencilMode(const NVNtexture *): NVNdepthStencilMode, the builder's word as the driver's NVN1 texture holds it. */
uint64_t nvnh_nvnTextureGetDepthStencilMode(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_tex_desc dd;
    const nvn_tex_desc *d = tex_of(c, c->x[0], "nvnTextureGetDepthStencilMode", &dd) ? &dd : NULL;
    if (!d)
        return RL_PENDING;
    c->x[0] = d->ds_mode;
    return nvn_ret(c);
}

/**
 * @brief nvnTextureGetStorageClass(const NVNtexture *): NVNstorageClass. The NVN1 driver returns 0x100 or a byte its
 * nvnTextureInitialize stores in the texture, from the same inputs its nvnTextureBuilderGetStorageClass reads (0x100
 * for a buffer or a linear texture, else what the format and device give); the layer keeps the builder's bytes and
 * runs the driver's own builder function on a copy of them (inferred to be the same value, see
 * .work/s2-console-questions.md). Stops when the texture has no builder bytes or the build read no driver layout.
 */
uint64_t nvnh_nvnTextureGetStorageClass(rl_ctx *c, uint8_t *mem)
{
    nvn_texture *t = (nvn_texture *)nvn_obj_get(c->x[0], NVN_TEXTURE);
    if (!t)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnTextureGetStorageClass on 0x%llx, not a texture", (unsigned long long)c->x[0]);
    if (!t->has_builder)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnTextureGetStorageClass: the layer holds no builder for 0x%llx", (unsigned long long)c->x[0]);
    uint64_t sp = c->sp, copy = (sp - NVN_TEXTURE_BUILDER_BYTES) & ~15ull, cls = 0;
    memcpy(mem + copy, t->builder, sizeof t->builder);
    c->sp = copy;
    int rc = nvn_builder_storage_class(c, mem, copy, &cls);
    c->sp = sp;
    if (rc != RL_OK)
        return (uint64_t)rc;
    if (!cls)
        return (uint64_t)rl_guest_fatal(c, "nvn", "nvnTextureGetStorageClass: the build read no driver layout to answer it from");
    c->x[0] = cls;
    return nvn_ret(c);
}

/**
 * @brief nvnTextureCompare(const NVNtexture *, const NVNtexture *): NVNboolean. The NVN1 driver compares the words
 * of the two texture objects its own nvnTextureInitialize filled: the pool, offset, derived addresses and size,
 * width, height, depth, levels, format, flags, swizzle, depth stencil mode and the 64-byte hardware descriptor. The
 * layer does not run that initialization on NVN1, so it compares what those words are derived from, the values it
 * read from the two builders: textures that differ in one of them differ in a word of the driver's, and the
 * driver's derived words (class, size, descriptor) follow from them and the device, which one program shares. A
 * stride only reaches the driver's words of a linear texture, so it only counts there. (On NVN2 the driver's own
 * function stays, as its nvnTextureInitialize runs.)
 */
uint64_t nvnh_nvnTextureCompare(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    nvn_tex_desc ad, bd;
    const nvn_tex_desc *a = tex_of(c, c->x[0], "nvnTextureCompare", &ad) ? &ad : NULL;
    const nvn_tex_desc *b = a && tex_of(c, c->x[1], "nvnTextureCompare", &bd) ? &bd : NULL;
    if (!a || !b)
        return RL_PENDING;
    c->x[0] = a->pool == b->pool && a->offset == b->offset && a->flags == b->flags && a->target == b->target &&
              a->width == b->width && a->height == b->height && a->depth == b->depth && a->levels == b->levels &&
              a->format == b->format && a->samples == b->samples && !memcmp(a->swizzle, b->swizzle, sizeof a->swizzle) &&
              a->ds_mode == b->ds_mode && (!(a->flags & (FLAG_LINEAR | FLAG_LINEAR_RENDER_TARGET)) || a->stride == b->stride);
    return nvn_ret(c);
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
    c->x[0] = nvn_obj_publish((nvn_obj *)s) != NULL;
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
    c->x[0] = nvn_obj_publish((nvn_obj *)p) != NULL;
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

/**
 * @brief The memory pool and offset a descriptor pool was initialized with. The driver keeps them at +0 and +8 of
 * the pool object and returns them (SDK 16.2.0 and 22.2.0 getters, one load each); the layer's pool initialization
 * does not write the driver's object, so it answers from its own record.
 */
static uint64_t pool_field(rl_ctx *c, uint32_t kind, bool offset)
{
    nvn_desc_pool *p = (nvn_desc_pool *)nvn_obj_get(c->x[0], kind);
    c->x[0] = p ? (offset ? p->offset : p->pool) : 0;
    return nvn_ret(c);
}

/** @brief nvnTexturePoolGetMemoryPool(const NVNtexturePool *). */
uint64_t nvnh_nvnTexturePoolGetMemoryPool(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return pool_field(c, NVN_TEXTURE_POOL, false);
}

/** @brief nvnTexturePoolGetMemoryOffset(const NVNtexturePool *). */
uint64_t nvnh_nvnTexturePoolGetMemoryOffset(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return pool_field(c, NVN_TEXTURE_POOL, true);
}

/** @brief nvnSamplerPoolGetMemoryPool(const NVNsamplerPool *). */
uint64_t nvnh_nvnSamplerPoolGetMemoryPool(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return pool_field(c, NVN_SAMPLER_POOL, false);
}

/** @brief nvnSamplerPoolGetMemoryOffset(const NVNsamplerPool *). */
uint64_t nvnh_nvnSamplerPoolGetMemoryOffset(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return pool_field(c, NVN_SAMPLER_POOL, true);
}

/** @brief Registrations by descriptor id (relift keeps one table for all texture pools and one for all sampler pools). */
static struct {
    uint64_t *tex, *sampler;
    uint8_t (*view)[17];
    bool *image;
    uint32_t ntex, nsampler;
    /** @brief Ids registered since the replay last took them (texture, sampler), one entry per registration. */
    uint32_t *pending[2], npending[2], cappending[2];
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

/** @brief Notes that id was registered, for nvn_tex_pending; the registry lock is held. Out of memory drops the note. */
static void note_pending(int kind, uint32_t id)
{
    if (reg.npending[kind] == reg.cappending[kind]) {
        uint32_t cap = reg.cappending[kind] ? 2 * reg.cappending[kind] : 1024;
        uint32_t *p = realloc(reg.pending[kind], cap * sizeof *p);
        if (!p)
            return;
        reg.pending[kind] = p;
        reg.cappending[kind] = cap;
    }
    reg.pending[kind][reg.npending[kind]++] = id;
}

uint32_t nvn_tex_pending(bool sampler, uint32_t *ids, uint32_t max)
{
    int kind = sampler;
    pthread_mutex_lock(&reg_lock);
    uint32_t n = reg.npending[kind] < max ? reg.npending[kind] : max;
    memcpy(ids, reg.pending[kind], n * sizeof *ids);
    memmove(reg.pending[kind], reg.pending[kind] + n, (reg.npending[kind] - n) * sizeof *ids);
    reg.npending[kind] -= n;
    pthread_mutex_unlock(&reg_lock);
    return n;
}

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
    note_pending(0, id);
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

bool nvn_sampler_registered(uint32_t id, uint8_t out[128])
{
    pthread_mutex_lock(&reg_lock);
    uint64_t s = id < reg.nsampler ? reg.sampler[id] : 0;
    pthread_mutex_unlock(&reg_lock);
    nvn_sampler *o = s ? (nvn_sampler *)nvn_obj_acquire(s, NVN_SAMPLER) : NULL;
    if (!o)
        return false;
    memcpy(out, o->builder_copy, 128);
    nvn_obj_release(&o->o);
    return true;
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
    note_pending(1, id);
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
    nvn_tex_desc dd;
    const nvn_tex_desc *d = nvn_texture_desc(c->x[0], &dd) ? &dd : NULL;
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

bool nvn_copy_plan_of(const nvn_region *sr, const nvn_region *dr, uint32_t flags, uint32_t sb, uint32_t sw, uint32_t sh,
                      uint32_t db, uint32_t dw, uint32_t dh, bool same_aspect, bool color, nvn_copy_plan *out,
                      const char **why)
{
    memset(out, 0, sizeof *out);
    const uint32_t known = NVN_COPY_LINEAR_FILTER | NVN_COPY_MIRROR_X | NVN_COPY_MIRROR_Y | NVN_COPY_MIRROR_Z;
    if (flags & ~known) {
        *why = "flags";
        return false;
    }
    bool texel_blocks = sw == 1 && sh == 1;
    bool mx = texel_blocks && (flags & NVN_COPY_MIRROR_X), my = texel_blocks && (flags & NVN_COPY_MIRROR_Y);
    bool same = sr->width == dr->width && sr->height == dr->height && sb == db && sw == dw && sh == dh && same_aspect;
    if (same && !mx && !my) {
        out->texel_copy = true;
        return true;
    }
    if (!texel_blocks || dw != 1 || dh != 1 || !same_aspect) {
        *why = "formats";
        return false;
    }
    out->src[0][0] = mx ? sr->x + sr->width : sr->x;
    out->src[1][0] = mx ? sr->x : sr->x + sr->width;
    out->src[0][1] = my ? sr->y + sr->height : sr->y;
    out->src[1][1] = my ? sr->y : sr->y + sr->height;
    out->dst[0][0] = dr->x;
    out->dst[0][1] = dr->y;
    out->dst[1][0] = dr->x + dr->width;
    out->dst[1][1] = dr->y + dr->height;
    out->linear = (flags & NVN_COPY_LINEAR_FILTER) && color;
    return true;
}
