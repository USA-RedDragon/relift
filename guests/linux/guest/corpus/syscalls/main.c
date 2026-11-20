#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

int main(void)
{
    struct utsname u;
    uname(&u);
    printf("uname: %s %s\n", u.sysname, u.machine);
    printf("pid>0=%d ppid>=0=%d\n", getpid() > 0, getppid() >= 0);
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    clock_gettime(CLOCK_MONOTONIC, &b);
    printf("monotonic ordered=%d\n", b.tv_sec > a.tv_sec || (b.tv_sec == a.tv_sec && b.tv_nsec >= a.tv_nsec));
    clock_gettime(CLOCK_REALTIME, &a);
    printf("realtime after 2020=%d\n", a.tv_sec > 1577836800);
    struct timespec ns = { 0, 2000000 };
    clock_gettime(CLOCK_MONOTONIC, &a);
    nanosleep(&ns, NULL);
    clock_gettime(CLOCK_MONOTONIC, &b);
    long us = (b.tv_sec - a.tv_sec) * 1000000L + (b.tv_nsec - a.tv_nsec) / 1000;
    printf("slept at least 2ms=%d\n", us >= 2000);
    unsigned char r1[32], r2[32];
    printf("getrandom=%zd %zd differ=%d\n", getrandom(r1, 32, 0), getrandom(r2, 32, 0), memcmp(r1, r2, 32) != 0);
    struct winsize ws;
    int rc = ioctl(1, TIOCGWINSZ, &ws);
    printf("ioctl winsz on a pipe: rc=%d isatty=%d\n", rc, isatty(1));
    struct iovec iov[3] = { { "one ", 4 }, { "two ", 4 }, { "three\n", 6 } };
    fflush(stdout);
    ssize_t w = writev(1, iov, 3);
    printf("writev=%zd\n", w);
    char *p = mmap(NULL, 1 << 20, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    printf("mmap ok=%d zero=%d\n", p != MAP_FAILED, p[12345] == 0);
    memset(p, 0x5a, 1 << 20);
    mprotect(p, 4096, PROT_READ);
    printf("mprotect ok, byte=%x\n", p[100]);
    munmap(p + 4096, 4096);
    char *q = mmap(p + 4096, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    printf("fixed remap zeroed=%d same=%d\n", q[0] == 0, q == p + 4096);
    munmap(p, 1 << 20);
    unsigned long b0 = (unsigned long)syscall(SYS_brk, 0);
    unsigned long b1 = (unsigned long)syscall(SYS_brk, b0 + 65536);
    memset((void *)b0, 1, 65536);
    printf("brk grew=%d\n", b1 == b0 + 65536);
    int fds[2];
    pipe(fds);
    write(fds[1], "pipe data", 9);
    char buf[16] = { 0 };
    struct iovec rv[2] = { { buf, 4 }, { buf + 4, 8 } };
    ssize_t n = readv(fds[0], rv, 2);
    printf("readv=%zd <%s>\n", n, buf);
    close(fds[0]);
    close(fds[1]);
    int d = dup(1);
    printf("dup>2=%d\n", d > 2);
    close(d);
    printf("write to closed fd: %zd errno=%d\n", write(d, "x", 1), errno);
    struct rlimit rl;
    getrlimit(RLIMIT_STACK, &rl);
    printf("stack limit >= 1MiB: %d\n", rl.rlim_cur >= (1 << 20));
    printf("sysconf pagesize=%ld\n", sysconf(_SC_PAGESIZE));
    printf("getuid consistent=%d\n", getuid() == geteuid());
    printf("syscall(getpid)=%d\n", (int)syscall(SYS_getpid) == getpid());
    char cwd[512];
    printf("getcwd ok=%d\n", getcwd(cwd, sizeof cwd) != NULL);
    printf("exit_group next\n");
    return 0;
}
