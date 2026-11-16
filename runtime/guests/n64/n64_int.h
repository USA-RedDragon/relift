/**
 * @file n64_int.h
 * @brief The machine's state, shared by its files.
 */
#ifndef RELIFT_N64_INT_H
#define RELIFT_N64_INT_H

#include "n64.h"

#include "core.h"

#include <string.h>

/** @brief One PI domain's bus timing. */
typedef struct n64_bsd {
    uint8_t latency;
    uint8_t pulse_width;
    uint8_t page_size;
    uint8_t release;
} n64_bsd;

/** @brief One SP DMA transfer's registers: the DMEM or IMEM address (bit 12 IMEM), the RDRAM address, the row shape. */
typedef struct n64_sp_dma {
    uint32_t mem;
    uint32_t dram;
    uint32_t length;
    uint32_t count;
    uint32_t skip;
} n64_sp_dma;

/** @brief One joybus controller port: whether a standard controller is plugged in, its buttons (RL_N64_PAD_*) and stick. */
typedef struct n64_pad {
    bool present;
    uint16_t buttons;
    int8_t x;
    int8_t y;
} n64_pad;

/** @brief The chips' state of the cartridge's save memory the machine finds out by behavior. */
typedef struct n64_flash {
    uint8_t mode;
    uint8_t status;
    uint8_t erase_setup;
    uint8_t erase_sector;
    uint8_t busy;
    uint8_t page[128];
    uint32_t offset;
    bool high_valid;
    uint16_t high;
    bool open_bus;
    bool stale;
    uint16_t stale_value;
    uint8_t pending_cmd;
    uint8_t pending_count;
    uint16_t burst;
    rl_event done;
} n64_flash;

/** @brief The save stores, bits of the dirty mask. */
enum { SAVE_EEPROM = 1, SAVE_SRAM = 2, SAVE_FLASH = 4 };

/** @brief The PI device a cartridge bus access reaches. */
enum { BUS_NONE, BUS_ROM, BUS_SRAM, BUS_FLASH };

/** @brief Where a code run is: not loaded, loaded and not checked, or checked and in dispatch. */
enum { CODE_OUT, CODE_LOADED, CODE_LIVE };

struct rl_n64 {
    const uint8_t *rom;
    size_t rom_len;
    uint8_t *rdram;
    bool pal;
    uint8_t spmem[0x2000];
    uint8_t pif_ram[64];
    rl_sched *s;
    rl_sched_cpu *k;
    rl_n64_stats stats;

    struct {
        uint32_t mode;
        uint32_t intr;
        uint32_t mask;
    } mi;

    struct {
        uint32_t regs[8];
    } ri;

    uint32_t rdram_regs[16];

    struct {
        uint32_t dram;
        uint32_t cart;
        uint32_t rd_len;
        uint32_t wr_len;
        bool busy;
        bool io_busy;
        bool error;
        bool interrupt;
        uint32_t latch;
        n64_bsd bsd[2];
        rl_event done;
        rl_event bus_write;
    } pi;

    struct {
        n64_sp_dma pending;
        n64_sp_dma current;
        bool full_rd, full_wr;
        bool busy_rd, busy_wr;
        /** @brief The RSP cycle the current row ends at. */
        uint64_t row_end;
        bool halted, broken, sstep, intbreak, io_full;
        uint8_t signals;
        uint32_t semaphore;
        uint32_t pc;
        /** @brief The RSP's clock: the cycle it has run to. */
        uint64_t cycle;
        /** @brief The latest RSP cycle of any SP access; the SP side never goes back. */
        uint64_t last;
        /** @brief Whether the RSP's code is running (its accesses come through the COP0 entries). */
        bool in_rsp;
        rl_event slice;
        rl_event dma_event;
        const rl_n64_rsp_ops *ops;
        void *user;
    } sp;

    struct {
        uint32_t start, end, current;
        bool start_valid, end_valid, xbus, freeze, flush, ready, start_gclk, crashed;
        bool buffer_busy, pipe_busy, tmem_busy;
        uint64_t clock_base;
        /** @brief Command words taken but not yet run: a command cut by DPC_END waits here for the rest (64-bit words size, from offset). */
        uint32_t queue_size, queue_offset;
        uint32_t queue[RL_N64_DP_QUEUE * 2u];
        const rl_n64_rdp_ops *ops;
        void *user;
    } dp;

    struct {
        bool enable;
        bool carry;
        uint32_t addr[2];
        uint32_t len[2];
        uint32_t count;
        uint32_t dacrate;
        uint32_t bitrate;
        /** @brief The DAC's rate in Hz and its sample period in half CPU cycles. */
        uint32_t freq;
        uint32_t period;
        /** @brief The half cycle the next sample is due at. */
        uint64_t next;
        /** @brief The DAC's level (16-bit samples times 256) and its decay per sample (16.16). */
        int32_t left, right;
        uint32_t decay;
        /** @brief The resampler: the next output frame's place after the last sample (32.32) and that sample. */
        uint64_t pos;
        int32_t prev_l, prev_r;
        int16_t *ring;
        uint32_t head, tail;
        uint64_t dropped, samples;
        rl_event done;
    } ai;

    struct {
        uint32_t dram;
        uint32_t rd_addr;
        uint32_t wr_addr;
        uint32_t latch;
        bool dma_busy;
        bool io_busy;
        bool interrupt;
        uint8_t pch_state;
        uint8_t dma_state;
        rl_event dma_read;
        rl_event dma_write;
        rl_event bus_write;
    } si;

    struct {
        uint8_t joy_at[5];
        bool joy_skip[5];
        bool joy_reset[5];
        n64_pad pad[4];
        bool pad_ports;
    } pif;

    struct {
        uint8_t sram[0x8000];
        uint8_t flash[0x20000];
        uint8_t eeprom[2048];
        uint32_t eeprom_len;
        bool eeprom_busy;
        bool flash_armed;
        uint8_t dirty;
        uint64_t changed;
        n64_flash fl;
        rl_event eeprom_done;
        uint8_t dev;
        uint32_t view;
        uint32_t raw;
    } cart;

    struct {
        uint32_t control;
        uint32_t origin;
        uint32_t width;
        uint32_t coincidence;
        uint32_t timing;
        uint32_t v_total;
        uint32_t h_total;
        uint32_t h_leap;
        uint32_t h_video;
        uint32_t v_video;
        uint32_t v_burst;
        uint32_t x_scale;
        uint32_t y_scale;
        uint32_t vcounter;
        uint32_t field;
        uint32_t leap_counter;
        uint32_t inactive;
        uint64_t next;
        uint64_t frac;
        uint64_t sys;
        uint8_t *screen;
        uint32_t screen_w, screen_h;
        bool screen_ready;
        rl_event line;
        rl_n64_frame_hook *hook;
        void *hook_user;
    } vi;

    struct {
        const rl_unit *const *units;
        uint32_t unit_count;
        const rl_n64_code *code;
        uint32_t count;
        uint8_t *state;
        uint32_t *order;
        rl_fn stub;
        uint8_t *filled;
        /** @brief The runs linked in SP memory (the boot code), by index. */
        uint32_t *sp_runs;
        uint32_t sp_count;
        /** @brief The dispatch slots of SP memory in kseg1, one per word: kept here, outside the dispatch table. */
        rl_fn sp_slots[0x2000 / 4];
    } code;
};

/** @brief The CPU's tick now. */
uint64_t rl_n64_now(const rl_n64 *m);
/** @brief The guest byte of RDRAM at address a (wrapping): RDRAM holds host-order 32-bit words. */
static inline uint8_t rl_n64_rd8(const rl_n64 *m, uint32_t a) { return m->rdram[(a ^ 3u) & (RL_N64_RDRAM_SIZE - 1u)]; }
/** @brief Writes the guest byte v of RDRAM at address a (wrapping). */
static inline void rl_n64_wr8(rl_n64 *m, uint32_t a, uint8_t v) { m->rdram[(a ^ 3u) & (RL_N64_RDRAM_SIZE - 1u)] = v; }
/** @brief The 32-bit word of RDRAM at a (a multiple of 4). */
static inline uint32_t rl_n64_rd32(const rl_n64 *m, uint32_t a)
{
    uint32_t w;
    memcpy(&w, m->rdram + (a & (RL_N64_RDRAM_SIZE - 4u)), 4);
    return w;
}
/** @brief Writes the 32-bit word v of RDRAM at a (a multiple of 4). */
static inline void rl_n64_wr32(rl_n64 *m, uint32_t a, uint32_t v) { memcpy(m->rdram + (a & (RL_N64_RDRAM_SIZE - 4u)), &v, 4); }

/** @brief Sets bits of MI_INTR. */
void rl_n64_mi_raise(rl_n64 *m, uint32_t bits);
/** @brief Clears bits of MI_INTR. */
void rl_n64_mi_lower(rl_n64 *m, uint32_t bits);

/** @brief PI register read (index 0 to 15). */
uint32_t rl_n64_pi_read(rl_n64 *m, unsigned index);
/** @brief PI register write. */
void rl_n64_pi_write(rl_n64 *m, unsigned index, uint32_t value);
/** @brief A CPU read of the cartridge bus at paddr (a word). */
uint32_t rl_n64_cart_read(rl_n64 *m, uint32_t paddr);
/** @brief A CPU write of the cartridge bus at paddr (a word); the bus is busy for a while after it. */
void rl_n64_cart_write(rl_n64 *m, uint32_t paddr, uint32_t v);
/** @brief Selects the device at cartridge bus address a: the ROM, the SRAM, the Flash chip once its command register has been written, or none; sets the bus latch to the address. */
void rl_n64_bus_select(rl_n64 *m, uint32_t a);
/** @brief The next halfword of the selected device, or the bus latch's when it gives none. */
uint16_t rl_n64_bus_read_half(rl_n64 *m);
/** @brief Writes a halfword to the selected device. */
void rl_n64_bus_write_half(rl_n64 *m, uint16_t v);
/** @brief The save chips at creation: SRAM zeroed, Flash and EEPROM erased, EEPROM at its larger size. */
void rl_n64_cart_init(rl_n64 *m);
/** @brief Sets up the save chips' events. */
void rl_n64_cart_attach(rl_n64 *m);
/** @brief Removes the save chips' events from the scheduler. */
void rl_n64_cart_detach(rl_n64 *m);
/** @brief The stores in the bits of SAVE_* changed now. */
void rl_n64_cart_touch(rl_n64 *m, uint8_t stores);
/** @brief The Flash chip's registers are at offset (from 0x08000000) for the access that starts there. */
void rl_n64_flash_select(rl_n64 *m, uint32_t offset);
/** @brief The Flash chip's next halfword; false when it gives none. */
bool rl_n64_flash_read(rl_n64 *m, uint16_t *out);
/** @brief The Flash chip takes a halfword. */
void rl_n64_flash_write(rl_n64 *m, uint16_t v);
/** @brief Puts the Flash chip in its power-on state. */
void rl_n64_flash_power(rl_n64 *m);
/** @brief Sets up the Flash chip's event. */
void rl_n64_flash_attach(rl_n64 *m);
/** @brief The joybus command of channel 4 (the cartridge's EEPROM): returns bit 0 valid, bit 1 over. */
unsigned rl_n64_cart_joybus(rl_n64 *m, unsigned send, unsigned recv, const uint8_t *input, uint8_t *output);
/** @brief Sets up the EEPROM's event. */
void rl_n64_eeprom_attach(rl_n64 *m);
/** @brief Sets up PI's events. */
void rl_n64_pi_attach(rl_n64 *m);

/** @brief SI register read at physical address a (0x04800000 to 0x048FFFFF). */
uint32_t rl_n64_si_read(rl_n64 *m, uint32_t a);
/** @brief SI register write. */
void rl_n64_si_write(rl_n64 *m, uint32_t a, uint32_t v);
/** @brief A read of PIF RAM or ROM at address a (ROM reads 0). */
uint32_t rl_n64_pif_read(rl_n64 *m, uint32_t a);
/** @brief A write of PIF RAM at address a; a command block flagged for the joybus is parsed. */
void rl_n64_pif_write(rl_n64 *m, uint32_t a, uint32_t v);
/** @brief A CPU word read of the PIF's address space over the SI bus: the latch while a write is still going. */
uint32_t rl_n64_si_bus_read(rl_n64 *m, uint32_t a);
/** @brief A CPU word write of the PIF's address space over the SI bus: ignored while a write is still going. */
void rl_n64_si_bus_write(rl_n64 *m, uint32_t a, uint32_t v);
/** @brief Reads the controller state from the port table when the machine was told to (before the joybus commands run). */
void rl_n64_pad_refresh(rl_n64 *m);
/** @brief Sets up SI's events. */
void rl_n64_si_attach(rl_n64 *m);
/** @brief Removes SI's events from the scheduler. */
void rl_n64_si_detach(rl_n64 *m);
/** @brief The PIF resets its joybus channels: every one skipped. */
void rl_n64_pif_init(rl_n64 *m);
/** @brief A PIF to RDRAM transfer from the PIF address a: runs the joybus commands in PIF RAM (a read of the command block), then copies 64 bytes to RDRAM at dram. */
void rl_n64_pif_dma_read(rl_n64 *m, uint32_t a, uint32_t dram);
/** @brief An RDRAM to PIF transfer of 64 bytes to the PIF address a; a command block with its flag set is parsed. */
void rl_n64_pif_dma_write(rl_n64 *m, uint32_t a, uint32_t dram);
/** @brief The cycles the command block in PIF RAM takes on the joybus. */
uint32_t rl_n64_pif_estimate(const rl_n64 *m);
/** @brief One standard controller's joybus command: send bytes in input, recv expected back in output. Returns bit 0 valid, bit 1 over. */
unsigned rl_n64_pad_comm(rl_n64 *m, unsigned port, unsigned send, unsigned recv, const uint8_t *input, uint8_t *output);

/** @brief AI register read (index 0 to 7), with the DAC brought up to now. */
uint32_t rl_n64_ai_read(rl_n64 *m, unsigned index);
/** @brief AI register write. */
void rl_n64_ai_write(rl_n64 *m, unsigned index, uint32_t value);
/** @brief Allocates the audio ring and sets the DAC's power-on rate. */
void rl_n64_ai_init(rl_n64 *m);
/** @brief Starts the DAC's clock at the CPU's tick now. */
void rl_n64_ai_attach(rl_n64 *m);
/** @brief Removes the AI event from the scheduler. */
void rl_n64_ai_detach(rl_n64 *m);

/** @brief VI register read (index 0 to 13), with the line counter brought up to now. */
uint32_t rl_n64_vi_read(rl_n64 *m, unsigned index);
/** @brief VI register write. */
void rl_n64_vi_write(rl_n64 *m, unsigned index, uint32_t value);
/** @brief Starts the line events. */
void rl_n64_vi_attach(rl_n64 *m);

/** @brief The SP side at creation: the RSP halted, the DP ready. */
void rl_n64_sp_init(rl_n64 *m);
/** @brief Sets up the SP events. */
void rl_n64_sp_attach(rl_n64 *m);
/** @brief Runs the RSP and the SP DMA up to CPU tick tick (nothing while the RSP's own code runs). */
void rl_n64_sp_catch_up(rl_n64 *m, uint64_t tick);
/** @brief A CPU read of the SP registers (0x04040000 to 0x0404001F) or SP_PC at a, now. */
uint32_t rl_n64_sp_read(rl_n64 *m, uint32_t a);
/** @brief A CPU write of the SP registers or SP_PC. */
void rl_n64_sp_write(rl_n64 *m, uint32_t a, uint32_t v);
/** @brief A CPU read of DP register i (0 to 7), now. */
uint32_t rl_n64_dp_read(rl_n64 *m, unsigned i);
/** @brief A CPU write of DP register i. */
void rl_n64_dp_write(rl_n64 *m, unsigned i, uint32_t v);
/** @brief IMEM [offset, offset + len) was written by the CPU. */
void rl_n64_imem_written(rl_n64 *m, uint32_t offset, uint32_t len);

#endif
