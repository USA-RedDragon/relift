/**
 * @file n64.h
 * @brief The Nintendo 64 machine over the device scheduler
 * (core/include/relift/sched.h): the physical memory map, MI (interrupt
 * mask and pending bits, version), the RI and RDRAM registers, PI (the
 * cartridge domains, DMA in both directions with its duration, status and
 * interrupt), the SP registers with DMA to DMEM and IMEM (the RSP does not
 * run yet), the DP registers, VI (the registers, the line counter, the
 * interrupt at its line and the raw framebuffer scan-out), AI and SI
 * registers, PIF RAM, the high-level boot and the binding of lifted code to
 * the RDRAM ranges the machine loads it to.
 *
 * One scheduler tick is one CPU cycle (93.75 MHz). The machine knows no
 * CPU: the run layer (run/) passes the CPU's accesses to rl_n64_read and
 * rl_n64_write and asks rl_n64_irq_pending which MI lines are up, and lifted
 * code reaches RDRAM directly through rl_n64_rdram.
 *
 * Timing of PI DMA and of the VI lines follows ares (ISC license, see
 * NOTICE).
 */
#ifndef RELIFT_N64_H
#define RELIFT_N64_H

#include <relift/abi_core.h>
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
    /** @brief RSP starts (SP_STATUS halt cleared); the RSP does not run yet. */
    uint64_t rsp_starts;
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
/** @brief A write of size bytes of the big-endian value at physical address paddr by the CPU now. */
RL_RUNTIME_API void rl_n64_write(rl_n64 *m, uint32_t paddr, uint32_t size, uint64_t value);
/** @brief MI_INTR & MI_MASK: the sources whose interrupt is up. */
RL_RUNTIME_API uint32_t rl_n64_irq_pending(const rl_n64 *m);
/** @brief Sets the frame hook (NULL: none). */
RL_RUNTIME_API void rl_n64_set_frame_hook(rl_n64 *m, rl_n64_frame_hook *hook, void *user);
/** @brief A VI register (0 to 13) as a read would see it. */
RL_RUNTIME_API uint32_t rl_n64_vi_reg(rl_n64 *m, unsigned index);

/**
 * @brief The framebuffer VI scans out, read raw from RDRAM with the VI
 * registers (origin, width, the visible size from the video and scale
 * registers; RGBA5551 or RGBA8888): writes width * height RGB pixels to rgb
 * (room for 640 * 576 * 3 bytes) and the size. Returns false when VI is off.
 */
RL_RUNTIME_API bool rl_n64_scanout(rl_n64 *m, uint8_t *rgb, uint32_t *width, uint32_t *height);

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
