/**
 * @file hostio_win.c
 * @brief hostio.h over Win32 and the C runtime's descriptors: Windows.
 *
 * A host descriptor is a CRT file descriptor over a handle opened with
 * CreateFileW, so a directory opens like a file. Guest paths are UTF-8 with
 * `/` separators, which Windows takes as they are, except that a path that
 * starts `/c/` or `/c` (one letter, then a separator or the end) names drive
 * C: and `getcwd` answers in that form, so the paths a Linux program sees
 * are absolute. There are no directory
 * descriptors to open relative to: a path relative to one is joined to the
 * directory's own path. Symbolic links, `O_NONBLOCK` and ownership have no
 * Windows form here: a link is followed, the descriptor stays blocking, and
 * every file belongs to user and group 1000.
 */
#if defined(_WIN32)

#include "hostio.h"

#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <windows.h>

#define PATH_MAX_W 32768

static int g_umask = 022;

static int64_t map_error(DWORD e)
{
    switch (e) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        return -L_ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return -L_EACCES;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return -L_EEXIST;
    case ERROR_DIR_NOT_EMPTY:
        return -L_ENOTEMPTY;
    case ERROR_DIRECTORY:
        return -L_ENOTDIR;
    case ERROR_INVALID_HANDLE:
        return -L_EBADF;
    case ERROR_BROKEN_PIPE:
    case ERROR_NO_DATA:
    case ERROR_PIPE_NOT_CONNECTED:
        return -L_EPIPE;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return -L_ENOMEM;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return -L_ENOSPC;
    case ERROR_WRITE_PROTECT:
        return -L_EROFS;
    case ERROR_INVALID_PARAMETER:
    case ERROR_INVALID_NAME:
    case ERROR_NEGATIVE_SEEK:
        return -L_EINVAL;
    case ERROR_FILENAME_EXCED_RANGE:
        return -L_ENAMETOOLONG;
    case ERROR_NOT_SAME_DEVICE:
        return -L_EXDEV;
    case ERROR_TOO_MANY_OPEN_FILES:
        return -L_EMFILE;
    case ERROR_NOT_SUPPORTED:
        return -L_ENOTSUP;
    default:
        return -L_EIO;
    }
}

static int64_t last_error(void) { return map_error(GetLastError()); }

static HANDLE handle_of(int fd) { return (HANDLE)_get_osfhandle(fd); }

static bool is_letter(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static bool widen(const char *s, wchar_t *out, size_t cap)
{
    if (s[0] == '/' && is_letter(s[1]) && (s[2] == '/' || s[2] == 0)) {
        if (cap < 4)
            return false;
        out[0] = (wchar_t)s[1];
        out[1] = L':';
        out[2] = L'/';
        s += s[2] ? 3 : 2;
        out += 3;
        cap -= 3;
    }
    return MultiByteToWideChar(CP_UTF8, 0, s, -1, out, (int)cap) > 0;
}

static bool narrow(const wchar_t *s, char *out, size_t cap)
{
    int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, out, (int)cap, NULL, NULL);
    return n > 0;
}

static bool is_absolute(const char *p) { return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':'); }

/** @brief The wide path of `path` relative to directory descriptor `dir`. */
static int64_t wpath(int dir, const char *path, wchar_t *out)
{
    if (dir == HIO_CWD || is_absolute(path))
        return widen(path, out, PATH_MAX_W) ? 0 : -L_ENAMETOOLONG;
    wchar_t base[PATH_MAX_W];
    DWORD n = GetFinalPathNameByHandleW(handle_of(dir), base, PATH_MAX_W, FILE_NAME_NORMALIZED);
    if (n == 0 || n >= PATH_MAX_W)
        return -L_EBADF;
    size_t skip = wcsncmp(base, L"\\\\?\\", 4) == 0 ? 4 : 0;
    wchar_t rel[PATH_MAX_W];
    if (!widen(path, rel, PATH_MAX_W))
        return -L_ENAMETOOLONG;
    if (wcslen(base + skip) + wcslen(rel) + 2 >= PATH_MAX_W)
        return -L_ENAMETOOLONG;
    wcscpy(out, base + skip);
    wcscat(out, L"\\");
    wcscat(out, rel);
    return 0;
}

void hio_init(void) {}

int64_t hio_open(int dir, const char *path, uint32_t lf, uint32_t mode)
{
    (void)mode;
    wchar_t w[PATH_MAX_W];
    int64_t rc = wpath(dir, path, w);
    if (rc < 0)
        return rc;
    if ((lf & L_O_TMPFILE) == L_O_TMPFILE || (lf & L_O_PATH))
        return -L_EINVAL;
    DWORD access = 0;
    switch (lf & L_O_ACCMODE) {
    case L_O_RDONLY:
        access = GENERIC_READ;
        break;
    case L_O_WRONLY:
        access = GENERIC_WRITE;
        break;
    case L_O_RDWR:
        access = GENERIC_READ | GENERIC_WRITE;
        break;
    default:
        return -L_EINVAL;
    }
    DWORD disp;
    if (lf & L_O_CREAT)
        disp = (lf & L_O_EXCL) ? CREATE_NEW : (lf & L_O_TRUNC) ? CREATE_ALWAYS : OPEN_ALWAYS;
    else
        disp = (lf & L_O_TRUNC) ? TRUNCATE_EXISTING : OPEN_EXISTING;
    DWORD attrs = FILE_ATTRIBUTE_NORMAL | FILE_FLAG_BACKUP_SEMANTICS;
    if (lf & L_O_NOFOLLOW)
        attrs |= FILE_FLAG_OPEN_REPARSE_POINT;
    HANDLE h = CreateFileW(w, access, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, disp, attrs, NULL);
    if (h == INVALID_HANDLE_VALUE)
        return last_error();
    if (lf & L_O_DIRECTORY) {
        BY_HANDLE_FILE_INFORMATION info;
        if (!GetFileInformationByHandle(h, &info) || !(info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
            CloseHandle(h);
            return -L_ENOTDIR;
        }
    }
    int fd = _open_osfhandle((intptr_t)h, _O_BINARY | ((lf & L_O_APPEND) ? _O_APPEND : 0));
    if (fd < 0) {
        CloseHandle(h);
        return -L_EMFILE;
    }
    return fd;
}

int64_t hio_close(int fd) { return _close(fd) < 0 ? -L_EBADF : 0; }

int64_t hio_read(int fd, void *buf, size_t n)
{
    HANDLE h = handle_of(fd);
    DWORD got = 0;
    if (!ReadFile(h, buf, (DWORD)(n > 0x7fffffffu ? 0x7fffffffu : n), &got, NULL)) {
        DWORD e = GetLastError();
        if (e == ERROR_BROKEN_PIPE || e == ERROR_HANDLE_EOF)
            return 0;
        return map_error(e);
    }
    return got;
}

int64_t hio_write(int fd, const void *buf, size_t n)
{
    int r = _write(fd, buf, (unsigned)(n > 0x7fffffffu ? 0x7fffffffu : n));
    return r < 0 ? last_error() : r;
}

static int64_t at_offset(int fd, uint64_t off, void *rbuf, const void *wbuf, size_t n)
{
    HANDLE h = handle_of(fd);
    OVERLAPPED o;
    memset(&o, 0, sizeof o);
    o.Offset = (DWORD)off;
    o.OffsetHigh = (DWORD)(off >> 32);
    DWORD done = 0;
    DWORD len = (DWORD)(n > 0x7fffffffu ? 0x7fffffffu : n);
    BOOL ok = rbuf ? ReadFile(h, rbuf, len, &done, &o) : WriteFile(h, wbuf, len, &done, &o);
    if (!ok) {
        DWORD e = GetLastError();
        if (e == ERROR_HANDLE_EOF)
            return 0;
        return map_error(e);
    }
    return done;
}

int64_t hio_pread(int fd, void *buf, size_t n, uint64_t off) { return at_offset(fd, off, buf, NULL, n); }

int64_t hio_pwrite(int fd, const void *buf, size_t n, uint64_t off) { return at_offset(fd, off, NULL, buf, n); }

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
    __int64 r = _lseeki64(fd, off, w);
    if (r < 0)
        return GetFileType(handle_of(fd)) == FILE_TYPE_DISK ? -L_EINVAL : -L_ESPIPE;
    return r;
}

static int64_t unix_time_s(FILETIME ft, uint64_t *nsec)
{
    uint64_t t = ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
    uint64_t since_epoch = t >= 116444736000000000ull ? t - 116444736000000000ull : 0;
    *nsec = (since_epoch % 10000000ull) * 100;
    return (int64_t)(since_epoch / 10000000ull);
}

static int64_t stat_handle(HANDLE h, l_stat *st)
{
    memset(st, 0, sizeof *st);
    DWORD type = GetFileType(h);
    st->st_uid = st->st_gid = 1000;
    st->st_blksize = 4096;
    if (type == FILE_TYPE_CHAR) {
        st->st_mode = L_S_IFCHR | 0620;
        st->st_nlink = 1;
        return 0;
    }
    if (type == FILE_TYPE_PIPE) {
        st->st_mode = L_S_IFIFO | 0600;
        st->st_nlink = 1;
        return 0;
    }
    BY_HANDLE_FILE_INFORMATION i;
    if (!GetFileInformationByHandle(h, &i))
        return last_error();
    uint64_t size = ((uint64_t)i.nFileSizeHigh << 32) | i.nFileSizeLow;
    bool dir = (i.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool ro = (i.dwFileAttributes & FILE_ATTRIBUTE_READONLY) != 0;
    st->st_dev = i.dwVolumeSerialNumber;
    st->st_ino = ((uint64_t)i.nFileIndexHigh << 32) | i.nFileIndexLow;
    st->st_mode = dir ? (L_S_IFDIR | (ro ? 0555 : 0755)) : (L_S_IFREG | (ro ? 0444 : 0644));
    st->st_nlink = i.nNumberOfLinks;
    st->st_size = dir ? 0 : (int64_t)size;
    st->st_blocks = (int64_t)((size + 511) / 512);
    st->st_atime_sec = unix_time_s(i.ftLastAccessTime, &st->st_atime_ns);
    st->st_mtime_sec = unix_time_s(i.ftLastWriteTime, &st->st_mtime_ns);
    st->st_ctime_sec = unix_time_s(i.ftCreationTime, &st->st_ctime_ns);
    return 0;
}

int64_t hio_fstat(int fd, l_stat *st) { return stat_handle(handle_of(fd), st); }

int64_t hio_stat(int dir, const char *path, bool nofollow, l_stat *st)
{
    wchar_t w[PATH_MAX_W];
    int64_t rc = wpath(dir, path, w);
    if (rc < 0)
        return rc;
    HANDLE h = CreateFileW(w, FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS | (nofollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0), NULL);
    if (h == INVALID_HANDLE_VALUE)
        return last_error();
    rc = stat_handle(h, st);
    CloseHandle(h);
    return rc;
}

int64_t hio_ftruncate(int fd, uint64_t len) { return _chsize_s(fd, (__int64)len) != 0 ? -L_EINVAL : 0; }

int64_t hio_fsync(int fd) { return FlushFileBuffers(handle_of(fd)) ? 0 : last_error(); }

int64_t hio_dup(int fd, int min)
{
    (void)min;
    int r = _dup(fd);
    if (r < 0)
        return -L_EMFILE;
    _setmode(r, _O_BINARY);
    return r;
}

int64_t hio_dup2(int fd, int target) { return _dup2(fd, target) < 0 ? -L_EBADF : target; }

int64_t hio_pipe(int fds[2]) { return _pipe(fds, 65536, _O_BINARY) < 0 ? -L_EMFILE : 0; }

int64_t hio_mkdir(int dir, const char *path, uint32_t mode)
{
    (void)mode;
    wchar_t w[PATH_MAX_W];
    int64_t rc = wpath(dir, path, w);
    if (rc < 0)
        return rc;
    return CreateDirectoryW(w, NULL) ? 0 : last_error();
}

int64_t hio_unlink(int dir, const char *path, bool remove_dir)
{
    wchar_t w[PATH_MAX_W];
    int64_t rc = wpath(dir, path, w);
    if (rc < 0)
        return rc;
    if (remove_dir)
        return RemoveDirectoryW(w) ? 0 : last_error();
    DWORD a = GetFileAttributesW(w);
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY))
        return -L_EISDIR;
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(w, a & ~FILE_ATTRIBUTE_READONLY);
    return DeleteFileW(w) ? 0 : last_error();
}

int64_t hio_rename(int od, const char *op, int nd, const char *np)
{
    wchar_t a[PATH_MAX_W], b[PATH_MAX_W];
    int64_t rc = wpath(od, op, a);
    if (rc < 0)
        return rc;
    rc = wpath(nd, np, b);
    if (rc < 0)
        return rc;
    return MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING) ? 0 : last_error();
}

int64_t hio_access(int dir, const char *path, uint32_t mode)
{
    wchar_t w[PATH_MAX_W];
    int64_t rc = wpath(dir, path, w);
    if (rc < 0)
        return rc;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES)
        return last_error();
    if ((mode & 2) && (a & FILE_ATTRIBUTE_READONLY) && !(a & FILE_ATTRIBUTE_DIRECTORY))
        return -L_EACCES;
    return 0;
}

int64_t hio_readlink(int dir, const char *path, char *buf, size_t n)
{
    (void)dir;
    (void)path;
    (void)buf;
    (void)n;
    return -L_EINVAL;
}

int64_t hio_getcwd(char *buf, size_t n)
{
    wchar_t w[PATH_MAX_W];
    if (!_wgetcwd(w, PATH_MAX_W))
        return -L_ENOENT;
    for (wchar_t *p = w; *p; p++)
        if (*p == L'\\')
            *p = L'/';
    char tmp[PATH_MAX_W];
    if (!narrow(w, tmp, sizeof tmp))
        return -L_ERANGE;
    char out[PATH_MAX_W];
    if (tmp[0] && tmp[1] == ':') {
        out[0] = '/';
        out[1] = (tmp[0] >= 'A' && tmp[0] <= 'Z') ? (char)(tmp[0] + 32) : tmp[0];
        strcpy(out + 2, tmp[2] == '/' ? tmp + 2 : "");
    } else {
        strcpy(out, tmp);
    }
    if (strlen(out) + 1 > n)
        return -L_ERANGE;
    strcpy(buf, out);
    return (int64_t)strlen(buf) + 1;
}

int64_t hio_chdir(const char *path)
{
    wchar_t w[PATH_MAX_W];
    if (!widen(path, w, PATH_MAX_W))
        return -L_ENAMETOOLONG;
    return _wchdir(w) < 0 ? -L_ENOENT : 0;
}

int64_t hio_chmod(int dir, const char *path, uint32_t mode)
{
    wchar_t w[PATH_MAX_W];
    int64_t rc = wpath(dir, path, w);
    if (rc < 0)
        return rc;
    DWORD a = GetFileAttributesW(w);
    if (a == INVALID_FILE_ATTRIBUTES)
        return last_error();
    a = (mode & 0222) ? (a & ~FILE_ATTRIBUTE_READONLY) : (a | FILE_ATTRIBUTE_READONLY);
    return SetFileAttributesW(w, a) ? 0 : last_error();
}

static FILETIME to_filetime(int64_t sec, int64_t nsec)
{
    uint64_t t = (uint64_t)sec * 10000000ull + (uint64_t)nsec / 100 + 116444736000000000ull;
    FILETIME ft = { (DWORD)t, (DWORD)(t >> 32) };
    return ft;
}

int64_t hio_utimens(int dir, const char *path, const int64_t t[4], bool nofollow)
{
    HANDLE h;
    bool own = false;
    if (path[0] == 0 && dir != HIO_CWD) {
        h = handle_of(dir);
    } else {
        wchar_t w[PATH_MAX_W];
        int64_t rc = wpath(dir, path, w);
        if (rc < 0)
            return rc;
        h = CreateFileW(w, FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, NULL, OPEN_EXISTING,
                        FILE_FLAG_BACKUP_SEMANTICS | (nofollow ? FILE_FLAG_OPEN_REPARSE_POINT : 0), NULL);
        if (h == INVALID_HANDLE_VALUE)
            return last_error();
        own = true;
    }
    FILETIME now, a, m;
    GetSystemTimePreciseAsFileTime(&now);
    FILETIME *pa = NULL, *pm = NULL;
    if (t[1] == 0x3fffffff) {
        a = now;
        pa = &a;
    } else if (t[1] != 0x3ffffffe) {
        a = to_filetime(t[0], t[1]);
        pa = &a;
    }
    if (t[3] == 0x3fffffff) {
        m = now;
        pm = &m;
    } else if (t[3] != 0x3ffffffe) {
        m = to_filetime(t[2], t[3]);
        pm = &m;
    }
    BOOL ok = SetFileTime(h, NULL, pa, pm);
    int64_t rc = ok ? 0 : last_error();
    if (own)
        CloseHandle(h);
    return rc;
}

bool hio_isatty(int fd) { return _isatty(fd) != 0; }

int64_t hio_winsize(int fd, uint16_t ws[4])
{
    CONSOLE_SCREEN_BUFFER_INFO i;
    if (!GetConsoleScreenBufferInfo(handle_of(fd), &i))
        return -L_ENOTTY;
    ws[0] = (uint16_t)(i.srWindow.Bottom - i.srWindow.Top + 1);
    ws[1] = (uint16_t)(i.srWindow.Right - i.srWindow.Left + 1);
    ws[2] = ws[3] = 0;
    return 0;
}

int64_t hio_nread(int fd, int32_t *n)
{
    HANDLE h = handle_of(fd);
    DWORD avail = 0;
    if (GetFileType(h) == FILE_TYPE_PIPE) {
        if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
            return last_error();
    } else {
        LARGE_INTEGER size, pos;
        if (!GetFileSizeEx(h, &size))
            return -L_ENOTTY;
        pos.QuadPart = 0;
        SetFilePointerEx(h, pos, &pos, FILE_CURRENT);
        avail = size.QuadPart > pos.QuadPart ? (DWORD)(size.QuadPart - pos.QuadPart) : 0;
    }
    *n = (int32_t)avail;
    return 0;
}

int64_t hio_set_nonblock(int fd, bool on)
{
    (void)fd;
    return on ? -L_EINVAL : 0;
}

int64_t hio_get_status_flags(int fd)
{
    (void)fd;
    return L_O_RDWR;
}

int64_t hio_set_status_flags(int fd, uint32_t flags)
{
    (void)fd;
    return (flags & L_O_NONBLOCK) ? -L_EINVAL : 0;
}

int64_t hio_poll(int fd, uint32_t events, int64_t timeout_ns)
{
    HANDLE h = handle_of(fd);
    if (h == INVALID_HANDLE_VALUE)
        return L_POLLNVAL;
    uint32_t out = events & L_POLLOUT;
    if (!(events & L_POLLIN))
        return out;
    DWORD type = GetFileType(h);
    ULONGLONG end = timeout_ns < 0 ? 0 : GetTickCount64() + (ULONGLONG)(timeout_ns / 1000000);
    for (;;) {
        if (type == FILE_TYPE_PIPE) {
            DWORD avail = 0;
            if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
                return out | L_POLLHUP;
            if (avail)
                return out | L_POLLIN;
        } else if (type == FILE_TYPE_CHAR) {
            if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0)
                return out | L_POLLIN;
        } else {
            return out | L_POLLIN;
        }
        if (out || (timeout_ns >= 0 && GetTickCount64() >= end))
            return out;
        Sleep(1);
    }
}

void *hio_waker_new(void) { return CreateEventW(NULL, TRUE, FALSE, NULL); }

void hio_waker_free(void *w)
{
    if (w)
        CloseHandle((HANDLE)w);
}

void hio_waker_kick(void *w) { SetEvent((HANDLE)w); }

void hio_waker_clear(void *w) { ResetEvent((HANDLE)w); }

int64_t hio_wait(int fd, uint32_t events, int64_t timeout_ns, void *waker)
{
    HANDLE ev = (HANDLE)waker;
    ULONGLONG end = timeout_ns < 0 ? 0 : GetTickCount64() + (ULONGLONG)((timeout_ns + 999999) / 1000000);
    HANDLE h = fd >= 0 ? handle_of(fd) : INVALID_HANDLE_VALUE;
    DWORD type = h != INVALID_HANDLE_VALUE ? GetFileType(h) : FILE_TYPE_UNKNOWN;
    uint32_t out = (fd >= 0) ? (events & L_POLLOUT) : 0;
    for (;;) {
        if (fd >= 0 && (events & L_POLLIN)) {
            if (type == FILE_TYPE_PIPE) {
                DWORD avail = 0;
                if (!PeekNamedPipe(h, NULL, 0, NULL, &avail, NULL))
                    return L_POLLIN | L_POLLHUP;
                if (avail)
                    return L_POLLIN;
            } else if (type == FILE_TYPE_CHAR) {
                if (WaitForSingleObject(h, 0) == WAIT_OBJECT_0)
                    return L_POLLIN;
            } else {
                return L_POLLIN;
            }
        } else if (out) {
            return out;
        }
        if (ev && WaitForSingleObject(ev, 0) == WAIT_OBJECT_0)
            return -L_EINTR;
        DWORD slice = 1;
        if (timeout_ns >= 0) {
            ULONGLONG now = GetTickCount64();
            if (now >= end)
                return 0;
            if (end - now < slice)
                slice = (DWORD)(end - now);
        }
        if (fd < 0 && ev) {
            DWORD ms = timeout_ns < 0 ? INFINITE : (DWORD)(end - GetTickCount64());
            if (WaitForSingleObject(ev, ms) == WAIT_OBJECT_0)
                return -L_EINTR;
            if (timeout_ns >= 0)
                return 0;
        } else if (ev) {
            WaitForSingleObject(ev, slice);
        } else {
            Sleep(slice);
        }
    }
}

typedef struct win_dir {
    HANDLE find;
    wchar_t pattern[PATH_MAX_W];
    WIN32_FIND_DATAW data;
    bool have;
    bool done;
    int fd;
} win_dir;

void *hio_dir_open(int fd)
{
    win_dir *d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    wchar_t base[PATH_MAX_W];
    DWORD n = GetFinalPathNameByHandleW(handle_of(fd), base, PATH_MAX_W - 3, FILE_NAME_NORMALIZED);
    if (n == 0 || n >= PATH_MAX_W - 3) {
        free(d);
        return NULL;
    }
    size_t skip = wcsncmp(base, L"\\\\?\\", 4) == 0 ? 4 : 0;
    wcscpy(d->pattern, base + skip);
    wcscat(d->pattern, L"\\*");
    d->fd = fd;
    d->find = INVALID_HANDLE_VALUE;
    return d;
}

void hio_dir_close(void *dir)
{
    win_dir *d = dir;
    if (d->find != INVALID_HANDLE_VALUE)
        FindClose(d->find);
    _close(d->fd);
    free(d);
}

int64_t hio_dir_next(void *dir, char name[256], uint64_t *ino, uint8_t *type)
{
    win_dir *d = dir;
    if (d->done)
        return 0;
    if (d->find == INVALID_HANDLE_VALUE) {
        d->find = FindFirstFileW(d->pattern, &d->data);
        if (d->find == INVALID_HANDLE_VALUE) {
            d->done = true;
            return GetLastError() == ERROR_FILE_NOT_FOUND ? 0 : last_error();
        }
    } else if (!FindNextFileW(d->find, &d->data)) {
        d->done = true;
        return GetLastError() == ERROR_NO_MORE_FILES ? 0 : last_error();
    }
    char tmp[1024];
    if (!narrow(d->data.cFileName, tmp, sizeof tmp))
        return -L_EIO;
    size_t n = strlen(tmp);
    if (n > 255)
        n = 255;
    memcpy(name, tmp, n);
    name[n] = 0;
    *ino = ((uint64_t)d->data.nFileSizeHigh << 32) ^ d->data.nFileSizeLow ^ (uint64_t)n;
    *type = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? L_DT_DIR : L_DT_REG;
    return 1;
}

void hio_dir_rewind(void *dir)
{
    win_dir *d = dir;
    if (d->find != INVALID_HANDLE_VALUE)
        FindClose(d->find);
    d->find = INVALID_HANDLE_VALUE;
    d->done = false;
}

int64_t hio_realtime_ns(void)
{
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    uint64_t nsec;
    int64_t sec = unix_time_s(ft, &nsec);
    return sec * 1000000000ll + (int64_t)nsec;
}

int64_t hio_cputime_ns(void)
{
    FILETIME c, e, k, u;
    if (!GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u))
        return 0;
    uint64_t kt = ((uint64_t)k.dwHighDateTime << 32) | k.dwLowDateTime;
    uint64_t ut = ((uint64_t)u.dwHighDateTime << 32) | u.dwLowDateTime;
    return (int64_t)((kt + ut) * 100);
}

int32_t hio_pid(void) { return (int32_t)GetCurrentProcessId(); }

int32_t hio_ppid(void) { return 1; }

uint32_t hio_uid(void) { return 1000; }

uint32_t hio_gid(void) { return 1000; }

void hio_nodename(char *buf, size_t n)
{
    DWORD len = (DWORD)n;
    if (n == 0)
        return;
    if (!GetComputerNameA(buf, &len))
        strncpy(buf, "localhost", n);
    buf[n - 1] = 0;
}

uint32_t hio_cpu_count(void)
{
    DWORD n = GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
    return n ? n : 1;
}

uint32_t hio_umask(uint32_t mask)
{
    int old = g_umask;
    g_umask = (int)mask;
    return (uint32_t)old;
}

#endif
