/**
 * @file test_gdb.c
 * @brief The GDB server (core/gdbstub.h) against the test guest: a client on
 * another host thread attaches over TCP, reads the target description, lists
 * the threads, reads and writes registers and guest memory, and detaches,
 * while the guest thread parks at its safe points.
 */
#include "tc.h"

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
#include <unistd.h>

/** @brief The client's view of one session, filled by the client thread for the test to check. */
typedef struct session {
    int port;
    uint64_t heap;
    char xml_head[64], threads[64], qc[32], x0[17], mem[17], wrote[8], memw[8], detach[8];
    bool ok;
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
    fprintf(stderr, "gdb-serve: heap 0x%llx\n", (unsigned long long)heap);
    for (int i = 0; i < 3000 && rl_ld32(m, heap + 16) != 1; i++) {
        hos_sched_point();
        usleep(10000);
    }
    CHECK_EQ(rl_ld32(m, heap + 16), 1u);
    CHECK_EQ(c->x[2], 0x42ull);
}

#endif
