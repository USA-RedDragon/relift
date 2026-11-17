/**
 * @file gpu3d_geo.c
 * @brief The geometry engine's commands: the four matrices and their stacks,
 * vertices through the clip matrix, colors from COLOR, DIF_AMB or the
 * lighting of NORMAL, texture coordinates with the texture matrix's three
 * transform modes, vertex lists of triangles, quads and their strips,
 * culling by facing, clipping against the six planes of the view volume, the
 * viewport transform, polygon and vertex RAM with strip vertex sharing and
 * the overflow flag, and BOX_TEST, POS_TEST and VEC_TEST.
 *
 * Fresh C from GBATEK ("DS 3D Matrix Load/Multiply", "DS 3D Matrix Stack",
 * "DS 3D Polygon Definitions by Vertices", "DS 3D Polygon Light Parameters",
 * "DS 3D Texture Coordinates", "DS 3D Tests"). All arithmetic is fixed
 * point: matrices with 12 fraction bits, sums of products kept whole and
 * shifted at the end.
 */
#include "nds_int.h"

#include <string.h>

enum {
    MODE_PROJ = 0,
    MODE_POS = 1,
    MODE_BOTH = 2,
    MODE_TEX = 3,
    ONE = 1 << 12,
};

static int32_t sext(uint32_t v, unsigned bits) { return (int32_t)(v << (32 - bits)) >> (32 - bits); }

static void identity(int32_t *m)
{
    memset(m, 0, 16 * sizeof *m);
    m[0] = m[5] = m[10] = m[15] = ONE;
}

/** @brief c = a * c (row vectors times matrices: a's rows combine c's rows). */
static void mul(int32_t *c, const int32_t *a)
{
    int32_t r[16];
    for (unsigned i = 0; i < 4; i++)
        for (unsigned j = 0; j < 4; j++) {
            int64_t s = 0;
            for (unsigned k = 0; k < 4; k++)
                s += (int64_t)a[i * 4 + k] * c[k * 4 + j];
            r[i * 4 + j] = (int32_t)(s >> 12);
        }
    memcpy(c, r, sizeof r);
}

static void update_clip(nds_gx *g)
{
    memcpy(g->clip, g->proj, sizeof g->clip);
    mul(g->clip, g->pos);
}

/** @brief The matrices the mode's loads and multiplies change (the vector matrix too in mode 2). */
static void apply(nds_gx *g, const int32_t *a, bool load)
{
    int32_t *t[2] = {NULL, NULL};
    switch (g->mtx_mode) {
    case MODE_PROJ:
        t[0] = g->proj;
        break;
    case MODE_POS:
        t[0] = g->pos;
        break;
    case MODE_BOTH:
        t[0] = g->pos;
        t[1] = g->vec;
        break;
    default:
        t[0] = g->texm;
        break;
    }
    for (unsigned i = 0; i < 2; i++) {
        if (!t[i])
            continue;
        if (load)
            memcpy(t[i], a, 16 * sizeof *a);
        else
            mul(t[i], a);
    }
    if (g->mtx_mode != MODE_TEX)
        update_clip(g);
}

/** @brief Builds the 4x4 matrix of a load or multiply of kind rows x cols (4x4, 4x3, 3x3) from p. */
static void expand(int32_t *a, const uint32_t *p, unsigned rows, unsigned cols)
{
    identity(a);
    for (unsigned i = 0; i < rows; i++)
        for (unsigned j = 0; j < cols; j++)
            a[i * 4 + j] = (int32_t)p[i * cols + j];
}

static void stack_op(nds_gx *g, uint8_t cmd, uint32_t p)
{
    bool pos = g->mtx_mode == MODE_POS || g->mtx_mode == MODE_BOTH;
    if (pos) {
        unsigned i;
        switch (cmd) {
        case 0x11:
            i = g->pos_sp;
            if (i >= 31)
                g->stat |= 0x8000;
            memcpy(g->pos_st[i & 31], g->pos, sizeof g->pos);
            memcpy(g->vec_st[i & 31], g->vec, sizeof g->vec);
            g->pos_sp = (uint8_t)((g->pos_sp + 1) & 63);
            return;
        case 0x12:
            g->pos_sp = (uint8_t)((g->pos_sp - sext(p, 6)) & 63);
            i = g->pos_sp;
            break;
        case 0x13:
            i = p & 31;
            if (i == 31)
                g->stat |= 0x8000;
            memcpy(g->pos_st[i], g->pos, sizeof g->pos);
            memcpy(g->vec_st[i], g->vec, sizeof g->vec);
            return;
        default:
            i = p & 31;
            break;
        }
        if (i >= 31)
            g->stat |= 0x8000;
        memcpy(g->pos, g->pos_st[i & 31], sizeof g->pos);
        memcpy(g->vec, g->vec_st[i & 31], sizeof g->vec);
        update_clip(g);
        return;
    }
    bool proj = g->mtx_mode == MODE_PROJ;
    int32_t *cur = proj ? g->proj : g->texm, *st = proj ? g->proj_st[0] : g->tex_st[0];
    uint8_t *sp = proj ? &g->proj_sp : &g->tex_sp;
    switch (cmd) {
    case 0x11:
        if (*sp & 1)
            g->stat |= 0x8000;
        memcpy(st, cur, 16 * sizeof *cur);
        *sp = (uint8_t)((*sp + 1) & 1);
        return;
    case 0x12:
        *sp = (uint8_t)((*sp - 1) & 1);
        if (*sp & 1)
            g->stat |= 0x8000;
        memcpy(cur, st, 16 * sizeof *cur);
        break;
    case 0x13:
        memcpy(st, cur, 16 * sizeof *cur);
        return;
    default:
        memcpy(cur, st, 16 * sizeof *cur);
        break;
    }
    if (proj)
        update_clip(g);
}

/** @brief A 5-bit channel with 4 fraction bits, as the engine interpolates it (all fraction bits set but for 0). */
static int32_t c9(unsigned c5) { return c5 ? (int32_t)(c5 << 4 | 15) : 0; }

static void set_color(nds_gx *g, uint32_t rgb)
{
    for (unsigned k = 0; k < 3; k++)
        g->col[k] = c9((rgb >> (5 * k)) & 31u);
}

/** @brief Texture coordinates of a normal or vertex source (mode 2 or 3): v has frac fraction bits. */
static void tex_from(nds_gx *g, const int32_t *v, unsigned shift)
{
    int64_t s = (int64_t)v[0] * g->texm[0] + (int64_t)v[1] * g->texm[4] + (int64_t)v[2] * g->texm[8];
    int64_t t = (int64_t)v[0] * g->texm[1] + (int64_t)v[1] * g->texm[5] + (int64_t)v[2] * g->texm[9];
    g->s = (int16_t)(int32_t)((s >> shift) + g->raw_s);
    g->t_ = (int16_t)(int32_t)((t >> shift) + g->raw_t);
}

static void texcoord(nds_gx *g, uint32_t p)
{
    g->raw_s = (int16_t)p;
    g->raw_t = (int16_t)(p >> 16);
    if ((g->tex >> 30) == 1) {
        int64_t s = (int64_t)g->raw_s * g->texm[0] + (int64_t)g->raw_t * g->texm[4] + g->texm[8] + g->texm[12];
        int64_t t = (int64_t)g->raw_s * g->texm[1] + (int64_t)g->raw_t * g->texm[5] + g->texm[9] + g->texm[13];
        g->s = (int16_t)(int32_t)(s >> 12);
        g->t_ = (int16_t)(int32_t)(t >> 12);
    } else {
        g->s = g->raw_s;
        g->t_ = g->raw_t;
    }
}

/** @brief A 10-bit vector (9 fraction bits) through the vector matrix, keeping 9 fraction bits. */
static void by_vec(const nds_gx *g, uint32_t p, int32_t *out)
{
    int32_t v[3] = {sext(p, 10), sext(p >> 10, 10), sext(p >> 20, 10)};
    for (unsigned j = 0; j < 3; j++)
        out[j] = (int32_t)(((int64_t)v[0] * g->vec[j] + (int64_t)v[1] * g->vec[4 + j] + (int64_t)v[2] * g->vec[8 + j]) >> 12);
}

static int32_t dot3(const int32_t *a, const int32_t *b)
{
    return (int32_t)(((int64_t)a[0] * b[0] + (int64_t)a[1] * b[1] + (int64_t)a[2] * b[2]) >> 9);
}

static unsigned normal(nds_gx *g, uint32_t p)
{
    if ((g->tex >> 30) == 2) {
        int32_t raw[3] = {sext(p, 10), sext(p >> 10, 10), sext(p >> 20, 10)};
        tex_from(g, raw, 21);
    }
    int32_t n[3];
    by_vec(g, p, n);
    int32_t c[3];
    unsigned lights = 0;
    for (unsigned k = 0; k < 3; k++)
        c[k] = (int32_t)((g->emission >> (5 * k)) & 31u);
    for (unsigned i = 0; i < 4; i++) {
        if (!(g->attr >> i & 1))
            continue;
        lights++;
        int32_t dif = -dot3(g->light[i], n);
        if (dif < 0)
            dif = 0;
        int32_t sh = -dot3(g->half[i], n);
        if (sh < 0)
            sh = 0;
        sh = (int32_t)(((int64_t)sh * sh) >> 9);
        if (g->shine_on) {
            unsigned idx = (unsigned)sh >> 2;
            sh = (int32_t)g->shine[idx > 127 ? 127 : idx] << 1;
        }
        for (unsigned k = 0; k < 3; k++) {
            int32_t lc = (int32_t)((g->light_col[i] >> (5 * k)) & 31u);
            int32_t d = (int32_t)((g->diffuse >> (5 * k)) & 31u), a = (int32_t)((g->ambient >> (5 * k)) & 31u);
            int32_t s = (int32_t)((g->specular >> (5 * k)) & 31u);
            c[k] += (int32_t)(((int64_t)s * lc * sh) >> 14) + (int32_t)(((int64_t)d * lc * dif) >> 14) + ((a * lc) >> 5);
        }
    }
    for (unsigned k = 0; k < 3; k++)
        g->col[k] = c9((unsigned)(c[k] > 31 ? 31 : c[k]));
    return lights > 1 ? 9 + lights - 1 : 9;
}

/** @brief The clip-space vertex of (x, y, z) and the current color and texture coordinates. */
static nds_gx_cvtx transform(const nds_gx *g, int32_t x, int32_t y, int32_t z)
{
    nds_gx_cvtx v;
    for (unsigned j = 0; j < 4; j++)
        v.c[j] = (int32_t)(((int64_t)x * g->clip[j] + (int64_t)y * g->clip[4 + j] + (int64_t)z * g->clip[8 + j] +
                            (int64_t)ONE * g->clip[12 + j]) >>
                           12);
    memcpy(v.col, g->col, sizeof v.col);
    v.s = g->s;
    v.t = g->t_;
    return v;
}

/** @brief True when the vertex is inside the plane: k is the axis, hi the upper side. */
static bool inside(const nds_gx_cvtx *v, unsigned k, bool hi)
{
    return hi ? v->c[k] <= v->c[3] : v->c[k] >= -v->c[3];
}

/** @brief The vertex where the edge from a (inside) to b (outside) meets the plane. */
/** @brief The ceiling of num / den for den > 0. */
static int64_t ceil_div(int64_t num, int64_t den) { return num >= 0 ? (num + den - 1) / den : -(-num / den); }

/**
 * @brief The vertex where the edge from a (inside) to b (outside) meets the
 * plane. Coordinates and texture coordinates are interpolated exactly; the
 * colors are interpolated as 5-bit values and rounded up (the oracle's clipped
 * colors), then widened like a COLOR command's.
 */
static nds_gx_cvtx cut(const nds_gx_cvtx *a, const nds_gx_cvtx *b, unsigned k, bool hi)
{
    int64_t da = hi ? (int64_t)a->c[3] - a->c[k] : (int64_t)a->c[3] + a->c[k];
    int64_t db = hi ? (int64_t)b->c[3] - b->c[k] : (int64_t)b->c[3] + b->c[k];
    int64_t den = da - db;
    nds_gx_cvtx r;
    for (unsigned j = 0; j < 4; j++)
        r.c[j] = (int32_t)(a->c[j] + ((int64_t)(b->c[j] - a->c[j]) * da) / den);
    r.c[k] = hi ? r.c[3] : -r.c[3];
    for (unsigned j = 0; j < 3; j++) {
        int64_t ca = a->col[j] >> 4, cb = b->col[j] >> 4;
        r.col[j] = c9((unsigned)ceil_div(ca * den + (cb - ca) * da, den));
    }
    r.s = (int32_t)(a->s + ((int64_t)(b->s - a->s) * da) / den);
    r.t = (int32_t)(a->t + ((int64_t)(b->t - a->t) * da) / den);
    return r;
}

/** @brief Clips the polygon in v (n vertices) against one plane in place; returns the new count. */
static unsigned clip_plane(nds_gx_cvtx *v, unsigned n, unsigned k, bool hi)
{
    nds_gx_cvtx out[NDS_GX_PVERTS];
    unsigned m = 0;
    for (unsigned i = 0; i < n; i++) {
        const nds_gx_cvtx *a = &v[i], *b = &v[(i + 1) % n];
        bool ia = inside(a, k, hi), ib = inside(b, k, hi);
        if (ia)
            out[m++] = *a;
        if (ia != ib && m < NDS_GX_PVERTS)
            out[m++] = ia ? cut(a, b, k, hi) : cut(b, a, k, hi);
    }
    memcpy(v, out, m * sizeof *v);
    return m;
}

/**
 * @brief Clips against the view volume; returns the new count (0 when nothing is left). far_ok is POLYGON_ATTR
 * bit 12: without it a polygon that crosses the far plane is dropped. *changed is set when any vertex moved.
 */
static unsigned clip(nds_gx_cvtx *v, unsigned n, bool far_ok, bool *changed)
{
    *changed = false;
    for (unsigned i = 0; i < n; i++)
        if (v[i].c[2] > v[i].c[3]) {
            if (!far_ok)
                return 0;
            *changed = true;
        }
    static const struct {
        uint8_t k;
        bool hi;
    } planes[6] = {{2, false}, {2, true}, {0, true}, {0, false}, {1, true}, {1, false}};
    for (unsigned p = 0; p < 6 && n; p++) {
        bool all = true;
        for (unsigned i = 0; i < n; i++)
            all &= inside(&v[i], planes[p].k, planes[p].hi);
        if (all)
            continue;
        *changed = true;
        n = clip_plane(v, n, planes[p].k, planes[p].hi);
    }
    return n;
}

/** @brief The sign of the polygon's facing from its first three vertices: >0 front, <0 back, 0 for a line or a point. */
static int facing(const nds_gx_cvtx *v)
{
    int64_t a[3], b[3], o[3];
    int32_t big = 0;
    for (unsigned i = 0; i < 3; i++)
        for (unsigned j = 0; j < 4; j++) {
            int32_t x = v[i].c[j] < 0 ? -v[i].c[j] : v[i].c[j];
            big = x > big ? x : big;
        }
    unsigned sh = 0;
    while ((big >> sh) >= (1 << 20))
        sh++;
    static const unsigned ax[3] = {0, 1, 3};
    for (unsigned j = 0; j < 3; j++) {
        o[j] = v[0].c[ax[j]] >> sh;
        a[j] = (v[1].c[ax[j]] >> sh) - o[j];
        b[j] = (v[2].c[ax[j]] >> sh) - o[j];
    }
    int64_t nx = a[1] * b[2] - a[2] * b[1], ny = a[2] * b[0] - a[0] * b[2], nw = a[0] * b[1] - a[1] * b[0];
    int64_t d = nx * o[0] + ny * o[1] + nw * o[2];
    return d > 0 ? 1 : d < 0 ? -1 : 0;
}

/** @brief The screen position and depth of a clip-space vertex: GBATEK's viewport formula, y measured down from the top (Y2). */
static void viewport(const nds_gx *g, nds_gx_vtx *out, uint32_t mode)
{
    int64_t x = out->v.c[0], y = out->v.c[1], z = out->v.c[2], w = out->v.c[3];
    if (w == 0)
        w = 1;
    int64_t vw = (int64_t)g->vp[2] - g->vp[0] + 1, vh = (int64_t)g->vp[3] - g->vp[1] + 1;
    out->x = (int32_t)((((x + w) * vw) / (2 * w) + g->vp[0]) & 0x1FF);
    out->y = (int32_t)((((w - y) * vh) / (2 * w) + 191 - g->vp[3]) & 0xFF);
    if (mode & 2) {
        out->z = (int32_t)w;
    } else {
        int64_t d = ((z * 0x4000) / w + 0x3FFF) * 0x200;
        out->z = (int32_t)(d < 0 ? 0 : d > 0xFFFFFF ? 0xFFFFFF : d);
    }
}

/** @brief Stores a finished polygon of n vertices (in order around it) in geometry RAM; strip is its position in a strip (-1 for none). */
static void polygon(nds_gx *g, nds_gx_cvtx *v, unsigned n, int strip)
{
    nds_gx_ram *r = &g->ram[g->geo];
    uint32_t attr = g->attr;
    int f = facing(v);
    if (n == 4 && f == 0)
        f = facing(v + 1);
    bool front = f >= 0;
    if (f > 0 && !(attr & 0x80))
        goto culled;
    if (f < 0 && !(attr & 0x40))
        goto culled;
    bool changed;
    unsigned k = clip(v, n, (attr >> 12) & 1, &changed);
    if (k == 0)
        goto culled;
    if (r->polys >= NDS_GX_POLYS) {
        g->ram_over = true;
        goto culled;
    }
    unsigned shared = (strip > 0 && !changed && g->share[0] >= 0) ? 2 : 0;
    if (r->verts + k - shared > NDS_GX_VERTS) {
        g->ram_over = true;
        goto culled;
    }
    nds_gx_poly *p = &r->poly[r->polys];
    memset(p, 0, sizeof *p);
    bool same = true;
    for (unsigned i = 0; i < k; i++) {
        int sh = -1;
        if (shared && i < 2)
            sh = (g->prim == 2 && (strip & 1)) ? (int)(1 - i) : (int)i;
        if (sh >= 0) {
            p->v[i] = (uint16_t)g->share[sh];
        } else {
            nds_gx_vtx *o = &r->vtx[r->verts];
            o->v = v[i];
            viewport(g, o, g->swap_mode);
            p->v[i] = (uint16_t)r->verts++;
        }
        const nds_gx_vtx *o = &r->vtx[p->v[i]];
        const nds_gx_vtx *o0 = &r->vtx[p->v[0]];
        same &= o->x == o0->x && o->y == o0->y;
    }
    p->n = (uint8_t)k;
    if (same && !(attr & 0x2000)) {
        bool near = false;
        for (unsigned i = 0; i < k; i++)
            near |= r->vtx[p->v[i]].v.c[3] <= ((int32_t)g->dot_depth << 9);
        if (!near) {
            r->verts -= k - shared;
            goto culled;
        }
    }
    p->attr = attr;
    p->tex = g->tex;
    p->pal = g->pal;
    p->front = front;
    p->clipped = changed;
    unsigned alpha = (attr >> 16) & 31u, fmt = (g->tex >> 26) & 7u;
    p->transl = (alpha > 0 && alpha < 31) || fmt == 1 || fmt == 6;
    p->index = (uint16_t)r->polys;
    r->polys++;
    if (strip >= 0 && !changed) {
        if (g->prim == 2) {
            g->share[0] = p->v[(strip & 1) ? 0 : 1];
            g->share[1] = p->v[2];
        } else {
            g->share[0] = p->v[3];
            g->share[1] = p->v[2];
        }
    } else {
        g->share[0] = g->share[1] = -1;
    }
    return;
culled:
    g->share[0] = g->share[1] = -1;
}

/** @brief A vertex command's vertex: adds it to the list and finishes a polygon when the list has one. */
static void vertex(nds_gx *g)
{
    if ((g->tex >> 30) == 3) {
        int32_t v[3] = {g->vx, g->vy, g->vz};
        tex_from(g, v, 24);
    }
    nds_gx_cvtx v = transform(g, g->vx, g->vy, g->vz);
    if (!g->in_list)
        return;
    uint32_t n = g->list_n++;
    nds_gx_cvtx poly[NDS_GX_PVERTS];
    switch (g->prim) {
    case 0:
        g->last[n % 3] = v;
        if (n % 3 == 2) {
            memcpy(poly, g->last, 3 * sizeof *poly);
            polygon(g, poly, 3, -1);
        }
        return;
    case 1:
        g->last[n % 4] = v;
        if (n % 4 == 3) {
            memcpy(poly, g->last, 4 * sizeof *poly);
            polygon(g, poly, 4, -1);
        }
        return;
    case 2:
        if (n < 2) {
            g->last[n] = v;
            return;
        }
        if ((n - 2) & 1) {
            poly[0] = g->last[1];
            poly[1] = g->last[0];
        } else {
            poly[0] = g->last[0];
            poly[1] = g->last[1];
        }
        poly[2] = v;
        polygon(g, poly, 3, (int)(n - 2));
        g->last[0] = g->last[1];
        g->last[1] = v;
        return;
    default:
        if (n < 2) {
            g->last[n] = v;
            return;
        }
        if (n & 1) {
            poly[0] = g->last[0];
            poly[1] = g->last[1];
            poly[2] = v;
            poly[3] = g->last[2];
            polygon(g, poly, 4, (int)(n - 3) / 2);
            g->last[0] = g->last[2];
            g->last[1] = v;
        } else {
            g->last[2] = v;
        }
        return;
    }
}

/** @brief True when some face of the box is inside the view volume. */
static bool box_test(const nds_gx *g, const uint32_t *p)
{
    int32_t x = (int16_t)p[0], y = (int16_t)(p[0] >> 16), z = (int16_t)p[1];
    int32_t w = (int16_t)(p[1] >> 16), h = (int16_t)p[2], d = (int16_t)(p[2] >> 16);
    nds_gx_cvtx c[8];
    for (unsigned i = 0; i < 8; i++)
        c[i] = transform(g, (int16_t)(x + ((i & 1) ? w : 0)), (int16_t)(y + ((i & 2) ? h : 0)), (int16_t)(z + ((i & 4) ? d : 0)));
    static const uint8_t faces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4}, {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    for (unsigned f = 0; f < 6; f++) {
        nds_gx_cvtx v[NDS_GX_PVERTS];
        for (unsigned i = 0; i < 4; i++)
            v[i] = c[faces[f][i]];
        bool changed;
        if (clip(v, 4, true, &changed))
            return true;
    }
    return false;
}

unsigned nds_gx_exec(rl_nds *m, uint8_t cmd, const uint32_t *p, uint64_t start)
{
    nds_gx *g = &m->gx;
    unsigned cycles = nds_gx_cycles(cmd);
    int32_t a[16];
    switch (cmd) {
    case 0x10:
        g->mtx_mode = p[0] & 3;
        break;
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x14:
        stack_op(g, cmd, p[0]);
        g->stack_busy = start + 2u * cycles;
        break;
    case 0x15:
        identity(a);
        apply(g, a, true);
        break;
    case 0x16:
        expand(a, p, 4, 4);
        apply(g, a, true);
        break;
    case 0x17:
        expand(a, p, 4, 3);
        apply(g, a, true);
        break;
    case 0x18:
    case 0x19:
    case 0x1A:
        expand(a, p, cmd == 0x1A ? 3 : 4, cmd == 0x18 ? 4 : 3);
        apply(g, a, false);
        if (g->mtx_mode == MODE_BOTH)
            cycles += 30;
        break;
    case 0x1B: {
        identity(a);
        a[0] = (int32_t)p[0];
        a[5] = (int32_t)p[1];
        a[10] = (int32_t)p[2];
        uint8_t mode = g->mtx_mode;
        if (mode == MODE_BOTH)
            g->mtx_mode = MODE_POS;
        apply(g, a, false);
        g->mtx_mode = mode;
        break;
    }
    case 0x1C:
        identity(a);
        a[12] = (int32_t)p[0];
        a[13] = (int32_t)p[1];
        a[14] = (int32_t)p[2];
        apply(g, a, false);
        if (g->mtx_mode == MODE_BOTH)
            cycles += 30;
        break;
    case 0x20:
        set_color(g, p[0]);
        break;
    case 0x21:
        cycles = normal(g, p[0]);
        break;
    case 0x22:
        texcoord(g, p[0]);
        break;
    case 0x23:
        g->vx = (int16_t)p[0];
        g->vy = (int16_t)(p[0] >> 16);
        g->vz = (int16_t)p[1];
        vertex(g);
        break;
    case 0x24:
        g->vx = (int16_t)(sext(p[0], 10) << 6);
        g->vy = (int16_t)(sext(p[0] >> 10, 10) << 6);
        g->vz = (int16_t)(sext(p[0] >> 20, 10) << 6);
        vertex(g);
        break;
    case 0x25:
        g->vx = (int16_t)p[0];
        g->vy = (int16_t)(p[0] >> 16);
        vertex(g);
        break;
    case 0x26:
        g->vx = (int16_t)p[0];
        g->vz = (int16_t)(p[0] >> 16);
        vertex(g);
        break;
    case 0x27:
        g->vy = (int16_t)p[0];
        g->vz = (int16_t)(p[0] >> 16);
        vertex(g);
        break;
    case 0x28:
        g->vx = (int16_t)(g->vx + sext(p[0], 10));
        g->vy = (int16_t)(g->vy + sext(p[0] >> 10, 10));
        g->vz = (int16_t)(g->vz + sext(p[0] >> 20, 10));
        vertex(g);
        break;
    case 0x29:
        g->attr_next = p[0];
        break;
    case 0x2A:
        g->tex = p[0];
        break;
    case 0x2B:
        g->pal = p[0] & 0x1FFF;
        break;
    case 0x30:
        g->diffuse = (uint16_t)(p[0] & 0x7FFF);
        g->ambient = (uint16_t)((p[0] >> 16) & 0x7FFF);
        if (p[0] & 0x8000)
            set_color(g, p[0]);
        break;
    case 0x31:
        g->specular = (uint16_t)(p[0] & 0x7FFF);
        g->emission = (uint16_t)((p[0] >> 16) & 0x7FFF);
        g->shine_on = (p[0] >> 15) & 1;
        break;
    case 0x32: {
        unsigned i = p[0] >> 30;
        by_vec(g, p[0], g->light[i]);
        g->half[i][0] = g->light[i][0] >> 1;
        g->half[i][1] = g->light[i][1] >> 1;
        g->half[i][2] = (g->light[i][2] - 0x200) >> 1;
        break;
    }
    case 0x33:
        g->light_col[p[0] >> 30] = (uint16_t)(p[0] & 0x7FFF);
        break;
    case 0x34:
        for (unsigned i = 0; i < 32; i++)
            for (unsigned k = 0; k < 4; k++)
                g->shine[i * 4 + k] = (uint8_t)(p[i] >> (8 * k));
        break;
    case 0x40:
        g->attr = g->attr_next;
        g->prim = p[0] & 3;
        g->in_list = true;
        g->list_n = 0;
        g->share[0] = g->share[1] = -1;
        break;
    case 0x41:
        break;
    case 0x60:
        for (unsigned i = 0; i < 4; i++)
            g->vp[i] = (uint8_t)(p[0] >> (8 * i));
        break;
    case 0x70:
        g->stat = (g->stat & ~2u) | (box_test(g, p) ? 2u : 0);
        g->test_busy = start + 2u * cycles;
        break;
    case 0x71: {
        g->vx = (int16_t)p[0];
        g->vy = (int16_t)(p[0] >> 16);
        g->vz = (int16_t)p[1];
        nds_gx_cvtx v = transform(g, g->vx, g->vy, g->vz);
        memcpy(g->pos_result, v.c, sizeof g->pos_result);
        g->test_busy = start + 2u * cycles;
        break;
    }
    case 0x72: {
        int32_t v[3] = {sext(p[0], 10), sext(p[0] >> 10, 10), sext(p[0] >> 20, 10)};
        for (unsigned j = 0; j < 3; j++) {
            int64_t s = ((int64_t)v[0] * g->vec[j] + (int64_t)v[1] * g->vec[4 + j] + (int64_t)v[2] * g->vec[8 + j]) >> 9;
            g->vec_result[j] = (int16_t)sext((uint32_t)s, 13);
        }
        g->test_busy = start + 2u * cycles;
        break;
    }
    default:
        break;
    }
    return cycles;
}

void nds_gx_geo_reset(rl_nds *m)
{
    nds_gx *g = &m->gx;
    identity(g->proj);
    identity(g->pos);
    identity(g->vec);
    identity(g->texm);
    update_clip(g);
    g->share[0] = g->share[1] = -1;
    g->vp[2] = 255;
    g->vp[3] = 191;
    g->dot_depth = 0x7FFF;
    g->clear_depth = 0x7FFF;
}
