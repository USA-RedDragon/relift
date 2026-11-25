/**
 * @file gdb.c
 * docgen:region registers
 * @brief The ARMv4T registers for the GDB server (core/gdbstub.h), as gdb's arm
 * target names them: r0 to r12 (0 to 12), sp (13), lr (14), pc (15) and cpsr
 * (16), 32 bits each, the current mode's bank. pc is the guest address the
 * thread resumes at where it parked at a sync point. Writing cpsr changes mode
 * as an MSR would (rl_arm32_set_cpsr).
 * docgen:end
 */
#include "arm32.h"

#include <string.h>

#include "gdbstub.h"

#if defined(RL_DEBUG_STUB)

/** @brief The target description: gdb's arm core feature, with cpsr numbered right after pc. */
static const char g_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\"><architecture>arm</architecture>"
    "<feature name=\"org.gnu.gdb.arm.core\">"
    "<reg name=\"r0\" bitsize=\"32\" regnum=\"0\"/><reg name=\"r1\" bitsize=\"32\"/><reg name=\"r2\" bitsize=\"32\"/>"
    "<reg name=\"r3\" bitsize=\"32\"/><reg name=\"r4\" bitsize=\"32\"/><reg name=\"r5\" bitsize=\"32\"/>"
    "<reg name=\"r6\" bitsize=\"32\"/><reg name=\"r7\" bitsize=\"32\"/><reg name=\"r8\" bitsize=\"32\"/>"
    "<reg name=\"r9\" bitsize=\"32\"/><reg name=\"r10\" bitsize=\"32\"/><reg name=\"r11\" bitsize=\"32\"/>"
    "<reg name=\"r12\" bitsize=\"32\"/><reg name=\"sp\" bitsize=\"32\" type=\"data_ptr\"/>"
    "<reg name=\"lr\" bitsize=\"32\"/><reg name=\"pc\" bitsize=\"32\" type=\"code_ptr\"/>"
    "<reg name=\"cpsr\" bitsize=\"32\" regnum=\"16\"/></feature></target>";

/** @brief Registers in g_xml. */
#define NREGS 17

static uint32_t reg_size(uint32_t n)
{
    (void)n;
    return 4;
}

static void reg_read(const rl_ctx *c, uint32_t n, uint8_t *o)
{
    uint32_t v = n < 15 ? c->r[n] : n == 15 ? c->pc : rl_arm32_cpsr(c);
    memcpy(o, &v, 4);
}

static void reg_write(rl_ctx *c, uint32_t n, const uint8_t *in)
{
    uint32_t v;
    memcpy(&v, in, 4);
    if (n < 15)
        c->r[n] = v;
    else if (n == 15)
        c->pc = v;
    else
        rl_arm32_set_cpsr(c, v);
}

static uint64_t pc_of(const rl_ctx *c) { return c->pc; }

static const rl_gdb_cpu g_gdb = {
    .target_xml = g_xml,
    .nregs = NREGS,
    .reg_size = reg_size,
    .reg_read = reg_read,
    .reg_write = reg_write,
    .pc = pc_of,
    .pc_reg = 15,
};

const rl_gdb_cpu *rl_arm32_gdb(void) { return &g_gdb; }

#endif
