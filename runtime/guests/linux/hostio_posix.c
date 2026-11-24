/**
 * @file hostio_posix.c
 * @brief hostio.h over POSIX file calls: Linux, Android, macOS and iOS.
 */
#if !defined(_WIN32)

#include "hostio.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#if defined(__linux__)
#include <sys/sysmacros.h>
#endif
#include <signal.h>
#include <sys/resource.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

int lx_errno(int e);

static int64_t fail(void) { return -(int64_t)lx_errno(errno); }

static int dirfd_of(int dir) { return dir == HIO_CWD ? AT_FDCWD : dir; }

void hio_init(void) { signal(SIGPIPE, SIG_IGN); }

int64_t hio_open(int dir, const char *path, uint32_t lf, uint32_t mode)
{
    int f = 0;
    switch (lf & L_O_ACCMODE) {
    case L_O_RDONLY:
        f = O_RDONLY;
        break;
    case L_O_WRONLY:
        f = O_WRONLY;
        break;
    case L_O_RDWR:
        f = O_RDWR;
        break;
    default:
        return -L_EINVAL;
    }
    if ((lf & L_O_TMPFILE) == L_O_TMPFILE || (lf & L_O_PATH))
        return -L_EINVAL;
    if (lf & L_O_CREAT)
        f |= O_CREAT;
    if (lf & L_O_EXCL)
        f |= O_EXCL;
    if (lf & L_O_NOCTTY)
        f |= O_NOCTTY;
    if (lf & L_O_TRUNC)
        f |= O_TRUNC;
    if (lf & L_O_APPEND)
        f |= O_APPEND;
    if (lf & L_O_NONBLOCK)
        f |= O_NONBLOCK;
    if (lf & L_O_DIRECTORY)
        f |= O_DIRECTORY;
    if (lf & L_O_NOFOLLOW)
        f |= O_NOFOLLOW;
    if ((lf & L_O_SYNC) == L_O_SYNC)
        f |= O_SYNC;
#ifdef O_DSYNC
    else if (lf & L_O_DSYNC)
        f |= O_DSYNC;
#endif
    int fd = openat(dirfd_of(dir), path, f | O_CLOEXEC, (mode_t)mode);
    return fd < 0 ? fail() : fd;
}

int64_t hio_close(int fd) { return close(fd) < 0 ? fail() : 0; }

int64_t hio_read(int fd, void *buf, size_t n)
{
    ssize_t r = read(fd, buf, n);
    return r < 0 ? fail() : r;
}

int64_t hio_write(int fd, const void *buf, size_t n)
{
    ssize_t r = write(fd, buf, n);
    return r < 0 ? fail() : r;
}

int64_t hio_pread(int fd, void *buf, size_t n, uint64_t off)
{
    ssize_t r = pread(fd, buf, n, (off_t)off);
    return r < 0 ? fail() : r;
}

int64_t hio_pwrite(int fd, const void *buf, size_t n, uint64_t off)
{
    ssize_t r = pwrite(fd, buf, n, (off_t)off);
    return r < 0 ? fail() : r;
}

int64_t hio_lseek(int fd, int64_t off, int whence)
{
    int w;
    switch (whence) {
    case L_SEEK_SET:
        w = SEEK_SET;
        break;
    case L_SEEK_CUR:
        w = SEEK_CUR;
        break;
    case L_SEEK_END:
        w = SEEK_END;
        break;
    default:
        return -L_EINVAL;
    }
    off_t r = lseek(fd, (off_t)off, w);
    return r < 0 ? fail() : (int64_t)r;
}

#if defined(__APPLE__)
#define ST_TIME(st, x) ((st)->st_##x##timespec)
#else
#define ST_TIME(st, x) ((st)->st_##x##tim)
#endif

static uint64_t linux_dev(dev_t d)
{
    uint64_t major = (uint64_t)major(d), minor = (uint64_t)minor(d);
    return (minor & 0xff) | ((major & 0xfff) << 8) | ((minor & ~0xffull) << 12) | ((major & ~0xfffull) << 32);
}

static void convert(const struct stat *s, l_stat *o)
{
    memset(o, 0, sizeof *o);
    o->st_dev = linux_dev(s->st_dev);
    o->st_ino = (uint64_t)s->st_ino;
    o->st_mode = (uint32_t)s->st_mode;
    o->st_nlink = (uint32_t)s->st_nlink;
    o->st_uid = (uint32_t)s->st_uid;
    o->st_gid = (uint32_t)s->st_gid;
    o->st_rdev = linux_dev(s->st_rdev);
    o->st_size = (int64_t)s->st_size;
    o->st_blksize = (int32_t)s->st_blksize;
    o->st_blocks = (int64_t)s->st_blocks;
    o->st_atime_sec = (int64_t)ST_TIME(s, a).tv_sec;
    o->st_atime_ns = (uint64_t)ST_TIME(s, a).tv_nsec;
    o->st_mtime_sec = (int64_t)ST_TIME(s, m).tv_sec;
    o->st_mtime_ns = (uint64_t)ST_TIME(s, m).tv_nsec;
    o->st_ctime_sec = (int64_t)ST_TIME(s, c).tv_sec;
    o->st_ctime_ns = (uint64_t)ST_TIME(s, c).tv_nsec;
}

int64_t hio_fstat(int fd, l_stat *st)
{
    struct stat s;
    if (fstat(fd, &s) < 0)
        return fail();
    convert(&s, st);
    return 0;
}

int64_t hio_stat(int dir, const char *path, bool nofollow, l_stat *st)
{
    struct stat s;
    if (fstatat(dirfd_of(dir), path, &s, nofollow ? AT_SYMLINK_NOFOLLOW : 0) < 0)
        return fail();
    convert(&s, st);
    return 0;
}

int64_t hio_ftruncate(int fd, uint64_t len) { return ftruncate(fd, (off_t)len) < 0 ? fail() : 0; }

int64_t hio_fsync(int fd) { return fsync(fd) < 0 ? fail() : 0; }

int64_t hio_dup(int fd, int min)
{
    int r = fcntl(fd, F_DUPFD_CLOEXEC, min);
    return r < 0 ? fail() : r;
}

int64_t hio_dup2(int fd, int target)
{
    int r = dup2(fd, target);
    if (r < 0)
        return fail();
    fcntl(r, F_SETFD, FD_CLOEXEC);
    return r;
}

int64_t hio_pipe(int fds[2])
{
    if (pipe(fds) < 0)
        return fail();
    for (int i = 0; i < 2; i++)
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    return 0;
}

int64_t hio_mkdir(int dir, const char *path, uint32_t mode) { return mkdirat(dirfd_of(dir), path, (mode_t)mode) < 0 ? fail() : 0; }

int64_t hio_symlink(const char *target, int dir, const char *path) { return symlinkat(target, dirfd_of(dir), path) < 0 ? fail() : 0; }

int64_t hio_link(int od, const char *op, int nd, const char *np, bool follow)
{
    return linkat(dirfd_of(od), op, dirfd_of(nd), np, follow ? AT_SYMLINK_FOLLOW : 0) < 0 ? fail() : 0;
}

int64_t hio_mkfifo(int dir, const char *path, uint32_t mode)
{
#if defined(__APPLE__)
    /* mkfifoat is macOS 13 and iOS 16 and later; the runtime builds for older ones. */
    if (__builtin_available(macOS 13.0, iOS 16.0, *))
        return mkfifoat(dirfd_of(dir), path, (mode_t)mode) < 0 ? fail() : 0;
    errno = ENOSYS;
    return fail();
#else
    return mkfifoat(dirfd_of(dir), path, (mode_t)mode) < 0 ? fail() : 0;
#endif
}

int64_t hio_statfs(int fd, const char *path, l_statfs *out)
{
    struct statvfs v;
    if ((path ? statvfs(path, &v) : fstatvfs(fd, &v)) < 0)
        return fail();
    memset(out, 0, sizeof *out);
    out->f_type = 0xef53;
    out->f_bsize = (int64_t)v.f_bsize;
    out->f_frsize = (int64_t)(v.f_frsize ? v.f_frsize : v.f_bsize);
    out->f_blocks = v.f_blocks;
    out->f_bfree = v.f_bfree;
    out->f_bavail = v.f_bavail;
    out->f_files = v.f_files;
    out->f_ffree = v.f_ffree;
    out->f_namelen = (int64_t)v.f_namemax;
    out->f_flags = (v.f_flag & ST_RDONLY) ? 1 : 0;
    return 0;
}

int64_t hio_unlink(int dir, const char *path, bool remove_dir)
{
    return unlinkat(dirfd_of(dir), path, remove_dir ? AT_REMOVEDIR : 0) < 0 ? fail() : 0;
}

int64_t hio_rename(int od, const char *op, int nd, const char *np)
{
    return renameat(dirfd_of(od), op, dirfd_of(nd), np) < 0 ? fail() : 0;
}

int64_t hio_access(int dir, const char *path, uint32_t mode) { return faccessat(dirfd_of(dir), path, (int)mode, 0) < 0 ? fail() : 0; }

int64_t hio_readlink(int dir, const char *path, char *buf, size_t n)
{
    ssize_t r = readlinkat(dirfd_of(dir), path, buf, n);
    return r < 0 ? fail() : r;
}

int64_t hio_getcwd(char *buf, size_t n)
{
    if (!getcwd(buf, n))
        return fail();
    return (int64_t)strlen(buf) + 1;
}

int64_t hio_chdir(const char *path) { return chdir(path) < 0 ? fail() : 0; }

int64_t hio_chmod(int dir, const char *path, uint32_t mode) { return fchmodat(dirfd_of(dir), path, (mode_t)mode, 0) < 0 ? fail() : 0; }

int64_t hio_utimens(int dir, const char *path, const int64_t t[4], bool nofollow)
{
    struct timespec ts[2];
    for (int i = 0; i < 2; i++) {
        ts[i].tv_sec = (time_t)t[2 * i];
        if (t[2 * i + 1] == 0x3fffffff)
            ts[i].tv_nsec = UTIME_NOW;
        else if (t[2 * i + 1] == 0x3ffffffe)
            ts[i].tv_nsec = UTIME_OMIT;
        else
            ts[i].tv_nsec = (long)t[2 * i + 1];
    }
    return utimensat(dirfd_of(dir), path, ts, nofollow ? AT_SYMLINK_NOFOLLOW : 0) < 0 ? fail() : 0;
}

bool hio_isatty(int fd) { return isatty(fd) == 1; }

int64_t hio_winsize(int fd, uint16_t ws[4])
{
    struct winsize w;
    if (ioctl(fd, TIOCGWINSZ, &w) < 0)
        return fail();
    ws[0] = w.ws_row;
    ws[1] = w.ws_col;
    ws[2] = w.ws_xpixel;
    ws[3] = w.ws_ypixel;
    return 0;
}

int64_t hio_nread(int fd, int32_t *n)
{
    int v = 0;
    if (ioctl(fd, FIONREAD, &v) < 0)
        return fail();
    *n = v;
    return 0;
}

int64_t hio_set_nonblock(int fd, bool on)
{
    int f = fcntl(fd, F_GETFL);
    if (f < 0)
        return fail();
    f = on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK);
    return fcntl(fd, F_SETFL, f) < 0 ? fail() : 0;
}

int64_t hio_get_status_flags(int fd)
{
    int f = fcntl(fd, F_GETFL);
    if (f < 0)
        return fail();
    int64_t r = 0;
    switch (f & O_ACCMODE) {
    case O_WRONLY:
        r |= L_O_WRONLY;
        break;
    case O_RDWR:
        r |= L_O_RDWR;
        break;
    default:
        break;
    }
    if (f & O_APPEND)
        r |= L_O_APPEND;
    if (f & O_NONBLOCK)
        r |= L_O_NONBLOCK;
    return r;
}

int64_t hio_set_status_flags(int fd, uint32_t lf)
{
    int f = fcntl(fd, F_GETFL);
    if (f < 0)
        return fail();
    f &= ~(O_APPEND | O_NONBLOCK);
    if (lf & L_O_APPEND)
        f |= O_APPEND;
    if (lf & L_O_NONBLOCK)
        f |= O_NONBLOCK;
    return fcntl(fd, F_SETFL, f) < 0 ? fail() : 0;
}

int64_t hio_poll(int fd, uint32_t events, int64_t timeout_ns)
{
    struct pollfd p = { .fd = fd, .events = 0, .revents = 0 };
    if (events & L_POLLIN)
        p.events |= POLLIN;
    if (events & L_POLLOUT)
        p.events |= POLLOUT;
    int ms = timeout_ns < 0 ? -1 : (int)((timeout_ns + 999999) / 1000000);
    int r = poll(&p, 1, ms);
    if (r < 0)
        return fail();
    uint32_t out = 0;
    if (p.revents & POLLIN)
        out |= L_POLLIN;
    if (p.revents & POLLOUT)
        out |= L_POLLOUT;
    if (p.revents & POLLERR)
        out |= L_POLLERR;
    if (p.revents & POLLHUP)
        out |= L_POLLHUP;
    if (p.revents & POLLNVAL)
        out |= L_POLLNVAL;
    return out;
}

void *hio_waker_new(void)
{
    int *p = malloc(2 * sizeof *p);
    if (!p)
        return NULL;
    if (pipe(p) < 0) {
        free(p);
        return NULL;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(p[i], F_SETFD, FD_CLOEXEC);
        fcntl(p[i], F_SETFL, fcntl(p[i], F_GETFL) | O_NONBLOCK);
    }
    return p;
}

void hio_waker_free(void *w)
{
    int *p = w;
    if (!p)
        return;
    close(p[0]);
    close(p[1]);
    free(p);
}

void hio_waker_kick(void *w)
{
    char b = 1;
    ssize_t r = write(((int *)w)[1], &b, 1);
    (void)r;
}

void hio_waker_clear(void *w)
{
    char buf[64];
    while (read(((int *)w)[0], buf, sizeof buf) > 0) {
    }
}

int64_t hio_wait(int fd, uint32_t events, int64_t timeout_ns, void *waker)
{
    struct pollfd p[2] = { { .fd = fd, .events = 0 }, { .fd = waker ? ((int *)waker)[0] : -1, .events = POLLIN } };
    if (events & L_POLLIN)
        p[0].events |= POLLIN;
    if (events & L_POLLOUT)
        p[0].events |= POLLOUT;
    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int ms = -1;
        if (timeout_ns >= 0) {
            struct timespec t1;
            clock_gettime(CLOCK_MONOTONIC, &t1);
            int64_t used = ((int64_t)t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec);
            int64_t left = timeout_ns - used;
            if (left <= 0)
                return 0;
            ms = (int)((left + 999999) / 1000000);
        }
        int r = poll(p, 2, ms);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return fail();
        }
        uint32_t out = 0;
        if (p[0].revents & POLLIN)
            out |= L_POLLIN;
        if (p[0].revents & POLLOUT)
            out |= L_POLLOUT;
        if (p[0].revents & POLLERR)
            out |= L_POLLERR;
        if (p[0].revents & POLLHUP)
            out |= L_POLLHUP;
        if (p[0].revents & POLLNVAL)
            out |= L_POLLNVAL;
        if (out)
            return out;
        if (p[1].revents & POLLIN)
            return -L_EINTR;
        if (r == 0 && timeout_ns >= 0)
            continue;
    }
}

void *hio_dir_open(int fd) { return fdopendir(fd); }

void hio_dir_close(void *dir) { closedir(dir); }

int64_t hio_dir_next(void *dir, char name[256], uint64_t *ino, uint8_t *type)
{
    errno = 0;
    struct dirent *e = readdir(dir);
    if (!e)
        return errno ? fail() : 0;
    size_t n = strlen(e->d_name);
    if (n > 255)
        n = 255;
    memcpy(name, e->d_name, n);
    name[n] = 0;
    *ino = (uint64_t)e->d_ino;
    switch (e->d_type) {
    case DT_REG:
        *type = L_DT_REG;
        break;
    case DT_DIR:
        *type = L_DT_DIR;
        break;
    case DT_LNK:
        *type = L_DT_LNK;
        break;
    case DT_CHR:
        *type = L_DT_CHR;
        break;
    case DT_BLK:
        *type = L_DT_BLK;
        break;
    case DT_FIFO:
        *type = L_DT_FIFO;
        break;
    case DT_SOCK:
        *type = L_DT_SOCK;
        break;
    default:
        *type = L_DT_UNKNOWN;
        break;
    }
    return 1;
}

void hio_dir_rewind(void *dir) { rewinddir(dir); }

int64_t hio_realtime_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

int64_t hio_cputime_ns(void)
{
    struct timespec t;
    if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &t) != 0)
        return 0;
    return (int64_t)t.tv_sec * 1000000000ll + t.tv_nsec;
}

int32_t hio_pid(void) { return (int32_t)getpid(); }

int32_t hio_ppid(void) { return (int32_t)getppid(); }

uint32_t hio_uid(void) { return (uint32_t)getuid(); }

uint32_t hio_gid(void) { return (uint32_t)getgid(); }

void hio_nodename(char *buf, size_t n)
{
    if (n == 0)
        return;
    if (gethostname(buf, n) != 0)
        strncpy(buf, "localhost", n);
    buf[n - 1] = 0;
}

uint32_t hio_cpu_count(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (uint32_t)n : 1;
}

uint32_t hio_umask(uint32_t mask) { return (uint32_t)umask((mode_t)mask); }

#endif

#if defined(__linux__)
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>

int64_t hio_epoll_create(void)
{
    int fd = epoll_create1(EPOLL_CLOEXEC);
    return fd < 0 ? fail() : fd;
}

int64_t hio_epoll_ctl(int ep, int op, int fd, uint32_t events, uint64_t data)
{
    struct epoll_event ev = { .events = events, .data.u64 = data };
    return epoll_ctl(ep, op, fd, &ev) < 0 ? fail() : 0;
}

int64_t hio_epoll_wait(int ep, hio_epoll_event *out, int max, int64_t timeout_ns, void *waker)
{
    int64_t w = hio_wait(ep, L_POLLIN, timeout_ns, waker);
    if (w <= 0)
        return w;
    struct epoll_event ev[64];
    if (max > 64)
        max = 64;
    int n;
    do
        n = epoll_wait(ep, ev, max, 0);
    while (n < 0 && errno == EINTR);
    if (n < 0)
        return fail();
    for (int i = 0; i < n; i++) {
        out[i].events = ev[i].events;
        out[i].data = ev[i].data.u64;
    }
    return n;
}

int64_t hio_eventfd(uint32_t init, uint32_t flags)
{
    int fd = eventfd(init, (int)flags);
    return fd < 0 ? fail() : fd;
}

int64_t hio_timerfd_create(int clock, uint32_t flags)
{
    int fd = timerfd_create(clock, (int)flags);
    return fd < 0 ? fail() : fd;
}

int64_t hio_timerfd_settime(int fd, uint32_t flags, const int64_t nv[4], int64_t old[4])
{
    struct itimerspec n = { .it_interval = { nv[0], nv[1] }, .it_value = { nv[2], nv[3] } }, o;
    if (timerfd_settime(fd, (int)flags, &n, &o) < 0)
        return fail();
    if (old) {
        old[0] = o.it_interval.tv_sec;
        old[1] = o.it_interval.tv_nsec;
        old[2] = o.it_value.tv_sec;
        old[3] = o.it_value.tv_nsec;
    }
    return 0;
}

int64_t hio_timerfd_gettime(int fd, int64_t v[4])
{
    struct itimerspec o;
    if (timerfd_gettime(fd, &o) < 0)
        return fail();
    v[0] = o.it_interval.tv_sec;
    v[1] = o.it_interval.tv_nsec;
    v[2] = o.it_value.tv_sec;
    v[3] = o.it_value.tv_nsec;
    return 0;
}
#endif

#if defined(__APPLE__)
#include <sys/event.h>

enum { EP_IN = 0x1, EP_OUT = 0x4, EP_ERR = 0x8, EP_HUP = 0x10, EP_RDHUP = 0x2000, EP_ONESHOT = 1u << 30, EP_ET = 1u << 31 };

int64_t hio_epoll_create(void)
{
    int kq = kqueue();
    if (kq < 0)
        return fail();
    fcntl(kq, F_SETFD, FD_CLOEXEC);
    return kq;
}

static int kev(int kq, int fd, int filter, uint16_t flags, void *udata)
{
    struct kevent k;
    EV_SET(&k, (uintptr_t)fd, filter, flags, 0, 0, udata);
    return kevent(kq, &k, 1, NULL, 0, NULL);
}

int64_t hio_epoll_ctl(int ep, int op, int fd, uint32_t events, uint64_t data)
{
    if (op == 2) {
        int a = kev(ep, fd, EVFILT_READ, EV_DELETE, NULL), b = kev(ep, fd, EVFILT_WRITE, EV_DELETE, NULL);
        return a < 0 && b < 0 ? -L_ENOENT : 0;
    }
    if (op != 1 && op != 3)
        return -L_EINVAL;
    uint16_t fl = EV_ADD | EV_ENABLE;
    if (events & EP_ET)
        fl |= EV_CLEAR;
    if (events & EP_ONESHOT)
        fl |= EV_ONESHOT;
    void *u = (void *)(uintptr_t)data;
    if (events & (EP_IN | EP_RDHUP)) {
        if (kev(ep, fd, EVFILT_READ, fl, u) < 0)
            return fail();
    } else {
        kev(ep, fd, EVFILT_READ, EV_DELETE, NULL);
    }
    if (events & EP_OUT) {
        if (kev(ep, fd, EVFILT_WRITE, fl, u) < 0)
            return fail();
    } else {
        kev(ep, fd, EVFILT_WRITE, EV_DELETE, NULL);
    }
    return 0;
}

int64_t hio_epoll_wait(int ep, hio_epoll_event *out, int max, int64_t timeout_ns, void *waker)
{
    int wfd = waker ? ((int *)waker)[0] : -1;
    static char kick_tag;
    if (wfd >= 0)
        kev(ep, wfd, EVFILT_READ, EV_ADD | EV_ENABLE, &kick_tag);
    struct kevent ev[64];
    if (max > 64)
        max = 64;
    struct timespec ts, *tp = NULL;
    if (timeout_ns >= 0) {
        ts.tv_sec = timeout_ns / 1000000000;
        ts.tv_nsec = timeout_ns % 1000000000;
        tp = &ts;
    }
    int n = kevent(ep, NULL, 0, ev, max, tp);
    int err = errno;
    if (wfd >= 0)
        kev(ep, wfd, EVFILT_READ, EV_DELETE, NULL);
    if (n < 0)
        return err == EINTR ? 0 : (errno = err, fail());
    int got = 0;
    for (int i = 0; i < n; i++) {
        if (ev[i].udata == &kick_tag) {
            if (!got)
                return -L_EINTR;
            continue;
        }
        uint32_t e = 0;
        if (ev[i].filter == EVFILT_READ)
            e |= EP_IN;
        if (ev[i].filter == EVFILT_WRITE)
            e |= EP_OUT;
        if (ev[i].flags & EV_EOF)
            e |= EP_HUP;
        if (ev[i].flags & EV_ERROR)
            e |= EP_ERR;
        out[got].events = e;
        out[got].data = (uint64_t)(uintptr_t)ev[i].udata;
        got++;
    }
    return got;
}

int64_t hio_eventfd(uint32_t init, uint32_t flags)
{
    (void)init;
    (void)flags;
    return -L_ENOSYS;
}

int64_t hio_timerfd_create(int clock, uint32_t flags)
{
    (void)clock;
    (void)flags;
    return -L_ENOSYS;
}

int64_t hio_timerfd_settime(int fd, uint32_t flags, const int64_t nv[4], int64_t old[4])
{
    (void)fd;
    (void)flags;
    (void)nv;
    (void)old;
    return -L_ENOSYS;
}

int64_t hio_timerfd_gettime(int fd, int64_t v[4])
{
    (void)fd;
    (void)v;
    return -L_ENOSYS;
}
#endif
