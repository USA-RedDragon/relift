/**
 * @file test_io.c
 * @brief I/O routing: each register range reaches its slot's device through
 * the scheduler as an access of the calling CPU, after the device was caught
 * up to that CPU's tick; the bank registers and the rest of the map do not.
 */
#include "gbt.h"

typedef struct probe {
    rl_device dev;
    unsigned slot;
    uint64_t catch_to;
    unsigned catch_ups;
    uint64_t last_addr;
    uint64_t last_value;
    uint64_t last_now;
    unsigned writes;
} probe;

static void probe_catch_up(rl_device *d, uint64_t from, uint64_t to)
{
    (void)from;
    probe *p = d->opaque;
    p->catch_to = to;
    p->catch_ups++;
}

static uint64_t probe_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now)
{
    (void)size;
    probe *p = d->opaque;
    p->last_addr = addr;
    p->last_now = now;
    return 0x10 + p->slot;
}

static void probe_write(rl_device *d, uint64_t addr, uint32_t size, uint64_t value, uint64_t now)
{
    (void)size;
    probe *p = d->opaque;
    p->last_addr = addr;
    p->last_value = value;
    p->last_now = now;
    p->writes++;
}

typedef struct rig {
    rl_sched *s;
    rl_sched_cpu *k;
    rl_gb *g;
    uint8_t *rom;
    probe p[RL_GB_SLOTS];
    rl_device *devs[RL_GB_SLOTS];
} rig;

static void rig_init(rig *r, uint8_t cgb_flag, bool all_probes)
{
    memset(r, 0, sizeof *r);
    r->rom = gbt_rom(2, 0x00, 0x00, cgb_flag);
    r->g = gbt_machine(RL_GB_CGB, r->rom, 2);
    r->s = rl_sched_create(NULL);
    r->k = rl_sched_add_cpu(r->s, 1);
    for (unsigned i = 0; i < RL_GB_SLOTS; i++) {
        probe *p = &r->p[i];
        p->slot = i;
        p->dev.name = "probe";
        p->dev.line = RL_LINE_NONE;
        p->dev.opaque = p;
        p->dev.catch_up = probe_catch_up;
        p->dev.read = probe_read;
        p->dev.write = probe_write;
        r->devs[i] = all_probes ? &p->dev : NULL;
    }
}

static void rig_free(rig *r)
{
    rl_gb_destroy(r->g);
    rl_sched_destroy(r->s);
    free(r->rom);
}

TEST(each_register_range_reaches_its_slots_device)
{
    static const struct {
        uint16_t addr;
        rl_gb_slot slot;
    } cases[] = {
        {0xFF00, RL_GB_SLOT_JOYPAD}, {0xFF01, RL_GB_SLOT_SERIAL}, {0xFF02, RL_GB_SLOT_SERIAL}, {0xFF04, RL_GB_SLOT_TIMER},
        {0xFF07, RL_GB_SLOT_TIMER},  {0xFF0F, RL_GB_SLOT_SYSTEM}, {0xFF10, RL_GB_SLOT_APU},    {0xFF26, RL_GB_SLOT_APU},
        {0xFF30, RL_GB_SLOT_APU},    {0xFF3F, RL_GB_SLOT_APU},    {0xFF40, RL_GB_SLOT_PPU},    {0xFF45, RL_GB_SLOT_PPU},
        {0xFF46, RL_GB_SLOT_DMA},    {0xFF47, RL_GB_SLOT_PPU},    {0xFF4B, RL_GB_SLOT_PPU},    {0xFF4D, RL_GB_SLOT_SYSTEM},
        {0xFF50, RL_GB_SLOT_SYSTEM}, {0xFF51, RL_GB_SLOT_DMA},    {0xFF55, RL_GB_SLOT_DMA},    {0xFF56, RL_GB_SLOT_SYSTEM},
        {0xFF68, RL_GB_SLOT_PPU},    {0xFF6C, RL_GB_SLOT_PPU},    {0xFF72, RL_GB_SLOT_SYSTEM}, {0xFF75, RL_GB_SLOT_SYSTEM},
        {0xFF76, RL_GB_SLOT_APU},    {0xFF77, RL_GB_SLOT_APU},    {0xFFFF, RL_GB_SLOT_SYSTEM},
    };
    rig r;
    rig_init(&r, 0x80, true);
    CHECK_EQ(rl_gb_attach(r.g, r.s, r.devs), 0);
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        CHECK_EQ(rl_gb_read(r.g, r.k, cases[i].addr), 0x10 + cases[i].slot);
        CHECK_EQ(r.p[cases[i].slot].last_addr, cases[i].addr);
        rl_gb_write(r.g, r.k, cases[i].addr, 0x5A);
        CHECK_EQ(r.p[cases[i].slot].last_addr, cases[i].addr);
        CHECK_EQ(r.p[cases[i].slot].last_value, 0x5A);
        CHECK(rl_gb_device(r.g, cases[i].slot) == &r.p[cases[i].slot].dev);
    }
    rig_free(&r);
}

TEST(registers_without_an_owner_read_ff_and_drop_writes)
{
    rig r;
    rig_init(&r, 0x80, true);
    CHECK_EQ(rl_gb_attach(r.g, r.s, r.devs), 0);
    static const uint16_t none[] = {0xFF03, 0xFF08, 0xFF27, 0xFF2F, 0xFF4E, 0xFF57, 0xFF67, 0xFF6D, 0xFF71, 0xFF78, 0xFF7F};
    for (size_t i = 0; i < sizeof none / sizeof none[0]; i++) {
        CHECK_EQ(rl_gb_read(r.g, r.k, none[i]), 0xFF);
        rl_gb_write(r.g, r.k, none[i], 0x00);
    }
    unsigned writes = 0;
    for (unsigned s = 0; s < RL_GB_SLOTS; s++)
        writes += r.p[s].writes;
    CHECK_EQ(writes, 0);
    CHECK_EQ(rl_sched_get_stats(r.s)->unmapped, 2 * (sizeof none / sizeof none[0]));
    rig_free(&r);
}

TEST(bank_registers_stay_out_of_the_scheduler)
{
    rig r;
    rig_init(&r, 0x80, true);
    CHECK_EQ(rl_gb_attach(r.g, r.s, r.devs), 0);
    uint64_t before = rl_sched_get_stats(r.s)->accesses;
    rl_gb_write(r.g, r.k, 0xFF4F, 1);
    rl_gb_write(r.g, r.k, 0xFF70, 3);
    CHECK_EQ(rl_gb_read(r.g, r.k, 0xFF4F), 0xFF);
    CHECK_EQ(rl_gb_read(r.g, r.k, 0xFF70), 0xFB);
    CHECK_EQ(rl_sched_get_stats(r.s)->accesses, before);
    CHECK_EQ(rl_gb_read(r.g, r.k, 0xFF4D), 0x16);
    CHECK_EQ(rl_sched_get_stats(r.s)->accesses, before + 1);
    rig_free(&r);
}

TEST(plain_memory_does_not_touch_the_scheduler)
{
    rig r;
    rig_init(&r, 0x00, true);
    CHECK_EQ(rl_gb_attach(r.g, r.s, r.devs), 0);
    rl_gb_write(r.g, r.k, 0xC000, 1);
    rl_gb_write(r.g, r.k, 0xFE00, 2);
    rl_gb_write(r.g, r.k, 0xFF80, 3);
    rl_gb_write(r.g, r.k, 0x8000, 4);
    rl_gb_read(r.g, r.k, 0x0100);
    rl_gb_read(r.g, r.k, 0xFFFE);
    CHECK_EQ(rl_sched_get_stats(r.s)->accesses, 0);
    CHECK_EQ(rl_sched_get_stats(r.s)->catch_ups, 0);
    rig_free(&r);
}

TEST(an_access_catches_the_owner_up_to_the_cpus_tick)
{
    rig r;
    rig_init(&r, 0x80, true);
    CHECK_EQ(rl_gb_attach(r.g, r.s, r.devs), 0);
    rl_sched_stall(r.k, 100);
    rl_gb_read(r.g, r.k, 0xFF05);
    probe *t = &r.p[RL_GB_SLOT_TIMER];
    CHECK_EQ(t->catch_to, 100);
    CHECK_EQ(t->last_now, 100);
    CHECK_EQ(t->catch_ups, 1);
    CHECK_EQ(r.p[RL_GB_SLOT_PPU].catch_ups, 0);
    rl_sched_stall(r.k, 28);
    rl_gb_write(r.g, r.k, 0xFF41, 0x08);
    CHECK_EQ(r.p[RL_GB_SLOT_PPU].catch_to, 128);
    CHECK_EQ(r.p[RL_GB_SLOT_PPU].last_now, 128);
    CHECK_EQ(t->catch_ups, 1);
    rig_free(&r);
}

TEST(a_stub_keeps_what_is_written)
{
    rig r;
    rig_init(&r, 0x80, false);
    CHECK_EQ(rl_gb_attach(r.g, r.s, NULL), 0);
    rl_gb_write(r.g, r.k, 0xFF06, 0xC3);
    rl_gb_write(r.g, r.k, 0xFFFF, 0x1F);
    CHECK_EQ(rl_gb_read(r.g, r.k, 0xFF06), 0xC3);
    CHECK_EQ(rl_gb_read(r.g, r.k, 0xFFFF), 0x1F);
    CHECK_EQ(rl_gb_read(r.g, r.k, 0xFF05), 0x00);
    rig_free(&r);
}

TEST(reads_without_a_cpu_see_ff_and_a_machine_attaches_once)
{
    rig r;
    rig_init(&r, 0x80, false);
    CHECK_EQ(rl_gb_read(r.g, NULL, 0xFF40), 0xFF);
    CHECK(rl_gb_device(r.g, RL_GB_SLOT_PPU) == NULL);
    CHECK_EQ(rl_gb_attach(r.g, r.s, NULL), 0);
    CHECK_EQ(rl_gb_read(r.g, NULL, 0xFF40), 0xFF);
    CHECK_EQ(rl_gb_attach(r.g, r.s, NULL), -1);
    rig_free(&r);
}

TEST(a_mixed_set_uses_stubs_for_the_missing_slots)
{
    rig r;
    rig_init(&r, 0x80, true);
    r.devs[RL_GB_SLOT_TIMER] = NULL;
    r.devs[RL_GB_SLOT_PPU] = NULL;
    CHECK_EQ(rl_gb_attach(r.g, r.s, r.devs), 0);
    CHECK_EQ(rl_gb_read(r.g, r.k, 0xFF00), 0x10 + RL_GB_SLOT_JOYPAD);
    rl_gb_write(r.g, r.k, 0xFF07, 0x05);
    CHECK_EQ(rl_gb_read(r.g, r.k, 0xFF07), 0x05);
    CHECK(rl_gb_device(r.g, RL_GB_SLOT_TIMER) != &r.p[RL_GB_SLOT_TIMER].dev);
    rig_free(&r);
}
