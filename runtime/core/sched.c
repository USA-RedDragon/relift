/**
 * @file sched.c
 * @brief The device scheduler: CPU clocks and budgets, the event queue,
 * device catch-up on access, interrupt lines, the hand-over of the run
 * between CPUs, and the lifted-code entries rl_sync, rl_io_read and
 * rl_io_write (relift/sched.h).
 */
#include "core.h"

#include <relift/sched.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** @brief rl_sched.holder when no CPU holds the run. */
#define NO_HOLDER UINT32_MAX

/** @brief One mapped guest range. */
typedef struct sched_range {
    /** @brief First guest address. */
    uint64_t lo;
    /** @brief Last guest address (inclusive, so a range may end at the top of the space). */
    uint64_t hi;
    /** @brief The device whose handlers answer. */
    rl_device *owner;
    /** @brief Devices caught up before an access: the owner and its observers. */
    uint64_t sync;
} sched_range;

struct rl_sched_cpu {
    rl_time *time;
    rl_time own;
    uint64_t origin_tick;
    uint64_t origin_cycle;
    uint32_t ratio;
    uint32_t index;
    uint64_t lines;
    uint64_t mask;
    rl_irq_hook *irq;
    rl_sched *sched;
    bool active;
    bool halted;
};

struct rl_sched {
    rl_sched_config cfg;
    rl_sched_stats stats;
    uint64_t time;
    uint64_t seq;
    rl_event **heap;
    uint32_t qn, qcap;
    rl_device *devs[RL_SCHED_MAX_DEVICES];
    uint32_t ndev;
    sched_range *ranges;
    uint32_t nr, rcap;
    uint32_t last;
    rl_sched_cpu cpus[RL_SCHED_MAX_CPUS];
    uint32_t ncpu;
    uint32_t route[RL_SCHED_LINES];
    uint32_t holder;
    bool started;
    pthread_mutex_t mu;
    pthread_cond_t cv;
};

rl_sched *rl_sched_create(const rl_sched_config *cfg)
{
    rl_sched *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;
    if (cfg)
        s->cfg = *cfg;
    if (s->cfg.slice == 0)
        s->cfg.slice = RL_SCHED_SLICE_DEFAULT;
    if (s->cfg.quantum == 0)
        s->cfg.quantum = 1;
    s->holder = NO_HOLDER;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    return s;
}

void rl_sched_destroy(rl_sched *s)
{
    if (!s)
        return;
    for (uint32_t i = 0; i < s->qn; i++)
        s->heap[i]->slot = 0;
    pthread_cond_destroy(&s->cv);
    pthread_mutex_destroy(&s->mu);
    free(s->heap);
    free(s->ranges);
    free(s);
}

const rl_sched_stats *rl_sched_get_stats(const rl_sched *s) { return &s->stats; }
uint64_t rl_sched_time(const rl_sched *s) { return s->time; }
rl_time *rl_sched_clock(rl_sched_cpu *c) { return c->time; }
void rl_sched_set_irq(rl_sched_cpu *c, rl_irq_hook *hook) { c->irq = hook; }
rl_sched *rl_sched_of(const rl_sched_cpu *c) { return c->sched; }
uint64_t rl_sched_pending(const rl_sched_cpu *c) { return c->lines & c->mask; }

rl_sched_cpu *rl_sched_current(rl_sched *s) { return s->holder == NO_HOLDER ? NULL : &s->cpus[s->holder]; }

uint64_t rl_sched_cycles(const rl_sched_cpu *c) { return c->time->deadline - (uint64_t)c->time->budget; }

uint64_t rl_sched_now(const rl_sched_cpu *c) { return c->origin_tick + (rl_sched_cycles(c) - c->origin_cycle) * c->ratio; }

static bool ev_before(const rl_event *a, const rl_event *b)
{
    return a->when < b->when || (a->when == b->when && a->seq < b->seq);
}

static void heap_put(rl_sched *s, uint32_t i, rl_event *e)
{
    s->heap[i] = e;
    e->slot = i + 1;
}

static void sift_up(rl_sched *s, uint32_t i)
{
    rl_event *e = s->heap[i];
    while (i > 0) {
        uint32_t p = (i - 1) / 2;
        if (!ev_before(e, s->heap[p]))
            break;
        heap_put(s, i, s->heap[p]);
        i = p;
    }
    heap_put(s, i, e);
}

static void sift_down(rl_sched *s, uint32_t i)
{
    rl_event *e = s->heap[i];
    for (;;) {
        uint32_t l = 2 * i + 1;
        if (l >= s->qn)
            break;
        uint32_t m = l + 1 < s->qn && ev_before(s->heap[l + 1], s->heap[l]) ? l + 1 : l;
        if (!ev_before(s->heap[m], e))
            break;
        heap_put(s, i, s->heap[m]);
        i = m;
    }
    heap_put(s, i, e);
}

static void heap_remove(rl_sched *s, rl_event *e)
{
    uint32_t i = e->slot - 1;
    e->slot = 0;
    s->qn--;
    if (i == s->qn)
        return;
    heap_put(s, i, s->heap[s->qn]);
    if (i > 0 && ev_before(s->heap[i], s->heap[(i - 1) / 2]))
        sift_up(s, i);
    else
        sift_down(s, i);
}

static uint64_t end_tick(const rl_sched_cpu *c)
{
    return c->origin_tick + (c->time->deadline - c->origin_cycle) * c->ratio;
}

static void set_budget(rl_sched_cpu *c, uint64_t now, uint64_t limit)
{
    uint64_t cyc = rl_sched_cycles(c);
    uint64_t n = limit > now ? (limit - now + c->ratio - 1) / c->ratio : 0;
    c->time->budget = (int64_t)n;
    c->time->deadline = cyc + n;
}

void rl_sched_break(rl_sched_cpu *c)
{
    c->time->deadline = rl_sched_cycles(c);
    c->time->budget = 0;
}

void rl_sched_stall(rl_sched_cpu *c, uint64_t cycles) { c->time->budget -= (int64_t)cycles; }

static void cut(rl_sched *s, uint64_t when)
{
    rl_sched_cpu *c = rl_sched_current(s);
    if (c && when < end_tick(c))
        set_budget(c, rl_sched_now(c), when);
}

void rl_sched_post(rl_sched *s, rl_event *e, uint64_t when)
{
    if (e->slot)
        heap_remove(s, e);
    if (s->qn == s->qcap) {
        uint32_t cap = s->qcap ? s->qcap * 2 : 16;
        rl_event **h = realloc(s->heap, cap * sizeof *h);
        if (!h) {
            rl_log(RL_LOG_ERROR, "sched", "out of memory queueing an event");
            abort();
        }
        s->heap = h;
        s->qcap = cap;
    }
    e->when = when;
    e->seq = s->seq++;
    heap_put(s, s->qn++, e);
    sift_up(s, s->qn - 1);
    cut(s, when);
}

void rl_sched_cancel(rl_sched *s, rl_event *e)
{
    if (e->slot)
        heap_remove(s, e);
}

static void catch_up(rl_sched *s, rl_device *d, uint64_t when)
{
    if (d->now >= when)
        return;
    uint64_t from = d->now;
    d->now = when;
    if (d->catch_up) {
        s->stats.catch_ups++;
        d->catch_up(d, from, when);
    }
}

void rl_sched_sync(rl_sched *s, uint64_t devices, uint64_t when)
{
    while (devices) {
        uint32_t i = (uint32_t)__builtin_ctzll(devices);
        devices &= devices - 1;
        catch_up(s, s->devs[i], when);
    }
}

static void fire_due(rl_sched *s, uint64_t upto)
{
    while (s->qn && s->heap[0]->when <= upto) {
        rl_event *e = s->heap[0];
        heap_remove(s, e);
        if (e->when > s->time)
            s->time = e->when;
        if (e->dev)
            catch_up(s, e->dev, e->when);
        s->stats.events++;
        e->fire(s, e, e->when);
    }
    if (upto > s->time)
        s->time = upto;
}

static uint64_t frontier(const rl_sched *s, const rl_sched_cpu *c, uint64_t now)
{
    uint64_t f = now;
    for (uint32_t i = 0; i < s->ncpu; i++) {
        const rl_sched_cpu *o = &s->cpus[i];
        if (o != c && o->active) {
            uint64_t t = rl_sched_now(o);
            if (t < f)
                f = t;
        }
    }
    return f;
}

static uint64_t deadline(const rl_sched *s, const rl_sched_cpu *c, uint64_t now)
{
    uint64_t lim = now + s->cfg.slice;
    if (s->qn && s->heap[0]->when < lim)
        lim = s->heap[0]->when;
    for (uint32_t i = 0; i < s->ncpu; i++) {
        const rl_sched_cpu *o = &s->cpus[i];
        if (o != c && o->active) {
            uint64_t t = rl_sched_now(o) + s->cfg.quantum;
            if (t < lim)
                lim = t;
        }
    }
    return lim;
}

static uint32_t slowest_other(const rl_sched *s, const rl_sched_cpu *c)
{
    uint32_t best = NO_HOLDER;
    uint64_t at = 0;
    for (uint32_t i = 0; i < s->ncpu; i++) {
        const rl_sched_cpu *o = &s->cpus[i];
        if (o == c || !o->active)
            continue;
        uint64_t t = rl_sched_now(o);
        if (best == NO_HOLDER || t < at) {
            best = i;
            at = t;
        }
    }
    return best;
}

static void hand_to(rl_sched *s, uint32_t next)
{
    pthread_mutex_lock(&s->mu);
    s->holder = next;
    s->stats.switches++;
    pthread_cond_broadcast(&s->cv);
    pthread_mutex_unlock(&s->mu);
}

static void wait_for_run(rl_sched *s, const rl_sched_cpu *c)
{
    pthread_mutex_lock(&s->mu);
    while (s->holder != c->index)
        pthread_cond_wait(&s->cv, &s->mu);
    pthread_mutex_unlock(&s->mu);
}

rl_sched_cpu *rl_sched_add_cpu(rl_sched *s, uint32_t ratio)
{
    if (ratio == 0 || s->ncpu == RL_SCHED_MAX_CPUS || s->started)
        return NULL;
    rl_sched_cpu *c = &s->cpus[s->ncpu];
    memset(c, 0, sizeof *c);
    c->time = &c->own;
    c->ratio = ratio;
    c->index = s->ncpu++;
    c->sched = s;
    c->active = true;
    c->origin_tick = s->time;
    if (s->holder == NO_HOLDER)
        s->holder = c->index;
    return c;
}

int rl_sched_attach(rl_sched_cpu *c, struct rl_thread *t)
{
    rl_time *tm = t && t->ctx ? rl_ctx_time(t->ctx) : NULL;
    if (!tm)
        return -1;
    *tm = *c->time;
    c->time = tm;
    t->clock = c;
    return 0;
}

void rl_sched_enter(rl_sched_cpu *c) { wait_for_run(c->sched, c); }

void rl_sched_leave(rl_sched_cpu *c)
{
    rl_sched *s = c->sched;
    c->active = false;
    if (s->holder == c->index)
        hand_to(s, slowest_other(s, c));
}

void rl_sched_set_ratio(rl_sched_cpu *c, uint32_t ratio)
{
    if (ratio == 0)
        return;
    c->origin_tick = rl_sched_now(c);
    c->origin_cycle = rl_sched_cycles(c);
    c->ratio = ratio;
    rl_sched_break(c);
}

int rl_sched_run(rl_sched_cpu *c)
{
    rl_sched *s = c->sched;
    s->started = true;
    s->stats.runs++;
    for (;;) {
        uint64_t now = rl_sched_now(c);
        fire_due(s, frontier(s, c, now));
        uint64_t lim = deadline(s, c, now);
        if (lim > now) {
            set_budget(c, now, lim);
            break;
        }
        uint32_t next = slowest_other(s, c);
        if (next == NO_HOLDER) {
            set_budget(c, now, now + 1);
            break;
        }
        set_budget(c, now, now);
        hand_to(s, next);
        wait_for_run(s, c);
    }
    return rl_sched_pending(c) != 0;
}

int rl_sched_halt(rl_sched_cpu *c)
{
    rl_sched *s = c->sched;
    int got = 0;
    c->halted = true;
    for (;;) {
        if (rl_sched_pending(c)) {
            got = 1;
            break;
        }
        bool others = false;
        for (uint32_t i = 0; i < s->ncpu; i++)
            if (i != c->index && s->cpus[i].active && !s->cpus[i].halted)
                others = true;
        if (!s->qn && !others)
            break;
        c->time->budget = 0;
        if (rl_sched_run(c)) {
            got = 1;
            break;
        }
    }
    c->halted = false;
    return got;
}

int rl_sched_add_device(rl_sched *s, rl_device *d)
{
    if (s->ndev == RL_SCHED_MAX_DEVICES)
        return -1;
    d->index = s->ndev;
    d->now = s->time;
    d->sched = s;
    s->devs[s->ndev++] = d;
    return 0;
}

int rl_sched_map(rl_sched *s, uint64_t addr, uint64_t size, rl_device *owner, uint64_t observers)
{
    if (size == 0 || addr + (size - 1) < addr || owner->sched != s)
        return -1;
    uint64_t hi = addr + (size - 1);
    uint32_t at = 0;
    while (at < s->nr && s->ranges[at].hi < addr)
        at++;
    if (at < s->nr && s->ranges[at].lo <= hi)
        return -1;
    if (s->nr == s->rcap) {
        uint32_t cap = s->rcap ? s->rcap * 2 : 16;
        sched_range *r = realloc(s->ranges, cap * sizeof *r);
        if (!r)
            return -1;
        s->ranges = r;
        s->rcap = cap;
    }
    memmove(&s->ranges[at + 1], &s->ranges[at], (s->nr - at) * sizeof *s->ranges);
    s->ranges[at] = (sched_range){.lo = addr, .hi = hi, .owner = owner, .sync = observers | rl_device_bit(owner)};
    s->nr++;
    s->last = 0;
    return 0;
}

static const sched_range *find_range(rl_sched *s, uint64_t addr)
{
    if (s->nr == 0)
        return NULL;
    const sched_range *r = &s->ranges[s->last];
    if (addr >= r->lo && addr <= r->hi)
        return r;
    uint32_t lo = 0, hi = s->nr;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (s->ranges[mid].hi < addr)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == s->nr || s->ranges[lo].lo > addr)
        return NULL;
    s->last = lo;
    return &s->ranges[lo];
}

static const sched_range *prepare(rl_sched_cpu *c, uint64_t addr, uint64_t at)
{
    rl_sched *s = c->sched;
    s->started = true;
    s->stats.accesses++;
    if (s->qn && s->heap[0]->when <= at)
        fire_due(s, frontier(s, c, at));
    const sched_range *r = find_range(s, addr);
    if (!r) {
        s->stats.unmapped++;
        return NULL;
    }
    rl_sched_sync(s, r->sync, at);
    return r;
}

uint64_t rl_sched_read(rl_sched_cpu *c, uint64_t addr, uint32_t size)
{
    uint64_t at = rl_sched_now(c);
    const sched_range *r = prepare(c, addr, at);
    if (!r || !r->owner->read)
        return size >= 8 ? UINT64_MAX : (1ull << (size * 8)) - 1;
    return r->owner->read(r->owner, addr, size, at);
}

void rl_sched_write(rl_sched_cpu *c, uint64_t addr, uint32_t size, uint64_t value)
{
    uint64_t at = rl_sched_now(c);
    const sched_range *r = prepare(c, addr, at);
    if (r && r->owner->write)
        r->owner->write(r->owner, addr, size, value, at);
}

void rl_sched_route(rl_sched *s, uint32_t line, uint32_t cpu)
{
    if (line < RL_SCHED_LINES && cpu < s->ncpu)
        s->route[line] = cpu;
}

void rl_sched_raise(rl_sched *s, uint32_t line)
{
    if (line >= RL_SCHED_LINES || s->ncpu == 0)
        return;
    rl_sched_cpu *c = &s->cpus[s->route[line]];
    uint64_t bit = 1ull << line;
    c->lines |= bit;
    if (c->mask & bit)
        rl_sched_break(c);
}

void rl_sched_lower(rl_sched *s, uint32_t line)
{
    if (line < RL_SCHED_LINES && s->ncpu)
        s->cpus[s->route[line]].lines &= ~(1ull << line);
}

void rl_device_raise(rl_device *d)
{
    if (d->line != RL_LINE_NONE)
        rl_sched_raise(d->sched, d->line);
}

void rl_sched_set_mask(rl_sched_cpu *c, uint64_t mask)
{
    c->mask = mask;
    if (c->lines & mask)
        rl_sched_break(c);
}

static rl_sched_cpu *ctx_clock(rl_ctx *c)
{
    struct rl_thread *t = rl_common(c)->thread;
    return t ? t->clock : NULL;
}

int rl_sync(rl_ctx *c, uint8_t *mem, uint64_t next)
{
    rl_sched_cpu *k = ctx_clock(c);
    if (!k)
        return rl_guest_fatal(c, "sched", "sync point reached on a thread without a scheduler clock");
    for (;;) {
        if (k->time->budget <= 0)
            rl_sched_run(k);
        uint64_t pc = k->irq ? k->irq(c, mem, next) : RL_NO_IRQ;
        if (pc == RL_NO_IRQ)
            return RL_OK;
        k->sched->stats.irqs++;
        rl_fn fn = rl_dispatch_find(pc);
        int r = fn ? fn(c, mem) : rl_core_miss(c, mem, pc);
        if (r != RL_OK)
            return RL_PENDING;
    }
}

uint64_t rl_io_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size)
{
    (void)mem;
    rl_sched_cpu *k = ctx_clock(c);
    if (!k) {
        rl_guest_fatal(c, "sched", "device read at 0x%llx on a thread without a scheduler clock", (unsigned long long)addr);
        return 0;
    }
    return rl_sched_read(k, addr, size);
}

void rl_io_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value)
{
    (void)mem;
    rl_sched_cpu *k = ctx_clock(c);
    if (!k) {
        rl_guest_fatal(c, "sched", "device write at 0x%llx on a thread without a scheduler clock", (unsigned long long)addr);
        return;
    }
    rl_sched_write(k, addr, size, value);
}
