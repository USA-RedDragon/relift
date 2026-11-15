/**
 * @file lockstep.c
 * @brief Lockstep check of the device slots against rustyboi's own run.
 *
 *   gb_lockstep [--model dmg|cgb|cgbe|agb|mgb] [--frames N] ROM...
 *
 * For each ROM, machine A is the device library on its own CPU (one native
 * step per instruction), machine B is the relift runtime: the memory layer
 * on the library's bus, the seven slot devices behind a scheduler, and the
 * bodies of the instructions run here in C, every access through
 * rl_gb_read and rl_gb_write. Forms the bodies do not cover (CB prefix,
 * HALT, STOP, DI, EI, RETI, DAA, SP+e forms) run on the library's CPU.
 * After every instruction the registers and the dot count must match; at
 * every frame boundary the presented frame and the video RAM, work RAM,
 * OAM, I/O register and high RAM contents must match. Then each machine is
 * timed alone over the same frames.
 */
#define _POSIX_C_SOURCE 199309L
#include "gbdev.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define ZF 0x80
#define NF 0x40
#define HF 0x20
#define CF 0x10

typedef struct machine {
    rl_gb *g;
    rl_sched *s;
    rl_sched_cpu *k;
    rl_gbdev *d;
    rlgbd *h;
    uint64_t bodies, fallbacks, boundaries;
} machine;

static double seconds(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec * 1e-9;
}

static uint8_t *load(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    *n = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(*n);
    if (b && fread(b, 1, *n, f) != *n) {
        free(b);
        b = NULL;
    }
    fclose(f);
    return b;
}

#define RD(a) rl_gb_read(m->g, m->k, (uint16_t)(a))
#define WR(a, v) rl_gb_write(m->g, m->k, (uint16_t)(a), (uint8_t)(v))
#define IMM() RD(r->pc++)
#define DE ((uint16_t)(r->d << 8 | r->e))
#define HL ((uint16_t)(r->h << 8 | r->l))
#define SET16(hi, lo, v) (r->hi = (uint8_t)((v) >> 8), r->lo = (uint8_t)(v))

static uint8_t inc8(rlgbd_regs *r, uint8_t v)
{
    v++;
    r->f = (r->f & CF) | (v == 0 ? ZF : 0) | ((v & 0xF) == 0 ? HF : 0);
    return v;
}

static uint8_t dec8(rlgbd_regs *r, uint8_t v)
{
    v--;
    r->f = (r->f & CF) | NF | (v == 0 ? ZF : 0) | ((v & 0xF) == 0xF ? HF : 0);
    return v;
}

static uint8_t *reg8(rlgbd_regs *r, int i)
{
    switch (i) {
    case 0: return &r->b;
    case 1: return &r->c;
    case 2: return &r->d;
    case 3: return &r->e;
    case 4: return &r->h;
    case 5: return &r->l;
    default: return &r->a;
    }
}

static int cond(const rlgbd_regs *r, int cc)
{
    switch (cc) {
    case 0: return !(r->f & ZF);
    case 1: return (r->f & ZF) != 0;
    case 2: return !(r->f & CF);
    default: return (r->f & CF) != 0;
    }
}

static void alu(rlgbd_regs *r, int op, uint8_t v)
{
    uint8_t a = r->a, c = (r->f & CF) ? 1 : 0;
    unsigned res;
    switch (op) {
    case 0:
        res = a + v;
        r->a = (uint8_t)res;
        r->f = (r->a == 0 ? ZF : 0) | (((a & 0xF) + (v & 0xF)) > 0xF ? HF : 0) | (res > 0xFF ? CF : 0);
        break;
    case 1:
        res = a + v + c;
        r->a = (uint8_t)res;
        r->f = (r->a == 0 ? ZF : 0) | (((a & 0xF) + (v & 0xF) + c) > 0xF ? HF : 0) | (res > 0xFF ? CF : 0);
        break;
    case 2:
        r->a = (uint8_t)(a - v);
        r->f = (r->a == 0 ? ZF : 0) | NF | ((a & 0xF) < (v & 0xF) ? HF : 0) | (a < v ? CF : 0);
        break;
    case 3:
        r->a = (uint8_t)(a - v - c);
        r->f = (r->a == 0 ? ZF : 0) | NF | ((a & 0xF) < ((v & 0xF) + c) ? HF : 0) | ((unsigned)a < (unsigned)v + c ? CF : 0);
        break;
    case 4:
        r->a = a & v;
        r->f = (r->a == 0 ? ZF : 0) | HF;
        break;
    case 5:
        r->a = a ^ v;
        r->f = (r->a == 0 ? ZF : 0);
        break;
    case 6:
        r->a = a | v;
        r->f = (r->a == 0 ? ZF : 0);
        break;
    default: {
        uint8_t t = (uint8_t)(a - v);
        r->f = (t == 0 ? ZF : 0) | NF | ((a & 0xF) < (v & 0xF) ? HF : 0) | (a < v ? CF : 0);
    } break;
    }
}

static uint16_t rr_get(rlgbd_regs *r, int i)
{
    switch (i) {
    case 0: return (uint16_t)(r->b << 8 | r->c);
    case 1: return DE;
    case 2: return HL;
    default: return r->sp;
    }
}

static void rr_set(rlgbd_regs *r, int i, uint16_t v)
{
    switch (i) {
    case 0: SET16(b, c, v); break;
    case 1: SET16(d, e, v); break;
    case 2: SET16(h, l, v); break;
    default: r->sp = v;
    }
}

/*
 * The body of one instruction after rlgbd_begin charged its fetch: the
 * explicit accesses in the hardware's order, the trailing internal cycles
 * left to rlgbd_end. Returns the instruction's dots, or -1 for a form the
 * library's own CPU runs.
 */
static int body(machine *m, rlgbd_regs *r, int op)
{
    uint16_t a16, t;
    uint8_t lo, hi, v;
    if (op >= 0x40 && op < 0x80 && op != 0x76) {
        int d = (op >> 3) & 7, s = op & 7;
        if (s == 6) {
            v = RD(HL);
            *reg8(r, d) = v;
            return 8;
        }
        if (d == 6) {
            WR(HL, *reg8(r, s));
            return 8;
        }
        *reg8(r, d) = *reg8(r, s);
        return 4;
    }
    if (op >= 0x80 && op < 0xC0) {
        int s = op & 7;
        if (s == 6) {
            alu(r, (op >> 3) & 7, RD(HL));
            return 8;
        }
        alu(r, (op >> 3) & 7, *reg8(r, s));
        return 4;
    }
    if (op < 0x40 && (op & 7) == 4) {
        int d = op >> 3;
        if (d == 6) {
            v = RD(HL);
            WR(HL, inc8(r, v));
            return 12;
        }
        *reg8(r, d) = inc8(r, *reg8(r, d));
        return 4;
    }
    if (op < 0x40 && (op & 7) == 5) {
        int d = op >> 3;
        if (d == 6) {
            v = RD(HL);
            WR(HL, dec8(r, v));
            return 12;
        }
        *reg8(r, d) = dec8(r, *reg8(r, d));
        return 4;
    }
    if (op < 0x40 && (op & 7) == 6) {
        int d = op >> 3;
        v = IMM();
        if (d == 6) {
            WR(HL, v);
            return 12;
        }
        *reg8(r, d) = v;
        return 8;
    }
    if (op < 0x40 && (op & 0xF) == 1) {
        lo = IMM();
        hi = IMM();
        rr_set(r, op >> 4, (uint16_t)(hi << 8 | lo));
        return 12;
    }
    if (op < 0x40 && (op & 0xF) == 3) {
        t = rr_get(r, op >> 4);
        rlgbd_idu(m->h, t);
        rr_set(r, op >> 4, t + 1);
        return 8;
    }
    if (op < 0x40 && (op & 0xF) == 0xB) {
        t = rr_get(r, op >> 4);
        rlgbd_idu(m->h, t);
        rr_set(r, op >> 4, t - 1);
        return 8;
    }
    if (op < 0x40 && (op & 0xF) == 9) {
        uint16_t hl = HL, o = rr_get(r, op >> 4);
        uint32_t res = (uint32_t)hl + o;
        SET16(h, l, res);
        r->f = (r->f & ZF) | (((hl & 0xFFF) + (o & 0xFFF)) > 0xFFF ? HF : 0) | (res > 0xFFFF ? CF : 0);
        return 8;
    }
    if (op >= 0xC0 && (op & 7) == 6) {
        alu(r, (op >> 3) & 7, IMM());
        return 8;
    }
    if (op >= 0xC0 && (op & 7) == 7) {
        rlgbd_internal(m->h);
        r->sp--;
        WR(r->sp, r->pc >> 8);
        r->sp--;
        WR(r->sp, r->pc & 0xFF);
        r->pc = (uint16_t)(op & 0x38);
        return 16;
    }
    switch (op) {
    case 0x00: return 4;
    case 0x02: WR((uint16_t)(r->b << 8 | r->c), r->a); return 8;
    case 0x12: WR(DE, r->a); return 8;
    case 0x0A: r->a = RD((uint16_t)(r->b << 8 | r->c)); return 8;
    case 0x1A: r->a = RD(DE); return 8;
    case 0x22: t = HL; WR(t, r->a); t++; SET16(h, l, t); return 8;
    case 0x32: t = HL; WR(t, r->a); t--; SET16(h, l, t); return 8;
    case 0x2A: t = HL; r->a = RD(t); t++; SET16(h, l, t); return 8;
    case 0x3A: t = HL; r->a = RD(t); t--; SET16(h, l, t); return 8;
    case 0x07: v = r->a >> 7; r->a = (uint8_t)(r->a << 1 | v); r->f = v ? CF : 0; return 4;
    case 0x0F: v = r->a & 1; r->a = (uint8_t)(r->a >> 1 | v << 7); r->f = v ? CF : 0; return 4;
    case 0x17: v = r->a >> 7; r->a = (uint8_t)(r->a << 1 | ((r->f & CF) ? 1 : 0)); r->f = v ? CF : 0; return 4;
    case 0x1F: v = r->a & 1; r->a = (uint8_t)(r->a >> 1 | ((r->f & CF) ? 0x80 : 0)); r->f = v ? CF : 0; return 4;
    case 0x2F: r->a = (uint8_t)~r->a; r->f |= NF | HF; return 4;
    case 0x37: r->f = (r->f & ZF) | CF; return 4;
    case 0x3F: r->f = (r->f & (ZF | CF)) ^ CF; return 4;
    case 0x18: {
        int8_t off = (int8_t)IMM();
        r->pc = (uint16_t)(r->pc + off);
        return 12;
    }
    case 0x20: case 0x28: case 0x30: case 0x38: {
        int8_t off = (int8_t)IMM();
        if (cond(r, (op >> 3) & 3)) {
            r->pc = (uint16_t)(r->pc + off);
            return 12;
        }
        return 8;
    }
    case 0xC3:
        lo = IMM();
        hi = IMM();
        r->pc = (uint16_t)(hi << 8 | lo);
        return 16;
    case 0xC2: case 0xCA: case 0xD2: case 0xDA:
        if (cond(r, (op >> 3) & 3)) {
            lo = IMM();
            hi = IMM();
            r->pc = (uint16_t)(hi << 8 | lo);
            return 16;
        }
        r->pc += 2;
        return 12;
    case 0xE9: r->pc = HL; return 4;
    case 0xCD: case 0xC4: case 0xCC: case 0xD4: case 0xDC:
        lo = IMM();
        hi = IMM();
        a16 = (uint16_t)(hi << 8 | lo);
        if (op != 0xCD && !cond(r, (op >> 3) & 3))
            return 12;
        rlgbd_internal(m->h);
        r->sp -= 2;
        WR(r->sp + 1, r->pc >> 8);
        WR(r->sp, r->pc & 0xFF);
        r->pc = a16;
        return 24;
    case 0xC9:
        lo = RD(r->sp);
        hi = RD(r->sp + 1);
        r->sp += 2;
        r->pc = (uint16_t)(hi << 8 | lo);
        return 16;
    case 0xC0: case 0xC8: case 0xD0: case 0xD8:
        if (!cond(r, (op >> 3) & 3))
            return 8;
        rlgbd_internal(m->h);
        lo = RD(r->sp);
        hi = RD(r->sp + 1);
        r->sp += 2;
        r->pc = (uint16_t)(hi << 8 | lo);
        return 20;
    case 0xC5: case 0xD5: case 0xE5: case 0xF5: {
        uint8_t h8, l8;
        switch (op) {
        case 0xC5: h8 = r->b; l8 = r->c; break;
        case 0xD5: h8 = r->d; l8 = r->e; break;
        case 0xE5: h8 = r->h; l8 = r->l; break;
        default: h8 = r->a; l8 = r->f;
        }
        rlgbd_idu(m->h, r->sp);
        rlgbd_internal(m->h);
        r->sp -= 2;
        WR(r->sp + 1, h8);
        WR(r->sp, l8);
        return 16;
    }
    case 0xC1: r->c = RD(r->sp); r->b = RD(r->sp + 1); r->sp += 2; return 12;
    case 0xD1: r->e = RD(r->sp); r->d = RD(r->sp + 1); r->sp += 2; return 12;
    case 0xE1: r->l = RD(r->sp); r->h = RD(r->sp + 1); r->sp += 2; return 12;
    case 0xF1: r->f = RD(r->sp) & 0xF0; r->a = RD(r->sp + 1); r->sp += 2; return 12;
    case 0xE0: v = IMM(); WR(0xFF00 | v, r->a); return 12;
    case 0xF0: v = IMM(); r->a = RD(0xFF00 | v); return 12;
    case 0xE2: WR(0xFF00 | r->c, r->a); return 8;
    case 0xF2: r->a = RD(0xFF00 | r->c); return 8;
    case 0xEA: lo = IMM(); hi = IMM(); WR(hi << 8 | lo, r->a); return 16;
    case 0xFA: lo = IMM(); hi = IMM(); r->a = RD(hi << 8 | lo); return 16;
    default: return -1;
    }
}

static void step_routed(machine *m)
{
    int32_t op = rlgbd_begin(m->h);
    if (op < 0) {
        m->boundaries++;
    } else {
        rlgbd_regs r;
        rlgbd_get_regs(m->h, &r);
        int total = body(m, &r, op);
        if (total >= 0) {
            rlgbd_set_regs(m->h, &r);
            rlgbd_end(m->h, (uint32_t)total);
            m->bodies++;
        } else {
            rlgbd_exec_native(m->h, (uint8_t)op);
            m->fallbacks++;
        }
    }
    rl_gbdev_sync(m->d, m->k);
}

static int open_routed(machine *m, rl_gb_model model, const uint8_t *rom, size_t len)
{
    memset(m, 0, sizeof *m);
    rl_gb_cart cart;
    if (rl_gb_cart_parse(rom, len, &cart) != 0)
        return -1;
    m->g = rl_gb_create(model, &cart, NULL);
    m->s = rl_sched_create(NULL);
    if (!m->g || !m->s)
        return -1;
    m->k = rl_sched_add_cpu(m->s, 1);
    m->d = rl_gbdev_create(m->g, m->s);
    if (!m->k || !m->d)
        return -1;
    m->h = rl_gbdev_handle(m->d);
    return 0;
}

static void close_routed(machine *m)
{
    rl_gbdev_destroy(m->d);
    rl_gb_destroy(m->g);
    rl_sched_destroy(m->s);
}

static int regs_eq(const rlgbd_regs *x, const rlgbd_regs *y) { return memcmp(x, y, sizeof *x) == 0; }

typedef struct pending {
    float *v;
    size_t n, cap;
} pending;

static int audio_pull(pending *p, rlgbd *h)
{
    static float x[2 * 1024];
    size_t n;
    while ((n = rlgbd_audio_take(h, x, 1024)) > 0) {
        if (p->n + 2 * n > p->cap) {
            p->cap = (p->n + 2 * n) * 2;
            p->v = realloc(p->v, p->cap * sizeof *p->v);
            if (!p->v)
                abort();
        }
        memcpy(p->v + p->n, x, 2 * n * sizeof *x);
        p->n += 2 * n;
    }
    return 1;
}

/** The two machines stand at the same dot, but a sample due at the very end may reach one side a call later: the streams must agree over their common prefix and differ in length by one frame at most. */
static int audio_eq(pending *a, rlgbd *ra, pending *b, rlgbd *rb)
{
    audio_pull(a, ra);
    audio_pull(b, rb);
    size_t k = a->n < b->n ? a->n : b->n;
    if (memcmp(a->v, b->v, k * sizeof *a->v) != 0)
        return 0;
    if ((a->n > b->n ? a->n - b->n : b->n - a->n) > 2)
        return 0;
    memmove(a->v, a->v + k, (a->n - k) * sizeof *a->v);
    memmove(b->v, b->v + k, (b->n - k) * sizeof *b->v);
    a->n -= k;
    b->n -= k;
    return 1;
}

static int lockstep(const char *path, rl_gb_model model, uint64_t frames)
{
    size_t n = 0;
    uint8_t *rom = load(path, &n);
    if (!rom) {
        printf("%s: cannot read\n", path);
        return 2;
    }
    machine m;
    uint32_t hw = (model == RL_GB_DMG) ? 0 : (model == RL_GB_CGBE) ? 2 : (model == RL_GB_AGB) ? 3 : (model == RL_GB_MGB) ? 4 : 1;
    rlgbd *ref = rlgbd_new(hw, rom, n);
    if (!ref || open_routed(&m, model, rom, n) != 0) {
        printf("%s: SKIP (not a supported cartridge)\n", path);
        rlgbd_free(ref);
        free(rom);
        return 0;
    }
    pending heard_ref = {0}, heard_m = {0};
    rlgbd_audio_enable(ref, 1);
    rlgbd_audio_enable(m.h, 1);
    uint64_t end = frames * RLGBD_FRAME_DOTS, next_frame = RLGBD_FRAME_DOTS, steps = 0, checked = 0;
    int bad = 0, frozen = 0;
    unsigned idle = 0;
    while (rlgbd_now(ref) < end && !bad) {
        uint64_t before = rlgbd_now(ref);
        rlgbd_step_native(ref);
        idle = rlgbd_now(ref) == before ? idle + 1 : 0;
        step_routed(&m);
        steps++;
        rlgbd_regs ra, rb;
        rlgbd_get_regs(ref, &ra);
        rlgbd_get_regs(m.h, &rb);
        if (!regs_eq(&ra, &rb) || rlgbd_now(ref) != rlgbd_now(m.h) || rl_sched_cycles(m.k) != rlgbd_now(m.h)) {
            printf("%s: DIVERGED at step %llu: pc %04x/%04x a %02x/%02x f %02x/%02x sp %04x/%04x dots %llu/%llu\n", path,
                   (unsigned long long)steps, ra.pc, rb.pc, ra.a, rb.a, ra.f, rb.f, ra.sp, rb.sp,
                   (unsigned long long)rlgbd_now(ref), (unsigned long long)rlgbd_now(m.h));
            bad = 1;
        } else if (rlgbd_now(ref) >= next_frame) {
            next_frame += RLGBD_FRAME_DOTS;
            checked++;
            if (!audio_eq(&heard_ref, ref, &heard_m, m.h)) {
                printf("%s: SAMPLES DIFFER at frame %llu\n", path, (unsigned long long)checked);
                bad = 1;
            } else if (rlgbd_present_hash(ref) != rlgbd_present_hash(m.h) || rlgbd_state_hash(ref) != rlgbd_state_hash(m.h)) {
                printf("%s: DEVICE STATE DIFFERS at frame %llu\n", path, (unsigned long long)checked);
                bad = 1;
            }
        }
        if (idle >= 16) {
            frozen = 1;
            break;
        }
    }
    if (!bad && (rlgbd_present_hash(ref) != rlgbd_present_hash(m.h) || rlgbd_state_hash(ref) != rlgbd_state_hash(m.h))) {
        printf("%s: DEVICE STATE DIFFERS at the end\n", path);
        bad = 1;
    }
    if (bad) {
        close_routed(&m);
        rlgbd_free(ref);
        free(heard_ref.v);
        free(heard_m.v);
        free(rom);
        return 1;
    }
    if (frozen) {
        printf("%s: PASS %llu steps, %llu frames compared, then STOP froze the clock (a joypad press wakes it; not driven here)\n", path,
               (unsigned long long)steps, (unsigned long long)checked);
        close_routed(&m);
        rlgbd_free(ref);
        free(heard_ref.v);
        free(heard_m.v);
        free(rom);
        return 0;
    }
    double t0 = seconds();
    while (rlgbd_now(ref) < 2 * end)
        rlgbd_step_native(ref);
    double t_native = seconds() - t0;
    t0 = seconds();
    while (rlgbd_now(m.h) < 2 * end)
        step_routed(&m);
    double t_routed = seconds() - t0;
    printf("%s: PASS %llu steps, %llu frames compared | bodies %llu, library CPU %llu, boundary %llu | per frame: native %.1f us, routed %.1f us (x%.2f)\n",
           path, (unsigned long long)steps, (unsigned long long)checked, (unsigned long long)m.bodies, (unsigned long long)m.fallbacks,
           (unsigned long long)m.boundaries, t_native / (double)frames * 1e6, t_routed / (double)frames * 1e6, t_routed / t_native);
    close_routed(&m);
    rlgbd_free(ref);
    free(heard_ref.v);
    free(heard_m.v);
    free(rom);
    return 0;
}

int main(int argc, char **argv)
{
    rl_gb_model model = RL_GB_CGB;
    uint64_t frames = 60;
    int failed = 0, ran = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--model") && i + 1 < argc) {
            const char *v = argv[++i];
            model = !strcmp(v, "dmg") ? RL_GB_DMG : !strcmp(v, "cgbe") ? RL_GB_CGBE : !strcmp(v, "agb") ? RL_GB_AGB : !strcmp(v, "mgb") ? RL_GB_MGB : RL_GB_CGB;
        } else if (!strcmp(argv[i], "--frames") && i + 1 < argc) {
            frames = strtoull(argv[++i], NULL, 10);
        } else {
            failed += lockstep(argv[i], model, frames) != 0;
            ran++;
        }
    }
    printf("%d ROMs, %d failed\n", ran, failed);
    return failed ? 1 : 0;
}
