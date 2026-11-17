/**
 * @file nds_dev.h
 * @brief The state and the internal calls of the cartridge slot (cart.c), the
 * cartridge backup chip (backup.c), the ARM7's SPI bus with its devices
 * (spi.c, firmware.c) and the real-time clock (rtc.c). Included by nds_int.h.
 */
#ifndef RELIFT_NDS_DEV_H
#define RELIFT_NDS_DEV_H

#include "nds.h"

#include <relift/sched.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Largest backup memory the SPI chip models (a 1 MiB FLASH). */
#define NDS_BACKUP_MAX 0x100000u
/** @brief Size of the generated firmware image (a 256 KiB flash chip). */
#define NDS_FIRMWARE_SIZE 0x40000u
/** @brief Offset in the I/O window of the cartridge data port (0x04100010). */
#define NDS_CARDPORT 0x100010u

/** @brief The families of backup chips. */
typedef enum nds_bk_kind {
    NDS_BK_NONE,
    NDS_BK_EEPROM,
    NDS_BK_FLASH,
    NDS_BK_FRAM,
} nds_bk_kind;

/** @brief The cartridge's backup chip on the AUXSPI bus. */
typedef struct nds_backup {
    nds_bk_kind kind;
    /** @brief Address bytes of a read or write command (1 for the 0.5 KiB EEPROM, 2 or 3 otherwise). */
    unsigned addr_bytes;
    /** @brief Memory size in bytes. */
    uint32_t len;
    /** @brief Bytes a write may carry (a page); 0 for no limit. */
    uint32_t page;
    /** @brief Kind and size are fixed (a save was loaded, an option named them, or a command chose them). */
    bool fixed;
    /** @brief The memory, 0xFF-filled until written. */
    uint8_t *data;
    bool dirty;
    /** @brief The tick of the last change. */
    uint64_t changed;
    /** @brief A command is open (chip select held). */
    bool open;
    unsigned pos;
    uint8_t op;
    uint32_t addr;
    bool wel;
    bool deep;
    uint8_t wp;
} nds_backup;

/** @brief The cartridge slot: ROMCTRL transfers, the data port and the AUXSPI bus. */
typedef struct nds_cart {
    const uint8_t *rom;
    size_t rom_len;
    /** @brief The ROM size rounded up to a power of two (the address space the chip mirrors). */
    size_t cap;
    uint32_t chip_id;
    /** @brief AUXSPICNT as written (bits 0-1, 6, 13, 14, 15). */
    uint16_t auxcnt;
    /** @brief ROMCTRL as written, without the status and write-only bits. */
    uint32_t romctrl;
    uint8_t cmd[8];
    bool busy;
    uint32_t words, produced, consumed, block_words;
    uint32_t addr;
    uint8_t op;
    /** @brief Ticks of one byte on the cartridge bus, and of the gap2 of a block. */
    uint64_t byte_ticks, gap2_ticks;
    /** @brief The completion tick of the last word produced (the start of the next one). */
    uint64_t at;
    /** @brief The tick the next word completes; NDS_NEVER while stalled or when no word is due. */
    uint64_t next;
    /** @brief The end of a transfer without data. */
    uint64_t end;
    uint32_t last;
    rl_event ev;
    rl_nds *m;
    /** @brief The byte read from AUXSPIDATA and the tick the AUXSPI transfer ends. */
    uint8_t spi_in;
    uint64_t spi_end;
    nds_backup bk;
} nds_cart;

/** @brief The SPI devices. */
enum {
    NDS_SPI_POWER = 0,
    NDS_SPI_FIRMWARE = 1,
    NDS_SPI_TOUCH = 2,
};

/** @brief The ARM7's SPI bus. */
typedef struct nds_spi {
    uint16_t cnt;
    uint8_t data;
    uint64_t end;
    rl_event ev;
    rl_nds *m;
    bool open[3];
    unsigned pos[3];
    uint8_t pm[8];
    /** @brief Set once register 0 bit 6 shut the console down, and the hook told of it. */
    bool off;
    rl_nds_power_off_fn *off_fn;
    void *off_user;
    /** @brief Firmware flash. */
    uint8_t op;
    uint32_t addr;
    bool wel;
    uint8_t *fw;
    /** @brief Touch screen controller: the control byte of the open conversion and the reply bits still to shift out. */
    uint32_t reply;
    unsigned reply_bits;
} nds_spi;

/** @brief The real-time clock. */
typedef struct nds_rtc {
    /** @brief The 16-bit register at 0x04000138. */
    uint16_t reg;
    /** @brief Seconds since 2000-01-01 at tick 0. */
    int64_t base;
    /** @brief The weekday counter: dow_val on the day number dow_day (days since 2000-01-01). */
    uint8_t dow_val;
    int64_t dow_day;
    uint8_t stat1, stat2, adjust, free_reg;
    uint8_t alarm1[3], alarm2[3], freq;
    bool open;
    unsigned nbits;
    uint8_t cmd;
    bool reading;
    uint8_t buf[7];
    unsigned len, byte;
    unsigned bit;
    uint8_t in;
    bool out;
} nds_rtc;

/** @brief Power-on state of the cartridge slot, with no cartridge. */
void nds_cart_reset(rl_nds *m);
/** @brief Posts the slot's event once the scheduler exists. */
void nds_cart_attach(rl_nds *m);
/** @brief Registers 0x1A0 to 0x1BB and the data port (word offset off) at tick now. */
uint32_t nds_cart_read(rl_nds *m, uint32_t off, uint64_t now);
/** @brief A write of the mask bits of v to one of them. */
void nds_cart_write(rl_nds *m, uint32_t off, uint32_t v, uint32_t mask, uint64_t now);
/** @brief Raises a cartridge DMA request for tick t if a word waits in the data port. */
void nds_cart_poke(rl_nds *m, uint64_t t);

/** @brief Starts a chip of kind with len bytes (kind NDS_BK_NONE for none); false when the size does not fit the kind. */
bool nds_bk_set(nds_backup *b, nds_bk_kind kind, uint32_t len);
/** @brief Frees the memory. */
void nds_bk_free(nds_backup *b);
/** @brief Ends a command (chip select released). */
void nds_bk_close(nds_backup *b);
/** @brief One byte on the AUXSPI bus: in is sent, the return value comes back; hold keeps the chip selected afterwards. */
uint8_t nds_bk_byte(nds_backup *b, uint8_t in, bool hold, uint64_t now);

/** @brief Power-on state of the SPI bus and the firmware. */
void nds_spi_reset(rl_nds *m);
/** @brief Frees the firmware image. */
void nds_spi_free(rl_nds *m);
/** @brief Posts the bus's event. */
void nds_spi_attach(rl_nds *m);
/** @brief SPICNT and SPIDATA (word offset 0x1C0) at tick now. */
uint32_t nds_spi_read(rl_nds *m, uint64_t now);
/** @brief A write of the mask bits of v to them. */
void nds_spi_write(rl_nds *m, uint32_t v, uint32_t mask, uint64_t now);
/** @brief Fills image (NDS_FIRMWARE_SIZE bytes) with the generated firmware; no Nintendo code, data or identifiers. */
void nds_firmware_build(uint8_t *image);
/** @brief The CRC-16 of the BIOS, firmware and user settings checks (poly 0xA001, reflected). */
uint16_t nds_crc16(uint16_t init, const uint8_t *p, size_t n);

/** @brief Power-on state of the clock (2000-01-01 00:00:00). */
void nds_rtc_reset(rl_nds *m);
/** @brief The RTC register at 0x04000138 read at tick now. */
uint32_t nds_rtc_read(rl_nds *m, uint64_t now);
/** @brief A write of the mask bits of v to it. */
void nds_rtc_write(rl_nds *m, uint32_t v, uint32_t mask, uint64_t now);

#endif
