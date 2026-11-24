/**
 * @file png.h
 * @brief A small PNG writer for debug captures: 8-bit RGB with stored (uncompressed) deflate blocks, so it needs no
 * compression library and writes fast.
 */
#ifndef RELIFT_CORE_PNG_H
#define RELIFT_CORE_PNG_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @brief Writes a w by h picture of 4-byte pixels (RGBA, or BGRA when bgra) whose rows are stride bytes apart to path
 * as an 8-bit RGB PNG; alpha is dropped. False when the file cannot be written.
 */
RL_RUNTIME_API bool rl_png_write_rgb(const char *path, uint32_t w, uint32_t h, const uint8_t *px, size_t stride,
                                     bool bgra);

#endif
