/**
 * @file test_fs.c
 * @brief fsp-srv through svcSendSyncRequest: files, directories, renames,
 * errors and path confinement on a host directory under
 * ~/spare/relift-phase4/tmp.
 *
 * The client encoder follows the message layout of libnx (ISC) sf/hipc.h and
 * sf/cmif.h, written with guest addresses.
 */
#include "hos_fs.h"
#include "t.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief A request being built. */
typedef struct rq {
    uint32_t cmd;
    bool send_pid;
    uint8_t data[48];
    uint32_t dsize;
    uint64_t x_addr[2], x_size[2];
    uint32_t nx;
    uint64_t a_addr, a_size;
    uint32_t na;
    uint64_t b_addr, b_size;
    uint32_t nb;
} rq;

/** @brief A parsed response. */
typedef struct rs {
    uint32_t svc_rc;
    uint32_t result;
    uint8_t data[48];
    uint32_t move[4];
    uint32_t nmove;
} rs;

static void put(uint32_t *w, uint32_t *pos, uint32_t v) { w[(*pos)++] = v; }

static void buf_desc(uint32_t *w, uint32_t *pos, uint64_t addr, uint64_t size)
{
    put(w, pos, (uint32_t)size);
    put(w, pos, (uint32_t)addr);
    put(w, pos, (uint32_t)(((addr >> 36) & 0x3FFFFF) << 2) | (uint32_t)(((size >> 32) & 0xF) << 24) |
                    (uint32_t)(((addr >> 32) & 0xF) << 28));
}

static void encode(const rq *q, uint32_t *w)
{
    uint32_t pos = 2;
    bool special = q->send_pid;
    uint8_t *b = (uint8_t *)w;
    uint32_t off, data_start, ndata;
    uint32_t hdr[4];
    memset(w, 0, RL_IPC_MSG);
    if (special) {
        put(w, &pos, 1);
        put(w, &pos, 0);
        put(w, &pos, 0);
    }
    for (uint32_t i = 0; i < q->nx; i++) {
        uint64_t a = q->x_addr[i];
        put(w, &pos, (i & 0x3F) | (uint32_t)(((a >> 36) & 0x3F) << 6) | (uint32_t)(((a >> 32) & 0xF) << 12) |
                         (uint32_t)(q->x_size[i] << 16));
        put(w, &pos, (uint32_t)a);
    }
    if (q->na)
        buf_desc(w, &pos, q->a_addr, q->a_size);
    if (q->nb)
        buf_desc(w, &pos, q->b_addr, q->b_size);
    data_start = pos;
    off = (pos * 4 + 15) & ~15u;
    hdr[0] = CMIF_IN_MAGIC;
    hdr[1] = 0;
    hdr[2] = q->cmd;
    hdr[3] = 0;
    memcpy(b + off, hdr, 16);
    off += 16;
    memcpy(b + off, q->data, q->dsize);
    off += q->dsize;
    off += 16;
    ndata = (off - data_start * 4 + 3) / 4;
    w[0] = HIPC_REQUEST | (q->nx << 16) | (q->na << 20) | (q->nb << 24);
    w[1] = ndata | (special ? 1u << 31 : 0);
}

static void decode(const uint32_t *w, rs *r)
{
    uint32_t pos = 2;
    const uint8_t *b = (const uint8_t *)w;
    uint32_t off, hdr[4];
    if (w[1] >> 31) {
        uint32_t sp = w[pos++];
        uint32_t ncopy = (sp >> 1) & 0xF;
        r->nmove = (sp >> 5) & 0xF;
        if (sp & 1)
            pos += 2;
        pos += ncopy;
        for (uint32_t i = 0; i < r->nmove && i < 4; i++)
            r->move[i] = w[pos++];
    }
    off = (pos * 4 + 15) & ~15u;
    memcpy(hdr, b + off, 16);
    r->result = hdr[0] == CMIF_OUT_MAGIC ? hdr[2] : 0xBADBAD;
    memcpy(r->data, b + off + 16, sizeof r->data);
}

static rs call(rl_ctx *c, uint32_t h, const rq *q)
{
    rs r;
    uint32_t w[RL_IPC_MSG / 4];
    uint32_t *tls = (uint32_t *)(void *)(t_mem() + c->tpidrro_el0);
    memset(&r, 0, sizeof r);
    encode(q, w);
    memcpy(tls, w, RL_IPC_MSG);
    r.svc_rc = t_svc(c, SVC_SEND_SYNC_REQUEST, h, 0, 0, 0, 0, 0);
    if (r.svc_rc == 0)
        decode(tls, &r);
    return r;
}

static void put_data(rq *q, const void *p, uint32_t n)
{
    memcpy(q->data + q->dsize, p, n);
    q->dsize += n;
}

static uint64_t u64_at(const rs *r, uint32_t off)
{
    uint64_t v;
    memcpy(&v, r->data + off, 8);
    return v;
}

static uint32_t u32_at(const rs *r, uint32_t off)
{
    uint32_t v;
    memcpy(&v, r->data + off, 4);
    return v;
}

/** @brief Guest scratch layout in the test heap. */
enum {
    OFF_SCRATCH = 0,
    OFF_PATH0 = 0x400,
    OFF_PATH1 = 0x800,
    OFF_A = 0x1000,
    OFF_B = 0x20000
};

static char g_tmp[PATH_MAX];

static int rm_cb(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void tmp_make(const char *name)
{
    const char *home = getenv("HOME");
    char base[PATH_MAX];
    snprintf(base, sizeof base, "%s/spare/relift-phase4/tmp", home ? home : ".");
    snprintf(g_tmp, sizeof g_tmp, "%s/%s", base, name);
    nftw(g_tmp, rm_cb, 8, FTW_DEPTH | FTW_PHYS);
    mkdir(base, 0777);
    mkdir(g_tmp, 0777);
}

static void tmp_clean(void) { nftw(g_tmp, rm_cb, 8, FTW_DEPTH | FTW_PHYS); }

static char *host(const char *rel)
{
    static char buf[PATH_MAX];
    snprintf(buf, sizeof buf, "%s%s", g_tmp, rel);
    return buf;
}

/** @brief Session state of one test: the context, heap base and the IFileSystem handle. */
typedef struct fsx {
    rl_ctx *c;
    uint64_t heap;
    uint32_t proxy;
    uint32_t fs;
} fsx;

static void put_path(const fsx *x, uint32_t off, const char *path)
{
    uint8_t *p = t_mem() + x->heap + off;
    memset(p, 0, 0x301);
    memcpy(p, path, strlen(path));
}

static rq with_paths(const fsx *x, uint32_t cmd, const char *p0, const char *p1)
{
    rq q;
    memset(&q, 0, sizeof q);
    q.cmd = cmd;
    if (p0) {
        put_path(x, OFF_PATH0, p0);
        q.x_addr[q.nx] = x->heap + OFF_PATH0;
        q.x_size[q.nx++] = 0x301;
    }
    if (p1) {
        put_path(x, OFF_PATH1, p1);
        q.x_addr[q.nx] = x->heap + OFF_PATH1;
        q.x_size[q.nx++] = 0x301;
    }
    return q;
}

static rs fs_call(const fsx *x, uint32_t cmd, const char *p0, const char *p1, const void *data, uint32_t n)
{
    rq q = with_paths(x, cmd, p0, p1);
    if (n)
        put_data(&q, data, n);
    return call(x->c, x->fs, &q);
}

static bool fs_open(fsx *x, const char *dir)
{
    uint64_t name = rl_ipc_name("sm:");
    rq q;
    rs r;
    memset(x, 0, sizeof *x);
    x->c = t_boot(NULL);
    x->heap = t_heap(x->c, 0x200000);
    if (!x->heap)
        return false;
    rl_hos_fs_init(dir);
    memcpy(t_mem() + x->heap, "sm:", 4);
    if (t_svc(x->c, SVC_CONNECT_TO_NAMED_PORT, 0, x->heap, 0, 0, 0, 0) != 0)
        return false;
    {
        uint32_t sm = (uint32_t)x->c->x[1];
        uint64_t zero = 0;
        memset(&q, 0, sizeof q);
        q.cmd = 0;
        q.send_pid = true;
        put_data(&q, &zero, 8);
        r = call(x->c, sm, &q);
        if (r.svc_rc || r.result)
            return false;
        memset(&q, 0, sizeof q);
        q.cmd = 1;
        name = rl_ipc_name("fsp-srv");
        put_data(&q, &name, 8);
        r = call(x->c, sm, &q);
        if (r.svc_rc || r.result || r.nmove != 1)
            return false;
        x->proxy = r.move[0];
    }
    memset(&q, 0, sizeof q);
    q.cmd = 1;
    q.send_pid = true;
    {
        uint64_t pid = 0;
        put_data(&q, &pid, 8);
    }
    r = call(x->c, x->proxy, &q);
    if (r.svc_rc || r.result)
        return false;
    memset(&q, 0, sizeof q);
    q.cmd = 18;
    r = call(x->c, x->proxy, &q);
    if (r.svc_rc || r.result || r.nmove != 1)
        return false;
    x->fs = r.move[0];
    return true;
}

static uint32_t fs_result(const fsx *x, uint32_t cmd, const char *p0, const char *p1, const void *data, uint32_t n)
{
    rs r = fs_call(x, cmd, p0, p1, data, n);
    return r.svc_rc ? r.svc_rc : r.result;
}

static uint32_t create_file(const fsx *x, const char *path, uint64_t size)
{
    uint64_t in[2] = { 0, size };
    return fs_result(x, 0, path, NULL, in, 16);
}

static uint32_t open_file(const fsx *x, const char *path, uint32_t mode, uint32_t *h)
{
    rs r = fs_call(x, 8, path, NULL, &mode, 4);
    *h = r.nmove ? r.move[0] : 0;
    return r.svc_rc ? r.svc_rc : r.result;
}

static uint32_t file_write(const fsx *x, uint32_t h, uint64_t off, const void *p, uint64_t n)
{
    uint64_t in[3] = { 0, off, n };
    rq q;
    memset(&q, 0, sizeof q);
    q.cmd = 1;
    put_data(&q, in, 24);
    q.na = 1;
    q.a_addr = x->heap + OFF_A;
    q.a_size = n;
    memcpy(t_mem() + q.a_addr, p, n);
    {
        rs r = call(x->c, h, &q);
        return r.svc_rc ? r.svc_rc : r.result;
    }
}

static uint32_t file_read(const fsx *x, uint32_t h, uint64_t off, uint64_t n, uint64_t buf_size, void *out, uint64_t *got)
{
    uint64_t in[3] = { 0, off, n };
    rq q;
    rs r;
    memset(&q, 0, sizeof q);
    q.cmd = 0;
    put_data(&q, in, 24);
    q.nb = 1;
    q.b_addr = x->heap + OFF_B;
    q.b_size = buf_size;
    memset(t_mem() + q.b_addr, 0xEE, buf_size);
    r = call(x->c, h, &q);
    *got = (r.svc_rc || r.result) ? 0 : u64_at(&r, 0);
    memcpy(out, t_mem() + q.b_addr, *got);
    return r.svc_rc ? r.svc_rc : r.result;
}

static uint64_t file_size(const fsx *x, uint32_t h)
{
    rq q;
    rs r;
    memset(&q, 0, sizeof q);
    q.cmd = 4;
    r = call(x->c, h, &q);
    return u64_at(&r, 0);
}

static int slurp(const char *rel, char *out, size_t n)
{
    int fd = open(host(rel), O_RDONLY);
    ssize_t k;
    if (fd < 0)
        return -1;
    k = read(fd, out, n);
    close(fd);
    return (int)k;
}

TEST(fs_proxy_commands)
{
    fsx x;
    rq q;
    rs r;
    tmp_make("fs-proxy");
    CHECK(fs_open(&x, g_tmp));
    memset(&q, 0, sizeof q);
    q.cmd = 1003;
    r = call(x.c, x.proxy, &q);
    CHECK_EQ(r.svc_rc, 0);
    CHECK_EQ(r.result, 0);
    memset(&q, 0, sizeof q);
    q.cmd = 12345;
    r = call(x.c, x.proxy, &q);
    CHECK_EQ(r.svc_rc, 0);
    CHECK_EQ(r.result, SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(fs_result(&x, 99, NULL, NULL, NULL, 0), SF_RESULT_UNKNOWN_COMMAND);
    CHECK_EQ(fs_result(&x, 10, NULL, NULL, NULL, 0), 0);
    tmp_clean();
}

TEST(fs_file_roundtrip)
{
    fsx x;
    uint32_t h = 0, mode;
    char buf[64];
    uint64_t got = 0;
    rs r;
    tmp_make("fs-file");
    CHECK(fs_open(&x, g_tmp));
    CHECK_EQ(fs_result(&x, 2, "/relift-test", NULL, NULL, 0), 0);
    CHECK_EQ(fs_result(&x, 2, "/relift-test", NULL, NULL, 0), RL_MAKE_RESULT(2, 2));
    CHECK_EQ(create_file(&x, "/relift-test/min_init.log", 0), 0);
    CHECK_EQ(create_file(&x, "/relift-test/min_init.log", 0), RL_MAKE_RESULT(2, 2));
    CHECK_EQ(create_file(&x, "/missing/min_init.log", 0), RL_MAKE_RESULT(2, 1));
    r = fs_call(&x, 7, "/relift-test/min_init.log", NULL, NULL, 0);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(u32_at(&r, 0), 1);
    r = fs_call(&x, 7, "/relift-test", NULL, NULL, 0);
    CHECK_EQ(u32_at(&r, 0), 0);
    CHECK_EQ(fs_result(&x, 7, "/nope", NULL, NULL, 0), RL_MAKE_RESULT(2, 1));

    mode = 1;
    CHECK_EQ(open_file(&x, "/relift-test/nope.log", mode, &h), RL_MAKE_RESULT(2, 1));
    CHECK_EQ(open_file(&x, "/relift-test", mode, &h), RL_MAKE_RESULT(2, 1));
    CHECK_EQ(open_file(&x, "/relift-test/min_init.log", 0, &h), RL_MAKE_RESULT(2, 6072));
    CHECK_EQ(open_file(&x, "/relift-test/min_init.log", 2 | 4, &h), 0);
    CHECK(h != 0);
    CHECK_EQ(file_write(&x, h, 0, "PASS foo\n", 9), 0);
    CHECK_EQ(file_write(&x, h, 9, "PASS bar\n", 9), 0);
    CHECK_EQ(file_size(&x, h), 18);
    CHECK_EQ(file_read(&x, h, 0, 4, 4, buf, &got), RL_MAKE_RESULT(2, 6202));
    CHECK_EQ(t_svc(x.c, SVC_CLOSE_HANDLE, h, 0, 0, 0, 0, 0), 0);
    CHECK_EQ(slurp("/relift-test/min_init.log", buf, sizeof buf), 18);
    CHECK(memcmp(buf, "PASS foo\nPASS bar\n", 18) == 0);

    CHECK_EQ(open_file(&x, "/relift-test/min_init.log", 2, &h), 0);
    CHECK_EQ(file_write(&x, h, 0, "XX", 2), 0);
    CHECK_EQ(file_write(&x, h, 17, "YY", 2), RL_MAKE_RESULT(2, 6201));
    CHECK_EQ(t_svc(x.c, SVC_CLOSE_HANDLE, h, 0, 0, 0, 0, 0), 0);

    CHECK_EQ(open_file(&x, "/relift-test/min_init.log", 1, &h), 0);
    CHECK_EQ(file_read(&x, h, 0, 18, 64, buf, &got), 0);
    CHECK_EQ(got, 18);
    CHECK(memcmp(buf, "XXSS foo\nPASS bar\n", 18) == 0);
    CHECK_EQ(file_read(&x, h, 9, 64, 64, buf, &got), 0);
    CHECK_EQ(got, 9);
    CHECK(memcmp(buf, "PASS bar\n", 9) == 0);
    CHECK_EQ(file_read(&x, h, 100, 8, 8, buf, &got), 0);
    CHECK_EQ(got, 0);
    CHECK_EQ(file_read(&x, h, 3, 4, 2, buf, &got), 0);
    CHECK_EQ(got, 2);
    CHECK_EQ(file_write(&x, h, 0, "Z", 1), RL_MAKE_RESULT(2, 6203));
    CHECK_EQ(t_svc(x.c, SVC_CLOSE_HANDLE, h, 0, 0, 0, 0, 0), 0);

    CHECK_EQ(open_file(&x, "/relift-test/min_init.log", 3, &h), 0);
    {
        rq q;
        uint64_t sz = 4;
        memset(&q, 0, sizeof q);
        q.cmd = 3;
        put_data(&q, &sz, 8);
        r = call(x.c, h, &q);
        CHECK_EQ(r.result, 0);
        CHECK_EQ(file_size(&x, h), 4);
        memset(&q, 0, sizeof q);
        q.cmd = 2;
        r = call(x.c, h, &q);
        CHECK_EQ(r.result, 0);
        sz = 0;
        memset(&q, 0, sizeof q);
        q.cmd = 77;
        r = call(x.c, h, &q);
        CHECK_EQ(r.result, SF_RESULT_UNKNOWN_COMMAND);
    }
    CHECK_EQ(t_svc(x.c, SVC_CLOSE_HANDLE, h, 0, 0, 0, 0, 0), 0);

    CHECK_EQ(create_file(&x, "/sized.bin", 5000), 0);
    {
        struct stat st;
        CHECK_EQ(stat(host("/sized.bin"), &st), 0);
        CHECK_EQ(st.st_size, 5000);
    }
    tmp_clean();
}

TEST(fs_paths_and_renames)
{
    fsx x;
    struct stat st;
    rs r;
    tmp_make("fs-paths");
    CHECK(fs_open(&x, g_tmp));
    CHECK_EQ(fs_result(&x, 2, "/a/../../escape", NULL, NULL, 0), RL_MAKE_RESULT(2, 6002));
    CHECK_EQ(fs_result(&x, 2, "/..", NULL, NULL, 0), RL_MAKE_RESULT(2, 6002));
    CHECK_EQ(fs_result(&x, 2, "relative", NULL, NULL, 0), RL_MAKE_RESULT(2, 6004));
    CHECK_EQ(fs_result(&x, 2, "//d1//./sub", NULL, NULL, 0), RL_MAKE_RESULT(2, 1));
    CHECK_EQ(fs_result(&x, 2, "//d1/", NULL, NULL, 0), 0);
    CHECK_EQ(stat(host("/d1"), &st), 0);
    CHECK(S_ISDIR(st.st_mode));
    CHECK_EQ(fs_result(&x, 2, "/d1/sub", NULL, NULL, 0), 0);
    CHECK_EQ(create_file(&x, "/d1/f.txt", 3), 0);

    CHECK_EQ(fs_result(&x, 3, "/d1", NULL, NULL, 0), RL_MAKE_RESULT(2, 8));
    CHECK_EQ(fs_result(&x, 1, "/d1", NULL, NULL, 0), RL_MAKE_RESULT(2, 1));
    CHECK_EQ(fs_result(&x, 5, "/d1/f.txt", "/d1/g.txt", NULL, 0), 0);
    CHECK_EQ(stat(host("/d1/f.txt"), &st), -1);
    CHECK_EQ(stat(host("/d1/g.txt"), &st), 0);
    CHECK_EQ(create_file(&x, "/d1/h.txt", 0), 0);
    CHECK_EQ(fs_result(&x, 5, "/d1/g.txt", "/d1/h.txt", NULL, 0), RL_MAKE_RESULT(2, 2));
    CHECK_EQ(fs_result(&x, 5, "/d1/none.txt", "/d1/z.txt", NULL, 0), RL_MAKE_RESULT(2, 1));
    CHECK_EQ(fs_result(&x, 5, "/d1/sub", "/d1/z.txt", NULL, 0), RL_MAKE_RESULT(2, 1));
    CHECK_EQ(fs_result(&x, 6, "/d1/sub", "/d1/sub2", NULL, 0), 0);
    CHECK_EQ(stat(host("/d1/sub2"), &st), 0);
    CHECK_EQ(fs_result(&x, 6, "/d1/g.txt", "/d1/q", NULL, 0), RL_MAKE_RESULT(2, 1));
    CHECK_EQ(fs_result(&x, 1, "/d1/g.txt", NULL, NULL, 0), 0);
    CHECK_EQ(fs_result(&x, 1, "/d1/g.txt", NULL, NULL, 0), RL_MAKE_RESULT(2, 1));
    CHECK_EQ(fs_result(&x, 3, "/d1/sub2", NULL, NULL, 0), 0);

    CHECK_EQ(fs_result(&x, 2, "/tree", NULL, NULL, 0), 0);
    CHECK_EQ(fs_result(&x, 2, "/tree/a", NULL, NULL, 0), 0);
    CHECK_EQ(create_file(&x, "/tree/a/f", 1), 0);
    CHECK_EQ(create_file(&x, "/tree/g", 1), 0);
    CHECK_EQ(fs_result(&x, 13, "/tree", NULL, NULL, 0), 0);
    CHECK_EQ(stat(host("/tree"), &st), 0);
    CHECK_EQ(stat(host("/tree/g"), &st), -1);
    CHECK_EQ(create_file(&x, "/tree/g", 1), 0);
    CHECK_EQ(fs_result(&x, 4, "/tree", NULL, NULL, 0), 0);
    CHECK_EQ(stat(host("/tree"), &st), -1);
    CHECK_EQ(fs_result(&x, 4, "/tree", NULL, NULL, 0), RL_MAKE_RESULT(2, 1));

    r = fs_call(&x, 11, "/", NULL, NULL, 0);
    CHECK_EQ(r.result, 0);
    CHECK(u64_at(&r, 0) > 0);
    r = fs_call(&x, 12, "/", NULL, NULL, 0);
    CHECK_EQ(r.result, 0);
    CHECK(u64_at(&r, 0) > 0);
    r = fs_call(&x, 14, "/d1/h.txt", NULL, NULL, 0);
    CHECK_EQ(r.result, 0);
    CHECK(u64_at(&r, 8) > 1000000000ull);
    CHECK_EQ(r.data[0x18], 1);
    tmp_clean();
}

TEST(fs_directory_listing)
{
    fsx x;
    rs r;
    rq q;
    uint32_t h = 0, mode;
    const uint8_t *b = NULL;
    tmp_make("fs-dir");
    CHECK(fs_open(&x, g_tmp));
    CHECK_EQ(fs_result(&x, 2, "/dir", NULL, NULL, 0), 0);
    CHECK_EQ(fs_result(&x, 2, "/dir/zdir", NULL, NULL, 0), 0);
    CHECK_EQ(create_file(&x, "/dir/b.bin", 300), 0);
    CHECK_EQ(create_file(&x, "/dir/a.bin", 7), 0);
    CHECK_EQ(create_file(&x, "/dir/c.bin", 0), 0);

    mode = 3;
    r = fs_call(&x, 9, "/dir", NULL, &mode, 4);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(r.nmove, 1);
    h = r.move[0];
    memset(&q, 0, sizeof q);
    q.cmd = 1;
    r = call(x.c, h, &q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(u64_at(&r, 0), 4);

    memset(&q, 0, sizeof q);
    q.cmd = 0;
    q.nb = 1;
    q.b_addr = x.heap + OFF_B;
    q.b_size = 3 * 0x310;
    memset(t_mem() + q.b_addr, 0xEE, q.b_size);
    r = call(x.c, h, &q);
    CHECK_EQ(r.result, 0);
    CHECK_EQ(u64_at(&r, 0), 3);
    b = t_mem() + q.b_addr;
    CHECK(strcmp((const char *)b, "a.bin") == 0);
    CHECK_EQ(b[0x304], 1);
    CHECK_EQ(b[0x301], 0);
    CHECK_EQ(*(const uint64_t *)(const void *)(b + 0x308), 7);
    CHECK(strcmp((const char *)b + 0x310, "b.bin") == 0);
    CHECK_EQ(*(const uint64_t *)(const void *)(b + 0x310 + 0x308), 300);
    CHECK(strcmp((const char *)b + 0x620, "c.bin") == 0);
    r = call(x.c, h, &q);
    CHECK_EQ(u64_at(&r, 0), 1);
    CHECK(strcmp((const char *)b, "zdir") == 0);
    CHECK_EQ(b[0x304], 0);
    CHECK_EQ(b[0x301], 1);
    CHECK_EQ(*(const uint64_t *)(const void *)(b + 0x308), 0);
    r = call(x.c, h, &q);
    CHECK_EQ(u64_at(&r, 0), 0);
    CHECK_EQ(t_svc(x.c, SVC_CLOSE_HANDLE, h, 0, 0, 0, 0, 0), 0);

    mode = 1;
    r = fs_call(&x, 9, "/dir", NULL, &mode, 4);
    h = r.move[0];
    memset(&q, 0, sizeof q);
    q.cmd = 1;
    r = call(x.c, h, &q);
    CHECK_EQ(u64_at(&r, 0), 1);
    CHECK_EQ(t_svc(x.c, SVC_CLOSE_HANDLE, h, 0, 0, 0, 0, 0), 0);
    mode = 2;
    r = fs_call(&x, 9, "/dir", NULL, &mode, 4);
    h = r.move[0];
    memset(&q, 0, sizeof q);
    q.cmd = 1;
    r = call(x.c, h, &q);
    CHECK_EQ(u64_at(&r, 0), 3);
    CHECK_EQ(t_svc(x.c, SVC_CLOSE_HANDLE, h, 0, 0, 0, 0, 0), 0);
    mode = 3;
    CHECK_EQ(fs_result(&x, 9, "/nodir", NULL, &mode, 4), RL_MAKE_RESULT(2, 1));
    tmp_clean();
}
