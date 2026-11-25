/**
 * @file test_run.c
 * @brief The ARMv4T CPU on the Game Boy Advance machine with hand-written
 * lifted functions: the post-boot registers, data accesses through the
 * bus with their wait states, SWI through the high-level BIOS (a
 * VBlankIntrWait whose interrupt runs inside the BIOS call), IRQ entry at
 * a sync point through the BIOS stub and back, the order of the code
 * fetches, and code in RAM that checks the unit the CPU fetched (the
 * pipeline's copy for the next two, memory otherwise).
 */
#include "cpu.h"
#include "runner.h"

static const rl_guest_desc test_guest = {
    .name = "gba-run-test",
    .page_size = 0x1000,
    .address_bits = 32,
    .tick_hz = RL_GBA_HZ,
};

static struct {
    int handler_runs;
    bool handler_div;
    uint32_t div_r0;
    uint32_t handler_mode;
    uint32_t handler_lr;
    uint32_t handler_r0;
    int swis;
    int spins;
    uint32_t after_cpsr;
    uint32_t after_r4;
    uint32_t after_r1;
    uint32_t after_sp;
    uint64_t cycles_after;
} hs;

static uint64_t handler(rl_ctx *c, uint8_t *mem)
{
    hs.handler_runs++;
    hs.handler_mode = c->m;
    hs.handler_lr = c->r[14];
    hs.handler_r0 = c->r[0];
    uint32_t f = (uint32_t)rl_gba_cpu_read(c, mem, 0x04000202u, 2);
    rl_gba_cpu_write(c, mem, 0x04000202u, 2, f);
    uint32_t b = (uint32_t)rl_gba_cpu_read(c, mem, 0x03007FF8u, 2);
    rl_gba_cpu_write(c, mem, 0x03007FF8u, 2, b | f);
    if (hs.handler_div) {
        uint32_t lr = c->r[14];
        c->r[0] = 100;
        c->r[1] = 7;
        rl_arm32_exception(c, RL_ARM32_SVC, 0x08000104u);
        if (rl_gba_swi_trap(c, mem, 0x06))
            return RL_PENDING;
        hs.div_r0 = c->r[0];
        c->r[14] = lr;
    }
    c->r[1] = 0xBADu;
    c->time.budget -= 20;
    RL_MUSTTAIL return rl_indirect(c, c->r[14])(c, mem);
}

static void setup_irq(rl_ctx *c, uint8_t *mem)
{
    rl_gba_cpu_write(c, mem, 0x03FFFFFCu, 4, 0x08000100u);
    rl_gba_cpu_write(c, mem, 0x04000004u, 2, 0x0008u);
    rl_gba_cpu_write(c, mem, 0x04000200u, 2, 0x0001u);
    rl_gba_cpu_write(c, mem, 0x04000208u, 2, 0x0001u);
    rl_arm32_set_i(c, 0);
}

static void done(rl_ctx *c)
{
    hs.after_cpsr = rl_arm32_cpsr(c);
    hs.after_r4 = c->r[4];
    hs.after_r1 = c->r[1];
    hs.after_sp = c->r[13];
    hs.cycles_after = rl_sched_cycles(rl_gba_run_current()->k);
}

/** @brief Three VBlankIntrWait calls in ARM state, then stops. */
static uint64_t swi_main(rl_ctx *c, uint8_t *mem)
{
    setup_irq(c, mem);
    c->r[4] = 0x44u;
    c->r[1] = 0;
    for (int i = 0; i < 3; i++) {
        uint32_t next = 0x08000008u + 4u * (uint32_t)i;
        rl_arm32_exception(c, RL_ARM32_SVC, next);
        c->time.budget -= 3;
        int rc = rl_gba_swi_trap(c, mem, 0x05);
        if (rc)
            return RL_PENDING;
        hs.swis++;
    }
    done(c);
    return rl_pend_exit_process(c, 0);
}

/** @brief Spins with a sync point per instruction until two interrupts ran, then stops. */
static uint64_t spin_main(rl_ctx *c, uint8_t *mem)
{
    setup_irq(c, mem);
    c->r[4] = 0x44u;
    c->r[1] = 0;
    while (hs.handler_runs < 2) {
        hs.spins++;
        rl_gba_cpu_fetch(c, 0x08000200u, 0);
        if (c->time.budget <= 0) {
            int rc = rl_sync(c, mem, 0x08000204u);
            if (rc)
                return RL_PENDING;
        }
    }
    done(c);
    return rl_pend_exit_process(c, 0);
}

static const rl_fn_entry fns[] = {
    {0x08000100u, 4, 0, handler},
};

static rl_fn g_main;

static uint64_t entry(rl_ctx *c, uint8_t *mem) { return g_main(c, mem); }

static const rl_fn_entry entries[] = {
    {0x08000000u, 4, 0, entry},
};

static rl_gba_run run;
static uint8_t rom[0x400];

static int boot(rl_fn main_fn)
{
    if (rl_core_init(&test_guest, rl_arm32_cpu()) != 0)
        return -1;
    rl_units_reset();
    static rl_unit u[2];
    memset(u, 0, sizeof u);
    for (int i = 0; i < 2; i++) {
        u[i].abi_version = RL_ABI_VERSION;
        u[i].ctx_size = sizeof(rl_ctx);
        u[i].name = "gba-run-test";
    }
    u[0].fn_count = 1;
    u[0].fns = fns;
    u[1].fn_count = 1;
    u[1].fns = entries;
    const rl_unit *list[] = {&u[0], &u[1]};
    if (rl_units_register(list, 2) != 0)
        return -1;
    memset(&hs, 0, sizeof hs);
    g_main = main_fn;
    rl_gba_cart cart = {.rom = rom, .rom_len = sizeof rom};
    rl_gba *g = rl_gba_create(&cart);
    if (!g)
        return -1;
    return rl_gba_run_init(&run, g);
}

TEST(gba_run_boots_in_system_mode_at_the_cartridge)
{
    CHECK(boot(swi_main) == 0);
    rl_ctx *c = run.c;
    CHECK_EQ(run.entry, 0x08000000u);
    CHECK_EQ(c->m, RL_ARM32_SYS);
    CHECK_EQ(c->r[13], 0x03007F00u);
    CHECK_EQ(*rl_arm32_banked(c, RL_ARM32_IRQ, 13), 0x03007FA0u);
    CHECK_EQ(*rl_arm32_banked(c, RL_ARM32_SVC, 13), 0x03007FE0u);
}

TEST(gba_run_data_accesses_charge_the_bus)
{
    CHECK(boot(swi_main) == 0);
    rl_ctx *c = run.c;
    rl_sched_run(run.k);
    uint64_t t0 = rl_sched_cycles(run.k);
    rl_gba_cpu_write(c, NULL, 0x02000000u, 4, 0x12345678u);
    CHECK_EQ(rl_sched_cycles(run.k) - t0, 6);
    rl_gba_cpu_fetch(c, 0x03000000u, 0);
    t0 = rl_sched_cycles(run.k);
    CHECK_EQ(rl_gba_cpu_read(c, NULL, 0x02000000u, 4), 0x12345678u);
    CHECK_EQ(rl_gba_cpu_read(c, NULL, 0x02000004u, 4), 0);
    CHECK_EQ(rl_sched_cycles(run.k) - t0, 12);
    CHECK_EQ(rl_gba_cpu_read(c, NULL, 0x03000002u, 2), 0);
}

TEST(gba_run_fetches_follow_the_arm7_bus_order)
{
    CHECK(boot(swi_main) == 0);
    rl_ctx *c = run.c;
    rl_sched_run(run.k);
    rl_gba_cpu_write(c, NULL, 0x04000204u, 2, 0x0014u);
    rl_gba_cpu_fetch(c, 0x08000010u, 0);
    uint64_t t0 = rl_sched_cycles(run.k);
    rl_gba_cpu_fetch(c, 0x08000014u, 0);
    CHECK_EQ(rl_sched_cycles(run.k) - t0, 4);
    rl_gba_cpu_fetch(c, 0x08000018u, 0);
    rl_gba_cpu_read(c, NULL, 0x03000000u, 4);
    rl_gba_cpu_idle(c, 1);
    t0 = rl_sched_cycles(run.k);
    rl_gba_cpu_fetch(c, 0x0800001Cu, 0);
    CHECK_EQ(rl_sched_cycles(run.k) - t0, 6);
    t0 = rl_sched_cycles(run.k);
    rl_gba_cpu_refill(c, 0x08000101u, 1, 0);
    CHECK_EQ(rl_sched_cycles(run.k), t0);
    rl_gba_cpu_refill(c, 0x08000101u, 1, 1);
    CHECK_EQ(rl_sched_cycles(run.k) - t0, 6);
}

TEST(gba_run_vblankintrwait_takes_the_interrupt_inside_the_bios)
{
    CHECK(boot(swi_main) == 0);
    CHECK_EQ(rl_gba_run_start(&run), RL_PENDING);
    CHECK_EQ(rl_common(run.c)->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(hs.swis, 3);
    CHECK_EQ(hs.handler_runs, 3);
    CHECK_EQ(hs.handler_mode, RL_ARM32_IRQ);
    CHECK_EQ(hs.handler_lr, RL_GBA_BIOS_IRQ_RETURN);
    CHECK_EQ(hs.handler_r0, 0x04000000u);
    CHECK_EQ(hs.after_cpsr & 0xFFu, RL_ARM32_SYS);
    CHECK_EQ(hs.after_r4, 0x44u);
    CHECK_EQ(hs.after_r1, 1);
    CHECK_EQ(hs.after_sp, 0x03007F00u);
    CHECK_EQ(run.stats.bios_irqs, 3);
    uint64_t first = (160u - 126u) * RL_GBA_LINE_CYCLES - 841u;
    CHECK(hs.cycles_after > first + 2u * RL_GBA_FRAME_CYCLES);
    CHECK(hs.cycles_after < first + 2u * RL_GBA_FRAME_CYCLES + 2000u);
    CHECK_EQ(run.c->depth, 0);
}

TEST(gba_run_a_swi_from_a_handler_inside_a_swi_returns_to_the_outer_caller)
{
    CHECK(boot(swi_main) == 0);
    hs.handler_div = true;
    CHECK_EQ(rl_gba_run_start(&run), RL_PENDING);
    CHECK_EQ(rl_common(run.c)->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(hs.swis, 3);
    CHECK_EQ(hs.handler_runs, 3);
    CHECK_EQ(hs.div_r0, 14u);
    CHECK_EQ(hs.after_cpsr & 0xFFu, RL_ARM32_SYS);
    CHECK_EQ(hs.after_sp, 0x03007F00u);
    CHECK_EQ(run.c->depth, 0);
}

TEST(gba_run_an_irq_at_a_sync_point_runs_the_stub_and_returns)
{
    CHECK(boot(spin_main) == 0);
    CHECK_EQ(rl_gba_run_start(&run), RL_PENDING);
    CHECK_EQ(rl_common(run.c)->pend.kind, RL_PEND_EXIT_PROCESS);
    CHECK_EQ(hs.handler_runs, 2);
    CHECK_EQ(run.stats.irqs, 2);
    CHECK_EQ(hs.handler_lr, RL_GBA_BIOS_IRQ_RETURN);
    CHECK_EQ(hs.after_cpsr & 0xFFu, RL_ARM32_SYS);
    CHECK_EQ(hs.after_r4, 0x44u);
    CHECK_EQ(hs.after_r1, 0);
    CHECK_EQ(hs.after_sp, 0x03007F00u);
    CHECK_EQ(*rl_arm32_banked(run.c, RL_ARM32_IRQ, 13), 0x03007FA0u);
    CHECK_EQ(*rl_arm32_banked(run.c, RL_ARM32_IRQ, 14), 0x08000208u);
    const uint64_t second_vblank = (160u - 126u) * RL_GBA_LINE_CYCLES - 841u + RL_GBA_FRAME_CYCLES;
    CHECK(hs.cycles_after > second_vblank);
    CHECK(hs.cycles_after < second_vblank + 200u);
}

TEST(gba_run_an_irq_rising_after_the_last_access_began_waits_an_instruction)
{
    CHECK(boot(spin_main) == 0);
    rl_ctx *c = run.c;
    rl_sched_run(run.k);
    setup_irq(c, NULL);
    rl_gba_cpu_fetch(c, 0x03000000u, 0);
    rl_gba_request_irq(run.g, RL_GBA_IRQ_VBLANK);
    c->time.budget -= 4;
    CHECK_EQ(rl_sync(c, NULL, 0x03000004u), RL_OK);
    CHECK(rl_gba_irq_line(run.g));
    CHECK_EQ(hs.handler_runs, 0);
    rl_gba_cpu_fetch(c, 0x03000004u, 0);
    CHECK_EQ(rl_sync(c, NULL, 0x03000008u), RL_OK);
    CHECK_EQ(hs.handler_runs, 1);
}

static uint64_t variant_a(rl_ctx *c, uint8_t *mem)
{
    (void)c;
    (void)mem;
    return 11;
}

static uint64_t variant_b(rl_ctx *c, uint8_t *mem)
{
    (void)c;
    (void)mem;
    return 22;
}

static const rl_gba_variant variants[] = {
    {0x03000100u, 0xE12FFF1Eu, variant_a},
    {0x03000100u, 0xE3A00001u, variant_b},
    {0x03000103u, 0x4770u, variant_a},
};

TEST(gba_run_code_in_ram_runs_the_unit_the_cpu_fetched)
{
    CHECK(boot(swi_main) == 0);
    rl_ctx *c = run.c;
    run.variants = variants;
    run.variant_count = 3;
    rl_sched_run(run.k);
    rl_gba_cpu_write(c, NULL, 0x03000100u, 4, 0xE3A00001u);
    rl_gba_cpu_write(c, NULL, 0x03000104u, 4, 0xE1A00000u);
    CHECK_EQ(rl_gba_cpu_fetch_code(c, 0x03000100u, 0, 0xE3A00001u), 0);
    CHECK_EQ(rl_gba_cpu_fetch_code(c, 0x03000100u, 0, 0xE12FFF1Eu), 1);
    CHECK_EQ(rl_gba_cpu_variant(c, 0x03000100u, 0)(c, NULL), 22);
    CHECK_EQ(run.stats.code_changes, 1);
    rl_gba_cpu_refill(c, 0x03000100u, 0, 1);
    rl_gba_cpu_write(c, NULL, 0x03000100u, 4, 0xE12FFF1Eu);
    rl_gba_cpu_write(c, NULL, 0x03000104u, 4, 0xE12FFF1Eu);
    CHECK_EQ(rl_gba_cpu_fetch_code(c, 0x03000100u, 0, 0xE3A00001u), 0);
    CHECK_EQ(rl_gba_cpu_fetch_code(c, 0x03000104u, 0, 0xE1A00000u), 0);
    CHECK_EQ(rl_gba_cpu_fetch_code(c, 0x03000108u, 0, 0xE12FFF1Eu), 1);
    CHECK_EQ(rl_gba_cpu_variant(c, 0x03000108u, 0) == variant_a, 0);
    rl_gba_cpu_write(c, NULL, 0x03000102u, 2, 0x4770u);
    CHECK_EQ(rl_gba_cpu_fetch_code(c, 0x03000102u, 1, 0x4770u), 0);
    CHECK_EQ(rl_gba_cpu_fetch_code(c, 0x03000102u, 1, 0x46C0u), 1);
    CHECK(rl_gba_cpu_variant(c, 0x03000102u, 1) == variant_a);
}

#include "gdbstub.h"

#if defined(RL_DEBUG_STUB)

#include <relift/settings.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

/** @brief Instructions of count_main. */
#define COUNT_N 3000000u

static struct {
    uint64_t cycles;
    uint32_t r0;
} cnt;

/** @brief Counts r0 to COUNT_N with a sync point per instruction, then records the cycle count and stops. */
static uint64_t count_main(rl_ctx *c, uint8_t *mem)
{
    c->r[0] = 0;
    while (c->r[0] < COUNT_N) {
        c->r[0]++;
        rl_gba_cpu_fetch(c, 0x08000200u, 0);
        if (c->time.budget <= 0) {
            int rc = rl_sync(c, mem, 0x08000204u);
            if (rc)
                return RL_PENDING;
        }
    }
    cnt.cycles = rl_sched_cycles(rl_gba_run_current()->k);
    cnt.r0 = c->r[0];
    return rl_pend_exit_process(c, 0);
}

/** @brief What the client saw. */
typedef struct gclient {
    int port;
    char xml[64], r0[16], pc[16], if1[8], if2[8], cont[8];
    bool ok;
} gclient;

static bool gask(int fd, const char *pkt, char *out, size_t n)
{
    char buf[256];
    unsigned sum = 0;
    for (const char *q = pkt; *q; q++)
        sum += (unsigned char)*q;
    int len = snprintf(buf, sizeof buf, "$%s#%02x", pkt, sum & 0xff);
    if (send(fd, buf, (size_t)len, 0) != len)
        return false;
    char ch;
    do {
        if (recv(fd, &ch, 1, 0) != 1)
            return false;
    } while (ch != '$');
    size_t o = 0;
    for (;;) {
        if (recv(fd, &ch, 1, 0) != 1)
            return false;
        if (ch == '#')
            break;
        if (o + 1 < n)
            out[o++] = ch;
    }
    out[o] = 0;
    char cs[2];
    return recv(fd, cs, 2, MSG_WAITALL) == 2 && send(fd, "+", 1, 0) == 1;
}

static void *gclient_run(void *arg)
{
    gclient *s = arg;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)s->port) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bool up = false;
    for (int i = 0; i < 300 && !up; i++)
        up = connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0 || (usleep(10000), false);
    char r[2048];
    s->ok = up && gask(fd, "qXfer:features:read:target.xml:0,7ff", r, sizeof r);
    snprintf(s->xml, sizeof s->xml, "%s", strstr(r, "<architecture>") ? strstr(r, "<architecture>") : "");
    s->ok = s->ok && gask(fd, "p0", r, sizeof r);
    snprintf(s->r0, sizeof s->r0, "%s", r);
    s->ok = s->ok && gask(fd, "pf", r, sizeof r);
    snprintf(s->pc, sizeof s->pc, "%s", r);
    s->ok = s->ok && gask(fd, "m4000202,2", r, sizeof r);
    snprintf(s->if1, sizeof s->if1, "%s", r);
    s->ok = s->ok && gask(fd, "m4000202,2", r, sizeof r);
    snprintf(s->if2, sizeof s->if2, "%s", r);
    s->ok = s->ok && gask(fd, "D", r, sizeof r);
    snprintf(s->cont, sizeof s->cont, "%s", r);
    close(fd);
    return NULL;
}

TEST(gdb_server_stops_a_gba_run_at_a_sync_point_and_keeps_its_timing)
{
    /* A run with no debugger gives the reference cycle count. */
    CHECK(boot(count_main) == 0);
    CHECK_EQ(rl_gba_run_start(&run), RL_PENDING);
    CHECK_EQ(cnt.r0, COUNT_N);
    uint64_t ref = cnt.cycles;
    rl_gba_run_free(&run);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t sl = sizeof sa;
    CHECK(bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0 && getsockname(fd, (struct sockaddr *)&sa, &sl) == 0);
    close(fd);
    gclient s = { .port = ntohs(sa.sin_port) };
    char port[16];
    snprintf(port, sizeof port, "%d", s.port);
    rl_settings_declare_runtime();
    CHECK_EQ(rl_settings_set("debug.gdb", port), RL_SETTING_OK);
    CHECK(boot(count_main) == 0);
    pthread_t th;
    CHECK_EQ(pthread_create(&th, NULL, gclient_run, &s), 0);
    /* The client attaches while the guest counts, stops it at a sync point, reads and detaches. */
    CHECK_EQ(rl_gba_run_start(&run), RL_PENDING);
    pthread_join(th, NULL);
    CHECK(s.ok);
    CHECK(strncmp(s.xml, "<architecture>arm</architecture>", 32) == 0);
    unsigned long r0 = strtoul(s.r0, NULL, 16);
    uint32_t r0v = (uint32_t)((r0 >> 24) | ((r0 >> 8) & 0xff00u) | ((r0 << 8) & 0xff0000u) | (r0 << 24));
    CHECK(r0v > 0 && r0v < COUNT_N);
    CHECK(strcmp(s.pc, "04020008") == 0);
    CHECK(strcmp(s.if1, s.if2) == 0);
    CHECK(strcmp(s.cont, "OK") == 0);
    /* Stopping did not move the guest's clock. */
    CHECK_EQ(cnt.r0, COUNT_N);
    CHECK_EQ(cnt.cycles, ref);
}
#endif
