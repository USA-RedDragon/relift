/**
 * @file vk_capture.c
 * @brief Debug capture of presented guest images: with RELIFT_VK_CAPTURE set
 * to <dir>[:N], every Nth vkQueuePresentKHR writes the guest swapchain image
 * as an 8-bit RGB PNG named frame-<frame number>-<guest ms>ms.png. Off by
 * default. The PNG uses stored (uncompressed) deflate blocks.
 */
#include "vkf.h"
#include "hostio.h"

#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static pthread_mutex_t g_cap_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_cap_read;
static char g_cap_dir[PATH_MAX];
static uint64_t g_cap_every = 1;
static uint64_t g_cap_frame;

static void set_spec(const char *spec)
{
    g_cap_dir[0] = 0;
    g_cap_every = 1;
    g_cap_frame = 0;
    if (!spec || !*spec)
        return;
    const char *colon = strrchr(spec, ':');
    size_t n = colon ? (size_t)(colon - spec) : strlen(spec);
    if (colon) {
        char *end;
        unsigned long long every = strtoull(colon + 1, &end, 10);
        if (*end || !every) {
            rl_log(RL_LOG_ERROR, "vk", "%s=%s: the part after ':' must be a frame count above 0", RL_VK_CAPTURE_ENV,
                   spec);
            return;
        }
        g_cap_every = every;
    }
    if (!n || n >= sizeof g_cap_dir)
        return;
    memcpy(g_cap_dir, spec, n);
    g_cap_dir[n] = 0;
    if (mkdir(g_cap_dir, 0777) != 0 && errno != EEXIST) {
        rl_log(RL_LOG_ERROR, "vk", "%s: cannot create %s: %s", RL_VK_CAPTURE_ENV, g_cap_dir, strerror(errno));
        g_cap_dir[0] = 0;
        return;
    }
    rl_log(RL_LOG_INFO, "vk", "capturing every %llu presented frame%s to %s", (unsigned long long)g_cap_every,
           g_cap_every == 1 ? "" : "s", g_cap_dir);
}

void rl_vk_capture(const char *spec)
{
    pthread_mutex_lock(&g_cap_lock);
    set_spec(spec);
    g_cap_read = true;
    pthread_mutex_unlock(&g_cap_lock);
}

bool vkf_capture_due(uint64_t *frame)
{
    pthread_mutex_lock(&g_cap_lock);
    if (!g_cap_read) {
        set_spec(getenv(RL_VK_CAPTURE_ENV));
        g_cap_read = true;
    }
    uint64_t f = g_cap_frame++;
    bool due = g_cap_dir[0] && f % g_cap_every == 0;
    pthread_mutex_unlock(&g_cap_lock);
    *frame = f;
    return due;
}

static uint32_t crc_table[256];

static void crc_init(void)
{
    for (uint32_t n = 0; n < 256; n++) {
        uint32_t c = n;
        for (int k = 0; k < 8; k++)
            c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
}

static uint32_t crc(uint32_t c, const uint8_t *p, size_t n)
{
    c ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++)
        c = crc_table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static bool chunk(FILE *f, const char type[4], const uint8_t *data, uint32_t n)
{
    uint8_t h[8];
    be32(h, n);
    memcpy(h + 4, type, 4);
    uint32_t c = crc(crc(0, h + 4, 4), data, n);
    uint8_t t[4];
    be32(t, c);
    return fwrite(h, 1, 8, f) == 8 && (!n || fwrite(data, 1, n, f) == n) && fwrite(t, 1, 4, f) == 4;
}

bool vkf_capture_write(uint64_t frame, uint64_t guest_ns, uint32_t w, uint32_t h, const uint8_t *px, size_t stride,
                       bool bgra)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, crc_init);
    char path[PATH_MAX + 64];
    pthread_mutex_lock(&g_cap_lock);
    snprintf(path, sizeof path, "%s/frame-%06llu-%08llums.png", g_cap_dir, (unsigned long long)frame,
             (unsigned long long)(guest_ns / 1000000u));
    pthread_mutex_unlock(&g_cap_lock);
    size_t row = 1 + (size_t)w * 3, raw = row * h;
    size_t blocks = raw / 65535 + 1;
    size_t zn = 2 + raw + blocks * 5 + 4;
    if (zn > UINT32_MAX)
        return false;
    uint8_t *z = malloc(zn);
    uint8_t *line = malloc(row);
    if (!z || !line) {
        free(z);
        free(line);
        return false;
    }
    size_t at = 0, left = raw, in_block = 0;
    uint32_t s1 = 1, s2 = 0;
    z[at++] = 0x78;
    z[at++] = 0x01;
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t *src = px + (size_t)y * stride;
        line[0] = 0;
        for (uint32_t x = 0; x < w; x++) {
            line[1 + x * 3] = src[x * 4 + (bgra ? 2 : 0)];
            line[2 + x * 3] = src[x * 4 + 1];
            line[3 + x * 3] = src[x * 4 + (bgra ? 0 : 2)];
        }
        for (size_t i = 0; i < row; i++) {
            if (!in_block) {
                size_t len = left < 65535 ? left : 65535;
                z[at++] = left <= 65535 ? 1 : 0;
                z[at++] = (uint8_t)len;
                z[at++] = (uint8_t)(len >> 8);
                z[at++] = (uint8_t)~len;
                z[at++] = (uint8_t)(~len >> 8);
                in_block = len;
            }
            z[at++] = line[i];
            s1 = (s1 + line[i]) % 65521u;
            s2 = (s2 + s1) % 65521u;
            in_block--;
            left--;
        }
    }
    free(line);
    be32(z + at, (s2 << 16) | s1);
    at += 4;
    uint8_t ihdr[13];
    be32(ihdr, w);
    be32(ihdr + 4, h);
    ihdr[8] = 8;
    ihdr[9] = 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    FILE *f = fopen(path, "wb");
    bool ok = f && fwrite("\x89PNG\r\n\x1a\n", 1, 8, f) == 8 && chunk(f, "IHDR", ihdr, 13) &&
              chunk(f, "IDAT", z, (uint32_t)at) && chunk(f, "IEND", NULL, 0);
    if (f && fclose(f) != 0)
        ok = false;
    free(z);
    if (!ok)
        rl_log(RL_LOG_ERROR, "vk", "capture: cannot write %s", path);
    return ok;
}
