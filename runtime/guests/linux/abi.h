/**
 * @file abi.h
 * @brief Linux aarch64 kernel ABI values: system call numbers, errno values,
 * flags and structure layouts, as the kernel's UAPI headers
 * (asm-generic/unistd.h, errno-base.h, errno.h, fcntl.h, mman-common.h,
 * signal.h, stat.h, ioctls.h and the arm64 sigcontext.h) give them. They are
 * written out here because the host the runtime builds for is not Linux
 * everywhere; tests/test_uapi.c checks them against the UAPI headers
 * where the host has them.
 */
#ifndef RL_LINUX_ABI_H
#define RL_LINUX_ABI_H

#include <stdint.h>

/**
 * @brief System call ids. These are the asm-generic/unistd.h numbers aarch64
 * uses; every architecture maps its own numbers to them (arch_*.c), so the
 * system call files name a call one way.
 */
enum rl_linux_nr {
    NR_getcwd = 17,
    NR_dup = 23,
    NR_setxattr = 5,
    NR_lsetxattr = 6,
    NR_fsetxattr = 7,
    NR_getxattr = 8,
    NR_lgetxattr = 9,
    NR_fgetxattr = 10,
    NR_listxattr = 11,
    NR_llistxattr = 12,
    NR_flistxattr = 13,
    NR_removexattr = 14,
    NR_lremovexattr = 15,
    NR_fremovexattr = 16,
    NR_eventfd2 = 19,
    NR_epoll_create1 = 20,
    NR_epoll_ctl = 21,
    NR_epoll_pwait = 22,
    NR_dup3 = 24,
    NR_fcntl = 25,
    NR_ioctl = 29,
    NR_mknodat = 33,
    NR_mkdirat = 34,
    NR_unlinkat = 35,
    NR_symlinkat = 36,
    NR_linkat = 37,
    NR_renameat = 38,
    NR_statfs = 43,
    NR_fstatfs = 44,
    NR_ftruncate = 46,
    NR_faccessat = 48,
    NR_chdir = 49,
    NR_flock = 32,
    NR_fchmod = 52,
    NR_fchmodat = 53,
    NR_fchownat = 54,
    NR_fchown = 55,
    NR_openat = 56,
    NR_close = 57,
    NR_pipe2 = 59,
    NR_getdents64 = 61,
    NR_lseek = 62,
    NR_read = 63,
    NR_write = 64,
    NR_readv = 65,
    NR_writev = 66,
    NR_pread64 = 67,
    NR_pwrite64 = 68,
    NR_sendfile = 71,
    NR_pselect6 = 72,
    NR_ppoll = 73,
    NR_readlinkat = 78,
    NR_newfstatat = 79,
    NR_fstat = 80,
    NR_fsync = 82,
    NR_timerfd_create = 85,
    NR_timerfd_settime = 86,
    NR_timerfd_gettime = 87,
    NR_fdatasync = 83,
    NR_utimensat = 88,
    NR_exit = 93,
    NR_exit_group = 94,
    NR_set_tid_address = 96,
    NR_futex = 98,
    NR_set_robust_list = 99,
    NR_get_robust_list = 100,
    NR_nanosleep = 101,
    NR_clock_gettime = 113,
    NR_clock_getres = 114,
    NR_clock_nanosleep = 115,
    NR_sched_yield = 124,
    NR_sched_getaffinity = 123,
    NR_getitimer = 102,
    NR_setitimer = 103,
    NR_kill = 129,
    NR_setpgid = 154,
    NR_getpgid = 155,
    NR_getsid = 156,
    NR_setsid = 157,
    NR_tkill = 130,
    NR_tgkill = 131,
    NR_sigaltstack = 132,
    NR_rt_sigsuspend = 133,
    NR_rt_sigaction = 134,
    NR_rt_sigprocmask = 135,
    NR_rt_sigpending = 136,
    NR_rt_sigtimedwait = 137,
    NR_rt_sigreturn = 139,
    NR_times = 153,
    NR_uname = 160,
    NR_getrlimit = 163,
    NR_setrlimit = 164,
    NR_getrusage = 165,
    NR_umask = 166,
    NR_prctl = 167,
    NR_gettimeofday = 169,
    NR_getpid = 172,
    NR_getppid = 173,
    NR_getuid = 174,
    NR_geteuid = 175,
    NR_getgid = 176,
    NR_getegid = 177,
    NR_gettid = 178,
    NR_sysinfo = 179,
    NR_brk = 214,
    NR_clone = 220,
    NR_munmap = 215,
    NR_mremap = 216,
    NR_mmap = 222,
    NR_mprotect = 226,
    NR_madvise = 233,
    NR_prlimit64 = 261,
    NR_getrandom = 278,
    NR_statx = 291,
    NR_rseq = 293,
    NR_socket = 198,
    NR_socketpair = 199,
    NR_bind = 200,
    NR_listen = 201,
    NR_accept = 202,
    NR_connect = 203,
    NR_getsockname = 204,
    NR_getpeername = 205,
    NR_sendto = 206,
    NR_recvfrom = 207,
    NR_setsockopt = 208,
    NR_getsockopt = 209,
    NR_shutdown = 210,
    NR_sendmsg = 211,
    NR_recvmsg = 212,
    NR_execve = 221,
    NR_accept4 = 242,
    NR_wait4 = 260,
    NR_clone3 = 435,
    NR_close_range = 436,
    NR_fchmodat2 = 452
};

/** @brief Linux errno values (asm-generic/errno-base.h and errno.h). */
enum rl_linux_errno {
    L_EPERM = 1,
    L_ENOENT = 2,
    L_ESRCH = 3,
    L_EINTR = 4,
    L_EIO = 5,
    L_ENXIO = 6,
    L_E2BIG = 7,
    L_ENOEXEC = 8,
    L_EBADF = 9,
    L_ECHILD = 10,
    L_EAGAIN = 11,
    L_ENOMEM = 12,
    L_EACCES = 13,
    L_EFAULT = 14,
    L_EBUSY = 16,
    L_EEXIST = 17,
    L_EXDEV = 18,
    L_ENODEV = 19,
    L_ENOTDIR = 20,
    L_EISDIR = 21,
    L_EINVAL = 22,
    L_ENFILE = 23,
    L_EMFILE = 24,
    L_ENOTTY = 25,
    L_ETXTBSY = 26,
    L_EFBIG = 27,
    L_ENOSPC = 28,
    L_ESPIPE = 29,
    L_EROFS = 30,
    L_EMLINK = 31,
    L_EPIPE = 32,
    L_EDOM = 33,
    L_ERANGE = 34,
    L_EDEADLK = 35,
    L_ENAMETOOLONG = 36,
    L_ENOSYS = 38,
    L_ENOTEMPTY = 39,
    L_ELOOP = 40,
    L_ENODATA = 61,
    L_EOVERFLOW = 75,
    L_ENOTSUP = 95,
    L_ENOTSOCK = 88,
    L_EDESTADDRREQ = 89,
    L_EMSGSIZE = 90,
    L_EPROTOTYPE = 91,
    L_ENOPROTOOPT = 92,
    L_EPROTONOSUPPORT = 93,
    L_ESOCKTNOSUPPORT = 94,
    L_EPFNOSUPPORT = 96,
    L_EAFNOSUPPORT = 97,
    L_EADDRINUSE = 98,
    L_EADDRNOTAVAIL = 99,
    L_ENETDOWN = 100,
    L_ENETUNREACH = 101,
    L_ENETRESET = 102,
    L_ECONNABORTED = 103,
    L_ECONNRESET = 104,
    L_ENOBUFS = 105,
    L_EISCONN = 106,
    L_ENOTCONN = 107,
    L_ESHUTDOWN = 108,
    L_ETOOMANYREFS = 109,
    L_ECONNREFUSED = 111,
    L_EHOSTDOWN = 112,
    L_EHOSTUNREACH = 113,
    L_EALREADY = 114,
    L_EINPROGRESS = 115,
    L_ETIMEDOUT = 110
};

/**
 * @brief Kernel-internal results a blocking system call returns when a signal
 * interrupted it (include/linux/errno.h; user space never sees them). The
 * system call return path turns each into -EINTR or runs the call again
 * after the handler, by SA_RESTART and the code (signal(7)).
 */
enum rl_linux_restart {
    /** @brief Run again after a handler with SA_RESTART, else EINTR. */
    L_ERESTARTSYS = 512,
    /** @brief Run again when no handler ran, else EINTR. */
    L_ERESTARTNOHAND = 514,
    /** @brief EINTR after a handler; the remaining time is returned by the call. */
    L_ERESTART_RESTARTBLOCK = 516
};

/** @brief open flags (arm64 asm/fcntl.h over asm-generic/fcntl.h). */
enum rl_linux_open {
    L_O_ACCMODE = 3,
    L_O_RDONLY = 0,
    L_O_WRONLY = 1,
    L_O_RDWR = 2,
    L_O_CREAT = 0100,
    L_O_EXCL = 0200,
    L_O_NOCTTY = 0400,
    L_O_TRUNC = 01000,
    L_O_APPEND = 02000,
    L_O_NONBLOCK = 04000,
    L_O_DSYNC = 010000,
    L_O_DIRECTORY = 040000,
    L_O_NOFOLLOW = 0100000,
    L_O_DIRECT = 0200000,
    L_O_LARGEFILE = 0400000,
    L_O_NOATIME = 01000000,
    L_O_CLOEXEC = 02000000,
    L_O_SYNC = 04010000,
    L_O_PATH = 010000000,
    L_O_TMPFILE = 020040000
};

/** @brief fcntl commands and flags. */
enum rl_linux_fcntl {
    L_F_DUPFD = 0,
    L_F_GETFD = 1,
    L_F_SETFD = 2,
    L_F_GETFL = 3,
    L_F_SETFL = 4,
    L_F_DUPFD_CLOEXEC = 1030,
    L_FD_CLOEXEC = 1
};

/** @brief lseek whence. */
enum { L_SEEK_SET = 0, L_SEEK_CUR = 1, L_SEEK_END = 2 };

/** @brief The directory descriptor that means the working directory. */
#define L_AT_FDCWD (-100)
/** @brief *at flags. */
enum {
    L_AT_REMOVEDIR = 0x200,
    L_AT_SYMLINK_NOFOLLOW = 0x100,
    L_AT_EACCESS = 0x200,
    L_AT_EMPTY_PATH = 0x1000
};

/** @brief File type bits of st_mode. */
enum {
    L_S_IFMT = 0170000,
    L_S_IFSOCK = 0140000,
    L_S_IFLNK = 0120000,
    L_S_IFREG = 0100000,
    L_S_IFBLK = 0060000,
    L_S_IFDIR = 0040000,
    L_S_IFCHR = 0020000,
    L_S_IFIFO = 0010000
};

/** @brief Directory entry types (linux_dirent64.d_type). */
enum { L_DT_UNKNOWN = 0, L_DT_FIFO = 1, L_DT_CHR = 2, L_DT_DIR = 4, L_DT_BLK = 6, L_DT_REG = 8, L_DT_LNK = 10, L_DT_SOCK = 12 };

/** @brief mmap protection and flags (asm-generic/mman-common.h and mman.h). */
enum rl_linux_mman {
    L_PROT_NONE = 0,
    L_PROT_READ = 1,
    L_PROT_WRITE = 2,
    L_PROT_EXEC = 4,
    L_MAP_SHARED = 0x01,
    L_MAP_PRIVATE = 0x02,
    L_MAP_SHARED_VALIDATE = 0x03,
    L_MAP_TYPE = 0x0f,
    L_MAP_FIXED = 0x10,
    L_MAP_ANONYMOUS = 0x20,
    L_MAP_GROWSDOWN = 0x0100,
    L_MAP_NORESERVE = 0x4000,
    L_MAP_POPULATE = 0x8000,
    L_MAP_STACK = 0x20000,
    L_MAP_FIXED_NOREPLACE = 0x100000,
    L_MREMAP_MAYMOVE = 1,
    L_MREMAP_FIXED = 2
};

/** @brief Clocks. */
enum { L_CLOCK_REALTIME = 0, L_CLOCK_MONOTONIC = 1, L_CLOCK_PROCESS_CPUTIME_ID = 2, L_CLOCK_THREAD_CPUTIME_ID = 3, L_CLOCK_MONOTONIC_RAW = 4, L_CLOCK_REALTIME_COARSE = 5, L_CLOCK_MONOTONIC_COARSE = 6, L_CLOCK_BOOTTIME = 7 };
#define L_TIMER_ABSTIME 1

/** @brief ioctl requests used on terminals. */
enum { L_TCGETS = 0x5401, L_TCSETS = 0x5402, L_TIOCGWINSZ = 0x5413, L_FIONREAD = 0x541b, L_FIONBIO = 0x5421 };

/** @brief Signal numbers (asm-generic/signal.h). */
enum {
    L_SIGHUP = 1,
    L_SIGINT = 2,
    L_SIGQUIT = 3,
    L_SIGILL = 4,
    L_SIGTRAP = 5,
    L_SIGABRT = 6,
    L_SIGBUS = 7,
    L_SIGFPE = 8,
    L_SIGKILL = 9,
    L_SIGUSR1 = 10,
    L_SIGSEGV = 11,
    L_SIGUSR2 = 12,
    L_SIGPIPE = 13,
    L_SIGALRM = 14,
    L_SIGTERM = 15,
    L_SIGSTKFLT = 16,
    L_SIGCHLD = 17,
    L_SIGCONT = 18,
    L_SIGSTOP = 19,
    L_SIGTSTP = 20,
    L_SIGTTIN = 21,
    L_SIGTTOU = 22,
    L_SIGURG = 23,
    L_SIGXCPU = 24,
    L_SIGXFSZ = 25,
    L_SIGVTALRM = 26,
    L_SIGPROF = 27,
    L_SIGWINCH = 28,
    L_SIGIO = 29,
    L_SIGPWR = 30,
    L_SIGSYS = 31,
    L_NSIG = 64
};

/** @brief sigaction flags and special handlers. */
enum {
    L_SA_NOCLDSTOP = 0x1,
    L_SA_NOCLDWAIT = 0x2,
    L_SA_SIGINFO = 0x4,
    L_SA_UNSUPPORTED = 0x400,
    L_SA_EXPOSE_TAGBITS = 0x800,
    L_SA_ONSTACK = 0x08000000,
    L_SA_RESTART = 0x10000000,
    L_SA_NODEFER = 0x40000000,
    L_SA_RESETHAND = 0x80000000u,
    L_SA_RESTORER = 0x04000000
};
#define L_SIG_DFL 0ull
#define L_SIG_IGN 1ull
enum { L_SIG_BLOCK = 0, L_SIG_UNBLOCK = 1, L_SIG_SETMASK = 2 };

/** @brief siginfo si_code values. */
enum { L_SI_USER = 0, L_SI_TKILL = -6 };

/** @brief Sizes of kernel structures on aarch64. */
enum {
    L_SIGSET_BYTES = 8,
    L_SIGINFO_BYTES = 128,
    L_UCONTEXT_MCONTEXT = 176,
    L_SIGCONTEXT_BYTES = 4384,
    L_MINSIGSTKSZ = 5120,
    L_SS_DISABLE = 2,
    L_SS_ONSTACK = 1
};

/** @brief Resource limits. */
enum { L_RLIMIT_CPU = 0, L_RLIMIT_FSIZE = 1, L_RLIMIT_DATA = 2, L_RLIMIT_STACK = 3, L_RLIMIT_CORE = 4, L_RLIMIT_RSS = 5, L_RLIMIT_NPROC = 6, L_RLIMIT_NOFILE = 7, L_RLIMIT_MEMLOCK = 8, L_RLIMIT_AS = 9, L_RLIM_NLIMITS = 16 };
#define L_RLIM_INFINITY (~0ull)

/** @brief futex operations. */
enum { L_FUTEX_WAIT = 0, L_FUTEX_WAKE = 1, L_FUTEX_REQUEUE = 3, L_FUTEX_CMP_REQUEUE = 4, L_FUTEX_WAKE_OP = 5, L_FUTEX_WAIT_BITSET = 9, L_FUTEX_WAKE_BITSET = 10, L_FUTEX_PRIVATE = 128, L_FUTEX_CLOCK_REALTIME = 256, L_FUTEX_CMD_MASK = ~(128 | 256) };

/** @brief prctl options. */
enum { L_PR_SET_NAME = 15, L_PR_GET_NAME = 16 };

/** @brief ppoll event bits. */
enum { L_POLLIN = 1, L_POLLOUT = 4, L_POLLERR = 8, L_POLLHUP = 16, L_POLLNVAL = 32 };

/** @brief Auxiliary vector tags (linux/auxvec.h). */
enum {
    L_AT_NULL = 0,
    L_AT_PHDR = 3,
    L_AT_PHENT = 4,
    L_AT_PHNUM = 5,
    L_AT_PAGESZ = 6,
    L_AT_BASE = 7,
    L_AT_FLAGS = 8,
    L_AT_ENTRY = 9,
    L_AT_UID = 11,
    L_AT_EUID = 12,
    L_AT_GID = 13,
    L_AT_EGID = 14,
    L_AT_PLATFORM = 15,
    L_AT_HWCAP = 16,
    L_AT_CLKTCK = 17,
    L_AT_SECURE = 23,
    L_AT_RANDOM = 25,
    L_AT_HWCAP2 = 26,
    L_AT_EXECFN = 31,
    L_AT_MINSIGSTKSZ = 51
};

/** @brief clone flags. */
enum {
    L_CLONE_VM = 0x100,
    L_CLONE_FS = 0x200,
    L_CLONE_FILES = 0x400,
    L_CLONE_SIGHAND = 0x800,
    L_CLONE_THREAD = 0x10000,
    L_CLONE_SYSVSEM = 0x40000,
    L_CLONE_SETTLS = 0x80000,
    L_CLONE_PARENT_SETTID = 0x100000,
    L_CLONE_CHILD_CLEARTID = 0x200000,
    L_CLONE_DETACHED = 0x400000,
    L_CLONE_CHILD_SETTID = 0x1000000,
    L_CLONE_THREAD_SET = L_CLONE_VM | L_CLONE_FS | L_CLONE_FILES | L_CLONE_SIGHAND | L_CLONE_THREAD | L_CLONE_SYSVSEM
};

/** @brief The guest page size. */
#define L_PAGE 4096ull

/** @brief struct stat of aarch64 (asm-generic/stat.h), 128 bytes. */
typedef struct l_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint32_t st_mode;
    uint32_t st_nlink;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_rdev;
    uint64_t pad1;
    int64_t st_size;
    int32_t st_blksize;
    int32_t pad2;
    int64_t st_blocks;
    int64_t st_atime_sec;
    uint64_t st_atime_ns;
    int64_t st_mtime_sec;
    uint64_t st_mtime_ns;
    int64_t st_ctime_sec;
    uint64_t st_ctime_ns;
    uint32_t unused4;
    uint32_t unused5;
} l_stat;
_Static_assert(sizeof(l_stat) == 128, "l_stat size");

/** @brief struct statfs of aarch64 (the generic 64-bit layout). */
typedef struct l_statfs {
    int64_t f_type;
    int64_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
    int32_t f_fsid[2];
    int64_t f_namelen;
    int64_t f_frsize;
    int64_t f_flags;
    int64_t f_spare[4];
} l_statfs;
_Static_assert(sizeof(l_statfs) == 120, "l_statfs size");

/** @brief struct timespec of aarch64: two 64-bit words. */
typedef struct l_timespec {
    int64_t sec;
    int64_t nsec;
} l_timespec;

/** @brief struct utsname: six 65-byte fields. */
typedef struct l_utsname {
    char sysname[65];
    char nodename[65];
    char release[65];
    char version[65];
    char machine[65];
    char domainname[65];
} l_utsname;

#endif
