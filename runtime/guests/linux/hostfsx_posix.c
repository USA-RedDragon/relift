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
