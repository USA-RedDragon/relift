/**
 * @file nds_int.h
 * @brief The machine record and the helpers the files of this layer share.
 */
#ifndef RELIFT_NDS_INT_H
#define RELIFT_NDS_INT_H

#include "gpu2d.h"
#include "gpu3d.h"
#include "nds.h"
#include "nds_dev.h"

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
/** @brief One DMA channel. */
typedef struct nds_dma_ch {
    /** @brief The registers as written: source and destination address, count and control (the whole 32-bit word). */
    uint32_t sad, dad, cnt;
    /** @brief The working source, destination and remaining units of the transfer. */
    uint32_t src, dst, len;
    /** @brief The tick from which the transfer may run (start delay or trigger tick). */
    uint64_t ready;
    /** @brief Armed: enabled and its start condition met. */
    bool active;
    /** @brief The first unit of a trigger is still to come (it is a nonsequential access). */
    bool first;
    /** @brief The unit that runs next is the second of the trigger. */
    bool after_first;
    /** @brief Units left in the GXFIFO channel's current burst. */
    uint8_t burst;
    /** @brief The unit's read is done and its value latched; its write comes next. */
    bool half;
    uint32_t latch;
} nds_dma_ch;

/** @brief The four DMA channels of one CPU and the engine that runs them one unit at a time. */
typedef struct nds_dma {
    nds_dma_ch ch[4];
    /** @brief DMAxFILL, ARM9 only. */
    uint32_t fill[4];
    /** @brief Runs the next slice of transfers. */
    rl_event ev;
    /** @brief The tick the engine's bus time has reached. */
    uint64_t at;
    /** @brief A main RAM step waits for the other CPU to pass this tick (0 when none). */
    uint64_t retry;
    rl_nds *m;
    uint8_t cpu;
} nds_dma;

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

/** @brief One of the sound controller's 16 channels: its registers and its playback (sound.c). */
typedef struct nds_snd_ch {
    /** @brief The registers as written (cnt bit 31 is the busy bit the channel clears). */
    uint32_t cnt, sad, len;
    uint16_t tmr, pnt;
    /** @brief The tick of the timer's next overflow, NDS_NEVER while the timer is stopped. */
    uint64_t next;
    /** @brief The sample the next overflow plays: negative during the start delay. */
    int32_t pos;
    /** @brief Latched at the start: the source address, the sample count and the loop start sample. */
    uint32_t src, total, loop;
    /** @brief The sample the channel puts out now (PCM16 scale), and whether it puts one out at all. */
    int16_t out;
    bool sounding;
    /** @brief The last sample of a one-shot sound plays: the next overflow ends it. */
    bool last;
    /** @brief The start's first period keeps the old sample (the Hold flag). */
    bool held;
    /** @brief The IMA-ADPCM decoder, its state at the loop start, and whether that was saved. */
    int16_t pcm, loop_pcm;
    uint8_t index, loop_index;
    bool loop_saved;
    /** @brief The noise generator of channels 14 and 15. */
    uint16_t lfsr;
} nds_snd_ch;

/** @brief One capture unit: its registers and its progress. */
typedef struct nds_snd_cap {
    uint8_t cnt;
    uint32_t dad, len;
    /** @brief The tick of its timer's next overflow (NDS_NEVER while stopped), the byte offset and the word being filled. */
    uint64_t next;
    uint32_t off, word;
    uint8_t fill;
} nds_snd_cap;

/** @brief Stereo frames the host's sample buffer holds. */
#define NDS_SND_RING 16384u

/** @brief The ARM7's sound controller (0x04000400 to 0x0400051F, sound.c). */
typedef struct nds_sound {
    nds_snd_ch ch[16];
    uint16_t soundcnt, bias;
    nds_snd_cap cap[2];
    /** @brief The tick the unit has reached and the tick of the next output sample. */
    uint64_t at, out_next;
    /** @brief The output samples for the host (interleaved left and right), a ring read by rl_nds_audio_take. */
    int16_t ring[NDS_SND_RING * 2];
    uint32_t rd, n;
    /** @brief Output samples made in all and dropped because the ring was full. */
    uint64_t made, dropped;
    /** @brief Catches up every few milliseconds. */
    rl_event ev;
} nds_sound;

/** @brief KEYCNT of each CPU and the host's input. */
typedef struct nds_keypad {
    uint16_t cnt[2];
    rl_nds_input in;
} nds_keypad;

/** @brief Sets of the ARM9's instruction (8 KiB) and data (4 KiB) caches: four ways of 32-byte lines. */
#define NDS_ICACHE_SETS 64u
#define NDS_DCACHE_SETS 32u
/**
 * @brief Ticks at the end of an ARM9 write to main RAM that the CPU does not wait for: the console's uncached STR
 * takes 14 ticks where the RAM is busy 18 (a write buffer drains one STR each 18).
 */
#define NDS_MAIN_WRITE_POSTED 4u

/** @brief Entries of the ARM9's write buffer. */
#define NDS_WB_ENTRIES 8u

/** @brief Region attributes of a 4 KiB page: instruction cacheable, data cacheable, bufferable. */
enum { NDS_ATTR_IC = 1, NDS_ATTR_DC = 2, NDS_ATTR_WB = 4 };

/** @brief The tags of one ARM9 cache (sized for the larger one). */
typedef struct nds_cache_side {
    uint32_t tag[NDS_ICACHE_SETS][4];
    uint8_t valid[NDS_ICACHE_SETS];
    uint8_t dirty[NDS_ICACHE_SETS];
    /** @brief The round-robin counter: the way the next fill replaces. */
    uint8_t next;
} nds_cache_side;

/** @brief The ARM9's caches, write buffer and region attributes (cache.c). */
typedef struct nds_cache_state {
    nds_cache_side i, d;
    /** @brief The pseudo-random replacement's state. */
    uint32_t lfsr;
    /** @brief The write buffer: the tick each entry is written by, oldest first, and the last one's. */
    uint64_t wb_done[NDS_WB_ENTRIES];
    uint8_t wb_head, wb_n;
    uint64_t wb_last;
    /** @brief NDS_ATTR_ bits per 4 KiB page from the protection regions (the highest region wins). */
    uint8_t attr[1u << 20];
} nds_cache_state;

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
    nds_cache_state cache;

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
    /** @brief RCNT (ARM7 word offset 0x134, low half): a register file that starts at 0x8000. */
    uint16_t rcnt;
    /** @brief The ARM7 wrote HALTCNT: the store ends in a halt. */
    bool halt_req;
    /** @brief The debug port hook (rl_nds_set_debug_hook) and its argument. */
    rl_nds_debug_fn *debug_fn;
    void *debug_user;
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
    nds_dma dma[2];
    nds_keypad pad;
    nds_cart cart;
    nds_spi spi;
    nds_rtc rtc;
    nds_sound snd;
    nds_gx gx;
    /** @brief POWCNT2 (ARM7 0x04000304, bits 0 and 1) and WIFIWAITCNT (0x04000206, all 16 bits). */
    uint8_t powcnt2;
    uint16_t wifiwait;

    /** @brief The tick from which main RAM is free, and who booked it last (NDS_RAM_...). */
    uint64_t ram_free;
    uint8_t ram_side;
    /** @brief The tick the ARM9's last bus data access ended. */
    uint64_t bus9_end;
    /**
     * @brief The ARM9's uncached code fetches (rl_nds_code_cost): the word read last (1 when none), the target of a
     * refill whose second fetch comes next (with bit 0 set; 0 when none), and the end and cost of the last fetch
     * that read a word while no data access followed (end 0 when none).
     */
    uint32_t fetch9_word;
    uint32_t fetch9_refill;
    uint64_t fetch9_end;
    unsigned fetch9_cost;
    /** @brief The last rl_nds_code_cost was a sequential fetch that hit in the instruction cache. */
    bool fetch9_hit;
    /** @brief The ARM9's last instruction TCM data access opened an LDM or STM: its second word costs one more. */
    bool itcm_multi;
    /** @brief The ARM9's last TCM data access was a single-word store to the instruction TCM, ended at itcm_store_end. */
    bool itcm_store;
    uint64_t itcm_store_end;
    /**
     * @brief The ARM7's code burst in main RAM: the last fetch's name (rl_nds_code_cost7), the address the burst
     * fetches next, the tick its last fetch ended and its fetches so far (at most 255).
     */
    uint32_t burst7_name;
    uint32_t burst7_next;
    uint64_t burst7_end;
    uint8_t burst7_len;
    /** @brief The tick the ARM7's last write to work RAM ended, while no other access followed (WRAM7_WRITE_N). */
    uint64_t wram7_write_end;
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
static inline uint64_t nds_now(const rl_nds *m, unsigned cpu) { return m->cpu[cpu] ? rl_sched_now_inline(m->cpu[cpu]) : 0; }
/** @brief Charges ticks to cpu (an ARM7 charge is rounded up to whole cycles). */
static inline void nds_charge(rl_nds *m, unsigned cpu, uint64_t ticks)
{
    if (!m->cpu[cpu] || ticks == 0)
        return;
    rl_sched_stall_inline(m->cpu[cpu], cpu == RL_NDS_ARM9 ? ticks : (ticks + 1) / 2);
}

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

/** @brief Decodes addr for a bus master that has no TCM access (a DMA channel of cpu). */
nds_where nds_decode_bus(rl_nds *m, unsigned cpu, uint32_t addr, bool write);
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
static inline bool nds_shared(unsigned cpu, uint32_t addr)
{
    unsigned r = addr >> 24;
    if (cpu == RL_NDS_ARM9)
        return (r >= 0x02 && r <= 0x04) || (r >= 0x08 && r <= 0x0A);
    if (r == 0x03)
        return addr < 0x03800000u;
    return r == 0x02 || r == 0x04 || r == 0x06 || (r >= 0x08 && r <= 0x0A);
}
/** @brief Waits until the other CPU is past cpu in the order (rl_sched_order); nothing before rl_nds_attach. */
void nds_order(rl_nds *m, unsigned cpu);
/** @brief Sets the CPUs' ranks from EXMEMCNT bit 15. */
void nds_ranks(rl_nds *m);

/** @brief Who booked main RAM last: a CPU, or a CPU's DMA (NDS_RAM_DMA + the CPU). */
enum { NDS_RAM_CPU9 = RL_NDS_ARM9, NDS_RAM_CPU7 = RL_NDS_ARM7, NDS_RAM_DMA = 2 };
/** @brief True when EXMEMCNT bit 15 gives cpu (and its DMA) main RAM first. */
bool nds_ram_first(const rl_nds *m, unsigned cpu);
/**
 * @brief Before a main RAM access of cpu at its tick (after nds_order): runs the other CPU's DMA up to that tick,
 * and, when the other CPU comes first, waits as long as the RAM is busy, letting the other side take it each
 * time it frees up (bus.c).
 */
void nds_ram_wait(rl_nds *m, unsigned cpu);
/** @brief Where cpu's next main RAM access may start: its tick, for the ARM9 the bus clock edge at or before it. */
uint64_t nds_ram_pos(const rl_nds *m, unsigned cpu);
/** @brief The tick from which main RAM takes a new access: ram_free, and after an ARM7 access its precharge. */
uint64_t nds_ram_busy(const rl_nds *m);
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
/** @brief A DMA channel's start timing changed at tick now: posts the line edges the VBlank and HBlank starts need. */
void nds_lcd_dma_changed(rl_nds *m, uint64_t now);

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

/** @brief Power-on state of the 3D engine (gpu3d_fifo.c). */
void nds_gx_reset(rl_nds *m);
/** @brief Power-on state of the geometry engine's matrices, viewport and depths (gpu3d_geo.c). */
void nds_gx_geo_reset(rl_nds *m);
/** @brief Starts the 3D engine's clock at the ARM9's current tick. */
void nds_gx_attach(rl_nds *m);
/** @brief True when the ARM9's I/O word at off belongs to the 3D engine (DISP3DCNT and 0x320 to 0x6A3). */
bool nds_gx_reg(unsigned cpu, uint32_t off);
/** @brief A 3D engine register word at tick now. */
uint32_t nds_gx_read(rl_nds *m, uint32_t off, uint64_t now);
/** @brief A write of the mask bits of v to a 3D engine register word at tick now (GXFIFO and the command ports queue commands). */
void nds_gx_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask, uint64_t now);
/** @brief Runs the geometry engine's queue up to tick to. */
void nds_gx_run(rl_nds *m, uint64_t to);
/** @brief The cycles of command cmd from GBATEK's table (0 for an invalid one). */
unsigned nds_gx_cycles(uint8_t cmd);
/** @brief Runs command cmd with its parameters p at tick start; returns the cycles it takes (gpu3d_geo.c). */
unsigned nds_gx_exec(rl_nds *m, uint8_t cmd, const uint32_t *p, uint64_t start);
/** @brief Starts GXFIFO DMA at tick now when the FIFO is less than half full (after a channel is enabled or ends a burst). */
void nds_gx_dma_check(rl_nds *m, uint64_t now);
/** @brief The tick the last store to a full FIFO waits until (0 for none), and clears it. */
uint64_t nds_gx_stall(rl_nds *m);
/** @brief The IF bits the 3D engine holds set at tick now (the GXFIFO interrupt while its condition holds). */
uint32_t nds_gx_irq_hold(rl_nds *m, uint64_t now);
/** @brief The rendering engine's work at display line `line` (its render tick when): the frame start and one line (gpu3d_render.c). */
void nds_gx_line(rl_nds *m, unsigned line, uint64_t when);
/** @brief The display's 3D line hook: line y of the finished 3D layer. */
bool nds_gx_line3d(void *user, unsigned y, uint32_t *out);

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

/** @brief Power-on state of both DMA engines. */
void nds_dma_reset(rl_nds *m);
/** @brief Starts the engines' clocks at the CPUs' current ticks. */
void nds_dma_attach(rl_nds *m);
/** @brief DMA registers (word offset off, 0xB0 to 0xEF) at tick now. */
uint32_t nds_dma_read(rl_nds *m, unsigned cpu, uint32_t off);
/** @brief A write of the mask bits of v to a DMA register at tick now; enabling an immediate channel starts it. */
void nds_dma_write(rl_nds *m, unsigned cpu, uint32_t off, uint32_t v, uint32_t mask, uint64_t now);
/**
 * @brief If cpu has a DMA transfer that may run now, the CPU waits on the bus until it is done (called before a
 * bus access of cpu, with its clock at the access).
 */
void nds_dma_wait(rl_nds *m, unsigned cpu);
/** @brief Runs cpu's DMA transfers whose next unit starts by tick t (a main RAM access of the other CPU at t). */
void nds_dma_catch_up(rl_nds *m, unsigned cpu, uint64_t t);
/** @brief True when a channel of cpu is armed (the CPU's bus accesses may have to wait). */
static inline bool nds_dma_live(const rl_nds *m, unsigned cpu)
{
    for (unsigned i = 0; i < 4; i++)
        if (m->dma[cpu].ch[i].active)
            return true;
    return false;
}

/** @brief Start timings of the DMA channels (the channel's mode bits), per CPU (GBATEK "DS DMA Transfers"). */
enum {
    NDS_DMA9_IMMEDIATE = 0,
    NDS_DMA9_VBLANK = 1,
    NDS_DMA9_HBLANK = 2,
    NDS_DMA9_DISPLAY_START = 3,
    NDS_DMA9_MAIN_DISPLAY = 4,
    NDS_DMA9_CARD = 5,
    NDS_DMA9_GBA_SLOT = 6,
    NDS_DMA9_GXFIFO = 7,
    NDS_DMA7_IMMEDIATE = 0,
    NDS_DMA7_VBLANK = 1,
    NDS_DMA7_CARD = 2,
    /** @brief Mode 3 of channels 0 and 2: the wireless interrupt. */
    NDS_DMA7_WIRELESS = 3,
    /** @brief Mode 3 of channels 1 and 3: the GBA slot (the same mode value as NDS_DMA7_WIRELESS, other channels). */
    NDS_DMA7_GBA_SLOT = 8 | 3,
};
/**
 * @brief A start condition of timing (NDS_DMA9_* for the ARM9, NDS_DMA7_* for
 * the ARM7) happens at tick: every enabled channel of cpu set to that timing
 * transfers its count (the display lane raises VBLANK, HBLANK, DISPLAY_START
 * and MAIN_DISPLAY, the 3D lane GXFIFO, the cartridge CARD itself). A channel
 * that is still running ignores the trigger.
 */
void nds_dma_start(rl_nds *m, unsigned cpu, unsigned timing, uint64_t tick);
/** @brief True when an enabled channel of cpu starts on timing (it may be running now). */
bool nds_dma_waits(const rl_nds *m, unsigned cpu, unsigned timing);

/** @brief KEYINPUT with KEYCNT (word offset 0x130) of cpu, or EXTKEYIN with its neighbor RCNT (0x134, ARM7). */
uint32_t nds_keypad_read(rl_nds *m, unsigned cpu, uint32_t off);
/** @brief A write of the mask bits of v to KEYCNT (word offset 0x130, the upper half). */
void nds_keypad_write(rl_nds *m, unsigned cpu, uint32_t v, uint32_t mask, uint64_t now);
/** @brief EXTKEYIN as the ARM7 reads it. */
uint16_t nds_extkeyin(const rl_nds *m);

/** @brief Power-on state of the ARM7's system registers. */
void nds_sys_reset(rl_nds *m);
/** @brief True when the ARM7 register at word offset off is one of nds_sys_*. */
bool nds_sys_has(uint32_t off);
/** @brief POWCNT2, BIOSPROT, WIFIWAITCNT (half of word 0x204) or a sound register at word offset off, at tick now. */
uint32_t nds_sys_read(rl_nds *m, uint32_t off, uint64_t now);
/** @brief A write of the mask bits of v to one of them at tick now. */
void nds_sys_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask, uint64_t now);

/** @brief Power-on state of the sound controller. */
void nds_snd_reset(rl_nds *m);
/** @brief Starts the sound controller's clock at the ARM7's current tick and posts its catch-up. */
void nds_snd_attach(rl_nds *m);
/** @brief Runs the channels, the captures and the mixer up to tick to. */
void nds_snd_run(rl_nds *m, uint64_t to);
/** @brief A sound register (word offset 0x400 to 0x51C) at tick now. */
uint32_t nds_snd_read(rl_nds *m, uint32_t off, uint64_t now);
/** @brief A write of the mask bits of v to a sound register at tick now (a start bit that goes from 0 to 1 starts). */
void nds_snd_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask, uint64_t now);

/** @brief Empties the caches and the write buffer and reads the regions. */
void nds_cache_reset(rl_nds *m);
/** @brief Rebuilds the page attributes after a protection unit change. */
void nds_cache_mpu(rl_nds *m);
/** @brief The NDS_ATTR_ bits of addr with the cache enables of the control register. */
static inline unsigned nds_cache_attr(const rl_nds *m, uint32_t addr)
{
    uint32_t c = m->cp15.control;
    if (!(c & RL_CP15_MPU))
        return 0;
    unsigned a = m->cache.attr[addr >> 12];
    if (!(c & RL_CP15_ICACHE))
        a &= ~(unsigned)NDS_ATTR_IC;
    if (!(c & RL_CP15_DCACHE))
        a &= ~(unsigned)NDS_ATTR_DC;
    return a;
}
/**
 * @brief Charges a cached read of addr: a hit, or a line fill after the write buffer drains (and the write-back of a
 * dirty victim). A data hit costs nothing beyond its instruction's cycle but for a further word of an LDM (seq).
 * Returns true for a hit.
 */
bool nds_cache_read(rl_nds *m, uint32_t addr, bool code, bool seq);
/** @brief A store hit in a write-back line: marks it dirty and charges the hit; false on a miss or a write-through line. */
bool nds_cache_write_hit(rl_nds *m, uint32_t addr, bool write_back, bool seq);
/** @brief Queues a buffered write of ticks bus time to addr, stalling while the buffer is full. */
void nds_cache_buffer(rl_nds *m, uint32_t addr, unsigned ticks);
/** @brief Waits until the write buffer is empty (before an access that goes to the bus). */
void nds_cache_before_bus(rl_nds *m);
/** @brief A CP15 c7 cache operation (MCR key, value). */
void nds_cache_op(rl_nds *m, uint32_t key, uint32_t v);

/** @brief The access cost in ticks of cpu (kind RL_NDS_CODE or RL_NDS_DATA) at addr. */
unsigned nds_cost(const rl_nds *m, unsigned cpu, unsigned kind, uint32_t addr, unsigned size, bool seq);
/** @brief A read on the bus of cpu without its TCMs (a DMA's view) at tick now; no cost. */
uint32_t nds_bus_load(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint64_t now, bool *main);
/** @brief The write twin of nds_bus_load. */
void nds_bus_store(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value, uint64_t now, bool *main);

/** @brief The scheduler address of cpu's I/O address addr: the ARM7's registers sit 4 GiB above the ARM9's. */
static inline uint64_t nds_io_key(unsigned cpu, uint32_t addr) { return (uint64_t)cpu << 32 | addr; }

#endif
