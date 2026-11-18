/**
 * @file fxaa_ref.h
 * @brief The C reference of the FXAA pass (fxaa_ref.c).
 */
#ifndef RELIFT_TESTS_FXAA_REF_H
#define RELIFT_TESTS_FXAA_REF_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief FXAA 3.11 Quality at its extreme preset (39) on width by height RGB888 pixels into out (not in), with
 * edges clamped. Returns false when it cannot allocate its work buffer (out is then a copy of in).
 */
bool fxaa_ref(const uint8_t *in, uint8_t *out, uint32_t width, uint32_t height);

#endif
