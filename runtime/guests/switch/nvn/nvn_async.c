/**
 * @file nvn_async.c
 * @brief The replay worker (RELIFT_NVN_ASYNC_REPLAY): one thread that runs queued items in the order they were posted,
 * and the drain every other NVN entry point makes first. See NOTES.md of the lane that made it for the design; in short,
 * a submit's replay can run beside the game's own thread, and anything that could see what replay changes waits for the
 * worker to be idle.
 */
#include "nvn_async.h"
#include "nvn_priv.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

typedef struct item {
    nvn_async_fn run;
    void *arg;
    struct item *next;
} item;

static struct {
    pthread_mutex_t mu;
    pthread_cond_t work, idle;
    item *head, *tail;
    bool busy, started;
    /** @brief Items posted and not yet finished, read without the lock by every entry point's drain. */
    _Atomic uint32_t pending;
} A = { .mu = PTHREAD_MUTEX_INITIALIZER, .work = PTHREAD_COND_INITIALIZER, .idle = PTHREAD_COND_INITIALIZER };

static _Thread_local bool t_worker;

bool nvn_async_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RELIFT_NVN_ASYNC_REPLAY");
        on = e && *e == '1';
    }
    return on != 0;
}

static void *worker_main(void *arg)
{
    (void)arg;
    t_worker = true;
    pthread_mutex_lock(&A.mu);
    for (;;) {
        while (!A.head)
            pthread_cond_wait(&A.work, &A.mu);
        item *it = A.head;
        A.head = it->next;
        if (!A.head)
            A.tail = NULL;
        A.busy = true;
        pthread_mutex_unlock(&A.mu);
        it->run(it->arg);
        free(it);
        pthread_mutex_lock(&A.mu);
        A.busy = false;
        atomic_fetch_sub_explicit(&A.pending, 1, memory_order_release);
        if (!A.head)
            pthread_cond_broadcast(&A.idle);
    }
    return NULL;
}

bool nvn_async_post(nvn_async_fn run, void *arg)
{
    item *it = calloc(1, sizeof *it);
    if (!it)
        return false;
    it->run = run;
    it->arg = arg;
    pthread_mutex_lock(&A.mu);
    if (!A.started) {
        pthread_t th;
        if (pthread_create(&th, NULL, worker_main, NULL) != 0) {
            pthread_mutex_unlock(&A.mu);
            free(it);
            return false;
        }
        pthread_detach(th);
        A.started = true;
    }
    if (A.tail)
        A.tail->next = it;
    else
        A.head = it;
    A.tail = it;
    atomic_fetch_add_explicit(&A.pending, 1, memory_order_relaxed);
    pthread_cond_signal(&A.work);
    pthread_mutex_unlock(&A.mu);
    return true;
}

void nvn_async_drain(void)
{
    if (t_worker || !atomic_load_explicit(&A.pending, memory_order_acquire))
        return;
    pthread_mutex_lock(&A.mu);
    while (A.head || A.busy)
        pthread_cond_wait(&A.idle, &A.mu);
    pthread_mutex_unlock(&A.mu);
}

bool nvn_async_on_worker(void) { return t_worker; }

uint32_t nvn_async_pending(void) { return atomic_load_explicit(&A.pending, memory_order_acquire); }

static uint64_t g_stack_top;

void nvn_async_set_stack(uint64_t top) { g_stack_top = top; }

rl_ctx *nvn_async_snapshot(const rl_ctx *c)
{
    rl_ctx *snap = malloc(sizeof *snap);
    if (!snap)
        return NULL;
    *snap = *c;
    /* The worker's own guest stack: the game's thread goes on using the one it had. */
    if (g_stack_top)
        snap->sp = g_stack_top;
    return snap;
}
