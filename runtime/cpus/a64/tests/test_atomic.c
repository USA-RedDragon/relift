/**
 * @file test_atomic.c
 * @brief The LSE atomic helpers: every rl_atomic op at every size with
 * signed and unsigned edge cases, rl_atomic128, rl_cas and rl_cas128
 * success and failure, and multi-threaded counters that prove atomicity.
 */
#include "runner.h"

#include <relift/a64/abi.h>

#include <pthread.h>

/** @brief Scratch memory standing in for guest memory; guest address a is buf + a. */
static _Alignas(64) uint8_t buf[256];

/** @brief Reads size bytes at offset a. */
static uint64_t get(uint64_t a, unsigned size) {
    uint64_t v = 0;
    memcpy(&v, buf + a, size);
    return v;
}

static void put(uint64_t a, unsigned size, uint64_t v) {
    memcpy(buf + a, &v, size);
}

/** @brief The reference result of op on the old value o and operand v at size bytes (both truncated). */
static uint64_t ref(int op, unsigned size, uint64_t o, uint64_t v) {
    unsigned bits = 8 * size;
    uint64_t mask = bits == 64 ? ~0ull : (1ull << bits) - 1;
    o &= mask;
    v &= mask;
    int64_t so = bits == 64 ? (int64_t)o : (int64_t)(o << (64 - bits)) >> (64 - bits);
    int64_t sv = bits == 64 ? (int64_t)v : (int64_t)(v << (64 - bits)) >> (64 - bits);
    uint64_t r;
    switch (op) {
    case RL_AT_ADD: r = o + v; break;
    case RL_AT_CLR: r = o & ~v; break;
    case RL_AT_EOR: r = o ^ v; break;
    case RL_AT_SET: r = o | v; break;
    case RL_AT_SMAX: r = so > sv ? o : v; break;
    case RL_AT_SMIN: r = so < sv ? o : v; break;
    case RL_AT_UMAX: r = o > v ? o : v; break;
    case RL_AT_UMIN: r = o < v ? o : v; break;
    default: r = v; break;
    }
    return r & mask;
}

static const uint64_t edges[] = {
    0, 1, 2, 0x7f, 0x80, 0xff, 0x7fff, 0x8000, 0xffff, 0x7fffffff, 0x80000000u, 0xffffffffu,
    0x7fffffffffffffffull, 0x8000000000000000ull, 0xffffffffffffffffull, 0x0123456789abcdefull,
    0xfedcba9876543210ull, 0x00ff00ff00ff00ffull,
};
#define NEDGES (sizeof edges / sizeof edges[0])

TEST(atomic_every_op_and_size)
{
    static const unsigned sizes[] = {1, 2, 4, 8};
    unsigned n = 0;
    for (int op = RL_AT_ADD; op <= RL_AT_SWP; op++)
        for (unsigned si = 0; si < 4; si++)
            for (unsigned i = 0; i < NEDGES; i++)
                for (unsigned j = 0; j < NEDGES; j++)
                    for (int ord = RL_ORD_PLAIN; ord <= RL_ORD_SC; ord++) {
                        unsigned size = sizes[si];
                        uint64_t a = 8 * size; /* aligned, and away from the canaries */
                        memset(buf, 0xa5, sizeof buf);
                        put(a, size, edges[i]);
                        uint64_t mask = size == 8 ? ~0ull : (1ull << (8 * size)) - 1;
                        uint64_t old = rl_atomic(buf, a, size, op, edges[j], ord);
                        CHECK_EQ(old, edges[i] & mask);
                        CHECK_EQ(get(a, size), ref(op, size, edges[i], edges[j]));
                        CHECK_EQ(buf[a - 1], 0xa5);
                        CHECK_EQ(buf[a + size], 0xa5);
                        n++;
                    }
    CHECK_EQ(n, 9u * 4 * NEDGES * NEDGES * 2);
}

TEST(atomic_signed_cases)
{
    /* -1 against 1 at each size: signed max keeps 1, unsigned max keeps -1. */
    put(0, 1, 0xff);
    CHECK_EQ(rl_atomic(buf, 0, 1, RL_AT_SMAX, 1, RL_ORD_SC), 0xff);
    CHECK_EQ(get(0, 1), 1);
    put(0, 1, 0xff);
    CHECK_EQ(rl_atomic(buf, 0, 1, RL_AT_UMAX, 1, RL_ORD_SC), 0xff);
    CHECK_EQ(get(0, 1), 0xff);
    put(0, 2, 0x8000);
    rl_atomic(buf, 0, 2, RL_AT_SMIN, 0x7fff, RL_ORD_PLAIN);
    CHECK_EQ(get(0, 2), 0x8000);
    rl_atomic(buf, 0, 2, RL_AT_UMIN, 0x7fff, RL_ORD_PLAIN);
    CHECK_EQ(get(0, 2), 0x7fff);
    put(0, 4, 0x80000000u);
    rl_atomic(buf, 0, 4, RL_AT_SMAX, 0xffffffffu, RL_ORD_PLAIN);
    CHECK_EQ(get(0, 4), 0xffffffffu);
    put(0, 8, 0x8000000000000000ull);
    rl_atomic(buf, 0, 8, RL_AT_SMAX, 0, RL_ORD_PLAIN);
    CHECK_EQ(get(0, 8), 0);
    /* High operand bits beyond the size are ignored. */
    put(0, 4, 5);
    rl_atomic(buf, 0, 4, RL_AT_SMIN, 0xffffffff00000003ull, RL_ORD_PLAIN);
    CHECK_EQ(get(0, 4), 3);
    put(0, 8, 0);
    rl_atomic(buf, 0, 4, RL_AT_ADD, 0xffffffffffffffffull, RL_ORD_PLAIN);
    CHECK_EQ(get(0, 8), 0xffffffffu);
}

TEST(atomic_cas_success_and_failure)
{
    static const unsigned sizes[] = {1, 2, 4, 8};
    for (unsigned si = 0; si < 4; si++) {
        unsigned size = sizes[si];
        uint64_t mask = size == 8 ? ~0ull : (1ull << (8 * size)) - 1;
        for (int ord = RL_ORD_PLAIN; ord <= RL_ORD_SC; ord++) {
            put(16, 8, 0);
            put(16, size, 0x1122334455667788ull);
            CHECK_EQ(rl_cas(buf, 16, size, 0x1122334455667788ull, 0x99, ord), 0x1122334455667788ull & mask);
            CHECK_EQ(get(16, size), 0x99);
            CHECK_EQ(rl_cas(buf, 16, size, 0x98, 0x42, ord), 0x99);
            CHECK_EQ(get(16, size), 0x99);
        }
    }
    rl_u128 *p = (rl_u128 *)(void *)(buf + 32);
    rl_u128 x = ((rl_u128)0x0102030405060708ull << 64) | 0x1112131415161718ull;
    rl_u128 y = ((rl_u128)0xa0a1a2a3a4a5a6a7ull << 64) | 0xb0b1b2b3b4b5b6b7ull;
    *p = x;
    CHECK(rl_cas128(buf, 32, x, y, RL_ORD_SC) == x);
    CHECK(*p == y);
    CHECK(rl_cas128(buf, 32, x ^ 1, x, RL_ORD_PLAIN) == y);
    CHECK(*p == y);
    CHECK(rl_cas128(buf, 32, y ^ ((rl_u128)1 << 64), x, RL_ORD_PLAIN) == y);
    CHECK(*p == y);
}

TEST(atomic128_ops)
{
    rl_u128 *p = (rl_u128 *)(void *)(buf + 64);
    rl_u128 x = ((rl_u128)0xff00ff00ff00ff00ull << 64) | 0x0f0f0f0f0f0f0f0full;
    rl_u128 v = ((rl_u128)0x00000000ffffffffull << 64) | 0xffff0000ffff0000ull;
    *p = x;
    CHECK(rl_atomic128(buf, 64, RL_AT_CLR, v, RL_ORD_SC) == x);
    CHECK(*p == (x & ~v));
    *p = x;
    CHECK(rl_atomic128(buf, 64, RL_AT_SET, v, RL_ORD_PLAIN) == x);
    CHECK(*p == (x | v));
    *p = x;
    CHECK(rl_atomic128(buf, 64, RL_AT_SWP, v, RL_ORD_SC) == x);
    CHECK(*p == v);
    *p = ~(rl_u128)0;
    CHECK(rl_atomic128(buf, 64, RL_AT_ADD, 1, RL_ORD_PLAIN) == ~(rl_u128)0);
    CHECK(*p == 0);
    *p = x;
    rl_atomic128(buf, 64, RL_AT_EOR, v, RL_ORD_PLAIN);
    CHECK(*p == (x ^ v));
}

/* ---- Threads ------------------------------------------------------------ */

#define THREADS 4
#define ITERS 100000

/** @brief Counters, each on its own line. */
static _Alignas(64) uint8_t shared[8][64];

static void *worker(void *arg) {
    uintptr_t id = (uintptr_t)arg;
    uint8_t *m = &shared[0][0];
    for (unsigned i = 0; i < ITERS; i++) {
        rl_atomic(m, 0 * 64, 4, RL_AT_ADD, 1, RL_ORD_PLAIN);
        rl_atomic(m, 1 * 64, 8, RL_AT_ADD, 3, RL_ORD_SC);
        /* CAS loops on 4, 8 and 16 bytes. */
        uint64_t e4 = 0, e8 = 0;
        for (;;) {
            uint64_t got = rl_cas(m, 2 * 64, 4, e4, (e4 + 1) & 0xffffffffu, RL_ORD_SC);
            if (got == e4)
                break;
            e4 = got;
        }
        for (;;) {
            uint64_t got = rl_cas(m, 3 * 64, 8, e8, e8 + 5, i & 1 ? RL_ORD_SC : RL_ORD_PLAIN);
            if (got == e8)
                break;
            e8 = got;
        }
        rl_u128 e16 = 0;
        for (;;) {
            /* Both halves move together; a torn update would split them. */
            rl_u128 nv = ((rl_u128)((uint64_t)(e16 >> 64) + 1) << 64) | (uint64_t)((uint64_t)e16 + 1);
            rl_u128 got = rl_cas128(m, 4 * 64, e16, nv, RL_ORD_SC);
            if (got == e16)
                break;
            e16 = got;
        }
        rl_atomic128(m, 5 * 64, RL_AT_ADD, ((rl_u128)1 << 64) | 1, RL_ORD_PLAIN);
        rl_atomic(m, 6 * 64, 4, RL_AT_SMAX, (uint32_t)(int32_t)(i * THREADS + id) - 200000, RL_ORD_PLAIN);
        rl_atomic(m, 7 * 64, 2, id & 1 ? RL_AT_SET : RL_AT_CLR, 1u << id, RL_ORD_SC);
    }
    return NULL;
}

TEST(atomic_threads_are_atomic)
{
    memset(shared, 0, sizeof shared);
    uint8_t *m = &shared[0][0];
    rl_atomic(m, 6 * 64, 4, RL_AT_SWP, (uint32_t)INT32_MIN, RL_ORD_PLAIN);
    pthread_t t[THREADS];
    for (uintptr_t i = 0; i < THREADS; i++)
        CHECK(pthread_create(&t[i], NULL, worker, (void *)i) == 0);
    for (unsigned i = 0; i < THREADS; i++)
        pthread_join(t[i], NULL);
    uint64_t total = (uint64_t)THREADS * ITERS;
    uint32_t c4;
    uint64_t c8, a8, e8;
    memcpy(&c4, m + 0 * 64, 4);
    memcpy(&a8, m + 1 * 64, 8);
    memcpy(&c8, m + 2 * 64, 4);
    memcpy(&e8, m + 3 * 64, 8);
    CHECK_EQ(c4, total);
    CHECK_EQ(a8, 3 * total);
    CHECK_EQ(c8 & 0xffffffffu, total);
    CHECK_EQ(e8, 5 * total);
    rl_u128 c16, a16;
    memcpy(&c16, m + 4 * 64, 16);
    memcpy(&a16, m + 5 * 64, 16);
    CHECK_EQ((uint64_t)c16, total);
    CHECK_EQ((uint64_t)(c16 >> 64), total);
    CHECK_EQ((uint64_t)a16, total);
    CHECK_EQ((uint64_t)(a16 >> 64), total);
    int32_t mx;
    memcpy(&mx, m + 6 * 64, 4);
    CHECK_EQ((uint32_t)mx, (uint32_t)((ITERS - 1) * THREADS + THREADS - 1 - 200000));
    uint16_t bits;
    memcpy(&bits, m + 7 * 64, 2);
    CHECK_EQ(bits, 0x0a); /* odd ids set their bit, even ids cleared theirs */
}
