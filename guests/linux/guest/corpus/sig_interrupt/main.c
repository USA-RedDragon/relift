#define _GNU_SOURCE
#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static volatile int hits;
static int word;
static int pfd[2];
static pid_t main_tid;

static void handler(int sig)
{
    (void)sig;
    hits++;
}

static void install(int flags)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sa.sa_flags = flags;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
}

static void ms(int n)
{
    struct timespec t = { 0, n * 1000000L };
    nanosleep(&t, NULL);
}

/** 100 ms in: the signal; 100 ms later: the event the blocked call waits for. */
static void *helper(void *arg)
{
    int what = (int)(long)arg;
    ms(100);
    syscall(SYS_tgkill, getpid(), main_tid, SIGUSR1);
    ms(100);
    if (what == 1)
        write(pfd[1], "ok", 2);
    if (what == 2) {
        __atomic_store_n(&word, 1, __ATOMIC_SEQ_CST);
        syscall(SYS_futex, &word, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
    }
    return NULL;
}

static const char *name(int e) { return e == EINTR ? "EINTR" : e == 0 ? "none" : "other"; }

static void run(const char *label, int what, int flags)
{
    install(flags);
    hits = 0;
    word = 0;
    pthread_t th;
    pthread_create(&th, NULL, helper, (void *)(long)what);
    long r = 0;
    int e = 0;
    struct timespec rem = { 0, 0 };
    if (what == 0) {
        struct timespec req = { 3, 0 };
        r = nanosleep(&req, &rem);
    } else if (what == 1) {
        char b[8];
        r = read(pfd[0], b, sizeof b);
    } else if (what == 2) {
        r = syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    } else {
        struct timespec to = { 3, 0 };
        r = syscall(SYS_futex, &word, FUTEX_WAIT_PRIVATE, 0, &to, NULL, 0);
    }
    if (r < 0)
        e = errno;
    pthread_join(th, NULL);
    printf("%s flags=%s: ret=%ld errno=%s handler_ran=%d", label, flags ? "SA_RESTART" : "none", r < 0 ? -1 : r, name(e), hits);
    if (what == 0)
        printf(" rem_between_2s_and_3s=%d", rem.tv_sec == 2);
    puts("");
    if (what == 1 && r < 0) {
        char b[8];
        printf("  read after: %zd\n", read(pfd[0], b, sizeof b));
    }
}

int main(void)
{
    main_tid = syscall(SYS_gettid);
    pipe(pfd);
    for (int f = 0; f < 2; f++) {
        int flags = f ? SA_RESTART : 0;
        run("nanosleep", 0, flags);
        run("pipe read", 1, flags);
        run("futex wait", 2, flags);
        run("futex wait with timeout", 3, flags);
    }
    return 0;
}
