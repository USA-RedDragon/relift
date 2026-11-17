/**
 * @file rsp.h
 * @brief The RSP layer: runs lifted RSP microcode out of IMEM, never
 * interpreting it.
 *
 * The program gives the layer every lifted RSP image (rl_rsp_image): a unit
 * whose functions hold the image's instructions at their offsets in the
 * image, the image's bytes and, per word, where its block goes without a
 * computed jump. An image is bound wherever IMEM holds its bytes: at a
 * word w, every image word equal to IMEM's there is a candidate offset, and
 * the base is w minus the offset (modulo 4 KiB). Each IMEM word has a
 * dispatch slot. A slot holds the function of an image only after the layer
 * checked that the bytes in IMEM equal the image's bytes at every word
 * lifted code can reach from that slot without a computed jump (each
 * computed jump goes through a slot again). Any write
 * to IMEM puts the check back in every slot, and the lifted code that runs
 * leaves at its next block, so code never runs over bytes it was not lifted
 * from. A word that no image's check accepts is missed code: a report with
 * the registers, IMEM and DMEM (written to files with a dump prefix), and
 * the program stops.
 *
 * A block's cost assumes the pipeline (relift_cpu_rsp::pipe) the code
 * before it in its image leaves. Where lifted code leaves through dispatch
 * or halts, it gives the layer its pipeline, and the layer charges the
 * difference the target's first words make (pipe.h).
 *
 * The layer knows neither the SP interface nor the clock of the machine:
 * COP0 reads and writes and BREAK go to the bus the guest layer gives, with
 * the RSP's cycle, and the guest layer runs the RSP in slices with
 * rl_rsp_run.
 */
#ifndef RELIFT_RSP_H
#define RELIFT_RSP_H

#include <relift/abi_core.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief rl_rsp_image.succ of a block whose exit leaves the image or is computed. */
#define RL_RSP_NO_SUCC 0xFFFFu
/** @brief Flag of rl_rsp_image.succ entry 2: the block holds a delay slot (two words). */
#define RL_RSP_SUCC_SLOT 1u
/** @brief rl_rsp_image.pipe_at of a word that is no block's. */
#define RL_RSP_NO_PIPE 0xFFFFu

/** @brief One lifted RSP image, as the program lists it. */
typedef struct rl_rsp_image {
    /** @brief The unit; its function entries are offsets in the image. */
    const rl_unit *unit;
    /** @brief The length in bytes, a multiple of 4 and at most 0x1000. */
    uint32_t len;
    /** @brief FNV-1a (64-bit) of the bytes, the image's name in reports. */
    uint64_t hash;
    /** @brief The bytes, big-endian, as lifted. */
    const uint8_t *bytes;
    /**
     * @brief Three entries per word: the offsets (shifted right by 2) of
     * the words the word's block goes to without a computed jump, inside
     * the image (RL_RSP_NO_SUCC for none), then RL_RSP_SUCC_SLOT when the
     * block holds the next word as its delay slot.
     */
    const uint16_t *succ;
    /**
     * @brief The pipelines the image's costs assume, three numbers each
     * (relift_cpu_rsp::pipe::State::pack), or NULL.
     */
    const uint64_t *pipes;
    /**
     * @brief Per word, the index in pipes of the pipeline the cost of the
     * word's block assumes on entry (RL_RSP_NO_PIPE for none), or NULL.
     */
    const uint16_t *pipe_at;
} rl_rsp_image;

/** @brief The guest layer's side of COP0 and BREAK; cycle is the RSP cycle of the access. */
typedef struct rl_rsp_bus {
    uint32_t (*mfc0)(void *user, uint32_t rd, uint64_t cycle);
    void (*mtc0)(void *user, uint32_t rd, uint32_t value, uint64_t cycle);
    void (*brk)(void *user, uint64_t cycle);
    /** @brief Whether the RSP is halted now (after a COP0 write or BREAK). */
    bool (*halted)(void *user);
} rl_rsp_bus;

/** @brief Counters for --stats and tests. */
typedef struct rl_rsp_stats {
    /** @brief RSP cycles run. */
    uint64_t cycles;
    /** @brief Slices run (rl_rsp_run calls that ran code). */
    uint64_t slices;
    /** @brief Checks that bound an image at a word. */
    uint64_t binds;
    /** @brief IMEM writes that unbound the slots. */
    uint64_t invalidations;
    /** @brief Words no image's check accepted (each one stops the program). */
    uint64_t misses;
    /** @brief Entries through dispatch whose pipeline did not become the one the block's cost assumes within the window. */
    uint64_t pipe_unmet;
} rl_rsp_stats;

/** @brief An RSP; opaque. */
typedef struct rl_rsp rl_rsp;

/**
 * @brief An RSP over dmem and imem (4 KiB each, the guest layer's) with the
 * images (kept, not copied) and the bus, or NULL. The registers are zero.
 */
RL_RUNTIME_API rl_rsp *rl_rsp_create(uint8_t *dmem, const uint8_t *imem, const rl_rsp_image *const *images,
                                     uint32_t count, const rl_rsp_bus *bus, void *user);
/** @brief Frees the RSP. */
RL_RUNTIME_API void rl_rsp_destroy(rl_rsp *r);
/** @brief The images do not fit this runtime (ABI version or context size): writes why and returns false. */
RL_RUNTIME_API bool rl_rsp_images_ok(const rl_rsp_image *const *images, uint32_t count, char *why, size_t n);

/**
 * @brief Runs lifted code from IMEM address *pc, at RSP cycle from, until
 * cycle to or until the RSP halts. Returns the cycle it stopped at; *pc is
 * the next instruction's address.
 */
RL_RUNTIME_API uint64_t rl_rsp_run(rl_rsp *r, uint32_t *pc, uint64_t from, uint64_t to);
/** @brief IMEM [offset, offset + len) changed: every slot is checked again, and running code leaves at its next block. */
RL_RUNTIME_API void rl_rsp_imem_written(rl_rsp *r, uint32_t offset, uint32_t len);
/** @brief Where missed code writes PREFIX.imem and PREFIX.dmem, or NULL for no files. */
RL_RUNTIME_API void rl_rsp_set_dump(rl_rsp *r, const char *prefix);
/** @brief The counters. */
RL_RUNTIME_API const rl_rsp_stats *rl_rsp_get_stats(const rl_rsp *r);

/** @brief Scalar register n (0 to 31), for reports and tests. */
RL_RUNTIME_API uint32_t rl_rsp_gpr(const rl_rsp *r, unsigned n);
/** @brief Vector register n (0 to 31) as 16 big-endian bytes (element 0 first), for reports and tests. */
RL_RUNTIME_API void rl_rsp_vpr(const rl_rsp *r, unsigned n, uint8_t out[16]);

#endif
