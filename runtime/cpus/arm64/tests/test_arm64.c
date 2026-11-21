/**
 * @file test_arm64.c
 * @brief The A64 CPU layer over the core with a test guest that is no real
 * OS (16 KiB pages, 40 address bits, a 1 MHz clock). This binary links the
 * core and the A64 layer only, so nothing here can lean on a guest OS.
 */
#include "arm64.h"
#include "runner.h"

#include <stdlib.h>
#include <unistd.h>

static const rl_guest_desc test_guest = {
    .name = "arm64-test",
    .page_size = 0x4000,
    .address_bits = 40,
    .tick_hz = 1000000,
};

static rl_ctx *boot(void)
{
    if (rl_core_init(&test_guest, rl_arm64_cpu()) != 0)
        return NULL;
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return NULL;
    rl_thread_bind(t);
    return t->ctx;
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

TEST(arm64_descriptor_matches_the_context)
{
    const rl_cpu_desc *d = rl_arm64_cpu();
    CHECK_EQ(d->ctx_size, sizeof(rl_ctx));
    CHECK_EQ(d->ctx_size, 960);
    CHECK_EQ(d->time_offset, offsetof(rl_ctx, time));
    CHECK_EQ(d->ctx_align, 64);
    CHECK_EQ(d->common_offset, offsetof(rl_ctx, pend));
    CHECK_EQ(d->code_shift, 2);
    CHECK(!d->big_endian);
    CHECK_EQ(rl_core_init(&test_guest, d), 0);
    CHECK_EQ(rl_guest_order32(0x11223344u), 0x11223344u);
}

TEST(arm64_context_defaults)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ((uintptr_t)c % 64, 0);
    CHECK_EQ(c->fpcr, RL_FPCR_DEFAULT);
    CHECK_EQ(c->pend.sp, RL_SP_NONE);
    CHECK(c->thread == rl_thread_current());
    CHECK(&c->pend == &rl_common(c)->pend);
    CHECK_EQ(rl_pend_exit_thread(c), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_THREAD);
}

TEST(arm64_fpcr_follows_the_thread)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    rl_msr_fpcr(c, RL_FPCR_FZ);
    CHECK_EQ(rl_mrs_fpcr(c), RL_FPCR_FZ);
#if defined(__aarch64__)
    CHECK_EQ(rl_host_get_fpcr(), RL_FPCR_FZ);
#endif
    rl_msr_fpcr(c, 0);
    CHECK_EQ(rl_host_get_fpcr(), 0);
}

TEST(arm64_cntpct_counts_guest_ticks)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint64_t t0 = rl_clock_ns();
    uint64_t a = rl_mrs_cntpct(c);
    rl_sleep_ns(10000000);
    uint64_t d = rl_mrs_cntpct(c) - a;
    uint64_t t1 = rl_clock_ns();
    CHECK(d >= 10000);
    CHECK(d <= rl_ns_to_ticks(t1 - t0) + 1);
}

TEST(arm64_undefined_names_the_instruction)
{
    char buf[32];
    CHECK(!strcmp(rl_arm64_insn_kind(0x00000005u, buf, sizeof buf), "UDF #5"));
    CHECK(!strcmp(rl_arm64_insn_kind(0xD4207D00u, buf, sizeof buf), "BRK #1000"));
    CHECK(!strcmp(rl_arm64_insn_kind(0xD4400000u, buf, sizeof buf), "HLT #0"));
    CHECK(!strcmp(rl_arm64_insn_kind(0x8B020020u, buf, sizeof buf), "unliftable"));
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->x[30] = 0x1230;
    c->sp = 0x8000;
    int fds[2];
    static char out[32768];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_undefined(c, rl_mem_base(), 0x4000, 0xD4207D00u);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->pend.code, RL_EXIT_SOFTWARE);
    CHECK(strstr(out, "kind: undefined instruction\nsummary: undefined instruction 0xd4207d00 (BRK #1000) at guest pc "
                      "0x4000\n") != NULL);
    CHECK(strstr(out, "\ntransfer: none\nat: 0x4000") != NULL);
    CHECK(strstr(out, "x30 0000000000001230\n") != NULL);
    CHECK(strstr(out, "sp  0000000000008000  pc  ") != NULL);
}

TEST(arm64_brk_reports_a_breakpoint)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->x[30] = 0x1230;
    c->sp = 0x8000;
    int fds[2];
    static char out[32768];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_brk(c, rl_mem_base(), 1000);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "kind: breakpoint\nsummary: breakpoint BRK #1000\n") != NULL);
    CHECK(strstr(out, "x30 0000000000001230\n") != NULL);
}

static _Alignas(16) uint8_t g_unw_mem[0x4000];

TEST(arm64_unw_jumpto_installs_a_libunwind_register_block)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint8_t *mem = g_unw_mem;
    const uint64_t b = 0x3000;
    for (int i = 0; i < 30; i++)
        rl_st64(mem, b + 8u * (uint64_t)i, 0x100u + (unsigned)i);
    rl_st64(mem, b + 0xf0, 0xBAD);
    rl_st64(mem, b + 0xf8, 0x7FF0);
    rl_st64(mem, b + 0x100, 0x4444);
    for (int i = 0; i < 32; i++)
        rl_st64(mem, b + 0x110 + 8u * (uint64_t)i, 0xD00u + (unsigned)i);
    c->x[0] = b;
    c->v[3].d[1] = 0xFEED;
    CHECK_EQ(rl_arm64_unw_jumpto(c, mem), RL_PENDING);
    CHECK_EQ(c->x[0], 0x100);
    CHECK_EQ(c->x[1], 0x101);
    CHECK_EQ(c->x[29], 0x11D);
    CHECK_EQ(c->x[30], 0x4444);
    CHECK_EQ(c->sp, 0x7FF0);
    CHECK_EQ(c->v[0].d[0], 0xD00);
    CHECK_EQ(c->v[9].d[0], 0xD09);
    CHECK_EQ(c->v[31].d[0], 0xD1F);
    CHECK_EQ(c->v[3].d[1], 0);
    CHECK_EQ(c->pend.kind, RL_PEND_UNWIND);
    CHECK_EQ(c->pend.pc, 0x4444);
    CHECK_EQ(c->pend.sp, 0x7FF0);
}

TEST(arm64_resume_installs_registers)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    rl_resume_regs r;
    memset(&r, 0, sizeof r);
    for (int i = 0; i < 31; i++)
        r.x[i] = 0x100u + (unsigned)i;
    for (int i = 0; i < 8; i++)
        r.d8_15[i] = 0xD00u + (unsigned)i;
    r.sp = 0x7FF0;
    r.pc = 0x4444;
    c->v[9].d[1] = 0xFEED;
    CHECK_EQ(rl_resume(c, rl_mem_base(), &r), RL_PENDING);
    CHECK_EQ(c->x[30], 0x11E);
    CHECK_EQ(c->sp, 0x7FF0);
    CHECK_EQ(c->v[9].d[0], 0xD01);
    CHECK_EQ(c->v[9].d[1], 0xFEED);
    CHECK_EQ(c->pend.kind, RL_PEND_UNWIND);
    CHECK_EQ(c->pend.pc, 0x4444);
    CHECK_EQ(c->pend.sp, 0x7FF0);
}

static uint64_t entry_fn(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 0x600D;
    return RL_OK;
}

static uint64_t jumps_missing(rl_ctx *c, uint8_t *mem) { RL_JUMP(0x10002, UINT64_C(0x10004)); }

static uint64_t calls_missing(rl_ctx *c, uint8_t *mem)
{
    c->x[30] = 0x1000c;
    uint64_t rc = rl_indirect(c, 0x10002)(c, mem);
    if (rc)
        return rc;
    c->x[1] = 1;
    return RL_OK;
}

TEST(arm64_dispatch_through_lookup)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    static const rl_fn_entry e[] = { { 0x10000, 4, 0, entry_fn }, { 0x10004, 4, 0, jumps_missing },
                                     { 0x10008, 8, 0, calls_missing } };
    static const rl_fn_info fi[] = { { 0x10000, 0x10000, 4, 0, entry_fn, "entry_fn", NULL },
                                     { 0x10004, 0x10004, 4, 0, jumps_missing, "jumps_missing", NULL },
                                     { 0x10008, 0x10008, 8, 0, calls_missing, "calls_missing", NULL } };
    static const rl_fn_map fm = { RL_FN_MAP_VERSION, 3, fi, "test", "shard0000", 0x10000, NULL, NULL };
    static const rl_fn_map *const fms[] = { &fm };
    rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(rl_ctx);
    u.fn_count = 3;
    u.fns = e;
    u.name = "arm64-test";
    u.map_count = 1;
    u.maps = fms;
    const rl_unit *list[] = { &u };
    CHECK_EQ(rl_units_register(list, 1), 0);
    rl_dispatch_view(c);
    CHECK(rl_lookup(c, 0x10000) == entry_fn);
    CHECK(rl_lookup(c, 0x10002) == rl_dispatch_miss);
    CHECK(rl_lookup(c, 0x0FFFC) == rl_dispatch_miss);
    CHECK(rl_jump_find(c, 0x10002) == NULL);
    CHECK_EQ(rl_call_guest(c, rl_mem_base(), 0x10000), RL_OK);
    CHECK_EQ(c->x[0], 0x600D);
    int fds[2];
    static char out[32768];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_call_guest(c, rl_mem_base(), 0x10004);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->pend.pc, 0);
    CHECK(strstr(out, "kind: missed code\nsummary: no function at guest 0x10002\n") != NULL);
    CHECK(strstr(out, "\ntransfer: jump\ntarget: 0x10002 test+0x2\n") != NULL);
    CHECK(strstr(out, "\nsite: 0x10004 test+0x4\nsite function: 0x10004 test+0x4 jumps_missing +0x0\n") != NULL);
    c->pend.kind = RL_PEND_NONE;
    saved = capture_begin(fds);
    CHECK(saved >= 0);
    rc = rl_call_guest(c, rl_mem_base(), 0x10008);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK(strstr(out, "\ntransfer: call\ntarget: 0x10002 test+0x2\n") != NULL);
    CHECK(strstr(out, "\nsite: 0x10008 test+0x8\nsite function: 0x10008 test+0x8 calls_missing +0x0\n") != NULL);
    CHECK(strstr(out, "\ncaller function: 0x10008 test+0x8 calls_missing (host calls_missing+0x") != NULL);
    CHECK(strstr(out, "guest backtrace: lifted frames, innermost first; tail calls leave no frame\n  #0 0x10008 test+0x8 "
                      "calls_missing (nearest)\n") != NULL);
    c->pend.kind = RL_PEND_NONE;
}

static uint64_t ret_rewritten(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[30] = 0x20100;
    return rl_arm64_ret(c);
}

static uint64_t ret_moved_here(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 0xBEEF;
    c->x[30] = 0;
    return rl_arm64_ret(c);
}

static uint64_t leaf_ok(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[1] = 7;
    return rl_arm64_ret(c);
}

static uint64_t tail_with_new_x30(rl_ctx *c, uint8_t *mem)
{
    c->x[30] = 0x20300;
    RL_MUSTTAIL return leaf_ok(c, mem);
}

static uint64_t after_tail(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[2] = c->x[1] + 1;
    return 0;
}

static uint64_t ret_wild(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    return c->x[30] ^ (1ull << 53);
}

static uint64_t call_wild(rl_ctx *c, uint8_t *mem)
{
    c->x[30] = 0x2050c;
    uint64_t rc = ret_wild(c, mem);
    if (RL_UNLIKELY(rc != UINT64_C(0x2050c)))
        return rl_arm64_ret_elsewhere(c, rc, UINT64_C(0x20508));
    return 0;
}

static uint64_t ret_one(rl_ctx *c, uint8_t *mem)
{
    (void)c;
    (void)mem;
    return 1;
}

static rl_ctx *boot_ret(void)
{
    rl_ctx *c = boot();
    if (!c)
        return NULL;
    static const rl_fn_entry e[] = {
        { 0x20000, 16, 0, ret_rewritten },
        { 0x20100, 16, 0, ret_moved_here },
        { 0x20200, 16, 0, tail_with_new_x30 },
        { 0x20300, 16, 0, after_tail },
        { 0x20400, 16, 0, ret_wild },
        { 0x20500, 16, 0, call_wild },
        { 0x20600, 16, 0, ret_one },
    };
    static rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(rl_ctx);
    u.fn_count = 7;
    u.fns = e;
    u.name = "arm64-ret";
    const rl_unit *list[] = { &u };
    if (rl_units_register(list, 1) != 0)
        return NULL;
    rl_dispatch_view(c);
    return c;
}

TEST(arm64_a_return_elsewhere_continues_at_its_target)
{
    rl_ctx *c = boot_ret();
    CHECK(c != NULL);
    c->x[30] = 0;
    CHECK_EQ(rl_call_guest(c, rl_mem_base(), 0x20000), RL_OK);
    CHECK_EQ(c->x[0], 0xBEEF);
    CHECK_EQ(c->x[30], 0);
    CHECK_EQ(c->pend.kind, RL_PEND_NONE);
}

TEST(arm64_a_tail_transfer_with_another_x30_returns_to_that_x30)
{
    rl_ctx *c = boot_ret();
    CHECK(c != NULL);
    c->x[30] = 0;
    CHECK_EQ(rl_call_guest(c, rl_mem_base(), 0x20200), RL_OK);
    CHECK_EQ(c->x[1], 7);
    CHECK_EQ(c->x[2], 8);
}

TEST(arm64_a_return_elsewhere_records_a_redirect_that_passes_every_frame)
{
    rl_ctx *c = boot_ret();
    CHECK(c != NULL);
    CHECK_EQ(rl_arm64_ret_elsewhere(c, 0x20100, 0x2000c), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_REDIRECT);
    CHECK_EQ(c->pend.pc, 0x20100);
    CHECK_EQ(c->pend.sp, RL_SP_NONE);
    CHECK_EQ(c->pc, 0x2000c);
    c->pend.kind = RL_PEND_NONE;
}

TEST(arm64_a_pending_callee_passes_through_the_return_check)
{
    rl_ctx *c = boot_ret();
    CHECK(c != NULL);
    c->pend.kind = RL_PEND_EXIT_THREAD;
    c->pend.pc = 0;
    CHECK_EQ(rl_arm64_ret_elsewhere(c, RL_PENDING, 0x2000c), RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_THREAD);
    CHECK_EQ(c->pend.pc, 0);
    c->pend.kind = RL_PEND_NONE;
}

TEST(arm64_a_return_to_1_is_a_return_not_a_pending_record)
{
    rl_ctx *c = boot_ret();
    CHECK(c != NULL);
    c->x[30] = 0;
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_call_guest(c, rl_mem_base(), 0x20600);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "summary: no function at guest 0x1\n") != NULL);
}

TEST(arm64_a_return_to_a_non_canonical_address_is_an_instruction_abort)
{
    rl_ctx *c = boot_ret();
    CHECK(c != NULL);
    c->x[30] = 0x20008;
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_call_guest(c, rl_mem_base(), 0x20400);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "instruction abort: return to 0x20000000020008 from the function entered at guest pc 0x20400, outside the 39-bit address space") != NULL);
    CHECK(strstr(out, "failed pointer authentication") != NULL);
}

TEST(arm64_a_call_returning_to_a_non_canonical_address_is_an_instruction_abort)
{
    rl_ctx *c = boot_ret();
    CHECK(c != NULL);
    c->x[30] = 0;
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_call_guest(c, rl_mem_base(), 0x20500);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "instruction abort: return to 0x2000000002050c from the call at guest pc 0x20508, outside the 39-bit address space") != NULL);
}

TEST(arm64_a_fetch_from_a_non_canonical_address_is_an_instruction_abort)
{
    rl_ctx *c = boot_ret();
    CHECK(c != NULL);
    rl_arm64_set_va_bits(36);
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_call_guest(c, rl_mem_base(), 0x1000000000ull);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "instruction abort: fetch from guest pc 0x1000000000, outside the 36-bit address space") != NULL);
    CHECK(strstr(out, "missed code") == NULL);
}

TEST(arm64_align_fault_names_the_base_register_and_pc)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->x[8] = 0x10966e35f1ull;
    int fds[2];
    static char out[32768];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_align_fault(c, rl_mem_base(), 0x8124e4c, 0xb8290108u);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "kind: alignment fault\nsummary: alignment fault: the atomic, ordered or exclusive access 0xb8290108 "
                      "through x8 = 0x10966e35f1 is not naturally aligned, at guest pc 0x8124e4c\n") != NULL);
}
