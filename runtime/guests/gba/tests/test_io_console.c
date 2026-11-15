/**
 * @file test_io_console.c
 * @brief The I/O registers against the console: the dirty-state and
 * RegisterRamReset dumps of relift's hardware probe (part B), replayed
 * through the machine and compared halfword by halfword. The registers that
 * depend on the time since the console's boot are not compared: the DISPSTAT
 * flag bits (0x004 bits 0 to 2), VCOUNT (0x006) and IF (0x202).
 */
#include "mach.h"
#include "vectors_io.h"

/* The probe's dump loop reads with this instruction (mov r0, r0, lsr #8) as the last prefetch. */
#define OPEN_BUS 0xE1A00420u

static const uint16_t dirty[][2] = {
    {0x084, 0x0080}, {0x000, 0x0080}, {0x004, 0x5038}, {0x008, 0x4D8F}, {0x00A, 0x4D8F}, {0x00C, 0xDD8F}, {0x00E, 0xDD8F},
    {0x048, 0x3F3F}, {0x04A, 0x3F3F}, {0x04C, 0x5555}, {0x050, 0x3FFF}, {0x052, 0x1010}, {0x054, 0x0010}, {0x060, 0x007F},
    {0x062, 0xF7C0}, {0x064, 0x4700}, {0x068, 0xF7C0}, {0x06C, 0x4700}, {0x070, 0x0040}, {0x090, 0x1234}, {0x092, 0x5678},
    {0x094, 0x9ABC}, {0x096, 0xDEF0}, {0x098, 0x0F1E}, {0x09A, 0x2D3C}, {0x09C, 0x4B5A}, {0x09E, 0x6978}, {0x072, 0x2000},
    {0x074, 0x4000}, {0x078, 0xF700}, {0x07C, 0x4077}, {0x080, 0xFF77}, {0x082, 0x770E}, {0x088, 0xC200}, {0x134, 0x0000},
    {0x128, 0x4003}, {0x12A, 0xBEEF}, {0x132, 0x43FF}, {0x102, 0x0003}, {0x106, 0x0003}, {0x10A, 0x0003}, {0x10E, 0x0003},
    {0x0BA, 0x0260}, {0x0C6, 0x0260}, {0x0D2, 0x0260}, {0x0DE, 0x0260}, {0x204, 0x4317},
};

static void make_dirty(mach *m)
{
    for (unsigned i = 0; i < sizeof dirty / sizeof dirty[0]; i++)
        rl_gba_write(m->g, 0x04000000u + dirty[i][0], 2, dirty[i][1]);
}

static unsigned compare(mach *m, const gba_io_dump *d)
{
    unsigned bad = 0;
    for (uint32_t o = 0; o < 0x400; o += 2) {
        rl_gba_set_open_bus(m->g, OPEN_BUS);
        uint16_t want = d->io[o / 2], got = (uint16_t)rl_gba_read(m->g, 0x04000000u + o, 2);
        if (o == 0x004) {
            want &= (uint16_t)~7u;
            got &= (uint16_t)~7u;
        } else if (o == 0x006 || o == 0x202 || (o >= 0x060 && o < 0x0B0)) {
            continue;
        }
        if (got != want) {
            printf("flags %d: %03x is %04x, console %04x\n", d->flags, o, got, want);
            bad++;
        }
    }
    rl_gba_set_open_bus(m->g, OPEN_BUS);
    uint32_t word = rl_gba_read(m->g, 0x04000800u, 4);
    if (word != d->memcnt) {
        printf("flags %d: 800 is %08x, console %08x\n", d->flags, word, d->memcnt);
        bad++;
    }
    return bad;
}

TEST(io_dumps_equal_the_console)
{
    mach m = mach_new();
    CHECK(m.g != NULL);
    const rl_gba_bus *bus = rl_gba_bios_bus(m.g);
    rl_gba_bios_boot_io(rl_gba_bios_of(m.g), bus);
    rl_gba_write(m.g, 0x04000000u, 2, 0x80);
    rl_gba_write(m.g, 0x04000208u, 2, 0);
    rl_gba_write(m.g, 0x04000200u, 2, 0);
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof gba_io_console / sizeof gba_io_console[0]; i++) {
        const gba_io_dump *d = &gba_io_console[i];
        make_dirty(&m);
        if (d->flags >= 0) {
            rl_gba_swi_cpu cpu = {.r = {(uint32_t)d->flags}, .ret = 0x08000100u, .cpsr = 0x1F, .sp_svc = 0x03007FE0u, .sp_sys = 0x03007F00u};
            CHECK_EQ(rl_gba_swi(rl_gba_bios_of(m.g), bus, 1, &cpu), RL_GBA_SWI_OK);
        }
        bad += compare(&m, d);
    }
    CHECK_EQ(bad, 0u);
    mach_free(&m);
}
