/**
 * @file gpu3d.h
 * @brief The 3D engine of the ARM9 (gpu3d_fifo.c, gpu3d_geo.c,
 * gpu3d_render.c): the geometry engine behind GXFIFO and the command ports,
 * which turns matrices, vertices and attributes into polygons in polygon and
 * vertex RAM, and the rendering engine, which draws the polygons of the last
 * swap one line at a time into the 3D layer of engine A. Included by
 * nds_int.h.
 */
#ifndef RELIFT_NDS_GPU3D_H
#define RELIFT_NDS_GPU3D_H

#include <relift/sched.h>

#include <stdbool.h>
#include <stdint.h>

/** @brief Entries of the command FIFO (256) and of the pipe in front of it (4). */
#define NDS_GX_FIFO 256u
#define NDS_GX_PIPE 4u
/** @brief Polygons and vertices one polygon and vertex RAM holds. */
#define NDS_GX_POLYS 2048u
#define NDS_GX_VERTS 6144u
/** @brief Most vertices of one polygon after clipping (a quad plus one per clip plane). */
#define NDS_GX_PVERTS 10u

/** @brief One FIFO entry: a command, one of its parameters and the tick it was written. */
typedef struct nds_gx_entry {
    uint32_t param;
    uint8_t cmd;
    uint64_t at;
} nds_gx_entry;

/** @brief A vertex as the geometry engine builds it: clip coordinates and the attributes of the vertex. */
typedef struct nds_gx_cvtx {
    /** @brief x, y, z, w in clip space (12 fraction bits). */
    int32_t c[4];
    /** @brief Red, green and blue in 9 bits (the 6-bit color shifted left by 3). */
    int32_t col[3];
    /** @brief The texture coordinates (4 fraction bits). */
    int32_t s, t;
} nds_gx_cvtx;

/** @brief A vertex in vertex RAM: the clip-space vertex and its screen position. */
typedef struct nds_gx_vtx {
    nds_gx_cvtx v;
    /** @brief Screen x (0 to 511, 9 bits) and y (0 to 255, 8 bits, 0 at the top). */
    int32_t x, y;
    /** @brief The Z-buffer depth (24 bits). */
    int32_t z;
} nds_gx_vtx;

/** @brief A polygon in polygon RAM. */
typedef struct nds_gx_poly {
    /** @brief Its vertices in vertex RAM, in order around the polygon. */
    uint16_t v[NDS_GX_PVERTS];
    uint8_t n;
    /** @brief The polygon attributes, texture parameters and palette base in force at its BEGIN_VTXS. */
    uint32_t attr, tex, pal;
    /** @brief The front side faces the viewer. */
    bool front;
    /** @brief It goes to the translucent list (alpha 1 to 30, or an A3I5 or A5I3 texture). */
    bool transl;
    /** @brief The polygon reaches past the far plane or another plane (any vertex was clipped). */
    bool clipped;
    /** @brief The top and bottom screen lines (bottom excluded unless the polygon is one line high). */
    int32_t top, bottom;
    /** @brief The W values of the vertices after normalizing them to 16 bits. */
    uint16_t wn[NDS_GX_PVERTS];
    /** @brief The sort key of polygon RAM order (see gpu3d_render.c). */
    uint16_t index;
} nds_gx_poly;

/** @brief One set of polygon and vertex RAM. */
typedef struct nds_gx_ram {
    nds_gx_poly poly[NDS_GX_POLYS];
    nds_gx_vtx vtx[NDS_GX_VERTS];
    uint32_t polys, verts;
} nds_gx_ram;

/** @brief The rendering engine's per-pixel buffers of one line (attributes in nds_gx_attr bits). */
typedef struct nds_gx_lbuf {
    /** @brief The topmost and the second color: 18-bit color with alpha (0 to 31) in bits 24 to 28. */
    uint32_t col[2][256];
    uint32_t depth[256];
    uint32_t attr[256];
} nds_gx_lbuf;

/** @brief The state of the 3D engine. */
typedef struct nds_gx {
    /** @brief The queue: pipe entries first, then FIFO entries, from head. */
    nds_gx_entry q[NDS_GX_FIFO + NDS_GX_PIPE];
    uint16_t head, pipe_n, fifo_n;
    /** @brief The packed command word being filled: its commands, the one taking parameters, and parameters still due. */
    uint8_t pk[4];
    uint8_t pk_n, pk_i;
    uint8_t pk_left;
    /** @brief The tick the engine has run to, and the tick it is free from (the end of the running command). */
    uint64_t t, free_at;
    /** @brief The end of the matrix stack command and the test command that run last (GXSTAT bits 14 and 0). */
    uint64_t stack_busy, test_busy;
    /** @brief A SWAP_BUFFERS waits for VBlank: its parameter and the tick of the swap. */
    bool swap_wait;
    uint32_t swap_param;
    uint64_t swap_at;
    /** @brief The tick a store to a full FIFO waits until (the store's master pays it). */
    uint64_t stall_to;
    /** @brief Times IRQ and DMA conditions were checked; the event that rechecks them. */
    rl_event ev;

    /** @brief Matrices: projection, position, vector (directional), texture, and clip (position times projection). */
    int32_t proj[16], pos[16], vec[16], texm[16], clip[16];
    int32_t proj_st[1][16], pos_st[32][16], vec_st[32][16], tex_st[1][16];
    uint8_t proj_sp, pos_sp, tex_sp;
    uint8_t mtx_mode;
    /** @brief GXSTAT bits 1, 15 and 30-31 as the engine keeps them. */
    uint32_t stat;

    /** @brief The vertex being built: coordinates (12 fraction bits), color, texture coordinates. */
    int16_t vx, vy, vz;
    int32_t col[3];
    int16_t raw_s, raw_t;
    int32_t s, t_;
    /** @brief Lights: direction after the vector matrix, half vector, color; the material colors; the shininess table. */
    int32_t light[4][3], half[4][3];
    uint16_t light_col[4];
    uint16_t diffuse, ambient, specular, emission;
    bool shine_on;
    uint8_t shine[128];
    /** @brief POLYGON_ATTR as written and as latched by BEGIN_VTXS; TEXIMAGE_PARAM and PLTT_BASE. */
    uint32_t attr_next, attr, tex, pal;
    /** @brief The vertex list: primitive type, vertices so far, and the vertices a strip carries over. */
    uint8_t prim;
    bool in_list;
    uint32_t list_n;
    nds_gx_cvtx last[4];
    /** @brief Vertex RAM indices of the shared strip vertices when the previous strip polygon was stored unclipped (-1 otherwise). */
    int32_t share[2];
    /** @brief VIEWPORT: x1, y1, x2, y2. */
    uint8_t vp[4];
    /** @brief Results of the tests. */
    int32_t pos_result[4];
    int16_t vec_result[3];
    /** @brief The SWAP_BUFFERS parameter in force for the polygons being built. */
    uint32_t swap_mode;
    /** @brief Geometry and rendering RAM, and which one the geometry engine fills. */
    nds_gx_ram ram[2];
    uint8_t geo;
    /** @brief The SWAP_BUFFERS parameter of the rendering RAM. */
    uint32_t render_mode;
    /** @brief Polygon or vertex RAM ran out since the last swap. */
    bool ram_over;

    /** @brief Rendering registers. */
    uint16_t disp3dcnt;
    uint16_t edge[8];
    uint8_t alpha_ref;
    uint32_t clear_color;
    uint16_t clear_depth, clear_ofs;
    uint32_t fog_color;
    uint16_t fog_ofs;
    uint8_t fog[32];
    uint16_t toon[32];
    uint16_t dot_depth;

    /** @brief The rendering engine: the order of the polygons of the frame, the lines drawn, the finished 3D layer. */
    uint16_t order[NDS_GX_POLYS];
    uint32_t order_n;
    bool frame_on;
    int32_t next_line;
    nds_gx_lbuf lines[3];
    uint32_t out[192][256];
    bool have;
} nds_gx;

#endif
