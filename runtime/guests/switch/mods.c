/**
 * @file mods.c
 * @brief Run-time data mods of a Switch program: layout detection, the romfs
 * layers and the cheats. Mod folders follow the Atmosphere layout
 * (contents/<title id>/romfs, cheats/<build id>.txt) and the Ryujinx layout
 * (mods/<mod name>/romfs, cheats/<build id>.txt). Cheat files use the text
 * format the Atmosphere documentation describes: [name] or {name} lines, each
 * followed by 8 digit hex words.
 */
#include "hos_mods.h"

#include "cheat_vm.h"
#include "hos.h"
#include "hid_priv.h"
#include "hos_fs.h"

#include <relift/input.h>

#include <ctype.h>
#include <stdarg.h>
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#define LIST_SEP ';'
#else
#define LIST_SEP ':'
#endif

#define MAX_CONFLICTS 64u
#define MAX_CONFLICT_TEXT 512u

typedef struct cheat {
    char *name;
    uint32_t *ops;
    size_t n;
    uint8_t *edge;
    bool dead;
    bool warned_code;
    bool warned_fault;
} cheat;

typedef struct mod {
    rl_hos_mod_info info;
    cheat *cheats;
    size_t ncheats;
} mod;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static mod *g_mods;
static size_t g_nmods;
static char g_conflicts[MAX_CONFLICTS][MAX_CONFLICT_TEXT];
static size_t g_nconflicts;
static uint64_t g_main_base;
static cheat_vm g_vm;
static uint64_t (*g_keys)(void *);
static void *g_keys_user;

static bool is_dir(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool join(char *out, size_t n, const char *a, const char *b)
{
    return snprintf(out, n, "%s/%s", a, b) < (int)n;
}

static bool is_title_name(const char *s)
{
    if (strlen(s) != 16)
        return false;
    for (int i = 0; i < 16; i++)
        if (!isxdigit((unsigned char)s[i]))
            return false;
    return true;
}

static bool has_content(const char *dir)
{
    char p[PATH_MAX];
    return (join(p, sizeof p, dir, "romfs") && is_dir(p)) || (join(p, sizeof p, dir, "cheats") && is_dir(p));
}

static void cheats_free(mod *m)
{
    for (size_t i = 0; i < m->ncheats; i++) {
        free(m->cheats[i].name);
        free(m->cheats[i].ops);
        free(m->cheats[i].edge);
    }
    free(m->cheats);
    m->cheats = NULL;
    m->ncheats = 0;
}

static void clear_locked(void)
{
    for (size_t i = 0; i < g_nmods; i++) {
        cheats_free(&g_mods[i]);
        free((char *)g_mods[i].info.name);
        free((char *)g_mods[i].info.path);
    }
    free(g_mods);
    g_mods = NULL;
    g_nmods = 0;
    g_nconflicts = 0;
    g_main_base = 0;
    memset(&g_vm, 0, sizeof g_vm);
}

void rl_hos_mods_clear(void)
{
    pthread_mutex_lock(&g_lock);
    clear_locked();
    pthread_mutex_unlock(&g_lock);
}

static const char *base_name(const char *path)
{
    const char *s = strrchr(path, '/');
    return s ? s + 1 : path;
}

static void add_mod(const char *path, int layout)
{
    char p[PATH_MAX];
    mod *grown = realloc(g_mods, (g_nmods + 1) * sizeof *g_mods);
    mod *m;
    if (!grown)
        return;
    g_mods = grown;
    m = &g_mods[g_nmods];
    memset(m, 0, sizeof *m);
    m->info.path = strdup(path);
    m->info.name = strdup(base_name(path));
    if (!m->info.path || !m->info.name) {
        free((char *)m->info.path);
        free((char *)m->info.name);
        return;
    }
    m->info.layout = layout;
    m->info.romfs = join(p, sizeof p, path, "romfs") && is_dir(p);
    m->info.has_cheats = join(p, sizeof p, path, "cheats") && is_dir(p);
    g_nmods++;
    rl_log(RL_LOG_INFO, "mods", "data mod %s (%s layout)%s%s", m->info.name,
           layout == RL_MOD_ATMOSPHERE ? "Atmosphere" : "Ryujinx", m->info.romfs ? ", romfs" : "",
           m->info.has_cheats ? ", cheats" : "");
}

static int name_cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }

static bool title_dir(const char *parent, uint64_t pid, char *out, size_t n)
{
    char name[17];
    snprintf(name, sizeof name, "%016llx", (unsigned long long)pid);
    if (join(out, n, parent, name) && is_dir(out))
        return true;
    for (char *c = name; *c; c++)
        *c = (char)toupper((unsigned char)*c);
    return join(out, n, parent, name) && is_dir(out);
}

static void add_children(const char *dir)
{
    char **names = NULL;
    size_t n = 0, cap = 0;
    DIR *d = opendir(dir);
    struct dirent *de;
    while (d && (de = readdir(d))) {
        char p[PATH_MAX];
        if (de->d_name[0] == '.' || !join(p, sizeof p, dir, de->d_name) || !is_dir(p) || !has_content(p))
            continue;
        if (n == cap) {
            char **g = realloc(names, (cap ? cap * 2 : 8) * sizeof *g);
            if (!g)
                break;
            names = g;
            cap = cap ? cap * 2 : 8;
        }
        names[n] = strdup(de->d_name);
        if (names[n])
            n++;
    }
    if (d)
        closedir(d);
    if (n)
        qsort(names, n, sizeof *names, name_cmp);
    for (size_t i = 0; i < n; i++) {
        char p[PATH_MAX];
        if (join(p, sizeof p, dir, names[i]))
            add_mod(p, RL_MOD_RYUJINX);
        free(names[i]);
    }
    free(names);
}

static void resolve_entry(const char *entry, uint64_t pid)
{
    char path[PATH_MAX], sub[PATH_MAX];
    size_t len = strlen(entry);
    if (len >= sizeof path) {
        rl_log(RL_LOG_WARN, "mods", "mod path too long: %s", entry);
        return;
    }
    memcpy(path, entry, len + 1);
    while (len > 1 && path[len - 1] == '/')
        path[--len] = 0;
    if (!is_dir(path)) {
        rl_log(RL_LOG_WARN, "mods", "mod folder %s does not exist", path);
        return;
    }
    if (has_content(path)) {
        const char *name = base_name(path);
        if (is_title_name(name)) {
            char want[17];
            snprintf(want, sizeof want, "%016llx", (unsigned long long)pid);
            if (strcasecmp(name, want) != 0) {
                rl_log(RL_LOG_WARN, "mods", "%s is for title %s, not this program (%s): skipped", path, name, want);
                return;
            }
            add_mod(path, RL_MOD_ATMOSPHERE);
        } else
            add_mod(path, RL_MOD_RYUJINX);
        return;
    }
    {
        char contents[PATH_MAX];
        if (join(contents, sizeof contents, path, "contents") && is_dir(contents) &&
            title_dir(contents, pid, sub, sizeof sub) && has_content(sub)) {
            add_mod(sub, RL_MOD_ATMOSPHERE);
            return;
        }
    }
    if (title_dir(path, pid, sub, sizeof sub)) {
        if (has_content(sub))
            add_mod(sub, RL_MOD_ATMOSPHERE);
        else
            add_children(sub);
        return;
    }
    {
        char contents[PATH_MAX];
        if (join(contents, sizeof contents, path, "contents") && is_dir(contents)) {
            if (title_dir(contents, pid, sub, sizeof sub))
                add_children(sub);
            return;
        }
    }
    {
        size_t before = g_nmods;
        add_children(path);
        if (g_nmods == before)
            rl_log(RL_LOG_WARN, "mods", "%s holds no mods (no romfs/ or cheats/ folder)", path);
    }
}

size_t rl_hos_mods_set(const char *list, uint64_t program_id)
{
    char *copy, *p;
    size_t n;
    pthread_mutex_lock(&g_lock);
    clear_locked();
    copy = list ? strdup(list) : NULL;
    for (p = copy; p && *p;) {
        char *e = strchr(p, LIST_SEP);
        if (e)
            *e = 0;
        if (*p)
            resolve_entry(p, program_id);
        p = e ? e + 1 : NULL;
    }
    free(copy);
    n = g_nmods;
    pthread_mutex_unlock(&g_lock);
    return n;
}

size_t rl_hos_mods_count(void)
{
    size_t n;
    pthread_mutex_lock(&g_lock);
    n = g_nmods;
    pthread_mutex_unlock(&g_lock);
    return n;
}

const rl_hos_mod_info *rl_hos_mods_info(size_t i)
{
    const rl_hos_mod_info *r;
    pthread_mutex_lock(&g_lock);
    r = i < g_nmods ? &g_mods[i].info : NULL;
    pthread_mutex_unlock(&g_lock);
    return r;
}

size_t rl_hos_mods_conflict_count(void)
{
    size_t n;
    pthread_mutex_lock(&g_lock);
    n = g_nconflicts;
    pthread_mutex_unlock(&g_lock);
    return n;
}

const char *rl_hos_mods_conflict(size_t i)
{
    const char *r;
    pthread_mutex_lock(&g_lock);
    r = i < g_nconflicts ? g_conflicts[i] : NULL;
    pthread_mutex_unlock(&g_lock);
    return r;
}

static void on_conflict(void *user, const char *path, const char *winner, const char *loser)
{
    char text[MAX_CONFLICT_TEXT];
    (void)user;
    snprintf(text, sizeof text, "romfs conflict on %s: %s wins over %s", path, winner, loser);
    rl_log(RL_LOG_WARN, "mods", "%s", text);
    pthread_mutex_lock(&g_lock);
    if (g_nconflicts < MAX_CONFLICTS)
        memcpy(g_conflicts[g_nconflicts++], text, sizeof text);
    pthread_mutex_unlock(&g_lock);
}

void rl_hos_mods_apply_romfs(const char *base)
{
    rl_romfs_layer *layers;
    char (*paths)[PATH_MAX];
    size_t n = 0;
    pthread_mutex_lock(&g_lock);
    layers = calloc(g_nmods + 1, sizeof *layers);
    paths = calloc(g_nmods + 1, sizeof *paths);
    if (layers && paths) {
        if (base && *base)
            layers[n++] = (rl_romfs_layer) { "base", base, true };
        else if (g_nmods)
            rl_log(RL_LOG_WARN, "mods", "no base romfs tree: data mods are served alone");
        for (size_t i = 0; i < g_nmods; i++) {
            if (!g_mods[i].info.romfs || !join(paths[n], PATH_MAX, g_mods[i].info.path, "romfs"))
                continue;
            layers[n] = (rl_romfs_layer) { g_mods[i].info.name, paths[n], false };
            n++;
        }
    }
    if (layers && paths && (n > 1 || (n == 1 && !layers[0].base)))
        rl_hos_fs_set_romfs_layers(layers, n, on_conflict, NULL);
    else
        rl_hos_fs_set_romfs(base);
    pthread_mutex_unlock(&g_lock);
    free(layers);
    free(paths);
}

static bool build_id_matches(const char *stem, const char *build_id)
{
    size_t n = strlen(stem), b = strlen(build_id);
    if (n == 0 || n > b)
        return false;
    for (size_t i = 0; i < n; i++)
        if (!isxdigit((unsigned char)stem[i]) || tolower((unsigned char)stem[i]) != tolower((unsigned char)build_id[i]))
            return false;
    return true;
}

static bool parse_word(const char *s, size_t n, uint32_t *out)
{
    uint32_t v = 0;
    if (n != 8)
        return false;
    for (size_t i = 0; i < n; i++) {
        int c = tolower((unsigned char)s[i]);
        if (!isxdigit(c))
            return false;
        v = v << 4 | (uint32_t)(c <= '9' ? c - '0' : c - 'a' + 10);
    }
    *out = v;
    return true;
}

static cheat *new_cheat(mod *m, const char *name, size_t len)
{
    cheat *g = realloc(m->cheats, (m->ncheats + 1) * sizeof *g);
    cheat *c;
    if (!g)
        return NULL;
    m->cheats = g;
    c = &m->cheats[m->ncheats];
    memset(c, 0, sizeof *c);
    c->name = malloc(len + 1);
    if (!c->name)
        return NULL;
    memcpy(c->name, name, len);
    c->name[len] = 0;
    m->ncheats++;
    return c;
}

static void parse_cheat_text(mod *m, const char *file, char *text)
{
    cheat *cur = NULL;
    size_t cap = 0;
    for (char *line = text; line && *line;) {
        char *next = strchr(line, '\n'), *p = line;
        if (next)
            *next++ = 0;
        while (*p == ' ' || *p == '\t' || *p == '\r')
            p++;
        if (*p == '[' || *p == '{') {
            char close = *p == '[' ? ']' : '}';
            char *e = strchr(p, close);
            if (!e) {
                rl_log(RL_LOG_WARN, "mods", "%s: unterminated cheat name: %s", file, p);
                cur = NULL;
            } else {
                cur = new_cheat(m, p + 1, (size_t)(e - p - 1));
                cap = 0;
            }
        } else if (*p && *p != '#' && *p != ';' && !(p[0] == '/' && p[1] == '/')) {
            while (*p) {
                char *e;
                uint32_t w;
                while (*p == ' ' || *p == '\t' || *p == '\r')
                    p++;
                if (!*p)
                    break;
                for (e = p; *e && *e != ' ' && *e != '\t' && *e != '\r'; e++)
                    ;
                if (!cur) {
                    rl_log(RL_LOG_WARN, "mods", "%s: opcode outside a cheat ignored", file);
                    break;
                }
                if (!parse_word(p, (size_t)(e - p), &w) || cur->n >= CHEAT_MAX_WORDS) {
                    rl_log(RL_LOG_WARN, "mods", "%s: cheat '%s' has a bad or excess word: disabled", file, cur->name);
                    cur->dead = true;
                    break;
                }
                if (cur->n == cap) {
                    uint32_t *g = realloc(cur->ops, (cap ? cap * 2 : 16) * sizeof *g);
                    if (!g) {
                        cur->dead = true;
                        break;
                    }
                    cur->ops = g;
                    cap = cap ? cap * 2 : 16;
                }
                cur->ops[cur->n++] = w;
                p = e;
            }
        }
        line = next;
    }
}

static char *read_text(const char *path)
{
    FILE *f = fopen(path, "rb");
    long n;
    char *buf = NULL;
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 && n < (1 << 24) && fseek(f, 0, SEEK_SET) == 0) {
        buf = malloc((size_t)n + 1);
        if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) {
            free(buf);
            buf = NULL;
        } else if (buf)
            buf[n] = 0;
    }
    fclose(f);
    return buf;
}

static void validate_cheats(mod *m)
{
    for (size_t i = 0; i < m->ncheats; i++) {
        cheat *c = &m->cheats[i];
        size_t at = 0;
        while (!c->dead && at < c->n) {
            size_t len = cheat_vm_length(c->ops, c->n, at);
            if (!len) {
                rl_log(RL_LOG_WARN, "mods", "mod %s: cheat '%s' has an unsupported opcode at word %zu: disabled",
                       m->info.name, c->name, at);
                c->dead = true;
            }
            at += len;
        }
        if (!c->dead && c->n) {
            c->edge = calloc(c->n, 1);
            if (!c->edge)
                c->dead = true;
        }
    }
}

static void load_cheats(mod *m, const char *build_id)
{
    char dir[PATH_MAX];
    DIR *d;
    struct dirent *de;
    char **names = NULL;
    size_t n = 0, cap = 0;
    if (!m->info.has_cheats || !join(dir, sizeof dir, m->info.path, "cheats"))
        return;
    d = opendir(dir);
    while (d && (de = readdir(d))) {
        size_t len = strlen(de->d_name);
        char stem[256];
        if (len < 5 || len - 4 >= sizeof stem || strcasecmp(de->d_name + len - 4, ".txt") != 0)
            continue;
        memcpy(stem, de->d_name, len - 4);
        stem[len - 4] = 0;
        if (!build_id_matches(stem, build_id))
            continue;
        if (n == cap) {
            char **g = realloc(names, (cap ? cap * 2 : 4) * sizeof *g);
            if (!g)
                break;
            names = g;
            cap = cap ? cap * 2 : 4;
        }
        names[n] = strdup(de->d_name);
        if (names[n])
            n++;
    }
    if (d)
        closedir(d);
    if (n)
        qsort(names, n, sizeof *names, name_cmp);
    for (size_t i = 0; i < n; i++) {
        char path[PATH_MAX];
        char *text = join(path, sizeof path, dir, names[i]) ? read_text(path) : NULL;
        if (text)
            parse_cheat_text(m, path, text);
        else
            rl_log(RL_LOG_WARN, "mods", "cannot read cheat file %s", names[i]);
        free(text);
        free(names[i]);
    }
    free(names);
    validate_cheats(m);
    m->info.cheats = 0;
    for (size_t i = 0; i < m->ncheats; i++)
        if (!m->cheats[i].dead)
            m->info.cheats++;
}

void rl_hos_mods_attach(uint64_t main_base, const char *build_id)
{
    pthread_mutex_lock(&g_lock);
    g_main_base = main_base;
    for (size_t i = 0; i < g_nmods; i++) {
        cheats_free(&g_mods[i]);
        g_mods[i].info.cheats = 0;
        g_mods[i].info.cheats_need_rebuild = 0;
        if (!g_mods[i].info.has_cheats)
            continue;
        if (build_id && *build_id)
            load_cheats(&g_mods[i], build_id);
        else
            rl_log(RL_LOG_WARN, "mods", "mod %s: the main module's build id is unknown: cheats not loaded",
                   g_mods[i].info.name);
    }
    pthread_mutex_unlock(&g_lock);
}

void rl_hos_mods_set_keys(uint64_t (*fn)(void *user), void *user)
{
    pthread_mutex_lock(&g_lock);
    g_keys = fn;
    g_keys_user = user;
    pthread_mutex_unlock(&g_lock);
}

static int env_read(void *user, uint64_t addr, void *dst, unsigned n)
{
    rl_vm_block b;
    (void)user;
    if (!rl_vm_contains(&hos.vm, addr, n) || !rl_vm_uniform(&hos.vm, addr, n, &b) || !(b.perm & RL_VM_R))
        return CHEAT_MEM_UNMAPPED;
    rl_vm_kernel_read(&hos.vm, dst, addr, n);
    return CHEAT_MEM_OK;
}

static int env_write(void *user, uint64_t addr, const void *src, unsigned n)
{
    rl_vm_block b;
    (void)user;
    if (!rl_vm_contains(&hos.vm, addr, n) || !rl_vm_uniform(&hos.vm, addr, n, &b) || b.perm == RL_VM_NONE)
        return CHEAT_MEM_UNMAPPED;
    if (b.perm & RL_VM_X)
        return CHEAT_MEM_CODE;
    rl_vm_kernel_write(&hos.vm, addr, src, n);
    return CHEAT_MEM_OK;
}

static uint64_t env_base(void *user, unsigned region)
{
    (void)user;
    switch (region) {
    case 0:
        return g_main_base;
    case 1:
        return hos.lay.heap_start;
    case 2:
        return hos.lay.alias_start;
    case 3:
        return hos.lay.aslr_start;
    default:
        return 0;
    }
}

static uint64_t env_keys(void *user)
{
    if (g_keys)
        return g_keys(g_keys_user);
    (void)user;
    rl_input_snapshot s;
    rl_input_sample(&s);
    return s.pads[0].connected ? hid_core_pad_buttons(0, &s) : 0;
}

void rl_hos_mods_frame(void)
{
    cheat_env env = { NULL, env_read, env_write, env_base, env_keys };
    pthread_mutex_lock(&g_lock);
    if (g_nmods && hos.up) {
        hos_lock();
        for (size_t i = 0; i < g_nmods; i++)
            for (size_t k = 0; k < g_mods[i].ncheats; k++) {
                cheat *c = &g_mods[i].cheats[k];
                cheat_program p = { c->ops, c->n, c->edge };
                cheat_result r;
                if (c->dead || !c->n)
                    continue;
                cheat_vm_run(&g_vm, &env, &p, &r);
                if (r.code_writes && !c->warned_code) {
                    c->warned_code = true;
                    g_mods[i].info.cheats_need_rebuild++;
                    rl_log(RL_LOG_WARN, "mods",
                           "mod %s: cheat '%s' writes lifted code at 0x%llx: it needs a rebuild and is not applied",
                           g_mods[i].info.name, c->name, (unsigned long long)r.code_addr);
                }
                if (r.faults && !c->warned_fault) {
                    c->warned_fault = true;
                    rl_log(RL_LOG_WARN, "mods", "mod %s: cheat '%s' touched unmapped memory", g_mods[i].info.name,
                           c->name);
                }
                if (r.bad_opcode) {
                    c->dead = true;
                    rl_log(RL_LOG_WARN, "mods", "mod %s: cheat '%s' stopped at word %zu: disabled",
                           g_mods[i].info.name, c->name, r.bad_at);
                }
            }
        hos_unlock();
    }
    pthread_mutex_unlock(&g_lock);
}

typedef struct jw {
    char *buf;
    size_t cap, len;
} jw;

static void jput(jw *w, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void jput(jw *w, const char *fmt, ...)
{
    va_list ap;
    int n;
    va_start(ap, fmt);
    n = vsnprintf(w->buf && w->len < w->cap ? w->buf + w->len : NULL, w->buf && w->len < w->cap ? w->cap - w->len : 0,
                  fmt, ap);
    va_end(ap);
    if (n > 0)
        w->len += (size_t)n;
}

static void jstr(jw *w, const char *s)
{
    jput(w, "\"");
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\')
            jput(w, "\\%c", c);
        else if (c < 0x20)
            jput(w, "\\u%04x", c);
        else
            jput(w, "%c", c);
    }
    jput(w, "\"");
}

size_t rl_hos_mods_json(char *buf, size_t cap)
{
    jw w = { buf, cap, 0 };
    pthread_mutex_lock(&g_lock);
    jput(&w, "{\"data_mods\":[");
    for (size_t i = 0; i < g_nmods; i++) {
        const rl_hos_mod_info *m = &g_mods[i].info;
        jput(&w, "%s{\"name\":", i ? "," : "");
        jstr(&w, m->name);
        jput(&w, ",\"path\":");
        jstr(&w, m->path);
        jput(&w, ",\"layout\":\"%s\",\"romfs\":%s,\"cheats\":%u,\"cheats_need_rebuild\":%u}",
             m->layout == RL_MOD_ATMOSPHERE ? "atmosphere" : "ryujinx", m->romfs ? "true" : "false", m->cheats,
             m->cheats_need_rebuild);
    }
    jput(&w, "],\"conflicts\":[");
    for (size_t i = 0; i < g_nconflicts; i++) {
        jput(&w, "%s", i ? "," : "");
        jstr(&w, g_conflicts[i]);
    }
    jput(&w, "]}");
    pthread_mutex_unlock(&g_lock);
    if (buf && cap)
        buf[w.len < cap ? w.len : cap - 1] = 0;
    return w.len;
}

void rl_hos_mods_install(void)
{
    const char *list = getenv("RELIFT_MODS");
    if (!list || !*list)
        return;
    rl_hos_mods_set(list, hos.cfg.program_id);
    rl_hos_mods_apply_romfs(getenv("RELIFT_ROMFS"));
}
