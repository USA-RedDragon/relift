/* The SWI case generator the console probe ran, so the tests replay the same 3,529 records against the HLE BIOS
 * over a fake GBA memory and compare with the console's results (vectors_console.h).
 *
 * A record is one SWI call (n == 1) or a group of n calls of the same SWI whose results are folded into
 * one hash (dense math sweeps). For every call j of record i:
 *   1. swi_case_make(i, j, &c, m): sets c (SWI number, r0..r3 in) and writes every input byte and every
 *      output prefill byte through m, so a call never depends on an earlier call.
 *   2. the caller runs the SWI with r0..r3 = c.r, r4..r11 = SWI_R4..SWI_R11, sp_sys = SWI_SP_SYS,
 *      lr_sys = SWI_LR_SYS, CPSR = SWI_CPSR, from the ARM stub (or the Thumb stub when c.thumb), with
 *      SWI_STK_SYS bytes below sp_sys and SWI_STK_SVC bytes below 0x03007FE0 filled with SWI_FILL first.
 *   3. swi_case_hash(i, j, &c, m) returns the FNV-1a hash of every output window of the call.
 *   4. the stack windows (SWI_STK_SYS bytes below sp_sys, SWI_STK_SVC below 0x03007FE0) are hashed
 *      (swi_fnv over the bytes in rising address order).
 * Record fields for n == 1: r[0..3] out, r12 out, cpsr out, mem = case hash, stk = hash of the sys window
 * then the svc window (one running FNV over both), cycles = timer cycles of the call minus the empty stub.
 * For n > 1: r[0] = swi_fold over every call of (r0,r1,r2,r3,r12,cpsr out), mem = swi_fold over the case
 * hashes, stk = swi_fold over the stack hashes, r[1..3] = r12 = cpsr = 0, cycles = sum. swi_fold(h, v) =
 * FNV-1a step over the 4 little-endian bytes of v.
 */
#ifndef SWICASE_H
#define SWICASE_H

#include <stdint.h>

#define SWI_FNV_INIT 0x811C9DC5u
#define SWI_SP_SYS 0x03007C00u
#define SWI_SP_SVC 0x03007FE0u
#define SWI_LR_SYS 0x0300C0DEu
#define SWI_CPSR 0x6000009Fu
#define SWI_R4 0x44444444u
#define SWI_R5 0x55555555u
#define SWI_R6 0x66666666u
#define SWI_R7 0x77777777u
#define SWI_R8 0x88888888u
#define SWI_R9 0x99999999u
#define SWI_R10 0xAAAAAAAAu
#define SWI_R11 0xBBBBBBBBu
#define SWI_FILL 0xA5u
#define SWI_STK_SYS 96u
#define SWI_STK_SVC 32u

typedef struct swi_mem {
    void *u;
    uint8_t (*rd8)(void *u, uint32_t a);
    void (*wr8)(void *u, uint32_t a, uint8_t v);
    void (*wr16)(void *u, uint32_t a, uint16_t v);
    void (*wr32)(void *u, uint32_t a, uint32_t v);
} swi_mem;

typedef struct swi_case {
    uint8_t swi;
    uint8_t thumb;
    uint32_t r[4];
} swi_case;

typedef struct swi_rec {
    uint32_t r[4];
    uint32_t r12;
    uint32_t cpsr;
    uint32_t mem;
    uint32_t stk;
    uint32_t cycles;
} swi_rec;

unsigned swi_record_count(void);
unsigned swi_record_calls(unsigned i);
const char *swi_record_name(unsigned i);
void swi_case_make(unsigned i, unsigned j, swi_case *c, const swi_mem *m);
uint32_t swi_case_hash(unsigned i, unsigned j, const swi_case *c, const swi_mem *m);

static inline uint32_t swi_fnv(uint32_t h, uint8_t b) { return (h ^ b) * 0x01000193u; }
static inline uint32_t swi_fold(uint32_t h, uint32_t v)
{
    for (int k = 0; k < 4; k++)
        h = swi_fnv(h, (uint8_t)(v >> (8 * k)));
    return h;
}


/* record flags (swi_record_flags) */
#define SWI_REC_IRQ 1u
#define SWI_REC_BLANK 2u

#define SWI_FLAGWORD 0x03007FF8u
#define SWI_SRC 0x02000000u
#define SWI_DST 0x02008000u
#define SWI_INFO 0x0200E000u
#define SWI_VDST 0x06013000u

unsigned swi_record_flags(unsigned i);

#endif
