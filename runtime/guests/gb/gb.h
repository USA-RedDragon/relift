/**
 * @file gb.h
 * @brief The Game Boy and Game Boy Color memory map: cartridge ROM and RAM
 * behind the memory bank controller, video RAM, work RAM, echo RAM, object
 * attribute memory, high RAM, and the I/O registers routed to the device
 * scheduler (core/include/relift/sched.h).
 *
 * This layer knows the machine's memory, not its CPU: a CPU layer or lifted
 * code calls rl_gb_read and rl_gb_write with a 16-bit address. Addresses in
 * the I/O range go to the scheduler as accesses of the CPU that made them, so
 * the devices that own them are caught up to that tick first. The picture
 * unit, the sound unit, the timer, the serial port, the joypad and the DMA
 * engines are not in this layer: each is a rl_device that a later chunk
 * supplies for its slot (rl_gb_device_slot). Until then a slot holds a stub
 * that stores what is written and reads it back.
 */
#ifndef RELIFT_GB_H
#define RELIFT_GB_H

#include <relift/sched.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief The hardware revisions the post-boot state is kept for. */
typedef enum rl_gb_model {
    /** @brief Original Game Boy (DMG-CPU A to C). */
    RL_GB_DMG,
    /** @brief The earliest Japanese Game Boy. */
    RL_GB_DMG0,
    /** @brief Game Boy Pocket. */
    RL_GB_MGB,
    /** @brief Super Game Boy. */
    RL_GB_SGB,
    /** @brief Super Game Boy 2. */
    RL_GB_SGB2,
    /** @brief Game Boy Color, CGB-CPU-0. */
    RL_GB_CGB0,
    /** @brief Game Boy Color, CPU-CGB-A or B. */
    RL_GB_CGBB,
    /** @brief Game Boy Color, CPU-CGB-C (the default). */
    RL_GB_CGB,
    /** @brief Game Boy Color, CPU-CGB-D or E. */
    RL_GB_CGBE,
    /** @brief Game Boy Advance in Game Boy Color mode. */
    RL_GB_AGB,
    /** @brief Number of models. */
    RL_GB_MODELS
} rl_gb_model;

/** @brief True for the models that have the Game Boy Color feature set. */
static inline bool rl_gb_model_is_cgb(rl_gb_model m) { return m >= RL_GB_CGB0 && m < RL_GB_MODELS; }

/** @brief The memory bank controllers this layer models. */
typedef enum rl_gb_mbc {
    /** @brief No controller: 32 KiB of ROM, optionally RAM. */
    RL_GB_MBC_NONE,
    /** @brief MBC1; see rl_gb_cart.multicart for the multicart wiring. */
    RL_GB_MBC1,
    /** @brief MBC2 with its 512 half-bytes of RAM. */
    RL_GB_MBC2,
    /** @brief MBC3, with a real-time clock when rl_gb_cart.rtc is set. */
    RL_GB_MBC3,
    /** @brief MBC5, with a rumble motor when rl_gb_cart.rumble is set. */
    RL_GB_MBC5,
    /** @brief A controller this layer does not model (MBC6, MBC7, MMM01, HuC1, HuC3, camera, TAMA5). */
    RL_GB_MBC_UNSUPPORTED
} rl_gb_mbc;

/** @brief What the cartridge is: the header fields the memory map depends on. */
typedef struct rl_gb_cart {
    /** @brief The ROM image; it stays the caller's and must outlive the machine. */
    const uint8_t *rom;
    /** @brief Length of rom in bytes. */
    size_t rom_len;
    /** @brief The controller. */
    rl_gb_mbc mbc;
    /** @brief External RAM size in bytes (MBC2's built-in RAM is not counted). */
    uint32_t ram_len;
    /** @brief The RAM is battery backed. */
    bool battery;
    /** @brief A real-time clock is on the board (MBC3). */
    bool rtc;
    /** @brief A rumble motor is on the board (MBC5). */
    bool rumble;
    /** @brief The MBC1 board is wired as a multicart. */
    bool multicart;
    /** @brief The header's Game Boy Color flag ($0143 bit 7): the cartridge runs in color mode on color hardware. */
    bool cgb;
} rl_gb_cart;

/**
 * @brief Reads the cartridge type, the ROM and RAM size codes and the Color
 * flag out of the header of rom into cart (rom and rom_len are set too), and
 * detects an MBC1 multicart. Returns 0, or -1 when the image is shorter than
 * the header or a code is not assigned. A controller this layer does not
 * model still parses, with mbc set to RL_GB_MBC_UNSUPPORTED.
 */
int rl_gb_cart_parse(const uint8_t *rom, size_t len, rl_gb_cart *cart);

/** @brief The scheduler devices that own the I/O registers. */
typedef enum rl_gb_slot {
    /** @brief The joypad, $FF00. */
    RL_GB_SLOT_JOYPAD,
    /** @brief The serial port, $FF01-$FF02. */
    RL_GB_SLOT_SERIAL,
    /** @brief The timer, $FF04-$FF07. */
    RL_GB_SLOT_TIMER,
    /** @brief The sound unit, $FF10-$FF26, wave RAM $FF30-$FF3F and the PCM registers $FF76-$FF77. */
    RL_GB_SLOT_APU,
    /** @brief The picture unit, $FF40-$FF45, $FF47-$FF4B, $FF68-$FF6C. */
    RL_GB_SLOT_PPU,
    /** @brief The OAM DMA register $FF46 and the HDMA registers $FF51-$FF55. */
    RL_GB_SLOT_DMA,
    /** @brief Interrupt flag and enable ($FF0F, $FFFF), speed switch, boot ROM disable and the other system registers. */
    RL_GB_SLOT_SYSTEM,
    /** @brief Number of slots. */
    RL_GB_SLOTS
} rl_gb_slot;

/** @brief One run of I/O addresses and the slot that owns it. */
typedef struct rl_gb_io_range {
    /** @brief First address. */
    uint16_t lo;
    /** @brief Last address, inclusive. */
    uint16_t hi;
    /** @brief The owner. */
    rl_gb_slot slot;
} rl_gb_io_range;

/** @brief The I/O ranges the scheduler is given, in address order; the table ends at the entry whose hi is 0. */
extern const rl_gb_io_range rl_gb_io_ranges[];

/** @brief The Game Boy memory. */
typedef struct rl_gb rl_gb;

/** @brief The result of creating a machine. */
typedef enum rl_gb_status {
    /** @brief Success. */
    RL_GB_OK = 0,
    /** @brief Out of memory. */
    RL_GB_ERR_MEMORY = -1,
    /** @brief The cartridge is empty or its size does not fit its header. */
    RL_GB_ERR_ROM = -2,
    /** @brief The controller is not modelled. */
    RL_GB_ERR_UNSUPPORTED = -3
} rl_gb_status;

/**
 * @brief Creates the memory of a machine of the given model with cart
 * inserted, all memory cleared and the bank registers at their power-on
 * values. Returns NULL when status is not RL_GB_OK (status says why; it may
 * be NULL).
 */
rl_gb *rl_gb_create(rl_gb_model model, const rl_gb_cart *cart, rl_gb_status *status);
/** @brief Frees the machine's memory. A battery save is not written; see rl_gb_save. */
void rl_gb_destroy(rl_gb *g);
/** @brief The model. */
rl_gb_model rl_gb_get_model(const rl_gb *g);
/** @brief True when the machine runs in color mode: color hardware and a cartridge with the Color flag. */
bool rl_gb_color_mode(const rl_gb *g);

/**
 * @brief Reads the byte at addr as the CPU of c does. ROM, RAM and the
 * other plain memory answer at once; an I/O address goes to the scheduler
 * as an access of c at its current tick. With c NULL an I/O address reads
 * $FF.
 */
uint8_t rl_gb_read(rl_gb *g, rl_sched_cpu *c, uint16_t addr);
/** @brief The write twin of rl_gb_read. Writes to $0000-$7FFF go to the memory bank controller. */
void rl_gb_write(rl_gb *g, rl_sched_cpu *c, uint16_t addr, uint8_t value);
/**
 * @brief A read for a DMA engine or a debugger: the same map as rl_gb_read,
 * but an I/O address reads $FF and nothing is caught up.
 */
uint8_t rl_gb_peek(const rl_gb *g, uint16_t addr);

/** @brief Video RAM bank (0, or 0 and 1 on color hardware) for the picture unit; 8 KiB. */
uint8_t *rl_gb_vram(rl_gb *g, unsigned bank);
/** @brief The video RAM bank the CPU sees at $8000-$9FFF (the VBK register). */
unsigned rl_gb_vram_bank(const rl_gb *g);
/** @brief Object attribute memory, 160 bytes. */
uint8_t *rl_gb_oam(rl_gb *g);
/** @brief High RAM, 127 bytes ($FF80-$FFFE). */
uint8_t *rl_gb_hram(rl_gb *g);
/** @brief The work RAM bank the CPU sees at $D000-$DFFF (1 to 7). */
unsigned rl_gb_wram_bank(const rl_gb *g);
/** @brief The ROM bank mapped at $0000-$3FFF. */
unsigned rl_gb_rom_bank0(const rl_gb *g);
/** @brief The ROM bank mapped at $4000-$7FFF. */
unsigned rl_gb_rom_bank1(const rl_gb *g);
/** @brief The external RAM bank mapped at $A000-$BFFF. */
unsigned rl_gb_ram_bank(const rl_gb *g);
/** @brief True while external RAM (or the clock registers) answer at $A000-$BFFF. */
bool rl_gb_ram_enabled(const rl_gb *g);
/** @brief The external RAM, ram_len bytes (512 for MBC2), or NULL when the cartridge has none. */
uint8_t *rl_gb_ram(rl_gb *g, size_t *len);
/** @brief A source of host time in whole seconds for the MBC3 clock. */
typedef uint64_t rl_gb_time_fn(void *ctx);
/** @brief Sets where the MBC3 clock reads host time from; fn NULL restores the system clock. The clock keeps its value. */
void rl_gb_set_time_source(rl_gb *g, rl_gb_time_fn *fn, void *ctx);
/** @brief True while an MBC5 rumble cartridge's motor bit is set. */
bool rl_gb_rumble(const rl_gb *g);

#endif
