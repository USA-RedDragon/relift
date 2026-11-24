/**
 * @file hostproc_win.c
 * @brief Processes on a Windows host: the host cannot fork, so fork,
 * exec and the routed signals report ENOSYS and the program runs as one
 * process.
 */
#include "hostio.h"

#if defined(_WIN32)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

void hio_unsetenv(const char *name) { _putenv_s(name, ""); }

int hio_sig_to_host(int sig) { return sig; }

int hio_sig_from_host(int sig) { return sig; }

int64_t hio_fork(void) { return -L_ENOSYS; }

int64_t hio_wait4(int32_t pid, int32_t *status, uint32_t options, int64_t cpu_ns[2])
{
    (void)pid;
    (void)status;
    (void)options;
    (void)cpu_ns;
    return -L_ECHILD;
}

int64_t hio_kill(int32_t pid, int sig)
{
    (void)pid;
    (void)sig;
    return -L_ESRCH;
}

int64_t hio_setsid(void) { return -L_EPERM; }

int64_t hio_setpgid(int32_t pid, int32_t pgid)
{
    (void)pid;
    (void)pgid;
    return -L_EPERM;
}

int64_t hio_getpgid(int32_t pid)
{
    (void)pid;
    return (int64_t)GetCurrentProcessId();
}

int64_t hio_getsid(int32_t pid)
{
    (void)pid;
    return (int64_t)GetCurrentProcessId();
}

bool hio_self_exe(char *buf, size_t n)
{
    DWORD r = GetModuleFileNameA(NULL, buf, (DWORD)n);
    return r > 0 && r < n;
}

bool hio_realpath(const char *path, char *buf, size_t n)
{
    DWORD r = GetFullPathNameA(path, (DWORD)n, buf, NULL);
    return r > 0 && r < n && GetFileAttributesA(buf) != INVALID_FILE_ATTRIBUTES;
}

int64_t hio_exec(const char *exe, char *const argv[], char *const envp[], const int (*keep)[2], int n)
{
    (void)exe;
    (void)argv;
    (void)envp;
    (void)keep;
    (void)n;
    return -L_ENOSYS;
}

int64_t hio_signal_pipe(void) { return -L_ENOSYS; }

void hio_signal_accept(void) {}

void hio_die_by_signal(int sig) { (void)sig; }

#endif
