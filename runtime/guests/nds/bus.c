/**
 * @file bus.c
 * @brief Timed accesses of the CPUs: the TCM fast path, the bus clock edge,
 * the order of shared accesses between the two CPUs (order.c), main RAM
 * contention and the cost of each access.
 *
 * ARM9 accesses that the protection unit makes cacheable or bufferable go
 * through the cache and write buffer costs (cache.c) instead.
 *
 * Fresh C from GBATEK ("DS Memory Timings": external accesses of the ARM9
 * are delayed to the bus clock; one CPU at a time is on the main memory
 * bus, EXMEMCNT bit 15 says which one wins a tie).
 */
#include "nds_int.h"

#include "bios.h"

void nds_charge(rl_nds *m, unsigned cpu, uint64_t ticks)
{
    if (!m->cpu[cpu] || ticks == 0)
        return;
    rl_sched_stall(m->cpu[cpu], cpu == RL_NDS_ARM9 ? ticks : (ticks + 1) / 2);
}

unsigned nds_cost(const rl_nds *m, unsigned cpu, unsigned kind, uint32_t addr, unsigned size, bool seq)
{
    unsigned r = addr >> 24;
    return m->costs.t[cpu][kind][r > 14 ? 15 : r][size >> 1][seq];
}

/**
 * @brief The ARM9's data access in a TCM: the instruction's own cycle covers
 * the first word in the data TCM; each further word of an LDM or STM takes a
 * cycle, and an instruction TCM access one more, but for the second word of
 * an LDM or STM (the console: LDR and STR in DTCM 1 cycle, LDMn and STMn
 * max(2, n); LDR in ITCM 2, LDMn there n + 2).
 */
static void tcm_charge(rl_nds *m, const uint8_t *p, bool seq)
{
    bool itcm = p >= m->itcm && p < m->itcm + RL_CP15_ITCM_SIZE;
    unsigned ticks = seq ? 1u : 0u;
    if (itcm) {
        ticks += !seq || m->itcm_multi;
        m->itcm_multi = !seq;
    }
    nds_charge(m, RL_NDS_ARM9, ticks);
}

/**
 * @brief Charges the ARM9 a bus data access that takes ticks; busy holds main
 * RAM that much longer (a write's last ticks, which the CPU does not wait for).
 *
 * The access starts on a bus clock edge (an even tick) no earlier than the
 * instruction's own cycle, which the code fetch charged, and, nonsequential,
 * no earlier than main RAM is free. A nonsequential access gives one tick
 * back: the next instruction executes beside it (it leaves its execute stage
 * when the access ends), so a load or store and one ALU instruction after it
 * take the access's time (the console: LDR from I/O and ADD 8 cycles, two
 * LDRs 16). A sequential word of the same LDM or STM follows directly; a
 * nonsequential access after another of the same instruction (SWP's write,
 * LDRD's second word) starts when that one ends, so SWP takes what LDR and
 * STR take.
 */
static void bus9(rl_nds *m, bool main, unsigned how, unsigned ticks, unsigned busy)
{
    uint64_t now = nds_now(m, RL_NDS_ARM9);
    bool seq = how == RL_NDS_S;
    uint64_t start = seq ? now : now & ~(uint64_t)1;
    if (how >= RL_NDS_AFTER && m->bus9_end > start)
        start = m->bus9_end;
    if (main && !seq && m->ram_free > start)
        start = m->ram_free;
    uint64_t end = start + ticks;
    m->bus9_end = end;
    if (main)
        m->ram_free = end + busy;
    uint64_t to = seq ? end : end - 1u;
    if (to > now)
        nds_charge(m, RL_NDS_ARM9, to - now);
}

/** @brief The ticks of the ARM9's bus data access as how says (RL_NDS_PAIR: a bus cycle over sequential). */
static unsigned cost9(const rl_nds *m, uint32_t addr, unsigned size, unsigned how, bool main)
{
    if (how == RL_NDS_PAIR)
        return main ? nds_cost(m, RL_NDS_ARM9, RL_NDS_DATA, addr, size, false)
                    : nds_cost(m, RL_NDS_ARM9, RL_NDS_DATA, addr, size, true) + 2u;
    return nds_cost(m, RL_NDS_ARM9, RL_NDS_DATA, addr, size, how == RL_NDS_S);
}

static void finish(rl_nds *m, unsigned cpu, bool main, unsigned ticks)
{
    if (!main) {
        nds_charge(m, cpu, ticks);
        return;
    }
    uint64_t now = nds_now(m, cpu);
    uint64_t start = now > m->ram_free ? now : m->ram_free;
    m->ram_free = start + ticks;
    nds_charge(m, cpu, m->ram_free - now);
}

/*
 * The ARM7's main RAM, from the console's timings (code in its work RAM or
 * in main RAM, data in each). A nonsequential access takes 6 cycles and starts
 * no earlier than 3 cycles after the RAM's last access ended (its
 * precharge); a write lets the CPU go after 4 of its 6, and each further
 * word of an STM after its 2, 2 cycles behind the RAM as the first. A
 * halfword or byte access takes a cycle less, but a byte write lets the CPU
 * go when a word write would (STRB from work RAM code costs STR's, from main
 * RAM code STRH's). A code fetch right after the previous one, at
 * the next address in the same 32-byte block, is sequential: 2 cycles, or 1
 * for a Thumb halfword. Any other cycle in between, a data access or an
 * internal cycle, ends the burst, and internal cycles overlap the precharge:
 * MUL, MLA and UMLAL in main RAM code all cost 7 more than an ADD there.
 */
#define MAIN7_N 12u
#define MAIN7_S 4u
#define MAIN7_WRITE 8u
#define MAIN7_PRECHARGE 6u
/*
 * An ARM7 write to work RAM (shared or its own) followed by a code fetch
 * from work RAM costs a cycle more: STR, STM and PUSH there cost 1 more than
 * LDR, LDM and POP on the console in work RAM code, but no more in main RAM
 * code, and SWP (whose write an internal cycle follows) costs only its
 * load's and store's cycles. The store charges it and a main RAM fetch right
 * after gives it back.
 */
#define WRAM7_WRITE_N 2u

/** @brief Ticks until a nonsequential main RAM access of the ARM7 at tick now ends, the RAM's busy end in *busy. */
static uint64_t main7(rl_nds *m, uint64_t now, bool write, unsigned size, uint64_t *busy)
{
    uint64_t start = m->ram_free + MAIN7_PRECHARGE;
    if (start < now)
        start = now;
    unsigned n = size < 4 ? MAIN7_N - 2u : MAIN7_N;
    *busy = start + n;
    if (!write)
        return start + n;
    return start + (size == 2 ? MAIN7_WRITE - 2u : MAIN7_WRITE);
}

/*
 * The lifted code names each fetch by the instruction that makes it: an
 * instruction's own fetch by its address (the CPU fetches 8 or 4 bytes
 * further on), a refill's two by the branch target. A fetch whose name is
 * the last one's or the next after it continues the stream; another starts
 * it at its name. The stream's address decides the 32-byte block. A
 * refill's first fetch always starts the stream: a branch to the next
 * instruction costs the console a full refill (B, BX and MOV PC there 18
 * cycles more than a NOP, Thumb B 9).
 */
unsigned rl_nds_code_cost7(rl_nds *m, uint32_t addr, unsigned size, bool seq)
{
    uint64_t now = nds_now(m, RL_NDS_ARM7);
    unsigned back = 0;
    if (m->wram7_write_end == now && m->wram7_write_end) {
        back = WRAM7_WRITE_N / 2u;
        now -= WRAM7_WRITE_N;
    }
    m->wram7_write_end = 0;
    uint64_t from = now < m->burst7_end ? m->burst7_end : now;
    bool goes_on = seq && (addr == m->burst7_name || addr == m->burst7_name + size);
    uint32_t at = goes_on ? m->burst7_next : addr;
    uint64_t end;
    if (goes_on && from == m->burst7_end && (at & 31u) && m->ram_free <= from) {
        end = from + (size < 4 ? MAIN7_S / 2u : MAIN7_S);
        m->ram_free = end;
    } else {
        end = main7(m, from, false, size, &m->ram_free);
    }
    m->burst7_name = addr;
    m->burst7_next = at + size;
    m->burst7_end = end;
    unsigned cyc = (unsigned)((end - from + 1u) / 2u);
    return cyc > back ? cyc - back : 0u;
}

/** @brief Charges the ARM7 a main RAM data access: nonsequential, or the next word of an LDM or STM. */
static void data7(rl_nds *m, bool write, bool seq, unsigned size)
{
    uint64_t now = nds_now(m, RL_NDS_ARM7);
    uint64_t end;
    if (seq) {
        uint64_t ram = (m->ram_free > now ? m->ram_free : now) + MAIN7_S;
        m->ram_free = ram;
        end = write ? ram - (MAIN7_N - MAIN7_WRITE) : ram;
        if (end < now)
            end = now;
    } else {
        end = main7(m, now, write, size, &m->ram_free);
    }
    m->burst7_end = 0;
    nds_charge(m, RL_NDS_ARM7, end - now);
}

static uint32_t bus_read(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, nds_where w)
{
    switch (w.kind) {
    case NDS_K_MEM:
        return nds_ld(w.p, size);
    case NDS_K_VRAM:
        return nds_vram_read_slow(m, nds_vram_cpu_view(cpu, addr), addr, size);
    case NDS_K_IO:
        if (m->sched)
            return (uint32_t)rl_sched_read(m->cpu[cpu], nds_io_key(cpu, addr), size);
        return nds_io_read(m, cpu, addr, size, 0);
    case NDS_K_SLOT:
        return nds_slot_read(m, cpu, addr, size);
    case NDS_K_BIOS:
        return rl_nds_bios_read(cpu, addr, size);
    default:
        return 0;
    }
}

uint32_t nds_bus_load(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint64_t now, bool *main)
{
    nds_where w = nds_decode_bus(m, cpu, addr, false);
    *main = w.main;
    switch (w.kind) {
    case NDS_K_MEM:
        return nds_ld(w.p, size);
    case NDS_K_IO:
        return nds_io_read(m, cpu, addr, size, now);
    case NDS_K_SLOT:
        return nds_slot_read(m, cpu, addr, size);
    default:
        return 0;
    }
}

void nds_bus_store(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value, uint64_t now, bool *main)
{
    nds_where w = nds_decode_bus(m, cpu, addr, true);
    *main = w.main;
    if (w.kind == NDS_K_MEM && w.p) {
        nds_st(w.p, size, value);
        nds_watch_store(m, w.p);
    } else if (w.kind == NDS_K_IO) {
        nds_io_write(m, cpu, addr, size, value, now);
        m->halt_req = false;
    }
}

static uint32_t timed_read(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, unsigned how, bool code)
{
    bool seq = how == RL_NDS_S || how == RL_NDS_PAIR;
    addr &= ~(size - 1);
    if (cpu == RL_NDS_ARM9) {
        uint8_t *p = nds_tcm(m, addr, code, false);
        if (p) {
            if (code)
                nds_charge(m, cpu, 1);
            else
                tcm_charge(m, p, seq);
            return nds_ld(p, size);
        }
        if (nds_cache_attr(m, addr) & (code ? NDS_ATTR_IC : NDS_ATTR_DC)) {
            nds_where w = nds_decode(m, cpu, addr, code, false);
            if (w.kind == NDS_K_MEM || w.kind == NDS_K_BIOS) {
                if (nds_dma_live(m, cpu))
                    nds_dma_wait(m, cpu);
                if (nds_shared(cpu, addr))
                    nds_order(m, cpu);
                nds_cache_read(m, addr, code, seq);
                return bus_read(m, cpu, addr, size, w);
            }
        }
        nds_cache_before_bus(m);
        if (!code) {
            if (nds_dma_live(m, cpu))
                nds_dma_wait(m, cpu);
            if (nds_shared(cpu, addr))
                nds_order(m, cpu);
            nds_where w = nds_decode(m, cpu, addr, code, false);
            uint32_t v = bus_read(m, cpu, addr, size, w);
            bus9(m, w.main, how, cost9(m, addr, size, how, w.main), 0);
            return v;
        }
        if (nds_now(m, cpu) & 1)
            nds_charge(m, cpu, 1);
    }
    if (nds_dma_live(m, cpu))
        nds_dma_wait(m, cpu);
    if (nds_shared(cpu, addr))
        nds_order(m, cpu);
    nds_where w = nds_decode(m, cpu, addr, code, false);
    uint32_t v = bus_read(m, cpu, addr, size, w);
    if (cpu == RL_NDS_ARM7 && w.main && !code)
        data7(m, false, seq, size);
    else
        finish(m, cpu, w.main, nds_cost(m, cpu, code ? RL_NDS_CODE : RL_NDS_DATA, addr, size, seq && !(code && cpu == RL_NDS_ARM9)));
    return v;
}

unsigned rl_nds_code_cost(rl_nds *m, uint32_t addr, unsigned size)
{
    if (nds_cache_attr(m, addr) & NDS_ATTR_IC) {
        nds_cache_read(m, addr, true, false);
        return 0;
    }
    nds_cache_before_bus(m);
    return nds_cost(m, RL_NDS_ARM9, RL_NDS_CODE, addr, size, false);
}

uint32_t rl_nds_fetch(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, bool seq)
{
    return timed_read(m, cpu, addr, size, seq ? RL_NDS_S : RL_NDS_N, true);
}

uint32_t rl_nds_load(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, unsigned how)
{
    return timed_read(m, cpu, addr, size, how, false);
}

void rl_nds_store(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value, unsigned how)
{
    bool seq = how == RL_NDS_S;
    addr &= ~(size - 1);
    value &= nds_mask(size);
    if (cpu == RL_NDS_ARM9) {
        uint8_t *p = nds_tcm(m, addr, false, true);
        if (p) {
            tcm_charge(m, p, seq);
            nds_st(p, size, value);
            nds_watch_store(m, p);
            return;
        }
        unsigned at = nds_cache_attr(m, addr);
        nds_where w = nds_decode(m, cpu, addr, false, true);
        if (w.kind == NDS_K_MEM && w.p && (at & (NDS_ATTR_WB | NDS_ATTR_DC))) {
            if (nds_dma_live(m, cpu))
                nds_dma_wait(m, cpu);
            if (nds_shared(cpu, addr))
                nds_order(m, cpu);
            if (nds_seen_by_display(cpu, addr))
                nds_disp_sync(m, nds_now(m, cpu));
            nds_st(w.p, size, value);
            nds_watch_store(m, w.p);
            bool back = (at & NDS_ATTR_DC) && (at & NDS_ATTR_WB);
            if (!((at & NDS_ATTR_DC) && nds_cache_write_hit(m, addr, back, seq))) {
                nds_cache_buffer(m, addr, nds_cost(m, cpu, RL_NDS_DATA, addr, size, seq));
                if (seq)
                    nds_charge(m, cpu, 1);
            }
            return;
        }
        nds_cache_before_bus(m);
    }
    if (nds_dma_live(m, cpu))
        nds_dma_wait(m, cpu);
    if (nds_shared(cpu, addr))
        nds_order(m, cpu);
    nds_where w = nds_decode(m, cpu, addr, false, true);
    if (size == 1 && nds_drops_byte(cpu, addr))
        w.kind = NDS_K_NONE;
    else if (nds_seen_by_display(cpu, addr))
        nds_disp_sync(m, nds_now(m, cpu));
    if (w.kind == NDS_K_MEM && w.p) {
        nds_st(w.p, size, value);
        nds_watch_store(m, w.p);
    } else if (w.kind == NDS_K_VRAM) {
        nds_vram_write_slow(m, nds_vram_cpu_view(cpu, addr), addr, size, value);
    } else if (w.kind == NDS_K_IO) {
        if (m->sched)
            rl_sched_write(m->cpu[cpu], nds_io_key(cpu, addr), size, value);
        else
            nds_io_write(m, cpu, addr, size, value, 0);
        uint64_t wait = nds_gx_stall(m);
        if (wait > nds_now(m, cpu))
            nds_charge(m, cpu, wait - nds_now(m, cpu));
    }
    unsigned cost = nds_cost(m, cpu, RL_NDS_DATA, addr, size == 1 && w.main ? 4u : size, seq);
    if (cpu == RL_NDS_ARM9) {
        unsigned posted = w.main && !seq ? NDS_MAIN_WRITE_POSTED : 0;
        bus9(m, w.main, how, cost - posted, posted);
    } else if (w.main) {
        data7(m, true, seq, size);
    } else {
        bool first = how == RL_NDS_N && addr >> 24 == 0x03;
        bool more = how == RL_NDS_S && addr >> 24 == 0x03 && m->wram7_write_end;
        finish(m, cpu, w.main, cost + (first ? WRAM7_WRITE_N : 0u));
        m->wram7_write_end = first || more ? nds_now(m, cpu) : 0;
    }
    if (m->halt_req) {
        m->halt_req = false;
        if (!rl_nds_halt(m, cpu))
            m->halt_lost[cpu] = true;
    }
}

bool rl_nds_halt_lost(rl_nds *m, unsigned cpu)
{
    if (cpu > 1 || !m->halt_lost[cpu])
        return false;
    m->halt_lost[cpu] = false;
    return true;
}

void rl_nds_idle(rl_nds *m, unsigned cpu, unsigned ticks) { nds_charge(m, cpu, ticks); }
