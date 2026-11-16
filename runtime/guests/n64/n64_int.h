/**
 * @file n64_int.h
 * @brief The machine's state, shared by its files.
 */
#ifndef RELIFT_N64_INT_H
#define RELIFT_N64_INT_H

#include "n64.h"

#include "core.h"

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

/** @brief Where a code run is: not loaded, loaded and not checked, or checked and in dispatch. */
enum { CODE_OUT, CODE_LOADED, CODE_LIVE };

struct rl_n64 {
    const uint8_t *rom;
    size_t rom_len;
    uint8_t *rdram;
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
        bool start_valid, end_valid, xbus, freeze, flush, busy, ready;
        uint64_t clock_base;
        rl_n64_dp_submit *submit;
        void *submit_user;
    } dp;

    struct {
        uint32_t dram;
        uint32_t len;
        uint32_t control;
        uint32_t dacrate;
        uint32_t bitrate;
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
    } code;
};

/** @brief The CPU's tick now. */
uint64_t rl_n64_now(const rl_n64 *m);
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
