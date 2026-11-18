/**
 * @file upscale_ref.h
 * @brief The C reference of relift's spatial upscaler choices (upscale_ref.c, the arithmetic of shaders/upscale.comp).
 */
#ifndef RELIFT_TESTS_UPSCALE_REF_H
#define RELIFT_TESTS_UPSCALE_REF_H

#include <relift/upscale.h>

#include <stdint.h>

/**
 * @brief Upscales the in rectangle of src (sw by sh RGBA8 words, red in the low byte) into the out rectangle of dst
 * (dw words a row) with choice, a spatial choice of upscale.h (RL_UPSCALE_OFF is the copy at the picture's own size).
 */
void upscale_ref(uint32_t choice, const uint32_t *src, uint32_t sw, const rl_upscale_rect *in, uint32_t *dst,
                 uint32_t dw, const rl_upscale_rect *out);

#endif
