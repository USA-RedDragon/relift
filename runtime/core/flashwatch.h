/**
 * @file flashwatch.h
 * @brief The window's flash watch, for every guest: with present.flash naming a folder, each new frame the presenter
 * shows is read back, given a signature and pushed through the frame jump detector (framejump.h). When a frame is a
 * one-frame flash between two that agree, <folder>/flash-<frame>/ gets the frame before it, it and the frame after it
 * as present-<n>.png, exactly as they reached the window, and flash.json with the differences. A guest layer that
 * knows more about its frames (the NVN layer's nvn_flash.c) can keep its own trigger beside this one.
 */
#ifndef RELIFT_CORE_FLASHWATCH_H
#define RELIFT_CORE_FLASHWATCH_H

#include "framejump.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Most flashes saved in one run. */
#define RL_FLASH_MAX 8u

/** @brief The watch's state: the detector, and the last two pictures with their frame numbers (older first). */
typedef struct rl_flashwatch {
    char dir[1024];
    rl_jump jump;
    uint8_t *px[2];
    size_t cap[2];
    uint32_t width[2], height[2];
    bool bgra[2];
    uint64_t frame[2];
    uint32_t held, saved;
} rl_flashwatch;

/**
 * @brief Reads `<folder>[:<threshold>]` (threshold in luminance steps, default 10) into f; an empty spec or a folder
 * that cannot be made leaves the watch off. True when it is on.
 */
bool rl_flashwatch_open(rl_flashwatch *f, const char *spec);
/** @brief Whether the watch is on. */
static inline bool rl_flashwatch_on(const rl_flashwatch *f) { return f->dir[0] != 0; }
/**
 * @brief Adds the picture of frame number frame (w by h, 4-byte pixels, rows stride bytes apart, BGRA when bgra).
 * Writes the flash folder when the frame before it is a flash; returns that frame's number, else 0.
 */
uint64_t rl_flashwatch_push(rl_flashwatch *f, uint64_t frame, const uint8_t *px, uint32_t w, uint32_t h, size_t stride,
                            bool bgra);
/** @brief Frees what the watch holds; it is off afterwards. */
void rl_flashwatch_close(rl_flashwatch *f);

#endif
