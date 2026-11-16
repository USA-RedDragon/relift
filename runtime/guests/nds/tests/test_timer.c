/**
 * @file test_timer.c
 * @brief The timers of both CPUs (counting in bus cycles, two ticks each)
 * and the ARM9's math unit.
 */
#include "ndst.h"

#define TM0 0x04000100u
#define TM1 0x04000104u
#define IE 0x04000210u
#define IF 0x04000214u
#define IME 0x04000208u
#define DIVCNT 0x04000280u
#define NUMER 0x04000290u
#define DENOM 0x04000298u
#define RESULT 0x040002A0u
#define REM 0x040002A8u
#define SQRTCNT 0x040002B0u
#define SQRTRES 0x040002B4u
#define PARAM 0x040002B8u

TEST(timers_count_bus_cycles_from_the_cycle_after_the_start)
{
    ndst *t = ndst_new();
    w7(t, TM0, 4, 0x00C0FFF0u);
    ndst_both(t, 4);
    CHECK_EQ(r7(t, TM0, 2), 0xFFF0);
    CHECK_EQ(r7(t, TM0 + 2, 2), 0xC0);
    ndst_both(t, 6);
    CHECK_EQ(r7(t, TM0, 2), 0xFFF1);
    ndst_both(t, 20);
    CHECK_EQ(r7(t, TM0, 2), 0xFFF8);
    ndst_both(t, 34);
    CHECK_EQ(r7(t, TM0, 2), 0xFFFF);
    CHECK_EQ(r7(t, IF, 4), 0);
    ndst_both(t, 36);
    CHECK_EQ(r7(t, IF, 4), RL_NDS_IRQ_TIMER0);
    CHECK_EQ(r7(t, TM0, 2), 0xFFF0);
    CHECK_EQ(r9(t, IF, 4), 0);
    ndst_both(t, 36 + 32);
    CHECK_EQ(r7(t, TM0, 2), 0xFFF0);
    CHECK_EQ(r9(t, TM0, 2), 0);
}

TEST(timer_overflow_irq_reaches_the_input_two_ticks_later)
{
    ndst *t = ndst_new();
    rl_nds *m = t->m;
    w9(t, IE, 4, RL_NDS_IRQ_TIMER0 << 2);
    w9(t, IME, 4, 1);
    w9(t, TM0 + 8, 4, 0x00C0FF00u);
    ndst_both(t, 2 * 257);
    CHECK_EQ(r9(t, IF, 4), 0);
    ndst_both(t, 2 * 258);
    CHECK_EQ(r9(t, IF, 4), RL_NDS_IRQ_TIMER0 << 2);
    CHECK_EQ(rl_nds_irq_at(m, RL_NDS_ARM9), 2 * 258 + 2);
    CHECK(!rl_nds_irq_line(m, RL_NDS_ARM9));
    ndst_both(t, 2 * 258 + 2);
    CHECK(rl_nds_irq_line(m, RL_NDS_ARM9));
}

TEST(timer_prescalers_divide_the_bus_clock)
{
    ndst *t = ndst_new();
    w7(t, TM0, 4, 0x00810000u);
    ndst_both(t, 128);
    CHECK_EQ(r7(t, TM0, 2), 0);
    ndst_both(t, 130);
    CHECK_EQ(r7(t, TM0, 2), 1);
    ndst_both(t, 2 * (64 * 10 + 1));
    CHECK_EQ(r7(t, TM0, 2), 10);
    w9(t, TM0, 4, 0x00830000u);
    ndst_both(t, 2 * 1025);
    CHECK_EQ(r9(t, TM0, 2), 1);
    ndst_both(t, 2 * 2049);
    CHECK_EQ(r9(t, TM0, 2), 2);
}

TEST(timer_count_up_follows_the_previous_overflow)
{
    ndst *t = ndst_new();
    w7(t, TM1, 4, 0x00840000u);
    w7(t, TM0, 4, 0x0080FFFEu);
    ndst_both(t, 200);
    uint32_t a = r7(t, TM1, 2);
    ndst_both(t, 240);
    uint32_t b = r7(t, TM1, 2);
    CHECK_EQ(b - a, 10);
    CHECK(a > 40 && a < 52);
    w7(t, TM0 + 2, 1, 0x00);
    ndst_both(t, 400);
    CHECK_EQ(r7(t, TM1, 2), b);
}

TEST(a_stopped_timer_holds_and_a_restart_reloads)
{
    ndst *t = ndst_new();
    w9(t, TM0, 4, 0x00801000u);
    ndst_both(t, 102);
    CHECK_EQ(r9(t, TM0, 2), 0x1000 + 49);
    w9(t, TM0 + 2, 2, 0);
    ndst_both(t, 300);
    CHECK_EQ(r9(t, TM0, 2), 0x1000 + 49 + 1);
    w9(t, TM0, 2, 0x2000);
    CHECK_EQ(r9(t, TM0, 2), 0x1000 + 50);
    w9(t, TM0 + 2, 2, 0x80);
    ndst_both(t, 304);
    CHECK_EQ(r9(t, TM0, 2), 0x2000);
}

/** @brief One division case: mode, numerator, denominator and the expected quotient, remainder and DIV0 flag. */
typedef struct div_case {
    unsigned mode;
    uint64_t n, d, q, r;
    bool div0;
} div_case;

static const div_case div_cases[] = {
    {0, 100, 7, 14, 2, false},
    {0, (uint32_t)-100, 7, (uint64_t)-14, (uint64_t)-2, false},
    {0, 5, 0, 0x00000000FFFFFFFFull, 5, true},
    {0, (uint32_t)-5, 0, 0xFFFFFFFF00000001ull, (uint64_t)-5, true},
    {0, 0x80000000u, 0xFFFFFFFFu, 0x0000000080000000ull, 0, false},
    {0, 5, 0x100000000ull, 0x00000000FFFFFFFFull, 5, false},
    {0, 0xFFFFFFFF00000064ull, 0xFFFFFFFF00000007ull, 14, 2, false},
    {1, 0x100000000ull, 2, 0x80000000u, 0, false},
    {1, UINT64_MAX, 0, 1, UINT64_MAX, true},
    {1, 0x8000000000000000ull, 0xFFFFFFFFu, 0x8000000000000000ull, 0, false},
    {1, 1000, 0x100000000ull, UINT64_MAX, 1000, false},
    {3, (uint64_t)-1000000000000ll, 1000, (uint64_t)-1000000000ll, 0, false},
    {2, 0x7FFFFFFFFFFFFFFFull, 0x100000000ull, 0x7FFFFFFFu, 0xFFFFFFFFu, false},
    {2, 7, 0, UINT64_MAX, 7, true},
    {2, (uint64_t)-7, (uint64_t)-2, 3, (uint64_t)-1, false},
};

TEST(division_modes_overflows_and_division_by_zero_follow_gbatek)
{
    ndst *t = ndst_new();
    for (unsigned i = 0; i < sizeof div_cases / sizeof div_cases[0]; i++) {
        const div_case *c = &div_cases[i];
        w9(t, DIVCNT, 2, c->mode);
        w9(t, NUMER, 4, (uint32_t)c->n);
        w9(t, NUMER + 4, 4, (uint32_t)(c->n >> 32));
        w9(t, DENOM, 4, (uint32_t)c->d);
        w9(t, DENOM + 4, 4, (uint32_t)(c->d >> 32));
        uint64_t q = r9(t, RESULT, 4) | (uint64_t)r9(t, RESULT + 4, 4) << 32;
        uint64_t r = r9(t, REM, 4) | (uint64_t)r9(t, REM + 4, 4) << 32;
        if (q != c->q || r != c->r || ((r9(t, DIVCNT, 2) >> 14) & 1) != c->div0)
            fprintf(stderr, "  case %u: q 0x%llx r 0x%llx cnt 0x%x\n", i, (unsigned long long)q, (unsigned long long)r,
                    r9(t, DIVCNT, 2));
        CHECK_EQ(q, c->q);
        CHECK_EQ(r, c->r);
        CHECK_EQ((r9(t, DIVCNT, 2) >> 14) & 1, c->div0);
        CHECK_EQ(r9(t, DIVCNT, 2) & 3, c->mode);
    }
}

TEST(the_math_unit_is_busy_for_its_time_and_only_on_the_arm9)
{
    ndst *t = ndst_new();
    ndst_both(t, 1000);
    w9(t, DIVCNT, 2, 0);
    CHECK_EQ(r9(t, DIVCNT, 2) & 0x8000, 0x8000);
    ndst_to(t, RL_NDS_ARM9, 1035);
    CHECK_EQ(r9(t, DIVCNT, 2) & 0x8000, 0x8000);
    ndst_to(t, RL_NDS_ARM9, 1036);
    CHECK_EQ(r9(t, DIVCNT, 2) & 0x8000, 0);
    w9(t, DENOM, 4, 3);
    w9(t, DIVCNT, 2, 1);
    ndst_to(t, RL_NDS_ARM9, 1036 + 67);
    CHECK_EQ(r9(t, DIVCNT, 2), 0x8001);
    ndst_to(t, RL_NDS_ARM9, 1036 + 68);
    CHECK_EQ(r9(t, DIVCNT, 2), 0x0001);
    w9(t, PARAM, 4, 16);
    CHECK_EQ(r9(t, SQRTCNT, 2), 0x8000);
    ndst_to(t, RL_NDS_ARM9, 1104 + 26);
    CHECK_EQ(r9(t, SQRTCNT, 2), 0);
    w7(t, DIVCNT, 2, 2);
    w7(t, NUMER, 4, 9);
    CHECK_EQ(r7(t, RESULT, 4), 0);
    CHECK_EQ(r9(t, DIVCNT, 2), 1);
}

TEST(square_roots_of_32_and_64_bit_inputs)
{
    ndst *t = ndst_new();
    static const struct {
        unsigned mode;
        uint64_t x;
        uint32_t root;
    } cases[] = {
        {0, 16, 4},
        {0, 17, 4},
        {0, 0xFFFFFFFFu, 0xFFFF},
        {0, 0x100000004ull, 2},
        {1, 0x100000000ull, 0x10000},
        {1, UINT64_MAX, 0xFFFFFFFFu},
        {1, 1ull << 62, 1u << 31},
        {1, 0, 0},
        {1, 999999999999ull, 999999},
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        w9(t, SQRTCNT, 2, cases[i].mode);
        w9(t, PARAM, 4, (uint32_t)cases[i].x);
        w9(t, PARAM + 4, 4, (uint32_t)(cases[i].x >> 32));
        CHECK_EQ(r9(t, SQRTRES, 4), cases[i].root);
        CHECK_EQ(r9(t, SQRTCNT, 2) & 1, cases[i].mode);
    }
}
