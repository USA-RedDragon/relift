/**
 * @file trigger.c
 * @brief The capture trigger and the zero-band detector (trigger.h).
 */
#include "trigger.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define AT_MAX 64u

static struct {
    pthread_mutex_t lock;
    rl_trigger_hit q[RL_TRIGGER_QUEUE];
    uint32_t head, n;
    uint64_t dropped;
    bool parsed;
    uint64_t at[AT_MAX];
    uint32_t nat;
    char file[512];
} tg = { .lock = PTHREAD_MUTEX_INITIALIZER };

void rl_trigger_fire(uint64_t present, const char *source, const char *fmt, ...)
{
    rl_trigger_hit h = { .present = present };
    snprintf(h.source, sizeof h.source, "%s", source ? source : "?");
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(h.detail, sizeof h.detail, fmt, ap);
        va_end(ap);
    }
    pthread_mutex_lock(&tg.lock);
    if (tg.n == RL_TRIGGER_QUEUE)
        tg.dropped++;
    else
        tg.q[(tg.head + tg.n++) % RL_TRIGGER_QUEUE] = h;
    pthread_mutex_unlock(&tg.lock);
    rl_log(RL_LOG_INFO, "trigger", "present %llu: %s: %s", (unsigned long long)present, h.source, h.detail);
}

bool rl_trigger_take(rl_trigger_hit *hit)
{
    pthread_mutex_lock(&tg.lock);
    bool got = tg.n != 0;
    if (got) {
        *hit = tg.q[tg.head];
        tg.head = (tg.head + 1) % RL_TRIGGER_QUEUE;
        tg.n--;
    }
    pthread_mutex_unlock(&tg.lock);
    return got;
}

uint64_t rl_trigger_dropped(void)
{
    pthread_mutex_lock(&tg.lock);
    uint64_t d = tg.dropped;
    pthread_mutex_unlock(&tg.lock);
    return d;
}

void rl_trigger_reset(void)
{
    pthread_mutex_lock(&tg.lock);
    tg.head = tg.n = 0;
    tg.dropped = 0;
    tg.parsed = false;
    tg.nat = 0;
    tg.file[0] = 0;
    pthread_mutex_unlock(&tg.lock);
}

/** @brief Reads RELIFT_TRIGGER_AT and RELIFT_TRIGGER_FILE once. */
static void parse(void)
{
    if (tg.parsed)
        return;
    tg.parsed = true;
    const char *at = getenv("RELIFT_TRIGGER_AT");
    while (at && *at && tg.nat < AT_MAX) {
        char *end;
        unsigned long long v = strtoull(at, &end, 10);
        if (end == at)
            break;
        tg.at[tg.nat++] = v;
        at = *end == ',' ? end + 1 : end;
    }
    const char *f = getenv("RELIFT_TRIGGER_FILE");
    if (f && strlen(f) < sizeof tg.file)
        memcpy(tg.file, f, strlen(f) + 1);
}

void rl_trigger_poll(uint64_t present)
{
    pthread_mutex_lock(&tg.lock);
    parse();
    bool at = false;
    for (uint32_t i = 0; i < tg.nat; i++)
        at |= tg.at[i] == present;
    char file[sizeof tg.file];
    memcpy(file, tg.file, sizeof file);
    pthread_mutex_unlock(&tg.lock);
    if (at)
        rl_trigger_fire(present, "at", "RELIFT_TRIGGER_AT names present %llu", (unsigned long long)present);
    if (file[0]) {
        FILE *f = fopen(file, "rb");
        if (f) {
            fclose(f);
            remove(file);
            rl_trigger_fire(present, "file", "%s appeared", file);
        }
    }
}

/** @brief The largest channel of R, G and B of the pixel at p. */
static uint8_t peak(const uint8_t *p)
{
    uint8_t m = p[0] > p[1] ? p[0] : p[1];
    return m > p[2] ? m : p[2];
}

/** @brief A column run of zeros that had content: column x, rows y0 to y1, and the band it joined (union-find). */
typedef struct zrun {
    uint32_t x, y0, y1, parent;
} zrun;

static uint32_t root(zrun *r, uint32_t i)
{
    while (r[i].parent != i) {
        r[i].parent = r[r[i].parent].parent;
        i = r[i].parent;
    }
    return i;
}

uint32_t rl_zband_find(const uint8_t *px, const uint8_t *prev, uint32_t w, uint32_t h, size_t stride,
                       const rl_zband_opts *opts, rl_zband *out, uint32_t max)
{
    rl_zband_opts o = opts ? *opts : (rl_zband_opts){ 0 };
    if (!o.content)
        o.content = 8;
    if (!o.min_rows)
        o.min_rows = h / 16 ? h / 16 : 1;
    if (!o.max_cols)
        o.max_cols = w / 8 ? w / 8 : 1;
    if (!o.had_content_pct)
        o.had_content_pct = 90;
    if (!px || !prev || !w || !h)
        return 0;
    uint32_t cap = w * 8u, n = 0;
    zrun *runs = malloc(sizeof *runs * cap);
    if (!runs)
        return 0;
    /* Column runs: zero now, mostly content before. */
    for (uint32_t x = 0; x < w && n < cap; x++) {
        uint32_t y = 0;
        while (y < h && n < cap) {
            if (peak(px + (size_t)y * stride + (size_t)x * 4) > o.level) {
                y++;
                continue;
            }
            uint32_t y0 = y, had = 0;
            while (y < h && peak(px + (size_t)y * stride + (size_t)x * 4) <= o.level) {
                had += peak(prev + (size_t)y * stride + (size_t)x * 4) > o.content;
                y++;
            }
            if (y - y0 >= o.min_rows && (uint64_t)had * 100u >= (uint64_t)(y - y0) * o.had_content_pct)
                runs[n] = (zrun){ x, y0, y, n }, n++;
        }
    }
    /* Join runs of neighbouring columns that overlap by half the shorter one. */
    uint32_t prev_lo = 0, prev_hi = 0;
    for (uint32_t i = 0; i < n;) {
        uint32_t j = i;
        while (j < n && runs[j].x == runs[i].x)
            j++;
        if (prev_hi > prev_lo && runs[prev_lo].x + 1 == runs[i].x)
            for (uint32_t a = i; a < j; a++)
                for (uint32_t b = prev_lo; b < prev_hi; b++) {
                    uint32_t lo = runs[a].y0 > runs[b].y0 ? runs[a].y0 : runs[b].y0;
                    uint32_t hi = runs[a].y1 < runs[b].y1 ? runs[a].y1 : runs[b].y1;
                    uint32_t la = runs[a].y1 - runs[a].y0, lb = runs[b].y1 - runs[b].y0;
                    if (hi > lo && 2 * (hi - lo) >= (la < lb ? la : lb)) {
                        uint32_t ra = root(runs, a), rb = root(runs, b);
                        if (ra != rb)
                            runs[ra].parent = rb;
                    }
                }
        prev_lo = i;
        prev_hi = j;
        i = j;
    }
    /* Bands: the box of each joined set, kept when narrow enough. */
    rl_zband found[RL_ZBAND_MAX * 4];
    uint32_t area[RL_ZBAND_MAX * 4], nf = 0, total = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (root(runs, i) != i)
            continue;
        rl_zband b = { UINT32_MAX, 0, UINT32_MAX, 0 };
        uint32_t px_n = 0;
        for (uint32_t k = 0; k < n; k++)
            if (root(runs, k) == i) {
                b.x0 = runs[k].x < b.x0 ? runs[k].x : b.x0;
                b.x1 = runs[k].x + 1 > b.x1 ? runs[k].x + 1 : b.x1;
                b.y0 = runs[k].y0 < b.y0 ? runs[k].y0 : b.y0;
                b.y1 = runs[k].y1 > b.y1 ? runs[k].y1 : b.y1;
                px_n += runs[k].y1 - runs[k].y0;
            }
        if (b.x1 - b.x0 > o.max_cols)
            continue;
        total++;
        if (nf < sizeof found / sizeof found[0]) {
            found[nf] = b;
            area[nf++] = px_n;
        }
    }
    free(runs);
    /* Largest first. */
    for (uint32_t i = 1; i < nf; i++)
        for (uint32_t k = i; k > 0 && area[k] > area[k - 1]; k--) {
            rl_zband tb = found[k];
            found[k] = found[k - 1];
            found[k - 1] = tb;
            uint32_t ta = area[k];
            area[k] = area[k - 1];
            area[k - 1] = ta;
        }
    for (uint32_t i = 0; i < nf && i < max; i++)
        out[i] = found[i];
    return total;
}
