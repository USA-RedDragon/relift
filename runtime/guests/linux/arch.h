/**
 * @file arch.h
 * @brief What the Linux layer needs from one instruction set and its kernel
 * ABI. A build links exactly one implementation (arch_arm64.c or
 * arch_i386.c), so the common files never name a register or a layout:
 * system call entry and arguments, the initial stack's word size and
 * auxiliary vector, the signal frame, the registers a new thread starts
 * with, and the guest address space.
 *
 * i386 context interface for the CPU layer (x86-lift implements it; the
 * Linux layer reaches registers only through it):
 *
 * - struct rl_x86_regs below is the whole integer state a system call or a
 *   signal frame reads or writes. rl_x86_regs_get and rl_x86_regs_set copy it
 *   from and to a context.
 * - rl_x86_fx_get and rl_x86_fx_set copy the 512-byte FXSAVE image (x87,
 *   MMX, XMM0-7, MXCSR) a signal frame carries.
 * - A trap into the Linux layer is rl_int80(c, mem) for `int 0x80` and
 *   rl_sysenter(c, mem) for `sysenter`. On entry eip holds the address of
 *   the next instruction (what the kernel saves); on return the result is in
 *   eax and execution continues at eip, or, for sysenter, at the address
 *   the vDSO's return path holds (the vDSO page is guest code that is
 *   lifted like the program; see vdso_i386.h).
 * - rl_x86_cpu(), rl_x86_set_safepoint_hook and rl_x86_trampoline mirror the
 *   arm64 layer's.
 */
#ifndef RL_LINUX_ARCH_H
#define RL_LINUX_ARCH_H

#include "linux_priv.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The architecture's name: what AT_PLATFORM-less reports and logs call it. */
extern const char lx_arch_name[];
/** @brief Bytes in a pointer and in a stack word: 8 or 4. */
extern const unsigned lx_arch_word;
/** @brief The AT_PLATFORM string. */
extern const char lx_arch_platform[];
/** @brief Bytes a signal mask takes in a signal frame's ucontext and in rt_sigaction: always 8 here. */
#define LX_SIGMASK_BYTES 8

/** @brief Starts the runtime core for this architecture's CPU. 0 or -1. */
int lx_arch_core_init(void);
/** @brief Reserves the guest address space into lx.vm and tells the CPU layer its size. 0 or -1. */
int lx_arch_vm_init(rl_vm *vm);
/** @brief Installs the CPU layer's hooks (the signal safe point). */
void lx_arch_install_hooks(void);
/** @brief Whether the build returns through dispatch, which run-time-chosen return addresses (signal handlers, executable memory) need. */
bool lx_arch_trampoline(void);

/** @brief Reads the system call number and its six arguments from the context. */
void lx_arch_sys_args(const rl_ctx *c, uint32_t *nr, uint64_t a[6]);
/** @brief Stores a system call's result. */
void lx_arch_sys_result(rl_ctx *c, int64_t r);
/** @brief Prepares the context to run a system call again, as the kernel does when it restarts one. */
void lx_arch_sys_restart(rl_ctx *c, uint32_t nr, const uint64_t a[6]);
/** @brief Called when a system call starts (the arm64 exclusive monitor clears). */
void lx_arch_sys_begin(rl_ctx *c);
/**
 * @brief Runs the system call nr: maps the architecture's number and
 * argument conventions to lx_dispatch's and converts results. arm64 has none
 * and calls lx_dispatch directly.
 */
int64_t lx_arch_syscall(rl_ctx *c, uint8_t *mem, uint32_t nr, const uint64_t a[6]);
/** @brief The system call numbers whose first argument or path the trace prints (nr, 0 when none). */
bool lx_arch_traces_path(uint32_t nr);
/** @brief What a signal frame's saved pc adds to the pc of an interruption by a system call (the instruction size of the trap on arm64, 0 where the context already holds the next pc). */
uint64_t lx_arch_trap_bias(void);

/** @brief The guest stack pointer. */
uint64_t lx_arch_sp(const rl_ctx *c);
/** @brief Sets the guest stack pointer. */
void lx_arch_set_sp(rl_ctx *c, uint64_t sp);
/** @brief The address the interrupted code continues at (the pc a signal frame saves). */
uint64_t lx_arch_pc(const rl_ctx *c);
/** @brief Sets it. */
void lx_arch_set_pc(rl_ctx *c, uint64_t pc);
/** @brief Where a clone's child starts: the instruction after the clone trap. */
uint64_t lx_arch_after_trap(const rl_ctx *c);

/**
 * @brief Appends the architecture's auxiliary vector entries (hardware
 * capabilities, the vDSO, the minimum signal stack) to out, which has room
 * for 8; the count. The common code adds the rest.
 */
size_t lx_arch_aux_extra(const rl_linux_program *p, uint64_t (*out)[2]);
/** @brief Maps what the architecture adds to the address space (the i386 vDSO page); 0 or -1. */
int lx_arch_map_extra(void);
/** @brief The highest address plus one the stack and the mmap area may use: where the layout's top reserve begins. */
uint64_t lx_arch_space_top(const rl_vm *vm);

/** @brief Bytes a signal frame takes, for the room check. */
uint64_t lx_arch_frame_bytes(void);
/** @brief The frame's address for a handler entered with stack pointer sp (the architecture's alignment). */
uint64_t lx_arch_frame_place(uint64_t sp);
/**
 * @brief Writes the signal frame at guest address frame and sets the
 * registers to enter the handler: the arguments, the return address
 * (the restorer), and sp. resume is the pc the interrupted code continues at.
 * Returns 0, or -1 when the handler cannot be entered (no restorer where the
 * ABI needs one).
 */
int lx_arch_frame_build(rl_ctx *c, uint8_t *mem, uint64_t frame, int sig, const lx_pending_sig *p, const lx_sigaction *act, uint64_t resume);
/** @brief Reads the frame back after the handler: restores the registers, returns the pc the frame holds, and the blocked mask in *mask. */
uint64_t lx_arch_frame_restore(rl_ctx *c, uint8_t *mem, uint64_t frame, uint64_t *mask);
/** @brief Makes the context ready for the pending redirect to pc (a handler that changed where to continue). */
void lx_arch_redirect(rl_ctx *c, uint64_t resume_from);

/**
 * @brief Fills the new thread's context n for a clone made by c with the
 * flags and arguments a (the kernel's order for this architecture): the
 * registers, the result 0, the stack, the thread pointer. 0 or -errno.
 */
int lx_arch_clone_regs(rl_ctx *n, const rl_ctx *c, uint64_t flags, const uint64_t a[6]);
/** @brief The clone argument that holds the child tid address: index into a. */
unsigned lx_arch_clone_ctid_arg(void);
/** @brief Sets the initial stack pointer. */
void lx_arch_start_regs(rl_ctx *c, uint64_t sp, uint64_t entry);

/** @brief Enters guest code at entry on this context (the CPU layer's rl_call_guest): RL_OK, or RL_PENDING with the pending record set. */
int lx_arch_call_guest(rl_ctx *c, uint8_t *mem, uint64_t entry);

/** @brief Runs the system call layer for the trap entry: the loop of dispatch, restart and signal delivery. */
int lx_syscall_entry(rl_ctx *c, uint8_t *mem);
/** @brief The neutral dispatcher: nr is an enum rl_linux_nr value. */
int64_t lx_dispatch(rl_ctx *c, uint8_t *mem, uint32_t nr, const uint64_t a[6]);

#endif
