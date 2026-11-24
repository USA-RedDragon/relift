/**
 * @file test_core.c
 * @brief The core alone, driven through a fake guest and a fake CPU that
 * resemble no real layer: 8 KiB guest pages, a 32-bit address space, a 40.5
 * MHz clock, big-endian data, a 2-byte code granule and a small context
 * whose common block is not at the end. This binary links only the core.
 */
#include "runner.h"
#include "hostlib.h"
#include "capture.h"

#include "crash.h"

#include <dirent.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

/** @brief The fake CPU's context. */
struct rl_ctx {
    /** @brief Sixteen 32-bit registers. */
    uint32_t r[16];
    /** @brief Target of the current indirect transfer. */
    uint32_t pc;
    /** @brief Set by ctx_init. */
    uint32_t init_mark;
    /** @brief The core's block. */
    rl_ctx_common k;
    /** @brief Something after the block, so a wrong offset shows. */
    uint64_t tail;
};

static int g_binds;

static void fake_init(rl_ctx *c) { c->init_mark = 0x5EED; }
static void fake_bind(rl_ctx *c) { (void)c; g_binds++; }
static void fake_describe(const rl_ctx *c, char *buf, size_t n) { snprintf(buf, n, "r15 0x%x", c->r[15]); }
static uint64_t fake_call_site(const rl_ctx *c) { return c->r[14] - 2u; }

static const rl_cpu_desc fake_cpu = {
    .name = "fake",
    .ctx_size = sizeof(struct rl_ctx),
    .ctx_align = 8,
    .common_offset = offsetof(struct rl_ctx, k),
    .code_shift = 1,
    .big_endian = true,
    .ctx_init = fake_init,
    .bind = fake_bind,
    .describe = fake_describe,
    .call_site = fake_call_site,
};

static const rl_guest_desc fake_guest = {
    .name = "fake",
    .page_size = 0x2000,
    .address_bits = 32,
    .tick_hz = 40500000,
};

/** @brief Starts the core with the fake descriptors and reserves a 32-bit space. */
static int boot(rl_vm *vm)
{
    if (rl_core_init(&fake_guest, &fake_cpu) != 0 || rl_vm_init(vm, 32, 0) != 0)
        return -1;
    rl_set_mem_base(vm->base);
    rl_fault_install(vm);
    return 0;
}

/** @brief Starts capturing stderr into a pipe; returns the saved fd. */
static int capture_begin(int fds[2])
{
    fflush(stderr);
    if (pipe(fds) != 0)
        return -1;
    int saved = dup(2);
    dup2(fds[1], 2);
    return saved;
}

/** @brief Stops capturing, restores stderr and reads what was written into buf. */
static void capture_end(int fds[2], int saved, char *buf, size_t n)
{
    fflush(stderr);
    dup2(saved, 2);
    close(saved);
    close(fds[1]);
    size_t len = 0;
    while (len + 1 < n) {
        ssize_t got = read(fds[0], buf + len, n - 1 - len);
        if (got <= 0)
            break;
        len += (size_t)got;
    }
    buf[len] = 0;
    close(fds[0]);
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle))
        n++;
    return n;
}

/** @brief Makes a fresh report folder under TMPDIR and points stop reports at it. */
static void report_dir(char *dir, size_t n)
{
    const char *tmp = getenv("TMPDIR");
    snprintf(dir, n, "%s/relift-report-XXXXXX", tmp && *tmp ? tmp : ".");
    if (!mkdtemp(dir))
        dir[0] = 0;
    rl_report_set_dir(dir);
}

/** @brief Removes the report folder report_dir made, with the files in it, and points stop reports nowhere. */
static void report_dir_done(const char *dir)
{
    rl_report_set_dir("");
    DIR *d = dir[0] ? opendir(dir) : NULL;
    if (!d)
        return;
    struct dirent *e;
    char path[1024];
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        unlink(path);
    }
    closedir(d);
    rmdir(dir);
}

TEST(core_needs_descriptors)
{
    rl_vm vm;
    CHECK(rl_core_cpu() == NULL);
    CHECK_EQ(rl_vm_init(&vm, 32, 0), -1);
    CHECK(rl_thread_alloc() == NULL);
    CHECK_EQ(rl_units_register(NULL, 0), -1);
    rl_guest_desc g = fake_guest;
    g.page_size = 0x3000;
    CHECK_EQ(rl_core_init(&g, &fake_cpu), -1);
    g = fake_guest;
    g.tick_hz = 0;
    CHECK_EQ(rl_core_init(&g, &fake_cpu), -1);
    rl_cpu_desc cpu = fake_cpu;
    cpu.common_offset = (uint32_t)sizeof(struct rl_ctx) - 8;
    CHECK_EQ(rl_core_init(&fake_guest, &cpu), -1);
    cpu = fake_cpu;
    cpu.ctx_align = 4;
    CHECK_EQ(rl_core_init(&fake_guest, &cpu), -1);
    CHECK(rl_core_cpu() == NULL);
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    CHECK(rl_core_guest() == &fake_guest);
}

TEST(core_vm_uses_the_guest_page_and_width)
{
    rl_vm vm;
    CHECK_EQ(boot(&vm), 0);
    CHECK_EQ(vm.size, 1ull << 32);
    CHECK_EQ(vm.guest_page, 0x2000);
    CHECK(vm.host_page >= 0x2000);
    CHECK_EQ(rl_vm_set(&vm, 0x10000, 0x1000, RL_VM_RW, 0, RL_VM_FRESH), -1);
    CHECK_EQ(rl_vm_set(&vm, 0x11000, 0x2000, RL_VM_RW, 0, RL_VM_FRESH), -1);
    CHECK_EQ(rl_vm_set(&vm, 0xFFFFE000ull, 0x2000, RL_VM_RW, 1, RL_VM_FRESH), 0);
    CHECK_EQ(rl_vm_set(&vm, 0xFFFFE000ull, 0x4000, RL_VM_RW, 1, RL_VM_FRESH), -1);
    CHECK(!t_write(0xFFFFFFFFull, 7).faulted);
    CHECK_EQ(rl_vm_query(&vm, 0xFFFFE123ull).tag, 1);
    rl_vm wide;
    CHECK_EQ(rl_vm_init(&wide, 33, 0), -1);
    rl_vm_destroy(&vm);
}

TEST(core_faults_report_guest_addresses)
{
    rl_vm vm;
    CHECK_EQ(boot(&vm), 0);
    uint64_t hp = vm.host_page;
    CHECK_EQ(rl_vm_set(&vm, 4 * hp, hp, RL_VM_R, 0, RL_VM_FRESH), 0);
    CHECK(!t_read(4 * hp + 5).faulted);
    t_probe p = t_write(4 * hp + 5, 1);
    CHECK(p.faulted);
    CHECK_EQ(p.addr, 4 * hp + 5);
    CHECK_EQ(rl_vm_set(&vm, 4 * hp, hp, RL_VM_RW, 0, RL_VM_KEEP), 0);
    CHECK(!t_write(4 * hp + 5, 1).faulted);
    CHECK_EQ(rl_vm_set(&vm, 4 * hp, hp, RL_VM_NONE, 0, RL_VM_DISCARD), 0);
    p = t_read(4 * hp + 9);
    CHECK(p.faulted);
    CHECK_EQ(p.addr, 4 * hp + 9);
    rl_vm_destroy(&vm);
}

/** A corrupted value is found wherever else readable guest memory holds it, 8-byte aligned, and nowhere unreadable. */
TEST(core_fault_finds_every_readable_copy_of_a_word)
{
    rl_vm vm;
    CHECK_EQ(boot(&vm), 0);
    CHECK_EQ(rl_vm_set(&vm, 0x100000, 0x10000, RL_VM_RW, 0, RL_VM_FRESH), 0);
    CHECK_EQ(rl_vm_set(&vm, 0x200000, 0x10000, RL_VM_RW, 0, RL_VM_FRESH), 0);
    const uint64_t v = 0x0300010000010000ull;
    memcpy(vm.base + 0x100008, &v, 8);
    memcpy(vm.base + 0x10fff8, &v, 8);
    memcpy(vm.base + 0x200100, &v, 8);
    memcpy(vm.base + 0x200204, &v, 8);
    CHECK_EQ(rl_vm_set(&vm, 0x200000, 0x10000, RL_VM_NONE, 0, RL_VM_KEEP), 0);
    uint64_t at[4];
    CHECK_EQ(rl_fault_find_word(v, at, 4), 2);
    CHECK_EQ(at[0], 0x100008);
    CHECK_EQ(at[1], 0x10fff8);
    rl_vm_destroy(&vm);
}

TEST(core_permissions_are_its_own_encoding)
{
    rl_vm vm;
    CHECK_EQ(boot(&vm), 0);
    uint64_t hp = vm.host_page;
    CHECK_EQ(rl_vm_set(&vm, 2 * hp, hp, RL_VM_X, 0, RL_VM_FRESH), 0);
    CHECK_EQ(rl_vm_host_prot(&vm, 2 * hp), PROT_NONE);
    CHECK_EQ(rl_vm_set(&vm, 2 * hp, hp, RL_VM_RX, 0, RL_VM_KEEP), 0);
    CHECK_EQ(rl_vm_host_prot(&vm, 2 * hp), PROT_READ);
    CHECK_EQ(rl_vm_set(&vm, 2 * hp, hp, RL_VM_W, 0, RL_VM_KEEP), 0);
    CHECK_EQ(rl_vm_host_prot(&vm, 2 * hp), PROT_READ | PROT_WRITE);
    CHECK_EQ(rl_vm_set(&vm, 2 * hp, hp, 1u | 2u | 4u, 0, RL_VM_KEEP), 0);
    CHECK_EQ(rl_vm_host_prot(&vm, 2 * hp), PROT_NONE);
    rl_vm_destroy(&vm);
}

TEST(vm_block_map_coalesces)
{
    rl_vm vm;
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    CHECK_EQ(rl_vm_init(&vm, 32, 0x4000), 0);
    CHECK_EQ(rl_vm_set(&vm, 0x20000, 0x10000, RL_VM_RW, 5, RL_VM_FRESH), 0);
    CHECK_EQ(rl_vm_set(&vm, 0x24000, 0x2000, RL_VM_R, 5, RL_VM_KEEP), 0);
    CHECK_EQ(vm.count, 5);
    CHECK_EQ(rl_vm_set(&vm, 0x24000, 0x2000, RL_VM_RW, 5, RL_VM_KEEP), 0);
    CHECK_EQ(vm.count, 3);
    rl_vm_block b = rl_vm_query(&vm, 0x2FFFF);
    CHECK_EQ(b.addr, 0x20000);
    CHECK_EQ(b.size, 0x10000);
    CHECK_EQ(rl_vm_set(&vm, 0x20000, 0x10000, RL_VM_NONE, 0, RL_VM_DISCARD), 0);
    CHECK_EQ(vm.count, 1);
    CHECK_EQ(rl_vm_set(&vm, 0x20001, 0x2000, RL_VM_R, 0, RL_VM_KEEP), -1);
    rl_vm_destroy(&vm);
}

TEST(capture_tool_is_seen_from_renderdocs_layer_variable)
{
    const char *was = getenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
    unsetenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
    static const char *const rd[] = { "librenderdoc.so", NULL };
    CHECK_EQ(rl_capture_tool(), rl_hostlib_loaded(rd) != NULL);
    setenv("ENABLE_VULKAN_RENDERDOC_CAPTURE", "1", 1);
    CHECK(rl_capture_tool());
    if (was)
        setenv("ENABLE_VULKAN_RENDERDOC_CAPTURE", was, 1);
    else
        unsetenv("ENABLE_VULKAN_RENDERDOC_CAPTURE");
}

TEST(vm_keeps_the_ranges_of_recent_map_changes)
{
    rl_vm vm;
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    CHECK_EQ(rl_vm_init(&vm, 32, 0x4000), 0);
    uint64_t gen = vm.map_gen;
    CHECK(!rl_vm_changed_since(&vm, gen, 0x100000, 0x10000));
    CHECK_EQ(rl_vm_set(&vm, 0x20000, 0x10000, RL_VM_RW, 5, RL_VM_FRESH), 0);
    CHECK(rl_vm_changed_since(&vm, gen, 0x2c000, 0x8000));
    CHECK(!rl_vm_changed_since(&vm, gen, 0x30000, 0x8000));
    CHECK(!rl_vm_changed_since(&vm, gen, 0x10000, 0x10000));
    gen = vm.map_gen;
    for (uint32_t i = 0; i < RL_VM_CHANGES; i++)
        CHECK_EQ(rl_vm_set(&vm, 0x20000, 0x4000, (i & 1) ? RL_VM_R : RL_VM_RW, 5, RL_VM_KEEP), 0);
    CHECK(!rl_vm_changed_since(&vm, gen, 0x40000, 0x4000));
    CHECK_EQ(rl_vm_set(&vm, 0x20000, 0x4000, RL_VM_RW, 5, RL_VM_KEEP), 0);
    /* More changes than the ring holds: every range counts as changed. */
    CHECK(rl_vm_changed_since(&vm, gen, 0x40000, 0x4000));
    rl_vm_destroy(&vm);
}

/** @brief The map a kernel write races in vm_kernel_writes_keep_access_while_the_map_changes. */
typedef struct map_churn {
    rl_vm *vm;
    uint64_t at;
    uint64_t rounds;
    uint64_t done;
} map_churn;

/**
 * @brief Changes the tag of one page back and forth, which splits it from its
 * neighbor and merges it again, and counts each change in `done`.
 */
static void *churn_map(void *arg)
{
    map_churn *m = arg;
    for (uint64_t i = 0; i < m->rounds; i++) {
        rl_vm_set(m->vm, m->at, m->vm->host_page, RL_VM_RW, i & 1 ? 9 : 8, RL_VM_KEEP);
        __atomic_store_n(&m->done, i + 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

/**
 * The writer and the map change take turns: each map change is answered by a
 * batch of kernel writes, and the writer then waits for the next change. The
 * work is fixed by the round count, so the run takes the same number of lock
 * hand-offs however the host schedules the two threads, where a fixed count
 * of writes against an unbounded change loop ran for seconds to minutes
 * depending on how the host's lock is handed over.
 */
TEST(vm_kernel_writes_keep_access_while_the_map_changes)
{
    rl_vm vm;
    CHECK_EQ(boot(&vm), 0);
    uint64_t hp = vm.host_page;
    CHECK_EQ(rl_vm_set(&vm, 2 * hp, 2 * hp, RL_VM_RW, 8, RL_VM_FRESH), 0);
    for (uint64_t k = 0; k < 256; k++)
        CHECK_EQ(rl_vm_set(&vm, (8 + k) * hp, hp, RL_VM_RW, 16 + (k & 1), RL_VM_FRESH), 0);
    uint64_t target = (8 + 255) * hp;
    map_churn m = { &vm, 2 * hp, 20000, 0 };
    pthread_t t;
    CHECK_EQ(pthread_create(&t, NULL, churn_map, &m), 0);
    uint8_t bytes[64];
    memset(bytes, 0x5a, sizeof bytes);
    uint64_t seen = 0;
    while (seen < m.rounds) {
        for (int i = 0; i < 16; i++)
            rl_vm_kernel_write(&vm, target, bytes, sizeof bytes);
        uint64_t now = __atomic_load_n(&m.done, __ATOMIC_ACQUIRE);
        if (now == seen)
            sched_yield();
        seen = now;
    }
    pthread_join(t, NULL);
    CHECK_EQ(rl_vm_host_prot(&vm, target), PROT_READ | PROT_WRITE);
    CHECK(!t_write(target + 5, 1).faulted);
    rl_vm_destroy(&vm);
}

TEST(core_clock_uses_the_guest_rate)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    CHECK_EQ(rl_ns_to_ticks(1000000000ull), 40500000);
    CHECK_EQ(rl_ticks_to_ns(40500000), 1000000000ull);
    CHECK_EQ(rl_ns_to_ticks(1000), 40);
    uint64_t t0 = rl_clock_ns();
    uint64_t a = rl_ns_to_ticks(t0);
    rl_sleep_ns(10000000);
    uint64_t t1 = rl_clock_ns();
    uint64_t d = rl_ns_to_ticks(t1) - a;
    CHECK(d >= 405000);
    CHECK(d <= rl_ns_to_ticks(t1 - t0) + 1);
}

TEST(core_guest_byte_order)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    CHECK_EQ(rl_guest_order16(0x1122), 0x2211);
    CHECK_EQ(rl_guest_order32(0x11223344u), 0x44332211u);
    CHECK_EQ(rl_guest_order64(0x1122334455667788ull), 0x8877665544332211ull);
    rl_cpu_desc le = fake_cpu;
    le.big_endian = false;
    CHECK_EQ(rl_core_init(&fake_guest, &le), 0);
    CHECK_EQ(rl_guest_order32(0x11223344u), 0x11223344u);
}

static void thread_body(struct rl_thread *t)
{
    t->ctx->r[0] = t->ctx->init_mark + 1;
    t->entry = rl_thread_current() == t;
}

TEST(core_threads_carry_the_cpu_context)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    struct rl_thread *t = rl_thread_alloc();
    CHECK(t != NULL);
    CHECK_EQ((uintptr_t)t->ctx % fake_cpu.ctx_align, 0);
    CHECK_EQ(t->ctx->init_mark, 0x5EED);
    CHECK(t->ctx->k.thread == t);
    CHECK(rl_common(t->ctx) == &t->ctx->k);
    CHECK_EQ(t->ctx->k.pend.sp, RL_SP_NONE);
    CHECK_EQ(t->ctx->tail, 0);
    int binds = g_binds;
    CHECK_EQ(rl_thread_spawn(t, thread_body), 0);
    rl_thread_join(t);
    CHECK_EQ(g_binds, binds + 1);
    CHECK_EQ(t->ctx->r[0], 0x5EEE);
    CHECK_EQ(t->entry, 1);
    rl_thread_bind(t);
    CHECK_EQ(g_binds, binds + 2);
    rl_thread_bind(NULL);
    rl_thread_free(t);
}

static uint64_t fn_a(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->r[1] = 0xA;
    return RL_OK;
}

static uint64_t fn_b(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->r[1] = 0xB;
    return RL_OK;
}

static rl_unit unit_of(const rl_fn_entry *e, uint32_t n, uint32_t ctx_size)
{
    rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = ctx_size;
    u.fn_count = n;
    u.fns = e;
    u.name = "fake";
    return u;
}

TEST(core_dispatch_uses_the_code_granule)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    static const rl_fn_entry odd[] = { { 0x1003, 2, 0, fn_a } };
    rl_unit u = unit_of(odd, 1, sizeof(struct rl_ctx));
    const rl_unit *list[] = { &u };
    CHECK_EQ(rl_units_register(list, 1), -1);
    static const rl_fn_entry two[] = { { 0x1000, 2, 0, fn_a }, { 0x1002, 2, 0, fn_b } };
    u = unit_of(two, 2, 896);
    CHECK_EQ(rl_units_register(list, 1), -1);
    u = unit_of(two, 2, sizeof(struct rl_ctx));
    CHECK_EQ(rl_units_register(list, 1), 0);
    CHECK(rl_dispatch_find(0x1000) == fn_a);
    CHECK(rl_dispatch_find(0x1002) == fn_b);
    CHECK(rl_dispatch_find(0x1001) == NULL);
    CHECK(rl_dispatch_find(0x1004) == NULL);
    struct rl_thread *t = rl_thread_alloc();
    CHECK(t != NULL);
    CHECK_EQ(t->ctx->k.disp_lo, 0x1000);
    CHECK_EQ(t->ctx->k.disp_count, 2);
    CHECK(t->ctx->k.disp_table[(0x1002 - t->ctx->k.disp_lo) >> 1] == fn_b);
    CHECK_EQ(t->ctx->k.disp_table[1](t->ctx, rl_mem_base()), RL_OK);
    CHECK_EQ(t->ctx->r[1], 0xB);
    static const rl_fn_entry over[] = { { 0x1002, 2, RL_FN_OVERRIDE, fn_a }, { 0x1000, 2, 0, fn_b } };
    u = unit_of(over, 2, sizeof(struct rl_ctx));
    CHECK_EQ(rl_units_register(list, 1), 0);
    CHECK(rl_dispatch_find(0x1002) == fn_a);
    CHECK(rl_dispatch_find(0x1000) == fn_a);
    CHECK(rl_dispatch_find_ctx(t->ctx, 0x1000) == fn_a);
    rl_fn own[2] = {fn_b, NULL};
    rl_dispatch_own(t->ctx, own, 0x2000, 2);
    CHECK(t->ctx->k.disp_table == own);
    CHECK(rl_dispatch_find_ctx(t->ctx, 0x2000) == fn_b);
    CHECK(rl_dispatch_find_ctx(t->ctx, 0x2002) == NULL);
    CHECK(rl_dispatch_find_ctx(t->ctx, 0x2001) == NULL);
    CHECK(rl_dispatch_find_ctx(t->ctx, 0x1000) == NULL);
    CHECK(rl_dispatch_find(0x1000) == fn_a);
    rl_thread_free(t);
}

TEST(core_hot_units_fill_a_fixed_table)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    CHECK_EQ(rl_dispatch_reserve(0x1000, 0x1010), 0);
    static const rl_fn_entry base[] = { { 0x1000, 2, 0, fn_a } };
    rl_unit u = unit_of(base, 1, sizeof(struct rl_ctx));
    memset(u.image_id, 0x5A, sizeof u.image_id);
    const rl_unit *list[] = { &u };
    CHECK_EQ(rl_units_register(list, 1), 0);
    CHECK(rl_dispatch_image() != NULL);
    struct rl_thread *t = rl_thread_alloc();
    CHECK(t != NULL);
    rl_fn *table = t->ctx->k.disp_table;
    CHECK_EQ(t->ctx->k.disp_count, 8);
    char why[256];
    static const rl_fn_entry hot[] = { { 0x1000, 2, 0, fn_b }, { 0x100a, 2, 0, fn_b } };
    rl_unit h = unit_of(hot, 2, sizeof(struct rl_ctx));
    CHECK_EQ(rl_unit_add(&h, why, sizeof why), -1);
    CHECK(strstr(why, "different image set") != NULL);
    memcpy(h.image_id, u.image_id, sizeof h.image_id);
    h.ctx_size = 896;
    CHECK_EQ(rl_unit_add(&h, why, sizeof why), -1);
    CHECK(strstr(why, "ABI version") != NULL);
    static const rl_fn_entry outside[] = { { 0x1010, 2, 0, fn_b } };
    rl_unit o = h;
    o.ctx_size = sizeof(struct rl_ctx);
    o.fns = outside;
    o.fn_count = 1;
    CHECK_EQ(rl_unit_add(&o, why, sizeof why), -1);
    CHECK(strstr(why, "outside the code range") != NULL);
    h.ctx_size = sizeof(struct rl_ctx);
    CHECK_EQ(rl_unit_add(&h, why, sizeof why), 0);
    CHECK(rl_dispatch_find(0x1000) == fn_a);
    CHECK(rl_dispatch_find(0x100a) == fn_b);
    CHECK(t->ctx->k.disp_table == table);
    CHECK(table[5] == fn_b);
    rl_thread_free(t);
}

static uint64_t fn_first(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->r[1] = 0xF;
    return RL_OK;
}

TEST(core_code_maps_fill_every_slot_of_their_pages)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    CHECK_EQ(rl_dispatch_reserve(0x1000, 0x1000 + 2 * RL_CODE_PAGE_SLOTS), 0);
    static const rl_fn_entry hot[] = { { 0x1000, 2, 0, fn_a } };
    static const rl_fn_entry cold[] = { { 0x1000, 0, 0, fn_b }, { 0x1002, 4, RL_FN_FRAGMENT | RL_FN_RANGE, fn_b } };
    rl_unit h = unit_of(hot, 1, sizeof(struct rl_ctx));
    rl_unit cu = unit_of(cold, 2, sizeof(struct rl_ctx));
    const rl_unit *list[] = { &h, &cu };
    CHECK_EQ(rl_units_register(list, 2), 0);
    CHECK(rl_dispatch_find(0x1004) == fn_b);
    CHECK(rl_dispatch_find(0x1006) == NULL);
    static uint8_t pat[RL_CODE_PAGE_BYTES];
    pat[0] = RL_CODE_HOT | RL_CODE_COLD << 2 | RL_CODE_COLD << 4 | RL_CODE_COLD << 6;
    static const uint32_t pages[] = { 0 };
    rl_code_map m = { RL_CODE_MAP_VERSION, 1, 1, 0x1000, pages, pat, &h, &cu, "fake" };
    const rl_code_map *maps[] = { &m };
    CHECK_EQ(rl_code_maps_register(maps, 1, NULL), -1);
    pat[0] = RL_CODE_HOT | RL_CODE_HOT << 2;
    CHECK_EQ(rl_code_maps_register(maps, 1, NULL), -1);
    pat[0] = RL_CODE_COLD | RL_CODE_COLD << 2 | RL_CODE_COLD << 4;
    CHECK_EQ(rl_code_maps_register(maps, 1, NULL), -1);
    pat[0] = RL_CODE_HOT | RL_CODE_COLD << 2;
    CHECK_EQ(rl_code_maps_register(maps, 1, NULL), -1);
    m.pattern_count = 0;
    pat[0] = RL_CODE_HOT | RL_CODE_COLD << 2 | RL_CODE_COLD << 4;
    CHECK_EQ(rl_code_maps_register(maps, 1, NULL), -1);
    m.pattern_count = 1;
    CHECK_EQ(rl_code_maps_register(maps, 1, fn_first), 0);
    CHECK(rl_dispatch_find(0x1000) == fn_a);
    CHECK(rl_dispatch_find(0x1002) == fn_first);
    CHECK(rl_dispatch_find(0x1004) == fn_first);
    CHECK(rl_dispatch_find(0x1006) == NULL);
    CHECK(rl_dispatch_find(0x1000 + 2 * (RL_CODE_PAGE_SLOTS - 1)) == NULL);
    CHECK_EQ(rl_code_class(0x1006, NULL), RL_CODE_TRAP);
    CHECK_EQ(rl_code_class(0x1002, NULL), RL_CODE_COLD);
    CHECK_EQ(rl_code_class(0x1000 + 2 * RL_CODE_PAGE_SLOTS, NULL), -1);
    CHECK(rl_core_cold(0x1006) == NULL);
    CHECK(rl_core_cold(0x1002) == fn_b);
    CHECK(rl_dispatch_find(0x1002) == fn_b);
    CHECK(rl_core_cold(0x1002) == fn_b);
    CHECK(rl_dispatch_find(0x1004) == fn_first);
}

TEST(core_dev_miss_without_a_server_says_why)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    struct rl_thread *t = rl_thread_alloc();
    CHECK(t != NULL);
    int fds[2];
    char out[2048];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    rl_miss_info m = { 0x4444, RL_SITE_NONE, RL_MISS_CALL, 0 };
    rl_fn f = rl_core_miss_load(t->ctx, &m, 0);
    capture_end(fds, saved, out, sizeof out);
    CHECK(f == NULL);
#if defined(RL_HOTLOAD)
    CHECK(strstr(out, "hotload") != NULL);
#else
    CHECK_EQ(out[0], 0);
#endif
    rl_thread_free(t);
}

TEST(core_pending_and_fatal_reports)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    struct rl_thread *t = rl_thread_alloc();
    CHECK(t != NULL);
    rl_ctx *c = t->ctx;
    CHECK_EQ(rl_pend_exit_thread(c), RL_PENDING);
    CHECK_EQ(c->k.pend.kind, RL_PEND_EXIT_THREAD);
    c->r[15] = 0x1234;
    int fds[2];
    static char out[32768];
    char dir[512];
    report_dir(dir, sizeof dir);
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int a = rl_guest_fatal(c, "test", "bad thing %d", 7);
    int b = rl_core_miss(c, rl_mem_base(), 0x4444, 0);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(a, RL_PENDING);
    CHECK_EQ(b, RL_PENDING);
    CHECK_EQ(c->k.pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->k.pend.code, RL_EXIT_SOFTWARE);
    CHECK_EQ(c->k.pend.sp, RL_SP_NONE);
    CHECK_EQ(count_of(out, "kind: test\nsummary: bad thing 7\n"), 1);
    CHECK_EQ(count_of(out, "kind: missed code\nsummary: no function at guest 0x4444\n"), 1);
    CHECK_EQ(count_of(out, "transfer: entry\ntarget: 0x4444\n"), 1);
    CHECK_EQ(count_of(out, "  r15 0x1234\n"), 2);
    CHECK_EQ(count_of(out, "end of report\n"), 2);
    report_dir_done(dir);
    rl_report_reset();
    rl_thread_free(t);
}

TEST(core_unknown_calls_are_reported_once)
{
    int fds[2];
    char out[2048];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    bool s1 = rl_report_unknown(1, "os", "unknown call %d", 1);
    rl_report_unknown(1, "os", "unknown call %d", 1);
    rl_report_unknown(2, "os", "unknown call %d", 2);
    rl_cfg()->strict = true;
    bool s2 = rl_report_unknown(1, "os", "unknown call %d", 1);
    capture_end(fds, saved, out, sizeof out);
    CHECK(!s1);
    CHECK(s2);
    CHECK_EQ(count_of(out, "unknown call 1"), 1);
    CHECK_EQ(count_of(out, "unknown call 2"), 1);
    CHECK_EQ(count_of(out, "[relift warn os]"), 2);
}

TEST(core_config_names_no_guest)
{
    setenv("RELIFT_STRICT", "1", 1);
    setenv("RELIFT_TRACE_OS", "1", 1);
    setenv("RELIFT_ADDRESS_BITS", "32", 1);
    rl_config_load();
    CHECK(rl_cfg()->strict);
    CHECK(rl_cfg()->trace_os);
    CHECK_EQ(rl_cfg()->address_bits, 32);
    unsetenv("RELIFT_STRICT");
    unsetenv("RELIFT_TRACE_OS");
    unsetenv("RELIFT_ADDRESS_BITS");
    rl_config_load();
    CHECK(!rl_cfg()->strict);
    CHECK(!rl_cfg()->trace_os);
    CHECK_EQ(rl_cfg()->address_bits, 0);
}

static uint8_t g_peek_bytes[16] = { 0x1f, 0x20, 0x03, 0xd5, 0xc0, 0x03, 0x5f, 0xd6 };

static bool fake_peek(uint64_t a, void *out, size_t n)
{
    if (a < 0x1800 || a + n > 0x1800 + sizeof g_peek_bytes)
        return false;
    memcpy(out, g_peek_bytes + (a - 0x1800), n);
    return true;
}

static void fake_where(uint64_t a, char *buf, size_t n) { snprintf(buf, n, "fake segment at 0x%llx", (unsigned long long)a); }

static const rl_report_guest fake_report_guest = { fake_peek, fake_where, 4, NULL };

static const rl_fn_info report_fns[] = {
    { 0x1000, 0x1000, 0x20, 0, fn_a, "_ZN5guest4fake1aEv", "guest::fake::a()" },
};
static const rl_fn_map report_map = { RL_FN_MAP_VERSION, 1, report_fns, "fake", "shard0000", 0x1000, NULL, NULL };

/** @brief Reads the whole file at path into buf (NUL terminated); returns its length or -1. */
static long read_all(const char *path, char *buf, size_t n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    size_t k = fread(buf, 1, n - 1, f);
    fclose(f);
    buf[k] = 0;
    return (long)k;
}

TEST(core_stop_report_names_the_jump_site_and_the_target)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    rl_fn_maps_reset();
    const rl_fn_map *maps[] = { &report_map };
    CHECK_EQ(rl_fn_maps_register(maps, 1), 0);
    rl_report_reset();
    rl_report_info("relift", "0.0.0-test");
    rl_report_module("fake", 0x1000, 0x1000, "00112233");
    rl_report_set_guest(&fake_report_guest);
    struct rl_thread *t = rl_thread_alloc();
    CHECK(t != NULL);
    rl_ctx *c = t->ctx;
    c->r[15] = 0x1234;
    char dir[512], path[1400];
    report_dir(dir, sizeof dir);
    CHECK(dir[0] != 0);
    rl_miss_info m = { 0x1800, 0x1008, RL_MISS_JUMP, 0 };
    int fds[2];
    static char out[65536], file[65536];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    CHECK_EQ(rl_report_stop(c, "missed code", "no function at guest 0x1800", &m, path, sizeof path), 0);
    capture_end(fds, saved, out, sizeof out);
    CHECK(strncmp(path, dir, strlen(dir)) == 0);
    CHECK(read_all(path, file, sizeof file) > 0);
    CHECK(strstr(out, file) != NULL);
    CHECK(strstr(out, "[relift error stop] missed code: no function at guest 0x1800; report written to ") != NULL);
    CHECK_EQ(strncmp(file, "relift stop report 1\nkind: missed code\nsummary: no function at guest 0x1800\n"
                           "relift: 0.0.0-test\nmodule: fake base 0x1000 size 0x1000 build 00112233\n",
                     strlen("relift stop report 1\nkind: missed code\nsummary: no function at guest 0x1800\n"
                            "relift: 0.0.0-test\nmodule: fake base 0x1000 size 0x1000 build 00112233\n")),
             0);
    CHECK_EQ(count_of(file, "\ntransfer: jump\ntarget: 0x1800 fake+0x800\n"), 1);
    CHECK_EQ(count_of(file, "\ntarget code: not lifted (outside every lifted function)\n"), 1);
    CHECK_EQ(count_of(file, "\ntarget where: fake segment at 0x1800\n"), 1);
    CHECK_EQ(count_of(file, "\ntarget words 0x1800: d503201f d65f03c0 00000000 00000000\n"), 1);
    CHECK_EQ(count_of(file, "\nsite: 0x1008 fake+0x8\nsite function: 0x1000 fake+0x0 guest::fake::a() +0x8\n"), 1);
    CHECK_EQ(count_of(file, "\nsite words: unreadable\n"), 1);
    CHECK_EQ(count_of(file, "\nregisters:\n  r15 0x1234\n"), 1);
    CHECK_EQ(count_of(file, "\nhost backtrace:\n  #0 "), 1);
    CHECK_EQ(count_of(file, "\nend of report\n"), 1);
    report_dir_done(dir);
    rl_report_reset();
    rl_fn_maps_reset();
    rl_thread_free(t);
}

TEST(core_stop_reports_of_one_second_get_files_of_their_own)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    rl_report_reset();
    struct rl_thread *t = rl_thread_alloc();
    CHECK(t != NULL);
    char dir[512], a[1400], b[1400], c2[1400];
    report_dir(dir, sizeof dir);
    int fds[2];
    static char out[32768];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    CHECK_EQ(rl_report_stop(t->ctx, "test", "one", NULL, a, sizeof a), 0);
    CHECK_EQ(rl_report_stop(t->ctx, "test", "two", NULL, b, sizeof b), 0);
    CHECK_EQ(rl_report_stop(t->ctx, "test", "three", NULL, c2, sizeof c2), 0);
    capture_end(fds, saved, out, sizeof out);
    CHECK(strcmp(a, b) != 0);
    CHECK(strcmp(b, c2) != 0);
    CHECK(strcmp(a, c2) != 0);
    static char text[32768];
    CHECK(read_all(a, text, sizeof text) > 0);
    CHECK(strstr(text, "\nsummary: one\n") != NULL);
    CHECK(read_all(b, text, sizeof text) > 0);
    CHECK(strstr(text, "\nsummary: two\n") != NULL);
    report_dir_done(dir);
    rl_report_reset();
    rl_thread_free(t);
}

TEST(core_a_host_fault_report_goes_to_a_file_too)
{
    char dir[512];
    report_dir(dir, sizeof dir);
    CHECK(dir[0] != 0);
    rl_crash_info ci;
    memset(&ci, 0, sizeof ci);
    ci.kind = "SIGSEGV";
    ci.pc = 0x1234;
    int fds[2];
    static char out[32768];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    rl_crash_report(&ci);
    capture_end(fds, saved, out, sizeof out);
    CHECK(strstr(out, "[relift crash] SIGSEGV") != NULL);
    DIR *d = opendir(dir);
    CHECK(d != NULL);
    int files = 0;
    char path[1024] = "";
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL)
        if (!strncmp(e->d_name, "relift-crash-", 13)) {
            files++;
            snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        }
    if (d)
        closedir(d);
    CHECK_EQ(files, 1);
    static char text[32768];
    CHECK(read_all(path, text, sizeof text) > 0);
    CHECK(strstr(text, "[relift crash] SIGSEGV") != NULL);
    report_dir_done(dir);
    rl_crash_set_file(NULL);
}

TEST(core_stop_report_takes_a_call_site_from_the_cpu)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    rl_fn_maps_reset();
    const rl_fn_map *maps[] = { &report_map };
    CHECK_EQ(rl_fn_maps_register(maps, 1), 0);
    rl_report_reset();
    rl_report_module("fake", 0x1000, 0x1000, NULL);
    struct rl_thread *t = rl_thread_alloc();
    CHECK(t != NULL);
    rl_ctx *c = t->ctx;
    c->r[14] = 0x100c;
    rl_miss_info e = rl_core_miss_classify(c, 0x1800, 0);
    CHECK_EQ(e.reason, RL_MISS_ENTRY);
    CHECK_EQ(e.site, RL_SITE_NONE);
    rl_miss_info m = rl_core_miss_classify(c, 0x1800, (uintptr_t)fn_a + 1);
    CHECK_EQ(m.reason, RL_MISS_CALL);
    CHECK_EQ(m.site, 0x100a);
    char dir[512];
    report_dir(dir, sizeof dir);
    int fds[2];
    static char out[65536];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    CHECK_EQ(rl_core_miss_at(c, rl_mem_base(), &m), RL_PENDING);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(count_of(out, "\ntransfer: call\ntarget: 0x1800 fake+0x800\n"), 1);
    CHECK_EQ(count_of(out, "\nsite: 0x100a fake+0xa\nsite function: 0x1000 fake+0x0 guest::fake::a() +0xa\n"), 1);
    CHECK_EQ(count_of(out, "\ncaller function: 0x1000 fake+0x0 guest::fake::a() (host _ZN5guest4fake1aEv+0x1)\n"), 1);
    CHECK_EQ(count_of(out, "module: fake base 0x1000 size 0x1000 build -\n"), 1);
    CHECK_EQ(c->k.pend.kind, RL_PEND_EXIT_PROCESS);
    report_dir_done(dir);
    rl_report_reset();
    rl_fn_maps_reset();
    rl_thread_free(t);
}

static void *fake_assets(const char *name, size_t *len)
{
    if (strcmp(name, "packaged/in-an-apk.bin") != 0)
        return NULL;
    *len = 3;
    return memcpy(malloc(3), "apk", 3);
}

TEST(core_finds_shipped_files_by_the_executable_not_the_working_directory)
{
    char exe[1024], dir[1100], file[1200], cwd[1024];
    CHECK(rl_exe_dir(exe, sizeof exe));
    snprintf(dir, sizeof dir, "%s/assets-%d", exe, (int)getpid());
    snprintf(file, sizeof file, "%s/found.bin", dir);
    CHECK_EQ(mkdir(dir, 0755), 0);
    FILE *f = fopen(file, "wb");
    CHECK(f != NULL);
    fwrite("beside", 1, 6, f);
    fclose(f);
    CHECK(getcwd(cwd, sizeof cwd) != NULL);
    CHECK_EQ(chdir("/"), 0);

    char name[64];
    snprintf(name, sizeof name, "assets-%d/found.bin", (int)getpid());
    size_t len = 0;
    char *data = rl_asset_load(name, &len);
    CHECK(data != NULL);
    CHECK_EQ(len, 6);
    CHECK(data && memcmp(data, "beside", 6) == 0);
    free(data);
    CHECK(rl_asset_load("assets-missing/none.bin", &len) == NULL);

    rl_set_asset_loader(fake_assets);
    data = rl_asset_load("packaged/in-an-apk.bin", &len);
    CHECK(data != NULL);
    CHECK_EQ(len, 3);
    CHECK(data && memcmp(data, "apk", 3) == 0);
    free(data);
    rl_set_asset_loader(NULL);
    CHECK(rl_asset_load("packaged/in-an-apk.bin", &len) == NULL);

    CHECK_EQ(chdir(cwd), 0);
    CHECK_EQ(unlink(file), 0);
    CHECK_EQ(rmdir(dir), 0);
}

static unsigned g_at_exit_runs, g_at_exit_hooked;

static void count_at_exit(void) { g_at_exit_runs++; }

static void hook_after_at_exit(uint32_t code)
{
    (void)code;
    g_at_exit_hooked = g_at_exit_runs;
}

TEST(core_the_process_exit_runs_what_layers_added_once_before_it_ends)
{
    CHECK(rl_at_process_exit(count_at_exit));
    rl_set_exit_hook(hook_after_at_exit);
    rl_process_exit(0);
    CHECK_EQ(g_at_exit_runs, 1u);
    CHECK_EQ(g_at_exit_hooked, 1u);
    rl_process_exit(0);
    CHECK_EQ(g_at_exit_runs, 1u);
    rl_set_exit_hook(NULL);
}

/** @brief Two lifted functions on guest page 2 of a boot() space, registered, and the page made FILE code. */
static int cw_boot(rl_vm *vm, uint64_t *page)
{
    if (boot(vm) != 0 || rl_cw_init(vm) != 0)
        return -1;
    uint64_t gp = vm->guest_page;
    *page = 2 * vm->host_page;
    static rl_fn_entry fns[2];
    fns[0] = (rl_fn_entry){ *page, 2, 0, fn_a };
    fns[1] = (rl_fn_entry){ *page + gp - 2, 2, 0, fn_b };
    static rl_unit u;
    u = unit_of(fns, 2, sizeof(struct rl_ctx));
    const rl_unit *list[] = { &u };
    if (rl_units_register(list, 1) != 0)
        return -1;
    if (rl_vm_set(vm, *page, vm->host_page, RL_VM_RX, 0, RL_VM_FRESH) != 0)
        return -1;
    return rl_cw_set(*page, vm->host_page, RL_CW_FILE);
}

TEST(codewatch_a_write_to_lifted_code_drops_its_dispatch)
{
    rl_vm vm;
    uint64_t pg;
    CHECK_EQ(cw_boot(&vm, &pg), 0);
    rl_cw_page q = rl_cw_query(pg);
    CHECK_EQ(q.kind, RL_CW_FILE);
    CHECK(q.pristine);
    CHECK(q.watched);
    CHECK(t_write(pg + 4, 1).faulted);
    CHECK_EQ(rl_vm_set(&vm, pg, vm.host_page, RL_VM_RX | RL_VM_W, 0, RL_VM_KEEP), 0);
    CHECK_EQ(rl_vm_host_prot(&vm, pg), PROT_READ);
    CHECK(rl_dispatch_find(pg) == fn_a);
    CHECK(!t_write(pg + 4, 0x77).faulted);
    CHECK_EQ(t_mem()[pg + 4], 0x77);
    q = rl_cw_query(pg);
    CHECK(!q.pristine);
    CHECK(!q.watched);
    CHECK_EQ(q.gen, 1);
    CHECK(rl_dispatch_find(pg) == NULL);
    CHECK(rl_dispatch_find(pg + vm.guest_page - 2) == NULL);
    CHECK_EQ(rl_vm_host_prot(&vm, pg), PROT_READ | PROT_WRITE);
    CHECK_EQ(rl_cw_stats_get().faults, 1);
    CHECK(!t_write(pg + 5, 0x78).faulted);
    CHECK_EQ(rl_cw_stats_get().faults, 1);
    CHECK_EQ(rl_cw_set(pg, vm.host_page, RL_CW_FILE), 0);
    CHECK(rl_dispatch_find(pg) == fn_a);
    CHECK(rl_cw_query(pg).pristine);
    CHECK_EQ(rl_vm_host_prot(&vm, pg), PROT_READ);
    rl_vm_destroy(&vm);
}

TEST(codewatch_run_pages_fault_only_while_watched)
{
    rl_vm vm;
    uint64_t pg;
    CHECK_EQ(cw_boot(&vm, &pg), 0);
    uint64_t run = 6 * vm.host_page;
    CHECK_EQ(rl_vm_set(&vm, run, vm.host_page, RL_VM_RW | RL_VM_X, 0, RL_VM_FRESH), 0);
    CHECK_EQ(rl_cw_set(run, vm.host_page, RL_CW_RUN), 0);
    CHECK(!rl_cw_query(run).watched);
    CHECK(!t_write(run, 1).faulted);
    CHECK_EQ(rl_cw_watch(run), 0);
    CHECK(rl_cw_query(run).watched);
    CHECK_EQ(rl_vm_host_prot(&vm, run), PROT_READ);
    uint32_t g = rl_cw_query(run).gen;
    CHECK(!t_write(run + 9, 2).faulted);
    CHECK_EQ(rl_cw_query(run).gen, g + 1);
    CHECK_EQ(rl_vm_host_prot(&vm, run), PROT_READ | PROT_WRITE);
    CHECK(rl_dispatch_find(pg) == fn_a);
    for (uint32_t i = 0; i < RL_CW_CHECKED_FAULTS; i++) {
        CHECK_EQ(rl_cw_watch(run), 0);
        CHECK(!t_write(run + 9, (uint8_t)i).faulted);
    }
    CHECK(rl_cw_checked(run));
    CHECK_EQ(rl_cw_watch(run), 0);
    CHECK(!rl_cw_query(run).watched);
    CHECK_EQ(rl_cw_watch(4 * vm.host_page), -1);
    CHECK_EQ(rl_cw_set(run, vm.host_page, RL_CW_NONE), 0);
    CHECK_EQ(rl_cw_query(run).kind, RL_CW_NONE);
    CHECK(!rl_cw_checked(run));
    rl_vm_destroy(&vm);
}

TEST(codewatch_a_read_only_code_page_still_faults_for_the_guest)
{
    rl_vm vm;
    uint64_t pg;
    CHECK_EQ(cw_boot(&vm, &pg), 0);
    t_probe p = t_write(pg + 2, 1);
    CHECK(p.faulted);
    CHECK_EQ(p.addr, pg + 2);
    CHECK(rl_cw_query(pg).pristine);
    CHECK_EQ(rl_cw_stats_get().faults, 0);
    rl_vm_destroy(&vm);
}

TEST(codewatch_kernel_writes_and_remaps_end_the_code)
{
    rl_vm vm;
    uint64_t pg;
    CHECK_EQ(cw_boot(&vm, &pg), 0);
    uint8_t b[4] = { 1, 2, 3, 4 };
    rl_vm_kernel_write(&vm, pg + 8, b, sizeof b);
    CHECK_EQ(t_mem()[pg + 11], 4);
    CHECK(!rl_cw_query(pg).pristine);
    CHECK(rl_dispatch_find(pg) == NULL);
    CHECK_EQ(rl_vm_host_prot(&vm, pg), PROT_READ);
    CHECK_EQ(rl_cw_set(pg, vm.host_page, RL_CW_FILE), 0);
    CHECK_EQ(rl_vm_set(&vm, pg, vm.host_page, RL_VM_NONE, 0, RL_VM_DISCARD), 0);
    CHECK(!rl_cw_query(pg).pristine);
    CHECK_EQ(rl_vm_set(&vm, pg, vm.host_page, RL_VM_RW, 0, RL_VM_FRESH), 0);
    CHECK_EQ(rl_vm_host_prot(&vm, pg), PROT_READ | PROT_WRITE);
    CHECK_EQ(t_mem()[pg + 11], 0);
    CHECK_EQ(rl_cw_set(pg, vm.host_page, RL_CW_FILE), 0);
    CHECK_EQ(rl_vm_set(&vm, pg, vm.host_page, RL_VM_RW | RL_VM_X, 0, RL_VM_KEEP), 0);
    /* A write into a host page ends the code of all its guest pages (the fault path does the same for the whole host page). */
    rl_cw_will_write(pg, vm.host_page);
    CHECK_EQ(rl_vm_host_prot(&vm, pg), PROT_READ | PROT_WRITE);
    CHECK(rl_dispatch_find(pg) == NULL);
    rl_vm_destroy(&vm);
}

/** @brief A thread that watches a code page over and over while the test writes it. */
typedef struct cw_race {
    uint64_t page;
    uint64_t rounds;
    int stop;
} cw_race;

static void *cw_rewatch(void *arg)
{
    cw_race *r = arg;
    while (!__atomic_load_n(&r->stop, __ATOMIC_ACQUIRE)) {
        rl_cw_watch(r->page);
        __atomic_add_fetch(&r->rounds, 1, __ATOMIC_RELEASE);
    }
    return NULL;
}

/**
 * The fault hook's unwatch and another thread's watch race; once both stop, the host page has write access exactly
 * when the page is not watched, so no later write can land on watched code unseen.
 */
TEST(codewatch_watch_races_writes_and_ends_consistent)
{
    rl_vm vm;
    uint64_t pg;
    CHECK_EQ(cw_boot(&vm, &pg), 0);
    uint64_t run = 6 * vm.host_page;
    CHECK_EQ(rl_vm_set(&vm, run, vm.host_page, RL_VM_RW | RL_VM_X, 0, RL_VM_FRESH), 0);
    uint64_t rounds = 0;
    for (int k = 0; k < 40; k++) {
        CHECK_EQ(rl_cw_set(run, vm.host_page, RL_CW_RUN), 0);
        cw_race r = { run, 0, 0 };
        pthread_t t;
        CHECK_EQ(pthread_create(&t, NULL, cw_rewatch, &r), 0);
        /* At least 400 writes, and until the other thread has run once (it may not be scheduled for a while on a loaded
         * machine); bounded, so a thread that never runs still ends the loop. */
        for (int i = 0; i < 400 || (i < 100000000 && __atomic_load_n(&r.rounds, __ATOMIC_ACQUIRE) == 0); i++)
            CHECK(!t_write(run + 1, (uint8_t)i).faulted);
        __atomic_store_n(&r.stop, 1, __ATOMIC_RELEASE);
        pthread_join(t, NULL);
        rounds += r.rounds;
        bool watched = rl_cw_query(run).watched;
        CHECK_EQ(rl_vm_host_prot(&vm, run), watched ? PROT_READ : PROT_READ | PROT_WRITE);
        uint32_t g = rl_cw_query(run).gen;
        CHECK(!t_write(run + 2, 1).faulted);
        CHECK(!watched || rl_cw_query(run).gen == g + 1);
    }
    CHECK(rounds > 0);
    rl_vm_destroy(&vm);
}
