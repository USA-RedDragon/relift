/**
 * @file touchpad_int.h
 * @brief Inside the on-screen controller: the canvas the overlay is drawn on
 * (premultiplied RGBA8, antialiased shapes from distance functions) and the
 * stroke font.
 */
#ifndef RELIFT_TOUCHPAD_INT_H
#define RELIFT_TOUCHPAD_INT_H

#include <stdbool.h>
#include <stdint.h>

/** @brief One glyph of the stroke font: its points in rl_tp_font_points and its advance (grid units). */
typedef struct rl_tp_glyph {
    uint16_t first;
    uint16_t count;
    uint8_t advance;
} rl_tp_glyph;

/** @brief Points of every glyph; x -128 ends a polyline. */
extern const int8_t rl_tp_font_points[][2];
/** @brief Glyphs of ASCII 32 to 126. */
extern const rl_tp_glyph rl_tp_font_glyphs[95];

/** @brief A straight (not premultiplied) color, 0 to 1. */
typedef struct rl_tp_color {
    float r, g, b, a;
} rl_tp_color;

/** @brief A canvas and the rectangle drawn on since it was last cleared. */
typedef struct rl_tp_canvas {
    uint8_t *px;
    uint32_t w, h;
    int32_t x0, y0, x1, y1;
} rl_tp_canvas;

/** @brief A color from 0xRRGGBB and an alpha. */
rl_tp_color rl_tp_rgb(uint32_t rgb, float a);

/** @brief Clears the drawn rectangle and resets it. */
void rl_tp_clear(rl_tp_canvas *c);

/** @brief A circle: fill, and a stroke of width sw centered on the edge; opacity scales both. */
void rl_tp_circle(rl_tp_canvas *c, float cx, float cy, float r, rl_tp_color fill, rl_tp_color stroke, float sw,
                  float opacity);
/** @brief A rounded rectangle from (x0, y0) to (x1, y1). */
void rl_tp_rrect(rl_tp_canvas *c, float x0, float y0, float x1, float y1, float radius, rl_tp_color fill,
                 rl_tp_color stroke, float sw, float opacity);
/** @brief A d-pad cross of side s and arm width a centered at (cx, cy). */
void rl_tp_cross(rl_tp_canvas *c, float cx, float cy, float s, float a, rl_tp_color fill, rl_tp_color stroke, float sw,
                 float opacity);
/** @brief A filled triangle with an outline of width ow outside it. */
void rl_tp_triangle(rl_tp_canvas *c, const float p[6], rl_tp_color fill, rl_tp_color outline, float ow);
/** @brief A line with round ends. */
void rl_tp_line(rl_tp_canvas *c, float x0, float y0, float x1, float y1, float width, rl_tp_color color);
/** @brief A dashed outline of a rectangle. */
void rl_tp_dashed_rect(rl_tp_canvas *c, float x0, float y0, float x1, float y1, float width, float dash, float gap,
                       rl_tp_color color);

/** @brief Text alignment. */
enum { RL_TP_LEFT = 0, RL_TP_CENTER = 1, RL_TP_RIGHT = 2 };
/** @brief The width of text at a font size (the cap height is 0.72 of it). */
float rl_tp_text_width(const char *text, float size, bool bold);
/** @brief Text whose vertical middle of capitals is y; outline of width ow in its own color (ow 0 for none). */
void rl_tp_text(rl_tp_canvas *c, float x, float y, const char *text, float size, bool bold, int align,
                rl_tp_color color, rl_tp_color outline, float ow);

#endif
