/**
 * @file test_bus.c
 * @brief An external bus (rl_gb_set_bus) takes every access outside the I/O
 * registers; the registers still go through the scheduler.
 */
#include "gbt.h"

typedef struct bus_log {
    uint8_t mem[0x10000];
    unsigned reads, writes, peeks;
    uint16_t last_addr;
} bus_log;

static uint8_t log_read(void *ctx, uint16_t addr)
{
    bus_log *b = ctx;
    b->reads++;
    b->last_addr = addr;
    return b->mem[addr];
}

static void log_write(void *ctx, uint16_t addr, uint8_t value)
{
    bus_log *b = ctx;
    b->writes++;
    b->last_addr = addr;
    b->mem[addr] = value;
}

static uint8_t log_peek(void *ctx, uint16_t addr)
{
    bus_log *b = ctx;
    b->peeks++;
    return b->mem[addr];
}

TEST(a_bus_takes_memory_accesses_and_leaves_the_registers_to_the_scheduler)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0x80);
    rl_gb *g = gbt_machine(RL_GB_CGB, rom, 2);
    CHECK(g != NULL);
    rl_sched *s = rl_sched_create(NULL);
    rl_sched_cpu *k = rl_sched_add_cpu(s, 1);
    CHECK_EQ(rl_gb_attach(g, s, NULL), 0);
    bus_log *b = calloc(1, sizeof *b);
    b->mem[0x0100] = 0x11;
    b->mem[0xFF90] = 0x22;
    rl_gb_set_bus(g, &(rl_gb_bus){log_read, log_write, log_peek, b});
    CHECK_EQ(rl_gb_read(g, k, 0x0100), 0x11);
    CHECK_EQ(rl_gb_read(g, k, 0x9000), 0x00);
    CHECK_EQ(rl_gb_read(g, k, 0xFF90), 0x22);
    rl_gb_write(g, k, 0x2000, 0x07);
    rl_gb_write(g, k, 0xFF4F, 0x01);
    rl_gb_write(g, k, 0xFF70, 0x03);
    CHECK_EQ(b->reads, 3);
    CHECK_EQ(b->writes, 3);
    CHECK_EQ(b->mem[0x2000], 0x07);
    CHECK_EQ(b->mem[0xFF4F], 0x01);
    CHECK_EQ(rl_sched_get_stats(s)->accesses, 0);
    rl_gb_write(g, k, 0xFF06, 0xC3);
    CHECK_EQ(rl_gb_read(g, k, 0xFF06), 0xC3);
    CHECK_EQ(rl_sched_get_stats(s)->accesses, 2);
    CHECK_EQ(b->reads, 3);
    CHECK_EQ(rl_gb_peek(g, 0x0100), 0x11);
    CHECK_EQ(rl_gb_peek(g, 0xFF06), 0xFF);
    CHECK_EQ(b->peeks, 1);
    CHECK_EQ(b->reads, 3);
    free(b);
    rl_gb_destroy(g);
    rl_sched_destroy(s);
    free(rom);
}

TEST(removing_the_bus_restores_the_own_memory)
{
    uint8_t *rom = gbt_rom(2, 0x00, 0x00, 0x80);
    rl_gb *g = gbt_machine(RL_GB_CGB, rom, 2);
    CHECK(g != NULL);
    bus_log *b = calloc(1, sizeof *b);
    rl_gb_set_bus(g, &(rl_gb_bus){log_read, log_write, log_peek, b});
    rl_gb_write(g, NULL, 0xC000, 0x42);
    CHECK_EQ(b->mem[0xC000], 0x42);
    rl_gb_set_bus(g, NULL);
    CHECK_EQ(rl_gb_read(g, NULL, 0xC000), 0x00);
    rl_gb_write(g, NULL, 0xC000, 0x43);
    CHECK_EQ(rl_gb_read(g, NULL, 0xC000), 0x43);
    CHECK_EQ(gbt_bank_at(g, 0x4000), 1);
    CHECK_EQ(b->writes, 1);
    free(b);
    rl_gb_destroy(g);
    free(rom);
}
