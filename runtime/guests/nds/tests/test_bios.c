/**
 * @file test_bios.c
 * @brief The high-level BIOS of both CPUs: the oracle's answers to a DS test
 * ROM's thousands of calls (digests, bios_oracle.h), the places where the
 * oracle's open BIOS and GBATEK disagree (GBATEK is followed), the tables,
 * and the documented behavior of the calls that touch memory or interrupt
 * state.
 */
#include "ndst.h"

#include <math.h>

#include "bios.h"
#include "nds_int.h"

#include "bios_cases.h"
#include "bios_oracle.h"

/** @brief The CPU the host-side accessors act for. */
static ndst *g_t;

u32 kit_hld(u32 a) { return rl_nds_read(g_t->m, kit_cpu == 9 ? RL_NDS_ARM9 : RL_NDS_ARM7, a, 1); }

void kit_hst(u32 a, u32 v) { rl_nds_write(g_t->m, kit_cpu == 9 ? RL_NDS_ARM9 : RL_NDS_ARM7, a, 1, v); }

/** @brief The callback the BIOS calls: the case's callback functions. */
static uint32_t hook_call(void *user, uint32_t fn, const uint32_t args[4])
{
    (void)user;
    return kit_callback_dispatch(fn, args[0], args[1], args[2]);
}

/** @brief Whether the last call ended other than by returning. */
static rl_nds_swi_status g_status;

void kit_host_swi(unsigned n, u32 *r)
{
    rl_nds_bios_hooks hooks = {.call = hook_call};
    rl_nds_bios_reset reset;
    g_status = rl_nds_bios_swi(g_t->m, kit_cpu == 9 ? RL_NDS_ARM9 : RL_NDS_ARM7, n, r, &hooks, &reset);
}

/** @brief One 32-bit value folded into a digest, as the oracle runner folds it. */
static uint32_t fold(uint32_t d, uint32_t v)
{
    for (unsigned k = 0; k < 4; k++)
        d = (d ^ (v >> (8 * k) & 0xFFu)) * 16777619u;
    return d;
}

/** @brief True when the oracle's open BIOS never finished case i of group name on cpu (so it is skipped here). */
static bool skipped(const char *name, unsigned cpu, unsigned i)
{
    for (const bios_skip *s = bios_oracle_skips; s->group[0]; s++)
        if (!strcmp(s->group, name) && s->cpu == cpu && s->index == i)
            return true;
    return false;
}

/** @brief The record fields a group's digest folds (the results GBATEK documents); r0..r3 are 0..3, the hash 4. */
static unsigned fields_of(const char *name, unsigned f[2])
{
    if (!strcmp(name, "sqrt") || !strcmp(name, "sine") || !strcmp(name, "pitch") || !strcmp(name, "vol") || !strcmp(name, "dbg")) {
        f[0] = 0;
        return 1;
    }
    if (!strcmp(name, "crc16")) {
        f[0] = 0;
        f[1] = 3;
        return 2;
    }
    f[0] = 4;
    return 1;
}

/** @brief The oracle's digest of group name on cpu, or NULL. */
static const bios_oracle *oracle_of(const char *name, unsigned cpu)
{
    for (unsigned i = 0; i < sizeof bios_oracle_digests / sizeof bios_oracle_digests[0]; i++)
        if (!strcmp(bios_oracle_digests[i].group, name) && bios_oracle_digests[i].cpu == cpu)
            return &bios_oracle_digests[i];
    return NULL;
}

/** @brief Keeps the other CPU's clock far ahead so the host thread's accesses never wait for it. */
static void settle(unsigned cpu)
{
    unsigned other = cpu == 9 ? RL_NDS_ARM7 : RL_NDS_ARM9;
    ndst_to(g_t, other, rl_sched_now(g_t->k[cpu == 9 ? RL_NDS_ARM9 : RL_NDS_ARM7]) + (1ull << 40));
}

/** @brief Runs every group of the kit on cpu and checks the digests and the cases the oracle cannot answer. */
static void sweep(unsigned cpu, unsigned *fails)
{
    g_t = ndst_new();
    kit_cpu = cpu;
    rl_nds_write(g_t->m, RL_NDS_ARM9, 0x04000247u, 1, 0);
    for (u32 g = 0; g < KIT_NGROUPS; g++) {
        const group *gr = &kit_groups[g];
        if (!(gr->cpus & (cpu == 9 ? G9 : G7)) || !strcmp(gr->name, "wait") || !strcmp(gr->name, "div"))
            continue;
        const bios_oracle *o = oracle_of(gr->name, cpu);
        unsigned f[2], nf = fields_of(gr->name, f), folded = 0;
        uint32_t d = 2166136261u;
        for (u32 i = 0; i < gr->n; i++) {
            rec r;
            settle(cpu);
            kit_case(gr, i, &r);
            const uint32_t v[5] = {r.r[0], r.r[1], r.r[2], r.r[3], r.h};
            bool oracle_case = o && !skipped(gr->name, cpu, i) && !((!strcmp(gr->name, "lzcb") || !strcmp(gr->name, "rlcb")) && (i % 20) >= 18) &&
                               !(!strcmp(gr->name, "sqrt") && r.aux >= 0x80000000u);
            if (oracle_case) {
                for (unsigned k = 0; k < nf; k++)
                    d = fold(d, v[f[k]]);
                folded++;
            }
            if (!strcmp(gr->name, "sqrt") && !(r.r[0] <= 0xFFFFu && (uint64_t)r.r[0] * r.r[0] <= r.aux && (uint64_t)(r.r[0] + 1) * (r.r[0] + 1) > r.aux)) {
                fprintf(stderr, "sqrt(%08x) = %08x\n", r.aux, r.r[0]);
                (*fails)++;
            }
            if (!strcmp(gr->name, "lzcb") || !strcmp(gr->name, "rlcb")) {
                u32 want = (i % 20) == 18 ? (u32)-5 : (i % 20) == 19 ? (u32)-7 : ld32(A_SRC) >> 8;
                if (r.r[0] != want) {
                    fprintf(stderr, "%s case %u returns %08x, GBATEK says %08x\n", gr->name, i, r.r[0], want);
                    (*fails)++;
                }
            }
            if (!strcmp(gr->name, "huff")) {
                u32 want = (i % 25) == 23 ? (u32)-5 : (i % 25) == 24 ? (u32)-7 : ld32(A_SRC) >> 8;
                bool bad = r.r[0] != want;
                for (u32 k = 0; (i % 25) < 23 && k < huff_exp_len; k++)
                    bad = bad || ld8(A_DST + k) != huff_exp[k];
                if (bad) {
                    fprintf(stderr, "huff case %u is wrong\n", i);
                    (*fails)++;
                }
            }
            if (!strcmp(gr->name, "cpuset_bios") && cpu == 7 && ld8(A_DST) != 0x5C) {
                fprintf(stderr, "CpuSet from the BIOS region copied on the ARM7\n");
                (*fails)++;
            }
        }
        if (o && (folded != o->cases || d != o->digest)) {
            fprintf(stderr, "%s on the ARM%u: %u cases digest %08x, the oracle's %u cases digest %08x\n", gr->name, cpu, folded, d, o ? o->cases : 0, o ? o->digest : 0);
            (*fails)++;
        }
    }
}

TEST(arm9_calls_match_the_oracle)
{
    unsigned fails = 0;
    sweep(9, &fails);
    CHECK_EQ(fails, 0);
}

TEST(arm7_calls_match_the_oracle)
{
    unsigned fails = 0;
    sweep(7, &fails);
    CHECK_EQ(fails, 0);
}

TEST(div_follows_gbatek_on_the_edge_grid_and_matches_the_oracle_for_non_negative_numerators)
{
    g_t = ndst_new();
    const bios_oracle *o = oracle_of("div", 9);
    CHECK(o != NULL);
    for (unsigned cpu = 9; cpu >= 7; cpu -= 2) {
        kit_cpu = cpu;
        uint32_t d = 2166136261u;
        unsigned folded = 0;
        for (unsigned a = 0; a < NEDGE; a++)
            for (unsigned b = 1; b < NEDGE; b++) {
                if (skipped("div", 9, a * (NEDGE - 1) + b - 1))
                    continue;
                u32 r[4] = {edge_vals[a], edge_vals[b], PRE_R2, PRE_R3};
                settle(cpu);
                do_swi(0x09, r);
                int64_t n = (int32_t)edge_vals[a], m = (int32_t)edge_vals[b], q = n / m;
                CHECK_EQ(r[0], (uint32_t)q);
                CHECK_EQ(r[1], (uint32_t)(n - q * m));
                CHECK_EQ(r[3], (uint32_t)(q < 0 ? -q : q));
                if (edge_vals[a] & 0x80000000u)
                    continue;
                d = fold(fold(fold(d, r[0]), r[1]), r[3]);
                folded++;
            }
        CHECK_EQ(folded, o->cases);
        CHECK_EQ(d, o->digest);
    }
}

TEST(sqrt_of_the_top_half_of_the_range_is_the_documented_integer_root)
{
    g_t = ndst_new();
    kit_cpu = 7;
    u32 r[4] = {0x80000000u, 0, 0, 0};
    do_swi(0x0D, r);
    CHECK_EQ(r[0], 0xB504);
    r[0] = 0xFFFFFFFFu;
    do_swi(0x0D, r);
    CHECK_EQ(r[0], 0xFFFF);
}

TEST(div_by_zero_never_returns)
{
    g_t = ndst_new();
    kit_cpu = 9;
    u32 r[4] = {5, 0, 0, 0};
    do_swi(0x09, r);
    CHECK_EQ(g_status, RL_NDS_SWI_HANG);
}

TEST(div_example_of_gbatek)
{
    g_t = ndst_new();
    kit_cpu = 7;
    u32 r[4] = {(u32)-1234, 10, 0, 0};
    do_swi(0x09, r);
    CHECK_EQ(r[0], (u32)-123);
    CHECK_EQ(r[1], (u32)-4);
    CHECK_EQ(r[3], 123);
}

TEST(wait_by_loop_leaves_the_loop_counter_as_the_oracle_does)
{
    g_t = ndst_new();
    kit_cpu = 7;
    u32 r[4] = {10, 0, 0, 0};
    uint64_t t0 = rl_sched_now(g_t->k[RL_NDS_ARM7]);
    settle(7);
    do_swi(0x03, r);
    CHECK_EQ(r[0], 0);
    CHECK(rl_sched_now(g_t->k[RL_NDS_ARM7]) - t0 >= 2 * 4 * 10);
    r[0] = 0;
    do_swi(0x03, r);
    CHECK_EQ(r[0], 0xFFFFFFFFu);
    r[0] = 0xFFFFFFFFu;
    do_swi(0x03, r);
    CHECK_EQ(r[0], 0xFFFFFFFEu);
}

TEST(the_tables_follow_their_formulas)
{
    for (unsigned i = 0; i < 64; i++)
        CHECK_EQ(rl_nds_bios_sine(i), (uint32_t)llround(32767.0 * sin(i * 3.14159265358979323846 / 128.0)));
    for (unsigned i = 0; i < 768; i++)
        CHECK_EQ(rl_nds_bios_pitch(i), (uint32_t)llround(65536.0 * (pow(2.0, i / 768.0) - 1.0)));
    CHECK_EQ(rl_nds_bios_sine(63), 0x7FF5);
    CHECK_EQ(rl_nds_bios_pitch(767), 0xFF8A);
    CHECK_EQ(rl_nds_bios_volume(0), 0);
    CHECK_EQ(rl_nds_bios_volume(723), 0x7F);
    for (unsigned i = 1; i < 724; i++)
        CHECK(rl_nds_bios_volume(i) <= 0x7F);
}

TEST(crc16_of_gbatek_example_bytes)
{
    g_t = ndst_new();
    kit_cpu = 9;
    for (unsigned i = 0; i < 8; i++)
        st8(A_SRC + i, 0x10 + i);
    u32 r[4] = {0xFFFF, A_SRC, 8, 0};
    settle(9);
    do_swi(0x0E, r);
    uint32_t crc = 0xFFFF;
    for (unsigned i = 0; i < 8; i++) {
        crc ^= 0x10 + i;
        for (unsigned j = 0; j < 8; j++)
            crc = crc & 1 ? (crc >> 1) ^ 0xA001 : crc >> 1;
    }
    CHECK_EQ(r[0], crc);
    CHECK_EQ(r[3], 0x1716);
}

TEST(cpu_set_from_the_arm7_bios_region_is_refused_and_the_arm9_reads_it)
{
    g_t = ndst_new();
    kit_cpu = 7;
    st32(A_DST, 0x12345678u);
    u32 r[4] = {0x00000100u, A_DST, 1u | 1u << 26, 0};
    settle(7);
    do_swi(0x0B, r);
    CHECK_EQ(ld32(A_DST), 0x12345678u);
    kit_cpu = 9;
    r[0] = 0x0200F000u;
    r[1] = A_DST;
    r[2] = 1u | 1u << 26 | 1u << 24;
    st32(0x0200F000u, 0xCAFEF00Du);
    settle(9);
    do_swi(0x0B, r);
    CHECK_EQ(ld32(A_DST), 0xCAFEF00Du);
}

TEST(cpu_fast_set_takes_any_word_count)
{
    g_t = ndst_new();
    kit_cpu = 9;
    for (unsigned i = 0; i < 12; i++) {
        st32(A_SRC + 4 * i, 0xA0000000u + i);
        st32(A_DST + 4 * i, 0);
    }
    u32 r[4] = {A_SRC, A_DST, 5, 0};
    settle(9);
    do_swi(0x0C, r);
    for (unsigned i = 0; i < 12; i++)
        CHECK_EQ(ld32(A_DST + 4 * i), i < 5 ? 0xA0000000u + i : 0);
}

TEST(the_bios_regions_read_back_ff_on_the_arm7_and_zero_on_the_arm9)
{
    ndst *t = ndst_new();
    g_t = t;
    ndst_to(t, RL_NDS_ARM7, (1ull << 40));
    CHECK_EQ(rl_nds_load(t->m, RL_NDS_ARM9, 0xFFFF0000u, 4, false), 0);
    CHECK_EQ(rl_nds_load(t->m, RL_NDS_ARM9, 0xFFFF7FFCu, 4, false), 0);
    ndst_to(t, RL_NDS_ARM9, (1ull << 41));
    CHECK_EQ(rl_nds_load(t->m, RL_NDS_ARM7, 0x00000000u, 4, false), 0xFFFFFFFFu);
    CHECK_EQ(rl_nds_load(t->m, RL_NDS_ARM7, 0x00003FFEu, 2, false), 0xFFFF);
    CHECK_EQ(rl_nds_load(t->m, RL_NDS_ARM7, 0x00001204u, 1, false), 0xFF);
}

TEST(numbers_no_routine_answers_jump_to_zero_per_cpu)
{
    g_t = ndst_new();
    static const unsigned bad7[] = {0x01, 0x02, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1E, 0x20, 0x7F, 0xFF};
    static const unsigned bad9[] = {0x01, 0x02, 0x07, 0x08, 0x0A, 0x17, 0x19, 0x1A, 0x1B, 0x1C, 0x1D, 0x1E, 0x20, 0x7F, 0xFF};
    for (unsigned i = 0; i < sizeof bad7 / sizeof bad7[0]; i++) {
        u32 r[4] = {0, 0, 0, 0};
        rl_nds_bios_reset reset;
        CHECK_EQ(rl_nds_bios_swi(g_t->m, RL_NDS_ARM7, bad7[i], r, NULL, &reset), RL_NDS_SWI_INVALID);
    }
    for (unsigned i = 0; i < sizeof bad9 / sizeof bad9[0]; i++) {
        u32 r[4] = {0, 0, 0, 0};
        rl_nds_bios_reset reset;
        CHECK_EQ(rl_nds_bios_swi(g_t->m, RL_NDS_ARM9, bad9[i], r, NULL, &reset), RL_NDS_SWI_INVALID);
    }
}

TEST(soft_reset_clears_the_top_of_each_cpus_ram_and_gives_the_documented_stacks)
{
    g_t = ndst_new();
    rl_nds_write(g_t->m, RL_NDS_ARM7, 0x0380FE00u, 4, 0xFFFFFFFFu);
    rl_nds_write(g_t->m, RL_NDS_ARM7, 0x0380FFFCu, 4, 0xFFFFFFFFu);
    rl_nds_write(g_t->m, RL_NDS_ARM7, 0x027FFE34u, 4, 0x02380001u);
    settle(7);
    u32 r[4] = {0, 0, 0, 0};
    rl_nds_bios_reset reset = {0};
    CHECK_EQ(rl_nds_bios_swi(g_t->m, RL_NDS_ARM7, 0x00, r, NULL, &reset), RL_NDS_SWI_RESET);
    CHECK_EQ(rl_nds_read(g_t->m, RL_NDS_ARM7, 0x0380FE00u, 4), 0);
    CHECK_EQ(rl_nds_read(g_t->m, RL_NDS_ARM7, 0x0380FFFCu, 4), 0);
    CHECK_EQ(reset.pc, 0x02380001u);
    CHECK_EQ(reset.sp_sys, 0x0380FF00u);
    CHECK_EQ(reset.sp_irq, 0x0380FFB0u);
    CHECK_EQ(reset.sp_svc, 0x0380FFDCu);
    CHECK(!reset.cp15);
    rl_nds_write(g_t->m, RL_NDS_ARM9, 0x027FFE24u, 4, 0x02000800u);
    ndst_to(g_t, RL_NDS_ARM7, rl_sched_now(g_t->k[RL_NDS_ARM7]) + (1ull << 40));
    CHECK_EQ(rl_nds_bios_swi(g_t->m, RL_NDS_ARM9, 0x00, r, NULL, &reset), RL_NDS_SWI_RESET);
    CHECK_EQ(reset.pc, 0x02000800u);
    CHECK_EQ(reset.sp_sys, 0x00803EC0u);
    CHECK_EQ(reset.sp_irq, 0x00803FA0u);
    CHECK_EQ(reset.sp_svc, 0x00803FC0u);
    CHECK(reset.cp15);
}

TEST(custom_post_writes_postflg_and_boot_procs_report_unknown_values)
{
    g_t = ndst_new();
    u32 r[4] = {1, 2, 3, 4};
    rl_nds_bios_reset reset;
    settle(9);
    CHECK_EQ(rl_nds_bios_swi(g_t->m, RL_NDS_ARM9, 0x1F, r, NULL, &reset), RL_NDS_SWI_OK);
    CHECK_EQ(r9(g_t, 0x04000300u, 1), 1);
    ndst_to(g_t, RL_NDS_ARM9, rl_sched_now(g_t->k[RL_NDS_ARM7]) + (1ull << 40));
    CHECK_EQ(rl_nds_bios_swi(g_t->m, RL_NDS_ARM7, 0x1D, r, NULL, &reset), RL_NDS_SWI_UNKNOWN_RESULT);
    CHECK_EQ(r[0] | r[1] | r[2] | r[3], 0);
}

TEST(is_debugger_reports_a_normal_console_and_destroys_the_scratch_halfword)
{
    g_t = ndst_new();
    kit_cpu = 7;
    st16(0x027FFFFAu, 0x1234);
    u32 r[4] = {0, 0, 0, 0};
    settle(7);
    do_swi(0x0F, r);
    CHECK_EQ(r[0], 0);
    CHECK_EQ(ld16(0x027FFFFAu), 0xFFFF);
    kit_cpu = 9;
    ndst_to(g_t, RL_NDS_ARM7, rl_sched_now(g_t->k[RL_NDS_ARM9]) + (1ull << 40));
    st16(0x027FFFF8u, 0x4321);
    do_swi(0x0F, r);
    CHECK_EQ(r[0], 0);
    CHECK_EQ(ld16(0x027FFFF8u), 0xFFFF);
}

/** @brief The BIOS flag word of the ARM7 (the protocol of IntrWait): the handler stand-in sets it. */
#define FLAGS7 0x0380FFF8u

/** @brief The CPU and flag word address the handler stand-in acts for. */
static unsigned g_hcpu;
static uint32_t g_hflags;

/** @brief An interrupt window that acts as the game's handler: acknowledges IF and ORs it into the flag word. */
static bool handler_standin(void *user)
{
    ndst *t = user;
    uint32_t iflag = rl_nds_read(t->m, g_hcpu, 0x04000214u, 4);
    if (!iflag)
        return false;
    rl_nds_write(t->m, g_hcpu, 0x04000214u, 4, iflag);
    rl_nds_write(t->m, g_hcpu, g_hflags, 4, rl_nds_read(t->m, g_hcpu, g_hflags, 4) | iflag);
    return true;
}

TEST(intr_wait_forces_ime_waits_for_a_new_flag_and_clears_it)
{
    ndst *t = ndst_new();
    g_t = t;
    g_hcpu = RL_NDS_ARM7;
    g_hflags = FLAGS7;
    rl_nds_write(t->m, RL_NDS_ARM7, 0x04000210u, 4, RL_NDS_IRQ_VBLANK);
    rl_nds_write(t->m, RL_NDS_ARM7, FLAGS7, 4, RL_NDS_IRQ_VBLANK);
    rl_nds_request_irq(t->m, RL_NDS_ARM7, RL_NDS_IRQ_VBLANK);
    ndst_to(t, RL_NDS_ARM9, (1ull << 40));
    rl_nds_bios_hooks hooks = {.user = t, .irq = handler_standin};
    rl_nds_bios_reset reset;
    u32 r[4] = {1, RL_NDS_IRQ_VBLANK, 0, 0};
    CHECK_EQ(rl_nds_bios_swi(t->m, RL_NDS_ARM7, 0x04, r, &hooks, &reset), RL_NDS_SWI_OK);
    CHECK_EQ(rl_nds_read(t->m, RL_NDS_ARM7, 0x04000208u, 4), 1);
    CHECK_EQ(rl_nds_read(t->m, RL_NDS_ARM7, FLAGS7, 4), 0);
}

TEST(intr_wait_returns_at_once_on_the_arm7_when_an_old_flag_is_set_and_r0_is_zero)
{
    ndst *t = ndst_new();
    g_t = t;
    g_hcpu = RL_NDS_ARM7;
    g_hflags = FLAGS7;
    rl_nds_write(t->m, RL_NDS_ARM7, FLAGS7, 4, RL_NDS_IRQ_VBLANK | RL_NDS_IRQ_TIMER0);
    ndst_to(t, RL_NDS_ARM9, (1ull << 40));
    rl_nds_bios_hooks hooks = {.user = t, .irq = handler_standin};
    rl_nds_bios_reset reset;
    u32 r[4] = {0, RL_NDS_IRQ_VBLANK, 0, 0};
    CHECK_EQ(rl_nds_bios_swi(t->m, RL_NDS_ARM7, 0x04, r, &hooks, &reset), RL_NDS_SWI_OK);
    CHECK_EQ(rl_nds_read(t->m, RL_NDS_ARM7, FLAGS7, 4), RL_NDS_IRQ_TIMER0);
}

TEST(arm9_intr_wait_with_r0_zero_still_waits_for_an_interrupt_the_documented_bug)
{
    ndst *t = ndst_new();
    g_t = t;
    rl_nds_cp15_write(t->m, RL_CP15_KEY(0, 9, 1, 0), 0x0080000Au);
    rl_nds_cp15_write(t->m, RL_CP15_KEY(0, 1, 0, 0), rl_nds_cp15(t->m)->control | RL_CP15_DTCM_ON);
    uint32_t flags9 = 0x00803FF8u;
    g_hcpu = RL_NDS_ARM9;
    g_hflags = flags9;
    rl_nds_write(t->m, RL_NDS_ARM9, 0x04000210u, 4, RL_NDS_IRQ_TIMER0);
    rl_nds_write(t->m, RL_NDS_ARM9, flags9, 4, RL_NDS_IRQ_VBLANK);
    rl_nds_request_irq(t->m, RL_NDS_ARM9, RL_NDS_IRQ_TIMER0);
    ndst_to(t, RL_NDS_ARM7, (1ull << 40));
    rl_nds_bios_hooks hooks = {.user = t, .irq = handler_standin};
    rl_nds_bios_reset reset;
    u32 r[4] = {0, RL_NDS_IRQ_VBLANK, 0, 0};
    uint64_t t0 = rl_sched_now(t->k[RL_NDS_ARM9]);
    CHECK_EQ(rl_nds_bios_swi(t->m, RL_NDS_ARM9, 0x04, r, &hooks, &reset), RL_NDS_SWI_OK);
    CHECK(rl_sched_now(t->k[RL_NDS_ARM9]) > t0);
}

TEST(sound_bias_steps_to_the_level_and_waits_between_steps)
{
    g_t = ndst_new();
    kit_cpu = 7;
    w7(g_t, 0x04000504u, 2, 0xC080u);
    settle(7);
    u32 r[4] = {1, 2, 0, 0};
    uint64_t t0 = rl_sched_now(g_t->k[RL_NDS_ARM7]);
    do_swi(0x08, r);
    CHECK_EQ(r7(g_t, 0x04000504u, 2), 0x0200u);
    CHECK(rl_sched_now(g_t->k[RL_NDS_ARM7]) - t0 >= 2 * (0x200 - 0x80) * (2 * 4 + 8));
    r[0] = 0;
    r[1] = 0;
    settle(7);
    do_swi(0x08, r);
    CHECK_EQ(r7(g_t, 0x04000504u, 2), 0x0000u);
}

TEST(the_arm9_wait_loop_costs_by_cache_state_as_gbatek_tabulates)
{
    g_t = ndst_new();
    kit_cpu = 9;
    u32 r[4] = {1000, 0, 0, 0};
    settle(9);
    uint64_t t0 = rl_sched_now(g_t->k[RL_NDS_ARM9]);
    do_swi(0x03, r);
    uint64_t uncached = rl_sched_now(g_t->k[RL_NDS_ARM9]) - t0;
    CHECK(uncached >= 16 * 1000 && uncached < 16 * 1000 + 100);
    rl_nds_cp15_write(g_t->m, RL_CP15_KEY(0, 6, 0, 0), 0xFFFF0000u | 31u << 1 | 1u);
    rl_nds_cp15_write(g_t->m, RL_CP15_KEY(0, 2, 0, 1), 1);
    rl_nds_cp15_write(g_t->m, RL_CP15_KEY(0, 1, 0, 0), rl_nds_cp15(g_t->m)->control | RL_CP15_MPU | RL_CP15_ICACHE);
    r[0] = 1000;
    settle(9);
    t0 = rl_sched_now(g_t->k[RL_NDS_ARM9]);
    do_swi(0x03, r);
    uint64_t cached = rl_sched_now(g_t->k[RL_NDS_ARM9]) - t0;
    CHECK(cached >= 4 * 1000 && cached < 4 * 1000 + 100);
}
