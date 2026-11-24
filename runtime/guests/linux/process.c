/**
 * @file process.c
 * @brief Processes: fork and vfork over a host fork of the whole relift
 * process, execve of the program itself over a host exec of the relift
 * binary, wait4 over the host's, sessions and process groups, and the
 * helper thread that routes the host signals a guest takes from outside
 * (a child's SIGCHLD, a terminal's SIGINT, another process's kill).
 *
 * A guest's process id is its host process id, so a child's pid, wait4's
 * answer and kill of another process are the host's.
 *
 * A host fork copies only the calling thread: the forking thread takes the
 * Linux layer's locks, the space's and the log's first (no other thread
 * holds one mid-change when the process is copied) and the child makes
 * them new, keeps only its own thread, a new wake handle and no pending
 * signals, and starts its own helper thread (the parent's does not exist
 * in it). A vfork's parent thread stays blocked until the child execs or
 * ends: it reads a close-on-exec pipe whose write end only the child holds.
 *
 * execve runs only the program this binary was built from (its own path,
 * or a link to it under the guest root): the relift binary starts again
 * with the new arguments and environment and gets the descriptors that are
 * not close-on-exec at the same numbers, the signal mask and the ignored
 * signals through RELIFT_LINUX_EXEC, which the new process reads and
 * removes. Any other program finds nothing (ENOENT), and the path is
 * logged.
 */
#include "hostio.h"
#include "linux_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    L_CLONE_VFORK = 0x4000,
    FORK_FLAGS = 0xff | L_CLONE_VM | L_CLONE_VFORK | L_CLONE_SETTLS | L_CLONE_PARENT_SETTID | L_CLONE_CHILD_CLEARTID |
                 L_CLONE_DETACHED | L_CLONE_CHILD_SETTID,
    PATH_CAP = 4096,
    MAX_ARGS = 1 << 16,
    MAX_ARG_BYTES = 1 << 22,
};

#define EXEC_ENV "RELIFT_LINUX_EXEC"

/** @brief The relift binary's host path, for execve. */
static char g_self[PATH_CAP];
/** @brief The canonical host path of the program's executable, what an execve must name. */
static char g_prog[PATH_CAP];
/** @brief The read end of the routed host signals' pipe, -1 for none. */
static int g_sig_fd = -1;
/** @brief The helper thread's wake handle (a timer change kicks it). */
static void *g_helper_waker;


static void route(const hio_sig_note *n)
{
    if (n->sig == L_SIGCHLD)
        lx_child_event();
    lx_signal_post(n->sig, n->code ? n->code : L_SI_USER, n->pid, n->uid);
}

/** @brief Set to make the helper thread return (before an execve renumbers descriptors); it sets g_helper_gone when it has. */
static int g_helper_stop;
static int g_helper_gone;

static void helper_body(struct rl_thread *rt)
{
    (void)rt;
    hio_signal_accept();
    for (;;) {
        if (__atomic_load_n(&g_helper_stop, __ATOMIC_ACQUIRE)) {
            __atomic_store_n(&g_helper_gone, 1, __ATOMIC_RELEASE);
            return;
        }
        int64_t left = lx_itimer_run();
        int64_t r = hio_wait(g_sig_fd, L_POLLIN, left, g_helper_waker);
        if (r == -L_EINTR) {
            hio_waker_clear(g_helper_waker);
            continue;
        }
        if (r > 0 && g_sig_fd >= 0) {
            hio_sig_note notes[32];
            int64_t n = hio_read(g_sig_fd, notes, sizeof notes);
            if (n <= 0 && n != -L_EINTR && n != -L_EAGAIN)
                return;
            for (int64_t i = 0; n > 0 && i < n / (int64_t)sizeof *notes; i++)
                route(&notes[i]);
        }
    }
}

/** @brief Starts the helper thread (in a new process or a forked child). */
static int32_t g_started_in;

static void start_helper(void)
{
    if (g_started_in == hio_pid())
        return;
    g_started_in = hio_pid();
    __atomic_store_n(&g_helper_stop, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_helper_gone, 0, __ATOMIC_RELEASE);
    int64_t fd = hio_signal_pipe();
    g_sig_fd = fd < 0 ? -1 : (int)fd;
    if (g_helper_waker)
        hio_waker_free(g_helper_waker);
    g_helper_waker = hio_waker_new();
    struct rl_thread *rt = rl_thread_alloc();
    if (!rt || rl_thread_spawn(rt, helper_body) != 0) {
        rl_log(RL_LOG_WARN, "linux", "cannot start the signal and timer thread: outside signals and interval timers do not arrive");
        return;
    }
    rl_thread_detach(rt);
}

/**
 * @brief Stops the helper thread and waits until it has returned, so
 * nothing polls or reads the signal pipe while an execve moves guest
 * descriptors onto low numbers (the helper would read a guest pipe that
 * took the signal pipe's number). Up to a second.
 */
static void stop_helper(void)
{
    if (g_started_in != hio_pid())
        return;
    __atomic_store_n(&g_helper_stop, 1, __ATOMIC_RELEASE);
    lx_helper_kick();
    for (int i = 0; i < 1000 && !__atomic_load_n(&g_helper_gone, __ATOMIC_ACQUIRE); i++)
        rl_sleep_ns(1000000);
    g_started_in = 0;
}

void lx_helper_kick(void)
{
    if (g_helper_waker)
        hio_waker_kick(g_helper_waker);
}

/** @brief The value of key= in the exec state string, NULL when absent. */
static const char *field(const char *s, const char *key)
{
    size_t n = strlen(key);
    for (const char *p = s; p && *p; p = strchr(p, ';') ? strchr(p, ';') + 1 : NULL)
        if (strncmp(p, key, n) == 0 && p[n] == '=')
            return p + n + 1;
    return NULL;
}

void lx_process_start(lx_thread *main_thread)
{
    if (!hio_self_exe(g_self, sizeof g_self))
        g_self[0] = 0;
    char hp[PATH_CAP];
    g_prog[0] = 0;
    if (lx.prog && lx_host_path(lx.prog->exec_path, hp, sizeof hp) == 0 && !hio_realpath(hp, g_prog, sizeof g_prog))
        g_prog[0] = 0;
    const char *exec = getenv(EXEC_ENV);
    if (exec) {
        const char *m = field(exec, "mask");
        if (m)
            main_thread->blocked = strtoull(m, NULL, 16) & ~((1ull << (L_SIGKILL - 1)) | (1ull << (L_SIGSTOP - 1)));
        const char *ign = field(exec, "ign");
        uint64_t bits = ign ? strtoull(ign, NULL, 16) : 0;
        for (int s = 1; s <= L_NSIG; s++)
            if (bits & (1ull << (s - 1)))
                lx.sig[s].handler = L_SIG_IGN;
        lx_itimer_restore(field(exec, "it"));
        hio_unsetenv(EXEC_ENV);
    }
    start_helper();
}

int64_t lx_fork(rl_ctx *c, const uint64_t a[6])
{
    uint64_t flags = a[0];
    if ((flags & ~(uint64_t)FORK_FLAGS) || ((flags & L_CLONE_VM) && !(flags & L_CLONE_VFORK))) {
        rl_report_unknown(0x4c58464f524bull, "linux", "clone with flags 0x%llx shares what a host fork cannot", (unsigned long long)flags);
        return -L_EINVAL;
    }
    bool vfork = (flags & L_CLONE_VFORK) != 0;
    int vp[2] = { -1, -1 };
    if (vfork && hio_pipe(vp) < 0)
        return -L_EAGAIN;
    lx_threads_fork(RL_FORK_PREPARE);
    rl_vm_fork(&lx.vm, RL_FORK_PREPARE);
    rl_log_fork(RL_FORK_PREPARE);
    int64_t pid = hio_fork();
    if (pid != 0) {
        rl_log_fork(RL_FORK_PARENT);
        rl_vm_fork(&lx.vm, RL_FORK_PARENT);
        lx_threads_fork(RL_FORK_PARENT);
        if (pid < 0) {
            if (vfork) {
                hio_close(vp[0]);
                hio_close(vp[1]);
            }
            return pid;
        }
        if (vfork) {
            hio_close(vp[1]);
            char b;
            while (hio_read(vp[0], &b, 1) == -L_EINTR)
                ;
            hio_close(vp[0]);
        }
        int32_t p32 = (int32_t)pid;
        if (flags & L_CLONE_PARENT_SETTID)
            lx_put(a[2], &p32, 4);
        return pid;
    }
    rl_log_fork(RL_FORK_CHILD);
    rl_vm_fork(&lx.vm, RL_FORK_CHILD);
    lx_threads_fork(RL_FORK_CHILD);
    lx.pid = hio_pid();
    lx.next_tid = lx.pid + 1;
    lx_thread *me = lx_cur();
    me->tid = lx.pid;
    lx_itimer_fork_child();
    start_helper();
    if (vfork)
        hio_close(vp[0]);
    int32_t t32 = me->tid;
    if (flags & L_CLONE_CHILD_SETTID)
        lx_put(a[4], &t32, 4);
    if (flags & L_CLONE_CHILD_CLEARTID)
        me->clear_child_tid = a[4];
    if (flags & L_CLONE_SETTLS)
        c->tpidr_el0 = a[3];
    if (a[1])
        c->sp = a[1];
    return 0;
}

/** @brief Reads the NULL-terminated array of guest strings at addr into a host array; NULL on a bad address or too much. */
static char **read_strings(uint64_t addr, int64_t *err)
{
    *err = -L_EFAULT;
    size_t n = 0, cap = 16, bytes = 0;
    char **out = calloc(cap, sizeof *out);
    if (!out)
        return NULL;
    for (uint64_t at = addr; addr; at += 8) {
        uint64_t p;
        if (!lx_get(&p, at, 8))
            goto bad;
        if (!p)
            break;
        if (n + 2 >= cap) {
            cap *= 2;
            char **grown = realloc(out, cap * sizeof *out);
            if (!grown)
                goto bad;
            out = grown;
        }
        static char scratch[1 << 17];
        int64_t len = lx_cstr(p, scratch, sizeof scratch);
        if (len < 0)
            goto bad;
        char *s = malloc((size_t)len + 1);
        if (!s)
            goto bad;
        memcpy(s, scratch, (size_t)len);
        s[len] = 0;
        out[n++] = s;
        bytes += (size_t)len + 1;
        if (n > MAX_ARGS || bytes > MAX_ARG_BYTES) {
            *err = -L_E2BIG;
            goto bad;
        }
    }
    out[n] = NULL;
    return out;
bad:
    for (size_t i = 0; i < n; i++)
        free(out[i]);
    free(out);
    return NULL;
}

static void free_strings(char **v)
{
    for (size_t i = 0; v && v[i]; i++)
        free(v[i]);
    free(v);
}

static int64_t sys_execve(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    char path[PATH_CAP], hp[PATH_CAP], real[PATH_CAP];
    if (lx_cstr(a[0], path, sizeof path) < 0)
        return -L_EFAULT;
    int64_t rc = lx_host_path(path, hp, sizeof hp);
    if (rc < 0)
        return rc;
    if (!hio_realpath(hp, real, sizeof real))
        return -L_ENOENT;
    if (!g_prog[0] || !g_self[0] || strcmp(real, g_prog) != 0) {
        rl_log(RL_LOG_WARN, "linux", "execve of %s: only the program itself (%s) runs here; ENOENT", path,
               lx.prog ? lx.prog->exec_path : "?");
        return -L_ENOENT;
    }
    int64_t err;
    char **argv = read_strings(a[1], &err);
    if (!argv)
        return err;
    char **envp = read_strings(a[2], &err);
    if (!envp) {
        free_strings(argv);
        return err;
    }
    size_t ne = 0;
    for (size_t i = 0; envp[i]; i++)
        if (strncmp(envp[i], EXEC_ENV "=", sizeof EXEC_ENV) != 0)
            envp[ne++] = envp[i];
        else
            free(envp[i]);
    static int keep[LX_MAX_FDS][2];
    int n = lx_fs_exec_fds(keep, LX_MAX_FDS);
    size_t cap = 256 + (size_t)n * 6;
    char *state = malloc(cap);
    if (!state) {
        free_strings(argv);
        free_strings(envp);
        return -L_ENOMEM;
    }
    uint64_t ign = 0;
    for (int s = 1; s <= L_NSIG; s++)
        if (lx.sig[s].handler == L_SIG_IGN)
            ign |= 1ull << (s - 1);
    size_t at = (size_t)snprintf(state, cap, EXEC_ENV "=mask=%llx;ign=%llx;it=", (unsigned long long)lx_cur()->blocked,
                                 (unsigned long long)ign);
    at += (size_t)lx_itimer_save(state + at, cap - at);
    at += (size_t)snprintf(state + at, cap - at, ";fds=");
    for (int i = 0; i < n && at < cap; i++)
        at += (size_t)snprintf(state + at, cap - at, i ? ",%d" : "%d", keep[i][0]);
    envp[ne++] = state;
    envp[ne] = NULL;
    stop_helper();
    rc = hio_exec(g_self, argv, envp, (const int (*)[2])keep, n);
    start_helper();
    free_strings(argv);
    free_strings(envp);
    return rc;
}

static int64_t sys_wait4(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int32_t pid = (int32_t)a[0];
    uint32_t opts = (uint32_t)a[2];
    if (opts & ~(1u | 2u | 8u | 0x20000000u | 0x40000000u | 0x80000000u))
        return -L_EINVAL;
    for (;;) {
        uint64_t gen = lx_child_gen();
        int32_t st = 0;
        int64_t cpu[2] = { 0, 0 };
        int64_t r = hio_wait4(pid, &st, opts, cpu);
        if (r > 0) {
            if (a[1] && !lx_put(a[1], &st, 4))
                return -L_EFAULT;
            if (a[3]) {
                uint64_t ru[18] = { 0 };
                ru[0] = (uint64_t)(cpu[0] / 1000000000);
                ru[1] = (uint64_t)((cpu[0] % 1000000000) / 1000);
                ru[2] = (uint64_t)(cpu[1] / 1000000000);
                ru[3] = (uint64_t)((cpu[1] % 1000000000) / 1000);
                if (!lx_put(a[3], ru, sizeof ru))
                    return -L_EFAULT;
            }
            return r;
        }
        if (r < 0 || (opts & 1))
            return r;
        if (lx_child_wait(gen))
            return -L_ERESTARTSYS;
    }
}

static int64_t sys_setsid(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    (void)a;
    return hio_setsid();
}

static int64_t sys_setpgid(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    return hio_setpgid((int32_t)a[0], (int32_t)a[1]);
}

static int64_t sys_getpgid(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    return hio_getpgid((int32_t)a[0]);
}

static int64_t sys_getsid(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    return hio_getsid((int32_t)a[0]);
}

lx_sys_fn *lx_process_call(uint32_t nr)
{
    switch (nr) {
    case NR_execve: return sys_execve;
    case NR_wait4: return sys_wait4;
    case NR_setsid: return sys_setsid;
    case NR_setpgid: return sys_setpgid;
    case NR_getpgid: return sys_getpgid;
    case NR_getsid: return sys_getsid;
    default: return NULL;
    }
}
