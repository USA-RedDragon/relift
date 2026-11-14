/**
 * @file child.h
 * @brief Runs the test program itself as a child (`<self> child <mode>`)
 * and collects its stdout and stderr and how it ended. For the tests that
 * build on Windows too (crash_test, fiber_tests), which cannot fork.
 */
#ifndef RELIFT_TESTS_CHILD_H
#define RELIFT_TESTS_CHILD_H

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

/** @brief How a child ended. */
typedef struct child_run {
    /** @brief Its stdout and stderr. */
    char out[32768];
    /** @brief POSIX: true when a signal ended it. */
    bool signaled;
    /** @brief The signal (POSIX, when signaled) or the exit code (Windows: the exception code after a crash). */
    unsigned long code;
} child_run;

#if defined(_WIN32)
static int child_spawn(const char *self, const char *mode, child_run *r)
{
    SECURITY_ATTRIBUTES sa = { sizeof sa, NULL, TRUE };
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0))
        return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si;
    memset(&si, 0, sizeof si);
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    si.hStdOutput = wr;
    si.hStdError = wr;
    PROCESS_INFORMATION pi;
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "\"%s\" child %s", self, mode);
    if (!CreateProcessA(NULL, cmd, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi))
        return -1;
    CloseHandle(wr);
    size_t len = 0;
    DWORD got;
    while (len + 1 < sizeof r->out && ReadFile(rd, r->out + len, (DWORD)(sizeof r->out - 1 - len), &got, NULL) && got)
        len += got;
    r->out[len] = 0;
    CloseHandle(rd);
    WaitForSingleObject(pi.hProcess, 30000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    r->signaled = false;
    r->code = code;
    return 0;
}
#else
static int child_spawn(const char *self, const char *mode, child_run *r)
{
    int fds[2];
    if (pipe(fds) != 0)
        return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    char *argv[] = { (char *)self, (char *)"child", (char *)mode, NULL };
    pid_t pid;
    int e = posix_spawn(&pid, self, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (e != 0) {
        close(fds[0]);
        return -1;
    }
    size_t len = 0;
    ssize_t got;
    while (len + 1 < sizeof r->out && (got = read(fds[0], r->out + len, sizeof r->out - 1 - len)) > 0)
        len += (size_t)got;
    r->out[len] = 0;
    close(fds[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    r->signaled = WIFSIGNALED(st);
    r->code = r->signaled ? (unsigned long)WTERMSIG(st) : (unsigned long)WEXITSTATUS(st);
    return 0;
}
#endif

/** @brief True when the child ended in a crash: SIGSEGV, SIGBUS or SIGILL, or the matching Windows exception codes. */
static inline bool child_crashed(const child_run *r)
{
#if defined(_WIN32)
    return r->code == (DWORD)EXCEPTION_ACCESS_VIOLATION || r->code == (DWORD)EXCEPTION_ILLEGAL_INSTRUCTION ||
           r->code == (DWORD)EXCEPTION_STACK_OVERFLOW;
#else
    return r->signaled && (r->code == SIGSEGV || r->code == SIGBUS || r->code == SIGILL);
#endif
}

/** @brief True when the child ended through abort(). */
static inline bool child_aborted(const child_run *r)
{
#if defined(_WIN32)
    return r->code == 3 || r->code == 0xC0000409ul;
#else
    return r->signaled && r->code == SIGABRT;
#endif
}

#endif
