/**
 * @file mem.c
 * @brief Guest memory: access checks and copies for system call arguments,
 * and brk, mmap, munmap, mprotect, mremap and madvise over the core's guest
 * address map (rl_vm). A page is mapped when its block's tag is 1; the
 * unmapped space has tag 0 and no permissions.
 */
#include "arch.h"
#include "linux_priv.h"

#include <errno.h>
#include <string.h>

enum { TAG_FREE = 0, TAG_MAPPED = 1 };

static bool in_space(uint64_t a, uint64_t n) { return rl_vm_contains(&lx.vm, a, n); }

bool lx_ok(uint64_t a, uint64_t n, uint32_t perm)
{
    if (n == 0)
        return true;
    if (!in_space(a, n))
        return false;
    uint64_t end = a + n;
    while (a < end) {
        rl_vm_block b = rl_vm_query(&lx.vm, a);
        if ((b.perm & perm) != perm)
            return false;
        a = b.addr + b.size;
    }
    return true;
}

bool lx_get(void *dst, uint64_t a, uint64_t n)
{
    if (!lx_ok(a, n, RL_VM_R))
        return false;
    if (n)
        memcpy(dst, rl_mem_base() + a, n);
    return true;
}

bool lx_put(uint64_t a, const void *src, uint64_t n)
{
    if (!lx_ok(a, n, RL_VM_W))
        return false;
    if (n)
        memcpy(rl_mem_base() + a, src, n);
    return true;
}

int64_t lx_cstr(uint64_t a, char *dst, size_t cap)
{
    for (size_t i = 0; i < cap; i++) {
        if (!lx_ok(a + i, 1, RL_VM_R))
            return -L_EFAULT;
        char ch = (char)rl_mem_base()[a + i];
        dst[i] = ch;
        if (ch == 0)
            return (int64_t)i;
    }
    return -L_ENAMETOOLONG;
}

static uint32_t vm_perm(uint32_t prot)
{
    uint32_t p = 0;
    if (prot & L_PROT_READ)
        p |= RL_VM_R;
    if (prot & L_PROT_WRITE)
        p |= RL_VM_R | RL_VM_W;
    if (prot & L_PROT_EXEC)
        p |= RL_VM_R | RL_VM_X;
    return p;
}

/**
 * @brief A request for executable memory in a build with direct returns:
 * the build only proves direct returns safe for code that is in the file,
 * so a program that makes executable memory stops with a report.
 */
static int64_t exec_refused(rl_ctx *c, uint32_t prot)
{
    if (!(prot & L_PROT_EXEC) || lx_arch_trampoline())
        return 0;
    rl_guest_fatal(c, "linux",
                   "the program maps executable memory (protection 0x%x), which a build with direct returns does not take: "
                   "rebuild with emit.trampoline = true",
                   prot);
    return LX_PENDING;
}

int lx_map(uint64_t addr, uint64_t size, uint32_t prot)
{
    return rl_vm_set(&lx.vm, addr, size, vm_perm(prot), TAG_MAPPED, RL_VM_FRESH);
}

bool lx_mapped(uint64_t addr, uint64_t size)
{
    if (!in_space(addr, size))
        return false;
    uint64_t end = addr + size;
    while (addr < end) {
        rl_vm_block b = rl_vm_query(&lx.vm, addr);
        if (b.tag != TAG_MAPPED)
            return false;
        addr = b.addr + b.size;
    }
    return true;
}

static bool range_free(uint64_t addr, uint64_t size)
{
    if (!in_space(addr, size))
        return false;
    uint64_t end = addr + size;
    while (addr < end) {
        rl_vm_block b = rl_vm_query(&lx.vm, addr);
        if (b.tag != TAG_FREE)
            return false;
        addr = b.addr + b.size;
    }
    return true;
}

uint64_t lx_find_free(uint64_t size)
{
    uint64_t top = lx.mmap_top;
    while (top > lx.mmap_floor && top - lx.mmap_floor >= size) {
        rl_vm_block b = rl_vm_query(&lx.vm, top - 1);
        uint64_t lo = b.addr > lx.mmap_floor ? b.addr : lx.mmap_floor;
        if (b.tag == TAG_FREE && top - lo >= size)
            return top - size;
        top = b.addr;
    }
    return 0;
}

int64_t lx_sys_brk(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint64_t want = a[0];
    if (want < lx.brk_start || want > lx.brk_limit)
        return (int64_t)lx.brk;
    uint64_t need = lx_page_up(want);
    if (need > lx.brk_mapped) {
        if (lx_map(lx.brk_mapped, need - lx.brk_mapped, L_PROT_READ | L_PROT_WRITE) != 0)
            return (int64_t)lx.brk;
    } else if (need < lx.brk_mapped) {
        rl_vm_set(&lx.vm, need, lx.brk_mapped - need, RL_VM_NONE, TAG_FREE, RL_VM_DISCARD);
    }
    if (want < lx.brk) {
        uint64_t keep = lx_page_up(want);
        if (keep > want && keep <= lx.brk_mapped)
            memset(rl_mem_base() + want, 0, keep - want);
    }
    lx.brk_mapped = need;
    lx.brk = want;
    return (int64_t)want;
}

/**
 * @brief Whether an anonymous reservation of size bytes is the one the
 * interpreter makes before it maps a library whose segments ask for more than
 * page alignment: room for the library and its alignment, to align inside,
 * after which it maps the file at the aligned address inside it.
 */
static bool lib_reservation(const rl_linux_module *m, uint64_t size)
{
    if (m->role != RL_LINUX_LIBRARY || size < m->span || !range_free(m->base, size))
        return false;
    return size <= m->span + m->align || size <= 2 * m->align;
}

/** @brief Whether a fixed mapping may take [addr, addr + size): the mmap area, or the range the program's libraries load into. */
static bool fixed_ok(uint64_t addr, uint64_t size)
{
    if (!in_space(addr, size) || addr + size > lx.stack_lo)
        return false;
    return addr >= lx.mmap_floor || (lx.lib_hi && addr >= lx.lib_lo && addr + size <= lx.lib_hi);
}

int64_t lx_sys_mmap(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint64_t addr = a[0], len = a[1], flags = a[3], off = a[5];
    uint32_t prot = (uint32_t)a[2];
    int fd = (int)a[4];
    uint32_t type = (uint32_t)flags & L_MAP_TYPE;
    if (len == 0 || (off & (L_PAGE - 1)) != 0)
        return -L_EINVAL;
    if (type != L_MAP_SHARED && type != L_MAP_PRIVATE && type != L_MAP_SHARED_VALIDATE)
        return -L_EINVAL;
    if (prot & ~(uint32_t)(L_PROT_READ | L_PROT_WRITE | L_PROT_EXEC))
        return -L_EINVAL;
    if (len > lx.vm.size)
        return -L_ENOMEM;
    if (exec_refused(c, prot))
        return LX_PENDING;
    uint64_t size = lx_page_up(len);
    bool anon = (flags & L_MAP_ANONYMOUS) != 0;
    int host = -1;
    const rl_linux_module *vm = anon ? NULL : lx_virt_fd(fd);
    if (!anon) {
        host = lx_host_fd(fd);
        if (host < 0 && !vm)
            return -L_EBADF;
        if (type != L_MAP_PRIVATE && (prot & L_PROT_WRITE))
            return -L_ENODEV;
    }
    uint64_t at;
    if (flags & (L_MAP_FIXED | L_MAP_FIXED_NOREPLACE)) {
        if ((addr & (L_PAGE - 1)) != 0 || !fixed_ok(addr, size))
            return -L_EINVAL;
        if ((flags & L_MAP_FIXED_NOREPLACE) && !(flags & L_MAP_FIXED) && !range_free(addr, size))
            return -L_EEXIST;
        at = addr;
    } else {
        at = 0;
        if (addr && (addr & (L_PAGE - 1)) == 0 && addr >= lx.mmap_floor && addr + size <= lx.mmap_top && range_free(addr, size))
            at = addr;
        if (!at && vm && vm->role == RL_LINUX_LIBRARY && off == 0) {
            if (size > vm->span || !range_free(vm->base, size))
                return -L_ENOMEM;
            at = vm->base;
        }
        if (!at && anon && lx.pending_lib && prot == L_PROT_NONE && lib_reservation(lx.pending_lib, size))
            at = lx.pending_lib->base;
        if (!at)
            at = lx_find_free(size);
        if (!at)
            return -L_ENOMEM;
    }
    if ((flags & L_MAP_FIXED) && !range_free(at, size) && rl_vm_set(&lx.vm, at, size, RL_VM_NONE, TAG_FREE, RL_VM_DISCARD) != 0)
        return -L_ENOMEM;
    if (lx_map(at, size, anon ? prot : (L_PROT_READ | L_PROT_WRITE)) != 0)
        return -L_ENOMEM;
    if (vm) {
        if (off < vm->file_size) {
            uint64_t n = vm->file_size - off < len ? vm->file_size - off : len;
            memcpy(rl_mem_base() + at, vm->file + off, n);
        }
        if (prot != (L_PROT_READ | L_PROT_WRITE))
            rl_vm_set(&lx.vm, at, size, vm_perm(prot), TAG_MAPPED, RL_VM_KEEP);
    } else if (!anon) {
        int64_t rc = lx_fill_from_file(host, at, len, off);
        if (rc < 0) {
            rl_vm_set(&lx.vm, at, size, RL_VM_NONE, TAG_FREE, RL_VM_DISCARD);
            return rc;
        }
        if (prot != (L_PROT_READ | L_PROT_WRITE))
            rl_vm_set(&lx.vm, at, size, vm_perm(prot), TAG_MAPPED, RL_VM_KEEP);
    }
    return (int64_t)at;
}

uint64_t lx_guest_alloc(uint64_t size)
{
    uint64_t len = lx_page_up(size);
    if (len == 0)
        return 0;
    lx_mutex_lock(&lx_big);
    uint64_t at = lx_find_free(len);
    if (at && lx_map(at, len, L_PROT_READ | L_PROT_WRITE) != 0)
        at = 0;
    lx_mutex_unlock(&lx_big);
    return at;
}

void lx_guest_free(uint64_t addr, uint64_t size)
{
    uint64_t len = lx_page_up(size);
    lx_mutex_lock(&lx_big);
    rl_vm_set(&lx.vm, addr, len, RL_VM_NONE, TAG_FREE, RL_VM_DISCARD);
    lx_mutex_unlock(&lx_big);
}

int64_t lx_sys_munmap(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint64_t addr = a[0], len = a[1];
    if ((addr & (L_PAGE - 1)) != 0 || len == 0)
        return -L_EINVAL;
    uint64_t size = lx_page_up(len);
    if (!in_space(addr, size))
        return -L_EINVAL;
    if (rl_vm_set(&lx.vm, addr, size, RL_VM_NONE, TAG_FREE, RL_VM_DISCARD) != 0)
        return -L_ENOMEM;
    return 0;
}

int64_t lx_sys_mprotect(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)mem;
    uint64_t addr = a[0], len = a[1];
    uint32_t prot = (uint32_t)a[2];
    if ((addr & (L_PAGE - 1)) != 0)
        return -L_EINVAL;
    if (prot & ~(uint32_t)(L_PROT_READ | L_PROT_WRITE | L_PROT_EXEC))
        return -L_EINVAL;
    if (len == 0)
        return 0;
    if (exec_refused(c, prot))
        return LX_PENDING;
    uint64_t size = lx_page_up(len);
    if (!lx_mapped(addr, size))
        return -L_ENOMEM;
    if (rl_vm_set(&lx.vm, addr, size, vm_perm(prot), TAG_MAPPED, RL_VM_KEEP) != 0)
        return -L_ENOMEM;
    return 0;
}

int64_t lx_sys_mremap(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint64_t old = a[0], old_len = a[1], new_len = a[2], flags = a[3];
    if ((old & (L_PAGE - 1)) != 0 || new_len == 0 || (flags & ~(uint64_t)(L_MREMAP_MAYMOVE | L_MREMAP_FIXED)) != 0)
        return -L_EINVAL;
    if (flags & L_MREMAP_FIXED)
        return -L_ENOSYS;
    uint64_t osz = lx_page_up(old_len), nsz = lx_page_up(new_len);
    if (!lx_mapped(old, osz))
        return -L_EFAULT;
    if (nsz == osz)
        return (int64_t)old;
    if (nsz < osz) {
        rl_vm_set(&lx.vm, old + nsz, osz - nsz, RL_VM_NONE, TAG_FREE, RL_VM_DISCARD);
        return (int64_t)old;
    }
    rl_vm_block b = rl_vm_query(&lx.vm, old);
    uint32_t perm = b.perm;
    if (range_free(old + osz, nsz - osz) && old + nsz <= lx.mmap_top) {
        if (rl_vm_set(&lx.vm, old + osz, nsz - osz, perm, TAG_MAPPED, RL_VM_FRESH) != 0)
            return -L_ENOMEM;
        return (int64_t)old;
    }
    if (!(flags & L_MREMAP_MAYMOVE))
        return -L_ENOMEM;
    uint64_t at = lx_find_free(nsz);
    if (!at)
        return -L_ENOMEM;
    if (rl_vm_set(&lx.vm, at, nsz, RL_VM_RW, TAG_MAPPED, RL_VM_FRESH) != 0)
        return -L_ENOMEM;
    rl_vm_kernel_copy(&lx.vm, at, old, osz);
    rl_vm_set(&lx.vm, at, nsz, perm, TAG_MAPPED, RL_VM_KEEP);
    rl_vm_set(&lx.vm, old, osz, RL_VM_NONE, TAG_FREE, RL_VM_DISCARD);
    return (int64_t)at;
}

int64_t lx_sys_madvise(rl_ctx *c, uint8_t *mem, const uint64_t a[6])
{
    (void)c;
    (void)mem;
    uint64_t addr = a[0], len = a[1];
    int advice = (int)a[2];
    if ((addr & (L_PAGE - 1)) != 0)
        return -L_EINVAL;
    if (advice < 0 || (advice > 24 && advice != 100 && advice != 101))
        return -L_EINVAL;
    if (len == 0)
        return 0;
    uint64_t size = lx_page_up(len);
    if (!lx_mapped(addr, size))
        return -L_ENOMEM;
    if (advice == 4) {
        rl_vm_block b = rl_vm_query(&lx.vm, addr);
        uint64_t end = addr + size;
        uint64_t at = addr;
        while (at < end) {
            b = rl_vm_query(&lx.vm, at);
            uint64_t stop = b.addr + b.size < end ? b.addr + b.size : end;
            rl_vm_set(&lx.vm, at, stop - at, b.perm, TAG_MAPPED, RL_VM_FRESH);
            at = stop;
        }
    }
    return 0;
}
