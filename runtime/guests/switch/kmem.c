/**
 * @file kmem.c
 * @brief Process memory: region layout, heap, permissions, svcMapMemory,
 * svcQueryMemory, shared and transfer memory, module segments and TLS pages.
 *
 * Region sizes, the order of checks and the result codes follow Ryujinx (MIT)
 * KPageTableBase and Syscall.
 */
#include "hos.h"

#include <stdlib.h>
#include <string.h>

#define REGION_ALIGN 0x200000ull

static uint64_t tag_of(uint32_t state, uint32_t attr) { return state | ((uint64_t)attr << 32); }
static uint32_t state_of(const rl_vm_block *b) { return (uint32_t)b->tag; }
static uint32_t attr_of(const rl_vm_block *b) { return (uint32_t)(b->tag >> 32); }
static bool aligned(uint64_t v) { return (v & (HOS_PAGE - 1)) == 0; }
static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static bool inside(uint64_t addr, uint64_t size, uint64_t start, uint64_t end)
{
    return addr >= start && addr + size <= end && addr + size > addr;
}

static bool overlaps(uint64_t addr, uint64_t size, uint64_t start, uint64_t end)
{
    return start < end && addr < end && start < addr + size;
}

static bool check(uint64_t addr, uint64_t size, uint32_t smask, uint32_t sval, uint32_t pmask, uint32_t pval,
                  uint32_t amask, uint32_t aval, rl_vm_block *out)
{
    rl_vm_block b;
    if (!rl_vm_uniform(&hos.vm, addr, size, &b))
        return false;
    if ((state_of(&b) & smask) != sval || (hos_perm(b.perm) & pmask) != pval || (attr_of(&b) & amask) != aval)
        return false;
    if (out)
        *out = b;
    return true;
}

static bool is_free(uint64_t addr, uint64_t size)
{
    return check(addr, size, 0xFFFFFFFFu, MS_FREE, 0, 0, 0, 0, NULL);
}

static uint64_t find_free_top(uint64_t start, uint64_t end, uint64_t size)
{
    if (end > hos.vm.size)
        end = hos.vm.size;
    if (end < start + size)
        return 0;
    uint64_t a = end - size;
    for (;;) {
        rl_vm_block b = rl_vm_query(&hos.vm, a);
        if (state_of(&b) == MS_FREE && b.addr + b.size >= a + size)
            return a;
        if (b.addr < start + size)
            return 0;
        a = b.addr - size;
    }
}

static void layout_39(hos_layout *l, uint64_t code_start, uint64_t code_size)
{
    l->bits = 39;
    l->as_start = 0;
    l->as_end = 1ull << 39;
    l->code_start = code_start & ~(REGION_ALIGN - 1);
    l->code_end = align_up(code_start + code_size, REGION_ALIGN);
    l->aslr_start = 0x8000000;
    l->aslr_end = l->aslr_start + 0x7ff8000000ull;
    uint64_t base = (l->code_start - l->aslr_start >= l->as_end - l->code_end) ? l->aslr_start : l->code_end;
    l->alias_start = base;
    l->alias_end = base + 0x1000000000ull;
    l->heap_start = l->alias_end;
    l->heap_end = l->heap_start + 0x180000000ull;
    l->stack_start = l->heap_end;
    l->stack_end = l->stack_start + 0x80000000ull;
    l->tlsio_start = l->stack_end;
    l->tlsio_end = l->tlsio_start + 0x1000000000ull;
}

static void layout_36(hos_layout *l)
{
    l->bits = 36;
    l->as_start = 0;
    l->as_end = 1ull << 36;
    l->code_start = 0x8000000;
    l->code_end = 0x80000000;
    l->aslr_start = 0x8000000;
    l->aslr_end = l->aslr_start + 0xff8000000ull;
    l->alias_start = l->code_end;
    l->alias_end = l->alias_start + 0x180000000ull;
    l->heap_start = l->alias_end;
    l->heap_end = l->heap_start + 0x180000000ull;
    l->stack_start = 0x8000000;
    l->stack_end = 0x80000000;
    l->tlsio_start = 0x8000000;
    l->tlsio_end = 0x80000000;
}

int hos_mem_init(void)
{
    unsigned want = hos.cfg.address_space_type == 1 ? 36 : 39;
    if (hos.cfg.address_space_type == 0 || hos.cfg.address_space_type == 2) {
        rl_log(RL_LOG_ERROR, "kernel", "32-bit address spaces are not supported");
        return -1;
    }
    if (rl_cfg()->address_bits == 36 || rl_cfg()->address_bits == 39)
        want = rl_cfg()->address_bits;
    if (want == 39 && rl_vm_init(&hos.vm, 39, 0) != 0) {
        rl_log(RL_LOG_INFO, "kernel", "host cannot reserve 2^39 bytes; using the 36-bit layout");
        want = 36;
    }
    if (want == 36 && rl_vm_init(&hos.vm, 36, 0) != 0) {
        rl_log(RL_LOG_ERROR, "kernel", "cannot reserve the guest address space");
        return -1;
    }
    if (want == 39)
        layout_39(&hos.lay, hos.cfg.code_start, hos.cfg.code_size);
    else
        layout_36(&hos.lay);
    rl_set_mem_base(hos.vm.base);
    rl_fault_install(&hos.vm);
    return 0;
}

rl_result rl_hos_map_segment(uint64_t addr, uint64_t size, uint32_t perm, uint32_t state)
{
    rl_result rc = KR_SUCCESS;
    hos_lock();
    if (!aligned(addr) || !aligned(size) || !size)
        rc = KR_INVALID_ADDRESS;
    else if (!is_free(addr, size))
        rc = KR_INVALID_MEM_STATE;
    else if (rl_vm_set(&hos.vm, addr, size, hos_vm_perm(perm), tag_of(state, 0), RL_VM_FRESH))
        rc = KR_OUT_OF_MEMORY;
    hos_unlock();
    return rc;
}

rl_result rl_hos_map_main_stack(uint64_t size, uint64_t *top)
{
    rl_result rc = KR_SUCCESS;
    size = align_up(size, HOS_PAGE);
    hos_lock();
    uint64_t a = find_free_top(hos.lay.stack_start, hos.lay.stack_end, size);
    if (!a || !size)
        rc = KR_OUT_OF_MEMORY;
    else if (rl_vm_set(&hos.vm, a, size, RL_VM_RW, tag_of(MS_STACK, 0), RL_VM_FRESH))
        rc = KR_OUT_OF_MEMORY;
    else {
        hos.stack_bytes += size;
        *top = a + size;
    }
    hos_unlock();
    return rc;
}

uint64_t hos_tls_alloc(void)
{
    for (uint32_t p = 0; p < hos.tls_page_count; p++)
        for (uint32_t s = 0; s < 8; s++)
            if (!(hos.tls_used[p] & (1u << s))) {
                uint64_t slot = hos.tls_pages[p] + s * 0x200u;
                hos.tls_used[p] |= (uint8_t)(1u << s);
                memset(hos.vm.base + slot, 0, 0x200);
                return slot;
            }
    if (hos.tls_page_count >= HOS_TLS_PAGES)
        return 0;
    uint64_t a = find_free_top(hos.lay.tlsio_start, hos.lay.tlsio_end, HOS_PAGE);
    if (!a || rl_vm_set(&hos.vm, a, HOS_PAGE, RL_VM_RW, tag_of(MS_THREAD_LOCAL, 0), RL_VM_FRESH))
        return 0;
    hos.tls_pages[hos.tls_page_count] = a;
    hos.tls_used[hos.tls_page_count] = 1;
    hos.tls_page_count++;
    return a;
}

void hos_tls_free(uint64_t slot)
{
    for (uint32_t p = 0; p < hos.tls_page_count; p++)
        if (slot >= hos.tls_pages[p] && slot < hos.tls_pages[p] + HOS_PAGE)
            hos.tls_used[p] &= (uint8_t) ~(1u << ((slot - hos.tls_pages[p]) / 0x200u));
}

uint64_t hos_used_memory(void)
{
    uint64_t used = hos.stack_bytes;
    uint64_t a = 0;
    while (a < hos.vm.size) {
        rl_vm_block b = rl_vm_query(&hos.vm, a);
        uint32_t s = state_of(&b);
        if (s == MS_HEAP || s == MS_CODE_STATIC || s == MS_CODE_MUTABLE || s == MS_THREAD_LOCAL)
            used += b.size;
        a = b.addr + b.size;
    }
    return used;
}

rl_result hos_set_heap_size(uint64_t size, uint64_t *out)
{
    rl_result rc = KR_SUCCESS;
    if (size & 0xfffffffe001fffffull)
        return KR_INVALID_SIZE;
    hos_lock();
    uint64_t start = hos.lay.heap_start, cur = hos.heap_size;
    if (size > hos.lay.heap_end - hos.lay.heap_start) {
        rc = KR_OUT_OF_MEMORY;
    } else if (size > cur) {
        if (!is_free(start + cur, size - cur))
            rc = KR_INVALID_MEM_STATE;
        else if (rl_vm_set(&hos.vm, start + cur, size - cur, RL_VM_RW, tag_of(MS_HEAP, 0), RL_VM_FRESH))
            rc = KR_OUT_OF_MEMORY;
    } else if (size < cur) {
        if (!check(start + size, cur - size, 0xFFFFFFFFu, MS_HEAP, 0xFFFFFFFFu, HOS_PERM_RW, 0xFF, 0, NULL))
            rc = KR_INVALID_MEM_STATE;
        else if (rl_vm_set(&hos.vm, start + size, cur - size, RL_VM_NONE, 0, RL_VM_DISCARD))
            rc = KR_OUT_OF_MEMORY;
    }
    if (rc == KR_SUCCESS) {
        hos.heap_size = size;
        *out = start;
    }
    hos_unlock();
    return rc;
}

rl_result hos_set_memory_permission(uint64_t addr, uint64_t size, uint32_t perm)
{
    if (!aligned(addr))
        return KR_INVALID_ADDRESS;
    if (!aligned(size) || size == 0)
        return KR_INVALID_SIZE;
    if (addr + size <= addr)
        return KR_INVALID_MEM_STATE;
    if (perm != HOS_PERM_NONE && (perm | HOS_PERM_W) != HOS_PERM_RW)
        return KR_INVALID_PERMISSION;
    rl_result rc = KR_SUCCESS;
    rl_vm_block b;
    hos_lock();
    if (!rl_vm_contains(&hos.vm, addr, size) ||
        !check(addr, size, MS_PERMISSION_CHANGE_ALLOWED, MS_PERMISSION_CHANGE_ALLOWED, 0, 0,
               0xFFu & ~(uint32_t)(MA_IPC_MAPPED | MA_DEVICE_MAPPED), 0, &b))
        rc = KR_INVALID_MEM_STATE;
    else if (hos_vm_perm(perm) != b.perm && rl_vm_set(&hos.vm, addr, size, hos_vm_perm(perm), b.tag, RL_VM_KEEP))
        rc = KR_OUT_OF_MEMORY;
    hos_unlock();
    return rc;
}

rl_result hos_set_memory_attribute(uint64_t addr, uint64_t size, uint32_t mask, uint32_t value)
{
    if (!aligned(addr))
        return KR_INVALID_ADDRESS;
    if (!aligned(size) || size == 0)
        return KR_INVALID_SIZE;
    uint32_t all = mask | value;
    if (mask != all || (all | MA_UNCACHED) != MA_UNCACHED)
        return KR_INVALID_COMBINATION;
    rl_result rc = KR_SUCCESS;
    rl_vm_block b;
    uint32_t smask = (mask & MA_UNCACHED) ? MS_ATTRIBUTE_CHANGE_ALLOWED : 0;
    hos_lock();
    if (!rl_vm_contains(&hos.vm, addr, size) ||
        !check(addr, size, smask, smask, 0, 0, MA_BORROWED | MA_IPC_MAPPED, 0, &b))
        rc = KR_INVALID_MEM_STATE;
    else {
        uint32_t attr = (attr_of(&b) & ~mask) | (mask & value);
        if (rl_vm_set(&hos.vm, addr, size, b.perm, tag_of(state_of(&b), attr), RL_VM_KEEP))
            rc = KR_OUT_OF_MEMORY;
    }
    hos_unlock();
    return rc;
}

static rl_result map_args(uint64_t dst, uint64_t src, uint64_t size)
{
    if (!aligned(src | dst))
        return KR_INVALID_ADDRESS;
    if (!aligned(size) || size == 0)
        return KR_INVALID_SIZE;
    if (src + size <= src || dst + size <= dst)
        return KR_INVALID_MEM_STATE;
    if (!inside(src, size, hos.lay.as_start, hos.lay.as_end))
        return KR_INVALID_MEM_STATE;
    if (!inside(dst, size, hos.lay.stack_start, hos.lay.stack_end) ||
        overlaps(dst, size, hos.lay.heap_start, hos.lay.heap_end) ||
        overlaps(dst, size, hos.lay.alias_start, hos.lay.alias_end))
        return KR_INVALID_MEM_RANGE;
    return KR_SUCCESS;
}

rl_result hos_map_memory(uint64_t dst, uint64_t src, uint64_t size)
{
    rl_result rc = map_args(dst, src, size);
    if (rc)
        return rc;
    rl_vm_block b;
    hos_lock();
    if (!check(src, size, MS_MAP_ALLOWED, MS_MAP_ALLOWED, 0xFFFFFFFFu, HOS_PERM_RW,
               0xFFu & ~(uint32_t)(MA_IPC_MAPPED | MA_DEVICE_MAPPED), 0, &b) ||
        !is_free(dst, size)) {
        rc = KR_INVALID_MEM_STATE;
    } else if (rl_vm_set(&hos.vm, dst, size, RL_VM_RW, tag_of(MS_STACK, 0), RL_VM_FRESH)) {
        rc = KR_OUT_OF_MEMORY;
    } else {
        rl_vm_kernel_copy(&hos.vm, dst, src, size);
        if (rl_vm_set(&hos.vm, src, size, RL_VM_NONE, tag_of(state_of(&b), MA_BORROWED), RL_VM_KEEP))
            rc = KR_OUT_OF_MEMORY;
    }
    hos_unlock();
    return rc;
}

rl_result hos_unmap_memory(uint64_t dst, uint64_t src, uint64_t size)
{
    rl_result rc = map_args(dst, src, size);
    if (rc)
        return rc;
    rl_vm_block b;
    hos_lock();
    if (!check(src, size, MS_MAP_ALLOWED, MS_MAP_ALLOWED, 0xFFFFFFFFu, HOS_PERM_NONE, 0xFF, MA_BORROWED, &b) ||
        !check(dst, size, 0xFFFFFFFFu, MS_STACK, 0, 0, 0xFFu & ~(uint32_t)(MA_IPC_MAPPED | MA_DEVICE_MAPPED), 0, NULL)) {
        rc = KR_INVALID_MEM_STATE;
    } else if (rl_vm_set(&hos.vm, src, size, RL_VM_RW, tag_of(state_of(&b), 0), RL_VM_KEEP)) {
        rc = KR_OUT_OF_MEMORY;
    } else {
        rl_vm_kernel_copy(&hos.vm, src, dst, size);
        if (rl_vm_set(&hos.vm, dst, size, RL_VM_NONE, 0, RL_VM_DISCARD))
            rc = KR_OUT_OF_MEMORY;
    }
    hos_unlock();
    return rc;
}

bool hos_user_range(uint64_t addr, uint64_t size, uint32_t perm)
{
    uint64_t a = addr;
    if (!rl_vm_contains(&hos.vm, addr, size))
        return false;
    while (a < addr + size) {
        rl_vm_block b = rl_vm_query(&hos.vm, a);
        if (!(state_of(&b) & MS_MAPPED) || (hos_perm(b.perm) & perm) != perm)
            return false;
        a = b.addr + b.size;
    }
    return true;
}

rl_result hos_query_memory(uint64_t info_addr, uint64_t addr, uint32_t *page_info)
{
    struct {
        uint64_t addr, size;
        uint32_t type, attr, perm, ipc_refs, device_refs, pad;
    } mi;
    memset(&mi, 0, sizeof mi);
    *page_info = 0;
    hos_lock();
    if (addr >= hos.lay.as_end) {
        mi.addr = hos.lay.as_end;
        mi.size = 0 - hos.lay.as_end;
        mi.type = MS_INACCESSIBLE;
    } else {
        rl_vm_block b = rl_vm_query(&hos.vm, addr);
        mi.addr = b.addr;
        mi.size = b.size;
        mi.type = state_of(&b) & 0xFF;
        mi.attr = attr_of(&b);
        mi.perm = hos_perm(b.perm);
    }
    rl_result rc = KR_SUCCESS;
    if (!hos_user_range(info_addr, sizeof mi, HOS_PERM_W))
        rc = KR_INVALID_MEM_STATE;
    else
        rl_vm_kernel_write(&hos.vm, info_addr, &mi, sizeof mi);
    hos_unlock();
    return rc;
}

static void shmem_destroy(hos_kobj *o)
{
    hos_shmem *s = (hos_shmem *)o;
    rl_shm_destroy(&s->shm);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

hos_shmem *rl_hos_shmem_create(uint64_t size, uint32_t user_perm)
{
    hos_shmem *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    size = align_up(size, HOS_PAGE);
    if (rl_shm_create(&s->shm, size) != 0) {
        free(s);
        return NULL;
    }
    pthread_mutex_init(&s->lock, NULL);
    hos_kobj_init(&s->obj, KOBJ_SHARED_MEMORY, shmem_destroy);
    s->size = size;
    s->user_perm = user_perm;
    return s;
}

uint8_t *rl_hos_shmem_ptr(hos_shmem *s)
{
    return s->shm.host;
}

uint8_t *rl_hos_shmem_lock(hos_shmem *s)
{
    pthread_mutex_lock(&s->lock);
    return s->shm.host;
}

void rl_hos_shmem_unlock(hos_shmem *s) { pthread_mutex_unlock(&s->lock); }

static int shmem_view(const hos_shmem *s, uint64_t addr)
{
    for (int i = 0; i < HOS_SHMEM_VIEWS; i++)
        if (s->views[i] == addr && addr)
            return i;
    return -1;
}

rl_result hos_map_shared_memory(uint32_t h, uint64_t addr, uint64_t size, uint32_t perm)
{
    if (!aligned(addr))
        return KR_INVALID_ADDRESS;
    if (!aligned(size) || size == 0)
        return KR_INVALID_SIZE;
    if (addr + size <= addr)
        return KR_INVALID_MEM_STATE;
    if ((perm | HOS_PERM_W) != HOS_PERM_RW)
        return KR_INVALID_PERMISSION;
    rl_result rc = KR_SUCCESS;
    hos_lock();
    hos_shmem *s = (hos_shmem *)hos_handle_get(h, KOBJ_SHARED_MEMORY);
    int slot = s ? shmem_view(s, 0) : -1;
    if (s)
        for (int i = 0; i < HOS_SHMEM_VIEWS && slot < 0; i++)
            if (!s->views[i])
                slot = i;
    if (!s)
        rc = KR_INVALID_HANDLE;
    else if (!inside(addr, size, hos.lay.aslr_start, hos.lay.aslr_end) ||
             overlaps(addr, size, hos.lay.heap_start, hos.lay.heap_end) ||
             overlaps(addr, size, hos.lay.alias_start, hos.lay.alias_end))
        rc = KR_INVALID_MEM_RANGE;
    else if (size != s->size)
        rc = KR_INVALID_SIZE;
    else if ((perm & ~s->user_perm) != 0)
        rc = KR_INVALID_PERMISSION;
    else if (!is_free(addr, size))
        rc = KR_INVALID_MEM_STATE;
    else if (slot < 0)
        rc = KR_OUT_OF_RESOURCE;
    else if (((addr | size) & (hos.vm.host_page - 1)) != 0) {
        if (rl_once(0x53484D454D000000ull))
            rl_log(RL_LOG_WARN, "kernel",
                   "shared memory at 0x%llx size 0x%llx is not aligned to the 0x%llx host page: not supported yet",
                   (unsigned long long)addr, (unsigned long long)size, (unsigned long long)hos.vm.host_page);
        rc = KR_INVALID_ADDRESS;
    } else if (rl_vm_map_shm(&hos.vm, addr, size, &s->shm) ||
               rl_vm_set(&hos.vm, addr, size, hos_vm_perm(perm), tag_of(MS_SHARED_MEMORY, 0), RL_VM_KEEP)) {
        rc = KR_OUT_OF_MEMORY;
    } else {
        s->views[slot] = addr;
    }
    hos_unlock();
    if (s)
        hos_kobj_unref(&s->obj);
    return rc;
}

rl_result hos_unmap_shared_memory(uint32_t h, uint64_t addr, uint64_t size)
{
    if (!aligned(addr))
        return KR_INVALID_ADDRESS;
    if (!aligned(size) || size == 0)
        return KR_INVALID_SIZE;
    if (addr + size <= addr)
        return KR_INVALID_MEM_STATE;
    rl_result rc = KR_SUCCESS;
    hos_lock();
    hos_shmem *s = (hos_shmem *)hos_handle_get(h, KOBJ_SHARED_MEMORY);
    int slot = s ? shmem_view(s, addr) : -1;
    if (!s)
        rc = KR_INVALID_HANDLE;
    else if (slot < 0 || s->size != size || !check(addr, size, 0xFFFFFFFFu, MS_SHARED_MEMORY, 0, 0, 0xFF, 0, NULL))
        rc = KR_INVALID_MEM_STATE;
    else {
        s->views[slot] = 0;
        if (rl_vm_set(&hos.vm, addr, size, RL_VM_NONE, 0, RL_VM_DISCARD))
            rc = KR_OUT_OF_MEMORY;
    }
    hos_unlock();
    if (s)
        hos_kobj_unref(&s->obj);
    return rc;
}

static void tmem_destroy(hos_kobj *o)
{
    hos_tmem *t = (hos_tmem *)o;
    rl_vm_block b;
    hos_lock();
    if (check(t->addr, t->size, MS_TRANSFER_MEMORY_ALLOWED, MS_TRANSFER_MEMORY_ALLOWED, 0, 0, 0xFF, MA_BORROWED, &b))
        rl_vm_set(&hos.vm, t->addr, t->size, RL_VM_RW, tag_of(state_of(&b), 0), RL_VM_KEEP);
    hos_unlock();
    free(t);
}

rl_result hos_create_transfer_memory(uint32_t *out, uint64_t addr, uint64_t size, uint32_t perm)
{
    if (!aligned(addr))
        return KR_INVALID_ADDRESS;
    if (!aligned(size) || size == 0)
        return KR_INVALID_SIZE;
    if (addr + size <= addr)
        return KR_INVALID_MEM_STATE;
    if (perm > HOS_PERM_RW || perm == HOS_PERM_W)
        return KR_INVALID_PERMISSION;
    hos_tmem *t = calloc(1, sizeof *t);
    if (!t)
        return KR_OUT_OF_RESOURCE;
    hos_kobj_init(&t->obj, KOBJ_TRANSFER_MEMORY, tmem_destroy);
    t->addr = addr;
    t->size = size;
    t->perm = perm;
    rl_result rc = KR_SUCCESS;
    rl_vm_block b;
    hos_lock();
    if (!check(addr, size, MS_TRANSFER_MEMORY_ALLOWED, MS_TRANSFER_MEMORY_ALLOWED, 0xFFFFFFFFu, HOS_PERM_RW, 0xFF, 0, &b))
        rc = KR_INVALID_MEM_STATE;
    else if (rl_vm_set(&hos.vm, addr, size, hos_vm_perm(perm), tag_of(state_of(&b), MA_BORROWED), RL_VM_KEEP))
        rc = KR_OUT_OF_MEMORY;
    else
        rc = hos_handle_add(&t->obj, out);
    hos_unlock();
    hos_kobj_unref(&t->obj);
    return rc;
}
