/**
 * @file hostfsx_posix.c
 * @brief Ownership, fchmod and extended attributes on a POSIX host for the
 * Linux guest. macOS spells the attribute calls with an options argument;
 * other hosts without them report ENOTSUP.
 */
#include "hostio.h"

#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__) || defined(__APPLE__)
#include <sys/xattr.h>
#endif

int lx_errno(int e);

static int64_t fail(void) { return -(int64_t)lx_errno(errno); }

static int dirfd_of(int dir) { return dir == HIO_CWD ? AT_FDCWD : dir; }

int64_t hio_chown(int dir, const char *path, int64_t uid, int64_t gid, bool nofollow)
{
    return fchownat(dirfd_of(dir), path, (uid_t)uid, (gid_t)gid, nofollow ? AT_SYMLINK_NOFOLLOW : 0) < 0 ? fail() : 0;
}

int64_t hio_fchown(int fd, int64_t uid, int64_t gid) { return fchown(fd, (uid_t)uid, (gid_t)gid) < 0 ? fail() : 0; }

int64_t hio_fchmod(int fd, uint32_t mode) { return fchmod(fd, (mode_t)mode) < 0 ? fail() : 0; }

int64_t hio_lock(int fd, bool get, bool ofd, int16_t *type, int16_t whence, int64_t *start, int64_t *len, int32_t *pid)
{
    struct flock f;
    memset(&f, 0, sizeof f);
    f.l_type = (short)(*type == 0 ? F_RDLCK : *type == 1 ? F_WRLCK : F_UNLCK);
    f.l_whence = whence;
    f.l_start = (off_t)*start;
    f.l_len = (off_t)*len;
    int cmd = get ? F_GETLK : F_SETLK;
#if defined(F_OFD_SETLK)
    if (ofd)
        cmd = get ? F_OFD_GETLK : F_OFD_SETLK;
#else
    if (ofd)
        return -L_EINVAL;
#endif
    if (fcntl(fd, cmd, &f) < 0)
        return errno == EACCES ? -L_EAGAIN : fail();
    if (get) {
        *type = (int16_t)(f.l_type == F_RDLCK ? 0 : f.l_type == F_WRLCK ? 1 : 2);
        *start = (int64_t)f.l_start;
        *len = (int64_t)f.l_len;
        *pid = (int32_t)f.l_pid;
    }
    return 0;
}

int64_t hio_flock(int fd, int op)
{
    int o = (op & 1 ? LOCK_SH : 0) | (op & 2 ? LOCK_EX : 0) | (op & 8 ? LOCK_UN : 0) | LOCK_NB;
    return flock(fd, o) < 0 ? fail() : 0;
}

int64_t hio_memfd(const char *name, uint32_t flags)
{
#if defined(__linux__) && defined(MFD_CLOEXEC)
    int r = memfd_create(name, MFD_CLOEXEC | (flags & 2 ? MFD_ALLOW_SEALING : 0));
    return r < 0 ? fail() : r;
#else
    (void)name;
    (void)flags;
    char path[] = "/tmp/relift-memfd-XXXXXX";
    int r = mkstemp(path);
    if (r < 0)
        return fail();
    unlink(path);
    fcntl(r, F_SETFD, FD_CLOEXEC);
    return r;
#endif
}

int64_t hio_seals(int fd, bool add, uint32_t seals)
{
#if defined(__linux__) && defined(F_ADD_SEALS)
    int r = add ? fcntl(fd, F_ADD_SEALS, (int)seals) : fcntl(fd, F_GET_SEALS);
    return r < 0 ? fail() : r;
#else
    (void)fd;
    (void)add;
    (void)seals;
    return -L_EINVAL;
#endif
}

int64_t hio_xattr(char what, int fd, const char *path, bool nofollow, const char *name, void *value, size_t size, int flags)
{
#if defined(__linux__)
    ssize_t r;
    switch (what) {
    case 's':
        r = path ? (nofollow ? lsetxattr(path, name, value, size, flags) : setxattr(path, name, value, size, flags))
                 : fsetxattr(fd, name, value, size, flags);
        break;
    case 'g':
        r = path ? (nofollow ? lgetxattr(path, name, value, size) : getxattr(path, name, value, size)) : fgetxattr(fd, name, value, size);
        break;
    case 'l':
        r = path ? (nofollow ? llistxattr(path, value, size) : listxattr(path, value, size)) : flistxattr(fd, value, size);
        break;
    default:
        r = path ? (nofollow ? lremovexattr(path, name) : removexattr(path, name)) : fremovexattr(fd, name);
        break;
    }
    return r < 0 ? fail() : (int64_t)r;
#elif defined(__APPLE__)
    int o = nofollow ? XATTR_NOFOLLOW : 0;
    int so = o | (flags & 1 ? XATTR_CREATE : 0) | (flags & 2 ? XATTR_REPLACE : 0);
    ssize_t r;
    switch (what) {
    case 's':
        r = path ? setxattr(path, name, value, size, 0, so) : fsetxattr(fd, name, value, size, 0, so);
        break;
    case 'g':
        r = path ? getxattr(path, name, value, size, 0, o) : fgetxattr(fd, name, value, size, 0, o);
        break;
    case 'l':
        r = path ? listxattr(path, value, size, o) : flistxattr(fd, value, size, o);
        break;
    default:
        r = path ? removexattr(path, name, o) : fremovexattr(fd, name, o);
        break;
    }
    return r < 0 ? fail() : (int64_t)r;
#else
    (void)what;
    (void)fd;
    (void)path;
    (void)nofollow;
    (void)name;
    (void)value;
    (void)size;
    (void)flags;
    return -L_ENOTSUP;
#endif
}

#endif
