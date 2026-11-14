/**
 * @file devload.c
 * @brief The dev-build miss path of docs/abi.md section 6: the miss lock, the
 * connection to relift over RELIFT_DEV_SOCKET (RL_DEV_HELLO once, then one
 * RL_DEV_LIFT per miss), opening the library relift built and registering
 * its unit. Built without RL_HOTLOAD it only says no.
 */
#include "core.h"

#if defined(RL_HOTLOAD)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <windows.h>
#include <afunix.h>
typedef SOCKET dev_sock;
#define DEV_BADSOCK INVALID_SOCKET
static SRWLOCK g_miss = SRWLOCK_INIT;
static void miss_lock(void) { AcquireSRWLockExclusive(&g_miss); }
static void miss_unlock(void) { ReleaseSRWLockExclusive(&g_miss); }
static int net_init(void)
{
    WSADATA w;
    return WSAStartup(MAKEWORD(2, 2), &w);
}
static void sock_close(dev_sock s) { closesocket(s); }
static int net_err(void) { return WSAGetLastError(); }
static uint32_t host_page(void)
{
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
}
static uint64_t host_pid(void) { return GetCurrentProcessId(); }
static void *lib_open(const char *path, char *err, size_t n)
{
    wchar_t w[2048];
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 2048)) {
        snprintf(err, n, "the path is not UTF-8 or too long");
        return NULL;
    }
    HMODULE h = LoadLibraryW(w);
    if (!h)
        snprintf(err, n, "LoadLibraryW error %lu", (unsigned long)GetLastError());
    return (void *)h;
}
static void *lib_sym(void *h, const char *s) { return (void *)(uintptr_t)GetProcAddress((HMODULE)h, s); }
static void lib_close(void *h) { FreeLibrary((HMODULE)h); }
#else
#include <dlfcn.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
typedef int dev_sock;
#define DEV_BADSOCK (-1)
static pthread_mutex_t g_miss = PTHREAD_MUTEX_INITIALIZER;
static void miss_lock(void) { pthread_mutex_lock(&g_miss); }
static void miss_unlock(void) { pthread_mutex_unlock(&g_miss); }
static int net_init(void) { return 0; }
static void sock_close(dev_sock s) { close(s); }
static int net_err(void) { return errno; }
static uint32_t host_page(void) { return (uint32_t)sysconf(_SC_PAGESIZE); }
static uint64_t host_pid(void) { return (uint64_t)getpid(); }
static void *lib_open(const char *path, char *err, size_t n)
{
    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h)
        snprintf(err, n, "%s", dlerror());
    return h;
}
static void *lib_sym(void *h, const char *s) { return dlsym(h, s); }
static void lib_close(void *h) { dlclose(h); }
#endif

static dev_sock g_sock = DEV_BADSOCK;
static uint32_t g_seq;
static bool g_dead;
static uint32_t g_loaded;

static uint64_t now_ns(void)
{
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (uint64_t)ts.tv_sec * 1000000000u + (uint64_t)ts.tv_nsec;
}

static int send_all(const void *p, size_t n)
{
    const char *b = p;
    while (n) {
        int k = (int)send(g_sock, b, (int)n, 0);
        if (k <= 0)
            return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static int recv_all(void *p, size_t n)
{
    char *b = p;
    while (n) {
        int k = (int)recv(g_sock, b, (int)n, 0);
        if (k <= 0)
            return -1;
        b += k;
        n -= (size_t)k;
    }
    return 0;
}

static void drop(void)
{
    if (g_sock != DEV_BADSOCK)
        sock_close(g_sock);
    g_sock = DEV_BADSOCK;
    g_dead = true;
}

/** @brief Reads a reply of kind want to request seq; 0, or -1 with why set. */
static int read_reply(uint16_t want, uint32_t seq, rl_dev_reply *r, char *path, size_t pathn, char *msg, size_t msgn,
                      char *why, size_t whyn)
{
    rl_dev_hdr h;
    if (recv_all(&h, sizeof h) || recv_all(r, sizeof *r)) {
        snprintf(why, whyn, "the connection to relift was lost");
        return -1;
    }
    if (h.magic != RL_DEV_MAGIC || h.kind != want || h.seq != seq ||
        h.payload_len != sizeof *r + (uint64_t)r->path_len + r->msg_len) {
        snprintf(why, whyn, "relift sent a malformed reply (kind %u, seq %u)", h.kind, h.seq);
        return -1;
    }
    if (r->path_len >= pathn || r->msg_len >= msgn) {
        snprintf(why, whyn, "relift sent a reply too long (path %u, message %u bytes)", r->path_len, r->msg_len);
        return -1;
    }
    if (recv_all(path, r->path_len) || recv_all(msg, r->msg_len)) {
        snprintf(why, whyn, "the connection to relift was lost");
        return -1;
    }
    path[r->path_len] = 0;
    msg[r->msg_len] = 0;
    return 0;
}

/** @brief Step 2: connects and says hello, once per process. */
static int dev_connect(char *why, size_t n)
{
    if (g_sock != DEV_BADSOCK)
        return 0;
    if (g_dead) {
        snprintf(why, n, "the relift dev server failed earlier");
        return -1;
    }
    g_dead = true;
    const char *p = getenv(RL_DEV_SOCKET_ENV);
    if (!p || !*p) {
        snprintf(why, n, "%s is not set: this dev build runs only under relift run", RL_DEV_SOCKET_ENV);
        return -1;
    }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof sa);
    sa.sun_family = AF_UNIX;
    if (strlen(p) >= sizeof sa.sun_path) {
        snprintf(why, n, "%s is %u bytes long, the host limit is %u", RL_DEV_SOCKET_ENV, (unsigned)strlen(p),
                 (unsigned)sizeof sa.sun_path - 1);
        return -1;
    }
    memcpy(sa.sun_path, p, strlen(p));
    const uint8_t *image = rl_dispatch_image();
    if (!image) {
        snprintf(why, n, "no static unit carries an image id");
        return -1;
    }
    if (net_init() != 0) {
        snprintf(why, n, "the host socket layer did not start");
        return -1;
    }
    g_sock = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_sock == DEV_BADSOCK) {
        snprintf(why, n, "socket(AF_UNIX) failed: error %d", net_err());
        return -1;
    }
    if (connect(g_sock, (struct sockaddr *)&sa, sizeof sa) != 0) {
        snprintf(why, n, "cannot connect to relift at %s: error %d", p, net_err());
        drop();
        return -1;
    }
    struct {
        rl_dev_hdr h;
        rl_dev_hello b;
    } m;
    memset(&m, 0, sizeof m);
    m.h = (rl_dev_hdr){ RL_DEV_MAGIC, RL_DEV_HELLO, 0, ++g_seq, sizeof m.b };
    m.b.abi_version = RL_ABI_VERSION;
    m.b.host_page = host_page();
    memcpy(m.b.image_id, image, RL_IMAGE_ID_BYTES);
    m.b.pid = host_pid();
    rl_dev_reply r;
    char path[8], msg[1024];
    if (send_all(&m, sizeof m) != 0) {
        snprintf(why, n, "the connection to relift was lost");
        drop();
        return -1;
    }
    if (read_reply(RL_DEV_HELLO_REPLY, m.h.seq, &r, path, sizeof path, msg, sizeof msg, why, n) != 0) {
        drop();
        return -1;
    }
    if (r.status != RL_DEV_OK) {
        snprintf(why, n, "relift refused this program (status %d): %s", r.status, msg);
        drop();
        return -1;
    }
    g_dead = false;
    return 0;
}

/** @brief Steps 3 to 5 for target, under the miss lock. */
static int dev_lift(rl_ctx *c, uint64_t target, uint32_t fp_mode, char *why, size_t n)
{
    if (dev_connect(why, n) != 0)
        return -1;
    struct rl_thread *t = rl_common(c)->thread;
    struct {
        rl_dev_hdr h;
        rl_dev_lift_req b;
    } m;
    memset(&m, 0, sizeof m);
    m.h = (rl_dev_hdr){ RL_DEV_MAGIC, RL_DEV_LIFT, 0, ++g_seq, sizeof m.b };
    m.b.target = target;
    m.b.thread_id = t ? t->host_tid : 0;
    m.b.reason = RL_MISS_CALL;
    m.b.fp_mode = fp_mode;
    rl_dev_reply r;
    static char path[4096], msg[65536];
    uint64_t t0 = now_ns();
    if (send_all(&m, sizeof m) != 0) {
        snprintf(why, n, "the connection to relift was lost");
        drop();
        return -1;
    }
    if (read_reply(RL_DEV_LIFT_REPLY, m.h.seq, &r, path, sizeof path, msg, sizeof msg, why, n) != 0) {
        drop();
        return -1;
    }
    uint64_t t1 = now_ns();
    if (r.status != RL_DEV_OK) {
        snprintf(why, n, "relift could not build code for 0x%llx (status %d): %s", (unsigned long long)target, r.status,
                 msg);
        return -1;
    }
    char err[1024];
    void *h = lib_open(path, err, sizeof err);
    if (!h) {
        snprintf(why, n, "cannot load %s: %s", path, err);
        return -1;
    }
    rl_unit_fn *uf = (rl_unit_fn *)(uintptr_t)lib_sym(h, RL_UNIT_SYMBOL);
    if (!uf) {
        snprintf(why, n, "%s exports no %s", path, RL_UNIT_SYMBOL);
        lib_close(h);
        return -1;
    }
    uint64_t t2 = now_ns();
    const rl_unit *u = uf();
    if (rl_unit_add(u, why, n) != 0) {
        lib_close(h);
        return -1;
    }
    uint64_t t3 = now_ns();
    g_loaded++;
    rl_log(RL_LOG_INFO, "hotload",
           "loaded 0x%llx: %u functions in %.1f ms (relift %.1f ms, load %.2f ms, register %.3f ms), %u libraries so "
           "far%s%s",
           (unsigned long long)target, u->fn_count, (double)(t3 - t0) / 1e6, (double)(t1 - t0) / 1e6,
           (double)(t2 - t1) / 1e6, (double)(t3 - t2) / 1e6, g_loaded, *msg ? ": " : "", msg);
    return 0;
}

rl_fn rl_core_miss_load(rl_ctx *c, uint64_t target, uint32_t fp_mode)
{
    char why[2048];
    miss_lock();
    rl_fn f = rl_dispatch_find(target);
    if (!f) {
        why[0] = 0;
        if (dev_lift(c, target, fp_mode, why, sizeof why) == 0) {
            f = rl_dispatch_find(target);
            if (!f)
                snprintf(why, sizeof why, "the library relift built has no function at 0x%llx",
                         (unsigned long long)target);
        }
        if (!f)
            rl_log(RL_LOG_ERROR, "hotload", "%s", why);
    }
    miss_unlock();
    return f;
}

#else

rl_fn rl_core_miss_load(rl_ctx *c, uint64_t target, uint32_t fp_mode)
{
    (void)c;
    (void)target;
    (void)fp_mode;
    return NULL;
}

#endif
