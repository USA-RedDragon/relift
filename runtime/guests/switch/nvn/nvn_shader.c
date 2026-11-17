/**
 * @file nvn_shader.c
 * @brief The game's SM86 programs and their SPIR-V translations.
 *
 * A program is its shader program header (when it has one) and its code
 * through the first BRA to itself, read at the GPU address nvnProgramSetShaders
 * names; its key is the SHA-256 of those bytes. Translations come from the
 * tables relift build wrote to the build's shader folder (table.rlspv, the
 * SPIR-V, read a module at a time; table.rlspi, what each translation's
 * interface needs), both sorted by key. A program the table lacks is written
 * to <folder>/programs/<key>.bin when the game sets it, and the host
 * device's capabilities to <folder>/device.json (RELIFT_NVN_SHADERS
 * overrides the folder), so the next relift build translates it for that
 * device; a draw that needs it stops the game.
 */
#include "nvn_priv.h"

#include <relift/datacheck.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#if defined(_WIN32)
#include <direct.h>
#endif

#define SPH_BYTES 0x80u
#define MAX_PROGRAM_BYTES (4u << 20)

static const uint8_t bra_self[12] = { 0x47, 0x79, 0, 0, 0xf0, 0xff, 0xff, 0xff, 0xff, 0xff, 0x83, 0x03 };

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

#define TABLE_HEAD 12u
#define TABLE_ENTRY 40u

const char *nvn_shader_dir(void)
{
    const char *d = getenv("RELIFT_NVN_SHADERS");
    if (d && *d)
        return d;
    d = rl_switch_shaders_dir();
    return d && *d ? d : "shaders";
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/** @brief A table file: its head (magic, count, entries) in memory, the file for the rest. */
typedef struct table {
    bool tried;
    FILE *f;
    uint8_t *head;
    uint32_t n;
    /** @brief For the interface table: all of it (the records follow the entries). */
    uint8_t *all;
    size_t len;
} table;

static table g_spv, g_iface;

static bool seek(FILE *f, uint64_t at)
{
#if defined(_WIN32)
    return _fseeki64(f, (long long)at, SEEK_SET) == 0;
#else
    return fseeko(f, (off_t)at, SEEK_SET) == 0;
#endif
}

/** @brief Opens <dir>/name (magic) once; whole reads all of it into memory. */
static table *open_table(table *t, const char *name, const char *magic, bool whole)
{
    if (t->tried)
        return t->head ? t : NULL;
    t->tried = true;
    char path[1100];
    snprintf(path, sizeof path, "%s/%s", nvn_shader_dir(), name);
    FILE *f = fopen(path, "rb");
    uint8_t h[TABLE_HEAD];
    if (!f)
        return NULL;
    if (fread(h, 1, TABLE_HEAD, f) != TABLE_HEAD || memcmp(h, magic, 8)) {
        rl_log(RL_LOG_ERROR, "nvn", "%s is not a shader table", path);
        fclose(f);
        return NULL;
    }
    uint32_t n = le32(h + 8);
    size_t hb = TABLE_HEAD + (size_t)n * TABLE_ENTRY;
    if (whole) {
        if (!seek(f, 0) || fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            return NULL;
        }
        long end = ftell(f);
        t->len = end > 0 ? (size_t)end : 0;
        t->all = t->len >= hb ? malloc(t->len) : NULL;
        if (!t->all || !seek(f, 0) || fread(t->all, 1, t->len, f) != t->len) {
            free(t->all);
            t->all = NULL;
            fclose(f);
            rl_log(RL_LOG_ERROR, "nvn", "cannot read %s", path);
            return NULL;
        }
        fclose(f);
        t->head = t->all;
    } else {
        t->head = malloc(hb);
        if (!t->head || !seek(f, 0) || fread(t->head, 1, hb, f) != hb) {
            free(t->head);
            t->head = NULL;
            fclose(f);
            rl_log(RL_LOG_ERROR, "nvn", "cannot read %s", path);
            return NULL;
        }
        t->f = f;
    }
    t->n = n;
    return t;
}

/** @brief The entry of key (hex) in t (sorted 40-byte entries of a 32-byte key and two u32), or NULL. */
static const uint8_t *table_find(const table *t, const char *key)
{
    uint8_t k[32];
    for (int i = 0; i < 32; i++) {
        int hi = hexv(key[2 * i]), lo = hexv(key[2 * i + 1]);
        if (hi < 0 || lo < 0)
            return NULL;
        k[i] = (uint8_t)(hi << 4 | lo);
    }
    uint32_t lo = 0, hi = t->n;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        const uint8_t *e = t->head + TABLE_HEAD + (size_t)mid * TABLE_ENTRY;
        int c = memcmp(e, k, 32);
        if (!c)
            return e;
        if (c < 0)
            lo = mid + 1;
        else
            hi = mid;
    }
    return NULL;
}

static const uint8_t *spirv_entry(const char *key)
{
    pthread_mutex_lock(&g_lock);
    table *t = open_table(&g_spv, "table.rlspv", "RLSPV001", false);
    pthread_mutex_unlock(&g_lock);
    return t ? table_find(t, key) : NULL;
}

static bool has_sph(const uint8_t *b)
{
    uint32_t w;
    memcpy(&w, b, 4);
    uint32_t kind = w & 0x1f, version = (w >> 5) & 0x1f, stage = (w >> 10) & 0xf;
    return (kind == 1 || kind == 2) && version == 4 && stage >= 1 && stage <= 5;
}

int nvn_shader_stage(const uint8_t *mem, uint64_t code)
{
    const uint8_t *b = mem + code;
    if (!has_sph(b))
        return 5;
    uint32_t w;
    memcpy(&w, b, 4);
    static const int map[] = { -1, 0, 3, 4, 2, 1 };
    return map[(w >> 10) & 0xf];
}

uint64_t nvn_shader_extent(const uint8_t *mem, uint64_t code)
{
    const uint8_t *b = mem + code;
    uint64_t at = has_sph(b) ? SPH_BYTES : 0;
    for (; at + 16 <= MAX_PROGRAM_BYTES; at += 16)
        if (!memcmp(b + at, bra_self, sizeof bra_self))
            return at + 16;
    return 0;
}

static void make_dir(const char *p)
{
#if defined(_WIN32)
    _mkdir(p);
#else
    mkdir(p, 0777);
#endif
}

void nvn_mkdirs(const char *dir)
{
    char p[1024];
    snprintf(p, sizeof p, "%s", dir);
    for (char *s = p + 1; *s; s++)
        if (*s == '/') {
            *s = 0;
            make_dir(p);
            *s = '/';
        }
    make_dir(p);
}

static bool exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

bool nvn_shader_key(const uint8_t *mem, uint64_t code, char key[65], uint64_t *len)
{
    *len = nvn_shader_extent(mem, code);
    if (!*len)
        return false;
    rl_data_sha256(mem + code, (size_t)*len, key);
    return true;
}

bool nvn_shader_note(const uint8_t *mem, uint64_t code, char key[65])
{
    uint64_t len;
    if (!nvn_shader_key(mem, code, key, &len))
        return false;
    if (spirv_entry(key))
        return true;
    const char *dir = nvn_shader_dir();
    char sub[1024], bin[1100];
    snprintf(sub, sizeof sub, "%s/programs", dir);
    snprintf(bin, sizeof bin, "%s/%s.bin", sub, key);
    pthread_mutex_lock(&g_lock);
    if (!exists(bin)) {
        nvn_mkdirs(sub);
        FILE *f = fopen(bin, "wb");
        if (f) {
            fwrite(mem + code, 1, (size_t)len, f);
            fclose(f);
        } else {
            rl_log(RL_LOG_ERROR, "nvn", "cannot write %s: %s", bin, strerror(errno));
        }
    }
    pthread_mutex_unlock(&g_lock);
    return true;
}

uint32_t *nvn_shader_spirv(const char *key, size_t *words)
{
    const uint8_t *e = spirv_entry(key);
    *words = 0;
    if (!e)
        return NULL;
    uint32_t at = le32(e + 32), n = le32(e + 36);
    uint32_t *w = malloc((size_t)n * 4);
    pthread_mutex_lock(&g_lock);
    bool ok = w && seek(g_spv.f, at) && fread(w, 4, n, g_spv.f) == n;
    pthread_mutex_unlock(&g_lock);
    if (!ok) {
        free(w);
        rl_log(RL_LOG_ERROR, "nvn", "cannot read translation %s from %s/table.rlspv", key, nvn_shader_dir());
        return NULL;
    }
    *words = n;
    return w;
}

uint32_t nvn_shader_iface(const char *key, nvn_iface_rec *out, uint32_t max)
{
    pthread_mutex_lock(&g_lock);
    table *t = open_table(&g_iface, "table.rlspi", "RLSPI001", true);
    pthread_mutex_unlock(&g_lock);
    const uint8_t *e = t ? table_find(t, key) : NULL;
    if (!e)
        return 0;
    uint32_t first = le32(e + 32), count = le32(e + 36);
    size_t recs = TABLE_HEAD + (size_t)t->n * TABLE_ENTRY, room = (t->len - recs) / 16;
    if (first > room || count > room - first)
        return 0;
    for (uint32_t i = 0; i < count && i < max; i++) {
        const uint8_t *r = t->all + recs + 16 * ((size_t)first + i);
        out[i] = (nvn_iface_rec){ le32(r), le32(r + 4), le32(r + 8), le32(r + 12) };
    }
    return count < max ? count : max;
}
