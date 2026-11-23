/**
 * @file vm.c
 * @brief Guest address space: reservation, the exact map at the guest page
 * size and the host protection policy of docs/abi.md section 2.
 *
 * Invariant: a guest page that is not mapped reads as zero. Unmapping
 * (RL_VM_DISCARD) releases whole host pages and zeroes the guest pages of
 * partly covered host pages, so mapping needs no work.
 */
#include "core.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(__ANDROID__) && __ANDROID_API__ < 30
#error "relift needs Android API level 30 or newer (memfd_create): build for API level 30 or later (for example with the NDK clang named <arch>-linux-android30-clang)"
#endif

#if defined(_WIN32)
#include <windows.h>

#define PROT_NONE 0
#define PROT_READ 1
#define PROT_WRITE 2
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif
#endif

#if defined(__APPLE__)
#include <mach/mach.h>
#endif

static void *os_reserve(uint64_t len)
{
#if defined(_WIN32)
    return VirtualAlloc(NULL, len, MEM_RESERVE, PAGE_NOACCESS);
#else
    void *p = mmap(NULL, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}

static void os_release(void *p, uint64_t len)
{
#if defined(_WIN32)
    (void)len;
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, len);
#endif
}

static int os_protect(void *p, uint64_t len, int prot)
{
#if defined(_WIN32)
    DWORD w = prot & PROT_WRITE ? PAGE_READWRITE : prot & PROT_READ ? PAGE_READONLY : PAGE_NOACCESS;
    DWORD old;
    if (VirtualProtect(p, len, w, &old))
        return 0;
    if (!VirtualAlloc(p, len, MEM_COMMIT, PAGE_NOACCESS))
        return -1;
    return VirtualProtect(p, len, w, &old) ? 0 : -1;
#else
    return mprotect(p, len, prot);
#endif
}

static int os_decommit(void *p, uint64_t len)
{
#if defined(_WIN32)
    return VirtualFree(p, len, MEM_DECOMMIT) ? 0 : -1;
#else
    return mmap(p, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE | MAP_FIXED, -1, 0) == MAP_FAILED ? -1 : 0;
#endif
}

static const char *os_error(void)
{
#if defined(_WIN32)
    static _Thread_local char b[32];
    snprintf(b, sizeof b, "error %lu", (unsigned long)GetLastError());
    return b;
#else
    return strerror(errno);
#endif
}

static uint64_t align_down(uint64_t v, uint64_t a) { return v & ~(a - 1); }
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static int prot_of(uint32_t perm)
{
    int p = PROT_NONE;
    if (perm & RL_VM_R)
        p |= PROT_READ;
    if (perm & RL_VM_W)
        p |= PROT_READ | PROT_WRITE;
    return p;
}

int rl_vm_init(rl_vm *vm, unsigned bits, uint64_t host_page)
{
    const rl_guest_desc *g = rl_core_guest();
    if (!g || bits > g->address_bits || bits >= 64) {
        memset(vm, 0, sizeof *vm);
        return -1;
    }
    return rl_vm_init_size(vm, 1ull << bits, host_page);
}

int rl_vm_init_size(rl_vm *vm, uint64_t size, uint64_t host_page)
{
    uint64_t real = rl_os_page_size();
    const rl_guest_desc *g = rl_core_guest();
    memset(vm, 0, sizeof *vm);
    if (!g || size == 0 || (g->address_bits < 64 && size > 1ull << g->address_bits))
        return -1;
    if (host_page == 0)
        host_page = rl_cfg()->host_page;
    if (host_page < real)
        host_page = real;
    if (host_page < g->page_size)
        host_page = g->page_size;
    if (host_page & (host_page - 1))
        return -1;
    vm->host_page = host_page;
    vm->guest_page = g->page_size;
    if (size & (host_page - 1))
        return -1;
    vm->size = size;
    vm->map_len = RL_VM_GUARD_LO + vm->size + RL_VM_GUARD_HI + host_page;
    void *p = os_reserve(vm->map_len);
    if (!p) {
        vm->map_len = 0;
        return -1;
    }
    vm->map_start = p;
    vm->base = (uint8_t *)(uintptr_t)align_up((uintptr_t)p + RL_VM_GUARD_LO, host_page);
    vm->cap = 64;
    vm->blocks = calloc(vm->cap, sizeof *vm->blocks);
    if (!vm->blocks) {
        os_release(p, vm->map_len);
        return -1;
    }
    vm->blocks[0] = (rl_vm_block){ 0, vm->size, 0, RL_VM_NONE, 0 };
    vm->count = 1;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&vm->lock, &a);
    pthread_mutexattr_destroy(&a);
    return 0;
}

void rl_vm_destroy(rl_vm *vm)
{
    if (vm->map_start) {
        os_release(vm->map_start, vm->map_len);
        pthread_mutex_destroy(&vm->lock);
    }
    free(vm->blocks);
    memset(vm, 0, sizeof *vm);
}

/** @brief Takes the space's lock; the map is const to readers, the lock is not. */
static void vm_lock(const rl_vm *vm) { pthread_mutex_lock((pthread_mutex_t *)&vm->lock); }

static void vm_unlock(const rl_vm *vm) { pthread_mutex_unlock((pthread_mutex_t *)&vm->lock); }

static uint32_t find(const rl_vm *vm, uint64_t addr)
{
    uint32_t lo = 0, hi = vm->count;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (vm->blocks[mid].addr <= addr)
            lo = mid;
        else
            hi = mid;
    }
    return lo;
}

static int reserve(rl_vm *vm, uint32_t extra)
{
    if (vm->count + extra <= vm->cap)
        return 0;
    uint32_t cap = vm->cap * 2 + extra;
    rl_vm_block *b = realloc(vm->blocks, cap * sizeof *b);
    if (!b)
        return -1;
    vm->blocks = b;
    vm->cap = cap;
    return 0;
}

static uint32_t split(rl_vm *vm, uint64_t at)
{
    if (at >= vm->size)
        return vm->count;
    uint32_t i = find(vm, at);
    rl_vm_block *b = &vm->blocks[i];
    if (b->addr == at)
        return i;
    memmove(&vm->blocks[i + 2], &vm->blocks[i + 1], (vm->count - i - 1) * sizeof *b);
    vm->blocks[i + 1] = *b;
    vm->blocks[i + 1].addr = at;
    vm->blocks[i + 1].size = b->addr + b->size - at;
    b->size = at - b->addr;
    vm->count++;
    return i + 1;
}

static bool same(const rl_vm_block *a, const rl_vm_block *b)
{
    return a->perm == b->perm && a->tag == b->tag;
}

static void merge_around(rl_vm *vm, uint32_t i)
{
    if (i + 1 < vm->count && same(&vm->blocks[i], &vm->blocks[i + 1])) {
        vm->blocks[i].size += vm->blocks[i + 1].size;
        memmove(&vm->blocks[i + 1], &vm->blocks[i + 2], (vm->count - i - 2) * sizeof(rl_vm_block));
        vm->count--;
    }
    if (i > 0 && same(&vm->blocks[i - 1], &vm->blocks[i])) {
        vm->blocks[i - 1].size += vm->blocks[i].size;
        memmove(&vm->blocks[i], &vm->blocks[i + 1], (vm->count - i - 1) * sizeof(rl_vm_block));
        vm->count--;
    }
}

static int union_perm(const rl_vm *vm, uint64_t hp_start, uint64_t hp_end)
{
    uint32_t perm = 0;
    if (hp_end > vm->size)
        hp_end = vm->size;
    for (uint32_t i = find(vm, hp_start); i < vm->count && vm->blocks[i].addr < hp_end; i++)
        perm |= vm->blocks[i].perm;
    return prot_of(perm);
}

bool rl_vm_host_writable(const rl_vm *vm, uint64_t addr) { return (rl_vm_host_prot(vm, addr) & PROT_WRITE) != 0; }

/** @brief rl_vm_host_prot with the lock held. */
static int page_prot(const rl_vm *vm, uint64_t addr)
{
    uint64_t s = align_down(addr, vm->host_page);
    return union_perm(vm, s, s + vm->host_page);
}

int rl_vm_host_prot(const rl_vm *vm, uint64_t addr)
{
    vm_lock(vm);
    int prot = page_prot(vm, addr);
    vm_unlock(vm);
    return prot;
}

static int host_protect(rl_vm *vm, uint64_t addr, uint64_t len, int prot)
{
    vm->protect_calls++;
    if (os_protect(vm->base + addr, len, prot) != 0) {
        rl_log(RL_LOG_ERROR, "vm", "protect(0x%llx, 0x%llx, %d) failed: %s", (unsigned long long)addr,
               (unsigned long long)len, prot, os_error());
        return -1;
    }
    return 0;
}

int rl_vm_host_access(rl_vm *vm, uint64_t addr, uint64_t size, bool access)
{
    if ((addr | size) & (vm->host_page - 1) || !rl_vm_contains(vm, addr, size))
        return -1;
    return os_protect(vm->base + addr, size, access ? PROT_READ | PROT_WRITE : PROT_NONE);
}

int rl_vm_host_watch(rl_vm *vm, uint64_t addr, uint64_t size, bool watch)
{
    if ((addr | size) & (vm->host_page - 1) || !rl_vm_contains(vm, addr, size))
        return -1;
    return os_protect(vm->base + addr, size, watch ? PROT_READ : PROT_READ | PROT_WRITE);
}

static int apply_range(rl_vm *vm, uint64_t addr, uint64_t size, uint32_t perm)
{
    uint64_t hp = vm->host_page;
    uint64_t s = align_down(addr, hp), e = align_up(addr + size, hp);
    uint64_t in_s = align_up(addr, hp), in_e = align_down(addr + size, hp);
    int rc = 0;
    if (in_s >= in_e) {
        for (uint64_t p = s; p < e; p += hp)
            if (host_protect(vm, p, hp, union_perm(vm, p, p + hp)))
                rc = -1;
        return rc;
    }
    if (s < in_s && host_protect(vm, s, hp, union_perm(vm, s, in_s)))
        rc = -1;
    if (host_protect(vm, in_s, in_e - in_s, prot_of(perm)))
        rc = -1;
    if (in_e < e && host_protect(vm, in_e, hp, union_perm(vm, in_e, e)))
        rc = -1;
    return rc;
}

static int discard(rl_vm *vm, uint64_t addr, uint64_t size)
{
    uint64_t hp = vm->host_page;
    uint64_t in_s = align_up(addr, hp), in_e = align_down(addr + size, hp);
    if (in_s < in_e) {
        if (os_decommit(vm->base + in_s, in_e - in_s) != 0) {
            rl_log(RL_LOG_ERROR, "vm", "release 0x%llx failed: %s", (unsigned long long)in_s, os_error());
            return -1;
        }
    }
    uint64_t edges[2][2] = { { addr, in_s < in_e ? in_s : addr + size }, { in_e > in_s ? in_e : addr + size, addr + size } };
    for (int k = 0; k < 2; k++) {
        uint64_t a = edges[k][0], b = edges[k][1];
        if (a >= b)
            continue;
        for (uint64_t p = align_down(a, hp); p < b; p += hp) {
            uint64_t zs = p > a ? p : a, ze = p + hp < b ? p + hp : b;
            int prot = page_prot(vm, p);
            if (!(prot & PROT_WRITE) && host_protect(vm, p, hp, PROT_READ | PROT_WRITE))
                return -1;
            memset(vm->base + zs, 0, ze - zs);
            if (!(prot & PROT_WRITE) && host_protect(vm, p, hp, prot))
                return -1;
        }
    }
    return 0;
}

/** @brief rl_vm_set with the lock held. */
static int set_locked(rl_vm *vm, uint64_t addr, uint64_t size, uint32_t perm, uint64_t tag, int content)
{
    if (content == RL_VM_DISCARD && discard(vm, addr, size))
        return -1;
    if (reserve(vm, 2))
        return -1;
    split(vm, addr);
    split(vm, addr + size);
    uint32_t i = find(vm, addr);
    uint32_t j = addr + size >= vm->size ? vm->count : find(vm, addr + size);
    vm->blocks[i] = (rl_vm_block){ addr, size, tag, perm, 0 };
    if (j > i + 1) {
        memmove(&vm->blocks[i + 1], &vm->blocks[j], (vm->count - j) * sizeof(rl_vm_block));
        vm->count -= j - i - 1;
    }
    merge_around(vm, i);
    return apply_range(vm, addr, size, perm);
}

int rl_vm_set(rl_vm *vm, uint64_t addr, uint64_t size, uint32_t perm, uint64_t tag, int content)
{
    __atomic_add_fetch(&vm->map_gen, 1, __ATOMIC_RELEASE);
    if ((addr | size) & (vm->guest_page - 1) || size == 0 || !rl_vm_contains(vm, addr, size))
        return -1;
    vm_lock(vm);
    int rc = set_locked(vm, addr, size, perm, tag, content);
    vm_unlock(vm);
    return rc;
}

rl_vm_block rl_vm_query(const rl_vm *vm, uint64_t addr)
{
    vm_lock(vm);
    rl_vm_block b = vm->blocks[find(vm, addr)];
    vm_unlock(vm);
    return b;
}

bool rl_vm_uniform(const rl_vm *vm, uint64_t addr, uint64_t size, rl_vm_block *out)
{
    if (!rl_vm_contains(vm, addr, size) || size == 0)
        return false;
    rl_vm_block b = rl_vm_query(vm, addr);
    if (out)
        *out = b;
    return addr + size <= b.addr + b.size;
}

static void unlock_range(rl_vm *vm, uint64_t addr, uint64_t size)
{
    uint64_t hp = vm->host_page;
    for (uint64_t p = align_down(addr, hp); p < addr + size; p += hp)
        if ((page_prot(vm, p) & (PROT_READ | PROT_WRITE)) != (PROT_READ | PROT_WRITE))
            host_protect(vm, p, hp, PROT_READ | PROT_WRITE);
}

static void relock_range(rl_vm *vm, uint64_t addr, uint64_t size)
{
    uint64_t hp = vm->host_page;
    for (uint64_t p = align_down(addr, hp); p < addr + size; p += hp) {
        int prot = page_prot(vm, p);
        if ((prot & (PROT_READ | PROT_WRITE)) != (PROT_READ | PROT_WRITE))
            host_protect(vm, p, hp, prot);
    }
}

void rl_vm_kernel_copy(rl_vm *vm, uint64_t dst, uint64_t src, uint64_t size)
{
    if (!size)
        return;
    vm_lock(vm);
    unlock_range(vm, src, size);
    unlock_range(vm, dst, size);
    memmove(vm->base + dst, vm->base + src, size);
    relock_range(vm, src, size);
    relock_range(vm, dst, size);
    vm_unlock(vm);
}

void rl_vm_kernel_write(rl_vm *vm, uint64_t dst, const void *src, uint64_t size)
{
    if (!size)
        return;
    vm_lock(vm);
    unlock_range(vm, dst, size);
    memcpy(vm->base + dst, src, size);
    relock_range(vm, dst, size);
    vm_unlock(vm);
}

void rl_vm_kernel_read(rl_vm *vm, void *dst, uint64_t src, uint64_t size)
{
    if (!size)
        return;
    vm_lock(vm);
    unlock_range(vm, src, size);
    memcpy(dst, vm->base + src, size);
    relock_range(vm, src, size);
    vm_unlock(vm);
}

#if defined(_WIN32)
int rl_shm_create(rl_shm *s, uint64_t size)
{
    uint64_t page = rl_os_page_size();
    memset(s, 0, sizeof *s);
    s->fd = -1;
    size = align_up(size ? size : 1, page < 0x4000 ? 0x4000 : page);
    HANDLE h = CreateFileMappingW(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE, (DWORD)(size >> 32), (DWORD)size, NULL);
    if (!h)
        return -1;
    void *p = MapViewOfFile(h, FILE_MAP_ALL_ACCESS, 0, 0, size);
    if (!p) {
        CloseHandle(h);
        return -1;
    }
    s->handle = h;
    s->host = p;
    s->size = size;
    return 0;
}

void rl_shm_destroy(rl_shm *s)
{
    if (s->host)
        UnmapViewOfFile(s->host);
    if (s->handle)
        CloseHandle((HANDLE)s->handle);
    memset(s, 0, sizeof *s);
    s->fd = -1;
}

int rl_vm_map_shm(rl_vm *vm, uint64_t addr, uint64_t size, const rl_shm *s)
{
    __atomic_add_fetch(&vm->map_gen, 1, __ATOMIC_RELEASE);
    (void)vm;
    (void)addr;
    (void)size;
    (void)s;
    return -1;
}
#elif defined(__APPLE__)
int rl_shm_create(rl_shm *s, uint64_t size)
{
    uint64_t page = rl_os_page_size();
    memset(s, 0, sizeof *s);
    s->fd = -1;
    size = align_up(size ? size : 1, page < 0x4000 ? 0x4000 : page);
    vm_address_t at = 0;
    if (vm_allocate(mach_task_self(), &at, (vm_size_t)size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS)
        return -1;
    memory_object_size_t got = size;
    mach_port_t entry = MACH_PORT_NULL;
    kern_return_t kr = mach_make_memory_entry_64(mach_task_self(), &got, (memory_object_offset_t)at,
                                                 VM_PROT_READ | VM_PROT_WRITE | MAP_MEM_VM_SHARE, &entry, MACH_PORT_NULL);
    if (kr != KERN_SUCCESS || got < size) {
        if (kr == KERN_SUCCESS)
            mach_port_deallocate(mach_task_self(), entry);
        vm_deallocate(mach_task_self(), at, (vm_size_t)size);
        return -1;
    }
    s->handle = (void *)(uintptr_t)entry;
    s->host = (uint8_t *)(uintptr_t)at;
    s->size = size;
    return 0;
}

void rl_shm_destroy(rl_shm *s)
{
    if (s->host)
        vm_deallocate(mach_task_self(), (vm_address_t)(uintptr_t)s->host, (vm_size_t)s->size);
    if (s->handle)
        mach_port_deallocate(mach_task_self(), (mach_port_t)(uintptr_t)s->handle);
    memset(s, 0, sizeof *s);
    s->fd = -1;
}

int rl_vm_map_shm(rl_vm *vm, uint64_t addr, uint64_t size, const rl_shm *s)
{
    __atomic_add_fetch(&vm->map_gen, 1, __ATOMIC_RELEASE);
    if (((addr | size) & (vm->host_page - 1)) || size > s->size || !rl_vm_contains(vm, addr, size))
        return -1;
    vm_address_t at = (vm_address_t)(uintptr_t)(vm->base + addr);
    kern_return_t kr = vm_map(mach_task_self(), &at, (vm_size_t)size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
                              (mach_port_t)(uintptr_t)s->handle, 0, FALSE, VM_PROT_NONE, VM_PROT_READ | VM_PROT_WRITE,
                              VM_INHERIT_NONE);
    return kr == KERN_SUCCESS ? 0 : -1;
}
#else
int rl_shm_create(rl_shm *s, uint64_t size)
{
    static unsigned serial;
    uint64_t page = rl_os_page_size();
    memset(s, 0, sizeof *s);
    s->fd = -1;
    size = align_up(size ? size : 1, page < 0x4000 ? 0x4000 : page);
#if defined(__linux__) || defined(__ANDROID__)
    s->fd = memfd_create("relift-shm", MFD_CLOEXEC);
#else
    for (int tries = 0; tries < 16 && s->fd < 0; tries++) {
        char name[64];
        snprintf(name, sizeof name, "/relift-%d-%u", (int)getpid(), __atomic_add_fetch(&serial, 1, __ATOMIC_RELAXED));
        s->fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (s->fd >= 0)
            shm_unlink(name);
    }
#endif
    (void)serial;
    if (s->fd < 0)
        return -1;
    if (ftruncate(s->fd, (off_t)size) != 0) {
        close(s->fd);
        s->fd = -1;
        return -1;
    }
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    if (p == MAP_FAILED) {
        close(s->fd);
        s->fd = -1;
        return -1;
    }
    s->host = p;
    s->size = size;
    return 0;
}

void rl_shm_destroy(rl_shm *s)
{
    if (s->host)
        munmap(s->host, s->size);
    if (s->fd >= 0)
        close(s->fd);
    memset(s, 0, sizeof *s);
    s->fd = -1;
}

int rl_vm_map_shm(rl_vm *vm, uint64_t addr, uint64_t size, const rl_shm *s)
{
    __atomic_add_fetch(&vm->map_gen, 1, __ATOMIC_RELEASE);
    if (((addr | size) & (vm->host_page - 1)) || size > s->size || !rl_vm_contains(vm, addr, size))
        return -1;
    void *p = mmap(vm->base + addr, size, PROT_NONE, MAP_SHARED | MAP_FIXED, s->fd, 0);
    return p == MAP_FAILED ? -1 : 0;
}
#endif
