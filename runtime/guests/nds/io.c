/**
 * @file io.c
 * @brief The I/O registers of both CPUs as one scheduler device: the word
 * dispatch with byte lanes to the interrupt controllers (irq.c), IPC
 * (ipc.c), the timers (timer.c), DMA (dma.c), the ARM9's math unit (math.c),
 * the display timing (lcd.c), the
 * system registers (EXMEMCNT and EXMEMSTAT,
 * WRAMCNT and WRAMSTAT, VRAMCNT, POSTFLG, HALTCNT) and register file stubs
 * for the registers no device models yet.
 *
 * Fresh C from GBATEK ("DS I/O Maps", "DS Memory Control").
 */
#include "nds_int.h"

#include <stdio.h>
#include <string.h>

static uint32_t regs_word(const rl_nds *m, unsigned cpu, uint32_t off)
{
    const uint8_t *p = &m->regs[cpu][off];
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void regs_put(rl_nds *m, unsigned cpu, uint32_t off, uint32_t v, uint32_t mask)
{
    for (unsigned i = 0; i < 4; i++)
        if (mask >> (8 * i) & 0xFF)
            m->regs[cpu][off + i] = (uint8_t)(v >> (8 * i));
}

static uint16_t exmem_read(const rl_nds *m, unsigned cpu)
{
    if (cpu == RL_NDS_ARM9)
        return (uint16_t)(m->exmem[RL_NDS_ARM9] | 0x6000);
    return (uint16_t)((m->exmem[RL_NDS_ARM7] & 0x7F) | ((m->exmem[RL_NDS_ARM9] & 0xFF80) | 0x6000));
}

static void exmem_write(rl_nds *m, unsigned cpu, uint16_t v, uint16_t mask)
{
    uint16_t own = mask & 0x7F;
    m->exmem[cpu] = (uint16_t)((m->exmem[cpu] & ~own) | (v & own));
    if (cpu == RL_NDS_ARM9) {
        uint16_t shared = mask & 0xE880;
        m->exmem[RL_NDS_ARM9] = (uint16_t)((m->exmem[RL_NDS_ARM9] & ~shared) | (v & shared));
        nds_ranks(m);
    }
    nds_costs_rebuild(m);
}

static bool math_reg(unsigned cpu, uint32_t off) { return cpu == RL_NDS_ARM9 && off >= 0x280 && off < 0x2C0; }

static bool dma_reg(unsigned cpu, uint32_t off) { return off >= 0xB0 && off < (cpu == RL_NDS_ARM9 ? 0xF0u : 0xE0u); }

static uint32_t read32(rl_nds *m, unsigned cpu, uint32_t off, uint64_t now)
{
    if (nds_disp_reg(cpu, off))
        return nds_disp_read(m, off);
    if (nds_gx_reg(cpu, off))
        return nds_gx_read(m, off, now);
    if (off >= 0x100 && off < 0x110)
        return nds_timer_read(m, cpu, off, now);
    if (dma_reg(cpu, off))
        return nds_dma_read(m, cpu, off);
    if (off == 0x130 || (off == 0x134 && cpu == RL_NDS_ARM7))
        return nds_keypad_read(m, cpu, off);
    if ((off >= 0x1A0 && off < 0x1C0 && off != 0x1A8 && off != 0x1AC) || off == NDS_CARDPORT)
        return nds_cart_read(m, off, now);
    if (cpu == RL_NDS_ARM7 && off == 0x1C0)
        return nds_spi_read(m, now);
    if (cpu == RL_NDS_ARM7 && off == 0x138)
        return nds_rtc_read(m, now);
    if (math_reg(cpu, off))
        return nds_math_read(m, off, now);
    switch (off) {
    case 0x180:
    case 0x184:
    case 0x188:
    case NDS_FIFORECV:
        return nds_ipc_read(m, cpu, off, now);
    case 0x208:
    case 0x20C:
    case 0x210:
    case 0x214:
        return nds_irq_read(m, cpu, off);
    case 0x004:
        return nds_lcd_read(m, cpu, now);
    case 0x204:
        return exmem_read(m, cpu) | (cpu == RL_NDS_ARM7 && (m->powcnt2 & 2) ? (uint32_t)m->wifiwait << 16 : 0);
    case 0x240:
        if (cpu == RL_NDS_ARM7)
            return m->vramstat | (uint32_t)m->wramcnt << 8;
        return (uint32_t)m->vramcnt[0] | (uint32_t)m->vramcnt[1] << 8 | (uint32_t)m->vramcnt[2] << 16 |
               (uint32_t)m->vramcnt[3] << 24;
    case 0x244:
        if (cpu == RL_NDS_ARM7)
            return 0;
        return (uint32_t)m->vramcnt[4] | (uint32_t)m->vramcnt[5] << 8 | (uint32_t)m->vramcnt[6] << 16 |
               (uint32_t)m->wramcnt << 24;
    case 0x248:
        return cpu == RL_NDS_ARM7 ? 0 : (uint32_t)m->vramcnt[7] | (uint32_t)m->vramcnt[8] << 8;
    case 0x300:
        return m->postflg[cpu];
    default:
        if (cpu == RL_NDS_ARM7 && nds_sys_has(off))
            return nds_sys_read(m, off);
        if (off < NDS_REGS)
            return regs_word(m, cpu, off);
        return 0;
    }
}

static void write32(rl_nds *m, unsigned cpu, uint32_t off, uint32_t v, uint32_t mask, uint64_t now)
{
    if (nds_disp_reg(cpu, off)) {
        nds_disp_write(m, off, v, mask);
        return;
    }
    if (nds_gx_reg(cpu, off)) {
        nds_gx_write(m, off, v, mask, now);
        return;
    }
    if (off >= 0x100 && off < 0x110) {
        nds_timer_write(m, cpu, off, v, mask, now);
        return;
    }
    if (dma_reg(cpu, off)) {
        nds_dma_write(m, cpu, off, v, mask, now);
        return;
    }
    if (off == 0x130) {
        nds_keypad_write(m, cpu, v, mask, now);
        return;
    }
    if (off >= 0x1A0 && off < 0x1C0) {
        nds_cart_write(m, off, v, mask, now);
        return;
    }
    if (cpu == RL_NDS_ARM7 && off == 0x1C0) {
        nds_spi_write(m, v, mask, now);
        return;
    }
    if (cpu == RL_NDS_ARM7 && off == 0x138) {
        nds_rtc_write(m, v, mask, now);
        return;
    }
    if (off == 0x134 && cpu == RL_NDS_ARM7) {
        m->rcnt = (uint16_t)((m->rcnt & ~mask) | (v & mask & 0xFFFFu));
        return;
    }
    if (math_reg(cpu, off)) {
        nds_math_write(m, off, v, mask, now);
        return;
    }
    switch (off) {
    case 0x180:
    case 0x184:
    case 0x188:
    case NDS_FIFORECV:
        nds_ipc_write(m, cpu, off, v, mask, now);
        return;
    case 0x208:
    case 0x20C:
    case 0x210:
    case 0x214:
        nds_irq_write(m, cpu, off, v, mask, now);
        return;
    case 0x004:
        nds_lcd_write(m, cpu, v, mask, now);
        return;
    case 0x204:
        if (mask & 0xFFFF)
            exmem_write(m, cpu, (uint16_t)v, (uint16_t)mask);
        if (cpu == RL_NDS_ARM7 && (m->powcnt2 & 2) && (mask >> 16))
            m->wifiwait = (uint16_t)((m->wifiwait & ~(mask >> 16)) | ((v >> 16) & (mask >> 16)));
        return;
    case 0x240:
    case 0x244:
    case 0x248:
        if (cpu == RL_NDS_ARM7)
            return;
        for (unsigned i = 0; i < 4; i++) {
            if (!(mask >> (8 * i) & 0xFF))
                continue;
            uint8_t b = (uint8_t)(v >> (8 * i));
            unsigned r = off - 0x240 + i;
            if (r == 7)
                m->wramcnt = b & 3;
            else if (r < 7)
                nds_vram_write_cnt(m, r, b);
            else if (r < 10)
                nds_vram_write_cnt(m, r - 1, b);
        }
        return;
    case 0x300:
        if (mask & 0xFF)
            m->postflg[cpu] = (uint8_t)(v & (cpu == RL_NDS_ARM9 ? 3 : 1));
        if (cpu == RL_NDS_ARM7 && (mask & 0xFF00) && ((v >> 14) & 3) >= 2)
            m->halt_req = true;
        return;
    default:
        if (cpu == RL_NDS_ARM7 && nds_sys_has(off))
            nds_sys_write(m, off, v, mask);
        else if (off < NDS_REGS)
            regs_put(m, cpu, off, v, mask);
        return;
    }
}

uint32_t nds_io_read(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint64_t now)
{
    uint32_t off = addr - 0x04000000u;
    uint32_t w = read32(m, cpu, off & ~3u, now);
    return (w >> (8 * (off & 3))) & nds_mask(size);
}

/** @brief The no$gba debug port's string out (0x04FFFA10): the NUL-terminated string at addr goes to stdout. */
static void debug_string(rl_nds *m, unsigned cpu, uint32_t addr)
{
    char s[1024];
    rl_nds_code_read(m, cpu, addr, (uint8_t *)s, sizeof s - 1);
    s[sizeof s - 1] = 0;
    fputs(s, stdout);
}

void nds_io_write(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value, uint64_t now)
{
    uint32_t off = addr - 0x04000000u;
    if (off == 0xFFFA10u && size == 4) {
        debug_string(m, cpu, value);
        return;
    }
    unsigned sh = 8 * (off & 3);
    write32(m, cpu, off & ~3u, (value & nds_mask(size)) << sh, nds_mask(size) << sh, now);
}

void nds_io_reset(rl_nds *m)
{
    memset(m->regs, 0, sizeof m->regs);
    m->rcnt = 0x8000;
    memset(&m->pad, 0, sizeof m->pad);
    m->wramcnt = 0;
    m->exmem[0] = m->exmem[1] = 0;
    m->postflg[0] = m->postflg[1] = 0;
    memset(m->vramcnt, 0, sizeof m->vramcnt);
    nds_vram_rebuild(m);
    m->halt_req = false;
    nds_irq_reset(m);
    nds_ipc_reset(m);
    nds_timer_reset(m);
    nds_dma_reset(m);
    nds_sys_reset(m);
    nds_cart_reset(m);
    nds_spi_reset(m);
    nds_rtc_reset(m);
    nds_math_reset(m);
    nds_lcd_reset(m);
    nds_disp_reset(m);
    nds_gx_reset(m);
    nds_disp_set_3d(m, nds_gx_line3d, m);
}

static uint64_t io_read(rl_device *d, uint64_t addr, uint32_t size, uint64_t now)
{
    rl_nds *m = ((nds_dev *)d->opaque)->m;
    return nds_io_read(m, (unsigned)(addr >> 32), (uint32_t)addr, size, now);
}

static void io_write(rl_device *d, uint64_t addr, uint32_t size, uint64_t value, uint64_t now)
{
    rl_nds *m = ((nds_dev *)d->opaque)->m;
    nds_io_write(m, (unsigned)(addr >> 32), (uint32_t)addr, size, (uint32_t)value, now);
}

int nds_io_attach(rl_nds *m)
{
    m->d_io = (nds_dev){.dev = {.name = "nds io", .read = io_read, .write = io_write, .line = RL_LINE_NONE}, .m = m};
    m->d_io.dev.opaque = &m->d_io;
    if (rl_sched_add_device(m->sched, &m->d_io.dev) != 0 || nds_disp_add(m) != 0)
        return -1;
    for (unsigned cpu = 0; cpu < 2; cpu++)
        if (rl_sched_map(m->sched, nds_io_key(cpu, 0x04000000u), 0x01000000u, &m->d_io.dev, rl_device_bit(&m->d_disp.dev)) != 0)
            return -1;
    return 0;
}
