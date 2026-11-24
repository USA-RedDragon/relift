/**
 * @file hostproc_posix.c
 * @brief Processes on a POSIX host: fork, wait, kill, exec with a chosen
 * descriptor set, the signal numbers of the host, and the pipe the signals
 * a guest takes from outside arrive on.
 */
#include "hostio.h"

#if !defined(_WIN32)

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/syscall.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

int lx_errno(int e);

static int64_t fail(void) { return -(int64_t)lx_errno(errno); }

/** @brief Linux signal numbers (1 to 31) by host signal name. */
static const struct {
    int linux_sig;
    int host;
} sigmap[] = {
    { 1, SIGHUP },   { 2, SIGINT },   { 3, SIGQUIT },  { 4, SIGILL },    { 5, SIGTRAP },  { 6, SIGABRT },
    { 7, SIGBUS },   { 8, SIGFPE },   { 9, SIGKILL },  { 10, SIGUSR1 },  { 11, SIGSEGV }, { 12, SIGUSR2 },
    { 13, SIGPIPE }, { 14, SIGALRM }, { 15, SIGTERM }, { 17, SIGCHLD },  { 18, SIGCONT }, { 19, SIGSTOP },
    { 20, SIGTSTP }, { 21, SIGTTIN }, { 22, SIGTTOU }, { 23, SIGURG },   { 24, SIGXCPU }, { 25, SIGXFSZ },
    { 26, SIGVTALRM }, { 27, SIGPROF }, { 28, SIGWINCH }, { 29, SIGIO }, { 31, SIGSYS },
};

void hio_unsetenv(const char *name) { unsetenv(name); }

int hio_sig_to_host(int sig)
{
    for (size_t i = 0; i < sizeof sigmap / sizeof *sigmap; i++)
        if (sigmap[i].linux_sig == sig)
            return sigmap[i].host;
#if defined(__linux__)
    if (sig >= 32 && sig <= 64)
        return sig;
#endif
    return 0;
}

int hio_sig_from_host(int sig)
{
    for (size_t i = 0; i < sizeof sigmap / sizeof *sigmap; i++)
        if (sigmap[i].host == sig)
            return sigmap[i].linux_sig;
#if defined(__linux__)
    if (sig >= 32 && sig <= 64)
        return sig;
#endif
    return 0;
}

int64_t hio_fork(void)
{
    pid_t p = fork();
    return p < 0 ? fail() : (int64_t)p;
}

int64_t hio_wait4(int32_t pid, int32_t *status, uint32_t options, int64_t cpu_ns[2])
{
    int o = WNOHANG;
    if (options & 2)
        o |= WUNTRACED;
    if (options & 8)
        o |= WCONTINUED;
    int st = 0;
    struct rusage ru;
    memset(&ru, 0, sizeof ru);
    pid_t r;
    do
        r = wait4((pid_t)pid, &st, o, &ru);
    while (r < 0 && errno == EINTR);
    if (r < 0)
        return fail();
    if (r == 0)
        return 0;
    int32_t s = 0;
    if (WIFEXITED(st))
        s = (WEXITSTATUS(st) & 0xff) << 8;
    else if (WIFSIGNALED(st))
        s = (hio_sig_from_host(WTERMSIG(st)) & 0x7f) | (WCOREDUMP(st) ? 0x80 : 0);
    else if (WIFSTOPPED(st))
        s = (hio_sig_from_host(WSTOPSIG(st)) << 8) | 0x7f;
#ifdef WIFCONTINUED
    else if (WIFCONTINUED(st))
        s = 0xffff;
#endif
    if (status)
        *status = s;
    if (cpu_ns) {
        cpu_ns[0] = (int64_t)ru.ru_utime.tv_sec * 1000000000ll + (int64_t)ru.ru_utime.tv_usec * 1000;
        cpu_ns[1] = (int64_t)ru.ru_stime.tv_sec * 1000000000ll + (int64_t)ru.ru_stime.tv_usec * 1000;
    }
    return r;
}

int64_t hio_kill(int32_t pid, int sig)
{
    int h = sig ? hio_sig_to_host(sig) : 0;
    if (sig && !h)
        return -L_EINVAL;
    return kill((pid_t)pid, h) < 0 ? fail() : 0;
}

int64_t hio_setsid(void)
{
    pid_t r = setsid();
    return r < 0 ? fail() : (int64_t)r;
}

int64_t hio_setpgid(int32_t pid, int32_t pgid) { return setpgid((pid_t)pid, (pid_t)pgid) < 0 ? fail() : 0; }

int64_t hio_getpgid(int32_t pid)
{
    pid_t r = getpgid((pid_t)pid);
    return r < 0 ? fail() : (int64_t)r;
}

int64_t hio_getsid(int32_t pid)
{
    pid_t r = getsid((pid_t)pid);
    return r < 0 ? fail() : (int64_t)r;
}

bool hio_self_exe(char *buf, size_t n)
{
#if defined(__APPLE__)
    uint32_t len = (uint32_t)n;
    char raw[PATH_MAX];
    uint32_t rl = sizeof raw;
    if (_NSGetExecutablePath(raw, &rl) != 0)
        return false;
    (void)len;
    return hio_realpath(raw, buf, n);
#else
    ssize_t r = readlink("/proc/self/exe", buf, n - 1);
    if (r <= 0)
        return false;
    buf[r] = 0;
    return true;
#endif
}

bool hio_realpath(const char *path, char *buf, size_t n)
{
    char tmp[PATH_MAX];
    if (!realpath(path, tmp) || strlen(tmp) >= n)
        return false;
    strcpy(buf, tmp);
    return true;
}

/** @brief The number one above the highest descriptor that may be open. */
static int fd_limit(void)
{
    struct rlimit r;
    if (getrlimit(RLIMIT_NOFILE, &r) == 0 && r.rlim_cur != RLIM_INFINITY && r.rlim_cur < 1 << 20)
        return (int)r.rlim_cur;
    return 1 << 16;
}

int64_t hio_exec(const char *exe, char *const argv[], char *const envp[], const int (*keep)[2], int n)
{
    if (access(exe, X_OK) != 0)
        return fail();
    int top = 0;
    for (int i = 0; i < n; i++) {
        if (keep[i][0] >= top)
            top = keep[i][0] + 1;
        if (keep[i][1] >= top)
            top = keep[i][1] + 1;
    }
    int *tmp = calloc((size_t)n + 1, sizeof *tmp);
    if (!tmp)
        return -L_ENOMEM;
    for (int i = 0; i < n; i++) {
        tmp[i] = fcntl(keep[i][1], F_DUPFD_CLOEXEC, top);
        if (tmp[i] < 0) {
            int64_t e = fail();
            for (int k = 0; k < i; k++)
                close(tmp[k]);
            free(tmp);
            return e;
        }
    }
    /* No way back from here: descriptors are renumbered. */
    int lim = fd_limit();
#if defined(__linux__) && defined(SYS_close_range)
    bool ranged = syscall(SYS_close_range, 0u, ~0u, 4u /* CLOSE_RANGE_CLOEXEC */) == 0;
#else
    bool ranged = false;
#endif
    if (!ranged)
        for (int fd = 0; fd < lim; fd++)
            fcntl(fd, F_SETFD, FD_CLOEXEC);
    for (int i = 0; i < n; i++)
        if (dup2(tmp[i], keep[i][0]) < 0)
            _exit(127);
    execve(exe, argv, envp);
    fprintf(stderr, "relift: execve of %s failed after its descriptors moved: %s\n", exe, strerror(errno));
    _exit(127);
}

static int g_sig_pipe[2] = { -1, -1 };

/** @brief The host signals routed to the guest. */
static const int routed[] = { SIGHUP, SIGINT, SIGQUIT, SIGUSR1, SIGUSR2, SIGTERM, SIGCHLD, SIGWINCH };

static void on_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    int saved = errno;
    hio_sig_note n = { hio_sig_from_host(sig), si ? (int32_t)si->si_pid : 0, si ? (uint32_t)si->si_uid : 0, 0 };
    if (si && sig == SIGCHLD)
        n.code = si->si_code == CLD_EXITED ? 1 : si->si_code == CLD_KILLED ? 2 : si->si_code == CLD_DUMPED ? 3
                 : si->si_code == CLD_STOPPED ? 5 : si->si_code == CLD_CONTINUED ? 6 : 4;
    if (g_sig_pipe[1] >= 0) {
        ssize_t w = write(g_sig_pipe[1], &n, sizeof n);
        (void)w;
    }
    errno = saved;
}

int64_t hio_signal_pipe(void)
{
    sigset_t set;
    sigemptyset(&set);
    for (size_t i = 0; i < sizeof routed / sizeof *routed; i++)
        sigaddset(&set, routed[i]);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    if (g_sig_pipe[0] >= 0) {
        close(g_sig_pipe[0]);
        close(g_sig_pipe[1]);
    }
    if (pipe(g_sig_pipe) < 0)
        return fail();
    for (int i = 0; i < 2; i++)
        fcntl(g_sig_pipe[i], F_SETFD, FD_CLOEXEC);
    fcntl(g_sig_pipe[1], F_SETFL, O_NONBLOCK);
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigfillset(&sa.sa_mask);
    for (size_t i = 0; i < sizeof routed / sizeof *routed; i++)
        sigaction(routed[i], &sa, NULL);
    return g_sig_pipe[0];
}

void hio_signal_accept(void)
{
    sigset_t set;
    sigemptyset(&set);
    for (size_t i = 0; i < sizeof routed / sizeof *routed; i++)
        sigaddset(&set, routed[i]);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

void hio_die_by_signal(int sig)
{
    int h = hio_sig_to_host(sig);
    if (!h)
        return;
    fflush(stdout);
    fflush(stderr);
    signal(h, SIG_DFL);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, h);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
    raise(h);
}

#endif
