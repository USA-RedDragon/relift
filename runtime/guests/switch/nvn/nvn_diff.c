/**
 * @file nvn_diff.c
 * @brief The differential capture: keeps every compute dispatch of the last frames with its exact inputs and outputs
 * on the GPU, and saves the frame a trigger (core/trigger.h) names, for runtime/tools/nvn_diff.py to replay offline.
 *
 * RELIFT_NVN_DIFF=<dir>[:<detectors>] turns it on; detectors is a comma list of zband (zero bands, the default) and
 * jump (a one-frame luminance outlier), or none (only RELIFT_TRIGGER_AT, RELIFT_TRIGGER_FILE or another module's
 * rl_trigger_fire). Every compute dispatch of a frame is recorded into one of NVN_DIFF_SLOTS device-local arenas
 * (RELIFT_NVN_DIFF_MIB each, default 1536) by commands in the frame's own batches, with no extra submit:
 *
 * - before the dispatch, the bytes of each bound range (storage buffers, user uniform banks, compute bases: the
 *   binding table) that no earlier dispatch of the frame had bound yet, and each bound texture and storage image the
 *   frame had not yet captured: the state the frame's dispatches start from;
 * - after it, every range it may write (storage buffers, and compute bases when the program stores through them)
 *   and its storage images: what the game's GPU made.
 *
 * A barrier sits around each copy, so a captured dispatch never overlaps its neighbours (the runtime's overlap mode
 * is off for them). c[0] and c[1] as the runtime built them, the specialization constants, the guest push data and
 * the dispatch's place in the submit order are kept on the CPU. RELIFT_NVN_DIFF_KEYS=<hex prefix>[,...] limits the
 * capture to those programs. When the arena is full, later dispatches keep their records and pre-state but lose
 * their outputs ("post_dropped").
 *
 * At each present the presented picture is read back at NVN_DIFF_W by NVN_DIFF_H for the detectors. When a trigger
 * names a frame still held, <dir>/diff-<present>/ gets capture.json (the dispatches, their ranges, textures and the
 * arena offset of each copy), arena.bin, c0-<i>.bin and c1-<i>.bin, the detector's picture and the one before it
 * (look-<present>.png), and trigger.json. At most RELIFT_NVN_DIFF_MAX (default 3) frames are saved per run.
 */
#include "nvn_vk.h"

#include "framejump.h"
#include "png.h"
#include "trigger.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#define NVN_DIFF_SLOTS 2u
#define NVN_DIFF_W 480u
#define NVN_DIFF_H 270u
#define MAX_DISP 512u
#define MAX_RANGES 48u
#define MAX_TEX 32u
#define MAX_SPANS 8192u
#define MAX_COVER 4096u
#define MAX_SPECS 64u
#define CHUNK (64ull << 20)

/** @brief What a span of the arena holds. */
enum { SPAN_PRE_BUF, SPAN_POST_BUF, SPAN_PRE_IMG, SPAN_POST_IMG, SPAN_EXACT_BUF, SPAN_EXACT_IMG, SPAN_AFTER_BUF,
       SPAN_AFTER_IMG };
static const char *const span_names[] = { "pre",   "post",       "pre_image", "post_image",
                                          "exact", "exact_image", "after",     "after_image" };

/** @brief One copy in the arena: a guest range (buffers) or a texture of a dispatch (images). */
typedef struct dspan {
    uint64_t guest, gpu, size, at;
    uint32_t disp, tex;
    uint8_t what;
} dspan;

/** @brief A texture or storage image a dispatch bound. */
typedef struct dtex {
    const vimg *v;
    uint64_t storage;
    uint32_t id, slot, kind, dim, level, layer, format, w, h, d, levels, layers, target, copy_levels;
    int32_t vk_format;
    uint64_t bytes;
    bool image;
    uint8_t view[17];
} dtex;

/** @brief A bound buffer range of a dispatch. */
typedef struct drange {
    uint64_t gpu, guest, size;
    uint32_t kind, slot;
    bool write, unmapped;
} drange;

/** @brief One captured dispatch. */
typedef struct ddisp {
    uint64_t index, queue, submit, seq;
    uint32_t pos, groups[3], nspec, nrange, ntex, c1_size;
    bool indirect, post_dropped;
    /** @brief Every input was copied just before it and the read-only ones again just after (exact spans). */
    bool exact;
    char key[72];
    uint32_t spec_id[MAX_SPECS], spec_val[MAX_SPECS];
    uint64_t c1_guest;
    uint8_t c0[NVN_BANK2_BYTES];
    uint8_t *c1;
    uint8_t push[NVN_PUSH_BYTES];
    bool push_used;
    drange range[MAX_RANGES];
    dtex tex[MAX_TEX];
} ddisp;

/** @brief One frame's capture. */
typedef struct dslot {
    uint64_t frame;
    VkBuffer buf;
    VkDeviceMemory mem;
    uint64_t used, dropped_bytes;
    ddisp *disp;
    uint32_t ndisp, nspan, ncover, nimg_seen;
    uint64_t commands;
    dspan *span;
    /** @brief Guest ranges captured as pre-state this frame, sorted and disjoint. */
    uint64_t (*cover)[2];
    /** @brief Images captured as pre-state this frame. */
    const vimg **img_seen;
} dslot;

static struct {
    bool parsed, on, zband, jump;
    char dir[480];
    uint64_t budget;
    uint32_t saved, max_saves;
    char keys[16][72];
    uint32_t nkeys;
    /** @brief RELIFT_NVN_DIFF_EXACT: programs whose every input is copied just before and again just after them. */
    char exact[16][72];
    uint32_t nexact;
    dslot slot[NVN_DIFF_SLOTS];
    /** @brief The dispatch being recorded (NULL when it is not captured) and its slot. */
    ddisp *cur;
    dslot *cur_slot;
    /** @brief Textures bound since the last dispatch began. */
    dtex pend[MAX_TEX];
    uint32_t npend;
    /** @brief The detectors' picture: a GPU image, its readback, and the last two pictures on the CPU. */
    VkImage look;
    VkDeviceMemory look_mem;
    staging look_buf;
    bool look_recorded;
    uint8_t *pic[2];
    uint64_t pic_frame[2];
    rl_jump jump_state;
    uint64_t last_saved;
} df;

static void make_dir(const char *path)
{
#if defined(_WIN32)
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

bool nvn_diff_on(void)
{
    if (df.parsed)
        return df.on;
    df.parsed = true;
    const char *spec = getenv("RELIFT_NVN_DIFF");
    if (!spec || !*spec)
        return false;
    const char *colon = strrchr(spec, ':');
    size_t n = colon ? (size_t)(colon - spec) : strlen(spec);
    const char *det = colon ? colon + 1 : "zband";
    if (!n || n >= sizeof df.dir)
        return false;
    memcpy(df.dir, spec, n);
    df.dir[n] = 0;
    df.zband = strstr(det, "zband") != NULL;
    df.jump = strstr(det, "jump") != NULL;
    const char *mib = getenv("RELIFT_NVN_DIFF_MIB");
    df.budget = (mib && atoll(mib) > 0 ? (uint64_t)atoll(mib) : 1536ull) << 20;
    const char *mx = getenv("RELIFT_NVN_DIFF_MAX");
    df.max_saves = mx && atoi(mx) > 0 ? (uint32_t)atoi(mx) : 3u;
    const char *keys = getenv("RELIFT_NVN_DIFF_KEYS");
    while (keys && *keys && df.nkeys < 16) {
        size_t k = strcspn(keys, ",");
        if (k && k < sizeof df.keys[0]) {
            memcpy(df.keys[df.nkeys], keys, k);
            df.keys[df.nkeys++][k] = 0;
        }
        keys += k + (keys[k] == ',');
    }
    const char *ex = getenv("RELIFT_NVN_DIFF_EXACT");
    while (ex && *ex && df.nexact < 16) {
        size_t k = strcspn(ex, ",");
        if (k && k < sizeof df.exact[0]) {
            memcpy(df.exact[df.nexact], ex, k);
            df.exact[df.nexact++][k] = 0;
        }
        ex += k + (ex[k] == ',');
    }
    for (uint32_t i = 0; i < NVN_DIFF_SLOTS; i++) {
        dslot *s = &df.slot[i];
        s->frame = UINT64_MAX;
        s->disp = calloc(MAX_DISP, sizeof *s->disp);
        s->span = calloc(MAX_SPANS, sizeof *s->span);
        s->cover = calloc(MAX_COVER, sizeof *s->cover);
        s->img_seen = calloc(MAX_SPANS, sizeof *s->img_seen);
        if (!s->disp || !s->span || !s->cover || !s->img_seen)
            return false;
    }
    df.pic[0] = calloc((size_t)NVN_DIFF_W * NVN_DIFF_H, 4);
    df.pic[1] = calloc((size_t)NVN_DIFF_W * NVN_DIFF_H, 4);
    df.pic_frame[0] = df.pic_frame[1] = UINT64_MAX;
    df.last_saved = UINT64_MAX;
    if (!df.pic[0] || !df.pic[1])
        return false;
    make_dir(df.dir);
    df.on = true;
    rl_log(RL_LOG_INFO, "nvn-diff",
           "differential capture on: every compute dispatch keeps its inputs and outputs for %u frames (%llu MiB "
           "each), detectors%s%s, saved to %s",
           NVN_DIFF_SLOTS, (unsigned long long)(df.budget >> 20), df.zband ? " zband" : "", df.jump ? " jump" : "",
           df.dir);
    return true;
}

/** @brief Whether program key is captured (RELIFT_NVN_DIFF_KEYS). */
/** @brief Whether program key's inputs are copied whole around it (RELIFT_NVN_DIFF_EXACT). */
static bool key_exact(const char *key)
{
    for (uint32_t i = 0; i < df.nexact; i++)
        if (!strncmp(key, df.exact[i], strlen(df.exact[i])))
            return true;
    return false;
}

static bool key_wanted(const char *key)
{
    if (!df.nkeys)
        return true;
    for (uint32_t i = 0; i < df.nkeys; i++)
        if (!strncmp(key, df.keys[i], strlen(df.keys[i])))
            return true;
    return false;
}

/** @brief The slot of the current frame, emptied when it held an older one; NULL when it cannot be made. */
static dslot *slot_now(void)
{
    dslot *s = &df.slot[g.presents % NVN_DIFF_SLOTS];
    if (s->frame != g.presents) {
        s->frame = g.presents;
        for (uint32_t i = 0; i < s->ndisp; i++) {
            free(s->disp[i].c1);
            s->disp[i].c1 = NULL;
        }
        s->ndisp = s->nspan = s->ncover = s->nimg_seen = 0;
        s->used = s->dropped_bytes = 0;
        s->commands = 0;
    }
    if (!s->buf && !nvn_vk_gpu_buffer(&s->buf, &s->mem, df.budget)) {
        s->buf = NULL;
        rl_log(RL_LOG_ERROR, "nvn-diff", "cannot make a %llu MiB arena; the capture stops",
               (unsigned long long)(df.budget >> 20));
        df.on = false;
        return NULL;
    }
    return s;
}

/** @brief Adds a span of size bytes; its arena offset, or UINT64_MAX when the arena or the span list is full. */
static uint64_t add_span(dslot *s, uint8_t what, uint64_t guest, uint64_t gpu, uint64_t size, uint32_t disp,
                         uint32_t tex)
{
    uint64_t at = (s->used + 255u) & ~255ull;
    if (s->nspan == MAX_SPANS || at + size > df.budget) {
        s->dropped_bytes += size;
        return UINT64_MAX;
    }
    s->span[s->nspan++] = (dspan){ .guest = guest, .gpu = gpu, .size = size, .at = at, .disp = disp, .tex = tex,
                                   .what = what };
    s->used = at + size;
    return at;
}

/** @brief Records a copy of guest range [lo, hi) of one pool buffer into the arena as a span. */
static void copy_range(dslot *s, uint8_t what, uint64_t lo, uint64_t hi, uint64_t gpu, uint32_t disp)
{
    while (lo < hi) {
        void *buf;
        uint64_t off, before, rest;
        if (!nvn_guest_buffer_rest(lo, &buf, &off, &before, &rest) || !rest)
            return;
        uint64_t n = hi - lo < rest ? hi - lo : rest;
        uint64_t at = add_span(s, what, lo, gpu, n, disp, UINT32_MAX);
        if (at != UINT64_MAX) {
            VkBufferCopy c = { .srcOffset = off, .dstOffset = at, .size = n };
            VK(vkCmdCopyBuffer)(g.cb, (VkBuffer)buf, s->buf, 1, &c);
        }
        lo += n;
        gpu += n;
    }
}

/** @brief Records copies of the parts of [lo, hi) not yet covered this frame, and covers them. */
static void copy_uncovered(dslot *s, uint64_t lo, uint64_t hi, uint64_t gpu, uint32_t disp)
{
    uint64_t at = lo;
    uint32_t i = 0;
    while (i < s->ncover && s->cover[i][1] <= lo)
        i++;
    for (uint32_t k = i; k < s->ncover && s->cover[k][0] < hi && at < hi; k++) {
        if (s->cover[k][0] > at)
            copy_range(s, SPAN_PRE_BUF, at, s->cover[k][0], gpu + (at - lo), disp);
        if (s->cover[k][1] > at)
            at = s->cover[k][1];
    }
    if (at < hi)
        copy_range(s, SPAN_PRE_BUF, at, hi, gpu + (at - lo), disp);
    /* Cover [lo, hi): merge with the intervals it meets. */
    uint32_t j = i;
    uint64_t nlo = lo, nhi = hi;
    while (j < s->ncover && s->cover[j][0] <= hi) {
        nlo = s->cover[j][0] < nlo ? s->cover[j][0] : nlo;
        nhi = s->cover[j][1] > nhi ? s->cover[j][1] : nhi;
        j++;
    }
    if (i == j && s->ncover == MAX_COVER)
        return;
    memmove(&s->cover[i + 1], &s->cover[j], (size_t)(s->ncover - j) * sizeof s->cover[0]);
    s->ncover = s->ncover - (j - i) + 1;
    s->cover[i][0] = nlo;
    s->cover[i][1] = nhi;
}

/** @brief Records a copy of t's captured levels into the arena as a span. */
static void copy_image(dslot *s, uint8_t what, dtex *t, uint32_t disp, uint32_t ti)
{
    const vimg *v = t->v;
    if (!t->bytes)
        return;
    uint64_t at = add_span(s, what, t->storage, 0, t->bytes, disp, ti);
    if (at == UINT64_MAX)
        return;
    VkBufferImageCopy bic[16];
    uint32_t n = 0;
    uint64_t off = at;
    bool volume = t->target == NVN_TARGET_3D;
    uint32_t first = t->image ? t->level : 0;
    for (uint32_t lv = first; lv < first + t->copy_levels && n < 16; lv++) {
        uint32_t lw = t->w >> lv ? t->w >> lv : 1, lh = t->h >> lv ? t->h >> lv : 1;
        uint32_t ld = volume ? (t->d >> lv ? t->d >> lv : 1) : 1;
        uint32_t bpb, bw, bh;
        if (v->decoded || !nvn_tex_format(v->d.format, &bpb, &bw, &bh))
            bpb = 4, bw = bh = 1;
        bic[n++] = (VkBufferImageCopy){ .bufferOffset = off,
                                        .imageSubresource = { .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                                                              .mipLevel = lv,
                                                              .baseArrayLayer = 0,
                                                              .layerCount = volume ? 1 : t->layers },
                                        .imageExtent = { lw, lh, ld } };
        off += (uint64_t)((lw + bw - 1) / bw) * ((lh + bh - 1) / bh) * bpb * ld * (volume ? 1 : t->layers);
    }
    VK(vkCmdCopyImageToBuffer)(g.cb, v->image, VK_IMAGE_LAYOUT_GENERAL, s->buf, n, bic);
}

/** @brief Fills the size fields of a texture entry: what a copy of it takes (0: not captured). */
static void size_texture(dtex *t)
{
    const vimg *v = t->v;
    t->bytes = 0;
    if (v->aspect != VK_IMAGE_ASPECT_COLOR_BIT)
        return;
    uint32_t bpb, bw, bh;
    if (v->decoded || !nvn_tex_format(v->d.format, &bpb, &bw, &bh))
        bpb = 4, bw = bh = 1;
    bool volume = v->d.target == NVN_TARGET_3D;
    t->w = v->d.width;
    t->h = v->d.height;
    t->d = volume ? (v->d.depth ? v->d.depth : 1) : 1;
    t->levels = v->d.levels ? v->d.levels : 1;
    t->layers = volume ? 1 : nvn_vk_image_range(v).layerCount;
    if (t->layers > 64)
        return;
    t->copy_levels = t->image ? 1 : t->levels;
    if (t->copy_levels > 16)
        t->copy_levels = 16;
    uint32_t first = t->image ? t->level : 0;
    if (first >= t->levels)
        return;
    for (uint32_t lv = first; lv < first + t->copy_levels; lv++) {
        uint32_t lw = t->w >> lv ? t->w >> lv : 1, lh = t->h >> lv ? t->h >> lv : 1;
        uint32_t ld = volume ? (t->d >> lv ? t->d >> lv : 1) : 1;
        t->bytes += (uint64_t)((lw + bw - 1) / bw) * ((lh + bh - 1) / bh) * bpb * ld * t->layers;
    }
}

void nvn_diff_dispatch_reset(void) { df.npend = 0; }

void nvn_diff_texture(const vimg *v, uint32_t id, uint32_t slot, uint32_t kind, uint32_t dim, bool image,
                      uint32_t level, uint32_t layer, const uint8_t view[17])
{
    if (!df.on || !v)
        return;
    for (uint32_t i = 0; i < df.npend; i++)
        if (df.pend[i].v == v && df.pend[i].slot == slot)
            return;
    if (df.npend == MAX_TEX)
        return;
    dtex *t = &df.pend[df.npend++];
    memset(t, 0, sizeof *t);
    *t = (dtex){ .v = v, .storage = v->storage, .id = id, .slot = slot, .kind = kind, .dim = dim, .level = level,
                 .layer = layer, .format = v->d.format, .target = v->d.target, .vk_format = (int32_t)v->format,
                 .image = image };
    memcpy(t->view, view, 17);
    size_texture(t);
}

void nvn_diff_dispatch_begin(replay *r, const nvn_diff_dispatch *d)
{
    df.cur = NULL;
    if (!df.on || !key_wanted(d->key))
        return;
    dslot *s = slot_now();
    if (!s)
        return;
    s->commands++;
    if (s->ndisp == MAX_DISP)
        return;
    uint32_t di = s->ndisp++;
    ddisp *e = &s->disp[di];
    memset(e, 0, sizeof *e);
    e->index = s->commands - 1;
    e->queue = r->queue;
    e->submit = r->submit;
    e->pos = r->pos;
    e->seq = r->seq;
    snprintf(e->key, sizeof e->key, "%s", d->key);
    memcpy(e->groups, d->groups, sizeof e->groups);
    e->indirect = d->indirect;
    memcpy(e->c0, d->c0, d->c0_bytes < sizeof e->c0 ? d->c0_bytes : sizeof e->c0);
    e->nspec = d->nspec < MAX_SPECS ? d->nspec : MAX_SPECS;
    for (uint32_t i = 0; i < e->nspec; i++) {
        e->spec_id[i] = d->spec_map[i].constantID;
        e->spec_val[i] = d->spec_vals[d->spec_map[i].offset / 4];
    }
    e->c1_guest = d->c1_guest;
    if (d->c1_guest && d->c1_size && d->c1_size <= 0x10000 && (e->c1 = malloc(d->c1_size))) {
        memcpy(e->c1, rl_mem_base() + d->c1_guest, d->c1_size);
        e->c1_size = d->c1_size;
    }
    if (d->push) {
        memcpy(e->push, d->push, NVN_PUSH_BYTES);
        e->push_used = true;
    }
    for (uint32_t i = 0; i < d->nrange && e->nrange < MAX_RANGES; i++) {
        drange *rg = &e->range[e->nrange++];
        *rg = (drange){ .gpu = d->range[i].gpu, .size = d->range[i].size, .kind = d->range[i].kind,
                        .slot = d->range[i].slot, .write = d->range[i].write };
        rg->guest = nvn_gpu_guest(rg->gpu);
        rg->unmapped = !nvn_gpu_mapped(rg->gpu, rg->size);
    }
    memcpy(e->tex, df.pend, (size_t)df.npend * sizeof df.pend[0]);
    e->ntex = df.npend;
    df.npend = 0;
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    for (uint32_t i = 0; i < e->nrange; i++)
        if (!e->range[i].unmapped)
            copy_uncovered(s, e->range[i].guest, e->range[i].guest + e->range[i].size, e->range[i].gpu, di);
    e->exact = key_exact(e->key);
    for (uint32_t i = 0; e->exact && i < e->nrange; i++)
        if (!e->range[i].unmapped)
            copy_range(s, SPAN_EXACT_BUF, e->range[i].guest, e->range[i].guest + e->range[i].size, e->range[i].gpu, di);
    for (uint32_t i = 0; e->exact && i < e->ntex; i++)
        copy_image(s, SPAN_EXACT_IMG, &e->tex[i], di, i);
    for (uint32_t i = 0; i < e->ntex; i++) {
        bool seen = false;
        for (uint32_t k = 0; k < s->nimg_seen && !seen; k++)
            seen = s->img_seen[k] == e->tex[i].v;
        if (seen || s->nimg_seen == MAX_SPANS)
            continue;
        s->img_seen[s->nimg_seen++] = e->tex[i].v;
        copy_image(s, SPAN_PRE_IMG, &e->tex[i], di, i);
    }
    nvn_vk_barrier(g.cb);
    df.cur = e;
    df.cur_slot = s;
}

void nvn_diff_dispatch_end(replay *r)
{
    ddisp *e = df.cur;
    dslot *s = df.cur_slot;
    df.cur = NULL;
    if (!e || !s)
        return;
    uint32_t di = (uint32_t)(e - s->disp);
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    uint64_t before = s->dropped_bytes;
    /* The written ranges, merged: storage buffers inside a compute base's span are copied once. */
    uint64_t w[MAX_RANGES][3];
    uint32_t nw = 0;
    for (uint32_t i = 0; i < e->nrange; i++)
        if (e->range[i].write && !e->range[i].unmapped) {
            uint64_t lo = e->range[i].guest, hi = lo + e->range[i].size, gpu = e->range[i].gpu;
            uint32_t k = nw;
            while (k > 0 && w[k - 1][0] > lo) {
                memcpy(w[k], w[k - 1], sizeof w[k]);
                k--;
            }
            w[k][0] = lo;
            w[k][1] = hi;
            w[k][2] = gpu;
            nw++;
        }
    uint32_t nm = 0;
    for (uint32_t i = 0; i < nw; i++) {
        if (nm && w[i][0] <= w[nm - 1][1] && w[i][2] - w[i][0] == w[nm - 1][2] - w[nm - 1][0]) {
            if (w[i][1] > w[nm - 1][1])
                w[nm - 1][1] = w[i][1];
            continue;
        }
        memcpy(w[nm++], w[i], sizeof w[i]);
    }
    for (uint32_t i = 0; i < nm; i++)
        copy_range(s, SPAN_POST_BUF, w[i][0], w[i][1], w[i][2], di);
    for (uint32_t i = 0; i < e->ntex; i++)
        if (e->tex[i].image)
            copy_image(s, SPAN_POST_IMG, &e->tex[i], di, i);
    for (uint32_t i = 0; e->exact && i < e->nrange; i++)
        if (!e->range[i].write && !e->range[i].unmapped)
            copy_range(s, SPAN_AFTER_BUF, e->range[i].guest, e->range[i].guest + e->range[i].size, e->range[i].gpu,
                       di);
    for (uint32_t i = 0; e->exact && i < e->ntex; i++)
        if (!e->tex[i].image)
            copy_image(s, SPAN_AFTER_IMG, &e->tex[i], di, i);
    e->post_dropped = s->dropped_bytes != before;
    nvn_vk_barrier(g.cb);
}

void nvn_diff_present_record(replay *r, VkImage src, const uint32_t rect[4], uint64_t frame)
{
    (void)frame;
    df.look_recorded = false;
    if (!df.on || (!df.zband && !df.jump))
        return;
    if (!df.look) {
        VkImageCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                                 .imageType = VK_IMAGE_TYPE_2D,
                                 .format = VK_FORMAT_R8G8B8A8_UNORM,
                                 .extent = { NVN_DIFF_W, NVN_DIFF_H, 1 },
                                 .mipLevels = 1,
                                 .arrayLayers = 1,
                                 .samples = VK_SAMPLE_COUNT_1_BIT,
                                 .tiling = VK_IMAGE_TILING_OPTIMAL,
                                 .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
        VkMemoryRequirements req;
        if (VK(vkCreateImage)(g.dev, &ci, NULL, &df.look) != VK_SUCCESS)
            return;
        VK(vkGetImageMemoryRequirements)(g.dev, df.look, &req);
        int32_t t = nvn_vk_mem_type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                     .allocationSize = req.size,
                                     .memoryTypeIndex = (uint32_t)t };
        if (t < 0 || VK(vkAllocateMemory)(g.dev, &mai, NULL, &df.look_mem) != VK_SUCCESS ||
            VK(vkBindImageMemory)(g.dev, df.look, df.look_mem, 0) != VK_SUCCESS ||
            !nvn_vk_readback_new(&df.look_buf, (uint64_t)NVN_DIFF_W * NVN_DIFF_H * 4)) {
            df.zband = df.jump = false;
            return;
        }
        nvn_vk_begin(r);
        VkImageMemoryBarrier ib = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                                    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = df.look,
                                    .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
        VK(vkCmdPipelineBarrier)(g.cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                 NULL, 0, NULL, 1, &ib);
    }
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    VkImageBlit b = { .srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                      .srcOffsets = { { (int32_t)rect[0], (int32_t)rect[1], 0 },
                                      { (int32_t)(rect[0] + rect[2]), (int32_t)(rect[1] + rect[3]), 1 } },
                      .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                      .dstOffsets = { { 0, 0, 0 }, { (int32_t)NVN_DIFF_W, (int32_t)NVN_DIFF_H, 1 } } };
    VK(vkCmdBlitImage)(g.cb, src, VK_IMAGE_LAYOUT_GENERAL, df.look, VK_IMAGE_LAYOUT_GENERAL, 1, &b,
                       VK_FILTER_NEAREST);
    nvn_vk_barrier(g.cb);
    VkBufferImageCopy bic = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
                              .imageExtent = { NVN_DIFF_W, NVN_DIFF_H, 1 } };
    VK(vkCmdCopyImageToBuffer)(g.cb, df.look, VK_IMAGE_LAYOUT_GENERAL, df.look_buf.buf, 1, &bic);
    nvn_vk_barrier(g.cb);
    df.look_recorded = true;
}

/** @brief Writes the JSON string of a texture entry's fields. */
static void json_tex(FILE *f, const dtex *t, int64_t pre, int64_t post)
{
    fprintf(f,
            "{\"id\":%u,\"slot\":%u,\"kind\":%u,\"dim\":%u,\"image\":%d,\"storage\":\"0x%llx\",\"format\":%u,"
            "\"vk_format\":%d,\"width\":%u,\"height\":%u,\"depth\":%u,\"levels\":%u,\"layers\":%u,\"target\":%u,"
            "\"level\":%u,\"layer\":%u,\"copy_levels\":%u,\"bytes\":%llu,\"view\":\"",
            t->id, t->slot, t->kind, t->dim, (int)t->image, (unsigned long long)t->storage, t->format, t->vk_format,
            t->w, t->h, t->d, t->levels, t->layers, t->target, t->level, t->layer, t->copy_levels,
            (unsigned long long)t->bytes);
    for (uint32_t i = 0; i < 17; i++)
        fprintf(f, "%02x", t->view[i]);
    fprintf(f, "\",\"pre_span\":%lld,\"post_span\":%lld}", (long long)pre, (long long)post);
}

/** @brief Writes n bytes at p to dir/name. */
static bool put(const char *dir, const char *name, const void *p, size_t n)
{
    char path[768];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    bool ok = fwrite(p, 1, n, f) == n;
    return fclose(f) == 0 && ok;
}

/** @brief Saves slot s (frame hit->present) to <dir>/diff-<present>/. */
static void save(replay *r, dslot *s, const rl_trigger_hit *hit)
{
    char dir[640], path[768];
    snprintf(dir, sizeof dir, "%s/diff-%06llu", df.dir, (unsigned long long)s->frame);
    make_dir(dir);
    if (!nvn_vk_flush(r))
        return;
    /* The arena, in chunks through one readback buffer. */
    snprintf(path, sizeof path, "%s/arena.bin", dir);
    FILE *f = fopen(path, "wb");
    staging st = { 0 };
    bool ok = f && nvn_vk_readback_new(&st, CHUNK);
    for (uint64_t at = 0; ok && at < s->used; at += CHUNK) {
        uint64_t n = s->used - at < CHUNK ? s->used - at : CHUNK;
        nvn_vk_begin(r);
        VkBufferCopy c = { .srcOffset = at, .dstOffset = 0, .size = n };
        VK(vkCmdCopyBuffer)(g.cb, s->buf, st.buf, 1, &c);
        nvn_vk_barrier(g.cb);
        ok = nvn_vk_flush(r) && fwrite(st.map, 1, n, f) == n;
    }
    if (f)
        ok &= fclose(f) == 0;
    if (st.buf)
        nvn_vk_staging_free(&st);
    if (!ok) {
        rl_log(RL_LOG_ERROR, "nvn-diff", "cannot save the arena of present %llu to %s", (unsigned long long)s->frame,
               dir);
        return;
    }
    snprintf(path, sizeof path, "%s/capture.json", dir);
    f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "{\"present\":%llu,\"arena_bytes\":%llu,\"dropped_bytes\":%llu,\"commands\":%llu,\"dispatches\":[\n",
            (unsigned long long)s->frame, (unsigned long long)s->used, (unsigned long long)s->dropped_bytes,
            (unsigned long long)s->commands);
    for (uint32_t i = 0; i < s->ndisp; i++) {
        ddisp *e = &s->disp[i];
        char name[64];
        snprintf(name, sizeof name, "c0-%u.bin", i);
        put(dir, name, e->c0, sizeof e->c0);
        if (e->c1) {
            snprintf(name, sizeof name, "c1-%u.bin", i);
            put(dir, name, e->c1, e->c1_size);
        }
        fprintf(f,
                "%s{\"i\":%u,\"index\":%llu,\"key\":\"%s\",\"queue\":\"0x%llx\",\"submit\":%llu,\"pos\":%u,\"seq\":%llu,"
                "\"groups\":[%u,%u,%u],\"indirect\":%d,\"post_dropped\":%d,\"exact\":%d,\"c0\":\"c0-%u.bin\"",
                i ? ",\n" : "", i, (unsigned long long)e->index, e->key, (unsigned long long)e->queue,
                (unsigned long long)e->submit, e->pos, (unsigned long long)e->seq, e->groups[0], e->groups[1],
                e->groups[2], (int)e->indirect, (int)e->post_dropped, (int)e->exact, i);
        if (e->c1)
            fprintf(f, ",\"c1\":\"c1-%u.bin\",\"c1_guest\":\"0x%llx\"", i, (unsigned long long)e->c1_guest);
        fprintf(f, ",\"specs\":[");
        for (uint32_t k = 0; k < e->nspec; k++)
            fprintf(f, "%s[%u,%u]", k ? "," : "", e->spec_id[k], e->spec_val[k]);
        fprintf(f, "],\"push\":\"");
        for (uint32_t k = 0; e->push_used && k < NVN_PUSH_BYTES; k++)
            fprintf(f, "%02x", e->push[k]);
        fprintf(f, "\",\"ranges\":[");
        static const char *const kinds[] = { "storage", "uniform", "base" };
        for (uint32_t k = 0; k < e->nrange; k++) {
            const drange *rg = &e->range[k];
            fprintf(f, "%s{\"kind\":\"%s\",\"slot\":%u,\"gpu\":\"0x%llx\",\"guest\":\"0x%llx\",\"size\":%llu,"
                    "\"write\":%d,\"unmapped\":%d}",
                    k ? "," : "", kinds[rg->kind < 3 ? rg->kind : 0], rg->slot, (unsigned long long)rg->gpu,
                    (unsigned long long)rg->guest, (unsigned long long)rg->size, (int)rg->write, (int)rg->unmapped);
        }
        fprintf(f, "],\"textures\":[");
        for (uint32_t k = 0; k < e->ntex; k++) {
            int64_t pre = -1, post = -1;
            for (uint32_t q = 0; q < s->nspan; q++)
                if (s->span[q].disp == i && s->span[q].tex == k &&
                    (s->span[q].what == SPAN_PRE_IMG || s->span[q].what == SPAN_POST_IMG))
                    *(s->span[q].what == SPAN_PRE_IMG ? &pre : &post) = (int64_t)q;
            fprintf(f, "%s", k ? "," : "");
            json_tex(f, &e->tex[k], pre, post);
        }
        fprintf(f, "]}");
    }
    fprintf(f, "\n],\"spans\":[\n");
    for (uint32_t q = 0; q < s->nspan; q++) {
        const dspan *p = &s->span[q];
        fprintf(f, "%s{\"what\":\"%s\",\"disp\":%u,\"guest\":\"0x%llx\",\"gpu\":\"0x%llx\",\"size\":%llu,\"at\":%llu",
                q ? ",\n" : "", span_names[p->what], p->disp, (unsigned long long)p->guest,
                (unsigned long long)p->gpu, (unsigned long long)p->size, (unsigned long long)p->at);
        if (p->tex != UINT32_MAX)
            fprintf(f, ",\"tex\":%u", p->tex);
        fprintf(f, "}");
    }
    fprintf(f, "\n]}\n");
    fclose(f);
    snprintf(path, sizeof path, "%s/trigger.json", dir);
    f = fopen(path, "w");
    if (f) {
        fprintf(f, "{\"present\":%llu,\"source\":\"%s\",\"detail\":\"", (unsigned long long)hit->present,
                hit->source);
        for (const char *c = hit->detail; *c; c++)
            fprintf(f, *c == '"' || *c == '\\' ? "\\%c" : "%c", *c);
        fprintf(f, "\",\"look_w\":%u,\"look_h\":%u}\n", NVN_DIFF_W, NVN_DIFF_H);
        fclose(f);
    }
    for (uint32_t k = 0; k < 2; k++)
        if (df.pic_frame[k] == s->frame || df.pic_frame[k] + 1 == s->frame) {
            snprintf(path, sizeof path, "%s/look-%06llu.png", dir, (unsigned long long)df.pic_frame[k]);
            rl_png_write_rgb(path, NVN_DIFF_W, NVN_DIFF_H, df.pic[k], (size_t)NVN_DIFF_W * 4, false);
        }
    rl_log(RL_LOG_WARN, "nvn-diff",
           "present %llu (%s: %s): %u dispatches, %llu MiB of inputs and outputs%s saved in %s; replay it with "
           "runtime/tools/nvn_diff.py",
           (unsigned long long)s->frame, hit->source, hit->detail, s->ndisp, (unsigned long long)(s->used >> 20),
           s->dropped_bytes ? " (arena full: some outputs dropped)" : "", dir);
}

void nvn_diff_presented(replay *r, uint64_t frame)
{
    if (!df.on)
        return;
    rl_trigger_poll(frame);
    if (df.look_recorded) {
        df.look_recorded = false;
        uint32_t k = (uint32_t)(frame % 2);
        memcpy(df.pic[k], df.look_buf.map, (size_t)NVN_DIFF_W * NVN_DIFF_H * 4);
        df.pic_frame[k] = frame;
        uint32_t o = k ^ 1u;
        if (df.zband && df.pic_frame[o] + 1 == frame) {
            rl_zband z[4];
            /* The presented picture went through scaling and post-processing after the image the bands are made
             * in: channels up to 2 count as zero, as in the band finder of runtime/tools/nvn_diff.py's image. */
            rl_zband_opts zo = { .level = 2 };
            uint32_t n = rl_zband_find(df.pic[k], df.pic[o], NVN_DIFF_W, NVN_DIFF_H, (size_t)NVN_DIFF_W * 4, &zo, z, 4);
            if (n)
                rl_trigger_fire(frame, "zband",
                                "%u zero bands in the presented picture at %ux%u, largest x %u-%u y %u-%u", n,
                                NVN_DIFF_W, NVN_DIFF_H, z[0].x0, z[0].x1, z[0].y0, z[0].y1);
        }
        if (df.jump) {
            uint8_t sig[RL_JUMP_SIG];
            uint64_t bad;
            rl_jump_sig_rgba(df.pic[k], NVN_DIFF_W, NVN_DIFF_H, (size_t)NVN_DIFF_W * 4, false, sig);
            if (rl_jump_push(&df.jump_state, frame, sig, &bad))
                rl_trigger_fire(bad, "jump", "differs from both neighbours by %.1f and %.1f luminance steps",
                                (double)df.jump_state.d_prev / 16.0, (double)df.jump_state.d_next / 16.0);
        }
    }
    rl_trigger_hit hit;
    while (rl_trigger_take(&hit)) {
        if (hit.present == df.last_saved)
            continue;
        dslot *s = &df.slot[hit.present % NVN_DIFF_SLOTS];
        if (s->frame != hit.present) {
            rl_log(RL_LOG_INFO, "nvn-diff", "present %llu (%s) is no longer held", (unsigned long long)hit.present,
                   hit.source);
            continue;
        }
        if (df.saved >= df.max_saves)
            continue;
        df.saved++;
        df.last_saved = hit.present;
        save(r, s, &hit);
    }
}
