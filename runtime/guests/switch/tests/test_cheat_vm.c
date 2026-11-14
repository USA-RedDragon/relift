/**
 * @file test_cheat_vm.c
 * @brief The cheat virtual machine on a flat host buffer: stores, conditions,
 * loops, register arithmetic, static registers, key conditions, lengths and
 * refused code writes.
 */
#include "cheat_vm.h"
#include "t.h"

#include <string.h>

#define MEM 0x1000u
#define CODE_AT 0x800u

typedef struct fenv {
    uint8_t mem[MEM];
    uint64_t keys;
} fenv;

static int f_read(void *u, uint64_t a, void *d, unsigned n)
{
    fenv *e = u;
    if (a + n > MEM)
        return CHEAT_MEM_UNMAPPED;
    memcpy(d, e->mem + a, n);
    return CHEAT_MEM_OK;
}

static int f_write(void *u, uint64_t a, const void *s, unsigned n)
{
    fenv *e = u;
    if (a + n > MEM)
        return CHEAT_MEM_UNMAPPED;
    if (a >= CODE_AT)
        return CHEAT_MEM_CODE;
    memcpy(e->mem + a, s, n);
    return CHEAT_MEM_OK;
}

static uint64_t f_base(void *u, unsigned r)
{
    (void)u;
    return r == 1 ? 0x100 : 0;
}

static uint64_t f_keys(void *u) { return ((fenv *)u)->keys; }

static cheat_result run(fenv *e, cheat_vm *vm, const uint32_t *ops, size_t n, uint8_t *edge)
{
    cheat_env env = { e, f_read, f_write, f_base, f_keys };
    cheat_program p = { ops, n, edge };
    cheat_result r;
    cheat_vm_run(vm, &env, &p, &r);
    return r;
}

static uint32_t m32(const fenv *e, unsigned a)
{
    uint32_t v;
    memcpy(&v, e->mem + a, 4);
    return v;
}

TEST(cheat_store_and_regions)
{
    fenv e;
    cheat_vm vm;
    const uint32_t heap[] = { 0x04100000, 0x00000004, 0xDEADBEEF };
    const uint32_t main_[] = { 0x04000000, 0x00000020, 0x11223344 };
    cheat_result r;
    memset(&e, 0, sizeof e);
    memset(&vm, 0, sizeof vm);
    r = run(&e, &vm, heap, 3, NULL);
    CHECK_EQ(r.writes, 1);
    CHECK_EQ(m32(&e, 0x100 + 4), 0xDEADBEEF);
    r = run(&e, &vm, main_, 3, NULL);
    CHECK_EQ(r.writes, 1);
    CHECK_EQ(m32(&e, 0x20), 0x11223344);
    CHECK_EQ(r.bad_opcode, 0);
}

TEST(cheat_eight_byte_store_and_load)
{
    fenv e;
    cheat_vm vm;
    const uint32_t ops[] = { 0x08000000, 0x00000040, 0x01234567, 0x89ABCDEF, 0x58000000, 0x00000040,
                             0x600000 << 8, 0x00000000, 0x00000000 };
    cheat_result r;
    uint64_t v;
    memset(&e, 0, sizeof e);
    memset(&vm, 0, sizeof vm);
    r = run(&e, &vm, ops, 6, NULL);
    memcpy(&v, e.mem + 0x40, 8);
    CHECK_EQ(v, 0x0123456789ABCDEFull);
    CHECK_EQ(r.writes, 1);
}

TEST(cheat_conditional_else_and_nesting)
{
    fenv e;
    cheat_vm vm;
    const uint32_t ops[] = {
        0x14000050, 0x00000000, 0x00000005, 0x04000000, 0x00000100, 0x00000001, 0x21000000,
        0x04000000, 0x00000104, 0x00000002, 0x20000000, 0x04000000, 0x00000108, 0x00000003,
    };
    cheat_result r;
    memset(&e, 0, sizeof e);
    memset(&vm, 0, sizeof vm);
    r = run(&e, &vm, ops, sizeof ops / 4, NULL);
    CHECK_EQ(r.writes, 2);
    CHECK_EQ(m32(&e, 0x100), 0);
    CHECK_EQ(m32(&e, 0x104), 2);
    CHECK_EQ(m32(&e, 0x108), 3);
    {
        uint32_t five = 5;
        memcpy(e.mem + 0, &five, 4);
    }
    memset(&e.mem[0x100], 0, 0x20);
    {
        const uint32_t eq0[] = { 0x14000050, 0x00000000, 0x00000005 };
        (void)eq0;
    }
}

TEST(cheat_loop_and_register_arithmetic)
{
    fenv e;
    cheat_vm vm;
    const uint32_t ops[] = {
        0x40010000, 0x00000000, 0x00000200, 0x30000000, 0x00000004, 0x64011000, 0x00000000, 0x00000007, 0x31000000,
    };
    const uint32_t math[] = {
        0x40000000, 0x00000000, 0x0000000A, 0x94020100, 0x00000005, 0xA4200400, 0x00000300,
        0x94230000 | 0x0100, 0x00000000,
    };
    cheat_result r;
    memset(&e, 0, sizeof e);
    memset(&vm, 0, sizeof vm);
    r = run(&e, &vm, ops, sizeof ops / 4, NULL);
    CHECK_EQ(r.bad_opcode, 0);
    CHECK_EQ(r.writes, 4);
    for (unsigned i = 0; i < 4; i++)
        CHECK_EQ(m32(&e, 0x200 + 4 * i), 7);
    CHECK_EQ(m32(&e, 0x210), 0);
    r = run(&e, &vm, math, 7, NULL);
    CHECK_EQ(r.bad_opcode, 0);
    CHECK_EQ(m32(&e, 0x300), 15);
}


TEST(cheat_static_register_write_persists)
{
    fenv e;
    cheat_vm vm;
    const uint32_t w[] = { 0x40010000, 0x00000000, 0x00000042, 0xC3000801 };
    const uint32_t rd[] = { 0xC3000000, 0xA4000400, 0x00000310 };
    cheat_result r;
    memset(&e, 0, sizeof e);
    memset(&vm, 0, sizeof vm);
    r = run(&e, &vm, w, 4, NULL);
    CHECK_EQ(r.bad_opcode, 0);
    CHECK_EQ(vm.statics[0], 0x42);
    r = run(&e, &vm, rd, 3, NULL);
    CHECK_EQ(r.bad_opcode, 0);
    CHECK_EQ(m32(&e, 0x310), 0x42);
}

TEST(cheat_key_conditions)
{
    fenv e;
    cheat_vm vm;
    uint8_t edge[8];
    const uint32_t ops[] = { 0x80000001, 0x04000000, 0x00000300, 0x00000009 };
    cheat_result r;
    memset(&e, 0, sizeof e);
    memset(&vm, 0, sizeof vm);
    r = run(&e, &vm, ops, 4, NULL);
    CHECK_EQ(r.writes, 0);
    e.keys = 1;
    r = run(&e, &vm, ops, 4, NULL);
    CHECK_EQ(r.writes, 1);
    CHECK_EQ(m32(&e, 0x300), 9);
    {
        const uint32_t x[] = { 0xC4000000, 0x00000000, 0x00000002, 0x04000000, 0x00000304, 0x00000001 };
        memset(edge, 0, sizeof edge);
        e.keys = 2;
        r = run(&e, &vm, x, 6, edge);
        CHECK_EQ(r.writes, 1);
        r = run(&e, &vm, x, 6, edge);
        CHECK_EQ(r.writes, 0);
        e.keys = 0;
        r = run(&e, &vm, x, 6, edge);
        e.keys = 2;
        r = run(&e, &vm, x, 6, edge);
        CHECK_EQ(r.writes, 1);
    }
}

TEST(cheat_refused_writes_and_faults)
{
    fenv e;
    cheat_vm vm;
    const uint32_t ops[] = { 0x04000000, 0x00000900, 0x11111111, 0x04000000, 0x00002000, 0x22222222,
                             0x54000000, 0x00002000 };
    cheat_result r;
    memset(&e, 0, sizeof e);
    memset(&vm, 0, sizeof vm);
    r = run(&e, &vm, ops, 8, NULL);
    CHECK_EQ(r.code_writes, 1);
    CHECK_EQ(r.code_addr, 0x900);
    CHECK_EQ(r.faults, 2);
    CHECK_EQ(r.writes, 0);
}

TEST(cheat_bad_opcodes_are_reported)
{
    fenv e;
    cheat_vm vm;
    const uint32_t bad_width[] = { 0x03000000, 0x00000000, 0x00000001 };
    const uint32_t truncated[] = { 0x04000000, 0x00000000 };
    cheat_result r;
    memset(&e, 0, sizeof e);
    memset(&vm, 0, sizeof vm);
    r = run(&e, &vm, bad_width, 3, NULL);
    CHECK(r.bad_opcode);
    CHECK_EQ(r.bad_at, 0);
    CHECK_EQ(cheat_vm_length(truncated, 2, 0), 0);
    CHECK_EQ(cheat_vm_length(bad_width, 3, 0), 0);
}
