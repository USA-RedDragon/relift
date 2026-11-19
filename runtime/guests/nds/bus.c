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
 *
 * Main RAM between the CPUs, from the consoles' timings of each CPU's loads,
 * stores, LDM and STM, code and DMA against the other's (check/nds-timing,
 * idx 4000 on): the RAM serves one access at a time. When it frees up, the
 * CPU EXMEMCNT bit 15 puts first (and its DMA) gets it if it has asked by
 * then, whoever asked earlier; the other CPU gets it only when the first one
 * is not asking at that moment, so a stream of the first CPU's stores, code
 * fetches or DMA units with no gap keeps it out entirely. The first CPU also
 * takes the RAM between two words of the other's burst (an LDM or STM, a
 * cache line fill, sequential code, consecutive DMA units), whose next word
 * then starts over as a nonsequential access: an ARM9 LDM of 8 words under an
 * ARM7 load loop with the ARM7 first takes 8 times one load of each.
 */
#include "nds_int.h"

#include "bios.h"

bool nds_ram_first(const rl_nds *m, unsigned cpu)
{
    bool arm7_first = (m->exmem[RL_NDS_ARM9] & 0x8000u) != 0;
    return (cpu == RL_NDS_ARM7) == arm7_first;
}

/*
 * The ARM7's main RAM, from the console's timings (code in its work RAM or
 * in main RAM, data in each). A nonsequential access takes 6 cycles and
 * the RAM takes no new access for 3 cycles after it (its precharge, which
 * keeps the ARM9 out as well); a write lets the CPU go after 4 of its 6, and
 * each further word of an STM after its 2, 2 cycles behind the RAM as the
 * first. A halfword or byte access takes a cycle less, but a byte write lets
 * the CPU go when a word write would (STRB from work RAM code costs STR's,
 * from main RAM code STRH's). A code fetch right after the previous one, at
 * the next address in the same 32-byte block, is sequential: 2 cycles, or 1
 * for a Thumb halfword. Any other cycle in between, a data access or an
 * internal cycle, ends the burst, and internal cycles overlap the precharge:
 * MUL, MLA and UMLAL in main RAM code all cost 7 more than an ADD there.
 */
#define MAIN7_N 12u
#define MAIN7_S 4u
#define MAIN7_WRITE 8u
#define MAIN7_PRECHARGE 6u

uint64_t nds_ram_pos(const rl_nds *m, unsigned cpu)
{
    uint64_t now = nds_now(m, cpu);
    return cpu == RL_NDS_ARM9 ? now & ~(uint64_t)1 : now;
}

uint64_t nds_ram_busy(const rl_nds *m) { return m->ram_free + (m->ram_side == NDS_RAM_CPU7 ? MAIN7_PRECHARGE : 0u); }

/*
 * Main RAM goes to the side EXMEMCNT bit 15 favors whenever it asks, and a
 * stream of its accesses with no gap keeps the other out for as long as it
 * runs: on the consoles an ARM7 STR loop, DMA or code in main RAM keeps an
 * uncached ARM9 kernel from finishing even its 4 warm-up iterations while
 * it runs (8000 ticks and more in hwtest 3c), and the ARM9's streams do the
 * same to the ARM7 the other way.
 */
void nds_ram_wait(rl_nds *m, unsigned cpu)
{
    if (!m->sched)
        return;
    nds_dma_catch_up(m, cpu ^ 1u, nds_ram_pos(m, cpu));
    if (nds_ram_first(m, cpu))
        return;
    while (nds_ram_busy(m) > nds_now(m, cpu)) {
        nds_charge(m, cpu, nds_ram_busy(m) - nds_now(m, cpu));
        nds_order(m, cpu);
        nds_dma_catch_up(m, cpu ^ 1u, nds_ram_pos(m, cpu));
    }
}

/** @brief True when the other side has had main RAM since cpu's last access: cpu's burst starts over. */
static bool ram_cut(const rl_nds *m, unsigned cpu) { return m->ram_side != cpu; }

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
 * max(2, n); LDR in ITCM 2, LDMn there n + 2). A load right after a
 * single-word store waits a cycle in the data TCM (the lifted code charges
 * it with the store) but not in the instruction TCM, whose own extra cycle
 * covers it: STR then LDR there costs what two LDRs do (PUSH then POP
 * still waits). Right after is within the store's waiting cycle and the
 * load's fetch.
 */
static void tcm_charge(rl_nds *m, const uint8_t *p, bool seq, bool write)
{
    bool itcm = p >= m->itcm && p < m->itcm + RL_CP15_ITCM_SIZE;
    unsigned ticks = seq ? 1u : 0u;
    if (itcm) {
        ticks += !seq || m->itcm_multi;
        m->itcm_multi = !seq;
        if (!write && !seq && m->itcm_store && nds_now(m, RL_NDS_ARM9) - m->itcm_store_end <= 2u && ticks)
            ticks--;
    }
    m->itcm_store = itcm && write && !seq;
    nds_charge(m, RL_NDS_ARM9, ticks);
    m->itcm_store_end = nds_now(m, RL_NDS_ARM9);
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
    bool seq = how == RL_NDS_S;
    if (main && !seq)
        nds_ram_wait(m, RL_NDS_ARM9);
    uint64_t now = nds_now(m, RL_NDS_ARM9);
    uint64_t start = seq ? now : now & ~(uint64_t)1;
    if (how >= RL_NDS_AFTER && m->bus9_end > start)
        start = m->bus9_end;
    if (main && !seq && nds_ram_busy(m) > start)
        start = nds_ram_busy(m);
    uint64_t end = start + ticks;
    m->bus9_end = end;
    if (main) {
        m->ram_free = (end + busy + 1u) & ~(uint64_t)1;
        m->ram_side = NDS_RAM_CPU9;
    }
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
    nds_ram_wait(m, cpu);
    uint64_t now = nds_now(m, cpu);
    uint64_t busy = nds_ram_busy(m);
    uint64_t start = now > busy ? now : busy;
    m->ram_free = start + ticks;
    m->ram_side = (uint8_t)cpu;
    nds_charge(m, cpu, m->ram_free - now);
}

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
    uint64_t start = nds_ram_busy(m);
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
 * cycles more than a NOP, Thumb B 9). The fetch right after a stream's
 * first that reaches a new 32-byte block is nonsequential (a refill whose
 * second word is the next block's first); later in a stream the blocks
 * do not matter (16 ADDs in a row cost 2 cycles each, across two blocks).
 */
unsigned rl_nds_code_cost7(rl_nds *m, uint32_t addr, unsigned size, bool seq)
{
    nds_order(m, RL_NDS_ARM7);
    bool goes_on = seq && (addr == m->burst7_name || addr == m->burst7_name + size);
    bool uncut = goes_on && !ram_cut(m, RL_NDS_ARM7);
    if (!uncut)
        nds_ram_wait(m, RL_NDS_ARM7);
    uint64_t now = nds_now(m, RL_NDS_ARM7);
    unsigned back = 0;
    if (m->wram7_write_end == now && m->wram7_write_end) {
        back = WRAM7_WRITE_N / 2u;
        now -= WRAM7_WRITE_N;
    }
    m->wram7_write_end = 0;
    uint64_t from = now < m->burst7_end ? m->burst7_end : now;
    uint32_t at = goes_on ? m->burst7_next : addr;
    uint64_t end;
    bool block = (at & 31u) == 0 && m->burst7_len < 2u;
    if (uncut && from == m->burst7_end && !block && m->ram_free <= from) {
        end = from + (size < 4 ? MAIN7_S / 2u : MAIN7_S);
        m->ram_free = end;
    } else {
        end = main7(m, from, false, size, &m->ram_free);
    }
    m->burst7_len = goes_on && m->burst7_len < 255u ? m->burst7_len + 1u : 1u;
    m->burst7_name = addr;
    m->burst7_next = at + size;
    m->burst7_end = end;
    m->ram_side = NDS_RAM_CPU7;
    unsigned cyc = (unsigned)((end - from + 1u) / 2u);
    return cyc > back ? cyc - back : 0u;
}

/** @brief Charges the ARM7 a main RAM data access: nonsequential, or the next word of an LDM or STM. */
static void data7(rl_nds *m, bool write, bool seq, unsigned size)
{
    seq = seq && !ram_cut(m, RL_NDS_ARM7);
    if (!seq)
        nds_ram_wait(m, RL_NDS_ARM7);
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
    m->ram_side = NDS_RAM_CPU7;
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

/**
 * @brief True when the display reads what a store to addr changes: palette,
 * VRAM, OAM, its registers, VRAMCNT and the 3D engine's registers (a command
 * may swap the buffers the 3D layer's lines come from).
 */
static bool display_reads(unsigned cpu, uint32_t addr)
{
    uint32_t off = addr & 0x00FFFFFCu;
    if (nds_seen_by_display(cpu, addr))
        return true;
    return cpu == RL_NDS_ARM9 && (addr >> 24) == 0x04 && (nds_disp_reg(cpu, off) || nds_gx_reg(cpu, off) || (off >= 0x240 && off < 0x24C));
}

void nds_bus_store(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value, uint64_t now, bool *main)
{
    nds_where w = nds_decode_bus(m, cpu, addr, true);
    *main = w.main;
    if (display_reads(cpu, addr))
        nds_disp_catch_up(m, now);
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
                tcm_charge(m, p, seq, false);
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
                return w.kind == NDS_K_MEM ? nds_ld(w.p, size) : bus_read(m, cpu, addr, size, w);
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
            if (w.main && how == RL_NDS_S && ram_cut(m, RL_NDS_ARM9))
                how = RL_NDS_N;
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

/*
 * The ARM9's uncached code fetches, from the console's timings: the CPU
 * reads code from the bus a word at a time, so a Thumb fetch in the word it
 * read last costs nothing and any other costs a word (Thumb ADDS, SUBS and
 * BNE in main RAM 54 cycles, 3 words; a Thumb B to the next instruction 90).
 * An instruction's internal cycles overlap the fetch before them (MUL and an
 * ADD with a register shift cost what MOV does there, 90 cycles a loop).
 */

/** @brief The address an ARM9 code fetch reads (rl_nds_code_cost names it by the instruction or the target). */
static uint32_t fetch9_at(rl_nds *m, uint32_t addr, unsigned size, bool seq)
{
    uint32_t mark = addr | 1u;
    if (!seq) {
        m->fetch9_refill = mark;
        return addr;
    }
    bool second = m->fetch9_refill == mark;
    m->fetch9_refill = 0;
    return addr + (second ? size : 2u * size);
}

unsigned rl_nds_code_cost(rl_nds *m, uint32_t addr, unsigned size, bool seq)
{
    uint32_t at = fetch9_at(m, addr, size, seq);
    m->fetch9_add = m->fetch9_drop = 0;
    m->fetch9_flush = false;
    if (nds_cache_attr(m, addr) & NDS_ATTR_IC) {
        m->fetch9_word = 1;
        m->fetch9_end = 0;
        if (!nds_cache_read(m, addr, true, false))
            m->fetch9_drop = m->cache.i_evicted;
        if (seq)
            m->fetch9_add = (addr & ~31u) | 1u;
        return 0;
    }
    m->fetch9_flush = true;
    nds_cache_before_bus(m);
    uint32_t word = at & ~3u;
    unsigned cost = size == 2 && word == m->fetch9_word ? 0u : nds_cost(m, RL_NDS_ARM9, RL_NDS_CODE, addr, 4, false);
    m->fetch9_word = word;
    bool main = cost && addr >> 24 == 0x02;
    if (main) {
        nds_order(m, RL_NDS_ARM9);
        nds_ram_wait(m, RL_NDS_ARM9);
    }
    uint64_t now = nds_now(m, RL_NDS_ARM9);
    unsigned back = 0;
    if (cost && m->fetch9_end && now > m->fetch9_end) {
        uint64_t idle = now - m->fetch9_end;
        back = (unsigned)(idle < m->fetch9_cost ? idle : m->fetch9_cost);
        if (back > cost)
            back = cost;
    }
    uint64_t start = (m->fetch9_end > now ? m->fetch9_end : now) - back;
    unsigned held = 0;
    if (main && ram_cut(m, RL_NDS_ARM9) && nds_ram_busy(m) > start)
        held = (unsigned)(nds_ram_busy(m) - start);
    if (cost) {
        m->fetch9_end = start + held + cost;
        m->fetch9_cost = cost;
    }
    if (main) {
        if (m->fetch9_end > m->ram_free)
            m->ram_free = m->fetch9_end;
        m->ram_side = NDS_RAM_CPU9;
    }
    return held + cost - back;
}

void rl_nds_code_lines(const rl_nds *m, uint32_t *add, uint32_t *drop, bool *flush)
{
    *add = m->fetch9_add;
    *drop = m->fetch9_drop;
    *flush = m->fetch9_flush;
}

uint32_t rl_nds_fetch(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, bool seq)
{
    return timed_read(m, cpu, addr, size, seq ? RL_NDS_S : RL_NDS_N, true);
}

uint32_t rl_nds_load(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, unsigned how)
{
    if (cpu == RL_NDS_ARM9)
        m->fetch9_end = 0;
    return timed_read(m, cpu, addr, size, how, false);
}

void rl_nds_store(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value, unsigned how)
{
    bool seq = how == RL_NDS_S;
    if (cpu == RL_NDS_ARM9)
        m->fetch9_end = 0;
    addr &= ~(size - 1);
    value &= nds_mask(size);
    if (cpu == RL_NDS_ARM9) {
        uint8_t *p = nds_tcm(m, addr, false, true);
        if (p) {
            tcm_charge(m, p, seq, true);
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
    if (cpu == RL_NDS_ARM9 && w.main && seq && ram_cut(m, RL_NDS_ARM9)) {
        seq = false;
        how = RL_NDS_N;
    }
    unsigned cost = nds_cost(m, cpu, RL_NDS_DATA, addr, size == 1 && w.main ? 4u : size, seq);
    if (cpu == RL_NDS_ARM9) {
        unsigned posted = w.main ? NDS_MAIN_WRITE_POSTED : 0;
        bus9(m, w.main, how, seq ? cost : cost - posted, posted);
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
