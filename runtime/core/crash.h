/**
 * @file crash.h
 * @brief Crash reports: the function maps of the lifted code, the host
 * signal or exception handler, and a report with the faulting guest
 * address, the fiber (guest CPU) that faulted, a guest backtrace and the
 * host backtrace (docs/abi.md section 11).
 *
 * Depends on the C library, the host OS and the fibers only, so a program
 * can link the crash handler without the rest of the core (the crash test
 * does).
 */
#ifndef RELIFT_CRASH_H
#define RELIFT_CRASH_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Most host frames a report walks. */
#define RL_CRASH_MAX_FRAMES 64

/**
 * @brief Registers function maps for crash reports. Call before guest code
 * runs (not from a signal handler); maps must outlive the process. Returns 0,
 * or -1 for a map with another version, an unsorted map, or no memory.
 */
RL_RUNTIME_API int rl_fn_maps_register(const rl_fn_map *const *maps, uint32_t count);
/** @brief Forgets every registered map (tests). */
RL_RUNTIME_API void rl_fn_maps_reset(void);

/** @brief A function found in the maps. */
typedef struct rl_fn_hit {
    /** @brief The map holding the function. */
    const rl_fn_map *map;
    /** @brief The function. */
    const rl_fn_info *fn;
    /** @brief Host pc minus the host function start, or guest address minus guest_addr. */
    uint64_t off;
    /** @brief True when the pc lies in the map's lifted-code section, so the match is certain. */
    bool exact;
} rl_fn_hit;

/** @brief Finds the lifted function whose host code holds pc. Safe in a signal handler. */
RL_RUNTIME_API bool rl_fn_find_host(uintptr_t pc, rl_fn_hit *out);
/** @brief Finds the lifted function whose guest range holds a. Safe in a signal handler. */
RL_RUNTIME_API bool rl_fn_find_guest(uint64_t a, rl_fn_hit *out);

/**
 * @brief Tells reports where guest memory is: base is guest address 0, and
 * [lo, lo + len) is the host reservation including its guards.
 */
RL_RUNTIME_API void rl_crash_set_space(const uint8_t *base, uintptr_t lo, uint64_t len);

/** @brief Writes extra report lines (the CPU layer's register summary of the current thread) into buf. */
typedef void rl_crash_extra(char *buf, size_t n);
/** @brief Sets the extra-lines hook (NULL for none). */
RL_RUNTIME_API void rl_crash_set_extra(rl_crash_extra *fn);

/** @brief What a report is about. */
typedef struct rl_crash_info {
    /** @brief Kind for the first line, such as "SIGSEGV" or "EXCEPTION_ACCESS_VIOLATION". */
    const char *kind;
    /** @brief Host fault address, 0 when the kind has none. */
    uintptr_t addr;
    /** @brief True when addr is meaningful. */
    bool has_addr;
    /** @brief Host pc of the faulting instruction. */
    uintptr_t pc;
    /** @brief Host sp at the fault, which tells the fiber (guest CPU) that faulted; 0 when unknown. */
    uintptr_t sp;
    /** @brief True when the host reported a stack overflow (Windows); a fault in a fiber's guard also counts. */
    bool stack_overflow;
    /** @brief Host frames, innermost first: frames[0] is pc. */
    uintptr_t frames[RL_CRASH_MAX_FRAMES];
    /** @brief Number of frames. */
    int nframes;
} rl_crash_info;

/** @brief Formats and writes the report for ci to stderr. Safe in a signal handler. */
RL_RUNTIME_API void rl_crash_report(const rl_crash_info *ci);
/** @brief Formats the report into buf (NUL terminated); returns the length. */
RL_RUNTIME_API size_t rl_crash_format(const rl_crash_info *ci, char *buf, size_t n);

/**
 * @brief Installs the crash handler: SIGSEGV, SIGBUS and SIGILL on POSIX
 * hosts (the core's fault handler takes SIGSEGV and SIGBUS over and calls
 * rl_crash_signal when it cannot resolve a fault), a vectored exception
 * handler on Windows. Idempotent.
 */
RL_RUNTIME_API void rl_crash_install(void);

/**
 * @brief POSIX: reports signal sig with its siginfo and ucontext (as void
 * pointers). The caller then restores the default action so the signal ends
 * the process.
 */
RL_RUNTIME_API void rl_crash_signal(int sig, void *siginfo, void *ucontext);

/** @brief Writes "module+0xoff" or "symbol+0xoff [module]" for a host pc into buf; false when nothing is known. Host-OS part. */
bool rl_crash_os_symbolize(uintptr_t pc, char *buf, size_t n);
/** @brief Writes n bytes to stderr. Host-OS part. */
void rl_crash_os_write(const char *s, size_t n);

#endif
