/**
 * @file host1x.h
 * @brief The host1x command processor and the multimedia engines behind it
 * (NVDEC, VIC). A channel submit runs its command buffers here, on the
 * calling thread, before its syncpoints complete.
 *
 * The command stream format, the THI method registers and the engine
 * register layouts follow Ryujinx (MIT, Copyright (c) Ryujinx Team and
 * Contributors) Ryujinx.Graphics.Host1x, Ryujinx.Graphics.Nvdec and
 * Ryujinx.Graphics.Vic.
 */
#ifndef RELIFT_HOST1X_H
#define RELIFT_HOST1X_H

#include "nv.h"

/** @brief Host1x class ids (the class field of SetClass). */
enum {
    H1X_CLASS_HOST1X = 0x01,
    H1X_CLASS_VIC = 0x5d,
    H1X_CLASS_NVJPG = 0xc0,
    H1X_CLASS_NVDEC = 0xf0
};

/** @brief Words of an engine's method register file. */
#define H1X_ENGINE_REGS 0x400u

/** @brief The command stream state of one channel: what the next data word goes to. */
typedef struct h1x_stream {
    uint32_t class_id;
    uint32_t offset;
    uint32_t count;
    uint32_t mask;
    bool incrementing;
} h1x_stream;

/** @brief Runs the words of one command buffer on the stream of a channel. nv lock held. */
void h1x_run(h1x_stream *s, const uint32_t *words, uint32_t n);

/** @brief Host pointer to size bytes at host1x address addr, or NULL when they are not inside one mapped buffer. nv lock held. */
uint8_t *h1x_ptr(uint64_t addr, uint64_t size);

/**
 * @brief A surface in the GPU's block-linear layout: 64-byte by 8-row GOBs,
 * stacked 1 << block_h_log2 high into blocks, blocks in rows.
 */
typedef struct h1x_bl {
    uint8_t *base;
    /** @brief Row width in bytes and height in rows of the surface. */
    uint32_t width, height;
    uint32_t block_h_log2;
} h1x_bl;

/** @brief Bytes of a block-linear surface of width bytes by height rows. */
uint64_t h1x_bl_size(uint32_t width, uint32_t height, uint32_t block_h_log2);
/** @brief Copies row y of a block-linear surface (n bytes from byte x) into dst. */
void h1x_bl_read_row(const h1x_bl *s, uint32_t x, uint32_t y, uint8_t *dst, uint32_t n);
/** @brief Copies n bytes from src into row y of a block-linear surface, from byte x. */
void h1x_bl_write_row(const h1x_bl *s, uint32_t x, uint32_t y, const uint8_t *src, uint32_t n);

/** @brief Runs NVDEC's Execute method on its register file (method word offsets). */
void nvdec_execute(const uint32_t *regs);
/** @brief Runs VIC's Execute method on its register file (method word offsets). */
void vic_execute(const uint32_t *regs);
/** @brief Drops NVDEC's decoder state. */
void nvdec_reset(void);

/** @brief Drops the engines' state (tests, process end). nv lock held. */
void h1x_reset(void);

#endif
