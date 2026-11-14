/**
 * @file fs.c
 * @brief fsp-srv (IFileSystemProxy) with the SD card file system on a host
 * directory: IFileSystem, IFile and IDirectory over POSIX calls. The data
 * storages are in fs_romfs.c.
 *
 * Command ids, argument layouts and results follow switchbrew.org
 * (Filesystem services), libnx (ISC) fs_sf.h and Atmosphere's result
 * numbers for nn::fs, read as specifications.
 */
#include "hos_fs.h"
#include "fs_priv.h"
#include "ipc.h"
#include "services.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/** @brief Size of a guest path buffer (FspPath) in bytes. */
#define FS_GUEST_PATH 0x301u
/** @brief Size of one FsDirectoryEntry in bytes. */
#define FS_DIRENT_SIZE 0x310u
/** @brief Largest piece moved between a host file and guest memory in one step. */
#define FS_CHUNK 0x10000u

/** @brief File open mode bits (FsOpenMode). */
enum {
    FS_MODE_READ = 1,
    FS_MODE_WRITE = 2,
    FS_MODE_APPEND = 4
};

/** @brief Directory open mode bits (FsDirOpenMode). */
enum {
    FS_DIR_READ_DIRS = 1,
    FS_DIR_READ_FILES = 2,
    FS_DIR_NO_FILE_SIZE = 1u << 31
};

static char g_root[PATH_MAX];

static rl_result errno_result(int e)
{
    switch (e) {
    case ENOENT:
    case ENOTDIR:
    case EISDIR:
        return FS_PATH_NOT_FOUND;
    case EEXIST:
        return FS_PATH_ALREADY_EXISTS;
    case ENOTEMPTY:
        return FS_DIRECTORY_NOT_EMPTY;
    case EBUSY:
    case ETXTBSY:
        return FS_TARGET_LOCKED;
    case ENOSPC:
    case EDQUOT:
        return FS_USABLE_SPACE_NOT_ENOUGH;
    case ENAMETOOLONG:
        return FS_PATH_TOO_LONG;
    default:
        return FS_UNEXPECTED;
    }
}

/**
 * @brief Resolves guest path argument idx (an X or A buffer holding a
 * NUL-terminated path starting with '/') to a host path under the root.
 * Empty and "." components are dropped; ".." is refused.
 */
static rl_result host_path(const rl_ipc_req *r, uint32_t idx, char *out, size_t n)
{
    char gp[FS_GUEST_PATH + 1];
    rl_ipc_buf b = rl_ipc_auto_in(r, idx);
    uint64_t len = b.size < FS_GUEST_PATH ? b.size : FS_GUEST_PATH;
    size_t used = strlen(g_root);
    const char *p;
    if (b.size == 0)
        return FS_INVALID_PATH;
    if (!rl_ipc_buf_read(&b, gp, len))
        return KR_INVALID_MEM_STATE;
    gp[len] = 0;
    if (len == FS_GUEST_PATH && strlen(gp) == len)
        return FS_PATH_TOO_LONG;
    if (gp[0] != '/')
        return FS_INVALID_PATH_FORMAT;
    if (used >= n)
        return FS_PATH_TOO_LONG;
    memcpy(out, g_root, used);
    out[used] = 0;
    p = gp;
    while (*p) {
        const char *e;
        size_t clen;
        while (*p == '/')
            p++;
        if (!*p)
            break;
        e = p;
        while (*e && *e != '/')
            e++;
        clen = (size_t)(e - p);
        if (clen == 1 && p[0] == '.') {
            p = e;
            continue;
        }
        if (clen == 2 && p[0] == '.' && p[1] == '.')
            return FS_INVALID_PATH;
        if (used + 1 + clen >= n)
            return FS_PATH_TOO_LONG;
        out[used++] = '/';
        memcpy(out + used, p, clen);
        used += clen;
        out[used] = 0;
        p = e;
    }
    return KR_SUCCESS;
}

static rl_result stat_path(const char *path, struct stat *st)
{
    return stat(path, st) == 0 ? KR_SUCCESS : errno_result(errno);
}

static rl_result create_file(rl_ipc_req *r)
{
    struct {
        uint32_t option;
        uint32_t pad;
        uint64_t size;
    } in;
    char path[PATH_MAX];
    int fd;
    rl_result rc;
    if (!rl_ipc_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    rc = host_path(r, 0, path, sizeof path);
    if (rc != KR_SUCCESS)
        return rc;
    if (in.size > (uint64_t)INT64_MAX)
        return FS_INVALID_SIZE;
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (fd < 0)
        return errno_result(errno);
    if (in.size && ftruncate(fd, (off_t)in.size) != 0) {
        rc = errno_result(errno);
        close(fd);
        unlink(path);
        return rc;
    }
    close(fd);
    return KR_SUCCESS;
}

static rl_result delete_file(rl_ipc_req *r)
{
    char path[PATH_MAX];
    struct stat st;
    rl_result rc = host_path(r, 0, path, sizeof path);
    if (rc != KR_SUCCESS)
        return rc;
    if (lstat(path, &st) != 0)
        return errno_result(errno);
    if (S_ISDIR(st.st_mode))
        return FS_PATH_NOT_FOUND;
    return unlink(path) == 0 ? KR_SUCCESS : errno_result(errno);
}

static rl_result create_directory(rl_ipc_req *r)
{
    char path[PATH_MAX];
    rl_result rc = host_path(r, 0, path, sizeof path);
    if (rc != KR_SUCCESS)
        return rc;
    return mkdir(path, 0777) == 0 ? KR_SUCCESS : errno_result(errno);
}

static rl_result delete_directory(rl_ipc_req *r)
{
    char path[PATH_MAX];
    rl_result rc = host_path(r, 0, path, sizeof path);
    if (rc != KR_SUCCESS)
        return rc;
    return rmdir(path) == 0 ? KR_SUCCESS : errno_result(errno);
}

/** @brief Removes everything under dir, and dir itself unless keep_dir. */
static rl_result remove_tree(const char *dir, int keep_dir)
{
    DIR *d = opendir(dir);
    struct dirent *de;
    rl_result rc = KR_SUCCESS;
    if (!d)
        return errno_result(errno);
    while (rc == KR_SUCCESS && (de = readdir(d)) != NULL) {
        char child[PATH_MAX];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (snprintf(child, sizeof child, "%s/%s", dir, de->d_name) >= (int)sizeof child) {
            rc = FS_PATH_TOO_LONG;
            break;
        }
        if (lstat(child, &st) != 0)
            rc = errno_result(errno);
        else if (S_ISDIR(st.st_mode))
            rc = remove_tree(child, 0);
        else if (unlink(child) != 0)
            rc = errno_result(errno);
    }
    closedir(d);
    if (rc == KR_SUCCESS && !keep_dir && rmdir(dir) != 0)
        rc = errno_result(errno);
    return rc;
}

static rl_result delete_directory_recursively(rl_ipc_req *r)
{
    char path[PATH_MAX];
    rl_result rc = host_path(r, 0, path, sizeof path);
    if (rc != KR_SUCCESS)
        return rc;
    return remove_tree(path, 0);
}

static rl_result clean_directory_recursively(rl_ipc_req *r)
{
    char path[PATH_MAX];
    rl_result rc = host_path(r, 0, path, sizeof path);
    if (rc != KR_SUCCESS)
        return rc;
    return remove_tree(path, 1);
}

static rl_result rename_kind(rl_ipc_req *r, int want_dir)
{
    char from[PATH_MAX], to[PATH_MAX];
    struct stat st;
    rl_result rc = host_path(r, 0, from, sizeof from);
    if (rc == KR_SUCCESS)
        rc = host_path(r, 1, to, sizeof to);
    if (rc != KR_SUCCESS)
        return rc;
    if (lstat(from, &st) != 0)
        return errno_result(errno);
    if (S_ISDIR(st.st_mode) != (want_dir != 0))
        return FS_PATH_NOT_FOUND;
    if (lstat(to, &st) == 0)
        return FS_PATH_ALREADY_EXISTS;
    return rename(from, to) == 0 ? KR_SUCCESS : errno_result(errno);
}

static rl_result rename_file(rl_ipc_req *r) { return rename_kind(r, 0); }

static rl_result rename_directory(rl_ipc_req *r) { return rename_kind(r, 1); }

static rl_result get_entry_type(rl_ipc_req *r)
{
    char path[PATH_MAX];
    struct stat st;
    rl_result rc = host_path(r, 0, path, sizeof path);
    if (rc == KR_SUCCESS)
        rc = stat_path(path, &st);
    if (rc != KR_SUCCESS)
        return rc;
    *(uint32_t *)rl_ipc_out(r, 4) = S_ISDIR(st.st_mode) ? 0u : 1u;
    return KR_SUCCESS;
}

/** @brief State of an IFile. */
typedef struct fs_file {
    int fd;
    uint32_t mode;
} fs_file;

/** @brief One entry of an open directory. */
typedef struct fs_dirent {
    char name[FS_GUEST_PATH];
    uint8_t type;
    uint64_t size;
} fs_dirent;

/** @brief State of an IDirectory: the entries are read when it is opened. */
typedef struct fs_dir {
    fs_dirent *ents;
    uint32_t count;
    uint32_t pos;
} fs_dir;

static void file_destroy(void *state)
{
    fs_file *f = state;
    close(f->fd);
    free(f);
}

static void dir_destroy(void *state)
{
    fs_dir *d = state;
    free(d->ents);
    free(d);
}

static rl_result file_read(rl_ipc_req *r);
static rl_result file_write(rl_ipc_req *r);
static rl_result file_flush(rl_ipc_req *r);
static rl_result file_set_size(rl_ipc_req *r);
static rl_result file_get_size(rl_ipc_req *r);
static rl_result dir_read(rl_ipc_req *r);
static rl_result dir_get_entry_count(rl_ipc_req *r);

static const rl_ipc_cmd file_cmds[] = {
    { 0, "Read", file_read },         { 1, "Write", file_write },       { 2, "Flush", file_flush },
    { 3, "SetSize", file_set_size },  { 4, "GetSize", file_get_size },
};

static const rl_ipc_class file_class = { "nn::fssrv::sf::IFile", file_cmds, sizeof file_cmds / sizeof file_cmds[0], 0,
                                         file_destroy };

static const rl_ipc_cmd dir_cmds[] = {
    { 0, "Read", dir_read },
    { 1, "GetEntryCount", dir_get_entry_count },
};

static const rl_ipc_class dir_class = { "nn::fssrv::sf::IDirectory", dir_cmds, sizeof dir_cmds / sizeof dir_cmds[0], 0,
                                        dir_destroy };

static rl_result open_file(rl_ipc_req *r)
{
    uint32_t mode;
    char path[PATH_MAX];
    struct stat st;
    int flags, fd;
    fs_file *f;
    rl_ipc_object *o;
    rl_result rc;
    if (!rl_ipc_in(r, &mode, 4))
        return SF_RESULT_INVALID_HEADER;
    rc = host_path(r, 0, path, sizeof path);
    if (rc != KR_SUCCESS)
        return rc;
    if ((mode & ~7u) != 0 || (mode & (FS_MODE_READ | FS_MODE_WRITE)) == 0)
        return FS_INVALID_OPEN_MODE;
    if ((mode & FS_MODE_READ) && (mode & FS_MODE_WRITE))
        flags = O_RDWR;
    else
        flags = (mode & FS_MODE_WRITE) ? O_WRONLY : O_RDONLY;
    fd = open(path, flags | O_CLOEXEC);
    if (fd < 0)
        return errno_result(errno);
    if (fstat(fd, &st) != 0 || S_ISDIR(st.st_mode)) {
        close(fd);
        return FS_PATH_NOT_FOUND;
    }
    f = calloc(1, sizeof *f);
    if (!f) {
        close(fd);
        return KR_OUT_OF_RESOURCE;
    }
    f->fd = fd;
    f->mode = mode;
    o = rl_ipc_object_new(&file_class, f);
    if (!o) {
        file_destroy(f);
        return KR_OUT_OF_RESOURCE;
    }
    return rl_ipc_push_object(r, o);
}

static int dirent_cmp(const void *a, const void *b)
{
    return strcmp(((const fs_dirent *)a)->name, ((const fs_dirent *)b)->name);
}

static rl_result open_directory(rl_ipc_req *r)
{
    uint32_t mode;
    char path[PATH_MAX];
    DIR *dh;
    struct dirent *de;
    fs_dir *d;
    rl_ipc_object *o;
    uint32_t cap = 0;
    rl_result rc;
    if (!rl_ipc_in(r, &mode, 4))
        return SF_RESULT_INVALID_HEADER;
    rc = host_path(r, 0, path, sizeof path);
    if (rc != KR_SUCCESS)
        return rc;
    dh = opendir(path);
    if (!dh)
        return errno_result(errno);
    d = calloc(1, sizeof *d);
    if (!d) {
        closedir(dh);
        return KR_OUT_OF_RESOURCE;
    }
    while ((de = readdir(dh)) != NULL) {
        char child[PATH_MAX];
        struct stat st;
        int is_dir;
        fs_dirent *e;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..") || strlen(de->d_name) >= FS_GUEST_PATH - 1)
            continue;
        if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >= (int)sizeof child || stat(child, &st) != 0)
            continue;
        is_dir = S_ISDIR(st.st_mode);
        if (!(mode & (is_dir ? FS_DIR_READ_DIRS : FS_DIR_READ_FILES)))
            continue;
        if (d->count == cap) {
            fs_dirent *grown;
            cap = cap ? cap * 2 : 16;
            grown = realloc(d->ents, (size_t)cap * sizeof *grown);
            if (!grown) {
                closedir(dh);
                dir_destroy(d);
                return KR_OUT_OF_RESOURCE;
            }
            d->ents = grown;
        }
        e = &d->ents[d->count++];
        memset(e, 0, sizeof *e);
        memcpy(e->name, de->d_name, strlen(de->d_name));
        e->type = is_dir ? 0 : 1;
        e->size = (is_dir || (mode & FS_DIR_NO_FILE_SIZE)) ? 0 : (uint64_t)st.st_size;
    }
    closedir(dh);
    if (d->count > 1)
        qsort(d->ents, d->count, sizeof *d->ents, dirent_cmp);
    o = rl_ipc_object_new(&dir_class, d);
    if (!o) {
        dir_destroy(d);
        return KR_OUT_OF_RESOURCE;
    }
    return rl_ipc_push_object(r, o);
}

static rl_result commit(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

static rl_result space_size(rl_ipc_req *r, int total)
{
    struct statvfs sv;
    uint64_t v;
    if (statvfs(g_root, &sv) != 0)
        return errno_result(errno);
    v = (uint64_t)(total ? sv.f_blocks : sv.f_bavail) * (uint64_t)sv.f_frsize;
    *(uint64_t *)rl_ipc_out(r, 8) = v;
    return KR_SUCCESS;
}

static rl_result get_free_space_size(rl_ipc_req *r) { return space_size(r, 0); }

static rl_result get_total_space_size(rl_ipc_req *r) { return space_size(r, 1); }

static rl_result get_file_time_stamp_raw(rl_ipc_req *r)
{
    char path[PATH_MAX];
    struct stat st;
    uint64_t *out;
    rl_result rc = host_path(r, 0, path, sizeof path);
    if (rc == KR_SUCCESS)
        rc = stat_path(path, &st);
    if (rc != KR_SUCCESS)
        return rc;
    out = rl_ipc_out(r, 0x20);
    out[0] = (uint64_t)st.st_ctime;
    out[1] = (uint64_t)st.st_mtime;
    out[2] = (uint64_t)st.st_atime;
    ((uint8_t *)out)[0x18] = 1;
    return KR_SUCCESS;
}

static const rl_ipc_cmd fs_cmds[] = {
    { 0, "CreateFile", create_file },
    { 1, "DeleteFile", delete_file },
    { 2, "CreateDirectory", create_directory },
    { 3, "DeleteDirectory", delete_directory },
    { 4, "DeleteDirectoryRecursively", delete_directory_recursively },
    { 5, "RenameFile", rename_file },
    { 6, "RenameDirectory", rename_directory },
    { 7, "GetEntryType", get_entry_type },
    { 8, "OpenFile", open_file },
    { 9, "OpenDirectory", open_directory },
    { 10, "Commit", commit },
    { 11, "GetFreeSpaceSize", get_free_space_size },
    { 12, "GetTotalSpaceSize", get_total_space_size },
    { 13, "CleanDirectoryRecursively", clean_directory_recursively },
    { 14, "GetFileTimeStampRaw", get_file_time_stamp_raw },
};

static const rl_ipc_class fs_class = { "nn::fssrv::sf::IFileSystem", fs_cmds, sizeof fs_cmds / sizeof fs_cmds[0], 0,
                                       NULL };

/** @brief The layout of the first 24 bytes of an IFile Read or Write request. */
typedef struct file_io_in {
    uint32_t option;
    uint32_t pad;
    int64_t offset;
    uint64_t size;
} file_io_in;

static rl_result file_read(rl_ipc_req *r)
{
    fs_file *f = r->self->state;
    file_io_in in;
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    uint8_t *buf;
    uint64_t done = 0;
    if (!rl_ipc_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    if (!(f->mode & FS_MODE_READ))
        return FS_READ_NOT_PERMITTED;
    if (in.offset < 0)
        return FS_INVALID_OFFSET;
    if (in.size > out.size)
        in.size = out.size;
    buf = malloc(in.size < FS_CHUNK ? (size_t)in.size + 1 : FS_CHUNK);
    if (!buf)
        return KR_OUT_OF_RESOURCE;
    while (done < in.size) {
        uint64_t want = in.size - done < FS_CHUNK ? in.size - done : FS_CHUNK;
        ssize_t n = pread(f->fd, buf, (size_t)want, (off_t)((uint64_t)in.offset + done));
        rl_ipc_buf piece;
        if (n < 0) {
            int e = errno;
            free(buf);
            return errno_result(e);
        }
        if (n == 0)
            break;
        piece.addr = out.addr + done;
        piece.size = (uint64_t)n;
        piece.attr = out.attr;
        if (!rl_ipc_buf_write(&piece, buf, (uint64_t)n)) {
            free(buf);
            return KR_INVALID_MEM_STATE;
        }
        done += (uint64_t)n;
    }
    free(buf);
    *(uint64_t *)rl_ipc_out(r, 8) = done;
    return KR_SUCCESS;
}

static rl_result file_write(rl_ipc_req *r)
{
    fs_file *f = r->self->state;
    file_io_in in;
    rl_ipc_buf src = rl_ipc_auto_in(r, 0);
    struct stat st;
    uint8_t *buf;
    uint64_t done = 0;
    if (!rl_ipc_in(r, &in, sizeof in))
        return SF_RESULT_INVALID_HEADER;
    if (!(f->mode & FS_MODE_WRITE))
        return FS_WRITE_NOT_PERMITTED;
    if (in.offset < 0)
        return FS_INVALID_OFFSET;
    if (in.size > src.size)
        return FS_INVALID_SIZE;
    if (!(f->mode & FS_MODE_APPEND)) {
        if (fstat(f->fd, &st) != 0)
            return errno_result(errno);
        if ((uint64_t)in.offset + in.size > (uint64_t)st.st_size)
            return FS_APPEND_REQUIRED;
    }
    buf = malloc(in.size < FS_CHUNK ? (size_t)in.size + 1 : FS_CHUNK);
    if (!buf)
        return KR_OUT_OF_RESOURCE;
    while (done < in.size) {
        uint64_t want = in.size - done < FS_CHUNK ? in.size - done : FS_CHUNK;
        rl_ipc_buf piece;
        ssize_t n;
        piece.addr = src.addr + done;
        piece.size = want;
        piece.attr = src.attr;
        if (!rl_ipc_buf_read(&piece, buf, want)) {
            free(buf);
            return KR_INVALID_MEM_STATE;
        }
        n = pwrite(f->fd, buf, (size_t)want, (off_t)((uint64_t)in.offset + done));
        if (n <= 0) {
            int e = n < 0 ? errno : ENOSPC;
            free(buf);
            return errno_result(e);
        }
        done += (uint64_t)n;
    }
    free(buf);
    return KR_SUCCESS;
}

static rl_result file_flush(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

static rl_result file_set_size(rl_ipc_req *r)
{
    fs_file *f = r->self->state;
    uint64_t size;
    if (!rl_ipc_in(r, &size, 8))
        return SF_RESULT_INVALID_HEADER;
    if (!(f->mode & FS_MODE_WRITE))
        return FS_WRITE_NOT_PERMITTED;
    if (size > (uint64_t)INT64_MAX)
        return FS_INVALID_SIZE;
    return ftruncate(f->fd, (off_t)size) == 0 ? KR_SUCCESS : errno_result(errno);
}

static rl_result file_get_size(rl_ipc_req *r)
{
    fs_file *f = r->self->state;
    struct stat st;
    if (fstat(f->fd, &st) != 0)
        return errno_result(errno);
    *(uint64_t *)rl_ipc_out(r, 8) = (uint64_t)st.st_size;
    return KR_SUCCESS;
}

static rl_result dir_read(rl_ipc_req *r)
{
    fs_dir *d = r->self->state;
    rl_ipc_buf out = rl_ipc_auto_out(r, 0);
    uint64_t room = out.size / FS_DIRENT_SIZE;
    uint64_t n = d->count - d->pos;
    uint8_t *buf;
    if (n > room)
        n = room;
    if (n) {
        buf = calloc((size_t)n, FS_DIRENT_SIZE);
        if (!buf)
            return KR_OUT_OF_RESOURCE;
        for (uint64_t i = 0; i < n; i++) {
            const fs_dirent *e = &d->ents[d->pos + i];
            uint8_t *rec = buf + i * FS_DIRENT_SIZE;
            memcpy(rec, e->name, FS_GUEST_PATH);
            rec[0x301] = e->type == 0 ? 1 : 0;
            rec[0x304] = e->type;
            memcpy(rec + 0x308, &e->size, 8);
        }
        if (!rl_ipc_buf_write(&out, buf, n * FS_DIRENT_SIZE)) {
            free(buf);
            return KR_INVALID_MEM_STATE;
        }
        free(buf);
        d->pos += (uint32_t)n;
    }
    *(uint64_t *)rl_ipc_out(r, 8) = n;
    return KR_SUCCESS;
}

static rl_result dir_get_entry_count(rl_ipc_req *r)
{
    fs_dir *d = r->self->state;
    *(uint64_t *)rl_ipc_out(r, 8) = d->count;
    return KR_SUCCESS;
}

static rl_result set_current_process(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

static rl_result open_sd_card_file_system(rl_ipc_req *r)
{
    rl_ipc_object *o = rl_ipc_object_new(&fs_class, NULL);
    if (!o)
        return KR_OUT_OF_RESOURCE;
    return rl_ipc_push_object(r, o);
}

static rl_result disable_auto_save_data_creation(rl_ipc_req *r)
{
    (void)r;
    return KR_SUCCESS;
}

static rl_result get_global_access_log_mode(rl_ipc_req *r)
{
    *(uint32_t *)rl_ipc_out(r, 4) = 0;
    return KR_SUCCESS;
}

static const rl_ipc_cmd proxy_cmds[] = {
    { 1, "SetCurrentProcess", set_current_process },
    { 18, "OpenSdCardFileSystem", open_sd_card_file_system },
    { 200, "OpenDataStorageByCurrentProcess", rl_fs_open_data_storage_current },
    { 201, "OpenDataStorageByProgramId", rl_fs_open_data_storage_program },
    { 202, "OpenDataStorageByDataId", rl_fs_open_data_storage_data_id },
    { 203, "OpenPatchDataStorageByCurrentProcess", rl_fs_open_patch_data_storage },
    { 1003, "DisableAutoSaveDataCreation", disable_auto_save_data_creation },
    RL_SRV_STUB(1004, "SetGlobalAccessLogMode"),
    { 1005, "GetGlobalAccessLogMode", get_global_access_log_mode },
    RL_SRV_STUB(1006, "OutputAccessLogToSdCard"),
};

static const rl_ipc_class proxy_class = { "nn::fssrv::sf::IFileSystemProxy", proxy_cmds,
                                          sizeof proxy_cmds / sizeof proxy_cmds[0], 0, NULL };

static rl_ipc_object *proxy_factory(void) { return rl_ipc_object_new(&proxy_class, NULL); }

void rl_hos_fs_init(const char *sdmc_host_dir)
{
    size_t n;
    if (!sdmc_host_dir || !*sdmc_host_dir)
        sdmc_host_dir = "./sdmc";
    n = strlen(sdmc_host_dir);
    if (n >= sizeof g_root)
        n = sizeof g_root - 1;
    memcpy(g_root, sdmc_host_dir, n);
    while (n > 1 && g_root[n - 1] == '/')
        n--;
    g_root[n] = 0;
    rl_ipc_register_service("fsp-srv", proxy_factory);
}

static void make_dirs(const char *path)
{
    char buf[4096];
    size_t n = strlen(path);
    if (n == 0 || n >= sizeof buf)
        return;
    memcpy(buf, path, n + 1);
    for (size_t i = 1; i <= n; i++)
        if (buf[i] == '/' || buf[i] == 0) {
            char c = buf[i];
            buf[i] = 0;
            if (mkdir(buf, 0777) != 0 && errno != EEXIST)
                rl_log(RL_LOG_WARN, "fs", "cannot create %s: %s", buf, strerror(errno));
            buf[i] = c;
        }
}

void rl_srv_install_fs(void)
{
    const char *sdmc = getenv("RELIFT_SDMC");
    if (!sdmc || !*sdmc)
        sdmc = "./sdmc";
    make_dirs(sdmc);
    rl_hos_fs_init(sdmc);
    rl_fs_romfs_env();
}
