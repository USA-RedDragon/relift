/**
 * @file gdbtrap.h
 * @brief What the GDB server needs from the host CPU: planting a trap over the first host instruction of a lifted
 * function, stepping one host instruction after it, and watching pages for writes. gdbtrap.c implements it for the
 * hosts that have it (Linux on x86-64 and 64-bit Arm); everywhere else rl_gdbtrap_install fails and the server
 * answers breakpoint and watchpoint requests as unsupported. gdbstub.c holds the tables; the host layer calls back
 * through rl_gdbtrap_hooks and never sees them.
 */
#ifndef RELIFT_GDBTRAP_H
#define RELIFT_GDBTRAP_H

#include "core.h"

#include <stdbool.h>
#include <stdint.h>

/** @brief What the host layer asks the server, from its trap handler and the watched-page fault hook. */
typedef struct rl_gdbtrap_hooks {
    /** @brief The breakpoint (slot + 1) whose trap is at host, or 0. */
    int (*break_at)(const uint8_t *host);
    /** @brief A thread hit breakpoint slot with context c: parks it; true when the breakpoint is still set. */
    bool (*break_hit)(int slot, rl_ctx *c);
    /** @brief The host instruction bytes breakpoint slot's trap replaced. */
    uint32_t (*break_orig)(int slot);
    /** @brief Plants breakpoint slot's trap again when it is still set. */
    void (*break_rearm)(int slot);
    /** @brief A write fault f: true when it is on a watched page, which is then opened (page) and tells if a watched range was hit. */
    bool (*watch_fault)(const rl_fault *f, uint64_t *page, bool *hit);
    /** @brief Watches page again after the store that faulted ran. */
    void (*watch_close)(uint64_t page);
    /** @brief The calling thread's store hit the watched guest address addr: parks it. */
    void (*watch_hit)(uint64_t addr);
} rl_gdbtrap_hooks;

/** @brief Installs the trap handler and the fault hook; false on a host without them. */
bool rl_gdbtrap_install(const rl_gdbtrap_hooks *hooks);

/** @brief Plants the trap over the host instruction at host and returns the bytes it replaced. */
uint32_t rl_gdbtrap_arm(uint8_t *host);

/** @brief Puts the bytes orig back at host. */
void rl_gdbtrap_disarm(uint8_t *host, uint32_t orig);

#endif
