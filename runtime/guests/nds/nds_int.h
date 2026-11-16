/**
 * @file nds_int.h
 * @brief The machine record and the helpers the files of this layer share.
 */
#ifndef RELIFT_NDS_INT_H
#define RELIFT_NDS_INT_H

#include "gpu2d.h"
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

/** @brief Lines drawn per frame; VBlank starts at this line. */
#define NDS_VISIBLE 192u
/** @brief Ticks from the start of a line to the HBlank flag and interrupt (the oracle's 1,584 bus cycles). */
#define NDS_HBLANK_TICKS 3168u

/** @brief Bytes of one watched line of code memory. */
#define NDS_LINE 32u
/** @brief First watch line of each code memory: main RAM, shared work RAM, ARM7 work RAM, ITCM. */
enum {
    NDS_WATCH_MAIN = 0,
    NDS_WATCH_SWRAM = NDS_WATCH_MAIN + RL_NDS_MAIN_SIZE / NDS_LINE,
    NDS_WATCH_WRAM7 = NDS_WATCH_SWRAM + RL_NDS_SWRAM_SIZE / NDS_LINE,
    NDS_WATCH_ITCM = NDS_WATCH_WRAM7 + RL_NDS_WRAM7_SIZE / NDS_LINE,
    NDS_WATCH_VRAM = NDS_WATCH_ITCM + RL_CP15_ITCM_SIZE / NDS_LINE,
    NDS_WATCH_LINES = NDS_WATCH_VRAM + RL_NDS_VRAM_SIZE / NDS_LINE,
};

/** @brief The video RAM banks A to I. */
enum {
    NDS_BANK_A,
    NDS_BANK_B,
    NDS_BANK_C,
    NDS_BANK_D,
    NDS_BANK_E,
    NDS_BANK_F,
    NDS_BANK_G,
    NDS_BANK_H,
    NDS_BANK_I,
    NDS_BANKS
};

/** @brief The views of video RAM (vram.c): what the CPUs and the engines see. */
enum {
    /** @brief The ARM9's plain access, 0x06800000 mirrored every 1 MiB. */
    NDS_V_LCDC,
    /** @brief Engine A BG (512 KiB) and engine B BG (128 KiB), also the ARM9's 0x06000000 and 0x06200000. */
    NDS_V_BGA,
    NDS_V_BGB,
    /** @brief Engine A OBJ (256 KiB) and engine B OBJ (128 KiB), 0x06400000 and 0x06600000. */
    NDS_V_OBJA,
    NDS_V_OBJB,
    /** @brief The ARM7's 0x06000000 (C and D as work RAM, 256 KiB). */
    NDS_V_ARM7,
    /** @brief The four 128 KiB texture slots and the six 16 KiB texture palette slots of the 3D engine. */
    NDS_V_TEX,
    NDS_V_TEXPAL,
    /** @brief The extended palettes: BG (four 8 KiB slots) and OBJ (8 KiB) of each engine. */
    NDS_V_BGEXTA,
    NDS_V_OBJEXTA,
    NDS_V_BGEXTB,
    NDS_V_OBJEXTB,
    NDS_VIEWS
};
/** @brief Pages of all views together. */
#define NDS_VPAGES 194u

/** @brief One page of a view: the banks that answer it. */
typedef struct nds_vpage {
    /** @brief The page's first byte when exactly one bank answers it, else NULL. */
    uint8_t *p;
    /** @brief One bit per bank that answers the page. */
    uint16_t banks;
    /** @brief For each bank of banks, the offset in rl_nds.vram of the page's first byte. */
    uint32_t at[NDS_BANKS];
} nds_vpage;

/** @brief Where a view's pages sit in rl_nds.vpg. */
typedef struct nds_vview {
    /** @brief Index of its first page. */
    uint16_t first;
    /** @brief Page count minus one (a power of two minus one). */
    uint16_t pages;
    /** @brief log2 of the page size. */
    uint8_t shift;
    /** @brief The page size minus one. */
    uint32_t mask;
} nds_vview;

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

/** @brief The display timing (lcd.c). */
typedef struct nds_lcd {
    /** @brief A tick at which a frame started (line 0, dot 0). */
    uint64_t origin;
    /** @brief DISPSTAT of each CPU: the IRQ enables (bits 3 to 5) and LYC (bits 7 to 15). */
    uint16_t stat[2];
    /** @brief The next line edge that requests an interrupt. */
    rl_event ev;
} nds_lcd;

/** @brief Pixels the main memory display FIFO holds. */
#define NDS_FIFO_PX 512u

/**
 * @brief The 3D engine's line hook: fills out with line y of the 3D layer
 * (256 pixels, 18-bit color in bits 0 to 17 and alpha 0 to 31 in bits 24
 * to 28, alpha 0 transparent) and returns true, or returns false when it
 * has no picture.
 */
typedef bool nds_line3d_fn(void *user, unsigned y, uint32_t *out);

/** @brief The display device (disp.c). */
typedef struct nds_disp {
    nds_eng eng[2];
    /** @brief POWCNT1. */
    uint16_t powcnt;
    /** @brief DISPCAPCNT, and true while a capture runs (from line 0 to VBlank). */
    uint32_t capcnt;
    bool capturing;
    /** @brief The main memory display FIFO. */
    uint16_t fifo[NDS_FIFO_PX];
    uint16_t fifo_rd, fifo_n;
    /** @brief The 3D layer of the line, its hook, and whether the hook gave a picture. */
    uint32_t l3d[NDS_W];
    nds_line3d_fn *line3d_fn;
    void *line3d_user;
    bool l3d_on;
    /** @brief Forced blank of each engine at each line's render tick (a line shows white two lines on as well). */
    bool fb[2][RL_NDS_LINES];
    /** @brief The next line to process and the tick it starts at. */
    unsigned line;
    uint64_t t0;
    /** @brief The frame being drawn and the last finished one: both screens, top first, 18-bit pixels. */
    uint32_t frame[NDS_W * NDS_H * 2];
    uint32_t last[NDS_W * NDS_H * 2];
    bool have;
    uint64_t frames;
    rl_nds_frame_fn *frame_fn;
    void *frame_user;
    /** @brief True while lines are drawn (the frame hook runs inside). */
    bool busy;
    /** @brief The start of VBlank, which hands over the frame. */
    rl_event ev;
} nds_disp;

/** @brief What an address decodes to. */
typedef enum nds_kind {
    /** @brief Nothing: reads 0, writes are dropped. */
    NDS_K_NONE,
    /** @brief Memory at nds_where.p. */
    NDS_K_MEM,
    /** @brief A video RAM page where several banks overlap (nds_vram_read_slow). */
    NDS_K_VRAM,
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
    /** @brief VRAMCNT_A to VRAMCNT_I (WRAMCNT sits between G and H in the I/O map). */
    uint8_t vramcnt[NDS_BANKS];
    /** @brief VRAMSTAT of the ARM7: bit 0 for C, bit 1 for D given to it. */
    uint8_t vramstat;
    /** @brief The page tables of the video RAM views. */
    nds_vpage vpg[NDS_VPAGES];
    nds_vview vview[NDS_VIEWS];
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
    nds_lcd lcd;
    nds_disp disp;

    /** @brief The tick from which main RAM is free. */
    uint64_t ram_free;
    rl_nds_costs costs;

    nds_dev d_io;
    nds_dev d_disp;
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
/** @brief True when a byte store of cpu at addr is dropped (the ARM9's palette, video RAM and OAM take halfwords and words only). */
bool nds_drops_byte(unsigned cpu, uint32_t addr);
/** @brief True when the display can see a store of cpu at addr (the ARM9's palette, video RAM and OAM). */
static inline bool nds_seen_by_display(unsigned cpu, uint32_t addr)
{
    unsigned r = addr >> 24;
    return cpu == RL_NDS_ARM9 && r >= 0x05 && r <= 0x07;
}
/** @brief Rebuilds every view's page table from VRAMCNT. */
void nds_vram_rebuild(rl_nds *m);
/** @brief A write of VRAMCNT of bank b. */
void nds_vram_write_cnt(rl_nds *m, unsigned b, uint8_t v);
/** @brief A read of size bytes at addr of view, the OR of every bank that answers it (0 for none). */
uint32_t nds_vram_read_slow(const rl_nds *m, unsigned view, uint32_t addr, unsigned size);
/** @brief A write of size bytes at addr of view to every bank that answers it. */
void nds_vram_write_slow(rl_nds *m, unsigned view, uint32_t addr, unsigned size, uint32_t value);
/** @brief The view the CPU's 0x06xxxxxx address addr goes to. */
unsigned nds_vram_cpu_view(unsigned cpu, uint32_t addr);
/** @brief The first byte of bank b in rl_nds.vram. */
uint8_t *nds_vram_bank(rl_nds *m, unsigned b);
/** @brief The page of view at addr (the address's low bits select it, mirrored over the view). */
static inline const nds_vpage *nds_vram_page(const rl_nds *m, unsigned view, uint32_t addr)
{
    const nds_vview *v = &m->vview[view];
    return &m->vpg[v->first + ((addr >> v->shift) & v->pages)];
}
/** @brief A read of size bytes (1, 2 or 4, aligned) at addr of view, as an engine sees it. */
static inline uint32_t nds_vram_rd(const rl_nds *m, unsigned view, uint32_t addr, unsigned size)
{
    const nds_vpage *pg = nds_vram_page(m, view, addr);
    if (pg->p)
        return nds_ld(pg->p + (addr & m->vview[view].mask), size);
    return pg->banks ? nds_vram_read_slow(m, view, addr, size) : 0;
}

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

/** @brief Power-on state of the display timing. */
void nds_lcd_reset(rl_nds *m);
/** @brief Starts the display at line 0 at the ARM9's current tick and posts its first edge. */
void nds_lcd_attach(rl_nds *m);
/** @brief The position in the frame at tick t, in ticks from line 0 dot 0. */
uint64_t nds_lcd_pos(const rl_nds *m, uint64_t t);
/** @brief DISPSTAT (bits 0 to 15) and VCOUNT (bits 16 to 24) of cpu at tick now. */
uint32_t nds_lcd_read(const rl_nds *m, unsigned cpu, uint64_t now);
/** @brief A write of the mask bits of v to cpu's DISPSTAT and VCOUNT word at tick now. */
void nds_lcd_write(rl_nds *m, unsigned cpu, uint32_t v, uint32_t mask, uint64_t now);

/** @brief Power-on state of the display. */
void nds_disp_reset(rl_nds *m);
/** @brief Adds the display device to the scheduler. */
int nds_disp_add(rl_nds *m);
/** @brief Lines the display up with the display timing at tick now and posts the start of VBlank. */
void nds_disp_resync(rl_nds *m, uint64_t now);
/** @brief Draws every line whose render tick is at or before to (nothing when called from inside the frame hook). */
void nds_disp_catch_up(rl_nds *m, uint64_t to);
/** @brief Brings the display to tick now through the scheduler (before a write it could see). */
void nds_disp_sync(rl_nds *m, uint64_t now);
/** @brief True when the ARM9's I/O word at off belongs to the display (both engines, capture, the FIFO, POWCNT1). */
bool nds_disp_reg(unsigned cpu, uint32_t off);
/** @brief A display register word. */
uint32_t nds_disp_read(rl_nds *m, uint32_t off);
/** @brief A write of the mask bits of v to a display register word. */
void nds_disp_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask);
/** @brief Sets the 3D engine's line hook (NULL: the 3D layer is transparent). */
void nds_disp_set_3d(rl_nds *m, nds_line3d_fn *fn, void *user);

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
