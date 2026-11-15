/**
 * @file gba_int.h
 * @brief The machine record and the helpers the files of this layer share.
 */
#ifndef RELIFT_GBA_INT_H
#define RELIFT_GBA_INT_H

#include "gba.h"
#include "ppu/ppu.h"

/** @brief No pending timer reload. */
#define GBA_NONE UINT64_MAX

/** @brief How an access reaches the bus. */
typedef enum gba_who {
    GBA_CPU,
    GBA_DMA,
} gba_who;

/** @brief One state of the written interrupt registers, current from tick tau on. */
typedef struct gba_irq_state {
    uint64_t tau;
    uint16_t ie, iflag;
    bool ime;
} gba_irq_state;

/** @brief Length of the interrupt state log. */
#define GBA_IRQ_LOG 8u

/** @brief The interrupt controller. */
typedef struct gba_irq {
    gba_irq_state log[GBA_IRQ_LOG];
    unsigned n;
    bool line, wake;
    /** @brief When the line last rose, and when the halt wake did. */
    uint64_t rise, wake_at;
    uint16_t wake_mask;
    uint64_t done;
    rl_event ev;
} gba_irq;

/** @brief One timer. */
typedef struct gba_timer {
    uint32_t val;
    uint64_t at;
    uint64_t load;
    uint16_t reload;
    uint8_t ctl;
    rl_event ev;
} gba_timer;

/** @brief One DMA channel. */
typedef struct gba_dma_ch {
    uint32_t sad, dad;
    uint16_t cnt_l, cnt_h;
    uint32_t src, dst, len, data;
    bool active;
    uint64_t ready;
    rl_event ev;
} gba_dma_ch;

/** @brief The display timing. */
typedef struct gba_lcd {
    uint64_t origin;
    uint8_t stat;
    uint8_t vcompare;
    bool capture;
    rl_event ev;
} gba_lcd;

/** @brief Flash chip state. */
typedef struct gba_flash {
    uint16_t id;
    uint8_t phase, mode, busy_kind, busy_data, toggle;
    bool ident, erase, bank, timed;
    uint64_t busy_until, page_deadline;
    uint32_t page_base;
    bool page_open;
    uint8_t page[128];
} gba_flash;

/** @brief EEPROM chip state. */
typedef struct gba_eeprom {
    uint8_t state, cmd;
    uint32_t nbits, nread, addr;
    uint64_t shift, busy_until;
    uint8_t block[8];
    bool sized;
} gba_eeprom;

/** @brief The cartridge backup. */
typedef struct gba_save {
    rl_gba_save_kind kind;
    uint8_t *data;
    size_t len, cap;
    bool owned, dirty;
    uint64_t changed;
    gba_flash fl;
    gba_eeprom ee;
} gba_save;

/** @brief The keypad. */
typedef struct gba_keypad {
    uint16_t keys, cnt;
    bool manual, resolved, sampled_once;
    uint64_t sampled;
    uint32_t ref[10];
    rl_event ev;
} gba_keypad;
/** @brief The picture unit's place in the frame and its outputs. */
typedef struct gba_ppu_sched {
    uint64_t t0, due;
    unsigned line, stage, dot;
    uint64_t frames, hash;
    bool have;
    uint16_t last[GBA_PPU_W * GBA_PPU_H];
    rl_gba_frame_fn *frame_fn;
    void *frame_user;
    rl_event ev;
} gba_ppu_sched;

/** @brief A text background's fetch unit in the memory contention model. */
typedef struct gba_ct_text {
    int fetches, left;
    uint32_t addr;
    bool flip;
} gba_ct_text;

/** @brief The sprite drawer's view of one object in the memory contention model. */
typedef struct gba_ct_obj {
    bool affine, c256, hflip;
    int w, h, left, tx, ty;
    int16_t m[4];
    unsigned tile;
} gba_ct_obj;

/** @brief The sprite unit's OAM walk in the memory contention model. */
typedef struct gba_ct_walk {
    unsigned index, matrix;
    int step, wait, pending, x0, y0;
    bool delay;
} gba_ct_walk;

/**
 * @brief When the picture unit itself reads video RAM, palette RAM and OAM
 * (contend.c): a CPU or DMA access that meets such a read waits a cycle.
 */
typedef struct gba_contend {
    uint8_t io[0x56];
    uint16_t latch[3];
    uint64_t latch_next;
    uint64_t bg_t0;
    unsigned bg_k, mg_k, bg_y;
    bool bg_on, mg_blank;
    gba_ct_text text[4];
    int32_t ref_init[2][2], ref_cur[2][2], aff[2][2];
    uint32_t aff_tile[2];
    bool ref_written[2][2];
    uint8_t mos_bg, mos_obj;
    uint16_t bg_latch;
    uint64_t ob_t0;
    unsigned ob_k, ob_end, ob_y, ob_rd;
    int ob_mosaic;
    bool ob_on, drawing;
    gba_ct_walk walk;
    gba_ct_obj st[2];
    uint64_t bg_slot, obj_slot, oam_slot, pram_slot;
} gba_contend;

/** @brief The Game Pak prefetch buffer. */
typedef struct gba_prefetch {
    uint16_t slot[8];
    uint32_t addr, load;
    int32_t wait;
    bool stopped, ahead;
    /** @brief WAITCNT turned the prefetch off with a halfword on the way: it still arrives, then the unit stops. */
    bool drain;
} gba_prefetch;

enum { GBA_FIFO_WORDS = 7 };

/** @brief A sound FIFO of the built-in sound unit stand-in (fifo.c). */
typedef struct gba_fifo {
    uint32_t q[GBA_FIFO_WORDS];
    uint8_t rd, wr, count;
    /** @brief The word being played and how many of its bytes are left. */
    uint32_t out;
    uint8_t out_n;
} gba_fifo;

/** @brief The built-in stand-in for a slot device. */
typedef struct gba_stub {
    rl_device dev;
    rl_gba *g;
} gba_stub;

/** @brief A device of this layer. */
typedef struct gba_dev {
    rl_device dev;
    rl_gba *g;
} gba_dev;

/** @brief The machine. */
struct rl_gba {
    rl_gba_cart cart;
    uint8_t ewram[0x40000];
    uint8_t iwram[0x8000];
    uint8_t pal[0x400];
    uint8_t vram[0x18000];
    uint8_t oam[0x400];
    uint8_t regs[0x400];

    rl_sched *sched;
    rl_sched_cpu *cpu;
    uint64_t cycles;
    uint32_t mdr;

    uint16_t waitcnt;
    uint32_t memcnt;
    uint8_t postflg;
    uint16_t dispcnt;
    rl_gba_waits waits;

    gba_prefetch pf;
    bool rom_burst;
    uint16_t page_addr;
    /** @brief The last two code fetches, oldest first: address (bit 0 set for a halfword) and what it read. */
    uint32_t code_at[2];
    uint32_t code_unit[2];

    gba_irq irq;
    gba_timer tm[4];
    gba_dma_ch dma[4];
    bool dma_rom_burst;
    int dma_last;
    bool dma_running;
    gba_lcd lcd;
    gba_fifo fifo[2];
    gba_keypad pad;
    gba_save save;
    gba_ppu ppu;
    gba_ppu_sched ps;
    gba_contend ct;

    bool halt_req, stop_req, halted, in_bios;
    /** @brief The IRQ input rose, or came too late for the last sample: the next instruction boundary samples it. */
    bool irq_want;
    uint64_t dma_overlap;
    uint64_t acc_start;

    rl_gba_bios bios;
    rl_gba_bus bios_bus;
    rl_gba_cpu_hooks hooks;
    rl_gba_timer_fn *timer_fn;
    void *timer_user;
    rl_gba_line_fn *vblank_fn;
    void *vblank_user;

    gba_dev d_lcd, d_timer, d_dma, d_sys, d_pad, d_ppu;
    gba_stub stub[RL_GBA_SLOTS];
    rl_device *slot[RL_GBA_SLOTS];
};

static inline rl_gba *gba_of(rl_device *d) { return ((gba_dev *)d->opaque)->g; }

/** @brief The CPU's tick now (0 before rl_gba_attach). */
uint64_t gba_now(const rl_gba *g);
/** @brief Charges n cycles to the CPU. */
void gba_charge(rl_gba *g, unsigned n);
/** @brief addr aligned to size, except in the backup window where the byte lane of the exact address matters. */
static inline uint32_t gba_align(uint32_t addr, unsigned size) { return addr >= 0x0E000000u ? addr : addr & ~(size - 1); }

/** @brief The open bus byte of an I/O address. */
static inline uint8_t gba_open_byte(const rl_gba *g, uint32_t addr) { return (uint8_t)(g->mdr >> (8 * (addr & 3))); }

uint8_t gba_keypad_read(rl_gba *g, uint32_t addr, uint64_t now);
void gba_keypad_write(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now);
void gba_keypad_attach(rl_gba *g);

int gba_save_init(rl_gba *g, const rl_gba_cart *cart);
void gba_save_free(rl_gba *g);
uint8_t gba_save_read(rl_gba *g, uint32_t addr);
void gba_save_write(rl_gba *g, uint32_t addr, uint8_t v);
void gba_save_touch(gba_save *s, uint64_t now);
void gba_save_put(rl_gba *g, size_t off, uint8_t v);
void gba_save_fill(rl_gba *g, size_t off, size_t n, uint8_t v);
bool gba_eeprom_hit(const rl_gba *g, uint32_t addr);
uint32_t gba_eeprom_read(rl_gba *g, unsigned size);
void gba_eeprom_write(rl_gba *g, unsigned size, uint32_t value);
void gba_eeprom_hint(rl_gba *g, uint32_t dst, uint32_t count);
void gba_eeprom_size(rl_gba *g, size_t len);
bool gba_flash_valid(rl_gba_save_kind kind, uint16_t id);
uint8_t gba_flash_read(rl_gba *g, uint32_t addr);
void gba_flash_write(rl_gba *g, uint32_t addr, uint8_t v);

void gba_waits_rebuild(rl_gba *g);
void gba_pf_step(rl_gba *g, unsigned clocks);
/** @brief A WAITCNT write of v: turning the prefetch off lets a halfword on the way arrive, and the cartridge access after it is nonsequential. */
void gba_pf_waitcnt(rl_gba *g, uint16_t v);
uint32_t gba_bus_read(rl_gba *g, gba_who who, uint32_t addr, unsigned size, bool seq);
void gba_bus_write(rl_gba *g, gba_who who, uint32_t addr, unsigned size, uint32_t value, bool seq);

uint32_t gba_mem_read(rl_gba *g, uint32_t addr, unsigned size);
void gba_mem_write(rl_gba *g, uint32_t addr, unsigned size, uint32_t value);
uint16_t gba_rom_half(rl_gba *g, uint32_t addr);
uint32_t gba_io_read(rl_gba *g, uint32_t addr, unsigned size);
void gba_io_write(rl_gba *g, uint32_t addr, unsigned size, uint32_t value);
void gba_after_write(rl_gba *g, uint32_t addr, unsigned size);

void gba_irq_reset(rl_gba *g);
void gba_irq_set(rl_gba *g, uint16_t bits, uint64_t tau);
void gba_irq_ack(rl_gba *g, uint16_t bits, uint64_t tau);
void gba_irq_ie(rl_gba *g, uint16_t ie, uint64_t tau);
void gba_irq_ime(rl_gba *g, bool ime, uint64_t tau);
gba_irq_state gba_irq_at(const rl_gba *g, uint64_t t);
bool gba_irq_wants(const rl_gba *g);
void gba_irq_attach(rl_gba *g);
void gba_halt(rl_gba *g);

void gba_timer_reset(rl_gba *g);
void gba_fifo_reset(rl_gba *g);
/** @brief A write of size bytes of value at FIFO register offset off (0xA0 to 0xA7) of the built-in stand-in. */
void gba_fifo_write(rl_gba *g, uint32_t off, unsigned size, uint32_t value);
/** @brief A write of hi to the high byte of SOUNDCNT_H: its reset bits empty the FIFOs. */
void gba_fifo_control(rl_gba *g, uint8_t hi);
/** @brief Timer timer overflowed, reloading at tick when: the built-in stand-in's FIFOs that it clocks give a byte and may request DMA. */
void gba_fifo_timer(rl_gba *g, unsigned timer, uint64_t when);
void gba_timer_attach(rl_gba *g);
uint8_t gba_timer_read(rl_gba *g, uint32_t addr, uint64_t now);
void gba_timer_write(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now);

void gba_dma_reset(rl_gba *g);
void gba_dma_attach(rl_gba *g);
uint8_t gba_dma_read(rl_gba *g, uint32_t addr);
void gba_dma_write(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now);
void gba_dma_trigger(rl_gba *g, unsigned timing, uint64_t now);
void gba_dma_hdma(rl_gba *g, uint64_t now);
/** @brief rl_gba_dma_fifo for a request made at tick when. */
void gba_dma_fifo_at(rl_gba *g, unsigned fifo, uint64_t when);
void gba_dma_run(rl_gba *g);
bool gba_dma_due(const rl_gba *g);
/** @brief True when no DMA channel is armed and no event falls due in the next cycles. */
bool gba_dma_quiet(const rl_gba *g, unsigned cycles);

uint64_t gba_lcd_pos(const rl_gba *g, uint64_t t);

void gba_ppu_dev_reset(rl_gba *g);
/** @brief Power-on state of the memory contention model. */
void gba_contend_reset(rl_gba *g);
/** @brief Forgets the model's line state after the display position moved. */
void gba_contend_resync(rl_gba *g);
/** @brief Runs the model up to now, then stores a written byte of the picture unit's registers (0x04000000 to 0x04000055). */
void gba_contend_reg(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now);
/** @brief Whether the picture unit read the memory of addr (palette RAM, video RAM or OAM) in the cycle that ends at now. */
bool gba_contend_busy(rl_gba *g, uint32_t addr, uint64_t now);
void gba_ppu_dev_attach(rl_gba *g);
void gba_ppu_dev_resync(rl_gba *g);
void gba_ppu_dev_catch_up(rl_gba *g, uint64_t to);
uint8_t gba_ppu_dev_read(rl_gba *g, uint32_t addr);
void gba_ppu_dev_write(rl_gba *g, uint32_t addr, uint8_t v);
static inline void gba_ppu_sync(rl_gba *g, uint64_t now)
{
    if (g->ps.due <= now && g->sched)
        rl_sched_sync(g->sched, rl_device_bit(&g->d_ppu.dev), now);
}

void gba_lcd_reset(rl_gba *g);
void gba_lcd_attach(rl_gba *g);
void gba_lcd_repost(rl_gba *g);
uint8_t gba_lcd_read(rl_gba *g, uint32_t addr, uint64_t now);
void gba_lcd_write(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now);

uint8_t gba_sys_read(rl_gba *g, uint32_t addr, uint64_t now);
void gba_sys_write(rl_gba *g, uint32_t addr, uint8_t v, uint64_t now);

#endif
