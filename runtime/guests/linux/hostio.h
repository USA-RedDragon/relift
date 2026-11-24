/**
 * @file hostio.h
 * @brief The host calls the file system calls make, one function per
 * operation, so the files of the Linux layer never name a host API: each
 * returns a value or the negative Linux errno, and takes and gives Linux
 * flags, modes and times. hostio_posix.c serves hosts with POSIX file calls
 * (Linux, Android, macOS, iOS); hostio_win.c serves Windows.
 */
#ifndef RL_LINUX_HOSTIO_H
#define RL_LINUX_HOSTIO_H

#include "abi.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The host descriptor that means the working directory in the *at calls. */
#define HIO_CWD (-100)

/** @brief Prepares the host for the guest's file calls: a write to a closed pipe must give EPIPE, not end the process. */
void hio_init(void);
/** @brief Opens path (relative to directory descriptor dir) with Linux flags and mode; the host descriptor. */
int64_t hio_open(int dir, const char *path, uint32_t flags, uint32_t mode);
/** @brief Closes a host descriptor. */
int64_t hio_close(int fd);
/** @brief read(2). */
int64_t hio_read(int fd, void *buf, size_t n);
/** @brief write(2). */
int64_t hio_write(int fd, const void *buf, size_t n);
/** @brief pread(2). */
int64_t hio_pread(int fd, void *buf, size_t n, uint64_t off);
/** @brief pwrite(2). */
int64_t hio_pwrite(int fd, const void *buf, size_t n, uint64_t off);
/** @brief lseek(2) with Linux whence values. */
int64_t hio_lseek(int fd, int64_t off, int whence);
/** @brief fstat(2) into the aarch64 layout. */
int64_t hio_fstat(int fd, l_stat *st);
/** @brief fstatat(2) with AT_SYMLINK_NOFOLLOW. */
int64_t hio_stat(int dir, const char *path, bool nofollow, l_stat *st);
/** @brief ftruncate(2). */
int64_t hio_ftruncate(int fd, uint64_t len);
/** @brief fsync(2). */
int64_t hio_fsync(int fd);
/** @brief dup(2): the lowest host descriptor at or above min. */
int64_t hio_dup(int fd, int min);
/** @brief dup2 onto the host descriptor target. */
int64_t hio_dup2(int fd, int target);
/** @brief pipe(2): fds[0] reads what fds[1] writes. */
int64_t hio_pipe(int fds[2]);
/** @brief mkdirat(2). */
int64_t hio_mkdir(int dir, const char *path, uint32_t mode);
/** @brief symlinkat(2): a link at path whose text is target. */
int64_t hio_symlink(const char *target, int dir, const char *path);
/** @brief linkat(2); follow follows a symbolic link at the old path. */
int64_t hio_link(int old_dir, const char *old_path, int new_dir, const char *new_path, bool follow);
/** @brief mkfifoat(3): a named pipe. */
int64_t hio_mkfifo(int dir, const char *path, uint32_t mode);
/** @brief statvfs of the file system that holds path (dir NULL path, or the descriptor fd when path is NULL), in Linux's statfs layout. */
int64_t hio_statfs(int fd, const char *path, l_statfs *out);
/** @brief unlinkat(2); remove_dir selects rmdir. */
int64_t hio_unlink(int dir, const char *path, bool remove_dir);
/** @brief renameat(2). */
int64_t hio_rename(int old_dir, const char *old_path, int new_dir, const char *new_path);
/** @brief faccessat(2). */
int64_t hio_access(int dir, const char *path, uint32_t mode);
/** @brief readlinkat(2): the byte count. */
int64_t hio_readlink(int dir, const char *path, char *buf, size_t n);
/** @brief getcwd(3): the length including the NUL. */
int64_t hio_getcwd(char *buf, size_t n);
/** @brief chdir(2). */
int64_t hio_chdir(const char *path);
/** @brief fchmodat(2). */
int64_t hio_chmod(int dir, const char *path, uint32_t mode);
/** @brief utimensat(2) with seconds and nanoseconds for access and modify times; nsec values of UTIME_NOW (0x3fffffff) and UTIME_OMIT (0x3ffffffe) are as Linux has them. */
int64_t hio_utimens(int dir, const char *path, const int64_t times[4], bool nofollow);
/** @brief Whether the descriptor is a terminal. */
bool hio_isatty(int fd);
/** @brief The terminal size of the descriptor: rows, columns, pixels wide, pixels high; -ENOTTY when it is not a terminal. */
int64_t hio_winsize(int fd, uint16_t ws[4]);
/** @brief The bytes ready to read on the descriptor, FIONREAD. */
int64_t hio_nread(int fd, int32_t *n);
/** @brief Sets or clears O_NONBLOCK on the descriptor. */
int64_t hio_set_nonblock(int fd, bool on);
/** @brief The status flags (O_APPEND, O_NONBLOCK) of the descriptor in Linux values. */
int64_t hio_get_status_flags(int fd);
/** @brief Sets O_APPEND and O_NONBLOCK from Linux flags. */
int64_t hio_set_status_flags(int fd, uint32_t flags);
/** @brief Waits up to timeout_ns (negative: forever) until the descriptor can be read or written; the ready Linux POLL* bits. */
int64_t hio_poll(int fd, uint32_t events, int64_t timeout_ns);

/** @brief One event of hio_epoll_wait: the Linux EPOLL* bits and the data word given to hio_epoll_ctl. */
typedef struct hio_epoll_event {
    uint32_t events;
    uint64_t data;
} hio_epoll_event;

/** @brief epoll_create1(2); the descriptor, close-on-exec. */
int64_t hio_epoll_create(void);
/**
 * @brief epoll_ctl(2) with Linux operation and event values (EPOLLIN, EPOLLOUT, EPOLLRDHUP,
 * EPOLLPRI, EPOLLERR, EPOLLHUP, EPOLLET, EPOLLONESHOT) over host descriptors.
 */
int64_t hio_epoll_ctl(int ep, int op, int fd, uint32_t events, uint64_t data);
/**
 * @brief Waits up to timeout_ns (negative: for ever) for events on ep, or until the waker is
 * kicked: how many events were stored in out (at most max; 0 for the timeout), or -EINTR for
 * the kick.
 */
int64_t hio_epoll_wait(int ep, hio_epoll_event *out, int max, int64_t timeout_ns, void *waker);
/** @brief eventfd2(2) with Linux flags (EFD_NONBLOCK, EFD_CLOEXEC, EFD_SEMAPHORE); the descriptor. */
int64_t hio_eventfd(uint32_t init, uint32_t flags);
/** @brief timerfd_create(2) with Linux clock id and flags (TFD_NONBLOCK, TFD_CLOEXEC); the descriptor. */
int64_t hio_timerfd_create(int clock, uint32_t flags);
/** @brief timerfd_settime(2): new_value and old_value are interval seconds, interval nanoseconds, value seconds, value nanoseconds; old_value may be NULL. */
int64_t hio_timerfd_settime(int fd, uint32_t flags, const int64_t new_value[4], int64_t old_value[4]);
/** @brief timerfd_gettime(2), in the layout of hio_timerfd_settime. */
int64_t hio_timerfd_gettime(int fd, int64_t value[4]);

/**
 * @brief A per-thread wake handle: a host-blocking wait that is given it
 * returns when it is kicked. The handle is level triggered: a kick stays
 * until hio_waker_clear, so a kick before the wait is not lost.
 */
void *hio_waker_new(void);
/** @brief Frees a handle. */
void hio_waker_free(void *w);
/** @brief Makes every wait on the handle, now and until it is cleared, return. */
void hio_waker_kick(void *w);
/** @brief Clears the kick. */
void hio_waker_clear(void *w);
/**
 * @brief Waits until descriptor fd (when not negative) is ready for the Linux
 * POLL bits in events, timeout_ns passes (negative: never) or the waker is
 * kicked. The ready bits (a ready descriptor wins over a kick), 0 for the
 * timeout, or -EINTR for the kick.
 */
int64_t hio_wait(int fd, uint32_t events, int64_t timeout_ns, void *waker);

/** @brief Opens a directory stream on the host descriptor; the stream takes the descriptor over. */
void *hio_dir_open(int fd);
/** @brief Closes the stream and its descriptor. */
void hio_dir_close(void *dir);
/** @brief The next entry: name, inode and Linux d_type; 1 for an entry, 0 at the end, or -errno. */
int64_t hio_dir_next(void *dir, char name[256], uint64_t *ino, uint8_t *type);
/** @brief Rewinds the stream. */
void hio_dir_rewind(void *dir);

/** @brief The host's wall clock in nanoseconds since the Unix epoch. */
int64_t hio_realtime_ns(void);
/** @brief Processor time used by the process in nanoseconds. */
int64_t hio_cputime_ns(void);
/** @brief The host's process id. */
int32_t hio_pid(void);
/** @brief The host's parent process id. */
int32_t hio_ppid(void);
/** @brief The host's user id, or 1000. */
uint32_t hio_uid(void);
/** @brief The host's group id, or 1000. */
uint32_t hio_gid(void);
/** @brief The host's name for itself. */
void hio_nodename(char *buf, size_t n);
/** @brief How many processors the host has online. */
uint32_t hio_cpu_count(void);
/** @brief Sets the process's umask and returns the old one. */
uint32_t hio_umask(uint32_t mask);

/** @brief fchownat(2) (nofollow: AT_SYMLINK_NOFOLLOW); -1 leaves an id as it is. */
int64_t hio_chown(int dir, const char *path, int64_t uid, int64_t gid, bool nofollow);
/** @brief fchown(2). */
int64_t hio_fchown(int fd, int64_t uid, int64_t gid);
/** @brief fchmod(2). */
int64_t hio_fchmod(int fd, uint32_t mode);
/** @brief The extended attribute calls, by what: 's'et, 'g'et, 'l'ist or 'r'emove; on path (nofollow for the l* calls) or, with path NULL, on descriptor fd. get and list return the size (with size 0, the size needed). flags are Linux's XATTR_CREATE (1) and XATTR_REPLACE (2). */
int64_t hio_xattr(char what, int fd, const char *path, bool nofollow, const char *name, void *value, size_t size, int flags);

/** @brief Removes name from the process's environment. */
void hio_unsetenv(const char *name);
/** @brief The host signal of Linux signal sig, 0 for one the host does not have. */
int hio_sig_to_host(int sig);
/** @brief The Linux signal of host signal sig, 0 for none. */
int hio_sig_from_host(int sig);
/** @brief fork(2): the child's process id in the parent, 0 in the child, or -errno (-ENOSYS where the host cannot fork). */
int64_t hio_fork(void);
/**
 * @brief wait4(2) with Linux options (WNOHANG, WUNTRACED, WCONTINUED) that
 * never blocks: the process id, 0 when none has changed, or -errno.
 * *status gets the Linux status word, cpu_ns the child's user and system
 * time in nanoseconds (may be NULL).
 */
int64_t hio_wait4(int32_t pid, int32_t *status, uint32_t options, int64_t cpu_ns[2]);
/** @brief kill(2) with a Linux signal (0 checks the target). */
int64_t hio_kill(int32_t pid, int sig);
/** @brief setsid(2). */
int64_t hio_setsid(void);
/** @brief setpgid(2). */
int64_t hio_setpgid(int32_t pid, int32_t pgid);
/** @brief getpgid(2). */
int64_t hio_getpgid(int32_t pid);
/** @brief getsid(2). */
int64_t hio_getsid(int32_t pid);
/** @brief The absolute host path of the running executable, false when unknown. */
bool hio_self_exe(char *buf, size_t n);
/** @brief The canonical absolute host path of path (symbolic links resolved), false when it does not exist. */
bool hio_realpath(const char *path, char *buf, size_t n);
/**
 * @brief Replaces the process with exe, argv and envp (NULL terminated),
 * with host descriptor keep[i][1] open as number keep[i][0] for each of
 * the n pairs and every other descriptor closed. Returns -errno only when
 * nothing changed yet (exe is not executable); a failure after the
 * descriptors moved ends the process.
 */
int64_t hio_exec(const char *exe, char *const argv[], char *const envp[], const int (*keep)[2], int n);
/**
 * @brief Routes the host signals a Linux guest takes from outside (hangup,
 * interrupt, quit, the user signals, terminate, child and window change)
 * to a pipe: blocks them in the calling thread (every thread made after it
 * inherits that) and returns the read end, where each signal arrives as
 * an hio_sig_note; -errno on failure. Called again (in a forked child) it
 * makes a new pipe.
 */
int64_t hio_signal_pipe(void);
/** @brief Lets the calling thread (the one that reads the signal pipe) take the routed signals. */
void hio_signal_accept(void);
/** @brief Ends the process with host signal sig at its default action, as the kernel does for a fatal signal. */
void hio_die_by_signal(int sig);

/** @brief The most descriptors one message carries (SCM_MAX_FD). */
#define HIO_MAX_RIGHTS 253

/** @brief One socket message in a flat buffer, with its address in the guest's sockaddr layout and SCM_RIGHTS descriptors as host descriptors. */
typedef struct hio_msg {
    /** @brief The bytes. */
    void *data;
    /** @brief Their count (the buffer's size for a receive). */
    size_t len;
    /** @brief The peer's address (guest layout), or NULL. */
    uint8_t *name;
    /** @brief Its length; a receive sets the address's full length (0 for none). */
    uint32_t namelen;
    /** @brief The passed descriptors. */
    int rights[HIO_MAX_RIGHTS];
    /** @brief Their count. */
    int nrights;
    /** @brief A receive's Linux MSG_ result flags (MSG_TRUNC, MSG_CTRUNC, MSG_OOB, MSG_EOR). */
    uint32_t flags;
} hio_msg;

/** @brief socket(2) with Linux domain, type (SOCK_NONBLOCK honored; the host descriptor is close-on-exec) and protocol. */
int64_t hio_socket(int domain, int type, int protocol);
/** @brief socketpair(2), as hio_socket. */
int64_t hio_socketpair(int domain, int type, int protocol, int sv[2]);
/** @brief bind(2) to a guest-layout address. */
int64_t hio_bind(int h, const uint8_t *addr, uint32_t len);
/** @brief connect(2) to a guest-layout address. */
int64_t hio_connect(int h, const uint8_t *addr, uint32_t len);
/** @brief listen(2). */
int64_t hio_listen(int h, int backlog);
/** @brief accept(2): the new host descriptor, its peer in addr (guest layout, *len in and out), non-blocking when asked. */
int64_t hio_accept(int h, uint8_t *addr, uint32_t *len, bool nonblock);
/** @brief getsockname(2) or getpeername(2), guest layout. */
int64_t hio_sockname(int h, bool peer, uint8_t *addr, uint32_t *len);
/** @brief shutdown(2). */
int64_t hio_shutdown(int h, int how);
/** @brief sendmsg(2) of one flat message with Linux MSG_ flags; never raises SIGPIPE (EPIPE is the guest's to raise). */
int64_t hio_sendmsg(int h, const hio_msg *m, uint32_t flags);
/** @brief recvmsg(2) into one flat buffer with Linux MSG_ flags; passed descriptors come close-on-exec. */
int64_t hio_recvmsg(int h, hio_msg *m, uint32_t flags);
/** @brief setsockopt(2) with Linux level, name and value layout. */
int64_t hio_setsockopt(int h, int level, int name, const void *val, uint32_t len);
/** @brief getsockopt(2) with Linux level, name and value layout; *len in and out. */
int64_t hio_getsockopt(int h, int level, int name, void *val, uint32_t *len);

/** @brief One routed signal: the Linux signal, the sender's process and user ids and si_code. */
typedef struct hio_sig_note {
    int32_t sig;
    int32_t pid;
    uint32_t uid;
    int32_t code;
} hio_sig_note;

#endif
