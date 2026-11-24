/**
 * @file framejump.c
 * @brief The frame jump detector (framejump.h).
 */
#include "framejump.h"

#include <string.h>

uint8_t rl_jump_luma(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint8_t)((299u * r + 587u * g + 114u * b + 500u) / 1000u);
}

void rl_jump_sig_rgba(const uint8_t *px, uint32_t w, uint32_t h, size_t stride, bool bgra, uint8_t sig[RL_JUMP_SIG])
{
    for (uint32_t y = 0; y < RL_JUMP_SIG_H; y++) {
        uint32_t sy = (uint32_t)(((uint64_t)(2 * y + 1) * h) / (2 * RL_JUMP_SIG_H));
        const uint8_t *row = px + (size_t)(sy < h ? sy : h - 1) * stride;
        for (uint32_t x = 0; x < RL_JUMP_SIG_W; x++) {
            uint32_t sx = (uint32_t)(((uint64_t)(2 * x + 1) * w) / (2 * RL_JUMP_SIG_W));
            const uint8_t *p = row + (size_t)(sx < w ? sx : w - 1) * 4;
            sig[y * RL_JUMP_SIG_W + x] = bgra ? rl_jump_luma(p[2], p[1], p[0]) : rl_jump_luma(p[0], p[1], p[2]);
        }
    }
}

uint32_t rl_jump_dist(const uint8_t a[RL_JUMP_SIG], const uint8_t b[RL_JUMP_SIG])
{
    uint64_t sum = 0;
    for (uint32_t i = 0; i < RL_JUMP_SIG; i++)
        sum += a[i] > b[i] ? (uint32_t)(a[i] - b[i]) : (uint32_t)(b[i] - a[i]);
    return (uint32_t)(sum * 16u / RL_JUMP_SIG);
}

bool rl_jump_push(rl_jump *j, uint64_t present, const uint8_t sig[RL_JUMP_SIG], uint64_t *outlier)
{
    bool hit = false;
    j->d_next = j->d_prev = j->d_skip = 0;
    if (j->count == 2) {
        uint32_t t = j->threshold ? j->threshold : RL_JUMP_DEFAULT_THRESHOLD;
        j->d_next = rl_jump_dist(sig, j->sig[1]);
        j->d_prev = rl_jump_dist(j->sig[1], j->sig[0]);
        j->d_skip = rl_jump_dist(sig, j->sig[0]);
        uint32_t lo = j->d_next < j->d_prev ? j->d_next : j->d_prev;
        hit = lo >= t && 2 * j->d_skip <= lo;
        if (hit && outlier)
            *outlier = j->present[1];
        memcpy(j->sig[0], j->sig[1], RL_JUMP_SIG);
        j->present[0] = j->present[1];
        memcpy(j->sig[1], sig, RL_JUMP_SIG);
        j->present[1] = present;
        return hit;
    }
    memcpy(j->sig[j->count], sig, RL_JUMP_SIG);
    j->present[j->count] = present;
    j->count++;
    return false;
}
