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

/** @brief The state a revision's boot program hands the cartridge. */
typedef struct rl_gb_boot {
    /** @brief The revision. */
    rl_gb_model model;
    /** @brief The entry is for a color cartridge on color hardware (false for the compatibility mode and for monochrome hardware). */
    bool color_mode;
    /** @brief CPU registers. */
    uint8_t a, f, b, c, d, e, h, l;
    /** @brief Program counter: the cartridge entry point. */
    uint16_t pc;
    /** @brief Stack pointer. */
    uint16_t sp;
    /** @brief The 16-bit divider counter ($FF04 is its high byte). */
    uint16_t div_counter;
    /** @brief The picture unit's position in its frame, in dots since the start of line 0 (456 dots per line). */
    uint32_t frame_dot;
    /** @brief Interrupt enable, $FFFF. */
    uint8_t ie;
    /** @brief The I/O registers $FF00-$FF7F as the CPU reads them. */
    uint8_t io[128];
} rl_gb_boot;

/**
 * @brief The table entry of a revision, for a cartridge whose header the boot
 * program does not look at. color_mode selects the full color hand-off over
 * the compatibility one on color hardware and is ignored elsewhere. The
 * compatibility hand-off of color hardware lands later by up to a few
 * thousand dots depending on the title and licensee bytes of the header;
 * div_counter holds the base value. NULL for a model out of range.
 */
const rl_gb_boot *rl_gb_boot_state(rl_gb_model model, bool color_mode);
/**
 * @brief Copies the entry into out and applies what the header changes: the
 * flags of the original and pocket models follow the header checksum byte,
 * and the divider of the Super Game Boys follows the bits of the header
 * they send to the console. rom may be NULL.
 */
void rl_gb_boot_state_for(rl_gb_model model, bool color_mode, const uint8_t *rom, size_t len, rl_gb_boot *out);

/** @brief The Game Boy memory. */
typedef struct rl_gb rl_gb;

/** @brief An external owner of the memory behind the I/O registers: the exact device library (gbdev.h) keeps video RAM, OAM, work RAM and the cartridge itself, because the picture unit and the DMA engines read and write them. */
typedef struct rl_gb_bus {
    /** @brief Reads the byte at addr, one M-cycle of the instruction in flight. */
    uint8_t (*read)(void *ctx, uint16_t addr);
    /** @brief Writes the byte at addr, one M-cycle of the instruction in flight. */
    void (*write)(void *ctx, uint16_t addr, uint8_t value);
    /** @brief Reads the byte at addr with no timing effect (for rl_gb_peek), or NULL for $FF. */
    uint8_t (*peek)(void *ctx, uint16_t addr);
    /** @brief Passed to the three. */
    void *ctx;
} rl_gb_bus;

/**
 * @brief Hands every access outside the I/O registers (the ones the
 * scheduler's devices own) to bus instead of this layer's own memory, which
 * is then unused: no direct read pages, no controller, no save. NULL goes
 * back to the own memory. The controller registers and RAM contents of this
 * layer are not carried over, so set a bus right after rl_gb_create.
 */
void rl_gb_set_bus(rl_gb *g, const rl_gb_bus *bus);

/** @brief The post-boot state of g (rl_gb_boot_state_for for its model and cartridge). */
const rl_gb_boot *rl_gb_boot_of(const rl_gb *g);
/**
 * @brief Puts the memory-side registers (the bank registers and the
 * controller) at their post-boot values and, once attached, the stub
 * devices' registers. RAM, VRAM and the rest are left alone. rl_gb_create
 * does this already.
 */
void rl_gb_reset(rl_gb *g);

/**
 * @brief Adds the device of every slot to s, in slot order (the order the
 * scheduler catches devices up in), and maps the I/O ranges to them. A NULL
 * entry of devs gets a built-in stub that stores what is written to its
 * registers and reads it back (seeded with the post-boot values, see
 * rl_gb_reset); devs itself may be NULL for stubs everywhere. The devices
 * must not have been added to s yet, and they stay the caller's. Returns 0,
 * or -1 when s cannot take them or g is attached already.
 */
int rl_gb_attach(rl_gb *g, rl_sched *s, rl_device *const devs[RL_GB_SLOTS]);
/** @brief The device that owns slot (the stub when the caller gave none), or NULL before rl_gb_attach. */
rl_device *rl_gb_device(rl_gb *g, rl_gb_slot slot);

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
/** @brief Bytes of the clock record that follows the RAM in a save of a cartridge with a clock. */
#define RL_GB_SAVE_CLOCK_BYTES 48u
/** @brief Bytes of the battery save of g: the RAM, then the clock record when the board has a clock. 0 when the cartridge has no battery. */
size_t rl_gb_save_size(const rl_gb *g);
/**
 * @brief Writes the battery save into out (rl_gb_save_size bytes). The
 * clock record is five 32-bit little-endian registers (seconds, minutes,
 * hours, day low, day high with halt and carry), the same five latched, and
 * a 64-bit host time, the layout other emulators share. Clears the dirty
 * flag. Returns the bytes written.
 */
size_t rl_gb_save_bytes(rl_gb *g, uint8_t *out);
/**
 * @brief Restores a save made by rl_gb_save_bytes. A clock keeps counting
 * from the host time recorded in the save. Returns 0, or -1 when len fits
 * neither the RAM alone nor the RAM with the clock record, or the
 * cartridge has no battery.
 */
int rl_gb_load_bytes(rl_gb *g, const uint8_t *data, size_t len);
/** @brief True when the RAM or the clock changed since the last save or load. */
bool rl_gb_save_dirty(const rl_gb *g);
/**
 * @brief Writes the battery save to the file at path through a temporary
 * file and a rename, so a crash leaves the old save. Returns 0, 1 when the
 * cartridge has no battery (nothing written), or -1 on a failure.
 */
int rl_gb_save(rl_gb *g, const char *path);
/** @brief Loads the battery save at path. Returns 0, 1 when the file does not exist or the cartridge has no battery, or -1 when it cannot be read or has the wrong size. */
int rl_gb_load(rl_gb *g, const char *path);
/** @brief True while an MBC5 rumble cartridge's motor bit is set. */
bool rl_gb_rumble(const rl_gb *g);

struct rl_touchpad_guest;
/** @brief The GB and GBC on-screen controller layouts (touchpad.h). */
const struct rl_touchpad_guest *rl_gb_touchpad(void);

#endif
