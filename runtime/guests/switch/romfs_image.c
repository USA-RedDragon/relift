/**
 * @file romfs_image.c
 * @brief The synthesized RomFS image: see romfs_image.h.
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

#include "hostio.h"

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
    uint32_t meta;
    uint64_t data;
    uint64_t size;
    char *name;
} img_file;

struct rl_romfs_image {
    char *root;
    img_dir *dirs;
    uint32_t ndirs;
    img_file *files;
    uint32_t nfiles;
    uint8_t *meta;
    uint64_t data_off;
    uint64_t size;
};

typedef struct scan_ent {
    char *name;
    bool dir;
    uint64_t size;
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

static bool dir_path(const rl_romfs_image *img, uint32_t d, char *buf, size_t n)
{
    size_t used;
    int w;
    if (d == 0) {
        w = snprintf(buf, n, "%s", img->root);
        return w >= 0 && (size_t)w < n;
    }
    if (!dir_path(img, img->dirs[d].parent, buf, n))
        return false;
    used = strlen(buf);
    w = snprintf(buf + used, n - used, "/%s", img->dirs[d].name);
    return w >= 0 && (size_t)w < n - used;
}

static bool file_path(const rl_romfs_image *img, uint32_t f, char *buf, size_t n)
{
    size_t used;
    int w;
    if (!dir_path(img, img->files[f].parent, buf, n))
        return false;
    used = strlen(buf);
    w = snprintf(buf + used, n - used, "/%s", img->files[f].name);
    return w >= 0 && (size_t)w < n - used;
}

static int ent_cmp(const void *a, const void *b)
{
    return strcmp(((const scan_ent *)a)->name, ((const scan_ent *)b)->name);
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

static bool push_file(rl_romfs_image *img, uint32_t *cap, uint32_t parent, char *name, uint64_t size)
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
    img->files[img->nfiles].meta = 0;
    img->files[img->nfiles].data = 0;
    img->files[img->nfiles].size = size;
    img->files[img->nfiles].name = name;
    img->nfiles++;
    return true;
}

static int read_dir(const rl_romfs_image *img, uint32_t d, scan_ent **out, size_t *count)
{
    char path[PATH_MAX];
    DIR *dh;
    struct dirent *de;
    scan_ent *e = NULL;
    size_t n = 0, cap = 0;
    if (!dir_path(img, d, path, sizeof path))
        return ENAMETOOLONG;
    dh = opendir(path);
    if (!dh)
        return errno;
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
        if (n == cap) {
            scan_ent *g;
            cap = cap ? cap * 2 : 16;
            g = realloc(e, cap * sizeof *g);
            if (!g) {
                closedir(dh);
                ents_free(e, n);
                return ENOMEM;
            }
            e = g;
        }
        e[n].name = malloc(len + 1);
        if (!e[n].name) {
            closedir(dh);
            ents_free(e, n);
            return ENOMEM;
        }
        memcpy(e[n].name, de->d_name, len + 1);
        e[n].dir = S_ISDIR(st.st_mode);
        e[n].size = e[n].dir ? 0 : (uint64_t)st.st_size;
        n++;
    }
    closedir(dh);
    if (n > 1)
        qsort(e, n, sizeof *e, ent_cmp);
    *out = e;
    *count = n;
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
                               : push_file(img, &fcap, d, e[i].name, e[i].size);
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

rl_romfs_image *rl_romfs_image_open(const char *root)
{
    rl_romfs_image *img = calloc(1, sizeof *img);
    struct stat st;
    int rc;
    if (!img)
        return NULL;
    if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode)) {
        int e = errno ? errno : ENOTDIR;
        free(img);
        errno = e;
        return NULL;
    }
    img->root = strdup(root);
    if (!img->root) {
        free(img);
        errno = ENOMEM;
        return NULL;
    }
    {
        size_t n = strlen(img->root);
        while (n > 1 && img->root[n - 1] == '/')
            img->root[--n] = 0;
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
    free(img->root);
    free(img);
}

uint64_t rl_romfs_image_size(const rl_romfs_image *img) { return img->size; }

uint32_t rl_romfs_image_file_count(const rl_romfs_image *img) { return img->nfiles; }

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
