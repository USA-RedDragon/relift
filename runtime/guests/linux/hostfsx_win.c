/**
 * @file hostfsx_win.c
 * @brief Ownership and extended attributes on a Windows host: the host has
 * neither in the POSIX sense, so ownership changes succeed without effect
 * when they keep the ids and the attribute calls report ENOTSUP.
 */
#include "hostio.h"

#if defined(_WIN32)

#include <io.h>
#include <sys/stat.h>

int64_t hio_chown(int dir, const char *path, int64_t uid, int64_t gid, bool nofollow)
{
    (void)dir;
    (void)path;
    (void)uid;
    (void)gid;
    (void)nofollow;
    return 0;
}

int64_t hio_fchown(int fd, int64_t uid, int64_t gid)
{
    (void)fd;
    (void)uid;
    (void)gid;
    return 0;
}

int64_t hio_fchmod(int fd, uint32_t mode)
{
    (void)fd;
    (void)mode;
    return 0;
}

int64_t hio_xattr(char what, int fd, const char *path, bool nofollow, const char *name, void *value, size_t size, int flags)
{
    (void)what;
    (void)fd;
    (void)path;
    (void)nofollow;
    (void)name;
    (void)value;
    (void)size;
    (void)flags;
    return -L_ENOTSUP;
}

#endif
