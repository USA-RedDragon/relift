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
    /** @brief The plain video RAM store: the five 2 MiB windows of 0x06000000 packed (512, 128, 256, 128 KiB and 1 MiB). */
    RL_NDS_VRAM_SIZE = 0x200000,
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
    /** @brief Video RAM as plain storage (RL_NDS_VRAM_SIZE) until VRAM banking exists. */
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
 * @brief A data read of size bytes (1, 2 or 4) at addr, aligned down; seq
 * true for the second and later words of LDM. Waits for the other CPU when
 * the access is ordered (rl_nds_ordered), stalls on a busy main RAM,
 * charges the cost and returns the value, zero-extended. A store to the
 * ARM7's HALTCNT that asks for a halt ends in rl_nds_halt.
 */
uint32_t rl_nds_load(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, bool seq);
/** @brief The write twin of rl_nds_load. */
void rl_nds_store(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value, bool seq);
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
/** @brief The write twin of rl_nds_read. */
void rl_nds_write(rl_nds *m, unsigned cpu, uint32_t addr, unsigned size, uint32_t value);

/** @brief The backing store of a region and its length. */
uint8_t *rl_nds_mem(rl_nds *m, rl_nds_region r, size_t *len);

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
