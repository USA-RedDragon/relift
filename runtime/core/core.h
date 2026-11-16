/**
 * @file core.h
 * @brief Runtime core: logging, configuration, the guest and CPU descriptors,
 * guest memory, faults, host threads, the clock and dispatch.
 *
 * The core knows no guest OS and no guest CPU. A CPU layer (runtime/cpus/)
 * describes its context and code granule with an rl_cpu_desc; a guest layer
 * (runtime/guests/) describes its page size, address width and clock with an
 * rl_guest_desc and builds its OS model on top of these pieces. The core
 * includes no header of either, and runtime/charter checks that.
 */
#ifndef RELIFT_CORE_H
#define RELIFT_CORE_H

#include <relift/abi_core.h>

#include <setjmp.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Marks a function that logs with printf-style arguments. */
#define RL_PRINTF(f, a) __attribute__((format(printf, f, a)))

/** @brief Log levels. */
enum rl_log_level {
    /** @brief Debug detail, shown only with RELIFT_LOG=debug. */
    RL_LOG_DEBUG = 0,
    /** @brief Information. */
    RL_LOG_INFO = 1,
    /** @brief Something the guest did that the runtime does not handle fully. */
    RL_LOG_WARN = 2,
    /** @brief A failure. */
    RL_LOG_ERROR = 3
};

/** @brief Writes one log line to stderr with a level and a subsystem tag. */
RL_RUNTIME_API void rl_log(int level, const char *tag, const char *fmt, ...) RL_PRINTF(3, 4);
/** @brief rl_log with a va_list. */
RL_RUNTIME_API void rl_vlog(int level, const char *tag, const char *fmt, va_list ap);

/**
 * @brief Returns true the first time it is called with a given key, false
 * afterwards. Used to report an unknown guest OS call or command once.
 */
RL_RUNTIME_API bool rl_once(uint64_t key);
/** @brief Forgets every key rl_once has seen (tests). */
RL_RUNTIME_API void rl_once_reset(void);

/**
 * @brief Reports a guest OS call, service or command the guest layer does not
 * implement: logs it as a warning the first time key is seen. Returns true
 * when RELIFT_STRICT is set, and the guest layer then stops the guest instead
 * of returning an error to it.
 */
RL_RUNTIME_API bool rl_report_unknown(uint64_t key, const char *tag, const char *fmt, ...) RL_PRINTF(3, 4);

/** @brief Runtime switches read from the environment at start-up. */
typedef struct rl_config {
    /** @brief RELIFT_STRICT: stop on an unknown guest OS call or command instead of returning an error (rl_report_unknown). */
    bool strict;
    /** @brief RELIFT_TRACE_OS: the guest layer logs every guest OS call and service request with its result and time. */
    bool trace_os;
    /** @brief RELIFT_LOG: lowest level printed (debug, info, warn, error). */
    int log_level;
    /** @brief RELIFT_HOST_PAGE: page size the memory manager assumes, at least the real one (16384 simulates Apple hosts). */
    uint32_t host_page;
    /** @brief RELIFT_ADDRESS_BITS: asks the guest layer for this address space width; 0 keeps the guest's own choice. The guest layer decides which widths it accepts. */
    uint32_t address_bits;
    /** @brief True once rl_config_load has run. */
    bool loaded;
} rl_config;

/** @brief The configuration, read once by rl_config_load and changeable by tests. */
RL_RUNTIME_API rl_config *rl_cfg(void);
/** @brief Reads the environment into rl_cfg(). Called by the runtime's init; tests may call it again. */
RL_RUNTIME_API void rl_config_load(void);

/**
 * @brief What the guest layer tells the core about the guest platform, given
 * to rl_core_init.
 */
typedef struct rl_guest_desc {
    /** @brief Guest name for reports. */
    const char *name;
    /** @brief Guest page size in bytes: the granule of rl_vm_set, a power of two. */
    uint64_t page_size;
    /** @brief Widest guest address space the guest layer reserves, in bits (rl_vm_init refuses more). */
    uint32_t address_bits;
    /** @brief Rate of the guest's clock in ticks per second (rl_ns_to_ticks). */
    uint64_t tick_hz;
} rl_guest_desc;

/**
 * @brief What the CPU layer tells the core about its context and code, given
 * to rl_core_init. The core never reads a register; it allocates contexts of
 * ctx_size bytes and reaches its own fields at common_offset.
 */
typedef struct rl_cpu_desc {
    /** @brief CPU name for reports. */
    const char *name;
    /** @brief sizeof(rl_ctx) of this CPU layer; units must carry the same value. */
    uint32_t ctx_size;
    /** @brief Alignment of rl_ctx, a power of two at least sizeof(void *). */
    uint32_t ctx_align;
    /** @brief Offset of the rl_ctx_common block in rl_ctx, 8-byte aligned. */
    uint32_t common_offset;
    /** @brief log2 of the code granule: guest function entries are aligned to it, and the dispatch table has one slot per granule. */
    uint32_t code_shift;
    /** @brief True when guest data is big-endian (rl_guest_order16 and friends swap). */
    bool big_endian;
    /** @brief Offset of the rl_time clock in rl_ctx, 8-byte aligned, or 0 for a CPU without one (rl_sync, rl_io_read). */
    uint32_t time_offset;
    /** @brief Sets the CPU's initial state in a zeroed context (after the core filled its block), or NULL. */
    void (*ctx_init)(rl_ctx *c);
    /** @brief Called when a host thread binds a context (host CPU state that follows the guest's), or NULL. */
    void (*bind)(rl_ctx *c);
    /** @brief Writes a short register summary of c for crash reports into buf (NUL terminated), or NULL. */
    void (*describe)(const rl_ctx *c, char *buf, size_t n);
    /**
     * @brief Writes the whole register file of c for a stop report into buf,
     * one or more lines of `name value` pairs, each line ending in a newline
     * (NUL terminated), or NULL to use describe.
     */
    void (*regs)(const rl_ctx *c, char *buf, size_t n);
    /**
     * @brief The guest address of the computed call that missed, from the
     * architectural state the call left in c (the return address it set), or
     * NULL when the CPU cannot tell. Called only for a miss entered by a host
     * call from lifted code. Returns RL_SITE_NONE when unknown.
     */
    uint64_t (*call_site)(const rl_ctx *c);
} rl_cpu_desc;

/**
 * @brief Sets the guest and CPU descriptors. The guest layer calls it before
 * it reserves guest memory or creates a thread. Returns 0, or -1 when a
 * descriptor is malformed. The descriptors must outlive the runtime.
 */
RL_RUNTIME_API int rl_core_init(const rl_guest_desc *guest, const rl_cpu_desc *cpu);
/** @brief The guest descriptor, NULL before rl_core_init. */
RL_RUNTIME_API const rl_guest_desc *rl_core_guest(void);
/** @brief The CPU descriptor, NULL before rl_core_init. */
RL_RUNTIME_API const rl_cpu_desc *rl_core_cpu(void);
/** @brief The core's block of a context. */
RL_RUNTIME_API rl_ctx_common *rl_common(rl_ctx *c);
/** @brief The clock of a context (rl_cpu_desc.time_offset), or NULL when the CPU has none. */
RL_RUNTIME_API rl_time *rl_ctx_time(rl_ctx *c);

/**
 * @brief What rl_preempt runs: the guest layer's scheduler, at a preemption
 * point whose clock budget another host thread ended (docs/abi.md section
 * 14.1). The guest registers are in the lifted code's locals, not in c, so
 * the hook must neither read nor write them; it may block, and it clears
 * the budget before it returns.
 */
typedef void rl_preempt_hook(rl_ctx *c);
/** @brief Sets the preemption hook; without one rl_preempt only clears the budget. */
RL_RUNTIME_API void rl_set_preempt_hook(rl_preempt_hook *hook);
/** @brief Ends the budget of c's clock from any host thread, so c's thread runs the preemption hook at its next preemption point. */
RL_RUNTIME_API void rl_preempt_request(rl_ctx *c);
/** @brief Gives c's clock a budget that does not run out: no preemption pending. */
RL_RUNTIME_API void rl_preempt_clear(rl_ctx *c);

/** @brief Converts a 16-bit value between guest and host byte order (the CPU descriptor's big_endian). */
RL_RUNTIME_API uint16_t rl_guest_order16(uint16_t v);
/** @brief Converts a 32-bit value between guest and host byte order. */
RL_RUNTIME_API uint32_t rl_guest_order32(uint32_t v);
/** @brief Converts a 64-bit value between guest and host byte order. */
RL_RUNTIME_API uint64_t rl_guest_order64(uint64_t v);

/** @brief Host monotonic clock in nanoseconds. */
RL_RUNTIME_API uint64_t rl_clock_ns(void);
/** @brief A clock in nanoseconds that the guest layer gives the guest instead of the host's. */
typedef uint64_t rl_guest_clock_fn(void);
/** @brief Sets the guest's clock (NULL: the host's, rl_clock_ns), which rl_guest_clock_ns and the CPU's counter read. */
RL_RUNTIME_API void rl_set_guest_clock(rl_guest_clock_fn *fn);
/** @brief The guest's clock in nanoseconds: the one set with rl_set_guest_clock, else rl_clock_ns. */
RL_RUNTIME_API uint64_t rl_guest_clock_ns(void);
/** @brief Converts nanoseconds to guest clock ticks (rl_guest_desc.tick_hz). */
RL_RUNTIME_API uint64_t rl_ns_to_ticks(uint64_t ns);
/** @brief Converts guest clock ticks to nanoseconds. */
RL_RUNTIME_API uint64_t rl_ticks_to_ns(uint64_t ticks);
/** @brief Real host page size from the OS. */
RL_RUNTIME_API uint32_t rl_os_page_size(void);
/** @brief The process's memory: the physical footprint on Apple hosts, the resident set on Linux and Android, private bytes on Windows; 0 when unknown. */
RL_RUNTIME_API uint64_t rl_os_memory_bytes(void);

/**
 * @brief Guest page permission bits: the core's own encoding, not any guest
 * OS's. The bit positions are chosen to match no guest encoding, so a guest
 * layer that forgets to translate its own permission values fails its tests.
 */
enum rl_vm_perm {
    /** @brief No access. */
    RL_VM_NONE = 0,
    /** @brief Readable. */
    RL_VM_R = 1u << 8,
    /** @brief Writable; the host page is made readable too. */
    RL_VM_W = 1u << 9,
    /** @brief Executable; recorded for the guest only, never given to host memory (code is lifted). */
    RL_VM_X = 1u << 10,
    /** @brief Readable and writable. */
    RL_VM_RW = RL_VM_R | RL_VM_W,
    /** @brief Readable and executable. */
    RL_VM_RX = RL_VM_R | RL_VM_X
};

/** @brief What an rl_vm_set does to the contents of the pages. */
enum rl_vm_content {
    /** @brief Keep the bytes (permission or state change). */
    RL_VM_KEEP = 0,
    /** @brief The pages become newly mapped: they read as zero. */
    RL_VM_FRESH = 1,
    /** @brief The pages become unmapped: the bytes are dropped and the host memory released where possible. */
    RL_VM_DISCARD = 2
};

/** @brief One block of the guest address map: a range with one permission and one guest-defined tag. */
typedef struct rl_vm_block {
    /** @brief First guest address. */
    uint64_t addr;
    /** @brief Size in bytes, a multiple of the guest page. */
    uint64_t size;
    /** @brief Opaque to the core: the guest layer's own state for the block (its memory state and attributes). */
    uint64_t tag;
    /** @brief enum rl_vm_perm. */
    uint32_t perm;
    /** @brief Reserved, zero. */
    uint32_t reserved;
} rl_vm_block;

/**
 * @brief The guest address space: one PROT_NONE host reservation plus the
 * exact guest map at the guest page size.
 *
 * Host protection follows docs/abi.md section 2: a host page gets the union
 * of the permissions of the guest pages in it.
 */
typedef struct rl_vm {
    /** @brief Guest address 0 in host memory: the `mem` lifted code gets. */
    uint8_t *base;
    /** @brief Guest address space size in bytes (2^bits). */
    uint64_t size;
    /** @brief Start of the host mapping including the low guard. */
    uint8_t *map_start;
    /** @brief Length of the host mapping including both guards. */
    uint64_t map_len;
    /** @brief Host page size the manager works in: real or simulated, raised to the guest page when that is larger. */
    uint64_t host_page;
    /** @brief Guest page size (rl_guest_desc.page_size). */
    uint64_t guest_page;
    /** @brief Sorted, coalesced blocks covering [0, size). */
    rl_vm_block *blocks;
    /** @brief Number of blocks in use. */
    uint32_t count;
    /** @brief Allocated block slots. */
    uint32_t cap;
    /** @brief Number of host mprotect calls made (tests read it). */
    uint64_t protect_calls;
} rl_vm;

/** @brief Low guard below guest address 0 (docs/abi.md section 2). */
#define RL_VM_GUARD_LO (1ull << 20)
/** @brief High guard above the end of the guest space. */
#define RL_VM_GUARD_HI (4ull << 30)

/**
 * @brief Reserves a guest space of 2^bits bytes plus guards. Returns 0, or -1
 * when the host cannot reserve it (the caller then tries a smaller layout),
 * before rl_core_init, or when bits exceeds the guest's address_bits.
 * host_page 0 means the configured or real page size.
 */
RL_RUNTIME_API int rl_vm_init(rl_vm *vm, unsigned bits, uint64_t host_page);
/**
 * @brief Like rl_vm_init with a guest space of size bytes, a multiple of the
 * host page, for hosts that cannot reserve a whole layout (iOS caps an app's
 * address space near 64 GiB, guards included).
 */
RL_RUNTIME_API int rl_vm_init_size(rl_vm *vm, uint64_t size, uint64_t host_page);
/** @brief Releases the reservation and the map. */
RL_RUNTIME_API void rl_vm_destroy(rl_vm *vm);
/** @brief Sets [addr, addr+size) to perm and tag. Both page aligned. Returns 0 or -1 (host failure). */
RL_RUNTIME_API int rl_vm_set(rl_vm *vm, uint64_t addr, uint64_t size, uint32_t perm, uint64_t tag, int content);
/** @brief Returns the coalesced block containing addr (addr < vm->size). */
RL_RUNTIME_API rl_vm_block rl_vm_query(const rl_vm *vm, uint64_t addr);
/** @brief True when [addr, addr+size) lies in one block; *out receives that block. */
RL_RUNTIME_API bool rl_vm_uniform(const rl_vm *vm, uint64_t addr, uint64_t size, rl_vm_block *out);
/** @brief Host protection (PROT_*) the policy gives the host page containing addr. */
RL_RUNTIME_API int rl_vm_host_prot(const rl_vm *vm, uint64_t addr);
/**
 * @brief Gives the host pages of [addr, addr+size) read and write access, or
 * none, without changing the map: for tracking accesses to read-write pages
 * through a fault hook. addr and size must be host-page aligned. Logs nothing
 * and touches no shared state, so a fault hook may call it. Returns 0 or -1.
 */
RL_RUNTIME_API int rl_vm_host_access(rl_vm *vm, uint64_t addr, uint64_t size, bool access);
/** @brief True when guest range [addr, addr+size) is inside the space. */
static inline bool rl_vm_contains(const rl_vm *vm, uint64_t addr, uint64_t size)
{
    return addr < vm->size && size <= vm->size - addr;
}
/**
 * @brief Copies between guest ranges as the kernel would, ignoring guest
 * permissions: host pages are made writable for the copy and restored.
 */
RL_RUNTIME_API void rl_vm_kernel_copy(rl_vm *vm, uint64_t dst, uint64_t src, uint64_t size);
/** @brief Copies host bytes into guest memory ignoring guest permissions. */
RL_RUNTIME_API void rl_vm_kernel_write(rl_vm *vm, uint64_t dst, const void *src, uint64_t size);
/** @brief Copies guest bytes out ignoring guest permissions (they must be host readable or are made so). */
RL_RUNTIME_API void rl_vm_kernel_read(rl_vm *vm, void *dst, uint64_t src, uint64_t size);

/** @brief A host shared-memory object: one set of pages that can be mapped several times. */
typedef struct rl_shm {
    /** @brief File descriptor of the pages (-1 where the host has none). */
    int fd;
    /** @brief The host's object handle where it has no file descriptor (Windows). */
    void *handle;
    /** @brief Size in bytes, a multiple of the host page. */
    uint64_t size;
    /** @brief Writable host mapping of the pages. */
    uint8_t *host;
} rl_shm;

/** @brief Creates a zero-filled shared-memory object of size bytes (rounded up to the host page). Returns 0 or -1. */
RL_RUNTIME_API int rl_shm_create(rl_shm *s, uint64_t size);
/** @brief Unmaps the host view and closes the object. */
RL_RUNTIME_API void rl_shm_destroy(rl_shm *s);
/**
 * @brief Maps size bytes of s at guest addr with no access (rl_vm_set then
 * gives the permissions). addr and size must be host-page aligned. Returns 0 or -1.
 */
RL_RUNTIME_API int rl_vm_map_shm(rl_vm *vm, uint64_t addr, uint64_t size, const rl_shm *s);

/** @brief Guest fault description passed to fault hooks and catchers. */
typedef struct rl_fault {
    /** @brief Host signal (SIGSEGV or SIGBUS). */
    int sig;
    /** @brief Host fault address. */
    uintptr_t host_addr;
    /** @brief Guest address (host_addr - base) when in_guest. */
    uint64_t guest_addr;
    /** @brief True when the address is inside the reservation or its guards. */
    bool in_guest;
    /** @brief Host pc of the faulting instruction, 0 when unknown. */
    uintptr_t host_pc;
} rl_fault;

/** @brief Installs SIGSEGV and SIGBUS handlers for the space in vm. Idempotent. */
RL_RUNTIME_API void rl_fault_install(rl_vm *vm);
/** @brief Gives the calling host thread an alternate signal stack. Idempotent per thread. */
RL_RUNTIME_API void rl_fault_thread_init(void);

#if defined(_WIN32)
/** @brief The jump buffer of a fault catcher: sigjmp_buf where the host has signals, jmp_buf on Windows. */
typedef jmp_buf rl_fault_jmp;
#define RL_FAULT_SETJMP(jb) setjmp(jb)
#define RL_FAULT_LONGJMP(jb, v) longjmp(jb, v)
#else
typedef sigjmp_buf rl_fault_jmp;
#define RL_FAULT_SETJMP(jb) sigsetjmp(jb, 1)
#define RL_FAULT_LONGJMP(jb, v) siglongjmp(jb, v)
#endif

/**
 * @brief A per-thread fault catcher (tests and probing). While armed, a guest
 * fault on this thread records the fault and jumps to jb with 1: arm, then
 * `if (RL_FAULT_SETJMP(fc.jb) == 0) { access } else { caught }`, then disarm.
 */
typedef struct rl_fault_catch {
    /** @brief Jump target, set by the caller with RL_FAULT_SETJMP. */
    rl_fault_jmp jb;
    /** @brief The fault, valid after a catch. */
    rl_fault fault;
} rl_fault_catch;

/** @brief Arms c for the calling thread; returns the previous catcher. */
RL_RUNTIME_API rl_fault_catch *rl_fault_arm(rl_fault_catch *c);
/** @brief Disarms the catcher and restores prev. */
RL_RUNTIME_API void rl_fault_disarm(rl_fault_catch *prev);

/**
 * @brief Called first at a guest fault, before any catcher, on the faulting
 * thread inside the signal handler; return true when the fault was resolved
 * and the access should retry.
 */
typedef bool rl_fault_hook(const rl_fault *f);
/** @brief Sets the guest layer's fault hook (NULL for none). */
RL_RUNTIME_API void rl_fault_set_hook(rl_fault_hook *hook);
/** @brief True once rl_fault_install has run, so guest faults reach the hook. */
RL_RUNTIME_API bool rl_fault_installed(void);

/** @brief Runtime thread object (guest-neutral part). */
struct rl_thread {
    /** @brief The CPU layer's context (rl_cpu_desc.ctx_size bytes, ctx_align aligned), owned by this object. */
    rl_ctx *ctx;
    /** @brief Guest layer's per-thread object. */
    void *guest;
    /** @brief Guest entry address. */
    uint64_t entry;
    /** @brief Host thread handle, opaque. */
    void *host;
    /** @brief Host thread id for reports. */
    uint64_t host_tid;
    /** @brief The scheduler clock of this thread's CPU (rl_sched_attach), or NULL. */
    struct rl_sched_cpu *clock;
};

/** @brief The calling host thread's runtime thread, NULL for a host-only thread. */
RL_RUNTIME_API struct rl_thread *rl_thread_current(void);
/** @brief Makes t the calling host thread's runtime thread and runs the CPU layer's bind hook. */
RL_RUNTIME_API void rl_thread_bind(struct rl_thread *t);
/** @brief Allocates a thread object with a zeroed, aligned context (dispatch view copied in, then the CPU layer's ctx_init); NULL before rl_core_init. */
RL_RUNTIME_API struct rl_thread *rl_thread_alloc(void);
/** @brief Frees a thread object and its context. */
RL_RUNTIME_API void rl_thread_free(struct rl_thread *t);
/** @brief Host thread body type. */
typedef void rl_thread_body(struct rl_thread *t);
/** @brief Starts a host thread that binds t and runs body(t). Returns 0 or -1. */
RL_RUNTIME_API int rl_thread_spawn(struct rl_thread *t, rl_thread_body *body);
/** @brief Waits for a host thread started by rl_thread_spawn to finish. */
RL_RUNTIME_API void rl_thread_join(struct rl_thread *t);
/** @brief Detaches the host thread: it frees itself when it ends. */
RL_RUNTIME_API void rl_thread_detach(struct rl_thread *t);
/** @brief Sleeps ns nanoseconds on the host. */
RL_RUNTIME_API void rl_sleep_ns(uint64_t ns);
/** @brief Yields the host thread. */
RL_RUNTIME_API void rl_host_yield(void);
/** @brief Fills n bytes at dst from the host's random source; returns false when it has none. */
RL_RUNTIME_API bool rl_host_random(void *dst, size_t n);

/** @brief Guest exit hook: called with the status when the guest process exits; the default ends the host process. */
typedef void rl_exit_hook(uint32_t code);
/** @brief Sets the exit hook (tests catch the exit instead of ending the process). */
RL_RUNTIME_API void rl_set_exit_hook(rl_exit_hook *hook);
/** @brief Ends the guest process with code (runs the hook). */
RL_RUNTIME_API void rl_process_exit(uint32_t code);
/** @brief Sets an RL_PEND_EXIT_PROCESS record in c and returns RL_PENDING. */
RL_RUNTIME_API int rl_pend_exit_process(rl_ctx *c, uint32_t code);
/** @brief Sets an RL_PEND_EXIT_THREAD record in c and returns RL_PENDING. */
RL_RUNTIME_API int rl_pend_exit_thread(rl_ctx *c);
/**
 * @brief Reports a fatal guest error with the CPU layer's register summary of
 * c appended, then sets an RL_PEND_EXIT_PROCESS record with RL_EXIT_SOFTWARE
 * and returns RL_PENDING.
 */
RL_RUNTIME_API int rl_guest_fatal(rl_ctx *c, const char *tag, const char *fmt, ...) RL_PRINTF(3, 4);

/** @brief rl_miss_info.site when the transfer site is unknown. */
#define RL_SITE_NONE UINT64_MAX
/** @brief rl_miss_info.reason of a stop at an instruction rather than a transfer (target and site are its address). */
#define RL_MISS_NONE 0u

/**
 * @brief Reports a fatal stop at the guest instruction at pc (undefined
 * instruction, trap): writes the stop report of kind with the formatted
 * summary, sets an RL_PEND_EXIT_PROCESS record with RL_EXIT_SOFTWARE and
 * returns RL_PENDING.
 */
RL_RUNTIME_API int rl_guest_stop_at(rl_ctx *c, const char *kind, uint64_t pc, const char *fmt, ...) RL_PRINTF(4, 5);

/** @brief What the runtime knows about a dispatch miss. */
typedef struct rl_miss_info {
    /** @brief Guest address with no function. */
    uint64_t target;
    /** @brief Guest address of the instruction that transferred there, or RL_SITE_NONE. */
    uint64_t site;
    /** @brief enum rl_miss_reason. */
    uint32_t reason;
    /** @brief Host return address into the lifted caller (RL_MISS_CALL), or 0. */
    uintptr_t host_ret;
} rl_miss_info;

/**
 * @brief The shipped-build miss path: writes the stop report for m (docs/abi.md
 * section 11, stop reports) and sets an RL_PEND_EXIT_PROCESS record with
 * RL_EXIT_SOFTWARE. Returns RL_PENDING.
 */
RL_RUNTIME_API int rl_core_miss_at(rl_ctx *c, uint8_t *mem, const rl_miss_info *m);
/**
 * @brief The miss entry of a CPU layer's rl_dispatch_miss: host_ret is the
 * address the dispatcher returns to. A return address in lifted code makes it
 * a computed call (site from rl_cpu_desc.call_site), anything else an entry
 * from the runtime. Returns RL_PENDING.
 */
RL_RUNTIME_API int rl_core_miss(rl_ctx *c, uint8_t *mem, uint64_t target, uintptr_t host_ret);
/** @brief rl_miss_info of a miss entered from host_ret, as rl_core_miss classifies it. */
RL_RUNTIME_API rl_miss_info rl_core_miss_classify(rl_ctx *c, uint64_t target, uintptr_t host_ret);

struct rl_fn_hit;

/**
 * @brief Guest-layer hooks for stop reports. Each may be NULL.
 */
typedef struct rl_report_guest {
    /** @brief Copies n bytes of guest code or data at guest address a into out without faulting; false when a is not readable. */
    bool (*peek)(uint64_t a, void *out, size_t n);
    /** @brief Writes what lies at guest address a (region, bank, segment) into buf, or leaves it empty. */
    void (*where)(uint64_t a, char *buf, size_t n);
    /** @brief The width in bytes of the instruction words a report prints at a target (default 4). */
    uint32_t word_bytes;
    /**
     * @brief For a guest whose code at one address changes (a ROM bank
     * window): the lifted function at guest address a in the code mapped
     * now. Returns 1 with out filled, 0 when a is in such a window and no
     * function of the mapped code holds it, -1 when a is not in one (the
     * registered function maps answer). NULL for none.
     */
    int (*find)(uint64_t a, struct rl_fn_hit *out);
} rl_report_guest;

/** @brief Reads n bytes of guest memory at a through the guest layer's peek (or module code by default); false when unreadable. */
RL_RUNTIME_API bool rl_report_peek(uint64_t a, void *out, size_t n);
/** @brief Sets the guest layer's report hooks (NULL for none). */
RL_RUNTIME_API void rl_report_set_guest(const rl_report_guest *g);
/**
 * @brief Adds one `key: value` header line to every stop report, such as
 * `relift`, `guest`, `mods` or `module` (`<name> base 0x<base> size 0x<size>
 * build <id>`). A key may repeat. Copies both; up to 64 lines.
 */
RL_RUNTIME_API void rl_report_info(const char *key, const char *value);
/** @brief Adds a module header line and the module range used for `name+0xoff` of any guest address. */
RL_RUNTIME_API void rl_report_module(const char *name, uint64_t base, uint64_t size, const char *build_id);
/** @brief Forgets the header lines and modules (tests). */
RL_RUNTIME_API void rl_report_reset(void);
/**
 * @brief Sets the folder stop reports are written to. Without one, reports
 * go to RELIFT_REPORT_DIR, else `reports` in the app's user folder when the
 * program names an app, else the working directory.
 */
RL_RUNTIME_API void rl_report_set_dir(const char *dir);
/**
 * @brief Writes a stop report: kind (such as "missed code") and the one-line
 * summary, the transfer of m when not NULL, the registers of c, the guest
 * and host backtraces. The report goes to a new file in the report folder
 * and to stderr (and the system log on Apple hosts). Formats into static
 * buffers, so it works with a corrupt heap. path receives the file written
 * (empty when none) when not NULL. Returns 0 when the file was written.
 */
RL_RUNTIME_API int rl_report_stop(rl_ctx *c, const char *kind, const char *summary, const rl_miss_info *m, char *path,
                                  size_t path_n);
/**
 * @brief Works out the report folder ahead of a crash (the crash handler
 * cannot): rl_fault_install and rl_report_set_dir call it. Creates the
 * folder when report files are written at all.
 */
RL_RUNTIME_API void rl_report_prepare(void);

/**
 * @brief The thread entry of a guest that uses RL_PEND_REDIRECT: runs the
 * function at guest pc and, each time the host frames unwind with a redirect
 * record, runs the function at its target in a fresh frame. Returns RL_OK
 * when a function returns normally, or RL_PENDING with any other record
 * set.
 */
RL_RUNTIME_API int rl_run_from(rl_ctx *c, uint8_t *mem, uint64_t pc);

/** @brief Registers units into the dispatch table. Call after rl_core_init and before any guest thread starts. Returns 0 or -1 (a unit failed its checks). */
RL_RUNTIME_API int rl_units_register(const rl_unit *const *units, uint32_t count);
/**
 * @brief Checks each code map against its units and the dispatch table
 * (docs/abi.md section 5.1): every RL_CODE_HOT slot is an entry of the hot
 * unit and set, every RL_CODE_COLD slot set and no entry of the hot unit
 * (the cold unit's entries and ranges filled it), and every RL_CODE_TRAP
 * slot no hot entry and still empty. Trap slots stay empty, so a dispatch
 * there is a miss and the miss path reports the class.
 * Then, when cold_first is not NULL (a dev build), stores cold_first in
 * every RL_CODE_COLD slot, so the first dispatch to each is logged
 * (rl_core_cold). Call after every static unit is registered. Returns 0 or
 * -1 (a map failed its checks).
 */
RL_RUNTIME_API int rl_code_maps_register(const rl_code_map *const *maps, uint32_t count, rl_fn cold_first);
/** @brief The enum rl_code_class of guest address a in the registered code maps, with the map's module name in module when not NULL; -1 when no map covers a. */
RL_RUNTIME_API int rl_code_class(uint64_t a, const char **module);
/**
 * @brief The cold function of the RL_CODE_COLD slot of guest target, for a
 * CPU layer's cold_first of rl_code_maps_register: logs the first dispatch to
 * the slot with its module offset and puts the cold function back in it.
 * Returns NULL when target is not such a slot.
 */
RL_RUNTIME_API rl_fn rl_core_cold(uint64_t target);
/** @brief Widens the dispatch table to cover guest code [lo, hi), so units loaded later fit without moving it. Call before any guest thread starts. Returns 0 or -1. */
RL_RUNTIME_API int rl_dispatch_reserve(uint64_t lo, uint64_t hi);
/** @brief Sets the dispatch slot of guest address a to fn (NULL: a miss), for guest layers that bind code at run time. Returns 0, or -1 when a is outside the table or misaligned. */
RL_RUNTIME_API int rl_dispatch_set(uint64_t a, rl_fn fn);
/**
 * @brief Registers a hot-loaded unit (docs/abi.md section 6, step 5): checks
 * its ABI version, context size, image id and entries, adds its function
 * maps, then stores each entry into an empty table slot with a release
 * store (an RL_FN_OVERRIDE entry replaces). The table never moves. Returns
 * 0, or -1 with the reason in why. Callers hold the miss lock.
 */
RL_RUNTIME_API int rl_unit_add(const rl_unit *u, char *why, size_t n);
/** @brief The image id of the registered static units, or NULL when none carries one. */
RL_RUNTIME_API const uint8_t *rl_dispatch_image(void);
/**
 * @brief The dev-build miss path (docs/abi.md section 6, steps 1 to 5): under
 * the miss lock, asks relift over RELIFT_DEV_SOCKET for the code at m->target
 * (with its site and reason; never for a trap slot of a code map),
 * loads and registers the library it builds, and returns the function now
 * registered at target. fp_mode is the CPU layer's floating-point control
 * value for the request. Returns NULL after logging why when the dev path
 * fails, and always in a build without RL_HOTLOAD.
 */
RL_RUNTIME_API rl_fn rl_core_miss_load(rl_ctx *c, const rl_miss_info *m, uint32_t fp_mode);
/** @brief Sets the image id of the guest image set (RL_IMAGE_ID_BYTES bytes) that hot-loaded units must carry, for static units that carry none. NULL leaves it unset. */
RL_RUNTIME_API void rl_dispatch_set_image(const uint8_t *id);
/** @brief Drops the dispatch table (tests). */
RL_RUNTIME_API void rl_units_reset(void);
/** @brief Copies the dispatch view into c. */
RL_RUNTIME_API void rl_dispatch_view(rl_ctx *c);
/** @brief The registered function at guest address a, or NULL (host-side lookup; lifted code uses its CPU layer's inline one). */
RL_RUNTIME_API rl_fn rl_dispatch_find(uint64_t a);

/** @brief Guest memory base used by runtime entries that get only a context. */
RL_RUNTIME_API uint8_t *rl_mem_base(void);
/** @brief Sets the guest memory base (the guest layer calls this after it reserves the space). */
RL_RUNTIME_API void rl_set_mem_base(uint8_t *base);

#endif
