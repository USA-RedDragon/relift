/**
 * @file test_bus_console.c
 * @brief Bus timing against the console: every record of relift's hardware
 * probe (console_timing.h) replayed as the ARM7TDMI's bus activity through
 * rl_gba_fetch, rl_gba_load, rl_gba_store and rl_gba_idle, from the probe's
 * WAITCNT write to its reads of the stopped timers. Each instruction starts
 * with the fetch two instructions ahead; a fetch is nonsequential after a
 * data access, an internal cycle or a branch, whose reload fetches the
 * target and the next instruction. The count comes from the timer devices.
 */
#include "console_timing.h"
#include "mach.h"

#define WAITCNT 0x04000204u
#define TM0 0x04000100u
#define TM1 0x04000104u
#define PROBE_SP 0x03007F00u
#define PROBE_KERNS 0x08001F1Cu
#define TIME_KERNEL 0x030008D8u
#define TIME_RETURN 0x03000928u
#define AGREE_FLOOR 365u

typedef enum kop { K_NONE, K_PLAIN, K_IDLE, K_MUL, K_MLA, K_UMULL, K_SMULL, K_BRANCH, K_LOAD, K_STORE, K_LDM, K_STM, K_SWP } kop;

typedef struct kern {
    uint32_t addr;
    uint8_t thumb;
    uint8_t op;
    uint8_t n;
    uint32_t param;
} kern;

static const kern kerns[] = {
    {0x08000180u, 0, K_NONE, 0, 0},           {0x080001A0u, 0, K_PLAIN, 0, 0},
    {0x08000200u, 0, K_PLAIN, 0, 0},          {0x08000260u, 0, K_IDLE, 1, 0},
    {0x080002C0u, 0, K_MUL, 0, 0x00000001u},  {0x08000320u, 0, K_MUL, 0, 0x00000100u},
    {0x08000380u, 0, K_MUL, 0, 0x00010000u},  {0x080003E0u, 0, K_MUL, 0, 0x01000000u},
    {0x08000440u, 0, K_MLA, 0, 0x00000001u},  {0x080004A0u, 0, K_MLA, 0, 0x01000000u},
    {0x08000500u, 0, K_UMULL, 0, 0x00000001u}, {0x08000560u, 0, K_UMULL, 0, 0x01000000u},
    {0x080005C0u, 0, K_SMULL, 0, 0xFFFFFFFFu}, {0x08000620u, 0, K_SMULL, 0, 0x01000000u},
    {0x08000680u, 0, K_BRANCH, 0, 0},         {0x080006E0u, 0, K_PLAIN, 0, 0},
    {0x08000740u, 0, K_LOAD, 4, 0},           {0x080007A0u, 0, K_LOAD, 1, 0},
    {0x08000800u, 0, K_LOAD, 2, 0},           {0x08000860u, 0, K_LOAD, 1, 0},
    {0x080008C0u, 0, K_STORE, 4, 0},          {0x08000920u, 0, K_STORE, 1, 0},
    {0x08000980u, 0, K_STORE, 2, 0},          {0x080009E0u, 0, K_LDM, 1, 0},
    {0x08000A40u, 0, K_LDM, 4, 0},            {0x08000AA0u, 0, K_STM, 4, 0},
    {0x08000B00u, 0, K_SWP, 4, 0},            {0x08000B60u, 1, K_NONE, 0, 0},
    {0x08000B80u, 1, K_PLAIN, 0, 0},          {0x08000BC0u, 1, K_PLAIN, 0, 0},
    {0x08000C00u, 1, K_PLAIN, 0, 0},          {0x08000C40u, 1, K_BRANCH, 0, 0},
    {0x08000C80u, 1, K_PLAIN, 0, 0},          {0x08000CC0u, 1, K_LOAD, 4, 0},
    {0x08000D00u, 1, K_LOAD, 1, 0},           {0x08000D40u, 1, K_LOAD, 2, 0},
    {0x08000D80u, 1, K_STORE, 4, 0},          {0x08000DC0u, 1, K_STORE, 1, 0},
    {0x08000E00u, 1, K_STORE, 2, 0},
};

static const uint32_t code_base[] = {0, 0x03006880u, 0x02000C60u, 0x06015000u};
static const uint32_t data_base[] = {0x08000400u, 0x03006830u, 0x02000C10u, 0x06003000u,
                                     0x05000200u, 0x07000000u, 0x0E007FC0u};

typedef struct cpu {
    rl_gba *g;
    uint32_t pc;
    unsigned size;
    bool ns;
} cpu;

static void fetch(cpu *c)
{
    rl_gba_fetch(c->g, c->pc + 2 * c->size, c->size, !c->ns);
    c->ns = false;
}

static void plain(cpu *c, unsigned n)
{
    for (; n; n--) {
        fetch(c);
        c->pc += c->size;
    }
}

static void idle(cpu *c, unsigned n)
{
    for (; n; n--)
        rl_gba_idle(c->g, 1);
    c->ns = true;
}

static uint32_t load(cpu *c, uint32_t addr, unsigned size, unsigned words)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < words; i++)
        v = rl_gba_load(c->g, addr + 4 * i, size, i != 0);
    c->ns = true;
    return v;
}

static void store(cpu *c, uint32_t addr, unsigned size, uint32_t value, unsigned words)
{
    for (unsigned i = 0; i < words; i++)
        rl_gba_store(c->g, addr + 4 * i, size, value, i != 0);
    c->ns = true;
}

static void jump(cpu *c, uint32_t target)
{
    c->size = (target & 1) ? 2 : 4;
    c->pc = target & ~1u;
    rl_gba_fetch(c->g, c->pc, c->size, false);
    rl_gba_fetch(c->g, c->pc + c->size, c->size, true);
    c->ns = false;
}

static unsigned mul_cycles(uint32_t m, bool sign)
{
    unsigned n = 1;
    for (unsigned k = 8; k < 32; k += 8) {
        uint32_t hi = m >> k;
        if (hi && (!sign || hi != 0xFFFFFFFFu >> k))
            n++;
    }
    return n;
}

static void body(cpu *c, const kern *k, uint32_t data)
{
    fetch(c);
    switch (k->op) {
    case K_IDLE:
        idle(c, k->n);
        break;
    case K_MUL:
        idle(c, mul_cycles(k->param, true));
        break;
    case K_MLA:
        idle(c, 1 + mul_cycles(k->param, true));
        break;
    case K_UMULL:
        idle(c, 1 + mul_cycles(k->param, false));
        break;
    case K_SMULL:
        idle(c, 1 + mul_cycles(k->param, true));
        break;
    case K_BRANCH:
        jump(c, (c->pc + c->size) | (c->size == 2));
        return;
    case K_LOAD:
        load(c, data, k->n, 1);
        idle(c, 1);
        break;
    case K_STORE:
        store(c, data, k->n, 0, 1);
        break;
    case K_LDM:
        load(c, data, 4, k->n);
        idle(c, 1);
        break;
    case K_STM:
        store(c, data, 4, 0, k->n);
        break;
    case K_SWP:
        load(c, data, 4, 1);
        store(c, data, 4, 0, 1);
        idle(c, 1);
        break;
    default:
        break;
    }
    c->pc += c->size;
}

static void run_kernel(cpu *c, const kern *k, uint32_t entry, uint32_t data)
{
    for (unsigned iter = 64; iter; iter--) {
        for (unsigned i = 0; i < 16 && k->op != K_NONE; i++)
            body(c, k, data);
        plain(c, 1);
        fetch(c);
        if (iter > 1)
            jump(c, entry);
        else
            c->pc += c->size;
    }
    fetch(c);
    jump(c, TIME_RETURN);
}

static uint32_t replay(const ct_cfg *cf, unsigned kid)
{
    const kern *k = &kerns[kid];
    mach m = mach_new();
    rl_gba_write(m.g, WAITCNT, 2, 3);
    unsigned nn = cf->n == 4 ? 0 : cf->n == 3 ? 1 : cf->n == 2 ? 2 : 3;
    uint32_t wc = 3u | nn << 2 | (uint32_t)(cf->s == 1) << 4 | (uint32_t)cf->pf << 14;
    uint32_t entry = (cf->code == CT_ROM ? k->addr : code_base[cf->code]) | k->thumb;
    uint32_t data = data_base[cf->data];
    cpu c = {m.g, 0x03006254u, 4, false};

    fetch(&c);
    store(&c, WAITCNT, 2, wc, 1);
    c.pc += 4;
    fetch(&c);
    load(&c, PROBE_KERNS + 16 * kid + 8, 1, 1);
    idle(&c, 1);
    c.pc += 4;
    fetch(&c);
    load(&c, PROBE_KERNS + 16 * kid + 12, 4, 1);
    idle(&c, 1);
    c.pc += 4;
    plain(&c, 2);
    fetch(&c);
    load(&c, PROBE_SP + 8, 4, 1);
    idle(&c, 1);
    c.pc += 4;
    fetch(&c);
    jump(&c, TIME_KERNEL);

    fetch(&c);
    store(&c, PROBE_SP - 36, 4, 0, 9);
    c.pc += 4;
    plain(&c, 10);
    fetch(&c);
    store(&c, TM0, 4, 0, 1);
    c.pc += 4;
    fetch(&c);
    store(&c, TM1, 4, 0, 1);
    c.pc += 4;
    plain(&c, 3);
    fetch(&c);
    store(&c, TM1, 4, 0x00840000u, 1);
    c.pc += 4;
    fetch(&c);
    store(&c, TM0, 4, 0x00800000u, 1);
    c.pc += 4;
    plain(&c, 1);
    fetch(&c);
    jump(&c, entry);

    run_kernel(&c, k, entry, data);

    fetch(&c);
    store(&c, TM0, 4, 0, 1);
    c.pc += 4;
    fetch(&c);
    uint32_t lo = load(&c, TM0, 4, 1);
    idle(&c, 1);
    c.pc += 4;
    plain(&c, 2);
    fetch(&c);
    uint32_t hi = load(&c, TM1, 4, 1);
    idle(&c, 1);
    mach_free(&m);
    return (lo & 0xFFFFu) | hi << 16;
}

TEST(console_probe_timing_records)
{
    unsigned n = sizeof ct_recs / sizeof ct_recs[0];
    unsigned ncfg = sizeof ct_cfgs / sizeof ct_cfgs[0];
    unsigned agree = 0;
    for (unsigned ci = 0; ci < ncfg; ci++) {
        unsigned total = 0, same = 0;
        for (unsigned i = 0; i < n; i++) {
            const ct_rec *r = &ct_recs[i];
            if (r->cfg != ci)
                continue;
            uint32_t got = replay(&ct_cfgs[ci], r->kernel);
            total++;
            if (got == r->cycles)
                same++;
            else
                printf("  cfg %u kernel %u: model %u, console %u\n", ci, r->kernel, (unsigned)got, (unsigned)r->cycles);
        }
        printf("  cfg %u: %u of %u agree\n", ci, same, total);
        agree += same;
    }
    printf("  console timing: %u of %u records agree\n", agree, n);
    CHECK(agree >= AGREE_FLOOR);
}
