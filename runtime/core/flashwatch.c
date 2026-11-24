/**
 * @file flashwatch.c
 * @brief The window's flash watch (flashwatch.h).
 */
#include "flashwatch.h"

#include "core.h"
#include "png.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#define MKDIR(p) mkdir((p), 0755)
#endif

bool rl_flashwatch_open(rl_flashwatch *f, const char *spec)
{
    memset(f, 0, sizeof *f);
    if (!spec || !*spec)
        return false;
    char buf[sizeof f->dir];
    snprintf(buf, sizeof buf, "%s", spec);
    char *colon = strrchr(buf, ':');
    if (colon && colon != buf && colon[1] >= '0' && colon[1] <= '9') {
        char *end;
        unsigned long t = strtoul(colon + 1, &end, 10);
        if (!*end && t) {
            f->jump.threshold = (uint32_t)t * 16u;
            *colon = 0;
        }
    }
    for (char *s = buf + 1; *s; s++)
        if (*s == '/' || *s == '\\') {
            char sep = *s;
            *s = 0;
            MKDIR(buf);
            *s = sep;
        }
    if (MKDIR(buf) != 0 && errno != EEXIST) {
        rl_log(RL_LOG_WARN, "present", "present.flash: cannot make the folder %s (%s); no flash watch", buf,
               strerror(errno));
        return false;
    }
    memcpy(f->dir, buf, sizeof buf);
    rl_log(RL_LOG_INFO, "present", "present.flash: watching every new frame for one-frame flashes into %s", f->dir);
    return true;
}

/** @brief Copies a picture into slot i of the held ones. */
static bool hold(rl_flashwatch *f, int i, uint64_t frame, const uint8_t *px, uint32_t w, uint32_t h, size_t stride,
                 bool bgra)
{
    size_t need = (size_t)w * h * 4;
    if (need > f->cap[i]) {
        uint8_t *n = realloc(f->px[i], need);
        if (!n)
            return false;
        f->px[i] = n;
        f->cap[i] = need;
    }
    for (uint32_t y = 0; y < h; y++)
        memcpy(f->px[i] + (size_t)y * w * 4, px + y * stride, (size_t)w * 4);
    f->width[i] = w;
    f->height[i] = h;
    f->bgra[i] = bgra;
    f->frame[i] = frame;
    return true;
}

static void write_png(const rl_flashwatch *f, const char *dir, uint64_t frame, const uint8_t *px, uint32_t w,
                      uint32_t h, size_t stride, bool bgra)
{
    char path[sizeof f->dir + 64];
    snprintf(path, sizeof path, "%s/present-%06llu.png", dir, (unsigned long long)frame);
    if (!rl_png_write_rgb(path, w, h, px, stride, bgra))
        rl_log(RL_LOG_ERROR, "present", "present.flash: cannot write %s", path);
}

uint64_t rl_flashwatch_push(rl_flashwatch *f, uint64_t frame, const uint8_t *px, uint32_t w, uint32_t h, size_t stride,
                            bool bgra)
{
    if (!rl_flashwatch_on(f) || !px || !w || !h)
        return 0;
    uint8_t sig[RL_JUMP_SIG];
    rl_jump_sig_rgba(px, w, h, stride, bgra, sig);
    uint64_t bad = 0;
    bool flash = rl_jump_push(&f->jump, frame, sig, &bad) && f->held == 2 && f->frame[1] == bad;
    if (flash && f->saved < RL_FLASH_MAX) {
        char dir[sizeof f->dir + 32];
        snprintf(dir, sizeof dir, "%s/flash-%06llu", f->dir, (unsigned long long)bad);
        MKDIR(dir);
        for (int i = 0; i < 2; i++)
            write_png(f, dir, f->frame[i], f->px[i], f->width[i], f->height[i], (size_t)f->width[i] * 4, f->bgra[i]);
        write_png(f, dir, frame, px, w, h, stride, bgra);
        char path[sizeof dir + 16];
        snprintf(path, sizeof path, "%s/flash.json", dir);
        FILE *j = fopen(path, "w");
        if (j) {
            fprintf(j,
                    "{\"flash\": %llu, \"before\": %llu, \"after\": %llu, \"to_before\": %.2f, \"to_after\": %.2f, "
                    "\"before_to_after\": %.2f, \"threshold\": %.2f}\n",
                    (unsigned long long)bad, (unsigned long long)f->frame[0], (unsigned long long)frame,
                    f->jump.d_prev / 16.0, f->jump.d_next / 16.0, f->jump.d_skip / 16.0,
                    (f->jump.threshold ? f->jump.threshold : RL_JUMP_DEFAULT_THRESHOLD) / 16.0);
            fclose(j);
        }
        f->saved++;
        rl_log(RL_LOG_WARN, "present", "present.flash: frame %llu flashed between %llu and %llu (%.1f and %.1f steps "
               "off, they %.1f apart): %s", (unsigned long long)bad, (unsigned long long)f->frame[0],
               (unsigned long long)frame, f->jump.d_prev / 16.0, f->jump.d_next / 16.0, f->jump.d_skip / 16.0, dir);
    }
    uint8_t *t = f->px[0];
    size_t c = f->cap[0];
    f->px[0] = f->px[1];
    f->cap[0] = f->cap[1];
    f->width[0] = f->width[1];
    f->height[0] = f->height[1];
    f->bgra[0] = f->bgra[1];
    f->frame[0] = f->frame[1];
    f->px[1] = t;
    f->cap[1] = c;
    if (hold(f, 1, frame, px, w, h, stride, bgra))
        f->held = f->held < 2 ? f->held + 1 : 2;
    else
        f->held = 0;
    return flash ? bad : 0;
}

void rl_flashwatch_close(rl_flashwatch *f)
{
    free(f->px[0]);
    free(f->px[1]);
    memset(f, 0, sizeof *f);
}
