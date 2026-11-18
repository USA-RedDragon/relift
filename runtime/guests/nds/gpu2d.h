/**
 * @file gpu2d.h
 * @brief The two 2D engines of the display (engine A and engine B), one
 * line at a time: the BG layers (text, affine, extended affine, bitmaps and
 * engine A's large bitmap), the OBJ layer, the windows, mosaic, the color
 * effects and master brightness, as 18-bit colors (6 bits per channel).
 * An engine knows no scheduler: it reads palette, OAM and video RAM through
 * the machine's views, and the display device (disp.c) calls it at each
 * line's render tick.
 */
#ifndef RELIFT_NDS_GPU2D_H
#define RELIFT_NDS_GPU2D_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Pixels per line. */
#define NDS_W 256u
/** @brief Lines per screen. */
#define NDS_H 192u

struct rl_nds;

/** @brief One pixel of a layer before the merge: an 18-bit color, its priority and flags. */
typedef struct nds_px {
    /** @brief Red in bits 0 to 5, green 6 to 11, blue 12 to 17. */
    uint32_t c;
    /** @brief Priority 0 (front) to 3; 4 for the backdrop. */
    uint8_t prio;
    /** @brief NDS_PX_* bits. */
    uint8_t flags;
    /** @brief The alpha of a bitmap OBJ (0 to 15) or of a 3D pixel (0 to 31). */
    uint8_t alpha;
} nds_px;

/** @brief Flags of a layer pixel. */
enum {
    /** @brief The pixel is opaque. */
    NDS_PX_ON = 1,
    /** @brief A semi-transparent OBJ pixel. */
    NDS_PX_SEMI = 2,
    /** @brief A bitmap OBJ pixel (blends with its own alpha). */
    NDS_PX_BITMAP = 4,
    /** @brief A pixel of the 3D layer (blends with its own alpha). */
    NDS_PX_3D = 8,
    /** @brief An OBJ pixel with mosaic on. */
    NDS_PX_MOSAIC = 16,
};

/** @brief One engine's registers and line state. */
typedef struct nds_eng {
    /** @brief 0 for engine A, 1 for engine B. */
    unsigned id;
    uint32_t dispcnt;
    uint16_t bgcnt[4], hofs[4], vofs[4];
    /** @brief BG2 and BG3 rotation and scaling: PA, PB, PC, PD. */
    int16_t pa[2], pb[2], pc[2], pd[2];
    /** @brief BG2 and BG3 reference points as written (28-bit signed, 8 fraction bits). */
    int32_t refx[2], refy[2];
    /** @brief The internal reference points of the line being drawn. */
    int32_t curx[2], cury[2];
    uint16_t winh[2], winv[2], winin, winout, mosaic, bldcnt, bldalpha, bldy, bright;
    /** @brief The registers as written, by byte (offsets 0x00 to 0x6F). */
    uint8_t raw[0x70];
    /** @brief The vertical and horizontal window flags of WIN0 and WIN1 (the horizontal one carries over from line to line). */
    bool winy[2], winh_on[2];
    /** @brief The rows of the BG and OBJ mosaics: counters and the line they show. */
    unsigned bg_mos_cnt, bg_mos_y, obj_mos_cnt, obj_mos_y;
    /** @brief The internal reference points at the BG mosaic's row. */
    int32_t mosx[2], mosy[2];

    /** @brief The layers of the line: BG0 to BG3, then OBJ. */
    nds_px bg[4][NDS_W];
    nds_px obj[NDS_W];
    /** @brief The OBJ window of the line. */
    uint8_t objwin[NDS_W];
    /** @brief The line after the merge and the effects, before master brightness. */
    uint32_t out[NDS_W];
} nds_eng;

/** @brief Power-on state of engine id. */
void nds_eng_reset(nds_eng *e, unsigned id);
/** @brief A register read of the word at offset off (0x00 to 0x6C from the engine's base). */
uint32_t nds_eng_read(const nds_eng *e, uint32_t off);
/** @brief A write of the mask bits of v to the word at offset off. */
void nds_eng_write(nds_eng *e, uint32_t off, uint32_t v, uint32_t mask);
/** @brief The start of the frame: the affine reference points reload, the mosaic rows restart. */
void nds_eng_frame(nds_eng *e);
/**
 * @brief Draws line y (0 to 191) into e->out: layers, windows, effects; line3d
 * is engine A's 3D layer (256 pixels, NULL when it has none).
 */
void nds_eng_line(struct rl_nds *m, nds_eng *e, unsigned y, const uint32_t *line3d);
/** @brief The line's end: the affine reference points advance by PB and PD, the vertical windows and mosaics step. */
void nds_eng_line_end(nds_eng *e, unsigned y);
/** @brief Master brightness of the engine on an 18-bit color. */
uint32_t nds_eng_bright(const nds_eng *e, uint32_t c);
/** @brief Master brightness over the n colors of line in place, as nds_eng_bright on each. */
void nds_eng_bright_line(const nds_eng *e, uint32_t *line, unsigned n);

/**
 * @brief The 18-bit color of a 16-bit one: each 5-bit channel doubled, and
 * bit 15 as green's low bit (as the oracle shows it).
 */
static inline uint32_t nds_c18(uint32_t c)
{
    return (c & 0x1Fu) << 1 | (c & 0x3E0u) << 2 | (c & 0x8000u) >> 9 | (c & 0x7C00u) << 3;
}

/** @brief Draws the BG layer n of line y into e->bg[n] (gpu2d_bg.c). */
void nds_eng_bg(struct rl_nds *m, nds_eng *e, unsigned n, unsigned y, const uint32_t *line3d);
/** @brief Draws the OBJ layer and the OBJ window of line y (gpu2d_obj.c). */
void nds_eng_obj(struct rl_nds *m, nds_eng *e, unsigned y);

#endif
