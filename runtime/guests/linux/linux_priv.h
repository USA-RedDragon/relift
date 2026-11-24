/**
 * @file linux_priv.h
 * @brief What the files of the Linux layer share: the process state, guest
 * memory access and the system call tables.
 */
#ifndef RL_LINUX_PRIV_H
#define RL_LINUX_PRIV_H

#include "abi.h"
#include "core.h"
#include "linux.h"
#include "hostio.h"
#include "lxsync.h"

#include <stdbool.h>
#include <stddef.h>

/** @brief The result a system call returns to end the process or the thread instead of giving a value. */
#define LX_PENDING INT64_MIN

/** @brief Highest descriptor number plus one. */
#define LX_MAX_FDS 1024

/** @brief What an open descriptor is. */
typedef struct lx_fd {
    /** @brief The host descriptor, -1 when the slot is free or the file is virtual. */
    int host;
    /** @brief The file of the program this descriptor reads, NULL for a host file. */
    const rl_linux_module *virt;
    /** @brief The read position in a virtual file. */
    uint64_t virt_pos;
    /** @brief The path of the directory of program files this descriptor lists (a directory the import's paths lie under), or NULL. */
    char *vdir;
    /** @brief FD_CLOEXEC: an execve does not pass the descriptor on. */
    bool cloexec;
    /** @brief The directory stream getdents64 reads, opened on first use. */
    void *dir;
    /** @brief SO_RCVTIMEO of a socket in nanoseconds, 0 for none. */
    int64_t rcv_timeout_ns;
    /** @brief SO_SNDTIMEO of a socket in nanoseconds, 0 for none. */
    int64_t snd_timeout_ns;
    /** @brief 0 not looked at yet, 1 a regular file or directory (never blocks), 3 a pipe or FIFO, 4 a socket, 2 anything else. */
    uint8_t kind;
    /** @brief Whether an entry read from the stream waits in stash_*, because it did not fit the last buffer. */
    bool has_stash;
    /** @brief The waiting entry's type. */
    uint8_t stash_type;
    /** @brief The waiting entry's inode. */
    uint64_t stash_ino;
    /** @brief The waiting entry's name. */
    char stash_name[256];
} lx_fd;

/** @brief One signal's disposition. */
typedef struct lx_sigaction {
    /** @brief SIG_DFL, SIG_IGN or a guest address. */
    uint64_t handler;
    /** @brief SA_* flags. */
    uint64_t flags;
    /** @brief The guest's return trampoline. */
    uint64_t restorer;
    /** @brief Signals blocked while the handler runs. */
    uint64_t mask;
} lx_sigaction;

/** @brief A queued signal. */
typedef struct lx_pending_sig {
    /** @brief si_code. */
    int32_t code;
    /** @brief si_pid. */
    int32_t pid;
    /** @brief si_uid. */
    uint32_t uid;
    /** @brief Whether the signal is queued. */
    bool queued;
} lx_pending_sig;

/** @brief One guest thread: the state the kernel keeps per thread. */
typedef struct lx_thread {
    /** @brief The runtime thread, whose context holds the registers. */
    struct rl_thread *rt;
    /** @brief The thread id. */
    int32_t tid;
    /** @brief Whether this is the thread the program started on. */
    bool main;
    /** @brief The address of the instruction the thread starts at (a clone's child). */
    uint64_t start;
    /** @brief The blocked signals, bit n - 1 for signal n. */
    uint64_t blocked;
    /** @brief The queued signals sent to this thread, by number. */
    lx_pending_sig pending[L_NSIG + 1];
    /** @brief The alternate signal stack. */
    uint64_t altstack_sp;
    /** @brief Its size. */
    uint64_t altstack_size;
    /** @brief Whether a handler runs on it now. */
    bool on_altstack;
    /** @brief How many signal handlers run nested on this thread. */
    int handlers;
    /** @brief What a handler's saved pc adds to the pc of the interruption: 4 after an svc, 0 at a safe point. */
    uint64_t resume_bias;
    /** @brief Where set_tid_address or clone says the thread id is cleared when the thread ends. */
    uint64_t clear_child_tid;
    /** @brief The wake handle blocking waits of this thread take (hio_waker_*). */
    void *waker;
    /** @brief The next thread. */
    struct lx_thread *next;
} lx_thread;

/** @brief The process. */
typedef struct lx_state {
    /** @brief The guest address space. */
    rl_vm vm;
    /** @brief The program. */
    const rl_linux_program *prog;
    /** @brief The first address the program break may take. */
    uint64_t brk_start;
    /** @brief The program break. */
    uint64_t brk;
    /** @brief The end of the pages mapped for the break. */
    uint64_t brk_mapped;
    /** @brief The highest address the break may reach. */
    uint64_t brk_limit;
    /** @brief The range the program's libraries load into (the interpreter maps them there), 0 and 0 for a program with none. */
    uint64_t lib_lo, lib_hi;
    /** @brief The library the interpreter opened last and has not mapped yet (it maps it next), or NULL. */
    const rl_linux_module *pending_lib;
    /** @brief Whether the program has a program interpreter. */
    bool dynamic;
    /** @brief The signal whose default action ends the process, 0 while none has. */
    int death_signal;
    /** @brief The guest root's host directory without a trailing slash (rl_linux_program.root, or RELIFT_LINUX_ROOT when set; empty disables it), NULL for none. */
    char *root;
    /** @brief Length of root. */
    size_t root_len;
    /** @brief The top of the area mmap takes from, growing down. */
    uint64_t mmap_top;
    /** @brief The lowest address mmap may take. */
    uint64_t mmap_floor;
    /** @brief The main stack's lowest address. */
    uint64_t stack_lo;
    /** @brief One above the main stack's highest address. */
    uint64_t stack_hi;
    /** @brief The thread-ID the process answers with. */
    int32_t pid;
    /** @brief Descriptors. */
    lx_fd fds[LX_MAX_FDS];
    /** @brief Dispositions by signal number. */
    lx_sigaction sig[L_NSIG + 1];
    /** @brief The umask. */
    uint32_t umask;
    /** @brief The threads, newest first; threads_lock guards the list, live and exit_status. */
    lx_thread *threads;
    /** @brief Threads that have not ended. */
    int live;
    /** @brief The status the last thread to end gave. */
    uint32_t exit_status;
    /** @brief The next thread id. */
    int32_t next_tid;
    /** @brief Resource limits, soft then hard. */
    uint64_t rlim[L_RLIM_NLIMITS][2];
} lx_state;

/** @brief The process. */
extern lx_state lx;
/** @brief Guards the address map, the descriptor table and the thread list. */
extern lx_mutex lx_big;
/** @brief Signals the last thread's end to the main thread. */
extern lx_cond lx_ended;

/** @brief The calling guest thread. */
static inline lx_thread *lx_cur(void) { return rl_thread_current()->guest; }

/** @brief clone for threads. */
int64_t lx_sys_clone(rl_ctx *c, uint8_t *mem, const uint64_t a[6]);
/** @brief clone without CLONE_THREAD: a fork or vfork over a host fork of the process (process.c). */
int64_t lx_fork(rl_ctx *c, const uint64_t a[6]);
/** @brief The threads' locks and list across a host fork (enum rl_fork_step): in the child only the calling thread is left. */
void lx_threads_fork(int step);
/** @brief A child process changed state: wakes the waits of lx_child_wait. */
void lx_child_event(void);
/** @brief The count of child state changes, for lx_child_wait. */
uint64_t lx_child_gen(void);
/** @brief Waits until the count passes gen or a signal is ready for the thread; true for the signal. */
bool lx_child_wait(uint64_t gen);
/** @brief Ends the process with code, or by the signal whose default action ended it (lx.death_signal), as the kernel does. */
void lx_exit_process(uint32_t code);
/** @brief Starts the helper thread that routes host signals and runs the interval timers, and applies what an execve passed (signal mask, ignored signals); then hides that from the environment. */
void lx_process_start(lx_thread *main_thread);
/** @brief The helper thread's timer work: fires the interval timers that are due and returns the nanoseconds until the next, -1 for none (timers.c). */
int64_t lx_itimer_run(void);
/** @brief Writes the interval timers (remaining:interval nanoseconds, comma separated) for an execve to pass on; the length. */
int lx_itimer_save(char *out, size_t cap);
/** @brief Arms the interval timers an execve passed on (lx_itimer_save's text). */
void lx_itimer_restore(const char *s);
/** @brief In a forked child: no interval timers (they are not inherited). */
void lx_itimer_fork_child(void);
/** @brief Wakes the helper thread to look at the timers again. */
void lx_helper_kick(void);
/** @brief Queues signal sig for the process from a thread that is not a guest thread, with the sender's ids and si_code. */
void lx_signal_post(int sig, int32_t code, int32_t pid, uint32_t uid);
/** @brief The host path of guest path path under the guest root (symbolic links followed): 0 or -errno. */
int64_t lx_host_path(const char *path, char *out, size_t cap);
/** @brief As lx_host_path, following a symbolic link at the last component only when follow is set. */
int64_t lx_host_path_at(const char *path, char *out, size_t cap, bool follow);
/** @brief The host directory descriptor a *at call's dirfd stands for (HIO_CWD for AT_FDCWD or an absolute path): 0 or -EBADF. */
int lx_dir_arg(int64_t dirfd, const char *path, int *host);
/** @brief Fills keep with the guest descriptors an execve passes on (open, not close-on-exec, host backed): pairs of guest number and host descriptor; returns the count. */
int lx_fs_exec_fds(int (*keep)[2], int cap);
/** @brief Ends the calling thread: clears its tid word, wakes its waiters, leaves the list. */
void lx_thread_end(lx_thread *t);
/** @brief Waits until the other threads have ended. */
void lx_wait_others(void);
/** @brief Wakes up to n futex waiters on guest address a; how many woke. */
int lx_futex_wake(uint64_t a, uint32_t n, uint32_t bits);
/**
 * @brief FUTEX_REQUEUE: wakes up to nwake waiters of the futex at a and moves
 * up to nrequeue more to the one at a2, where a wake of a2 finds them. The
 * number woken plus moved, as the kernel returns it.
 */
int64_t lx_futex_requeue(uint64_t a, uint32_t nwake, uint64_t a2, uint32_t nrequeue);
/** @brief futex wait: 0, -EAGAIN, -ETIMEDOUT or -EINTR. */
int64_t lx_futex_wait(uint64_t a, uint32_t val, int64_t timeout_ns, uint32_t bits);

/** @brief Whether [a, a + n) lies in guest memory with the permission bits perm (RL_VM_R, RL_VM_W). */
bool lx_ok(uint64_t a, uint64_t n, uint32_t perm);
/** @brief Copies n guest bytes at a to dst; false when they are not all readable. */
bool lx_get(void *dst, uint64_t a, uint64_t n);
/** @brief Copies n bytes to guest address a; false when they are not all writable. */
bool lx_put(uint64_t a, const void *src, uint64_t n);
/** @brief Reads a NUL-terminated guest string of at most cap - 1 bytes into dst: its length, or -EFAULT or -ENAMETOOLONG. */
int64_t lx_cstr(uint64_t a, char *dst, size_t cap);
/** @brief The Linux errno of the host errno e. */
int lx_errno(int e);
/** @brief -lx_errno(errno). */
int64_t lx_fail(void);

/**
 * @brief Everything rl_linux_main does before the program's first
 * instruction: the address space, the image, the descriptors, the main
 * thread and the initial stack. 0 and the main thread's context in *out, or
 * 1 after logging why not.
 */
int lx_start(const rl_linux_program *p, int argc, char **argv, rl_ctx **out);

/** @brief Maps the pages of [addr, addr + size) with the guest permission perm (L_PROT_*), zero filled. */
int lx_map(uint64_t addr, uint64_t size, uint32_t prot);
/** @brief Whether every page of the range is mapped. */
bool lx_mapped(uint64_t addr, uint64_t size);
/** @brief The first address of a free range of size bytes below the top of the mmap area, or 0. */
uint64_t lx_find_free(uint64_t size);

/** @brief One system call: arguments a[0..5], the result or -errno. */
typedef int64_t lx_sys_fn(rl_ctx *c, uint8_t *mem, const uint64_t a[6]);

/**
 * @brief Maps size bytes (page rounded) of zeroed read-write guest memory in
 * the mmap area, for host code that puts structures in guest memory (a
 * library wrapper's strings and shadows): its guest address, or 0. Thread
 * safe. Callers build any malloc-like layer on top.
 */
uint64_t lx_guest_alloc(uint64_t size);
/** @brief Unmaps what lx_guest_alloc returned. */
void lx_guest_free(uint64_t addr, uint64_t size);

int64_t lx_sys_brk(rl_ctx *c, uint8_t *mem, const uint64_t a[6]);
int64_t lx_sys_mmap(rl_ctx *c, uint8_t *mem, const uint64_t a[6]);
int64_t lx_sys_munmap(rl_ctx *c, uint8_t *mem, const uint64_t a[6]);
int64_t lx_sys_mprotect(rl_ctx *c, uint8_t *mem, const uint64_t a[6]);
int64_t lx_sys_mremap(rl_ctx *c, uint8_t *mem, const uint64_t a[6]);
int64_t lx_sys_madvise(rl_ctx *c, uint8_t *mem, const uint64_t a[6]);

/** @brief eventfd, timerfd and epoll, by number: NULL for a number they do not take. */
lx_sys_fn *lx_event_call(uint32_t nr);
/** @brief hio_epoll_wait for the calling thread: interrupted (-EINTR) when a signal is ready. */
int64_t lx_epoll_wait(int ep, hio_epoll_event *out, int max, int64_t timeout_ns);
/** @brief The file and descriptor system calls, by number: NULL for a number they do not take. */
lx_sys_fn *lx_fs_call(uint32_t nr);
/** @brief The process, time and information system calls. */
lx_sys_fn *lx_proc_call(uint32_t nr);
/** @brief The process system calls of process.c (execve, wait4, sessions), or NULL. */
lx_sys_fn *lx_process_call(uint32_t nr);
/** @brief write(2) of a host buffer to guest descriptor fd, with write's blocking, signal and SIGPIPE rules. */
int64_t lx_fd_write(int fd, const uint8_t *buf, uint64_t n);
/** @brief fcntl's record locks (F_GETLK 5, F_SETLK 6, F_SETLKW 7 and the OFD ones 36 to 38) on host descriptor h with the guest's struct flock at addr; a blocking one waits so a signal interrupts it. */
int64_t lx_record_lock(int h, uint32_t cmd, uint64_t addr);
/** @brief The file system calls of fsx.c (ownership, extended attributes, fchmod, fchmodat2, sendfile, pselect6), or NULL. */
lx_sys_fn *lx_fsx_call(uint32_t nr);
/** @brief The socket system calls of net.c, or NULL. */
lx_sys_fn *lx_net_call(uint32_t nr);
/** @brief A write on a socket with the blocking and signal rules of send (net.c). */
int64_t lx_sock_write(int fd, int h, const uint8_t *buf, uint64_t n);
/** @brief Puts host descriptor host in the lowest free guest slot (takes lx_big); the guest descriptor, or -EMFILE (closing host). */
int64_t lx_fd_install(int host, bool cloexec);
/** @brief Closes guest descriptor fd (takes lx_big). */
void lx_fd_close(int fd);
/** @brief The guest's name for a host path in place (the guest root's prefix taken off); the new length. */
size_t lx_guest_name(char *path, size_t len);
/** @brief Reads and checks a guest iovec array (iov_get of fs.c). */
int64_t lx_iov_get(uint64_t addr, uint64_t cnt, uint64_t (*v)[2], uint64_t *total, uint32_t need);
/** @brief The interval timer system calls (getitimer, setitimer), or NULL. */
lx_sys_fn *lx_timer_call(uint32_t nr);
/** @brief The signal system calls. */
lx_sys_fn *lx_signal_call(uint32_t nr);

/** @brief The file of the program that guest descriptor fd reads (an interpreter's open of a library), or NULL. */
const rl_linux_module *lx_virt_fd(int fd);
/** @brief The directory of program files that guest descriptor fd lists, or NULL. */
const char *lx_vdir_fd(int fd);
/** @brief The host descriptor of guest descriptor fd, or -1. */
int lx_host_fd(int fd);
/** @brief Reads len bytes of the file open as host descriptor host from offset off into guest memory at addr (the rest of the last page stays zero): 0 or -errno. */
int64_t lx_fill_from_file(int host, uint64_t addr, uint64_t len, uint64_t off);

/** @brief Sets up the descriptors: guest 0, 1 and 2 are the host's. */
void lx_fs_init(void);
/** @brief Closes what the process has open. */
void lx_fs_shutdown(void);

/** @brief rt_sigreturn: ends the innermost signal handler's host entry; LX_PENDING, or -ENOSYS outside a handler. */
int64_t lx_sigreturn(rl_ctx *c);
/** @brief The arm64 safe point hook: delivers the signals a thread is ready for at a preemption check. */
int lx_safepoint(rl_ctx *c, uint8_t *mem);
/** @brief Delivers the queued signals the process does not block, after a system call. Returns RL_OK or RL_PENDING. */
int lx_signal_deliver(rl_ctx *c, uint8_t *mem);
/** @brief Whether a signal waits that the thread does not block and that does something (a handler, or the default action that ends the process). */
bool lx_signal_ready(void);
/**
 * @brief The flags of the handler the next delivered signal would run: 1 and
 * *flags when there is one, 0 when the next deliverable signal (if any) runs
 * no handler.
 */
int lx_signal_peek(uint64_t *flags);
/** @brief Wakes thread t from a blocking wait so it sees a signal sent to it. */
void lx_thread_kick(lx_thread *t);
/**
 * @brief The calling thread's interruptible wait for host descriptor h (-1: none)
 * to be ready for the POLL bits in events, or timeout_ns (negative: never).
 * The ready bits, 0 for the timeout, or -EINTR when a signal is ready.
 */
int64_t lx_wait(int h, uint32_t events, int64_t timeout_ns);
/** @brief Makes a signal pending for the process. */
void lx_signal_raise(int sig, int32_t code);
/** @brief Sets the signals' initial state. */
void lx_signal_init(void);

/** @brief Logs how many instructions ran in the cold copy when RELIFT_STATS=1. */
void lx_report_cold(void);
/** @brief Fills in the resource limits. */
void lx_proc_init(void);

/** @brief The page-rounded size. */
static inline uint64_t lx_page_up(uint64_t v) { return (v + L_PAGE - 1) & ~(L_PAGE - 1); }
/** @brief The page-aligned address at or below v. */
static inline uint64_t lx_page_down(uint64_t v) { return v & ~(L_PAGE - 1); }

#endif
