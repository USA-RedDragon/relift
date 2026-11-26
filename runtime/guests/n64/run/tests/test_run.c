/**
 * @file test_run.c
 * @brief The VR4300 CPU on the Nintendo 64 machine with hand-written lifted
 * functions: the high-level boot's registers, code bound by its hash at the
 * first entry, a run whose bytes changed is missed code, an instruction
 * cache invalidation checks again, a VI interrupt taken at a sync point
 * enters the general vector and returns, and boot code in DMEM starts the
 * CPU there and is checked again after a write to DMEM. The ROM is
 * synthetic.
 */
#include "cpu.h"
#include "runner.h"

#include <stddef.h>
#include <string.h>

/** @brief Whether the n guest bytes of RDRAM at a equal b. */
static bool ram_eq(rl_n64 *m, uint32_t a, const uint8_t *b, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint8_t v;
        rl_n64_rdram_get(m, a + i, &v, 1);
        if (v != b[i])
            return false;
    }
    return true;
}

static const rl_guest_desc test_guest = {
    .name = "n64-run-test",
    .page_size = 0x1000,
    .address_bits = 32,
    .tick_hz = RL_N64_HZ,
};

#define K0(a) (UINT64_C(0xFFFFFFFF80000000) | (a))
#define K1(a) (UINT64_C(0xFFFFFFFFA0000000) | (a))

static struct {
    int mains;
    int handlers;
    uint64_t handler_epc;
    int spins;
    int boots;
    uint64_t boot_sp, boot_ra, boot_t3;
} hs;

static uint64_t boot_fn(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    hs.boots++;
    hs.boot_sp = c->r[29];
    hs.boot_ra = c->r[31];
    hs.boot_t3 = c->r[11];
    rl_n64_write(c->machine, 0x04000044u, 4, 0);
    return RL_OK;
}

static uint64_t handler(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    hs.handlers++;
    hs.handler_epc = c->cp0_epc;
    rl_n64_write(c->machine, 0x04400010u, 4, 0);
    RL_MUSTTAIL return rl_mips_eret_to(c, rl_mips_eret(c))(c, mem);
}

/** @brief The budget before the first handler's ERET and after it. */
static int64_t eret_budget[2];

/**
 * @brief Takes the VI interrupt, and the first time raises the SP interrupt
 * while EXL is set and gives the slice more time. ERET may go anywhere (an
 * OS switching threads), so it must end the budget itself for the line to be
 * taken at the next sync point.
 */
static uint64_t handler2(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    hs.handlers++;
    rl_mips_cp0_sync(c);
    rl_n64_write(c->machine, 0x04400010u, 4, 0);
    rl_n64_write(c->machine, 0x04040010u, 4, hs.handlers == 1 ? 0x10u : 0x08u);
    if (hs.handlers == 1) {
        c->time.deadline += 100;
        c->time.budget += 100;
        eret_budget[0] = c->time.budget;
    }
    uint64_t to = rl_mips_eret(c);
    if (hs.handlers == 1)
        eret_budget[1] = c->time.budget;
    RL_MUSTTAIL return rl_mips_eret_to(c, to)(c, mem);
}

static uint64_t main_fn(rl_ctx *c, uint8_t *mem)
{
    (void)mem;
    hs.mains++;
    c->r[2] = 42;
    return RL_OK;
}

static uint64_t spin_fn(rl_ctx *c, uint8_t *mem)
{
    hs.mains++;
    rl_n64_write(c->machine, 0x0430000Cu, 4, c->r[3] ? 0x82u : 0x80u);
    rl_n64_write(c->machine, 0x04400000u, 4, 0x3002u);
    rl_n64_write(c->machine, 0x0440000Cu, 4, 0x10u);
    rl_n64_write(c->machine, 0x04400018u, 4, 0x20Du);
    rl_n64_write(c->machine, 0x0440001Cu, 4, 0xC15u);
    rl_n64_write(c->machine, 0x04400020u, 4, 0x0C150C15u);
    c->cp0_status = 0x34000401u;
    int rc = rl_n64_cp0_sync(c, mem, K0(0x1004));
    while (!rc && hs.handlers < (c->r[3] ? 2 : 1) && hs.spins < 1000000) {
        hs.spins++;
        c->time.budget -= 100;
        if (c->time.budget <= 0)
            rc = rl_sync(c, mem, K0(0x1004));
    }
    return rc;
}

static const rl_fn_entry fns_main[] = {
    {K0(0x1000), 8, RL_FN_RANGE, main_fn},
};
static const rl_fn_entry fns_spin[] = {
    {K0(0x180), 8, RL_FN_RANGE, handler},
    {K0(0x1000), 8, RL_FN_RANGE, spin_fn},
};

static const rl_fn_entry fns_spin2[] = {
    {K0(0x180), 8, RL_FN_RANGE, handler2},
    {K0(0x1000), 8, RL_FN_RANGE, spin_fn},
};

static const rl_fn_entry fns_boot[] = {
    {K1(0x04000040), 8, RL_FN_RANGE, boot_fn},
};

static uint8_t rom[0x102000];
static rl_n64_run run;
static rl_n64 *mach;
static rl_unit unit;
static rl_n64_code code[2];

static uint64_t fnv(const uint8_t *b, uint32_t n)
{
    uint64_t h = 0xCBF29CE484222325ull;
    for (uint32_t i = 0; i < n; i++)
        h = (h ^ b[i]) * 0x100000001B3ull;
    return h;
}

static int boot_with(const rl_fn_entry *fns, uint32_t n, bool dmem)
{
    if (rl_core_init(&test_guest, rl_mips_cpu()) != 0)
        return -1;
    rl_units_reset();
    if (mach) {
        rl_n64_run_free(&run);
        rl_n64_destroy(mach);
    }
    memset(&hs, 0, sizeof hs);
    for (size_t i = 0; i < sizeof rom; i++)
        rom[i] = (uint8_t)(i * 13u + 5u);
    rom[0x3E] = 'E';
    memset(&unit, 0, sizeof unit);
    unit.abi_version = RL_ABI_VERSION;
    unit.ctx_size = sizeof(rl_ctx);
    unit.name = "n64-run-test";
    unit.fn_count = n;
    unit.fns = fns;
    code[0] = (rl_n64_code){0, 0, 0x1000u, 8u, K0(0x1000), fnv(rom + 0x1000, 8)};
    static const uint8_t zero[8];
    code[1] = (rl_n64_code){0, 0, 0, 8u, K0(0x180), fnv(zero, 8)};
    if (dmem)
        code[1] = (rl_n64_code){0, 0, 0x40u, 8u, K1(0x04000040), fnv(rom + 0x40, 8)};
    mach = rl_n64_create(rom, sizeof rom);
    static const rl_unit *list[] = {&unit};
    if (!mach || rl_n64_code_init(mach, list, 1, code, 2) != 0)
        return -1;
    return rl_n64_run_init(&run, mach, 6102, 0x80001000u);
}

static int boot(const rl_fn_entry *fns, uint32_t n) { return boot_with(fns, n, false); }

TEST(n64_run_boots_with_the_registers_the_boot_code_leaves)
{
    CHECK(boot(fns_main, 1) == 0);
    rl_ctx *c = run.c;
    CHECK_EQ(run.entry, K0(0x1000));
    CHECK_EQ(c->r[29], UINT64_C(0xFFFFFFFFA4001FF0));
    CHECK_EQ(c->r[22], 0x3Fu);
    CHECK_EQ(c->r[20], 1u);
    CHECK_EQ(c->cp0_status, 0x34000000u);
    CHECK_EQ(rl_n64_read(mach, 0x318u, 4), RL_N64_RDRAM_SIZE);
    CHECK(ram_eq(mach, 0x1000, rom + 0x1000, 0x100000));
}

TEST(n64_run_binds_code_by_its_hash_at_the_first_entry)
{
    CHECK(boot(fns_main, 1) == 0);
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(hs.mains, 1);
    CHECK_EQ(run.c->r[2], 42u);
    CHECK_EQ(rl_n64_get_stats(mach)->checks, 1u);
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(rl_n64_get_stats(mach)->checks, 1u);
    rl_mips_cache(run.c, 0x10u, K0(0x1000), 1);
    CHECK_EQ(rl_n64_get_stats(mach)->invalidations, 1u);
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(rl_n64_get_stats(mach)->checks, 2u);
    CHECK_EQ(hs.mains, 3);
}

TEST(n64_run_changed_bytes_are_missed_code)
{
    CHECK(boot(fns_main, 1) == 0);
    rl_n64_rdram(mach)[(0x1004) ^ 3] ^= 1u;
    CHECK_EQ(rl_n64_run_start(&run), RL_PENDING);
    CHECK_EQ(hs.mains, 0);
    CHECK_EQ(rl_n64_get_stats(mach)->mismatches, 1u);
}

TEST(n64_run_a_vi_interrupt_enters_the_vector_at_a_sync_point)
{
    CHECK(boot(fns_spin, 2) == 0);
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(hs.handlers, 1);
    CHECK_EQ(hs.handler_epc, K0(0x1004));
    CHECK_EQ(rl_n64_irq_pending(mach), 0);
    CHECK_EQ(run.c->cp0_status & 2u, 0u);
}

TEST(n64_run_an_interrupt_raised_in_a_handler_is_taken_right_after_eret)
{
    CHECK(boot(fns_spin2, 2) == 0);
    run.c->r[3] = 1;
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(hs.handlers, 2);
    CHECK(eret_budget[0] > 0);
    CHECK(eret_budget[1] <= 0);
    CHECK_EQ(rl_n64_irq_pending(mach), 0);
}

TEST(n64_run_starts_the_boot_code_in_dmem)
{
    CHECK(boot_with(fns_boot, 1, true) == 0);
    CHECK_EQ(run.entry, K1(0x04000040));
    CHECK_EQ(memcmp(rl_n64_spmem(mach), rom, 0x1000), 0);
    CHECK_EQ(rl_n64_read(mach, 0x318u, 4), 0);
    CHECK_EQ(rl_n64_read(mach, 0x0470000Cu, 4), 0x14u);
    CHECK_EQ(rl_n64_run_start(&run), RL_OK);
    CHECK_EQ(hs.boots, 1);
    CHECK_EQ(hs.boot_t3, K1(0x04000040));
    CHECK_EQ(hs.boot_sp, UINT64_C(0xFFFFFFFFA4001FF0));
    CHECK_EQ(hs.boot_ra, UINT64_C(0xFFFFFFFFA4001554));
    CHECK_EQ(rl_n64_get_stats(mach)->checks, 1u);
    CHECK_EQ(rl_n64_get_stats(mach)->invalidations, 1u);
    CHECK_EQ(rl_n64_run_start(&run), RL_PENDING);
    CHECK_EQ(hs.boots, 1);
    CHECK_EQ(rl_n64_get_stats(mach)->mismatches, 1u);
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

/** @brief Rounds of count_fn. */
#define COUNT_N 3000000u

static struct {
    uint64_t cycles;
    bool wait_for_debugger;
} cs;

/** @brief Counts r8 with a sync point per round at 0x80001004, to COUNT_N or until RDRAM 0x2000 holds 1. */
static uint64_t count_fn(rl_ctx *c, uint8_t *mem)
{
    c->r[8] = 0;
    c->r[9] = 0x99;
    for (;;) {
        if (cs.wait_for_debugger) {
            uint8_t b[4];
            rl_n64_rdram_get_cpu(c->machine, 0x2000, b, 4);
            if (b[3] == 1)
                break;
        } else if (c->r[8] >= COUNT_N) {
            break;
        }
        c->r[8]++;
        c->time.budget -= 2;
        if (c->time.budget <= 0) {
            int rc = rl_sync(c, mem, K0(0x1004));
            if (rc)
                return (uint64_t)rc;
        }
    }
    cs.cycles = rl_sched_cycles(run.k);
    return RL_OK;
}

static const rl_fn_entry fns_count[] = {
    {K0(0x1000), 8, RL_FN_RANGE, count_fn},
};

/**
 * @brief For tools/gdb/stub-test.sh: with RELIFT_GDB_TEST_PORT set, serves an N64 run that counts until the debugger
 * writes 1 to the word at 0x80002000; without it, passes at once.
 */
TEST(gdb_serve_an_n64_run_for_a_debugger)
{
    const char *port = getenv("RELIFT_GDB_TEST_PORT");
    if (!port)
        return;
    rl_settings_declare_runtime();
    CHECK_EQ(rl_settings_set("debug.gdb", port), RL_SETTING_OK);
    CHECK(boot(fns_count, 1) == 0);
    cs.wait_for_debugger = true;
    static const uint8_t word[4] = { 0x12, 0x34, 0x56, 0x78 };
    rl_n64_rdram_put(mach, 0x2004, word, 4);
    fprintf(stderr, "gdb-serve: n64\n");
    rl_n64_run_start(&run);
    cs.wait_for_debugger = false;
}

/** @brief What the client saw. */
typedef struct mclient {
    int port;
    char xml[64], r8[20], r9[20], pc[20], mem[16], io[12], io2[12], spst[12], spst2[12], mi[12], ai[12], ai2[12], dp[12], rom[12], pif[12], det[8], threads[512], rpc[20], imem[16];
    bool ok;
} mclient;

static bool mask(int fd, const char *pkt, char *out, size_t n)
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
    char cs2[2];
    return recv(fd, cs2, 2, MSG_WAITALL) == 2 && send(fd, "+", 1, 0) == 1;
}

static void *mclient_run(void *arg)
{
    mclient *s = arg;
    int fd = -1;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)s->port) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    /* A socket whose connect was refused cannot connect again on macOS, so each try gets its own. */
    bool up = false;
    for (int i = 0; i < 300 && !up; i++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        up = fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0;
        if (!up) {
            if (fd >= 0)
                close(fd);
            usleep(10000);
        }
    }
    char r[4096];
    s->ok = up && mask(fd, "qXfer:features:read:target.xml:0,fff", r, sizeof r);
    snprintf(s->xml, sizeof s->xml, "%s", strstr(r, "<architecture>") ? strstr(r, "<architecture>") : "");
    s->ok = s->ok && mask(fd, "p8", r, sizeof r);
    snprintf(s->r8, sizeof s->r8, "%s", r);
    s->ok = s->ok && mask(fd, "p9", r, sizeof r);
    snprintf(s->r9, sizeof s->r9, "%s", r);
    s->ok = s->ok && mask(fd, "p25", r, sizeof r);
    snprintf(s->pc, sizeof s->pc, "%s", r);
    s->ok = s->ok && mask(fd, "mffffffff80001000,4", r, sizeof r);
    snprintf(s->mem, sizeof s->mem, "%s", r);
    s->ok = s->ok && mask(fd, "mffffffffa4400010,4", r, sizeof r);
    snprintf(s->io, sizeof s->io, "%s", r);
    static const struct {
        const char *pkt;
        size_t off;
    } more[] = {
        { "mffffffffa4040010,4", offsetof(mclient, spst) },
        { "mffffffffa4300008,4", offsetof(mclient, mi) },
        { "mffffffffa4500004,4", offsetof(mclient, ai) },
        { "mffffffffa4100008,4", offsetof(mclient, dp) },
        { "mffffffffb0000000,4", offsetof(mclient, rom) },
        { "mffffffffbfc00000,4", offsetof(mclient, pif) },
        { "mffffffffa4040010,4", offsetof(mclient, spst2) },
        { "mffffffffa4500004,4", offsetof(mclient, ai2) },
        { "mffffffffa4400010,4", offsetof(mclient, io2) },
    };
    for (size_t k = 0; k < sizeof more / sizeof more[0]; k++) {
        s->ok = s->ok && mask(fd, more[k].pkt, r, sizeof r);
        snprintf((char *)s + more[k].off, 12, "%s", r);
    }
    s->ok = s->ok && mask(fd, "qXfer:threads:read::0,1ff", r, sizeof r);
    snprintf(s->threads, sizeof s->threads, "%s", r);
    s->ok = s->ok && mask(fd, "Hg2", r, sizeof r) && mask(fd, "p25", r, sizeof r);
    snprintf(s->rpc, sizeof s->rpc, "%s", r);
    s->ok = s->ok && mask(fd, "m4001000,4", r, sizeof r);
    snprintf(s->imem, sizeof s->imem, "%s", r);
    s->ok = s->ok && mask(fd, "D", r, sizeof r);
    snprintf(s->det, sizeof s->det, "%s", r);
    close(fd);
    return NULL;
}

TEST(gdb_server_stops_an_n64_run_at_a_sync_point_and_keeps_its_timing)
{
    CHECK(boot(fns_count, 1) == 0);
    rl_n64_run_start(&run);
    uint64_t ref = cs.cycles;
    CHECK(ref > 0);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t sl = sizeof sa;
    CHECK(bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0 && getsockname(fd, (struct sockaddr *)&sa, &sl) == 0);
    close(fd);
    static mclient s;
    s.port = ntohs(sa.sin_port);
    char port[16];
    snprintf(port, sizeof port, "%d", s.port);
    rl_settings_declare_runtime();
    CHECK_EQ(rl_settings_set("debug.gdb", port), RL_SETTING_OK);
    CHECK(boot(fns_count, 1) == 0);
    char why[128];
    CHECK_EQ(rl_n64_run_rsp(&run, NULL, 0, why, sizeof why), 0);
    static const uint8_t ucode[4] = { 0x24, 0x01, 0x00, 0x05 };
    memcpy(rl_n64_spmem(mach) + 0x1000, ucode, 4);
    pthread_t th;
    CHECK_EQ(pthread_create(&th, NULL, mclient_run, &s), 0);
    rl_n64_run_start(&run);
    pthread_join(th, NULL);
    CHECK(s.ok);
    CHECK(strncmp(s.xml, "<architecture>mips:4300</architecture>", 38) == 0);
    /* Registers big-endian: r8 counts, r9 is 0x99, pc is the sync point's next. */
    unsigned long long r8 = strtoull(s.r8, NULL, 16);
    CHECK(r8 > 0 && r8 < COUNT_N);
    CHECK(strcmp(s.r9, "0000000000000099") == 0);
    CHECK(strcmp(s.pc, "ffffffff80001004") == 0);
    /* RDRAM through kseg0 holds the code bytes the cartridge boot copied there. */
    char want[9];
    snprintf(want, sizeof want, "%02x%02x%02x%02x", rom[0x1000], rom[0x1001], rom[0x1002], rom[0x1003]);
    CHECK(strcmp(s.mem, want) == 0);
    /* Registers read as peeks: the same twice, no RSP catch-up, the cartridge from its image, the PIF bus refused. */
    CHECK_EQ(strlen(s.io), 8u);
    CHECK(strcmp(s.io, s.io2) == 0);
    CHECK_EQ(strlen(s.spst), 8u);
    CHECK(strcmp(s.spst, s.spst2) == 0);
    CHECK_EQ(strlen(s.mi), 8u);
    CHECK_EQ(strlen(s.ai), 8u);
    CHECK(strcmp(s.ai, s.ai2) == 0);
    CHECK_EQ(strlen(s.dp), 8u);
    char wrom[9];
    snprintf(wrom, sizeof wrom, "%02x%02x%02x%02x", rom[0], rom[1], rom[2], rom[3]);
    CHECK(strcmp(s.rom, wrom) == 0);
    CHECK(strcmp(s.pif, "E14") == 0);
    /* Thread 2, the RSP: its pc in the SP window and IMEM read there. */
    CHECK(strstr(s.threads, "<thread id=\"2\"") != NULL);
    CHECK(strcmp(s.rpc, "0000000004001000") == 0);
    CHECK(strcmp(s.imem, "24010005") == 0);
    CHECK(strcmp(s.det, "OK") == 0);
    CHECK_EQ(cs.cycles, ref);
}
#endif
