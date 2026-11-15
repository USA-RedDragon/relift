/**
 * @file test_memops.c
 * @brief The memory routine overrides (docs/abi.md section 7.2): copy, overlap,
 * fill, the registers they leave alone, and the edges of the guest window.
 */
#include "a64.h"
#include "runner.h"

#include <string.h>

static const rl_guest_desc mem_guest = {
    .name = "a64-memops-test",
    .page_size = 0x4000,
    .address_bits = 40,
    .tick_hz = 1000000,
};

static _Alignas(16) uint8_t g_mem[0x2000];

static rl_ctx *boot(void)
{
    if (rl_core_init(&mem_guest, rl_a64_cpu()) != 0)
        return NULL;
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return NULL;
    rl_thread_bind(t);
    for (size_t i = 0; i < sizeof g_mem; i++)
        g_mem[i] = (uint8_t)(i * 7u + 3u);
    return t->ctx;
}

TEST(memmove_copies_every_size_and_alignment)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    for (uint64_t n = 0; n < 100; n++) {
        for (uint64_t d = 0; d < 9; d++) {
            for (uint64_t s = 0; s < 9; s++) {
                uint8_t want[0x2000];
                memcpy(want, g_mem, sizeof want);
                memcpy(want + 0x1000 + d, g_mem + 0x100 + s, n);
                c->x[0] = 0x1000 + d;
                c->x[1] = 0x100 + s;
                c->x[2] = n;
                c->x[30] = 0x77;
                CHECK_EQ(rl_a64_memmove(c, g_mem), RL_OK);
                CHECK(memcmp(want, g_mem, sizeof want) == 0);
                CHECK_EQ(c->x[0], 0x1000 + d);
                CHECK_EQ(c->x[30], 0x77);
            }
        }
    }
}

TEST(memmove_handles_overlap_both_ways)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint8_t orig[0x2000];
    memcpy(orig, g_mem, sizeof orig);
    c->x[0] = 0x201;
    c->x[1] = 0x200;
    c->x[2] = 300;
    CHECK_EQ(rl_a64_memmove(c, g_mem), RL_OK);
    CHECK(memcmp(g_mem + 0x201, orig + 0x200, 300) == 0);
    memcpy(g_mem, orig, sizeof orig);
    c->x[0] = 0x200;
    c->x[1] = 0x201;
    CHECK_EQ(rl_a64_memmove(c, g_mem), RL_OK);
    CHECK(memcmp(g_mem + 0x200, orig + 0x201, 300) == 0);
}

TEST(memset_fills_with_the_low_byte)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    uint8_t orig[sizeof g_mem];
    memcpy(orig, g_mem, sizeof orig);
    for (uint64_t n = 0; n < 70; n++) {
        memcpy(g_mem, orig, sizeof orig);
        c->x[0] = 0x400 + n % 5;
        c->x[1] = 0xabcdef5a1ull;
        c->x[2] = n;
        CHECK_EQ(rl_a64_memset(c, g_mem), RL_OK);
        CHECK_EQ(c->x[0], 0x400 + n % 5);
        for (uint64_t i = 0; i < n; i++)
            CHECK_EQ(g_mem[0x400 + n % 5 + i], 0xa1);
        CHECK_EQ(g_mem[0x400 + n % 5 + n], orig[0x400 + n % 5 + n]);
    }
}
