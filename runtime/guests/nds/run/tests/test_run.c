/**
 * @file test_run.c
 * @brief Both CPUs on the Nintendo DS machine with hand-written lifted
 * functions: the boot state, a word each way over the IPC FIFO, halts that
 * end the run, an entry check that goes on in another unit's function, and
 * one that finds no lifted code and stops the program.
 */
#include "cpu.h"
#include "runner.h"

#include <string.h>

#include "gdbstub.h"

static const rl_guest_desc test_guest = {
    .name = "nds-run-test",
    .page_size = 0x1000,
    .address_bits = 32,
    .tick_hz = RL_NDS_TICK_HZ,
};

#define FCNT 0x04000184u
#define FSEND 0x04000188u
#define FRECV 0x04100000u
#define HALTCNT 0x04000301u
#define RESULT 0x02100000u
#define ENTRY9 0x02000000u
#define ENTRY7 0x02380000u
#define CODE_LEN 8u

static struct {
    uint32_t boot_sp9, boot_lr7, boot_mode9;
    int exits;
    bool other_bytes;
    /** @brief count mode: each CPU counts r0 to COUNT_N with a sync point per round, then halts. */
    bool count;
    uint64_t cycles[2];
    /** @brief Rounds the ARM9 has counted, for the debugger client to attach mid-run. */
    uint32_t progress;
} T;

/** @brief Rounds of the count mode. */
#define COUNT_N 20000000u

/** @brief The cartridge: a header with both binaries of CODE_LEN bytes; the ARM9's first byte is `first9`. */
static uint8_t g_rom[0x400];

static void put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static void make_rom(uint8_t first9)
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
    g_rom[0x200] = first9;
}

/** @brief Polls until the FIFO holds a word, at most a while. */
static void wait_recv(rl_ctx *c, uint8_t *mem)
{
    for (int n = 0; n < 100000 && (rl_nds_cpu_read(c, mem, FCNT, 2) & 0x100); n++)
        c->time.budget -= 4;
}

static uint64_t arm9_body(rl_ctx *c, uint8_t *mem)
{
    T.boot_sp9 = c->r[13];
    T.boot_mode9 = c->m;
    rl_nds_cpu_write(c, mem, FCNT, 2, 0x8000);
    rl_nds_cpu_write(c, mem, FSEND, 4, 0x1234);
    wait_recv(c, mem);
    uint32_t v = (uint32_t)rl_nds_cpu_read(c, mem, FRECV, 4);
    rl_nds_cpu_write(c, mem, RESULT, 4, v);
    rl_nds_cpu_cp_write(c, RL_CP15_KEY(0, 7, 0, 4), 0, 1);
    return RL_OK;
}

/** @brief The count mode of either CPU: r0 counts to COUNT_N, a sync point each round at pc + 4. */
static int count_body(rl_ctx *c, uint8_t *mem, uint32_t pc, unsigned cpu)
{
    c->r[0] = 0;
    c->r[6] = 0x60u + cpu;
    while (c->r[0] < COUNT_N) {
        c->r[0]++;
        c->time.budget -= 2;
        if (cpu == RL_NDS_ARM9 && !(c->r[0] & 0xfff))
            __atomic_store_n(&T.progress, c->r[0], __ATOMIC_RELEASE);
        if (c->time.budget <= 0 && rl_sync(c, mem, pc + 4u) != RL_OK)
            return RL_PENDING;
    }
    T.cycles[cpu] = rl_sched_cycles(((rl_nds_arm *)c->machine)->k);
    return RL_OK;
}

static uint64_t arm9_main(rl_ctx *c, uint8_t *mem)
{
    if (rl_nds_cpu_enter(c, 0, 0))
        return rl_nds_cpu_variant(c, ENTRY9, 0)(c, mem);
    if (T.count) {
        if (count_body(c, mem, ENTRY9, RL_NDS_ARM9) != RL_OK)
            return RL_PENDING;
        rl_nds_cpu_cp_write(c, RL_CP15_KEY(0, 7, 0, 4), 0, 1);
        return RL_OK;
    }
    return arm9_body(c, mem);
}

/** @brief The ARM9's function of the second unit at the same entry, lifted from other bytes. */
static uint64_t arm9_other(rl_ctx *c, uint8_t *mem)
{
    if (rl_nds_cpu_enter(c, 2, 0))
        return rl_nds_cpu_variant(c, ENTRY9, 0)(c, mem);
    T.other_bytes = true;
    return arm9_body(c, mem);
}

static uint64_t arm7_main(rl_ctx *c, uint8_t *mem)
{
    if (rl_nds_cpu_enter(c, 1, 0))
        return rl_nds_cpu_variant(c, ENTRY7, 0)(c, mem);
    if (T.count) {
        if (count_body(c, mem, ENTRY7, RL_NDS_ARM7) != RL_OK)
            return RL_PENDING;
        rl_nds_cpu_write(c, mem, HALTCNT, 1, 0x80);
        return RL_OK;
    }
    T.boot_lr7 = c->r[14];
    rl_nds_cpu_write(c, mem, FCNT, 2, 0x8000);
    wait_recv(c, mem);
    uint32_t v = (uint32_t)rl_nds_cpu_read(c, mem, FRECV, 4);
    rl_nds_cpu_write(c, mem, FSEND, 4, v + 1);
    rl_nds_cpu_write(c, mem, HALTCNT, 1, 0x80);
    return RL_OK;
}

static uint8_t g_blob9[CODE_LEN], g_blob7[CODE_LEN], g_blob9b[CODE_LEN];
static rl_unit g_units[3];
static rl_nds_fn g_fns[3];
static rl_nds_unit g_nds_units[3];
static rl_nds_program g_prog;

static void on_exit_code(uint32_t code)
{
    (void)code;
    T.exits++;
}

/** @brief The program: unit 0 the ARM9 static, 1 the ARM7 static, 2 another ARM9 unit at the same entry lifted from bytes with first byte 0x55. */
static void make_program(void)
{
    for (unsigned i = 0; i < CODE_LEN; i++) {
        g_blob9[i] = (uint8_t)(0x90 + i);
        g_blob7[i] = (uint8_t)(0x70 + i);
        g_blob9b[i] = (uint8_t)(0x90 + i);
    }
    g_blob9b[0] = 0x55;
    rl_fn fns[3] = {arm9_main, arm7_main, arm9_other};
    const uint8_t *blobs[3] = {g_blob9, g_blob7, g_blob9b};
    uint32_t cpus[3] = {RL_NDS_ARM9, RL_NDS_ARM7, RL_NDS_ARM9};
    uint32_t at[3] = {ENTRY9, ENTRY7, ENTRY9};
    for (unsigned u = 0; u < 3; u++) {
        memset(&g_units[u], 0, sizeof g_units[u]);
        g_units[u].abi_version = RL_ABI_VERSION;
        g_units[u].ctx_size = sizeof(rl_ctx);
        g_fns[u] = (rl_nds_fn){at[u], 0, CODE_LEN, fns[u]};
        g_nds_units[u] = (rl_nds_unit){cpus[u], &g_units[u], blobs[u], &g_fns[u], 1, "test", NULL, NULL, 0};
    }
    g_prog = (rl_nds_program){g_rom, sizeof g_rom, g_nds_units, 3, "test", NULL, 0, NULL};
}

static int run_mode(rl_nds_run *r, uint8_t first9, bool count)
{
    memset(&T, 0, sizeof T);
    T.count = count;
    if (rl_core_init(&test_guest, rl_arm32_cpu()) != 0)
        return -1;
    make_rom(first9);
    make_program();
    if (rl_nds_run_init(r, &g_prog) != 0)
        return -1;
    rl_set_exit_hook(on_exit_code);
    int rc = rl_nds_run_start(r);
    rl_set_exit_hook(NULL);
    return rc;
}

static int run_once(rl_nds_run *r, uint8_t first9) { return run_mode(r, first9, false); }

TEST(nds_run_boots_both_cpus_and_trades_words_over_the_fifo)
{
    static rl_nds_run r;
    CHECK_EQ(run_once(&r, 0x90), 0);
    CHECK_EQ(T.boot_sp9, 0x03002F7Cu);
    CHECK_EQ(T.boot_mode9, RL_ARM32_SYS);
    CHECK_EQ(T.boot_lr7, ENTRY7);
    CHECK_EQ(rl_nds_read(r.m, RL_NDS_ARM9, RESULT, 4), 0x1235u);
    CHECK_EQ(rl_nds_read(r.m, RL_NDS_ARM9, 0x027FFE24u, 4), ENTRY9);
    CHECK_EQ(r.cpu[RL_NDS_ARM9].stopped, 1);
    CHECK_EQ(r.cpu[RL_NDS_ARM7].stopped, 1);
    CHECK_EQ(r.cpu[RL_NDS_ARM9].stats.compares, 1);
    CHECK(!T.other_bytes);
    rl_nds_run_free(&r);
}

TEST(nds_run_entry_with_other_bytes_goes_on_in_the_unit_lifted_from_them)
{
    static rl_nds_run r;
    CHECK_EQ(run_once(&r, 0x55), 0);
    CHECK(T.other_bytes);
    CHECK_EQ(r.cpu[RL_NDS_ARM9].stats.variants, 1);
    CHECK_EQ(rl_nds_read(r.m, RL_NDS_ARM9, RESULT, 4), 0x1235u);
    rl_nds_run_free(&r);
}

TEST(nds_run_code_no_unit_holds_stops_the_program)
{
    static rl_nds_run r;
    CHECK_EQ(run_once(&r, 0x33), 1);
    CHECK_EQ(T.exits, 1);
    CHECK_EQ(r.cpu[RL_NDS_ARM9].stats.unfound, 1);
    CHECK_EQ(r.cpu[RL_NDS_ARM9].stopped, 2);
    rl_nds_run_free(&r);
}

#if defined(RL_DEBUG_STUB)
#include <relift/settings.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

/** @brief What the client saw. */
typedef struct nclient {
    int port;
    char threads[1024], r0a[16], r0b[16], r6b[16], pcb[16], mem7[16], io[8], det[8], ioa[80], iob[80];
    bool ok;
} nclient;

static bool nask(int fd, const char *pkt, char *out, size_t n)
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

static void *nclient_run(void *arg)
{
    nclient *s = arg;
    int fd = -1;
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)s->port) };
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (int i = 0; i < 2000 && __atomic_load_n(&T.progress, __ATOMIC_ACQUIRE) < COUNT_N / 8; i++)
        usleep(1000);
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
    char r[2048];
    s->ok = up && nask(fd, "qXfer:threads:read::0,7ff", r, sizeof r);
    snprintf(s->threads, sizeof s->threads, "%s", r);
    s->ok = s->ok && nask(fd, "Hg1", r, sizeof r) && nask(fd, "p0", r, sizeof r);
    snprintf(s->r0a, sizeof s->r0a, "%s", r);
    s->ok = s->ok && nask(fd, "Hg2", r, sizeof r) && nask(fd, "p0", r, sizeof r);
    snprintf(s->r0b, sizeof s->r0b, "%s", r);
    s->ok = s->ok && nask(fd, "p6", r, sizeof r);
    snprintf(s->r6b, sizeof s->r6b, "%s", r);
    s->ok = s->ok && nask(fd, "pf", r, sizeof r);
    snprintf(s->pcb, sizeof s->pcb, "%s", r);
    s->ok = s->ok && nask(fd, "m2380000,4", r, sizeof r);
    snprintf(s->mem7, sizeof s->mem7, "%s", r);
    /* Twice each, while stopped: IPCSYNC/IPCFIFOCNT, the FIFO receive port, IME/IE/IF, VCOUNT/DISPSTAT, timer 0. */
    static const char *const io_pk[] = { "m4000180,8", "m4100000,4", "m4000208,10", "m4000004,4", "m4000100,4" };
    for (int round = 0; round < 2; round++)
        for (int k = 0; k < 5; k++) {
            s->ok = s->ok && nask(fd, io_pk[k], r, sizeof r);
            char *dst = round ? s->iob : s->ioa;
            size_t o = strlen(dst);
            snprintf(dst + o, 80 - o, "%.15s,", r);
        }
    s->ok = s->ok && nask(fd, "Hg1", r, sizeof r) && nask(fd, "m4000180,8", r, sizeof r);
    snprintf(s->io, sizeof s->io, "%.7s", r);
    s->ok = s->ok && nask(fd, "D", r, sizeof r);
    snprintf(s->det, sizeof s->det, "%s", r);
    close(fd);
    return NULL;
}

static uint32_t le32(const char *hex)
{
    unsigned long v = strtoul(hex, NULL, 16);
    return (uint32_t)((v >> 24) | ((v >> 8) & 0xff00u) | ((v << 8) & 0xff0000u) | (v << 24));
}

TEST(gdb_server_stops_both_nds_cpus_and_keeps_their_timing)
{
    static rl_nds_run r;
    CHECK_EQ(run_mode(&r, 0x90, true), 0);
    uint64_t ref9 = T.cycles[RL_NDS_ARM9], ref7 = T.cycles[RL_NDS_ARM7];
    CHECK(ref9 > 0 && ref7 > 0);
    rl_nds_run_free(&r);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t sl = sizeof sa;
    CHECK(bind(fd, (struct sockaddr *)&sa, sizeof sa) == 0 && getsockname(fd, (struct sockaddr *)&sa, &sl) == 0);
    close(fd);
    static nclient s;
    s.port = ntohs(sa.sin_port);
    char port[16];
    snprintf(port, sizeof port, "%d", s.port);
    rl_settings_declare_runtime();
    CHECK_EQ(rl_settings_set("debug.gdb", port), RL_SETTING_OK);
    pthread_t th;
    CHECK_EQ(pthread_create(&th, NULL, nclient_run, &s), 0);
    CHECK_EQ(run_mode(&r, 0x90, true), 0);
    pthread_join(th, NULL);
    rl_nds_run_free(&r);
    CHECK(s.ok);
    /* Two threads: the one holding the run stopped at its sync point, the other waiting with its last sync pc. */
    CHECK(strstr(s.threads, "<thread id=\"1\"") != NULL);
    CHECK(strstr(s.threads, "<thread id=\"2\"") != NULL);
    CHECK(strstr(s.threads, "stopped; pc 0x2000004") || strstr(s.threads, "stopped; pc 0x2380004"));
    /* The ARM9 runs ahead (the CPUs agree on order only at shared accesses), so the ARM7 may not have started. */
    CHECK(strstr(s.threads, "pc 0x2380004") || strstr(s.threads, "pc 0x2380000"));
    CHECK(strstr(s.threads, "running") == NULL);
    CHECK(le32(s.r0a) > 0 && le32(s.r0a) < COUNT_N);
    CHECK(le32(s.r0b) < COUNT_N);
    CHECK(le32(s.pcb) == ENTRY7 + 4u || le32(s.pcb) == ENTRY7);
    CHECK(le32(s.r6b) == (le32(s.pcb) == ENTRY7 ? 0u : 0x60u + RL_NDS_ARM7));
    /* The ARM7's map at its entry holds its code (0x70 ...). */
    CHECK(strcmp(s.mem7, "70717273") == 0);
    CHECK(strcmp(s.ioa, s.iob) == 0);
    CHECK(strstr(s.ioa, "E") == NULL);
    CHECK(strlen(s.io) == 7);
    CHECK(strcmp(s.det, "OK") == 0);
    CHECK_EQ(T.cycles[RL_NDS_ARM9], ref9);
    CHECK_EQ(T.cycles[RL_NDS_ARM7], ref7);
}
#endif
