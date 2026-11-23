/**
 * @file nvn_shader.c
 * @brief The game's SM86 (NVN2) and SM53 (NVN1) programs and their SPIR-V
 * translations.
 *
 * An NVN2 program is its shader program header (when it has one) and its
 * code through the first BRA to itself, read at the GPU address
 * nvnProgramSetShaders names; an NVN1 program is what follows its code
 * header there (nvn_v1_program); its key is the SHA-256 of those bytes.
 * Translations come from the tables relift build wrote, table.rlspv (the
 * SPIR-V, read a module at a time) and table.rlspi (what each translation's
 * interface needs), both sorted by key, in the table folder. table.rlspv is
 * "RLSPV002" (each module a Zstandard frame; entries of the key, the frame's
 * u64 offset, its u32 length and the module's u32 length in words) or the older
 * uncompressed "RLSPV001" (entries of the key, a u32 offset and the length in
 * words).
 *
 * The table folder's table.stamp names the translator that made the tables;
 * the build hands the program the stamp it expects (rl_nvn_set_table_stamp),
 * and nvn_shader_stamp_check refuses tables of another translator. With the
 * gpu.math setting at native the run reads table-native.rlspv,
 * table-native.rlspi and table-native.stamp instead, translated with the
 * host GPU's own float math, whose stamp is the expected one with -native
 * after it. pick_tables is the one place that choice is made.
 *
 * With the translator linked (nvn_xlate.c), tables that do not fit the device and translator are made again on the
 * device, and every read of a table waits for that first.
 *
 * Nothing in the program names the build machine's folders. The table folder
 * is $RELIFT_NVN_TABLES when set (what the launchers of a packaged app and
 * the mobile entries set), else the shader folder. The shader folder is
 * $RELIFT_NVN_SHADERS when set (the launchers and mobile entries point it at
 * the user's own data folder), else "shaders" beside the executable, which is
 * where relift build puts it next to the program. The build finds every
 * program in the game's files and translates it, so a program the table
 * lacks is a stop with a report: the scan did not find it. A development
 * run with $RELIFT_NVN_RECORD set also writes each program the game sets to
 * <shader folder>/programs/<key>.bin, a tool for finding what the scan
 * misses; the host device's capabilities go to <shader folder>/device.json
 * for the build to translate for.
 */
#include "nvn_priv.h"

#include <relift/datacheck.h>
#include <relift/settings.h>

#include "zstd.h"

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
#define TABLE_ENTRY_ZSTD 48u

static char g_default_dir[1100];
static pthread_once_t g_default_once = PTHREAD_ONCE_INIT;

static void default_dir(void)
{
    char exe[1024];
    if (rl_exe_dir(exe, sizeof exe))
        snprintf(g_default_dir, sizeof g_default_dir, "%s/shaders", exe);
    else
        snprintf(g_default_dir, sizeof g_default_dir, "shaders");
}

const char *nvn_shader_dir(void)
{
    const char *d = getenv("RELIFT_NVN_SHADERS");
    if (d && *d)
        return d;
    pthread_once(&g_default_once, default_dir);
    return g_default_dir;
}

const char *nvn_shader_table_dir(void)
{
    const char *made = nvn_xlate_table_dir();
    if (made)
        return made;
    const char *d = getenv("RELIFT_NVN_TABLES");
    return d && *d ? d : nvn_shader_dir();
}

static int hexv(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint64_t le64(const uint8_t *p) { return le32(p) | (uint64_t)le32(p + 4) << 32; }

/** @brief A table file: its head (magic, count, entries) in memory, the file
 * for the rest. */
typedef struct table {
    bool tried;
    FILE *f;
    uint8_t *head;
    uint32_t n;
    /** @brief For the interface table: all of it (the records follow the
     * entries). */
    uint8_t *all;
    size_t len;
    /**
     * @brief For the SPIR-V table: each entry's offset in 64 bits. The file
     * keeps 32 (relift-gpu-sm86's Table::to_bytes) and lays the modules out
     * back to back in entry order, so the offsets are the running sum of the
     * lengths; each stored offset must equal the sum's low 32 bits.
     */
    uint64_t *at;
    /** @brief Bytes per entry: 48 for a compressed table, else 40. */
    uint32_t entry;
    bool zstd;
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

/** @brief The tables the run reads (pick_tables). */
static struct {
    bool native;
    /** @brief The file names' stem: "table" or "table-native". */
    char base[16];
} g_tables;
static pthread_mutex_t g_tables_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_tables_picked;
static rl_nvn_table_maker g_table_maker;

static const char *g_expected_stamp;
/** @brief 0 not checked yet, 1 the stamp is accepted, 2 it is not. */
static int g_stamp_state;
static char g_stamp_why[512];

void rl_nvn_set_table_stamp(const char *stamp)
{
    pthread_mutex_lock(&g_lock);
    g_expected_stamp = stamp;
    g_stamp_state = 0;
    pthread_mutex_unlock(&g_lock);
}

void rl_nvn_set_table_maker(rl_nvn_table_maker make) { g_table_maker = make; }

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/**
 * @brief Picks the tables the run reads, once: the native-math ones when
 * gpu.math is native, else the exact ones. A native table that is missing goes
 * to the table maker (rl_nvn_set_table_maker) when one is set; without one, or
 * when it makes none, the stamp check stops the run with the reason.
 */
static void pick_tables_locked(void)
{
    if (g_tables_picked)
        return;
    g_tables_picked = true;
    g_tables.native = rl_setting_enum("gpu.math") == 1;
    snprintf(g_tables.base, sizeof g_tables.base, "%s", g_tables.native ? "table-native" : "table");
    char want[96];
    const char *expected = g_expected_stamp;
    bool stamped = expected != NULL;
    snprintf(want, sizeof want, "%s-native", stamped ? expected : "");
    const char *dir = nvn_shader_table_dir();
    char path[1100];
    snprintf(path, sizeof path, "%s/%s.rlspv", dir, g_tables.base);
    if (g_tables.native && !file_exists(path)) {
        rl_log(RL_LOG_WARN, "nvn", "gpu.math is native but %s has no native-math shader table (%s)", dir, path);
        if (g_table_maker && g_table_maker(dir, g_tables.base, "native", stamped ? want : NULL))
            rl_log(RL_LOG_INFO, "nvn", "native-math shader table made in %s", dir);
    }
    rl_log(RL_LOG_INFO, "nvn", "shader tables: %s float math (%s/%s.*)", g_tables.native ? "native" : "exact", dir,
           g_tables.base);
}

static void pick_tables(void)
{
    pthread_mutex_lock(&g_tables_lock);
    pick_tables_locked();
    pthread_mutex_unlock(&g_tables_lock);
}

void nvn_shader_tables_repick(void)
{
    pthread_mutex_lock(&g_tables_lock);
    g_tables_picked = false;
    pthread_mutex_unlock(&g_tables_lock);
    pthread_mutex_lock(&g_lock);
    g_stamp_state = 0;
    pthread_mutex_unlock(&g_lock);
}

/** @brief The path of the picked tables' file with extension ext. */
static void table_path(char *out, size_t n, const char *ext)
{
    pick_tables();
    snprintf(out, n, "%s/%s.%s", nvn_shader_table_dir(), g_tables.base, ext);
}

bool nvn_shader_math_native(void)
{
    pick_tables();
    return g_tables.native;
}

const char *nvn_shader_expected_stamp(void)
{
    pthread_mutex_lock(&g_lock);
    const char *s = g_expected_stamp;
    pthread_mutex_unlock(&g_lock);
    return s;
}

/** @brief Reads the table folder's table.stamp (trimmed) into out; false
 * without one. */
static bool read_stamp(char *out, size_t size)
{
    char path[1100];
    table_path(path, sizeof path, "stamp");
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    size_t n = fread(out, 1, size - 1, f);
    fclose(f);
    while (n && (out[n - 1] == '\n' || out[n - 1] == '\r' || out[n - 1] == ' '))
        n--;
    out[n] = 0;
    return n > 0;
}

bool nvn_shader_stamp_check(char *why, size_t why_n)
{
    pick_tables();
    nvn_xlate_wait();
    pthread_mutex_lock(&g_lock);
    if (!g_stamp_state) {
        char have[96], wantbuf[96];
        bool stamped = read_stamp(have, sizeof have);
        const char *want = NULL;
        if (g_expected_stamp) {
            snprintf(wantbuf, sizeof wantbuf, "%s%s", g_expected_stamp, g_tables.native ? "-native" : "");
            want = wantbuf;
        }
        const char *dir = nvn_shader_table_dir();
        const char *allow = getenv("RELIFT_NVN_TABLE_STAMP");
        bool allowed = allow && !strcmp(allow, "allow");
        g_stamp_state = 1;
        if (stamped)
            rl_log(RL_LOG_INFO, "nvn", "shader tables in %s: translator stamp %s (this build expects %s)", dir, have,
                   want ? want : "none");
        else
            rl_log(RL_LOG_INFO, "nvn", "shader tables in %s: no translator stamp (this build expects %s)", dir,
                   want ? want : "none");
        char path[1100], magic[8] = { 0 };
        table_path(path, sizeof path, "rlspi");
        FILE *spi = fopen(path, "rb");
        bool old = spi && fread(magic, 1, 8, spi) == 8 && !memcmp(magic, "RLSPI001", 8);
        if (spi)
            fclose(spi);
        if (old) {
            snprintf(g_stamp_why, sizeof g_stamp_why,
                     "the shader tables in %s lack the storage image array records (interface table RLSPI001, this "
                     "runtime needs RLSPI002): they were made by an older relift; run relift shaders with this one",
                     dir);
            g_stamp_state = 2;
            rl_log(RL_LOG_ERROR, "nvn", "%s", g_stamp_why);
        } else if (want && (!stamped || strcmp(have, want))) {
            char made[128];
            if (stamped)
                snprintf(made, sizeof made, "were made by translator %s", have);
            else if (g_tables.native)
                snprintf(made, sizeof made,
                         "have no native-math table (gpu.math is native; no translator "
                         "stamp in table-native.stamp)");
            else
                snprintf(made, sizeof made, "have no translator stamp (made by an older relift)");
            snprintf(g_stamp_why, sizeof g_stamp_why,
                     "shader table stamp mismatch: the tables in %s %s, but this "
                     "program was built by translator %s; "
                     "run relift shaders and relink with one relift, or set "
                     "RELIFT_NVN_TABLE_STAMP=allow for a "
                     "deliberate A/B run",
                     dir, made, want);
            if (allowed)
                rl_log(RL_LOG_WARN, "nvn", "%s (allowed by RELIFT_NVN_TABLE_STAMP=allow)", g_stamp_why);
            else {
                g_stamp_state = 2;
                rl_log(RL_LOG_ERROR, "nvn", "%s", g_stamp_why);
            }
        }
    }
    bool ok = g_stamp_state == 1;
    if (!ok && why && why_n)
        snprintf(why, why_n, "%s", g_stamp_why);
    pthread_mutex_unlock(&g_lock);
    return ok;
}

/** @brief Opens the picked table file with extension ext (magic) once; whole
 * reads all of it into memory. */
static table *open_table(table *t, const char *ext, const char *magic, bool whole)
{
    if (t->tried)
        return t->head ? t : NULL;
    t->tried = true;
    char path[1100];
    table_path(path, sizeof path, ext);
    FILE *f = fopen(path, "rb");
    uint8_t h[TABLE_HEAD];
    if (!f)
        return NULL;
    bool read = fread(h, 1, TABLE_HEAD, f) == TABLE_HEAD;
    bool zstd = read && !strcmp(magic, "RLSPV001") && !memcmp(h, "RLSPV002", 8);
    if (!read || (!zstd && memcmp(h, magic, 8))) {
        rl_log(RL_LOG_ERROR, "nvn", "%s is not a shader table", path);
        fclose(f);
        return NULL;
    }
    uint32_t n = le32(h + 8);
    t->zstd = zstd;
    t->entry = zstd ? TABLE_ENTRY_ZSTD : TABLE_ENTRY;
    size_t hb = TABLE_HEAD + (size_t)n * t->entry;
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
        t->at = malloc(((size_t)n + 1) * sizeof *t->at);
        uint64_t at = hb;
        for (uint32_t i = 0; t->at && zstd && i < n; i++)
            t->at[i] = le64(t->head + TABLE_HEAD + (size_t)i * t->entry + 32);
        for (uint32_t i = 0; t->at && !zstd && i < n; i++) {
            const uint8_t *e = t->head + TABLE_HEAD + (size_t)i * TABLE_ENTRY;
            if (le32(e + 32) != (uint32_t)at) {
                rl_log(RL_LOG_ERROR, "nvn", "%s: entry %u is not where the entries before it end", path, i);
                free(t->at);
                t->at = NULL;
                break;
            }
            t->at[i] = at;
            at += 4ull * le32(e + 36);
        }
        if (!t->at) {
            free(t->head);
            t->head = NULL;
            fclose(f);
            return NULL;
        }
        t->f = f;
    }
    t->n = n;
    return t;
}

/** @brief The entry of key (hex) in t (sorted 40-byte entries of a 32-byte key
 * and two u32), or NULL. */
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
        const uint8_t *e = t->head + TABLE_HEAD + (size_t)mid * t->entry;
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

bool nvn_shader_refusal(const char *key, char *why, size_t why_n)
{
    char path[1200], line[1024];
    snprintf(path, sizeof path, "%s/failed.txt", nvn_shader_table_dir());
    FILE *f = fopen(path, "r");
    bool found = false;
    while (f && !found && fgets(line, sizeof line, f)) {
        if (strncmp(line, key, 64) || line[64] != '\t')
            continue;
        line[strcspn(line, "\r\n")] = 0;
        snprintf(why, why_n, "%s", line + 65);
        found = true;
    }
    if (f)
        fclose(f);
    return found;
}

void nvn_shader_tables_reopen(void)
{
    pthread_mutex_lock(&g_lock);
    /* What was read of the old tables stays allocated: a lookup that began before may still hold a pointer into it. */
    memset(&g_spv, 0, sizeof g_spv);
    memset(&g_iface, 0, sizeof g_iface);
    g_stamp_state = 0;
    pthread_mutex_unlock(&g_lock);
}

static const uint8_t *spirv_entry(const char *key)
{
    nvn_xlate_wait();
    pthread_mutex_lock(&g_lock);
    table *t = open_table(&g_spv, "rlspv", "RLSPV001", false);
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
    if (nvn_generation() == 1) {
        int stage = 5;
        uint64_t start, len;
        if (nvn_v1_program(mem, code, &start, &len))
            nvn_v1_sph(mem + start, &stage);
        return stage;
    }
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

/** @brief Where the bytes of the program at code start and how many there are;
 * false when it has no end. */
static bool program_span(const uint8_t *mem, uint64_t code, uint64_t *start, uint64_t *len)
{
    if (nvn_generation() == 1)
        return nvn_v1_program(mem, code, start, len);
    *start = code;
    *len = nvn_shader_extent(mem, code);
    return *len != 0;
}

/**
 * @brief A program's key kept by its GPU address, so a draw does not hash the
 * program again: valid while the program's bytes equal the copy kept with it (a
 * compare runs many times faster than SHA-256 and is exact).
 */
typedef struct key_entry {
    uint64_t code, start, len;
    char key[65];
    uint8_t *bytes;
} key_entry;

static struct {
    pthread_mutex_t mu;
    key_entry *slots;
    uint32_t cap, n;
} g_keys = { PTHREAD_MUTEX_INITIALIZER, NULL, 0, 0 };

static uint32_t key_slot(uint64_t code, uint32_t cap)
{
    return (uint32_t)((code >> 4) * 0x9E3779B97F4A7C15ull >> 32) & (cap - 1);
}

static key_entry *key_find(uint64_t code)
{
    if (!g_keys.cap)
        return NULL;
    for (uint32_t i = key_slot(code, g_keys.cap);; i = (i + 1) & (g_keys.cap - 1)) {
        key_entry *e = &g_keys.slots[i];
        if (!e->code || e->code == code)
            return e;
    }
}

static bool key_grow(void)
{
    uint32_t cap = g_keys.cap ? g_keys.cap * 2 : 1024;
    key_entry *slots = calloc(cap, sizeof *slots);
    if (!slots)
        return false;
    for (uint32_t i = 0; i < g_keys.cap; i++) {
        key_entry *e = &g_keys.slots[i];
        if (!e->code)
            continue;
        uint32_t j = key_slot(e->code, cap);
        while (slots[j].code)
            j = (j + 1) & (cap - 1);
        slots[j] = *e;
    }
    free(g_keys.slots);
    g_keys.slots = slots;
    g_keys.cap = cap;
    return true;
}

bool nvn_shader_key(const uint8_t *mem, uint64_t code, char key[65], uint64_t *len)
{
    uint64_t start;
    if (!program_span(mem, code, &start, len))
        return false;
    pthread_mutex_lock(&g_keys.mu);
    key_entry *e = key_find(code);
    if (e && e->code && e->start == start && e->len == *len && memcmp(e->bytes, mem + start, *len) == 0) {
        memcpy(key, e->key, 65);
        pthread_mutex_unlock(&g_keys.mu);
        return true;
    }
    rl_data_sha256(mem + start, (size_t)*len, key);
    bool fresh = !e || !e->code;
    uint8_t *copy = malloc(*len ? *len : 1);
    if (copy && code && (!fresh || (g_keys.n + 1) * 2 <= g_keys.cap || key_grow())) {
        if (fresh) {
            e = key_find(code);
            e->code = code;
            g_keys.n++;
        }
        free(e->bytes);
        memcpy(copy, mem + start, *len);
        e->bytes = copy;
        e->start = start;
        e->len = *len;
        memcpy(e->key, key, 65);
    } else {
        free(copy);
    }
    pthread_mutex_unlock(&g_keys.mu);
    return true;
}

bool nvn_shader_note(const uint8_t *mem, uint64_t code, char key[65])
{
    uint64_t start, len;
    if (!program_span(mem, code, &start, &len))
        return false;
    rl_data_sha256(mem + start, (size_t)len, key);
    const char *record = getenv("RELIFT_NVN_RECORD");
    if (!record || !*record || spirv_entry(key))
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
            fwrite(mem + start, 1, (size_t)len, f);
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
    uint64_t at = g_spv.at[(size_t)(e - g_spv.head - TABLE_HEAD) / g_spv.entry];
    uint32_t n = g_spv.zstd ? le32(e + 44) : le32(e + 36);
    uint32_t stored = g_spv.zstd ? le32(e + 40) : 4 * n;
    uint32_t *w = malloc((size_t)n * 4);
    uint8_t *frame = g_spv.zstd ? malloc(stored) : (uint8_t *)w;
    pthread_mutex_lock(&g_lock);
    bool ok = w && frame && seek(g_spv.f, at) && fread(frame, 1, stored, g_spv.f) == stored;
    pthread_mutex_unlock(&g_lock);
    if (ok && g_spv.zstd) {
        size_t got = ZSTD_decompress(w, (size_t)n * 4, frame, stored);
        ok = !ZSTD_isError(got) && got == (size_t)n * 4;
    }
    if (g_spv.zstd)
        free(frame);
    if (!ok) {
        free(w);
        char path[1100];
        table_path(path, sizeof path, "rlspv");
        rl_log(RL_LOG_ERROR, "nvn", "cannot read translation %s from %s", key, path);
        return NULL;
    }
    *words = n;
    return w;
}

bool nvn_mesh_info_of(const nvn_iface_rec *recs, uint32_t n, nvn_mesh_info *out)
{
    *out = (nvn_mesh_info){ 0 };
    for (uint32_t i = 0; i < n; i++) {
        const nvn_iface_rec *r = &recs[i];
        if (r->kind == NVN_IFACE_MESH) {
            out->present = true;
            out->stage = r->bank & 0xff;
            out->native = (r->bank >> 8 & 1) != 0;
            out->indexed = (r->bank >> 9 & 1) != 0;
            out->slots = r->offset;
            out->topology = r->spec;
        } else if (r->kind == NVN_IFACE_MESH_LIMITS) {
            out->max_vertices = r->bank;
            out->max_primitives = r->offset;
            out->payload_words = r->spec;
        } else if (r->kind == NVN_IFACE_MESH_RECORD) {
            out->record_words = r->bank;
            out->index_words = r->offset;
            out->task_lanes = r->spec & 0xffff;
            out->mesh_lanes = r->spec >> 16;
        }
    }
    return out->present;
}

void nvn_shader_helper_key(const char *kind, const char *key, char hkey[65])
{
    static const char scan[] = "relift mesh scan", vertex[] = "relift mesh vertex";
    const char *what = !strcmp(kind, "scan") ? scan : vertex;
    size_t n = strlen(what);
    uint8_t buf[sizeof vertex + 32];
    memcpy(buf, what, n);
    for (uint32_t i = 0; i < 32; i++) {
        int hi = hexv(key[2 * i]), lo = hexv(key[2 * i + 1]);
        buf[n + i] = (uint8_t)(hi << 4 | lo);
    }
    rl_data_sha256(buf, n + 32, hkey);
}

uint32_t nvn_shader_iface(const char *key, nvn_iface_rec *out, uint32_t max)
{
    nvn_xlate_wait();
    pthread_mutex_lock(&g_lock);
    table *t = open_table(&g_iface, "rlspi", "RLSPI002", true);
    pthread_mutex_unlock(&g_lock);
    const uint8_t *e = t ? table_find(t, key) : NULL;
    if (!e)
        return 0;
    uint32_t first = le32(e + 32), count = le32(e + 36);
    size_t recs = TABLE_HEAD + (size_t)t->n * TABLE_ENTRY, room = (t->len - recs) / 16;
    if (first > room || count > room - first)
        return 0;
    for (uint32_t i = 0; out && i < count && i < max; i++) {
        const uint8_t *r = t->all + recs + 16 * ((size_t)first + i);
        out[i] = (nvn_iface_rec){ le32(r), le32(r + 4), le32(r + 8), le32(r + 12) };
    }
    return count;
}

bool nvn_shader_image_arrays(uint64_t *used)
{
    *used = 0;
    nvn_xlate_wait();
    pthread_mutex_lock(&g_lock);
    table *t = open_table(&g_iface, "rlspi", "RLSPI002", true);
    pthread_mutex_unlock(&g_lock);
    if (!t)
        return false;
    size_t recs = TABLE_HEAD + (size_t)t->n * TABLE_ENTRY;
    bool any = false;
    for (size_t at = recs; at + 16 <= t->len; at += 16) {
        uint32_t b = le32(t->all + at + 4);
        if (le32(t->all + at) != NVN_IFACE_IMAGE_BINDING || b < 32 || (b - 32) % 8 >= 6 || (b - 32) / 8 >= 9)
            continue;
        *used |= 1ull << ((b - 32) / 8 * 6 + (b - 32) % 8);
        any = true;
    }
    return any;
}

uint32_t nvn_shader_count(void)
{
    nvn_xlate_wait();
    pthread_mutex_lock(&g_lock);
    table *t = open_table(&g_spv, "rlspv", "RLSPV001", false);
    pthread_mutex_unlock(&g_lock);
    return t ? t->n : 0;
}

bool nvn_shader_key_at(uint32_t i, char key[65])
{
    nvn_xlate_wait();
    pthread_mutex_lock(&g_lock);
    table *t = open_table(&g_spv, "rlspv", "RLSPV001", false);
    pthread_mutex_unlock(&g_lock);
    if (!t || i >= t->n)
        return false;
    static const char hex[] = "0123456789abcdef";
    const uint8_t *e = t->head + TABLE_HEAD + (size_t)i * t->entry;
    for (int j = 0; j < 32; j++) {
        key[2 * j] = hex[e[j] >> 4];
        key[2 * j + 1] = hex[e[j] & 15];
    }
    key[64] = 0;
    return true;
}

bool nvn_shader_table_hash(char hex[65])
{
    nvn_xlate_wait();
    pthread_mutex_lock(&g_lock);
    table *t = open_table(&g_spv, "rlspv", "RLSPV001", false);
    pthread_mutex_unlock(&g_lock);
    if (!t)
        return false;
    size_t hb = TABLE_HEAD + (size_t)t->n * t->entry;
    char stamp[64] = "";
    read_stamp(stamp, sizeof stamp);
    uint8_t *buf = malloc(hb + sizeof stamp);
    if (!buf)
        return false;
    memcpy(buf, t->head, hb);
    memcpy(buf + hb, stamp, sizeof stamp);
    rl_data_sha256(buf, hb + sizeof stamp, hex);
    free(buf);
    return true;
}
