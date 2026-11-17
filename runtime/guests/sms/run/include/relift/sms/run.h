/**
 * @file relift/sms/run.h
 * @brief The entries lifted Master System and Game Gear code calls, on top
 * of relift/z80/abi.h: every memory access, port access, instruction end
 * and HALT goes to the machine (runtime/guests/sms/run).
 *
 * Every block ends in a sync point and the budget is zero at each
 * instruction boundary, so rl_sms_sync runs after every instruction: it
 * moves the machine's clock by the T-states the instruction took (minus the
 * budget), checks that the code the next instruction assumes is still
 * mapped, and takes interrupts. The info word says what kind of exit the
 * block has and which code it was lifted from (RL_SMS_SYNC_*).
 */
#ifndef RELIFT_SMS_RUN_H
#define RELIFT_SMS_RUN_H

#include <relift/z80/abi.h>

/** @brief Sync kinds in bits 0-1 of the info word. */
#define RL_SMS_SYNC_JUMP 0u
#define RL_SMS_SYNC_CALL 1u
#define RL_SMS_SYNC_RETURN 2u
/** @brief Set when the exit may continue in lifted code of the block's own unit directly, without dispatch. */
#define RL_SMS_SYNC_CHECK 4u
/** @brief Bits 3-4: the slot (0 to 2) of a ROM unit, or RL_SMS_SLOT_RAM. */
#define RL_SMS_SYNC_SLOT_SHIFT 3u
#define RL_SMS_SLOT_RAM 3u
/** @brief Bits 5-15: the ROM bank of the unit, or the RAM group, plus one. */
#define RL_SMS_SYNC_UNIT_SHIFT 5u
/** @brief Bits 16-31: the return address of a call. */
#define RL_SMS_SYNC_RET_SHIFT 16u

#define RL_SMS_SYNC_KIND(i) ((i) & 3u)
#define RL_SMS_SYNC_SLOT(i) (((i) >> RL_SMS_SYNC_SLOT_SHIFT) & 3u)
#define RL_SMS_SYNC_UNIT(i) (((i) >> RL_SMS_SYNC_UNIT_SHIFT) & 0x7FFu)
#define RL_SMS_SYNC_RET(i) ((uint16_t)((i) >> RL_SMS_SYNC_RET_SHIFT))

/** @brief A memory read of size bytes (1 or 2) at addr. */
RL_RUNTIME_API uint64_t rl_sms_read(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size);
/** @brief A memory write. */
RL_RUNTIME_API void rl_sms_write(rl_ctx *c, uint8_t *mem, uint64_t addr, uint32_t size, uint64_t value);
/** @brief IN: the byte the port gives at the end of the CPU's I/O cycle. */
RL_RUNTIME_API uint8_t rl_sms_in(rl_ctx *c, uint16_t port);
/** @brief OUT. */
RL_RUNTIME_API void rl_sms_out(rl_ctx *c, uint16_t port, uint8_t value);
/** @brief The end of an instruction whose exit goes to next; returns RL_OK, or RL_PENDING to unwind. */
RL_RUNTIME_API int rl_sms_sync(rl_ctx *c, uint8_t *mem, uint64_t next, uint32_t info);
/**
 * @brief HALT, after its own 4 T-states: runs NOP cycles of 4 T-states until
 * an interrupt can be taken (an NMI, or INT with iff1 set) or the run ends,
 * and returns R with one more in its low 7 bits per NOP.
 */
RL_RUNTIME_API uint8_t rl_sms_halt(rl_ctx *c, uint8_t *mem, uint8_t iff1, uint8_t r);

#endif
