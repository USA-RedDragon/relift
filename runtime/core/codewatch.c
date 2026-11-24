/**
 * @file codewatch.c
 * @brief Code written at run time (docs/abi.md section 6.1): the code page
 * table, write detection on executable guest pages and invalidation.
 *
 * One entry per guest page in chunks of CHUNK_PAGES pages, allocated the first
 * time a page of the chunk is registered, so the fault hook only reads memory
 * that already exists and takes no lock. A page is FILE (bytes the build
 * lifted, pristine until written) or RUN (any other executable page). A
 * watched page has no host write access whatever the guest map says (vm.c asks
 * rl_cw_host_masked); the first write to it faults into the hook here, which
 * bumps the page's generation, clears pristine, empties the dispatch slots of a
 * FILE page and gives the host page its write access back, so the store retries.
 *
 * Host protection is only ever set from the policy under the space's lock
 * (rl_vm_host_reapply), never to a fixed value, so a watch that races the
 * hook's unwatch leaves the host page as the flags say after the last of them.
 * rl_vm_host_watch and rl_vm_host_access set fixed values and must not be used
 * on code pages.
 */
#include "core.h"

#include <stdlib.h>
#include <string.h>

#define CHUNK_PAGES 1024u

/** @brief Entry flag: the page holds code the build lifted. */
#define E_FILE 1u
/** @brief Entry flag: the page is executable code that was not lifted. */
#define E_RUN 2u
/** @brief Entry flag: a FILE page whose bytes are still the lifted ones. */
#define E_PRISTINE 4u
/** @brief Entry flag: the host page has no write access because of this page. */
#define E_WATCHED 8u

typedef struct cw_chunk {
    uint32_t gen[CHUNK_PAGES];
    uint8_t flags[CHUNK_PAGES];
    uint16_t faults[CHUNK_PAGES];
} cw_chunk;

static rl_vm *g_vm;
static uint8_t *g_base;
static cw_chunk **g_chunks;
static uint64_t g_nchunks;
static uint64_t g_page_shift;
static uint64_t g_watched;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static rl_cw_stats g_stats;

static unsigned log2u(uint64_t v)
{
    unsigned s = 0;
    while ((1ull << s) < v)
        s++;
    return s;
}

int rl_cw_init(rl_vm *vm)
{
    pthread_mutex_lock(&g_lock);
    int rc = 0;
    if (g_vm != vm || g_base != vm->base) {
        for (uint64_t i = 0; i < g_nchunks; i++)
            free(g_chunks[i]);
        free(g_chunks);
        g_page_shift = log2u(vm->guest_page);
        g_nchunks = (vm->size >> g_page_shift) / CHUNK_PAGES + 1;
        g_chunks = calloc(g_nchunks, sizeof *g_chunks);
        __atomic_store_n(&g_watched, 0, __ATOMIC_RELAXED);
        memset(&g_stats, 0, sizeof g_stats);
        if (!g_chunks) {
            g_nchunks = 0;
            g_vm = NULL;
            rc = -1;
        } else {
            g_base = vm->base;
            __atomic_store_n(&g_vm, vm, __ATOMIC_RELEASE);
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (rc == 0 && rl_fault_installed() && !rl_fault_add_hook(rl_cw_fault))
        rc = -1;
    return rc;
}

/** @brief The chunk of guest page p, allocating it when make is set (normal context, g_lock held). */
static cw_chunk *chunk_of(uint64_t p, bool make)
{
    uint64_t c = p / CHUNK_PAGES;
    if (c >= g_nchunks)
        return NULL;
    cw_chunk *ck = __atomic_load_n(&g_chunks[c], __ATOMIC_ACQUIRE);
    if (!ck && make) {
        ck = calloc(1, sizeof *ck);
        if (ck)
            __atomic_store_n(&g_chunks[c], ck, __ATOMIC_RELEASE);
    }
    return ck;
}

/** @brief Resets the host protection of the host pages over guest [addr, addr+size) to the policy (vm.c). */
static void reprotect(uint64_t addr, uint64_t size)
{
    rl_vm *vm = g_vm;
    uint64_t hp = vm->host_page;
    uint64_t s = addr & ~(hp - 1), e = (addr + size + hp - 1) & ~(hp - 1);
    rl_vm_host_reapply(vm, s, e - s);
}

/** @brief Ends the code of page p (lock-free): new generation, not pristine, dispatch slots of a FILE page emptied. */
static void invalidate_page(cw_chunk *ck, uint32_t i, uint64_t p)
{
    uint8_t fl = __atomic_fetch_and(&ck->flags[i], (uint8_t)~E_PRISTINE, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&ck->gen[i], 1, __ATOMIC_ACQ_REL);
    __atomic_add_fetch(&g_stats.invalidations, 1, __ATOMIC_RELAXED);
    if (fl & E_PRISTINE)
        rl_dispatch_drop(p << g_page_shift, 1ull << g_page_shift);
}

/** @brief Clears the watched flag of page p; true when it was set. */
static bool unwatch_page(cw_chunk *ck, uint32_t i)
{
    uint8_t fl = __atomic_fetch_and(&ck->flags[i], (uint8_t)~E_WATCHED, __ATOMIC_ACQ_REL);
    if (!(fl & E_WATCHED))
        return false;
    __atomic_sub_fetch(&g_watched, 1, __ATOMIC_ACQ_REL);
    return true;
}

static bool watch_page(cw_chunk *ck, uint32_t i)
{
    uint8_t fl = __atomic_fetch_or(&ck->flags[i], (uint8_t)E_WATCHED, __ATOMIC_ACQ_REL);
    if (fl & E_WATCHED)
        return false;
    __atomic_add_fetch(&g_watched, 1, __ATOMIC_ACQ_REL);
    return true;
}

int rl_cw_set(uint64_t addr, uint64_t size, int kind)
{
    rl_vm *vm = __atomic_load_n(&g_vm, __ATOMIC_ACQUIRE);
    if (!vm || size == 0 || ((addr | size) & (vm->guest_page - 1)) || !rl_vm_contains(vm, addr, size))
        return -1;
    if (kind == RL_CW_FILE && rl_dispatch_keep() != 0)
        return -1;
    pthread_mutex_lock(&g_lock);
    int rc = 0;
    bool changed = false;
    for (uint64_t p = addr >> g_page_shift; p < (addr + size) >> g_page_shift; p++) {
        cw_chunk *ck = chunk_of(p, kind != RL_CW_NONE);
        if (!ck) {
            if (kind != RL_CW_NONE)
                rc = -1;
            continue;
        }
        uint32_t i = (uint32_t)(p % CHUNK_PAGES);
        uint8_t old = __atomic_load_n(&ck->flags[i], __ATOMIC_ACQUIRE);
        if (old & (E_FILE | E_RUN))
            invalidate_page(ck, i, p);
        changed |= unwatch_page(ck, i);
        ck->faults[i] = 0;
        uint8_t fl = 0;
        if (kind == RL_CW_FILE) {
            fl = E_FILE | E_PRISTINE;
            rl_dispatch_restore(p << g_page_shift, 1ull << g_page_shift);
        } else if (kind == RL_CW_RUN) {
            fl = E_RUN;
        }
        __atomic_store_n(&ck->flags[i], fl, __ATOMIC_RELEASE);
        if (kind == RL_CW_FILE) {
            watch_page(ck, i);
            changed = true;
        }
    }
    pthread_mutex_unlock(&g_lock);
    if (changed)
        reprotect(addr, size);
    return rc;
}

/** @brief The entry of guest address a, or NULL when no page of its chunk was registered. */
static cw_chunk *entry(uint64_t a, uint32_t *i)
{
    rl_vm *vm = __atomic_load_n(&g_vm, __ATOMIC_ACQUIRE);
    if (!vm || a >= vm->size)
        return NULL;
    uint64_t p = a >> g_page_shift;
    *i = (uint32_t)(p % CHUNK_PAGES);
    return chunk_of(p, false);
}

rl_cw_page rl_cw_query(uint64_t addr)
{
    rl_cw_page r = { RL_CW_NONE, false, false, 0 };
    uint32_t i;
    cw_chunk *ck = entry(addr, &i);
    if (!ck)
        return r;
    uint8_t fl = __atomic_load_n(&ck->flags[i], __ATOMIC_ACQUIRE);
    r.kind = fl & E_FILE ? RL_CW_FILE : fl & E_RUN ? RL_CW_RUN : RL_CW_NONE;
    r.pristine = (fl & E_PRISTINE) != 0;
    r.watched = (fl & E_WATCHED) != 0;
    r.gen = __atomic_load_n(&ck->gen[i], __ATOMIC_ACQUIRE);
    return r;
}

int rl_cw_watch(uint64_t addr)
{
    uint32_t i;
    pthread_mutex_lock(&g_lock);
    cw_chunk *ck = entry(addr, &i);
    bool code = ck && (__atomic_load_n(&ck->flags[i], __ATOMIC_ACQUIRE) & (E_FILE | E_RUN));
    bool changed = code && ck->faults[i] < RL_CW_CHECKED_FAULTS && watch_page(ck, i);
    pthread_mutex_unlock(&g_lock);
    if (!code)
        return -1;
    if (changed)
        reprotect(addr & ~(g_vm->guest_page - 1), g_vm->guest_page);
    return 0;
}

bool rl_cw_checked(uint64_t addr)
{
    uint32_t i;
    cw_chunk *ck = entry(addr, &i);
    return ck && __atomic_load_n(&ck->faults[i], __ATOMIC_ACQUIRE) >= RL_CW_CHECKED_FAULTS;
}

/**
 * @brief Ends the code on the guest pages of [addr, addr+size) and clears their watch, without touching host
 * protection. Returns whether any page was watched. Lock-free: the fault hook calls it.
 */
static bool written(uint64_t addr, uint64_t size, bool count_fault)
{
    rl_vm *vm = __atomic_load_n(&g_vm, __ATOMIC_ACQUIRE);
    if (!vm || size == 0 || !rl_vm_contains(vm, addr, size))
        return false;
    bool was = false;
    for (uint64_t p = addr >> g_page_shift; p <= (addr + size - 1) >> g_page_shift; p++) {
        cw_chunk *ck = chunk_of(p, false);
        if (!ck) {
            p |= CHUNK_PAGES - 1;
            continue;
        }
        uint32_t i = (uint32_t)(p % CHUNK_PAGES);
        uint8_t fl = __atomic_load_n(&ck->flags[i], __ATOMIC_ACQUIRE);
        if (!(fl & (E_FILE | E_RUN)))
            continue;
        invalidate_page(ck, i, p);
        if (unwatch_page(ck, i)) {
            was = true;
            if (count_fault && ck->faults[i] < UINT16_MAX)
                __atomic_add_fetch(&ck->faults[i], 1, __ATOMIC_RELAXED);
        }
    }
    return was;
}

void rl_cw_will_write(uint64_t addr, uint64_t size)
{
    if (!size || !__atomic_load_n(&g_vm, __ATOMIC_ACQUIRE))
        return;
    if (written(addr, size, false))
        reprotect(addr, size);
}

bool rl_cw_written(uint64_t addr, uint64_t size) { return written(addr, size, false); }

bool rl_cw_fault(const rl_fault *f)
{
    rl_vm *vm = __atomic_load_n(&g_vm, __ATOMIC_ACQUIRE);
    if (!vm || !f->in_guest || f->guest_addr >= vm->size)
        return false;
    uint64_t hp = vm->host_page;
    uint64_t hs = f->guest_addr & ~(hp - 1);
    bool watched = false;
    for (uint64_t a = hs; a < hs + hp; a += vm->guest_page) {
        uint32_t i;
        cw_chunk *ck = entry(a, &i);
        if (ck && (__atomic_load_n(&ck->flags[i], __ATOMIC_ACQUIRE) & E_WATCHED))
            watched = true;
    }
    if (!watched || !rl_vm_guest_writes(vm, hs))
        return false;
    written(hs, hp, true);
    __atomic_add_fetch(&g_stats.faults, 1, __ATOMIC_RELAXED);
    return rl_vm_host_reapply(vm, hs, hp) == 0;
}

bool rl_cw_host_masked(const rl_vm *vm, uint64_t addr, uint64_t size)
{
    if (__atomic_load_n(&g_vm, __ATOMIC_ACQUIRE) != vm || !__atomic_load_n(&g_watched, __ATOMIC_ACQUIRE))
        return false;
    uint64_t end = addr + size;
    if (end > vm->size)
        end = vm->size;
    for (uint64_t a = addr & ~(vm->guest_page - 1); a < end; a += vm->guest_page) {
        uint32_t i;
        cw_chunk *ck = entry(a, &i);
        if (!ck) {
            a = ((a >> g_page_shift) | (CHUNK_PAGES - 1)) << g_page_shift;
            continue;
        }
        if (__atomic_load_n(&ck->flags[i], __ATOMIC_ACQUIRE) & E_WATCHED)
            return true;
    }
    return false;
}

bool rl_cw_any_watched(const rl_vm *vm)
{
    return __atomic_load_n(&g_vm, __ATOMIC_ACQUIRE) == vm && __atomic_load_n(&g_watched, __ATOMIC_ACQUIRE) != 0;
}

rl_cw_stats rl_cw_stats_get(void)
{
    rl_cw_stats s;
    s.faults = __atomic_load_n(&g_stats.faults, __ATOMIC_RELAXED);
    s.invalidations = __atomic_load_n(&g_stats.invalidations, __ATOMIC_RELAXED);
    return s;
}
