/**
 * @file test_srv_lm.c
 * @brief lm driven through IPC the way the SDK logs: OpenLogger, then Log
 * with a packet in an auto-select buffer.
 */
#include "tc.h"

#include <string.h>
#include <unistd.h>

/** @brief Starts capturing stderr into a pipe; returns the saved descriptor or -1. */
static int capture_begin(int fds[2])
{
    fflush(stderr);
    if (pipe(fds) != 0)
        return -1;
    int saved = dup(2);
    dup2(fds[1], 2);
    return saved;
}

/** @brief Stops capturing, restores stderr and reads what was written into buf. */
static void capture_end(int fds[2], int saved, char *buf, size_t n)
{
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(fds[1]);
    ssize_t got = read(fds[0], buf, n - 1);
    buf[got > 0 ? got : 0] = 0;
    close(fds[0]);
}

TEST(lm_logger_prints_the_packet_message)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    tc_obj lm = tc_open_service(c, heap, "lm");
    CHECK(lm.handle);
    tc_req q = tc_new(0);
    q.send_pid = true;
    tc_put64(&q, 0);
    tc_obj logger;
    CHECK_EQ(tc_sub(c, lm, q, &logger), 0);
    CHECK(logger.handle);

    static const char msg[] = "hello from the sdk\n";
    uint8_t pkt[0x40] = { 0 };
    uint32_t n = 0x18;
    pkt[n++] = 6; /* module */
    pkt[n++] = 3;
    memcpy(pkt + n, "sdk", 3);
    n += 3;
    pkt[n++] = 2; /* message */
    pkt[n++] = sizeof msg - 1;
    memcpy(pkt + n, msg, sizeof msg - 1);
    n += sizeof msg - 1;
    uint32_t payload = n - 0x18;
    memcpy(pkt + 0x14, &payload, 4);
    uint64_t buf = heap + 0x100000;
    memcpy(t_mem() + buf, pkt, n);

    tc_req l = tc_new(0);
    l.a_addr[0] = buf;
    l.a_size[0] = n;
    l.na = 1;
    char out[2048];
    int fds[2];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    tc_res r = tc_send(c, logger, l);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(r.svc_rc, 0);
    CHECK_EQ(r.result, 0);
    CHECK(strstr(out, "[sdk] hello from the sdk") != NULL);
    CHECK_EQ(tc_cmd(c, logger, 1), 0);
}
