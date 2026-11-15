/**
 * @file swi.c
 * @brief The SWI dispatcher and its stack frames, the bus accesses, the IRQ
 * stub, the post-boot state and the small calls (resets, halts, checksum).
 */
#include "bios_int.h"

#include <setjmp.h>
#include <stdlib.h>

/**
 * @brief One data access of a call on a timed bus, made again on the
 * machine's clock at its place in the call (bios_flush): at, the cycle of
 * the call it starts at; wait, the cycles the call counted for it.
 */
typedef struct bios_access {
    uint64_t at;
    uint32_t addr, old, value;
    uint8_t size, wait;
    bool store, seq, hidden;
} bios_access;

static _Thread_local bios_access *accesses;
static _Thread_local size_t access_cap;

static unsigned access_cycles(bios_call *c, uint32_t a, unsigned size, bool seq)
{
    if (c->quiet)
        return 0;
    unsigned n = c->bus->cycles ? c->bus->cycles(c->bus->user, a, size, seq) : 1u;
    c->bus_cyc += n;
    return n;
}

static uint32_t bus_addr(uint32_t a, unsigned size) { return a >= 0x0E000000u && a < 0x10000000u ? a : a & ~(size - 1u); }

static bool in_ram(uint32_t a) { return a >= 0x02000000u && a < 0x08000000u && (a < 0x04000000u || a >= 0x05000000u); }

static uint64_t model_now(const bios_call *c);

static void note(bios_call *c, uint32_t a, unsigned size, unsigned wait, bool seq, bool store, uint32_t v)
{
    if (!c->timed || c->quiet || c->nacc == SIZE_MAX)
        return;
    if (c->nacc == access_cap) {
        size_t cap = access_cap ? access_cap * 2 : 4096;
        bios_access *p = realloc(accesses, cap * sizeof *p);
        if (!p) {
            c->nacc = SIZE_MAX;
            return;
        }
        accesses = p;
        access_cap = cap;
    }
    bool hidden = store && c->model && in_ram(a);
    accesses[c->nacc++] = (bios_access){
        .at = c->model ? model_now(c) : c->cyc,
        .addr = a,
        .old = hidden ? c->bus->read(c->bus->user, a, size) : 0,
        .value = v,
        .size = (uint8_t)size,
        .wait = (uint8_t)wait,
        .store = store,
        .seq = seq,
        .hidden = hidden,
    };
}

static uint32_t rd(bios_call *c, uint32_t a, unsigned size, bool seq)
{
    a = bus_addr(a, size);
    unsigned n = access_cycles(c, a & ~(size - 1u), size, seq);
    note(c, a, size, n, seq, false, 0);
    c->cyc += n;
    return c->bus->read(c->bus->user, a, size);
}

static void wr(bios_call *c, uint32_t a, unsigned size, uint32_t v, bool seq)
{
    a = bus_addr(a, size);
    unsigned n = access_cycles(c, a & ~(size - 1u), size, seq);
    note(c, a, size, n, seq, true, v);
    c->cyc += n;
    c->bus->write(c->bus->user, a, size, v);
}

static bool bus_timed(const rl_gba_bus *bus) { return bus->load && bus->store && bus->idle && bus->point; }

void bios_tick(bios_call *c, unsigned n) { c->cyc += n; }

void bios_point(bios_call *c, uint32_t pc)
{
    if (!c->timed || (c->psr & 0x80u))
        return;
    uint32_t regs[16];
    for (unsigned i = 0; i < 4; i++)
        regs[i] = c->r[i];
    for (unsigned i = 4; i < 13; i++)
        regs[i] = c->hi[i];
    regs[13] = c->sp;
    regs[14] = c->lr;
    regs[15] = c->sp_svc;
    for (;;) {
        int r = c->bus->point(c->bus->user, pc + 4u, c->psr, regs);
        if (r == RL_GBA_POINT_LEFT && c->out)
            longjmp(*c->out, 1);
        if (r != RL_GBA_POINT_TAKEN)
            return;
        c->irqs++;
    }
}

/**
 * @brief The call's accesses due by now, on the machine's clock: a store to
 * work or video memory that the slide hid is made there (a handler taken
 * earlier saw the memory without it); I/O and the backup were accessed when
 * the routine ran, so only their cycles pass here.
 * Each takes the time the bus gives it (DMA and picture unit included), and
 * the call counts the cycles it had counted for it.
 */
static void replay_due(bios_call *c)
{
    if (c->nacc == SIZE_MAX)
        return;
    for (; c->done < c->nacc && accesses[c->done].at <= c->spent; c->done++) {
        const bios_access *x = &accesses[c->done];
        if (x->addr < 0x02000000u || (x->addr >= 0x04000000u && x->addr < 0x05000000u) || x->addr >= 0x0E000000u)
            c->bus->idle(c->bus->user, x->wait);
        else if (x->store)
            c->bus->store(c->bus->user, x->addr, x->size, x->hidden ? x->value : c->bus->read(c->bus->user, x->addr, x->size), x->seq);
        else
            (void)c->bus->load(c->bus->user, x->addr, x->size, x->seq);
        c->spent += x->wait;
    }
}

static uint64_t next_due(const bios_call *c)
{
    return c->nacc != SIZE_MAX && c->done < c->nacc ? accesses[c->done].at : UINT64_MAX;
}

void bios_flush(bios_call *c, bool open)
{
    if (!c->timed) {
        c->spent = c->cyc;
        return;
    }
    replay_due(c);
    while (c->spent < c->cyc) {
        uint64_t n = c->cyc - c->spent, due = next_due(c);
        bool masked = !open || (c->psr & 0x80u);
        if (!masked)
            n = 1;
        else if (due > c->spent && due - c->spent < n)
            n = due - c->spent;
        if (!masked)
            bios_point(c, RL_GBA_BIOS_SLIDE);
        c->spent += n;
        c->bus->idle(c->bus->user, (unsigned)n);
        replay_due(c);
    }
}

/**
 * @brief Moves the call's accesses still to come so that they all end by
 * cycle end of the call (the model's places are estimates; the call's cycles
 * are the fitted total).
 */
static void fit_accesses(bios_call *c, uint64_t end)
{
    if (!c->timed || c->nacc == SIZE_MAX)
        return;
    for (size_t i = c->nacc; i-- > c->done;) {
        end = end > accesses[i].wait ? end - accesses[i].wait : 0;
        if (accesses[i].at > end)
            accesses[i].at = end;
        end = accesses[i].at;
    }
}

/** @brief The rest of the call's accesses, at its end. */
static void replay_rest(bios_call *c)
{
    if (!c->timed)
        return;
    c->spent = UINT64_MAX - 1;
    replay_due(c);
}

void bios_halt(bios_call *c, uint32_t value)
{
    bios_flush(c, true);
    uint32_t a = 0x04000301u;
    c->cyc += access_cycles(c, a, 1, false);
    c->spent = c->cyc;
    if (c->timed)
        c->bus->store(c->bus->user, a, 1, value, false);
    else
        c->bus->write(c->bus->user, a, 1, value);
}

void bios_set_flags(bios_call *c, uint32_t nzcv) { c->psr = (c->psr & 0x0FFFFFFFu) | (nzcv & 0xF0000000u); }

uint32_t bios_nzcv_sub(uint32_t a, uint32_t b)
{
    uint32_t r = a - b;
    return (r & 0x80000000u) | (r ? 0 : 0x40000000u) | (a >= b ? 0x20000000u : 0) | ((((a ^ b) & (a ^ r)) >> 31) << 28);
}

uint32_t bios_nzcv_add(uint32_t a, uint32_t b)
{
    uint32_t r = a + b;
    return (r & 0x80000000u) | (r ? 0 : 0x40000000u) | (r < a ? 0x20000000u : 0) | (((~(a ^ b) & (a ^ r)) >> 31) << 28);
}

uint32_t bios_nzcv_logic(uint32_t r, uint32_t carry_in) { return (r & 0x80000000u) | (r ? 0 : 0x40000000u) | (carry_in & 0x20000000u); }

static uint32_t ror(uint32_t v, unsigned n) { return n ? (v >> n) | (v << (32 - n)) : v; }

uint32_t bios_ld32(bios_call *c, uint32_t a, bool seq) { return ror(rd(c, a, 4, seq), (a & 3u) * 8); }

uint32_t bios_ldm(bios_call *c, uint32_t a, bool seq) { return rd(c, a, 4, seq); }

uint32_t bios_ld16(bios_call *c, uint32_t a, bool seq) { return ror(rd(c, a, 2, seq) & 0xFFFFu, (a & 1u) * 8); }

uint32_t bios_ld16s(bios_call *c, uint32_t a, bool seq)
{
    uint32_t v = rd(c, a, 2, seq) & 0xFFFFu;
    if (a & 1u)
        return (uint32_t)(int32_t)(int8_t)(v >> 8);
    return (uint32_t)(int32_t)(int16_t)v;
}

uint32_t bios_ld8(bios_call *c, uint32_t a, bool seq) { return rd(c, a, 1, seq) & 0xFFu; }

void bios_st32(bios_call *c, uint32_t a, uint32_t v, bool seq) { wr(c, a, 4, v, seq); }

void bios_st16(bios_call *c, uint32_t a, uint32_t v, bool seq) { wr(c, a, 2, v & 0xFFFFu, seq); }

void bios_st8(bios_call *c, uint32_t a, uint32_t v, bool seq) { wr(c, a, 1, v & 0xFFu, seq); }

void bios_push(bios_call *c, unsigned n, const uint32_t *v)
{
    c->sp -= 4 * n;
    for (unsigned i = 0; i < n; i++)
        bios_st32(c, c->sp + 4 * i, v[i], i != 0);
}

void bios_save(bios_call *c, unsigned first, unsigned last, bool lr)
{
    uint32_t v[10];
    unsigned n = 0;
    for (unsigned i = first; i <= last; i++)
        v[n++] = c->hi[i];
    if (lr)
        v[n++] = RL_GBA_BIOS_SWI_RETURN;
    bios_push(c, n, v);
}

void rl_gba_bios_init(rl_gba_bios *b)
{
    b->open_bus = RL_GBA_BIOS_OPEN_BOOT;
    b->cycles = 0;
}

rl_gba_boot rl_gba_bios_boot_state(void)
{
    return (rl_gba_boot){
        .pc = 0x08000000u,
        .cpsr = 0x0000001Fu,
        .sp_sys = 0x03007F00u,
        .sp_irq = 0x03007FA0u,
        .sp_svc = 0x03007FE0u,
    };
}

void rl_gba_bios_boot_io(rl_gba_bios *b, const rl_gba_bus *bus)
{
    bios_call c = {.b = b, .bus = bus};
    static const uint16_t regs[][2] = {
        {0x0000, 0x0080}, {0x0020, 0x0100}, {0x0026, 0x0100}, {0x0030, 0x0100}, {0x0036, 0x0100},
        {0x0082, 0x880E}, {0x0088, 0x0200}, {0x0134, 0x8000},
    };
    for (unsigned i = 0; i < sizeof regs / sizeof regs[0]; i++)
        bios_st16(&c, 0x04000000u + regs[i][0], regs[i][1], false);
    bios_st8(&c, 0x04000300u, 1, false);
    b->open_bus = RL_GBA_BIOS_OPEN_BOOT;
}

uint32_t rl_gba_bios_read(const rl_gba_bios *b, uint32_t addr, unsigned size)
{
    uint32_t v = b->open_bus >> ((addr & 3u) * 8);
    return size == 4 ? b->open_bus : size == 2 ? v & 0xFFFFu : v & 0xFFu;
}

/**
 * @brief Halt, Stop and CustomHalt: HALTCNT is written with 0, 0x80 or r2;
 * an interrupt that ends the halt is taken inside the call, at relift's slide
 * (bios.h), before the return.
 */
static void halt_write(bios_call *c, unsigned number)
{
    if (number == 0x02)
        c->r[2] = 0;
    else if (number == 0x03)
        c->r[2] = 0x80;
    c->hi[12] = 0x04000000u;
    bios_tick(c, number == 0x02 ? BIOS_HALT_CYCLES + 4 : number == 0x03 ? BIOS_HALT_CYCLES + 1 : BIOS_HALT_CYCLES);
    bios_halt(c, c->r[2]);
    bios_tick(c, BIOS_HALT_WAKE_MASKED);
    bios_flush(c, false);
    bios_tick(c, BIOS_HALT_WAKE - BIOS_HALT_WAKE_MASKED);
}

static void checksum(bios_call *c)
{
    c->r[0] = 0xBAAE187Fu;
    c->r[1] = 1;
    c->r[3] = 0x4000;
}

static void soft_reset(bios_call *c, rl_gba_swi_cpu *cpu)
{
    uint32_t flag = bios_ld8(c, 0x03007FFAu, false);
    for (uint32_t a = 0x03007E00u; a < 0x03008000u; a += 4)
        bios_st32(c, a, 0, a != 0x03007E00u);
    bios_tick(c, 0x80 * 3 + 40);
    cpu->ret = flag ? 0x02000000u : 0x08000000u;
    for (unsigned i = 0; i < 4; i++)
        c->r[i] = 0;
    c->status = RL_GBA_SWI_RESET;
}

static void multiboot(bios_call *c)
{
    c->r[0] = 1;
    bios_tick(c, 20);
}

typedef struct bios_model {
    bool fitted;
    int32_t base;
    int32_t k[BIOS_EVENTS];
} bios_model;

static const bios_model models[0x20] = {
    [0x01] = {true, 165, {[BE_RESET_PALETTE] = 64, [BE_RESET_VRAM] = 64, [BE_RESET_OAM] = 64, [BE_RESET_SIO] = 154,
                          [BE_RESET_SOUND] = 203, [BE_RESET_IO] = 238, [BE_RESET_BLOCK] = 5}},
    [0x06] = {true, 50, {[BE_SHIFT_STEP] = 5, [BE_SUB_STEP] = 8}},
    [0x07] = {true, 53, {[BE_SHIFT_STEP] = 5, [BE_SUB_STEP] = 8}},
    [0x08] = {true, 53, {[BE_SQRT_SHIFT] = 6, [BE_SQRT_ROUND] = 6, [BE_SHIFT_STEP] = 5, [BE_SUB_STEP] = 8}},
    [0x09] = {true, 77, {[BE_MUL] = 1}},
    [0x0A] = {true, 0, {62, 65, 67, 69, 143, 145, 147, 146, 148, 150, 146, 146, [BE_SHIFT_STEP] = 5, [BE_SUB_STEP] = 8,
                        [BE_MUL] = 1}},
    [0x0B] = {true, 0, {[BE_REFUSED] = 75, [BE_ZERO] = 73, [BE_SET_W_CALL] = 85, [BE_SET_H_CALL] = 87, [BE_SET_H_FILL] = 7,
                        [BE_SET_W_COPY] = 8, [BE_SET_W_FILL] = 6, [BE_SET_H_COPY] = 9}},
    [0x0C] = {true, 0, {[BE_REFUSED] = 72, [BE_ZERO] = 70, [BE_FAST_CALL] = 79, [BE_FAST_COPY] = 7, [BE_FAST_FILL] = 5,
                        [BE_FAST_FILL_START] = 9}},
    [0x0D] = {true, 41004, {0}},
    [0x0E] = {true, 59, {[BE_AFFINE_ITER] = 56, [BE_MUL] = 1}},
    [0x0F] = {true, 55, {[BE_AFFINE_ITER] = 37, [BE_MUL] = 1}},
    [0x10] = {true, 99, {[BE_BIT_BYTE] = 12, [BE_BIT_UNIT] = 22, [BE_BIT_OFFSET] = 2, [BE_BIT_WORD] = 1, [BE_ZERO] = -37}},
    [0x11] = {true, 69, {[BE_REFUSED] = 1, [BE_ZERO] = -1, [BE_LZ_FLAG] = 9, [BE_LZ_LITERAL] = 16, [BE_LZ_REF] = 25,
                         [BE_LZ_REF_BYTE] = 7}},
    [0x12] = {true, 75, {[BE_REFUSED] = 1, [BE_LZ_FLAG] = 9, [BE_LZ_LITERAL] = 20, [BE_LZ_REF] = 28, [BE_LZ_REF_BYTE] = 21}},
    [0x13] = {true, 94, {[BE_HUFF_WORD] = 9, [BE_HUFF_BIT] = 28, [BE_HUFF_LEAF] = 14}},
    [0x14] = {true, 81, {[BE_ZERO] = -4, [BE_RL_COMP_RUN] = 17, [BE_RL_COMP_BYTE] = 6, [BE_RL_RAW_RUN] = 12,
                         [BE_RL_RAW_BYTE] = 9}},
    [0x15] = {true, 84, {[BE_ZERO] = -4, [BE_RL_COMP_RUN] = 27, [BE_RL_RAW_RUN] = 20, [BE_RL_ANY_RUN] = -1,
                         [BE_PUT_EVEN] = 15, [BE_PUT_ODD] = 16}},
    [0x16] = {true, 71, {[BE_DIFF_ITEM] = 11}},
    [0x17] = {true, 70, {[BE_PUT_EVEN] = 16, [BE_PUT_ODD] = 20}},
    [0x18] = {true, 71, {[BE_ZERO] = 2, [BE_DIFF_ITEM] = 11}},
    [0x1F] = {true, 123, {[BE_MIDI_CLAMP] = 2, [BE_MIDI_MUL_FP] = 1, [BE_MIDI_MUL_R1] = 1}},
};

static int64_t events_cost(const bios_model *m, const bios_call *c)
{
    int64_t v = 0;
    for (unsigned i = 0; i < BIOS_EVENTS; i++)
        v += (int64_t)m->k[i] * c->ev[i];
    return v;
}

static uint64_t charged(unsigned number, const bios_call *c)
{
    if (number >= 0x20 || !models[number].fitted)
        return c->cyc;
    const bios_model *m = &models[number];
    return c->bus_cyc + (uint64_t)(m->base + events_cost(m, c)) - BIOS_SITE_FIT;
}

static uint64_t model_now(const bios_call *c) { return c->bus_cyc + (uint64_t)events_cost(c->model, c); }

static void hide(bios_call *c)
{
    if (c->nacc == SIZE_MAX)
        return;
    for (size_t i = c->nacc; i-- > 0;)
        if (accesses[i].hidden)
            c->bus->write(c->bus->user, accesses[i].addr, accesses[i].size, accesses[i].old);
}


/**
 * @brief The dispatcher's read of the SWI's comment byte, at the caller's
 * wait states: the byte at LR - 2 in either state (bits 16 to 23 of an ARM
 * SWI, hence GBATEK's SWI NN0000h in ARM code).
 */
static void read_comment(bios_call *c, uint32_t lr) { (void)rd(c, lr - 2u, 1, false); }

/**
 * @brief The return's reload of the caller's code at ret (the exception
 * return is a branch: a nonsequential and a sequential fetch there, in the
 * caller's region); gives the cycles the wait table counts for it.
 */
static uint64_t return_refill(bios_call *c, uint32_t ret, bool thumb)
{
    unsigned size = thumb ? 2u : 4u;
    uint32_t a = ret & ~(size - 1u);
    uint64_t n = 0;
    for (unsigned i = 0; i < 2; i++) {
        n += c->bus->cycles ? c->bus->cycles(c->bus->user, a + i * size, size, i != 0) : 1u;
        if (c->timed && c->bus->fetch)
            c->bus->fetch(c->bus->user, a + i * size, size, i != 0);
    }
    return n;
}

rl_gba_swi_status rl_gba_swi(rl_gba_bios *b, const rl_gba_bus *bus, unsigned number, rl_gba_swi_cpu *cpu)
{
    jmp_buf out;
    bios_call c = {.b = b, .bus = bus, .sp = cpu->sp_sys, .sp_svc = cpu->sp_svc, .status = RL_GBA_SWI_OK, .timed = bus_timed(bus), .out = &out};
    for (unsigned i = 0; i < 4; i++)
        c.r[i] = cpu->r[i];
    uint32_t r2 = cpu->r[2];
    for (unsigned i = 4; i < 13; i++)
        c.hi[i] = cpu->r[i];
    c.hi[11] = (cpu->cpsr & 0x80u) | 0x1Fu;
    c.lr = cpu->lr_sys;
    c.psr = 0x80u | 0x13u;
    if (setjmp(out)) {
        b->open_bus = RL_GBA_BIOS_OPEN_SWI;
        b->bus_cycles = c.bus_cyc;
        b->total = c.cyc;
        b->cycles = 0;
        return RL_GBA_SWI_LEFT;
    }
    uint32_t svc[4] = {cpu->cpsr, cpu->r[11], cpu->r[12], cpu->ret};
    for (unsigned i = 0; i < 4; i++)
        bios_st32(&c, cpu->sp_svc - 16 + 4 * i, svc[i], i != 0);
    c.sp_svc = cpu->sp_svc - 16;
    read_comment(&c, cpu->ret);
    uint32_t sys[2] = {r2, cpu->lr_sys};
    bios_push(&c, 2, sys);
    bios_tick(&c, BIOS_ENTRY_MASKED);
    bios_flush(&c, false);
    c.psr = c.hi[11];
    c.lr = RL_GBA_BIOS_SWI_RETURN;
    bios_tick(&c, BIOS_ENTRY_CYCLES - BIOS_ENTRY_MASKED);
    if ((number & 0xFFu) < 0x20 && models[number & 0xFFu].fitted && (number & 0xFFu) != 0x01 && c.timed) {
        c.model = &models[number & 0xFFu];
    }
    switch (number & 0xFFu) {
    case 0x00:
        soft_reset(&c, cpu);
        break;
    case 0x01:
        bios_register_ram_reset(&c);
        break;
    case 0x02:
        halt_write(&c, 0x02);
        break;
    case 0x03:
        halt_write(&c, 0x03);
        break;
    case 0x04:
        bios_intr_wait(&c);
        break;
    case 0x05:
        c.r[0] = 1;
        c.r[1] = 1;
        bios_tick(&c, 2);
        bios_intr_wait(&c);
        break;
    case 0x06:
        bios_div(&c);
        break;
    case 0x07:
        bios_div_arm(&c);
        break;
    case 0x08:
        bios_sqrt(&c);
        break;
    case 0x09:
        bios_arctan(&c);
        break;
    case 0x0A:
        bios_arctan2(&c);
        break;
    case 0x0B:
        bios_cpu_set(&c);
        break;
    case 0x0C:
        bios_cpu_fast_set(&c);
        break;
    case 0x0D:
        checksum(&c);
        break;
    case 0x0E:
        bios_bg_affine(&c);
        break;
    case 0x0F:
        bios_obj_affine(&c);
        break;
    case 0x10:
        bios_bit_unpack(&c);
        break;
    case 0x11:
        bios_lz77(&c, false);
        break;
    case 0x12:
        bios_lz77(&c, true);
        break;
    case 0x13:
        bios_huff(&c);
        break;
    case 0x14:
        bios_rl(&c, false);
        break;
    case 0x15:
        bios_rl(&c, true);
        break;
    case 0x16:
        bios_diff8(&c, false);
        break;
    case 0x17:
        bios_diff8(&c, true);
        break;
    case 0x18:
        bios_diff16(&c);
        break;
    case 0x19:
        bios_sound_bias(&c);
        break;
    case 0x1F:
        bios_midi_key2freq(&c);
        break;
    case 0x25:
        multiboot(&c);
        break;
    case 0x26:
        soft_reset(&c, cpu);
        cpu->ret = 0x08000000u;
        break;
    case 0x27:
        halt_write(&c, 0x27);
        break;
    default:
        c.status = RL_GBA_SWI_UNKNOWN;
        break;
    }
    c.psr &= ~0x20u;
    c.sp = cpu->sp_sys - 8;
    c.lr = RL_GBA_BIOS_SWI_RETURN;
    bios_tick(&c, BIOS_EXIT_CYCLES - BIOS_EXIT_MASKED);
    if (c.model)
        hide(&c);
    bool fit = (number & 0xFFu) < 0x20 && models[number & 0xFFu].fitted && c.status != RL_GBA_SWI_HANG;
    uint64_t total = fit ? charged(number & 0xFFu, &c) : c.cyc + BIOS_EXIT_MASKED;
    fit_accesses(&c, total);
    c.cyc = total > c.spent + BIOS_EXIT_MASKED ? total - BIOS_EXIT_MASKED : c.spent;
    bios_flush(&c, true);
    c.psr = 0xD3u;
    c.cyc = total > c.spent ? total : c.spent;
    bios_flush(&c, false);
    replay_rest(&c);
    total += return_refill(&c, cpu->ret, (cpu->cpsr & 0x20u) != 0);
    c.r[2] = r2;
    c.hi[11] = cpu->r[11];
    c.hi[12] = cpu->r[12];
    bool in_svc = (cpu->cpsr & 0x1Fu) == 0x13u;
    c.sp = in_svc ? cpu->sp_svc : cpu->sp_sys;
    c.lr = in_svc ? cpu->ret : cpu->lr_sys;
    c.sp_svc = cpu->sp_svc;
    bios_point(&c, cpu->ret);
    cpu->r[0] = c.r[0];
    cpu->r[1] = c.r[1];
    cpu->r[2] = r2;
    cpu->r[3] = c.r[3];
    b->open_bus = RL_GBA_BIOS_OPEN_SWI;
    b->bus_cycles = c.bus_cyc;
    for (unsigned i = 0; i < BIOS_EVENTS; i++)
        b->events[i] = c.ev[i];
    b->total = total;
    b->cycles = c.timed ? 0 : total;
    return c.status;
}

uint32_t rl_gba_bios_irq_entry(rl_gba_bios *b, const rl_gba_bus *bus, rl_gba_irq_cpu *cpu)
{
    bios_call c = {.b = b, .bus = bus, .sp = cpu->sp, .timed = bus_timed(bus), .psr = 0x92u};
    uint32_t v[6] = {cpu->r[0], cpu->r[1], cpu->r[2], cpu->r[3], cpu->r12, cpu->lr};
    bios_tick(&c, BIOS_STUB_PRE);
    bios_push(&c, 6, v);
    bios_tick(&c, BIOS_STUB_MID);
    cpu->sp = c.sp;
    cpu->r[0] = 0x04000000u;
    cpu->lr = RL_GBA_BIOS_IRQ_RETURN;
    uint32_t target = bios_ld32(&c, 0x03FFFFFCu, false);
    bios_tick(&c, 1);
    bios_tick(&c, BIOS_STUB_POST - 3 + (cpu->from_code ? BIOS_STUB_CODE_EXTRA : 0));
    bios_flush(&c, false);
    c.cyc += return_refill(&c, target, (target & 1u) != 0);
    b->open_bus = RL_GBA_BIOS_OPEN_IRQ;
    b->total = c.cyc;
    b->cycles = c.timed ? 0 : c.cyc;
    return target & ~3u;
}

uint32_t rl_gba_bios_irq_return(rl_gba_bios *b, const rl_gba_bus *bus, rl_gba_irq_cpu *cpu)
{
    bios_call c = {.b = b, .bus = bus, .sp = cpu->sp, .timed = bus_timed(bus), .psr = 0x92u};
    uint32_t v[6];
    bios_tick(&c, 1);
    for (unsigned i = 0; i < 6; i++)
        v[i] = bios_ldm(&c, c.sp + 4 * i, i != 0);
    bios_tick(&c, 1);
    bios_tick(&c, BIOS_STUB_RETURN - 2 - (cpu->from_code ? BIOS_STUB_RETURN_REFILL : 0));
    bios_flush(&c, false);
    cpu->sp = c.sp + 24;
    for (unsigned i = 0; i < 4; i++)
        cpu->r[i] = v[i];
    cpu->r12 = v[4];
    cpu->lr = v[5];
    b->open_bus = RL_GBA_BIOS_OPEN_IRQ_DONE;
    b->total = c.cyc;
    b->cycles = c.timed ? 0 : c.cyc;
    return v[5] - 4;
}
