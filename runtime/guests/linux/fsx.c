/**
 * @file fsx.c
 * @brief More file system calls: fchmod and fchmodat2, ownership (fchown,
 * fchownat), extended attributes, sendfile and pselect6. Paths resolve
 * under the guest root as in fs.c.
 */
#include "hostio.h"
#include "linux_priv.h"

#include <stdlib.h>
#include <string.h>

enum {
    PATH_CAP = 4096,
    NAME_CAP = 256,
    XATTR_MAX = 65536,
    L_AT_SYMLINK_NOFOLLOW_ = 0x100,
    L_AT_EMPTY_PATH_ = 0x1000,
};

/** @brief The host path and directory of a guest *at path: 0 or -errno. */
static int64_t at_path(int64_t dirfd, uint64_t addr, bool follow, char *hp, int *dir)
{
    char path[PATH_CAP];
    if (lx_cstr(addr, path, sizeof path) < 0)
        return -L_EFAULT;
    int64_t rc = lx_dir_arg(dirfd, path, dir);
    if (rc == 0)
        rc = lx_host_path_at(path, hp, PATH_CAP, follow);
    return rc;
}

static int64_t sys_fchmod(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = lx_host_fd((int)a[0]);
    return h < 0 ? -L_EBADF : hio_fchmod(h, (uint32_t)a[1] & 07777);
}

/** @brief fchmodat2(2): fchmodat with AT_SYMLINK_NOFOLLOW (a symbolic link's mode cannot change: EOPNOTSUPP) and AT_EMPTY_PATH. */
static int64_t sys_fchmodat2(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint64_t flags = a[3];
    if (flags & ~(uint64_t)(L_AT_SYMLINK_NOFOLLOW_ | L_AT_EMPTY_PATH_))
        return -L_EINVAL;
    char p0;
    if ((flags & L_AT_EMPTY_PATH_) && lx_get(&p0, a[1], 1) && p0 == 0) {
        int h = lx_host_fd((int)a[0]);
        return h < 0 ? -L_EBADF : hio_fchmod(h, (uint32_t)a[2] & 07777);
    }
    char hp[PATH_CAP];
    int dir;
    bool follow = !(flags & L_AT_SYMLINK_NOFOLLOW_);
    int64_t rc = at_path((int64_t)(int32_t)a[0], a[1], follow, hp, &dir);
    if (rc < 0)
        return rc;
    if (!follow) {
        l_stat st;
        rc = hio_stat(dir, hp, true, &st);
        if (rc < 0)
            return rc;
        if ((st.st_mode & L_S_IFMT) == L_S_IFLNK)
            return -L_ENOTSUP;
    }
    return hio_chmod(dir, hp, (uint32_t)a[2] & 07777);
}

static int64_t sys_fchownat(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint64_t flags = a[4];
    if (flags & ~(uint64_t)(L_AT_SYMLINK_NOFOLLOW_ | L_AT_EMPTY_PATH_))
        return -L_EINVAL;
    int64_t uid = (int32_t)a[2], gid = (int32_t)a[3];
    char p0;
    if ((flags & L_AT_EMPTY_PATH_) && lx_get(&p0, a[1], 1) && p0 == 0) {
        int h = lx_host_fd((int)a[0]);
        return h < 0 ? -L_EBADF : hio_fchown(h, uid, gid);
    }
    char hp[PATH_CAP];
    int dir;
    bool nofollow = (flags & L_AT_SYMLINK_NOFOLLOW_) != 0;
    int64_t rc = at_path((int64_t)(int32_t)a[0], a[1], !nofollow, hp, &dir);
    return rc < 0 ? rc : hio_chown(dir, hp, uid, gid, nofollow);
}

static int64_t sys_fchown(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int h = lx_host_fd((int)a[0]);
    return h < 0 ? -L_EBADF : hio_fchown(h, (int32_t)a[1], (int32_t)a[2]);
}

/**
 * @brief The extended attribute calls: what is s, g, l or r; on is 'p'
 * (path, following links), 'l' (path, not following) or 'f' (descriptor).
 */
static int64_t xattr(char what, char on, const uint64_t a[6])
{
    char hp[PATH_CAP], name[NAME_CAP];
    const char *path = NULL;
    int fd = -1;
    if (on == 'f') {
        fd = lx_host_fd((int)a[0]);
        if (fd < 0)
            return -L_EBADF;
    } else {
        int dir;
        int64_t rc = at_path(-100, a[0], on == 'p', hp, &dir);
        if (rc < 0)
            return rc;
        path = hp;
    }
    if (what != 'l') {
        int64_t n = lx_cstr(a[1], name, sizeof name);
        if (n < 0)
            return n == -L_ENAMETOOLONG ? -L_ERANGE : n;
        if (n == 0)
            return -L_ERANGE;
    }
    uint64_t vaddr = what == 'l' ? a[1] : a[2];
    uint64_t size = what == 'l' ? a[2] : what == 'r' ? 0 : a[3];
    if (size > XATTR_MAX)
        size = XATTR_MAX;
    void *buf = size ? malloc(size) : NULL;
    if (size && !buf)
        return -L_ENOMEM;
    if (what == 's' && size && !lx_get(buf, vaddr, size)) {
        free(buf);
        return -L_EFAULT;
    }
    int64_t r = hio_xattr(what, fd, path, on == 'l', name, buf, size, what == 's' ? (int)a[4] : 0);
    if (r > 0 && (what == 'g' || what == 'l') && size && !lx_put(vaddr, buf, (uint64_t)r))
        r = -L_EFAULT;
    free(buf);
    return r;
}

#define XATTR_CALL(fn, what, on)                                       \
    static int64_t fn(rl_ctx *c, uint8_t *mem, const uint64_t a[6]) \
    {                                                                  \
        (void)c;                                                       \
        (void)mem;                                                     \
        return xattr(what, on, a);                                     \
    }

XATTR_CALL(sys_setxattr, 's', 'p')
XATTR_CALL(sys_lsetxattr, 's', 'l')
XATTR_CALL(sys_fsetxattr, 's', 'f')
XATTR_CALL(sys_getxattr, 'g', 'p')
XATTR_CALL(sys_lgetxattr, 'g', 'l')
XATTR_CALL(sys_fgetxattr, 'g', 'f')
XATTR_CALL(sys_listxattr, 'l', 'p')
XATTR_CALL(sys_llistxattr, 'l', 'l')
XATTR_CALL(sys_flistxattr, 'l', 'f')
XATTR_CALL(sys_removexattr, 'r', 'p')
XATTR_CALL(sys_lremovexattr, 'r', 'l')
XATTR_CALL(sys_fremovexattr, 'r', 'f')

/** @brief sendfile(2): copies through a buffer, from the offset at a[2] (updated) or the input's position, with the output's write rules. */
static int64_t sys_sendfile(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int in = lx_host_fd((int)a[1]);
    if (in < 0 || lx_host_fd((int)a[0]) < 0)
        return -L_EBADF;
    int64_t off = -1;
    if (a[2] && !lx_get(&off, a[2], 8))
        return -L_EFAULT;
    if (a[2] && off < 0)
        return -L_EINVAL;
    uint64_t want = a[3] > 0x7ffff000u ? 0x7ffff000u : a[3];
    enum { CHUNK = 1 << 16 };
    uint8_t *buf = malloc(CHUNK);
    if (!buf)
        return -L_ENOMEM;
    /* The data goes out with write(2)'s rules on the output, so pipes and sockets keep their blocking and signal behavior. */
    uint64_t done = 0;
    int64_t rc = 0;
    while (done < want) {
        size_t n = want - done < CHUNK ? (size_t)(want - done) : CHUNK;
        int64_t r = off >= 0 ? hio_pread(in, buf, n, (uint64_t)off) : hio_read(in, buf, n);
        if (r <= 0) {
            rc = r;
            break;
        }
        int64_t w = lx_fd_write((int)a[0], buf, (uint64_t)r);
        if (w <= 0) {
            rc = w;
            break;
        }
        if (off >= 0)
            off += w;
        done += (uint64_t)w;
        if (w < r) {
            if (off < 0)
                hio_lseek(in, w - r, 1);
            break;
        }
    }
    free(buf);
    if (a[2] && !lx_put(a[2], &off, 8))
        return -L_EFAULT;
    return done ? (int64_t)done : rc;
}

/** @brief pselect6(2) over poll: the three sets become poll events and back. The signal mask argument is not applied during the wait. */
static int64_t sys_pselect6(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int n = (int)a[0];
    if (n < 0 || n > LX_MAX_FDS)
        return -L_EINVAL;
    size_t words = ((size_t)n + 63) / 64;
    uint64_t in[3][LX_MAX_FDS / 64], out[3][LX_MAX_FDS / 64];
    memset(in, 0, sizeof in);
    memset(out, 0, sizeof out);
    for (int s = 0; s < 3; s++)
        if (a[1 + s] && words && !lx_get(in[s], a[1 + s], words * 8))
            return -L_EFAULT;
    int64_t timeout = -1;
    if (a[4]) {
        l_timespec ts;
        if (!lx_get(&ts, a[4], sizeof ts))
            return -L_EFAULT;
        if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000)
            return -L_EINVAL;
        timeout = ts.sec * 1000000000ll + ts.nsec;
    }
    static const uint32_t ev[3] = { L_POLLIN, L_POLLOUT, 0x2 /* POLLPRI */ };
    int64_t end = timeout < 0 ? 0 : (int64_t)rl_clock_ns() + timeout;
    int64_t ready;
    for (;;) {
        ready = 0;
        memset(out, 0, sizeof out);
        int only = -1, valid = 0;
        uint32_t only_ev = 0;
        for (int fd = 0; fd < n; fd++) {
            uint32_t want = 0;
            for (int s = 0; s < 3; s++)
                if (in[s][fd / 64] & (1ull << (fd % 64)))
                    want |= ev[s];
            if (!want)
                continue;
            int h = lx_host_fd(fd);
            if (h < 0)
                return -L_EBADF;
            int64_t r = hio_poll(h, want, 0);
            if (r < 0)
                return r;
            valid++;
            only = h;
            only_ev = want;
            for (int s = 0; s < 3; s++) {
                bool hit = s == 0 ? (r & (L_POLLIN | L_POLLHUP | L_POLLERR)) : s == 1 ? (r & (L_POLLOUT | L_POLLERR)) : (r & 0x2);
                if ((in[s][fd / 64] & (1ull << (fd % 64))) && hit) {
                    out[s][fd / 64] |= 1ull << (fd % 64);
                    ready++;
                }
            }
        }
        int64_t left = timeout < 0 ? -1 : end - (int64_t)rl_clock_ns();
        if (ready || timeout == 0 || (timeout > 0 && left <= 0))
            break;
        int64_t w = valid == 1 ? lx_wait(only, only_ev, left) : lx_wait(-1, 0, left < 0 || left > 1000000 ? 1000000 : left);
        if (w == -L_EINTR)
            return -L_ERESTARTNOHAND;
    }
    for (int s = 0; s < 3; s++)
        if (a[1 + s] && words && !lx_put(a[1 + s], out[s], words * 8))
            return -L_EFAULT;
    return ready;
}

lx_sys_fn *lx_fsx_call(uint32_t nr)
{
    switch (nr) {
    case NR_setxattr: return sys_setxattr;
    case NR_lsetxattr: return sys_lsetxattr;
    case NR_fsetxattr: return sys_fsetxattr;
    case NR_getxattr: return sys_getxattr;
    case NR_lgetxattr: return sys_lgetxattr;
    case NR_fgetxattr: return sys_fgetxattr;
    case NR_listxattr: return sys_listxattr;
    case NR_llistxattr: return sys_llistxattr;
    case NR_flistxattr: return sys_flistxattr;
    case NR_removexattr: return sys_removexattr;
    case NR_lremovexattr: return sys_lremovexattr;
    case NR_fremovexattr: return sys_fremovexattr;
    case NR_fchmod: return sys_fchmod;
    case NR_fchownat: return sys_fchownat;
    case NR_fchown: return sys_fchown;
    case NR_sendfile: return sys_sendfile;
    case NR_pselect6: return sys_pselect6;
    case NR_fchmodat2: return sys_fchmodat2;
    default: return NULL;
    }
}
