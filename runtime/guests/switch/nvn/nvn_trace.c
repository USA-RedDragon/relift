/**
 * @file nvn_trace.c
 * @brief The NVN frame trace: one frame's draws and dispatches, with everything they read and write, on disk.
 *
 * RELIFT_NVN_TRACE=<dir>:<present>[:<frames>] arms the trace at the first present at or past <present>: the next
 * <frames> frames (default 1) are written to <dir>/frame-<present of the frame>/. Each draw and dispatch becomes
 * one line of trace.jsonl: the programs, the render targets, the fixed-function and dynamic state, every bound
 * uniform and storage buffer with its GPU and guest address, the pushed constants, and every texture and image
 * the command bound with its host image, format, size, view and swizzle. The bytes go to files that the lines
 * name: buffer ranges before and after the command, images and render targets before and after it (every level and
 * layer, in the host image's format). Files are named by a hash of their bytes, so a range that did not change
 * is written once. With RELIFT_NVN_TRACE_GBUFFER set, the trace starts at the first frame after a frame that drew into four
 * or more render targets at once (a deferred renderer's G-buffer pass), at or past <present>. Each traced command submits the batch before and after it so the files show the GPU's work.
 * Beside it, order.jsonl gives the order everything ran in, one line per event with a running "seq": each submit
 * ("submit": its queue and its number among all submits), each replayed command ("command": the NVN call, its queue,
 * submit and index "pos" in the flattened set, its first five argument words, and for event waits and signals the
 * event's word and whether the wait let the set through; a wait that holds the set appears each time it is checked)
 * the queue-level nvnQueueFenceSync and nvnQueueWaitSync calls ("queue"), and each conditional render check a draw,
 * dispatch or clear makes ("render": the counter address, the two 64-bit words compared, and "runs" 1 when the
 * command runs), and each counter report the completion writes ("report": the guest address, the counter type, the
 * value written, and whether the counter was valid, in the order the reports thread writes them). Each draw and dispatch line of
 * trace.jsonl carries the same "queue", "submit", "pos" and the "seq" of its order line.
 * The reader is runtime/tools/nvn_trace.py.
 */
#include "nvn_vk.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#define MAX_TEX 128u
#define MAX_CACHE 1024u
#define MAX_FILES 65536u
#define MAX_COPIES 512u
#define BUF_CAP (64ull << 20)
#define IMG_CAP (512ull << 20)

/** @brief Makes the folder path; one that already exists is left as it is. */
static void make_dir(const char *path)
{
#if defined(_WIN32)
    _mkdir(path);
#else
    mkdir(path, 0777);
#endif
}

typedef struct tex_entry {
    vimg *v;
    uint32_t id, slot, kind, dim;
    bool image;
    uint8_t view[17];
    char pre[40], post[40];
} tex_entry;

typedef struct buf_entry {
    uint32_t stage, slot;
    bool storage;
    uint64_t addr, guest, size;
    /** @brief The range is not inside one memory pool: its bytes are not read. */
    bool unmapped;
    char pre[40], post[40];
} buf_entry;

typedef struct rt_entry {
    vimg *v;
    uint32_t level;
    char pre[40], post[40];
} rt_entry;

static struct {
    bool parsed;
    char dir[480];
    uint64_t from, frames, done, wide_seen;
    bool gbuffer;
    FILE *out;
    uint64_t cmd;
    char frame_dir[512];
    uint64_t seen[MAX_FILES];
    uint32_t nseen;
    struct {
        vimg *v;
        uint64_t written;
        char name[40];
    } cache[MAX_CACHE];
    uint32_t ncache;
    const char *kind;
    uint32_t ntex, nbuf, nrt;
    tex_entry tex[MAX_TEX];
    buf_entry buf[NVN_STAGES * (NVN_UBO_SLOTS + NVN_STORAGE_SLOTS)];
    rt_entry rt[MAX_RT + 1];
} tr;

bool nvn_trace_active;
bool nvn_trace_watching;
uint64_t nvn_trace_wide_draws;

/** @brief The guest range RELIFT_NVN_TRACE_WATCH names (watch_size 0: none) and the hash of its bytes last seen. */
static uint64_t watch_at, watch_size, watch_hash;
/** @brief RELIFT_NVN_TRACE_WATCH_POISON=<hex word>: the word the watched range is filled with at each present (0: off). */
static uint32_t watch_poison;
static void watch_look(const char *after);

/** @brief The order file of the frame being traced, its next line number, and the lock its writers take. */
static struct {
    FILE *f;
    uint64_t seq;
    pthread_mutex_t lock;
} ord = { NULL, 0, PTHREAD_MUTEX_INITIALIZER };

uint64_t nvn_trace_order(const char *kind, const char *name, uint64_t queue, uint64_t submit, uint32_t pos,
                         const uint64_t *args, uint32_t n, uint32_t value, bool met)
{
    if (!__atomic_load_n(&nvn_trace_active, __ATOMIC_ACQUIRE))
        return 0;
    pthread_mutex_lock(&ord.lock);
    uint64_t seq = 0;
    if (ord.f) {
        seq = ++ord.seq;
        fprintf(ord.f, "{\"seq\":%llu,\"frame\":%llu,\"kind\":\"%s\",\"name\":\"%s\",\"queue\":\"0x%llx\",\"submit\":%llu",
                (unsigned long long)seq, (unsigned long long)g.presents, kind, name, (unsigned long long)queue,
                (unsigned long long)submit);
        if (!strcmp(kind, "command"))
            fprintf(ord.f, ",\"pos\":%u", pos);
        fprintf(ord.f, ",\"args\":[");
        for (uint32_t i = 0; i < n; i++)
            fprintf(ord.f, "%s\"0x%llx\"", i ? "," : "", (unsigned long long)args[i]);
        fprintf(ord.f, "]");
        if (!strcmp(kind, "render"))
            fprintf(ord.f, ",\"pos\":%u,\"runs\":%u", pos, value);
        else if (!strcmp(name, "nvnCommandBufferWaitEvent") || !strcmp(name, "nvnCommandBufferSignalEvent") ||
                 !strcmp(name, "nvnCommandBufferWaitSync"))
            fprintf(ord.f, ",\"value\":%u,\"met\":%d", value, (int)met);
        fprintf(ord.f, "}\n");
    }
    pthread_mutex_unlock(&ord.lock);
    return seq;
}

void nvn_trace_image_gone(const vimg *v)
{
    for (uint32_t i = 0; i < tr.ncache && i < MAX_CACHE; i++)
        if (tr.cache[i].v == v)
            tr.cache[i].v = NULL;
    for (uint32_t i = 0; i < tr.ntex && i < MAX_TEX; i++)
        if (tr.tex[i].v == v)
            tr.tex[i].v = NULL;
    for (uint32_t i = 0; i < tr.nrt && i < MAX_RT + 1; i++)
        if (tr.rt[i].v == v)
            tr.rt[i].v = NULL;
}

bool nvn_trace_parse(const char *spec, char *dir, size_t dirsz, uint64_t *from, uint64_t *frames)
{
    if (!spec)
        return false;
    const char *l1 = strrchr(spec, ':');
    if (!l1 || l1 == spec)
        return false;
    char *end;
    uint64_t last = strtoull(l1 + 1, &end, 10);
    if (end == l1 + 1 || *end)
        return false;
    const char *dir_end = l1;
    uint64_t present = last, count = 1;
    const char *l2 = l1;
    while (l2 > spec && l2[-1] != ':')
        l2--;
    if (l2 > spec + 1) {
        uint64_t mid = strtoull(l2, &end, 10);
        if (end == l1 && end != l2) {
            present = mid;
            count = last;
            dir_end = l2 - 1;
        }
    }
    size_t len = (size_t)(dir_end - spec);
    if (!len || len >= dirsz || !count)
        return false;
    memcpy(dir, spec, len);
    dir[len] = 0;
    *from = present;
    *frames = count;
    return true;
}

static uint64_t hash_bytes(const void *p, uint64_t n)
{
    const uint8_t *b = p;
    uint64_t h = 0xcbf29ce484222325ull ^ n;
    for (uint64_t i = 0; i + 8 <= n; i += 8) {
        uint64_t w;
        memcpy(&w, b + i, 8);
        h = (h ^ w) * 0x100000001b3ull;
        h ^= h >> 29;
    }
    for (uint64_t i = n & ~7ull; i < n; i++)
        h = (h ^ b[i]) * 0x100000001b3ull;
    return h ^ (h >> 32);
}

static bool seen(uint64_t h)
{
    for (uint32_t i = 0; i < tr.nseen; i++)
        if (tr.seen[i] == h)
            return true;
    if (tr.nseen < MAX_FILES)
        tr.seen[tr.nseen++] = h;
    return false;
}

static void put_file(const char *sub, const char *ext, const void *p, uint64_t n, char *name)
{
    uint64_t h = hash_bytes(p, n);
    snprintf(name, 40, "%s/%016llx.%s", sub, (unsigned long long)h, ext);
    if (seen(h ^ (uint64_t)sub[0]))
        return;
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", tr.frame_dir, name);
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite(p, 1, n, f);
    fclose(f);
}

static void dump_range(uint64_t gpu, uint64_t size, char *name)
{
    name[0] = 0;
    if (!gpu || !size)
        return;
    if (size > BUF_CAP)
        size = BUF_CAP;
    if (!nvn_gpu_mapped(gpu, size))
        return;
    uint64_t guest = nvn_gpu_guest(gpu);
    put_file("bufs", "bin", rl_mem_base() + guest, size, name);
}

static uint64_t image_bytes(const vimg *v, uint32_t *layers)
{
    VkImageSubresourceRange rr = nvn_vk_image_range(v);
    bool volume = v->d.target == NVN_TARGET_3D;
    *layers = volume ? 1 : rr.layerCount;
    if (volume)
        rr.layerCount = v->d.depth ? v->d.depth : 1;
    uint32_t bpb, bw, bh;
    if (v->aspect == VK_IMAGE_ASPECT_DEPTH_BIT)
        return (uint64_t)v->d.width * v->d.height * 4 * rr.layerCount;
    if (v->aspect != VK_IMAGE_ASPECT_COLOR_BIT || !nvn_tex_format(v->d.format, &bpb, &bw, &bh))
        return 0;
    if (v->decoded)
        return (uint64_t)v->d.width * v->d.height * 4 * rr.layerCount;
    return (uint64_t)((v->d.width + bw - 1) / bw) * ((v->d.height + bh - 1) / bh) * bpb * rr.layerCount;
}

static void dump_image(replay *r, vimg *v, bool cacheable, char *name)
{
    name[0] = 0;
    bool volume = v->d.target == NVN_TARGET_3D;
    uint32_t layers;
    uint64_t bytes = image_bytes(v, &layers);
    if (!bytes || bytes > IMG_CAP)
        return;
    bool mips = false;
    uint64_t alloc = bytes;
    {
        uint32_t bpb, bw, bh;
        if (!volume && !v->decoded && v->aspect == VK_IMAGE_ASPECT_COLOR_BIT && v->d.levels > 1 && layers <= 64 &&
            nvn_tex_format(v->d.format, &bpb, &bw, &bh) && bw == 1 && bh == 1 && (uint64_t)layers * v->d.levels <= MAX_COPIES) {
            uint64_t extra = 0;
            for (uint32_t lv = 1; lv < v->d.levels; lv++)
                extra += (uint64_t)(v->d.width >> lv ? v->d.width >> lv : 1) * (v->d.height >> lv ? v->d.height >> lv : 1) * bpb * layers;
            if (bytes + extra <= IMG_CAP) {
                mips = true;
                alloc = bytes + extra;
            }
        }
    }
    if (cacheable)
        for (uint32_t i = 0; i < tr.ncache; i++)
            if (tr.cache[i].v == v && tr.cache[i].written == v->written) {
                memcpy(name, tr.cache[i].name, 40);
                return;
            }
    staging s;
    if (!nvn_vk_staging_new(&s, alloc))
        return;
    if (!nvn_vk_flush(r)) {
        nvn_vk_staging_free(&s);
        return;
    }
    nvn_vk_begin(r);
    nvn_vk_barrier(g.cb);
    uint64_t per = bytes / layers;
    VkBufferImageCopy bic[MAX_COPIES];
    uint32_t n = layers < 64 ? layers : 64;
    for (uint32_t l = 0; l < n; l++)
        bic[l] = (VkBufferImageCopy){ .bufferOffset = per * l,
                                      .imageSubresource = { .aspectMask = v->aspect, .baseArrayLayer = volume ? 0 : l, .layerCount = 1 },
                                      .imageExtent = { v->d.width, v->d.height, volume ? v->d.depth : 1 } };
    uint64_t total = per * n;
    uint32_t nbic = n;
    if (mips) {
        uint32_t bpb, bw, bh;
        nvn_tex_format(v->d.format, &bpb, &bw, &bh);
        for (uint32_t lv = 1; lv < v->d.levels; lv++) {
            uint32_t lw = v->d.width >> lv ? v->d.width >> lv : 1, lh = v->d.height >> lv ? v->d.height >> lv : 1;
            for (uint32_t l = 0; l < layers; l++) {
                bic[nbic++] = (VkBufferImageCopy){ .bufferOffset = total,
                                                   .imageSubresource = { .aspectMask = v->aspect, .mipLevel = lv, .baseArrayLayer = l, .layerCount = 1 },
                                                   .imageExtent = { lw, lh, 1 } };
                total += (uint64_t)lw * lh * bpb;
            }
        }
    }
    VK(vkCmdCopyImageToBuffer)(g.cb, v->image, VK_IMAGE_LAYOUT_GENERAL, s.buf, nbic, bic);
    nvn_vk_barrier(g.cb);
    if (nvn_vk_flush(r))
        put_file("imgs", "raw", s.map, total, name);
    nvn_vk_staging_free(&s);
    if (cacheable && name[0]) {
        uint32_t slot = tr.ncache < MAX_CACHE ? tr.ncache++ : 0;
        tr.cache[slot].v = v;
        tr.cache[slot].written = v->written;
        memcpy(tr.cache[slot].name, name, 40);
    }
}

static bool is_target(const vimg *v)
{
    for (uint32_t i = 0; i < tr.nrt; i++)
        if (tr.rt[i].v == v)
            return true;
    return false;
}

void nvn_trace_present(replay *r)
{
    (void)r;
    if (!tr.parsed) {
        tr.parsed = true;
        if (!nvn_trace_parse(getenv("RELIFT_NVN_TRACE"), tr.dir, sizeof tr.dir, &tr.from, &tr.frames))
            tr.from = UINT64_MAX;
        tr.gbuffer = getenv("RELIFT_NVN_TRACE_GBUFFER") != NULL;
        const char *w = getenv("RELIFT_NVN_TRACE_WATCH");
        char *end = NULL;
        if (w && (watch_at = strtoull(w, &end, 16), end && *end == ':'))
            watch_size = strtoull(end + 1, NULL, 0);
        if (watch_size > BUF_CAP)
            watch_size = BUF_CAP;
        nvn_trace_watching = watch_size != 0;
        const char *poison = getenv("RELIFT_NVN_TRACE_WATCH_POISON");
        watch_poison = poison && *poison ? (uint32_t)strtoul(poison, NULL, 16) : 0;
    }
    if (watch_size)
        watch_look("present"); /* a write after the frame's last command and before its present */
    if (watch_poison && watch_size) {
        /* every present starts the range as the poison word: a dispatch that reads it shows the game wrote nothing */
        for (uint64_t i = 0; i + 4 <= watch_size; i += 4)
            memcpy(rl_mem_base() + watch_at + i, &watch_poison, 4);
        watch_hash = hash_bytes(rl_mem_base() + watch_at, watch_size);
    }
    if (tr.out) {
        fclose(tr.out);
        tr.out = NULL;
        pthread_mutex_lock(&ord.lock);
        if (ord.f)
            fclose(ord.f);
        ord.f = NULL;
        pthread_mutex_unlock(&ord.lock);
        nvn_trace_active = false;
        tr.done++;
    }
    bool wide = nvn_trace_wide_draws != tr.wide_seen;
    tr.wide_seen = nvn_trace_wide_draws;
    if (tr.from == UINT64_MAX || g.presents < tr.from || tr.done >= tr.frames || (tr.gbuffer && !wide))
        return;
    char sub[640];
    snprintf(tr.frame_dir, sizeof tr.frame_dir, "%s/frame-%llu", tr.dir, (unsigned long long)g.presents);
    make_dir(tr.dir);
    make_dir(tr.frame_dir);
    snprintf(sub, sizeof sub, "%s/bufs", tr.frame_dir);
    make_dir(sub);
    snprintf(sub, sizeof sub, "%s/imgs", tr.frame_dir);
    make_dir(sub);
    snprintf(sub, sizeof sub, "%s/trace.jsonl", tr.frame_dir);
    tr.out = fopen(sub, "w");
    snprintf(sub, sizeof sub, "%s/order.jsonl", tr.frame_dir);
    pthread_mutex_lock(&ord.lock);
    ord.f = tr.out ? fopen(sub, "w") : NULL;
    ord.seq = 0;
    pthread_mutex_unlock(&ord.lock);
    tr.cmd = 0;
    tr.nseen = 0;
    tr.ncache = 0;
    nvn_trace_active = tr.out != NULL;
    if (nvn_trace_active)
        rl_log(RL_LOG_INFO, "nvn", "tracing frame %llu into %s", (unsigned long long)g.presents, tr.frame_dir);
}

/**
 * @brief The watched range's bytes as a file and a line, when they changed since the last look: "cpu" when the change
 * came between commands (the guest wrote them), or the command name that ran before the look.
 */
static void watch_look(const char *after)
{
    if (!watch_size)
        return;
    const uint8_t *p = rl_mem_base() + watch_at;
    uint64_t h = hash_bytes(p, watch_size);
    if (h == watch_hash)
        return;
    watch_hash = h;
    if (!tr.out) {
        char hexs[2 * 64 + 1];
        uint64_t n = watch_size < 64 ? watch_size : 64;
        for (uint64_t i = 0; i < n; i++)
            snprintf(hexs + 2 * i, 3, "%02x", p[i]);
        static uint32_t logged;
        if (watch_poison && logged++ > 400)
            return;
        rl_log(RL_LOG_INFO, "nvn-watch", "present %llu: 0x%llx changed before %s: %s",
               (unsigned long long)g.presents, (unsigned long long)watch_at, after, hexs);
        return;
    }
    char name[40];
    put_file("bufs", "bin", p, watch_size, name);
    fprintf(tr.out, "{\"kind\":\"watch\",\"before\":%llu,\"after\":\"%s\",\"guest\":\"0x%llx\",\"size\":%llu,\"bytes\":\"%s\"}\n",
            (unsigned long long)tr.cmd, after, (unsigned long long)watch_at, (unsigned long long)watch_size, name);
}

void nvn_trace_command(replay *r, const nvn_cmd *e, bool done)
{
    if (!tr.out) {
        if (!done)
            watch_look(nvn_cmd_name(e->op));
        return;
    }
    if (!done) {
        watch_look("cpu");
        fprintf(tr.out, "{\"kind\":\"cmd\",\"before\":%llu,\"name\":\"%s\",\"x\":[", (unsigned long long)tr.cmd,
                nvn_cmd_name(e->op));
        for (uint32_t i = 0; i < 8; i++)
            fprintf(tr.out, "%s\"0x%llx\"", i ? "," : "", (unsigned long long)e->x[i]);
        fprintf(tr.out, "]");
        if (e->op == NVN_CMD_UPDATE_UNIFORM_BUFFER) {
            const void *data = nvn_cmd_bytes(r->s, e->x[5]);
            char name[40] = "";
            if (data && e->x[4] && e->x[4] <= BUF_CAP)
                put_file("bufs", "bin", data, e->x[4], name);
            fprintf(tr.out, ",\"dst\":\"0x%llx\",\"data\":\"%s\"", (unsigned long long)(nvn_gpu_guest(e->x[1]) + e->x[3]),
                    name);
        }
        fprintf(tr.out, "}\n");
        return;
    }
    if (watch_size) {
        if (!nvn_vk_flush(r))
            return;
        watch_look(nvn_cmd_name(e->op));
    }
}

/** @brief The commands replayed since the last present, for the GPU trace's report (RELIFT_NVN_GPU_TRACE). */
#define RING 8192u
static struct {
    nvn_cmd e[RING];
    uint64_t dst[RING];
    uint32_t data[RING][4];
    uint64_t n, present;
} ring;

void nvn_trace_ring_note(replay *r, const nvn_cmd *e)
{
    if (ring.present != g.presents) {
        ring.present = g.presents;
        ring.n = 0;
    }
    uint32_t k = (uint32_t)(ring.n++ % RING);
    ring.e[k] = *e;
    ring.dst[k] = 0;
    memset(ring.data[k], 0, sizeof ring.data[k]);
    if (e->op == NVN_CMD_UPDATE_UNIFORM_BUFFER) {
        const uint8_t *data = nvn_cmd_bytes(r->s, e->x[5]);
        ring.dst[k] = nvn_gpu_guest(e->x[1]) + e->x[3];
        if (data)
            memcpy(ring.data[k], data, e->x[4] < 16 ? e->x[4] : 16);
    }
}

void nvn_trace_ring_dump(const char *why, uint32_t most)
{
    uint64_t first = ring.n > RING ? ring.n - RING : 0;
    if (most && ring.n - first > most)
        first = ring.n - most;
    rl_log(RL_LOG_INFO, "gpu-trace", "%s: the %llu commands replayed in present %llu before it:", why,
           (unsigned long long)(ring.n - first), (unsigned long long)ring.present);
    for (uint64_t i = first; i < ring.n; i++) {
        uint32_t k = (uint32_t)(i % RING);
        const nvn_cmd *e = &ring.e[k];
        if (e->op == NVN_CMD_UPDATE_UNIFORM_BUFFER)
            rl_log(RL_LOG_INFO, "gpu-trace", "  %llu %s guest 0x%llx size %llu: %08x %08x %08x %08x", (unsigned long long)i,
                   nvn_cmd_name(e->op), (unsigned long long)ring.dst[k], (unsigned long long)e->x[4], ring.data[k][0],
                   ring.data[k][1], ring.data[k][2], ring.data[k][3]);
        else
            rl_log(RL_LOG_INFO, "gpu-trace", "  %llu %s 0x%llx 0x%llx 0x%llx 0x%llx 0x%llx", (unsigned long long)i,
                   nvn_cmd_name(e->op), (unsigned long long)e->x[1], (unsigned long long)e->x[2],
                   (unsigned long long)e->x[3], (unsigned long long)e->x[4], (unsigned long long)e->x[5]);
    }
}

static void collect_buffers(const nvn_gfx *st, uint32_t first, uint32_t last)
{
    tr.nbuf = 0;
    for (uint32_t s = first; s <= last; s++) {
        if (!st->code[s])
            continue;
        for (uint32_t b = 0; b < NVN_UBO_SLOTS; b++)
            if (st->ubo[s][b].addr && st->ubo[s][b].size && tr.nbuf < sizeof tr.buf / sizeof tr.buf[0]) {
                buf_entry *e = &tr.buf[tr.nbuf++];
                memset(e, 0, sizeof *e);
                e->stage = s;
                e->slot = b;
                e->addr = st->ubo[s][b].addr;
                e->size = st->ubo[s][b].size;
            }
        for (uint32_t k = 0; k < NVN_STORAGE_SLOTS; k++) {
            uint64_t addr = s == NVN_STAGE_COMPUTE ? st->storage[k].addr : st->gstorage[s][k].addr;
            uint64_t size = s == NVN_STAGE_COMPUTE ? st->storage[k].size : st->gstorage[s][k].size;
            if (addr && size && tr.nbuf < sizeof tr.buf / sizeof tr.buf[0]) {
                buf_entry *e = &tr.buf[tr.nbuf++];
                memset(e, 0, sizeof *e);
                e->stage = s;
                e->slot = k;
                e->storage = true;
                e->addr = addr;
                e->size = size;
            }
        }
    }
    for (uint32_t i = 0; i < tr.nbuf; i++) {
        buf_entry *e = &tr.buf[i];
        e->guest = nvn_gpu_guest(e->addr);
        e->unmapped = !nvn_gpu_mapped(e->addr, e->size > BUF_CAP ? BUF_CAP : e->size);
        if (e->unmapped) {
            char where[512];
            nvn_gpu_describe(e->addr, where, sizeof where);
            rl_log(RL_LOG_WARN, "nvn", "trace: present %llu %s stage %u %s slot %u at 0x%llx (0x%llx bytes) is not in one memory pool: %s",
                   (unsigned long long)g.presents, tr.kind, e->stage, e->storage ? "storage" : "uniform", e->slot,
                   (unsigned long long)e->addr, (unsigned long long)e->size, where);
        }
    }
}

void nvn_trace_begin(replay *r, const char *kind)
{
    nvn_gfx *st = r->st;
    tr.kind = kind;
    tr.ntex = 0;
    tr.nrt = 0;
    if (!nvn_vk_flush(r))
        return;
    bool compute = !strcmp(kind, "dispatch");
    collect_buffers(st, compute ? NVN_STAGE_COMPUTE : 0, compute ? NVN_STAGE_COMPUTE : NVN_STAGE_COMPUTE - 1);
    for (uint32_t i = 0; i < tr.nbuf; i++)
        dump_range(tr.buf[i].addr, tr.buf[i].size, tr.buf[i].pre);
    if (!compute) {
        for (uint32_t i = 0; i < st->ncolor; i++)
            if (st->color[i].v) {
                tr.rt[tr.nrt].v = st->color[i].v;
                tr.rt[tr.nrt].level = st->color[i].level;
                tr.nrt++;
            }
        if (st->depth.v) {
            tr.rt[tr.nrt].v = st->depth.v;
            tr.rt[tr.nrt].level = st->depth.level;
            tr.nrt++;
        }
        for (uint32_t i = 0; i < tr.nrt; i++)
            dump_image(r, tr.rt[i].v, false, tr.rt[i].pre);
    }
}

void nvn_trace_texture(replay *r, vimg *v, uint32_t id, uint32_t slot, uint32_t kind, uint32_t dim, bool image,
                       const uint8_t view[17])
{
    if (!v)
        return;
    for (uint32_t i = 0; i < tr.ntex; i++)
        if (tr.tex[i].v == v && tr.tex[i].slot == slot)
            return;
    if (tr.ntex == MAX_TEX)
        return;
    tex_entry *e = &tr.tex[tr.ntex++];
    memset(e, 0, sizeof *e);
    e->v = v;
    e->id = id;
    e->slot = slot;
    e->kind = kind;
    e->dim = dim;
    e->image = image;
    memcpy(e->view, view, 17);
    dump_image(r, v, !image && !is_target(v), e->pre);
}

static void hex(FILE *f, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%02x", p[i]);
}

static void json_file(FILE *f, const char *key, const char *name)
{
    if (name[0])
        fprintf(f, ",\"%s\":\"%s\"", key, name);
}

void nvn_trace_end(replay *r, const nvn_trace_cmd *c)
{
    FILE *f = tr.out;
    const nvn_gfx *st = r->st;
    if (!f)
        return;
    bool compute = !strcmp(tr.kind, "dispatch");
    nvn_vk_flush(r);
    for (uint32_t i = 0; i < tr.nbuf; i++)
        dump_range(tr.buf[i].addr, tr.buf[i].size, tr.buf[i].post);
    for (uint32_t i = 0; i < tr.nrt; i++)
        dump_image(r, tr.rt[i].v, false, tr.rt[i].post);
    for (uint32_t i = 0; i < tr.ntex; i++)
        dump_image(r, tr.tex[i].v, !tr.tex[i].image && !is_target(tr.tex[i].v), tr.tex[i].post);
    fprintf(f, "{\"i\":%llu,\"frame\":%llu,\"kind\":\"%s\"", (unsigned long long)tr.cmd++,
            (unsigned long long)g.presents, tr.kind);
    fprintf(f, ",\"queue\":\"0x%llx\",\"submit\":%llu,\"pos\":%u,\"seq\":%llu", (unsigned long long)r->queue,
            (unsigned long long)r->submit, r->pos, (unsigned long long)r->seq);
    if (compute) {
        fprintf(f, ",\"groups\":[%u,%u,%u]", c->groups[0], c->groups[1], c->groups[2]);
        char name[40] = "";
        if (c->c0) {
            put_file("bufs", "bin", c->c0, c->c0_bytes, name);
            json_file(f, "c0", name);
        }
        fprintf(f, ",\"specs\":[");
        for (uint32_t i = 0; i < c->nspec; i++)
            fprintf(f, "%s[%u,%u]", i ? "," : "", c->spec_map[i].constantID,
                    c->spec_vals[c->spec_map[i].offset / 4]);
        fprintf(f, "]");
        if (c->c1_guest && c->c1_size && c->c1_size <= BUF_CAP) {
            put_file("bufs", "bin", rl_mem_base() + c->c1_guest, c->c1_size, name);
            fprintf(f, ",\"c1_guest\":\"0x%llx\"", (unsigned long long)c->c1_guest);
            json_file(f, "c1", name);
        }
    }
    else
        fprintf(f, ",\"mode\":%u,\"indexed\":%d,\"index_type\":%u,\"base_vertex\":%d,\"first\":%u,\"count\":%u,"
                   "\"first_instance\":%u,\"instances\":%u",
                c->mode, (int)c->indexed, c->itype, c->base, c->first, c->count, st->inst_base, st->inst_count);
    fprintf(f, ",\"programs\":{");
    bool first = true;
    static const char *const names[] = { "vertex", "fragment", "geometry", "tess_control", "tess_eval", "compute" };
    for (uint32_t s = 0; s < NVN_STAGES; s++)
        if (c->keys[s]) {
            fprintf(f, "%s\"%s\":{\"key\":\"%s\",\"code\":\"0x%llx\"}", first ? "" : ",", names[s], c->keys[s],
                    (unsigned long long)st->code[s]);
            first = false;
        }
    fprintf(f, "}");
    if (!compute) {
        fprintf(f, ",\"viewport\":[%g,%g,%g,%g],\"depth_range\":[%g,%g],\"scissor\":[%d,%d,%u,%u],\"scissor_set\":%d",
                (double)st->viewports[0][0], (double)st->viewports[0][1], (double)st->viewports[0][2],
                (double)st->viewports[0][3], (double)st->depth_ranges[0][0], (double)st->depth_ranges[0][1],
                st->scissors[0].offset.x, st->scissors[0].offset.y, st->scissors[0].extent.width,
                st->scissors[0].extent.height, (int)st->scissor_set);
        fprintf(f, ",\"state\":{\"depth_stencil\":\"");
        hex(f, st->depth_stencil, sizeof st->depth_stencil);
        fprintf(f, "\",\"color_state\":\"");
        hex(f, st->color_state, sizeof st->color_state);
        fprintf(f, "\",\"channel_mask\":\"");
        hex(f, st->channel_mask, sizeof st->channel_mask);
        fprintf(f, "\",\"polygon\":\"");
        hex(f, st->polygon, sizeof st->polygon);
        fprintf(f, "\",\"multisample\":\"");
        hex(f, st->multisample, sizeof st->multisample);
        fprintf(f, "\",\"blend\":[");
        for (uint32_t i = 0; i < st->ncolor; i++) {
            fprintf(f, "%s\"", i ? "," : "");
            hex(f, st->blend[i], sizeof st->blend[i]);
            fprintf(f, "\"");
        }
        fprintf(f, "],\"stencil_ref\":[%u,%u],\"alpha_ref\":%g,\"point_size\":%g,\"sample_mask\":%u,\"render_mode\":%u,"
                   "\"rasterizer_discard\":%d,\"primitive_restart\":%d,\"depth_clamp\":%d}",
                st->stencil_ref[0], st->stencil_ref[1], (double)st->alpha_ref, (double)st->point_size,
                st->sample_mask, st->render_mode, (int)st->rasterizer_discard, (int)st->primitive_restart,
                (int)st->depth_clamp);
        fprintf(f, ",\"targets\":[");
        for (uint32_t i = 0; i < tr.nrt; i++) {
            const vimg *v = tr.rt[i].v;
            fprintf(f,
                    "%s{\"depth\":%d,\"vimg\":\"%p\",\"storage\":\"0x%llx\",\"format\":%u,\"vk_format\":%d,"
                    "\"width\":%u,\"height\":%u,\"level\":%u",
                    i ? "," : "", (int)(v->aspect == VK_IMAGE_ASPECT_DEPTH_BIT), (const void *)v,
                    (unsigned long long)v->storage, v->d.format, (int)v->format, v->d.width, v->d.height,
                    tr.rt[i].level);
            json_file(f, "pre", tr.rt[i].pre);
            json_file(f, "post", tr.rt[i].post);
            fprintf(f, "}");
        }
        fprintf(f, "]");
    }
    fprintf(f, ",\"buffers\":[");
    for (uint32_t i = 0; i < tr.nbuf; i++) {
        const buf_entry *e = &tr.buf[i];
        fprintf(f, "%s{\"stage\":\"%s\",\"kind\":\"%s\",\"slot\":%u,\"gpu\":\"0x%llx\",\"guest\":\"0x%llx\",\"size\":%llu",
                i ? "," : "", names[e->stage], e->storage ? "storage" : "uniform", e->slot,
                (unsigned long long)e->addr, (unsigned long long)e->guest, (unsigned long long)e->size);
        if (e->unmapped)
            fprintf(f, ",\"unmapped\":1");
        json_file(f, "pre", e->pre);
        json_file(f, "post", e->post);
        fprintf(f, "}");
    }
    fprintf(f, "],\"textures\":[");
    for (uint32_t i = 0; i < tr.ntex; i++) {
        const tex_entry *e = &tr.tex[i];
        const vimg *v = e->v;
        fprintf(f,
                "%s{\"id\":%u,\"slot\":%u,\"image\":%d,\"vimg\":\"%p\",\"storage\":\"0x%llx\",\"format\":%u,"
                "\"vk_format\":%d,\"width\":%u,\"height\":%u,\"depth\":%u,\"levels\":%u,\"target\":%u,\"kind\":%u,"
                "\"dim\":%u,\"desc_swizzle\":[%u,%u,%u,%u],\"view\":\"",
                i ? "," : "", e->id, e->slot, (int)e->image, (const void *)v, (unsigned long long)v->storage,
                v->d.format, (int)v->format, v->d.width, v->d.height, v->d.depth, v->d.levels, v->d.target, e->kind,
                e->dim, v->d.swizzle[0], v->d.swizzle[1], v->d.swizzle[2], v->d.swizzle[3]);
        hex(f, e->view, 17);
        fprintf(f, "\"");
        json_file(f, "pre", e->pre);
        json_file(f, "post", e->post);
        fprintf(f, "}");
    }
    fprintf(f, "],\"push\":{");
    first = true;
    for (uint32_t s = 0; s < NVN_STAGES; s++) {
        bool cs = s == NVN_STAGE_COMPUTE;
        if (!c->keys[s] || !(cs ? st->push_used[s] : st->gfx_push_used))
            continue;
        fprintf(f, "%s\"%s\":\"", first ? "" : ",", names[s]);
        hex(f, cs ? st->push[s][0] : st->gfx_push[0], NVN_PUSH_BYTES);
        fprintf(f, "\"");
        first = false;
    }
    fprintf(f, "}}\n");
    fflush(f);
}
