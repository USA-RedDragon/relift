/**
 * @file host1x.c
 * @brief The host1x command processor: decodes the opcodes of a command
 * buffer and writes the THI registers of the class it selects. A write of
 * THI Method1 writes the engine method that Method0 named; a write of the
 * engine's Execute method runs the engine.
 *
 * Opcodes and the THI register offsets follow Ryujinx (MIT, Copyright (c)
 * Ryujinx Team and Contributors) Host1xDevice and ThiDevice. Syncpoint
 * increments in the stream are not counted here: every increment a submit
 * declares completes when the submit returns, after its commands ran.
 */
#include "host1x.h"

#include <string.h>

enum { OP_SET_CLASS = 0, OP_INCR = 1, OP_NONINCR = 2, OP_MASK = 3, OP_IMM = 4 };
enum { THI_METHOD0 = 0x10, THI_METHOD1 = 0x11 };
enum { METHOD_EXECUTE = 0xC0 };

typedef struct h1x_engine {
    uint32_t class_id;
    uint32_t method;
    uint32_t regs[H1X_ENGINE_REGS];
    void (*execute)(const uint32_t *regs);
} h1x_engine;

static h1x_engine engines[] = {
    { H1X_CLASS_NVDEC, 0, { 0 }, nvdec_execute },
    { H1X_CLASS_VIC, 0, { 0 }, vic_execute },
};

void h1x_reset(void)
{
    for (size_t i = 0; i < sizeof engines / sizeof engines[0]; i++) {
        engines[i].method = 0;
        memset(engines[i].regs, 0, sizeof engines[i].regs);
    }
    nvdec_reset();
}

static h1x_engine *engine_of(uint32_t class_id)
{
    for (size_t i = 0; i < sizeof engines / sizeof engines[0]; i++)
        if (engines[i].class_id == class_id)
            return &engines[i];
    return NULL;
}

static void thi_write(uint32_t class_id, uint32_t offset, uint32_t value)
{
    h1x_engine *e = engine_of(class_id);
    if (!e)
        return;
    if (offset == THI_METHOD0) {
        e->method = value;
    } else if (offset == THI_METHOD1 && e->method < H1X_ENGINE_REGS) {
        e->regs[e->method] = value;
        if (e->method == METHOD_EXECUTE)
            e->execute(e->regs);
    }
}

void h1x_run(h1x_stream *s, const uint32_t *words, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) {
        uint32_t v = words[i];
        if (s->mask) {
            uint32_t bit = (uint32_t)__builtin_ctz(s->mask);
            s->mask &= s->mask - 1;
            thi_write(s->class_id, s->offset + bit, v);
            continue;
        }
        if (s->count) {
            s->count--;
            thi_write(s->class_id, s->offset, v);
            if (s->incrementing)
                s->offset++;
            continue;
        }
        switch (v >> 28) {
        case OP_SET_CLASS:
            s->mask = v & 0x3F;
            s->class_id = (v >> 6) & 0x3FF;
            s->offset = (v >> 16) & 0xFFF;
            break;
        case OP_INCR:
        case OP_NONINCR:
            s->count = v & 0xFFFF;
            s->offset = (v >> 16) & 0xFFF;
            s->incrementing = (v >> 28) == OP_INCR;
            break;
        case OP_MASK:
            s->mask = v & 0xFFFF;
            s->offset = (v >> 16) & 0xFFF;
            break;
        case OP_IMM:
            s->offset = (v >> 16) & 0xFFF;
            thi_write(s->class_id, s->offset, v & 0xFFF);
            break;
        default:
            rl_log(RL_LOG_WARN, "host1x", "unsupported opcode %u (word 0x%08x)", v >> 28, v);
            break;
        }
    }
}

uint8_t *h1x_ptr(uint64_t addr, uint64_t size)
{
    nv_map *m = nv_map_by_dma(addr);
    if (!m || !m->addr || addr + size < addr || addr + size > m->dma_addr + m->size)
        return NULL;
    return hos_mem() + m->addr + (addr - m->dma_addr);
}

static uint32_t gobs_wide(uint32_t width) { return (width + 63) / 64; }

uint64_t h1x_bl_size(uint32_t width, uint32_t height, uint32_t block_h_log2)
{
    uint32_t block_rows = 8u << block_h_log2;
    return (uint64_t)gobs_wide(width) * ((height + block_rows - 1) / block_rows) * (512ull << block_h_log2);
}

static uint64_t bl_offset(const h1x_bl *s, uint32_t x, uint32_t y)
{
    uint32_t bh = s->block_h_log2;
    uint64_t block = (uint64_t)(y >> (3 + bh)) * gobs_wide(s->width) + (x >> 6);
    uint64_t gob = (block << bh) + ((y >> 3) & ((1u << bh) - 1));
    uint32_t in = ((x & 63) >> 5) * 256 + ((y & 7) >> 1) * 64 + ((x & 31) >> 4) * 32 + (y & 1) * 16 + (x & 15);
    return gob * 512 + in;
}

void h1x_bl_read_row(const h1x_bl *s, uint32_t x, uint32_t y, uint8_t *dst, uint32_t n)
{
    while (n) {
        uint32_t run = 16 - (x & 15);
        if (run > n)
            run = n;
        memcpy(dst, s->base + bl_offset(s, x, y), run);
        dst += run;
        x += run;
        n -= run;
    }
}

void h1x_bl_write_row(const h1x_bl *s, uint32_t x, uint32_t y, const uint8_t *src, uint32_t n)
{
    while (n) {
        uint32_t run = 16 - (x & 15);
        if (run > n)
            run = n;
        memcpy(s->base + bl_offset(s, x, y), src, run);
        src += run;
        x += run;
        n -= run;
    }
}
