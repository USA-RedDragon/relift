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
        if (lx.fds[i].host < 0) {
            lx.fds[i] = (lx_fd){ .host = host, .cloexec = cloexec };
            return i;
        }
    hio_close(host);
    return -L_EMFILE;
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

static int64_t sys_read(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    uint64_t n = a[2];
    if (n > 0x7ffff000u)
        n = 0x7ffff000u;
    if (!lx_ok(a[1], n, RL_VM_W))
        return -L_EFAULT;
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
    int h = lx_host_fd((int)a[0]);
    if (h < 0)
        return -L_EBADF;
    return hio_lseek(h, (int64_t)a[1], (int)a[2]);
}

static int64_t sys_fstat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
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
        int h = lx_host_fd((int)dirfd);
        return h < 0 ? -L_EBADF : hio_fstat(h, st);
    }
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
    if (lx.fds[to].host >= 0)
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
    int64_t ready = 0;
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
        int64_t r = hio_poll(h, (uint32_t)p[i].events, n == 1 ? timeout : 0);
        if (r < 0)
            return r;
        p[i].revents = (int16_t)r;
        if (r)
            ready++;
    }
    if (ready == 0 && n != 1 && timeout > 0)
        rl_sleep_ns((uint64_t)timeout);
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
