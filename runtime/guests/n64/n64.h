/**
 * @file n64.h
 * @brief The Nintendo 64 machine over the device scheduler
 * (core/include/relift/sched.h): the physical memory map, MI (interrupt
 * mask and pending bits, version), the RI and RDRAM registers, PI (the
 * cartridge domains, DMA in both directions with its duration, status and
 * interrupt), the SP interface (DMA to DMEM and IMEM with its duration, the
 * status, semaphore and signals, the RSP's start, halt and BREAK, run in
 * slices by the run layer), the DP command registers, VI (the registers,
 * the line counter, the
 * interrupt at its line and the raw framebuffer scan-out), SI (DMA with its
 * timing), the PIF's joybus with standard controllers, the cartridge's SRAM,
 * Flash and EEPROM, the AI registers, the high-level boot and the binding
 * of lifted code to the RDRAM ranges the machine loads it to.
 *
 * One scheduler tick is one CPU cycle (93.75 MHz). The machine knows no
 * CPU: the run layer (run/) passes the CPU's accesses to rl_n64_read and
 * rl_n64_write and asks rl_n64_irq_pending which MI lines are up, and lifted
 * code reaches RDRAM directly through rl_n64_rdram.
 *
 * Timing of PI DMA, SP DMA and the VI lines follows ares (ISC license, see
 * NOTICE).
 */
#ifndef RELIFT_N64_H
#define RELIFT_N64_H

#include <relift/abi_core.h>
#include <relift/ports.h>
#include <relift/sched.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief CPU cycles per second (the VR4300's PClock). */
#define RL_N64_HZ 93750000u
/** @brief Bytes of RDRAM (with the expansion pak). */
#define RL_N64_RDRAM_SIZE 0x800000u
/** @brief The scheduler line of MI's interrupt output, which is the CPU's IP2. */
#define RL_N64_LINE_MI 2u

/** @brief The MI interrupt sources, as bits of MI_INTR and MI_MASK. */
enum {
    RL_N64_IRQ_SP = 0x01,
    RL_N64_IRQ_SI = 0x02,
    RL_N64_IRQ_AI = 0x04,
    RL_N64_IRQ_VI = 0x08,
    RL_N64_IRQ_PI = 0x10,
    RL_N64_IRQ_DP = 0x20,
};

/** @brief The machine; opaque. */
typedef struct rl_n64 rl_n64;

/**
 * @brief A run of lifted code words as the program lists it: the unit that
 * holds its functions, where its bytes come from and where they are linked,
 * and FNV-1a (64-bit) of the bytes.
 */
typedef struct rl_n64_code {
    /** @brief The unit, an index into the units given to rl_n64_code_init. */
    uint32_t unit;
    /** @brief 0 for the ROM, 1 + n for decoded stream n. */
    uint32_t source;
    /** @brief The offset in the source. */
    uint32_t offset;
    /** @brief The length in bytes. */
    uint32_t len;
    /** @brief The link address (sign-extended kseg0). */
    uint64_t link;
    /** @brief FNV-1a (64-bit) of the bytes. */
    uint64_t hash;
} rl_n64_code;

/** @brief Counters for --stats and tests. */
typedef struct rl_n64_stats {
    /** @brief PI DMAs from the cartridge. */
    uint64_t pi_dmas;
    /** @brief SP DMAs. */
    uint64_t sp_dmas;
    /** @brief Code runs bound by a load (each time). */
    uint64_t binds;
    /** @brief Hash checks that matched. */
    uint64_t checks;
    /** @brief Hash checks that did not match, or entries no bound run covers. */
    uint64_t mismatches;
    /** @brief Bound runs a write or a cache operation unbound. */
    uint64_t invalidations;
    /** @brief VI frames. */
    uint64_t frames;
    /** @brief RSP starts (SP_STATUS halt cleared while halted). */
    uint64_t rsp_starts;
    /** @brief BREAKs the RSP ran. */
    uint64_t rsp_breaks;
    /** @brief Command lists handed to the RDP side (DPC_END writes and unfreezes with commands). */
    uint64_t dp_submits;
} rl_n64_stats;

/** @brief Called at the start of each VI frame (the line the picture begins at, or every 200 lines of a VI that is off). */
typedef void rl_n64_frame_hook(void *user, rl_n64 *m);

/** @brief A machine with the big-endian ROM (kept, not copied) and zeroed RDRAM, or NULL. */
RL_RUNTIME_API rl_n64 *rl_n64_create(const uint8_t *rom, size_t rom_len);
/** @brief Frees the machine. */
RL_RUNTIME_API void rl_n64_destroy(rl_n64 *m);
/** @brief Attaches the machine to scheduler s, whose CPU k it serves, and starts VI. Returns 0 or -1. */
RL_RUNTIME_API int rl_n64_attach(rl_n64 *m, rl_sched *s, rl_sched_cpu *k);
/** @brief RDRAM, RL_N64_RDRAM_SIZE bytes in the guest's byte order: the base lifted code addresses physical memory from. */
RL_RUNTIME_API uint8_t *rl_n64_rdram(rl_n64 *m);
/** @brief SP DMEM (0x1000 bytes) followed by IMEM (0x1000 bytes). */
RL_RUNTIME_API uint8_t *rl_n64_spmem(rl_n64 *m);
/** @brief The counters. */
RL_RUNTIME_API const rl_n64_stats *rl_n64_get_stats(const rl_n64 *m);

/** @brief A read of size bytes (1, 2, 4 or 8) at physical address paddr by the CPU now: the big-endian value. */
RL_RUNTIME_API uint64_t rl_n64_read(rl_n64 *m, uint32_t paddr, uint32_t size);
/**
 * @brief The cycles a CPU read at physical address paddr that starts now
 * keeps the CPU waiting (bus.c): none in RDRAM, the RCP's read latency
 * elsewhere, plus the PI bus cycle (or the rest of a bus write in flight)
 * in the cartridge's address space. Ask before rl_n64_read, which ends a
 * bus write in flight.
 */
RL_RUNTIME_API uint32_t rl_n64_read_stall(rl_n64 *m, uint32_t paddr);
/** @brief A write of size bytes of the big-endian value at physical address paddr by the CPU now. */
RL_RUNTIME_API void rl_n64_write(rl_n64 *m, uint32_t paddr, uint32_t size, uint64_t value);
/** @brief MI_INTR & MI_MASK: the sources whose interrupt is up. */
RL_RUNTIME_API uint32_t rl_n64_irq_pending(const rl_n64 *m);
/** @brief Sets the frame hook (NULL: none). */
RL_RUNTIME_API void rl_n64_set_frame_hook(rl_n64 *m, rl_n64_frame_hook *hook, void *user);
/** @brief A VI register (0 to 13) as a read would see it. */
RL_RUNTIME_API uint32_t rl_n64_vi_reg(rl_n64 *m, unsigned index);

/** @brief The bits of a controller's button word, as the joybus reports them. */
enum {
    RL_N64_PAD_A = 0x8000,
    RL_N64_PAD_B = 0x4000,
    RL_N64_PAD_Z = 0x2000,
    RL_N64_PAD_START = 0x1000,
    RL_N64_PAD_UP = 0x0800,
    RL_N64_PAD_DOWN = 0x0400,
    RL_N64_PAD_LEFT = 0x0200,
    RL_N64_PAD_RIGHT = 0x0100,
    RL_N64_PAD_RESET = 0x0080,
    RL_N64_PAD_L = 0x0020,
    RL_N64_PAD_R = 0x0010,
    RL_N64_PAD_C_UP = 0x0008,
    RL_N64_PAD_C_DOWN = 0x0004,
    RL_N64_PAD_C_LEFT = 0x0002,
    RL_N64_PAD_C_RIGHT = 0x0001,
};

/**
 * @brief Plugs a standard controller into port (0 to 3) with the buttons
 * (RL_N64_PAD_*) and the stick (x right and y up, -85 to 85) the joybus
 * reports from now on. Opposite directions cancel and L, R and Start
 * together read as the reset button with the stick centered, as on the
 * console. A port with no controller answers the joybus as an empty port.
 */
RL_RUNTIME_API void rl_n64_set_pad(rl_n64 *m, unsigned port, uint16_t buttons, int8_t x, int8_t y);
/** @brief Unplugs the controller of port. */
RL_RUNTIME_API void rl_n64_pad_remove(rl_n64 *m, unsigned port);
/** @brief The native ports of the Nintendo 64 (generated from relift-input): declare them with rl_ports_declare. */
extern const rl_port_decl rl_n64_ports[];
/** @brief Number of entries of rl_n64_ports. */
extern const uint32_t rl_n64_ports_count;
/**
 * @brief Makes controller 1 follow the port table's p1 port (declared with
 * rl_n64_ports, a, b, z, start, up, down, left, right, l, r, c_up, c_down,
 * c_left, c_right and the axes stick_x and stick_y), read each time the
 * joybus commands run. Off: the controller is unplugged.
 */
RL_RUNTIME_API void rl_n64_pad_use_ports(rl_n64 *m, bool on);

/**
 * @brief The cartridge's save memory, on disk as raw images under one base
 * path: EEPROM (.eep, 512 or 2048 bytes), SRAM (.sra, 32 KiB) and Flash
 * (.fla, 128 KiB). No game table says which the game has: SRAM answers at
 * 0x08000000 until the game writes the Flash command register at
 * 0x08010000, which makes the Flash chip answer from then on; EEPROM
 * answers on joybus channel 4 and reports 16 Kbit unless a 512-byte file
 * was loaded.
 *
 * Reads the files that exist under base. Returns how many it loaded, or -1
 * when one did not fit (the others are still loaded). A Flash file is a
 * game that used Flash, so the chip answers from the start.
 */
RL_RUNTIME_API int rl_n64_save_load(rl_n64 *m, const char *base);
/** @brief Writes the stores the game changed since the last write, each atomically; returns how many it wrote or -1. */
RL_RUNTIME_API int rl_n64_save_write(rl_n64 *m, const char *base);
/** @brief As rl_n64_save_write when a store changed and nothing changed for quiet CPU cycles; returns 1 when it wrote, 0 when not, -1 on an error. */
RL_RUNTIME_API int rl_n64_save_poll(rl_n64 *m, const char *base, uint64_t quiet);
/** @brief Whether a store changed since the last write. */
RL_RUNTIME_API bool rl_n64_save_dirty(const rl_n64 *m);
/**
 * @brief The base path of the save files of the ROM at rom_path: its name
 * without the extension in the save folder setting (save.dir), else in the
 * program's user folder, else next to the ROM. False when out is too small.
 */
RL_RUNTIME_API bool rl_n64_save_path(char *out, size_t size, const char *rom_path);
/** @brief Declares the settings of the machine layer (save.dir). */
RL_RUNTIME_API void rl_n64_declare_settings(void);

/**
 * @brief The picture VI outputs now, as ares' software path draws it: the
 * lines and columns the video registers give inside the 640 by 480 (NTSC) or
 * 640 by 576 (PAL) output, read from RDRAM through the scale registers
 * (RGBA5551 or RGBA8888, the guard band at the edges, no filters), the lines
 * of the other field kept from the last picture when VI interlaces. Writes
 * width * height RGB pixels to rgb (room for 640 * 576 * 3 bytes) and the
 * size. Returns false when VI has never been on.
 */
RL_RUNTIME_API bool rl_n64_scanout(rl_n64 *m, uint8_t *rgb, uint32_t *width, uint32_t *height);
/**
 * @brief The framebuffer VI points at, read raw from RDRAM with its origin,
 * width and the visible size from the video and scale registers, no scaling
 * and no window: writes width * height RGB pixels to rgb (room for 640 * 576
 * * 3 bytes) and the size. Returns false when VI is off.
 */
RL_RUNTIME_API bool rl_n64_framebuffer(rl_n64 *m, uint8_t *rgb, uint32_t *width, uint32_t *height);

/**
 * @brief How the machine runs the RSP; the run layer's RSP layer fills it
 * (the machine knows no CPU). run runs lifted code from *pc at RSP cycle
 * from until cycle to or until the RSP halts, returns the cycle it stopped
 * at (a block may end past to) and leaves the next pc in *pc; imem_written
 * says IMEM [offset, offset + len) changed.
 */
typedef struct rl_n64_rsp_ops {
    uint64_t (*run)(void *user, uint32_t *pc, uint64_t from, uint64_t to);
    void (*imem_written)(void *user, uint32_t offset, uint32_t len);
} rl_n64_rsp_ops;

/** @brief Sets how the RSP runs (NULL: a start halts at once, with a report). */
RL_RUNTIME_API void rl_n64_set_rsp(rl_n64 *m, const rl_n64_rsp_ops *ops, void *user);
/** @brief The RSP's MFC0 of rd (0 to 7 SP, 8 to 15 DP) at RSP cycle cycle. */
RL_RUNTIME_API uint32_t rl_n64_rsp_mfc0(rl_n64 *m, uint32_t rd, uint64_t cycle);
/** @brief The RSP's MTC0 of value to rd at RSP cycle cycle. */
RL_RUNTIME_API void rl_n64_rsp_mtc0(rl_n64 *m, uint32_t rd, uint32_t value, uint64_t cycle);
/** @brief The RSP's BREAK at RSP cycle cycle: halted and broken, the SP interrupt when SP_STATUS asks for it. */
RL_RUNTIME_API void rl_n64_rsp_break(rl_n64 *m, uint64_t cycle);
/** @brief Whether the RSP is halted. */
RL_RUNTIME_API bool rl_n64_rsp_halted(const rl_n64 *m);
/** @brief SP_PC: where the RSP starts or stopped. */
RL_RUNTIME_API uint32_t rl_n64_sp_pc(const rl_n64 *m);

/**
 * @brief The RDP side's command hook: the command list [start, end) of
 * RDRAM (or of DMEM when xbus) is ready, as a DPC_END write or an unfreeze
 * makes it. The hook owns its completion: it reports progress with
 * rl_n64_dp_progress and raises the DP interrupt itself. Without a hook the
 * commands are taken at once (DPC_CURRENT reaches DPC_END) and do nothing.
 */
typedef void rl_n64_dp_submit(void *user, rl_n64 *m, uint32_t start, uint32_t end, bool xbus);
/** @brief Sets the DP command hook (NULL: none). */
RL_RUNTIME_API void rl_n64_set_dp_submit(rl_n64 *m, rl_n64_dp_submit *hook, void *user);
/** @brief The RDP side took the commands up to current; busy says whether it still works on them. */
RL_RUNTIME_API void rl_n64_dp_progress(rl_n64 *m, uint32_t current, bool busy);

/** @brief The memory side of the boot the boot variant cic (6101 to 8303; 0 boots as 6102) does: see rl_n64_boot. */
typedef struct rl_n64_boot_info {
    /** @brief The boot variant. */
    uint32_t cic;
    /** @brief The entry point the boot jumps to (adjusted for 6103 and 6106). */
    uint32_t entry;
} rl_n64_boot_info;

/**
 * @brief The high-level boot: no boot code runs. Sets what the console's
 * boot leaves in memory and in the RCP: the ROM header and boot area in
 * DMEM, the first MiB of the game from ROM offset 0x1000 at the entry
 * point (as a PI DMA, which binds the code there), the PI domain 1 timing
 * from the header, the RI registers, and the RDRAM size word at 0x80000318
 * (0x800003F0 for 6105) with the other boot words at 0x80000300. The CPU
 * registers are the run layer's.
 */
RL_RUNTIME_API void rl_n64_boot(rl_n64 *m, const rl_n64_boot_info *b);

/**
 * @brief Gives the machine the program's units and code runs. Reserves the
 * dispatch table over every unit and run and registers the units' function
 * maps; no run is bound yet. Call after rl_core_init and before any guest
 * thread starts. Returns 0 or -1.
 */
RL_RUNTIME_API int rl_n64_code_init(rl_n64 *m, const rl_unit *const *units, uint32_t unit_count,
                                    const rl_n64_code *code, uint32_t count);
/**
 * @brief The function to enter at the dispatch slot of pc, whose slot held
 * entry_stub: binds the run there whose hash matches RDRAM now (its
 * functions replace the stub in every slot of the run). Returns NULL when
 * no bound run at pc matches; *why then says why.
 */
RL_RUNTIME_API rl_fn rl_n64_code_check(rl_n64 *m, uint64_t pc, char *why, size_t n);
/** @brief The function dispatch holds for a run that is loaded but not checked yet (the run layer's entry check). */
RL_RUNTIME_API void rl_n64_code_set_stub(rl_n64 *m, rl_fn entry_stub);
/** @brief The source bytes [offset, offset + len) of source were copied to RDRAM at dram: runs linked there are loaded and checked at their next entry. */
RL_RUNTIME_API void rl_n64_code_loaded(rl_n64 *m, uint32_t source, uint32_t offset, uint32_t dram, uint32_t len);
/** @brief RDRAM [paddr, paddr + len) changed or its instruction cache lines were invalidated: runs there are checked again at their next entry. */
RL_RUNTIME_API void rl_n64_code_written(rl_n64 *m, uint32_t paddr, uint32_t len);

#endif
