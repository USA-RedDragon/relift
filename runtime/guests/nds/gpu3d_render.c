/**
 * @file gpu3d_render.c
 * @brief The rendering engine: the polygons of the last swap drawn one line
 * at a time into the 3D layer. At the frame's start (display line 214) the
 * engine sorts the polygons (opaque first, by their top and bottom lines;
 * translucent ones too unless the swap asked for manual order) and
 * normalizes each polygon's W values to 16 bits. Each line then starts from
 * the rear plane (the clear registers or the clear bitmaps in texture slots
 * 2 and 3), walks the two edges of each polygon that cross it, interpolates
 * color, texture coordinates, depth and W along the edges and across the
 * span with the engine's limited-precision perspective factor (or linearly
 * when the W values agree), samples the texture in its format, blends it
 * with the vertex color (modulation, decal, toon, highlight), runs the
 * alpha and depth tests, the shadow stencil and the translucent polygon ID
 * rule, and blends translucent pixels. A line is finished one line later
 * (edge marking looks at the lines above and below): edge marking, fog and
 * anti-aliasing, then it goes to the 3D layer the display reads 48 lines on.
 *
 * Line y is drawn at display line (y + 214) mod 263, so the frame's first
 * 48 lines are ready when line 0 is shown (the 48-line buffer); the
 * rendering registers and texture memory are read when a line is drawn.
 *
 * Fresh C from GBATEK ("DS 3D Overview", "DS 3D Polygon Attributes", "DS 3D
 * Texture Formats", "DS 3D Texture Blending", "DS 3D Toon, Edge, Fog,
 * Alpha-Blending, Anti-Aliasing", "DS 3D Shadow Polygons", "DS 3D
 * Rear-Plane") and from published descriptions of the hardware's
 * interpolation (a perspective factor with 9 fraction bits along edges and 8
 * across spans, the linear path for equal W values, W normalization).
 */
#include "nds_int.h"

#include <stdlib.h>
#include <string.h>

/** @brief Attribute buffer bits of a pixel. */
enum {
    A_OPAQUE_ID = 0x3F,
    A_TRANSL_ID_SHIFT = 6,
    A_TRANSL_ID = 0x3F << A_TRANSL_ID_SHIFT,
    A_FOG = 1u << 12,
    A_TRANSL = 1u << 13,
    A_EDGE = 1u << 14,
    A_BACK = 1u << 15,
    A_STENCIL = 1u << 16,
    A_COVER_SHIFT = 17,
    A_COVER = 31u << A_COVER_SHIFT,
};

/** @brief DISP3DCNT bits. */
enum {
    D_TEX = 1u << 0,
    D_HIGHLIGHT = 1u << 1,
    D_ALPHA_TEST = 1u << 2,
    D_BLEND = 1u << 3,
    D_AA = 1u << 4,
    D_EDGE = 1u << 5,
    D_FOG_ALPHA = 1u << 6,
    D_FOG = 1u << 7,
    D_REAR_BITMAP = 1u << 14,
};

/** @brief The display line at which the rendering engine starts a frame. */
#define RENDER_START 214u

/** @brief The 24-bit depth of a 15-bit clear depth. */
static uint32_t depth24(uint32_t d15) { return d15 * 0x200u + ((d15 + 1u) / 0x8000u) * 0x1FFu; }

/** @brief 6-bit expansion of a 5-bit channel. */
static uint32_t c6(uint32_t c5) { return c5 ? c5 * 2u + 1u : 0; }

/** @brief The 18-bit color of a 15-bit one, channels expanded to 6 bits. */
static uint32_t rgb18(uint32_t c)
{
    return c6(c & 31u) | c6((c >> 5) & 31u) << 6 | c6((c >> 10) & 31u) << 12;
}

static uint32_t pack(uint32_t r, uint32_t g, uint32_t b, uint32_t a) { return r | g << 6 | b << 12 | a << 24; }
static uint32_t ch(uint32_t c, unsigned k) { return (c >> (6 * k)) & 63u; }
static uint32_t alpha_of(uint32_t c) { return (c >> 24) & 31u; }

/** @brief The interpolation of one stretch (along an edge or across a span). */
typedef struct interp {
    int64_t x, len;
    /** @brief The perspective factor and its fraction bits, or the linear path. */
    int64_t factor;
    unsigned bits;
    bool linear;
} interp;

/**
 * @brief Sets up the interpolation at x of len between W values w0 and w1;
 * edge selects the edge variant (9 bits). linear is the polygon's choice of
 * the linear path for every edge and span (see setup_poly).
 */
static void interp_setup(interp *ip, int64_t x, int64_t len, uint32_t w0, uint32_t w1, bool edge, bool linear)
{
    ip->x = x;
    ip->len = len;
    ip->bits = edge ? 9 : 8;
    ip->linear = linear;
    ip->factor = 0;
    if (ip->linear || len <= 0)
        return;
    int64_t num, den;
    if (edge) {
        uint32_t w0n, w0d, w1d;
        if ((w0 & 1) && !(w1 & 1)) {
            w0n = w0 - 1;
            w0d = w0 + 1;
            w1d = w1;
        } else {
            w0n = w0 & 0xFFFEu;
            w0d = w0 & 0xFFFEu;
            w1d = w1 & 0xFFFEu;
        }
        num = (x * (int64_t)(w0n >> 1)) << 9;
        den = (len - x) * (int64_t)(w1d >> 1) + x * (int64_t)(w0d >> 1);
    } else {
        num = (x * (int64_t)w0) << 8;
        den = (len - x) * (int64_t)w1 + x * (int64_t)w0;
    }
    ip->factor = den ? num / den : 0;
}

/** @brief The attribute between a (at 0) and b (at len), the hardware's unsigned way. */
static int64_t interp_at(const interp *ip, int64_t a, int64_t b)
{
    if (ip->len <= 0 || a == b)
        return a;
    if (ip->linear) {
        if (a < b)
            return a + ((b - a) * ip->x) / ip->len;
        return b + ((a - b) * (ip->len - ip->x)) / ip->len;
    }
    int64_t one = (int64_t)1 << ip->bits;
    if (a < b)
        return a + (((b - a) * ip->factor) >> ip->bits);
    return b + (((a - b) * (one - ip->factor)) >> ip->bits);
}

/** @brief Plain linear interpolation for Z (screen space), the unsigned way. */
static int64_t lerp(int64_t a, int64_t b, int64_t x, int64_t len)
{
    if (len <= 0 || a == b)
        return a;
    if (a < b)
        return a + ((b - a) * x) / len;
    return b + ((a - b) * (len - x)) / len;
}

/** @brief The attributes at a point: 9-bit color, texture coordinates, Z depth, W depth, normalized W. */
typedef struct point {
    int64_t col[3], s, t, z, w, wn;
} point;

/**
 * @brief One edge crossing a line: the run of pixels it covers there [r0, r1)
 * and the attributes at the span end it gives.
 */
typedef struct edge {
    int32_t r0, r1;
    bool xmajor, positive, vertical, diag;
    /** @brief The top and bottom vertices' screen positions. */
    int32_t ax, ay, bx, by;
    point p;
} edge;

/** @brief The attributes of vertex i of poly. */
static point vpoint(const nds_gx_ram *r, const nds_gx_poly *p, unsigned i, bool wbuf)
{
    const nds_gx_vtx *v = &r->vtx[p->v[i]];
    point o;
    for (unsigned k = 0; k < 3; k++)
        o.col[k] = v->v.col[k];
    o.s = v->v.s;
    o.t = v->v.t;
    o.z = wbuf ? 0 : v->z;
    o.w = v->v.c[3];
    o.wn = p->wn[i];
    return o;
}

static void point_interp(point *o, const interp *ip, const point *a, const point *b, int64_t zx, int64_t zlen)
{
    for (unsigned k = 0; k < 3; k++)
        o->col[k] = interp_at(ip, a->col[k], b->col[k]);
    o->s = interp_at(ip, a->s, b->s);
    o->t = interp_at(ip, a->t, b->t);
    o->w = interp_at(ip, a->w, b->w);
    o->wn = interp_at(ip, a->wn, b->wn);
    o->z = lerp(a->z, b->z, zx, zlen);
}

/**
 * @brief The edge from vertex ia (top) to ib (bottom) of poly on line y. The
 * slope is |dx| times the reciprocal of dy with 18 fraction bits (exactly 1.0
 * when |dx| equals dy). A y-major edge covers one pixel: the edge's x at the
 * top of the line, truncated, one pixel further left when the edge runs to
 * the left. An x-major edge covers the pixels between its x at the top and at
 * the bottom of the line, each rounded. right selects the span end the edge
 * gives: its attributes are interpolated by line for a y-major edge, and by
 * the edge's 18-bit x at the line's top or bottom (whichever is the span's
 * end) for an x-major edge or a 45-degree one longer than a line.
 */
static void edge_at(const nds_gx_ram *r, const nds_gx_poly *p, unsigned ia, unsigned ib, int32_t y, bool right, bool wbuf,
                    edge *e)
{
    const nds_gx_vtx *a = &r->vtx[p->v[ia]], *b = &r->vtx[p->v[ib]];
    int32_t dy = b->y - a->y, dx = b->x - a->x, adx = dx < 0 ? -dx : dx;
    int64_t k = y - a->y;
    e->positive = dx >= 0;
    e->vertical = dx == 0;
    e->xmajor = adx > dy;
    e->diag = adx == dy;
    e->ax = a->x;
    e->ay = a->y;
    e->bx = b->x;
    e->by = b->y;
    int64_t inc = adx == dy ? 1 << 18 : (int64_t)adx * ((1 << 18) / dy);
    if (e->xmajor) {
        int32_t near = (int32_t)((inc * k + 0x20000) >> 18), far = (int32_t)((inc * (k + 1) + 0x20000) >> 18);
        e->r0 = e->positive ? a->x + near : a->x - far;
        e->r1 = e->positive ? a->x + far : a->x - near;
    } else {
        int32_t d = (int32_t)((inc * k) >> 18);
        e->r0 = e->vertical ? a->x : e->positive ? a->x + d : a->x - 1 - d;
        e->r1 = e->r0 + 1;
    }
    point pa = vpoint(r, p, ia, wbuf), pb = vpoint(r, p, ib, wbuf);
    int64_t pos, len;
    if (e->xmajor || (e->diag && dy > 1)) {
        int64_t j = right == e->positive ? k + 1 : k;
        pos = j * inc;
        len = (int64_t)adx << 18;
        if (pos > len)
            pos = len;
    } else {
        pos = k;
        len = dy;
    }
    interp ip;
    interp_setup(&ip, pos, len, p->wn[ia], p->wn[ib], true, p->linear);
    point_interp(&e->p, &ip, &pa, &pb, pos, len);
}

/** @brief A texel of the polygon's texture at (s, t) in texels: 15-bit color and 5-bit alpha in bits 24 to 28. */
static uint32_t texel(rl_nds *m, uint32_t tex, uint32_t pal, int32_t s, int32_t t)
{
    unsigned fmt = (tex >> 26) & 7u;
    int32_t w = 8 << ((tex >> 20) & 7u), h = 8 << ((tex >> 23) & 7u);
    if (tex & (1u << 16)) {
        if ((tex & (1u << 18)) && (s & w))
            s = (w - 1) - (s & (w - 1));
        else
            s &= w - 1;
    } else {
        s = s < 0 ? 0 : s >= w ? w - 1 : s;
    }
    if (tex & (1u << 17)) {
        if ((tex & (1u << 19)) && (t & h))
            t = (h - 1) - (t & (h - 1));
        else
            t &= h - 1;
    } else {
        t = t < 0 ? 0 : t >= h ? h - 1 : t;
    }
    uint32_t base = (tex & 0xFFFFu) << 3, at = (uint32_t)(t * w + s);
    uint32_t pbase = pal << 4;
    bool zero_clear = (tex >> 29) & 1;
    unsigned idx;
    switch (fmt) {
    case 1: {
        uint32_t b = nds_vram_rd(m, NDS_V_TEX, base + at, 1);
        uint32_t c = nds_vram_rd(m, NDS_V_TEXPAL, pbase + (b & 31u) * 2, 2);
        uint32_t a = (b >> 5) & 7u;
        return (c & 0x7FFFu) | ((a * 4 + a / 2) << 24);
    }
    case 2:
        idx = (nds_vram_rd(m, NDS_V_TEX, base + at / 4, 1) >> ((at & 3) * 2)) & 3u;
        pbase = pal << 3;
        break;
    case 3:
        idx = (nds_vram_rd(m, NDS_V_TEX, base + at / 2, 1) >> ((at & 1) * 4)) & 15u;
        break;
    case 4:
        idx = nds_vram_rd(m, NDS_V_TEX, base + at, 1);
        break;
    case 5: {
        uint32_t blk = (uint32_t)((t >> 2) * (w >> 2) + (s >> 2));
        uint32_t word = nds_vram_rd(m, NDS_V_TEX, base + blk * 4, 4);
        unsigned v = (word >> ((t & 3) * 8 + (s & 3) * 2)) & 3u;
        uint32_t slot1 = 0x20000u + ((base & 0x1FFFFu) >> 1) + (base >= 0x40000u ? 0x10000u : 0) + blk * 2;
        uint32_t info = nds_vram_rd(m, NDS_V_TEX, slot1, 2);
        uint32_t pa = pbase + (info & 0x3FFFu) * 4;
        unsigned mode = (info >> 14) & 3u;
        uint32_t c0 = nds_vram_rd(m, NDS_V_TEXPAL, pa, 2), c1 = nds_vram_rd(m, NDS_V_TEXPAL, pa + 2, 2);
        if (v < 2)
            return (v ? c1 : c0) | 31u << 24;
        if (mode == 0 || mode == 2) {
            if (v == 3 && mode == 0)
                return 0;
            return nds_vram_rd(m, NDS_V_TEXPAL, pa + v * 2, 2) | 31u << 24;
        }
        if (mode == 1 && v == 3)
            return 0;
        uint32_t out = 0;
        for (unsigned k = 0; k < 3; k++) {
            uint32_t a = (c0 >> (5 * k)) & 31u, b = (c1 >> (5 * k)) & 31u, c;
            if (mode == 1)
                c = (a + b) / 2;
            else if (v == 2)
                c = (a * 5 + b * 3) / 8;
            else
                c = (a * 3 + b * 5) / 8;
            out |= c << (5 * k);
        }
        return out | 31u << 24;
    }
    case 6: {
        uint32_t b = nds_vram_rd(m, NDS_V_TEX, base + at, 1);
        uint32_t c = nds_vram_rd(m, NDS_V_TEXPAL, pbase + (b & 7u) * 2, 2);
        return (c & 0x7FFFu) | (b >> 3) << 24;
    }
    case 7: {
        uint32_t c = nds_vram_rd(m, NDS_V_TEX, base + at * 2, 2);
        return (c & 0x7FFFu) | ((c & 0x8000u) ? 31u << 24 : 0);
    }
    default:
        return 0x7FFFu | 31u << 24;
    }
    if (idx == 0 && zero_clear)
        return 0;
    return (nds_vram_rd(m, NDS_V_TEXPAL, pbase + idx * 2, 2) & 0x7FFFu) | 31u << 24;
}

/** @brief The polygon's pixel color at point q: vertex color, texture and shading; alpha 0 to 31 in bits 24-28. */
static uint32_t shade(rl_nds *m, const nds_gx *g, const nds_gx_poly *p, const point *q, bool wire)
{
    uint32_t v[3];
    for (unsigned k = 0; k < 3; k++) {
        int64_t c = q->col[k] >> 3;
        v[k] = (uint32_t)(c < 0 ? 0 : c > 63 ? 63 : c);
    }
    uint32_t pa = (p->attr >> 16) & 31u, av = wire ? 63u : c6(pa);
    unsigned mode = (p->attr >> 4) & 3u;
    if (mode == 2) {
        uint32_t tc = g->toon[v[0] >> 1] & 0x7FFFu;
        if (g->disp3dcnt & D_HIGHLIGHT) {
            uint32_t gray = v[0];
            v[0] = v[1] = v[2] = gray;
        } else {
            for (unsigned k = 0; k < 3; k++)
                v[k] = c6((tc >> (5 * k)) & 31u);
        }
    }
    unsigned fmt = (p->tex >> 26) & 7u;
    uint32_t out[3], a;
    if ((g->disp3dcnt & D_TEX) && fmt) {
        uint32_t tx = texel(m, p->tex, p->pal, (int32_t)(q->s >> 4), (int32_t)(q->t >> 4));
        uint32_t t[3], ta = alpha_of(tx);
        for (unsigned k = 0; k < 3; k++)
            t[k] = c6((tx >> (5 * k)) & 31u);
        uint32_t at6 = c6(ta);

        if (mode == 1) {
            for (unsigned k = 0; k < 3; k++)
                out[k] = ta == 0 ? v[k] : ta == 31 ? t[k] : (t[k] * ta + v[k] * (31 - ta)) / 32;
            a = av;
        } else {
            for (unsigned k = 0; k < 3; k++)
                out[k] = ((t[k] + 1) * (v[k] + 1) - 1) / 64;
            a = ((at6 + 1) * (av + 1) - 1) / 64;
        }
        if (mode == 2 && (g->disp3dcnt & D_HIGHLIGHT)) {
            uint32_t tc = g->toon[v[0] >> 1] & 0x7FFFu;
            for (unsigned k = 0; k < 3; k++) {
                out[k] += c6((tc >> (5 * k)) & 31u);
                out[k] = out[k] > 63 ? 63 : out[k];
            }
        }
    } else {
        for (unsigned k = 0; k < 3; k++)
            out[k] = v[k];
        a = av;
        if (mode == 2 && (g->disp3dcnt & D_HIGHLIGHT)) {
            uint32_t tc = g->toon[v[0] >> 1] & 0x7FFFu;
            for (unsigned k = 0; k < 3; k++) {
                out[k] += c6((tc >> (5 * k)) & 31u);
                out[k] = out[k] > 63 ? 63 : out[k];
            }
        }
    }
    return pack(out[0], out[1], out[2], a >> 1);
}

/** @brief The depth test of a new pixel against the buffer. */
static bool depth_ok(const nds_gx_poly *p, uint32_t z, uint32_t old, uint32_t old_attr)
{
    if (p->attr & 0x4000) {
        uint32_t d = z > old ? z - old : old - z;
        return d <= 0x200;
    }
    if (p->front && (old_attr & A_BACK) && !(old_attr & A_TRANSL))
        return z <= old;
    return z < old;
}

/** @brief Fills line y's buffers with the rear plane. */
static void clear_line(rl_nds *m, nds_gx *g, nds_gx_lbuf *l, unsigned y)
{
    uint32_t id = (g->clear_color >> 24) & 0x3Fu;
    if (g->disp3dcnt & D_REAR_BITMAP) {
        unsigned ox = g->clear_ofs & 0xFFu, oy = (g->clear_ofs >> 8) & 0xFFu;
        for (unsigned x = 0; x < 256; x++) {
            uint32_t at = (((y + oy) & 0xFFu) * 256u + ((x + ox) & 0xFFu)) * 2u;
            uint32_t c = nds_vram_rd(m, NDS_V_TEX, 0x40000u + at, 2), d = nds_vram_rd(m, NDS_V_TEX, 0x60000u + at, 2);
            l->col[0][x] = l->col[1][x] = rgb18(c & 0x7FFFu) | ((c & 0x8000u) ? 31u << 24 : 0);
            l->depth[x] = l->depth2[x] = depth24(d & 0x7FFFu);
            l->attr[x] = id | ((d & 0x8000u) ? A_FOG : 0);
        }
        return;
    }
    uint32_t c = rgb18(g->clear_color & 0x7FFFu) | ((g->clear_color >> 16) & 31u) << 24;
    uint32_t d = depth24(g->clear_depth), a = id | ((g->clear_color & 0x8000u) ? A_FOG : 0);
    for (unsigned x = 0; x < 256; x++) {
        l->col[0][x] = l->col[1][x] = c;
        l->depth[x] = l->depth2[x] = d;
        l->attr[x] = a;
    }
}

/** @brief Translucent color c over dst: blended when DISP3DCNT enables it and dst is not transparent, else c. */
static uint32_t blend(const nds_gx *g, uint32_t c, uint32_t dst)
{
    uint32_t a = alpha_of(c), da = alpha_of(dst);
    if (!(g->disp3dcnt & D_BLEND) || !da)
        return c;
    uint32_t r[3];
    for (unsigned k = 0; k < 3; k++)
        r[k] = (ch(c, k) * (a + 1) + ch(dst, k) * (31 - a)) / 32;
    return pack(r[0], r[1], r[2], a > da ? a : da);
}

/**
 * @brief Draws one pixel of poly with color c at depth z; edge marks an edge
 * pixel. A pixel that fails the depth test at a partly covered (anti-aliased)
 * edge pixel still goes into the second color when it is in front of that
 * one, and a translucent pixel blends into both colors, so the final
 * anti-aliasing pass mixes the edge with what ends up behind it.
 */
static void plot(nds_gx *g, nds_gx_lbuf *l, const nds_gx_poly *p, unsigned x, uint32_t c, uint32_t z, bool edge, uint32_t cov)
{
    uint32_t a = alpha_of(c), old_attr = l->attr[x];
    unsigned id = (p->attr >> 24) & 0x3Fu;
    if ((g->disp3dcnt & D_ALPHA_TEST) ? a <= g->alpha_ref : a == 0)
        return;
    unsigned mode = (p->attr >> 4) & 3u;
    if (mode == 3) {
        if (id == 0) {
            if (!depth_ok(p, z, l->depth[x], old_attr))
                l->attr[x] = old_attr | A_STENCIL;
            return;
        }
        if (!(old_attr & A_STENCIL))
            return;
        l->attr[x] = old_attr & ~A_STENCIL;
        if ((old_attr & A_OPAQUE_ID) == id)
            return;
        old_attr &= ~A_STENCIL;
    }
    if (!depth_ok(p, z, l->depth[x], old_attr)) {
        if ((old_attr & A_EDGE) && ((old_attr & A_COVER) >> A_COVER_SHIFT) < 31 && z < l->depth2[x]) {
            l->col[1][x] = a == 31 ? c : blend(g, c, l->col[1][x]);
            if (a == 31 || (p->attr & 0x800))
                l->depth2[x] = z;
        }
        return;
    }
    bool fog = (p->attr >> 15) & 1;
    if (a == 31) {
        l->col[1][x] = l->col[0][x];
        l->col[0][x] = c;
        l->depth2[x] = l->depth[x];
        l->depth[x] = z;
        l->attr[x] = id | (fog ? A_FOG : 0) | (edge ? A_EDGE : 0) | (p->front ? 0 : A_BACK) | cov << A_COVER_SHIFT;
        return;
    }
    if ((old_attr & A_TRANSL) && ((old_attr & A_TRANSL_ID) >> A_TRANSL_ID_SHIFT) == id)
        return;
    l->col[0][x] = blend(g, c, l->col[0][x]);
    l->col[1][x] = blend(g, c, l->col[1][x]);
    if (p->attr & 0x800)
        l->depth[x] = z;
    uint32_t keep = old_attr & (A_OPAQUE_ID | A_EDGE | A_BACK | A_COVER);
    l->attr[x] = keep | A_TRANSL | (uint32_t)id << A_TRANSL_ID_SHIFT | ((old_attr & A_FOG) && fog ? A_FOG : 0);
}

/** @brief The depth a point gives in the frame's depth mode. */
static uint32_t depth_of(const point *q, bool wbuf)
{
    int64_t d = wbuf ? q->w : q->z;
    return (uint32_t)(d < 0 ? 0 : d > 0xFFFFFF ? 0xFFFFFF : d);
}

/**
 * @brief The anti-aliasing coverage (0 to 31) of pixel x of edge e's run on
 * line y; right is the side of the span. A y-major edge's pixel takes the
 * fraction of the edge's x at the middle of the line (the left edge the
 * part right of it, the right edge the part left of it), in 5 bits from the
 * edge's 18-bit position; the half step to the middle drops its low 9 bits
 * (console captures of long edges). A pixel the middle has left is
 * uncovered. An
 * x-major edge's pixel takes the part of the line inside the polygon at the
 * pixel's middle, in 5 bits rounded down, one lower still at a whole step
 * when the inside is below the edge (console captures of AA triangles and of
 * AA edges over polygons behind them).
 */
static uint32_t coverage(const edge *e, int32_t y, int32_t x, bool right)
{
    int64_t dy = e->by - e->ay, dx = e->bx - e->ax, adx = dx < 0 ? -dx : dx, k = y - e->ay;
    if (dy <= 0 || e->vertical)
        return 31;
    if (!e->xmajor) {
        int64_t inc = adx == dy ? 1 << 18 : adx * ((1 << 18) / dy);
        int64_t dist = inc * k + ((inc / 2) & ~(int64_t)0x1FF);
        int32_t pix = e->positive ? e->ax + (int32_t)(dist >> 18) : e->ax - 1 - (int32_t)(dist >> 18);
        uint32_t f5 = (uint32_t)((dist & 0x3FFFF) >> 13);
        if (pix != e->r0) {
            bool outward = right ? pix > e->r0 : pix < e->r0;
            return outward ? 31 : 0;
        }
        return right == e->positive ? f5 : 31 - f5;
    }
    int64_t xc2 = 2 * (int64_t)(x - e->ax) + 1;
    if (right && k == (e->positive ? 0 : dy - 1))
        xc2 -= 2;
    int64_t num = xc2 * dy - 2 * k * dx;
    int64_t den = 2 * dx;
    if (den < 0) {
        num = -num;
        den = -den;
    }
    bool below = right == e->positive;
    int64_t c = below ? den - num : num;
    if (c <= 0)
        return 0;
    if (c >= den)
        return 31;
    return (uint32_t)((c * 32 - (below ? 1 : 0)) / den);
}

/** @brief Twice dy times the x where edge (ia, ib) crosses the middle of line y, over 2 dy: for ordering two edges. */
static int64_t mid_num(const nds_gx_ram *r, const nds_gx_poly *p, unsigned ia, unsigned ib, int32_t y, int64_t *den)
{
    const nds_gx_vtx *a = &r->vtx[p->v[ia]], *b = &r->vtx[p->v[ib]];
    int64_t dy = b->y - a->y;
    *den = 2 * dy;
    return (int64_t)a->x * 2 * dy + (int64_t)(b->x - a->x) * (2 * (y - a->y) + 1);
}

/**
 * @brief Draws the part of poly on line y into l. The span runs from the left
 * edge to the right edge. The left edge's run is drawn unless it is an
 * x-major edge running to the right (a lower edge); the right edge's run is
 * drawn only for an x-major edge running to the right, unless the pixel's
 * polygon is drawn at full size (edge marking, anti-aliasing, wireframe, or a
 * translucent pixel with blending), which takes the right edge's run too but
 * for a vertical right edge (GBATEK "Polygon Size").
 */
static void raster(rl_nds *m, nds_gx *g, const nds_gx_ram *r, const nds_gx_poly *p, nds_gx_lbuf *l, int32_t y, bool wbuf)
{
    unsigned n = p->n;
    unsigned found = 0, ea[2] = {0, 0}, eb[2] = {0, 0};
    for (unsigned i = 0; i < n && found < 2; i++) {
        unsigned j = (i + 1) % n;
        int32_t yi = r->vtx[p->v[i]].y, yj = r->vtx[p->v[j]].y;
        if (yi == yj)
            continue;
        unsigned top = yi < yj ? i : j, bot = yi < yj ? j : i;
        int32_t yt = yi < yj ? yi : yj, yb = yi < yj ? yj : yi;
        if (y < yt || y >= yb)
            continue;
        ea[found] = top;
        eb[found] = bot;
        found++;
    }
    edge left, right;
    bool flat = found < 2;
    if (flat) {
        unsigned lo = 0, hi = 0;
        for (unsigned i = 1; i < n; i++) {
            if (r->vtx[p->v[i]].x < r->vtx[p->v[lo]].x)
                lo = i;
            if (r->vtx[p->v[i]].x > r->vtx[p->v[hi]].x)
                hi = i;
        }
        int32_t xl = r->vtx[p->v[lo]].x, xh = r->vtx[p->v[hi]].x;
        left = (edge){.r0 = xl, .r1 = xl + 1, .vertical = true, .positive = true, .p = vpoint(r, p, lo, wbuf)};
        right = (edge){.r0 = xh, .r1 = xh + 1, .vertical = true, .positive = true, .p = vpoint(r, p, hi, wbuf)};
    } else {
        int64_t d0, d1;
        int64_t n0 = mid_num(r, p, ea[0], eb[0], y, &d0), n1 = mid_num(r, p, ea[1], eb[1], y, &d1);
        unsigned li = n0 * d1 > n1 * d0 ? 1 : 0;
        edge_at(r, p, ea[li], eb[li], y, false, wbuf, &left);
        edge_at(r, p, ea[1 - li], eb[1 - li], y, true, wbuf, &right);
    }
    unsigned alpha = (p->attr >> 16) & 31u;
    bool wire = alpha == 0;
    bool full_all = (g->disp3dcnt & (D_AA | D_EDGE)) || wire;
    bool aa = (g->disp3dcnt & D_AA) != 0;
    bool full_transl = (g->disp3dcnt & D_BLEND) != 0;
    bool left_in = !(left.xmajor && left.positive);
    bool right_in = right.xmajor && right.positive;
    int32_t lo_full = left.r0, lo = left_in ? left.r0 : left.r1;
    int32_t hi_full = right.vertical ? right.r0 : right.r1, hi = right_in ? right.r1 : right.r0;
    if (hi_full < left.r1)
        hi_full = left.r1;
    if (hi < left.r1)
        hi = left_in ? left.r1 : lo;
    bool line_poly = !flat && left.ax == right.ax && left.ay == right.ay && left.bx == right.bx && left.by == right.by;
    if (flat || line_poly) {
        lo = lo_full = left.r0;
        hi = hi_full = right.r1;
    }
    bool top_or_bottom = y == p->top || y == p->bottom - 1 || p->top == p->bottom;
    int32_t xl = left.r0, xr = right.vertical ? right.r0 : right.r1;
    interp ip;
    int64_t len = xr - xl;
    for (int32_t x = lo_full < 0 ? 0 : lo_full; x < hi_full && x < 256; x++) {
        bool on_left = x < left.r1, on_right = right.vertical ? x == right.r0 - 1 : x >= right.r0;
        bool is_edge = on_left || on_right || top_or_bottom;
        if (wire && !is_edge)
            continue;
        interp_setup(&ip, x - xl, len, (uint32_t)left.p.wn, (uint32_t)right.p.wn, false, p->linear);
        point q;
        point_interp(&q, &ip, &left.p, &right.p, x - xl, len);
        uint32_t c = shade(m, g, p, &q, wire);
        bool full = full_all || (alpha_of(c) != 31 && full_transl);
        if (!full && (x < lo || x >= hi))
            continue;
        uint32_t cov = 31;
        if (aa) {
            if (on_left && !flat)
                cov = coverage(&left, y, x, false);
            else if (on_right && !flat)
                cov = coverage(&right, y, x, true);
        }
        if (cov == 0 && alpha_of(c) == 31)
            continue;
        plot(g, l, p, (unsigned)x, c, depth_of(&q, wbuf), is_edge, cov);
    }
}

/** @brief Color c fogged by the fog at depth d (DISP3DCNT's alpha-only mode keeps the color). */
static uint32_t fogged(const nds_gx *g, uint32_t c, uint32_t depth)
{
    uint32_t cnt = g->disp3dcnt;
    unsigned shift = (cnt >> 8) & 15u;
    uint32_t step = shift > 10 ? 0 : 0x400u >> shift;
    uint32_t d = depth >> 9, den;
    if (d < (uint32_t)g->fog_ofs + step) {
        den = g->fog[0];
    } else if (step == 0 || d >= (uint32_t)g->fog_ofs + step * 32) {
        den = g->fog[31];
    } else {
        uint32_t i = (d - g->fog_ofs) / step - 1, frac = (d - g->fog_ofs) % step;
        den = (g->fog[i] * (step - frac) + g->fog[i + 1] * frac) / step;
    }
    if (den >= 127)
        den = 128;
    uint32_t fc = rgb18(g->fog_color & 0x7FFFu), fa = (g->fog_color >> 16) & 31u;
    uint32_t r[3];
    for (unsigned k = 0; k < 3; k++)
        r[k] = (cnt & D_FOG_ALPHA) ? ch(c, k) : (ch(fc, k) * den + ch(c, k) * (128 - den)) / 128;
    uint32_t a = (fa * den + alpha_of(c) * (128 - den)) / 128;
    return pack(r[0], r[1], r[2], a);
}

/**
 * @brief Line y's edge marking, fog and anti-aliasing, into the 3D layer;
 * above and below are the neighbor lines (NULL off screen). Anti-aliasing
 * mixes a partly covered edge pixel with the color it covered, by coverage,
 * and a marked edge pixel as a coverage of 16.
 */
static void finish_line(nds_gx *g, const nds_gx_lbuf *l, const nds_gx_lbuf *above, const nds_gx_lbuf *below, uint32_t *out)
{
    uint32_t cnt = g->disp3dcnt;
    uint32_t clear_id = (g->clear_color >> 24) & 0x3Fu, clear_d = depth24(g->clear_depth);
    for (unsigned x = 0; x < 256; x++) {
        uint32_t c = l->col[0][x], at = l->attr[x];
        bool marked = false;
        if ((cnt & D_EDGE) && (at & A_EDGE)) {
            uint32_t id = at & A_OPAQUE_ID, d = l->depth[x];
            const nds_gx_lbuf *nb[2] = {above, below};
            for (unsigned k = 0; k < 2 && !marked; k++) {
                uint32_t nid = nb[k] ? nb[k]->attr[x] & A_OPAQUE_ID : clear_id, nd = nb[k] ? nb[k]->depth[x] : clear_d;
                marked = nid != id && d < nd;
            }
            for (int dx = -1; dx <= 1 && !marked; dx += 2) {
                int nx = (int)x + dx;
                uint32_t nid = (nx < 0 || nx > 255) ? clear_id : l->attr[nx] & A_OPAQUE_ID;
                uint32_t nd = (nx < 0 || nx > 255) ? clear_d : l->depth[nx];
                marked = nid != id && d < nd;
            }
            if (marked)
                c = rgb18(g->edge[id >> 3]) | alpha_of(c) << 24;
        }
        bool fog = (cnt & D_FOG) && (at & A_FOG);
        if (fog)
            c = fogged(g, c, l->depth[x]);
        uint32_t cov = marked ? 16 : (at & A_COVER) >> A_COVER_SHIFT;
        if ((cnt & D_AA) && cov < 31) {
            uint32_t b = l->col[1][x];
            if (fog)
                b = fogged(g, b, l->depth[x]);
            uint32_t r[4];
            for (unsigned k = 0; k < 3; k++)
                r[k] = (ch(c, k) * (cov + 1) + ch(b, k) * (31 - cov)) / 32;
            r[3] = (alpha_of(c) * (cov + 1) + alpha_of(b) * (31 - cov)) / 32;
            c = pack(r[0], r[1], r[2], r[3]);
        }
        out[x] = alpha_of(c) ? (c & 0x3FFFFu) | alpha_of(c) << 24 : 0;
    }
}

/**
 * @brief The polygon's top and bottom lines, its W values normalized to 16
 * bits, and its interpolation path: linear on every edge and span when all
 * its normalized W values are equal with the low bits clear, perspective
 * otherwise, also on the edges and spans whose two ends have equal W
 * (console captures of textured quads).
 */
static void setup_poly(nds_gx_ram *r, nds_gx_poly *p)
{
    int32_t top = 255, bot = 0;
    uint32_t wmax = 0;
    for (unsigned i = 0; i < p->n; i++) {
        const nds_gx_vtx *v = &r->vtx[p->v[i]];
        top = v->y < top ? v->y : top;
        bot = v->y > bot ? v->y : bot;
        uint32_t w = (uint32_t)v->v.c[3];
        wmax = w > wmax ? w : wmax;
    }
    p->top = top;
    p->bottom = bot;
    int sh = 0;
    if (wmax > 0xFFFF) {
        while ((wmax >> sh) > 0xFFFF)
            sh += 4;
    } else if (wmax) {
        while ((wmax << -sh) < 0x1000)
            sh -= 4;
    }
    for (unsigned i = 0; i < p->n; i++) {
        uint32_t w = (uint32_t)r->vtx[p->v[i]].v.c[3];
        p->wn[i] = (uint16_t)(sh >= 0 ? w >> sh : w << -sh);
    }
    p->linear = !(p->wn[0] & 0x7Eu);
    for (unsigned i = 1; i < p->n; i++)
        p->linear &= p->wn[i] == p->wn[0];
}

/** @brief The sort order: opaque before translucent; by top line then bottom line unless the swap asked for manual order of translucent polygons. */
static bool before(const nds_gx_poly *a, const nds_gx_poly *b, bool manual)
{
    if (a->transl != b->transl)
        return !a->transl;
    if (a->transl && manual)
        return a->index < b->index;
    if (a->bottom != b->bottom)
        return a->bottom < b->bottom;
    if (a->top != b->top)
        return a->top < b->top;
    return a->index < b->index;
}

/** @brief The frame's start: runs the geometry engine to the tick (a swap lands), sets up and sorts the polygons. */
static void frame_start(rl_nds *m, uint64_t when)
{
    nds_gx *g = &m->gx;
    nds_gx_run(m, when);
    nds_gx_ram *r = &g->ram[g->geo ^ 1];
    bool manual = g->render_mode & 1;
    g->order_n = r->polys;
    for (uint32_t i = 0; i < r->polys; i++) {
        setup_poly(r, &r->poly[i]);
        uint32_t k = i;
        while (k > 0 && before(&r->poly[i], &r->poly[g->order[k - 1]], manual)) {
            g->order[k] = g->order[k - 1];
            k--;
        }
        g->order[k] = (uint16_t)i;
    }
    g->frame_on = (m->disp.powcnt & 4) != 0;
    g->next_line = 0;
}

/** @brief Draws line y into the line ring. */
static void draw_line(rl_nds *m, unsigned y)
{
    nds_gx *g = &m->gx;
    nds_gx_lbuf *l = &g->lines[y % 3];
    nds_gx_ram *r = &g->ram[g->geo ^ 1];
    bool wbuf = (g->render_mode & 2) != 0;
    clear_line(m, g, l, y);
    bool masking = false;
    for (uint32_t i = 0; i < g->order_n; i++) {
        const nds_gx_poly *p = &r->poly[g->order[i]];
        bool mask = ((p->attr >> 4) & 3u) == 3 && !((p->attr >> 24) & 0x3Fu);
        if (mask && !masking)
            for (unsigned x = 0; x < 256; x++)
                l->attr[x] &= ~A_STENCIL;
        masking = mask;
        if ((int32_t)y < p->top || ((int32_t)y >= p->bottom && !(p->top == p->bottom && (int32_t)y == p->top)))
            continue;
        raster(m, g, r, p, l, (int32_t)y, wbuf);
    }
}

void nds_gx_line(rl_nds *m, unsigned line, uint64_t when)
{
    nds_gx *g = &m->gx;
    if (line == RENDER_START)
        frame_start(m, when);
    unsigned y = (line + RL_NDS_LINES - RENDER_START) % RL_NDS_LINES;
    if (y > NDS_H || y != (unsigned)g->next_line)
        return;
    g->next_line++;
    if (!g->frame_on) {
        if (y < NDS_H)
            memset(g->out[y], 0, sizeof g->out[y]);
        return;
    }
    if (y < NDS_H)
        draw_line(m, y);
    if (y >= 1) {
        unsigned f = y - 1;
        const nds_gx_lbuf *above = f ? &g->lines[(f - 1) % 3] : NULL, *below = f + 1 < NDS_H ? &g->lines[(f + 1) % 3] : NULL;
        finish_line(g, &g->lines[f % 3], above, below, g->out[f]);
    }
    g->have = true;
}

bool nds_gx_line3d(void *user, unsigned y, uint32_t *out)
{
    rl_nds *m = user;
    nds_gx *g = &m->gx;
    if (!g->have || y >= NDS_H)
        return false;
    memcpy(out, g->out[y], sizeof g->out[y]);
    return true;
}