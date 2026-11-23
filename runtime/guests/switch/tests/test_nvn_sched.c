/**
 * @file test_nvn_sched.c
 * @brief The replay order of submitted command sets: event waits that are
 * met, not yet met, and met by another queue or the CPU later.
 */
#include "nvn/nvn_sched.h"
#include "tc.h"

#include <stdlib.h>
#include <string.h>

#define RET 0x7000000ull

typedef struct fake {
    uint32_t words[8];
    uint64_t log[64];
    uint32_t nlog;
} fake;

static int fake_exec(void *user, const nvn_cmd *e, const nvn_cmdset *s)
{
    (void)s;
    fake *f = user;
    f->log[f->nlog++] = e->x[1];
    return RL_OK;
}

static bool fake_flush(void *user)
{
    (void)user;
    return true;
}

static uint32_t *fake_word(void *user, uint64_t addr) { return &((fake *)user)->words[addr - 1]; }

static const nvn_sched_ops ops = { fake_exec, fake_flush, fake_word, NULL, NULL, NULL };

typedef struct step {
    uint32_t op;
    uint64_t a, b, c;
} step;

/** @brief A set of steps: op WAIT_EVENT (event word index a, held as address a + 1, as 0 is no event; mode b, value c), SIGNAL_EVENT (index a, mode b, value c) or any other op, logged as a. */
static nvn_cmdset *make_set(const step *steps, uint32_t n)
{
    nvn_cmdset *s = calloc(1, sizeof *s + n * sizeof(nvn_cmd));
    s->n = n;
    for (uint32_t i = 0; i < n; i++) {
        nvn_cmd *e = &s->cmds[i];
        e->op = steps[i].op;
        e->x[1] = steps[i].a;
        if (e->op == NVN_CMD_WAIT_EVENT) {
            e->x[1] = steps[i].a + 1;
            e->x[2] = steps[i].b;
            e->x[3] = steps[i].c;
        } else if (e->op == NVN_CMD_SIGNAL_EVENT) {
            e->x[1] = steps[i].a + 1;
            e->x[2] = steps[i].b;
            e->x[5] = steps[i].c;
        }
    }
    return s;
}

static void submit(nvn_sched *sc, uint64_t queue, nvn_cmdset *s, fake *f)
{
    nvn_sched_item *items;
    uint32_t n;
    uint64_t bad;
    CHECK_EQ(nvn_sched_flatten(s, &items, &n, &bad), 0);
    CHECK(nvn_sched_add(sc, queue, items, n, f));
}

static void pump(nvn_sched *sc)
{
    nvn_sched_run *done = NULL;
    CHECK_EQ(nvn_sched_pump(sc, &ops, &done), RL_OK);
    while (done) {
        nvn_sched_run *next = done->next;
        nvn_sched_run_free(done);
        done = next;
    }
}

TEST(nvn_event_waits_compare_as_the_gpu_does)
{
    CHECK(nvn_event_wait_met(5, NVN_EVENT_WAIT_EQUAL, 5));
    CHECK(!nvn_event_wait_met(6, NVN_EVENT_WAIT_EQUAL, 5));
    CHECK(nvn_event_wait_met(6, NVN_EVENT_WAIT_GEQUAL_WRAP, 5));
    CHECK(nvn_event_wait_met(5, NVN_EVENT_WAIT_GEQUAL_WRAP, 5));
    CHECK(!nvn_event_wait_met(4, NVN_EVENT_WAIT_GEQUAL_WRAP, 5));
    CHECK(nvn_event_wait_met(2, NVN_EVENT_WAIT_GEQUAL_WRAP, 0xfffffffeu));
    CHECK(!nvn_event_wait_met(0xfffffffeu, NVN_EVENT_WAIT_GEQUAL_WRAP, 2));
}

TEST(nvn_met_wait_lets_the_run_through)
{
    fake f = { .words = { 3 } };
    nvn_sched sc = { 0 };
    step st[] = { { NVN_CMD_BARRIER, 10, 0, 0 }, { NVN_CMD_WAIT_EVENT, 0, NVN_EVENT_WAIT_EQUAL, 3 },
                  { NVN_CMD_BARRIER, 11, 0, 0 } };
    nvn_cmdset *s = make_set(st, 3);
    submit(&sc, 1, s, &f);
    pump(&sc);
    CHECK_EQ(f.nlog, 2);
    CHECK_EQ(f.log[1], 11);
    CHECK_EQ(sc.pending, 0);
    free(s);
}

TEST(nvn_unmet_wait_holds_its_queue_and_another_queue_releases_it)
{
    fake f = { 0 };
    nvn_sched sc = { 0 };
    step a[] = { { NVN_CMD_BARRIER, 1, 0, 0 }, { NVN_CMD_WAIT_EVENT, 0, NVN_EVENT_WAIT_EQUAL, 7 },
                 { NVN_CMD_BARRIER, 2, 0, 0 } };
    step later[] = { { NVN_CMD_BARRIER, 3, 0, 0 } };
    step b[] = { { NVN_CMD_BARRIER, 4, 0, 0 }, { NVN_CMD_SIGNAL_EVENT, 0, NVN_EVENT_SIGNAL_WRITE, 7 } };
    nvn_cmdset *sa = make_set(a, 3), *sl = make_set(later, 1), *sb = make_set(b, 2);
    submit(&sc, 1, sa, &f);
    pump(&sc);
    CHECK_EQ(f.nlog, 1);
    CHECK_EQ(sc.pending, 1);
    submit(&sc, 1, sl, &f);
    pump(&sc);
    CHECK_EQ(f.nlog, 1);
    CHECK_EQ(sc.pending, 2);
    submit(&sc, 2, sb, &f);
    pump(&sc);
    CHECK_EQ(f.words[0], 7);
    CHECK_EQ(sc.pending, 0);
    CHECK_EQ(f.nlog, 4);
    CHECK_EQ(f.log[0], 1);
    CHECK_EQ(f.log[1], 4);
    CHECK_EQ(f.log[2], 2);
    CHECK_EQ(f.log[3], 3);
    free(sa);
    free(sl);
    free(sb);
}

TEST(nvn_cpu_signal_releases_a_waiting_run)
{
    fake f = { 0 };
    nvn_sched sc = { 0 };
    step a[] = { { NVN_CMD_WAIT_EVENT, 1, NVN_EVENT_WAIT_GEQUAL_WRAP, 2 }, { NVN_CMD_BARRIER, 9, 0, 0 } };
    nvn_cmdset *s = make_set(a, 2);
    submit(&sc, 1, s, &f);
    pump(&sc);
    CHECK_EQ(f.nlog, 0);
    f.words[1] = 1;
    pump(&sc);
    CHECK_EQ(f.nlog, 0);
    f.words[1] = 3;
    pump(&sc);
    CHECK_EQ(f.nlog, 1);
    CHECK_EQ(sc.pending, 0);
    free(s);
}

TEST(nvn_signal_add_and_two_queues_waiting_on_each_other_in_turn)
{
    fake f = { 0 };
    nvn_sched sc = { 0 };
    step a[] = { { NVN_CMD_WAIT_EVENT, 0, NVN_EVENT_WAIT_EQUAL, 1 }, { NVN_CMD_BARRIER, 21, 0, 0 },
                 { NVN_CMD_SIGNAL_EVENT, 1, NVN_EVENT_SIGNAL_ADD, 5 } };
    step b[] = { { NVN_CMD_SIGNAL_EVENT, 0, NVN_EVENT_SIGNAL_ADD, 1 }, { NVN_CMD_WAIT_EVENT, 1, NVN_EVENT_WAIT_EQUAL, 5 },
                 { NVN_CMD_BARRIER, 22, 0, 0 } };
    nvn_cmdset *sa = make_set(a, 3), *sb = make_set(b, 3);
    submit(&sc, 1, sa, &f);
    submit(&sc, 2, sb, &f);
    pump(&sc);
    CHECK_EQ(sc.pending, 0);
    CHECK_EQ(f.nlog, 2);
    CHECK_EQ(f.log[0], 21);
    CHECK_EQ(f.log[1], 22);
    free(sa);
    free(sb);
}

TEST(nvn_calls_flatten_into_the_run_in_place)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400;
    c->x[0] = cb;
    c->x[1] = heap;
    c->x[2] = 0;
    c->x[3] = 0;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferInitialize(c, t_mem()), RET);
    c->x[0] = cb;
    c->x[1] = heap;
    c->x[3] = 0x4000;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferAddCommandMemory(c, t_mem()), RET);
    uint64_t handles[2];
    for (unsigned i = 0; i < 2; i++) {
        c->x[0] = cb;
        c->x[30] = RET;
        CHECK_EQ(nvnh_nvnCommandBufferBeginRecording(c, t_mem()), RET);
        c->x[0] = cb;
        c->x[1] = 3 + i;
        c->x[2] = 0x80;
        c->x[30] = RET;
        CHECK_EQ(nvnh_nvnCommandBufferSetStencilRef(c, t_mem()), RET);
        c->x[0] = cb;
        c->x[1] = heap + 0x2000;
        c->x[2] = NVN_EVENT_WAIT_EQUAL;
        c->x[3] = 9;
        c->x[30] = RET;
        CHECK_EQ(nvnh_nvnCommandBufferWaitEvent(c, t_mem()), RET);
        c->x[0] = cb;
        c->x[30] = RET;
        CHECK_EQ(nvnh_nvnCommandBufferEndRecording(c, t_mem()), RET);
        handles[i] = c->x[0];
    }
    c->x[0] = cb;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferBeginRecording(c, t_mem()), RET);
    c->x[0] = cb;
    c->x[1] = handles[0];
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferCallCommand(c, t_mem()), RET);
    c->x[0] = cb;
    c->x[1] = handles[1];
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferCallCommand(c, t_mem()), RET);
    c->x[0] = cb;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferEndRecording(c, t_mem()), RET);
    const nvn_cmdset *outer = nvn_cmdset_of(c->x[0]);
    CHECK(outer != NULL);
    CHECK_EQ(outer->n, 2);
    CHECK_EQ(nvn_cmdset_of(handles[0])->cmds[1].op, NVN_CMD_WAIT_EVENT);
    CHECK_EQ(nvn_cmdset_of(handles[0])->cmds[1].x[1], 0);
    nvn_sched_item *items;
    uint32_t n;
    uint64_t bad;
    CHECK_EQ(nvn_sched_flatten(outer, &items, &n, &bad), 0);
    CHECK_EQ(n, 4);
    CHECK_EQ(items[0].e->op, NVN_CMD_SET_STENCIL_REF);
    CHECK_EQ(items[1].e->op, NVN_CMD_WAIT_EVENT);
    CHECK_EQ(items[2].e->x[1], 4);
    CHECK(items[3].s == nvn_cmdset_of(handles[1]));
    free(items);
}

TEST(nvn_update_uniform_buffer_keeps_the_data_it_was_recorded_with)
{
    rl_ctx *c = t_boot(NULL);
    uint64_t heap = t_heap(c, 0x200000);
    uint64_t cb = heap + 0x400, data = heap + 0x1000;
    c->x[0] = cb;
    c->x[1] = heap;
    c->x[2] = 0;
    c->x[3] = 0;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferInitialize(c, t_mem()), RET);
    c->x[0] = cb;
    c->x[1] = heap;
    c->x[3] = 0x4000;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferAddCommandMemory(c, t_mem()), RET);
    c->x[0] = cb;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferBeginRecording(c, t_mem()), RET);
    for (unsigned i = 0; i < 16; i++)
        t_mem()[data + i] = (uint8_t)(0x10 + i);
    c->x[0] = cb;
    c->x[1] = 0x1000;
    c->x[2] = 0x100;
    c->x[3] = 0x20;
    c->x[4] = 16;
    c->x[5] = data;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferUpdateUniformBuffer(c, t_mem()), RET);
    for (unsigned i = 0; i < 16; i++)
        t_mem()[data + i] = 0;
    c->x[0] = cb;
    c->x[30] = RET;
    CHECK_EQ(nvnh_nvnCommandBufferEndRecording(c, t_mem()), RET);
    const nvn_cmdset *s = nvn_cmdset_of(c->x[0]);
    CHECK(s != NULL);
    CHECK_EQ(s->n, 1);
    CHECK_EQ(s->cmds[0].op, NVN_CMD_UPDATE_UNIFORM_BUFFER);
    CHECK_EQ(s->cmds[0].x[3], 0x20);
    const uint8_t *kept = nvn_cmd_bytes(s, s->cmds[0].x[5]);
    CHECK(kept != NULL);
    CHECK_EQ(kept[0], 0x10);
    CHECK_EQ(kept[15], 0x1f);
}

typedef struct noted {
    uint32_t op[32], pos[32], value[32];
    bool met[32];
    uint32_t n;
} noted;

static noted g_noted;

static void fake_note(void *user, const nvn_cmd *e, uint32_t pos, uint32_t value, bool met)
{
    (void)user;
    if (g_noted.n < 32) {
        g_noted.op[g_noted.n] = e->op;
        g_noted.pos[g_noted.n] = pos;
        g_noted.value[g_noted.n] = value;
        g_noted.met[g_noted.n] = met;
        g_noted.n++;
    }
}

/**
 * @brief The note hook (the frame trace's order file) hears every item in replay order: a command before it runs, an
 * event wait each time it is checked with the word it saw and whether it let the run through, and a signal with the
 * word it left.
 */
TEST(nvn_sched_notes_each_item_in_replay_order)
{
    static const nvn_sched_ops noting = { fake_exec, fake_flush, fake_word, fake_note, NULL, NULL };
    memset(&g_noted, 0, sizeof g_noted);
    fake f = { 0 };
    nvn_sched sc = { 0 };
    step a[] = { { NVN_CMD_BARRIER, 1, 0, 0 }, { NVN_CMD_WAIT_EVENT, 0, NVN_EVENT_WAIT_EQUAL, 7 },
                 { NVN_CMD_BARRIER, 2, 0, 0 } };
    step b[] = { { NVN_CMD_SIGNAL_EVENT, 0, NVN_EVENT_SIGNAL_WRITE, 7 } };
    nvn_cmdset *sa = make_set(a, 3), *sb = make_set(b, 1);
    nvn_sched_run *done = NULL;
    submit(&sc, 1, sa, &f);
    CHECK_EQ(nvn_sched_pump(&sc, &noting, &done), RL_OK);
    submit(&sc, 2, sb, &f);
    CHECK_EQ(nvn_sched_pump(&sc, &noting, &done), RL_OK);
    CHECK_EQ(sc.pending, 0);
    /* The barrier, the wait checked unmet by each pump pass that reaches it (twice in the first pump, once in the
     * second before the other queue's signal), the signal, the wait met, the barrier after it. */
    static const uint32_t want[] = { NVN_CMD_BARRIER,       NVN_CMD_WAIT_EVENT, NVN_CMD_WAIT_EVENT, NVN_CMD_WAIT_EVENT,
                                     NVN_CMD_SIGNAL_EVENT, NVN_CMD_WAIT_EVENT, NVN_CMD_BARRIER };
    CHECK_EQ(g_noted.n, 7);
    for (uint32_t i = 0; i < 7 && i < g_noted.n; i++)
        CHECK_EQ(g_noted.op[i], want[i]);
    for (uint32_t i = 1; i <= 3; i++) {
        CHECK_EQ(g_noted.pos[i], 1);
        CHECK_EQ(g_noted.value[i], 0);
        CHECK(!g_noted.met[i]);
    }
    CHECK_EQ(g_noted.value[4], 7);
    CHECK(g_noted.met[5]);
    CHECK_EQ(g_noted.value[5], 7);
    CHECK_EQ(g_noted.pos[6], 2);
    while (done) {
        nvn_sched_run *next = done->next;
        nvn_sched_run_free(done);
        done = next;
    }
    free(sa);
    free(sb);
}

/** @brief The syncs of the fake: index a + 1 is the sync's address, signaled[a] its state; a run that fences sync a
 * (an item of op FENCE_SYNC, logged as 100 + a) signals it when the run finishes. */
static bool g_signaled[4];
static uint64_t g_fenced[4];

static bool fake_signaled(void *user, uint64_t sync)
{
    (void)user;
    return g_signaled[sync - 1];
}

static int fake_exec_fence(void *user, const nvn_cmd *e, const nvn_cmdset *s)
{
    fake *f = user;
    if (e->op == NVN_CMD_FENCE_SYNC) {
        g_fenced[e->x[1] - 1] = (uint64_t)(uintptr_t)f;
        f->log[f->nlog++] = 100 + e->x[1] - 1;
        return RL_OK;
    }
    return fake_exec(user, e, s);
}

static void fake_finished(void *user)
{
    for (uint32_t i = 0; i < 4; i++)
        if (g_fenced[i] == (uint64_t)(uintptr_t)user) {
            g_signaled[i] = true;
            g_fenced[i] = 0;
        }
}

/**
 * @brief A queue that waits on a sync another queue fences behind an event wait (nvnCommandBufferWaitSync, as a
 * queue's nvnQueueWaitSync is replayed) holds its later work until that queue's run has finished, even though it was
 * submitted while that run was still waiting; the console would run it only after the fence.
 */
TEST(nvn_sched_a_wait_sync_holds_its_queue_until_the_other_queue_fenced)
{
    static const nvn_sched_ops syncing = { fake_exec_fence, fake_flush, fake_word, NULL, fake_signaled, fake_finished };
    memset(g_signaled, 0, sizeof g_signaled);
    memset(g_fenced, 0, sizeof g_fenced);
    fake f = { 0 };
    nvn_sched sc = { 0 };
    nvn_sched_run *done = NULL;
    /* Queue 1: copy (1), wait for event 0 to be 5, then fence sync 0. */
    step a[] = { { NVN_CMD_BARRIER, 1, 0, 0 }, { NVN_CMD_WAIT_EVENT, 0, NVN_EVENT_WAIT_EQUAL, 5 },
                 { NVN_CMD_FENCE_SYNC, 0, 0, 0 } };
    nvn_cmdset *sa = make_set(a, 3);
    sa->cmds[2].x[1] = 1;
    submit(&sc, 1, sa, &f);
    CHECK_EQ(nvn_sched_pump(&sc, &syncing, &done), RL_OK);
    CHECK(nvn_sched_queue_busy(&sc, 1));
    /* Queue 2: waits on sync 0, then clears (2). */
    step b[] = { { NVN_CMD_WAIT_SYNC, 0, 0, 0 }, { NVN_CMD_BARRIER, 2, 0, 0 } };
    nvn_cmdset *sb = make_set(b, 2);
    sb->cmds[0].x[1] = 1;
    submit(&sc, 2, sb, &f);
    CHECK_EQ(nvn_sched_pump(&sc, &syncing, &done), RL_OK);
    CHECK_EQ(f.nlog, 1);
    CHECK(nvn_sched_queue_busy(&sc, 2));
    /* Queue 3 signals the event: queue 1 goes on and fences, then queue 2 clears, in that order. */
    step c[] = { { NVN_CMD_SIGNAL_EVENT, 0, NVN_EVENT_SIGNAL_WRITE, 5 } };
    nvn_cmdset *scs = make_set(c, 1);
    submit(&sc, 3, scs, &f);
    CHECK_EQ(nvn_sched_pump(&sc, &syncing, &done), RL_OK);
    CHECK_EQ(sc.pending, 0);
    CHECK_EQ(f.nlog, 3);
    CHECK_EQ(f.log[0], 1);
    CHECK_EQ(f.log[1], 100);
    CHECK_EQ(f.log[2], 2);
    while (done) {
        nvn_sched_run *next = done->next;
        nvn_sched_run_free(done);
        done = next;
    }
    free(sa);
    free(sb);
    free(scs);
}
