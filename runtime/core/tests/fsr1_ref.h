/**
 * @file fsr1_ref.h
 * @brief A C port of FSR 1's EASU and RCAS (fsr1_ref.c) for the FSR 1 stage's tests.
 */
#ifndef RELIFT_TESTS_FSR1_REF_H
#define RELIFT_TESTS_FSR1_REF_H

#include <relift/upscale.h>

#include <stdint.h>

/**
 * @brief Upscales the in rectangle of src (sw RGBA8 words a row, red in the low byte) into the out rectangle of dst
 * (dw words a row, alpha 255) with EASU, then sharpens with RCAS at stops below its strongest.
 */
void fsr1_ref(const uint32_t *src, uint32_t sw, const rl_upscale_rect *in, uint32_t *dst, uint32_t dw,
              const rl_upscale_rect *out, float stops);

#endif
