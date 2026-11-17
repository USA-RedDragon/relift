/**
 * @file relift/abi_core.h
 * @brief The CPU-neutral half of the contract between relift-emitted C and
 * the relift C runtime: export macros, return codes, the pending record, the
 * block of the context the core owns, unit registration and the dev protocol.
 *
 * Lifted code does not include this header directly: it includes its CPU
 * layer's header (under runtime/cpus/), which includes this one and defines
 * struct rl_ctx. The runtime core includes only this one, so it never
 * sees a guest register. The rationale is in docs/abi.md. Any change to a type,
 * layout, signature or constant here bumps RL_ABI_VERSION.
 */
#ifndef RELIFT_ABI_CORE_H
#define RELIFT_ABI_CORE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if !defined(__clang__) && !defined(__GNUC__)
#error "relift/abi_core.h needs clang or a gcc-compatible compiler (zig cc is clang)"
#endif
#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "relift supports little-endian hosts only"
#endif
#if UINTPTR_MAX != UINT64_MAX
#error "relift supports 64-bit hosts only"
#endif

/**
 * @brief Marks a runtime entry point declared in a runtime header.
 *
 * The runtime library is built with hidden visibility by default, so every entry
 * a lifted or hot-loaded unit calls by name carries this. ELF and Mach-O: default
 * visibility. Windows: dllexport while building the runtime as a DLL (define
 * RL_BUILDING_RUNTIME), dllimport in everything else. Define RL_STATIC_RUNTIME
 * where the runtime is linked statically into the program (shipped builds) to
 * make it empty on Windows.
 */
#if defined(_WIN32) && !defined(RL_STATIC_RUNTIME)
#if defined(RL_BUILDING_RUNTIME)
#define RL_RUNTIME_API __declspec(dllexport)
#else
#define RL_RUNTIME_API __declspec(dllimport)
#endif
#elif defined(_WIN32)
#define RL_RUNTIME_API
#else
#define RL_RUNTIME_API __attribute__((visibility("default")))
#endif

/**
 * @brief Marks the `relift_unit` function a hot-loaded library exports.
 *
 * Units are built with hidden visibility; this is the only symbol they export.
 */
#if defined(_WIN32)
#define RL_UNIT_EXPORT __declspec(dllexport)
#else
#define RL_UNIT_EXPORT __attribute__((visibility("default")))
#endif

/** @brief Contract version. A hot-loaded unit with a different value is refused. */
#define RL_ABI_VERSION 10u

/** @brief Lifted function return on a CPU without a link register: normal return to the caller. A CPU with one returns where its return went instead (docs/abi.md section 5.4). */
#define RL_OK 0
/** @brief Lifted function return: a pending record is set in the context (pend.kind is not RL_PEND_NONE); return 1 up the host stack. */
#define RL_PENDING 1

/** @brief Pending record kinds (rl_pending.kind). */
enum rl_pending_kind {
    /** @brief Nothing pending. */
    RL_PEND_NONE = 0,
    /** @brief Guest unwind: resume at pend.pc in the frame whose guest stack pointer equals pend.sp. */
    RL_PEND_UNWIND = 1,
    /** @brief The guest thread exits (the guest OS's thread exit call); pend.code is unused. */
    RL_PEND_EXIT_THREAD = 2,
    /** @brief The process exits or stopped on a fatal guest error; pend.code is the exit status. */
    RL_PEND_EXIT_PROCESS = 3,
    /**
     * @brief Control goes to pend.pc with every host frame gone: frames pass the record up
     * (pend.sp is RL_SP_NONE) to the thread entry, which restarts at pend.pc (rl_run_from).
     */
    RL_PEND_REDIRECT = 4
};

/** @brief pend.sp value that no guest frame can match (the CPU layers keep guest stack pointers aligned). */
#define RL_SP_NONE UINT64_MAX

/** @brief 128-bit unsigned integer (clang and gcc extension; zig cc supports it on every target). */
__extension__ typedef unsigned __int128 rl_u128;

/** @brief Pending record: why lifted frames are returning RL_PENDING. */
typedef struct rl_pending {
    /** @brief RL_PEND_UNWIND: guest address of the landing pad to enter. */
    uint64_t pc;
    /** @brief RL_PEND_UNWIND: guest stack pointer of the target frame; RL_SP_NONE for the other kinds. */
    uint64_t sp;
    /** @brief One of enum rl_pending_kind. */
    uint32_t kind;
    /** @brief Exit status for RL_PEND_EXIT_PROCESS. */
    uint32_t code;
} rl_pending;

/** @brief Runtime thread object; opaque to lifted code. */
struct rl_thread;

/**
 * @brief Guest thread context. Each CPU layer defines struct rl_ctx (its
 * register file plus an rl_ctx_common block); the core sees it only as an
 * opaque pointer and reaches the common block through the CPU descriptor.
 */
typedef struct rl_ctx rl_ctx;

/**
 * @brief Type of every lifted function, host override with the lifted ABI, and
 * dispatch target. Returns RL_OK or RL_PENDING, or on a CPU with a link
 * register the guest address its return went to, or RL_PENDING with a
 * pending record set (docs/abi.md section 5.4).
 */
typedef uint64_t rl_fn_t(rl_ctx *c, uint8_t *mem);
/** @brief Pointer to a lifted function. */
typedef rl_fn_t *rl_fn;

/**
 * @brief The part of every rl_ctx the core owns. A CPU layer lays these fields
 * out in this order at one offset of its rl_ctx (it may spell them as plain
 * members of its own struct) and gives the offset to the core.
 */
typedef struct rl_ctx_common {
    /** @brief Pending record, valid while lifted frames return RL_PENDING. */
    rl_pending pend;
    /** @brief Runtime thread object. */
    struct rl_thread *thread;
    /** @brief Dispatch table: one entry per code granule of the CPU from disp_lo; NULL means miss. */
    rl_fn *disp_table;
    /** @brief Lowest guest code address covered by disp_table. */
    uint64_t disp_lo;
    /** @brief Number of entries in disp_table. */
    uint64_t disp_count;
} rl_ctx_common;

_Static_assert(sizeof(rl_pending) == 24, "rl_pending size");
_Static_assert(offsetof(rl_ctx_common, pend) == 0, "common pend");
_Static_assert(offsetof(rl_ctx_common, thread) == 24, "common thread");
_Static_assert(offsetof(rl_ctx_common, disp_table) == 32, "common disp_table");
_Static_assert(offsetof(rl_ctx_common, disp_lo) == 40, "common disp_lo");
_Static_assert(offsetof(rl_ctx_common, disp_count) == 48, "common disp_count");
_Static_assert(sizeof(rl_ctx_common) == 56, "rl_ctx_common size");

/**
 * @brief The guest cycle clock of a CPU whose guest drives devices through
 * registers (docs/abi.md section 13). The CPU layer puts one in its rl_ctx and
 * gives its offset in rl_cpu_desc.time_offset. The CPU is at cycle
 * deadline - budget. Lifted code subtracts block costs from budget and calls
 * rl_sync at a sync point when budget is zero or below; it keeps budget in a
 * local and stores it here before every runtime call.
 */
typedef struct rl_time {
    /** @brief Cycles left until the scheduler must run. */
    int64_t budget;
    /** @brief The cycle at which budget reaches zero. */
    uint64_t deadline;
} rl_time;

_Static_assert(offsetof(rl_time, budget) == 0, "time budget");
_Static_assert(offsetof(rl_time, deadline) == 8, "time deadline");
_Static_assert(sizeof(rl_time) == 16, "rl_time size");

/**
 * @brief The calling convention of rare calls out of lifted code, such as
 * rl_preempt: the callee keeps every register, so the caller saves none of
 * its live values around the call and needs no stack frame for them, and
 * the compiler lays the call out of the hot path.
 */
#if defined(__has_attribute)
#if __has_attribute(preserve_all) && (defined(__x86_64__) || defined(__aarch64__))
#define RL_COLD_CALL __attribute__((preserve_all, cold))
#endif
#endif
#ifndef RL_COLD_CALL
#define RL_COLD_CALL __attribute__((cold))
#endif

/**
 * @brief Whether the budget of t is spent, read at a preemption point of a
 * CPU whose guest OS preempts threads (docs/abi.md section 14.1): there the
 * budget stays positive while the thread may run on, and another host thread
 * ends it to make the thread call rl_preempt.
 */
static inline int rl_time_spent(rl_time *t) { return __atomic_load_n(&t->budget, __ATOMIC_RELAXED) <= 0; }
/**
 * @brief The slow path of a preemption point: runs the guest layer's
 * scheduler, which may block until the thread may run again. The context's
 * register fields are stale here (the registers live in the caller's
 * locals) and stay untouched.
 */
RL_RUNTIME_API RL_COLD_CALL void rl_preempt(rl_ctx *c);

/** @brief The interrupt hook's answer when no interrupt is taken. */
#define RL_NO_IRQ UINT64_MAX
/**
 * @brief The interrupt hook's answer when it took the interrupt as a
 * redirect (RL_PEND_REDIRECT) to its entry instead of a nested call: the
 * host frames unwind and the run goes on at the entry, for a handler that
 * does not return to the code it interrupted (it switches threads).
 */
#define RL_IRQ_REDIRECT (UINT64_MAX - 1u)

/**
 * @brief Runs devices and events up to the CPU's cycle, sets a new budget
 * and enters any interrupt the guest takes, as a nested call. When the handler
 * returns somewhere other than next (the CPU layer's return hook says so), the
 * record is RL_PEND_REDIRECT and this returns RL_PENDING. Called at a sync point when budget is zero or below, after the exit's
 * register stores; next is the guest pc the block goes to. Returns RL_OK, or
 * RL_PENDING to propagate the pending record.
 */
RL_RUNTIME_API int rl_sync(rl_ctx *c, uint8_t *mem, uint64_t next);
/**
 * @brief Tells the scheduler which interrupt lines may interrupt the CPU
 * from its current cycle on (rl_sched_set_mask on the CPU's clock). A CPU
 * layer whose interrupt enable lives in a register calls this wherever the
 * enable changes, with the lines the new state allows: all of them or none
 * for one master enable, the enabled lines for a per-line enable. When a
 * line in the new mask is already raised, the budget ends so the next sync
 * point sees it. Any delay between an enable instruction and its effect
 * is the caller's: call at the cycle the enable takes hold.
 */
RL_RUNTIME_API void rl_set_irq_mask(rl_ctx *c, uint64_t mask);
/** @brief A guest read of size bytes (1, 2, 4 or 8) at an address that may be a device register, at the CPU's current cycle. */
RL_RUNTIME_API uint64_t rl_io_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size);
/** @brief A guest write of size bytes of value (little-endian) at an address that may be a device register, at the CPU's current cycle. */
RL_RUNTIME_API void rl_io_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value);

/** @brief Defines a lifted function with the contract signature. */
#define RL_LIFTED(name) uint64_t name(rl_ctx *restrict c, uint8_t *mem)
/** @brief Marks a guaranteed tail call: `RL_MUSTTAIL return f(c, mem);`. */
#define RL_MUSTTAIL __attribute__((musttail))
/** @brief Marks the unlikely branch of a pending check. */
#define RL_UNLIKELY(x) __builtin_expect(!!(x), 0)
/**
 * @brief A transfer through dispatch that leaves no host frame (a computed
 * jump, a tail call, a jump to code outside the function's known targets),
 * as a statement inside a lifted function: `RL_JUMP(target, site);`, where
 * site is the guest address of the transferring instruction. The hot path is
 * the CPU layer's rl_jump_find and a tail call. On a miss the site goes to
 * c->pend.pc (unused while pend.kind is RL_PEND_NONE) and the CPU layer's
 * rl_dispatch_jump_miss runs in place, so the stop report names the site
 * while the hot path carries no constant.
 */
#define RL_JUMP(target, site)                                                                           \
    do {                                                                                                \
        rl_fn rl_j_ = rl_jump_find(c, (target));                                                        \
        if (RL_UNLIKELY(!rl_j_)) {                                                                      \
            c->pend.pc = (site);                                                                        \
            RL_MUSTTAIL return rl_dispatch_jump_miss(c, mem);                                           \
        }                                                                                               \
        RL_MUSTTAIL return rl_j_(c, mem);                                                               \
    } while (0)
/** @brief Marks the likely branch of a return check. */
#define RL_LIKELY(x) __builtin_expect(!!(x), 1)
/** @brief Compiler-only barrier at the back edge of a loop whose exit depends on a guest load. */
#define RL_LOOP_FENCE() __atomic_signal_fence(__ATOMIC_SEQ_CST)

/** @brief Called from a landing-pad dispatch whose switch has no case for pend.pc. Reports and stops. */
RL_RUNTIME_API _Noreturn void rl_bad_resume(rl_ctx *c, uint8_t *mem, uint64_t fn_guest_addr);

/** @brief Flags of a unit function entry (rl_fn_entry.flags). */
enum rl_fn_flags {
    /** @brief The function may return RL_PENDING; callers need a pending check. */
    RL_FN_MAY_UNWIND = 1u << 0,
    /** @brief The function has a landing-pad dispatch. */
    RL_FN_LANDING_PADS = 1u << 1,
    /** @brief The entry is not a function start (indirect jump target, hot-loaded fragment). */
    RL_FN_FRAGMENT = 1u << 2,
    /** @brief The entry is a host override of a guest symbol and replaces any lifted entry. */
    RL_FN_OVERRIDE = 1u << 3,
    /** @brief The entry fills every empty slot of [guest_addr, guest_addr + guest_size) with fn, rather than the one at guest_addr (a run of instructions of one enter-anywhere piece). */
    RL_FN_RANGE = 1u << 4
};

/** @brief One function a unit provides. */
typedef struct rl_fn_entry {
    /** @brief Guest entry address. */
    uint64_t guest_addr;
    /** @brief Guest bytes covered by this function (its lifted extent), for crash mapping. */
    uint32_t guest_size;
    /** @brief enum rl_fn_flags. */
    uint32_t flags;
    /** @brief Host function. */
    rl_fn fn;
} rl_fn_entry;

/** @brief Host code address to guest address map entry, sorted by host_off. */
typedef struct rl_line_entry {
    /** @brief Offset of the host instruction from the unit's code base. */
    uint32_t host_off;
    /** @brief Guest address of the guest instruction it was lifted from. */
    uint64_t guest_addr;
} rl_line_entry;

/** @brief rl_fn_map.version of this contract. */
#define RL_FN_MAP_VERSION 2u

/**
 * @brief One host function in a function map: its guest range, the guest
 * function it belongs to, host code and names. A split function has an entry
 * per piece, each over the guest range the piece holds; its stub has an
 * entry with guest_size 0.
 */
typedef struct rl_fn_info {
    /** @brief First guest address the host function covers. */
    uint64_t guest_addr;
    /** @brief Guest entry address of the guest function (guest_addr for a function that is not split). */
    uint64_t fn_addr;
    /** @brief Guest bytes it covers; [guest_addr, guest_addr + guest_size). */
    uint32_t guest_size;
    /** @brief enum rl_fn_flags, as in the function's rl_fn_entry. */
    uint32_t flags;
    /** @brief Host function. */
    rl_fn fn;
    /** @brief Host symbol of fn: the C identifier the emitter gave it. */
    const char *host_sym;
    /** @brief Readable guest name (the demangled guest symbol), or NULL to print host_sym. */
    const char *guest_name;
} rl_fn_info;

/**
 * @brief The function map of one shard: guest address ranges to function
 * names and host symbols, for crash reports and backtraces (docs/abi.md
 * section 11).
 */
typedef struct rl_fn_map {
    /** @brief RL_FN_MAP_VERSION. */
    uint32_t version;
    /** @brief Number of entries in fns. */
    uint32_t count;
    /** @brief Functions, sorted by guest_addr, ranges not overlapping. */
    const rl_fn_info *fns;
    /** @brief Guest module the functions belong to. */
    const char *module;
    /** @brief Shard name for reports. */
    const char *shard;
    /** @brief Guest load address of the module, for module+offset in reports. */
    uint64_t module_base;
    /** @brief Start of the host section that holds only lifted functions (RL_TEXT_LO), or NULL when unknown. */
    const void *text_lo;
    /** @brief End of that section (RL_TEXT_HI), or NULL. */
    const void *text_hi;
} rl_fn_map;

/**
 * @brief Places a lifted function in the section that holds only lifted
 * functions, so a host pc in [RL_TEXT_LO, RL_TEXT_HI) is lifted code and
 * belongs to the nearest function start below it.
 */
#if defined(RL_COLD_TEXT) && defined(__ELF__) && defined(__x86_64__)
#define RL_TEXT_SECTION "relift_cold"
#elif defined(_WIN32)
#define RL_TEXT_SECTION "relift$m"
#elif defined(__APPLE__)
#define RL_TEXT_SECTION "__TEXT,__relift_text,regular,pure_instructions"
#else
#define RL_TEXT_SECTION "relift_text"
#endif
/** @brief Attribute for a lifted function: `RL_TEXT RL_LIFTED(F) { ... }`. */
#define RL_TEXT __attribute__((section(RL_TEXT_SECTION)))

/**
 * @brief Declares the bounds of the lifted-code section in a translation
 * unit that names RL_TEXT_LO and RL_TEXT_HI. On Windows, exactly one
 * translation unit per image also expands RL_TEXT_BOUNDS_DEFINE().
 */
#if defined(RL_COLD_TEXT) && defined(__ELF__) && defined(__x86_64__)
#define RL_TEXT_BOUNDS_DECLARE()                                                                        \
    extern const char __start_relift_cold[] __attribute__((visibility("hidden")));                     \
    extern const char __stop_relift_cold[] __attribute__((visibility("hidden")))
#define RL_TEXT_BOUNDS_DEFINE()
#define RL_TEXT_LO ((const void *)__start_relift_cold)
#define RL_TEXT_HI ((const void *)__stop_relift_cold)
#elif defined(_WIN32)
#define RL_TEXT_BOUNDS_DECLARE() void rl_text_lo_mark(void); void rl_text_hi_mark(void)
#define RL_TEXT_BOUNDS_DEFINE()                                                                         \
    __attribute__((section("relift$a"), used)) void rl_text_lo_mark(void) {}                            \
    __attribute__((section("relift$z"), used)) void rl_text_hi_mark(void) {}
#define RL_TEXT_LO ((const void *)rl_text_lo_mark)
#define RL_TEXT_HI ((const void *)rl_text_hi_mark)
#elif defined(__APPLE__)
#define RL_TEXT_BOUNDS_DECLARE()                                                                        \
    extern const char rl_text_lo_sym[] __asm("section$start$__TEXT$__relift_text");                     \
    extern const char rl_text_hi_sym[] __asm("section$end$__TEXT$__relift_text")
#define RL_TEXT_BOUNDS_DEFINE()
#define RL_TEXT_LO ((const void *)rl_text_lo_sym)
#define RL_TEXT_HI ((const void *)rl_text_hi_sym)
#else
#define RL_TEXT_BOUNDS_DECLARE()                                                                        \
    extern const char __start_relift_text[] __attribute__((visibility("hidden")));                     \
    extern const char __stop_relift_text[] __attribute__((visibility("hidden")))
#define RL_TEXT_BOUNDS_DEFINE()
#define RL_TEXT_LO ((const void *)__start_relift_text)
#define RL_TEXT_HI ((const void *)__stop_relift_text)
#endif

/** @brief Size in bytes of rl_unit.image_id. */
#define RL_IMAGE_ID_BYTES 32u

/**
 * @brief A unit: the registration record of statically linked lifted code or
 * of one hot-loaded library.
 */
typedef struct rl_unit {
    /** @brief RL_ABI_VERSION the unit was built against. */
    uint32_t abi_version;
    /** @brief sizeof(rl_ctx) the unit was built against. */
    uint32_t ctx_size;
    /** @brief Identity of the guest image set (modules and game.lock) the unit was lifted from. */
    uint8_t image_id[RL_IMAGE_ID_BYTES];
    /** @brief Number of entries in fns. */
    uint32_t fn_count;
    /** @brief Number of entries in lines; 0 when the map is a side file. */
    uint32_t line_count;
    /** @brief Functions, sorted by guest_addr. */
    const rl_fn_entry *fns;
    /** @brief Host-to-guest line map, or NULL. */
    const rl_line_entry *lines;
    /** @brief Host address that rl_line_entry.host_off is relative to. */
    const void *code_base;
    /** @brief Unit name for reports. */
    const char *name;
    /** @brief Number of entries in maps. */
    uint32_t map_count;
    /** @brief Function maps of the unit's shards (crash reports), or NULL. */
    const rl_fn_map *const *maps;
} rl_unit;

/** @brief The class of one dispatch slot in an rl_code_map. */
enum rl_code_class {
    /** @brief No instruction (padding or data in an executable page): dispatch there stops the program. */
    RL_CODE_TRAP = 0,
    /** @brief An entry of the module's hot unit. */
    RL_CODE_HOT = 1,
    /** @brief An instruction only the module's cold unit enters. */
    RL_CODE_COLD = 2
};

/** @brief Dispatch slots per page of an rl_code_map. */
#define RL_CODE_PAGE_SLOTS 1024u
/** @brief Bytes of one page pattern: two bits per slot. */
#define RL_CODE_PAGE_BYTES (RL_CODE_PAGE_SLOTS / 4u)
/** @brief rl_code_map.version of this contract. */
#define RL_CODE_MAP_VERSION 1u

/**
 * @brief The class of every dispatch slot of a module's executable pages
 * (docs/abi.md section 5.1): with it the dispatch table over those pages has
 * no empty slot. Slot k of a page is bits 2 * (k % 4) and up of byte k / 4 of
 * its pattern, an enum rl_code_class.
 */
typedef struct rl_code_map {
    /** @brief RL_CODE_MAP_VERSION. */
    uint32_t version;
    /** @brief Number of pages. */
    uint32_t page_count;
    /** @brief Number of patterns. */
    uint32_t pattern_count;
    /** @brief Guest address of the first slot of the first page. */
    uint64_t lo;
    /** @brief Per page, the index of its pattern in patterns. */
    const uint32_t *pages;
    /** @brief The page patterns, RL_CODE_PAGE_BYTES each. */
    const uint8_t *patterns;
    /** @brief The module's hot unit: an entry at every RL_CODE_HOT slot. */
    const rl_unit *hot;
    /** @brief The module's cold unit: an entry at every RL_CODE_COLD slot. */
    const rl_unit *cold;
    /** @brief Module name for reports. */
    const char *module;
} rl_code_map;

/** @brief Name of the function a hot-loaded library exports: `RL_UNIT_EXPORT const rl_unit *relift_unit(void)`. */
#define RL_UNIT_SYMBOL "relift_unit"
/** @brief Type of the exported RL_UNIT_SYMBOL function. */
typedef const rl_unit *rl_unit_fn(void);

/** @brief Dev protocol frame magic, "RLDV" little-endian. */
#define RL_DEV_MAGIC 0x56444C52u
/**
 * @brief Environment variable with the dev socket path.
 *
 * The path must fit sockaddr_un.sun_path (108 bytes on Linux, 104 on macOS,
 * including the NUL). relift picks a short path; the runtime reports a longer one
 * as a dev path failure.
 */
#define RL_DEV_SOCKET_ENV "RELIFT_DEV_SOCKET"

/** @brief Dev protocol message kinds. */
enum rl_dev_kind {
    /** @brief Runtime to relift, once per connection: payload rl_dev_hello. */
    RL_DEV_HELLO = 1,
    /** @brief relift to runtime: payload rl_dev_reply, no path. */
    RL_DEV_HELLO_REPLY = 2,
    /** @brief Runtime to relift: payload rl_dev_lift_req. */
    RL_DEV_LIFT = 3,
    /** @brief relift to runtime: payload rl_dev_reply, then path_len bytes of library path, then msg_len bytes of text. */
    RL_DEV_LIFT_REPLY = 4
};

/** @brief Why the runtime asks for code (rl_dev_lift_req.reason). */
enum rl_miss_reason {
    /** @brief Indirect call to an address with no entry. */
    RL_MISS_CALL = 1,
    /** @brief Indirect jump or tail call to an address with no entry. */
    RL_MISS_JUMP = 2,
    /** @brief Thread entry or runtime callback into guest code. */
    RL_MISS_ENTRY = 3,
    /** @brief A landing pad with no case in its function's dispatch. */
    RL_MISS_RESUME = 4
};

/** @brief Values of rl_dev_reply.status. */
enum rl_dev_status {
    /** @brief Success. */
    RL_DEV_OK = 0,
    /** @brief relift failed for a reason not listed here; msg has the text. */
    RL_DEV_ERR_INTERNAL = 1,
    /** @brief relift refused the hello (image id or ABI version differs from the project's). */
    RL_DEV_ERR_HELLO = 2,
    /** @brief The lift or the compile of the library failed; msg has the compiler output. */
    RL_DEV_ERR_COMPILE = 3
};

/**
 * @brief Process exit status (EX_SOFTWARE) when the runtime stops on a missed-code
 * report: a shipped build missed, or the dev path failed (no socket, path too
 * long, refused hello, failed compile, a unit that fails the registration checks).
 */
#define RL_EXIT_SOFTWARE 70

/** @brief Header of every dev protocol frame; payload_len bytes follow. */
typedef struct rl_dev_hdr {
    /** @brief RL_DEV_MAGIC. */
    uint32_t magic;
    /** @brief enum rl_dev_kind. */
    uint16_t kind;
    /** @brief Reserved, zero. */
    uint16_t reserved;
    /** @brief Request sequence number; a reply carries the request's. */
    uint32_t seq;
    /** @brief Payload bytes after this header. */
    uint32_t payload_len;
} rl_dev_hdr;

/** @brief RL_DEV_HELLO payload. */
typedef struct rl_dev_hello {
    /** @brief RL_ABI_VERSION of the runtime. */
    uint32_t abi_version;
    /** @brief Host page size. */
    uint32_t host_page;
    /** @brief Image set identity; relift refuses a different one. */
    uint8_t image_id[RL_IMAGE_ID_BYTES];
    /** @brief Host process id, for relift's logs. */
    uint64_t pid;
} rl_dev_hello;

/** @brief RL_DEV_LIFT payload. */
typedef struct rl_dev_lift_req {
    /** @brief Guest address that missed. */
    uint64_t target;
    /** @brief Guest address of the instruction that branched there. Best effort: 0 when unknown, and lost after a tail call. */
    uint64_t site;
    /** @brief Guest thread id. */
    uint64_t thread_id;
    /**
     * @brief enum rl_miss_reason. Best effort: the runtime only knows the
     * target at a miss, so it cannot tell an indirect call from an indirect
     * jump and may report RL_MISS_CALL for both. relift must not depend on it.
     */
    uint32_t reason;
    /** @brief Floating-point control value the thread runs with, as the CPU layer defines it (docs/abi.md section 6). */
    uint32_t fp_mode;
} rl_dev_lift_req;

/** @brief RL_DEV_HELLO_REPLY and RL_DEV_LIFT_REPLY payload head. */
typedef struct rl_dev_reply {
    /** @brief enum rl_dev_status; 0 on success, otherwise msg holds the text. */
    int32_t status;
    /** @brief Bytes of the UTF-8 absolute path to the shared library (no NUL). */
    uint32_t path_len;
    /** @brief Bytes of the UTF-8 message (no NUL). */
    uint32_t msg_len;
    /** @brief Reserved, zero. */
    uint32_t reserved;
} rl_dev_reply;

_Static_assert(sizeof(rl_fn_entry) == 24, "rl_fn_entry size");
_Static_assert(sizeof(rl_fn_info) == 48, "rl_fn_info size");
_Static_assert(sizeof(rl_fn_map) == 56, "rl_fn_map size");
_Static_assert(sizeof(rl_dev_hdr) == 16, "rl_dev_hdr size");
_Static_assert(sizeof(rl_dev_hello) == 48, "rl_dev_hello size");
_Static_assert(sizeof(rl_dev_lift_req) == 32, "rl_dev_lift_req size");
_Static_assert(sizeof(rl_dev_reply) == 16, "rl_dev_reply size");

#endif
