/**
 * @file gdb.c
 * @brief The A64 registers for the GDB server (core/gdbstub.h), in gdb's own
 * aarch64 numbering: x0 to x30 (0 to 30), sp (31), pc (32), cpsr (33), v0 to v31
 * (34 to 65), fpsr (66), fpcr (67), and the read-only thread pointers tpidr_el0
 * and tpidrro_el0 (68, 69).
 *
 * pc is the guest address the context names: a system call's own address
 * (rl_svc_site) at a system call, the callee at an indirect call. NZCV is stored
 * only where docs/abi.md says, so cpsr can be stale.
 */
#include "arm64.h"

#include <string.h>

#include "gdbstub.h"

#if defined(RL_DEBUG_STUB)

/** @brief The target description: gdb's aarch64 core and fpu features plus the thread pointers. */
static const char g_xml[] =
    "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\">"
    "<target version=\"1.0\"><architecture>aarch64</architecture>"
    "<feature name=\"org.gnu.gdb.aarch64.core\">"
    "<reg name=\"x0\" bitsize=\"64\" regnum=\"0\"/><reg name=\"x1\" bitsize=\"64\"/><reg name=\"x2\" bitsize=\"64\"/>"
    "<reg name=\"x3\" bitsize=\"64\"/><reg name=\"x4\" bitsize=\"64\"/><reg name=\"x5\" bitsize=\"64\"/>"
    "<reg name=\"x6\" bitsize=\"64\"/><reg name=\"x7\" bitsize=\"64\"/><reg name=\"x8\" bitsize=\"64\"/>"
    "<reg name=\"x9\" bitsize=\"64\"/><reg name=\"x10\" bitsize=\"64\"/><reg name=\"x11\" bitsize=\"64\"/>"
    "<reg name=\"x12\" bitsize=\"64\"/><reg name=\"x13\" bitsize=\"64\"/><reg name=\"x14\" bitsize=\"64\"/>"
    "<reg name=\"x15\" bitsize=\"64\"/><reg name=\"x16\" bitsize=\"64\"/><reg name=\"x17\" bitsize=\"64\"/>"
    "<reg name=\"x18\" bitsize=\"64\"/><reg name=\"x19\" bitsize=\"64\"/><reg name=\"x20\" bitsize=\"64\"/>"
    "<reg name=\"x21\" bitsize=\"64\"/><reg name=\"x22\" bitsize=\"64\"/><reg name=\"x23\" bitsize=\"64\"/>"
    "<reg name=\"x24\" bitsize=\"64\"/><reg name=\"x25\" bitsize=\"64\"/><reg name=\"x26\" bitsize=\"64\"/>"
    "<reg name=\"x27\" bitsize=\"64\"/><reg name=\"x28\" bitsize=\"64\"/><reg name=\"x29\" bitsize=\"64\"/>"
    "<reg name=\"x30\" bitsize=\"64\"/><reg name=\"sp\" bitsize=\"64\" type=\"data_ptr\"/>"
    "<reg name=\"pc\" bitsize=\"64\" type=\"code_ptr\"/><reg name=\"cpsr\" bitsize=\"32\"/></feature>"
    "<feature name=\"org.gnu.gdb.aarch64.fpu\">"
    "<vector id=\"v2d\" type=\"ieee_double\" count=\"2\"/><vector id=\"v2u\" type=\"uint64\" count=\"2\"/>"
    "<vector id=\"v4f\" type=\"ieee_single\" count=\"4\"/><vector id=\"v4u\" type=\"uint32\" count=\"4\"/>"
    "<vector id=\"v16u\" type=\"uint8\" count=\"16\"/>"
    "<union id=\"vnd\"><field name=\"f\" type=\"v2d\"/><field name=\"u\" type=\"v2u\"/></union>"
    "<union id=\"vns\"><field name=\"f\" type=\"v4f\"/><field name=\"u\" type=\"v4u\"/></union>"
    "<union id=\"aarch64v\"><field name=\"d\" type=\"vnd\"/><field name=\"s\" type=\"vns\"/>"
    "<field name=\"q\" type=\"uint128\"/><field name=\"b\" type=\"v16u\"/></union>"
    "<reg name=\"v0\" bitsize=\"128\" type=\"aarch64v\" regnum=\"34\"/>"
    "<reg name=\"v1\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v2\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v3\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v4\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v5\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v6\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v7\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v8\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v9\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v10\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v11\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v12\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v13\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v14\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v15\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v16\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v17\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v18\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v19\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v20\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v21\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v22\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v23\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v24\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v25\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v26\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v27\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v28\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v29\" bitsize=\"128\" type=\"aarch64v\"/><reg name=\"v30\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"v31\" bitsize=\"128\" type=\"aarch64v\"/>"
    "<reg name=\"fpsr\" bitsize=\"32\"/><reg name=\"fpcr\" bitsize=\"32\"/></feature>"
    "<feature name=\"org.gnu.gdb.aarch64.tls\">"
    "<reg name=\"tpidr\" bitsize=\"64\" regnum=\"68\"/><reg name=\"tpidrro\" bitsize=\"64\"/></feature>"
    "</target>";

/** @brief Registers in g_xml. */
#define NREGS 70

static uint32_t reg_size(uint32_t n)
{
    if (n == 33 || n == 66 || n == 67)
        return 4;
    if (n >= 34 && n < 66)
        return 16;
    return 8;
}

/** @brief The cpsr gdb shows: N, Z, C and V in bits 31 to 28 from the stored flags. */
static uint32_t cpsr(const rl_ctx *c)
{
    return (uint32_t)(c->nzcv[0] & 1) << 31 | (uint32_t)(c->nzcv[1] & 1) << 30 | (uint32_t)(c->nzcv[2] & 1) << 29 |
           (uint32_t)(c->nzcv[3] & 1) << 28;
}

static void put64(uint8_t *o, uint64_t v) { memcpy(o, &v, 8); }

static void reg_read(const rl_ctx *c, uint32_t n, uint8_t *o)
{
    uint32_t v32;
    if (n < 31)
        put64(o, c->x[n]);
    else if (n == 31)
        put64(o, c->sp);
    else if (n == 32)
        put64(o, c->pc);
    else if (n == 33)
        v32 = cpsr(c), memcpy(o, &v32, 4);
    else if (n < 66)
        memcpy(o, &c->v[n - 34], 16);
    else if (n == 66)
        memcpy(o, &c->fpsr, 4);
    else if (n == 67)
        memcpy(o, &c->fpcr, 4);
    else if (n == 68)
        put64(o, c->tpidr_el0);
    else
        put64(o, c->tpidrro_el0);
}

static void reg_write(rl_ctx *c, uint32_t n, const uint8_t *in)
{
    uint64_t v;
    uint32_t v32;
    memcpy(&v, in, 8);
    if (n < 31)
        c->x[n] = v;
    else if (n == 31)
        c->sp = v;
    else if (n == 32)
        c->pc = v;
    else if (n == 33) {
        memcpy(&v32, in, 4);
        for (int i = 0; i < 4; i++)
            c->nzcv[i] = (uint8_t)(v32 >> (31 - i) & 1);
    } else if (n < 66)
        memcpy(&c->v[n - 34], in, 16);
    else if (n == 66)
        memcpy(&c->fpsr, in, 4);
    else if (n == 67)
        memcpy(&c->fpcr, in, 4);
}

static uint64_t pc_of(const rl_ctx *c) { return c->pc; }

static const rl_gdb_cpu g_gdb = {
    .target_xml = g_xml,
    .nregs = NREGS,
    .reg_size = reg_size,
    .reg_read = reg_read,
    .reg_write = reg_write,
    .pc = pc_of,
    .pc_reg = 32,
};

const rl_gdb_cpu *rl_arm64_gdb(void) { return &g_gdb; }

#endif
