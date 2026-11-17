/**
 * @file bios.c
 * @brief The software interrupts of the Nintendo DS BIOS: the dispatcher per
 * CPU, the arithmetic, the halt and wait calls, the reset and the small
 * calls (checksum, debugger test, delays).
 *
 * Fresh C from GBATEK ("BIOS Function Summary", "BIOS Arithmetic Functions",
 * "BIOS Halt Functions", "BIOS Reset Functions", "BIOS Misc Functions",
 * "DS Memory Control - BIOS") and, for the arithmetic, the loop structure of
 * relift's GBA BIOS (runtime/guests/gba/bios/math.c), which the console's
 * routines share.
 */
#include "bios_int.h"

#include "nds_int.h"

/** @brief The IntrWait flag word of cpu: DTCM + 0x3FF8 on the ARM9, 0x0380FFF8 on the ARM7. */
static uint32_t flag_addr(rl_nds *m, unsigned cpu)
{
    if (cpu == RL_NDS_ARM7)
        return 0x0380FFF8u;
    rl_arm32_tcm t;
    rl_arm32_cp15_tcm(rl_nds_cp15(m), &t);
    return t.dtcm_base + 0x3FF8u;
}

uint32_t nds_bios_ld(const nds_bios_env *e, uint32_t addr, unsigned size, bool seq)
{
    return rl_nds_load(e->m, e->cpu, addr & ~(size - 1u), size, seq);
}

void nds_bios_st(const nds_bios_env *e, uint32_t addr, unsigned size, uint32_t v, bool seq)
{
    rl_nds_store(e->m, e->cpu, addr & ~(size - 1u), size, v, seq);
}

void nds_bios_cycles(const nds_bios_env *e, uint64_t cycles)
{
    while (cycles) {
        unsigned n = cycles > 0x10000000u ? 0x10000000u : (unsigned)cycles;
        rl_nds_idle(e->m, e->cpu, e->cpu == RL_NDS_ARM9 ? n : 2u * n);
        cycles -= n;
    }
}

/** @brief The quotient and remainder loop of Div and Sqrt (the console's, shared with the GBA's). */
typedef struct udiv {
    uint32_t q, rem;
    bool hang;
} udiv;

static udiv udiv_run(uint32_t n, uint32_t d, uint32_t q0)
{
    udiv r = {.q = q0};
    uint32_t s = d;
    for (;;) {
        uint32_t h = n >> 1;
        bool more = s < h;
        if (s <= h)
            s <<= 1;
        if (!more)
            break;
        if (s == 0) {
            r.hang = true;
            return r;
        }
    }
    for (;;) {
        bool ge = n >= s;
        r.q = r.q * 2 + ge;
        if (ge)
            n -= s;
        if (s == d)
            break;
        s >>= 1;
    }
    r.rem = n;
    return r;
}

/** @brief Div: r0 = quotient, r1 = remainder, r3 = |quotient|; false when the console's loop never ends. */
static bool swi_div(uint32_t r[4])
{
    uint32_t num = r[0], den = r[1];
    uint32_t d = den & 0x80000000u ? 0u - den : den;
    uint32_t n = num & 0x80000000u ? 0u - num : num;
    udiv u = udiv_run(n, d, den & 0x80000000u);
    if (u.hang)
        return false;
    bool neg = ((den ^ num) & 0x80000000u) != 0;
    r[3] = u.q;
    r[0] = neg ? 0u - u.q : u.q;
    r[1] = num & 0x80000000u ? 0u - u.rem : u.rem;
    return true;
}

/** @brief Sqrt: r0 = floor(sqrt(r0)). */
static void swi_sqrt(uint32_t r[4])
{
    uint32_t v = r[0], x = v, g = 1;
    while (x > g) {
        x >>= 1;
        g <<= 1;
    }
    uint32_t prev;
    do {
        prev = g;
        udiv u = udiv_run(v, g, 0);
        g = (g + u.q) >> 1;
    } while (g < prev);
    r[0] = prev;
}

/** @brief GetCRC16: CRC-16 (polynomial 0xA001, reflected) over len bytes at addr as halfwords; r3 = the last halfword. */
static void swi_crc16(const nds_bios_env *e, uint32_t r[4])
{
    uint32_t crc = r[0] & 0xFFFFu, addr = r[1], len = r[2];
    uint32_t last = r[3];
    for (uint32_t i = 0; i + 2 <= len; i += 2) {
        last = nds_bios_ld(e, addr + i, 2, i != 0);
        for (unsigned half = 0; half < 2; half++) {
            crc ^= (last >> (8 * half)) & 0xFFu;
            for (unsigned j = 0; j < 8; j++)
                crc = crc & 1u ? (crc >> 1) ^ 0xA001u : crc >> 1;
        }
    }
    r[0] = crc;
    if (len)
        r[3] = last;
}

/** @brief Cycles of one WaitByLoop iteration on the ARM9 when the BIOS code is cached and when it is not (GBATEK's table). */
#define WAIT_LOOP_CACHED 4u
#define WAIT_LOOP_UNCACHED 16u
/** @brief Cycles of one WaitByLoop iteration on the ARM7. */
#define WAIT_LOOP_ARM7 4u

/** @brief True when the ARM9 caches code fetched from the BIOS region: caches and protection unit on, and the region holding it cacheable. */
static bool bios_code_cached(rl_nds *m)
{
    const rl_arm32_cp15 *p = rl_nds_cp15(m);
    if (!(p->control & RL_CP15_MPU) || !(p->control & RL_CP15_ICACHE))
        return false;
    int region = rl_arm32_cp15_region(p, 0xFFFF0000u);
    return region >= 0 && (p->icache_bits >> region & 1u) != 0;
}

/** @brief WaitByLoop: "SUB R0,1 / BGT LOP" run in the BIOS; r0 is the count. */
static void swi_wait_by_loop(const nds_bios_env *e, uint32_t r[4])
{
    int32_t n = (int32_t)r[0];
    uint32_t iterations = n > 0 ? (uint32_t)n : 1u;
    unsigned per = e->cpu == RL_NDS_ARM7 ? WAIT_LOOP_ARM7 : bios_code_cached(e->m) ? WAIT_LOOP_CACHED : WAIT_LOOP_UNCACHED;
    nds_bios_cycles(e, (uint64_t)iterations * per);
    r[0] = (uint32_t)(n > 0 ? 0 : n - 1);
}

/** @brief SoundBias (ARM7): steps SOUNDBIAS to level r0 (0, else 0x200) one at a time with r1 delay counts between steps. */
static void swi_sound_bias(const nds_bios_env *e, uint32_t r[4])
{
    const uint32_t reg = 0x04000504u;
    uint32_t want = r[0] ? 0x200u : 0u;
    uint32_t v = nds_bios_ld(e, reg, 2, false);
    uint32_t level = v & 0x3FFu;
    while (level != want) {
        level = level < want ? level + 1 : level - 1;
        nds_bios_st(e, reg, 2, (v & ~0x3FFu) | level, false);
        nds_bios_cycles(e, (uint64_t)r[1] * 4u + 8u);
    }
}

/*
 * Cycles from the interrupt that ends a BIOS wait to the instruction after
 * the call, besides the bus accesses the call makes (hwtest 3, the same on
 * the DSi and the 3DS: a timer interrupt or VBlank to the caller's next
 * instruction). wake_cycles is the halted CPU coming back and the call's
 * return (ARM9 cycles, ARM7 cycles); an IRQ taken inside IntrWait or
 * VBlankIntrWait costs the ARM9 RL_NDS_BIOS_WAKE_IRQ9 more (the BIOS's
 * dispatcher besides the handler's own code; run/cpu.c charges it).
 */
static const uint8_t wake_cycles[2] = {50, 42};

/** @brief Halt and IntrWait's wait: the ARM7 writes HALTCNT, the ARM9 waits for interrupt. False when nothing can wake the CPU. */
static bool cpu_halt(const nds_bios_env *e, uint32_t haltcnt)
{
    bool woke;
    if (e->cpu == RL_NDS_ARM7) {
        nds_bios_st(e, 0x04000301u, 1, haltcnt, false);
        woke = !rl_nds_halt_lost(e->m, RL_NDS_ARM7);
    } else {
        woke = rl_nds_halt(e->m, RL_NDS_ARM9);
    }
    if (woke)
        nds_bios_cycles(e, wake_cycles[e->cpu] - RL_NDS_BIOS_CALL_CYCLES);
    return woke;
}

/** @brief IntrWait: waits for one of the interrupts in r1 to set its bit in the BIOS flag word; r0 != 0 discards old flags first. */
static rl_nds_swi_status swi_intr_wait(const nds_bios_env *e, uint32_t r[4])
{
    const uint32_t flags = flag_addr(e->m, e->cpu);
    bool discard = r[0] != 0;
    uint32_t mask = r[1];
    nds_bios_st(e, 0x04000208u, 4, 1, false);
    uint32_t old = nds_bios_ld(e, flags, 4, false);
    if (discard) {
        nds_bios_st(e, flags, 4, old & ~mask, false);
    } else if (e->cpu == RL_NDS_ARM7 && (old & mask)) {
        nds_bios_st(e, flags, 4, old & ~mask, false);
        return RL_NDS_SWI_OK;
    }
    for (;;) {
        if (!cpu_halt(e, 0x80u))
            return RL_NDS_SWI_STOP;
        if (e->hooks->irq)
            while (e->hooks->irq(e->hooks->user)) {
            }
        uint32_t now = nds_bios_ld(e, flags, 4, false);
        if (now & mask) {
            nds_bios_st(e, flags, 4, now & ~mask, false);
            return RL_NDS_SWI_OK;
        }
        nds_bios_cycles(e, RL_NDS_BIOS_CALL_CYCLES);
    }
}

/** @brief True when the ARM9's data cache is on (IsDebugger then always reads the 8 MiB answer, GBATEK). */
static bool dcache_on(rl_nds *m) { return (rl_nds_cp15(m)->control & RL_CP15_DCACHE) != 0; }

/** @brief IsDebugger: 4 MiB of main RAM (mirrored) or 8 MiB, by writing a halfword and reading its mirror. */
static void swi_is_debugger(const nds_bios_env *e, uint32_t r[4])
{
    uint32_t at = e->cpu == RL_NDS_ARM7 ? 0x027FFFFAu : 0x027FFFF8u;
    uint32_t mirror = at - 0x00400000u;
    bool eight = true;
    for (unsigned pass = 0; pass < 2; pass++) {
        uint32_t v = pass ? 0xFFFFu : 0u;
        nds_bios_st(e, at, 2, v, false);
        eight = eight && nds_bios_ld(e, mirror, 2, false) != v;
    }
    r[0] = eight || (e->cpu == RL_NDS_ARM9 && dcache_on(e->m)) ? 1u : 0u;
}

/** @brief SoftReset: zero fill, the return address, and on the ARM9 the CP15 control word. */
static void swi_soft_reset(const nds_bios_env *e, rl_nds_bios_reset *out)
{
    bool arm9 = e->cpu == RL_NDS_ARM9;
    uint32_t base = arm9 ? flag_addr(e->m, RL_NDS_ARM9) - 0x3FF8u + 0x3E00u : 0x0380FE00u;
    uint32_t ret = nds_bios_ld(e, arm9 ? 0x027FFE24u : 0x027FFE34u, 4, false);
    for (uint32_t a = 0; a < 0x200; a += 4)
        nds_bios_st(e, base + a, 4, 0, a != 0);
    out->cp15 = arm9;
    out->pc = ret;
    out->sp_sys = arm9 ? 0x00803EC0u : 0x0380FF00u;
    out->sp_irq = arm9 ? 0x00803FA0u : 0x0380FFB0u;
    out->sp_svc = arm9 ? 0x00803FC0u : 0x0380FFDCu;
}

/** @brief Whether number is a function on cpu (GBATEK's summary: the rest jumps to 0). */
static bool valid(unsigned cpu, unsigned n)
{
    static const uint32_t nds9 = 1u << 0x00 | 1u << 0x03 | 1u << 0x04 | 1u << 0x05 | 1u << 0x06 | 1u << 0x09 | 1u << 0x0B | 1u << 0x0C |
                                 1u << 0x0D | 1u << 0x0E | 1u << 0x0F | 1u << 0x10 | 1u << 0x11 | 1u << 0x12 | 1u << 0x13 | 1u << 0x14 |
                                 1u << 0x15 | 1u << 0x16 | 1u << 0x18 | 1u << 0x1F;
    static const uint32_t nds7 = 1u << 0x00 | 1u << 0x03 | 1u << 0x04 | 1u << 0x05 | 1u << 0x06 | 1u << 0x07 | 1u << 0x08 | 1u << 0x09 |
                                 1u << 0x0B | 1u << 0x0C | 1u << 0x0D | 1u << 0x0E | 1u << 0x0F | 1u << 0x10 | 1u << 0x11 | 1u << 0x12 |
                                 1u << 0x13 | 1u << 0x14 | 1u << 0x15 | 1u << 0x1A | 1u << 0x1B | 1u << 0x1C | 1u << 0x1D | 1u << 0x1F;
    return n < 0x20 && ((cpu == RL_NDS_ARM9 ? nds9 : nds7) >> n & 1u);
}

rl_nds_swi_status rl_nds_bios_swi(rl_nds *m, unsigned cpu, unsigned number, uint32_t r[4], const rl_nds_bios_hooks *hooks,
                                  rl_nds_bios_reset *reset)
{
    nds_bios_env e = {.m = m, .cpu = cpu, .hooks = hooks};
    number &= 0xFFu;
    if (!valid(cpu, number))
        return RL_NDS_SWI_INVALID;
    nds_bios_cycles(&e, RL_NDS_BIOS_CALL_CYCLES);
    switch (number) {
    case 0x00:
        swi_soft_reset(&e, reset);
        return RL_NDS_SWI_RESET;
    case 0x03:
        swi_wait_by_loop(&e, r);
        break;
    case 0x04:
        return swi_intr_wait(&e, r);
    case 0x05: {
        uint32_t args[4] = {1, 1, r[2], r[3]};
        rl_nds_swi_status st = swi_intr_wait(&e, args);
        r[0] = args[0];
        r[1] = args[1];
        return st;
    }
    case 0x06:
        if (cpu == RL_NDS_ARM9) {
            if (!m->irq[RL_NDS_ARM9].ime)
                return RL_NDS_SWI_HANG;
            r[0] = 0;
        }
        return cpu_halt(&e, 0x80u) ? RL_NDS_SWI_OK : RL_NDS_SWI_STOP;
    case 0x07:
        return cpu_halt(&e, 0xC0u) ? RL_NDS_SWI_OK : RL_NDS_SWI_STOP;
    case 0x08:
        swi_sound_bias(&e, r);
        break;
    case 0x09:
        if (!swi_div(r))
            return RL_NDS_SWI_HANG;
        break;
    case 0x0B:
        nds_bios_cpu_set(&e, r);
        break;
    case 0x0C:
        nds_bios_cpu_fast_set(&e, r);
        break;
    case 0x0D:
        swi_sqrt(r);
        break;
    case 0x0E:
        swi_crc16(&e, r);
        break;
    case 0x0F:
        swi_is_debugger(&e, r);
        break;
    case 0x10:
        nds_bios_bit_unpack(&e, r);
        break;
    case 0x11:
        nds_bios_lz77(&e, r, false);
        break;
    case 0x12:
        nds_bios_lz77(&e, r, true);
        break;
    case 0x13:
        nds_bios_huff(&e, r);
        break;
    case 0x14:
        nds_bios_rl(&e, r, false);
        break;
    case 0x15:
        nds_bios_rl(&e, r, true);
        break;
    case 0x16:
        nds_bios_diff8(&e, r);
        break;
    case 0x18:
        nds_bios_diff16(&e, r);
        break;
    case 0x1A:
        r[0] = rl_nds_bios_sine(r[0]);
        break;
    case 0x1B:
        r[0] = rl_nds_bios_pitch(r[0]);
        break;
    case 0x1C:
        r[0] = rl_nds_bios_volume(r[0]);
        break;
    case 0x1D:
        nds_bios_boot_procs(&e, r);
        return RL_NDS_SWI_UNKNOWN_RESULT;
    case 0x1F:
        if (cpu == RL_NDS_ARM9)
            nds_bios_st(&e, 0x04000300u, 4, r[0], false);
        else
            return nds_bios_custom_halt(&e, r[2]);
        break;
    default:
        break;
    }
    return RL_NDS_SWI_OK;
}

uint32_t rl_nds_bios_read(unsigned cpu, uint32_t addr, unsigned size)
{
    (void)addr;
    return cpu == RL_NDS_ARM7 ? nds_mask(size) : 0u;
}
