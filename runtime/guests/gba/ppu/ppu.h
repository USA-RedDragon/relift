/**
 * @file ppu.h
 * @brief The picture unit: registers, one scanline at a time into a BGR555
 * frame. It knows no scheduler and no machine: it reads palette, video RAM
 * and OAM through pointers, and the owner calls gba_ppu_line_start at the
 * start of each of the 228 lines and gba_ppu_render at the line's render
 * dot.
 */
#ifndef RELIFT_GBA_PPU_H
#define RELIFT_GBA_PPU_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Width of the picture in pixels. */
#define GBA_PPU_W 240u
/** @brief Height of the picture in lines. */
#define GBA_PPU_H 160u
/** @brief Lines per frame, with the blanking lines. */
#define GBA_PPU_LINES 228u

/** @brief A pixel of a layer before the final merge. */
typedef struct gba_pixel {
    uint16_t color;
    uint8_t priority;
    uint8_t flags;
} gba_pixel;

/** @brief One background layer. */
typedef struct gba_ppu_bg {
    unsigned id;
    uint8_t enable[4];
    uint8_t priority, char_base, unused, mosaic, color_mode, screen_base, wrap, screen_size;
    uint16_t hoff, voff;
    int16_t pa, pb, pc, pd;
    int32_t x, y, lx, ly;
    struct {
        uint16_t character;
        uint8_t hflip, vflip, palette;
        uint16_t data;
        uint8_t active, px;
    } latch;
    struct {
        uint32_t screen_size, screen_wrap, hmosaic, vmosaic, cx, cy, tx, ty;
        uint8_t character;
    } affine;
    gba_pixel output[GBA_PPU_W];
    gba_pixel mosaic_latch;
    unsigned mosaic_offset, vmosaic;
    int32_t fx, fy;
} gba_ppu_bg;

/** @brief The sprite layer. */
typedef struct gba_ppu_obj_latch {
    uint8_t affine, affine_size, mode, mosaic, colors;
    uint16_t x;
    uint8_t affine_param, hflip, vflip;
    uint16_t character;
    uint8_t priority, palette;
    uint32_t width, height;
    uint16_t px;
    uint8_t py;
    int16_t pa, pb, pc, pd;
} gba_ppu_obj_latch;

/** @brief One window. */
typedef struct gba_ppu_window {
    uint8_t enable;
    uint8_t active[6];
    uint8_t x1, x2, y1, y2;
    uint8_t output[256];
    uint8_t h, v;
} gba_ppu_window;

/** @brief The picture unit. */
typedef struct gba_ppu {
    const uint8_t *pram, *vram, *oam;

    uint8_t mode, frame, bg_mosaic_w, bg_mosaic_h;
    uint8_t gbc_mode, force_blank[4], green_swap;
    bool stopped;

    gba_ppu_bg bg[4];

    struct {
        uint8_t enable[4];
        uint8_t hblank, mapping, mosaic_w, mosaic_h;
        gba_ppu_obj_latch latch[2];
        gba_pixel line_buffers[2][GBA_PPU_W];
        gba_pixel output, mosaic_latch;
        unsigned render_y;
        int32_t mosaic_y;
        uint8_t hmosaic_offset, vmosaic_offset;
        uint8_t obj_index;
        bool active, active_cycle, vram_stage_ready, vram_stage_active;
        unsigned state;
    } obj;

    gba_ppu_window win[4];

    struct {
        uint8_t blend_mode;
        uint8_t blend_above[6], blend_below[6];
        uint8_t eva, evb, evy;
        unsigned above_layer, below_layer;
        uint16_t color;
        bool blending;
        gba_pixel layers[6];
    } dac;

    uint16_t *line;
    uint16_t pixels[GBA_PPU_W * GBA_PPU_H];
    uint64_t frames;
} gba_ppu;

/** @brief Resets the unit to power on over the three memories. */
void gba_ppu_reset(gba_ppu *p, const uint8_t *pram, const uint8_t *vram, const uint8_t *oam);
/** @brief The value of the byte at I/O address addr (0x04000000 to 0x04000055), or -1 when the register reads open bus. */
int gba_ppu_read(const gba_ppu *p, uint32_t addr);
/** @brief Writes one byte of the I/O registers at addr (0x04000000 to 0x04000055). */
void gba_ppu_write(gba_ppu *p, uint32_t addr, uint8_t v);
/** @brief The start of line y: pipeline of the enable bits, affine reference points, mosaic rows. */
void gba_ppu_line_start(gba_ppu *p, unsigned y);
/** @brief Draws line y (below 160) into the frame and prepares the sprites of line y + 1. */
void gba_ppu_render(gba_ppu *p, unsigned y);
/** @brief FNV-1a 64 of the frame's pixels as little-endian halfwords. */
uint64_t gba_ppu_hash(const gba_ppu *p);

#endif
