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

typedef struct chunk {
    uint64_t stamp[CHUNK_PAGES];
    uint8_t flags[CHUNK_PAGES];
} chunk;

static chunk **g_chunks;
static uint64_t g_nchunks;
static uint64_t g_stamp;
static uint64_t g_faults, g_starts;
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

void nvn_watch_stats(uint64_t *faults, uint64_t *starts)
{
    *faults = __atomic_load_n(&g_faults, __ATOMIC_RELAXED);
    *starts = __atomic_load_n(&g_starts, __ATOMIC_RELAXED);
}
