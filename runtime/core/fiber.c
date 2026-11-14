/**
 * @file fiber.c
 * @brief Fiber stacks: a reservation with a guard below (PROT_NONE on POSIX;
 * on Windows uncommitted pages below a PAGE_GUARD page, so the host grows
 * the stack and raises STATUS_STACK_OVERFLOW like on a thread), the list of
 * live fibers and the per-thread set-up. The register switch is in the
 * host-arch files fiber_<arch>.S.
 */
#include "fiber.h"

#include <string.h>

#if defined(_WIN32)
#include <windows.h>
#else
#include <signal.h>
#include <sys/mman.h>
#include <unistd.h>
#endif

rl_fiber *rl_fiber_live[RL_FIBER_SLOTS];

/** @brief Bytes a Windows fiber commits up front; the rest is committed as the stack grows. */
#define WIN_COMMIT (64u << 10)
/** @brief Size of the alternate signal stack installed on a host thread without one. */
#define ALT_STACK (64u << 10)
/** @brief Stack kept for the stack overflow report on Windows (SetThreadStackGuarantee). */
#define WIN_GUARANTEE (32u << 10)

static size_t page_size(void)
{
#if defined(_WIN32)
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return si.dwPageSize;
#else
    return (size_t)sysconf(_SC_PAGESIZE);
#endif
}

static void list_add(rl_fiber *f)
{
    for (uint32_t i = 0; i < RL_FIBER_SLOTS; i++) {
        rl_fiber *none = NULL;
        if (__atomic_compare_exchange_n(&rl_fiber_live[i], &none, f, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED)) {
            f->slot = i + 1;
            return;
        }
    }
}

static void list_remove(rl_fiber *f)
{
    if (f->slot)
        __atomic_store_n(&rl_fiber_live[f->slot - 1], NULL, __ATOMIC_RELEASE);
    f->slot = 0;
}

int rl_fiber_init(rl_fiber *f, size_t stack, rl_fiber_entry *entry, void *arg)
{
    size_t pg = page_size();
    if (stack == 0)
        stack = RL_FIBER_STACK_DEFAULT;
    stack = (stack + pg - 1) & ~(pg - 1);
    size_t guard = (RL_FIBER_GUARD + pg - 1) & ~(pg - 1);
    size_t total = stack + guard;
    memset(f, 0, sizeof *f);
#if defined(_WIN32)
    uint8_t *p = VirtualAlloc(NULL, total, MEM_RESERVE, PAGE_READWRITE);
    if (!p)
        return -1;
    size_t commit = stack - pg < WIN_COMMIT ? stack - pg : WIN_COMMIT;
    uint8_t *lo = p + total - commit;
    if (stack < 2 * pg || !VirtualAlloc(lo, commit, MEM_COMMIT, PAGE_READWRITE) ||
        !VirtualAlloc(lo - pg, pg, MEM_COMMIT, PAGE_READWRITE | PAGE_GUARD)) {
        VirtualFree(p, 0, MEM_RELEASE);
        return -1;
    }
    f->limit = p + guard;
    const void *teb[3] = {p + total, lo, p};
#else
    uint8_t *p = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return -1;
    if (mprotect(p + guard, stack, PROT_READ | PROT_WRITE) != 0) {
        munmap(p, total);
        return -1;
    }
    f->limit = p + guard;
    const void *const *teb = NULL;
#endif
    f->alloc = p;
    f->alloc_len = total;
    f->top = p + total;
    f->sp = rl_fiber_prepare(f->top, entry, arg, teb);
    f->first_sp = f->sp;
    list_add(f);
    return 0;
}

void rl_fiber_free(rl_fiber *f)
{
    list_remove(f);
    if (!f->alloc)
        return;
#if defined(_WIN32)
    VirtualFree(f->alloc, 0, MEM_RELEASE);
#else
    munmap(f->alloc, f->alloc_len);
#endif
    f->alloc = NULL;
    f->sp = NULL;
}

const rl_fiber *rl_fiber_find(uintptr_t addr, bool *in_guard)
{
    for (uint32_t i = 0; i < RL_FIBER_SLOTS; i++) {
        const rl_fiber *f = __atomic_load_n(&rl_fiber_live[i], __ATOMIC_ACQUIRE);
        if (f && addr >= (uintptr_t)f->alloc && addr < (uintptr_t)f->top) {
            if (in_guard)
                *in_guard = addr < (uintptr_t)f->limit;
            return f;
        }
    }
    return NULL;
}

static _Thread_local bool t_ready;

void rl_fiber_thread_init(void)
{
    if (t_ready)
        return;
    t_ready = true;
#if defined(_WIN32)
    ULONG want = WIN_GUARANTEE;
    SetThreadStackGuarantee(&want);
#else
    stack_t cur;
    if (sigaltstack(NULL, &cur) == 0 && !(cur.ss_flags & SS_DISABLE) && cur.ss_sp)
        return;
    void *p = mmap(NULL, ALT_STACK, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (p == MAP_FAILED)
        return;
    stack_t ss;
    memset(&ss, 0, sizeof ss);
    ss.ss_sp = p;
    ss.ss_size = ALT_STACK;
    if (sigaltstack(&ss, NULL) != 0)
        munmap(p, ALT_STACK);
#endif
}
