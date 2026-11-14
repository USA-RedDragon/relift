/**
 * @file test_fs_romfs.c
 * @brief fsp-srv data storages through IPC: the program's RomFS and add-on
 * content as read-only IStorage objects. The test reads the image the way
 * libnn's romfs driver does (header, hash tables, metadata, file data) from a
 * synthetic tree and compares every file with its source.
 */
#include "hos_fs.h"
#include "hos_mods.h"
#include "tc.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define NONE 0xFFFFFFFFu
#define BIG_SIZE 1100000u
#define FS_TARGET_NOT_FOUND RL_MAKE_RESULT(2, 1002)
#define FS_OUT_OF_RANGE RL_MAKE_RESULT(2, 3005)
#define FS_INVALID_OFFSET RL_MAKE_RESULT(2, 6061)
#define FS_INVALID_SIZE RL_MAKE_RESULT(2, 6062)
#define FS_WRITE_NOT_PERMITTED RL_MAKE_RESULT(2, 6203)
#define BUF_OFF 0x10000u
#define BUF_SIZE 0x120000u

typedef struct env {
    rl_ctx *c;
    uint64_t heap;
    tc_obj proxy;
    char tree[512];
} env;

typedef struct image {
    env *e;
    tc_obj st;
    uint64_t size;
    uint64_t hdr[10];
    uint8_t *dh, *dm, *fh, *fm;
} image;

static uint32_t rc_of(tc_res r) { return r.svc_rc ? r.svc_rc : r.result; }

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void tree_remove(const char *dir) { nftw(dir, remove_one, 16, FTW_DEPTH | FTW_PHYS); }

static void write_file(const char *dir, const char *rel, const void *data, size_t n)
{
    char path[1024];
    int fd;
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0 || (n && write(fd, data, n) != (ssize_t)n)) {
        fprintf(stderr, "  cannot write %s\n", path);
        exit(2);
    }
    close(fd);
}

static void make_dir(const char *dir, const char *rel)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", dir, rel);
    if (mkdir(path, 0777) != 0) {
        fprintf(stderr, "  cannot create %s\n", path);
        exit(2);
    }
}

static void make_root(char *out, size_t n)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(out, n, "%s/relift-romfs-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(out)) {
        fprintf(stderr, "  mkdtemp %s failed\n", out);
        exit(2);
    }
}

static void fill(uint8_t *p, size_t n, uint32_t seed)
{
    for (size_t i = 0; i < n; i++)
        p[i] = (uint8_t)(i * 131u + (i >> 8) * 7u + seed);
}

static void make_tree(const char *dir)
{
    uint8_t *big = malloc(BIG_SIZE);
    char name[64];
    fill(big, BIG_SIZE, 3);
    write_file(dir, "a.txt", "hello, romfs\n", 13);
    write_file(dir, "empty.bin", "", 0);
    write_file(dir, "big.bin", big, BIG_SIZE);
    write_file(dir, "Zeta", "z", 1);
    make_dir(dir, "sub");
    make_dir(dir, "emptydir");
    make_dir(dir, "sub/deep");
    write_file(dir, "sub/deep/x.txt", "deep file", 9);
    for (int i = 0; i < 40; i++) {
        snprintf(name, sizeof name, "sub/n%02d", i);
        fill(big, 100 + (size_t)i * 37, (uint32_t)i);
        write_file(dir, name, big, 100 + (size_t)i * 37);
    }
    free(big);
}

static env boot(void)
{
    env e;
    memset(&e, 0, sizeof e);
    e.c = t_boot(NULL);
    e.heap = t_heap(e.c, 0x400000);
    e.proxy = tc_open_service(e.c, e.heap, "fsp-srv");
    return e;
}

static uint8_t *buf_ptr(const env *e) { return t_mem() + e->heap + BUF_OFF; }

static uint32_t st_read(const env *e, tc_obj st, int64_t off, uint64_t n)
{
    tc_req q = tc_new(0);
    tc_put64(&q, (uint64_t)off);
    tc_put64(&q, n);
    q.nb = 1;
    q.b_addr[0] = e->heap + BUF_OFF;
    q.b_size[0] = n;
    return rc_of(tc_send(e->c, st, q));
}

static uint32_t open_cmd(const env *e, uint32_t cmd, const void *in, uint32_t n, tc_obj *out)
{
    tc_req q = tc_new(cmd);
    if (n)
        tc_put(&q, in, n);
    memset(out, 0, sizeof *out);
    return tc_sub(e->c, e->proxy, q, out);
}

static uint32_t open_data_id(const env *e, uint64_t id, tc_obj *out)
{
    uint64_t in[2] = { 5, id };
    return open_cmd(e, 202, in, 16, out);
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static uint8_t *read_table(const env *e, tc_obj st, uint64_t off, uint64_t n)
{
    uint8_t *t = malloc(n ? n : 1);
    if (n && (st_read(e, st, (int64_t)off, n) != 0 || !t))
        return NULL;
    if (n)
        memcpy(t, buf_ptr(e), n);
    return t;
}

static bool image_open(image *im, env *e, tc_obj st)
{
    uint8_t h[0x50];
    memset(im, 0, sizeof *im);
    im->e = e;
    im->st = st;
    {
        tc_res r = tc_send(e->c, st, tc_new(4));
        if (rc_of(r) != 0)
            return false;
        im->size = tc_u64(&r, 0);
    }
    if (st_read(e, st, 0, 0x50) != 0)
        return false;
    memcpy(h, buf_ptr(e), sizeof h);
    for (int i = 0; i < 10; i++)
        im->hdr[i] = rd64(h + 8 * i);
    im->dh = read_table(e, st, im->hdr[1], im->hdr[2]);
    im->dm = read_table(e, st, im->hdr[3], im->hdr[4]);
    im->fh = read_table(e, st, im->hdr[5], im->hdr[6]);
    im->fm = read_table(e, st, im->hdr[7], im->hdr[8]);
    return im->dh && im->dm && im->fh && im->fm;
}

static void image_close(image *im)
{
    free(im->dh);
    free(im->dm);
    free(im->fh);
    free(im->fm);
}

static uint32_t name_hash(uint32_t parent, const uint8_t *name, size_t len, uint32_t count)
{
    uint32_t h = parent ^ 123456789u;
    for (size_t i = 0; i < len; i++)
        h = ((h >> 5) | (h << 27)) ^ name[i];
    return h % count;
}

static uint32_t find_dir(const image *im, uint32_t parent, const char *name, size_t len)
{
    uint32_t count = (uint32_t)(im->hdr[2] / 4);
    uint32_t s = rd32(im->dh + 4 * name_hash(parent, (const uint8_t *)name, len, count));
    while (s != NONE) {
        const uint8_t *rec = im->dm + s;
        if (rd32(rec) == parent && rd32(rec + 0x14) == len && memcmp(rec + 0x18, name, len) == 0)
            return s;
        s = rd32(rec + 0x10);
    }
    return NONE;
}

static uint32_t find_file(const image *im, uint32_t parent, const char *name, size_t len)
{
    uint32_t count = (uint32_t)(im->hdr[6] / 4);
    uint32_t s = rd32(im->fh + 4 * name_hash(parent, (const uint8_t *)name, len, count));
    while (s != NONE) {
        const uint8_t *rec = im->fm + s;
        if (rd32(rec) == parent && rd32(rec + 0x1C) == len && memcmp(rec + 0x20, name, len) == 0)
            return s;
        s = rd32(rec + 0x18);
    }
    return NONE;
}

static uint32_t lookup_file(const image *im, const char *path)
{
    uint32_t dir = 0;
    const char *p = path;
    while (*p == '/')
        p++;
    for (;;) {
        const char *e = strchr(p, '/');
        if (!e)
            return find_file(im, dir, p, strlen(p));
        dir = find_dir(im, dir, p, (size_t)(e - p));
        if (dir == NONE)
            return NONE;
        p = e + 1;
    }
}

static bool read_image_file(const image *im, uint32_t meta, uint8_t *out, uint64_t size)
{
    uint64_t at = rd64(im->fm + meta + 8);
    uint64_t done = 0;
    while (done < size) {
        uint64_t n = size - done < 0x10000 ? size - done : 0x10000;
        if (st_read(im->e, im->st, (int64_t)(im->hdr[9] + at + done), n) != 0)
            return false;
        memcpy(out + done, buf_ptr(im->e), n);
        done += n;
    }
    return true;
}

static bool same_as_host(const image *im, const char *root, const char *rel)
{
    char path[1024];
    struct stat hs;
    uint32_t meta = lookup_file(im, rel);
    uint8_t *want, *got;
    bool ok;
    int fd;
    snprintf(path, sizeof path, "%s/%s", root, rel);
    if (meta == NONE || stat(path, &hs) != 0 || rd64(im->fm + meta + 0x10) != (uint64_t)hs.st_size)
        return false;
    want = malloc((size_t)hs.st_size + 1);
    got = malloc((size_t)hs.st_size + 1);
    fd = open(path, O_RDONLY);
    ok = fd >= 0 && want && got && read(fd, want, (size_t)hs.st_size) == hs.st_size &&
         read_image_file(im, meta, got, (uint64_t)hs.st_size) && memcmp(want, got, (size_t)hs.st_size) == 0;
    if (fd >= 0)
        close(fd);
    free(want);
    free(got);
    return ok;
}

typedef struct walk {
    const image *im;
    const char *root;
    uint32_t dirs, files, bad;
} walk;

static void walk_dir(walk *w, uint32_t dir, const char *rel)
{
    uint32_t f = rd32(w->im->dm + dir + 0x0C);
    uint32_t d = rd32(w->im->dm + dir + 0x08);
    w->dirs++;
    while (f != NONE) {
        char path[1024];
        const uint8_t *rec = w->im->fm + f;
        snprintf(path, sizeof path, "%s%.*s", rel, (int)rd32(rec + 0x1C), (const char *)rec + 0x20);
        w->files++;
        if (!same_as_host(w->im, w->root, path))
            w->bad++;
        f = rd32(rec + 4);
    }
    while (d != NONE) {
        char path[1024];
        const uint8_t *rec = w->im->dm + d;
        snprintf(path, sizeof path, "%s%.*s/", rel, (int)rd32(rec + 0x14), (const char *)rec + 0x18);
        walk_dir(w, d, path);
        d = rd32(rec + 4);
    }
}

TEST(romfs_current_process_image)
{
    env e = boot();
    tc_obj st;
    image im;
    walk w;
    char root[512];
    uint8_t *got;
    make_root(root, sizeof root);
    make_tree(root);
    rl_hos_fs_set_romfs(root);
    CHECK_EQ(open_cmd(&e, 200, NULL, 0, &st), 0);
    CHECK(image_open(&im, &e, st));
    CHECK_EQ(im.hdr[0], 0x50);
    CHECK_EQ(im.hdr[9] % 0x200, 0);
    CHECK(im.size > im.hdr[9] + BIG_SIZE);
    CHECK_EQ(im.hdr[1], 0x50);
    CHECK_EQ(im.hdr[3], im.hdr[1] + im.hdr[2]);
    CHECK_EQ(im.hdr[5], im.hdr[3] + im.hdr[4]);
    CHECK_EQ(im.hdr[7], im.hdr[5] + im.hdr[6]);
    CHECK(im.hdr[9] >= im.hdr[7] + im.hdr[8]);

    CHECK(same_as_host(&im, root, "a.txt"));
    CHECK(same_as_host(&im, root, "empty.bin"));
    CHECK(same_as_host(&im, root, "big.bin"));
    CHECK(same_as_host(&im, root, "Zeta"));
    CHECK(same_as_host(&im, root, "sub/deep/x.txt"));
    CHECK(same_as_host(&im, root, "sub/n00"));
    CHECK(same_as_host(&im, root, "sub/n39"));
    CHECK_EQ(lookup_file(&im, "missing.txt"), NONE);
    CHECK_EQ(lookup_file(&im, "sub/missing"), NONE);
    CHECK_EQ(lookup_file(&im, "nodir/a.txt"), NONE);
    CHECK_EQ(find_dir(&im, 0, "emptydir", 8) != NONE, 1);

    memset(&w, 0, sizeof w);
    w.im = &im;
    w.root = root;
    walk_dir(&w, 0, "");
    CHECK_EQ(w.dirs, 4);
    CHECK_EQ(w.files, 4 + 1 + 40);
    CHECK_EQ(w.bad, 0);

    got = malloc(BIG_SIZE);
    CHECK_EQ(st_read(&e, st, (int64_t)im.hdr[9], 0x100000), 0);
    CHECK_EQ(st_read(&e, st, (int64_t)im.size - 4, 4), 0);
    CHECK(read_image_file(&im, lookup_file(&im, "big.bin"), got, BIG_SIZE));
    {
        uint8_t *want = malloc(BIG_SIZE);
        fill(want, BIG_SIZE, 3);
        CHECK(memcmp(got, want, BIG_SIZE) == 0);
        free(want);
    }
    free(got);
    image_close(&im);
    tc_close(e.c, st.handle);
    tree_remove(root);
}

TEST(romfs_storage_bounds_and_writes)
{
    env e = boot();
    tc_obj st;
    char root[512];
    uint64_t size;
    make_root(root, sizeof root);
    make_tree(root);
    rl_hos_fs_set_romfs(root);
    CHECK_EQ(open_cmd(&e, 200, NULL, 0, &st), 0);
    {
        tc_res r = tc_send(e.c, st, tc_new(4));
        CHECK_EQ(rc_of(r), 0);
        size = tc_u64(&r, 0);
    }
    CHECK_EQ(st_read(&e, st, (int64_t)size - 16, 16), 0);
    CHECK_EQ(st_read(&e, st, (int64_t)size - 16, 17), FS_OUT_OF_RANGE);
    CHECK_EQ(st_read(&e, st, (int64_t)size, 1), FS_OUT_OF_RANGE);
    CHECK_EQ(st_read(&e, st, (int64_t)size, 0), 0);
    CHECK_EQ(st_read(&e, st, -1, 4), FS_INVALID_OFFSET);
    {
        tc_req q = tc_new(0);
        tc_put64(&q, 0);
        tc_put64(&q, 64);
        q.nb = 1;
        q.b_addr[0] = e.heap + BUF_OFF;
        q.b_size[0] = 32;
        CHECK_EQ(rc_of(tc_send(e.c, st, q)), FS_INVALID_SIZE);
    }
    {
        tc_req q = tc_new(1);
        tc_put64(&q, 0);
        tc_put64(&q, 4);
        q.na = 1;
        q.a_addr[0] = e.heap + BUF_OFF;
        q.a_size[0] = 4;
        CHECK_EQ(rc_of(tc_send(e.c, st, q)), FS_WRITE_NOT_PERMITTED);
    }
    {
        tc_req q = tc_new(3);
        tc_put64(&q, 0);
        CHECK_EQ(rc_of(tc_send(e.c, st, q)), FS_WRITE_NOT_PERMITTED);
    }
    CHECK_EQ(tc_cmd(e.c, st, 2), 0);
    tc_close(e.c, st.handle);
    tree_remove(root);
}

TEST(romfs_program_id_and_data_id)
{
    env e = boot();
    tc_obj st, a, b;
    image im, ia;
    char root[512], dlc[512], dir0[640], dir1[640];
    const uint64_t id0 = 0x0100000000001001ull, id1 = 0x0100000000001002ull;
    uint64_t pid;
    make_root(root, sizeof root);
    make_tree(root);
    make_root(dlc, sizeof dlc);
    snprintf(dir0, sizeof dir0, "%s/%016llx", dlc, (unsigned long long)id0);
    snprintf(dir1, sizeof dir1, "%s/%016llx", dlc, (unsigned long long)id1);
    mkdir(dir0, 0777);
    mkdir(dir1, 0777);
    strcat(dir0, "/0");
    strcat(dir1, "/0");
    mkdir(dir0, 0777);
    mkdir(dir1, 0777);
    strcat(dir0, "/romfs");
    strcat(dir1, "/romfs");
    mkdir(dir0, 0777);
    mkdir(dir1, 0777);
    write_file(dir0, "pack.dat", "first add-on", 12);
    write_file(dir1, "pack.dat", "second add-on content", 21);
    rl_hos_fs_set_romfs(root);
    rl_hos_fs_set_addons(dlc);

    pid = hos.cfg.program_id;
    CHECK_EQ(open_cmd(&e, 201, &pid, 8, &st), 0);
    CHECK(image_open(&im, &e, st));
    CHECK(same_as_host(&im, root, "a.txt"));
    CHECK_EQ(lookup_file(&im, "pack.dat"), NONE);

    CHECK_EQ(open_data_id(&e, id0, &a), 0);
    CHECK_EQ(open_data_id(&e, id1, &b), 0);
    CHECK(image_open(&ia, &e, a));
    CHECK(lookup_file(&ia, "pack.dat") != NONE);
    {
        uint8_t got[32];
        uint32_t meta = lookup_file(&ia, "pack.dat");
        CHECK(read_image_file(&ia, meta, got, 12));
        CHECK(memcmp(got, "first add-on", 12) == 0);
    }
    {
        image ib;
        uint8_t got[32];
        uint32_t meta;
        CHECK(image_open(&ib, &e, b));
        meta = lookup_file(&ib, "pack.dat");
        CHECK_EQ(rd64(ib.fm + meta + 0x10), 21);
        CHECK(read_image_file(&ib, meta, got, 21));
        CHECK(memcmp(got, "second add-on content", 21) == 0);
        image_close(&ib);
    }
    CHECK_EQ(open_cmd(&e, 201, &id1, 8, &a), 0);
    CHECK_EQ(open_data_id(&e, 0x0100000000009999ull, &a), FS_TARGET_NOT_FOUND);
    {
        uint64_t other = 0x0100000000007777ull;
        CHECK_EQ(open_cmd(&e, 201, &other, 8, &a), FS_TARGET_NOT_FOUND);
    }
    image_close(&im);
    image_close(&ia);
    tree_remove(root);
    tree_remove(dlc);
}

TEST(romfs_absent)
{
    env e = boot();
    tc_obj st;
    char root[512];
    uint64_t id = 0x0100000000001001ull;
    CHECK_EQ(open_cmd(&e, 200, NULL, 0, &st), FS_TARGET_NOT_FOUND);
    CHECK_EQ(open_cmd(&e, 203, NULL, 0, &st), FS_TARGET_NOT_FOUND);
    CHECK_EQ(open_data_id(&e, id, &st), FS_TARGET_NOT_FOUND);
    CHECK_EQ(open_cmd(&e, 201, &id, 8, &st), FS_TARGET_NOT_FOUND);
    rl_hos_fs_set_romfs("/nonexistent-relift-romfs-dir");
    CHECK_EQ(open_cmd(&e, 200, NULL, 0, &st), FS_TARGET_NOT_FOUND);
    make_root(root, sizeof root);
    rl_hos_fs_set_romfs(root);
    CHECK_EQ(open_cmd(&e, 200, NULL, 0, &st), 0);
    {
        image im;
        CHECK(image_open(&im, &e, st));
        CHECK_EQ(lookup_file(&im, "anything"), NONE);
        CHECK_EQ(rd32(im.dm + 8), NONE);
        CHECK_EQ(rd32(im.dm + 12), NONE);
        image_close(&im);
    }
    tree_remove(root);
}

typedef struct warns {
    int n;
    char path[4][128];
    char winner[4][64];
    char loser[4][64];
} warns;

static void on_conflict(void *user, const char *path, const char *winner, const char *loser)
{
    warns *w = user;
    if (w->n < 4) {
        snprintf(w->path[w->n], sizeof w->path[0], "%s", path);
        snprintf(w->winner[w->n], sizeof w->winner[0], "%s", winner);
        snprintf(w->loser[w->n], sizeof w->loser[0], "%s", loser);
    }
    w->n++;
}

static void layer_tree(const char *dir, const char *const *files)
{
    for (; *files; files += 2) {
        char path[1024], *slash;
        snprintf(path, sizeof path, "%s/%s", dir, files[0]);
        slash = strrchr(path, '/');
        for (char *p = path + strlen(dir) + 1; p < slash; p++)
            if (*p == '/') {
                *p = 0;
                mkdir(path, 0777);
                *p = '/';
            }
        *slash = 0;
        mkdir(path, 0777);
        write_file(dir, files[0], files[1], strlen(files[1]));
    }
}

static bool image_has(const image *im, const char *rel, const char *want)
{
    uint32_t meta = lookup_file(im, rel);
    uint64_t n = strlen(want);
    uint8_t got[256];
    return meta != NONE && rd64(im->fm + meta + 0x10) == n && read_image_file(im, meta, got, n) &&
           memcmp(got, want, n) == 0;
}

TEST(romfs_layers_overlay_in_order)
{
    env e = boot();
    tc_obj st;
    image im;
    warns w;
    char base[512], m1[512], m2[512];
    const char *const bf[] = { "a.txt", "base a", "keep.txt", "base keep", "d/x", "base x", "d/y", "base y", NULL };
    const char *const f1[] = { "a.txt", "mod1 a", "d/x", "mod1 x", "new/n", "mod1 n", NULL };
    const char *const f2[] = { "d/x", "mod2 x", "new/n", "mod2 n", "new/o", "mod2 o", NULL };
    rl_romfs_layer layers[3];
    memset(&w, 0, sizeof w);
    make_root(base, sizeof base);
    make_root(m1, sizeof m1);
    make_root(m2, sizeof m2);
    layer_tree(base, bf);
    layer_tree(m1, f1);
    layer_tree(m2, f2);
    layers[0] = (rl_romfs_layer) { "base", base, true };
    layers[1] = (rl_romfs_layer) { "mod1", m1, false };
    layers[2] = (rl_romfs_layer) { "mod2", m2, false };
    rl_hos_fs_set_romfs_layers(layers, 3, on_conflict, &w);
    CHECK_EQ(open_cmd(&e, 200, NULL, 0, &st), 0);
    CHECK(image_open(&im, &e, st));
    CHECK(image_has(&im, "a.txt", "mod1 a"));
    CHECK(image_has(&im, "keep.txt", "base keep"));
    CHECK(image_has(&im, "d/x", "mod2 x"));
    CHECK(image_has(&im, "d/y", "base y"));
    CHECK(image_has(&im, "new/n", "mod2 n"));
    CHECK(image_has(&im, "new/o", "mod2 o"));
    CHECK_EQ(w.n, 2);
    CHECK_EQ(strcmp(w.path[0], "/d/x"), 0);
    CHECK_EQ(strcmp(w.winner[0], "mod2"), 0);
    CHECK_EQ(strcmp(w.loser[0], "mod1"), 0);
    CHECK_EQ(strcmp(w.path[1], "/new/n"), 0);
    image_close(&im);
    tc_close(e.c, st.handle);
    rl_hos_fs_set_romfs(NULL);
    tree_remove(base);
    tree_remove(m1);
    tree_remove(m2);
}

static void mkdirs(const char *root, const char *rel)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s", root, rel);
    for (char *p = path + strlen(root) + 1; *p; p++)
        if (*p == '/') {
            *p = 0;
            mkdir(path, 0777);
            *p = '/';
        }
    mkdir(path, 0777);
}

static void put(const char *root, const char *rel, const char *text)
{
    char dir[1024], *slash;
    snprintf(dir, sizeof dir, "%s", rel);
    slash = strrchr(dir, '/');
    if (slash) {
        *slash = 0;
        mkdirs(root, dir);
    }
    write_file(root, rel, text, strlen(text));
}

TEST(mods_both_layouts_overlay_and_read_through_fsp)
{
    env e = boot();
    tc_obj st;
    image im;
    char base[512], at[512], ry[512], tid[17], rel[128], list[2200];
    size_t n;
    make_root(base, sizeof base);
    make_root(at, sizeof at);
    make_root(ry, sizeof ry);
    snprintf(tid, sizeof tid, "%016llx", (unsigned long long)hos.cfg.program_id);
    put(base, "a.txt", "base a");
    put(base, "dir/b.txt", "base b");
    snprintf(rel, sizeof rel, "contents/%s/romfs/a.txt", tid);
    put(at, rel, "atmosphere a");
    snprintf(rel, sizeof rel, "contents/%s/romfs/dir/c.txt", tid);
    put(at, rel, "atmosphere c");
    snprintf(rel, sizeof rel, "contents/%s/exefs/main", tid);
    put(at, rel, "ignored");
    put(ry, "zeta/romfs/dir/b.txt", "ryujinx zeta b");
    put(ry, "alpha/romfs/a.txt", "ryujinx alpha a");
    put(ry, "empty/readme.txt", "no content");
    snprintf(list, sizeof list, "%s:%s", at, ry);
    n = rl_hos_mods_set(list, hos.cfg.program_id);
    CHECK_EQ(n, 3);
    CHECK_EQ(rl_hos_mods_info(0)->layout, RL_MOD_ATMOSPHERE);
    CHECK_EQ(strcmp(rl_hos_mods_info(0)->name, tid), 0);
    CHECK_EQ(rl_hos_mods_info(1)->layout, RL_MOD_RYUJINX);
    CHECK_EQ(strcmp(rl_hos_mods_info(1)->name, "alpha"), 0);
    CHECK_EQ(strcmp(rl_hos_mods_info(2)->name, "zeta"), 0);
    rl_hos_mods_apply_romfs(base);
    CHECK_EQ(open_cmd(&e, 200, NULL, 0, &st), 0);
    CHECK(image_open(&im, &e, st));
    CHECK(image_has(&im, "a.txt", "ryujinx alpha a"));
    CHECK(image_has(&im, "dir/b.txt", "ryujinx zeta b"));
    CHECK(image_has(&im, "dir/c.txt", "atmosphere c"));
    CHECK_EQ(lookup_file(&im, "main"), NONE);
    CHECK_EQ(rl_hos_mods_conflict_count(), 1);
    CHECK(strstr(rl_hos_mods_conflict(0), "/a.txt") != NULL);
    CHECK(strstr(rl_hos_mods_conflict(0), "alpha wins over") != NULL);
    {
        char json[1024];
        size_t need = rl_hos_mods_json(json, sizeof json);
        CHECK(need < sizeof json);
        CHECK(strstr(json, "\"layout\":\"atmosphere\"") != NULL);
        CHECK(strstr(json, "\"layout\":\"ryujinx\"") != NULL);
        CHECK(strstr(json, "alpha wins over") != NULL);
    }
    image_close(&im);
    tc_close(e.c, st.handle);
    rl_hos_mods_clear();
    rl_hos_fs_set_romfs(NULL);
    tree_remove(base);
    tree_remove(at);
    tree_remove(ry);
}

TEST(mods_folder_forms_and_wrong_title)
{
    char root[512], one[512], rel[128], tid[17];
    make_root(root, sizeof root);
    make_root(one, sizeof one);
    snprintf(tid, sizeof tid, "%016llx", (unsigned long long)hos.cfg.program_id);
    snprintf(rel, sizeof rel, "contents/%s/romfs/x", tid);
    put(root, rel, "x");
    put(root, "contents/0100000000000999/romfs/y", "y");
    CHECK_EQ(rl_hos_mods_set(root, hos.cfg.program_id), 1);
    CHECK_EQ(rl_hos_mods_info(0)->layout, RL_MOD_ATMOSPHERE);
    snprintf(rel, sizeof rel, "%s/contents", root);
    CHECK_EQ(rl_hos_mods_set(rel, hos.cfg.program_id), 1);
    snprintf(rel, sizeof rel, "%s/contents/0100000000000999", root);
    CHECK_EQ(rl_hos_mods_set(rel, hos.cfg.program_id), 0);
    CHECK_EQ(rl_hos_mods_set("/nonexistent-relift-mod", hos.cfg.program_id), 0);
    put(one, "romfs/z", "z");
    CHECK_EQ(rl_hos_mods_set(one, hos.cfg.program_id), 1);
    CHECK_EQ(rl_hos_mods_info(0)->layout, RL_MOD_RYUJINX);
    rl_hos_mods_clear();
    tree_remove(root);
    tree_remove(one);
}

static uint64_t keys_held;

static uint64_t held_keys(void *u)
{
    (void)u;
    return keys_held;
}

TEST(mods_cheat_writes_ram_every_frame)
{
    env e = boot();
    char mod[512];
    uint32_t v;
    uint8_t *heap = t_mem() + e.heap;
    make_root(mod, sizeof mod);
    put(mod, "cheats/0123456789ABCDEF.txt",
        "[Always]\n04100000 00000040 DEADBEEF\n\n{Gated}\n80000001\n04100000 00000044 00000007\n20000000\n"
        "[Code]\n04000000 00000000 11111111\n");
    put(mod, "cheats/FFFFFFFFFFFFFFFF.txt", "[Other build]\n04100000 00000048 00000001\n");
    CHECK_EQ(rl_hos_mods_set(mod, hos.cfg.program_id), 1);
    rl_hos_mods_set_keys(held_keys, NULL);
    keys_held = 0;
    rl_hos_mods_attach(hos.lay.code_start, "0123456789abcdef0000000000000000");
    CHECK_EQ(rl_hos_mods_info(0)->cheats, 3);
    CHECK_EQ(hos.lay.heap_start, e.heap);
    rl_hos_mods_frame();
    memcpy(&v, heap + 0x40, 4);
    CHECK_EQ(v, 0xDEADBEEF);
    memcpy(&v, heap + 0x44, 4);
    CHECK_EQ(v, 0);
    memcpy(&v, heap + 0x48, 4);
    CHECK_EQ(v, 0);
    memset(heap + 0x40, 0, 8);
    keys_held = 1;
    rl_hos_mods_frame();
    memcpy(&v, heap + 0x40, 4);
    CHECK_EQ(v, 0xDEADBEEF);
    memcpy(&v, heap + 0x44, 4);
    CHECK_EQ(v, 7);
    rl_hos_mods_set_keys(NULL, NULL);
    rl_hos_mods_clear();
    tree_remove(mod);
}
