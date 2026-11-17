/**
 * @file test_q3.c
 * @brief The two CPUs on their fibers with hand-written lifted-style cores
 * (the scenarios of spike 20-nds q3): FIFO ping-pong, a shared work RAM
 * hand-over with an IPCSYNC acknowledge, a racy flag in main RAM with bus
 * contention, an IPCSYNC IRQ to a halted or a computing ARM7, and main RAM
 * contention end times. Each scenario runs in two ways on the same machine:
 * a per-instruction lockstep reference (before every instruction the CPU
 * first in the order runs) and the fused way lifted code runs (time added
 * per block, waits only at shared accesses, IRQs at block boundaries). The
 * logs (tag, value, tick), end times and memory must match, and a second
 * fused run must repeat the first.
 */
#include "ndst.h"

#include <stdlib.h>

/** @brief A tick no scenario reaches unless an edge was lost. */
#define CAP 60000000ull
/** @brief Log entries per CPU. */
#define LOG_MAX 16384

#define IME 0x04000208u
#define IE 0x04000210u
#define IF 0x04000214u
#define SYNC 0x04000180u
#define FCNT 0x04000184u
#define FSEND 0x04000188u
#define FRECV 0x04100000u
#define WRAMCNT 0x04000247u
#define WRAMSTAT 0x04000241u
#define HALTCNT 0x04000301u

/** @brief How the cores run. */
typedef enum mode {
    /** @brief The reference: an order wait and an IRQ check before every instruction. */
    LOCKSTEP,
    /** @brief Fused blocks, waits at shared accesses, IRQs at block boundaries. */
    FUSED,
    /** @brief FUSED, but an IRQ check before every instruction while IRQs are enabled. */
    FUSED_IRQ_EXACT,
} mode;

/** @brief One log entry. */
typedef struct ent {
    uint32_t tag, val;
    uint64_t t;
} ent;

typedef struct core core;
/** @brief An interrupt handler of a test core. */
typedef void handler_fn(core *c);
/** @brief A test core's program. */
typedef void prog_fn(core *c);

/** @brief A lifted-style core. */
struct core {
    unsigned id;
    rl_sched_cpu *k;
    bool irq_en, aborted;
    handler_fn *handler;
    uint32_t handled;
    uint32_t nlog;
    ent log[LOG_MAX];
};

/** @brief The result of a run. */
typedef struct result {
    ent log[2][LOG_MAX];
    uint32_t nlog[2];
    uint64_t end[2];
    bool aborted[2];
    uint64_t hash;
    uint64_t ns, orders, waits, switches;
} result;

static struct {
    rl_nds *m;
    rl_sched *s;
    mode mode;
    int n, k, l;
    prog_fn *prog[2];
    core c[2];
} Q;

static uint64_t now_of(core *c) { return rl_sched_now(c->k); }

static void logv(core *c, uint32_t tag, uint32_t val)
{
    if (c->nlog < LOG_MAX)
        c->log[c->nlog++] = (ent){tag, val, now_of(c)};
}

static void spend(core *c, int64_t cycles)
{
    rl_time *t = rl_sched_clock(c->k);
    t->budget -= cycles;
    if (t->budget <= 0)
        rl_sched_run(c->k);
}

static void take_irqs(core *c)
{
    if (c->irq_en && rl_nds_irq_poll(Q.m, c->id)) {
        c->irq_en = false;
        c->handler(c);
        c->irq_en = true;
    }
}

/** @brief The reference's step before an instruction: the CPU first in the order runs, then takes a pending IRQ. */
static void step(core *c)
{
    rl_sched_order(c->k);
    take_irqs(c);
}

/** @brief n private instructions of one cycle each. */
static void priv(core *c, int n)
{
    if (Q.mode == LOCKSTEP || (Q.mode == FUSED_IRQ_EXACT && c->irq_en)) {
        for (int i = 0; i < n; i++) {
            if (Q.mode == LOCKSTEP)
                step(c);
            else
                take_irqs(c);
            spend(c, 1);
        }
        return;
    }
    spend(c, n);
}

/** @brief A loop's back edge: a block boundary; true when the run hit the cap. */
static bool back(core *c)
{
    if (Q.mode != LOCKSTEP)
        take_irqs(c);
    if (now_of(c) > CAP) {
        c->aborted = true;
        return true;
    }
    return false;
}

static void pre(core *c)
{
    if (Q.mode == LOCKSTEP)
        step(c);
    else if (Q.mode == FUSED_IRQ_EXACT)
        take_irqs(c);
}

static uint32_t ld(core *c, uint32_t a, unsigned size)
{
    pre(c);
    return rl_nds_load(Q.m, c->id, a, size, false);
}

static void st(core *c, uint32_t a, unsigned size, uint32_t v)
{
    pre(c);
    rl_nds_store(Q.m, c->id, a, size, v, false);
}

static void halt(core *c)
{
    st(c, HALTCNT, 1, 0x80);
    take_irqs(c);
}

static void s1_arm9(core *c)
{
    uint32_t it = 0;
    st(c, FCNT, 2, 0x8000);
    for (int i = 0; i < Q.n; i++) {
        priv(c, 3 + (i * 7) % 11);
        st(c, FSEND, 4, (uint32_t)i * 3 + 1);
        logv(c, 1, (uint32_t)i);
        for (;;) {
            if (back(c))
                return;
            uint32_t v = ld(c, FCNT, 2);
            priv(c, 2);
            if (!(v & 0x100))
                break;
            it++;
        }
        logv(c, 2, ld(c, FRECV, 4));
    }
    logv(c, 9, it);
}

static void s1_arm7(core *c)
{
    uint32_t it = 0;
    st(c, FCNT, 2, 0x8000);
    for (int i = 0; i < Q.n; i++) {
        for (;;) {
            if (back(c))
                return;
            uint32_t v = ld(c, FCNT, 2);
            priv(c, 2);
            if (!(v & 0x100))
                break;
            it++;
        }
        uint32_t w = ld(c, FRECV, 4);
        logv(c, 2, w);
        priv(c, 4 + (i * 5) % 7);
        st(c, FSEND, 4, w + 1000);
    }
    logv(c, 9, it);
}

static void s2_arm9(core *c)
{
    uint32_t it = 0;
    for (int r = 0; r < Q.n; r++) {
        for (uint32_t k = 0; k < 8; k++) {
            priv(c, 2);
            st(c, 0x03000000u + 4 * k, 4, (uint32_t)r * 16 + k);
        }
        st(c, WRAMCNT, 1, 3);
        for (;;) {
            if (back(c))
                return;
            uint32_t v = ld(c, SYNC, 2);
            priv(c, 2);
            if ((v & 15) == (uint32_t)((r + 1) & 15))
                break;
            it++;
        }
        logv(c, 1, (uint32_t)r);
        st(c, WRAMCNT, 1, 0);
        priv(c, 5 + (r * 3) % 13);
    }
    logv(c, 9, it);
}

static void s2_arm7(core *c)
{
    uint32_t it1 = 0, it2 = 0, sum = 0;
    for (int r = 0; r < Q.n; r++) {
        for (;;) {
            if (back(c))
                return;
            uint32_t v = ld(c, WRAMSTAT, 1);
            priv(c, 2);
            if (v == 3)
                break;
            it1++;
        }
        logv(c, 2, it1);
        for (uint32_t k = 0; k < 8; k++) {
            sum += ld(c, 0x03000000u + 4 * k, 4);
            priv(c, 1);
        }
        st(c, SYNC, 2, (uint32_t)((r + 1) & 15) << 8);
        for (;;) {
            if (back(c))
                return;
            uint32_t v = ld(c, WRAMSTAT, 1);
            priv(c, 2);
            if (v == 0)
                break;
            it2++;
        }
        logv(c, 3, it2);
    }
    logv(c, 9, sum);
}

#define FLAG 0x02000100u
#define ACK 0x02000104u
#define DATA 0x02000110u

static void s3_arm9(core *c)
{
    uint32_t it = 0;
    for (int i = 0; i < Q.n; i++) {
        priv(c, 20 + (i * 13) % 37);
        for (uint32_t k = 0; k < 4; k++)
            st(c, DATA + 4 * k, 4, (uint32_t)i * 4 + k);
        st(c, FLAG, 4, (uint32_t)i + 1);
        logv(c, 1, (uint32_t)i);
        for (;;) {
            if (back(c))
                return;
            uint32_t v = ld(c, ACK, 4);
            priv(c, 2);
            if (v == (uint32_t)i + 1)
                break;
            it++;
        }
    }
    logv(c, 9, it);
}

static void s3_arm7(core *c)
{
    uint32_t sum = 0, it = 0;
    for (int i = 0; i < Q.n; i++) {
        uint32_t n = 0;
        for (;;) {
            if (back(c))
                return;
            uint32_t v = ld(c, FLAG, 4);
            priv(c, 2);
            if (v == (uint32_t)i + 1)
                break;
            n++;
        }
        it += n;
        logv(c, 2, n);
        for (uint32_t k = 0; k < 4; k++)
            sum += ld(c, DATA + 4 * k, 4);
        st(c, ACK, 4, (uint32_t)i + 1);
        priv(c, 3);
    }
    logv(c, 9, sum);
    logv(c, 8, it);
}

static void s4_handler(core *c)
{
    priv(c, 12);
    uint32_t v = ld(c, SYNC, 2);
    logv(c, 3, v & 15);
    st(c, IF, 4, RL_NDS_IRQ_IPC_SYNC);
    c->handled++;
}

static void s4_arm9(core *c)
{
    st(c, SYNC, 2, 0x4000);
    for (int i = 0; i < Q.n; i++) {
        priv(c, 300 + (i * 17) % 90);
        st(c, SYNC, 2, 0x6000 | (uint32_t)(i & 15) << 8);
        logv(c, 1, (uint32_t)i);
    }
}

static void s4_arm7_setup(core *c)
{
    st(c, SYNC, 2, 0x4000);
    st(c, IE, 4, RL_NDS_IRQ_IPC_SYNC);
    st(c, IME, 4, 1);
    c->handler = s4_handler;
    c->irq_en = true;
}

static void s4a_arm7(core *c)
{
    s4_arm7_setup(c);
    while (c->handled < (uint32_t)Q.n) {
        if (now_of(c) > CAP) {
            c->aborted = true;
            return;
        }
        halt(c);
    }
}

static void s4b_arm7(core *c)
{
    s4_arm7_setup(c);
    while (c->handled < (uint32_t)Q.n) {
        priv(c, Q.l);
        if (back(c))
            return;
    }
}

static void s5(core *c)
{
    uint32_t base = c->id ? 0x02002000u : 0x02001000u;
    for (int i = 0; i < Q.n; i++) {
        priv(c, Q.k);
        st(c, base + 4 * (uint32_t)(i & 63), 4, (uint32_t)i);
    }
    logv(c, 9, 0);
}

/** @brief Both CPUs store to main RAM at the same tick, the ARM9 having written Q.l to EXMEMCNT. */
static void s6(core *c)
{
    if (c->id == 0)
        st(c, 0x04000204u, 2, (uint32_t)Q.l);
    while (now_of(c) < 1000)
        priv(c, 1);
    st(c, c->id ? 0x02003000u : 0x02003100u, 4, 1);
    logv(c, 6, 0);
}

/**
 * @brief The ARM7 (first for main RAM) runs a polling loop from main RAM code, which leaves the RAM no gap, until
 * tick 30000; the ARM9 loads from main RAM at tick 1000.
 */
static void s7(core *c)
{
    if (c->id == 0) {
        st(c, 0x04000204u, 2, 0xE880u);
        while (now_of(c) < 1000)
            priv(c, 1);
        ld(c, 0x02000000u, 4);
        logv(c, 7, 0);
        return;
    }
    while (now_of(c) < 30000) {
        if (back(c))
            return;
        pre(c);
        spend(c, rl_nds_code_cost7(Q.m, 0x02380000u, 4, false));
        spend(c, rl_nds_code_cost7(Q.m, 0x02380004u, 4, true));
        spend(c, rl_nds_code_cost7(Q.m, 0x02380008u, 4, true));
        ld(c, SYNC, 2);
    }
    logv(c, 7, 0);
}

static void body(rl_sched_cpu *k, void *arg)
{
    core *c = arg;
    rl_sched_run(k);
    Q.prog[c->id](c);
}

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 0x100000001B3ull;
    return h;
}

static void run(mode md, prog_fn *a9, prog_fn *a7, result *r)
{
    rl_sched_config cfg = {.ordered = true, .stack = 256u << 10};
    Q.s = rl_sched_create(&cfg);
    rl_sched_cpu *k9 = rl_sched_add_cpu(Q.s, 1), *k7 = rl_sched_add_cpu(Q.s, 2);
    Q.m = rl_nds_create();
    if (!Q.m || rl_nds_attach(Q.m, Q.s, k9, k7) != 0)
        abort();
    Q.mode = md;
    Q.prog[0] = a9;
    Q.prog[1] = a7;
    for (unsigned i = 0; i < 2; i++) {
        core *c = &Q.c[i];
        c->id = i;
        c->k = i ? k7 : k9;
        c->irq_en = c->aborted = false;
        c->handler = NULL;
        c->handled = 0;
        c->nlog = 0;
    }
    rl_sched_spawn(k9, body, &Q.c[0]);
    rl_sched_spawn(k7, body, &Q.c[1]);
    uint64_t t0 = rl_clock_ns();
    int rc = rl_sched_main(Q.s);
    r->ns = rl_clock_ns() - t0;
    if (rc != 0)
        abort();
    for (unsigned i = 0; i < 2; i++) {
        r->nlog[i] = Q.c[i].nlog;
        memcpy(r->log[i], Q.c[i].log, Q.c[i].nlog * sizeof(ent));
        r->end[i] = rl_sched_now(Q.c[i].k);
        r->aborted[i] = Q.c[i].aborted;
    }
    size_t len;
    uint8_t *p = rl_nds_mem(Q.m, RL_NDS_MAIN, &len);
    r->hash = fnv(0xCBF29CE484222325ull, p, len);
    p = rl_nds_mem(Q.m, RL_NDS_SWRAM, &len);
    r->hash = fnv(r->hash, p, len);
    const rl_sched_stats *st = rl_sched_get_stats(Q.s);
    r->orders = st->orders;
    r->waits = st->order_waits;
    r->switches = st->switches;
    rl_sched_destroy(Q.s);
    rl_nds_destroy(Q.m);
}

/** @brief Log entries of a and b that differ (count difference included). */
static uint32_t mismatches(const result *a, const result *b)
{
    uint32_t bad = 0;
    for (unsigned i = 0; i < 2; i++) {
        uint32_t n = a->nlog[i] > b->nlog[i] ? a->nlog[i] : b->nlog[i];
        for (uint32_t k = 0; k < n; k++) {
            if (k >= a->nlog[i] || k >= b->nlog[i]) {
                bad++;
                continue;
            }
            const ent *x = &a->log[i][k], *y = &b->log[i][k];
            bad += x->tag != y->tag || x->val != y->val || x->t != y->t;
        }
    }
    return bad;
}

static bool same(const result *a, const result *b)
{
    return mismatches(a, b) == 0 && a->end[0] == b->end[0] && a->end[1] == b->end[1] && a->hash == b->hash &&
           !a->aborted[0] && !a->aborted[1] && !b->aborted[0] && !b->aborted[1];
}

static void report(const char *name, const result *ref, const result *e)
{
    fprintf(stderr, "  %s: lockstep %.2f ms, fused %.2f ms; %llu ordered accesses, %llu waits, %llu switches; "
                    "%.1f ns per ordered access; end %llu/%llu ticks\n",
            name, ref->ns / 1e6, e->ns / 1e6, (unsigned long long)e->orders, (unsigned long long)e->waits,
            (unsigned long long)e->switches, e->orders ? (double)e->ns / (double)e->orders : 0.0,
            (unsigned long long)e->end[0], (unsigned long long)e->end[1]);
}

static result R[3];

/** @brief Runs the lockstep reference and the fused way twice; true when all three agree. */
static bool exact(const char *name, prog_fn *a9, prog_fn *a7)
{
    run(LOCKSTEP, a9, a7, &R[0]);
    run(FUSED, a9, a7, &R[1]);
    run(FUSED, a9, a7, &R[2]);
    report(name, &R[0], &R[1]);
    if (!same(&R[0], &R[1]))
        fprintf(stderr, "  %s: %u log entries differ from the reference\n", name, mismatches(&R[0], &R[1]));
    return same(&R[0], &R[1]) && same(&R[1], &R[2]);
}

TEST(q3_s1_fifo_ping_pong_matches_lockstep)
{
    Q.n = 4000;
    CHECK(exact("S1", s1_arm9, s1_arm7));
    CHECK_EQ(R[0].nlog[0], 2 * 4000 + 1);
    CHECK_EQ(R[1].log[0][1].val, 1001);
    CHECK_EQ(R[1].log[0][2 * 3999 + 1].val, 3999 * 3 + 1 + 1000);
    CHECK(R[1].waits > 1000);
}

TEST(q3_s2_wram_hand_over_matches_lockstep)
{
    Q.n = 4000;
    CHECK(exact("S2", s2_arm9, s2_arm7));
    uint32_t sum = 0;
    for (uint32_t r = 0; r < 4000; r++)
        for (uint32_t k = 0; k < 8; k++)
            sum += r * 16 + k;
    CHECK_EQ(R[1].log[1][R[1].nlog[1] - 1].val, sum);
}

TEST(q3_s3_racy_main_ram_flag_matches_lockstep)
{
    Q.n = 4000;
    CHECK(exact("S3", s3_arm9, s3_arm7));
    uint32_t sum = 0;
    for (uint32_t i = 0; i < 4000; i++)
        for (uint32_t k = 0; k < 4; k++)
            sum += i * 4 + k;
    CHECK_EQ(R[1].log[1][R[1].nlog[1] - 2].val, sum);
    CHECK(R[1].log[1][R[1].nlog[1] - 1].val > 0);
}

TEST(q3_s4a_ipcsync_irq_wakes_a_halted_arm7_exactly)
{
    Q.n = 200;
    CHECK(exact("S4a", s4_arm9, s4a_arm7));
    CHECK_EQ(R[1].nlog[1], 200);
    for (uint32_t i = 0; i < 200; i++) {
        CHECK_EQ(R[1].log[1][i].val, i & 15);
        CHECK(R[1].log[1][i].t > R[1].log[0][i].t);
        CHECK(R[1].log[1][i].t < R[1].log[0][i].t + 200);
    }
}

/** @brief S4b with blocks of l ARM7 instructions: the IRQ-exact fused run matches lockstep; plain fused IRQs are late by less than a block. */
static bool s4b(int l, uint64_t *late_max)
{
    Q.n = 200;
    Q.l = l;
    run(LOCKSTEP, s4_arm9, s4b_arm7, &R[0]);
    run(FUSED_IRQ_EXACT, s4_arm9, s4b_arm7, &R[1]);
    run(FUSED, s4_arm9, s4b_arm7, &R[2]);
    report(l == 8 ? "S4b block 8" : "S4b block 64", &R[0], &R[2]);
    if (mismatches(&R[0], &R[1]) != 0 || R[0].nlog[1] != 200 || R[2].nlog[1] != 200 || R[2].aborted[1])
        return false;
    *late_max = 0;
    uint64_t prev = 0;
    for (uint32_t i = 0; i < 200; i++) {
        const ent *ref = &R[0].log[1][i], *e = &R[2].log[1][i];
        if (e->val != ref->val || e->t < ref->t || e->t <= prev)
            return false;
        prev = e->t;
        uint64_t late = e->t - ref->t;
        if (late > *late_max)
            *late_max = late;
    }
    return R[0].end[0] == R[2].end[0] && mismatches(&R[0], &R[2]) <= 200;
}

TEST(q3_s4b_irqs_to_a_computing_arm7_arrive_within_a_block)
{
    uint64_t late;
    CHECK(s4b(8, &late));
    fprintf(stderr, "  S4b block 8: IRQ handler late by at most %llu ticks\n", (unsigned long long)late);
    CHECK(late > 0);
    CHECK(late <= 2 * 8);
    CHECK(s4b(64, &late));
    fprintf(stderr, "  S4b block 64: IRQ handler late by at most %llu ticks\n", (unsigned long long)late);
    CHECK(late <= 2 * 64);
}

TEST(q3_s5_contention_end_times_match_lockstep)
{
    static const int ks[] = {1, 4, 16, 64, 256};
    for (unsigned i = 0; i < sizeof ks / sizeof ks[0]; i++) {
        Q.k = ks[i];
        Q.n = 200000 / (Q.k + 4);
        char name[32];
        snprintf(name, sizeof name, "S5 K=%d", Q.k);
        CHECK(exact(name, s5, s5));
    }
}

TEST(q3_s6_exmemcnt_bit_15_decides_who_gets_main_ram_first)
{
    for (int md = LOCKSTEP; md <= FUSED; md++) {
        uint64_t end[2][2];
        for (unsigned arm7 = 0; arm7 < 2; arm7++) {
            Q.l = arm7 ? 0xE880 : 0x6880;
            run((mode)md, s6, s6, &R[0]);
            CHECK(R[0].nlog[0] == 1 && R[0].nlog[1] == 1);
            end[arm7][0] = R[0].log[0][0].t;
            end[arm7][1] = R[0].log[1][0].t;
        }
        CHECK(end[1][1] < end[0][1]);
        CHECK(end[1][0] > end[0][0]);
    }
}

/* inferred from hwtest 3c's rows 4055 to 4073: a gapless stream of the CPU first for main RAM keeps the other out */
TEST(q3_s7_a_gapless_stream_of_the_cpu_first_keeps_the_other_out_of_main_ram)
{
    for (int md = LOCKSTEP; md <= FUSED; md++) {
        run((mode)md, s7, s7, &R[0]);
        CHECK(R[0].nlog[0] == 1 && R[0].nlog[1] == 1);
        CHECK(R[0].log[0][0].t >= R[0].log[1][0].t);
    }
}
