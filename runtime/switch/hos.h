/**
 * @file hos.h
 * @brief Horizon kernel model for the Switch guest: results, memory states,
 * the process, kernel objects, handles, threads and synchronization.
 *
 * Behavior follows Ryujinx (MIT, Ryujinx.HLE/HOS/Kernel) and switchbrew.org.
 * One process per runtime. All kernel state is guarded by hos.lock.
 */
#ifndef RELIFT_HOS_H
#define RELIFT_HOS_H

#include "core.h"

#include <pthread.h>

/** @brief Horizon result code: module in bits 0-8, description in bits 9-21. */
typedef uint32_t rl_result;

/** @brief Builds a result from module and description. */
#define RL_MAKE_RESULT(mod, desc) ((uint32_t)(mod) | ((uint32_t)(desc) << 9))

/** @brief Success. */
#define KR_SUCCESS 0u
/** @brief Kernel results (module 1), names as in Ryujinx's KernelResult. */
#define KR_SESSION_COUNT_EXCEEDED RL_MAKE_RESULT(1, 7)
#define KR_NOT_IMPLEMENTED RL_MAKE_RESULT(1, 33)
#define KR_THREAD_TERMINATING RL_MAKE_RESULT(1, 59)
#define KR_INVALID_SIZE RL_MAKE_RESULT(1, 101)
#define KR_INVALID_ADDRESS RL_MAKE_RESULT(1, 102)
#define KR_OUT_OF_RESOURCE RL_MAKE_RESULT(1, 103)
#define KR_OUT_OF_MEMORY RL_MAKE_RESULT(1, 104)
#define KR_HANDLE_TABLE_FULL RL_MAKE_RESULT(1, 105)
#define KR_INVALID_MEM_STATE RL_MAKE_RESULT(1, 106)
#define KR_INVALID_PERMISSION RL_MAKE_RESULT(1, 108)
#define KR_INVALID_MEM_RANGE RL_MAKE_RESULT(1, 110)
#define KR_INVALID_PRIORITY RL_MAKE_RESULT(1, 112)
#define KR_INVALID_CPU_CORE RL_MAKE_RESULT(1, 113)
#define KR_INVALID_HANDLE RL_MAKE_RESULT(1, 114)
#define KR_USER_COPY_FAILED RL_MAKE_RESULT(1, 115)
#define KR_INVALID_COMBINATION RL_MAKE_RESULT(1, 116)
#define KR_TIMED_OUT RL_MAKE_RESULT(1, 117)
#define KR_CANCELLED RL_MAKE_RESULT(1, 118)
#define KR_MAXIMUM_EXCEEDED RL_MAKE_RESULT(1, 119)
#define KR_INVALID_ENUM_VALUE RL_MAKE_RESULT(1, 120)
#define KR_NOT_FOUND RL_MAKE_RESULT(1, 121)
#define KR_INVALID_THREAD RL_MAKE_RESULT(1, 122)
#define KR_PORT_REMOTE_CLOSED RL_MAKE_RESULT(1, 123)
#define KR_INVALID_STATE RL_MAKE_RESULT(1, 125)
#define KR_RESERVED_VALUE RL_MAKE_RESULT(1, 126)
#define KR_RES_LIMIT_EXCEEDED RL_MAKE_RESULT(1, 132)

/** @brief Memory state flag bits (Ryujinx MemoryState). */
enum {
    MS_PERMISSION_CHANGE_ALLOWED = 1u << 8,
    MS_FORCE_RW_BY_DEBUG = 1u << 9,
    MS_IPC_SEND_TYPE0 = 1u << 10,
    MS_IPC_SEND_TYPE3 = 1u << 11,
    MS_IPC_SEND_TYPE1 = 1u << 12,
    MS_MAPPED = 1u << 13,
    MS_PROCESS_PERMISSION_CHANGE_ALLOWED = 1u << 14,
    MS_MAP_ALLOWED = 1u << 15,
    MS_UNMAP_PROCESS_CODE_ALLOWED = 1u << 16,
    MS_TRANSFER_MEMORY_ALLOWED = 1u << 17,
    MS_QUERY_PHYSICAL_ALLOWED = 1u << 18,
    MS_MAP_DEVICE_ALLOWED = 1u << 19,
    MS_MAP_DEVICE_ALIGNED_ALLOWED = 1u << 20,
    MS_IPC_BUFFER_ALLOWED = 1u << 21,
    MS_POOL_ALLOCATED = 1u << 22,
    MS_MAP_PROCESS_ALLOWED = 1u << 23,
    MS_ATTRIBUTE_CHANGE_ALLOWED = 1u << 24,
    MS_CODE_MEMORY_ALLOWED = 1u << 25,
    MS_LINEAR_MAPPED = 1u << 26,
    MS_PERMISSION_LOCK_ALLOWED = 1u << 27
};

/** @brief Memory states (svcQueryMemory reports the low 8 bits as the type). */
enum {
    MS_FREE = 0x0,
    MS_IO = MS_MAPPED | 0x1,
    MS_NORMAL = MS_MAPPED | MS_QUERY_PHYSICAL_ALLOWED | 0x2,
    MS_CODE_STATIC = MS_FORCE_RW_BY_DEBUG | MS_IPC_SEND_TYPE0 | MS_IPC_SEND_TYPE3 | MS_IPC_SEND_TYPE1 | MS_MAPPED |
                     MS_PROCESS_PERMISSION_CHANGE_ALLOWED | MS_QUERY_PHYSICAL_ALLOWED | MS_MAP_DEVICE_ALLOWED |
                     MS_MAP_DEVICE_ALIGNED_ALLOWED | MS_POOL_ALLOCATED | MS_MAP_PROCESS_ALLOWED | MS_LINEAR_MAPPED | 0x3,
    MS_CODE_MUTABLE = MS_PERMISSION_CHANGE_ALLOWED | MS_IPC_SEND_TYPE0 | MS_IPC_SEND_TYPE3 | MS_IPC_SEND_TYPE1 |
                      MS_MAPPED | MS_MAP_ALLOWED | MS_TRANSFER_MEMORY_ALLOWED | MS_QUERY_PHYSICAL_ALLOWED |
                      MS_MAP_DEVICE_ALLOWED | MS_MAP_DEVICE_ALIGNED_ALLOWED | MS_IPC_BUFFER_ALLOWED | MS_POOL_ALLOCATED |
                      MS_MAP_PROCESS_ALLOWED | MS_ATTRIBUTE_CHANGE_ALLOWED | MS_CODE_MEMORY_ALLOWED | MS_LINEAR_MAPPED |
                      MS_PERMISSION_LOCK_ALLOWED | 0x4,
    MS_HEAP = MS_PERMISSION_CHANGE_ALLOWED | MS_IPC_SEND_TYPE0 | MS_IPC_SEND_TYPE3 | MS_IPC_SEND_TYPE1 | MS_MAPPED |
              MS_MAP_ALLOWED | MS_TRANSFER_MEMORY_ALLOWED | MS_QUERY_PHYSICAL_ALLOWED | MS_MAP_DEVICE_ALLOWED |
              MS_MAP_DEVICE_ALIGNED_ALLOWED | MS_IPC_BUFFER_ALLOWED | MS_POOL_ALLOCATED | MS_ATTRIBUTE_CHANGE_ALLOWED |
              MS_CODE_MEMORY_ALLOWED | MS_LINEAR_MAPPED | 0x5,
    MS_SHARED_MEMORY = MS_MAPPED | MS_POOL_ALLOCATED | MS_LINEAR_MAPPED | 0x6,
    MS_STACK = MS_IPC_SEND_TYPE0 | MS_IPC_SEND_TYPE3 | MS_IPC_SEND_TYPE1 | MS_MAPPED | MS_QUERY_PHYSICAL_ALLOWED |
               MS_MAP_DEVICE_ALLOWED | MS_MAP_DEVICE_ALIGNED_ALLOWED | MS_POOL_ALLOCATED | MS_LINEAR_MAPPED | 0xB,
    MS_THREAD_LOCAL = MS_MAPPED | MS_POOL_ALLOCATED | MS_LINEAR_MAPPED | 0xC,
    MS_TRANSFER_MEMORY = MS_IPC_SEND_TYPE3 | MS_IPC_SEND_TYPE1 | MS_MAPPED | MS_QUERY_PHYSICAL_ALLOWED |
                         MS_MAP_DEVICE_ALLOWED | MS_MAP_DEVICE_ALIGNED_ALLOWED | MS_POOL_ALLOCATED | MS_LINEAR_MAPPED | 0xE,
    MS_INACCESSIBLE = 0x10
};

/** @brief Memory attribute bits. */
enum {
    MA_BORROWED = 1u << 0,
    MA_IPC_MAPPED = 1u << 1,
    MA_DEVICE_MAPPED = 1u << 2,
    MA_UNCACHED = 1u << 3,
    MA_PERMISSION_LOCKED = 1u << 4
};

/** @brief Pseudo handle for the current thread. */
#define HOS_CUR_THREAD 0xFFFF8000u
/** @brief Pseudo handle for the current process. */
#define HOS_CUR_PROCESS 0xFFFF8001u
/** @brief Process id reported to the guest and, in place of what the client wrote, to services. */
#define HOS_PID 0x51ull
/** @brief Handle table size. */
#define HOS_HANDLES 1024u
/** @brief Most handles svcWaitSynchronization takes. */
#define HOS_WAIT_MAX 64

/** @brief Kernel object types. */
enum hos_kobj_type {
    KOBJ_THREAD = 1,
    KOBJ_PROCESS = 2,
    KOBJ_READABLE_EVENT = 3,
    KOBJ_WRITABLE_EVENT = 4,
    KOBJ_SESSION = 5,
    KOBJ_SHARED_MEMORY = 6,
    KOBJ_TRANSFER_MEMORY = 7,
    KOBJ_RESOURCE_LIMIT = 8
};

typedef struct hos_kobj hos_kobj;

/** @brief Base of every kernel object; reference counted. */
struct hos_kobj {
    /** @brief enum hos_kobj_type. */
    uint32_t type;
    /** @brief Reference count. */
    uint32_t refs;
    /** @brief Frees the object when the last reference goes. Called without hos.lock. */
    void (*destroy)(hos_kobj *o);
};

/** @brief Initializes o with one reference. */
void hos_kobj_init(hos_kobj *o, uint32_t type, void (*destroy)(hos_kobj *));
/** @brief Adds a reference. */
void hos_kobj_ref(hos_kobj *o);
/** @brief Drops a reference; destroys the object at zero. */
void hos_kobj_unref(hos_kobj *o);

/** @brief Readable event: the waitable half of an event. */
typedef struct hos_revent {
    /** @brief Base. */
    hos_kobj obj;
    /** @brief Signaled state. */
    bool signaled;
} hos_revent;

/** @brief Writable event: the signaling half; holds its readable event. */
typedef struct hos_wevent {
    /** @brief Base. */
    hos_kobj obj;
    /** @brief The readable half. */
    hos_revent *r;
} hos_wevent;

/** @brief Thread states. */
enum hos_thread_state {
    HT_CREATED = 0,
    HT_RUNNING = 1,
    HT_EXITED = 2
};

/** @brief What a waiting thread waits for. */
enum hos_wait_kind {
    HW_NONE = 0,
    HW_SYNC = 1,
    HW_MUTEX = 2,
    HW_CONDVAR = 3,
    HW_ARBITER = 4,
    HW_SLEEP = 5
};

typedef struct hos_thread hos_thread;

/** @brief Guest thread (KThread). */
struct hos_thread {
    /** @brief Base; waitable, signaled when the thread has exited. */
    hos_kobj obj;
    /** @brief Core runtime thread (context, host thread). */
    struct rl_thread *rt;
    /** @brief Thread id (svcGetThreadId). */
    uint64_t id;
    /** @brief Priority 0 (highest) to 63. */
    int32_t prio;
    /** @brief Ideal core. */
    int32_t core;
    /** @brief Affinity mask. */
    uint64_t affinity;
    /** @brief Guest address of the 0x200-byte TLS slot (tpidrro_el0). */
    uint64_t tls;
    /** @brief Entry, argument and stack top from svcCreateThread. */
    uint64_t entry, arg, stack_top;
    /** @brief enum hos_thread_state. */
    int state;
    /** @brief True for a host thread attached with hos_thread_attach. */
    bool attached;
    /** @brief Wakes this thread; used with hos.lock. */
    pthread_cond_t cv;
    /** @brief Set by the waker. */
    bool woken;
    /** @brief enum hos_wait_kind of the current wait. */
    int wait_kind;
    /** @brief Result the waker gives the waiter. */
    rl_result wait_result;
    /** @brief Objects of the current svcWaitSynchronization. */
    hos_kobj *wait_objs[HOS_WAIT_MAX];
    /** @brief Number of wait_objs. */
    int32_t wait_count;
    /** @brief Index of the object that ended the wait. */
    int32_t sync_index;
    /** @brief svcCancelSynchronization arrived while not waiting. */
    bool cancel_pending;
    /** @brief Mutex address waited for (mutex, condvar) or arbiter address. */
    uint64_t mutex_addr;
    /** @brief Tag this thread writes into the mutex word when it gets the mutex. */
    uint32_t mutex_tag;
    /** @brief Condition variable key waited on. */
    uint64_t cv_key;
    /** @brief The thread that owns the mutex this thread waits for. */
    hos_thread *mutex_owner;
    /** @brief First thread waiting for a mutex this thread owns. */
    hos_thread *mutex_waiters;
    /** @brief Next in the owner's mutex_waiters list. */
    hos_thread *next_mutex_waiter;
    /** @brief Next in hos.cv_waiters or hos.arb_waiters. */
    hos_thread *next_wait;
    /** @brief Next in hos.threads. */
    hos_thread *next_all;
};

/** @brief Most guest views of one shared memory object at a time. */
#define HOS_SHMEM_VIEWS 4

/**
 * @brief Shared memory object: host pages mapped writable for services and
 * mapped again into guest memory for each svcMapSharedMemory, so every view
 * sees the same bytes.
 */
typedef struct hos_shmem {
    /** @brief Base. */
    hos_kobj obj;
    /** @brief Size in bytes the guest maps. */
    uint64_t size;
    /** @brief Host pages. */
    rl_shm shm;
    /** @brief Guest addresses of the mapped views, 0 for a free slot. */
    uint64_t views[HOS_SHMEM_VIEWS];
    /** @brief Permission the guest may map it with. */
    uint32_t user_perm;
} hos_shmem;

/** @brief Transfer memory object: guest memory lent to a service in place. */
typedef struct hos_tmem {
    /** @brief Base. */
    hos_kobj obj;
    /** @brief Guest address. */
    uint64_t addr;
    /** @brief Size in bytes. */
    uint64_t size;
    /** @brief Permission the owner keeps while lent. */
    uint32_t perm;
} hos_tmem;

/** @brief Resource kinds of a resource limit. */
enum {
    RES_MEMORY = 0,
    RES_THREADS = 1,
    RES_EVENTS = 2,
    RES_TRANSFER_MEMORY = 3,
    RES_SESSIONS = 4,
    RES_COUNT = 5
};

/** @brief Resource limit object (reported values only). */
typedef struct hos_reslimit {
    /** @brief Base. */
    hos_kobj obj;
} hos_reslimit;

/** @brief Process start-up configuration, from the NPDM and the manifest. */
typedef struct rl_hos_config {
    /** @brief NPDM address space type: 0 32-bit, 1 36-bit, 2 32-bit without alias, 3 39-bit. */
    uint32_t address_space_type;
    /** @brief Start of the code region (where modules load). */
    uint64_t code_start;
    /** @brief Bytes of the code region. */
    uint64_t code_size;
    /** @brief Program id (svcGetInfo 18). */
    uint64_t program_id;
    /** @brief svcGetInfo TotalMemorySize. */
    uint64_t total_memory;
    /** @brief Allowed core mask (svcGetInfo 0). */
    uint64_t core_mask;
    /** @brief Allowed priority mask (svcGetInfo 1). */
    uint64_t priority_mask;
    /** @brief Most threads. */
    uint32_t max_threads;
    /** @brief Main thread priority. */
    int32_t main_prio;
    /** @brief Main thread ideal core. */
    int32_t main_core;
    /** @brief Main thread stack size. */
    uint64_t main_stack_size;
} rl_hos_config;

/** @brief Address space layout reported through svcGetInfo. */
typedef struct hos_layout {
    /** @brief Address space width in bits. */
    unsigned bits;
    /** @brief Address space start and end. */
    uint64_t as_start, as_end;
    /** @brief Code region. */
    uint64_t code_start, code_end;
    /** @brief ASLR region (svcGetInfo 12, 13). */
    uint64_t aslr_start, aslr_end;
    /** @brief Alias region (svcGetInfo 2, 3). */
    uint64_t alias_start, alias_end;
    /** @brief Heap region (svcGetInfo 4, 5). */
    uint64_t heap_start, heap_end;
    /** @brief Stack region (svcGetInfo 14, 15). */
    uint64_t stack_start, stack_end;
    /** @brief TLS and IO region. */
    uint64_t tlsio_start, tlsio_end;
} hos_layout;

/** @brief Most TLS pages (8 slots each). */
#define HOS_TLS_PAGES 128

/** @brief The guest process and all kernel state. */
typedef struct hos_process {
    /** @brief Kernel lock: guards everything below except vm reads by lifted code. */
    pthread_mutex_t lock;
    /** @brief True after rl_hos_init. */
    bool up;
    /** @brief Configuration. */
    rl_hos_config cfg;
    /** @brief Guest address space. */
    rl_vm vm;
    /** @brief Region layout. */
    hos_layout lay;
    /** @brief Current heap size. */
    uint64_t heap_size;
    /** @brief Main stack bytes, for UsedMemorySize. */
    uint64_t stack_bytes;
    /** @brief Handle table objects. */
    hos_kobj *handles[HOS_HANDLES];
    /** @brief Linear id (bits 15-29) of each handle slot. */
    uint16_t handle_ids[HOS_HANDLES];
    /** @brief Next linear id. */
    uint16_t next_handle_id;
    /** @brief All live threads. */
    hos_thread *threads;
    /** @brief Number of threads created and not yet exited. */
    uint32_t live_threads;
    /** @brief Threads waiting in svcWaitProcessWideKeyAtomic. */
    hos_thread *cv_waiters;
    /** @brief Threads waiting in svcWaitForAddress. */
    hos_thread *arb_waiters;
    /** @brief Next thread id. */
    uint64_t next_thread_id;
    /** @brief TLS page addresses. */
    uint64_t tls_pages[HOS_TLS_PAGES];
    /** @brief Used-slot bit masks per TLS page. */
    uint8_t tls_used[HOS_TLS_PAGES];
    /** @brief Number of TLS pages. */
    uint32_t tls_page_count;
    /** @brief The process resource limit. */
    hos_reslimit *reslimit;
    /** @brief Random entropy for svcGetInfo 11. */
    uint64_t entropy[4];
    /** @brief User exception context address (svcGetInfo 20). */
    uint64_t user_exception_ctx;
} hos_process;

/** @brief The process. */
extern hos_process hos;

/** @brief Fills cfg with defaults for an application with a 39-bit address space. */
RL_RUNTIME_API void rl_hos_default_config(rl_hos_config *cfg);
/**
 * @brief Brings up the process: config, address space (falls back to the
 * 36-bit layout when the host cannot reserve 2^39), fault handler, services.
 * Returns 0 or -1.
 */
RL_RUNTIME_API int rl_hos_init(const rl_hos_config *cfg);
/** @brief Tears the process down (tests): threads must have exited. */
RL_RUNTIME_API void rl_hos_shutdown(void);
/** @brief Guest memory base. */
static inline uint8_t *hos_mem(void) { return hos.vm.base; }

/** @brief Maps a module segment with state CODE_STATIC (text, rodata) or CODE_MUTABLE (data, bss). */
RL_RUNTIME_API rl_result rl_hos_map_segment(uint64_t addr, uint64_t size, uint32_t perm, uint32_t state);
/** @brief Maps the main thread stack in the stack region; *top receives its top. */
RL_RUNTIME_API rl_result rl_hos_map_main_stack(uint64_t size, uint64_t *top);

/**
 * @brief Makes the calling host thread a guest thread (the program's main
 * thread, or a test). Returns its context, or NULL.
 */
RL_RUNTIME_API rl_ctx *rl_hos_attach_thread(int32_t prio, int32_t core);
/** @brief Undoes rl_hos_attach_thread for the calling thread. */
RL_RUNTIME_API void rl_hos_detach_thread(void);
/**
 * @brief Runs guest code at entry on the calling attached thread as the main
 * thread (x0 = 0, x1 = main thread handle, sp = stack_top) and handles exits.
 */
RL_RUNTIME_API int rl_hos_run_main(uint64_t entry, uint64_t stack_top);

/** @brief The current guest thread, or NULL. */
hos_thread *hos_current(void);
/** @brief Locks the kernel. */
void hos_lock(void);
/** @brief Unlocks the kernel. */
void hos_unlock(void);

/** @brief Adds o to the handle table (adds a reference). Lock held. */
rl_result hos_handle_add(hos_kobj *o, uint32_t *out);
/** @brief Looks up a handle of the given type (0 any) and adds a reference; NULL if invalid. Lock held. */
hos_kobj *hos_handle_get(uint32_t h, uint32_t type);
/** @brief Removes a handle; returns the object (the caller drops the table's reference after unlocking). Lock held. */
hos_kobj *hos_handle_remove(uint32_t h);
/** @brief Number of used handle slots (tests). */
uint32_t hos_handle_count(void);

/** @brief Creates a readable event (one reference). */
RL_RUNTIME_API hos_revent *rl_hos_event_create(void);
/** @brief Signals a readable event and wakes its waiters. Takes the lock. */
RL_RUNTIME_API void rl_hos_event_signal(hos_revent *e);
/** @brief Clears a readable event. Takes the lock. */
RL_RUNTIME_API void rl_hos_event_clear(hos_revent *e);

/** @brief Creates a shared memory object (service side), zero filled. */
RL_RUNTIME_API hos_shmem *rl_hos_shmem_create(uint64_t size, uint32_t user_perm);
/** @brief Host pointer to a shared memory object's bytes (always writable, coherent with every guest view). */
RL_RUNTIME_API uint8_t *rl_hos_shmem_ptr(hos_shmem *s);

/** @brief Wakes threads waiting on o if it is signaled. Lock held. */
void hos_sync_signal(hos_kobj *o);
/** @brief Blocks t (the caller) until woken or timeout_ns passes (-1 forever). Lock held; returns true if woken. */
bool hos_block(hos_thread *t, int64_t timeout_ns);
/** @brief Wakes t with result r. Lock held. */
void hos_wake(hos_thread *t, rl_result r);

/** @brief Memory svcs. */
rl_result hos_set_heap_size(uint64_t size, uint64_t *out);
rl_result hos_set_memory_permission(uint64_t addr, uint64_t size, uint32_t perm);
rl_result hos_set_memory_attribute(uint64_t addr, uint64_t size, uint32_t mask, uint32_t value);
rl_result hos_map_memory(uint64_t dst, uint64_t src, uint64_t size);
rl_result hos_unmap_memory(uint64_t dst, uint64_t src, uint64_t size);
rl_result hos_query_memory(uint64_t info_addr, uint64_t addr, uint32_t *page_info);
rl_result hos_map_shared_memory(uint32_t h, uint64_t addr, uint64_t size, uint32_t perm);
rl_result hos_unmap_shared_memory(uint32_t h, uint64_t addr, uint64_t size);
rl_result hos_create_transfer_memory(uint32_t *out, uint64_t addr, uint64_t size, uint32_t perm);
/** @brief True when guest range [addr, addr+size) is mapped with the given user permission bits. Lock held. */
bool hos_user_range(uint64_t addr, uint64_t size, uint32_t perm);
/** @brief Memory in use for svcGetInfo 7. */
uint64_t hos_used_memory(void);
/** @brief Allocates a TLS slot; returns its guest address or 0. Lock held. */
uint64_t hos_tls_alloc(void);
/** @brief Frees a TLS slot. Lock held. */
void hos_tls_free(uint64_t slot);
/** @brief Sets up the address space for the configuration. */
int hos_mem_init(void);

/** @brief Thread svcs. */
rl_result hos_create_thread(uint32_t *out, uint64_t entry, uint64_t arg, uint64_t stack_top, int32_t prio, int32_t core);
rl_result hos_start_thread(uint32_t h);
void hos_sleep_thread(int64_t ns);
rl_result hos_get_thread_priority(uint32_t h, int32_t *out);
rl_result hos_set_thread_priority(uint32_t h, int32_t prio);
rl_result hos_get_thread_core_mask(uint32_t h, int32_t *core, uint64_t *mask);
rl_result hos_set_thread_core_mask(uint32_t h, int32_t core, uint64_t mask);
rl_result hos_get_thread_id(uint32_t h, uint64_t *out);
/** @brief Called by a thread when it leaves guest code: marks it exited and wakes joiners. */
void hos_thread_exited(hos_thread *t);
/** @brief Looks up a thread by handle (pseudo handles allowed) with a reference. Lock held. */
hos_thread *hos_thread_get(uint32_t h);

/** @brief Synchronization svcs. */
rl_result hos_wait_sync(int32_t *index, uint64_t handles_addr, int32_t count, int64_t timeout);
rl_result hos_cancel_sync(uint32_t h);
rl_result hos_signal_event(uint32_t h);
rl_result hos_clear_event(uint32_t h);
rl_result hos_reset_signal(uint32_t h);
rl_result hos_create_event(uint32_t *w, uint32_t *r);
rl_result hos_arbitrate_lock(uint32_t owner, uint64_t addr, uint32_t tag);
rl_result hos_arbitrate_unlock(uint64_t addr);
rl_result hos_wait_process_wide_key(uint64_t mutex, uint64_t key, uint32_t tag, int64_t timeout);
void hos_signal_process_wide_key(uint64_t key, int32_t count);
rl_result hos_wait_for_address(uint64_t addr, uint32_t type, int32_t value, int64_t timeout);
rl_result hos_signal_to_address(uint64_t addr, uint32_t type, int32_t value, int32_t count);

/** @brief Guest memory word helpers for the kernel: atomic on the host, with a mapped check. */
bool hos_read_u32(uint64_t addr, uint32_t *v);
/** @brief True when the guest word at addr is mapped and user-writable. */
bool hos_writable_u32(uint64_t addr);
/** @brief Atomic 32-bit store into guest memory. */
bool hos_write_u32(uint64_t addr, uint32_t v);
/** @brief Atomic 32-bit compare-and-swap in guest memory. */
bool hos_cas_u32(uint64_t addr, uint32_t *expected, uint32_t desired);

#endif
