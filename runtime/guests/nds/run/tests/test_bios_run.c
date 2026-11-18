/**
 * @file test_bios_run.c
 * @brief The SWI trap on both CPUs with hand-written lifted functions: an
 * IntrWait that takes a timer interrupt inside the call (the game's handler
 * runs between the SWI's exception and its return and sets the BIOS flag), a
 * decompression with a callback that is lifted code, and SoftReset.
 */
#include "cpu.h"
#include "runner.h"

#include <string.h>

static const rl_guest_desc test_guest = {
    .name = "nds-bios-run-test",
    .page_size = 0x1000,
    .address_bits = 32,
    .tick_hz = RL_NDS_TICK_HZ,
};

#define ENTRY9 0x02000000u
#define ENTRY7 0x02380000u
#define HANDLER7 0x02380100u
#define OPEN7 0x02380200u
#define GET8_7 0x02380300u
#define CODE_LEN 8u
#define TM0 0x04000100u
#define IF7 0x04000214u
#define IE7 0x04000210u
#define IME7 0x04000208u
#define FLAGS7 0x0380FFF8u
#define SLOT7 0x0380FFFCu
#define SRC 0x02100000u
#define DST 0x02100100u
#define CBS 0x02100200u
#define OUT 0x02100300u

static struct {
    unsigned handler_calls, open_calls, get8_calls;
    uint32_t handler_r0, handler_mode, mode_after, r0_after, r1_after;
    uint32_t flags_after, spsr_irq_i, lz_result;
} T;

static uint8_t g_rom[0x400];

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void make_rom(void)
{
    memset(g_rom, 0, sizeof g_rom);
    put32(g_rom + 0x20, 0x200);
    put32(g_rom + 0x24, ENTRY9);
    put32(g_rom + 0x28, ENTRY9);
    put32(g_rom + 0x2C, CODE_LEN);
    put32(g_rom + 0x30, 0x300);
    put32(g_rom + 0x34, ENTRY7);
    put32(g_rom + 0x38, ENTRY7);
    put32(g_rom + 0x3C, CODE_LEN);
    for (unsigned i = 0; i < CODE_LEN; i++) {
        g_rom[0x200 + i] = (uint8_t)(0x90 + i);
        g_rom[0x300 + i] = (uint8_t)(0x70 + i);
    }
}

/** @brief The ARM9: idles into a halt nothing ends. */
static uint64_t arm9_main(rl_ctx *c, uint8_t *mem)
{
    if (rl_nds_cpu_enter(c, 0, 0))
        return rl_nds_cpu_variant(c, ENTRY9, 0)(c, mem);
    rl_nds_cpu_cp_write(c, RL_CP15_KEY(0, 7, 0, 4), 0, 1);
    return RL_OK;
}

/** @brief The game's IRQ handler: stops the timer, acknowledges IF and ORs the bits into the BIOS flags, then returns through LR. */
static uint64_t handler7(rl_ctx *c, uint8_t *mem)
{
    T.handler_calls++;
    T.handler_r0 = c->r[0];
    T.handler_mode = c->m;
    rl_nds_cpu_write(c, mem, TM0 + 2, 2, 0);
    uint32_t iflag = (uint32_t)rl_nds_cpu_read(c, mem, IF7, 4);
    rl_nds_cpu_write(c, mem, IF7, 4, iflag);
    uint32_t flags = (uint32_t)rl_nds_cpu_read(c, mem, FLAGS7, 4);
    rl_nds_cpu_write(c, mem, FLAGS7, 4, flags | iflag);
    return rl_indirect(c, c->r[14])(c, mem);
}

/** @brief The Open callback of the decompression: returns the header word at r0. */
static uint64_t open7(rl_ctx *c, uint8_t *mem)
{
    T.open_calls++;
    c->r[0] = (uint32_t)rl_nds_cpu_read(c, mem, c->r[0], 4);
    return rl_indirect(c, c->r[14])(c, mem);
}

/** @brief The Get_8bit callback: returns the byte at r0. */
static uint64_t get8_7(rl_ctx *c, uint8_t *mem)
{
    T.get8_calls++;
    c->r[0] = (uint32_t)rl_nds_cpu_read(c, mem, c->r[0], 1);
    return rl_indirect(c, c->r[14])(c, mem);
}

/** @brief The lifted sequence of an SWI: the exception into supervisor mode, then the trap. */
static int swi(rl_ctx *c, uint8_t *mem, unsigned number, uint32_t next)
{
    rl_arm32_exception(c, RL_ARM32_SVC, next);
    return rl_nds_swi_trap(c, mem, number);
}

static uint64_t arm7_main(rl_ctx *c, uint8_t *mem)
{
    if (rl_nds_cpu_enter(c, 1, 0))
        return rl_nds_cpu_variant(c, ENTRY7, 0)(c, mem);
    rl_nds_cpu_write(c, mem, SLOT7, 4, HANDLER7);
    rl_nds_cpu_write(c, mem, IE7, 4, RL_NDS_IRQ_TIMER0);
    rl_nds_cpu_write(c, mem, IME7, 4, 0);
    rl_nds_cpu_write(c, mem, TM0, 4, 0x00C0FF00u);
    c->r[0] = 1;
    c->r[1] = RL_NDS_IRQ_TIMER0;
    int rc = swi(c, mem, 0x04, ENTRY7 + 4);
    if (rc != RL_OK)
        return rc;
    T.mode_after = c->m;
    T.r0_after = c->r[0];
    T.r1_after = c->r[1];
    T.flags_after = (uint32_t)rl_nds_cpu_read(c, mem, FLAGS7, 4);

    c->r[0] = SRC;
    c->r[1] = DST;
    c->r[2] = 0x1234;
    c->r[3] = CBS;
    rc = swi(c, mem, 0x12, ENTRY7 + 8);
    if (rc != RL_OK)
        return rc;
    T.lz_result = c->r[0];
    rl_nds_cpu_write(c, mem, OUT, 4, 1);
    rl_nds_cpu_write(c, mem, 0x04000301u, 1, 0x80);
    return RL_OK;
}

static uint8_t g_blob9[CODE_LEN], g_blob7[CODE_LEN];
static rl_unit g_units[2];
static rl_nds_fn g_fns9[1], g_fns7[4];
static rl_nds_unit g_nds_units[2];
static rl_nds_program g_prog;

static void make_program(void)
{
    for (unsigned i = 0; i < CODE_LEN; i++) {
        g_blob9[i] = (uint8_t)(0x90 + i);
        g_blob7[i] = (uint8_t)(0x70 + i);
    }
    for (unsigned u = 0; u < 2; u++) {
        memset(&g_units[u], 0, sizeof g_units[u]);
        g_units[u].abi_version = RL_ABI_VERSION;
        g_units[u].ctx_size = sizeof(rl_ctx);
    }
    g_fns9[0] = (rl_nds_fn){ENTRY9, 0, CODE_LEN, arm9_main};
    g_fns7[0] = (rl_nds_fn){ENTRY7, 0, CODE_LEN, arm7_main};
    g_fns7[1] = (rl_nds_fn){HANDLER7, 0, 4, handler7};
    g_fns7[2] = (rl_nds_fn){OPEN7, 0, 4, open7};
    g_fns7[3] = (rl_nds_fn){GET8_7, 0, 4, get8_7};
    g_nds_units[0] = (rl_nds_unit){RL_NDS_ARM9, &g_units[0], g_blob9, g_fns9, 1, "test", NULL, NULL, 0};
    g_nds_units[1] = (rl_nds_unit){RL_NDS_ARM7, &g_units[1], g_blob7, g_fns7, 4, "test", NULL, NULL, 0};
    g_prog = (rl_nds_program){g_rom, sizeof g_rom, g_nds_units, 2, "test", NULL, 0, NULL};
}

TEST(intr_wait_takes_the_interrupt_inside_the_call_and_the_lz_callbacks_run_lifted_code)
{
    static rl_nds_run r;
    memset(&T, 0, sizeof T);
    CHECK_EQ(rl_core_init(&test_guest, rl_arm32_cpu()), 0);
    make_rom();
    make_program();
    CHECK_EQ(rl_nds_run_init(&r, &g_prog), 0);
    static const uint8_t stream[] = {0x10, 0x06, 0x00, 0x00, 0x00, 'h', 'e', 'l', 'l', 'o', '!', 0, 0};
    for (unsigned i = 0; i < sizeof stream; i++)
        rl_nds_write(r.m, RL_NDS_ARM7, SRC + i, 1, stream[i]);
    rl_nds_write(r.m, RL_NDS_ARM7, CBS + 0, 4, OPEN7);
    rl_nds_write(r.m, RL_NDS_ARM7, CBS + 4, 4, 0);
    rl_nds_write(r.m, RL_NDS_ARM7, CBS + 8, 4, GET8_7);
    rl_nds_write(r.m, RL_NDS_ARM7, CBS + 12, 4, 0);
    rl_nds_write(r.m, RL_NDS_ARM7, CBS + 16, 4, 0);
    CHECK_EQ(rl_nds_run_start(&r), 0);
    CHECK_EQ(T.handler_calls, 1);
    CHECK_EQ(T.handler_r0, 0x04000000u);
    CHECK_EQ(T.handler_mode, RL_ARM32_IRQ);
    CHECK_EQ(T.mode_after, RL_ARM32_SYS);
    CHECK_EQ(T.flags_after, 0);
    CHECK_EQ(rl_nds_read(r.m, RL_NDS_ARM7, IME7, 4), 1);
    CHECK_EQ(r.cpu[RL_NDS_ARM7].stats.irqs, 1);
    CHECK_EQ(T.open_calls, 1);
    CHECK_EQ(T.get8_calls, 7);
    CHECK_EQ(T.lz_result, 6);
    CHECK_EQ(rl_nds_read(r.m, RL_NDS_ARM7, DST, 4), 0x6C6C6568u);
    CHECK_EQ(rl_nds_read(r.m, RL_NDS_ARM7, DST + 4, 2), 0x216Fu);
    CHECK_EQ(rl_nds_read(r.m, RL_NDS_ARM7, OUT, 4), 1);
    rl_nds_run_free(&r);
}
