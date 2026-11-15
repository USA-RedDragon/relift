/**
 * @file save_journal.c
 * @brief Journaled save data on host directories: a mount works on a copy of
 * the save directory and Commit swaps the copy in atomically.
 *
 * The committed state is the save directory itself, plain files with no
 * metadata. Next to it, in the same parent, a mount keeps hidden siblings
 * named after the directory: ".<name>.work" (the working copy the game reads
 * and writes), ".<name>.new" (the synced copy being committed),
 * ".<name>.old" (the previous state during the swap) and ".<name>.commit"
 * (the marker that says the new copy is complete). Recovery on the next
 * mount decides by the marker: with it the swap finishes, without it the
 * leftovers are dropped, so the save directory is always one whole state.
 */
#include "fs_priv.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hostio.h"

/** @brief Size of the buffer that moves file contents between directories. */
#define SAVE_COPY_CHUNK 0x10000u

/** @brief One mounted save data file system. */
struct rl_save_mount {
    char dir[PATH_MAX];
    char work[PATH_MAX];
    char stage[PATH_MAX];
    char old[PATH_MAX];
    char marker[PATH_MAX];
    char parent[PATH_MAX];
    int direct;
    int read_only;
    int crashed;
    rl_save_mount *next;
};

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static rl_save_mount *g_active;
static int g_fault;

static int exists(const char *path)
{
    struct stat st;
    return lstat(path, &st) == 0;
}

static int sync_fd(int fd)
{
#ifdef F_FULLFSYNC
    if (fcntl(fd, F_FULLFSYNC) == 0)
        return 0;
#endif
    return fsync(fd) == 0 ? 0 : errno;
}

static int sync_dir(const char *path)
{
#if defined(_WIN32)
    (void)path;
    return 0;
#else
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    int e;
    if (fd < 0)
        return errno;
    e = sync_fd(fd);
    close(fd);
    return e;
#endif
}

static int remove_tree(const char *path)
{
    struct stat st;
    int e = 0;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : errno;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        struct dirent *de;
        if (!d)
            return errno;
        while (!e && (de = readdir(d)) != NULL) {
            char child[PATH_MAX];
            if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
                continue;
            if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >= (int)sizeof child)
                e = ENAMETOOLONG;
            else
                e = remove_tree(child);
        }
        closedir(d);
        if (!e && rmdir(path) != 0)
            e = errno;
        return e;
    }
    return unlink(path) == 0 ? 0 : errno;
}

static int copy_file(const char *src, const char *dst, int sync)
{
    int in = open(src, O_RDONLY | O_CLOEXEC);
    int out, e = 0;
    char *buf;
    if (in < 0)
        return errno;
    out = open(dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
    if (out < 0) {
        e = errno;
        close(in);
        return e;
    }
    buf = malloc(SAVE_COPY_CHUNK);
    if (!buf)
        e = ENOMEM;
    while (!e) {
        ssize_t n = read(in, buf, SAVE_COPY_CHUNK);
        char *p = buf;
        if (n < 0) {
            e = errno;
            break;
        }
        if (n == 0)
            break;
        while (n > 0) {
            ssize_t w = write(out, p, (size_t)n);
            if (w <= 0) {
                e = w < 0 ? errno : ENOSPC;
                break;
            }
            p += w;
            n -= w;
        }
    }
    free(buf);
    if (!e && sync)
        e = sync_fd(out);
    close(in);
    if (close(out) != 0 && !e)
        e = errno;
    return e;
}

/** @brief Copies the files and directories under src into the new directory dst. */
static int copy_tree(const char *src, const char *dst, int sync)
{
    DIR *d;
    struct dirent *de;
    int e = 0;
    if (mkdir(dst, 0777) != 0)
        return errno;
    d = opendir(src);
    if (!d)
        return errno;
    while (!e && (de = readdir(d)) != NULL) {
        char from[PATH_MAX], to[PATH_MAX];
        struct stat st;
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (snprintf(from, sizeof from, "%s/%s", src, de->d_name) >= (int)sizeof from ||
            snprintf(to, sizeof to, "%s/%s", dst, de->d_name) >= (int)sizeof to) {
            e = ENAMETOOLONG;
        } else if (lstat(from, &st) != 0) {
            e = errno;
        } else if (S_ISDIR(st.st_mode)) {
            e = copy_tree(from, to, sync);
        } else if (S_ISREG(st.st_mode)) {
            e = copy_file(from, to, sync);
        }
    }
    closedir(d);
    if (!e && sync)
        e = sync_dir(dst);
    return e;
}

static int sibling(char *out, const char *parent, const char *name, const char *kind)
{
    return snprintf(out, PATH_MAX, "%s/.%s.%s", parent, name, kind) < PATH_MAX ? 0 : ENAMETOOLONG;
}

/**
 * @brief Brings the save directory to one committed state: finishes a swap
 * whose marker was written, drops the leftovers of one that was not.
 */
static int recover_dir(const rl_save_mount *m)
{
    int e;
    if (exists(m->marker)) {
        if (exists(m->stage)) {
            if (exists(m->dir) && rename(m->dir, m->old) != 0)
                return errno;
            if (rename(m->stage, m->dir) != 0)
                return errno;
        }
        e = remove_tree(m->old);
        if (e)
            return e;
        if (unlink(m->marker) != 0 && errno != ENOENT)
            return errno;
    } else {
        e = remove_tree(m->stage);
        if (e)
            return e;
        if (!exists(m->dir) && exists(m->old)) {
            if (rename(m->old, m->dir) != 0)
                return errno;
        } else {
            e = remove_tree(m->old);
            if (e)
                return e;
        }
    }
    return sync_dir(m->parent);
}

static const rl_save_mount *find_active(const char *dir)
{
    for (const rl_save_mount *m = g_active; m; m = m->next)
        if (!strcmp(m->dir, dir))
            return m;
    return NULL;
}

void rl_save_set_fault(int step) { g_fault = step; }

int rl_save_open(const char *dir, int mode, rl_save_mount **out)
{
    rl_save_mount *m;
    const char *slash = strrchr(dir, '/');
    char name[NAME_MAX + 1];
    int e = 0;
    if (!slash || !slash[1] || strlen(slash + 1) > NAME_MAX)
        return EINVAL;
    m = calloc(1, sizeof *m);
    if (!m)
        return ENOMEM;
    snprintf(m->parent, sizeof m->parent, "%.*s", slash == dir ? 1 : (int)(slash - dir), dir);
    snprintf(name, sizeof name, "%s", slash + 1);
    if (snprintf(m->dir, sizeof m->dir, "%s", dir) >= (int)sizeof m->dir || sibling(m->work, m->parent, name, "work") ||
        sibling(m->stage, m->parent, name, "new") || sibling(m->old, m->parent, name, "old") ||
        sibling(m->marker, m->parent, name, "commit")) {
        free(m);
        return ENAMETOOLONG;
    }
    m->direct = mode != RL_SAVE_JOURNALED;
    m->read_only = mode == RL_SAVE_READONLY;
    pthread_mutex_lock(&g_lock);
    if (mode == RL_SAVE_JOURNALED && find_active(m->dir)) {
        e = EBUSY;
    } else if (mode != RL_SAVE_DIRECT && !find_active(m->dir)) {
        e = recover_dir(m);
        if (!e)
            e = remove_tree(m->work);
    }
    if (!e && !exists(m->dir) && mkdir(m->dir, 0777) != 0)
        e = errno;
    if (!e && mode == RL_SAVE_JOURNALED) {
        e = copy_tree(m->dir, m->work, 0);
        if (e)
            remove_tree(m->work);
    }
    if (!e && mode == RL_SAVE_JOURNALED) {
        m->next = g_active;
        g_active = m;
    }
    pthread_mutex_unlock(&g_lock);
    if (e) {
        free(m);
        return e;
    }
    *out = m;
    return 0;
}

int rl_save_read_only(const rl_save_mount *m) { return m->read_only; }

const char *rl_save_root(const rl_save_mount *m) { return m->direct ? m->dir : m->work; }

static int fault_at(rl_save_mount *m, int step)
{
    if (g_fault != step)
        return 0;
    g_fault = 0;
    m->crashed = 1;
    return EIO;
}

int rl_save_commit(rl_save_mount *m)
{
    int e;
    if (m->direct)
        return 0;
    pthread_mutex_lock(&g_lock);
    if (m->crashed) {
        pthread_mutex_unlock(&g_lock);
        return EIO;
    }
    e = remove_tree(m->stage);
    if (!e)
        e = copy_tree(m->work, m->stage, 1);
    if (!e)
        e = fault_at(m, 1);
    if (e) {
        if (!m->crashed)
            remove_tree(m->stage);
        pthread_mutex_unlock(&g_lock);
        return e;
    }
    {
        int fd = open(m->marker, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0666);
        if (fd < 0) {
            e = errno;
        } else {
            e = sync_fd(fd);
            close(fd);
        }
    }
    if (!e)
        e = sync_dir(m->parent);
    if (e) {
        unlink(m->marker);
        remove_tree(m->stage);
        pthread_mutex_unlock(&g_lock);
        return e;
    }
    e = fault_at(m, 2);
    if (!e && rename(m->dir, m->old) != 0)
        e = errno;
    if (!e)
        e = fault_at(m, 3);
    if (!e && rename(m->stage, m->dir) != 0)
        e = errno;
    if (!e)
        e = sync_dir(m->parent);
    if (!e)
        e = fault_at(m, 4);
    if (!e && unlink(m->marker) != 0)
        e = errno;
    if (!e)
        e = sync_dir(m->parent);
    if (!e)
        e = fault_at(m, 5);
    if (!e)
        e = remove_tree(m->old);
    if (e && !m->crashed)
        e = recover_dir(m) ? e : 0;
    pthread_mutex_unlock(&g_lock);
    return e;
}

void rl_save_close(rl_save_mount *m)
{
    pthread_mutex_lock(&g_lock);
    for (rl_save_mount **p = &g_active; *p; p = &(*p)->next)
        if (*p == m) {
            *p = m->next;
            break;
        }
    if (!m->direct && !m->crashed)
        remove_tree(m->work);
    pthread_mutex_unlock(&g_lock);
    free(m);
}
