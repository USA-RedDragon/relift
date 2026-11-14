/**
 * @file test_a64.c
 * @brief The A64 CPU layer over the core with a test guest that is no real
 * OS (16 KiB pages, 40 address bits, a 1 MHz clock). This binary links the
 * core and the A64 layer only, so nothing here can lean on a guest OS.
 */
#include "a64.h"
#include "runner.h"

#include <stdlib.h>
#include <unistd.h>

static const rl_guest_desc test_guest = {
    .name = "a64-test",
    .page_size = 0x4000,
    .address_bits = 40,
    .tick_hz = 1000000,
};

static rl_ctx *boot(void)
{
    if (rl_core_init(&test_guest, rl_a64_cpu()) != 0)
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

TEST(a64_descriptor_matches_the_context)
{
    const rl_cpu_desc *d = rl_a64_cpu();
    CHECK_EQ(d->ctx_size, sizeof(rl_ctx));
    CHECK_EQ(d->ctx_size, 896);
    CHECK_EQ(d->ctx_align, 64);
    CHECK_EQ(d->common_offset, offsetof(rl_ctx, pend));
    CHECK_EQ(d->code_shift, 2);
    CHECK(!d->big_endian);
    CHECK_EQ(rl_core_init(&test_guest, d), 0);
    CHECK_EQ(rl_guest_order32(0x11223344u), 0x11223344u);
}

TEST(a64_context_defaults)
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

TEST(a64_fpcr_follows_the_thread)
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

TEST(a64_cntpct_counts_guest_ticks)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint64_t a = rl_mrs_cntpct(c);
    rl_sleep_ns(10000000);
    uint64_t d = rl_mrs_cntpct(c) - a;
    CHECK(d >= 10000);
    CHECK(d < 1000000);
}

TEST(a64_undefined_names_the_instruction)
{
    char buf[32];
    CHECK(!strcmp(rl_a64_insn_kind(0x00000005u, buf, sizeof buf), "UDF #5"));
    CHECK(!strcmp(rl_a64_insn_kind(0xD4207D00u, buf, sizeof buf), "BRK #1000"));
    CHECK(!strcmp(rl_a64_insn_kind(0xD4400000u, buf, sizeof buf), "HLT #0"));
    CHECK(!strcmp(rl_a64_insn_kind(0x8B020020u, buf, sizeof buf), "unliftable"));
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->x[30] = 0x1230;
    c->sp = 0x8000;
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_undefined(c, rl_mem_base(), 0x4000, 0xD4207D00u);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(c->pend.code, RL_EXIT_SOFTWARE);
    CHECK(strstr(out, "undefined instruction 0xd4207d00 (BRK #1000) at guest pc 0x4000 (lr 0x1230, sp 0x8000)") != NULL);
}

TEST(a64_brk_reports_a_breakpoint)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->x[30] = 0x1230;
    c->sp = 0x8000;
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_brk(c, rl_mem_base(), 1000);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "breakpoint BRK #1000 (lr 0x1230, sp 0x8000)") != NULL);
}

TEST(a64_resume_installs_registers)
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

static int entry_fn(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->x[0] = 0x600D;
    return RL_OK;
}

static int calls_missing(rl_ctx *c, uint8_t *mem)
{
    RL_MUSTTAIL return rl_indirect(c, 0x10002)(c, mem);
}

TEST(a64_dispatch_through_lookup)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    static const rl_fn_entry e[] = { { 0x10000, 4, 0, entry_fn }, { 0x10004, 4, 0, calls_missing } };
    rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(rl_ctx);
    u.fn_count = 2;
    u.fns = e;
    u.name = "a64-test";
    const rl_unit *list[] = { &u };
    CHECK_EQ(rl_units_register(list, 1), 0);
    rl_dispatch_view(c);
    CHECK(rl_lookup(c, 0x10000) == entry_fn);
    CHECK(rl_lookup(c, 0x10002) == rl_dispatch_miss);
    CHECK(rl_lookup(c, 0x0FFFC) == rl_dispatch_miss);
    CHECK_EQ(rl_call_guest(c, rl_mem_base(), 0x10000), RL_OK);
    CHECK_EQ(c->x[0], 0x600D);
    int fds[2];
    char out[1024];
    int saved = capture_begin(fds);
    CHECK(saved >= 0);
    int rc = rl_call_guest(c, rl_mem_base(), 0x10004);
    capture_end(fds, saved, out, sizeof out);
    CHECK_EQ(rc, RL_PENDING);
    CHECK_EQ(c->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK(strstr(out, "missed code: no function at guest 0x10002 (lr ") != NULL);
}
