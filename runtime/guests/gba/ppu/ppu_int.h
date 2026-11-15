/**
 * @file ppu_int.h
 * @brief What the files of the picture unit share.
 */
#ifndef RELIFT_GBA_PPU_INT_H
#define RELIFT_GBA_PPU_INT_H

#include "ppu/ppu.h"

enum { L_OBJ = 0, L_BG0 = 1, L_BG1 = 2, L_BG2 = 3, L_BG3 = 4, L_SFX = 5 };
enum { W_IN0 = 0, W_IN1 = 1, W_IN2 = 2, W_OUT = 3 };
enum { PX_ON = 1, PX_DIRECT = 2, PX_TRANS = 4, PX_MOSAIC = 8, PX_WIN = 16 };
enum { RD_BYTE = 1, RD_HALF = 2 };

static inline uint32_t ppu_h16(const uint8_t *m, uint32_t i) { return (uint32_t)(m[i * 2] | m[i * 2 + 1] << 8); }
static inline int32_t ppu_s28(int32_t v) { return (int32_t)((uint32_t)v << 4) >> 4; }

uint32_t gba_ppu_vram_bg(gba_ppu *p, unsigned size, uint32_t addr);
uint32_t gba_ppu_pram(const gba_ppu *p, unsigned index);
bool gba_ppu_blank(const gba_ppu *p);

void gba_ppu_bg_reset(gba_ppu_bg *b, unsigned id);
void gba_ppu_bg_set_enable(gba_ppu_bg *b, bool status);
void gba_ppu_bg_scanline(gba_ppu *p, gba_ppu_bg *b, unsigned y);
void gba_ppu_bg_run_line(gba_ppu *p, gba_ppu_bg *b);
void gba_ppu_bg_output(gba_ppu *p, gba_ppu_bg *b, unsigned x);

void gba_ppu_obj_reset(gba_ppu *p);
void gba_ppu_obj_set_enable(gba_ppu *p, bool status);
void gba_ppu_obj_render_scanline(gba_ppu *p, unsigned y);
void gba_ppu_obj_output(gba_ppu *p, unsigned x, unsigned y);

void gba_ppu_win_scanline(gba_ppu_window *w, unsigned y);
void gba_ppu_win_run(gba_ppu_window *w, unsigned x);
void gba_ppu_dac_upper(gba_ppu *p, unsigned x);
void gba_ppu_dac_lower(gba_ppu *p, unsigned x);

#endif
