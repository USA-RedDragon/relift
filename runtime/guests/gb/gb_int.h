/**
 * @file gb_int.h
 * @brief The machine record and the helpers the files of this layer share.
 */
#ifndef RELIFT_GB_INT_H
#define RELIFT_GB_INT_H

#include "gb.h"

/** @brief ROM bank size. */
#define GB_ROM_BANK 0x4000u
/** @brief External RAM bank size. */
#define GB_RAM_BANK 0x2000u
/** @brief MBC2 built-in RAM size in half-bytes. */
#define GB_MBC2_RAM 0x200u

/** @brief The real-time clock of an MBC3 cartridge. */
typedef struct gb_rtc {
    /** @brief Clock seconds elapsed since the clock was zero, as of `stamp`. */
    uint64_t seconds;
    /** @brief Host time (the time source) when `seconds` was last brought up to date. */
    uint64_t stamp;
    /** @brief The clock is halted (DH bit 6). */
    bool halt;
    /** @brief The day counter overflowed (DH bit 7, sticky until cleared). */
    bool carry;
    /** @brief The latched registers S, M, H, DL, DH. */
    uint8_t latched[5];
    /** @brief The last value written to the latch register. */
    uint8_t latch_last;
} gb_rtc;

/** @brief The built-in stand-in for a device: a register file. */
typedef struct gb_stub {
    /** @brief The device the scheduler holds. */
    rl_device dev;
    /** @brief Registers indexed by the low byte of the address ($FFFF is index 255). */
    uint8_t regs[256];
} gb_stub;

/** @brief The machine. */
struct rl_gb {
    /** @brief The model. */
    rl_gb_model model;
    /** @brief The cartridge. */
    rl_gb_cart cart;
    /** @brief The post-boot state. */
    rl_gb_boot boot;
    /** @brief Number of 16 KiB ROM banks (at least 2). */
    uint32_t rom_banks;
    /** @brief Color hardware with a Color cartridge. */
    bool color;

    /** @brief External RAM, or NULL. */
    uint8_t *ram;
    /** @brief External RAM bytes. */
    uint32_t ram_len;
    /** @brief Video RAM, two banks of 8 KiB. */
    uint8_t vram[2 * 0x2000];
    /** @brief Work RAM, eight banks of 4 KiB. */
    uint8_t wram[8 * 0x1000];
    /** @brief Object attribute memory. */
    uint8_t oam[0xA0];
    /** @brief The unusable $FEA0-$FEFF cells on color hardware. */
    uint8_t unusable[0x60];
    /** @brief High RAM. */
    uint8_t hram[0x7F];
    /** @brief VBK register bit 0. */
    uint8_t vbk;
    /** @brief SVBK register bits 0 to 2 as written. */
    uint8_t svbk;

    /** @brief MBC1 BANK1, MBC2 and MBC3 ROM bank, or the low 8 bits of MBC5's. */
    uint32_t rom_reg;
    /** @brief MBC1 BANK2, MBC3 RAM bank or clock register, MBC5 RAM bank, or the high bit of MBC5's ROM bank. */
    uint32_t ram_reg;
    /** @brief MBC1 banking mode. */
    bool mode;
    /** @brief External RAM enable. */
    bool ram_enable;
    /** @brief MBC5 rumble motor bit. */
    bool rumble;
    /** @brief The MBC3 clock. */
    gb_rtc rtc;
    /** @brief Host time for the clock. */
    rl_gb_time_fn *time_fn;
    /** @brief Argument of time_fn. */
    void *time_ctx;

    /** @brief The RAM or the clock changed since the last save. */
    bool dirty;

    /** @brief ROM bank at $0000. */
    uint32_t bank0;
    /** @brief ROM bank at $4000. */
    uint32_t bank1;
    /** @brief External RAM bank at $A000. */
    uint32_t rambank;
    /** @brief The scheduler, once attached. */
    rl_sched *sched;
    /** @brief The device of each slot. */
    rl_device *dev[RL_GB_SLOTS];
    /** @brief The stubs, used where the caller gave no device. */
    gb_stub stub[RL_GB_SLOTS];
    /** @brief Read pages of 4 KiB; NULL sends the access to the slow path. */
    const uint8_t *rd[16];
};

/** @brief True for the addresses the scheduler's devices own, besides $FF4F and $FF70. */
static inline bool gb_is_io(uint16_t addr) { return (addr >= 0xFF00 && addr < 0xFF80 && addr != 0xFF4F && addr != 0xFF70) || addr == 0xFFFF; }

/** @brief The work RAM bank at $D000-$DFFF: 1 on monochrome hardware, SVBK with 0 read as 1 on color. */
static inline unsigned gb_wram_bank(const rl_gb *g)
{
    if (!g->color)
        return 1;
    return g->svbk ? g->svbk : 1u;
}

/** @brief Copies the post-boot I/O values into the stubs' registers. */
void gb_seed_stubs(rl_gb *g);
/** @brief Puts the controller registers at their power-on values and maps the banks. */
void gb_reset_mapper(rl_gb *g);
/** @brief Sets the clock from a save's record (rl_gb_save_bytes layout). */
void gb_rtc_restore(rl_gb *g, const uint8_t *rec);
/** @brief Writes the clock record. */
void gb_rtc_record(rl_gb *g, uint8_t *rec);
/** @brief Recomputes bank0, bank1, rambank and the read pages from the controller registers. */
void gb_remap(rl_gb *g);
/** @brief Writes the clock register that RAM bank number reg (8 to 12) selects. */
void gb_rtc_write(rl_gb *g, unsigned reg, uint8_t value);
/** @brief The live clock register reg (0 seconds, 1 minutes, 2 hours, 3 day low, 4 day high and flags), without updating. */
uint8_t gb_rtc_peek(const rl_gb *g, unsigned reg);
/** @brief Host time in seconds from the time source. */
uint64_t gb_host_time(const rl_gb *g);
/** @brief Brings the clock up to the host time. */
void gb_rtc_update(rl_gb *g);
/** @brief A write to $0000-$7FFF. */
void gb_mbc_write(rl_gb *g, uint16_t addr, uint8_t value);
/** @brief A read of external RAM or the clock at $A000-$BFFF, for the pages without a direct pointer. */
uint8_t gb_ram_read(rl_gb *g, uint16_t addr);
/** @brief A write to $A000-$BFFF. */
void gb_ram_write(rl_gb *g, uint16_t addr, uint8_t value);

#endif
