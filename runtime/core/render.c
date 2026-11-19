/**
 * @file render.c
 * @brief The render log and its thread (relift/render.h): a ring of records
 * with one putter and one applier. Positions only grow; a record never wraps,
 * a pad record fills the ring's end instead. The putter publishes a record by
 * moving head, the applier frees it by moving tail; each sleeps on the other
 * only when it has to, and says so first, so a record costs no lock.
 */
#include "core.h"

#include <relift/render.h>

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

/** @brief The default log capacity. */
#define DEFAULT_BYTES ((size_t)4 << 20)
/** @brief The kind of a record that only fills the ring's end. */
#define PAD UINT32_MAX

typedef struct rec {
    uint32_t kind, len;
} rec;

struct rl_render {
    rl_render_apply_fn *apply;
    void *user;
    uint8_t *ring;
    size_t size;
    size_t max;
    /** @brief Bytes put (the putter's) and applied (the applier's). */
    size_t head, tail;
    /** @brief Where rl_render_begin put its record, and its size in the ring. */
    size_t open_at, open_size;
    /** @brief The ring bytes the putter waits for. */
    size_t need;
    uint8_t *scratch;
    uint32_t open_kind, open_len;
    bool sync;
    bool started;
    bool stop;
    /** @brief The putter waits for room or a drain; the applier waits for records. */
    bool put_waits, apply_waits;
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
};

static size_t round8(size_t n) { return (n + 7u) & ~(size_t)7; }

static size_t load(const size_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
static void store(size_t *p, size_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }

size_t rl_render_max_record(size_t log_bytes) { return (log_bytes ? log_bytes : DEFAULT_BYTES) / 4u - sizeof(rec); }

/** @brief Wakes the other side when it said it sleeps. */
static void wake(rl_render *r, bool *flag)
{
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (!__atomic_load_n(flag, __ATOMIC_SEQ_CST))
        return;
    pthread_mutex_lock(&r->lock);
    __atomic_store_n(flag, false, __ATOMIC_SEQ_CST);
    pthread_cond_broadcast(&r->cond);
    pthread_mutex_unlock(&r->lock);
}

/** @brief Sleeps until ready(r) holds, saying so in flag first. */
static void wait_for(rl_render *r, bool *flag, bool (*ready)(rl_render *))
{
    while (!ready(r)) {
        pthread_mutex_lock(&r->lock);
        __atomic_store_n(flag, true, __ATOMIC_SEQ_CST);
        __atomic_thread_fence(__ATOMIC_SEQ_CST);
        while (!ready(r) && __atomic_load_n(flag, __ATOMIC_SEQ_CST))
            pthread_cond_wait(&r->cond, &r->lock);
        __atomic_store_n(flag, false, __ATOMIC_SEQ_CST);
        pthread_mutex_unlock(&r->lock);
    }
}

static bool has_records(rl_render *r) { return load(&r->head) != r->tail || __atomic_load_n(&r->stop, __ATOMIC_ACQUIRE); }

static void *thread_main(void *arg)
{
    rl_render *r = arg;
    for (;;) {
        wait_for(r, &r->apply_waits, has_records);
        size_t head = load(&r->head);
        if (head == r->tail)
            break;
        while (r->tail != head) {
            size_t at = r->tail % r->size;
            const rec *h = (const rec *)(r->ring + at);
            if (h->kind != PAD)
                r->apply(r->user, h->kind, h + 1, h->len);
            store(&r->tail, r->tail + (h->kind == PAD ? r->size - at : sizeof(rec) + round8(h->len)));
            wake(r, &r->put_waits);
        }
    }
    return NULL;
}

static bool start(rl_render *r)
{
    if (r->started)
        return true;
    r->stop = false;
    if (pthread_create(&r->thread, NULL, thread_main, r) != 0)
        return false;
    r->started = true;
    return true;
}

static void stop_thread(rl_render *r)
{
    if (!r->started)
        return;
    __atomic_store_n(&r->stop, true, __ATOMIC_RELEASE);
    __atomic_store_n(&r->apply_waits, true, __ATOMIC_SEQ_CST);
    wake(r, &r->apply_waits);
    pthread_join(r->thread, NULL);
    r->started = false;
}

rl_render *rl_render_create(const rl_render_config *cfg)
{
    rl_render *r = calloc(1, sizeof *r);
    if (!r)
        return NULL;
    r->apply = cfg->apply;
    r->user = cfg->user;
    r->size = round8(cfg->log_bytes ? cfg->log_bytes : DEFAULT_BYTES);
    r->max = rl_render_max_record(r->size);
    const char *env = getenv("RELIFT_RENDER");
    r->sync = cfg->sync || (env && strcmp(env, "sync") == 0);
    r->ring = malloc(r->size);
    r->scratch = malloc(r->max);
    pthread_mutex_init(&r->lock, NULL);
    pthread_cond_init(&r->cond, NULL);
    if (!r->ring || !r->scratch || (!r->sync && !start(r))) {
        rl_render_free(r);
        return NULL;
    }
    return r;
}

void rl_render_free(rl_render *r)
{
    if (!r)
        return;
    stop_thread(r);
    pthread_mutex_destroy(&r->lock);
    pthread_cond_destroy(&r->cond);
    free(r->ring);
    free(r->scratch);
    free(r);
}

static bool has_room(rl_render *r) { return r->size - (r->head - load(&r->tail)) >= r->need; }

/** @brief Room for n ring bytes in one piece at head (after a pad when the end is too short). */
static size_t reserve(rl_render *r, size_t n)
{
    size_t at = r->head % r->size;
    if (r->size - at < n) {
        r->need = r->size - at;
        wake(r, &r->apply_waits);
        wait_for(r, &r->put_waits, has_room);
        rec *pad = (rec *)(r->ring + at);
        pad->kind = PAD;
        pad->len = 0;
        store(&r->head, r->head + (r->size - at));
        wake(r, &r->apply_waits);
        at = 0;
    }
    r->need = n;
    if (!has_room(r))
        wake(r, &r->apply_waits);
    wait_for(r, &r->put_waits, has_room);
    return at;
}

void *rl_render_begin(rl_render *r, uint32_t kind, uint32_t len)
{
    r->open_kind = kind;
    r->open_len = len;
    if (r->sync)
        return r->scratch;
    r->open_size = sizeof(rec) + round8(len);
    r->open_at = reserve(r, r->open_size);
    rec *h = (rec *)(r->ring + r->open_at);
    h->kind = kind;
    h->len = len;
    return h + 1;
}

void rl_render_commit(rl_render *r)
{
    if (r->sync) {
        r->apply(r->user, r->open_kind, r->scratch, r->open_len);
        return;
    }
    store(&r->head, r->head + r->open_size);
    if (r->head - load(&r->tail) >= r->size / 8u)
        wake(r, &r->apply_waits);
}

void rl_render_flush(rl_render *r)
{
    if (!r->sync)
        wake(r, &r->apply_waits);
}

void rl_render_put(rl_render *r, uint32_t kind, const void *data, uint32_t len)
{
    if (r->sync) {
        r->apply(r->user, kind, data, len);
        return;
    }
    void *p = rl_render_begin(r, kind, len);
    if (len)
        memcpy(p, data, len);
    rl_render_commit(r);
}

static bool drained(rl_render *r) { return load(&r->tail) == r->head; }

void rl_render_drain(rl_render *r)
{
    if (r->sync)
        return;
    wake(r, &r->apply_waits);
    wait_for(r, &r->put_waits, drained);
}

void rl_render_set_sync(rl_render *r, bool sync)
{
    if (sync == r->sync)
        return;
    if (sync) {
        rl_render_drain(r);
        r->sync = true;
        return;
    }
    if (start(r))
        r->sync = false;
}

bool rl_render_is_sync(const rl_render *r) { return r->sync; }
