/**
 * @file test_srv_aoc.c
 * @brief aoc:u driven through IPC: counts, lists, base ids, the events and
 * opening a listed add-on through fsp-srv.
 */
#include "hos_fs.h"
#include "tc.h"

#include <fcntl.h>
#include <ftw.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

#define BASE 0x0100000000011000ull
#define LIST_OFF 0x1000u

typedef struct env {
    rl_ctx *c;
    uint64_t heap;
    tc_obj aoc;
} env;

static uint32_t rc_of(tc_res r) { return r.svc_rc ? r.svc_rc : r.result; }

static int remove_one(const char *path, const struct stat *st, int flag, struct FTW *ftw)
{
    (void)st;
    (void)flag;
    (void)ftw;
    return remove(path);
}

static void tree_remove(const char *dir) { nftw(dir, remove_one, 16, FTW_DEPTH | FTW_PHYS); }

static void make_root(char *out, size_t n)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(out, n, "%s/relift-aoc-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    if (!mkdtemp(out)) {
        fprintf(stderr, "  mkdtemp %s failed\n", out);
        exit(2);
    }
}

static void make_addon(const char *root, uint64_t id, bool with_romfs)
{
    char path[1024];
    int fd;
    snprintf(path, sizeof path, "%s/%016llx", root, (unsigned long long)id);
    mkdir(path, 0777);
    strcat(path, "/0");
    mkdir(path, 0777);
    if (!with_romfs)
        return;
    strcat(path, "/romfs");
    mkdir(path, 0777);
    strcat(path, "/pack.dat");
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0 || write(fd, "dlc", 3) != 3) {
        fprintf(stderr, "  cannot write %s\n", path);
        exit(2);
    }
    close(fd);
}

static env boot(void)
{
    env e;
    e.c = t_boot(NULL);
    e.heap = t_heap(e.c, 0x200000);
    e.aoc = tc_open_service(e.c, e.heap, "aoc:u");
    return e;
}

static uint32_t *list_buf(const env *e) { return (uint32_t *)(void *)(t_mem() + e->heap + LIST_OFF); }

static tc_res list_call(const env *e, uint32_t cmd, uint32_t offset, uint32_t count, uint32_t cap_bytes, uint64_t app)
{
    tc_req q = tc_new(cmd);
    tc_put32(&q, offset);
    tc_put32(&q, count);
    if (cmd == 1)
        tc_put64(&q, app);
    else
        q.send_pid = true;
    q.nb = 1;
    q.b_addr[0] = e->heap + LIST_OFF;
    q.b_size[0] = cap_bytes;
    memset(list_buf(e), 0xEE, 64);
    return tc_send(e->c, e->aoc, q);
}

static uint32_t count_of(const env *e, uint32_t cmd, uint64_t app)
{
    tc_req q = tc_new(cmd);
    tc_res r;
    if (cmd == 0)
        tc_put64(&q, app);
    else
        q.send_pid = true;
    r = tc_send(e->c, e->aoc, q);
    return rc_of(r) ? 0xFFFFFFFFu : tc_u32(&r, 0);
}

static tc_res base_id_call(const env *e)
{
    tc_req q = tc_new(5);
    q.send_pid = true;
    return tc_send(e->c, e->aoc, q);
}

TEST(aoc_appears_through_sm)
{
    env e = boot();
    CHECK(e.aoc.handle);
    CHECK_EQ(tc_open_service_result(e.c, e.heap, "aoc:x"), SM_RESULT_NOT_REGISTERED);
}

TEST(aoc_without_add_ons_counts_zero)
{
    env e = boot();
    char root[512];
    rl_hos_fs_set_addons(NULL);
    CHECK_EQ(count_of(&e, 2, 0), 0);
    CHECK_EQ(count_of(&e, 0, hos.cfg.program_id), 0);
    {
        tc_res r = list_call(&e, 3, 0, 8, 32, 0);
        CHECK_EQ(rc_of(r), 0);
        CHECK_EQ(tc_u32(&r, 0), 0);
        CHECK_EQ(list_buf(&e)[0], 0xEEEEEEEEu);
    }
    make_root(root, sizeof root);
    rl_hos_fs_set_addons(root);
    CHECK_EQ(count_of(&e, 2, 0), 0);
    rl_hos_fs_set_addons("/nonexistent-relift-dlc-dir");
    CHECK_EQ(count_of(&e, 2, 0), 0);
    rl_hos_fs_set_addons(NULL);
    tree_remove(root);
}

TEST(aoc_base_ids)
{
    env e = boot();
    tc_res r = base_id_call(&e);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u64(&r, 0), BASE);
    {
        tc_req q = tc_new(4);
        tc_put64(&q, 0x01007EF00011E000ull);
        r = tc_send(e.c, e.aoc, q);
        CHECK_EQ(tc_u64(&r, 0), 0x01007EF00011F000ull);
    }
    {
        tc_req q = tc_new(4);
        tc_put64(&q, 0x01007EF00011E800ull);
        r = tc_send(e.c, e.aoc, q);
        CHECK_EQ(tc_u64(&r, 0), 0x01007EF00011F000ull);
    }
}

TEST(aoc_two_add_ons_count_and_list)
{
    env e = boot();
    char root[512];
    tc_res r;
    make_root(root, sizeof root);
    make_addon(root, BASE + 2, true);
    make_addon(root, BASE + 1, true);
    make_addon(root, BASE + 3, false);
    make_addon(root, 0x0100000000021001ull, true);
    make_addon(root, BASE, true);
    rl_hos_fs_set_addons(root);

    CHECK_EQ(count_of(&e, 2, 0), 2);
    CHECK_EQ(count_of(&e, 0, hos.cfg.program_id), 2);
    CHECK_EQ(count_of(&e, 0, 0x0100000000020000ull), 1);
    CHECK_EQ(count_of(&e, 0, 0x0100000000030000ull), 0);

    r = list_call(&e, 3, 0, 8, 32, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 2);
    CHECK_EQ(list_buf(&e)[0], 1);
    CHECK_EQ(list_buf(&e)[1], 2);
    CHECK_EQ(list_buf(&e)[2], 0xEEEEEEEEu);

    r = list_call(&e, 1, 0, 8, 32, 0x0100000000020000ull);
    CHECK_EQ(tc_u32(&r, 0), 1);
    CHECK_EQ(list_buf(&e)[0], 1);

    rl_hos_fs_set_addons(NULL);
    tree_remove(root);
}

TEST(aoc_list_pagination)
{
    env e = boot();
    char root[512];
    tc_res r;
    make_root(root, sizeof root);
    for (uint64_t i = 1; i <= 5; i++)
        make_addon(root, BASE + i, true);
    rl_hos_fs_set_addons(root);

    r = list_call(&e, 3, 1, 2, 32, 0);
    CHECK_EQ(tc_u32(&r, 0), 2);
    CHECK_EQ(list_buf(&e)[0], 2);
    CHECK_EQ(list_buf(&e)[1], 3);
    CHECK_EQ(list_buf(&e)[2], 0xEEEEEEEEu);

    r = list_call(&e, 3, 3, 10, 40, 0);
    CHECK_EQ(tc_u32(&r, 0), 2);
    CHECK_EQ(list_buf(&e)[0], 4);
    CHECK_EQ(list_buf(&e)[1], 5);

    r = list_call(&e, 3, 0, 10, 12, 0);
    CHECK_EQ(tc_u32(&r, 0), 3);
    CHECK_EQ(list_buf(&e)[2], 3);
    CHECK_EQ(list_buf(&e)[3], 0xEEEEEEEEu);

    r = list_call(&e, 3, 5, 4, 16, 0);
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    r = list_call(&e, 3, 99, 4, 16, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);
    r = list_call(&e, 3, 0, 0, 16, 0);
    CHECK_EQ(tc_u32(&r, 0), 0);

    rl_hos_fs_set_addons(NULL);
    tree_remove(root);
}

TEST(aoc_events_prepare_and_stubs)
{
    env e = boot();
    uint32_t evs[2] = { 8, 10 };
    for (uint32_t i = 0; i < 2; i++) {
        tc_req q = tc_new(evs[i]);
        tc_res r;
        q.send_pid = true;
        r = tc_send(e.c, e.aoc, q);
        CHECK_EQ(rc_of(r), 0);
        CHECK_EQ(r.ncopy, 1);
        *(uint32_t *)(void *)(t_mem() + e.heap + 0x800) = r.copy[0];
        CHECK_EQ(t_svc(e.c, SVC_WAIT_SYNCHRONIZATION, 0, e.heap + 0x800, 1, 0, 0, 0), KR_TIMED_OUT);
    }
    {
        tc_req q = tc_new(7);
        tc_put32(&q, 1);
        q.send_pid = true;
        CHECK_EQ(rc_of(tc_send(e.c, e.aoc, q)), 0);
        q = tc_new(6);
        tc_put32(&q, 1);
        tc_put64(&q, BASE);
        CHECK_EQ(rc_of(tc_send(e.c, e.aoc, q)), 0);
    }
    CHECK_EQ(tc_cmd(e.c, e.aoc, 50), 0);
    CHECK_EQ(tc_cmd(e.c, e.aoc, 11), 0);
    CHECK_EQ(tc_cmd(e.c, e.aoc, 12), 0);
    CHECK_EQ(tc_cmd(e.c, e.aoc, 99), SF_RESULT_UNKNOWN_COMMAND);
}

TEST(aoc_purchase_event_manager)
{
    env e = boot();
    tc_obj m;
    tc_res r;
    CHECK_EQ(tc_sub(e.c, e.aoc, tc_new(100), &m), 0);
    CHECK(m.handle);
    r = tc_send(e.c, m, tc_new(2));
    CHECK_EQ(rc_of(r), 0);
    CHECK_EQ(r.ncopy, 1);
    CHECK_EQ(tc_cmd(e.c, m, 0), 0);
    CHECK_EQ(tc_cmd(e.c, m, 1), 0);
    CHECK_EQ(tc_sub(e.c, e.aoc, tc_new(101), &m), 0);
    CHECK(m.handle);
}

TEST(aoc_listed_index_opens_its_romfs)
{
    env e = boot();
    char root[512];
    tc_obj proxy, st;
    tc_res r;
    uint64_t id;
    make_root(root, sizeof root);
    make_addon(root, BASE + 1, true);
    make_addon(root, BASE + 2, true);
    rl_hos_fs_set_addons(root);
    proxy = tc_open_service(e.c, e.heap, "fsp-srv");
    CHECK(proxy.handle);

    r = base_id_call(&e);
    id = tc_u64(&r, 0);
    r = list_call(&e, 3, 0, 8, 32, 0);
    CHECK_EQ(tc_u32(&r, 0), 2);
    id += list_buf(&e)[1];
    CHECK_EQ(id, BASE + 2);

    {
        uint64_t in[2] = { 5, id };
        tc_req q = tc_new(202);
        tc_put(&q, in, sizeof in);
        CHECK_EQ(tc_sub(e.c, proxy, q, &st), 0);
    }
    r = tc_send(e.c, st, tc_new(4));
    CHECK_EQ(rc_of(r), 0);
    CHECK(tc_u64(&r, 0) > 0x50);
    {
        tc_req q = tc_new(0);
        tc_put64(&q, 0);
        tc_put64(&q, 0x50);
        q.nb = 1;
        q.b_addr[0] = e.heap + 0x10000;
        q.b_size[0] = 0x50;
        CHECK_EQ(rc_of(tc_send(e.c, st, q)), 0);
        CHECK_EQ(*(uint64_t *)(void *)(t_mem() + e.heap + 0x10000), 0x50);
    }
    {
        uint64_t in[2] = { 5, BASE + 3 };
        tc_req q = tc_new(202);
        tc_obj none;
        tc_put(&q, in, sizeof in);
        CHECK_EQ(tc_sub(e.c, proxy, q, &none), RL_MAKE_RESULT(2, 1002));
    }
    rl_hos_fs_set_addons(NULL);
    tree_remove(root);
}
