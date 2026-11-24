/**
 * @file fs.c
 * @brief Files and descriptors: the guest's descriptor table over the
 * host's descriptors and the system calls that use it.
 */
#include "hostio.h"
#include "compat.h"
#include "linux_priv.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PATH_CAP 4096u
#define NAME_CAP 256u
#define IOV_MAX_COUNT 1024u

/** @brief Sets the guest root from the program or RELIFT_LINUX_ROOT (a debugging override; empty turns the root off). */
static void root_init(void)
{
    free(lx.root);
    lx.root = NULL;
    lx.root_len = 0;
    const char *r = getenv("RELIFT_LINUX_ROOT");
    if (!r)
        r = lx.prog ? lx.prog->root : NULL;
    if (!r || !*r)
        return;
    size_t n = strlen(r);
    while (n > 1 && r[n - 1] == '/')
        n--;
    lx.root = malloc(n + 1);
    if (!lx.root)
        return;
    memcpy(lx.root, r, n);
    lx.root[n] = 0;
    lx.root_len = n;
}

/** @brief Whether guest descriptor fd is in the comma list of an execve's passed descriptors (RELIFT_LINUX_EXEC fds=). */
static bool passed(const char *list, int fd)
{
    for (const char *p = list; p && *p >= '0' && *p <= '9';) {
        char *end;
        long v = strtol(p, &end, 10);
        if (v == fd)
            return true;
        p = *end == ',' ? end + 1 : end;
    }
    return false;
}

void lx_fs_init(void)
{
    root_init();
    for (int i = 0; i < LX_MAX_FDS; i++)
        lx.fds[i].host = -1;
    const char *exec = getenv("RELIFT_LINUX_EXEC");
    const char *list = exec ? strstr(exec, "fds=") : NULL;
    if (list)
        list += 4;
    for (int i = 0; i < 3; i++) {
        if (list && !passed(list, i))
            continue;
        int64_t h = hio_dup(i, 16);
        lx.fds[i].host = h < 0 ? i : (int)h;
    }
    for (const char *p = list; p && *p >= '0' && *p <= '9';) {
        char *end;
        long v = strtol(p, &end, 10);
        p = *end == ',' ? end + 1 : end;
        if (v < 3 || v >= LX_MAX_FDS)
            continue;
        int64_t h = hio_dup((int)v, 16);
        if (h >= 0) {
            hio_close((int)v);
            lx.fds[v].host = (int)h;
        }
    }
}

int lx_fs_exec_fds(int (*keep)[2], int cap)
{
    int n = 0;
    for (int i = 0; i < LX_MAX_FDS && n < cap; i++)
        if (lx.fds[i].host >= 0 && !lx.fds[i].cloexec && !lx.fds[i].virt && !lx.fds[i].vdir) {
            keep[n][0] = i;
            keep[n][1] = lx.fds[i].host;
            n++;
        }
    return n;
}

static int64_t host_path(const char *guest, char *out, size_t cap, bool follow);
static int64_t alloc_fd(int host, int min, bool cloexec);
static size_t guest_name(char *path, size_t len);
static int64_t sys_close(rl_ctx *c, uint8_t *mem, const uint64_t a[6]);
static int64_t iov_get(uint64_t addr, uint64_t cnt, uint64_t (*v)[2], uint64_t *total, uint32_t need);

int64_t lx_fd_install(int host, bool cloexec)
{
    lx_mutex_lock(&lx_big);
    int64_t r = alloc_fd(host, 0, cloexec);
    lx_mutex_unlock(&lx_big);
    return r;
}

void lx_fd_close(int fd)
{
    lx_mutex_lock(&lx_big);
    sys_close(NULL, NULL, (uint64_t[6]){ (uint64_t)fd });
    lx_mutex_unlock(&lx_big);
}

size_t lx_guest_name(char *path, size_t len) { return guest_name(path, len); }

int64_t lx_iov_get(uint64_t addr, uint64_t cnt, uint64_t (*v)[2], uint64_t *total, uint32_t need) { return iov_get(addr, cnt, v, total, need); }

int64_t lx_host_path(const char *path, char *out, size_t cap) { return host_path(path, out, cap, true); }

static int dir_arg(int64_t dirfd, const char *path, int *host);

int64_t lx_host_path_at(const char *path, char *out, size_t cap, bool follow) { return host_path(path, out, cap, follow); }

int lx_dir_arg(int64_t dirfd, const char *path, int *host) { return dir_arg(dirfd, path, host); }

void lx_fs_shutdown(void)
{
    for (int i = 0; i < LX_MAX_FDS; i++)
        if (lx.fds[i].host >= 0) {
            if (lx.fds[i].dir)
                hio_dir_close(lx.fds[i].dir);
            hio_close(lx.fds[i].host);
            lx.fds[i].host = -1;
        }
}

int lx_host_fd(int fd)
{
    if (fd < 0 || fd >= LX_MAX_FDS)
        return -1;
    return lx.fds[fd].host;
}

static int64_t alloc_fd(int host, int min, bool cloexec)
{
    for (int i = min < 0 ? 0 : min; i < LX_MAX_FDS; i++)
        if (lx.fds[i].host < 0 && !lx.fds[i].virt && !lx.fds[i].vdir) {
            lx.fds[i] = (lx_fd){ .host = host, .cloexec = cloexec };
            return i;
        }
    if (host >= 0)
        hio_close(host);
    return -L_EMFILE;
}

const char *lx_vdir_fd(int fd)
{
    if (fd < 0 || fd >= LX_MAX_FDS)
        return NULL;
    return lx.fds[fd].vdir;
}

const rl_linux_module *lx_virt_fd(int fd)
{
    if (fd < 0 || fd >= LX_MAX_FDS)
        return NULL;
    return lx.fds[fd].virt;
}

/** @brief The paths the loader's configuration lives at: a dynamic program finds none, whatever the machine under it has. */
static const char *const loader_absent[] = { "/etc/ld.so.cache", "/etc/ld.so.preload", "/etc/ld.so.conf" };

/** @brief Writes the lexically normalized absolute path (no ".", ".." or doubled slashes) to out; false for a relative or too long path. */
static bool normalize(const char *path, char *out, size_t cap)
{
    if (path[0] != '/')
        return false;
    size_t n = 0;
    const char *p = path;
    while (*p) {
        while (*p == '/')
            p++;
        const char *e = p;
        while (*e && *e != '/')
            e++;
        size_t len = (size_t)(e - p);
        if (len == 0) {
            break;
        } else if (len == 1 && p[0] == '.') {
        } else if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (n > 0 && out[n - 1] != '/')
                n--;
            if (n > 0)
                n--;
        } else {
            if (n + 1 + len + 1 > cap)
                return false;
            out[n++] = '/';
            memcpy(out + n, p, len);
            n += len;
        }
        p = e;
    }
    if (n == 0)
        out[n++] = '/';
    out[n] = 0;
    return true;
}

/** @brief The first components that stay the host's under a guest root: the kernel's views and the scratch directories. */
static const char *const root_passthrough[] = { "proc", "dev", "sys", "tmp", "run" };

/** @brief Whether the normalized absolute guest path lies in the guest root: the root holds its first component and that is not a host view. */
static bool in_root(const char *norm)
{
    const char *first = norm + 1;
    size_t len = strcspn(first, "/");
    if (len == 0)
        return true;
    for (size_t i = 0; i < sizeof root_passthrough / sizeof *root_passthrough; i++)
        if (strlen(root_passthrough[i]) == len && memcmp(first, root_passthrough[i], len) == 0)
            return false;
    char host[PATH_CAP];
    if (lx.root_len + 1 + len >= sizeof host)
        return false;
    memcpy(host, lx.root, lx.root_len);
    host[lx.root_len] = '/';
    memcpy(host + lx.root_len + 1, first, len);
    host[lx.root_len + 1 + len] = 0;
    l_stat st;
    return hio_stat(HIO_CWD, host, true, &st) == 0;
}

/**
 * @brief The host path of a guest path under the guest root: an absolute
 * path in the root (in_root) is looked up component by component below
 * it, its symbolic links read and followed as the guest sees them (an
 * absolute target starts again at the root, at most 40 of them), the last
 * component's only when follow is set; any other path is the host's as it
 * is. A relative path stays relative to the host's working directory,
 * which getcwd reports as a guest path. 0, or -ENAMETOOLONG or -ELOOP.
 */
static int64_t host_path(const char *guest, char *out, size_t cap, bool follow)
{
    char norm[PATH_CAP];
    if (!lx.root || guest[0] != '/' || !normalize(guest, norm, sizeof norm) || !in_root(norm)) {
        size_t n = strlen(guest);
        if (n >= cap)
            return -L_ENAMETOOLONG;
        memcpy(out, guest, n + 1);
        return 0;
    }
    size_t glen = strlen(guest);
    bool slash = glen > 1 && guest[glen - 1] == '/';
    char done[PATH_CAP], rest[PATH_CAP];
    size_t dlen = 0;
    done[0] = 0;
    memcpy(rest, norm, strlen(norm) + 1);
    int links = 0;
    const char *p = rest;
    for (;;) {
        while (*p == '/')
            p++;
        if (!*p)
            break;
        size_t len = strcspn(p, "/");
        if (len == 2 && p[0] == '.' && p[1] == '.') {
            while (dlen > 0 && done[dlen - 1] != '/')
                dlen--;
            if (dlen > 0)
                dlen--;
            done[dlen] = 0;
            p += len;
            continue;
        }
        if (len == 1 && p[0] == '.') {
            p += len;
            continue;
        }
        if (dlen + 1 + len >= sizeof done)
            return -L_ENAMETOOLONG;
        size_t mark = dlen;
        done[dlen++] = '/';
        memcpy(done + dlen, p, len);
        dlen += len;
        done[dlen] = 0;
        p += len;
        bool last = strspn(p, "/") == strlen(p);
        if (last && !follow)
            continue;
        char host[PATH_CAP], target[PATH_CAP];
        if (lx.root_len + dlen >= sizeof host)
            return -L_ENAMETOOLONG;
        memcpy(host, lx.root, lx.root_len);
        memcpy(host + lx.root_len, done, dlen + 1);
        l_stat st;
        if (hio_stat(HIO_CWD, host, true, &st) != 0 || (st.st_mode & L_S_IFMT) != L_S_IFLNK)
            continue;
        if (++links > 40)
            return -L_ELOOP;
        int64_t n = hio_readlink(HIO_CWD, host, target, sizeof target - 1);
        if (n <= 0)
            continue;
        target[n] = 0;
        char next[PATH_CAP];
        size_t tail = strlen(p);
        if (target[0] == '/') {
            dlen = 0;
        } else {
            dlen = mark;
        }
        done[dlen] = 0;
        if ((size_t)n + 1 + tail >= sizeof next)
            return -L_ENAMETOOLONG;
        memcpy(next, target, (size_t)n);
        next[n] = '/';
        memcpy(next + n + 1, p, tail + 1);
        memcpy(rest, next, strlen(next) + 1);
        p = rest;
    }
    if (dlen == 0) {
        done[0] = '/';
        done[1] = 0;
        dlen = 1;
    }
    if (!in_root(done)) {
        if (dlen + 1 >= cap)
            return -L_ENAMETOOLONG;
        memcpy(out, done, dlen + 1);
        return 0;
    }
    size_t n = lx.root_len + (dlen == 1 ? 0 : dlen);
    if (n + 2 >= cap)
        return -L_ENAMETOOLONG;
    memcpy(out, lx.root, lx.root_len);
    if (dlen == 1)
        out[lx.root_len] = 0;
    else
        memcpy(out + lx.root_len, done, dlen + 1);
    if (slash || dlen == 1) {
        out[n] = '/';
        out[n + 1] = 0;
    }
    return 0;
}

/** @brief The guest's name for a host path: a path below the guest root loses the root's prefix, any other stays; in place, returns the new length. */
static size_t guest_name(char *path, size_t len)
{
    if (!lx.root || len < lx.root_len || memcmp(path, lx.root, lx.root_len) != 0)
        return len;
    if (len == lx.root_len) {
        path[0] = '/';
        path[1] = 0;
        return 1;
    }
    if (path[lx.root_len] != '/')
        return len;
    memmove(path, path + lx.root_len, len - lx.root_len + 1);
    return len - lx.root_len;
}

/** @brief The module of the program that answers to the absolute path, NULL when none does. */
static const rl_linux_module *module_at(const char *path)
{
    char norm[PATH_CAP];
    if (!lx.prog || !normalize(path, norm, sizeof norm))
        return NULL;
    for (uint32_t i = 0; i < lx.prog->module_count; i++) {
        const rl_linux_module *m = &lx.prog->modules[i];
        for (uint32_t k = 0; k < m->path_count; k++)
            if (strcmp(m->paths[k], norm) == 0)
                return m;
    }
    return NULL;
}

static bool vdir_exists(const char *dir);

/** @brief Whether the absolute guest path names a file or directory of the guest root itself (a directory the import's files lie in is then the root's real one, with every file in it). */
static bool root_real(const char *path)
{
    if (!lx.root || path[0] != '/')
        return false;
    char hp[PATH_CAP];
    if (host_path(path, hp, sizeof hp, true) != 0 || strncmp(hp, lx.root, lx.root_len) != 0)
        return false;
    l_stat st;
    return hio_stat(HIO_CWD, hp, false, &st) == 0;
}

/** @brief Whether the file name is a shared object's: it has a ".so" that ends the name or a version suffix. */
static bool shared_object_name(const char *leaf)
{
    for (const char *p = strstr(leaf, ".so"); p; p = strstr(p + 1, ".so"))
        if (p[3] == 0 || p[3] == '.')
            return true;
    return false;
}

/** @brief Whether some module of the program is named leaf (a file of that name elsewhere is a probe miss of the loader's search). */
static bool module_named(const char *leaf)
{
    for (uint32_t i = 0; i < lx.prog->module_count; i++) {
        const rl_linux_module *m = &lx.prog->modules[i];
        for (uint32_t k = 0; k < m->path_count; k++) {
            const char *p = strrchr(m->paths[k], '/');
            if (strcmp(p ? p + 1 : m->paths[k], leaf) == 0)
                return true;
        }
    }
    return false;
}

/**
 * @brief What a dynamic program's open of a path that is not a module
 * finds: the loader's configuration and the probes of its library search
 * find nothing (a miss must not reach a file of the machine the program
 * runs on, whose libraries are not the ones the program was lifted with),
 * as does any other name in a directory the import's files lie in, and a
 * shared object the import does not hold at all: a stat or access of it
 * (c NULL) or an open of one the guest root does not hold finds nothing,
 * an open of one that exists (a load of an unknown name) stops the
 * program with a report. Returns false when the open goes on to the host
 * file system.
 */
static bool loader_miss(rl_ctx *c, const char *path, int64_t *result)
{
    char norm[PATH_CAP];
    if (!lx.dynamic || !normalize(path, norm, sizeof norm))
        return false;
    for (size_t i = 0; i < sizeof loader_absent / sizeof *loader_absent; i++)
        if (strcmp(norm, loader_absent[i]) == 0) {
            *result = -L_ENOENT;
            return true;
        }
    const char *leaf = strrchr(norm, '/');
    leaf = leaf ? leaf + 1 : norm;
    char parent[PATH_CAP];
    size_t plen = (size_t)(leaf - norm);
    memcpy(parent, norm, plen);
    parent[plen > 1 ? plen - 1 : plen] = 0;
    if (plen > 1 && vdir_exists(parent) && !root_real(norm)) {
        *result = -L_ENOENT;
        return true;
    }
    if (!shared_object_name(leaf))
        return false;
    if (module_named(leaf)) {
        *result = -L_ENOENT;
        return true;
    }
    if (!c || (lx.root && !root_real(norm))) {
        /* A stat or access probe, or an open of a name the guest root
         * does not hold either: nothing is there, as for the loader's own
         * probes. Only an open of a shared object that exists stops. */
        *result = -L_ENOENT;
        return true;
    }
    rl_guest_fatal(c, "linux", "the program loads the library %s, which is not in the import: import it with a sysroot that holds it", norm);
    *result = LX_PENDING;
    return true;
}

/** @brief Whether some file of the program lies under the directory path (without a trailing slash). */
static bool vdir_exists(const char *dir)
{
    char norm[PATH_CAP];
    if (!lx.prog || !normalize(dir, norm, sizeof norm))
        return false;
    size_t n = strcmp(norm, "/") == 0 ? 0 : strlen(norm);
    for (uint32_t i = 0; i < lx.prog->module_count; i++) {
        const rl_linux_module *m = &lx.prog->modules[i];
        for (uint32_t k = 0; k < m->path_count; k++)
            if (strncmp(m->paths[k], norm, n) == 0 && m->paths[k][n] == '/' && m->paths[k][n + 1] != 0)
                return true;
    }
    return false;
}

/**
 * @brief The index-th distinct entry under the directory (its first path
 * component below it, a directory when more follows), counting "." and "..";
 * false past the last one.
 */
static bool vdir_entry(const char *dir, uint64_t index, char *name, uint8_t *type)
{
    if (index == 0 || index == 1) {
        strcpy(name, index ? ".." : ".");
        *type = L_DT_DIR;
        return true;
    }
    size_t n = strcmp(dir, "/") == 0 ? 0 : strlen(dir);
    uint64_t seen = 2;
    for (uint32_t i = 0; i < lx.prog->module_count; i++) {
        const rl_linux_module *m = &lx.prog->modules[i];
        for (uint32_t k = 0; k < m->path_count; k++) {
            const char *p = m->paths[k];
            if (strncmp(p, dir, n) != 0 || p[n] != '/' || p[n + 1] == 0)
                continue;
            const char *leaf = p + n + 1;
            size_t len = strcspn(leaf, "/");
            if (len >= NAME_CAP)
                continue;
            bool dup = false;
            for (uint32_t j = 0; j <= i && !dup; j++) {
                const rl_linux_module *o = &lx.prog->modules[j];
                for (uint32_t l = 0; l < o->path_count && (j < i || l < k); l++) {
                    const char *q = o->paths[l];
                    if (strncmp(q, dir, n) == 0 && q[n] == '/' && strncmp(q + n + 1, leaf, len) == 0 &&
                        (q[n + 1 + len] == 0 || q[n + 1 + len] == '/') && strcspn(q + n + 1, "/") == len) {
                        dup = true;
                        break;
                    }
                }
            }
            if (dup)
                continue;
            if (seen++ == index) {
                memcpy(name, leaf, len);
                name[len] = 0;
                *type = leaf[len] ? L_DT_DIR : L_DT_REG;
                return true;
            }
        }
    }
    return false;
}

static void vdir_stat(l_stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_dev = 0x7e57;
    st->st_ino = 0x900;
    st->st_mode = L_S_IFDIR | 0755;
    st->st_nlink = 2;
    st->st_blksize = 4096;
}

static void virt_stat(const rl_linux_module *m, l_stat *st)
{
    memset(st, 0, sizeof *st);
    st->st_dev = 0x7e57;
    st->st_ino = 0x1000 + (uint64_t)(m - lx.prog->modules);
    st->st_mode = L_S_IFREG | 0755;
    st->st_nlink = 1;
    st->st_size = (int64_t)m->file_size;
    st->st_blksize = 4096;
    st->st_blocks = (int64_t)((m->file_size + 511) / 512);
}

static int dir_arg(int64_t dirfd, const char *path, int *host)
{
    if (path[0] == '/' || (int)dirfd == L_AT_FDCWD) {
        *host = HIO_CWD;
        return 0;
    }
    *host = lx_host_fd((int)dirfd);
    return *host < 0 ? -L_EBADF : 0;
}

static int64_t path_arg(uint64_t addr, char *buf)
{
    int64_t n = lx_cstr(addr, buf, PATH_CAP);
    return n < 0 ? n : 0;
}

static void epipe(int64_t r)
{
    if (r == -L_EPIPE)
        lx_signal_raise(L_SIGPIPE, L_SI_USER);
}

int64_t lx_fill_from_file(int host, uint64_t addr, uint64_t len, uint64_t off)
{
    uint8_t *base = rl_mem_base();
    uint64_t done = 0;
    while (done < len) {
        int64_t r = hio_pread(host, base + addr + done, len - done, off + done);
        if (r < 0)
            return r;
        if (r == 0)
            break;
        done += (uint64_t)r;
    }
    return 0;
}

/** @brief proc_view's answer when the path is not one it rewrites. */
#define PROC_HOST INT64_MIN

/**
 * @brief The thread count a guest sees in /proc/<self>/stat and status:
 * the host process also runs the Linux layer's signal and timer thread,
 * which is not the program's (CPython warns that a fork is unsafe when the
 * count is above one).
 */
static int proc_threads(void)
{
    /* openat runs under lx_big, which guards the count. */
    int n = lx.live;
    return n > 0 ? n : 1;
}

/** @brief Replaces the num_threads field (the 20th) of a /proc stat line in place; the new length. */
static size_t stat_threads(char *buf, size_t n, size_t cap, int threads)
{
    char *rp = NULL;
    for (size_t i = 0; i < n; i++)
        if (buf[i] == ')')
            rp = buf + i;
    if (!rp)
        return n;
    char *p = rp + 1;
    for (int field = 2; field < 19 && p < buf + n; field++) {
        p = memchr(p + 1, ' ', (size_t)(buf + n - p - 1));
        if (!p)
            return n;
    }
    char *start = p + 1, *end = memchr(start, ' ', (size_t)(buf + n - start));
    if (!end)
        return n;
    char num[16];
    int k = snprintf(num, sizeof num, "%d", threads);
    size_t tail = (size_t)(buf + n - end);
    if ((size_t)(start - buf) + (size_t)k + tail > cap)
        return n;
    memmove(start + k, end, tail);
    memcpy(start, num, (size_t)k);
    return (size_t)(start - buf) + (size_t)k + tail;
}

/** @brief Replaces the value of the Threads: line of a /proc status file in place; the new length. */
static size_t status_threads(char *buf, size_t n, size_t cap, int threads)
{
    buf[n < cap ? n : cap - 1] = 0;
    char *line = strstr(buf, "\nThreads:");
    if (!line)
        return n;
    char *start = line + 9;
    while (*start == '\t' || *start == ' ')
        start++;
    char *end = strchr(start, '\n');
    if (!end)
        return n;
    char num[16];
    int k = snprintf(num, sizeof num, "%d", threads);
    size_t tail = (size_t)(buf + n - end);
    if ((size_t)(start - buf) + (size_t)k + tail > cap)
        return n;
    memmove(start + k, end, tail);
    memcpy(start, num, (size_t)k);
    return (size_t)(start - buf) + (size_t)k + tail;
}

/**
 * @brief An open of the process's own /proc stat or status file (self,
 * thread-self or its pid), read-only: the host's file with the thread
 * count the guest's own, served from a pipe that holds the whole text. Any
 * other path gives PROC_HOST.
 */
static int64_t proc_view(const char *path, uint32_t flags)
{
    char norm[PATH_CAP], own[32];
    if (!normalize(path, norm, sizeof norm) || strncmp(norm, "/proc/", 6) != 0 || (flags & L_O_ACCMODE) != L_O_RDONLY)
        return PROC_HOST;
    const char *rest = norm + 6;
    snprintf(own, sizeof own, "%d/", (int)lx.pid);
    if (strncmp(rest, "self/", 5) == 0)
        rest += 5;
    else if (strncmp(rest, "thread-self/", 12) == 0)
        rest += 12;
    else if (strncmp(rest, own, strlen(own)) == 0)
        rest += strlen(own);
    else
        return PROC_HOST;
    bool stat = strcmp(rest, "stat") == 0;
    if (!stat && strcmp(rest, "status") != 0)
        return PROC_HOST;
    char host[32];
    snprintf(host, sizeof host, "/proc/self/%s", rest);
    int64_t h = hio_open(HIO_CWD, host, L_O_RDONLY, 0);
    if (h < 0)
        return h;
    char buf[8192];
    size_t n = 0;
    for (int64_t r; n < sizeof buf - 64 && (r = hio_read((int)h, buf + n, sizeof buf - 64 - n)) > 0;)
        n += (size_t)r;
    hio_close((int)h);
    int threads = proc_threads();
    n = stat ? stat_threads(buf, n, sizeof buf, threads) : status_threads(buf, n, sizeof buf, threads);
    int p[2];
    int64_t rc = hio_pipe(p);
    if (rc < 0)
        return rc;
    int64_t w = hio_write(p[1], buf, n);
    hio_close(p[1]);
    if (w != (int64_t)n) {
        hio_close(p[0]);
        return w < 0 ? w : -L_EIO;
    }
    return alloc_fd(p[0], 0, (flags & L_O_CLOEXEC) != 0);
}

static int64_t sys_openat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP];
    int64_t rc = path_arg(a[1], path);
    if (rc < 0)
        return rc;
    int dir;
    rc = dir_arg((int64_t)(int32_t)a[0], path, &dir);
    if (rc < 0)
        return rc;
    uint32_t flags = (uint32_t)a[2];
    uint32_t mode = (uint32_t)a[3] & ~lx.umask;
    const rl_linux_module *vm = module_at(path);
    if (vm) {
        if ((flags & L_O_ACCMODE) != L_O_RDONLY || (flags & (L_O_CREAT | L_O_TRUNC)))
            return -L_EACCES;
        int64_t fd = alloc_fd(-1, 0, (flags & L_O_CLOEXEC) != 0);
        if (fd >= 0) {
            lx.fds[fd].virt = vm;
            if (vm->role == RL_LINUX_LIBRARY)
                lx.pending_lib = vm;
        }
        return fd;
    }
    if (vdir_exists(path) && !root_real(path)) {
        if ((flags & L_O_ACCMODE) != L_O_RDONLY || (flags & (L_O_CREAT | L_O_TRUNC)))
            return -L_EISDIR;
        char norm[PATH_CAP];
        normalize(path, norm, sizeof norm);
        char *copy = strdup(norm);
        if (!copy)
            return -L_ENOMEM;
        int64_t fd = alloc_fd(-1, 0, (flags & L_O_CLOEXEC) != 0);
        if (fd < 0) {
            free(copy);
            return fd;
        }
        lx.fds[fd].vdir = copy;
        return fd;
    }
    int64_t miss;
    if (loader_miss(c, path, &miss))
        return miss;
    int64_t synth = proc_view(path, flags);
    if (synth != PROC_HOST)
        return synth;
    char hp[PATH_CAP];
    rc = host_path(path, hp, sizeof hp, !(flags & L_O_NOFOLLOW) && (flags & (L_O_CREAT | L_O_EXCL)) != (L_O_CREAT | L_O_EXCL));
    if (rc < 0)
        return rc;
    int64_t h = hio_open(dir, hp, flags, (flags & L_O_CREAT) ? mode : 0);
    if (h < 0)
        return h;
    return alloc_fd((int)h, 0, (flags & L_O_CLOEXEC) != 0);
}

/** @brief close_range(2): closes the descriptors first to last, or with CLOSE_RANGE_CLOEXEC (4) marks them close-on-exec; CLOSE_RANGE_UNSHARE (2) has nothing to unshare. */
static int64_t sys_close_range(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    uint32_t first = (uint32_t)a[0], last = (uint32_t)a[1], flags = (uint32_t)a[2];
    if ((flags & ~6u) || first > last)
        return -L_EINVAL;
    for (uint32_t fd = first; fd < LX_MAX_FDS && fd <= last; fd++) {
        if (lx.fds[fd].host < 0 && !lx.fds[fd].virt && !lx.fds[fd].vdir)
            continue;
        if (flags & 4)
            lx.fds[fd].cloexec = true;
        else
            sys_close(c, mem, (uint64_t[6]){ fd });
    }
    return 0;
}

static int64_t sys_close(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int fd = (int)a[0];
    if (lx_virt_fd(fd) || lx_vdir_fd(fd)) {
        if (lx.pending_lib == lx.fds[fd].virt)
            lx.pending_lib = NULL;
        free(lx.fds[fd].vdir);
        lx.fds[fd] = (lx_fd){ .host = -1 };
        return 0;
    }
    if (lx_host_fd(fd) < 0)
        return -L_EBADF;
    lx_fd *f = &lx.fds[fd];
    int64_t rc;
    if (f->dir)
        hio_dir_close(f->dir);
    rc = hio_close(f->host);
    *f = (lx_fd){ .host = -1 };
    return rc;
}

/**
 * @brief Before a read that may block: waits for the descriptor to be
 * readable so that a signal can interrupt the wait. 0 to go on with the
 * read, or -ERESTARTSYS (a read on a pipe, terminal or socket is restarted
 * after a handler with SA_RESTART, signal(7)).
 */
/** @brief The descriptor's kind (lx_fd.kind), looked at once: 1 a regular file or directory, 3 a pipe or FIFO, 2 anything else. */
static uint8_t fd_kind(int fd, int h)
{
    lx_fd *f = &lx.fds[fd];
    if (f->kind == 0) {
        l_stat st;
        uint32_t t = hio_fstat(h, &st) == 0 ? st.st_mode & L_S_IFMT : 0;
        f->kind = (t == L_S_IFREG || t == L_S_IFDIR) ? 1 : t == L_S_IFIFO ? 3 : t == L_S_IFSOCK ? 4 : 2;
    }
    return f->kind;
}

static int64_t wait_readable(int fd, int h)
{
    if (fd_kind(fd, h) == 1)
        return 0;
    int64_t fl = hio_get_status_flags(h);
    if (fl >= 0 && (fl & L_O_NONBLOCK))
        return 0;
    return lx_wait(h, L_POLLIN, -1) == -L_EINTR ? -L_ERESTARTSYS : 0;
}

static int64_t virt_read(const rl_linux_module *m, uint8_t *dst, uint64_t n, uint64_t off)
{
    if (off >= m->file_size)
        return 0;
    if (n > m->file_size - off)
        n = m->file_size - off;
    memcpy(dst, m->file + off, n);
    return (int64_t)n;
}

static int64_t sys_read(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    const rl_linux_module *vm = lx_virt_fd((int)a[0]);
    if (vm) {
        uint64_t n = a[2] > 0x7ffff000u ? 0x7ffff000u : a[2];
        if (!lx_ok(a[1], n, RL_VM_W))
            return -L_EFAULT;
        int64_t r = virt_read(vm, mem + a[1], n, lx.fds[a[0]].virt_pos);
        lx.fds[a[0]].virt_pos += (uint64_t)r;
        return r;
    }
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    uint64_t n = a[2];
    if (n > 0x7ffff000u)
        n = 0x7ffff000u;
    if (!lx_ok(a[1], n, RL_VM_W))
        return -L_EFAULT;
    int64_t w = wait_readable((int)a[0], h);
    if (w < 0)
        return w;
    return hio_read(h, mem + a[1], n);
}

/**
 * @brief A write that a signal can interrupt, as the kernel's: on a
 * blocking pipe the bytes go in pieces the pipe has room for (a write of
 * PIPE_BUF or less in one piece, so it stays atomic) after waiting until
 * it is writable; a signal before the first byte gives -ERESTARTSYS, after
 * some the count written. Other descriptors write as they are.
 */
static int64_t write_interruptible(int fd, int h, const uint8_t *buf, uint64_t n)
{
    uint8_t kind = n ? fd_kind(fd, h) : 0;
    if (kind == 4)
        return lx_sock_write(fd, h, buf, n);
    if (kind != 3)
        return hio_write(h, buf, n);
    int64_t fl = hio_get_status_flags(h);
    if (fl >= 0 && (fl & L_O_NONBLOCK))
        return hio_write(h, buf, n);
    uint64_t done = 0;
    for (;;) {
        if (lx_wait(h, L_POLLOUT, -1) == -L_EINTR)
            return done ? (int64_t)done : -L_ERESTARTSYS;
        uint64_t piece = n - done;
        if (n > 4096 && piece > 4096)
            piece = 4096;
        int64_t r = hio_write(h, buf + done, piece);
        if (r < 0)
            return done ? (int64_t)done : r;
        done += (uint64_t)r;
        if (done == n)
            return (int64_t)done;
    }
}

int64_t lx_fd_write(int fd, const uint8_t *buf, uint64_t n)
{
    int h = lx_host_fd(fd);
    if (h < 0)
        return -L_EBADF;
    int64_t r = write_interruptible(fd, h, buf, n);
    epipe(r);
    return r;
}

static int64_t sys_write(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    uint64_t n = a[2];
    if (n > 0x7ffff000u)
        n = 0x7ffff000u;
    if (!lx_ok(a[1], n, RL_VM_R))
        return -L_EFAULT;
    int64_t r = write_interruptible((int)a[0], h, mem + a[1], n);
    epipe(r);
    return r;
}

static int64_t sys_pread64(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    const rl_linux_module *vm = lx_virt_fd((int)a[0]);
    if (vm) {
        if ((int64_t)a[3] < 0)
            return -L_EINVAL;
        uint64_t n = a[2] > 0x7ffff000u ? 0x7ffff000u : a[2];
        if (!lx_ok(a[1], n, RL_VM_W))
            return -L_EFAULT;
        return virt_read(vm, mem + a[1], n, a[3]);
    }
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    if ((int64_t)a[3] < 0)
        return -L_EINVAL;
    uint64_t n = a[2] > 0x7ffff000u ? 0x7ffff000u : a[2];
    if (!lx_ok(a[1], n, RL_VM_W))
        return -L_EFAULT;
    return hio_pread(h, mem + a[1], n, a[3]);
}

static int64_t sys_pwrite64(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    if ((int64_t)a[3] < 0)
        return -L_EINVAL;
    uint64_t n = a[2] > 0x7ffff000u ? 0x7ffff000u : a[2];
    if (!lx_ok(a[1], n, RL_VM_R))
        return -L_EFAULT;
    int64_t r = hio_pwrite(h, mem + a[1], n, a[3]);
    epipe(r);
    return r;
}

static int64_t iov_get(uint64_t addr, uint64_t cnt, uint64_t (*v)[2], uint64_t *total, uint32_t need)
{
    if (cnt > IOV_MAX_COUNT)
        return -L_EINVAL;
    *total = 0;
    for (uint64_t i = 0; i < cnt; i++) {
        uint64_t e[2];
        if (!lx_get_iov(addr, i, e))
            return -L_EFAULT;
        if (e[1] > 0x7ffff000u || *total + e[1] > 0x7ffff000u)
            return -L_EINVAL;
        if (!lx_ok(e[0], e[1], need))
            return -L_EFAULT;
        v[i][0] = e[0];
        v[i][1] = e[1];
        *total += e[1];
    }
    return 0;
}

static int64_t sys_readv(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    uint64_t v[IOV_MAX_COUNT][2], total;
    int64_t rc = iov_get(a[1], a[2], v, &total, RL_VM_W);
    if (rc < 0)
        return rc;
    int64_t done = 0;
    for (uint64_t i = 0; i < a[2]; i++) {
        if (v[i][1] == 0)
            continue;
        if (!done) {
            int64_t w = wait_readable((int)a[0], h);
            if (w < 0)
                return w;
        }
        int64_t r = hio_read(h, mem + v[i][0], v[i][1]);
        if (r < 0)
            return done ? done : r;
        done += r;
        if ((uint64_t)r < v[i][1])
            break;
    }
    return done;
}

static int64_t sys_writev(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    uint64_t v[IOV_MAX_COUNT][2], total;
    int64_t rc = iov_get(a[1], a[2], v, &total, RL_VM_R);
    if (rc < 0)
        return rc;
    if (total == 0)
        return 0;
    uint8_t *buf = malloc(total);
    if (!buf)
        return -L_ENOMEM;
    uint64_t at = 0;
    for (uint64_t i = 0; i < a[2]; i++) {
        memcpy(buf + at, mem + v[i][0], v[i][1]);
        at += v[i][1];
    }
    int64_t r = write_interruptible((int)a[0], h, buf, total);
    free(buf);
    epipe(r);
    return r;
}

static int64_t sys_lseek(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    const rl_linux_module *vm = lx_virt_fd((int)a[0]);
    if (vm) {
        int64_t base = a[2] == 0 ? 0 : a[2] == 1 ? (int64_t)lx.fds[a[0]].virt_pos : a[2] == 2 ? (int64_t)vm->file_size : -1;
        int64_t at = base + (int64_t)a[1];
        if (base < 0 || at < 0)
            return -L_EINVAL;
        lx.fds[a[0]].virt_pos = (uint64_t)at;
        return at;
    }
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    return hio_lseek(h, (int64_t)a[1], (int)a[2]);
}

static int64_t sys_fstat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    const rl_linux_module *vm = lx_virt_fd((int)a[0]);
    if (vm) {
        l_stat st;
        virt_stat(vm, &st);
        return lx_put_stat(a[1], &st) ? 0 : -L_EFAULT;
    }
    if (lx_vdir_fd((int)a[0])) {
        l_stat st;
        vdir_stat(&st);
        return lx_put_stat(a[1], &st) ? 0 : -L_EFAULT;
    }
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    l_stat st;
    int64_t rc = hio_fstat(h, &st);
    if (rc < 0)
        return rc;
    return lx_put_stat(a[1], &st) ? 0 : -L_EFAULT;
}

static int64_t stat_of(int64_t dirfd, uint64_t path_addr, uint64_t flags, l_stat *st)
{
    char path[PATH_CAP];
    int64_t rc = path_arg(path_addr, path);
    if (rc < 0)
        return rc;
    if (path[0] == 0 && (flags & L_AT_EMPTY_PATH)) {
        if ((int)dirfd == L_AT_FDCWD)
            return hio_stat(HIO_CWD, ".", false, st);
        const rl_linux_module *vm = lx_virt_fd((int)dirfd);
        if (vm) {
            virt_stat(vm, st);
            return 0;
        }
        if (lx_vdir_fd((int)dirfd)) {
            vdir_stat(st);
            return 0;
        }
        int h = lx_host_fd((int)dirfd);
        return h < 0 ? -L_EBADF : hio_fstat(h, st);
    }
    const rl_linux_module *vm = module_at(path);
    if (vm) {
        virt_stat(vm, st);
        return 0;
    }
    if (vdir_exists(path) && !root_real(path)) {
        vdir_stat(st);
        return 0;
    }
    int64_t miss;
    if (loader_miss(NULL, path, &miss) && miss != LX_PENDING)
        return miss;
    int dir;
    rc = dir_arg(dirfd, path, &dir);
    if (rc < 0)
        return rc;
    char hp[PATH_CAP];
    rc = host_path(path, hp, sizeof hp, !(flags & L_AT_SYMLINK_NOFOLLOW));
    if (rc < 0)
        return rc;
    return hio_stat(dir, hp, (flags & L_AT_SYMLINK_NOFOLLOW) != 0, st);
}

static int64_t sys_newfstatat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    l_stat st;
    int64_t rc = stat_of((int64_t)(int32_t)a[0], a[1], a[3], &st);
    if (rc < 0)
        return rc;
    return lx_put_stat(a[2], &st) ? 0 : -L_EFAULT;
}

static uint32_t dev_major(uint64_t d) { return (uint32_t)(((d >> 8) & 0xfff) | ((d >> 32) & ~0xfffull)); }
static uint32_t dev_minor(uint64_t d) { return (uint32_t)((d & 0xff) | ((d >> 12) & ~0xffull)); }

static void put32(uint8_t *b, size_t o, uint32_t v) { memcpy(b + o, &v, 4); }
static void put64(uint8_t *b, size_t o, uint64_t v) { memcpy(b + o, &v, 8); }

static void statx_time(uint8_t *b, size_t o, int64_t sec, uint64_t nsec)
{
    put64(b, o, (uint64_t)sec);
    put32(b, o + 8, (uint32_t)nsec);
}

static int64_t sys_statx(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    l_stat st;
    int64_t rc = stat_of((int64_t)(int32_t)a[0], a[1], a[2], &st);
    if (rc < 0)
        return rc;
    uint8_t x[256];
    memset(x, 0, sizeof x);
    put32(x, 0, 0x7ffu);
    put32(x, 4, (uint32_t)st.st_blksize);
    put32(x, 16, st.st_nlink);
    put32(x, 20, st.st_uid);
    put32(x, 24, st.st_gid);
    x[28] = (uint8_t)st.st_mode;
    x[29] = (uint8_t)(st.st_mode >> 8);
    put64(x, 32, st.st_ino);
    put64(x, 40, (uint64_t)st.st_size);
    put64(x, 48, (uint64_t)st.st_blocks);
    statx_time(x, 64, st.st_atime_sec, st.st_atime_ns);
    statx_time(x, 96, st.st_ctime_sec, st.st_ctime_ns);
    statx_time(x, 112, st.st_mtime_sec, st.st_mtime_ns);
    put32(x, 128, dev_major(st.st_rdev));
    put32(x, 132, dev_minor(st.st_rdev));
    put32(x, 136, dev_major(st.st_dev));
    put32(x, 140, dev_minor(st.st_dev));
    return lx_put(a[4], x, sizeof x) ? 0 : -L_EFAULT;
}

static int64_t sys_ioctl(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    switch ((uint32_t)a[1]) {
    case L_TIOCGWINSZ: {
        uint16_t ws[4];
        int64_t rc = hio_winsize(h, ws);
        if (rc < 0)
            return rc;
        return lx_put(a[2], ws, sizeof ws) ? 0 : -L_EFAULT;
    }
    case L_FIONREAD: {
        int32_t n;
        int64_t rc = hio_nread(h, &n);
        if (rc < 0)
            return rc;
        return lx_put(a[2], &n, sizeof n) ? 0 : -L_EFAULT;
    }
    case L_FIONBIO: {
        int32_t on;
        if (!lx_get(&on, a[2], sizeof on))
            return -L_EFAULT;
        return hio_set_nonblock(h, on != 0);
    }
    default:
        return -L_ENOTTY;
    }
}

static int64_t sys_fcntl(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int fd = (int)a[0];
    int h = lx_host_fd(fd);
    if (h < 0)
        return -L_EBADF;
    switch ((uint32_t)a[1]) {
    case L_F_DUPFD:
    case L_F_DUPFD_CLOEXEC: {
        if ((int64_t)a[2] < 0 || a[2] >= LX_MAX_FDS)
            return -L_EINVAL;
        int64_t n = hio_dup(h, 0);
        if (n < 0)
            return n;
        return alloc_fd((int)n, (int)a[2], (uint32_t)a[1] == L_F_DUPFD_CLOEXEC);
    }
    case L_F_GETFD:
        return lx.fds[fd].cloexec ? L_FD_CLOEXEC : 0;
    case L_F_SETFD:
        lx.fds[fd].cloexec = (a[2] & L_FD_CLOEXEC) != 0;
        return 0;
    case L_F_GETFL: {
        int64_t f = hio_get_status_flags(h);
        return f < 0 ? f : (f | L_O_LARGEFILE);
    }
    case L_F_SETFL:
        return hio_set_status_flags(h, (uint32_t)a[2]);
    case 5:
    case 6:
    case 7:
    case 36:
    case 37:
    case 38:
        return lx_record_lock(h, (uint32_t)a[1], a[2]);
    case 1033:
        return hio_seals(h, true, (uint32_t)a[2]);
    case 1034:
        return hio_seals(h, false, 0);
    default:
        return -L_EINVAL;
    }
}

static int64_t sys_dup(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    int64_t n = hio_dup(h, 0);
    return n < 0 ? n : alloc_fd((int)n, 0, false);
}

static void drop_fd(int fd)
{
    lx_fd *f = &lx.fds[fd];
    if (f->dir)
        hio_dir_close(f->dir);
    if (f->host >= 0)
        hio_close(f->host);
    free(f->vdir);
    *f = (lx_fd){ .host = -1 };
}

static int64_t sys_dup3(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int from = (int)a[0], to = (int)a[1];
    int h = lx_host_fd(from);
    if (h < 0)
        return -L_EBADF;
    if (from == to || (a[2] & ~(uint64_t)L_O_CLOEXEC))
        return -L_EINVAL;
    if (to < 0 || to >= LX_MAX_FDS)
        return -L_EBADF;
    int64_t n = hio_dup(h, 0);
    if (n < 0)
        return n;
    if (lx.fds[to].host >= 0 || lx.fds[to].virt || lx.fds[to].vdir)
        drop_fd(to);
    lx.fds[to] = (lx_fd){ .host = (int)n, .cloexec = (a[2] & L_O_CLOEXEC) != 0 };
    return to;
}

static int64_t sys_pipe2(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if (a[1] & ~(uint64_t)(L_O_CLOEXEC | L_O_NONBLOCK))
        return -L_EINVAL;
    int p[2];
    int64_t rc = hio_pipe(p);
    if (rc < 0)
        return rc;
    if (a[1] & L_O_NONBLOCK) {
        hio_set_nonblock(p[0], true);
        hio_set_nonblock(p[1], true);
    }
    int64_t r = alloc_fd(p[0], 0, (a[1] & L_O_CLOEXEC) != 0);
    if (r < 0) {
        hio_close(p[1]);
        return r;
    }
    int64_t w = alloc_fd(p[1], 0, (a[1] & L_O_CLOEXEC) != 0);
    if (w < 0) {
        sys_close(NULL, NULL, (uint64_t[6]){ (uint64_t)r });
        return w;
    }
    int32_t fds[2] = { (int32_t)r, (int32_t)w };
    if (!lx_put(a[0], fds, sizeof fds)) {
        sys_close(NULL, NULL, (uint64_t[6]){ (uint64_t)r });
        sys_close(NULL, NULL, (uint64_t[6]){ (uint64_t)w });
        return -L_EFAULT;
    }
    return 0;
}

static int64_t sys_getcwd(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char buf[PATH_CAP];
    int64_t n = hio_getcwd(buf, sizeof buf);
    if (n < 0)
        return n;
    n = (int64_t)guest_name(buf, (size_t)n - 1) + 1;
    if ((uint64_t)n > a[1])
        return -L_ERANGE;
    return lx_put(a[0], buf, (uint64_t)n) ? n : -L_EFAULT;
}

static int64_t sys_chdir(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP];
    int64_t rc = path_arg(a[0], path);
    char hp[PATH_CAP];
    if (rc == 0)
        rc = host_path(path, hp, sizeof hp, true);
    return rc < 0 ? rc : hio_chdir(hp);
}

static int64_t sys_mkdirat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP];
    int64_t rc = path_arg(a[1], path);
    if (rc < 0)
        return rc;
    int dir;
    rc = dir_arg((int64_t)(int32_t)a[0], path, &dir);
    char hp[PATH_CAP];
    if (rc == 0)
        rc = host_path(path, hp, sizeof hp, false);
    return rc < 0 ? rc : hio_mkdir(dir, hp, (uint32_t)a[2] & ~lx.umask);
}

static int64_t sys_unlinkat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP];
    int64_t rc = path_arg(a[1], path);
    if (rc < 0)
        return rc;
    if (a[2] & ~(uint64_t)L_AT_REMOVEDIR)
        return -L_EINVAL;
    int dir;
    rc = dir_arg((int64_t)(int32_t)a[0], path, &dir);
    char hp[PATH_CAP];
    if (rc == 0)
        rc = host_path(path, hp, sizeof hp, false);
    return rc < 0 ? rc : hio_unlink(dir, hp, (a[2] & L_AT_REMOVEDIR) != 0);
}

static int64_t sys_symlinkat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char target[PATH_CAP], path[PATH_CAP], hp[PATH_CAP];
    int64_t rc = path_arg(a[0], target);
    if (rc == 0)
        rc = path_arg(a[2], path);
    int dir;
    if (rc == 0)
        rc = dir_arg((int64_t)(int32_t)a[1], path, &dir);
    if (rc == 0)
        rc = host_path(path, hp, sizeof hp, false);
    return rc < 0 ? rc : hio_symlink(target, dir, hp);
}

static int64_t sys_linkat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if (a[4] & ~(uint64_t)0x400)
        return -L_EINVAL;
    char op[PATH_CAP], np[PATH_CAP], ho[PATH_CAP], hn[PATH_CAP];
    int64_t rc = path_arg(a[1], op);
    if (rc == 0)
        rc = path_arg(a[3], np);
    int od, nd;
    if (rc == 0)
        rc = dir_arg((int64_t)(int32_t)a[0], op, &od);
    if (rc == 0)
        rc = dir_arg((int64_t)(int32_t)a[2], np, &nd);
    if (rc == 0)
        rc = host_path(op, ho, sizeof ho, (a[4] & 0x400) != 0);
    if (rc == 0)
        rc = host_path(np, hn, sizeof hn, false);
    return rc < 0 ? rc : hio_link(od, ho, nd, hn, (a[4] & 0x400) != 0);
}

/** @brief mknodat(2): named pipes and plain files; devices need privileges the guest does not have. */
static int64_t sys_mknodat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP], hp[PATH_CAP];
    int64_t rc = path_arg(a[1], path);
    int dir;
    if (rc == 0)
        rc = dir_arg((int64_t)(int32_t)a[0], path, &dir);
    if (rc == 0)
        rc = host_path(path, hp, sizeof hp, false);
    if (rc < 0)
        return rc;
    uint32_t mode = (uint32_t)a[2];
    uint32_t perm = mode & 07777 & ~lx.umask;
    switch (mode & L_S_IFMT) {
    case L_S_IFIFO:
        return hio_mkfifo(dir, hp, perm);
    case 0:
    case L_S_IFREG: {
        int64_t h = hio_open(dir, hp, L_O_CREAT | L_O_EXCL | L_O_WRONLY, perm);
        return h < 0 ? h : hio_close((int)h);
    }
    default:
        return -L_EPERM;
    }
}

static int64_t sys_statfs(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP], hp[PATH_CAP];
    int64_t rc = path_arg(a[0], path);
    if (rc == 0 && (module_at(path) || vdir_exists(path)) && !root_real(path))
        strcpy(path, "/");
    if (rc == 0)
        rc = host_path(path, hp, sizeof hp, true);
    l_statfs st;
    if (rc == 0)
        rc = hio_statfs(-1, hp, &st);
    return rc < 0 ? rc : lx_put(a[1], &st, sizeof st) ? 0 : -L_EFAULT;
}

static int64_t sys_fstatfs(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    l_statfs st;
    int64_t rc;
    int h = lx_host_fd((int)a[0]);
    if (h >= 0)
        rc = hio_statfs(h, NULL, &st);
    else if (lx_virt_fd((int)a[0]) || lx_vdir_fd((int)a[0]))
        rc = hio_statfs(-1, "/", &st);
    else
        rc = -L_EBADF;
    return rc < 0 ? rc : lx_put(a[1], &st, sizeof st) ? 0 : -L_EFAULT;
}

static int64_t sys_renameat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char op[PATH_CAP], np[PATH_CAP];
    int64_t rc = path_arg(a[1], op);
    if (rc < 0)
        return rc;
    rc = path_arg(a[3], np);
    if (rc < 0)
        return rc;
    int od, nd;
    rc = dir_arg((int64_t)(int32_t)a[0], op, &od);
    if (rc < 0)
        return rc;
    rc = dir_arg((int64_t)(int32_t)a[2], np, &nd);
    if (rc < 0)
        return rc;
    char ho[PATH_CAP], hn[PATH_CAP];
    rc = host_path(op, ho, sizeof ho, false);
    if (rc == 0)
        rc = host_path(np, hn, sizeof hn, false);
    return rc < 0 ? rc : hio_rename(od, ho, nd, hn);
}

static int64_t sys_faccessat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP];
    int64_t rc = path_arg(a[1], path);
    if (rc < 0)
        return rc;
    if (a[2] & ~7ull)
        return -L_EINVAL;
    if (module_at(path) || (vdir_exists(path) && !root_real(path)))
        return (a[2] & 2) ? -L_EACCES : 0;
    int64_t miss;
    if (loader_miss(NULL, path, &miss) && miss != LX_PENDING)
        return miss;
    int dir;
    rc = dir_arg((int64_t)(int32_t)a[0], path, &dir);
    char hp[PATH_CAP];
    if (rc == 0)
        rc = host_path(path, hp, sizeof hp, true);
    return rc < 0 ? rc : hio_access(dir, hp, (uint32_t)a[2]);
}

static int64_t sys_readlinkat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP], out[PATH_CAP];
    int64_t rc = path_arg(a[1], path);
    if (rc < 0)
        return rc;
    char norm[PATH_CAP];
    if (lx.prog && normalize(path, norm, sizeof norm) && strcmp(norm, "/proc/self/exe") == 0) {
        size_t n = strlen(lx.prog->exec_path);
        if (n > a[3])
            n = a[3];
        return lx_put(a[2], lx.prog->exec_path, n) ? (int64_t)n : -L_EFAULT;
    }
    int dir;
    rc = dir_arg((int64_t)(int32_t)a[0], path, &dir);
    if (rc < 0)
        return rc;
    char hp[PATH_CAP];
    rc = host_path(path, hp, sizeof hp, false);
    if (rc < 0)
        return rc;
    int64_t n = hio_readlink(dir, hp, out, sizeof out - 1);
    if (n < 0)
        return n;
    if (strncmp(path, "/proc/", 6) == 0) {
        out[n] = 0;
        n = (int64_t)guest_name(out, (size_t)n);
    }
    if ((uint64_t)n > a[3])
        n = (int64_t)a[3];
    return lx_put(a[2], out, (uint64_t)n) ? n : -L_EFAULT;
}

static int64_t sys_ftruncate(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    if ((int64_t)a[1] < 0)
        return -L_EINVAL;
    return hio_ftruncate(h, a[1]);
}

static int64_t sys_fsync(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = lx_host_fd((int)a[0]);
    return h < 0 ? -L_EBADF : hio_fsync(h);
}

static int64_t sys_fchmodat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP];
    int64_t rc = path_arg(a[1], path);
    if (rc < 0)
        return rc;
    int dir;
    rc = dir_arg((int64_t)(int32_t)a[0], path, &dir);
    char hp[PATH_CAP];
    if (rc == 0)
        rc = host_path(path, hp, sizeof hp, true);
    return rc < 0 ? rc : hio_chmod(dir, hp, (uint32_t)a[2]);
}

static int64_t sys_utimensat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP];
    int64_t t[4] = { 0, 0x3fffffff, 0, 0x3fffffff };
    if (a[2]) {
        int64_t in[4];
        if (!lx_get_timespec2(a[2], in))
            return -L_EFAULT;
        memcpy(t, in, sizeof t);
        for (int i = 1; i < 4; i += 2)
            if (t[i] != 0x3fffffff && t[i] != 0x3ffffffe && (t[i] < 0 || t[i] >= 1000000000))
                return -L_EINVAL;
    }
    int dir;
    if (a[1]) {
        int64_t rc = path_arg(a[1], path);
        if (rc < 0)
            return rc;
        rc = dir_arg((int64_t)(int32_t)a[0], path, &dir);
        if (rc < 0)
            return rc;
        char hp[PATH_CAP];
        rc = host_path(path, hp, sizeof hp, !(a[3] & L_AT_SYMLINK_NOFOLLOW));
        if (rc < 0)
            return rc;
        return hio_utimens(dir, hp, t, (a[3] & L_AT_SYMLINK_NOFOLLOW) != 0);
    }
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    return hio_utimens(h, "", t, false);
}

static int64_t sys_umask(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint32_t old = lx.umask;
    lx.umask = (uint32_t)a[0] & 0777;
    hio_umask(lx.umask);
    return old;
}

static int64_t sys_getdents64(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int fd = (int)a[0];
    if (lx_vdir_fd(fd)) {
        lx_fd *v = &lx.fds[fd];
        uint64_t cap = a[2];
        if (!lx_ok(a[1], cap, RL_VM_W))
            return -L_EFAULT;
        uint8_t *out = mem + a[1];
        uint64_t at = 0;
        char name[NAME_CAP];
        uint8_t type;
        while (vdir_entry(v->vdir, v->virt_pos, name, &type)) {
            size_t len = strlen(name);
            uint64_t reclen = (19 + len + 1 + 7) & ~7ull;
            if (at + reclen > cap) {
                if (at == 0)
                    return -L_EINVAL;
                break;
            }
            uint64_t ino = 0x901 + v->virt_pos, off = at + reclen;
            memcpy(out + at, &ino, 8);
            memcpy(out + at + 8, &off, 8);
            uint16_t rl = (uint16_t)reclen;
            memcpy(out + at + 16, &rl, 2);
            out[at + 18] = type;
            memcpy(out + at + 19, name, len + 1);
            memset(out + at + 19 + len + 1, 0, reclen - 19 - len - 1);
            at += reclen;
            v->virt_pos++;
        }
        return (int64_t)at;
    }
    int h = lx_host_fd(fd);
    if (h < 0)
        return -L_EBADF;
    lx_fd *f = &lx.fds[fd];
    if (!f->dir) {
        l_stat st;
        int64_t rc = hio_fstat(h, &st);
        if (rc < 0)
            return rc;
        if ((st.st_mode & L_S_IFMT) != L_S_IFDIR)
            return -L_ENOTDIR;
        int64_t d = hio_dup(h, 0);
        if (d < 0)
            return d;
        f->dir = hio_dir_open((int)d);
        if (!f->dir) {
            hio_close((int)d);
            return -L_ENOMEM;
        }
    }
    uint64_t cap = a[2];
    if (!lx_ok(a[1], cap, RL_VM_W))
        return -L_EFAULT;
    uint8_t *out = mem + a[1];
    uint64_t at = 0;
    for (;;) {
        if (!f->has_stash) {
            int64_t r = hio_dir_next(f->dir, f->stash_name, &f->stash_ino, &f->stash_type);
            if (r < 0)
                return at ? (int64_t)at : r;
            if (r == 0)
                break;
            f->has_stash = true;
        }
        size_t len = strlen(f->stash_name);
        uint64_t reclen = (19 + len + 1 + 7) & ~7ull;
        if (at + reclen > cap) {
            if (at == 0)
                return -L_EINVAL;
            break;
        }
        uint64_t off = at + reclen;
        memcpy(out + at, &f->stash_ino, 8);
        memcpy(out + at + 8, &off, 8);
        uint16_t rl = (uint16_t)reclen;
        memcpy(out + at + 16, &rl, 2);
        out[at + 18] = f->stash_type;
        memcpy(out + at + 19, f->stash_name, len + 1);
        memset(out + at + 19 + len + 1, 0, reclen - 19 - len - 1);
        at += reclen;
        f->has_stash = false;
    }
    return (int64_t)at;
}

static int64_t sys_ppoll(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint64_t n = a[1];
    if (n > LX_MAX_FDS)
        return -L_EINVAL;
    int64_t timeout = -1;
    if (a[2]) {
        l_timespec ts;
        if (!lx_get_timespec(a[2], &ts))
            return -L_EFAULT;
        if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000)
            return -L_EINVAL;
        timeout = ts.sec * 1000000000ll + ts.nsec;
    }
    struct {
        int32_t fd;
        int16_t events;
        int16_t revents;
    } p[LX_MAX_FDS];
    if (!lx_get(p, a[0], n * 8))
        return -L_EFAULT;
    int64_t end = timeout < 0 ? 0 : (int64_t)rl_clock_ns() + timeout;
    int64_t ready = 0;
    for (;;) {
        int valid = 0, only = -1;
        ready = 0;
        for (uint64_t i = 0; i < n; i++) {
            p[i].revents = 0;
            if (p[i].fd < 0)
                continue;
            int h = lx_host_fd(p[i].fd);
            if (h < 0) {
                p[i].revents = L_POLLNVAL;
                ready++;
                continue;
            }
            int64_t r = hio_poll(h, (uint32_t)p[i].events, 0);
            if (r < 0)
                return r;
            p[i].revents = (int16_t)r;
            if (r)
                ready++;
            valid++;
            only = h;
        }
        int64_t left = timeout < 0 ? -1 : end - (int64_t)rl_clock_ns();
        if (ready || timeout == 0 || (timeout > 0 && left <= 0))
            break;
        int64_t w;
        if (valid == 1 && n == 1)
            w = lx_wait(only, (uint32_t)p[0].events, left);
        else
            w = lx_wait(-1, 0, left < 0 || left > 1000000 ? 1000000 : left);
        if (w == -L_EINTR)
            return -L_ERESTARTNOHAND;
    }
    if (!lx_put(a[0], p, n * 8))
        return -L_EFAULT;
    return ready;
}

lx_sys_fn *lx_fs_call(uint32_t nr)
{
    switch (nr) {
    case NR_openat: return sys_openat;
    case NR_close: return sys_close;
    case NR_read: return sys_read;
    case NR_write: return sys_write;
    case NR_pread64: return sys_pread64;
    case NR_pwrite64: return sys_pwrite64;
    case NR_readv: return sys_readv;
    case NR_writev: return sys_writev;
    case NR_lseek: return sys_lseek;
    case NR_fstat: return sys_fstat;
    case NR_newfstatat: return sys_newfstatat;
    case NR_statx: return sys_statx;
    case NR_ioctl: return sys_ioctl;
    case NR_fcntl: return sys_fcntl;
    case NR_dup: return sys_dup;
    case NR_dup3: return sys_dup3;
    case NR_pipe2: return sys_pipe2;
    case NR_getcwd: return sys_getcwd;
    case NR_chdir: return sys_chdir;
    case NR_mkdirat: return sys_mkdirat;
    case NR_mknodat: return sys_mknodat;
    case NR_unlinkat: return sys_unlinkat;
    case NR_symlinkat: return sys_symlinkat;
    case NR_close_range: return sys_close_range;
    case NR_linkat: return sys_linkat;
    case NR_statfs: return sys_statfs;
    case NR_fstatfs: return sys_fstatfs;
    case NR_renameat: return sys_renameat;
    case NR_faccessat: return sys_faccessat;
    case NR_readlinkat: return sys_readlinkat;
    case NR_ftruncate: return sys_ftruncate;
    case NR_fsync:
    case NR_fdatasync: return sys_fsync;
    case NR_fchmodat: return sys_fchmodat;
    case NR_utimensat: return sys_utimensat;
    case NR_umask: return sys_umask;
    case NR_getdents64: return sys_getdents64;
    case NR_ppoll: return sys_ppoll;
    default: return NULL;
    }
}
