/**
 * @file test_mips.c
 * @brief The VR4300 CPU layer over the core with hand-written lifted
 * functions: context layout, exception entry and ERET, the vectors,
 * interrupts and their masking, Count and Compare from the cycle clock,
 * Random, the TLB instructions and translation with its exceptions, the
 * guard slow paths, the return address stack and the run loop. This binary
 * links the core and the MIPS layer only.
 */
#include "mips.h"

#include "runner.h"

#include <relift/sched.h>

static const rl_guest_desc test_guest = {
    .name = "mips-test",
    .page_size = 0x1000,
    .address_bits = 32,
    .tick_hz = 93750000,
};

#define K0(a) (UINT64_C(0xFFFFFFFF80000000) | (a))

static struct {
    int handler_runs;
    int after;
    uint64_t epc_seen;
} hs;

/** @brief An interrupt handler: acknowledges by clearing IP2 and IP7 (writes Compare), then ERET. */
static uint64_t handler(rl_ctx *c, uint8_t *mem)
{
    hs.handler_runs++;
    hs.epc_seen = c->cp0_epc;
    c->cp0_compare = c->cp0_compare;
    c->cp0_cause &= ~(uint32_t)RL_MIPS_CAUSE_IP7;
    RL_MUSTTAIL return rl_mips_eret_to(c, rl_mips_eret(c))(c, mem);
}

/** @brief A leaf: r2 += 1, then JR ra. */
static uint64_t leaf(rl_ctx *c, uint8_t *mem)
{
    c->r[2] += 1;
    RL_MUSTTAIL return rl_indirect(c, c->r[31])(c, mem);
}

/** @brief JAL leaf twice, then stops. */
static uint64_t caller(rl_ctx *c, uint8_t *mem)
{
    c->r[31] = K0(0x1008);
    rl_mips_call(c, K0(0x1008));
    if (rl_indirect(c, K0(0x2000))(c, mem))
        return RL_PENDING;
    c->r[31] = K0(0x1010);
    rl_mips_call(c, K0(0x1010));
    if (rl_indirect(c, K0(0x2000))(c, mem))
        return RL_PENDING;
    hs.after = 1;
    return rl_pend_exit_process(c, 0);
}

/** @brief A handler that does not return to EPC: switches to 0x3000. */
static uint64_t switcher(rl_ctx *c, uint8_t *mem)
{
    hs.handler_runs++;
    c->cp0_epc = K0(0x3000);
    RL_MUSTTAIL return rl_mips_eret_to(c, rl_mips_eret(c))(c, mem);
}

static uint64_t landing(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    c->r[3] = 0x5A;
    return rl_pend_exit_process(c, 0);
}

static uint64_t (*g_vector)(rl_ctx *, uint8_t *) = handler;

static uint64_t vector(rl_ctx *c, uint8_t *mem) { return g_vector(c, mem); }

/** @brief A lifted block with a sync point: takes interrupts with IP from the test. */
static uint32_t g_ip;

static uint64_t syncing(rl_ctx *c, uint8_t *mem)
{
    uint64_t v = rl_mips_irq_enter(c, K0(0x4004), g_ip);
    if (v != RL_NO_IRQ) {
        int r = rl_indirect(c, v)(c, mem);
        if (r)
            return RL_PENDING;
    }
    hs.after++;
    return rl_pend_exit_process(c, 0);
}

static const rl_fn_entry fns[] = {
    {K0(0x180), 4, 0, vector}, {K0(0x1000), 16, 0, caller},   {K0(0x2000), 8, 0, leaf},
    {K0(0x3000), 4, 0, landing}, {K0(0x4000), 8, 0, syncing},
};

static rl_ctx *boot(void)
{
    if (rl_core_init(&test_guest, rl_mips_cpu()) != 0)
        return NULL;
    rl_units_reset();
    static rl_unit u;
    memset(&u, 0, sizeof u);
    u.abi_version = RL_ABI_VERSION;
    u.ctx_size = sizeof(rl_ctx);
    u.fn_count = (uint32_t)(sizeof fns / sizeof fns[0]);
    u.fns = fns;
    u.name = "mips-test";
    const rl_unit *list[] = {&u};
    if (rl_units_register(list, 1) != 0)
        return NULL;
    memset(&hs, 0, sizeof hs);
    g_vector = handler;
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return NULL;
    rl_thread_bind(t);
    rl_ctx *c = t->ctx;
    c->time.deadline = 1000;
    c->time.budget = 1000;
    return c;
}

/** @brief Guest physical memory of the slow-path tests. */
static uint8_t test_mem[0x400000];

/** @brief The cycles f takes off c's budget. */
#define CYCLES(c, f) (__extension__({                                                                                \
    int64_t b0_ = (c)->time.budget;                                                                                    \
    f;                                                                                                                 \
    b0_ - (c)->time.budget;                                                                                            \
}))

/** @brief Moves the cycle clock on by n. */
static void spend(rl_ctx *c, int64_t n) { c->time.budget -= n; }

#define SLOT_OFFSET(name, field, bits) offsetof(rl_ctx, field),
static const size_t slot_offsets[] = {RL_MIPS_SLOTS(SLOT_OFFSET)};
#undef SLOT_OFFSET

TEST(mips_context_slots_are_in_slot_order)
{
    CHECK_EQ(sizeof slot_offsets / sizeof slot_offsets[0], 90);
    for (unsigned i = 1; i < 90; i++)
        CHECK(slot_offsets[i] > slot_offsets[i - 1]);
    CHECK_EQ(slot_offsets[66], offsetof(rl_ctx, fcr31));
    CHECK_EQ(slot_offsets[89], offsetof(rl_ctx, cp0_ll_bit));
}

TEST(mips_descriptor_matches_the_context)
{
    const rl_cpu_desc *d = rl_mips_cpu();
    CHECK_EQ(d->ctx_size, sizeof(rl_ctx));
    CHECK_EQ(d->common_offset, offsetof(rl_ctx, pend));
    CHECK_EQ(d->time_offset, offsetof(rl_ctx, time));
    CHECK_EQ(d->code_shift, 2);
    CHECK(d->big_endian);
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ(c->cp0_status, RL_MIPS_SR_BEV | RL_MIPS_SR_ERL);
    CHECK_EQ(rl_mips_random(c), 31);
}

TEST(mips_exception_entry_sets_epc_cause_and_exl)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->cp0_status = RL_MIPS_SR_IE;
    c->cp0_cause = 0x8000u | RL_MIPS_CAUSE_BD;
    uint64_t v = rl_mips_exception(c, RL_MIPS_EXC_SYS, K0(0x1234), false, 0);
    CHECK_EQ(v, K0(0x180));
    CHECK_EQ(c->cp0_epc, K0(0x1234));
    CHECK_EQ(c->cp0_cause, 0x8000u | RL_MIPS_EXC_SYS << 2);
    CHECK_EQ(c->cp0_status, RL_MIPS_SR_IE | RL_MIPS_SR_EXL);
    v = rl_mips_exception(c, RL_MIPS_EXC_OV, K0(0x2000), true, 0);
    CHECK_EQ(c->cp0_epc, K0(0x1234));
    CHECK_EQ(c->cp0_cause, 0x8000u | RL_MIPS_EXC_OV << 2);
    CHECK_EQ(rl_mips_eret(c), K0(0x1234));
    CHECK_EQ(c->cp0_status, RL_MIPS_SR_IE);
    rl_mips_exception(c, RL_MIPS_EXC_CPU, K0(0x2004), true, 1);
    CHECK_EQ(c->cp0_epc, K0(0x2000));
    CHECK_EQ(c->cp0_cause, RL_MIPS_CAUSE_BD | 1u << 28 | 0x8000u | RL_MIPS_EXC_CPU << 2);
    c->cp0_status = RL_MIPS_SR_BEV;
    CHECK_EQ(rl_mips_exception(c, RL_MIPS_EXC_BP, K0(0), false, 0), UINT64_C(0xFFFFFFFFBFC00380));
}

TEST(mips_eret_prefers_error_epc_and_clears_llbit)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->cp0_status = RL_MIPS_SR_ERL | RL_MIPS_SR_EXL;
    c->cp0_error_epc = K0(0x500);
    c->cp0_epc = K0(0x600);
    c->cp0_ll_bit = 1;
    CHECK_EQ(rl_mips_eret(c), K0(0x500));
    CHECK_EQ(c->cp0_status, RL_MIPS_SR_EXL);
    CHECK_EQ(c->cp0_ll_bit, 0);
    CHECK_EQ(rl_mips_eret(c), K0(0x600));
    CHECK_EQ(c->cp0_status, 0);
}

TEST(mips_nmi_and_reset_use_the_reset_vector)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->cp0_status = RL_MIPS_SR_TS | RL_MIPS_SR_IE;
    CHECK_EQ(rl_mips_nmi(c, K0(0x777)), RL_MIPS_RESET_VECTOR);
    CHECK_EQ(c->cp0_error_epc, K0(0x777));
    CHECK_EQ(c->cp0_status, RL_MIPS_SR_IE | RL_MIPS_SR_BEV | RL_MIPS_SR_SR | RL_MIPS_SR_ERL);
}

TEST(mips_interrupts_need_ie_and_no_exl_or_erl)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->cp0_status = RL_MIPS_SR_IE | 0x0400u;
    c->cp0_cause = 0;
    CHECK(!rl_mips_irq_pending(c));
    rl_mips_set_ip(c, 1u << 2);
    CHECK(rl_mips_irq_pending(c));
    CHECK_EQ(c->cp0_cause, 0x0400u);
    CHECK_EQ(c->time.budget, 0);
    c->cp0_status |= RL_MIPS_SR_EXL;
    CHECK(!rl_mips_irq_pending(c));
    c->cp0_status = RL_MIPS_SR_IE | RL_MIPS_SR_ERL | 0x0400u;
    CHECK(!rl_mips_irq_pending(c));
    c->cp0_status = 0x0400u;
    CHECK(!rl_mips_irq_pending(c));
    c->cp0_status = RL_MIPS_SR_IE | 0x0800u;
    CHECK(!rl_mips_irq_pending(c));
    c->cp0_cause |= 0x0100u;
    c->cp0_status = RL_MIPS_SR_IE | 0x0100u;
    CHECK(rl_mips_irq_pending(c));
}

TEST(mips_count_runs_at_half_the_cycle_rate_and_compare_raises_ip7)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    rl_mips_set_count(c, 100);
    spend(c, 10);
    CHECK_EQ(rl_mips_count(c), 105);
    CHECK_EQ(c->cp0_count, 105);
    spend(c, 1);
    CHECK_EQ(rl_mips_count(c), 105);
    spend(c, 1);
    CHECK_EQ(rl_mips_count(c), 106);
    c->cp0_compare = 110;
    CHECK_EQ(rl_mips_timer_cycles(c), 8);
    spend(c, 7);
    rl_mips_cp0_sync(c);
    CHECK_EQ(c->cp0_cause & RL_MIPS_CAUSE_IP7, 0);
    spend(c, 1);
    rl_mips_cp0_sync(c);
    CHECK_EQ(c->cp0_cause & RL_MIPS_CAUSE_IP7, RL_MIPS_CAUSE_IP7);
    c->cp0_cause &= ~(uint32_t)RL_MIPS_CAUSE_IP7;
    spend(c, 100);
    rl_mips_cp0_sync(c);
    CHECK_EQ(c->cp0_cause & RL_MIPS_CAUSE_IP7, 0);
    c->cp0_count = 0xFFFFFFF0u;
    c->cp0_compare = 4;
    rl_mips_cp0_sync(c);
    CHECK_EQ(rl_mips_timer_cycles(c), 40);
    spend(c, 40);
    rl_mips_cp0_sync(c);
    CHECK_EQ(c->cp0_cause & RL_MIPS_CAUSE_IP7, RL_MIPS_CAUSE_IP7);
    CHECK_EQ(c->cp0_count, 4);
}

TEST(mips_compare_equal_to_count_waits_a_full_wrap)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    rl_mips_set_count(c, 50);
    c->cp0_compare = 50;
    CHECK_EQ(rl_mips_timer_cycles(c), UINT64_C(1) << 33);
    spend(c, 2);
    rl_mips_cp0_sync(c);
    CHECK_EQ(c->cp0_cause & RL_MIPS_CAUSE_IP7, 0);
}

TEST(mips_random_counts_down_to_wired)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    CHECK_EQ(rl_mips_random(c), 31);
    spend(c, 1);
    CHECK_EQ(rl_mips_random(c), 30);
    spend(c, 29);
    CHECK_EQ(rl_mips_random(c), 1);
    spend(c, 1);
    CHECK_EQ(rl_mips_random(c), 0);
    spend(c, 1);
    CHECK_EQ(rl_mips_random(c), 31);
    c->cp0_wired = 28;
    CHECK_EQ(rl_mips_random(c), 31);
    spend(c, 3);
    CHECK_EQ(rl_mips_random(c), 28);
    spend(c, 1);
    CHECK_EQ(rl_mips_random(c), 31);
}

/** @brief Writes TLB entry i: a 4 KiB page pair at vpn2 (EntryHi with ASID) to pfn0 and pfn1 with flags (C, D, V, G). */
static void map(rl_ctx *c, unsigned i, uint64_t hi, uint64_t lo0, uint64_t lo1, uint64_t mask)
{
    c->cp0_index = i;
    c->cp0_entry_hi = hi;
    c->cp0_entry_lo0 = lo0;
    c->cp0_entry_lo1 = lo1;
    c->cp0_page_mask = mask;
    rl_mips_tlb(c, 1, 1);
}

/** @brief EntryLo of physical page pfn (4 KiB units), valid, with dirty and global as given. */
static uint64_t lo(uint64_t pfn, bool dirty, bool global) { return pfn << 6 | 3u << 3 | 2u | (dirty ? 4u : 0u) | global; }

TEST(mips_tlb_translates_kuseg_kseg2_and_kseg3)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->cp0_status = 0;
    map(c, 0, 0x00400000u | 7u, lo(0x100, true, false), lo(0x200, false, false), 0);
    uint64_t pa = 0;
    c->cp0_entry_hi = 7;
    CHECK_EQ(rl_mips_translate(c, 0x00400123u, false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x100123u);
    CHECK_EQ(rl_mips_translate(c, 0x00401FFCu, false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x200FFCu);
    CHECK_EQ(rl_mips_translate(c, 0x00401000u, true, &pa), RL_MIPS_XLAT_MODIFIED);
    CHECK_EQ(rl_mips_translate(c, 0x00402000u, false, &pa), RL_MIPS_XLAT_MISS);
    c->cp0_entry_hi = 8;
    CHECK_EQ(rl_mips_translate(c, 0x00400000u, false, &pa), RL_MIPS_XLAT_MISS);
    map(c, 1, UINT64_C(0xFFFFFFFFC0000000), lo(0x300, true, true), 0, 0);
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0xFFFFFFFFC0000010), true, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x300010u);
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0xFFFFFFFFC0001000), false, &pa), RL_MIPS_XLAT_INVALID);
    map(c, 2, UINT64_C(0xFFFFFFFFE0000000), lo(0x400, true, true), lo(0x500, true, true), 0);
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0xFFFFFFFFE0001004), false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x500004u);
    CHECK_EQ(rl_mips_translate(c, K0(0x123), false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x123u);
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0xFFFFFFFFA4000040), false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x04000040u);
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0x0000000100000000), false, &pa), RL_MIPS_XLAT_ADDRESS);
}

TEST(mips_tlb_large_pages_and_erl_kuseg)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->cp0_status = 0;
    map(c, 5, 0x01000000u, lo(0x1000, true, true), lo(0x2000, true, true), 0x01FFE000u);
    uint64_t pa = 0;
    CHECK_EQ(rl_mips_translate(c, 0x00ABCDEFu, false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x01ABCDEFu);
    CHECK_EQ(rl_mips_translate(c, 0x01000004u, false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x02000004u);
    CHECK_EQ(rl_mips_translate(c, 0x02000004u, false, &pa), RL_MIPS_XLAT_MISS);
    map(c, 6, 0x10000000u, lo(0x1000, true, true), lo(0x2000, true, true), 0x6000u);
    CHECK_EQ(rl_mips_translate(c, 0x10002000u, false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x01002000u);
    CHECK_EQ(rl_mips_translate(c, 0x10006000u, false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x02002000u);
    CHECK_EQ(rl_mips_translate(c, 0x10008000u, false, &pa), RL_MIPS_XLAT_MISS);
    c->cp0_status = RL_MIPS_SR_ERL;
    CHECK_EQ(rl_mips_translate(c, 0x00000040u, false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x40u);
}

TEST(mips_translation_by_mode_and_64_bit_segments)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint64_t pa = 0;
    c->cp0_status = 2u << 3;
    CHECK_EQ(rl_mips_translate(c, K0(0), false, &pa), RL_MIPS_XLAT_ADDRESS);
    CHECK_EQ(rl_mips_translate(c, 0x40000000u, false, &pa), RL_MIPS_XLAT_MISS);
    c->cp0_status = 1u << 3;
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0xFFFFFFFFC0000000), false, &pa), RL_MIPS_XLAT_MISS);
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0xFFFFFFFFE0000000), false, &pa), RL_MIPS_XLAT_ADDRESS);
    c->cp0_status = 2u << 3 | RL_MIPS_SR_UX;
    map(c, 0, UINT64_C(0x000000F000000000), lo(0x10, true, true), 0, 0);
    c->cp0_status = 2u << 3 | RL_MIPS_SR_UX;
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0x000000F000000008), false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x10008u);
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0x0000010000000000), false, &pa), RL_MIPS_XLAT_ADDRESS);
    c->cp0_status = RL_MIPS_SR_KX;
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0x9000000012345678), false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x12345678u);
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0x9000000112345678), false, &pa), RL_MIPS_XLAT_ADDRESS);
    CHECK_EQ(rl_mips_translate(c, K0(0x40), false, &pa), RL_MIPS_XLAT_OK);
    CHECK_EQ(pa, 0x40u);
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0xC000000000000000), false, &pa), RL_MIPS_XLAT_MISS);
    CHECK_EQ(rl_mips_translate(c, UINT64_C(0x4000000000000000), false, &pa), RL_MIPS_XLAT_MISS);
}

TEST(mips_tlb_miss_sets_badvaddr_context_entryhi_and_the_refill_vector)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->cp0_status = 0;
    c->cp0_entry_hi = 0x42;
    c->cp0_context = UINT64_C(0xFFFFFFFF80000000);
    uint64_t va = 0x12345678u;
    uint64_t v = rl_mips_xlat_exception(c, RL_MIPS_XLAT_MISS, true, va, K0(0x800), false);
    CHECK_EQ(v, K0(0));
    CHECK_EQ(c->cp0_bad_vaddr, va);
    CHECK_EQ(c->cp0_entry_hi, 0x12344000u | 0x42u);
    CHECK_EQ(c->cp0_context, UINT64_C(0xFFFFFFFF80000000) | (va >> 13) << 4);
    CHECK_EQ(c->cp0_cause >> 2 & 31u, RL_MIPS_EXC_TLBS);
    CHECK_EQ(c->cp0_epc, K0(0x800));
    v = rl_mips_xlat_exception(c, RL_MIPS_XLAT_MISS, false, va, K0(0x900), false);
    CHECK_EQ(v, K0(0x180));
    CHECK_EQ(c->cp0_epc, K0(0x800));
    CHECK_EQ(c->cp0_cause >> 2 & 31u, RL_MIPS_EXC_TLBL);
    c->cp0_status = RL_MIPS_SR_KX;
    v = rl_mips_xlat_exception(c, RL_MIPS_XLAT_MISS, false, UINT64_C(0xC000000123456000), K0(0x900), false);
    CHECK_EQ(v, K0(0x080));
    CHECK_EQ(c->cp0_xcontext & UINT64_C(0x1FFFFFFF0), (UINT64_C(3) << 31) | ((UINT64_C(0x123456000) >> 13) << 4));
    c->cp0_status = 0;
    v = rl_mips_xlat_exception(c, RL_MIPS_XLAT_INVALID, false, va, K0(0x900), true);
    CHECK_EQ(v, K0(0x180));
    CHECK_EQ(c->cp0_epc, K0(0x8FC));
    CHECK_EQ(c->cp0_cause & RL_MIPS_CAUSE_BD, RL_MIPS_CAUSE_BD);
    c->cp0_status = RL_MIPS_SR_BEV;
    v = rl_mips_xlat_exception(c, RL_MIPS_XLAT_MISS, false, va, K0(0x900), false);
    CHECK_EQ(v, UINT64_C(0xFFFFFFFFBFC00200));
    c->cp0_status = 0;
    rl_mips_xlat_exception(c, RL_MIPS_XLAT_MODIFIED, true, va, K0(0x900), false);
    CHECK_EQ(c->cp0_cause >> 2 & 31u, RL_MIPS_EXC_MOD);
    rl_mips_xlat_exception(c, RL_MIPS_XLAT_ADDRESS, false, va, K0(0x900), false);
    CHECK_EQ(c->cp0_cause >> 2 & 31u, RL_MIPS_EXC_ADEL);
}

TEST(mips_tlbr_tlbp_and_tlbwr)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    map(c, 3, 0x00800000u | 9u, lo(0x10, true, true), lo(0x11, false, false), 0x6000u);
    c->cp0_index = 3;
    c->cp0_entry_hi = 0;
    rl_mips_tlb(c, 0, 1);
    CHECK_EQ(c->cp0_entry_hi, 0x00800000u | 9u);
    CHECK_EQ(c->cp0_page_mask, 0x6000u);
    CHECK_EQ(c->cp0_entry_lo0, lo(0x10, true, false));
    CHECK_EQ(c->cp0_entry_lo1, lo(0x11, false, false));
    c->cp0_entry_hi = 0x00800000u | 1u;
    rl_mips_tlb(c, 3, 1);
    CHECK_EQ(c->cp0_index, UINT64_C(0x80000000));
    c->cp0_entry_hi = 0x00800000u | 9u;
    rl_mips_tlb(c, 3, 1);
    CHECK_EQ(c->cp0_index, 3);
    c->cp0_entry_hi = 0x00900000u;
    rl_mips_tlb(c, 3, 1);
    CHECK_EQ(c->cp0_index, UINT64_C(0x80000000));
    rl_mips_tlb(c, 3, 0);
    CHECK_EQ(c->cp0_index, UINT64_C(0x80000000));
    c->cp0_entry_hi = 0x00A00000u;
    c->cp0_entry_lo0 = lo(0x20, true, true);
    c->cp0_entry_lo1 = lo(0x21, true, true);
    c->cp0_page_mask = 0;
    spend(c, 5);
    unsigned r = (unsigned)rl_mips_random(c);
    rl_mips_tlb(c, 2, 1);
    CHECK_EQ(c->tlb[r].entry_hi, 0x00A00000u);
}

TEST(mips_guard_runs_a_tlb_mapped_load_and_store)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint8_t *mem = test_mem;
    c->cp0_status = 0;
    map(c, 0, 0x00400000u, lo(0x100, true, true), lo(0x101, true, true), 0);
    c->r[4] = 0x00400100u;
    c->r[5] = UINT64_C(0x1122334455667788);
    uint32_t sd = 0x3Fu << 26 | 4u << 21 | 5u << 16 | 8u;
    memset(c->icache, 0, sizeof c->icache);
    memset(c->dcache, 0, sizeof c->dcache);
    int rc = 0;
    CHECK_EQ(CYCLES(c, rc = rl_mips_guard(c, mem, 0x1000 + RL_MIPS_EXC_TLBS, K0(0x2000), sd)), 48 + 1 + 40);
    CHECK_EQ(rc, RL_OK);
    CHECK_EQ(rl_mips_ld(mem, 0x100108u, 8), UINT64_C(0x1122334455667788));
    uint32_t lw = 0x23u << 26 | 4u << 21 | 6u << 16 | 12u;
    CHECK_EQ(CYCLES(c, rc = rl_mips_guard(c, mem, 0x1000 + RL_MIPS_EXC_TLBL, K0(0x2000), lw)), 1 + 1);
    CHECK_EQ(rc, RL_OK);
    CHECK_EQ(c->r[6], 0x55667788u);
    uint32_t lb = 0x20u << 26 | 4u << 21 | 6u << 16 | 8u;
    CHECK_EQ(rl_mips_guard(c, mem, 0x1000 + RL_MIPS_EXC_TLBL, K0(0x2000), lb), RL_OK);
    CHECK_EQ(c->r[6], 0x11u);
    uint32_t lwl = 0x22u << 26 | 4u << 21 | 7u << 16 | 9u;
    c->r[7] = 0xAAAAAAAAu;
    CHECK_EQ(rl_mips_guard(c, mem, 0x1000 + RL_MIPS_EXC_TLBL, K0(0x2000), lwl), RL_OK);
    CHECK_EQ(c->r[7], 0x223344AAu);
    c->r[4] = 0x00402000u;
    CHECK_EQ(rl_mips_guard(c, mem, 0x1000 + RL_MIPS_EXC_TLBL, K0(0x2000), lw), RL_PENDING);
    rl_pending *p = &rl_common(c)->pend;
    CHECK_EQ(p->kind, RL_PEND_REDIRECT);
    CHECK_EQ(p->pc, K0(0));
    CHECK_EQ(c->cp0_bad_vaddr, 0x0040200Cu);
    CHECK_EQ(c->cp0_epc, K0(0x2000));
}

TEST(mips_guard_raises_the_static_exceptions_and_fpe)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint8_t *mem = test_mem;
    c->cp0_status = RL_MIPS_SR_CU1 | RL_MIPS_SR_FR;
    CHECK_EQ(rl_mips_guard(c, mem, 0x1000 + RL_MIPS_EXC_SYS, K0(0x2000), 0x0000000Cu), RL_PENDING);
    CHECK_EQ(rl_common(c)->pend.pc, K0(0x180));
    CHECK_EQ(c->cp0_cause >> 2 & 31u, RL_MIPS_EXC_SYS);
    c->cp0_status = RL_MIPS_SR_CU1 | RL_MIPS_SR_FR;
    CHECK_EQ(rl_mips_guard(c, mem, 0x1000 + RL_MIPS_EXC_CPU, K0(0x2000), 0x46000000u), RL_PENDING);
    CHECK_EQ(c->cp0_cause, (1u << 28) | RL_MIPS_EXC_CPU << 2);
    c->cp0_status = RL_MIPS_SR_FR;
    CHECK_EQ(rl_mips_guard(c, mem, 0x1000 + RL_MIPS_EXC_CPU, K0(0x2000), 0xE4A40000u), RL_PENDING);
    CHECK_EQ(c->cp0_cause, (1u << 28) | RL_MIPS_EXC_CPU << 2);
    c->cp0_status = RL_MIPS_SR_CU1 | RL_MIPS_SR_FR;
    c->f[2] = 0x3F800000;
    c->f[4] = 0x3F800000;
    c->fcr31 = 0;
    uint32_t add = 0x46000000u | 4u << 16 | 2u << 11 | 6u << 6;
    CHECK_EQ(rl_mips_guard(c, mem, RL_MIPS_GUARD_FP_SLOW, K0(0x2000), add), RL_OK);
    CHECK_EQ(c->f[6], 0x40000000u);
    c->f[2] = 0x00000001u;
    CHECK_EQ(rl_mips_guard(c, mem, RL_MIPS_GUARD_FP_SLOW, K0(0x2000), add), RL_PENDING);
    CHECK_EQ(c->cp0_cause >> 2 & 31u, RL_MIPS_EXC_FPE);
    CHECK_EQ(c->fcr31, RL_MIPS_FCR_E);
    c->cp0_status = RL_MIPS_SR_CU1 | RL_MIPS_SR_FR;
    c->r[8] = 0x00020000u;
    uint32_t ctc1 = 0x44C00000u | 8u << 16 | 31u << 11;
    CHECK_EQ(rl_mips_guard(c, mem, 0x1000 + RL_MIPS_EXC_FPE, K0(0x2000), ctc1), RL_PENDING);
    CHECK_EQ(c->fcr31, 0x00020000u);
    CHECK_EQ(c->cp0_cause >> 2 & 31u, RL_MIPS_EXC_FPE);
}

TEST(mips_guard_exit_runs_the_branch_then_the_slot)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint8_t *mem = test_mem;
    c->cp0_status = RL_MIPS_SR_CU1 | RL_MIPS_SR_FR;
    c->f[2] = 0x3F800000;
    c->f[4] = 0x3F800000;
    uint32_t jal = 0x03u << 26 | (0x2000u >> 2);
    uint32_t add = 0x46000000u | 4u << 16 | 2u << 11 | 6u << 6;
    rl_fn to = rl_mips_guard_exit(c, mem, RL_MIPS_GUARD_FP_SLOW | RL_MIPS_GUARD_IN_DELAY, K0(0x1000), jal, add);
    CHECK_EQ(c->r[31], K0(0x1008));
    CHECK_EQ(c->f[6], 0x40000000u);
    CHECK_EQ(c->pc, K0(0x2000));
    CHECK(to == leaf);
    c->f[2] = 1;
    to = rl_mips_guard_exit(c, mem, RL_MIPS_GUARD_FP_SLOW | RL_MIPS_GUARD_IN_DELAY, K0(0x1000), jal, add);
    CHECK(to == rl_mips_pending);
    CHECK_EQ(c->cp0_epc, K0(0x1000));
    CHECK_EQ(c->cp0_cause & RL_MIPS_CAUSE_BD, RL_MIPS_CAUSE_BD);
    c->cp0_status = RL_MIPS_SR_CU1 | RL_MIPS_SR_FR;
    c->r[1] = 5;
    c->r[2] = 6;
    c->f[2] = 1;
    uint32_t beql = 0x14u << 26 | 1u << 21 | 2u << 16 | 0x10u;
    to = rl_mips_guard_exit(c, mem, RL_MIPS_GUARD_FP_SLOW | RL_MIPS_GUARD_IN_DELAY, K0(0x1000), beql, add);
    CHECK_EQ(c->pc, K0(0x1008));
    CHECK(to == rl_dispatch_miss);
}

TEST(mips_calls_return_through_the_frame_stack)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    rl_mips_get_stats()->returns = 0;
    c->r[2] = 0;
    int r = rl_mips_run(c, test_mem, K0(0x1000));
    CHECK_EQ(r, RL_PENDING);
    CHECK_EQ(c->r[2], 2);
    CHECK_EQ(hs.after, 1);
    CHECK_EQ(rl_mips_get_stats()->returns, 2);
    CHECK_EQ(c->depth, 0);
}

TEST(mips_interrupt_entry_and_eret_close_the_frame)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->cp0_status = RL_MIPS_SR_IE | 0x0400u;
    g_ip = 1u << 2;
    int r = rl_mips_run(c, test_mem, K0(0x4000));
    CHECK_EQ(r, RL_PENDING);
    CHECK_EQ(hs.handler_runs, 1);
    CHECK_EQ(hs.epc_seen, K0(0x4004));
    CHECK_EQ(hs.after, 1);
    CHECK_EQ(c->depth, 0);
    CHECK_EQ(c->irq_open, 0);
    CHECK_EQ(c->cp0_status & RL_MIPS_SR_EXL, 0);
    c->cp0_status = RL_MIPS_SR_IE | 0x0400u | RL_MIPS_SR_EXL;
    hs.after = 0;
    rl_mips_run(c, test_mem, K0(0x4000));
    CHECK_EQ(hs.handler_runs, 1);
    CHECK_EQ(hs.after, 1);
}

TEST(mips_eret_elsewhere_redirects_out_of_the_interrupt)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    g_vector = switcher;
    c->cp0_status = RL_MIPS_SR_IE | 0x0400u;
    g_ip = 1u << 2;
    int r = rl_mips_run(c, test_mem, K0(0x4000));
    CHECK_EQ(r, RL_PENDING);
    CHECK_EQ(hs.handler_runs, 1);
    CHECK_EQ(hs.after, 0);
    CHECK_EQ(c->r[3], 0x5A);
    CHECK_EQ(c->depth, 0);
}

static int cache_calls;
static uint64_t cache_va;

static void on_cache(rl_ctx *c, uint32_t op, uint64_t vaddr)
{
    (void)c;
    cache_calls++;
    cache_va = vaddr | op;
}

TEST(mips_cache_calls_the_hook_when_it_runs)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    rl_mips_cache(c, 0x10, K0(0x100), 1);
    rl_mips_set_cache_hook(on_cache);
    rl_mips_cache(c, 0x10, K0(0x100), 0);
    CHECK_EQ(cache_calls, 0);
    rl_mips_cache(c, 0x10, K0(0x100), 1);
    CHECK_EQ(cache_calls, 1);
    CHECK_EQ(cache_va, K0(0x110));
    rl_mips_set_cache_hook(NULL);
}


TEST(mips_icache_fills_a_line_once_and_never_for_kseg1)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    memset(c->icache, 0, sizeof c->icache);
    CHECK_EQ(CYCLES(c, rl_mips_icache(c, K0(0x1000))), 48);
    CHECK_EQ(CYCLES(c, rl_mips_icache(c, K0(0x101C))), 0);
    CHECK_EQ(CYCLES(c, rl_mips_icache(c, K0(0x1020))), 48);
    CHECK_EQ(CYCLES(c, rl_mips_icache(c, K0(0x5000))), 48);
    CHECK_EQ(CYCLES(c, rl_mips_icache(c, K0(0x1000))), 48);
    CHECK_EQ(CYCLES(c, rl_mips_icache(c, UINT64_C(0xFFFFFFFFA0001000))), 0);
    rl_mips_cache(c, 0x10, K0(0x1000), 1);
    CHECK_EQ(CYCLES(c, rl_mips_icache(c, K0(0x1000))), 48);
    rl_mips_cache(c, 0x04, K0(0x1000), 1);
    CHECK_EQ(c->cp0_tag_lo, 0x80u | 0x100u);
    rl_mips_cache(c, 0x00, K0(0x1000), 1);
    CHECK_EQ(CYCLES(c, rl_mips_icache(c, K0(0x1000))), 48);
}

TEST(mips_dcache_hits_fills_and_writes_back_dirty_lines)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    memset(c->dcache, 0, sizeof c->dcache);
    CHECK_EQ(CYCLES(c, rl_mips_dcache(c, K0(0x100), 0, 1)), 40);
    CHECK_EQ(CYCLES(c, rl_mips_dcache(c, K0(0x10C), 1, 1)), 1);
    CHECK_EQ(CYCLES(c, rl_mips_dcache(c, K0(0x10C), 1, 0)), 0);
    CHECK_EQ(CYCLES(c, rl_mips_dcache(c, K0(0x2100), 0, 1)), 80);
    CHECK_EQ(CYCLES(c, rl_mips_dcache(c, K0(0x100), 0, 1)), 40);
    CHECK_EQ(CYCLES(c, rl_mips_dcache(c, UINT64_C(0xFFFFFFFFA0000100), 1, 1)), 0);
    CHECK_EQ(CYCLES(c, rl_mips_dcache(c, K0(0x100), 1, 1)), 1);
    CHECK_EQ(CYCLES(c, rl_mips_cache(c, 0x19, K0(0x100), 1)), 40);
    CHECK_EQ(CYCLES(c, rl_mips_cache(c, 0x19, K0(0x100), 1)), 0);
    rl_mips_dcache(c, K0(0x100), 1, 1);
    CHECK_EQ(CYCLES(c, rl_mips_cache(c, 0x15, K0(0x100), 1)), 40);
    CHECK_EQ(CYCLES(c, rl_mips_dcache(c, K0(0x100), 0, 1)), 40);
    CHECK_EQ(CYCLES(c, rl_mips_cache(c, 0x0D, K0(0x3100), 1)), 0);
    CHECK_EQ(CYCLES(c, rl_mips_dcache(c, K0(0x3104), 1, 1)), 1);
    CHECK_EQ(CYCLES(c, rl_mips_cache(c, 0x01, K0(0x3100), 1)), 40);
    CHECK_EQ(CYCLES(c, rl_mips_dcache(c, K0(0x3104), 0, 1)), 40);
}

TEST(mips_crash_reports_name_the_cause_and_registers)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    c->cp0_cause = RL_MIPS_EXC_TLBL << 2;
    c->r[3] = 0xCAFEF00Du;
    char buf[1024];
    rl_mips_cpu()->describe(c, buf, sizeof buf);
    CHECK(strstr(buf, "(TLBL)") != NULL);
    CHECK(strstr(buf, "r3 00000000cafef00d") != NULL);
}
