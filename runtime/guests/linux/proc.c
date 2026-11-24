/**
 * @file proc.c
 * @brief The process, time and information system calls: exit, identity,
 * clocks and sleeping, random bytes, limits, futex for one thread, and the
 * rest of what a libc asks the kernel at start-up.
 */
#include "hostio.h"
#include "compat.h"
#include "linux_priv.h"

#include <string.h>

static char g_comm[16] = "relift";

void lx_proc_init(void)
{
    for (int i = 0; i < L_RLIM_NLIMITS; i++)
        lx.rlim[i][0] = lx.rlim[i][1] = L_RLIM_INFINITY;
    lx.rlim[L_RLIMIT_STACK][0] = lx.stack_hi - lx.stack_lo;
    lx.rlim[L_RLIMIT_CORE][0] = 0;
    lx.rlim[L_RLIMIT_NOFILE][0] = LX_MAX_FDS;
    lx.rlim[L_RLIMIT_NOFILE][1] = LX_MAX_FDS;
    lx.rlim[L_RLIMIT_NPROC][0] = lx.rlim[L_RLIMIT_NPROC][1] = 4096;
    lx.rlim[L_RLIMIT_MEMLOCK][0] = lx.rlim[L_RLIMIT_MEMLOCK][1] = 64 * 1024;
    lx.umask = 022;
    lx.pid = hio_pid();
    lx.next_tid = lx.pid + 1;
}

static int64_t sys_exit_group(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)mem;
    rl_pend_exit_process(c, (uint32_t)a[0] & 0xff);
    return LX_PENDING;
}

static int64_t sys_exit(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)mem;
    lx_mutex_lock(&lx_big);
    lx.exit_status = (uint32_t)a[0] & 0xff;
    int others = lx.live - 1;
    lx_mutex_unlock(&lx_big);
    if (others <= 0) {
        rl_pend_exit_process(c, lx.exit_status);
        return LX_PENDING;
    }
    rl_pend_exit_thread(c);
    return LX_PENDING;
}

static int64_t sys_set_tid_address(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    lx_cur()->clear_child_tid = a[0];
    return lx_cur()->tid;
}

static int64_t sys_getpid(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    (void)a;
    return lx.pid;
}

static int64_t sys_gettid(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    (void)a;
    return lx_cur()->tid;
}

static int64_t sys_getppid(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    (void)a;
    return hio_ppid();
}

static int64_t sys_getuid(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    (void)a;
    return hio_uid();
}

static int64_t sys_getgid(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    (void)a;
    return hio_gid();
}

static int64_t sys_zero(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    (void)a;
    return 0;
}

static int64_t clock_ns(uint32_t clk, int64_t *ns)
{
    switch (clk) {
    case L_CLOCK_REALTIME:
    case L_CLOCK_REALTIME_COARSE:
        *ns = hio_realtime_ns();
        return 0;
    case L_CLOCK_MONOTONIC:
    case L_CLOCK_MONOTONIC_RAW:
    case L_CLOCK_MONOTONIC_COARSE:
    case L_CLOCK_BOOTTIME:
        *ns = (int64_t)rl_clock_ns();
        return 0;
    case L_CLOCK_PROCESS_CPUTIME_ID:
    case L_CLOCK_THREAD_CPUTIME_ID:
        *ns = hio_cputime_ns();
        return 0;
    default:
        return -L_EINVAL;
    }
}

static int64_t put_ns(uint64_t addr, int64_t ns)
{
    return lx_put_timespec(addr, ns / 1000000000, ns % 1000000000) ? 0 : -L_EFAULT;
}

static int64_t sys_clock_gettime(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int64_t ns;
    int64_t rc = clock_ns((uint32_t)a[0], &ns);
    return rc < 0 ? rc : put_ns(a[1], ns);
}

static int64_t sys_clock_getres(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint32_t clk = (uint32_t)a[0];
    if (clk > L_CLOCK_BOOTTIME)
        return -L_EINVAL;
    if (!a[1])
        return 0;
    return put_ns(a[1], clk == L_CLOCK_REALTIME_COARSE || clk == L_CLOCK_MONOTONIC_COARSE ? 4000000 : 1);
}

static int64_t sys_gettimeofday(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if (a[0]) {
        int64_t ns = hio_realtime_ns();
        if (!lx_put_timeval(a[0], ns / 1000000000, (ns % 1000000000) / 1000))
            return -L_EFAULT;
    }
    if (a[1]) {
        int32_t tz[2] = { 0, 0 };
        if (!lx_put(a[1], tz, sizeof tz))
            return -L_EFAULT;
    }
    return 0;
}

/**
 * @brief nanosleep and clock_nanosleep: sleeps until the interval or the
 * absolute time passes. A signal with a handler interrupts it: the call
 * returns -ERESTART_RESTARTBLOCK, which the return path makes -EINTR, and a
 * relative sleep stores the time left in rem first.
 */
static int64_t sleep_for(uint64_t req, uint64_t rem, bool absolute, uint32_t clk)
{
    l_timespec ts;
    if (!lx_get_timespec(req, &ts))
        return -L_EFAULT;
    if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000)
        return -L_EINVAL;
    int64_t ns = ts.sec * 1000000000ll + ts.nsec;
    if (absolute) {
        int64_t now;
        int64_t rc = clock_ns(clk, &now);
        if (rc < 0)
            return rc;
        ns -= now;
    }
    int64_t start = (int64_t)rl_clock_ns();
    if (ns > 0 && lx_wait(-1, 0, ns) == -L_EINTR) {
        if (rem && !absolute) {
            int64_t left = ns - ((int64_t)rl_clock_ns() - start);
            if (!lx_put_timespec(rem, left > 0 ? left / 1000000000 : 0, left > 0 ? left % 1000000000 : 0))
                return -L_EFAULT;
        }
        return -L_ERESTART_RESTARTBLOCK;
    }
    if (rem && !absolute) {
        if (!lx_put_timespec(rem, 0, 0))
            return -L_EFAULT;
    }
    return 0;
}

static int64_t sys_nanosleep(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    return sleep_for(a[0], a[1], false, 0);
}

static int64_t sys_clock_nanosleep(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint32_t clk = (uint32_t)a[0];
    if (clk == L_CLOCK_PROCESS_CPUTIME_ID || clk > L_CLOCK_BOOTTIME)
        return -L_EINVAL;
    return sleep_for(a[2], a[3], (a[1] & L_TIMER_ABSTIME) != 0, clk);
}

static int64_t sys_wait4(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    (void)a;
    return -L_ECHILD;
}

static int64_t sys_sched_yield(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    (void)a;
    rl_host_yield();
    return 0;
}

static int64_t sys_sched_getaffinity(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint64_t size = a[1];
    if (size < 8 || (size & 7) != 0)
        return -L_EINVAL;
    uint32_t n = hio_cpu_count();
    uint64_t mask = n >= 64 ? ~0ull : ((1ull << n) - 1);
    uint8_t buf[128] = { 0 };
    if (size > sizeof buf)
        size = sizeof buf;
    memcpy(buf, &mask, 8);
    return lx_put(a[2], buf, size) ? (int64_t)size : -L_EFAULT;
}

static int64_t sys_uname(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    l_utsname u;
    memset(&u, 0, sizeof u);
    strcpy(u.sysname, "Linux");
    hio_nodename(u.nodename, sizeof u.nodename);
    strcpy(u.release, "6.1.0-relift");
    strcpy(u.version, "#1 SMP relift");
    strcpy(u.machine, "aarch64");
    strcpy(u.domainname, "(none)");
    return lx_put(a[0], &u, sizeof u) ? 0 : -L_EFAULT;
}

static int64_t sys_getrandom(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    uint64_t len = a[1];
    if (a[2] & ~7ull)
        return -L_EINVAL;
    if (len > 33554431u)
        len = 33554431u;
    if (!lx_ok(a[0], len, RL_VM_W))
        return -L_EFAULT;
    if (len && !rl_host_random(mem + a[0], len))
        return -L_EAGAIN;
    return (int64_t)len;
}

static int64_t rlimit_get(uint32_t res, uint64_t out)
{
    if (res >= L_RLIM_NLIMITS)
        return -L_EINVAL;
    return (lx_rlim32 ? lx_put_ulongs(out, lx.rlim[res], 2) : lx_put(out, lx.rlim[res], 16)) ? 0 : -L_EFAULT;
}

static int64_t rlimit_set(uint32_t res, uint64_t in)
{
    if (res >= L_RLIM_NLIMITS)
        return -L_EINVAL;
    uint64_t v[2];
    if (!(lx_rlim32 ? lx_get_longs(in, (int64_t *)v, 2) : lx_get(v, in, sizeof v)))
        return -L_EFAULT;
    if (v[0] > v[1])
        return -L_EINVAL;
    if (v[1] > lx.rlim[res][1])
        return -L_EPERM;
    lx.rlim[res][0] = v[0];
    lx.rlim[res][1] = v[1];
    return 0;
}

static int64_t sys_getrlimit(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    return rlimit_get((uint32_t)a[0], a[1]);
}

static int64_t sys_setrlimit(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    return rlimit_set((uint32_t)a[0], a[1]);
}

static int64_t sys_prlimit64(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if (a[0] != 0 && (int32_t)a[0] != lx.pid)
        return -L_ESRCH;
    uint32_t res = (uint32_t)a[1];
    if (res >= L_RLIM_NLIMITS)
        return -L_EINVAL;
    if (a[3]) {
        int64_t rc = rlimit_get(res, a[3]);
        if (rc < 0)
            return rc;
    }
    return a[2] ? rlimit_set(res, a[2]) : 0;
}

static int64_t sys_getrusage(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    if ((int64_t)a[0] != 0 && (int64_t)a[0] != -1 && (int64_t)a[0] != 1)
        return -L_EINVAL;
    int64_t ru[18] = { 0 };
    int64_t ns = hio_cputime_ns();
    ru[0] = ns / 1000000000;
    ru[1] = (ns % 1000000000) / 1000;
    return lx_put_longs(a[1], ru, 18) ? 0 : -L_EFAULT;
}

static int64_t sys_times(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    int64_t cpu = hio_cputime_ns() / 10000000;
    if (a[0]) {
        int64_t t[4] = { cpu, 0, 0, 0 };
        if (!lx_put_longs(a[0], t, 4))
            return -L_EFAULT;
    }
    return (int64_t)(rl_clock_ns() / 10000000);
}

static int64_t sys_sysinfo(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    return lx_put_sysinfo(a[0], rl_clock_ns() / 1000000000, rl_os_memory_bytes(), 1) ? 0 : -L_EFAULT;
}

static int64_t sys_prctl(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    switch ((uint32_t)a[0]) {
    case L_PR_SET_NAME: {
        char n[16];
        int64_t k = lx_cstr(a[1], n, 16);
        if (k == -L_ENAMETOOLONG)
            k = 15;
        else if (k < 0)
            return k;
        memset(g_comm, 0, sizeof g_comm);
        memcpy(g_comm, n, (size_t)k);
        return 0;
    }
    case L_PR_GET_NAME:
        return lx_put(a[1], g_comm, sizeof g_comm) ? 0 : -L_EFAULT;
    default:
        return -L_EINVAL;
    }
}

static int64_t sys_futex(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint32_t op = (uint32_t)a[1];
    uint32_t cmd = op & (uint32_t)L_FUTEX_CMD_MASK;
    switch (cmd) {
    case L_FUTEX_WAIT:
    case L_FUTEX_WAIT_BITSET: {
        uint32_t bits = cmd == L_FUTEX_WAIT_BITSET ? (uint32_t)a[5] : ~0u;
        if (bits == 0)
            return -L_EINVAL;
        int64_t timeout = -1;
        if (a[3]) {
            l_timespec ts;
            if (!lx_get_timespec(a[3], &ts))
                return -L_EFAULT;
            if (ts.sec < 0 || ts.nsec < 0 || ts.nsec >= 1000000000)
                return -L_EINVAL;
            timeout = ts.sec * 1000000000ll + ts.nsec;
            if (cmd == L_FUTEX_WAIT_BITSET) {
                int64_t now;
                int64_t rc = clock_ns((op & L_FUTEX_CLOCK_REALTIME) ? L_CLOCK_REALTIME : L_CLOCK_MONOTONIC, &now);
                if (rc < 0)
                    return rc;
                timeout = timeout > now ? timeout - now : 0;
            }
        }
        return lx_futex_wait(a[0], (uint32_t)a[2], timeout, bits);
    }
    case L_FUTEX_WAKE:
    case L_FUTEX_WAKE_BITSET: {
        uint32_t bits = cmd == L_FUTEX_WAKE_BITSET ? (uint32_t)a[5] : ~0u;
        if (bits == 0)
            return -L_EINVAL;
        if (!lx_ok(a[0], 4, RL_VM_R))
            return -L_EFAULT;
        return lx_futex_wake(a[0], (uint32_t)a[2] > 0x7fffffffu ? 0x7fffffffu : (uint32_t)a[2], bits);
    }
    case L_FUTEX_REQUEUE:
    case L_FUTEX_CMP_REQUEUE: {
        if (cmd == L_FUTEX_CMP_REQUEUE) {
            uint32_t cur;
            if (!lx_get(&cur, a[0], 4))
                return -L_EFAULT;
            if (cur != (uint32_t)a[5])
                return -L_EAGAIN;
        }
        return lx_futex_wake(a[0], (uint32_t)a[2], ~0u);
    }
    case L_FUTEX_WAKE_OP: {
        uint32_t v, op2 = (uint32_t)a[5];
        if (!lx_get(&v, a[4], 4))
            return -L_EFAULT;
        uint32_t oparg = (op2 >> 12) & 0xfff, cmparg = op2 & 0xfff;
        if (op2 & (8u << 28))
            oparg = 1u << oparg;
        uint32_t nv;
        switch ((op2 >> 28) & 7) {
        case 0: nv = oparg; break;
        case 1: nv = v + oparg; break;
        case 2: nv = v | oparg; break;
        case 3: nv = v & ~oparg; break;
        case 4: nv = v ^ oparg; break;
        default: return -L_ENOSYS;
        }
        if (!lx_put(a[4], &nv, 4))
            return -L_EFAULT;
        int woke = lx_futex_wake(a[0], (uint32_t)a[2], ~0u);
        bool hit;
        switch ((op2 >> 24) & 15) {
        case 0: hit = v == cmparg; break;
        case 1: hit = v != cmparg; break;
        case 2: hit = (int32_t)v < (int32_t)cmparg; break;
        case 3: hit = (int32_t)v <= (int32_t)cmparg; break;
        case 4: hit = (int32_t)v > (int32_t)cmparg; break;
        case 5: hit = (int32_t)v >= (int32_t)cmparg; break;
        default: return -L_ENOSYS;
        }
        if (hit)
            woke += lx_futex_wake(a[4], (uint32_t)a[3], ~0u);
        return woke;
    }
    default:
        return -L_ENOSYS;
    }
}

lx_sys_fn *lx_proc_call(uint32_t nr)
{
    switch (nr) {
    case NR_exit: return sys_exit;
    case NR_exit_group: return sys_exit_group;
    case NR_set_tid_address: return sys_set_tid_address;
    case NR_getpid: return sys_getpid;
    case NR_gettid: return sys_gettid;
    case NR_getppid: return sys_getppid;
    case NR_getuid:
    case NR_geteuid: return sys_getuid;
    case NR_getgid:
    case NR_getegid: return sys_getgid;
    case NR_set_robust_list: return sys_zero;
    case NR_clock_gettime: return sys_clock_gettime;
    case NR_clock_getres: return sys_clock_getres;
    case NR_gettimeofday: return sys_gettimeofday;
    case NR_nanosleep: return sys_nanosleep;
    case NR_clock_nanosleep: return sys_clock_nanosleep;
    case NR_wait4: return sys_wait4;
    case NR_sched_yield: return sys_sched_yield;
    case NR_sched_getaffinity: return sys_sched_getaffinity;
    case NR_uname: return sys_uname;
    case NR_getrandom: return sys_getrandom;
    case NR_getrlimit: return sys_getrlimit;
    case NR_setrlimit: return sys_setrlimit;
    case NR_prlimit64: return sys_prlimit64;
    case NR_getrusage: return sys_getrusage;
    case NR_times: return sys_times;
    case NR_sysinfo: return sys_sysinfo;
    case NR_prctl: return sys_prctl;
    case NR_futex: return sys_futex;
    default: return NULL;
    }
}
