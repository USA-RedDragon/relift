/**
 * @file test_gdb.c
 * @brief The GDB server (core/gdbstub.h) against the test guest: a client on
 * another host thread attaches over TCP, reads the target description, lists
 * the threads, reads and writes registers and guest memory, and detaches,
 * while the guest thread parks at its safe points.
 */
#include "tc.h"

#include "crash.h"
#include "gdbstub.h"

#if defined(RL_DEBUG_STUB)

#include <relift/settings.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

/** @brief The client's view of one session, filled by the client thread for the test to check. */
typedef struct session {
    int port;
    uint64_t heap;
    char xml_head[64], threads[64], qc[32], x0[17], mem[17], wrote[8], memw[8], detach[8];
    bool ok, pages_match;
    volatile int done;
} session;

static int fd_of(int port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < 200; i++) {
        if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0)
            return fd;
        usleep(10000);
    }
    close(fd);
    return -1;
}

/** @brief Sends one packet and reads its reply body into out (acks on both sides). */
static bool ask(int fd, const char *pkt, char *out, size_t n)
{
    char buf[512];
    unsigned sum = 0;
    for (const char *p = pkt; *p; p++)
        sum += (unsigned char)*p;
    int len = snprintf(buf, sizeof buf, "$%s#%02x", pkt, sum & 0xff);
    if (send(fd, buf, (size_t)len, 0) != len)
        return false;
    char c;
    do {
        if (recv(fd, &c, 1, 0) != 1)
            return false;
    } while (c != '$');
    size_t o = 0;
    for (;;) {
        if (recv(fd, &c, 1, 0) != 1)
            return false;
        if (c == '#')
            break;
        if (o + 1 < n)
            out[o++] = c;
    }
    out[o] = 0;
    char cs[2];
    if (recv(fd, cs, 2, MSG_WAITALL) != 2)
        return false;
    return send(fd, "+", 1, 0) == 1;
}

static void *client(void *arg)
{
    session *s = arg;
    char r[8192];
    int fd = fd_of(s->port);
    s->ok = fd >= 0;
    if (s->ok) {
        char m[64];
        /* The server's ack of each packet comes ahead of its reply; ask skips to the '$'. */
        s->ok = ask(fd, "qSupported:multiprocess+", r, sizeof r) && strstr(r, "qXfer:features:read+") &&
                ask(fd, "qXfer:features:read:target.xml:0,ffb", r, sizeof r);
        snprintf(s->xml_head, sizeof s->xml_head, "%.60s", r);
        s->ok = s->ok && ask(fd, "qfThreadInfo", r, sizeof r);
        snprintf(s->threads, sizeof s->threads, "%s", r);
        static char whole[8192], first[64], rest[8192];
        s->ok = s->ok && ask(fd, "qXfer:threads:read::0,1000", whole, sizeof whole) &&
                ask(fd, "qXfer:threads:read::0,20", first, sizeof first) &&
                ask(fd, "qXfer:threads:read::20,1000", rest, sizeof rest);
        s->pages_match = s->ok && first[0] == 'm' && strlen(first) == 33 && strncmp(whole + 1, first + 1, 32) == 0 &&
                         strcmp(whole + 33, rest + 1) == 0;
        s->ok = s->ok && ask(fd, "qC", r, sizeof r);
        snprintf(s->qc, sizeof s->qc, "%s", r);
        s->ok = s->ok && ask(fd, "p0", r, sizeof r);
        snprintf(s->x0, sizeof s->x0, "%s", r);
        snprintf(m, sizeof m, "m%llx,8", (unsigned long long)s->heap);
        s->ok = s->ok && ask(fd, m, r, sizeof r);
        snprintf(s->mem, sizeof s->mem, "%s", r);
        s->ok = s->ok && ask(fd, "P1=efbeadde00000000", r, sizeof r);
        snprintf(s->wrote, sizeof s->wrote, "%s", r);
        snprintf(m, sizeof m, "M%llx,4:0df0ad0b", (unsigned long long)(s->heap + 8));
        s->ok = s->ok && ask(fd, m, r, sizeof r);
        snprintf(s->memw, sizeof s->memw, "%s", r);
        s->ok = s->ok && ask(fd, "D", r, sizeof r);
        snprintf(s->detach, sizeof s->detach, "%s", r);
        close(fd);
    }
    __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

TEST(gdb_server_reads_and_writes_guest_registers_and_memory)
{
    rl_ctx *c = t_boot(NULL);
    session s = { .port = 20000 + (int)(getpid() % 20000) };
    char port[16];
    snprintf(port, sizeof port, "%d", s.port);
    CHECK_EQ(rl_settings_set("debug.gdb", port), RL_SETTING_OK);
    CHECK(rl_gdb_start(rl_arm64_gdb(), hos_gdb_guest()));
    s.heap = t_heap(c, 0x200000);
    CHECK(s.heap != 0);
    uint8_t *m = t_mem();
    rl_st64(m, s.heap, 0x1122334455667788ull);
    c->x[0] = 0x0123456789abcdefull;
    pthread_t th;
    CHECK_EQ(pthread_create(&th, NULL, client, &s), 0);
    /* The guest thread runs on, passing a safe point each round, until the client is done. */
    while (!__atomic_load_n(&s.done, __ATOMIC_ACQUIRE))
        hos_sched_point();
    pthread_join(th, NULL);
    CHECK(s.ok);
    CHECK(strstr(s.xml_head, "<?xml") != NULL);
    CHECK(s.threads[0] == 'm');
    CHECK(s.pages_match);
    CHECK(strncmp(s.qc, "QC", 2) == 0);
    CHECK(strcmp(s.x0, "efcdab8967452301") == 0);
    CHECK(strcmp(s.mem, "8877665544332211") == 0);
    CHECK(strcmp(s.wrote, "OK") == 0);
    CHECK_EQ(c->x[1], 0xdeadbeefull);
    CHECK(strcmp(s.memw, "OK") == 0);
    CHECK_EQ(rl_ld32(m, s.heap + 8), 0x0badf00du);
    CHECK(strcmp(s.detach, "OK") == 0);
}

/**
 * @brief For tools/gdb/stub-test.sh, which runs a real gdb against it: with RELIFT_GDB_TEST_PORT set, serves the
 * guest with known registers and memory until the debugger writes 1 to the word at heap + 16 (up to 30 s);
 * without it, passes at once.
 */
#if defined(__linux__) && defined(__x86_64__)
static uint64_t fake_lifted(rl_ctx *c, uint8_t *mem);
#endif

TEST(gdb_serve_for_a_debugger)
{
    const char *port = getenv("RELIFT_GDB_TEST_PORT");
    if (!port)
        return;
    rl_ctx *c = t_boot(NULL);
    CHECK_EQ(rl_settings_set("debug.gdb", port), RL_SETTING_OK);
    CHECK(rl_gdb_start(rl_arm64_gdb(), hos_gdb_guest()));
    uint64_t heap = t_heap(c, 0x200000);
    uint8_t *m = t_mem();
    rl_st64(m, heap, 0x1122334455667788ull);
    rl_st64(m, heap + 8, 0x99aabbccddeeff00ull);
    c->x[0] = 0x0123456789abcdefull;
    c->x[29] = 0;
    c->sp = heap + 0x1000;
#if defined(__linux__) && defined(__x86_64__)
    /* A lifted function at guest 0x8001000 runs each round, then a store to heap + 0x40: a breakpoint and a watch. */
    static rl_fn_info info[1];
    info[0] = (rl_fn_info){ .guest_addr = 0x8001000, .fn_addr = 0x8001000, .guest_size = 4, .fn = fake_lifted,
                            .host_sym = "fake_lifted", .guest_name = "fake" };
    static rl_fn_map map = { RL_FN_MAP_VERSION, 1, info, "main", "main-0", 0x8000000, NULL, NULL };
    const rl_fn_map *maps[] = { &map };
    CHECK_EQ(rl_fn_maps_register(maps, 1), 0);
    uint64_t (*volatile call)(rl_ctx *, uint8_t *) = fake_lifted;
#endif
    fprintf(stderr, "gdb-serve: heap 0x%llx\n", (unsigned long long)heap);
    for (int i = 0; i < 3000 && rl_ld32(m, heap + 16) != 1; i++) {
        hos_sched_point();
#if defined(__linux__) && defined(__x86_64__)
        uint64_t x2 = c->x[2];
        call(c, m);
        c->x[2] = x2;
        rl_st64(m, heap + 0x40, (uint64_t)i);
#endif
        usleep(10000);
    }
    CHECK_EQ(rl_ld32(m, heap + 16), 1u);
    CHECK_EQ(c->x[2], 0x42ull);
}

#if defined(__linux__) && defined(__x86_64__)

/** @brief A stand-in lifted function: adds 1 to x5. */
__attribute__((noinline)) static uint64_t fake_lifted(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    __asm__ volatile("" ::: "memory");
    c->x[5] += 1;
    return RL_OK;
}

/** @brief What the breakpoint and watchpoint client saw, for the test to check. */
typedef struct traps {
    int port;
    uint64_t heap;
    /** @brief Steps the guest thread takes when the client allows: 1 call the function, 2 store to the watch. */
    volatile int go;
    volatile int done;
    char zb[8], stop_b[64], pc[17], x0[17], zw[8], stop_w[96];
    bool ok;
} traps;

static void *trap_client(void *arg)
{
    traps *s = arg;
    char r[4096], m[96];
    int fd = fd_of(s->port);
    s->ok = fd >= 0;
    if (s->ok) {
        s->ok = ask(fd, "Z0,8001000,4", r, sizeof r);
        snprintf(s->zb, sizeof s->zb, "%s", r);
        __atomic_store_n(&s->go, 1, __ATOMIC_RELEASE);
        /* The continue's answer is the stop reply of the breakpoint. */
        s->ok = s->ok && ask(fd, "vCont;c", r, sizeof r);
        snprintf(s->stop_b, sizeof s->stop_b, "%s", r);
        s->ok = s->ok && ask(fd, "p20", r, sizeof r);
        snprintf(s->pc, sizeof s->pc, "%s", r);
        s->ok = s->ok && ask(fd, "p5", r, sizeof r);
        snprintf(s->x0, sizeof s->x0, "%s", r);
        s->ok = s->ok && ask(fd, "z0,8001000,4", r, sizeof r);
        snprintf(m, sizeof m, "Z2,%llx,8", (unsigned long long)(s->heap + 0x40));
        s->ok = s->ok && ask(fd, m, r, sizeof r);
        snprintf(s->zw, sizeof s->zw, "%s", r);
        __atomic_store_n(&s->go, 2, __ATOMIC_RELEASE);
        s->ok = s->ok && ask(fd, "vCont;c", r, sizeof r);
        snprintf(s->stop_w, sizeof s->stop_w, "%s", r);
        snprintf(m, sizeof m, "z2,%llx,8", (unsigned long long)(s->heap + 0x40));
        s->ok = s->ok && ask(fd, m, r, sizeof r);
        s->ok = s->ok && ask(fd, "D", r, sizeof r);
        close(fd);
    }
    __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

TEST(gdb_server_stops_at_a_breakpoint_and_a_watchpoint)
{
    rl_ctx *c = t_boot(NULL);
    traps s = { .port = 20000 + (int)((getpid() + 7) % 20000) };
    char port[16];
    snprintf(port, sizeof port, "%d", s.port);
    CHECK_EQ(rl_settings_set("debug.gdb", port), RL_SETTING_OK);
    CHECK(rl_gdb_start(rl_arm64_gdb(), hos_gdb_guest()));
    static rl_fn_info info[1];
    info[0] = (rl_fn_info){ .guest_addr = 0x8001000, .fn_addr = 0x8001000, .guest_size = 4, .fn = fake_lifted,
                            .host_sym = "fake_lifted", .guest_name = "fake" };
    static rl_fn_map map = { RL_FN_MAP_VERSION, 1, info, "main", "main-0", 0x8000000, NULL, NULL };
    const rl_fn_map *maps[] = { &map };
    CHECK_EQ(rl_fn_maps_register(maps, 1), 0);
    s.heap = t_heap(c, 0x200000);
    uint8_t *m = t_mem();
    c->x[5] = 41;
    uint64_t (*volatile call)(rl_ctx *, uint8_t *) = fake_lifted;
    pthread_t th;
    CHECK_EQ(pthread_create(&th, NULL, trap_client, &s), 0);
    int step = 0;
    while (!__atomic_load_n(&s.done, __ATOMIC_ACQUIRE)) {
        hos_sched_point();
        int go = __atomic_load_n(&s.go, __ATOMIC_ACQUIRE);
        if (go >= 1 && step < 1) {
            call(c, m);
            step = 1;
        }
        if (go >= 2 && step < 2) {
            rl_st64(m, s.heap + 0x40, 0x5555);
            step = 2;
        }
    }
    pthread_join(th, NULL);
    rl_fn_maps_reset();
    CHECK(s.ok);
    CHECK(strcmp(s.zb, "OK") == 0);
    CHECK(strncmp(s.stop_b, "T05thread:", 10) == 0);
    CHECK(strcmp(s.pc, "0010000800000000") == 0);
    CHECK(strcmp(s.x0, "2900000000000000") == 0);
    CHECK_EQ(c->x[5], 42u);
    CHECK(strcmp(s.zw, "OK") == 0);
    char want[64];
    snprintf(want, sizeof want, "watch:%llx;", (unsigned long long)(s.heap + 0x40));
    CHECK(strstr(s.stop_w, want) != NULL);
    CHECK_EQ(rl_ld64(m, s.heap + 0x40), 0x5555u);
}

/** @brief A write watchpoint hit many times in a row: every continue runs the guest on to its next hit. */
typedef struct repeat {
    int port;
    uint64_t heap;
    int want;
    bool step;
    /** @brief Hits whose stop reply named a thread other than 1. */
    int from_b;
    int seen;
    int bad;
    bool ok;
    volatile int done;
} repeat;

static void *repeat_client(void *arg)
{
    repeat *s = arg;
    char r[4096], m[96], w[48];
    int fd = fd_of(s->port);
    s->ok = fd >= 0;
    if (s->ok) {
        struct timeval tv = { .tv_sec = 15 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        snprintf(m, sizeof m, "Z2,%llx,8", (unsigned long long)(s->heap + 0x40));
        s->ok = ask(fd, m, r, sizeof r) && strcmp(r, "OK") == 0;
        snprintf(w, sizeof w, "watch:%llx;", (unsigned long long)(s->heap + 0x40));
        for (int i = 0; s->ok && i < s->want; i++) {
            s->ok = ask(fd, "vCont;c", r, sizeof r);
            if (s->ok && strstr(r, w)) {
                s->seen++;
                if (strncmp(r, "T05thread:1;", 12) != 0)
                    s->from_b++;
            }
            else
                s->bad++;
            if (s->step && s->ok) {
                /* The way gdb steps over a hit on AArch64, where a watch traps before the store: it lifts the
                 * watch, steps the thread, puts the watch back. */
                snprintf(m, sizeof m, "z2,%llx,8", (unsigned long long)(s->heap + 0x40));
                s->ok = ask(fd, m, r, sizeof r);
                s->ok = s->ok && ask(fd, "vCont;s:1", r, sizeof r) && strncmp(r, "T05", 3) == 0;
                snprintf(m, sizeof m, "Z2,%llx,8", (unsigned long long)(s->heap + 0x40));
                s->ok = s->ok && ask(fd, m, r, sizeof r);
            }
        }
        snprintf(m, sizeof m, "z2,%llx,8", (unsigned long long)(s->heap + 0x40));
        s->ok = s->ok && ask(fd, m, r, sizeof r);
        s->ok = s->ok && ask(fd, "D", r, sizeof r);
        close(fd);
    }
    __atomic_store_n(&s->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/** @brief Which guest threads write the watched word in continue_after_watch_hits. */
enum { WRITER_MAIN = 1, WRITER_B = 2 };

/** @brief Thread B's state: its heap, its writes done, and the thread id its hits named. */
static struct {
    uint64_t heap;
    int rounds;
    volatile int done;
} g_b;

/** @brief Thread B: each round writes another word of the watched page, then the watched word. */
static uint64_t b_writer(rl_ctx *c, uint8_t *mem)
{
    (void)c;
    for (int i = 0; i < g_b.rounds; i++) {
        hos_sched_point();
        rl_st64(mem, g_b.heap + 0x90, (uint64_t)i);
        rl_st64(mem, g_b.heap + 0x40, (uint64_t)i);
        usleep(2000);
    }
    __atomic_store_n(&g_b.done, 1, __ATOMIC_RELEASE);
    return RL_OK;
}

static void continue_after_watch_hits(bool step, int port_bias, int writers)
{
    enum { ROUNDS = 40 };
    rl_ctx *c = t_boot(NULL);
    repeat s = { .port = 20000 + (int)((getpid() + port_bias) % 20000), .want = 6, .step = step };
    char port[16];
    snprintf(port, sizeof port, "%d", s.port);
    CHECK_EQ(rl_settings_set("debug.gdb", port), RL_SETTING_OK);
    CHECK(rl_gdb_start(rl_arm64_gdb(), hos_gdb_guest()));
    s.heap = t_heap(c, 0x200000);
    uint8_t *m = t_mem();
    uint32_t handle = 0;
    if (writers & WRITER_B) {
        rl_fn fn = b_writer;
        uint64_t addr = t_code(0);
        t_guest_fns(&addr, &fn, 1);
        g_b.heap = s.heap;
        g_b.rounds = ROUNDS;
        g_b.done = 0;
        CHECK_EQ(t_svc(c, SVC_CREATE_THREAD, 0, addr, 0, s.heap + 0x100000, 44, 1), 0u);
        handle = (uint32_t)c->x[1];
        CHECK_EQ(t_svc(c, SVC_START_THREAD, handle, 0, 0, 0, 0, 0), 0u);
    }
    pthread_t th;
    CHECK_EQ(pthread_create(&th, NULL, repeat_client, &s), 0);
    int round = 0;
    while (round < ROUNDS) {
        hos_sched_point();
        if (writers & WRITER_MAIN) {
            rl_st64(m, s.heap + 0x80, (uint64_t)round);
            rl_st64(m, s.heap + 0x40, (uint64_t)round);
        }
        round++;
        usleep(2000);
    }
    while (!__atomic_load_n(&s.done, __ATOMIC_ACQUIRE))
        hos_sched_point();
    pthread_join(th, NULL);
    if (writers & WRITER_B) {
        while (!__atomic_load_n(&g_b.done, __ATOMIC_ACQUIRE))
            hos_sched_point();
        CHECK_EQ(rl_ld64(m, s.heap + 0x90), (uint64_t)(ROUNDS - 1));
    }
    CHECK(s.ok);
    CHECK_EQ(s.seen, 6);
    CHECK_EQ(s.bad, 0);
    CHECK_EQ(round, (int)ROUNDS);
    if (writers & WRITER_MAIN)
        CHECK_EQ(rl_ld64(m, s.heap + 0x80), (uint64_t)(ROUNDS - 1));
    if (writers == WRITER_B)
        CHECK_EQ(s.from_b, 6);
}

TEST(gdb_server_continues_after_each_write_watch_hit) { continue_after_watch_hits(false, 13, WRITER_MAIN); }

TEST(gdb_server_steps_over_a_write_watch_hit_and_continues) { continue_after_watch_hits(true, 17, WRITER_MAIN); }

TEST(gdb_server_reports_a_write_watch_hit_of_a_second_thread) { continue_after_watch_hits(false, 23, WRITER_B); }

TEST(gdb_server_continues_after_both_threads_hit_a_write_watch)
{
    continue_after_watch_hits(false, 29, WRITER_MAIN | WRITER_B);
}

TEST(gdb_server_steps_over_hits_of_two_threads) { continue_after_watch_hits(true, 31, WRITER_MAIN | WRITER_B); }
#endif

#endif
