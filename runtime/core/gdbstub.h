/**
 * @file gdbstub.h
 * docgen:region overview
 * @brief A GDB remote-protocol server for guest state: gdb or lldb attach over
 * TCP and see guest threads, guest registers and guest memory by guest address
 * (.work/design/debug-stub.md). The core owns the protocol and the stop; a CPU
 * layer describes its registers with an rl_gdb_cpu, a guest layer lists its
 * threads with an rl_gdb_guest and calls rl_gdb_point at its safe points.
 *
 * Off unless the debug.gdb setting names a port; the server listens on
 * 127.0.0.1 only (on a phone, reach it with adb forward). Built where
 * RL_DEBUG_STUB is defined: this header defines it on Linux, Android and macOS
 * unless RL_NO_DEBUG_STUB is, which relift build sets for every player package
 * (an AppImage, a signed .app, an iOS app, a release-signed APK). Elsewhere
 * every call is an empty inline and the server is not in the program.
 * docgen:end
 */
#ifndef RELIFT_GDBSTUB_H
#define RELIFT_GDBSTUB_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#if !defined(RL_NO_DEBUG_STUB) && !defined(RL_DEBUG_STUB) && !defined(_WIN32) &&                                      \
    !(defined(__APPLE__) && TARGET_OS_IPHONE) && !defined(__wasm__)
/** @brief The stub is built in. */
#define RL_DEBUG_STUB 1
#endif

/** @brief One guest call frame of a backtrace: its pc, and the sp and frame pointer it runs with. */
typedef struct rl_gdb_frame {
    uint64_t pc, sp, fp;
} rl_gdb_frame;

/** @brief Reads n bytes of guest memory at a into dst; false when the guest cannot read them. */
typedef bool (*rl_gdb_read_fn)(uint64_t a, void *dst, size_t n);

/** @brief A CPU layer's registers as gdb numbers them, described by its target XML. */
typedef struct rl_gdb_cpu {
    /** @brief The target description (target.xml), naming the architecture and every register in gdb's order. */
    const char *target_xml;
    /** @brief Number of registers. */
    uint32_t nregs;
    /** @brief Bytes of register n. */
    uint32_t (*reg_size)(uint32_t n);
    /** @brief Writes register n of c to out, little-endian, reg_size(n) bytes. */
    void (*reg_read)(const rl_ctx *c, uint32_t n, uint8_t *out);
    /** @brief Sets register n of c from in, little-endian, reg_size(n) bytes. */
    void (*reg_write)(rl_ctx *c, uint32_t n, const uint8_t *in);
    /** @brief The guest pc of a stopped context, for stop replies and thread names. */
    uint64_t (*pc)(const rl_ctx *c);
    /** @brief The number of the pc register (8 bytes), which a breakpoint stop reports as the function's address. */
    uint32_t pc_reg;
    /**
     * @brief Fills out with the guest call frames of a stopped context whose pc is pc, innermost first, at most max,
     * reading guest memory with read; returns how many. NULL where the CPU keeps no frame chain.
     */
    uint32_t (*frames)(const rl_ctx *c, uint64_t pc, rl_gdb_frame *out, uint32_t max, rl_gdb_read_fn read);
} rl_gdb_cpu;

/** @brief One guest thread as the guest layer reports it. */
typedef struct rl_gdb_thread {
    /** @brief The guest's thread id, above 0. */
    uint64_t id;
    /** @brief Its context. */
    rl_ctx *ctx;
    /** @brief The guest core it holds, or -1. */
    int32_t core;
    /** @brief True while it may be running lifted code (it holds a core); false when it waits in the runtime. */
    bool running;
} rl_gdb_thread;

/** @brief A guest layer's side of the stub. */
typedef struct rl_gdb_guest {
    /** @brief Fills up to max threads into out and returns how many there are. */
    uint32_t (*threads)(rl_gdb_thread *out, uint32_t max);
    /** @brief The guest address space, for memory reads and writes. */
    struct rl_vm *vm;
} rl_gdb_guest;

#if defined(RL_DEBUG_STUB)
/** @brief Nonzero while the debugger wants every guest thread stopped; read at safe points. */
extern RL_RUNTIME_API int rl_gdb_stopping;
/**
 * @brief Starts the server when debug.gdb names a port: a host thread listens on 127.0.0.1:<port>. Returns true
 * when it listens. With debug.gdb_wait set, the call returns only once a debugger attached and continued.
 */
RL_RUNTIME_API bool rl_gdb_start(const rl_gdb_cpu *cpu, const rl_gdb_guest *guest);
/** @brief Parks the calling guest thread while the debugger holds the guest stopped. exact: c's registers are current. */
RL_RUNTIME_API void rl_gdb_park(rl_ctx *c, bool exact);
/** @brief A safe point: c's registers are exact when exact is true. Parks while the debugger holds the guest stopped. */
static inline void rl_gdb_point(rl_ctx *c, bool exact)
{
    if (__builtin_expect(__atomic_load_n(&rl_gdb_stopping, __ATOMIC_RELAXED), 0))
        rl_gdb_park(c, exact);
}
#else
static inline bool rl_gdb_start(const rl_gdb_cpu *cpu, const rl_gdb_guest *guest)
{
    (void)cpu;
    (void)guest;
    return false;
}
static inline void rl_gdb_point(rl_ctx *c, bool exact)
{
    (void)c;
    (void)exact;
}
#endif

#endif
