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
        uint32_t mem_addr;
        uint32_t dram_addr;
        uint32_t rd_len;
        uint32_t wr_len;
        uint32_t status;
        uint32_t semaphore;
        uint32_t pc;
    } sp;

    uint32_t dp[8];

    struct {
        uint32_t dram;
        uint32_t len;
        uint32_t control;
        uint32_t dacrate;
        uint32_t bitrate;
    } ai;

    struct {
        uint32_t dram;
        uint32_t pif_rd;
        uint32_t pif_wr;
        bool interrupt;
    } si;

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

/** @brief VI register read (index 0 to 13), with the line counter brought up to now. */
uint32_t rl_n64_vi_read(rl_n64 *m, unsigned index);
/** @brief VI register write. */
void rl_n64_vi_write(rl_n64 *m, unsigned index, uint32_t value);
/** @brief Starts the line events. */
void rl_n64_vi_attach(rl_n64 *m);

#endif
