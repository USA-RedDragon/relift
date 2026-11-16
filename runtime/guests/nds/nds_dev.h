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

#endif
