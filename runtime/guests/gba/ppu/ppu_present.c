/**
 * @file ppu_present.c
 * @brief Frames to the host: BGR555 to RGB888 and the window layer's present.
 */
#include "gba.h"
#include "ppu/ppu.h"

#include <relift/window.h>

void rl_gba_frame_rgb(const uint16_t *pixels, uint8_t *rgb)
{
    for (unsigned i = 0; i < GBA_PPU_W * GBA_PPU_H; i++) {
        unsigned c = pixels[i];
        unsigned r = c & 31u, g = (c >> 5) & 31u, b = (c >> 10) & 31u;
        rgb[i * 3 + 0] = (uint8_t)(r << 3 | r >> 2);
        rgb[i * 3 + 1] = (uint8_t)(g << 3 | g >> 2);
        rgb[i * 3 + 2] = (uint8_t)(b << 3 | b >> 2);
    }
}

bool rl_gba_present(rl_window *w, const uint16_t *pixels)
{
    static uint8_t rgb[GBA_PPU_W * GBA_PPU_H * 3];
    rl_gba_frame_rgb(pixels, rgb);
    return rl_window_present_rgb(w, rgb, GBA_PPU_W, GBA_PPU_H);
}
