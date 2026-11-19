/**
 * @file nvn_astc.h
 * @brief ASTC LDR block decoding (nvn_astc.c).
 */
#ifndef NVN_ASTC_H
#define NVN_ASTC_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Decodes one 16 byte ASTC block of bw x bh texels (4x4 to 12x12) into RGBA8 texels, row by row, 4 bytes
 * each. srgb selects the sRGB block decoding (the stored 8 bit values are sRGB encoded for color and alpha). Void
 * extent blocks give their constant; HDR modes and illegal encodings give opaque magenta, the spec's error color.
 */
void nvn_astc_block(uint32_t bw, uint32_t bh, bool srgb, const uint8_t block[16], uint8_t *rgba);

/**
 * @brief Decodes a w x h level (rows of ceil(w / bw) blocks of 16 bytes, from blocks) into rows of RGBA8 texels
 * in rgba, cropping the blocks at the right and bottom edges.
 */
void nvn_astc_image(uint32_t bw, uint32_t bh, bool srgb, uint32_t w, uint32_t h, const uint8_t *blocks, uint8_t *rgba);

#endif
