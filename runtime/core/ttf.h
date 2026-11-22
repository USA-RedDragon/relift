/**
 * @file ttf.h
 * @brief A small TrueType reader and rasterizer for the runtime's own text:
 * glyph outlines (glyf, simple and composite) found through cmap formats 4
 * and 12, drawn with exact area coverage. No hinting, kerning or shaping.
 */
#ifndef RELIFT_CORE_TTF_H
#define RELIFT_CORE_TTF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief A font over bytes the caller keeps alive. */
typedef struct rl_ttf {
    const uint8_t *d;
    size_t n;
    uint32_t glyf, loca, hmtx, cmap;
    uint16_t cmap_format;
    bool loca_long;
    uint16_t upem, nglyphs, nmetrics;
    int16_t ascent, descent;
} rl_ttf;

/** @brief A coverage bitmap of one glyph: w by h bytes, its top left at (x, y) from the pen on the baseline. */
typedef struct rl_ttf_bitmap {
    uint8_t *px;
    int32_t x, y;
    uint32_t w, h;
} rl_ttf_bitmap;

/** @brief Reads the tables of a TrueType font; false when data is not one (or is CFF-flavored). */
bool rl_ttf_init(rl_ttf *f, const uint8_t *data, size_t n);
/** @brief The glyph of code point cp, 0 (the missing glyph) when the font has none. */
uint32_t rl_ttf_glyph(const rl_ttf *f, uint32_t cp);
/** @brief The advance of glyph g at size px (pixels per em), in pixels. */
float rl_ttf_advance(const rl_ttf *f, uint32_t g, float px);
/** @brief The ascent and descent (descent is negative) at size px, in pixels. */
void rl_ttf_metrics(const rl_ttf *f, float px, float *ascent, float *descent);
/**
 * @brief Draws glyph g at size px with the pen moved right by dx (0 to 1 pixel) into a new coverage bitmap;
 * false (and an empty bitmap) for a glyph with no outline. Free px with free.
 */
bool rl_ttf_render(const rl_ttf *f, uint32_t g, float px, float dx, rl_ttf_bitmap *out);

#endif
