/**
 * @file fs.c
 * @brief Files and descriptors: the guest's descriptor table over the
 * host's descriptors and the system calls that use it.
 */
#include "hostio.h"
#include "linux_priv.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define PATH_CAP 4096u
#define NAME_CAP 256u
#define IOV_MAX_COUNT 1024u

void lx_fs_init(void)
{
    for (int i = 0; i < LX_MAX_FDS; i++)
        lx.fds[i].host = -1;
    for (int i = 0; i < 3; i++) {
        int64_t h = hio_dup(i, 16);
        lx.fds[i].host = h < 0 ? i : (int)h;
    }
}

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
 * as does any other name in a directory the import's files lie in, and a shared object the import does not hold at all (a load of an
 * unknown name) stops the program with a report. Returns false when the
 * open goes on to the host file system.
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
    if (plen > 1 && vdir_exists(parent)) {
        *result = -L_ENOENT;
        return true;
    }
    if (!shared_object_name(leaf))
        return false;
    if (module_named(leaf)) {
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
    if (vdir_exists(path)) {
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
    int64_t h = hio_open(dir, path, flags, (flags & L_O_CREAT) ? mode : 0);
    if (h < 0)
        return h;
    return alloc_fd((int)h, 0, (flags & L_O_CLOEXEC) != 0);
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
static int64_t wait_readable(int fd, int h)
{
    lx_fd *f = &lx.fds[fd];
    if (f->kind == 0) {
        l_stat st;
        uint32_t t = hio_fstat(h, &st) == 0 ? st.st_mode & L_S_IFMT : 0;
        f->kind = (t == L_S_IFREG || t == L_S_IFDIR) ? 1 : 2;
    }
    if (f->kind == 1)
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
    int64_t r = hio_write(h, mem + a[1], n);
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
        if (!lx_get(e, addr + 16 * i, 16))
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
    int64_t r = hio_write(h, buf, total);
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
        return lx_put(a[1], &st, sizeof st) ? 0 : -L_EFAULT;
    }
    if (lx_vdir_fd((int)a[0])) {
        l_stat st;
        vdir_stat(&st);
        return lx_put(a[1], &st, sizeof st) ? 0 : -L_EFAULT;
    }
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    l_stat st;
    int64_t rc = hio_fstat(h, &st);
    if (rc < 0)
        return rc;
    return lx_put(a[1], &st, sizeof st) ? 0 : -L_EFAULT;
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
    if (vdir_exists(path)) {
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
    return hio_stat(dir, path, (flags & L_AT_SYMLINK_NOFOLLOW) != 0, st);
}

static int64_t sys_newfstatat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    l_stat st;
    int64_t rc = stat_of((int64_t)(int32_t)a[0], a[1], a[3], &st);
    if (rc < 0)
        return rc;
    return lx_put(a[2], &st, sizeof st) ? 0 : -L_EFAULT;
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
    case 5: {
        uint16_t unlocked = 2;
        return lx_put(a[2], &unlocked, sizeof unlocked) ? 0 : -L_EFAULT;
    }
    case 6:
    case 7:
        return 0;
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
    return rc < 0 ? rc : hio_chdir(path);
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
    return rc < 0 ? rc : hio_mkdir(dir, path, (uint32_t)a[2] & ~lx.umask);
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
    return rc < 0 ? rc : hio_unlink(dir, path, (a[2] & L_AT_REMOVEDIR) != 0);
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
    return hio_rename(od, op, nd, np);
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
    if (module_at(path) || vdir_exists(path))
        return (a[2] & 2) ? -L_EACCES : 0;
    int64_t miss;
    if (loader_miss(NULL, path, &miss) && miss != LX_PENDING)
        return miss;
    int dir;
    rc = dir_arg((int64_t)(int32_t)a[0], path, &dir);
    return rc < 0 ? rc : hio_access(dir, path, (uint32_t)a[2]);
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
    uint64_t cap = a[3] > sizeof out ? sizeof out : a[3];
    int64_t n = hio_readlink(dir, path, out, cap);
    if (n < 0)
        return n;
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
    return rc < 0 ? rc : hio_chmod(dir, path, (uint32_t)a[2]);
}

static int64_t sys_utimensat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP];
    int64_t t[4] = { 0, 0x3fffffff, 0, 0x3fffffff };
    if (a[2]) {
        int64_t in[4];
        if (!lx_get(in, a[2], sizeof in))
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
        return hio_utimens(dir, path, t, (a[3] & L_AT_SYMLINK_NOFOLLOW) != 0);
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
        if (!lx_get(&ts, a[2], sizeof ts))
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
    case NR_unlinkat: return sys_unlinkat;
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
