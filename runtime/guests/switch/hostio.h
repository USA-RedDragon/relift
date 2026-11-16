/**
 * @file hostio.h
 * @brief The POSIX file calls of the Horizon file system layer that MinGW
 * does not provide, as small wrappers over the Win32 and CRT calls, so
 * fs.c and romfs_image.c read the same on every host. Include it after the
 * system headers.
 */
#ifndef RL_HOSTIO_H
#define RL_HOSTIO_H

#if defined(_WIN32)

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdint.h>
#include <sys/stat.h>
#include <windows.h>

#ifndef EDQUOT
#define EDQUOT (-1000)
#endif
#ifndef ESTALE
#define ESTALE (-1001)
#endif

#define off_t int64_t
#define stat _stat64
#define fstat _fstat64
#define lstat _stat64
#define O_CLOEXEC _O_BINARY
#define mkdir(path, mode) _mkdir(path)
#define fsync(fd) _commit(fd)

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

struct rl_statvfs {
    uint64_t f_blocks;
    uint64_t f_bavail;
    uint64_t f_frsize;
};
#define statvfs rl_statvfs

static inline int rl_statvfs_call(const char *path, struct rl_statvfs *sv)
{
    ULARGE_INTEGER avail, total, freeb;
    if (!GetDiskFreeSpaceExA(path, &avail, &total, &freeb))
        return -1;
    sv->f_frsize = 4096;
    sv->f_blocks = total.QuadPart / sv->f_frsize;
    sv->f_bavail = avail.QuadPart / sv->f_frsize;
    return 0;
}
#define rl_statvfs_fn(path, sv) rl_statvfs_call(path, sv)

static inline int64_t rl_io_at(int fd, void *buf, size_t n, int64_t at, int write)
{
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    OVERLAPPED o;
    DWORD got = 0;
    BOOL ok;
    o.Internal = o.InternalHigh = 0;
    o.Offset = (DWORD)at;
    o.OffsetHigh = (DWORD)((uint64_t)at >> 32);
    o.hEvent = NULL;
    if (n > 0x40000000u)
        n = 0x40000000u;
    ok = write ? WriteFile(h, buf, (DWORD)n, &got, &o) : ReadFile(h, buf, (DWORD)n, &got, &o);
    if (!ok) {
        if (!write && GetLastError() == ERROR_HANDLE_EOF)
            return 0;
        return -1;
    }
    return got;
}

#define pread(fd, buf, n, at) ((ssize_t)rl_io_at(fd, buf, n, at, 0))
#define pwrite(fd, buf, n, at) ((ssize_t)rl_io_at(fd, (void *)(buf), n, at, 1))
#define ftruncate(fd, size) _chsize_s(fd, size)

#else
#define rl_statvfs_fn(path, sv) statvfs(path, sv)
#endif

#endif
