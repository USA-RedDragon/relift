/**
 * @file nds.h
 * @brief The Nintendo DS machine over the device scheduler
 * (core/include/relift/sched.h): the ARM9 and ARM7 memory maps with their
 * mirrors, the shared work RAM banks, the tightly coupled memories of the
 * ARM9 (from the ARMv5TE CP15 registers of the CPU layer), the BIOS regions
 * as high-level stubs, the I/O registers of each CPU with its interrupt
 * controller, IPCSYNC and the IPC FIFOs, four timers per CPU, the ARM9's
 * division and square root unit, and the access costs of every region.
 *
 * Time. One scheduler tick is half a bus cycle (1/67,027,964 s): an ARM9
 * cycle is one tick (rl_sched_add_cpu ratio 1) and an ARM7 cycle two ticks
 * (ratio 2). Bus accesses of either CPU start on a bus cycle edge (an even
 * tick); the ARM9's tightly coupled memories do not touch the bus.
 *
 * This layer knows the machine, not the CPUs' instruction sets: a CPU layer
 * reports its bus traffic through rl_nds_fetch, rl_nds_load and
 * rl_nds_store, which charge the cost of each access to the CPU's clock
 * (rl_sched_stall) in the order the bus sees them. The cost tables follow
 * GBATEK's "DS Memory Timings" (first tier: no ARM9 cache model, every ARM9
 * access outside the TCMs goes to the bus).
 */
#ifndef RELIFT_NDS_H
#define RELIFT_NDS_H

#include "cp15.h"

#include <relift/sched.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Scheduler ticks per second: two per bus cycle. */
#define RL_NDS_TICK_HZ 67027964u
/** @brief Bus cycles per second (the ARM7 clock and the timers' F). */
#define RL_NDS_BUS_HZ 33513982u

/** @brief Ticks of one dot of the display (six bus cycles; GBATEK "DS Video Stuff"). */
#define RL_NDS_DOT_TICKS 12u
/** @brief Ticks of one line: 355 dots, 256 of them drawn. */
#define RL_NDS_LINE_TICKS 4260u
/** @brief Lines of one frame: 192 drawn, 71 of VBlank. */
#define RL_NDS_LINES 263u
/** @brief Ticks of one frame (59.8261 Hz). */
#define RL_NDS_FRAME_TICKS (RL_NDS_LINE_TICKS * RL_NDS_LINES)

/** @brief The CPUs, as the cpu argument of the bus calls. */
enum {
    RL_NDS_ARM9 = 0,
    RL_NDS_ARM7 = 1,
};

/** @brief Sizes of the memories. */
enum {
    RL_NDS_MAIN_SIZE = 0x400000,
    RL_NDS_SWRAM_SIZE = 0x8000,
    RL_NDS_WRAM7_SIZE = 0x10000,
    RL_NDS_PAL_SIZE = 0x800,
    RL_NDS_OAM_SIZE = 0x800,
    /** @brief The video RAM banks A to I one after the other, as the ARM9's LCDC window shows them (656 KiB). */
    RL_NDS_VRAM_SIZE = 0xA4000,
};

/** @brief The memories of the machine. */
typedef enum rl_nds_region {
    /** @brief Main RAM, 4 MiB. */
    RL_NDS_MAIN,
    /** @brief Shared work RAM, 32 KiB (WRAMCNT gives its halves to the CPUs). */
    RL_NDS_SWRAM,
    /** @brief The ARM7's own work RAM, 64 KiB. */
    RL_NDS_WRAM7,
    /** @brief ARM9 instruction TCM, 32 KiB. */
    RL_NDS_ITCM,
    /** @brief ARM9 data TCM, 16 KiB. */
    RL_NDS_DTCM,
    /** @brief Standard palettes, 2 KiB. */
    RL_NDS_PAL,
    /** @brief Video RAM, the banks A to I in their LCDC order (RL_NDS_VRAM_SIZE). */
    RL_NDS_VRAM,
    /** @brief Object attribute memory, 2 KiB. */
    RL_NDS_OAM,
    /** @brief Number of regions. */
    RL_NDS_REGIONS
} rl_nds_region;

/** @brief The machine. */
typedef struct rl_nds rl_nds;

/** @brief Creates a machine at power on: memory cleared, registers at their reset values, no BIOS image. NULL when memory runs out. */
rl_nds *rl_nds_create(void);
/** @brief Frees the machine. */
void rl_nds_destroy(rl_nds *m);

/** @brief The scheduler line that ends a halt of the ARM9 (IE & IF not zero); a CPU layer keeps it in the CPU's mask whatever its I bit. */
#define RL_NDS_LINE_WAKE9 0u
/** @brief The ARM7's wake line. */
#define RL_NDS_LINE_WAKE7 1u

/**
 * @brief Adds the machine's devices to s, maps each CPU's I/O registers and
 * binds the two CPUs: arm9 must count one tick per cycle and arm7 two, both
 * spawned on fibers (rl_sched_spawn) when they run timed code. s must be
 * created with rl_sched_config.ordered set, so that the CPUs agree on the
 * order of their shared accesses (rl_sched_order). Routes the wake lines,
 * sets the CPUs' masks to them and their ranks from EXMEMCNT. Returns 0, or
 * -1 when m is attached already, a CPU is missing or s cannot take the
 * devices.
 */
int rl_nds_attach(rl_nds *m, rl_sched *s, rl_sched_cpu *arm9, rl_sched_cpu *arm7);
/** @brief The scheduler CPU of cpu (RL_NDS_ARM9 or RL_NDS_ARM7), or NULL before rl_nds_attach. */
rl_sched_cpu *rl_nds_cpu(const rl_nds *m, unsigned cpu);
/** @brief The tick cpu is at (0 before rl_nds_attach). */
uint64_t rl_nds_now(const rl_nds *m, unsigned cpu);

/**
 * @brief A code fetch of size bytes (2 or 4) at addr, aligned. The ARM9
 * fetches every opcode as a nonsequential word outside its TCMs (seq is
 * ignored there). Charges the cost and returns the opcode.
 */
uint32_t rl_nds_fetch(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, bool seq);
/**
 * @brief The cost in ticks of an ARM9 code fetch of size bytes that lifted
 * code makes without reading the bytes: an instruction's own fetch named by
 * its address (seq true), or a refill's two named by the target (seq false,
 * then true). A cached fetch charges the instruction cache's hit or line
 * fill to the clock itself and gives 0, an uncached one waits for the write
 * buffer and gives the region's cost.
 */
unsigned rl_nds_code_cost(rl_nds *m, uint32_t addr, unsigned size, bool seq);
/**
 * @brief The ARM7's cycles of a code fetch of size bytes at addr in main RAM, which the fetch does not charge itself;
 * seq false for the first fetch of a refill.
 */
unsigned rl_nds_code_cost7(rl_nds *m, uint32_t addr, unsigned size, bool seq);
/** @brief How a data access follows the ones before it in its instruction (rl_nds_load). */
enum {
    /** @brief The instruction's first access: nonsequential. */
    RL_NDS_N = 0,
    /** @brief The next word of an LDM or STM: sequential. */
    RL_NDS_S = 1,
    /** @brief Nonsequential after another access of the instruction (SWP's write): it starts when that one ends. */
    RL_NDS_AFTER = 2,
    /**
     * @brief The second word of the ARM9's LDRD: a new transfer when that of the first word ends, a bus cycle more
     * than a sequential word (a full nonsequential access in main RAM).
     */
    RL_NDS_PAIR = 3,
};

/**
 * @brief A data read of size bytes (1, 2 or 4) at addr, aligned down; how
 * the access follows the instruction's others (RL_NDS_N and the rest).
 * Waits for the other CPU when the access is ordered (rl_nds_ordered),
 * stalls on a busy main RAM, charges the cost and returns the value,
 * zero-extended. A store to the ARM7's HALTCNT that asks for a halt ends in
 * rl_nds_halt.
 */
uint32_t rl_nds_load(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, unsigned how);
/** @brief The write twin of rl_nds_load. */
void rl_nds_store(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value, unsigned how);
/** @brief ticks internal ticks of cpu (an ARM7 cycle is two). */
void rl_nds_idle(rl_nds *m, unsigned cpu, unsigned ticks);

/**
 * @brief True when a data access of cpu at addr is ordered against the
 * other CPU (it reaches state the other CPU shares): I/O, shared work RAM,
 * main RAM, the GBA slot and the ARM7's video RAM window. The ARM9's TCMs, video RAM, palette and OAM, the ARM7's
 * own work RAM and both BIOS regions are private.
 */
bool rl_nds_ordered(const rl_nds *m, unsigned cpu, uint32_t addr);

/**
 * @brief A read without timing or ordering for a high-level BIOS, a
 * debugger or a test: the same map, I/O through the scheduler at the CPU's
 * current tick (before rl_nds_attach, at tick 0), nothing charged.
 */
uint32_t rl_nds_read(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size);
/** @brief The write twin of rl_nds_read (a HALTCNT write does not halt). */
void rl_nds_write(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value);

/** @brief The backing store of a region and its length. */
uint8_t *rl_nds_mem(rl_nds *m, rl_nds_region r, size_t *len);

/**
 * @brief True once after a HALTCNT store of cpu whose halt nothing could
 * wake (the other CPU is gone and no event is queued); the store returned
 * without a halt.
 */
bool rl_nds_halt_lost(rl_nds *m, unsigned cpu);

/**
 * @brief Watches the code memory (main RAM, shared and ARM7 work RAM, ITCM,
 * video RAM) that cpu fetches the len bytes at addr from now: from then on
 * every write there, by either CPU, timed or not, moves the code epoch.
 * Lines of 32 bytes; nothing for addresses outside code memory.
 */
void rl_nds_code_watch(rl_nds *m, unsigned cpu, uint32_t addr, uint32_t len);
/** @brief The number of writes to watched code memory so far. */
uint64_t rl_nds_code_epoch(const rl_nds *m);
/** @brief True when the len bytes cpu fetches as code at addr now are want (an address no memory answers never matches). */
bool rl_nds_code_equal(rl_nds *m, unsigned cpu, uint32_t addr, const uint8_t *want, uint32_t len);
/** @brief Copies the len bytes cpu fetches as code at addr into out, zero where no memory answers. */
void rl_nds_code_read(rl_nds *m, unsigned cpu, uint32_t addr, uint8_t *out, uint32_t len);

/** @brief The ARM9's CP15 registers (read-only; write them with rl_nds_cp15_write). */
const rl_arm32_cp15 *rl_nds_cp15(const rl_nds *m);
/**
 * @brief An MCR to the ARM9's CP15 register at key (RL_CP15_KEY): writes it,
 * moves the TCMs when they changed and returns what changed
 * (RL_CP15_CHANGED_*, RL_CP15_WAIT for a wait for interrupt, which the
 * caller passes on to rl_nds_halt).
 */
unsigned rl_nds_cp15_write(rl_nds *m, uint32_t key, uint32_t value);

/** @brief Interrupt sources, as bits of IE and IF (GBATEK "DS Interrupts"). */
enum {
    RL_NDS_IRQ_VBLANK = 1u << 0,
    RL_NDS_IRQ_HBLANK = 1u << 1,
    RL_NDS_IRQ_VCOUNT = 1u << 2,
    RL_NDS_IRQ_TIMER0 = 1u << 3,
    RL_NDS_IRQ_RTC = 1u << 7,
    RL_NDS_IRQ_DMA0 = 1u << 8,
    RL_NDS_IRQ_KEYPAD = 1u << 12,
    RL_NDS_IRQ_SLOT = 1u << 13,
    RL_NDS_IRQ_IPC_SYNC = 1u << 16,
    RL_NDS_IRQ_IPC_SEND_EMPTY = 1u << 17,
    RL_NDS_IRQ_IPC_RECV = 1u << 18,
    RL_NDS_IRQ_CARD_DONE = 1u << 19,
    RL_NDS_IRQ_CARD_IREQ = 1u << 20,
    RL_NDS_IRQ_GXFIFO = 1u << 21,
    RL_NDS_IRQ_LID = 1u << 22,
    RL_NDS_IRQ_SPI = 1u << 23,
    RL_NDS_IRQ_WIFI = 1u << 24,
};

/** @brief Pixels of a screen's line and lines of a screen. */
#define RL_NDS_SCREEN_W 256u
#define RL_NDS_SCREEN_H 192u

/**
 * @brief Called once per frame, at the render tick of the first VBlank line
 * (the HBlank edge of line 192), with the finished frame: both
 * screens, the top one first, RL_NDS_SCREEN_W x RL_NDS_SCREEN_H each, one
 * 18-bit color per pixel (red bits 0 to 5, green 6 to 11, blue 12 to 17).
 */
typedef void rl_nds_frame_fn(void *user, const uint32_t *pixels, uint64_t when);
/**
 * @brief Sets the frame hook (NULL for none). While a hook is set the
 * display keeps an event queued, so halted CPUs never count as halted for
 * good; without one the display draws only when something reads it.
 */
void rl_nds_set_frame_hook(rl_nds *m, rl_nds_frame_fn *fn, void *user);
/**
 * @brief Called when the ARM7 sets bit 6 of power management register 0
 * ("DS System Power", 1 = shut down): the program turned the console off.
 */
typedef void rl_nds_power_off_fn(void *user, uint64_t when);
/** @brief Sets the power-off hook (NULL for none). */
void rl_nds_set_power_off_hook(rl_nds *m, rl_nds_power_off_fn *fn, void *user);
/** @brief True once the program has turned the console off. */
bool rl_nds_powered_off(const rl_nds *m);
/** @brief Frames finished so far (the display caught up to the ARM9's tick). */
uint64_t rl_nds_frames(rl_nds *m);
/** @brief The last finished frame (as the frame hook gets it), or NULL before the first. */
const uint32_t *rl_nds_frame(rl_nds *m);
/** @brief FNV-1a 64 of a frame's 6-bit channels, red, green, blue per pixel. */
uint64_t rl_nds_frame_hash(const uint32_t *pixels);
/** @brief Converts a frame to 8-bit RGB (3 bytes per pixel), each channel c expanded as c << 2 | c >> 4. */
void rl_nds_frame_rgb(const uint32_t *pixels, uint8_t *rgb);

/** @brief The display line (VCOUNT) at the ARM9's current tick. */
unsigned rl_nds_vcount(const rl_nds *m);

/** @brief Requests the interrupts in sources of cpu at its current tick (for devices outside this layer). */
void rl_nds_request_irq(rl_nds *m, unsigned cpu, uint32_t sources);
/** @brief True while cpu's IRQ input is asserted at its current tick (IME, IE and IF meet, after the input's delay). */
bool rl_nds_irq_line(const rl_nds *m, unsigned cpu);
/** @brief The tick from which cpu's IRQ input is asserted, or UINT64_MAX while it is not. */
uint64_t rl_nds_irq_at(const rl_nds *m, unsigned cpu);

/**
 * @brief HALT of cpu (the ARM7's HALTCNT, the ARM9's wait for interrupt):
 * ordered like an I/O access; unless IE & IF is set already, cpu leaves the
 * order and the other CPU and the events run until a request wakes it. Its
 * clock then reads the later of its own tick and the request's tick plus
 * the IRQ delay. Returns true when woken, false when nothing could wake it
 * (the other CPU is gone and no event is queued).
 */
bool rl_nds_halt(rl_nds *m, unsigned cpu);
/**
 * @brief The interrupt check of a block boundary, for a CPU whose I bit is
 * clear: when IME and IE could let an interrupt in, waits for the other CPU
 * (rl_sched_order) and runs the events due, then returns true when the IRQ
 * input is asserted at cpu's tick. An interrupt therefore arrives at the
 * first block boundary at or after its tick.
 */
bool rl_nds_irq_poll(rl_nds *m, unsigned cpu);

/** @brief The buttons of rl_nds_input.keys, in the order of KEYINPUT. */
enum {
    RL_NDS_KEY_A = 1u << 0,
    RL_NDS_KEY_B = 1u << 1,
    RL_NDS_KEY_SELECT = 1u << 2,
    RL_NDS_KEY_START = 1u << 3,
    RL_NDS_KEY_RIGHT = 1u << 4,
    RL_NDS_KEY_LEFT = 1u << 5,
    RL_NDS_KEY_UP = 1u << 6,
    RL_NDS_KEY_DOWN = 1u << 7,
    RL_NDS_KEY_R = 1u << 8,
    RL_NDS_KEY_L = 1u << 9,
};

/** @brief What the player holds: the console's keys, the touch screen and the lid. */
typedef struct rl_nds_input {
    /** @brief RL_NDS_KEY_* pressed, as KEYINPUT. */
    uint16_t keys;
    /** @brief The X and Y buttons (EXTKEYIN, ARM7). */
    bool x, y;
    /** @brief The lid is closed (EXTKEYIN bit 7, the hinge). */
    bool lid_closed;
    /** @brief The pen is on the touch screen at pixel (touch_x, touch_y) of the lower screen (0..255, 0..191). */
    bool touch;
    uint8_t touch_x, touch_y;
} rl_nds_input;

/**
 * @brief Sets what the player holds, at cpu's current tick: KEYINPUT of both
 * CPUs, EXTKEYIN, and the touch screen controller's pen. A keypad interrupt
 * condition (KEYCNT) that becomes true requests IRQ 12 on that CPU.
 */
void rl_nds_set_input(rl_nds *m, const rl_nds_input *in);

/** @brief A date and time for the clock. */
typedef struct rl_nds_time {
    /** @brief 2000 to 2099. */
    uint16_t year;
    uint8_t month, day, hour, minute, second;
} rl_nds_time;

/**
 * @brief Sets the real-time clock to t as of tick 0 of the run; it then runs
 * with the emulated time. Without a call the clock starts at 2000-01-01
 * 00:00:00. The program passes the host's clock or a pinned time.
 */
void rl_nds_set_time(rl_nds *m, const rl_nds_time *t);

/**
 * @brief Gives the machine the cartridge image (kept by pointer: it must
 * outlive the machine), which the slot serves with the chip ID derived from
 * its size and header. Returns -1 for an image too small to hold a header.
 */
int rl_nds_set_cartridge(rl_nds *m, const uint8_t *rom, size_t len);
/** @brief The chip ID the cartridge reports (B8 and the first-stage command). */
uint32_t rl_nds_chip_id(const rl_nds *m);

/**
 * @brief The memory the boot code leaves for a cartridge (GBATEK "BIOS RAM
 * Usage"): the cartridge's chip ID and header CRC, the firmware's user
 * settings at 0x027FFC80, the boot indicator, and EXMEMCNT as the BIOS sets
 * it (the ARM7 owns the cartridge and the GBA slot). Call after the
 * cartridge is set and the header is in main RAM.
 */
void rl_nds_boot_state(rl_nds *m);

/**
 * @brief Chooses the backup chip by name (auto, eeprom512, eeprom8k, eeprom64k,
 * eeprom128k, fram8k, fram32k, flash256k, flash512k, flash1m) before a save
 * is loaded; "auto" keeps the rule of backup.c. Returns -1 for an unknown name.
 */
int rl_nds_save_set(rl_nds *m, const char *type);
/** @brief The save's length (0 until a chip is chosen), image and change flag. */
size_t rl_nds_save_len(const rl_nds *m);
const uint8_t *rl_nds_save_data(const rl_nds *m);
bool rl_nds_save_dirty(const rl_nds *m);
void rl_nds_save_clean(rl_nds *m);
/** @brief Loads a raw save image; its length names the chip unless one is chosen. Returns -1 if it does not fit. */
int rl_nds_save_load(rl_nds *m, const uint8_t *data, size_t len);
/** @brief Reads a save file: 0 loaded, 1 no file, -1 does not fit. */
int rl_nds_save_read(rl_nds *m, const char *path);
/** @brief Writes the save atomically: 0 written, 1 no chip yet, -1 failed. */
int rl_nds_save_write(rl_nds *m, const char *path);
/** @brief Writes a changed save once the last change is quiet ticks old: 1 wrote, 0 nothing to do, -1 failed. */
int rl_nds_save_poll(rl_nds *m, const char *path, uint64_t quiet);
/** @brief The default save path of a ROM path (the save.dir setting, the user folder or the ROM's folder). */
bool rl_nds_save_path(char *out, size_t size, const char *rom_path);
/** @brief Declares the save settings. */
void rl_nds_declare_settings(void);

/** @brief The 0x70 bytes of the newest user settings in the firmware image. */
void rl_nds_user_settings(const rl_nds *m, uint8_t out[0x70]);

/** @brief Kinds of access in the cost table. */
enum {
    RL_NDS_CODE = 0,
    RL_NDS_DATA = 1,
};

/**
 * @brief Ticks of one bus access: t[cpu][kind][region][log2 size][seq], region
 * addr >> 24 for addresses below 0x0F000000 and 15 above (the ARM9 BIOS).
 * TCM accesses cost one tick and are not in the table.
 */
typedef struct rl_nds_costs {
    /** @brief The ticks. */
    uint8_t t[2][2][16][3][2];
} rl_nds_costs;
/** @brief The current cost table (the GBA slot rows follow each CPU's EXMEMCNT). */
const rl_nds_costs *rl_nds_cost_table(const rl_nds *m);

#endif
