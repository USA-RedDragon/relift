/**
 * @file gba.h
 * @brief The Game Boy Advance machine over the device scheduler
 * (core/include/relift/sched.h): the memory map with its mirrors and open
 * bus, the wait states of every region with the Game Pak prefetch buffer,
 * the interrupt controller, the four timers, the four DMA channels, HALTCNT
 * the display timing (VCOUNT, DISPSTAT and the HBlank, VBlank and VCOUNT
 * match events) and the picture unit (ppu/), which draws each line lazily
 * and hands finished frames to a hook and to a frame hash.
 *
 * One scheduler tick is one CPU cycle (16,777,216 Hz). This layer knows the
 * machine, not the CPU: the ARM32 CPU layer reports its bus traffic through
 * rl_gba_fetch, rl_gba_load, rl_gba_store and rl_gba_idle, which charge the
 * cycles of every access to the attached CPU's clock (rl_sched_stall) in the
 * order the bus sees them, so an I/O access reaches its device at the tick
 * the console would. Lifted code that does not report every access uses the
 * wait table (rl_gba_wait_table) for plain memory and these calls for the
 * rest.
 *
 * The timing of the bus, the prefetch buffer, the timers, the interrupt
 * pipeline, DMA and the display follows ares (ISC license, see NOTICE),
 * reshaped from per-cycle threads into closed forms and scheduler events.
 */
#ifndef RELIFT_GBA_H
#define RELIFT_GBA_H

#include "bios/bios.h"

#include <relift/ports.h>
#include <relift/sched.h>
#include <relift/window.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief CPU cycles per second. */
#define RL_GBA_HZ 16777216u
/** @brief Cycles per scanline. */
#define RL_GBA_LINE_CYCLES 1232u
/** @brief Scanlines per frame. */
#define RL_GBA_LINES 228u
/** @brief Visible scanlines. */
#define RL_GBA_VISIBLE_LINES 160u
/** @brief Cycles per frame. */
#define RL_GBA_FRAME_CYCLES (RL_GBA_LINE_CYCLES * RL_GBA_LINES)
/** @brief The cycle of a line at which DISPSTAT's HBlank flag sets. */
#define RL_GBA_HBLANK_DOT 1007u

/** @brief The scheduler line of the CPU's IRQ input: IME set and IE & IF not zero, after the pipeline delay. */
#define RL_GBA_LINE_IRQ 0u
/** @brief The scheduler line that ends a halt: IE & IF not zero (IME does not matter). */
#define RL_GBA_LINE_WAKE 1u
/** @brief The scheduler line raised while a DMA channel is ready to run (rl_gba_service runs it). */
#define RL_GBA_LINE_DMA 2u

/** @brief The interrupt sources, as bits of IE and IF. */
enum {
    RL_GBA_IRQ_VBLANK = 0x0001,
    RL_GBA_IRQ_HBLANK = 0x0002,
    RL_GBA_IRQ_VCOUNT = 0x0004,
    RL_GBA_IRQ_TIMER0 = 0x0008,
    RL_GBA_IRQ_SERIAL = 0x0080,
    RL_GBA_IRQ_DMA0 = 0x0100,
    RL_GBA_IRQ_KEYPAD = 0x1000,
    RL_GBA_IRQ_CART = 0x2000,
};

/** @brief The devices this layer does not model yet; each gets a register-file stub unless the caller supplies one. */
typedef enum rl_gba_slot {
    /** @brief The picture unit: DISPCNT, 0x04000000-0x04000003, and 0x04000008-0x0400005F. */
    RL_GBA_SLOT_PPU,
    /** @brief The sound unit, 0x04000060-0x040000AF. */
    RL_GBA_SLOT_APU,
    /** @brief The serial port, 0x04000120-0x0400012F and 0x04000134-0x0400015B. */
    RL_GBA_SLOT_SERIAL,
    /** @brief The keypad, 0x04000130-0x04000133 (the machine's own keypad, from the p1 port, unless a device is supplied). */
    RL_GBA_SLOT_KEYPAD,
    /** @brief Number of slots. */
    RL_GBA_SLOTS
} rl_gba_slot;

/** @brief The kinds of battery backup a cartridge has. */
typedef enum rl_gba_save_kind {
    /** @brief None (or the caller's SRAM buffer in rl_gba_cart.sram). */
    RL_GBA_SAVE_NONE,
    /** @brief 32 KiB SRAM at 0x0E000000. */
    RL_GBA_SAVE_SRAM,
    /** @brief 64 KiB Flash. */
    RL_GBA_SAVE_FLASH64,
    /** @brief 128 KiB Flash in two banks. */
    RL_GBA_SAVE_FLASH128,
    /** @brief Serial EEPROM, 512 bytes or 8 KiB, on the DMA3 bit stream at 0x0D000000. */
    RL_GBA_SAVE_EEPROM
} rl_gba_save_kind;

/** @brief Flash chip ids: the device id in the high byte, the manufacturer in the low byte. */
enum {
    /** @brief Atmel AT29LV512, 64 KiB, 128-byte pages and no sector erase. */
    RL_GBA_FLASH_ATMEL = 0x3D1F,
    /** @brief Panasonic MN63F805MNP, 64 KiB, 4 KiB sectors (the default of RL_GBA_SAVE_FLASH64). */
    RL_GBA_FLASH_PANASONIC = 0x1B32,
    /** @brief Macronix MX29L512, 64 KiB. */
    RL_GBA_FLASH_MACRONIX64 = 0x1CC2,
    /** @brief SST 39VF512, 64 KiB. */
    RL_GBA_FLASH_SST = 0xD4BF,
    /** @brief Sanyo LE26FV10N1TS, 128 KiB. */
    RL_GBA_FLASH_SANYO = 0x1362,
    /** @brief Macronix MX29L010, 128 KiB (the default of RL_GBA_SAVE_FLASH128). */
    RL_GBA_FLASH_MACRONIX128 = 0x09C2
};

/** @brief The cartridge. */
typedef struct rl_gba_cart {
    /** @brief The ROM image (up to 32 MiB); it stays the caller's and must outlive the machine. */
    const uint8_t *rom;
    /** @brief Length of rom in bytes. */
    size_t rom_len;
    /**
     * @brief Battery SRAM at 0x0E000000 (mirrored), or NULL: reads then
     * return 0xFF and writes are dropped. Stays the caller's. Takes the
     * place of save when set.
     */
    uint8_t *sram;
    /** @brief Length of sram (a power of two, at most 64 KiB). */
    size_t sram_len;
    /** @brief The backup the machine keeps itself (ignored when sram is set); its contents start erased (0xFF). */
    rl_gba_save_kind save;
    /** @brief The Flash chip (RL_GBA_FLASH_*); 0 takes the default of the size. */
    uint16_t flash_id;
    /**
     * @brief Flash programs and erases take their datasheet time, during
     * which reads return the status bits (bit 7 inverted data or 0, bit 6
     * toggling). Off, they finish at once, as in every emulator and the
     * jsmolka tests; a game that polls until done sees no difference.
     */
    bool flash_timed;
    /** @brief The EEPROM size in bytes: 512 or 8192; 0 decides from a loaded file, else from the first DMA3 transfer. */
    uint32_t eeprom_len;
} rl_gba_cart;

/** @brief What the ROM says about its backup. */
typedef struct rl_gba_save_info {
    /** @brief The kind of the library string found, RL_GBA_SAVE_NONE when there is none. */
    rl_gba_save_kind kind;
    /** @brief The Flash chip to use (RL_GBA_FLASH_*) for the Flash kinds, else 0. */
    uint16_t flash_id;
} rl_gba_save_info;

/**
 * @brief Finds the backup library string of a ROM (EEPROM_V, SRAM_V,
 * SRAM_F_V, FLASH_V, FLASH512_V, FLASH1M_V, with or without the version
 * digits after it, as NanoBoyAdvance matches them; the first in the image
 * when several differ). The EEPROM size is not in the string.
 */
rl_gba_save_info rl_gba_save_detect(const uint8_t *rom, size_t len);

/** @brief The machine. */
typedef struct rl_gba rl_gba;

/**
 * @brief Creates a machine at power on with cart inserted: memory cleared,
 * I/O registers at their reset values, the BIOS in its post-boot state.
 * Returns NULL when cart is NULL, the ROM is larger than 32 MiB, the SRAM
 * size is not a power of two up to 64 KiB, or memory runs out.
 */
rl_gba *rl_gba_create(const rl_gba_cart *cart);
/** @brief Frees the machine; devices supplied for slots stay the caller's. */
void rl_gba_destroy(rl_gba *g);

/**
 * @brief Adds the machine's devices to s (the display, the timers, the DMA
 * channels, the system registers, then the slot devices, in that order),
 * maps the I/O registers to them and binds the CPU cpu, whose clock every
 * access is charged to. A NULL entry of devs (or devs NULL) gets a stub.
 * Sets cpu's interrupt mask to the IRQ and DMA lines and starts the display
 * at line 0, dot 0 at the current tick. Returns 0, or -1 when g is attached
 * already or s cannot take the devices.
 */
int rl_gba_attach(rl_gba *g, rl_sched *s, rl_sched_cpu *cpu, rl_device *const devs[RL_GBA_SLOTS]);
/** @brief The CPU bound by rl_gba_attach, or NULL. */
rl_sched_cpu *rl_gba_cpu(const rl_gba *g);
/** @brief The device of a slot (the stub when none was supplied), or NULL before rl_gba_attach. */
rl_device *rl_gba_device(rl_gba *g, rl_gba_slot slot);

/**
 * @brief A code fetch of size bytes (2 in Thumb state, 4 in ARM state) at
 * addr, aligned. seq is the ARM7TDMI's sequential signal: false for the
 * first fetch after a branch, a data access or an internal cycle. A fetch
 * from the Game Pak goes through the prefetch buffer when WAITCNT enables
 * it. Charges the cycles and returns the opcode.
 */
uint32_t rl_gba_fetch(rl_gba *g, uint32_t addr, unsigned size, bool seq);
/**
 * @brief A data read of size bytes (1, 2 or 4) at addr, aligned; seq true
 * for the second and later words of LDM. Runs a DMA transfer that is due
 * first (the CPU waits for it), charges the access and returns the value,
 * zero-extended. The ARM7TDMI's rotation of misaligned loads is the CPU
 * layer's.
 */
uint32_t rl_gba_load(rl_gba *g, uint32_t addr, unsigned size, bool seq);
/**
 * @brief The write twin of rl_gba_load. HALTCNT ignores this write: only
 * the BIOS halts the CPU (rl_gba_bios_bus), as the oracle's console model
 * does for writes from outside the BIOS region.
 */
void rl_gba_store(rl_gba *g, uint32_t addr, unsigned size, uint32_t value, bool seq);
/** @brief cycles internal cycles of the CPU: the bus is free, so the prefetch buffer fills. */
void rl_gba_idle(rl_gba *g, unsigned cycles);
/**
 * @brief Runs the DMA transfers that are due, as at the start of a bus
 * access; the CPU layer calls it when the DMA line ends a budget.
 */
void rl_gba_service(rl_gba *g);
/** @brief Cycles charged by this layer since creation (also when no CPU is attached). */
uint64_t rl_gba_cycles(const rl_gba *g);

/**
 * @brief A read without timing for the BIOS, a debugger or a test: the same
 * map, I/O through the scheduler at the CPU's current tick, nothing charged
 * and the prefetch buffer left alone.
 */
uint32_t rl_gba_read(rl_gba *g, uint32_t addr, unsigned size);
/** @brief The write twin of rl_gba_read (HALTCNT ignores it, as rl_gba_store). */
void rl_gba_write(rl_gba *g, uint32_t addr, unsigned size, uint32_t value);
/** @brief A read of memory only (no I/O: it reads as open bus), without any side effect. */
uint32_t rl_gba_peek(const rl_gba *g, uint32_t addr, unsigned size);

/** @brief Cycles of one access to each region for lifted code: c[addr >> 24][log2 size][seq]. */
typedef struct rl_gba_waits {
    /** @brief Index 0x0 to 0xF; addresses from 0x10000000 up cost one cycle. */
    uint8_t c[16][3][2];
} rl_gba_waits;
/**
 * @brief The current wait table, rebuilt on every WAITCNT or memory control
 * write. Game Pak entries are those without the prefetch buffer.
 */
const rl_gba_waits *rl_gba_wait_table(const rl_gba *g);
/** @brief One lookup in t. */
static inline unsigned rl_gba_wait(const rl_gba_waits *t, uint32_t addr, unsigned size, bool seq)
{
    return addr >= 0x10000000u ? 1u : t->c[addr >> 24][size >> 1][seq];
}

/**
 * @brief The CPU's last code fetch was the size bytes at addr, without
 * cycles: the boot's jump to the cartridge left the pipeline full, so the
 * Game Pak goes on sequentially after addr.
 */
void rl_gba_fetched(rl_gba *g, uint32_t addr, unsigned size);

/** @brief The value the bus holds now (what an unmapped address reads). */
uint32_t rl_gba_open_bus(const rl_gba *g);
/** @brief Sets the bus value, for a CPU layer that fetches opcodes without rl_gba_fetch. */
void rl_gba_set_open_bus(rl_gba *g, uint32_t value);

/** @brief The BIOS state (open bus value of the BIOS region, cycles of the last call). */
rl_gba_bios *rl_gba_bios_of(rl_gba *g);
/**
 * @brief The bus the BIOS's routines use: rl_gba_read and rl_gba_write, and
 * the wait table for their cycles. Its write of HALTCNT halts the CPU there
 * until IE & IF is not zero (STOP: until a keypad, serial or cartridge
 * interrupt), moving the clock over the halted time. After a write that
 * lets an interrupt in (IME, IE or IF) and after a halt ends, it calls the
 * CPU hook's irq, when set, while the request stands.
 */
const rl_gba_bus *rl_gba_bios_bus(rl_gba *g);

/**
 * @brief True when the CPU, at the end of the instruction whose last bus
 * access (rl_gba_fetch, rl_gba_load, rl_gba_store or the last cycle of
 * rl_gba_idle) was the latest one, takes an interrupt (when its I bit
 * allows): the IRQ input is asserted and rose no later than the first cycle
 * of that access. The console does not see a request that arrives during
 * the wait states of the instruction's last access until the end of the
 * next instruction. Runs the events due first.
 */
bool rl_gba_irq_sample(rl_gba *g);
/**
 * @brief The CPU layer is at an instruction boundary and asks for interrupts
 * (before rl_gba_irq_line and rl_gba_irq_sample): until the IRQ input rises
 * again or a sample finds it too late, the machine's own runs of due events
 * inside an access no longer end the CPU's time slice for it.
 */
void rl_gba_irq_boundary(rl_gba *g);

/** @brief Callbacks into the CPU layer. */
typedef struct rl_gba_cpu_hooks {
    /** @brief Passed to every callback. */
    void *user;
    /**
     * @brief Called where the console takes an interrupt inside a bus write
     * (the BIOS contract in rl_gba_bus): the CPU layer enters the handler
     * when its I bit allows and returns after the handler ran.
     */
    void (*irq)(void *user);
    /**
     * @brief Takes the interrupt inside a BIOS call (rl_gba_bus.point, with
     * the line asserted and spsr's I bit clear) and returns an
     * rl_gba_point. When set, rl_gba_bios_bus is timed: the BIOS's
     * accesses and cycles happen on the CPU's clock, and irq is not called.
     * regs: the registers the call gives a handler (as rl_gba_bus.point
     * gives them).
     */
    int (*point)(void *user, uint32_t lr, uint32_t spsr, const uint32_t *regs);
} rl_gba_cpu_hooks;
/** @brief Sets the hooks (NULL clears them). */
void rl_gba_set_cpu_hooks(rl_gba *g, const rl_gba_cpu_hooks *hooks);

/** @brief True while the CPU's IRQ input is asserted (IME, IE and IF, after the pipeline delay). */
bool rl_gba_irq_line(const rl_gba *g);
/** @brief Requests the interrupts in sources now (for devices outside this layer: keypad, serial, cartridge). */
void rl_gba_request_irq(rl_gba *g, uint16_t sources);
/** @brief The backup kind the machine has: the cart's save, RL_GBA_SAVE_SRAM for a caller's SRAM, else none. */
rl_gba_save_kind rl_gba_save_kind_of(const rl_gba *g);
/** @brief Length of the backup in bytes: the raw .sav size (an EEPROM of unknown size reads 0). */
size_t rl_gba_save_len(const rl_gba *g);
/** @brief The backup bytes in the order of a raw .sav (Flash bank 0 first; EEPROM blocks in address order, bytes as sent); valid for the machine's life. */
const uint8_t *rl_gba_save_data(const rl_gba *g);
/** @brief True when the backup changed since the last load, write or rl_gba_save_clean. */
bool rl_gba_save_dirty(const rl_gba *g);
/** @brief Marks the backup as saved. */
void rl_gba_save_clean(rl_gba *g);
/**
 * @brief Replaces the backup by a raw .sav image. Returns 0, or -1 (nothing
 * changed) when the kind has no backup or len is not a size the kind
 * allows (SRAM 32 KiB or the caller's buffer size, Flash 64 or 128 KiB,
 * EEPROM 512 or 8192). An EEPROM takes its size from len.
 */
int rl_gba_save_load(rl_gba *g, const uint8_t *data, size_t len);
/** @brief rl_gba_save_load of the file at path: 0, 1 when there is no file or no backup, -1 when it cannot be read or has the wrong size. */
int rl_gba_save_read(rl_gba *g, const char *path);
/**
 * @brief Writes the backup to path through a temporary file and a rename
 * (a crash leaves the old file), creating the folder. Returns 0, 1 when
 * there is nothing to write (no backup, or an EEPROM never used), -1 on
 * failure (the backup stays dirty).
 */
int rl_gba_save_write(rl_gba *g, const char *path);
/**
 * @brief Writes the backup like rl_gba_save_write when it is dirty and has
 * not changed for quiet cycles of the CPU clock. Returns 1 when it wrote,
 * 0 when it did nothing, -1 on failure.
 */
int rl_gba_save_poll(rl_gba *g, const char *path, uint64_t quiet);
/**
 * @brief Writes the path of the .sav of the ROM at rom_path: the name of
 * the ROM with .sav, in the save.dir setting's folder; when that is empty,
 * in <user dir>/save when the program names an app (rl_settings_set_app),
 * else next to the ROM. Returns false when out is too small.
 */
bool rl_gba_save_path(char *out, size_t size, const char *rom_path);
/** @brief Declares the runtime settings and save.dir (environment RELIFT_SAVE). */
void rl_gba_declare_settings(void);

/**
 * @brief Pins the keys to pressed (KEYINPUT bit order: A, B, Select, Start,
 * Right, Left, Up, Down, R, L, a set bit is a pressed key) instead of
 * reading the p1 port, and raises the KEYCNT interrupt when it now holds.
 */
void rl_gba_keypad_set(rl_gba *g, uint16_t pressed);
/** @brief Reads the keys from the p1 port of rl_gba_ports again (the default). */
void rl_gba_keypad_use_ports(rl_gba *g);
/** @brief The keys as the keypad last saw them, one bit per key as in rl_gba_keypad_set. */
uint16_t rl_gba_keypad_pressed(const rl_gba *g);
/** @brief The native ports of the GBA (generated from relift-input): declare them with rl_ports_declare. */
extern const rl_port_decl rl_gba_ports[];
/** @brief Number of entries of rl_gba_ports. */
extern const uint32_t rl_gba_ports_count;
/** @brief The on-screen controller's port (touchpad.h), declared when the run uses it. */
extern const rl_port_decl rl_gba_ports_touchpad[];
/** @brief Number of entries of rl_gba_ports_touchpad. */
extern const uint32_t rl_gba_ports_touchpad_count;
struct rl_touchpad_guest;
/** @brief The GBA on-screen controller layouts. */
const struct rl_touchpad_guest *rl_gba_touchpad(void);

/** @brief True while the CPU is halted (by HALTCNT; only seen from callbacks that run during the halt). */
bool rl_gba_halted(const rl_gba *g);

/**
 * @brief The sound unit's request for more FIFO data: arms DMA channel 1
 * (fifo 0, FIFO A) or 2 (fifo 1, FIFO B) when it is enabled in special
 * timing (four words, fixed destination).
 */
void rl_gba_dma_fifo(rl_gba *g, unsigned fifo);
/** @brief Called at a timer overflow (timer 0 to 3) at tick when, for the sound unit's FIFOs. */
typedef void rl_gba_timer_fn(void *user, unsigned timer, uint64_t when);
/** @brief Sets the timer overflow callback (fn NULL clears it). */
void rl_gba_set_timer_hook(rl_gba *g, rl_gba_timer_fn *fn, void *user);

/** @brief Called at a display edge, at tick when (line is the new VCOUNT). */
typedef void rl_gba_line_fn(void *user, unsigned line, uint64_t when);
/** @brief Sets the callback run at the start of VBlank (line 160), for presenting a frame (fn NULL clears it). */
void rl_gba_set_vblank_hook(rl_gba *g, rl_gba_line_fn *fn, void *user);
/**
 * @brief Called when the picture unit has finished a frame, at the start of
 * VBlank (tick when): pixels are the 240 by 160 BGR555 halfwords, valid
 * until the callback returns.
 */
typedef void rl_gba_frame_fn(void *user, const uint16_t *pixels, uint64_t when);
/**
 * @brief Sets the callback run at the end of each frame (fn NULL clears it).
 * While one is set the end of every frame is a scheduler event, so it runs
 * on time; without one the picture unit catches up when something reads it.
 */
void rl_gba_set_frame_hook(rl_gba *g, rl_gba_frame_fn *fn, void *user);
/** @brief Frames finished so far, up to the CPU's tick (the picture unit is brought up to date first). */
uint64_t rl_gba_frames(rl_gba *g);
/** @brief FNV-1a 64 of the last finished frame's pixels as little-endian halfwords (0 before the first). */
uint64_t rl_gba_frame_hash(rl_gba *g);
/** @brief The last finished frame (240 by 160 BGR555 halfwords), or NULL before the first. */
const uint16_t *rl_gba_frame(rl_gba *g);
/** @brief Expands a frame (240 by 160 BGR555 halfwords) to RGB888 rows, 5 bits to 8 by repeating the top bits. */
void rl_gba_frame_rgb(const uint16_t *pixels, uint8_t *rgb);
/** @brief Shows a frame in a host window through the window layer; false when the backend cannot present pixels (headless). One caller at a time. */
bool rl_gba_present(rl_window *w, const uint16_t *pixels);
/**
 * @brief Sets the cycle of a line at which the picture unit draws it (default
 * 24: NanoBoyAdvance, which follows the console, takes the first tile of a
 * line from the registers at that cycle, so a write at cycle 23 or before
 * shows in the whole line and a write from cycle 24 on shows from the second
 * tile at the earliest, in the next line in practice). Takes effect from the next line.
 */
void rl_gba_set_render_dot(rl_gba *g, unsigned dot);
/** @brief Sets whether the CPU is in STOP (the picture unit shows white). */
void rl_gba_set_stopped(rl_gba *g, bool stopped);

/** @brief Moves the display so that the current tick is dot of line (both wrapped into the frame). */
void rl_gba_lcd_set(rl_gba *g, unsigned line, unsigned dot);
/** @brief VCOUNT at the CPU's current tick. */
unsigned rl_gba_vcount(const rl_gba *g);

/** @brief Work RAM (256 KiB external, 32 KiB internal), palette (1 KiB), video RAM (96 KiB) and OAM (1 KiB). */
typedef enum rl_gba_region {
    RL_GBA_EWRAM,
    RL_GBA_IWRAM,
    RL_GBA_PALETTE,
    RL_GBA_VRAM,
    RL_GBA_OAM,
    RL_GBA_REGIONS
} rl_gba_region;
/** @brief The backing store of a region and its length. */
uint8_t *rl_gba_mem(rl_gba *g, rl_gba_region r, size_t *len);

#endif
