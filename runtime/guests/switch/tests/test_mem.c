/**
 * @file test_mem.c
 * @brief Memory svcs: heap growth, permissions and faults at the right 4 KiB
 * page (also with simulated 16 KiB host pages), MapMemory copy semantics,
 * QueryMemory, shared and transfer memory, and svcGetInfo regions.
 */
#include "t.h"

#include <stdlib.h>
#include <sys/mman.h>

/** @brief svcQueryMemory output. */
typedef struct t_meminfo {
    uint64_t addr, size;
    uint32_t type, attr, perm, ipc_refs, device_refs, pad;
} t_meminfo;

static t_meminfo query(rl_ctx *c, uint64_t info, uint64_t addr)
{
    t_meminfo mi;
    memset(&mi, 0xEE, sizeof mi);
    if (t_svc(c, SVC_QUERY_MEMORY, info, 0, addr, 0, 0, 0) == 0)
        memcpy(&mi, t_mem() + info, sizeof mi);
    return mi;
}

static rl_ctx *boot_pages(uint32_t host_page)
{
    rl_cfg()->host_page = host_page;
    return t_boot(NULL);
}

TEST(heap_grow_and_shrink)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x400000);
    CHECK(heap != 0);
    CHECK_EQ(heap, hos.lay.heap_start);
    CHECK(!t_write(heap, 1).faulted);
    CHECK(!t_write(heap + 0x3FFFFF, 2).faulted);
    CHECK(t_read(heap + 0x400000).faulted);
    CHECK_EQ(t_mem()[heap + 0x3FFFFF], 2);
    CHECK_EQ(t_svc(c, SVC_SET_HEAP_SIZE, 0, 0x1000, 0, 0, 0, 0), KR_INVALID_SIZE);
    CHECK_EQ(t_heap(c, 0x200000), heap);
    CHECK(t_read(heap + 0x200000).faulted);
    CHECK_EQ(t_heap(c, 0x400000), heap);
    CHECK_EQ(t_mem()[heap + 0x3FFFFF], 0);
    t_meminfo mi = query(c, heap + 0x100, heap + 0x1234);
    CHECK_EQ(mi.addr, heap);
    CHECK_EQ(mi.size, 0x400000);
    CHECK_EQ(mi.type, MS_HEAP & 0xFF);
    CHECK_EQ(mi.perm, HOS_PERM_RW);
    CHECK_EQ(t_svc(c, SVC_SET_HEAP_SIZE, 0, 0x200000000ull, 0, 0, 0, 0), KR_INVALID_SIZE);
}

static void permission_pages(uint32_t host_page)
{
    rl_ctx *c = boot_pages(host_page);
    CHECK_EQ(hos.vm.host_page, (host_page < rl_os_page_size() ? rl_os_page_size() : host_page));
    uint64_t heap = t_heap(c, 0x200000);
    CHECK(heap);
    uint64_t page = heap + 0x5000;
    t_mem()[page + 0x10] = 0x5A;
    CHECK_EQ(t_svc(c, SVC_SET_MEMORY_PERMISSION, page, 0x1000, HOS_PERM_R, 0, 0, 0), 0);
    CHECK(!t_read(page + 0x10).faulted);
    CHECK_EQ(t_mem()[page + 0x10], 0x5A);
    t_meminfo mi = query(c, heap, page);
    CHECK_EQ(mi.addr, page);
    CHECK_EQ(mi.size, 0x1000);
    CHECK_EQ(mi.perm, HOS_PERM_R);
    mi = query(c, heap, page + 0x1000);
    CHECK_EQ(mi.addr, page + 0x1000);
    CHECK_EQ(mi.perm, HOS_PERM_RW);
    CHECK_EQ(t_svc(c, SVC_SET_MEMORY_PERMISSION, page, 0x1000, HOS_PERM_NONE, 0, 0, 0), 0);
    if (hos.vm.host_page == 0x1000) {
        t_probe p = t_read(page + 0x10);
        CHECK(p.faulted);
        CHECK_EQ(p.addr, page + 0x10);
        p = t_write(page + 0xFFF, 1);
        CHECK(p.faulted);
        CHECK_EQ(p.addr, page + 0xFFF);
    } else {
        CHECK(!t_write(page - 1, 1).faulted);
        CHECK_EQ(rl_vm_host_prot(&hos.vm, page), PROT_READ | PROT_WRITE);
    }
    uint64_t big = heap + 0x40000;
    CHECK_EQ(t_svc(c, SVC_SET_MEMORY_PERMISSION, big, 0x10000, HOS_PERM_NONE, 0, 0, 0), 0);
    t_probe p = t_read(big + 0x8000);
    CHECK(p.faulted);
    CHECK_EQ(p.addr, big + 0x8000);
    p = t_write(big + 0xFFFF, 1);
    CHECK(p.faulted);
    CHECK_EQ(p.addr, big + 0xFFFF);
    CHECK(!t_write(big + 0x10000, 1).faulted);
    CHECK(!t_write(big - 1, 1).faulted);
    mi = query(c, heap, big + 0x4000);
    CHECK_EQ(mi.addr, big);
    CHECK_EQ(mi.size, 0x10000);
    CHECK_EQ(mi.perm, 0);
    CHECK_EQ(t_svc(c, SVC_SET_MEMORY_PERMISSION, big, 0x10000, HOS_PERM_RW, 0, 0, 0), 0);
    CHECK(!t_write(big + 0x8000, 1).faulted);
    mi = query(c, heap, heap);
    CHECK_EQ(mi.addr, heap);
    CHECK_EQ(mi.size, 0x5000);
    CHECK_EQ(t_svc(c, SVC_SET_MEMORY_PERMISSION, page, 0x1000, HOS_PERM_X, 0, 0, 0), KR_INVALID_PERMISSION);
    CHECK_EQ(t_svc(c, SVC_SET_MEMORY_PERMISSION, page + 1, 0x1000, HOS_PERM_R, 0, 0, 0), KR_INVALID_ADDRESS);
    CHECK_EQ(t_svc(c, SVC_SET_MEMORY_PERMISSION, heap + 0x200000, 0x1000, HOS_PERM_R, 0, 0, 0), KR_INVALID_MEM_STATE);
}

TEST(permissions_fault_at_4k_page) { permission_pages(0x1000); }
TEST(permissions_16k_host_pages) { permission_pages(0x4000); }

TEST(policy_union_on_16k_host_pages)
{
    rl_ctx *c = boot_pages(0x4000);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t hp = heap + 0x10000;
    for (int i = 0; i < 4; i++)
        CHECK_EQ(t_svc(c, SVC_SET_MEMORY_PERMISSION, hp + 0x1000u * (unsigned)i, 0x1000, HOS_PERM_NONE, 0, 0, 0), 0);
    if (hos.vm.host_page == 0x4000) {
        CHECK_EQ(rl_vm_host_prot(&hos.vm, hp), PROT_NONE);
        CHECK(t_read(hp + 0x2000).faulted);
    }
    CHECK_EQ(t_svc(c, SVC_SET_MEMORY_PERMISSION, hp + 0x1000, 0x1000, HOS_PERM_R, 0, 0, 0), 0);
    if (hos.vm.host_page == 0x4000) {
        CHECK_EQ(rl_vm_host_prot(&hos.vm, hp), PROT_READ);
        CHECK(!t_read(hp).faulted);
        CHECK(t_write(hp + 0x1000, 1).faulted);
    }
    t_meminfo mi = query(c, heap, hp);
    CHECK_EQ(mi.addr, hp);
    CHECK_EQ(mi.size, 0x1000);
    CHECK_EQ(mi.perm, 0);
    mi = query(c, heap, hp + 0x1000);
    CHECK_EQ(mi.perm, HOS_PERM_R);
}

TEST(map_memory_copy_semantics)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t src = heap + 0x10000, size = 0x4000;
    for (uint64_t i = 0; i < size; i++)
        t_mem()[src + i] = (uint8_t)(i * 7);
    uint64_t dst = hos.lay.stack_start + 0x100000;
    CHECK_EQ(t_svc(c, SVC_MAP_MEMORY, dst, src, size, 0, 0, 0), 0);
    CHECK_EQ(t_mem()[dst + 0x1234], (uint8_t)(0x1234 * 7));
    CHECK(t_read(src).faulted);
    t_meminfo mi = query(c, heap, src);
    CHECK_EQ(mi.perm, 0);
    CHECK_EQ(mi.attr, MA_BORROWED);
    CHECK_EQ(mi.type, MS_HEAP & 0xFF);
    mi = query(c, heap, dst);
    CHECK_EQ(mi.type, MS_STACK & 0xFF);
    CHECK_EQ(mi.size, size);
    t_mem()[dst + 0x2000] = 0xAB;
    CHECK_EQ(t_svc(c, SVC_MAP_MEMORY, dst, src, size, 0, 0, 0), KR_INVALID_MEM_STATE);
    CHECK_EQ(t_svc(c, SVC_MAP_MEMORY, heap + 0x80000, heap + 0x90000, 0x1000, 0, 0, 0), KR_INVALID_MEM_RANGE);
    CHECK_EQ(t_svc(c, SVC_UNMAP_MEMORY, dst, src, size, 0, 0, 0), 0);
    CHECK_EQ(t_mem()[src + 0x2000], 0xAB);
    CHECK_EQ(t_mem()[src + 0x1234], (uint8_t)(0x1234 * 7));
    CHECK(t_read(dst).faulted);
    mi = query(c, heap, src);
    CHECK_EQ(mi.attr, 0);
    CHECK_EQ(mi.perm, HOS_PERM_RW);
    CHECK_EQ(query(c, heap, dst).type, 0);
    CHECK_EQ(t_svc(c, SVC_UNMAP_MEMORY, dst, src, size, 0, 0, 0), KR_INVALID_MEM_STATE);
}

static uint32_t shmem_handle(hos_shmem *s)
{
    uint32_t h = 0;
    hos_lock();
    if (hos_handle_add(&s->obj, &h) != 0)
        h = 0;
    hos_unlock();
    hos_kobj_unref(&s->obj);
    return h;
}

TEST(shared_memory_16k_host_pages)
{
    rl_ctx *c = boot_pages(0x4000);
    uint64_t heap = t_heap(c, 0x200000);
    hos_shmem *s = rl_hos_shmem_create(0x4000, HOS_PERM_RW);
    CHECK(s);
    uint32_t h = shmem_handle(s);
    CHECK(h);
    rl_hos_shmem_ptr(s)[0] = 0x11;
    uint64_t at = t_free_map_addr();
    CHECK_EQ(t_svc(c, SVC_MAP_SHARED_MEMORY, h, at + 0x1000, 0x4000, HOS_PERM_RW, 0, 0), 0);
    CHECK_EQ(t_mem()[at + 0x1000], 0x11);
    CHECK(!t_write(at + 0x4FFF, 9).faulted);
    CHECK_EQ(rl_hos_shmem_ptr(s)[0x3FFF], 9);
    rl_hos_shmem_ptr(s)[0x10] = 0x22;
    CHECK_EQ(t_mem()[at + 0x1010], 0x22);
    t_meminfo mi = query(c, heap, at);
    CHECK_EQ(mi.addr, at);
    CHECK_EQ(mi.size, 0x1000);
    CHECK_EQ(mi.type, MS_INACCESSIBLE);
    mi = query(c, heap, at + 0x5000);
    CHECK_EQ(mi.addr, at + 0x5000);
    CHECK_EQ(mi.size, 0x3000);
    CHECK_EQ(mi.type, MS_INACCESSIBLE);
    CHECK_EQ(query(c, heap, at + 0x1000).type, MS_SHARED_MEMORY & 0xFF);
    CHECK_EQ(t_svc(c, SVC_MAP_SHARED_MEMORY, h, at + 0x400000, 0x4000, HOS_PERM_R, 0, 0), KR_INVALID_MEM_STATE);
    CHECK_EQ(t_svc(c, SVC_MAP_SHARED_MEMORY, h, at + 0x401000, 0x4000, HOS_PERM_R, 0, 0), 0);
    CHECK_EQ(t_mem()[at + 0x401010], 0x22);
    CHECK(!t_write(at + 0x1020, 0x33).faulted);
    CHECK_EQ(t_mem()[at + 0x401020], 0x33);
    CHECK_EQ(t_svc(c, SVC_UNMAP_SHARED_MEMORY, h, at + 0x401000, 0x4000, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_UNMAP_SHARED_MEMORY, h, at + 0x1000, 0x4000, 0, 0, 0), 0);
    mi = query(c, heap, at);
    CHECK_EQ(mi.type, 0);
    CHECK(mi.addr + mi.size >= at + 0x8000);
    CHECK(t_read(at + 0x1000).faulted);
    CHECK_EQ(t_svc(c, SVC_MAP_SHARED_MEMORY, h, at, 0x4000, HOS_PERM_RW, 0, 0), 0);
    CHECK_EQ(t_mem()[at], 0x11);
    CHECK_EQ(t_mem()[at + 0x20], 0x33);
    CHECK_EQ(t_mem()[at + 0x3FFF], 9);
    CHECK_EQ(query(c, heap, at + 0x4000).type, 0);
    CHECK_EQ(t_svc(c, SVC_UNMAP_SHARED_MEMORY, h, at, 0x4000, 0, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_CLOSE_HANDLE, h, 0, 0, 0, 0, 0), 0);
}

TEST(map_memory_16k_host_pages)
{
    rl_ctx *c = boot_pages(0x4000);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t src = heap + 0x11000, size = 0x1000;
    t_mem()[src] = 0x77;
    uint64_t dst = hos.lay.stack_start + 0x101000;
    CHECK_EQ(t_svc(c, SVC_MAP_MEMORY, dst, src, size, 0, 0, 0), 0);
    CHECK_EQ(t_mem()[dst], 0x77);
    CHECK(!t_write(src - 0x1000, 1).faulted);
    CHECK(!t_write(src + 0x1000, 1).faulted);
    CHECK(!t_write(dst, 2).faulted);
    CHECK_EQ(t_svc(c, SVC_UNMAP_MEMORY, dst, src, size, 0, 0, 0), 0);
    CHECK_EQ(t_mem()[src], 2);
    CHECK(t_read(dst).faulted);
}

TEST(query_memory_edges)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    t_meminfo mi = query(c, heap, 0);
    CHECK_EQ(mi.addr, 0);
    CHECK_EQ(mi.type, 0);
    mi = query(c, heap, hos.lay.as_end + 5);
    CHECK_EQ(mi.addr, hos.lay.as_end);
    CHECK_EQ(mi.type, MS_INACCESSIBLE);
    CHECK_EQ(mi.addr + mi.size, 0);
    mi = query(c, heap, c->tpidrro_el0);
    CHECK_EQ(mi.type, MS_THREAD_LOCAL & 0xFF);
    CHECK_EQ(t_svc(c, SVC_QUERY_MEMORY, 0x1000, 0, heap, 0, 0, 0), KR_INVALID_MEM_STATE);
}

TEST(module_segments)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t base = hos.cfg.code_start;
    CHECK_EQ(rl_hos_map_segment(base, 0x3000, HOS_PERM_RX, MS_CODE_STATIC), 0);
    CHECK_EQ(rl_hos_map_segment(base + 0x3000, 0x1000, HOS_PERM_R, MS_CODE_STATIC), 0);
    CHECK_EQ(rl_hos_map_segment(base + 0x4000, 0x2000, HOS_PERM_RW, MS_CODE_MUTABLE), 0);
    CHECK_EQ(rl_hos_map_segment(base + 0x4000, 0x1000, HOS_PERM_RW, MS_CODE_MUTABLE), KR_INVALID_MEM_STATE);
    uint64_t heap = t_heap(c, 0x200000);
    t_meminfo mi = query(c, heap, base + 0x1000);
    CHECK_EQ(mi.addr, base);
    CHECK_EQ(mi.size, 0x3000);
    CHECK_EQ(mi.perm, HOS_PERM_RX);
    CHECK_EQ(mi.type, MS_CODE_STATIC & 0xFF);
    CHECK(!t_read(base).faulted);
    CHECK(t_write(base + 0x3000, 1).faulted);
    CHECK(!t_write(base + 0x4000, 1).faulted);
    CHECK_EQ(t_svc(c, SVC_SET_MEMORY_PERMISSION, base, 0x1000, HOS_PERM_RW, 0, 0, 0), KR_INVALID_MEM_STATE);
}

TEST(get_info_regions)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t v[16];
    for (uint32_t id = 2; id <= 5; id++) {
        CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, id, HOS_CUR_PROCESS, 0, 0, 0), 0);
        v[id] = c->x[1];
    }
    CHECK(v[3] != 0);
    CHECK_EQ(v[5], 0x180000000ull);
    CHECK(v[4] >= v[2] + v[3] || v[2] >= v[4] + v[5]);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 12, HOS_CUR_PROCESS, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], 0x8000000);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 14, HOS_CUR_PROCESS, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], hos.lay.stack_start);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 18, HOS_CUR_PROCESS, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], hos.cfg.program_id);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 6, HOS_CUR_PROCESS, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], hos.cfg.total_memory);
    t_heap(c, 0x400000);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 7, HOS_CUR_PROCESS, 0, 0, 0), 0);
    CHECK(c->x[1] >= 0x400000);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 11, 0, 3, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 11, 0, 4, 0, 0), KR_INVALID_COMBINATION);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 2, 0x1234, 0, 0, 0), KR_INVALID_HANDLE);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 9, 0, 0, 0, 0), 0);
    uint32_t rl = (uint32_t)c->x[1];
    CHECK_EQ(t_svc(c, SVC_GET_RESOURCE_LIMIT_LIMIT_VALUE, 0, rl, RES_THREADS, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], hos.cfg.max_threads);
    CHECK_EQ(t_svc(c, SVC_GET_RESOURCE_LIMIT_CURRENT_VALUE, 0, rl, RES_THREADS, 0, 0, 0), 0);
    CHECK_EQ(c->x[1], 1);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 25, HOS_CUR_THREAD, UINT64_MAX, 0, 0), 0);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 999, 0, 0, 0, 0), KR_INVALID_ENUM_VALUE);
}

TEST(layout_36_bit)
{
    rl_hos_config cfg;
    rl_hos_default_config(&cfg);
    cfg.address_space_type = 1;
    rl_ctx *c = t_boot(&cfg);
    CHECK_EQ(hos.vm.size, 1ull << 36);
    CHECK_EQ(t_svc(c, SVC_GET_INFO, 0, 4, HOS_CUR_PROCESS, 0, 0, 0), 0);
    CHECK(c->x[1] + 0x180000000ull <= 1ull << 36);
    uint64_t heap = t_heap(c, 0x200000);
    CHECK_EQ(heap, c->x[1] ? heap : 0);
    CHECK(!t_write(heap, 1).faulted);
}

TEST(shared_memory)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x400000);
    hos_shmem *s = rl_hos_shmem_create(0x4000, HOS_PERM_R);
    CHECK(s);
    rl_hos_shmem_ptr(s)[5] = 0x42;
    uint32_t h;
    hos_lock();
    CHECK_EQ(hos_handle_add(&s->obj, &h), 0);
    hos_unlock();
    hos_kobj_unref(&s->obj);
    uint64_t at = t_free_map_addr();
    CHECK_EQ(t_svc(c, SVC_MAP_SHARED_MEMORY, h, at, 0x4000, HOS_PERM_RW, 0, 0), KR_INVALID_PERMISSION);
    CHECK_EQ(t_svc(c, SVC_MAP_SHARED_MEMORY, h, at, 0x1000, HOS_PERM_R, 0, 0), KR_INVALID_SIZE);
    CHECK_EQ(t_svc(c, SVC_MAP_SHARED_MEMORY, h, heap, 0x4000, HOS_PERM_R, 0, 0), KR_INVALID_MEM_RANGE);
    CHECK_EQ(t_svc(c, SVC_MAP_SHARED_MEMORY, h, at, 0x4000, HOS_PERM_R, 0, 0), 0);
    CHECK_EQ(t_mem()[at + 5], 0x42);
    CHECK(t_write(at, 1).faulted);
    rl_hos_shmem_ptr(s)[6] = 0x43;
    CHECK_EQ(t_mem()[at + 6], 0x43);
    uint64_t at2 = at + 0x400000;
    CHECK_EQ(t_svc(c, SVC_MAP_SHARED_MEMORY, h, at2, 0x4000, HOS_PERM_R, 0, 0), 0);
    CHECK_EQ(t_mem()[at2 + 6], 0x43);
    rl_hos_shmem_ptr(s)[7] = 0x44;
    CHECK_EQ(t_mem()[at + 7], 0x44);
    CHECK_EQ(t_mem()[at2 + 7], 0x44);
    CHECK_EQ(t_svc(c, SVC_UNMAP_SHARED_MEMORY, h, at2, 0x4000, 0, 0, 0), 0);
    CHECK_EQ(query(c, heap, at).type, MS_SHARED_MEMORY & 0xFF);
    CHECK_EQ(t_svc(c, SVC_UNMAP_SHARED_MEMORY, h, at, 0x4000, 0, 0, 0), 0);
    CHECK(t_read(at).faulted);
    CHECK_EQ(rl_hos_shmem_ptr(s)[6], 0x43);
    CHECK_EQ(t_svc(c, SVC_CLOSE_HANDLE, h, 0, 0, 0, 0, 0), 0);
}

TEST(transfer_memory)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t a = heap + 0x20000;
    CHECK_EQ(t_svc(c, SVC_CREATE_TRANSFER_MEMORY, 0, a, 0x4000, HOS_PERM_R, 0, 0), 0);
    uint32_t h = (uint32_t)c->x[1];
    t_meminfo mi = query(c, heap, a);
    CHECK_EQ(mi.attr, MA_BORROWED);
    CHECK_EQ(mi.perm, HOS_PERM_R);
    CHECK(t_write(a, 1).faulted);
    CHECK_EQ(t_svc(c, SVC_CREATE_TRANSFER_MEMORY, 0, a, 0x4000, HOS_PERM_R, 0, 0), KR_INVALID_MEM_STATE);
    CHECK_EQ(t_svc(c, SVC_CLOSE_HANDLE, h, 0, 0, 0, 0, 0), 0);
    mi = query(c, heap, a);
    CHECK_EQ(mi.attr, 0);
    CHECK_EQ(mi.perm, HOS_PERM_RW);
    CHECK(!t_write(a, 1).faulted);
    CHECK_EQ(t_svc(c, SVC_CREATE_TRANSFER_MEMORY, 0, a, 0x4000, HOS_PERM_W, 0, 0), KR_INVALID_PERMISSION);
}
