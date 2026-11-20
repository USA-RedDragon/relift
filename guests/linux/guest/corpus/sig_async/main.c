#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <ucontext.h>
#include <unistd.h>

static volatile int hits;
static volatile int stop;
static volatile int redirected;
static pid_t main_tid;
static int mode;

static void landing(void)
{
    redirected = 1;
    printf("landed after %d hit, redirected %d\n", hits, redirected);
    printf("done\n");
    fflush(stdout);
    _exit(7);
}

static void handler(int sig, siginfo_t *si, void *uc)
{
    (void)sig;
    (void)si;
    hits++;
    if (mode == 2) {
        ucontext_t *u = uc;
        u->uc_mcontext.pc = (unsigned long long)(void *)landing;
    } else {
        stop = 1;
    }
}

static void *sender(void *arg)
{
    (void)arg;
    struct timespec t = { 0, 50 * 1000000L };
    nanosleep(&t, NULL);
    syscall(SYS_tgkill, getpid(), main_tid, SIGUSR1);
    return NULL;
}

static void run(int m)
{
    mode = m;
    stop = 0;
    hits = 0;
    redirected = 0;
    pthread_t th;
    pthread_create(&th, NULL, sender, NULL);
    unsigned long n = 0;
    if (m == 2) {
        for (;;)
            n++;
    }
    while (!stop)
        n++;
    pthread_join(th, NULL);
    printf("mode %d: hits %d redirected %d spun %d\n", m, hits, redirected, n > 0);
}

static volatile unsigned long saved_sp;

int main(void)
{
    __asm__ volatile("mov x9, sp\n str x9, [%0]\n ldr x10, [%0]\n mov sp, x10" ::"r"(&saved_sp) : "x9", "x10", "memory");
    main_tid = syscall(SYS_gettid);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = handler;
    sa.sa_flags = SA_SIGINFO;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    run(1);
    run(1);
    run(2);
    return 0;
}
