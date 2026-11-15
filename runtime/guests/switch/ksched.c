/**
 * @file ksched.c
 * @brief The scheduler: per-core strict priority with affinity, the way
 * Horizon runs threads. Each emulated core has one holder, the only guest
 * thread that runs guest code there. A thread gives its core up when it
 * blocks or yields, and at its next svc when the scheduler chose another
 * thread for the core.
 *
 * The queues, the selection with its migration to idle cores, and the yields
 * are a port of the logic of Ryujinx (MIT) KPriorityQueue and KScheduler.
 * Horizon preempts at any instruction; here the scheduler ends the budget of
 * the holder's preemption clock and the holder leaves at its next svc or
 * preemption point (docs/abi.md section 14.1), and the round robin of
 * priorities 59 and 63 runs at the holder's svcs.
 */
#include "hos.h"

#include <stdlib.h>
#include <string.h>

#define RR_NS 10000000ull
#define LATE_LOG_NS 1000000ull
#define STATS_NS 1000000000ull

static int32_t rr_prio(int32_t core) { return core == HOS_CORES - 1 ? 63 : 59; }

static void q_add(hos_runq *q, int32_t c, int32_t p, hos_thread *t, bool front)
{
    if (front) {
        t->qprev[c] = NULL;
        t->qnext[c] = q->head[c][p];
        if (q->head[c][p])
            q->head[c][p]->qprev[c] = t;
        else
            q->tail[c][p] = t;
        q->head[c][p] = t;
    } else {
        t->qnext[c] = NULL;
        t->qprev[c] = q->tail[c][p];
        if (q->tail[c][p])
            q->tail[c][p]->qnext[c] = t;
        else
            q->head[c][p] = t;
        q->tail[c][p] = t;
    }
    q->mask[c] |= 1ull << p;
}

static void q_del(hos_runq *q, int32_t c, int32_t p, hos_thread *t)
{
    if (t->qprev[c])
        t->qprev[c]->qnext[c] = t->qnext[c];
    else
        q->head[c][p] = t->qnext[c];
    if (t->qnext[c])
        t->qnext[c]->qprev[c] = t->qprev[c];
    else
        q->tail[c][p] = t->qprev[c];
    t->qnext[c] = t->qprev[c] = NULL;
    if (!q->head[c][p])
        q->mask[c] &= ~(1ull << p);
}

static hos_thread *q_first(const hos_runq *q, int32_t c)
{
    uint64_t m = q->mask[c];
    return m ? q->head[c][__builtin_ctzll(m)] : NULL;
}

static hos_thread *q_next(const hos_runq *q, int32_t c, const hos_thread *t)
{
    if (t->qnext[c])
        return t->qnext[c];
    uint64_t m = t->prio >= HOS_PRIOS - 1 ? 0 : q->mask[c] & (~0ull << (t->prio + 1));
    return m ? q->head[c][__builtin_ctzll(m)] : NULL;
}

static hos_thread *q_at(const hos_runq *q, int32_t c, int n)
{
    hos_thread *t = q_first(q, c);
    while (t && n-- > 0)
        t = q_next(q, c, t);
    return t;
}

static hos_thread *q_requeue(hos_runq *q, int32_t c, int32_t p, hos_thread *t)
{
    q_del(q, c, p, t);
    q_add(q, c, p, t, false);
    return q->head[c][p];
}

static bool in_mask(uint64_t mask, int32_t c) { return (mask >> c) & 1; }

static void transfer(int32_t p, int32_t dst, hos_thread *t)
{
    hos_sched *s = &hos.sched;
    int32_t src = t->active_core;
    if (src == dst)
        return;
    t->active_core = dst;
    if (src >= 0)
        q_del(&s->scheduled, src, p, t);
    if (dst >= 0) {
        q_del(&s->suggested, dst, p, t);
        q_add(&s->scheduled, dst, p, t, false);
    }
    if (src >= 0)
        q_add(&s->suggested, src, p, t, true);
}

static void enqueue(hos_thread *t, bool front)
{
    hos_sched *s = &hos.sched;
    if (t->active_core >= 0)
        q_add(&s->scheduled, t->active_core, t->prio, t, front);
    for (int32_t c = 0; c < HOS_CORES; c++)
        if (c != t->active_core && in_mask(t->affinity, c))
            q_add(&s->suggested, c, t->prio, t, true);
    s->reselect = true;
}

static void dequeue(hos_thread *t)
{
    hos_sched *s = &hos.sched;
    if (t->active_core >= 0)
        q_del(&s->scheduled, t->active_core, t->prio, t);
    for (int32_t c = 0; c < HOS_CORES; c++)
        if (c != t->active_core && in_mask(t->affinity, c))
            q_del(&s->suggested, c, t->prio, t);
    s->reselect = true;
}

static void set_selected(int32_t c, hos_thread *t)
{
    hos_core *k = &hos.sched.cores[c];
    if (k->selected == t)
        return;
    if (k->selected)
        k->selected->last_sched_ns = rl_clock_ns();
    k->selected = t;
}

static void select_threads(void)
{
    hos_sched *s = &hos.sched;
    if (!s->reselect)
        return;
    s->reselect = false;
    for (int32_t c = 0; c < HOS_CORES; c++)
        set_selected(c, q_first(&s->scheduled, c));
    for (int32_t c = 0; c < HOS_CORES; c++) {
        if (q_first(&s->scheduled, c))
            continue;
        int32_t src[HOS_CORES];
        int n = 0;
        hos_thread *dst = NULL;
        for (hos_thread *t = q_first(&s->suggested, c); t; t = q_next(&s->suggested, c, t)) {
            if (t->active_core < 0 || t != s->cores[t->active_core].selected) {
                dst = t;
                break;
            }
            if (n < HOS_CORES)
                src[n++] = t->active_core;
        }
        if (dst) {
            if (dst->prio >= 2) {
                transfer(dst->prio, c, dst);
                set_selected(c, dst);
            }
            continue;
        }
        for (int i = 0; i < n; i++) {
            hos_thread *second = q_at(&s->scheduled, src[i], 1);
            if (!second)
                continue;
            hos_thread *moved = s->cores[src[i]].selected;
            set_selected(src[i], second);
            transfer(moved->prio, c, moved);
            set_selected(c, moved);
            break;
        }
    }
}

static void stat_tick(uint64_t now)
{
    hos_sched *s = &hos.sched;
    if (!s->stats)
        return;
    if (!s->stat_start_ns) {
        s->stat_start_ns = now;
        return;
    }
    if (now - s->stat_start_ns < STATS_NS)
        return;
    rl_log(RL_LOG_INFO, "sched", "%llu hand-overs, %llu preemptions in %llu ms; longest preemption delay %llu us (run %llu us)",
           (unsigned long long)s->handovers, (unsigned long long)s->preempts,
           (unsigned long long)((now - s->stat_start_ns) / 1000000ull), (unsigned long long)(s->late_ns / 1000ull),
           (unsigned long long)(s->late_max_ns / 1000ull));
    s->stat_start_ns = now;
    s->handovers = s->preempts = s->late_ns = 0;
}

static void grant(int32_t c, hos_thread *t)
{
    hos_sched *s = &hos.sched;
    hos_core *k = &s->cores[c];
    k->holder = t;
    t->cur_core = c;
    rl_preempt_clear(t->rt->ctx);
    uint64_t now = rl_clock_ns();
    t->slice_ns = now;
    if (k->last != t) {
        k->last = t;
        s->handovers++;
    }
    stat_tick(now);
    pthread_cond_signal(&t->cv);
}

static void kick(int32_t c)
{
    hos_core *k = &hos.sched.cores[c];
    hos_thread *sel = k->selected, *h = k->holder;
    if (h == sel) {
        if (h) {
            rl_preempt_clear(h->rt->ctx);
            k->preempt_ns = 0;
        }
        return;
    }
    if (h) {
        if (!k->preempt_ns) {
            k->preempt_ns = rl_clock_ns();
            rl_preempt_request(h->rt->ctx);
        }
        return;
    }
    if (sel && sel->runnable && sel->cur_core < 0)
        grant(c, sel);
}

static void update(void)
{
    if (hos.sched.free)
        return;
    select_threads();
    for (int32_t c = 0; c < HOS_CORES; c++)
        kick(c);
}

static void release(hos_thread *t)
{
    hos_sched *s = &hos.sched;
    int32_t c = t->cur_core;
    hos_core *k = &s->cores[c];
    k->holder = NULL;
    t->cur_core = -1;
    rl_preempt_clear(t->rt->ctx);
    if (k->preempt_ns) {
        uint64_t d = rl_clock_ns() - k->preempt_ns;
        k->preempt_ns = 0;
        s->preempts++;
        if (d > s->late_ns)
            s->late_ns = d;
        if (d > s->late_max_ns) {
            s->late_max_ns = d;
            if (d >= LATE_LOG_NS)
                rl_log(RL_LOG_INFO, "sched", "core %d gave way %llu us after a preemption request (thread %llu)", c,
                       (unsigned long long)(d / 1000ull), (unsigned long long)t->id);
        }
    }
    kick(c);
}

static void preempted(rl_ctx *c)
{
    (void)c;
    hos_sched_point();
}

void hos_sched_init(void)
{
    const char *e = getenv("RELIFT_SCHED");
    hos.sched.free = e && strstr(e, "free");
    hos.sched.stats = e && strstr(e, "stats");
    if (hos.sched.free)
        rl_log(RL_LOG_WARN, "sched", "RELIFT_SCHED=free: guest threads run freely, ignoring priorities and cores");
    rl_set_preempt_hook(preempted);
}

void hos_sched_ready(hos_thread *t)
{
    if (t->runnable)
        return;
    t->runnable = true;
    enqueue(t, false);
    update();
}

void hos_sched_unready(hos_thread *t)
{
    if (t->runnable) {
        t->runnable = false;
        dequeue(t);
    }
    if (t->cur_core >= 0)
        release(t);
    update();
}

void hos_sched_run(hos_thread *t)
{
    hos_sched *s = &hos.sched;
    if (s->free || !t->runnable)
        return;
    for (;;) {
        update();
        int32_t c = t->active_core;
        if (t->cur_core >= 0 && (t->cur_core != c || s->cores[t->cur_core].selected != t))
            release(t);
        if (t->cur_core >= 0)
            return;
        if (c >= 0 && s->cores[c].selected == t && !s->cores[c].holder) {
            grant(c, t);
            return;
        }
        pthread_cond_wait(&t->cv, &hos.lock);
    }
}

typedef struct cand_rule {
    bool equal;
    int32_t prio;
} cand_rule;

static hos_thread *rotate_candidate(int32_t c, hos_thread *sel, hos_thread *next, cand_rule r)
{
    hos_sched *s = &hos.sched;
    for (hos_thread *t = q_first(&s->suggested, c); t; t = q_next(&s->suggested, c, t)) {
        if (t->active_core >= 0) {
            hos_thread *first = q_first(&s->scheduled, t->active_core);
            if (first == t || (first && first->prio < 2))
                continue;
        }
        if (next == sel || !next || next->last_sched_ns >= t->last_sched_ns)
            if (r.equal ? t->prio == r.prio : t->prio < r.prio)
                return t;
    }
    return NULL;
}

static void rotate(int32_t c, int32_t p)
{
    hos_sched *s = &hos.sched;
    hos_thread *sel = s->scheduled.head[c][p];
    hos_thread *next = sel ? q_requeue(&s->scheduled, c, p, sel) : NULL;
    hos_thread *dst = rotate_candidate(c, sel, next, (cand_rule){ true, p });
    if (dst)
        transfer(p, c, dst);
    hos_thread *best = q_first(&s->scheduled, c);
    if (best && best->prio >= p) {
        dst = rotate_candidate(c, sel, next, (cand_rule){ false, best->prio });
        if (dst)
            transfer(dst->prio, c, dst);
    }
    s->reselect = true;
}

void hos_sched_point(void)
{
    hos_thread *t = hos_current();
    hos_sched *s = &hos.sched;
    if (!t || s->free)
        return;
    bool rr = t->prio >= 59;
    if (!rl_time_spent(&t->rt->ctx->time) && !rr && !s->stats)
        return;
    hos_lock();
    if (!t->runnable)
        rl_preempt_clear(t->rt->ctx);
    else {
        uint64_t now = rl_clock_ns();
        int32_t c = t->cur_core;
        if (rr && c >= 0 && t->prio == rr_prio(c) && now - t->slice_ns >= RR_NS) {
            rotate(c, t->prio);
            t->slice_ns = now;
        }
        stat_tick(now);
        hos_sched_run(t);
    }
    hos_unlock();
}

void hos_sched_leave(void)
{
    hos_thread *t = hos_current();
    if (!t || hos.sched.free)
        return;
    hos_lock();
    hos_sched_unready(t);
    hos_unlock();
}

void hos_sched_return(void)
{
    hos_thread *t = hos_current();
    if (!t || hos.sched.free)
        return;
    hos_lock();
    hos_sched_ready(t);
    hos_sched_run(t);
    hos_unlock();
}

static bool yield_same(hos_thread *t)
{
    hos_sched *s = &hos.sched;
    if (t->active_core < 0)
        return false;
    return q_requeue(&s->scheduled, t->active_core, t->prio, t) != t;
}

static bool yield_balance(hos_thread *t)
{
    hos_sched *s = &hos.sched;
    int32_t c = t->active_core, p = t->prio;
    if (c < 0)
        return false;
    hos_thread *next = q_requeue(&s->scheduled, c, p, t);
    hos_thread *dst = NULL;
    for (hos_thread *x = q_first(&s->suggested, c); x; x = q_next(&s->suggested, c, x)) {
        if (x->active_core >= 0) {
            hos_thread *sel = s->cores[x->active_core].selected;
            if (sel == x || (sel && sel->prio < 2))
                continue;
        }
        if ((x->last_sched_ns <= next->last_sched_ns || x->prio < next->prio) && x->prio <= p) {
            dst = x;
            break;
        }
    }
    if (dst) {
        transfer(dst->prio, c, dst);
        return true;
    }
    return next != t;
}

static bool yield_any(hos_thread *t)
{
    hos_sched *s = &hos.sched;
    int32_t c = t->active_core;
    if (c < 0)
        return false;
    transfer(t->prio, -1, t);
    if (q_first(&s->scheduled, c))
        return true;
    hos_thread *sel = NULL;
    for (hos_thread *x = q_first(&s->suggested, c); x; x = q_next(&s->suggested, c, x)) {
        if (x->active_core < 0)
            continue;
        hos_thread *first = q_first(&s->scheduled, x->active_core);
        if (first == x)
            continue;
        if (!first || first->prio >= 2)
            transfer(x->prio, c, x);
        sel = x;
        break;
    }
    return t != sel;
}

void hos_sched_yield(int64_t ns)
{
    hos_thread *t = hos_current();
    if (!t || hos.sched.free) {
        rl_host_yield();
        return;
    }
    hos_lock();
    if (t->runnable) {
        bool changed = ns == 0 ? yield_same(t) : ns == -1 ? yield_balance(t) : ns == -2 ? yield_any(t) : false;
        if (changed)
            hos.sched.reselect = true;
        hos_sched_run(t);
    }
    hos_unlock();
}

void hos_sched_set_prio(hos_thread *t, int32_t prio)
{
    if (t->prio == prio)
        return;
    if (!t->runnable) {
        t->prio = prio;
        return;
    }
    dequeue(t);
    t->prio = prio;
    enqueue(t, t == hos_current());
    update();
}

rl_result hos_sched_set_affinity(hos_thread *t, int32_t core, uint64_t mask)
{
    hos_sched *s = &hos.sched;
    if (core == -3) {
        core = t->core;
        if (core < 0 || !in_mask(mask, core))
            return KR_INVALID_COMBINATION;
    }
    uint64_t old_mask = t->affinity;
    t->core = core;
    t->affinity = mask;
    if (old_mask == mask)
        return KR_SUCCESS;
    int32_t old_core = t->active_core;
    if (old_core >= 0 && !in_mask(mask, old_core))
        t->active_core = core < 0 ? 63 - __builtin_clzll(mask) : core;
    if (!t->runnable)
        return KR_SUCCESS;
    for (int32_t c = 0; c < HOS_CORES; c++)
        if (in_mask(old_mask, c)) {
            if (c == old_core)
                q_del(&s->scheduled, c, t->prio, t);
            else
                q_del(&s->suggested, c, t->prio, t);
        }
    for (int32_t c = 0; c < HOS_CORES; c++)
        if (in_mask(mask, c)) {
            if (c == t->active_core)
                q_add(&s->scheduled, c, t->prio, t, false);
            else
                q_add(&s->suggested, c, t->prio, t, true);
        }
    s->reselect = true;
    update();
    return KR_SUCCESS;
}

int32_t hos_sched_processor(const hos_thread *t)
{
    if (t->cur_core >= 0)
        return t->cur_core;
    if (t->active_core >= 0)
        return t->active_core;
    return t->core >= 0 ? t->core : 0;
}
