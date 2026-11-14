/**
 * @file fiber.h
 * @brief Fibers: host stacks that one host thread switches between with a
 * hand-written register switch (docs/abi.md section 14). The device
 * scheduler runs each guest CPU on its own fiber.
 *
 * The switch saves only what the host ABI makes callee-saved: integer
 * registers, xmm6-xmm15 on Windows x86-64, d8-d15 on 64-bit Arm hosts, and on
 * Windows the three stack fields of the thread environment block. It saves
 * no floating-point control: every fiber of a thread runs with the host's
 * default mode, which rl_fiber_fp_mode_diff checks.
 *
 * Depends on the C library and the host OS only, like the crash handler,
 * so it also builds on Windows.
 */
#ifndef RELIFT_FIBER_H
#define RELIFT_FIBER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Default host stack of a fiber in bytes (reserved; pages are committed as the stack grows). */
#define RL_FIBER_STACK_DEFAULT (16u << 20)
/** @brief Guard below a fiber's stack in bytes, at least one host page. */
#define RL_FIBER_GUARD (64u << 10)
/** @brief Most fibers alive at once that crash reports and the debugger can name. */
#define RL_FIBER_SLOTS 64u

/** @brief Code a fiber starts in; it must never return (it switches away for good instead). */
typedef void rl_fiber_entry(void *arg);

/** @brief A fiber, or the host thread's own stack when alloc is NULL. Owned by the caller. */
typedef struct rl_fiber {
    /** @brief The saved stack pointer while the fiber is not running. */
    void *sp;
    /** @brief The stack pointer of the first switch, so a fiber that never ran can be told apart. */
    void *first_sp;
    /** @brief Start of the reservation, the guard included; NULL for a thread's own stack. */
    uint8_t *alloc;
    /** @brief Size of the reservation. */
    size_t alloc_len;
    /** @brief Lowest usable stack address, just above the guard. */
    uint8_t *limit;
    /** @brief End of the stack (exclusive). */
    uint8_t *top;
    /** @brief Slot in rl_fiber_live plus one, 0 when not listed. */
    uint32_t slot;
    /** @brief Name for crash reports and the debugger, such as "guest CPU 1". */
    char label[32];
} rl_fiber;

/** @brief The live fibers, read by crash reports and runtime/tools/gdb/relift_frames.py. */
extern rl_fiber *rl_fiber_live[RL_FIBER_SLOTS];

/**
 * @brief Reserves a stack of `stack` bytes (0 for RL_FIBER_STACK_DEFAULT,
 * rounded up to host pages) with a guard below it, and prepares the first
 * switch into entry(arg). Returns 0, or -1 when the host refuses the memory.
 */
int rl_fiber_init(rl_fiber *f, size_t stack, rl_fiber_entry *entry, void *arg);
/** @brief Releases the stack. f must not be running. */
void rl_fiber_free(rl_fiber *f);

/**
 * @brief Prepares the calling host thread for running fibers: on POSIX an
 * alternate signal stack when it has none, so a fault in a guard page is
 * reported; on Windows a stack guarantee for the stack overflow report.
 * Idempotent per thread.
 */
void rl_fiber_thread_init(void);

/** @brief Saves the running code's registers, stores its sp in *save and continues where load_sp was saved. Host-arch code. */
void rl_fiber_swap(void **save, void *load_sp);
/** @brief Writes the first-switch frame below top for entry(arg) and returns its sp. Host-arch code. */
void *rl_fiber_prepare(void *top, rl_fiber_entry *entry, void *arg, const void *const *teb);
/** @brief The host FP control bits that differ from the host default (flags masked out): 0 in the default mode. Host-arch code. */
uint64_t rl_fiber_fp_mode_diff(void);

/** @brief Leaves from (its registers saved in it) and runs to. */
static inline void rl_fiber_switch(rl_fiber *from, rl_fiber *to) { rl_fiber_swap(&from->sp, to->sp); }

/**
 * @brief The live fiber whose reservation (guard included) holds addr, or
 * NULL; *in_guard tells whether addr is in the guard. Safe in a signal or
 * exception handler.
 */
const rl_fiber *rl_fiber_find(uintptr_t addr, bool *in_guard);

#endif
