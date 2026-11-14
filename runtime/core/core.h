/**
 * @file core.h
 * @brief Guest-neutral runtime core: logging, configuration, guest memory,
 * faults, host threads, the clock and dispatch.
 *
 * Nothing here knows about a particular guest OS. The Switch layer builds its
 * kernel on top of these pieces.
 */
#ifndef RELIFT_CORE_H
#define RELIFT_CORE_H

#include <relift/abi.h>

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
 * afterwards. Used to report an unknown svc or service command once.
 */
RL_RUNTIME_API bool rl_once(uint64_t key);
/** @brief Forgets every key rl_once has seen (tests). */
RL_RUNTIME_API void rl_once_reset(void);

/** @brief Runtime switches read from the environment at start-up. */
typedef struct rl_config {
    /** @brief RELIFT_STRICT: stop on an unknown svc or service command instead of returning an error. */
    bool strict;
    /** @brief RELIFT_TRACE_SVC: log every svc and IPC request with its result and time. */
    bool trace_svc;
    /** @brief RELIFT_LOG: lowest level printed (debug, info, warn, error). */
    int log_level;
    /** @brief RELIFT_HOST_PAGE: page size the memory manager assumes, at least the real one (16384 simulates Apple hosts). */
    uint32_t host_page;
    /** @brief RELIFT_ADDRESS_BITS: force the guest address space width (36 or 39); 0 keeps the NPDM's. */
    uint32_t address_bits;
    /** @brief True once rl_config_load has run. */
    bool loaded;
} rl_config;

/** @brief The configuration, read once by rl_config_load and changeable by tests. */
RL_RUNTIME_API rl_config *rl_cfg(void);
/** @brief Reads the environment into rl_cfg(). Called by the runtime's init; tests may call it again. */
RL_RUNTIME_API void rl_config_load(void);

/** @brief Host monotonic clock in nanoseconds. */
RL_RUNTIME_API uint64_t rl_clock_ns(void);
/** @brief Converts nanoseconds to RL_CNTFRQ ticks. */
RL_RUNTIME_API uint64_t rl_ns_to_ticks(uint64_t ns);
/** @brief Converts RL_CNTFRQ ticks to nanoseconds. */
RL_RUNTIME_API uint64_t rl_ticks_to_ns(uint64_t ticks);
/** @brief Real host page size from the OS. */
RL_RUNTIME_API uint32_t rl_os_page_size(void);

/** @brief Guest page size every guest in scope uses. */
#define RL_GUEST_PAGE 0x1000ull

/** @brief Guest page permission bits, as Horizon encodes them. */
enum rl_perm {
    RL_PERM_NONE = 0,
    RL_PERM_R = 1,
    RL_PERM_W = 2,
    RL_PERM_X = 4,
    RL_PERM_RW = 3,
    RL_PERM_RX = 5
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
    /** @brief Size in bytes, a multiple of RL_GUEST_PAGE. */
    uint64_t size;
    /** @brief Guest-defined state (the Switch layer packs memory state and attributes here). */
    uint64_t tag;
    /** @brief enum rl_perm. */
    uint32_t perm;
    /** @brief Reserved, zero. */
    uint32_t reserved;
} rl_vm_block;

/**
 * @brief The guest address space: one PROT_NONE host reservation plus the
 * exact 4 KiB guest map.
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
    /** @brief Host page size the manager works in (real or simulated, a power of two at least 4 KiB). */
    uint64_t host_page;
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
 * when the host cannot reserve it (the caller then tries a smaller layout).
 * host_page 0 means the configured or real page size.
 */
RL_RUNTIME_API int rl_vm_init(rl_vm *vm, unsigned bits, uint64_t host_page);
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
    /** @brief File descriptor of the pages. */
    int fd;
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

/**
 * @brief A per-thread fault catcher (tests and probing). While armed, a guest
 * fault on this thread records the fault and siglongjmps to jb with 1:
 * arm, then `if (sigsetjmp(fc.jb, 1) == 0) { access } else { caught }`, then disarm.
 */
typedef struct rl_fault_catch {
    /** @brief Jump target, set by the caller with sigsetjmp. */
    sigjmp_buf jb;
    /** @brief The fault, valid after a catch. */
    rl_fault fault;
} rl_fault_catch;

/** @brief Arms c for the calling thread; returns the previous catcher. */
RL_RUNTIME_API rl_fault_catch *rl_fault_arm(rl_fault_catch *c);
/** @brief Disarms the catcher and restores prev. */
RL_RUNTIME_API void rl_fault_disarm(rl_fault_catch *prev);

/** @brief Called at a guest fault with no catcher; return true when the fault was resolved and the access should retry. */
typedef bool rl_fault_hook(const rl_fault *f);
/** @brief Sets the guest layer's fault hook (NULL for none). */
RL_RUNTIME_API void rl_fault_set_hook(rl_fault_hook *hook);

/** @brief Default FPCR of new guest threads. */
#define RL_FPCR_DEFAULT 0u

/** @brief Runtime thread object (guest-neutral part). */
struct rl_thread {
    /** @brief Guest context, 64-byte aligned, owned by this object. */
    rl_ctx *ctx;
    /** @brief Guest layer's per-thread object. */
    void *guest;
    /** @brief Guest entry address. */
    uint64_t entry;
    /** @brief Host thread handle, opaque. */
    void *host;
    /** @brief Host thread id for reports. */
    uint64_t host_tid;
};

/** @brief The calling host thread's runtime thread, NULL for a host-only thread. */
RL_RUNTIME_API struct rl_thread *rl_thread_current(void);
/** @brief Makes t the calling host thread's runtime thread and applies its FPCR. */
RL_RUNTIME_API void rl_thread_bind(struct rl_thread *t);
/** @brief Allocates a thread object with a zeroed, aligned context (dispatch view copied in). */
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
/** @brief Sets the host FPCR to fpcr on arm64 hosts; no effect elsewhere. */
RL_RUNTIME_API void rl_host_set_fpcr(uint32_t fpcr);
/** @brief Reads the host FPCR on arm64 hosts; 0 elsewhere. */
RL_RUNTIME_API uint32_t rl_host_get_fpcr(void);
/** @brief Sleeps ns nanoseconds on the host. */
RL_RUNTIME_API void rl_sleep_ns(uint64_t ns);
/** @brief Yields the host thread. */
RL_RUNTIME_API void rl_host_yield(void);

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

/** @brief Registers units into the dispatch table. Call before any guest thread starts. Returns 0 or -1 (a unit failed its checks). */
RL_RUNTIME_API int rl_units_register(const rl_unit *const *units, uint32_t count);
/** @brief Drops the dispatch table (tests). */
RL_RUNTIME_API void rl_units_reset(void);
/** @brief Copies the dispatch view into c. */
RL_RUNTIME_API void rl_dispatch_view(rl_ctx *c);
/**
 * @brief Calls guest code at entry from the host, as the runtime does for a
 * thread entry. Returns the lifted return code.
 */
RL_RUNTIME_API int rl_call_guest(rl_ctx *c, uint8_t *mem, uint64_t entry);

/** @brief Guest memory base used by runtime entries that get only a context. */
RL_RUNTIME_API uint8_t *rl_mem_base(void);
/** @brief Sets the guest memory base (the guest layer calls this after it reserves the space). */
RL_RUNTIME_API void rl_set_mem_base(uint8_t *base);

#endif
