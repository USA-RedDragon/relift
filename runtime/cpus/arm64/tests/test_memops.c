/**
 * @file test_memops.c
 * @brief The memory routine overrides (docs/abi.md section 7.2): copy, overlap,
 * fill, the registers they leave alone, and the edges of the guest window.
 */
#include "arm64.h"
#include "runner.h"
#include <stdbool.h>

#include <string.h>


#define CHECK_RET(call)               \
    do {                              \
        uint64_t want_ = c->x[30];    \
        CHECK_EQ(call, want_);        \
    } while (0)

static const rl_guest_desc mem_guest = {
    .name = "arm64-memops-test",
    .page_size = 0x4000,
    .address_bits = 40,
    .tick_hz = 1000000,
};

static _Alignas(16) uint8_t g_mem[0x2000];

static rl_ctx *boot(void)
{
    if (rl_core_init(&mem_guest, rl_arm64_cpu()) != 0)
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
                CHECK_RET(rl_arm64_memmove(c, g_mem));
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
    CHECK_RET(rl_arm64_memmove(c, g_mem));
    CHECK(memcmp(g_mem + 0x201, orig + 0x200, 300) == 0);
    memcpy(g_mem, orig, sizeof orig);
    c->x[0] = 0x200;
    c->x[1] = 0x201;
    CHECK_RET(rl_arm64_memmove(c, g_mem));
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
        CHECK_RET(rl_arm64_memset(c, g_mem));
        CHECK_EQ(c->x[0], 0x400 + n % 5);
        for (uint64_t i = 0; i < n; i++)
            CHECK_EQ(g_mem[0x400 + n % 5 + i], 0xa1);
        CHECK_EQ(g_mem[0x400 + n % 5 + n], orig[0x400 + n % 5 + n]);
    }
}

static int vm_boot(rl_vm *vm, rl_ctx **c)
{
    if (rl_core_init(&mem_guest, rl_arm64_cpu()) != 0 || rl_vm_init(vm, 32, 0) != 0)
        return -1;
    rl_set_mem_base(vm->base);
    rl_fault_install(vm);
    struct rl_thread *t = rl_thread_alloc();
    if (!t)
        return -1;
    rl_thread_bind(t);
    *c = t->ctx;
    return 0;
}

static bool faults_at(rl_ctx *c, rl_fn fn, uint64_t *addr)
{
    rl_fault_catch fc;
    rl_fault_catch *prev = rl_fault_arm(&fc);
    bool faulted = false;
    if (RL_FAULT_SETJMP(fc.jb) == 0) {
        fn(c, rl_mem_base());
    } else {
        faulted = true;
        *addr = fc.fault.guest_addr;
    }
    rl_fault_disarm(prev);
    return faulted;
}

TEST(memmove_into_an_unmapped_page_faults_at_a_guest_address)
{
    rl_vm vm;
    rl_ctx *c;
    CHECK_EQ(vm_boot(&vm, &c), 0);
    uint64_t hp = vm.host_page;
    CHECK_EQ(rl_vm_set(&vm, 8 * hp, 2 * hp, RL_VM_RW, 0, RL_VM_FRESH), 0);
    c->x[0] = 8 * hp + 2 * hp - 32;
    c->x[1] = 8 * hp;
    c->x[2] = 64;
    uint64_t a = 0;
    CHECK(faults_at(c, rl_arm64_memmove, &a));
    CHECK(a >= 10 * hp && a < 11 * hp);
    c->x[0] = 8 * hp + 100;
    c->x[1] = 10 * hp + 8;
    c->x[2] = 16;
    CHECK(faults_at(c, rl_arm64_memmove, &a));
    CHECK(a >= 10 * hp && a < 11 * hp);
    c->x[0] = 10 * hp + 3;
    c->x[1] = 0x41;
    c->x[2] = 5;
    CHECK(faults_at(c, rl_arm64_memset, &a));
    CHECK(a >= 10 * hp && a < 11 * hp);
    c->x[0] = 8 * hp;
    c->x[1] = 0x41;
    c->x[2] = 2 * hp;
    CHECK(!faults_at(c, rl_arm64_memset, &a));
}

TEST(memmove_is_seen_by_the_exclusive_monitor_as_a_plain_store)
{
    rl_ctx *c = boot();
    CHECK(c != NULL);
    rl_excl m = { 0 };
    uint64_t v = rl_ldx(&m, g_mem, 0x800, 8, RL_ORD_PLAIN);
    uint64_t other = v ^ 0x0101010101010101ull;
    memcpy(g_mem + 0x900, &other, 8);
    c->x[0] = 0x800;
    c->x[1] = 0x900;
    c->x[2] = 8;
    CHECK_RET(rl_arm64_memmove(c, g_mem));
    CHECK_EQ(rl_stx(&m, g_mem, 0x800, 8, 5, RL_ORD_PLAIN), 1);
    v = rl_ldx(&m, g_mem, 0x800, 8, RL_ORD_PLAIN);
    memcpy(g_mem + 0x900, &v, 8);
    CHECK_RET(rl_arm64_memmove(c, g_mem));
    CHECK_EQ(rl_stx(&m, g_mem, 0x800, 8, 5, RL_ORD_PLAIN), 0);
    CHECK_EQ(rl_ld64(g_mem, 0x800), 5);
}
