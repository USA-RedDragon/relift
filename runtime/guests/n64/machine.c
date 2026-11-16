/**
 * @file machine.c
 * @brief The physical memory map and the registers of MI, RI, the RDRAM
 * modules, SP, DP, AI, SI and PIF RAM, and the high-level boot.
 *
 * Every register is a word; a narrower access reads the word and takes its
 * bytes, and a narrower write writes its bytes shifted into the word as the
 * CPU's bus puts them out. The RSP and the RDP do not run yet: a start of
 * the RSP is counted and the RSP halts again at once, as if it broke.
 */
#include "n64_int.h"

#include <stdlib.h>
#include <string.h>

uint64_t rl_n64_now(const rl_n64 *m) { return m->k ? rl_sched_now(m->k) : 0; }

static void mi_update(rl_n64 *m)
{
    if (!m->s)
        return;
    if (m->mi.intr & m->mi.mask)
        rl_sched_raise(m->s, RL_N64_LINE_MI);
    else
        rl_sched_lower(m->s, RL_N64_LINE_MI);
}

void rl_n64_mi_raise(rl_n64 *m, uint32_t bits)
{
    m->mi.intr |= bits;
    mi_update(m);
}

void rl_n64_mi_lower(rl_n64 *m, uint32_t bits)
{
    m->mi.intr &= ~bits;
    mi_update(m);
}

uint32_t rl_n64_irq_pending(const rl_n64 *m) { return m->mi.intr & m->mi.mask; }

rl_n64 *rl_n64_create(const uint8_t *rom, size_t rom_len)
{
    rl_n64 *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->rdram = calloc(1, RL_N64_RDRAM_SIZE);
    if (!m->rdram) {
        free(m);
        return NULL;
    }
    m->rom = rom;
    m->rom_len = rom_len;
    m->sp.status = 1;
    return m;
}

void rl_n64_destroy(rl_n64 *m)
{
    if (!m)
        return;
    if (m->s) {
        rl_sched_cancel(m->s, &m->pi.done);
        rl_sched_cancel(m->s, &m->vi.line);
    }
    free(m->code.state);
    free(m->code.order);
    free(m->code.filled);
    free(m->rdram);
    free(m);
}

int rl_n64_attach(rl_n64 *m, rl_sched *s, rl_sched_cpu *k)
{
    if (!m || !s || !k)
        return -1;
    m->s = s;
    m->k = k;
    rl_n64_pi_attach(m);
    rl_n64_vi_attach(m);
    mi_update(m);
    return 0;
}

uint8_t *rl_n64_rdram(rl_n64 *m) { return m->rdram; }
uint8_t *rl_n64_spmem(rl_n64 *m) { return m->spmem; }
const rl_n64_stats *rl_n64_get_stats(const rl_n64 *m) { return &m->stats; }

static uint32_t ld32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static void st32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static uint32_t mi_read(rl_n64 *m, unsigned i)
{
    switch (i) {
    case 0:
        return m->mi.mode;
    case 1:
        return 0x02020102u;
    case 2:
        return m->mi.intr;
    case 3:
        return m->mi.mask;
    default:
        return 0;
    }
}

static void mi_write(rl_n64 *m, unsigned i, uint32_t v)
{
    if (i == 0) {
        m->mi.mode = (m->mi.mode & ~0x7Fu) | (v & 0x7Fu);
        if (v & 0x80u)
            m->mi.mode &= ~0x80u;
        if (v & 0x100u)
            m->mi.mode |= 0x80u;
        if (v & 0x200u)
            m->mi.mode &= ~0x100u;
        if (v & 0x400u)
            m->mi.mode |= 0x100u;
        if (v & 0x800u)
            rl_n64_mi_lower(m, RL_N64_IRQ_DP);
        if (v & 0x1000u)
            m->mi.mode &= ~0x200u;
        if (v & 0x2000u)
            m->mi.mode |= 0x200u;
    } else if (i == 3) {
        for (unsigned b = 0; b < 6; b++) {
            if (v & (1u << (2 * b)))
                m->mi.mask &= ~(1u << b);
            if (v & (1u << (2 * b + 1)))
                m->mi.mask |= 1u << b;
        }
        mi_update(m);
    }
}

/** @brief Copies an SP DMA: count rows of len bytes, skip bytes apart in RDRAM, in direction to_rdram. */
static void sp_dma(rl_n64 *m, uint32_t reg, bool to_rdram)
{
    uint32_t len = (reg & 0xFF8u) + 8u, count = (reg >> 12 & 0xFFu) + 1u, skip = reg >> 20 & 0xFF8u;
    uint32_t mem = m->sp.mem_addr & 0x1FF8u, bank = mem & 0x1000u, dram = m->sp.dram_addr & 0xFFFFF8u;
    uint32_t first = dram;
    m->stats.sp_dmas++;
    for (uint32_t r = 0; r < count; r++) {
        for (uint32_t k = 0; k < len; k++) {
            uint32_t s = bank | ((mem + k) & 0xFFFu);
            uint32_t d = (dram + k) & (RL_N64_RDRAM_SIZE - 1u);
            if (to_rdram)
                m->rdram[d] = m->spmem[s];
            else
                m->spmem[s] = m->rdram[d];
        }
        mem = bank | ((mem + len) & 0xFFFu);
        dram += len + skip;
    }
    m->sp.mem_addr = mem;
    m->sp.dram_addr = dram;
    if (to_rdram)
        rl_n64_code_written(m, first, dram - first);
}

static uint32_t sp_read(rl_n64 *m, unsigned i)
{
    switch (i) {
    case 0:
        return m->sp.mem_addr;
    case 1:
        return m->sp.dram_addr;
    case 2:
        return m->sp.rd_len;
    case 3:
        return m->sp.wr_len;
    case 4:
        return m->sp.status;
    case 5:
    case 6:
        return 0;
    case 7: {
        uint32_t v = m->sp.semaphore;
        m->sp.semaphore = 1;
        return v;
    }
    default:
        return 0;
    }
}

static void sp_status_write(rl_n64 *m, uint32_t v)
{
    uint32_t st = m->sp.status;
    if (v & 0x2u)
        st |= 0x1u;
    if (v & 0x4u)
        st &= ~0x2u;
    if (v & 0x8u)
        rl_n64_mi_lower(m, RL_N64_IRQ_SP);
    if (v & 0x10u)
        rl_n64_mi_raise(m, RL_N64_IRQ_SP);
    if (v & 0x20u)
        st &= ~0x20u;
    if (v & 0x40u)
        st |= 0x20u;
    if (v & 0x80u)
        st &= ~0x40u;
    if (v & 0x100u)
        st |= 0x40u;
    for (unsigned b = 0; b < 8; b++) {
        if (v & (1u << (9 + 2 * b)))
            st &= ~(1u << (7 + b));
        if (v & (1u << (10 + 2 * b)))
            st |= 1u << (7 + b);
    }
    if ((v & 0x1u) && (st & 0x1u)) {
        m->stats.rsp_starts++;
        rl_report_unknown(0x4E36345350ull, "n64", "the game starts the RSP, which does not run yet; it halts at once");
        st |= 0x2u;
        if (st & 0x40u)
            rl_n64_mi_raise(m, RL_N64_IRQ_SP);
    }
    m->sp.status = st;
}

static void sp_write(rl_n64 *m, unsigned i, uint32_t v)
{
    switch (i) {
    case 0:
        m->sp.mem_addr = v & 0x1FF8u;
        break;
    case 1:
        m->sp.dram_addr = v & 0xFFFFF8u;
        break;
    case 2:
        m->sp.rd_len = v;
        sp_dma(m, v, false);
        break;
    case 3:
        m->sp.wr_len = v;
        sp_dma(m, v, true);
        break;
    case 4:
        sp_status_write(m, v);
        break;
    case 7:
        m->sp.semaphore = 0;
        break;
    default:
        break;
    }
}

static uint32_t ai_read(rl_n64 *m, unsigned i)
{
    switch (i) {
    case 1:
        return 0;
    case 3:
        return 0x00100000u;
    default:
        return m->ai.dram;
    }
}

static void ai_write(rl_n64 *m, unsigned i, uint32_t v)
{
    switch (i) {
    case 0:
        m->ai.dram = v & 0xFFFFF8u;
        break;
    case 1:
        m->ai.len = v & 0x3FFF8u;
        break;
    case 2:
        m->ai.control = v & 1u;
        break;
    case 3:
        rl_n64_mi_lower(m, RL_N64_IRQ_AI);
        break;
    case 4:
        m->ai.dacrate = v & 0x3FFFu;
        break;
    case 5:
        m->ai.bitrate = v & 0xFu;
        break;
    default:
        break;
    }
}

static void si_dma(rl_n64 *m, bool to_pif)
{
    uint32_t d = m->si.dram & (RL_N64_RDRAM_SIZE - 64u) & ~7u;
    if (to_pif)
        memcpy(m->pif_ram, m->rdram + d, 64);
    else {
        memcpy(m->rdram + d, m->pif_ram, 64);
        rl_n64_code_written(m, d, 64);
    }
    m->si.interrupt = true;
    rl_n64_mi_raise(m, RL_N64_IRQ_SI);
}

static uint32_t si_read(rl_n64 *m, unsigned i)
{
    switch (i) {
    case 0:
        return m->si.dram;
    case 1:
        return m->si.pif_rd;
    case 4:
        return m->si.pif_wr;
    case 6:
        return m->si.interrupt ? 0x1000u : 0u;
    default:
        return 0;
    }
}

static void si_write(rl_n64 *m, unsigned i, uint32_t v)
{
    switch (i) {
    case 0:
        m->si.dram = v & 0xFFFFF8u;
        break;
    case 1:
        m->si.pif_rd = v;
        si_dma(m, false);
        break;
    case 4:
        m->si.pif_wr = v;
        si_dma(m, true);
        break;
    case 6:
        m->si.interrupt = false;
        rl_n64_mi_lower(m, RL_N64_IRQ_SI);
        break;
    default:
        break;
    }
}

/** @brief A word read at a word-aligned physical address outside RDRAM. */
static uint32_t read_word(rl_n64 *m, uint32_t a)
{
    if (a >= 0x03F00000u && a < 0x04000000u)
        return m->rdram_regs[(a >> 2) & 15u];
    if (a >= 0x04000000u && a < 0x04040000u)
        return ld32(m->spmem + (a & 0x1FFCu));
    unsigned r = (a >> 2) & 15u;
    switch (a >> 20) {
    case 0x040:
        if (a == 0x04080000u)
            return m->sp.pc;
        return (a & 0xFFFE0u) == 0x40000u ? sp_read(m, r) : 0;
    case 0x041:
        return r == 3 ? 0x80u : m->dp[r & 7u];
    case 0x043:
        return mi_read(m, r);
    case 0x044:
        return rl_n64_vi_read(m, r);
    case 0x045:
        return ai_read(m, r);
    case 0x046:
        return rl_n64_pi_read(m, r);
    case 0x047:
        return m->ri.regs[r & 7u];
    case 0x048:
        return si_read(m, r);
    default:
        break;
    }
    if (a >= 0x05000000u && a < 0x1FC00000u)
        return rl_n64_cart_read(m, a);
    if (a >= 0x1FC007C0u && a < 0x1FC00800u)
        return ld32(m->pif_ram + (a & 0x3Cu));
    return 0;
}

static void write_word(rl_n64 *m, uint32_t a, uint32_t v)
{
    if (a >= 0x03F00000u && a < 0x04000000u) {
        m->rdram_regs[(a >> 2) & 15u] = v;
        return;
    }
    if (a >= 0x04000000u && a < 0x04040000u) {
        st32(m->spmem + (a & 0x1FFCu), v);
        return;
    }
    unsigned r = (a >> 2) & 15u;
    switch (a >> 20) {
    case 0x040:
        if (a == 0x04080000u)
            m->sp.pc = v & 0xFFCu;
        else if ((a & 0xFFFE0u) == 0x40000u)
            sp_write(m, r, v);
        return;
    case 0x041:
        m->dp[r & 7u] = v;
        return;
    case 0x043:
        mi_write(m, r, v);
        return;
    case 0x044:
        rl_n64_vi_write(m, r, v);
        return;
    case 0x045:
        ai_write(m, r, v);
        return;
    case 0x046:
        rl_n64_pi_write(m, r, v);
        return;
    case 0x047:
        m->ri.regs[r & 7u] = v;
        return;
    case 0x048:
        si_write(m, r, v);
        return;
    default:
        break;
    }
    if (a >= 0x1FC007C0u && a < 0x1FC00800u)
        st32(m->pif_ram + (a & 0x3Cu), v);
}

uint64_t rl_n64_read(rl_n64 *m, uint32_t paddr, uint32_t size)
{
    paddr &= 0x1FFFFFFFu;
    if (paddr < RL_N64_RDRAM_SIZE) {
        uint64_t v = 0;
        for (uint32_t i = 0; i < size; i++)
            v = v << 8 | m->rdram[(paddr + i) & (RL_N64_RDRAM_SIZE - 1u)];
        return v;
    }
    if (size == 8)
        return (uint64_t)read_word(m, paddr & ~7u) << 32 | read_word(m, (paddr & ~7u) + 4u);
    uint32_t w = read_word(m, paddr & ~3u);
    if (size == 4)
        return w;
    unsigned shift = (4u - size - (paddr & 3u)) * 8u;
    return (w >> shift) & (size == 1 ? 0xFFu : 0xFFFFu);
}

void rl_n64_write(rl_n64 *m, uint32_t paddr, uint32_t size, uint64_t value)
{
    paddr &= 0x1FFFFFFFu;
    if (paddr < RL_N64_RDRAM_SIZE) {
        for (uint32_t i = 0; i < size; i++)
            m->rdram[(paddr + i) & (RL_N64_RDRAM_SIZE - 1u)] = (uint8_t)(value >> (8u * (size - 1u - i)));
        return;
    }
    if (size == 8) {
        write_word(m, paddr & ~7u, (uint32_t)(value >> 32));
        write_word(m, (paddr & ~7u) + 4u, (uint32_t)value);
        return;
    }
    unsigned shift = size == 4 ? 0 : (4u - size - (paddr & 3u)) * 8u;
    write_word(m, paddr & ~3u, (uint32_t)value << shift);
}

void rl_n64_boot(rl_n64 *m, const rl_n64_boot_info *b)
{
    size_t head = m->rom_len < 0x1000u ? m->rom_len : 0x1000u;
    memcpy(m->spmem, m->rom, head);
    uint32_t dram = b->entry & 0x1FFFFFFFu;
    uint32_t len = 0x100000u;
    if (m->rom_len < 0x1000u)
        len = 0;
    else if (m->rom_len - 0x1000u < len)
        len = (uint32_t)(m->rom_len - 0x1000u);
    if (dram + len > RL_N64_RDRAM_SIZE)
        len = dram < RL_N64_RDRAM_SIZE ? RL_N64_RDRAM_SIZE - dram : 0;
    memcpy(m->rdram + dram, m->rom + 0x1000u, len);
    rl_n64_code_loaded(m, 0, 0x1000u, dram, len);
    m->pi.dram = (dram + len) & 0xFFFFFEu;
    m->pi.cart = 0x10001000u + len;
    m->pi.wr_len = 0x7Fu;
    if (m->rom_len >= 4) {
        m->pi.bsd[0].latency = m->rom[3];
        m->pi.bsd[0].pulse_width = m->rom[2];
        m->pi.bsd[0].page_size = m->rom[1] & 0xFu;
        m->pi.bsd[0].release = (m->rom[1] >> 4) & 3u;
    }
    m->ri.regs[0] = 0x0Eu;
    m->ri.regs[1] = 0x40u;
    m->ri.regs[3] = 0x14u;
    m->ri.regs[4] = 0x00063634u;
    uint32_t tv = m->rom_len > 0x3E && (m->rom[0x3E] == 'P' || m->rom[0x3E] == 'D' || m->rom[0x3E] == 'F' ||
                                        m->rom[0x3E] == 'I' || m->rom[0x3E] == 'S' || m->rom[0x3E] == 'U' ||
                                        m->rom[0x3E] == 'X' || m->rom[0x3E] == 'Y')
                      ? 0u
                      : 1u;
    st32(m->rdram + 0x300u, tv);
    st32(m->rdram + 0x304u, 0);
    st32(m->rdram + 0x308u, 0xB0000000u);
    st32(m->rdram + 0x30Cu, 0);
    st32(m->rdram + 0x314u, 0);
    st32(m->rdram + (b->cic == 6105 ? 0x3F0u : 0x318u), RL_N64_RDRAM_SIZE);
}
