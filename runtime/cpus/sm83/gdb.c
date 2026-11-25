/**
 * @file gdb.c
 * docgen:region registers
 * @brief The SM83 registers for the GDB server (core/gdbstub.h), as gdb's
 * gbz80 architecture numbers them: af (a high, f low), bc, de, hl, sp, pc,
 * then ix, iy, af', bc', de', hl' and ir, which the SM83 does not have and
 * read as zero, 16 bits each, little-endian. pc is the guest address the
 * thread resumes at where it parked at a sync point; its bank is the one
 * mapped at 0x4000 now.
 * docgen:end
 */
#include "sm83.h"

#include <string.h>

#include "gdbstub.h"

#if defined(RL_DEBUG_STUB)

/** @brief The target description: gdb's z80 register set under the gbz80 architecture. */
static const char g_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\"><architecture>gbz80</architecture>"
    "<feature name=\"org.gnu.gdb.z80.cpu\">"
    "<reg name=\"af\" bitsize=\"16\" regnum=\"0\"/><reg name=\"bc\" bitsize=\"16\"/>"
    "<reg name=\"de\" bitsize=\"16\"/><reg name=\"hl\" bitsize=\"16\"/>"
    "<reg name=\"sp\" bitsize=\"16\" type=\"data_ptr\"/><reg name=\"pc\" bitsize=\"16\" type=\"code_ptr\"/>"
    "<reg name=\"ix\" bitsize=\"16\"/><reg name=\"iy\" bitsize=\"16\"/>"
    "<reg name=\"af'\" bitsize=\"16\"/><reg name=\"bc'\" bitsize=\"16\"/>"
    "<reg name=\"de'\" bitsize=\"16\"/><reg name=\"hl'\" bitsize=\"16\"/>"
    "<reg name=\"ir\" bitsize=\"16\"/></feature></target>";

/** @brief Registers gdb numbers (0 to 12). */
#define NREGS 13

static uint32_t reg_size(uint32_t n)
{
    (void)n;
    return 2;
}

static uint16_t value(const rl_ctx *c, uint32_t n)
{
    switch (n) {
    case 0:
        return (uint16_t)(c->a << 8 | c->f);
    case 1:
        return (uint16_t)(c->b << 8 | c->c);
    case 2:
        return (uint16_t)(c->d << 8 | c->e);
    case 3:
        return (uint16_t)(c->h << 8 | c->l);
    case 4:
        return c->sp;
    case 5:
        return c->pc;
    default:
        return 0;
    }
}

static void reg_read(const rl_ctx *c, uint32_t n, uint8_t *o)
{
    uint16_t v = value(c, n);
    o[0] = (uint8_t)v;
    o[1] = (uint8_t)(v >> 8);
}

static void reg_write(rl_ctx *c, uint32_t n, const uint8_t *in)
{
    uint8_t hi = in[1], lo = in[0];
    switch (n) {
    case 0:
        c->a = hi;
        c->f = lo & 0xF0u;
        break;
    case 1:
        c->b = hi;
        c->c = lo;
        break;
    case 2:
        c->d = hi;
        c->e = lo;
        break;
    case 3:
        c->h = hi;
        c->l = lo;
        break;
    case 4:
        c->sp = (uint16_t)(hi << 8 | lo);
        break;
    default:
        break;
    }
}

static uint64_t pc_of(const rl_ctx *c) { return c->pc; }

static const rl_gdb_cpu g_gdb = {
    .target_xml = g_xml,
    .nregs = NREGS,
    .reg_size = reg_size,
    .reg_read = reg_read,
    .reg_write = reg_write,
    .pc = pc_of,
    .pc_reg = 5,
};

const rl_gdb_cpu *rl_sm83_gdb(void) { return &g_gdb; }

#endif
