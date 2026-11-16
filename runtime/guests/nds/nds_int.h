/**
 * @file nds_int.h
 * @brief The machine record and the helpers the files of this layer share.
 */
#ifndef RELIFT_NDS_INT_H
#define RELIFT_NDS_INT_H

#include "nds.h"

/** @brief A tick that never comes. */
#define NDS_NEVER UINT64_MAX

/** @brief Bytes of each CPU's I/O register file stub (0x04000000 to 0x04001FFF). */
#define NDS_REGS 0x2000u

/** @brief Ticks from a request that meets IME and IE to the CPU's IRQ input (the spike's value, not measured). */
#define NDS_IRQ_DELAY 2u
/** @brief Words of each IPC FIFO. */
#define NDS_FIFO_WORDS 16u
/** @brief Offset of IPCFIFORECV from the I/O base. */
#define NDS_FIFORECV 0x100000u

/** @brief Bytes of one watched line of code memory. */
#define NDS_LINE 32u
/** @brief First watch line of each code memory: main RAM, shared work RAM, ARM7 work RAM, ITCM. */
enum {
    NDS_WATCH_MAIN = 0,
    NDS_WATCH_SWRAM = NDS_WATCH_MAIN + RL_NDS_MAIN_SIZE / NDS_LINE,
    NDS_WATCH_WRAM7 = NDS_WATCH_SWRAM + RL_NDS_SWRAM_SIZE / NDS_LINE,
    NDS_WATCH_ITCM = NDS_WATCH_WRAM7 + RL_NDS_WRAM7_SIZE / NDS_LINE,
    NDS_WATCH_LINES = NDS_WATCH_ITCM + RL_CP15_ITCM_SIZE / NDS_LINE,
};

/** @brief The interrupt controller of one CPU. */
typedef struct nds_irq {
    uint32_t ie, iflag;
    bool ime;
    /** @brief The tick from which the IRQ input is asserted, NDS_NEVER while IME, IE and IF do not meet. */
    uint64_t line_at;
    /** @brief The tick from which IE & IF wakes a halted CPU, NDS_NEVER while it is zero. */
    uint64_t wake_at;
} nds_irq;

/** @brief One IPC FIFO (the send FIFO of one CPU). */
typedef struct nds_fifo {
    uint32_t q[NDS_FIFO_WORDS];
    uint8_t rd, n;
} nds_fifo;

/** @brief IPCSYNC and the FIFOs. */
typedef struct nds_ipc {
    /** @brief IPCSYNC bits 8 to 11 and 14 written by each CPU. */
    uint8_t sync_out[2];
    bool sync_irq[2];
    /** @brief IPCFIFOCNT bits 2, 10, 14 and 15 of each CPU. */
    uint16_t cnt[2];
    /** @brief fifo[c] is the send FIFO of CPU c. */
    nds_fifo fifo[2];
    /** @brief The word each CPU received last (what an empty IPCFIFORECV reads). */
    uint32_t last[2];
    /** @brief The IRQ conditions at the last change, for the edges. */
    bool send_cond[2], recv_cond[2];
} nds_ipc;

/** @brief One timer, its counter in bus cycles (GBA timer.c's closed form). */
typedef struct nds_timer {
    uint32_t val;
    uint64_t at;
    uint64_t load;
    uint16_t reload;
    uint8_t ctl;
    uint8_t cpu, n;
    rl_nds *m;
    rl_event ev;
} nds_timer;

/** @brief The ARM9's division and square root unit. */
typedef struct nds_math {
    uint16_t divcnt, sqrtcnt;
    uint64_t numer, denom, result, rem, param;
    uint32_t sqrt_result;
    /** @brief The ticks at which the busy bits clear. */
    uint64_t div_done, sqrt_done;
} nds_math;

/** @brief What an address decodes to. */
typedef enum nds_kind {
    /** @brief Nothing: reads 0, writes are dropped. */
    NDS_K_NONE,
    /** @brief Memory at nds_where.p. */
    NDS_K_MEM,
    /** @brief The CPU's I/O registers. */
    NDS_K_IO,
    /** @brief A BIOS region: a high-level stub with no image, reads 0. */
    NDS_K_BIOS,
    /** @brief The GBA slot with no cartridge (open bus). */
    NDS_K_SLOT,
} nds_kind;

/** @brief The decode of one address. */
typedef struct nds_where {
    /** @brief The byte at the aligned address, for NDS_K_MEM; NULL for writes that are dropped. */
    uint8_t *p;
    /** @brief The kind. */
    uint8_t kind;
    /** @brief True for main RAM (the contended region). */
    bool main;
    /** @brief True for an ARM9 TCM hit. */
    bool tcm;
} nds_where;

/** @brief A device of this layer. */
typedef struct nds_dev {
    rl_device dev;
    rl_nds *m;
} nds_dev;

/** @brief The machine. */
struct rl_nds {
    uint8_t main[RL_NDS_MAIN_SIZE];
    uint8_t swram[RL_NDS_SWRAM_SIZE];
    uint8_t wram7[RL_NDS_WRAM7_SIZE];
    uint8_t itcm[RL_CP15_ITCM_SIZE];
    uint8_t dtcm[RL_CP15_DTCM_SIZE];
    uint8_t pal[RL_NDS_PAL_SIZE];
    uint8_t oam[RL_NDS_OAM_SIZE];
    uint8_t vram[RL_NDS_VRAM_SIZE];
    /** @brief Register file stubs of the registers no device models yet, per CPU. */
    uint8_t regs[2][NDS_REGS];

    rl_sched *sched;
    rl_sched_cpu *cpu[2];

    rl_arm32_cp15 cp15;
    rl_arm32_tcm tcm;

    uint8_t wramcnt;
    /** @brief EXMEMCNT of the ARM9 and the ARM7's own bits 0 to 6. */
    uint16_t exmem[2];
    uint8_t postflg[2];
    uint8_t vramcnt[10];
    /** @brief The ARM7 wrote HALTCNT: the store ends in a halt. */
    bool halt_req;
    /** @brief The CPU waits in rl_nds_halt. */
    bool halted[2];
    /** @brief A HALTCNT halt of the CPU found nothing that could wake it (rl_nds_halt_lost). */
    bool halt_lost[2];

    /** @brief One bit per NDS_LINE bytes of code memory (NDS_WATCH_*): lifted code was checked against it. */
    uint8_t watch[NDS_WATCH_LINES / 8];
    /** @brief Writes to watched lines so far (rl_nds_code_epoch). */
    uint64_t code_epoch;

    nds_irq irq[2];
    nds_ipc ipc;
    nds_timer tm[2][4];
    nds_math math;

    /** @brief The tick from which main RAM is free. */
    uint64_t ram_free;
    rl_nds_costs costs;

    nds_dev d_io;
};

/** @brief The low size bytes set (size 1, 2 or 4). */
static inline uint32_t nds_mask(unsigned size) { return size >= 4 ? UINT32_MAX : (1u << (8 * size)) - 1u; }

/** @brief A little-endian load of size bytes at p. */
static inline uint32_t nds_ld(const uint8_t *p, unsigned size)
{
    uint32_t v = 0;
    for (unsigned i = 0; i < size; i++)
        v |= (uint32_t)p[i] << (8 * i);
    return v;
}

/** @brief A little-endian store of size bytes of v at p. */
static inline void nds_st(uint8_t *p, unsigned size, uint32_t v)
{
    for (unsigned i = 0; i < size; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

/** @brief The tick of cpu now (0 before rl_nds_attach). */
static inline uint64_t nds_now(const rl_nds *m, unsigned cpu) { return m->cpu[cpu] ? rl_sched_now(m->cpu[cpu]) : 0; }
/** @brief Charges ticks to cpu (an ARM7 charge is rounded up to whole cycles). */
void nds_charge(rl_nds *m, unsigned cpu, uint64_t ticks);

/** @brief The ARM9's TCM byte at addr for this kind of access, or NULL when the access goes to the bus (ITCM over DTCM; load mode sends reads to the bus). */
static inline uint8_t *nds_tcm(rl_nds *m, uint32_t addr, bool code, bool write)
{
    const rl_arm32_tcm *t = &m->tcm;
    if (t->itcm_on && addr < t->itcm_size && (write || !t->itcm_load))
        return &m->itcm[addr & (RL_CP15_ITCM_SIZE - 1)];
    if (!code && t->dtcm_on && (uint64_t)(addr - t->dtcm_base) < t->dtcm_size && (write || !t->dtcm_load))
        return &m->dtcm[addr & (RL_CP15_DTCM_SIZE - 1)];
    return NULL;
}

/** @brief The watch line of the code memory byte p, or -1 when p is not in code memory. */
int nds_watch_line(const rl_nds *m, const uint8_t *p);
/** @brief A store to the bytes at p: moves the code epoch when they are in a watched line. */
static inline void nds_watch_store(rl_nds *m, const uint8_t *p)
{
    int l = nds_watch_line(m, p);
    if (l >= 0 && (m->watch[l >> 3] >> (l & 7) & 1u))
        m->code_epoch++;
}

/** @brief Decodes addr (aligned) for cpu; code true for an instruction fetch, write for a store. */
nds_where nds_decode(rl_nds *m, unsigned cpu, uint32_t addr, bool code, bool write);
/** @brief Rebuilds the GBA slot rows of the cost table from EXMEMCNT. */
void nds_costs_rebuild(rl_nds *m);
/** @brief The open bus value of the GBA slot for cpu at addr. */
uint32_t nds_slot_read(const rl_nds *m, unsigned cpu, uint32_t addr, unsigned size);

/** @brief An I/O read of size bytes at addr of cpu at tick now (any side effect included). */
uint32_t nds_io_read(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint64_t now);
/** @brief An I/O write. */
void nds_io_write(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value, uint64_t now);
/** @brief Power-on values of the I/O registers. */
void nds_io_reset(rl_nds *m);
/** @brief Adds the I/O device to the scheduler and maps both CPUs' I/O. */
int nds_io_attach(rl_nds *m);

/** @brief Power-on state of both interrupt controllers. */
void nds_irq_reset(rl_nds *m);
/** @brief Recomputes cpu's IRQ input and wake condition after a change at tick. */
void nds_irq_update(rl_nds *m, unsigned cpu, uint64_t tick);
/** @brief Sets bits in cpu's IF at tick. */
void nds_irq_set(rl_nds *m, unsigned cpu, uint32_t bits, uint64_t tick);
/** @brief IME, IE or IF (word offset off). */
uint32_t nds_irq_read(rl_nds *m, unsigned cpu, uint32_t off);
/** @brief A write of the mask bits of v to IME, IE or IF (acknowledge) at tick now. */
void nds_irq_write(rl_nds *m, unsigned cpu, uint32_t off, uint32_t v, uint32_t mask, uint64_t now);

/** @brief Power-on state of IPCSYNC and the FIFOs. */
void nds_ipc_reset(rl_nds *m);
/** @brief IPCSYNC, IPCFIFOCNT or IPCFIFORECV (which pops) at word offset off. */
uint32_t nds_ipc_read(rl_nds *m, unsigned cpu, uint32_t off, uint64_t now);
/** @brief A write of the mask bits of v to IPCSYNC, IPCFIFOCNT or IPCFIFOSEND. */
void nds_ipc_write(rl_nds *m, unsigned cpu, uint32_t off, uint32_t v, uint32_t mask, uint64_t now);

/** @brief True when cpu's bus access at addr reaches state the other CPU shares (rl_nds_ordered without the TCMs). */
bool nds_shared(unsigned cpu, uint32_t addr);
/** @brief Waits until the other CPU is past cpu in the order (rl_sched_order); nothing before rl_nds_attach. */
void nds_order(rl_nds *m, unsigned cpu);
/** @brief Sets the CPUs' ranks from EXMEMCNT bit 15. */
void nds_ranks(rl_nds *m);
/** @brief Raises or lowers cpu's wake line after IE & IF changed; a halted CPU's clock moves to the wake tick. */
void nds_wake_line(rl_nds *m, unsigned cpu);
/** @brief Routes the wake lines and sets the masks and ranks. */
int nds_order_attach(rl_nds *m);

/** @brief Power-on state of the eight timers. */
void nds_timer_reset(rl_nds *m);
/** @brief Starts the timers' clocks at the CPUs' current ticks. */
void nds_timer_attach(rl_nds *m);
/** @brief TMxCNT_L (counter) and TMxCNT_H (control) of word offset off at tick now. */
uint32_t nds_timer_read(rl_nds *m, unsigned cpu, uint32_t off, uint64_t now);
/** @brief A write of the mask bits of v to the reload and control of word offset off at tick now. */
void nds_timer_write(rl_nds *m, unsigned cpu, uint32_t off, uint32_t v, uint32_t mask, uint64_t now);

/** @brief Power-on state of the math unit. */
void nds_math_reset(rl_nds *m);
/** @brief A math unit register at word offset off at tick now (the busy bits are time-dependent). */
uint32_t nds_math_read(rl_nds *m, uint32_t off, uint64_t now);
/** @brief A write of the mask bits of v to a math unit register; control and parameter writes start the unit. */
void nds_math_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask, uint64_t now);

/** @brief The scheduler address of cpu's I/O address addr: the ARM7's registers sit 4 GiB above the ARM9's. */
static inline uint64_t nds_io_key(unsigned cpu, uint32_t addr) { return (uint64_t)cpu << 32 | addr; }

#endif
