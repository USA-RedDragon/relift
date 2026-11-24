/**
 * @file romfs_image.c
 * @brief RomFS images from extracted trees, image files and RomFS maps: see
 * romfs_image.h.
 *
 * The format (header fields, entry layouts and the name hash) follows
 * switchbrew.org (RomFS) and LibHac's and Ryujinx's readers, read as
 * specifications.
 */
#include "romfs_image.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "hostio_win.h"

#define NONE 0xFFFFFFFFu
#define HEADER_SIZE 0x50u
#define DATA_ALIGN 0x200u
#define FILE_ALIGN 0x10u
#define DIR_REC 0x18u
#define FILE_REC 0x20u

typedef struct img_dir {
    uint32_t parent;
    uint32_t first_dir, ndirs, first_file, nfiles;
    uint32_t meta;
    char *name;
} img_dir;

typedef struct img_file {
    uint32_t parent;
    uint32_t layer;
    uint32_t meta;
    uint64_t data;
    uint64_t size;
    uint64_t src;
    char *name;
} img_file;

typedef struct img_piece {
    uint64_t off;
    uint64_t size;
    uint64_t at;
    uint32_t src;
} img_piece;

/** @brief An image kept in host files: a plain image file, or a RomFS map with the files it reads. */
typedef struct img_store {
    int *fds;
    uint32_t nfds;
    uint64_t base;
    uint8_t *meta;
    uint64_t meta_size;
    img_piece *pieces;
    uint64_t npieces;
    uint64_t size;
} img_store;

typedef struct img_layer {
    char *name;
    char *root;
    bool base;
    img_store *store;
    uint8_t *dt;
    uint64_t dt_size;
    uint8_t *ft;
    uint64_t ft_size;
    uint64_t data_off;
} img_layer;

struct rl_romfs_image {
    img_layer *layers;
    uint32_t nlayers;
    rl_romfs_conflict_fn warn;
    void *warn_user;
    img_dir *dirs;
    uint32_t ndirs;
    img_file *files;
    uint32_t nfiles;
    uint8_t *meta;
    uint64_t data_off;
    uint64_t size;
    img_store *pass;
    uint32_t pass_files;
};

typedef struct scan_ent {
    char *name;
    uint32_t layer;
    bool dir;
    uint64_t size;
    uint64_t src;
} scan_ent;

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void put64(uint8_t *p, uint64_t v)
{
    put32(p, (uint32_t)v);
    put32(p + 4, (uint32_t)(v >> 32));
}

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint64_t get64(const uint8_t *p) { return (uint64_t)get32(p) | (uint64_t)get32(p + 4) << 32; }

static int pread_all(int fd, void *dst, uint64_t n, uint64_t at)
{
    uint8_t *out = dst;
    uint64_t done = 0;
    while (done < n) {
        size_t want = n - done > 0x40000000u ? 0x40000000u : (size_t)(n - done);
        ssize_t k = pread(fd, out + done, want, (off_t)(at + done));
        if (k < 0)
            return errno;
        if (k == 0)
            return EIO;
        done += (uint64_t)k;
    }
    return 0;
}

static uint64_t fd_size(int fd)
{
    struct stat st;
    return fstat(fd, &st) == 0 ? (uint64_t)st.st_size : 0;
}

static void store_close(img_store *s)
{
    if (!s)
        return;
    for (uint32_t i = 0; i < s->nfds; i++)
        if (s->fds[i] >= 0)
            close(s->fds[i]);
    free(s->fds);
    free(s->meta);
    free(s->pieces);
    free(s);
}

static bool is_absolute(const char *p)
{
    return p[0] == '/' || p[0] == '\\' || (p[0] && p[1] == ':');
}

static _Thread_local char g_missing[PATH_MAX];

const char *rl_romfs_image_missing_source(void) { return g_missing; }

static int map_source(img_store *s, const char *map_path, const uint8_t *rec, const uint8_t *path, uint32_t len,
                      uint32_t i)
{
    char full[PATH_MAX];
    const char *slash = strrchr(map_path, '/');
    size_t dir_len = slash ? (size_t)(slash - map_path) : 0;
    uint8_t check[32];
    int rc;
    if (memchr(path, 0, len))
        return EINVAL;
    if (is_absolute((const char *)path) || !slash) {
        if (len >= sizeof full)
            return ENAMETOOLONG;
        memcpy(full, path, len);
        full[len] = 0;
    } else {
        if (dir_len + 1 + len >= sizeof full)
            return ENAMETOOLONG;
        memcpy(full, map_path, dir_len);
        full[dir_len] = '/';
        memcpy(full + dir_len + 1, path, len);
        full[dir_len + 1 + len] = 0;
    }
    s->fds[i] = open(full, O_RDONLY | O_CLOEXEC);
    if (s->fds[i] < 0) {
        if (errno != ENOENT)
            return errno;
        memcpy(g_missing, full, strlen(full) + 1);
        return ENOLINK;
    }
    if (fd_size(s->fds[i]) != get64(rec))
        return ESTALE;
    rc = pread_all(s->fds[i], check, sizeof check, get64(rec + 8));
    if (rc)
        return rc;
    return memcmp(check, rec + 0x10, sizeof check) ? ESTALE : 0;
}

static int map_parse(img_store *s, const char *path, const uint8_t *b, uint64_t n)
{
    uint64_t nsrc, src_at, pieces_at, meta_at, end = 0x40;
    if (n < 0x40)
        return EINVAL;
    nsrc = get32(b + 8);
    s->size = get64(b + 0x10);
    s->meta_size = get64(b + 0x18);
    s->npieces = get64(b + 0x20);
    src_at = get64(b + 0x28);
    pieces_at = get64(b + 0x30);
    meta_at = get64(b + 0x38);
    if (nsrc == 0 || nsrc > 64 || s->meta_size > s->size || meta_at > n || s->meta_size > n - meta_at ||
        pieces_at > n || s->npieces > (n - pieces_at) / 0x20)
        return EINVAL;
    s->fds = malloc(nsrc * sizeof *s->fds);
    s->meta = malloc(s->meta_size ? (size_t)s->meta_size : 1);
    s->pieces = malloc(s->npieces ? (size_t)s->npieces * sizeof *s->pieces : 1);
    if (!s->fds || !s->meta || !s->pieces)
        return ENOMEM;
    for (uint32_t i = 0; i < nsrc; i++)
        s->fds[i] = -1;
    s->nfds = (uint32_t)nsrc;
    memcpy(s->meta, b + meta_at, (size_t)s->meta_size);
    for (uint32_t i = 0; i < nsrc; i++) {
        uint32_t len;
        int rc;
        if (src_at > n || n - src_at < 0x40)
            return EINVAL;
        len = get32(b + src_at + 0x30);
        if (len > n - src_at - 0x40)
            return EINVAL;
        rc = map_source(s, path, b + src_at, b + src_at + 0x40, len, i);
        if (rc)
            return rc;
        src_at += 0x40 + align_up(len, 8);
    }
    end = s->meta_size;
    uint64_t sizes[64];
    for (uint32_t i = 0; i < nsrc; i++)
        sizes[i] = fd_size(s->fds[i]);
    for (uint64_t i = 0; i < s->npieces; i++) {
        const uint8_t *r = b + pieces_at + i * 0x20;
        img_piece *p = &s->pieces[i];
        p->off = get64(r);
        p->size = get64(r + 8);
        p->src = get32(r + 0x10);
        p->at = get64(r + 0x18);
        if (p->src >= nsrc || p->off < end || p->size > s->size - p->off || p->off > s->size ||
            p->at > sizes[p->src] || p->size > sizes[p->src] - p->at)
            return EINVAL;
        end = p->off + p->size;
    }
    return 0;
}

static img_store *store_open(const char *path, uint64_t offset, uint64_t length, int *err)
{
    img_store *s = calloc(1, sizeof *s);
    uint8_t head[0x50];
    uint64_t total;
    int fd, rc;
    if (!s) {
        *err = ENOMEM;
        return NULL;
    }
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        *err = errno;
        free(s);
        return NULL;
    }
    total = fd_size(fd);
    if (offset > total || (length && length > total - offset) || total - offset < sizeof head) {
        close(fd);
        free(s);
        *err = EINVAL;
        return NULL;
    }
    rc = pread_all(fd, head, sizeof head, offset);
    if (!rc && !memcmp(head, "RLRFMAP1", 8) && offset == 0 && length == 0) {
        uint8_t *b = total < ((uint64_t)1 << 32) ? malloc((size_t)total) : NULL;
        rc = b ? pread_all(fd, b, total, 0) : ENOMEM;
        close(fd);
        if (!rc)
            rc = map_parse(s, path, b, total);
        free(b);
    } else if (!rc) {
        s->fds = malloc(sizeof *s->fds);
        if (!s->fds) {
            close(fd);
            rc = ENOMEM;
        } else {
            s->fds[0] = fd;
            s->nfds = 1;
            s->base = offset;
            s->size = length ? length : total - offset;
            if (get64(head) != HEADER_SIZE || get64(head + 0x48) > s->size)
                rc = EINVAL;
        }
    } else {
        close(fd);
    }
    if (rc) {
        store_close(s);
        *err = rc;
        return NULL;
    }
    return s;
}

static uint64_t first_piece_ending_after(const img_store *s, uint64_t off)
{
    uint64_t lo = 0, hi = s->npieces;
    while (lo < hi) {
        uint64_t mid = lo + (hi - lo) / 2;
        if (s->pieces[mid].off + s->pieces[mid].size > off)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

static int store_read(const img_store *s, uint64_t off, void *dst, uint64_t n)
{
    uint8_t *out = dst;
    if (off > s->size || n > s->size - off)
        return RL_ROMFS_RANGE;
    if (!s->pieces)
        return pread_all(s->fds[0], dst, n, s->base + off);
    while (n) {
        uint64_t take;
        if (off < s->meta_size) {
            take = s->meta_size - off < n ? s->meta_size - off : n;
            memcpy(out, s->meta + off, (size_t)take);
        } else {
            uint64_t i = first_piece_ending_after(s, off);
            if (i == s->npieces || off < s->pieces[i].off) {
                uint64_t gap = i == s->npieces ? n : s->pieces[i].off - off;
                take = gap < n ? gap : n;
                memset(out, 0, (size_t)take);
            } else {
                const img_piece *p = &s->pieces[i];
                uint64_t left = p->off + p->size - off;
                int rc;
                take = left < n ? left : n;
                rc = pread_all(s->fds[p->src], out, take, p->at + (off - p->off));
                if (rc)
                    return rc;
            }
        }
        out += take;
        off += take;
        n -= take;
    }
    return 0;
}

static bool is_file(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

static int layer_load_tables(img_layer *l)
{
    uint8_t h[0x50];
    uint64_t dm_off, ft_off;
    int rc = store_read(l->store, 0, h, sizeof h);
    if (rc)
        return rc == RL_ROMFS_RANGE ? EINVAL : rc;
    dm_off = get64(h + 0x18);
    l->dt_size = get64(h + 0x20);
    ft_off = get64(h + 0x38);
    l->ft_size = get64(h + 0x40);
    l->data_off = get64(h + 0x48);
    if (l->dt_size < DIR_REC || l->dt_size > INT32_MAX || l->ft_size > INT32_MAX)
        return EINVAL;
    l->dt = malloc((size_t)l->dt_size);
    l->ft = malloc(l->ft_size ? (size_t)l->ft_size : 1);
    if (!l->dt || !l->ft)
        return ENOMEM;
    rc = store_read(l->store, dm_off, l->dt, l->dt_size);
    if (!rc)
        rc = store_read(l->store, ft_off, l->ft, l->ft_size);
    return rc == RL_ROMFS_RANGE ? EINVAL : rc;
}

static uint32_t hash_count(uint32_t n)
{
    static const uint32_t small[] = { 2, 3, 5, 7, 11, 13, 17 };
    if (n < 3)
        return 3;
    if (n < 19)
        return n | 1;
    for (;;) {
        bool bad = false;
        for (uint32_t i = 0; i < sizeof small / sizeof small[0]; i++)
            bad = bad || n % small[i] == 0;
        if (!bad)
            return n;
        n++;
    }
}

static uint32_t name_hash(uint32_t parent, const char *name, size_t len, uint32_t count)
{
    uint32_t h = parent ^ 123456789u;
    for (size_t i = 0; i < len; i++)
        h = ((h >> 5) | (h << 27)) ^ (uint8_t)name[i];
    return h % count;
}

static bool rel_path(const rl_romfs_image *img, uint32_t d, char *buf, size_t n)
{
    size_t used;
    int w;
    if (d == 0) {
        buf[0] = 0;
        return n > 0;
    }
    if (!rel_path(img, img->dirs[d].parent, buf, n))
        return false;
    used = strlen(buf);
    w = snprintf(buf + used, n - used, "/%s", img->dirs[d].name);
    return w >= 0 && (size_t)w < n - used;
}

static bool dir_path(const rl_romfs_image *img, uint32_t layer, uint32_t d, char *buf, size_t n)
{
    size_t used;
    int w = snprintf(buf, n, "%s", img->layers[layer].root);
    if (w < 0 || (size_t)w >= n)
        return false;
    used = (size_t)w;
    return rel_path(img, d, buf + used, n - used);
}

static bool file_path(const rl_romfs_image *img, uint32_t f, char *buf, size_t n)
{
    size_t used;
    int w;
    if (!dir_path(img, img->files[f].layer, img->files[f].parent, buf, n))
        return false;
    used = strlen(buf);
    w = snprintf(buf + used, n - used, "/%s", img->files[f].name);
    return w >= 0 && (size_t)w < n - used;
}

static int ent_cmp(const void *a, const void *b)
{
    const scan_ent *x = a, *y = b;
    int c = strcmp(x->name, y->name);
    if (c)
        return c;
    return x->layer < y->layer ? -1 : x->layer > y->layer;
}

static void ents_free(scan_ent *e, size_t n)
{
    for (size_t i = 0; i < n; i++)
        free(e[i].name);
    free(e);
}

static bool push_dir(rl_romfs_image *img, uint32_t *cap, uint32_t parent, char *name)
{
    if (img->ndirs == *cap) {
        img_dir *g;
        *cap = *cap ? *cap * 2 : 64;
        g = realloc(img->dirs, (size_t)*cap * sizeof *g);
        if (!g)
            return false;
        img->dirs = g;
    }
    memset(&img->dirs[img->ndirs], 0, sizeof img->dirs[0]);
    img->dirs[img->ndirs].parent = parent;
    img->dirs[img->ndirs].name = name;
    img->ndirs++;
    return true;
}

static bool push_file(rl_romfs_image *img, uint32_t *cap, uint32_t parent, const scan_ent *e)
{
    if (img->nfiles == *cap) {
        img_file *g;
        *cap = *cap ? *cap * 2 : 256;
        g = realloc(img->files, (size_t)*cap * sizeof *g);
        if (!g)
            return false;
        img->files = g;
    }
    img->files[img->nfiles].parent = parent;
    img->files[img->nfiles].layer = e->layer;
    img->files[img->nfiles].meta = 0;
    img->files[img->nfiles].data = 0;
    img->files[img->nfiles].size = e->size;
    img->files[img->nfiles].src = e->src;
    img->files[img->nfiles].name = e->name;
    img->nfiles++;
    return true;
}

static const uint8_t *table_rec(const uint8_t *t, uint64_t size, uint32_t off, uint32_t fixed)
{
    uint32_t len;
    if (off == NONE || off > size || size - off < fixed)
        return NULL;
    len = get32(t + off + fixed - 4);
    return len <= size - off - fixed ? t + off : NULL;
}

static uint32_t image_dir(const rl_romfs_image *img, const img_layer *l, uint32_t d)
{
    uint32_t parent, child;
    const char *name = img->dirs[d].name;
    size_t len = strlen(name);
    uint32_t guard = 0;
    if (d == 0)
        return 0;
    parent = image_dir(img, l, img->dirs[d].parent);
    if (parent == NONE)
        return NONE;
    child = get32(l->dt + parent + 8);
    while (child != NONE && guard++ < l->dt_size / DIR_REC) {
        const uint8_t *r = table_rec(l->dt, l->dt_size, child, DIR_REC);
        if (!r)
            return NONE;
        if (get32(r + 0x14) == len && !memcmp(r + DIR_REC, name, len))
            return child;
        child = get32(r + 4);
    }
    return NONE;
}

static int grow_ents(scan_ent **e, size_t n, size_t *cap)
{
    scan_ent *g;
    if (n < *cap)
        return 0;
    *cap = *cap ? *cap * 2 : 16;
    g = realloc(*e, *cap * sizeof *g);
    if (!g)
        return ENOMEM;
    *e = g;
    return 0;
}

static int add_ent(scan_ent **e, size_t *n, size_t *cap, const uint8_t *name, size_t len, uint32_t layer, bool dir,
                   uint64_t size, uint64_t src)
{
    scan_ent *x;
    if (grow_ents(e, *n, cap))
        return ENOMEM;
    if (memchr(name, 0, len) || memchr(name, '/', len) || len == 0)
        return EINVAL;
    x = &(*e)[*n];
    x->name = malloc(len + 1);
    if (!x->name)
        return ENOMEM;
    memcpy(x->name, name, len);
    x->name[len] = 0;
    x->layer = layer;
    x->dir = dir;
    x->size = size;
    x->src = src;
    (*n)++;
    return 0;
}

static int read_image_dir(const rl_romfs_image *img, uint32_t layer, uint32_t d, scan_ent **out, size_t *count,
                          size_t *cap)
{
    const img_layer *l = &img->layers[layer];
    uint32_t at = image_dir(img, l, d), child, guard = 0;
    const uint8_t *r;
    if (at == NONE)
        return 0;
    r = l->dt + at;
    child = get32(r + 8);
    while (child != NONE) {
        const uint8_t *c = table_rec(l->dt, l->dt_size, child, DIR_REC);
        int rc;
        if (!c || guard++ > l->dt_size / DIR_REC)
            return EINVAL;
        rc = add_ent(out, count, cap, c + DIR_REC, get32(c + 0x14), layer, true, 0, 0);
        if (rc)
            return rc;
        child = get32(c + 4);
    }
    child = get32(r + 0xC);
    guard = 0;
    while (child != NONE) {
        const uint8_t *f = table_rec(l->ft, l->ft_size, child, FILE_REC);
        uint64_t off, size;
        int rc;
        if (!f || guard++ > l->ft_size / FILE_REC)
            return EINVAL;
        off = get64(f + 8);
        size = get64(f + 0x10);
        if (off > l->store->size || size > l->store->size - l->data_off - off)
            return EINVAL;
        rc = add_ent(out, count, cap, f + FILE_REC, get32(f + 0x1C), layer, false, size, l->data_off + off);
        if (rc)
            return rc;
        child = get32(f + 4);
    }
    return 0;
}

static int read_layer_dir(const rl_romfs_image *img, uint32_t layer, uint32_t d, scan_ent **out, size_t *count,
                          size_t *cap)
{
    char path[PATH_MAX];
    DIR *dh;
    struct dirent *de;
    scan_ent *e = *out;
    size_t n = *count;
    if (img->layers[layer].store)
        return read_image_dir(img, layer, d, out, count, cap);
    if (!dir_path(img, layer, d, path, sizeof path))
        return ENAMETOOLONG;
    dh = opendir(path);
    if (!dh)
        return img->nlayers > 1 && (errno == ENOENT || errno == ENOTDIR) ? 0 : errno;
    while ((de = readdir(dh)) != NULL) {
        char child[PATH_MAX];
        struct stat st;
        size_t len = strlen(de->d_name);
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;
        if (snprintf(child, sizeof child, "%s/%s", path, de->d_name) >= (int)sizeof child || stat(child, &st) != 0)
            continue;
        if (!S_ISDIR(st.st_mode) && !S_ISREG(st.st_mode))
            continue;
        if (n == *cap) {
            scan_ent *g;
            *cap = *cap ? *cap * 2 : 16;
            g = realloc(e, *cap * sizeof *g);
            if (!g) {
                closedir(dh);
                *out = e;
                *count = n;
                return ENOMEM;
            }
            e = g;
        }
        e[n].name = malloc(len + 1);
        if (!e[n].name) {
            closedir(dh);
            *out = e;
            *count = n;
            return ENOMEM;
        }
        memcpy(e[n].name, de->d_name, len + 1);
        e[n].layer = layer;
        e[n].dir = S_ISDIR(st.st_mode);
        e[n].size = e[n].dir ? 0 : (uint64_t)st.st_size;
        e[n].src = 0;
        n++;
    }
    closedir(dh);
    *out = e;
    *count = n;
    return 0;
}

static void report_conflict(const rl_romfs_image *img, uint32_t d, const scan_ent *e, size_t group_start,
                            size_t winner)
{
    char rel[PATH_MAX];
    char full[PATH_MAX];
    if (!img->warn || !rel_path(img, d, rel, sizeof rel))
        return;
    if (snprintf(full, sizeof full, "%s/%s", rel, e[winner].name) >= (int)sizeof full)
        return;
    for (size_t i = group_start; i < winner; i++)
        if (!img->layers[e[i].layer].base && !(e[i].dir && e[winner].dir))
            img->warn(img->warn_user, full, img->layers[e[winner].layer].name, img->layers[e[i].layer].name);
}

static int read_dir(const rl_romfs_image *img, uint32_t d, scan_ent **out, size_t *count)
{
    scan_ent *e = NULL, *m = NULL;
    size_t n = 0, cap = 0, k = 0;
    for (uint32_t l = 0; l < img->nlayers; l++) {
        int rc = read_layer_dir(img, l, d, &e, &n, &cap);
        if (rc) {
            ents_free(e, n);
            return rc;
        }
    }
    if (n > 1)
        qsort(e, n, sizeof *e, ent_cmp);
    m = n ? malloc(n * sizeof *m) : NULL;
    if (n && !m) {
        ents_free(e, n);
        return ENOMEM;
    }
    for (size_t i = 0; i < n;) {
        size_t j = i;
        while (j + 1 < n && !strcmp(e[j + 1].name, e[i].name))
            j++;
        report_conflict(img, d, e, i, j);
        m[k++] = e[j];
        for (size_t t = i; t < j; t++)
            free(e[t].name);
        i = j + 1;
    }
    free(e);
    *out = m;
    *count = k;
    return 0;
}

static int scan(rl_romfs_image *img)
{
    uint32_t dcap = 0, fcap = 0;
    char *root_name = strdup("");
    if (!root_name || !push_dir(img, &dcap, 0, root_name)) {
        free(root_name);
        return ENOMEM;
    }
    for (uint32_t d = 0; d < img->ndirs; d++) {
        scan_ent *e = NULL;
        size_t n = 0;
        int rc = read_dir(img, d, &e, &n);
        if (rc)
            return rc;
        img->dirs[d].first_dir = img->ndirs;
        img->dirs[d].first_file = img->nfiles;
        for (size_t i = 0; i < n; i++) {
            bool ok = e[i].dir ? push_dir(img, &dcap, d, e[i].name)
                               : push_file(img, &fcap, d, &e[i]);
            if (!ok) {
                ents_free(e, n);
                return ENOMEM;
            }
            e[i].name = NULL;
            if (e[i].dir)
                img->dirs[d].ndirs++;
            else
                img->dirs[d].nfiles++;
        }
        ents_free(e, n);
    }
    return 0;
}

static int layout(rl_romfs_image *img)
{
    uint64_t dir_meta = 0, file_meta = 0, cur = 0;
    uint32_t dcount = hash_count(img->ndirs), fcount = hash_count(img->nfiles);
    uint64_t dh_off, dm_off, fh_off, fm_off;
    uint8_t *m;
    for (uint32_t i = 0; i < img->ndirs; i++) {
        img->dirs[i].meta = (uint32_t)dir_meta;
        dir_meta += DIR_REC + align_up(strlen(img->dirs[i].name), 4);
        if (dir_meta > INT32_MAX)
            return EFBIG;
    }
    for (uint32_t i = 0; i < img->nfiles; i++) {
        img->files[i].meta = (uint32_t)file_meta;
        file_meta += FILE_REC + align_up(strlen(img->files[i].name), 4);
        cur = align_up(cur, FILE_ALIGN);
        img->files[i].data = cur;
        cur += img->files[i].size;
        if (file_meta > INT32_MAX)
            return EFBIG;
    }
    dh_off = HEADER_SIZE;
    dm_off = dh_off + 4ull * dcount;
    fh_off = dm_off + dir_meta;
    fm_off = fh_off + 4ull * fcount;
    img->data_off = align_up(fm_off + file_meta, DATA_ALIGN);
    img->size = img->data_off + cur;
    m = calloc(1, (size_t)img->data_off);
    if (!m)
        return ENOMEM;
    put64(m + 0x00, HEADER_SIZE);
    put64(m + 0x08, dh_off);
    put64(m + 0x10, 4ull * dcount);
    put64(m + 0x18, dm_off);
    put64(m + 0x20, dir_meta);
    put64(m + 0x28, fh_off);
    put64(m + 0x30, 4ull * fcount);
    put64(m + 0x38, fm_off);
    put64(m + 0x40, file_meta);
    put64(m + 0x48, img->data_off);
    memset(m + dh_off, 0xFF, 4ull * dcount);
    memset(m + fh_off, 0xFF, 4ull * fcount);
    for (uint32_t i = 0; i < img->ndirs; i++) {
        const img_dir *d = &img->dirs[i];
        const img_dir *p = &img->dirs[d->parent];
        uint8_t *rec = m + dm_off + d->meta;
        size_t len = strlen(d->name);
        uint32_t sib = NONE, bucket;
        if (i != 0 && i + 1 < p->first_dir + p->ndirs)
            sib = img->dirs[i + 1].meta;
        bucket = name_hash(i == 0 ? 0 : p->meta, d->name, len, dcount);
        put32(rec + 0x00, i == 0 ? 0 : p->meta);
        put32(rec + 0x04, sib);
        put32(rec + 0x08, d->ndirs ? img->dirs[d->first_dir].meta : NONE);
        put32(rec + 0x0C, d->nfiles ? img->files[d->first_file].meta : NONE);
        put32(rec + 0x14, (uint32_t)len);
        memcpy(rec + DIR_REC, d->name, len);
        {
            uint8_t *slot = m + dh_off + 4ull * bucket;
            uint32_t prev = (uint32_t)slot[0] | (uint32_t)slot[1] << 8 | (uint32_t)slot[2] << 16 |
                            (uint32_t)slot[3] << 24;
            put32(rec + 0x10, prev);
            put32(slot, d->meta);
        }
    }
    for (uint32_t i = 0; i < img->nfiles; i++) {
        const img_file *f = &img->files[i];
        const img_dir *p = &img->dirs[f->parent];
        uint8_t *rec = m + fm_off + f->meta;
        size_t len = strlen(f->name);
        uint32_t sib = NONE;
        uint8_t *slot;
        uint32_t prev;
        if (i + 1 < p->first_file + p->nfiles)
            sib = img->files[i + 1].meta;
        slot = m + fh_off + 4ull * name_hash(p->meta, f->name, len, fcount);
        prev = (uint32_t)slot[0] | (uint32_t)slot[1] << 8 | (uint32_t)slot[2] << 16 | (uint32_t)slot[3] << 24;
        put32(rec + 0x00, p->meta);
        put32(rec + 0x04, sib);
        put64(rec + 0x08, f->data);
        put64(rec + 0x10, f->size);
        put32(rec + 0x18, prev);
        put32(rec + 0x1C, (uint32_t)len);
        memcpy(rec + FILE_REC, f->name, len);
        put32(slot, f->meta);
    }
    img->meta = m;
    return 0;
}

static void layers_free(img_layer *l, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        free(l[i].name);
        free(l[i].root);
        store_close(l[i].store);
        free(l[i].dt);
        free(l[i].ft);
    }
    free(l);
}

static uint32_t count_files(const img_store *s)
{
    uint8_t h[0x50];
    uint8_t *ft;
    uint64_t size, at = 0;
    uint32_t n = 0;
    if (store_read(s, 0, h, sizeof h))
        return 0;
    size = get64(h + 0x40);
    ft = size && size <= INT32_MAX ? malloc((size_t)size) : NULL;
    if (!ft || store_read(s, get64(h + 0x38), ft, size)) {
        free(ft);
        return 0;
    }
    while (at + FILE_REC <= size) {
        at += FILE_REC + align_up(get32(ft + at + 0x1C), 4);
        n++;
    }
    free(ft);
    return n;
}

rl_romfs_image *rl_romfs_image_open_file(const char *path, uint64_t offset, uint64_t length)
{
    int err = 0;
    rl_romfs_image *img = calloc(1, sizeof *img);
    if (!img) {
        errno = ENOMEM;
        return NULL;
    }
    img->pass = store_open(path, offset, length, &err);
    if (!img->pass) {
        free(img);
        errno = err;
        return NULL;
    }
    img->size = img->pass->size;
    img->pass_files = count_files(img->pass);
    return img;
}

rl_romfs_image *rl_romfs_image_open_layers(const rl_romfs_layer *layers, size_t n, rl_romfs_conflict_fn warn,
                                           void *user)
{
    rl_romfs_image *img;
    int rc;
    if (n == 0 || n > UINT32_MAX) {
        errno = EINVAL;
        return NULL;
    }
    img = calloc(1, sizeof *img);
    if (!img)
        return NULL;
    img->layers = calloc(n, sizeof *img->layers);
    if (!img->layers) {
        free(img);
        errno = ENOMEM;
        return NULL;
    }
    if (n == 1 && is_file(layers[0].root)) {
        free(img->layers);
        free(img);
        return rl_romfs_image_open_file(layers[0].root, 0, 0);
    }
    img->warn = warn;
    img->warn_user = user;
    for (size_t i = 0; i < n; i++) {
        struct stat st;
        size_t len;
        if (stat(layers[i].root, &st) == 0 && S_ISREG(st.st_mode)) {
            int err = 0;
            img->layers[i].store = store_open(layers[i].root, 0, 0, &err);
            img->nlayers = (uint32_t)i + 1;
            if (!img->layers[i].store || (err = layer_load_tables(&img->layers[i])) != 0) {
                rl_romfs_image_close(img);
                errno = err ? err : EINVAL;
                return NULL;
            }
        } else if (stat(layers[i].root, &st) != 0 || !S_ISDIR(st.st_mode)) {
            int e = errno ? errno : ENOTDIR;
            rl_romfs_image_close(img);
            errno = e;
            return NULL;
        }
        img->layers[i].root = strdup(layers[i].root);
        img->layers[i].name = strdup(layers[i].name ? layers[i].name : layers[i].root);
        img->layers[i].base = layers[i].base;
        img->nlayers = (uint32_t)i + 1;
        if (!img->layers[i].root || !img->layers[i].name) {
            rl_romfs_image_close(img);
            errno = ENOMEM;
            return NULL;
        }
        len = strlen(img->layers[i].root);
        while (len > 1 && img->layers[i].root[len - 1] == '/')
            img->layers[i].root[--len] = 0;
    }
    rc = scan(img);
    if (!rc)
        rc = layout(img);
    if (rc) {
        rl_romfs_image_close(img);
        errno = rc;
        return NULL;
    }
    return img;
}

rl_romfs_image *rl_romfs_image_open(const char *root)
{
    rl_romfs_layer l = { root, root, true };
    return rl_romfs_image_open_layers(&l, 1, NULL, NULL);
}

void rl_romfs_image_close(rl_romfs_image *img)
{
    if (!img)
        return;
    for (uint32_t i = 0; i < img->ndirs; i++)
        free(img->dirs[i].name);
    for (uint32_t i = 0; i < img->nfiles; i++)
        free(img->files[i].name);
    free(img->dirs);
    free(img->files);
    free(img->meta);
    layers_free(img->layers, img->nlayers);
    store_close(img->pass);
    free(img);
}

uint64_t rl_romfs_image_size(const rl_romfs_image *img) { return img->size; }

uint32_t rl_romfs_image_file_count(const rl_romfs_image *img) { return img->pass ? img->pass_files : img->nfiles; }

static uint32_t first_ending_after(const rl_romfs_image *img, uint64_t rel)
{
    uint32_t lo = 0, hi = img->nfiles;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (img->files[mid].data + img->files[mid].size > rel)
            hi = mid;
        else
            lo = mid + 1;
    }
    return lo;
}

static int read_host(const rl_romfs_image *img, uint32_t f, uint64_t at, uint8_t *dst, uint64_t n)
{
    char path[PATH_MAX];
    int fd;
    uint64_t done = 0;
    const img_store *store = img->layers[img->files[f].layer].store;
    if (store) {
        int rc = store_read(store, img->files[f].src + at, dst, n);
        return rc == RL_ROMFS_RANGE ? EIO : rc;
    }
    if (!file_path(img, f, path, sizeof path))
        return ENAMETOOLONG;
    fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return errno;
    while (done < n) {
        ssize_t k = pread(fd, dst + done, (size_t)(n - done), (off_t)(at + done));
        if (k < 0) {
            int e = errno;
            close(fd);
            return e;
        }
        if (k == 0) {
            close(fd);
            return EIO;
        }
        done += (uint64_t)k;
    }
    close(fd);
    return 0;
}

int rl_romfs_image_read(const rl_romfs_image *img, uint64_t off, void *dst, uint64_t n)
{
    uint8_t *out = dst;
    if (off > img->size || n > img->size - off)
        return RL_ROMFS_RANGE;
    if (img->pass)
        return store_read(img->pass, off, dst, n);
    while (n) {
        uint64_t take;
        if (off < img->data_off) {
            take = img->data_off - off < n ? img->data_off - off : n;
            memcpy(out, img->meta + off, (size_t)take);
        } else {
            uint64_t rel = off - img->data_off;
            uint32_t f = first_ending_after(img, rel);
            if (f == img->nfiles || rel < img->files[f].data) {
                uint64_t gap = f == img->nfiles ? n : img->files[f].data - rel;
                take = gap < n ? gap : n;
                memset(out, 0, (size_t)take);
            } else {
                uint64_t left = img->files[f].data + img->files[f].size - rel;
                int rc;
                take = left < n ? left : n;
                rc = read_host(img, f, rel - img->files[f].data, out, take);
                if (rc)
                    return rc;
            }
        }
        out += take;
        off += take;
        n -= take;
    }
    return 0;
}
