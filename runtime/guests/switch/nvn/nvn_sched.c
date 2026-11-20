/**
 * @file nvn_sched.c
 * @brief The replay order of submitted command sets, see nvn_sched.h.
 */
#include "nvn_sched.h"

#include <stdlib.h>
#include <string.h>

#define MAX_BLOCKED 16u

bool nvn_event_wait_met(uint32_t cur, uint32_t mode, uint32_t value)
{
    if (mode == NVN_EVENT_WAIT_GEQUAL_WRAP)
        return (int32_t)(cur - value) >= 0;
    return cur == value;
}

static bool push(nvn_sched_item **items, uint32_t *n, uint32_t *cap, const nvn_cmd *e, const nvn_cmdset *s)
{
    if (*n == *cap) {
        uint32_t nc = *cap ? *cap * 2 : 64;
        nvn_sched_item *p = realloc(*items, nc * sizeof **items);
        if (!p)
            return false;
        *items = p;
        *cap = nc;
    }
    (*items)[(*n)++] = (nvn_sched_item){ e, s };
    return true;
}

static int flatten(const nvn_cmdset *s, nvn_sched_item **items, uint32_t *n, uint32_t *cap, uint64_t *bad, unsigned depth)
{
    for (uint32_t i = 0; i < s->n; i++) {
        const nvn_cmd *e = &s->cmds[i];
        if (e->op != NVN_CMD_CALL_COMMAND && e->op != NVN_CMD_CALL_COMMANDS) {
            if (!push(items, n, cap, e, s))
                return -1;
            continue;
        }
        uint32_t count = 1;
        uint64_t one = e->x[1];
        const uint64_t *handles = &one;
        if (e->op == NVN_CMD_CALL_COMMANDS) {
            count = (uint32_t)e->x[1];
            handles = nvn_cmd_bytes(s, e->x[2]);
            if (count && !handles) {
                *bad = 0;
                return 1;
            }
        }
        for (uint32_t k = 0; k < count; k++) {
            const nvn_cmdset *inner = nvn_cmdset_of(handles[k]);
            if (!inner || depth > 16) {
                *bad = handles[k];
                return 1;
            }
            int rc = flatten(inner, items, n, cap, bad, depth + 1);
            if (rc)
                return rc;
        }
    }
    return 0;
}

int nvn_sched_flatten(const nvn_cmdset *s, nvn_sched_item **items, uint32_t *n, uint64_t *bad)
{
    uint32_t cap = 0;
    *items = NULL;
    *n = 0;
    *bad = 0;
    int rc = flatten(s, items, n, &cap, bad, 0);
    if (rc) {
        free(*items);
        *items = NULL;
        *n = 0;
    }
    return rc;
}

bool nvn_sched_add(nvn_sched *s, uint64_t queue, nvn_sched_item *items, uint32_t n, void *user)
{
    nvn_sched_run *r = calloc(1, sizeof *r);
    if (!r)
        return false;
    r->queue = queue;
    r->items = items;
    r->n = n;
    r->user = user;
    if (s->tail)
        s->tail->next = r;
    else
        s->head = r;
    s->tail = r;
    s->pending++;
    return true;
}

void nvn_sched_run_free(nvn_sched_run *r)
{
    if (!r)
        return;
    free(r->items);
    free(r);
}

static void unlink_run(nvn_sched *s, nvn_sched_run *prev, nvn_sched_run *r)
{
    if (prev)
        prev->next = r->next;
    else
        s->head = r->next;
    if (s->tail == r)
        s->tail = prev;
    r->next = NULL;
    s->pending--;
}

/** @brief Replays r from where it stands; sets *blocked at a wait that is not met. Returns RL_OK or a stop. */
static int advance(nvn_sched_run *r, const nvn_sched_ops *ops, bool *blocked, bool *moved)
{
    while (r->pos < r->n) {
        const nvn_cmd *e = r->items[r->pos].e;
        if (e->op == NVN_CMD_WAIT_EVENT || e->op == NVN_CMD_SIGNAL_EVENT) {
            uint32_t *w = e->x[1] ? ops->word(r->user, e->x[1]) : NULL;
            if (!w)
                return ops->exec(r->user, e, r->items[r->pos].s);
            if (!ops->flush(r->user))
                return ops->exec(r->user, e, r->items[r->pos].s);
            if (e->op == NVN_CMD_WAIT_EVENT) {
                uint32_t cur = __atomic_load_n(w, __ATOMIC_ACQUIRE);
                if (!nvn_event_wait_met(cur, (uint32_t)e->x[2], (uint32_t)e->x[3])) {
                    *blocked = true;
                    return RL_OK;
                }
            } else if ((uint32_t)e->x[2] == NVN_EVENT_SIGNAL_ADD) {
                __atomic_add_fetch(w, (uint32_t)e->x[5], __ATOMIC_RELEASE);
            } else {
                __atomic_store_n(w, (uint32_t)e->x[5], __ATOMIC_RELEASE);
            }
        } else {
            int rc = ops->exec(r->user, e, r->items[r->pos].s);
            if (rc != RL_OK)
                return rc;
        }
        r->pos++;
        *moved = true;
    }
    return RL_OK;
}

int nvn_sched_pump(nvn_sched *s, const nvn_sched_ops *ops, nvn_sched_run **done)
{
    nvn_sched_run *done_tail = NULL;
    for (bool moved = true; moved;) {
        moved = false;
        uint64_t blocked[MAX_BLOCKED];
        uint32_t nblocked = 0;
        nvn_sched_run *prev = NULL, *r = s->head;
        while (r) {
            nvn_sched_run *next = r->next;
            bool held = false;
            for (uint32_t i = 0; i < nblocked; i++)
                held = held || blocked[i] == r->queue;
            if (held) {
                prev = r;
                r = next;
                continue;
            }
            bool stuck = false;
            int rc = advance(r, ops, &stuck, &moved);
            if (rc != RL_OK)
                return rc;
            if (stuck) {
                if (nblocked < MAX_BLOCKED)
                    blocked[nblocked++] = r->queue;
                prev = r;
            } else {
                unlink_run(s, prev, r);
                if (done_tail)
                    done_tail->next = r;
                else
                    *done = r;
                done_tail = r;
                moved = true;
            }
            r = next;
        }
    }
    return RL_OK;
}
