#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static volatile sig_atomic_t got[65];
static volatile int order;
static int seen[8];

static void handler(int sig)
{
    got[sig]++;
    if (order < 8)
        seen[order++] = sig;
}

static void info_handler(int sig, siginfo_t *si, void *ctx)
{
    (void)ctx;
    got[sig] += 10;
    if (order < 8)
        seen[order++] = si->si_signo * 100 + (si->si_code == SI_USER);
}

int main(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = handler;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR1, &sa, NULL);
    sigaction(SIGUSR2, &sa, NULL);
    raise(SIGUSR1);
    printf("after raise: usr1=%d\n", (int)got[SIGUSR1]);
    sigset_t block, old, pending;
    sigemptyset(&block);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGUSR2);
    sigprocmask(SIG_BLOCK, &block, &old);
    raise(SIGUSR2);
    raise(SIGUSR1);
    raise(SIGUSR1);
    printf("blocked: usr1=%d usr2=%d\n", (int)got[SIGUSR1], (int)got[SIGUSR2]);
    sigpending(&pending);
    printf("pending usr1=%d usr2=%d\n", sigismember(&pending, SIGUSR1), sigismember(&pending, SIGUSR2));
    sigprocmask(SIG_SETMASK, &old, NULL);
    printf("unblocked: usr1=%d usr2=%d\n", (int)got[SIGUSR1], (int)got[SIGUSR2]);
    struct sigaction si;
    memset(&si, 0, sizeof si);
    si.sa_sigaction = info_handler;
    si.sa_flags = SA_SIGINFO | SA_RESETHAND;
    sigemptyset(&si.sa_mask);
    sigaction(SIGUSR1, &si, NULL);
    kill(getpid(), SIGUSR1);
    struct sigaction cur;
    sigaction(SIGUSR1, NULL, &cur);
    printf("siginfo: usr1=%d reset=%d\n", (int)got[SIGUSR1], cur.sa_handler == SIG_DFL);
    signal(SIGUSR2, SIG_IGN);
    raise(SIGUSR2);
    printf("ignored: usr2=%d\n", (int)got[SIGUSR2]);
    for (int i = 0; i < order; i++)
        printf("seen[%d]=%d\n", i, seen[i]);
    return 0;
}
