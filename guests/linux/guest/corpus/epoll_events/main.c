#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

static int efd;

static void *waker(void *arg)
{
    (void)arg;
    struct timespec t = { 0, 30 * 1000000L };
    nanosleep(&t, NULL);
    uint64_t one = 5;
    write(efd, &one, 8);
    return NULL;
}

int main(void)
{
    int ep = epoll_create1(0);
    efd = eventfd(0, EFD_NONBLOCK);
    struct epoll_event ev = { .events = EPOLLIN, .data.u64 = 0xabcdef };
    printf("add: %d\n", epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev));
    printf("add again: %d errno %s\n", epoll_ctl(ep, EPOLL_CTL_ADD, efd, &ev), errno == EEXIST ? "EEXIST" : "other");

    struct epoll_event out[4];
    printf("empty wait: %d\n", epoll_wait(ep, out, 4, 20));

    pthread_t th;
    pthread_create(&th, NULL, waker, NULL);
    int n = epoll_wait(ep, out, 4, 2000);
    printf("woken: %d events 0x%x data 0x%llx\n", n, n > 0 ? out[0].events : 0, n > 0 ? (unsigned long long)out[0].data.u64 : 0ull);
    pthread_join(th, NULL);
    uint64_t v = 0;
    printf("read: %zd value %llu\n", read(efd, &v, 8), (unsigned long long)v);
    printf("read again: %zd errno %s\n", read(efd, &v, 8), errno == EAGAIN ? "EAGAIN" : "other");

    uint64_t a = 3, b = 4;
    write(efd, &a, 8);
    write(efd, &b, 8);
    read(efd, &v, 8);
    printf("sum: %llu\n", (unsigned long long)v);

    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK);
    struct itimerspec its = { .it_interval = { 0, 0 }, .it_value = { 0, 25 * 1000000L } };
    printf("settime: %d\n", timerfd_settime(tfd, 0, &its, NULL));
    struct epoll_event tev = { .events = EPOLLIN | EPOLLET, .data.u64 = 7 };
    epoll_ctl(ep, EPOLL_CTL_ADD, tfd, &tev);
    n = epoll_wait(ep, out, 4, 2000);
    uint64_t exp = 0;
    ssize_t r = read(tfd, &exp, 8);
    printf("timer: %d events, read %zd expirations %llu data %llu\n", n, r, (unsigned long long)exp, n > 0 ? (unsigned long long)out[0].data.u64 : 0ull);
    printf("del: %d\n", epoll_ctl(ep, EPOLL_CTL_DEL, tfd, NULL));
    printf("del again: %d errno %s\n", epoll_ctl(ep, EPOLL_CTL_DEL, tfd, NULL), errno == ENOENT ? "ENOENT" : "other");

    int p[2];
    pipe(p);
    struct epoll_event pev = { .events = EPOLLIN, .data.u64 = 1 };
    epoll_ctl(ep, EPOLL_CTL_ADD, p[0], &pev);
    write(p[1], "x", 1);
    n = epoll_wait(ep, out, 4, 1000);
    printf("pipe: %d events data %llu\n", n, n > 0 ? (unsigned long long)out[0].data.u64 : 0ull);
    close(p[1]);
    char c;
    read(p[0], &c, 1);
    n = epoll_wait(ep, out, 4, 1000);
    printf("hangup: %d events has HUP %d\n", n, n > 0 && (out[0].events & EPOLLHUP) != 0);
    return 0;
}
