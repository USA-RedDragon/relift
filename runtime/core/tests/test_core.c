/**
 * @file test_core.c
 * @brief The core alone, driven through a fake guest and a fake CPU that
 * resemble no real layer: 8 KiB guest pages, a 32-bit address space, a 40.5
 * MHz clock, big-endian data, a 2-byte code granule and a small context
 * whose common block is not at the end. This binary links only the core.
 */
#include "runner.h"

#include <stdlib.h>
#include <sys/mman.h>
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
    ssize_t got = read(fds[0], buf, n - 1);
    buf[got > 0 ? got : 0] = 0;
    close(fds[0]);
}

static int count_of(const char *hay, const char *needle)
{
    int n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle))
        n++;
    return n;
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

TEST(core_clock_uses_the_guest_rate)
{
    CHECK_EQ(rl_core_init(&fake_guest, &fake_cpu), 0);
    CHECK_EQ(rl_ns_to_ticks(1000000000ull), 40500000);
    CHECK_EQ(rl_ticks_to_ns(40500000), 1000000000ull);
    CHECK_EQ(rl_ns_to_ticks(1000), 40);
    uint64_t a = rl_ns_to_ticks(rl_clock_ns());
    rl_sleep_ns(10000000);
    uint64_t d = rl_ns_to_ticks(rl_clock_ns()) - a;
    CHECK(d >= 405000);
    CHECK(d < 40500000);
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

static int fn_a(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->r[1] = 0xA;
    return RL_OK;
}

static int fn_b(rl_ctx *c, uint8_t *mem)
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
    char out[2048];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int a = rl_guest_fatal(c, "test", "bad thing %d", 7);
    int b = rl_core_miss(c, rl_mem_base(), 0x4444);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(a, RL_PENDING);
    CHECK_EQ(b, RL_PENDING);
    CHECK_EQ(c->k.pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->k.pend.code, RL_EXIT_SOFTWARE);
    CHECK_EQ(c->k.pend.sp, RL_SP_NONE);
    CHECK_EQ(count_of(out, "bad thing 7 (r15 0x1234)"), 1);
    CHECK_EQ(count_of(out, "missed code: no function at guest 0x4444 (r15 0x1234)"), 1);
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
