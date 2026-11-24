/* Sockets: a socketpair passing a descriptor, TCP and UDP over loopback,
 * an AF_UNIX socket bound to a path, options and a child on the far end. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

static void pass_fd(void)
{
    int sv[2], p[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) || pipe(p))
        return;
    if (write(p[1], "through", 7) != 7)
        return;
    char byte = 'x';
    struct iovec iov = { &byte, 1 };
    char cbuf[CMSG_SPACE(sizeof(int))];
    memset(cbuf, 0, sizeof cbuf);
    struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cbuf, .msg_controllen = sizeof cbuf };
    struct cmsghdr *c = CMSG_FIRSTHDR(&m);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &p[0], sizeof(int));
    printf("sendmsg %zd\n", sendmsg(sv[0], &m, 0));
    char got = 0;
    struct iovec riov = { &got, 1 };
    char rbuf[CMSG_SPACE(sizeof(int))];
    struct msghdr r = { .msg_iov = &riov, .msg_iovlen = 1, .msg_control = rbuf, .msg_controllen = sizeof rbuf };
    ssize_t n = recvmsg(sv[1], &r, 0);
    struct cmsghdr *rc = CMSG_FIRSTHDR(&r);
    int fd = -1;
    if (rc && rc->cmsg_type == SCM_RIGHTS)
        memcpy(&fd, CMSG_DATA(rc), sizeof fd);
    char buf[16] = { 0 };
    ssize_t k = fd >= 0 ? read(fd, buf, sizeof buf - 1) : -1;
    printf("recvmsg %zd '%c', passed fd is new %d, reads %zd '%s'\n", n, got, fd >= 0 && fd != p[0], k, buf);
}

static void tcp(void)
{
    int ls = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1) || getsockname(ls, (struct sockaddr *)&a, &al))
        return;
    printf("tcp listening on a port: %d\n", ntohs(a.sin_port) != 0);
    pid_t pid = fork();
    if (pid == 0) {
        int cs = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(cs, (struct sockaddr *)&a, sizeof a))
            _exit(1);
        setsockopt(cs, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
        const char msg[] = "request from the child";
        if (send(cs, msg, sizeof msg, 0) != (ssize_t)sizeof msg)
            _exit(2);
        char reply[32] = { 0 };
        ssize_t n = recv(cs, reply, 5, MSG_WAITALL);
        _exit(n == 5 && strcmp(reply, "reply") == 0 ? 0 : 3);
    }
    struct sockaddr_in peer;
    socklen_t pl = sizeof peer;
    int as = accept4(ls, (struct sockaddr *)&peer, &pl, SOCK_CLOEXEC);
    char buf[64] = { 0 };
    ssize_t n = recv(as, buf, sizeof buf, 0);
    printf("accepted from loopback %d, got %zd '%s'\n", peer.sin_addr.s_addr == htonl(INADDR_LOOPBACK), n, buf);
    if (write(as, "reply", 5) != 5)
        return;
    int st;
    waitpid(pid, &st, 0);
    printf("tcp child exited %d\n", WEXITSTATUS(st));
    int type = 0;
    socklen_t tl = sizeof type;
    getsockopt(as, SOL_SOCKET, SO_TYPE, &type, &tl);
    printf("SO_TYPE stream %d, read after peer closed %zd\n", type == SOCK_STREAM, read(as, buf, sizeof buf));
    close(as);
    close(ls);
}

static void udp(void)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0), t = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t al = sizeof a;
    if (bind(s, (struct sockaddr *)&a, sizeof a) || getsockname(s, (struct sockaddr *)&a, &al))
        return;
    sendto(t, "datagram", 8, 0, (struct sockaddr *)&a, sizeof a);
    char buf[4];
    struct sockaddr_in from;
    socklen_t fl = sizeof from;
    ssize_t n = recvfrom(s, buf, sizeof buf, MSG_TRUNC, (struct sockaddr *)&from, &fl);
    printf("udp truncated datagram length %zd from loopback %d\n", n, from.sin_addr.s_addr == htonl(INADDR_LOOPBACK));
    struct timeval tv = { 0, 50000 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    errno = 0;
    n = recv(s, buf, sizeof buf, 0);
    printf("udp recv with a timeout: %zd %s\n", n, errno == EAGAIN ? "EAGAIN" : strerror(errno));
}

static void unix_path(void)
{
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    snprintf(a.sun_path, sizeof a.sun_path, "%s/sock", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    unlink(a.sun_path);
    int ls = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(ls, (struct sockaddr *)&a, sizeof a) || listen(ls, 1))
        return;
    int cs = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int rc = connect(cs, (struct sockaddr *)&a, sizeof a);
    struct pollfd pf = { ls, POLLIN, 0 };
    printf("unix connect %d, listener readable %d\n", rc, poll(&pf, 1, 1000) == 1 && (pf.revents & POLLIN));
    int as = accept(ls, NULL, NULL);
    struct sockaddr_un got;
    socklen_t gl = sizeof got;
    getsockname(ls, (struct sockaddr *)&got, &gl);
    printf("unix name matches %d\n", strcmp(got.sun_path, a.sun_path) == 0);
    char b;
    errno = 0;
    printf("nonblocking read on empty: %zd %s\n", read(cs, &b, 1), errno == EAGAIN ? "EAGAIN" : strerror(errno));
    close(as);
    close(cs);
    close(ls);
    unlink(a.sun_path);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    pass_fd();
    tcp();
    udp();
    unix_path();
    errno = 0;
    printf("bad family: %d %s\n", socket(12345, SOCK_STREAM, 0), errno == EAFNOSUPPORT ? "EAFNOSUPPORT" : strerror(errno));
    return 0;
}
