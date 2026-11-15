/**
 * @file oracle.c
 * @brief The oracle process, the scenario assembler and the replay.
 */
#include "oracle.h"

#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define CODE_BASE 0x03000000u
#define MAX_CODE 4096u
#define MAX_MARKS 1024u
#define LINE 8192

typedef struct orc {
    pid_t pid;
    FILE *to, *from;
    char line[LINE];
} orc;

static bool find_oracle(char *out, size_t len)
{
    const char *env = getenv("RELIFT_NBA_ORACLE");
    if (env && *env) {
        snprintf(out, len, "%s", env);
    } else {
        const char *home = getenv("HOME");
        if (!home)
            return false;
        snprintf(out, len, "%s/.cache/relift/gba-oracle/bin/nba-oracle", home);
    }
    return access(out, X_OK) == 0;
}

static bool write_rom(char *path, size_t len)
{
    const char *dir = getenv("TMPDIR");
    snprintf(path, len, "%s/relift-gba-idle-%ld.gba", dir && *dir ? dir : "/tmp", (long)getpid());
    FILE *f = fopen(path, "wb");
    if (!f)
        return false;
    uint8_t rom[0x200] = {0xFE, 0xFF, 0xFF, 0xEA};
    bool ok = fwrite(rom, 1, sizeof rom, f) == sizeof rom;
    return fclose(f) == 0 && ok;
}

static bool orc_open(orc *o, const char *bin, const char *rom)
{
    int in[2], out[2];
    if (pipe(in) != 0 || pipe(out) != 0)
        return false;
    o->pid = fork();
    if (o->pid < 0)
        return false;
    if (o->pid == 0) {
        dup2(in[0], 0);
        dup2(out[1], 1);
        close(in[1]);
        close(out[0]);
        execl(bin, bin, "--rom", rom, "--skip-bios", (char *)NULL);
        _exit(127);
    }
    close(in[0]);
    close(out[1]);
    o->to = fdopen(in[1], "w");
    o->from = fdopen(out[0], "r");
    return o->to && o->from && fgets(o->line, LINE, o->from) && strncmp(o->line, "READY", 5) == 0;
}

static void orc_close(orc *o)
{
    if (o->to) {
        fputs("QUIT\n", o->to);
        fclose(o->to);
    }
    if (o->from)
        fclose(o->from);
    if (o->pid > 0) {
        kill(o->pid, SIGTERM);
        waitpid(o->pid, NULL, 0);
    }
}

static bool terminal(const char *l)
{
    static const char *const ends[] = {"OK", "ERR", "REGS", "MEM", "IO", "FRAME"};
    for (unsigned i = 0; i < sizeof ends / sizeof ends[0]; i++)
        if (strncmp(l, ends[i], strlen(ends[i])) == 0)
            return true;
    return false;
}

typedef void line_fn(void *ctx, const char *line);

static bool orc_cmd(orc *o, line_fn *fn, void *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(o->to, fmt, ap);
    va_end(ap);
    fputc('\n', o->to);
    fflush(o->to);
    for (;;) {
        if (!fgets(o->line, LINE, o->from))
            return false;
        if (fn)
            fn(ctx, o->line);
        if (terminal(o->line))
            return strncmp(o->line, "ERR", 3) != 0;
    }
}

typedef struct prog {
    uint32_t code[MAX_CODE];
    unsigned n;
    unsigned mark[MAX_MARKS];
    unsigned nmark;
} prog;

static void emit(prog *p, uint32_t w)
{
    if (p->n < MAX_CODE)
        p->code[p->n++] = w;
}

static void ldr_const(prog *p, unsigned rd, uint32_t v)
{
    emit(p, 0xE59F0000u | rd << 12);
    emit(p, 0xEA000000u);
    emit(p, v);
}

static void mark(prog *p) { p->mark[p->nmark++] = p->n; }

static void assemble(prog *p, const dop *ops, unsigned pad)
{
    p->n = p->nmark = 0;
    for (unsigned i = 0; i < pad; i++)
        emit(p, 0xE1A00000u);
    for (const dop *d = ops; d->k; d++) {
        switch (d->k) {
        case 'w':
        case 'h':
            ldr_const(p, 1, d->addr);
            ldr_const(p, 2, d->val);
            mark(p);
            emit(p, d->size == 4 ? 0xE5812000u : d->size == 2 ? 0xE1C120B0u : 0xE5C12000u);
            break;
        case 'r':
            ldr_const(p, 1, d->addr);
            mark(p);
            emit(p, d->size == 4 ? 0xE5913000u : d->size == 2 ? 0xE1D130B0u : 0xE5D13000u);
            break;
        case 'n':
            for (uint32_t k = 0; k < d->val; k++)
                emit(p, 0xE1A00000u);
            break;
        case 'l':
            ldr_const(p, 0, d->val);
            emit(p, 0xE2500001u);
            emit(p, 0x1AFFFFFDu);
            break;
        case 'm':
            mark(p);
            emit(p, 0xE1A00000u);
            break;
        default:
            break;
        }
    }
    emit(p, 0xEAFFFFFEu);
}

typedef struct trace {
    uint64_t cyc;
    uint32_t r3;
    uint64_t halt_until;
    bool got;
} trace;

static void on_line(void *ctx, const char *l)
{
    trace *t = ctx;
    if (l[0] == 'H') {
        t->halt_until = strtoull(l + 2, NULL, 10);
    } else if (l[0] == 'T' && !t->got) {
        char *e;
        t->cyc = strtoull(l + 2, &e, 10);
        uint32_t r[16];
        strtoul(e, &e, 16);
        strtoul(e, &e, 16);
        for (unsigned i = 0; i < 16; i++)
            r[i] = (uint32_t)strtoul(e, &e, 16);
        t->r3 = r[3];
        t->got = true;
    }
}

typedef struct memdump {
    uint8_t *buf;
    uint32_t len;
    bool got;
} memdump;

static void on_mem(void *ctx, const char *l)
{
    memdump *m = ctx;
    if (strncmp(l, "MEM ", 4) != 0)
        return;
    for (uint32_t i = 0; i < m->len; i++) {
        char h[3] = {l[4 + 2 * i], l[5 + 2 * i], 0};
        m->buf[i] = (uint8_t)strtoul(h, NULL, 16);
    }
    m->got = true;
}

static uint64_t lcd_line_at_zero = 225, lcd_dot_at_zero = 1006;

dres oracle_run(const char *name, const dop *ops, unsigned pad, uint32_t mem_addr, uint32_t mem_len)
{
    dres r = {0};
    char bin[1024], rom[1024];
    if (!find_oracle(bin, sizeof bin)) {
        r.skipped = true;
        return r;
    }
    static prog p;
    assemble(&p, ops, pad);
    static trace tr[MAX_MARKS];
    memset(tr, 0, sizeof tr);
    orc o = {0};
    if (!write_rom(rom, sizeof rom) || !orc_open(&o, bin, rom)) {
        r.broken = true;
        orc_close(&o);
        return r;
    }
    static char hex[MAX_CODE * 8 + 1];
    for (unsigned i = 0; i < p.n; i++)
        for (unsigned b = 0; b < 4; b++)
            snprintf(&hex[i * 8 + b * 2], 3, "%02x", (p.code[i] >> (8 * b)) & 0xFF);
    bool ok = orc_cmd(&o, NULL, NULL, "POKE %x %s", CODE_BASE, hex) && orc_cmd(&o, NULL, NULL, "JUMP %x 0", CODE_BASE);
    for (unsigned i = 0; ok && i < p.nmark; i++) {
        uint32_t at = CODE_BASE + 4 * p.mark[i];
        ok = orc_cmd(&o, NULL, NULL, "RUN until 100000000 %x", at) && orc_cmd(&o, on_line, &tr[i], "STEP 1");
        if (ok && tr[i].halt_until == 0 && !tr[i].got)
            ok = false;
    }
    uint8_t *want = NULL;
    if (ok && mem_len) {
        want = malloc(mem_len);
        memdump md = {want, mem_len, false};
        ok = orc_cmd(&o, on_mem, &md, "MEM %x %x", mem_addr, mem_len) && md.got;
    }
    orc_close(&o);
    unlink(rom);
    if (!ok) {
        r.broken = true;
        free(want);
        return r;
    }

    uint8_t *img = calloc(1, 0x200);
    img[0] = 0xFE;
    img[1] = 0xFF;
    img[2] = 0xFF;
    img[3] = 0xEA;
    mach m = mach_new_with(img, 0x200, NULL, 0);
    rl_gba_lcd_set(m.g, (unsigned)lcd_line_at_zero, (unsigned)lcd_dot_at_zero);
    unsigned k = 0;
    uint64_t prev_end = 0, halt_charge = 0;
    for (const dop *d = ops; d->k; d++) {
        if (d->k != 'w' && d->k != 'r' && d->k != 'm' && d->k != 'h')
            continue;
        trace *t = &tr[k++];
        uint64_t end = t->cyc;
        if (d->k == 'w') {
            mach_wr(&m, end, d->addr, d->size, d->val);
        } else if (d->k == 'h') {
            mach_to(&m, end);
            uint64_t before = mach_now(&m), c0 = rl_gba_cycles(m.g);
            rl_gba_write(m.g, d->addr, d->size, d->val);
            r.halt_here += mach_now(&m) - before;
            halt_charge += rl_gba_cycles(m.g) - c0;
        } else if (d->k == 'r') {
            uint32_t got = mach_rd(&m, end - 1, d->addr, d->size);
            uint32_t want_v = d->size == 4 ? t->r3 : t->r3 & ((1u << (8 * d->size)) - 1u);
            r.reads++;
            if (got != want_v) {
                r.diffs++;
                printf("  %s pad %u: read %u of 0x%08x at tick %llu: here 0x%x, oracle 0x%x\n", name, pad, d->size, d->addr,
                       (unsigned long long)(end - 1), got, want_v);
            }
        } else {
            uint64_t start = prev_end;
            mach_to(&m, start);
            if (end > start + 1)
                r.stall_oracle += end - start - 1;
        }
        if (t->halt_until)
            r.halt_oracle += t->halt_until - prev_end;
        prev_end = end;
    }
    r.stall_here = rl_gba_cycles(m.g) - halt_charge;
    if (mem_len) {
        r.reads++;
        for (uint32_t i = 0; i < mem_len; i++) {
            uint32_t here = rl_gba_peek(m.g, mem_addr + i, 1);
            if (here != want[i]) {
                r.diffs++;
                printf("  %s pad %u: byte at 0x%08x: here 0x%02x, oracle 0x%02x\n", name, pad, mem_addr + i, here, want[i]);
                break;
            }
        }
    }
    free(want);
    mach_free(&m);
    return r;
}
