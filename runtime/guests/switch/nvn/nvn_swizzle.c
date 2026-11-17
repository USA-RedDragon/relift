/**
 * @file nvn_swizzle.c
 * @brief Copies between packed texels and a texture's storage: block-linear
 * (GOBs of 64 bytes by 8 rows, stacked in blocks) or linear.
 */
#include "nvn_priv.h"

/** @brief Offset of byte x of row y inside a GOB. */
static uint32_t gob_offset(uint32_t x, uint32_t y)
{
    return ((x % 64) / 32) * 256 + ((y % 8) / 2) * 64 + ((x % 32) / 16) * 32 + (y % 2) * 16 + (x % 16);
}

static uint32_t div_up(uint32_t a, uint32_t b) { return (a + b - 1) / b; }

/** @brief The block height (in GOBs) level l of d uses. */
static uint32_t level_block_h(const nvn_tex_desc *d, uint32_t hb)
{
    uint32_t b = d->block_h, n = div_up(hb, 8);
    while (b > 1 && n <= b / 2)
        b /= 2;
    return b;
}

static uint32_t level_block_d(const nvn_tex_desc *d, uint32_t depth)
{
    uint32_t b = d->block_d ? d->block_d : 1;
    while (b > 1 && depth <= b / 2)
        b /= 2;
    return b;
}

bool nvn_tex_copy(const nvn_tex_desc *d, uint8_t *storage, uint32_t level, uint32_t layer, const nvn_region *r,
                  uint8_t *packed, bool to_storage)
{
    uint32_t bpb, bw, bh;
    if (!nvn_tex_format(d->format, &bpb, &bw, &bh) || level >= NVN_TEX_MAX_LEVELS)
        return false;
    uint32_t w = d->width >> level ? d->width >> level : 1, h = d->height >> level ? d->height >> level : 1;
    uint32_t depth = d->target == 2 ? (d->depth >> level ? d->depth >> level : 1) : 1;
    uint32_t wb = div_up(w, bw), hb = div_up(h, bh);
    if (r->x < 0 || r->y < 0 || r->z < 0 || r->width < 0 || r->height < 0 || r->depth < 0)
        return false;
    uint32_t x0 = (uint32_t)r->x / bw, y0 = (uint32_t)r->y / bh;
    uint32_t cw = div_up((uint32_t)r->width, bw), ch = div_up((uint32_t)r->height, bh);
    uint32_t z0 = (uint32_t)r->z, nz = (uint32_t)r->depth;
    if (x0 + cw > wb || y0 + ch > hb || (d->target == 2 && z0 + nz > depth))
        return false;
    uint64_t row = (uint64_t)cw * bpb;
    uint8_t *base = storage + d->layer_stride * layer + d->level_offset[level];
    if (!d->block_h) {
        uint64_t stride = d->stride ? d->stride : (uint64_t)wb * bpb;
        for (uint32_t z = 0; z < nz; z++)
            for (uint32_t y = 0; y < ch; y++) {
                uint8_t *s = base + ((uint64_t)(z0 + z) * hb + y0 + y) * stride + (uint64_t)x0 * bpb;
                uint8_t *p = packed + ((uint64_t)z * ch + y) * row;
                memcpy(to_storage ? s : p, to_storage ? p : s, row);
            }
        return true;
    }
    uint32_t gbh = level_block_h(d, hb), gbd = d->target == 2 ? level_block_d(d, depth) : 1;
    uint32_t gobs_w = div_up(wb * bpb, 64);
    uint32_t block_rows = div_up(hb, 8 * gbh);
    uint64_t block_bytes = 512ull * gbh * gbd;
    for (uint32_t z = 0; z < nz; z++) {
        uint32_t zz = z0 + z;
        uint64_t slab = (uint64_t)(zz / gbd) * block_rows * gobs_w * block_bytes + (uint64_t)(zz % gbd) * 512 * gbh;
        for (uint32_t y = 0; y < ch; y++) {
            uint32_t yy = y0 + y;
            uint64_t yrow = slab + (uint64_t)(yy / (8 * gbh)) * gobs_w * block_bytes + (uint64_t)((yy / 8) % gbh) * 512;
            uint8_t *p = packed + ((uint64_t)z * ch + y) * row;
            for (uint32_t xb = 0; xb < row; xb++) {
                uint32_t bx = x0 * bpb + xb;
                uint8_t *s = base + yrow + (uint64_t)(bx / 64) * block_bytes + gob_offset(bx, yy);
                if (to_storage)
                    *s = p[xb];
                else
                    p[xb] = *s;
            }
        }
    }
    return true;
}
