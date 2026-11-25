/**
 * @file gdb.c
 * docgen:region registers
 * @brief The VR4300 registers for the GDB server (core/gdbstub.h), in the
 * order of gdb's 64-bit mips target: r0 to r31 (0 to 31), status (32), lo
 * (33), hi (34), badvaddr (35), cause (36), pc (37), f0 to f31 (38 to 69),
 * fcsr (70) and fir (71), 64 bits each, in the guest's big-endian order (gdb
 * needs `set endian big`). pc is the guest address the thread resumes at
 * where it parked at a sync point. fir reads as the VR4300's (0xB00) and
 * ignores writes.
 * docgen:end
 */
#include "mips.h"

#include <string.h>

#include "gdbstub.h"

#if defined(RL_DEBUG_STUB)

/** @brief The target description: gdb's mips cpu, cp0 and fpu features with their register numbers. */
static const char g_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\"><architecture>mips:4300</architecture>"
    "<feature name=\"org.gnu.gdb.mips.cpu\">"
    "<reg name=\"r0\" bitsize=\"64\" regnum=\"0\"/><reg name=\"r1\" bitsize=\"64\"/><reg name=\"r2\" bitsize=\"64\"/>"
    "<reg name=\"r3\" bitsize=\"64\"/><reg name=\"r4\" bitsize=\"64\"/><reg name=\"r5\" bitsize=\"64\"/>"
    "<reg name=\"r6\" bitsize=\"64\"/><reg name=\"r7\" bitsize=\"64\"/><reg name=\"r8\" bitsize=\"64\"/>"
    "<reg name=\"r9\" bitsize=\"64\"/><reg name=\"r10\" bitsize=\"64\"/><reg name=\"r11\" bitsize=\"64\"/>"
    "<reg name=\"r12\" bitsize=\"64\"/><reg name=\"r13\" bitsize=\"64\"/><reg name=\"r14\" bitsize=\"64\"/>"
    "<reg name=\"r15\" bitsize=\"64\"/><reg name=\"r16\" bitsize=\"64\"/><reg name=\"r17\" bitsize=\"64\"/>"
    "<reg name=\"r18\" bitsize=\"64\"/><reg name=\"r19\" bitsize=\"64\"/><reg name=\"r20\" bitsize=\"64\"/>"
    "<reg name=\"r21\" bitsize=\"64\"/><reg name=\"r22\" bitsize=\"64\"/><reg name=\"r23\" bitsize=\"64\"/>"
    "<reg name=\"r24\" bitsize=\"64\"/><reg name=\"r25\" bitsize=\"64\"/><reg name=\"r26\" bitsize=\"64\"/>"
    "<reg name=\"r27\" bitsize=\"64\"/><reg name=\"r28\" bitsize=\"64\"/><reg name=\"r29\" bitsize=\"64\"/>"
    "<reg name=\"r30\" bitsize=\"64\"/><reg name=\"r31\" bitsize=\"64\"/>"
    "<reg name=\"lo\" bitsize=\"64\" regnum=\"33\"/><reg name=\"hi\" bitsize=\"64\" regnum=\"34\"/>"
    "<reg name=\"pc\" bitsize=\"64\" regnum=\"37\"/></feature>"
    "<feature name=\"org.gnu.gdb.mips.cp0\">"
    "<reg name=\"status\" bitsize=\"64\" regnum=\"32\"/><reg name=\"badvaddr\" bitsize=\"64\" regnum=\"35\"/>"
    "<reg name=\"cause\" bitsize=\"64\" regnum=\"36\"/></feature>"
    "<feature name=\"org.gnu.gdb.mips.fpu\">"
    "<reg name=\"f0\" bitsize=\"64\" type=\"ieee_double\" regnum=\"38\"/>"
    "<reg name=\"f1\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f2\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f3\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f4\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f5\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f6\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f7\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f8\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f9\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f10\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f11\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f12\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f13\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f14\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f15\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f16\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f17\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f18\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f19\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f20\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f21\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f22\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f23\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f24\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f25\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f26\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f27\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f28\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f29\" bitsize=\"64\" type=\"ieee_double\"/><reg name=\"f30\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"f31\" bitsize=\"64\" type=\"ieee_double\"/>"
    "<reg name=\"fcsr\" bitsize=\"64\" group=\"float\" regnum=\"70\"/>"
    "<reg name=\"fir\" bitsize=\"64\" group=\"float\" regnum=\"71\"/></feature></target>";

/** @brief Registers gdb numbers (0 to 71). */
#define NREGS 72
/** @brief The VR4300's FCR0 (implementation and revision). */
#define VR4300_FIR 0xB00u

static uint32_t reg_size(uint32_t n)
{
    (void)n;
    return 8;
}

static uint64_t *slot(rl_ctx *c, uint32_t n)
{
    if (n < 32)
        return &c->r[n];
    if (n == 33)
        return &c->lo;
    if (n == 34)
        return &c->hi;
    if (n == 35)
        return &c->cp0_bad_vaddr;
    if (n == 37)
        return &c->pc;
    if (n >= 38 && n < 70)
        return &c->f[n - 38];
    return NULL;
}

static void put_be(uint8_t *o, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        o[i] = (uint8_t)(v >> (56 - 8 * i));
}

static uint64_t get_be(const uint8_t *in)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++)
        v = v << 8 | in[i];
    return v;
}

static void reg_read(const rl_ctx *c, uint32_t n, uint8_t *o)
{
    uint64_t *p = slot((rl_ctx *)(uintptr_t)c, n);
    uint64_t v = p ? *p : n == 32 ? c->cp0_status : n == 36 ? c->cp0_cause : n == 70 ? c->fcr31 : VR4300_FIR;
    put_be(o, v);
}

static void reg_write(rl_ctx *c, uint32_t n, const uint8_t *in)
{
    uint64_t v = get_be(in);
    uint64_t *p = slot(c, n);
    if (p)
        *p = n == 0 ? 0 : v;
    else if (n == 32)
        c->cp0_status = (uint32_t)v;
    else if (n == 36)
        c->cp0_cause = (uint32_t)v;
    else if (n == 70)
        c->fcr31 = (uint32_t)v;
}

static uint64_t pc_of(const rl_ctx *c) { return c->pc; }

static const rl_gdb_cpu g_gdb = {
    .target_xml = g_xml,
    .nregs = NREGS,
    .reg_size = reg_size,
    .reg_read = reg_read,
    .reg_write = reg_write,
    .pc = pc_of,
    .pc_reg = 37,
    .big_endian = true,
};

const rl_gdb_cpu *rl_mips_gdb(void) { return &g_gdb; }

#endif
