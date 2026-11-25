/**
 * @file report.c
 * @brief Stop reports: what a shipped build writes when the guest stops on
 * missed code, an instruction it cannot run or a landing pad it cannot
 * enter (docs/abi.md section 11, stop reports). The report names the target
 * and the transfer site as module+offset, what lies at the target, the
 * registers, the guest and host backtraces and the build's identity, in a
 * stable plain-text format that `relift report` reads. It goes to a file in
 * the report folder and to stderr. Formatting uses static buffers only, so a
 * corrupt heap does not stop the report.
 */
#include "core.h"
#include "crash.h"

#include <relift/settings.h>

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#if defined(_WIN32)
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <os/log.h>
#endif

/** @brief Version of the report format, the number on its first line. */
#define RL_REPORT_FORMAT 1
/** @brief Most header lines rl_report_info keeps. */
#define INFO_MAX 64
/** @brief Most modules rl_report_module keeps. */
#define MOD_MAX 64
/** @brief The host's path separator. */
#if defined(_WIN32)
#define SEP '\\'
#else
#define SEP '/'
#endif
/** @brief Instruction words a report prints at an address. */
#define WORDS 8

typedef struct info_line {
    char key[32];
    char value[256];
} info_line;

typedef struct mod_range {
    char name[64];
    uint64_t base;
    uint64_t size;
} mod_range;

static info_line g_info[INFO_MAX];
static uint32_t g_info_n;
static mod_range g_mods[MOD_MAX];
static uint32_t g_mod_n;
static const rl_report_guest *g_guest;
static char g_dir[1024];
static char g_text[96 * 1024];
static volatile int g_busy;

void rl_report_set_guest(const rl_report_guest *g) { g_guest = g; }

static void copy(char *dst, size_t n, const char *src)
{
    size_t k = src ? strlen(src) : 0;
    if (k >= n)
        k = n - 1;
    if (k)
        memcpy(dst, src, k);
    dst[k] = 0;
    for (size_t i = 0; i < k; i++)
        if (dst[i] == '\n' || dst[i] == '\r')
            dst[i] = ' ';
}

void rl_report_info(const char *key, const char *value)
{
    if (!key || g_info_n >= INFO_MAX)
        return;
    copy(g_info[g_info_n].key, sizeof g_info[0].key, key);
    copy(g_info[g_info_n].value, sizeof g_info[0].value, value);
    g_info_n++;
}

void rl_report_info_each(void (*fn)(void *user, const char *key, const char *value), void *user)
{
    for (uint32_t i = 0; i < g_info_n; i++)
        fn(user, g_info[i].key, g_info[i].value);
}

void rl_report_module(const char *name, uint64_t base, uint64_t size, const char *build_id)
{
    char v[256];
    snprintf(v, sizeof v, "%s base 0x%llx size 0x%llx build %s", name ? name : "?", (unsigned long long)base,
             (unsigned long long)size, build_id && *build_id ? build_id : "-");
    rl_report_info("module", v);
    if (g_mod_n < MOD_MAX) {
        copy(g_mods[g_mod_n].name, sizeof g_mods[0].name, name ? name : "?");
        g_mods[g_mod_n].base = base;
        g_mods[g_mod_n].size = size;
        g_mod_n++;
    }
}

void rl_report_reset(void)
{
    g_info_n = 0;
    g_mod_n = 0;
    g_guest = NULL;
    g_dir[0] = 0;
}

void rl_report_set_dir(const char *dir)
{
    copy(g_dir, sizeof g_dir, dir);
    rl_report_prepare();
}

typedef struct out_buf {
    char *p;
    size_t n;
    size_t len;
} out_buf;

static void put(out_buf *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

static void put(out_buf *o, const char *fmt, ...)
{
    if (o->len + 1 >= o->n)
        return;
    va_list ap;
    va_start(ap, fmt);
    int w = vsnprintf(o->p + o->len, o->n - o->len, fmt, ap);
    va_end(ap);
    if (w > 0)
        o->len = o->len + (size_t)w < o->n ? o->len + (size_t)w : o->n - 1;
}

/** @brief The lifted function at guest address a: the guest layer's bank-aware lookup, else the function maps. */
static bool guest_fn(uint64_t a, rl_fn_hit *h)
{
    if (g_guest && g_guest->find) {
        int r = g_guest->find(a, h);
        if (r >= 0)
            return r == 1;
    }
    return rl_fn_find_guest(a, h);
}

static const char *fn_name(const rl_fn_info *f)
{
    return f->guest_name ? f->guest_name : f->host_sym ? f->host_sym : "?";
}

/** @brief Writes " <module>+0x<off>" for guest address a when a module holds it. */
static void put_place(out_buf *o, uint64_t a)
{
    for (uint32_t i = 0; i < g_mod_n; i++)
        if (a >= g_mods[i].base && a - g_mods[i].base < g_mods[i].size) {
            put(o, " %s+0x%llx", g_mods[i].name, (unsigned long long)(a - g_mods[i].base));
            return;
        }
    rl_fn_hit h;
    if (guest_fn(a, &h) && h.map->module) {
        put(o, " %s+0x%llx", h.map->module, (unsigned long long)(a - h.map->module_base));
        return;
    }
    const char *mod = NULL;
    if (rl_code_class(a, &mod) >= 0 && mod)
        put(o, " (in the code of %s)", mod);
}

static void put_fn(out_buf *o, const rl_fn_hit *h)
{
    const rl_fn_map *m = h->map;
    uint64_t a = h->fn->fn_addr;
    put(o, "0x%llx", (unsigned long long)a);
    bool named = false;
    for (uint32_t i = 0; i < g_mod_n && !named; i++)
        if (a >= g_mods[i].base && a - g_mods[i].base < g_mods[i].size) {
            put(o, " %s+0x%llx", g_mods[i].name, (unsigned long long)(a - g_mods[i].base));
            named = true;
        }
    if (!named)
        put(o, " %s+0x%llx", m->module ? m->module : "?", (unsigned long long)(a - m->module_base));
    put(o, " %s", fn_name(h->fn));
}

/** @brief The default peek: guest memory at the base, only inside the registered code maps (module code is mapped). */
static bool peek_code(uint64_t a, void *out, size_t n)
{
    uint8_t *mem = rl_mem_base();
    if (!mem || rl_code_class(a, NULL) < 0 || rl_code_class(a + n - 1, NULL) < 0)
        return false;
    memcpy(out, mem + a, n);
    return true;
}

bool rl_report_peek(uint64_t a, void *out, size_t n)
{
    return g_guest && g_guest->peek ? g_guest->peek(a, out, n) : peek_code(a, out, n);
}

/** @brief Writes `<label> 0x<start>: <words>` for the instruction words from before words ahead of a; stops at the first unreadable one. */
static void put_words(out_buf *o, const char *label, uint64_t a, int before)
{
    bool (*peek)(uint64_t, void *, size_t) = rl_report_peek;
    uint32_t w = g_guest && g_guest->word_bytes ? g_guest->word_bytes : 4;
    if (w > 8)
        w = 8;
    a &= ~(uint64_t)(w - 1);
    uint8_t b[8 * 2 * WORDS];
    uint64_t start = a;
    for (int k = 0; k < before && start >= w; k++) {
        uint8_t t[8];
        if (!peek(start - w, t, w))
            break;
        start -= w;
    }
    int n = 0;
    for (; n < 2 * WORDS && start + (uint64_t)n * w < a + (uint64_t)WORDS * w; n++)
        if (!peek(start + (uint64_t)n * w, b + (size_t)n * w, w))
            break;
    if (!n) {
        put(o, "%s: unreadable\n", label);
        return;
    }
    put(o, "%s 0x%llx:", label, (unsigned long long)start);
    for (int i = 0; i < n; i++) {
        uint64_t v = 0;
        for (uint32_t k = 0; k < w; k++)
            v |= (uint64_t)b[(size_t)i * w + k] << (8 * k);
        put(o, " %0*llx", (int)(2 * w), (unsigned long long)v);
    }
    put(o, "\n");
}

static const char *reason_name(uint32_t r)
{
    switch (r) {
    case RL_MISS_CALL: return "call";
    case RL_MISS_JUMP: return "jump";
    case RL_MISS_ENTRY: return "entry";
    case RL_MISS_RESUME: return "resume";
    default: return "none";
    }
}

static void put_code(out_buf *o, uint64_t a)
{
    const char *mod = NULL;
    int cls = rl_code_class(a, &mod);
    rl_fn_hit h;
    bool lifted = rl_dispatch_find(a) != NULL;
    bool inside = guest_fn(a, &h);
    put(o, "target code: ");
    if (cls == RL_CODE_TRAP)
        put(o, "no instruction (the build saw padding or data here)\n");
    else if (lifted)
        put(o, "lifted\n");
    else if (cls >= 0)
        put(o, "not lifted (in the code of %s)\n", mod ? mod : "?");
    else if (inside)
        put(o, "not lifted (inside a lifted function's extent)\n");
    else
        put(o, "not lifted (outside every lifted function)\n");
    if (inside) {
        put(o, "target function: ");
        put_fn(o, &h);
        put(o, " +0x%llx\n", (unsigned long long)h.off);
    }
    if (g_guest && g_guest->where) {
        char w[256];
        w[0] = 0;
        g_guest->where(a, w, sizeof w);
        if (w[0])
            put(o, "target where: %s\n", w);
    }
}

static size_t format(rl_ctx *c, const char *kind, const char *summary, const rl_miss_info *m, const uintptr_t *frames,
                     int nframes, char *buf, size_t n)
{
    out_buf o = { buf, n, 0 };
    buf[0] = 0;
    const rl_cpu_desc *cpu = rl_core_cpu();
    const rl_guest_desc *guest = rl_core_guest();
    put(&o, "relift stop report %d\n", RL_REPORT_FORMAT);
    put(&o, "kind: %s\n", kind);
    put(&o, "summary: %s\n", summary);
    for (uint32_t i = 0; i < g_info_n; i++)
        put(&o, "%s: %s\n", g_info[i].key, g_info[i].value);
    if (guest && guest->name)
        put(&o, "guest layer: %s\n", guest->name);
    if (cpu)
        put(&o, "cpu: %s\n", cpu->name);
    const uint8_t *id = rl_dispatch_image();
    if (id) {
        put(&o, "image: ");
        for (uint32_t i = 0; i < RL_IMAGE_ID_BYTES; i++)
            put(&o, "%02x", id[i]);
        put(&o, "\n");
    }
#if defined(_WIN32)
    put(&o, "pid: %lu\n", (unsigned long)GetCurrentProcessId());
#else
    put(&o, "pid: %ld\n", (long)getpid());
#endif
    struct rl_thread *t = c ? rl_common(c)->thread : NULL;
    if (t)
        put(&o, "thread: %llu\n", (unsigned long long)t->host_tid);
    put(&o, "time: %lld\n", (long long)time(NULL));
    if (m) {
        put(&o, "transfer: %s\n", reason_name(m->reason));
        if (m->reason == RL_MISS_NONE) {
            put(&o, "at: 0x%llx", (unsigned long long)m->target);
            put_place(&o, m->target);
            put(&o, "\n");
            put_words(&o, "at words", m->target, WORDS / 2);
        } else {
            put(&o, "target: 0x%llx", (unsigned long long)m->target);
            put_place(&o, m->target);
            put(&o, "\n");
            put_code(&o, m->target);
            put_words(&o, "target words", m->target, 0);
        }
        rl_fn_hit h;
        if (m->reason == RL_MISS_NONE) {
            if (guest_fn(m->target, &h)) {
                put(&o, "at function: ");
                put_fn(&o, &h);
                put(&o, " +0x%llx\n", (unsigned long long)h.off);
            }
        } else if (m->site != RL_SITE_NONE) {
            put(&o, "site: 0x%llx", (unsigned long long)m->site);
            put_place(&o, m->site);
            put(&o, "\n");
            if (guest_fn(m->site, &h)) {
                put(&o, "site function: ");
                put_fn(&o, &h);
                put(&o, " +0x%llx\n", (unsigned long long)h.off);
            }
            put_words(&o, "site words", m->site, WORDS / 2);
        } else {
            put(&o, "site: unknown\n");
        }
        if (m->host_ret && rl_fn_find_host(m->host_ret - 1, &h)) {
            put(&o, "caller function: ");
            put_fn(&o, &h);
            put(&o, " (host %s+0x%llx)\n", h.fn->host_sym ? h.fn->host_sym : "?",
                (unsigned long long)(m->host_ret - (uintptr_t)h.fn->fn));
        }
    }
    if (c && cpu) {
        static char regs[4096];
        regs[0] = 0;
        if (cpu->regs)
            cpu->regs(c, regs, sizeof regs);
        else if (cpu->describe) {
            cpu->describe(c, regs, sizeof regs - 1);
            size_t k = strlen(regs);
            regs[k] = '\n';
            regs[k + 1] = 0;
        }
        put(&o, "registers:\n");
        const char *p = regs;
        while (*p) {
            const char *e = strchr(p, '\n');
            size_t k = e ? (size_t)(e - p) : strlen(p);
            put(&o, "  %.*s\n", (int)k, p);
            p += k + (e ? 1 : 0);
        }
    }
    put(&o, "guest backtrace: lifted frames, innermost first; tail calls leave no frame\n");
    int g = 0;
    rl_fn_hit h;
    for (int i = 0; i < nframes; i++)
        if (rl_fn_find_host(frames[i] - 1, &h)) {
            put(&o, "  #%d ", g++);
            put_fn(&o, &h);
            put(&o, "%s\n", h.exact ? "" : " (nearest)");
        }
    if (!g)
        put(&o, "  none\n");
    put(&o, "host backtrace:\n");
    for (int i = 0; i < nframes; i++) {
        char sym[256];
        put(&o, "  #%d %p ", i, (void *)frames[i]);
        if (rl_fn_find_host(frames[i] - 1, &h))
            put(&o, "%s+0x%llx", h.fn->host_sym ? h.fn->host_sym : "?",
                (unsigned long long)(frames[i] - (uintptr_t)h.fn->fn));
        else if (rl_crash_os_symbolize(frames[i], sym, sizeof sym))
            put(&o, "%s", sym);
        else
            put(&o, "?");
        put(&o, "\n");
    }
    put(&o, "end of report\n");
    return o.len;
}

static bool make_dir(char *path)
{
    for (char *p = path + 1; *p; p++) {
        if (*p != '/' && *p != '\\')
            continue;
        char k = *p;
        *p = 0;
#if defined(_WIN32)
        _mkdir(path);
#else
        mkdir(path, 0755);
#endif
        *p = k;
    }
#if defined(_WIN32)
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0755) == 0 || errno == EEXIST;
#endif
}

/**
 * @brief The folder report files go to; false when no file is written. A
 * folder set by rl_report_set_dir or RELIFT_REPORT_DIR always is; the
 * per-user default only in a shipped build (neither RL_HOTLOAD, a `relift
 * run` build, nor RL_DEV_RUN defined), so dev runs and tests leave no files
 * behind.
 */
static bool report_dir(char *out, size_t n)
{
    if (g_dir[0]) {
        copy(out, n, g_dir);
        return true;
    }
    const char *e = getenv("RELIFT_REPORT_DIR");
    if (e && *e) {
        copy(out, n, e);
        return true;
    }
#if defined(RL_HOTLOAD) || defined(RL_DEV_RUN)
    return false;
#else
    char user[1024];
    const char *app = rl_settings_app();
    if (rl_settings_user_dir(user, sizeof user, app && *app ? app : "relift")) {
        int k = snprintf(out, n, "%s%creports", user, SEP);
        return k > 0 && (size_t)k < n;
    }
    copy(out, n, ".");
    return true;
#endif
}

static int write_file(const char *prefix, const char *text, size_t len, char *path, size_t path_n)
{
    char dir[1100];
    if (!report_dir(dir, sizeof dir))
        return -1;
    make_dir(dir);
    time_t now = time(NULL);
    struct tm tmv;
#if defined(_WIN32)
    gmtime_s(&tmv, &now);
    unsigned long pid = (unsigned long)GetCurrentProcessId();
#else
    gmtime_r(&now, &tmv);
    unsigned long pid = (unsigned long)getpid();
#endif
    char p[1300];
    int fd = -1;
    for (int seq = 0; seq < 100 && fd < 0; seq++) {
        char suffix[8] = "";
        if (seq)
            snprintf(suffix, sizeof suffix, "-%d", seq);
        int k = snprintf(p, sizeof p, "%s%c%s-%04d%02d%02d-%02d%02d%02d-%lu%s.txt", dir, SEP, prefix,
                         tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, pid,
                         suffix);
        if (k <= 0 || (size_t)k >= sizeof p)
            return -1;
#if defined(_WIN32)
        fd = _open(p, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
        fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
#endif
        if (fd < 0 && errno != EEXIST)
            return -1;
    }
    if (fd < 0)
        return -1;
    size_t done = 0;
    while (done < len) {
#if defined(_WIN32)
        int w = _write(fd, text + done, (unsigned)(len - done));
#else
        ssize_t w = write(fd, text + done, len - done);
#endif
        if (w <= 0)
            break;
        done += (size_t)w;
    }
#if defined(_WIN32)
    _close(fd);
#else
    close(fd);
#endif
    if (path)
        copy(path, path_n, p);
    return done == len ? 0 : -1;
}

static void system_log(const char *text)
{
#if defined(__APPLE__)
    const char *p = text;
    while (*p) {
        const char *e = strchr(p, '\n');
        int k = e ? (int)(e - p) : (int)strlen(p);
        os_log_error(OS_LOG_DEFAULT, "relift: %{public}.*s", k, p);
        p += k + (e ? 1 : 0);
    }
#else
    (void)text;
#endif
}

int rl_report_stop(rl_ctx *c, const char *kind, const char *summary, const rl_miss_info *m, char *path, size_t path_n)
{
    if (path && path_n)
        path[0] = 0;
    if (__atomic_exchange_n(&g_busy, 1, __ATOMIC_ACQ_REL)) {
        rl_log(RL_LOG_ERROR, kind, "%s (another stop report is being written)", summary);
        return -1;
    }
    uintptr_t frames[RL_CRASH_MAX_FRAMES];
    int nframes = rl_crash_capture(frames, RL_CRASH_MAX_FRAMES);
    size_t len = format(c, kind, summary, m, frames, nframes, g_text, sizeof g_text);
    char file[1300], dir[1100];
    file[0] = 0;
    bool files = report_dir(dir, sizeof dir);
    int rc = files ? write_file("relift-stop", g_text, len, file, sizeof file) : 1;
    if (rc == 0)
        rl_log(RL_LOG_ERROR, "stop", "%s: %s; report written to %s", kind, summary, file);
    else if (rc > 0)
        rl_log(RL_LOG_ERROR, "stop", "%s: %s", kind, summary);
    else
        rl_log(RL_LOG_ERROR, "stop", "%s: %s; the report file could not be written", kind, summary);
    if (rl_cfg()->log_level <= RL_LOG_INFO) {
        fflush(stderr);
        rl_crash_os_write(g_text, len);
    }
    system_log(g_text);
    if (path)
        copy(path, path_n, file);
    __atomic_store_n(&g_busy, 0, __ATOMIC_RELEASE);
    return rc == 0 ? 0 : -1;
}

int rl_report_write(const char *prefix, const char *text, size_t len, char *path, size_t path_n)
{
    if (path && path_n)
        path[0] = 0;
    return write_file(prefix, text, len, path, path_n);
}

static char g_crash_dir[1100];
static volatile int g_crash_ready;

static void crash_file(const char *text, size_t len);

void rl_report_prepare(void)
{
    g_crash_ready = 0;
    if (!report_dir(g_crash_dir, sizeof g_crash_dir))
        return;
    make_dir(g_crash_dir);
    g_crash_ready = 1;
    rl_crash_set_file(crash_file);
}

/** @brief Appends the decimal digits of v to p; returns the new end. Async-signal-safe. */
static char *put_dec(char *p, char *end, unsigned long long v)
{
    char d[24];
    int k = 0;
    do {
        d[k++] = (char)('0' + v % 10);
        v /= 10;
    } while (v && k < (int)sizeof d);
    while (k && p < end)
        *p++ = d[--k];
    return p;
}

/** @brief The crash handler's file writer: the crash report text to a new file `relift-crash-<unix time>-<pid>.txt` in the folder rl_report_prepare found. */
static void crash_file(const char *text, size_t len)
{
    if (!g_crash_ready)
        return;
    char p[1300];
    char *end = p + sizeof p - 1, *q = p;
    for (const char *s2 = g_crash_dir; *s2 && q < end; s2++)
        *q++ = *s2;
    const char *name = "/relift-crash-";
#if defined(_WIN32)
    name = "\\relift-crash-";
    unsigned long long pid = GetCurrentProcessId();
    unsigned long long now = (unsigned long long)GetTickCount64() / 1000ull;
#else
    unsigned long long pid = (unsigned long long)getpid();
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    unsigned long long now = (unsigned long long)ts.tv_sec;
#endif
    for (const char *s2 = name; *s2 && q < end; s2++)
        *q++ = *s2;
    q = put_dec(q, end, now);
    if (q < end)
        *q++ = '-';
    q = put_dec(q, end, pid);
    const char *ext = ".txt";
    for (const char *s2 = ext; *s2 && q < end; s2++)
        *q++ = *s2;
    *q = 0;
#if defined(_WIN32)
    int fd = _open(p, _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY, _S_IREAD | _S_IWRITE);
#else
    int fd = open(p, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
#endif
    if (fd < 0)
        return;
    size_t done = 0;
    while (done < len) {
#if defined(_WIN32)
        int w = _write(fd, text + done, (unsigned)(len - done));
#else
        ssize_t w = write(fd, text + done, len - done);
#endif
        if (w <= 0)
            break;
        done += (size_t)w;
    }
#if defined(_WIN32)
    _close(fd);
#else
    close(fd);
#endif
}
