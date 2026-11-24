/**
 * @file compat.h
 * @brief Structure marshalling between the guest's layouts and the layer's:
 * the system call files hold times as 64-bit seconds and nanoseconds, a
 * stat as l_stat, a pointer as 64 bits, and call these to read and write the
 * guest's own form. On aarch64 every helper is the straight copy; on i386
 * they narrow to 32-bit `long` words, pack stat64 and the packed epoll
 * event.
 */
#ifndef RL_LINUX_COMPAT_H
#define RL_LINUX_COMPAT_H

#include "abi.h"

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Set for the system call being served when the guest passed the old
 * 32-bit time structures (i386 calls without the _time64 suffix). Always
 * false on aarch64. Cleared at each system call's start.
 */
extern _Thread_local bool lx_ts32;

/** @brief Set for the system call being served when the guest's struct rlimit has 32-bit words (i386 getrlimit and setrlimit). Always false on aarch64. */
extern _Thread_local bool lx_rlim32;

/** @brief Size in bytes of the guest's timespec for the current call. */
uint32_t lx_timespec_bytes(void);
/** @brief Reads a guest timespec. false when unreadable. */
bool lx_get_timespec(uint64_t addr, l_timespec *ts);
/** @brief Writes a guest timespec. false when unwritable. */
bool lx_put_timespec(uint64_t addr, int64_t sec, int64_t nsec);
/** @brief Writes a guest timeval (seconds and microseconds, `long` words in the guest). */
bool lx_put_timeval(uint64_t addr, int64_t sec, int64_t usec);
/** @brief Reads the two timespecs of utimensat into t as sec, nsec, sec, nsec. */
bool lx_get_timespec2(uint64_t addr, int64_t t[4]);
/** @brief Reads n guest `long` words at addr into out, sign-extended. */
bool lx_get_longs(uint64_t addr, int64_t *out, unsigned n);
/** @brief Writes n `long` words to addr, narrowed to the guest's long. */
bool lx_put_longs(uint64_t addr, const int64_t *v, unsigned n);
/** @brief Writes n unsigned `long` words, narrowing as lx_put_longs (all ones stay all ones). */
bool lx_put_ulongs(uint64_t addr, const uint64_t *v, unsigned n);
/** @brief Reads a guest pointer or `unsigned long`. */
bool lx_get_ptr(uint64_t addr, uint64_t *v);
/** @brief Writes a guest pointer or `unsigned long`. */
bool lx_put_ptr(uint64_t addr, uint64_t v);
/** @brief Writes st in the guest's struct stat form (stat64 on i386). */
bool lx_put_stat(uint64_t addr, const l_stat *st);
/** @brief Reads iovec entry i at addr: base and length. */
bool lx_get_iov(uint64_t addr, uint64_t i, uint64_t e[2]);
/** @brief Writes struct sysinfo in the guest's form. */
bool lx_put_sysinfo(uint64_t addr, uint64_t uptime, uint64_t ram, uint16_t procs);
/** @brief Bytes of the guest's struct epoll_event: 16 on aarch64, 12 (packed) on i386. */
uint32_t lx_epoll_event_bytes(void);
/** @brief Reads one epoll_event. */
bool lx_get_epoll_event(uint64_t addr, uint32_t *events, uint64_t *data);
/** @brief Writes event i of an epoll_wait result array. */
bool lx_put_epoll_event(uint64_t addr, uint64_t i, uint32_t events, uint64_t data);
/** @brief Reads the guest's struct sigaction (handler, flags, restorer, mask) from the kernel's rt_sigaction form. */
bool lx_get_sigaction(uint64_t addr, uint64_t k[4]);
/** @brief Writes it. */
bool lx_put_sigaction(uint64_t addr, const uint64_t k[4]);
/** @brief Reads stack_t as sp, flags, size. */
bool lx_get_stack_t(uint64_t addr, uint64_t *sp, int32_t *flags, uint64_t *size);
/** @brief Writes stack_t. */
bool lx_put_stack_t(uint64_t addr, uint64_t sp, int32_t flags, uint64_t size);
/** @brief Bytes of a siginfo's header up to the union: where si_pid sits is 12 on i386 and 16 on aarch64; this returns that offset. */
uint32_t lx_siginfo_union_offset(void);

#endif
