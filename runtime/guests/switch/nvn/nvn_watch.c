/**
 * @file nvn_watch.c
 * @brief Page write tracking for texture storage (nvn_watch.h).
 *
 * Per host page the layer keeps a write stamp (a global counter's value at the last write seen) and a flag byte, in
 * chunks allocated when a range is first watched, so the fault hook only reads and writes memory that already exists
 * and takes no lock. A page's flag says it is read-only because of a watch; a fault on a flagged page, or on a page
 * of a chunk that was once watched and the guest map still makes writable, is a write to watched memory.
 */
#include "nvn_watch.h"

#include "hos.h"

#include <stdlib.h>
#include <string.h>

#define CHUNK_PAGES 8192u
#define GROUP_PAGES 16u
#define F_EVER 1u
#define F_ON 2u
#define F_MIRROR 4u
#define F_DIRTY 8u
#define F_FRESH 16u

typedef struct chunk {
    uint64_t stamp[CHUNK_PAGES];
    uint8_t flags[CHUNK_PAGES];
    /** @brief Set by the fault hook when a mirrored page of the chunk was dirtied; the collector clears it before it scans. */
    uint32_t dirty;
} chunk;

static chunk **g_chunks;
static uint64_t g_nchunks;
static uint64_t g_stamp;
static uint64_t g_faults, g_starts, g_epoch;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static bool g_ready, g_failed;

/**
 * @brief A write to a watched page opens the whole aligned group of GROUP_PAGES pages around it: every page of the
 * group that was watched is stamped as written and made writable again, so a writer that fills a buffer front to back
 * faults once per group and not once per page.
 */
static bool fault_hook(const rl_fault *f)
{
    rl_vm *vm = &hos.vm;
    uint64_t hp = vm->host_page, page = f->guest_addr / hp;
    uint64_t c = page / CHUNK_PAGES;
    if (c >= g_nchunks)
        return false;
    chunk *ck = __atomic_load_n(&g_chunks[c], __ATOMIC_ACQUIRE);
    if (!ck)
        return false;
    uint32_t i = (uint32_t)(page % CHUNK_PAGES);
    uint8_t fl = __atomic_load_n(&ck->flags[i], __ATOMIC_ACQUIRE);
    if (!(fl & F_EVER) || !rl_vm_host_writable(vm, page * hp))
        return false;
    uint32_t lo = i & ~(GROUP_PAGES - 1), hi = lo + GROUP_PAGES;
    uint64_t stamp = __atomic_add_fetch(&g_stamp, 1, __ATOMIC_ACQ_REL);
    bool open[GROUP_PAGES];
    for (uint32_t k = lo; k < hi; k++) {
        open[k - lo] = k == i;
        if (!(__atomic_load_n(&ck->flags[k], __ATOMIC_ACQUIRE) & F_ON))
            continue;
        __atomic_store_n(&ck->stamp[k], stamp, __ATOMIC_RELEASE);
        if (__atomic_load_n(&ck->flags[k], __ATOMIC_ACQUIRE) & F_MIRROR) {
            __atomic_or_fetch(&ck->flags[k], F_DIRTY, __ATOMIC_RELEASE);
            __atomic_store_n(&ck->dirty, 1, __ATOMIC_RELEASE);
            __atomic_add_fetch(&g_epoch, 1, __ATOMIC_RELEASE);
        }
        __atomic_and_fetch(&ck->flags[k], (uint8_t)~F_ON, __ATOMIC_RELEASE);
        open[k - lo] = true;
    }
    __atomic_add_fetch(&g_faults, 1, __ATOMIC_RELAXED);
    bool ok = true;
    for (uint32_t k = lo; k < hi;) {
        if (!open[k - lo]) {
            k++;
            continue;
        }
        uint32_t e = k;
        while (e < hi && open[e - lo])
            e++;
        ok = rl_vm_host_access(vm, ((uint64_t)c * CHUNK_PAGES + k) * hp, (uint64_t)(e - k) * hp, true) == 0 && ok;
        k = e;
    }
    return ok;
}

static bool ready(void)
{
    pthread_mutex_lock(&g_lock);
    if (!g_ready && !g_failed) {
        rl_vm *vm = &hos.vm;
        uint64_t pages = vm->size / vm->host_page;
        g_nchunks = (pages + CHUNK_PAGES - 1) / CHUNK_PAGES;
        g_chunks = calloc(g_nchunks, sizeof *g_chunks);
        if (!rl_fault_installed() || !g_chunks || !rl_fault_add_hook(fault_hook))
            g_failed = true;
        else
            g_ready = true;
    }
    bool ok = g_ready;
    pthread_mutex_unlock(&g_lock);
    return ok;
}

static chunk *chunk_for(uint64_t c)
{
    chunk *ck = __atomic_load_n(&g_chunks[c], __ATOMIC_ACQUIRE);
    if (ck)
        return ck;
    pthread_mutex_lock(&g_lock);
    ck = g_chunks[c];
    if (!ck && (ck = calloc(1, sizeof *ck)))
        __atomic_store_n(&g_chunks[c], ck, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&g_lock);
    return ck;
}

bool nvn_watch_start(nvn_watch *w, uint64_t addr, uint64_t size)
{
    w->on = false;
    if (!ready())
        return false;
    rl_vm *vm = &hos.vm;
    uint64_t hp = vm->host_page;
    uint64_t first = (addr + hp - 1) / hp, end = (addr + size) / hp;
    if (end <= first || !rl_vm_contains(vm, first * hp, (end - first) * hp))
        return false;
    for (uint64_t p = first; p < end; p++)
        if (!rl_vm_host_writable(vm, p * hp))
            return false;
    uint64_t map_gen = __atomic_load_n(&vm->map_gen, __ATOMIC_ACQUIRE);
    uint64_t stamp = __atomic_load_n(&g_stamp, __ATOMIC_ACQUIRE);
    for (uint64_t p = first; p < end; p++) {
        chunk *ck = chunk_for(p / CHUNK_PAGES);
        if (!ck)
            return false;
        __atomic_or_fetch(&ck->flags[p % CHUNK_PAGES], F_EVER | F_ON, __ATOMIC_RELEASE);
    }
    if (rl_vm_host_watch(vm, first * hp, (end - first) * hp, true) != 0)
        return false;
    w->addr = first * hp;
    w->size = (end - first) * hp;
    w->stamp = stamp;
    w->map_gen = map_gen;
    w->on = true;
    __atomic_add_fetch(&g_starts, 1, __ATOMIC_RELAXED);
    return true;
}

void nvn_watch_release(uint64_t addr, uint64_t size)
{
    if (!g_ready || !size)
        return;
    rl_vm *vm = &hos.vm;
    uint64_t hp = vm->host_page, first = addr / hp, end = (addr + size + hp - 1) / hp;
    uint64_t stamp = __atomic_add_fetch(&g_stamp, 1, __ATOMIC_ACQ_REL);
    for (uint64_t p = first; p < end && p / CHUNK_PAGES < g_nchunks; p++) {
        chunk *ck = __atomic_load_n(&g_chunks[p / CHUNK_PAGES], __ATOMIC_ACQUIRE);
        uint32_t i = (uint32_t)(p % CHUNK_PAGES);
        if (!ck || !(__atomic_load_n(&ck->flags[i], __ATOMIC_ACQUIRE) & F_ON))
            continue;
        __atomic_store_n(&ck->stamp[i], stamp, __ATOMIC_RELEASE);
        __atomic_and_fetch(&ck->flags[i], (uint8_t)~F_ON, __ATOMIC_RELEASE);
        rl_vm_host_access(vm, p * hp, hp, true);
    }
}

bool nvn_watch_changed(const nvn_watch *w)
{
    if (!w->on)
        return true;
    rl_vm *vm = &hos.vm;
    if (__atomic_load_n(&vm->map_gen, __ATOMIC_ACQUIRE) != w->map_gen)
        return true;
    if (__atomic_load_n(&g_stamp, __ATOMIC_ACQUIRE) == w->stamp)
        return false;
    uint64_t hp = vm->host_page, first = w->addr / hp, end = (w->addr + w->size) / hp;
    for (uint64_t p = first; p < end; p++) {
        chunk *ck = __atomic_load_n(&g_chunks[p / CHUNK_PAGES], __ATOMIC_ACQUIRE);
        if (!ck || __atomic_load_n(&ck->stamp[p % CHUNK_PAGES], __ATOMIC_ACQUIRE) > w->stamp)
            return true;
    }
    return false;
}

void nvn_watch_touch(uint64_t addr, uint64_t size)
{
    if (!ready() || !size)
        return;
    rl_vm *vm = &hos.vm;
    uint64_t hp = vm->host_page, first = addr / hp, end = (addr + size + hp - 1) / hp;
    if (end > g_nchunks * CHUNK_PAGES)
        end = g_nchunks * CHUNK_PAGES;
    uint64_t stamp = __atomic_add_fetch(&g_stamp, 1, __ATOMIC_ACQ_REL);
    for (uint64_t p = first; p < end; p++) {
        chunk *ck = __atomic_load_n(&g_chunks[p / CHUNK_PAGES], __ATOMIC_ACQUIRE);
        if (ck && __atomic_load_n(&ck->flags[p % CHUNK_PAGES], __ATOMIC_ACQUIRE) & F_EVER)
            __atomic_store_n(&ck->stamp[p % CHUNK_PAGES], stamp, __ATOMIC_RELEASE);
    }
}

void nvn_watch_stats(uint64_t *faults, uint64_t *starts)
{
    *faults = __atomic_load_n(&g_faults, __ATOMIC_RELAXED);
    *starts = __atomic_load_n(&g_starts, __ATOMIC_RELAXED);
}

/** @brief The pages [first, end) a track call over [addr, addr + size) touches, rounded outward to whole host pages. */
static void track_pages(uint64_t addr, uint64_t size, uint64_t *first, uint64_t *end)
{
    uint64_t hp = hos.vm.host_page;
    *first = addr / hp;
    *end = (addr + size + hp - 1) / hp;
}

bool nvn_track_add(uint64_t addr, uint64_t size)
{
    if (!size || !ready())
        return false;
    uint64_t first, end;
    track_pages(addr, size, &first, &end);
    if (end * hos.vm.host_page > hos.vm.size)
        return false;
    for (uint64_t p = first; p < end; p++) {
        chunk *ck = chunk_for(p / CHUNK_PAGES);
        if (!ck)
            return false;
        __atomic_or_fetch(&ck->flags[p % CHUNK_PAGES], F_EVER | F_MIRROR | F_DIRTY | F_FRESH, __ATOMIC_RELEASE);
        __atomic_store_n(&ck->dirty, 1, __ATOMIC_RELEASE);
    }
    __atomic_add_fetch(&g_epoch, 1, __ATOMIC_RELEASE);
    return true;
}

void nvn_track_remove(uint64_t addr, uint64_t size)
{
    if (!g_ready)
        return;
    uint64_t first, end;
    track_pages(addr, size, &first, &end);
    for (uint64_t p = first; p < end && p / CHUNK_PAGES < g_nchunks; p++) {
        chunk *ck = __atomic_load_n(&g_chunks[p / CHUNK_PAGES], __ATOMIC_ACQUIRE);
        if (ck)
            __atomic_and_fetch(&ck->flags[p % CHUNK_PAGES], (uint8_t)~(F_MIRROR | F_DIRTY | F_FRESH), __ATOMIC_RELEASE);
    }
}

void nvn_track_dirty_all(uint64_t addr, uint64_t size)
{
    if (!g_ready)
        return;
    uint64_t first, end;
    track_pages(addr, size, &first, &end);
    for (uint64_t p = first; p < end && p / CHUNK_PAGES < g_nchunks; p++) {
        chunk *ck = __atomic_load_n(&g_chunks[p / CHUNK_PAGES], __ATOMIC_ACQUIRE);
        if (ck && (ck->flags[p % CHUNK_PAGES] & F_MIRROR)) {
            __atomic_or_fetch(&ck->flags[p % CHUNK_PAGES], F_DIRTY, __ATOMIC_RELEASE);
            __atomic_store_n(&ck->dirty, 1, __ATOMIC_RELEASE);
        }
    }
    __atomic_add_fetch(&g_epoch, 1, __ATOMIC_RELEASE);
}

uint64_t nvn_track_epoch(void) { return __atomic_load_n(&g_epoch, __ATOMIC_ACQUIRE); }

/**
 * @brief Arms the pages [first, last) of chunk ck (global page numbers from base) that the policy lets the guest write,
 * so the next write to them faults and dirties them again. Runs of pages the guest cannot write are left unarmed: only a
 * map change alters them.
 */
static void arm(chunk *ck, uint64_t base, uint32_t first, uint32_t last)
{
    rl_vm *vm = &hos.vm;
    uint64_t hp = vm->host_page;
    for (uint32_t k = first; k < last;) {
        if (!rl_vm_host_writable(vm, (base + k) * hp)) {
            k++;
            continue;
        }
        uint32_t e = k;
        while (e < last && rl_vm_host_writable(vm, (base + e) * hp))
            e++;
        for (uint32_t j = k; j < e; j++)
            __atomic_or_fetch(&ck->flags[j], F_EVER | F_ON, __ATOMIC_RELEASE);
        rl_vm_host_watch(vm, (base + k) * hp, (uint64_t)(e - k) * hp, true);
        k = e;
    }
}

void nvn_track_collect(void (*run)(void *user, uint64_t addr, uint64_t size, bool fresh), void *user)
{
    if (!g_ready)
        return;
    uint64_t hp = hos.vm.host_page;
    for (uint64_t c = 0; c < g_nchunks; c++) {
        chunk *ck = __atomic_load_n(&g_chunks[c], __ATOMIC_ACQUIRE);
        if (!ck || !__atomic_exchange_n(&ck->dirty, 0, __ATOMIC_ACQ_REL))
            continue;
        for (uint32_t i = 0; i < CHUNK_PAGES;) {
            uint8_t fl = __atomic_load_n(&ck->flags[i], __ATOMIC_ACQUIRE);
            if (!(fl & F_MIRROR) || !(fl & F_DIRTY)) {
                i++;
                continue;
            }
            bool fresh = fl & F_FRESH;
            uint32_t e = i + 1;
            while (e < CHUNK_PAGES) {
                uint8_t f2 = __atomic_load_n(&ck->flags[e], __ATOMIC_ACQUIRE);
                if (!(f2 & F_MIRROR) || !(f2 & F_DIRTY) || !!(f2 & F_FRESH) != fresh)
                    break;
                e++;
            }
            for (uint32_t j = i; j < e; j++)
                __atomic_and_fetch(&ck->flags[j], (uint8_t)~(F_DIRTY | F_FRESH), __ATOMIC_RELEASE);
            arm(ck, c * CHUNK_PAGES, i, e);
            run(user, (c * CHUNK_PAGES + i) * hp, (uint64_t)(e - i) * hp, fresh);
            i = e;
        }
    }
}
