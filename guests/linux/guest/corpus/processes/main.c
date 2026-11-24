/* fork, vfork, execve of the program itself, wait statuses, kill of a
 * child, a pipe between parent and child and close-on-exec. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static char self[4096];

static void report(const char *what, int st)
{
    if (WIFEXITED(st))
        printf("%s: exited %d\n", what, WEXITSTATUS(st));
    else if (WIFSIGNALED(st))
        printf("%s: killed by %d\n", what, WTERMSIG(st));
    else
        printf("%s: status %#x\n", what, st);
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "child") == 0) {
        /* The exec'd copy: fd 5 was passed, fd 6 was close-on-exec. */
        printf("exec child: argv[2]=%s env=%s fd5=%d fd6=%d\n", argc > 2 ? argv[2] : "-", getenv("CORPUS_VAR") ? getenv("CORPUS_VAR") : "-",
               fcntl(5, F_GETFD) >= 0, fcntl(6, F_GETFD) >= 0);
        if (fcntl(5, F_GETFD) >= 0) {
            char b[8] = { 0 };
            ssize_t n = read(5, b, sizeof b - 1);
            printf("exec child read %zd: %s\n", n, b);
        }
        fflush(stdout);
        return 42;
    }
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    if (n <= 0)
        return 1;
    self[n] = 0;
    setvbuf(stdout, NULL, _IONBF, 0);

    int p[2];
    if (pipe(p) != 0)
        return 1;
    pid_t pid = fork();
    if (pid == 0) {
        close(p[0]);
        const char msg[] = "hello from the child";
        if (write(p[1], msg, sizeof msg) < 0)
            _exit(2);
        _exit(7);
    }
    close(p[1]);
    char buf[64] = { 0 };
    ssize_t got = read(p[0], buf, sizeof buf);
    close(p[0]);
    int st;
    printf("fork: read %zd '%s', waited %d\n", got, buf, waitpid(pid, &st, 0) == pid);
    report("fork child", st);

    pid = fork();
    if (pid == 0) {
        pause();
        _exit(0);
    }
    usleep(50000);
    kill(pid, SIGTERM);
    waitpid(pid, &st, 0);
    report("killed child", st);

    pid = fork();
    if (pid == 0) {
        signal(SIGTERM, SIG_IGN);
        raise(SIGTERM);
        abort();
    }
    waitpid(pid, &st, 0);
    report("aborting child", st);

    int q[2];
    if (pipe(q) != 0)
        return 1;
    if (q[0] != 5) {
        dup2(q[0], 5);
        close(q[0]);
    }
    int cx = open("/proc/self/exe", O_RDONLY | O_CLOEXEC);
    if (cx >= 0 && cx != 6) {
        dup2(cx, 6);
        fcntl(6, F_SETFD, FD_CLOEXEC);
        close(cx);
    }
    if (write(q[1], "pass", 4) != 4)
        return 1;
    pid = vfork();
    if (pid == 0) {
        char *args[] = { self, "child", "two words", NULL };
        char *env[] = { "CORPUS_VAR=set", NULL };
        execve(self, args, env);
        _exit(127);
    }
    waitpid(pid, &st, 0);
    report("vfork exec child", st);

    pid = vfork();
    if (pid == 0) {
        char *args[] = { "/nonexistent/program", NULL };
        execve(args[0], args, environ);
        _exit(127);
    }
    waitpid(pid, &st, 0);
    report("vfork failed exec", st);

    printf("no children left: %d\n", waitpid(-1, &st, WNOHANG) == -1);
    return 0;
}
